// ZeroEngine3DViewport.cpp — The Main Window. The Portal. Ground Fucking Zero.
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// This is the entry point for the entire 3D viewport the WinMain,
// the message pump, the ImGui frame loop, the docking layout, the menu
// bar, the toolbar, every panel, every button, every goddamn pixel you
// see when you launch this application. This file is where the Win32
// window gets created, where Havok gets initialized (hkBaseSystem::init
// with the stolen SDK's memory allocator and error handler), where
// the Scene3DRenderer gets wired to the HWND, where ImGui gets set up
// with D3D9 backend, and where the render loop ticks until you close
// the window or the program crashes (whichever comes first, usually
// the crash, let's be fucking honest).
//
// Using their SDK, Running their shaders. Loading their levels.
// We rebuilt their fucking TOOLS.
// "The two most important days in your life are the day you were born
// and the day you find out why."
// I found out why on the day this viewport first rendered a Conquest level. That was
// my second birthday. Everything before that was just preparation.
// -----------------------------------------------------------------------

// What you're actually looking at:
// A lovingly hand crafted monument to deprecated rendering pipelines,
// mixed with just enough glue code to keep it from immediately crashing
// on a machine built after Pandemic's shutdown.

// Call stack of slop:
//   Win32 Application                  ← Still using the API that Microsoft now regrets creating.
//          ↓
//   Scene3DRenderer                     ← The part where Havok pretends it still matters
//          ↓
//   Scene3DLoader                       ← Magically loads .hkx files and we’re proud of it
//          ↓
//   HavokToDisplayConverter             ← The class whose name is longer than its useful lifetime
//          ↓
//   AnimatedCharacter                   ← Where the spooky model skeleton exists
//          ↓
//   hkgDisplayWorld → render() → Screen ← pixels appear… sometimes… if the stars align

// Features we're very proud of (please clap):
//  We kept the entire Havok 5.5.0 rendering pipe on life support
//  Animation blending because one animation at a time is for cowards

//               From the codefiles:
// • havok/hk550/Demo/Demos/DemoCommon/DemoFramework/hkDemo.cpp
// • havok/hk550/Demo/Demos/Animation/Api/MeshAndDeformation/Skinning/SkinningDemo.cpp
// • havok/hk550/Demo/Demos/Animation/Api/Blending/Normal/NormalBlendingDemo.cpp
//
// These files are now older than most Discord moderators.
// We have not asked for Havok's consent before continuing to use them.
// They would like to be forgotten anyway but we refuse!!!!.

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#pragma comment(lib, "comdlg32.lib")

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")
#endif
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <set>
#include <map>
#include <vector>

// ImGui glue DLL (compiled with modern compiler) runtime loaded
#include "imgui_glue.h"

// Havok v5.5.0 Core
#include <Common/Base/hkBase.h>
#include <Common/Base/System/hkBaseSystem.h>
#include <Common/Base/Memory/Memory/Pool/hkPoolMemory.h>
#include <Common/Base/Memory/hkThreadMemory.h>
#include <Common/Base/System/Io/FileSystem/hkFileSystem.h>
#include <Common/Base/System/Io/FileSystem/hkNativeFileSystem.h>

// ZeroEngine 3D Components
#include "Scene3DRenderer.h"
#include "Scene3DRendererInternal.h"
#include "LevelScene.h"
#include "GameShaderCache.h"  // for the Render Debug PS dropdown
#include "GameModelLoader.h"  // for the Render Debug meshPart inspector
#include "LevelInspector.h"
#include "Scene3DLoader.h"
#include "HavokToDisplayConverter.h"
#include "AnimationSystem.h"
#include "AssetBrowser.h"
#include "LuaAnimationRuntime.h"
#include "LevelReader.h"
#include "CollabSession.h"
#include "SplineEditor.h"
#include "LevelConstants.h"
#include "LevelValidator.h"
#include "LevelTemplates.h"
#include "MocapBridge.h"
#include "MocapRetargeter.h"
#include "MocapExporter.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// Audio subsystem (Wwise SDK v2.1.2821)
#include "AudioManager.h"

//
// Global State
//

static Scene3DRenderer* g_renderer = NULL;
static LevelInspector   g_levelInspector;
static AnimatedCharacter* g_character = NULL;

// Level reader — native PAK/BIN reader
// File-scope but exposed via an accessor for code that needs the
// reader (asset browser preview wants BIN-embedded textures, mesh
// surgery wants ModelInfo, etc.). The accessor keeps the variable
// itself private and means callers can be in different translation
// units without dragging the LevelReader header everywhere.
static ZeroEngine::LevelReader g_levelReader;
ZeroEngine::LevelReader* GetGlobalLevelReader() { return &g_levelReader; }
static std::string g_levelLoadError;

// Mocap pipeline
static MocapBridge      g_mocapBridge;
static MocapRetargeter  g_mocapRetargeter;
static std::vector<RetargetedFrame> g_mocapFrames;
static float g_mocapPlayTime = 0.0f;
static bool  g_mocapPlaying = false;
static std::string g_mocapVideoPath;
// Model Viewer state
static IDirect3DTexture9*       g_mvRenderTarget = NULL;
static IDirect3DSurface9*       g_mvRTSurface = NULL;
static IDirect3DSurface9*       g_mvDepthSurface = NULL;
static int                      g_mvTexW = 512;
static int                      g_mvTexH = 384;
static uint32_t                 g_mvModelCrc = 0;
static std::string              g_mvModelName;
static bool                     g_mvHasCollision = false;
static int                      g_mvCollisionTris = 0;
static int                      g_mvBuildProgress = -1; // -1 = idle
static std::vector<float>       g_mvCollVerts;   // generated collision verts
static std::vector<uint16_t>    g_mvCollIndices; // generated collision indices
static std::vector<uint8_t>     g_mvMoppData;    // generated MOPP data

static void ReleaseModelViewerRenderTarget()
{
    if (g_mvDepthSurface) { g_mvDepthSurface->Release(); g_mvDepthSurface = NULL; }
    if (g_mvRTSurface) { g_mvRTSurface->Release(); g_mvRTSurface = NULL; }
    if (g_mvRenderTarget) { g_mvRenderTarget->Release(); g_mvRenderTarget = NULL; }
}

// ─────────────────────────────────────────────────────────────────────────
//  F4 wireframe overlay - the goddamn Blender-grade rewrite
// ─────────────────────────────────────────────────────────────────────────
// The old approach was D3DFILL_WIREFRAME on the triangle list. The result
// looked like ASS for two reasons that compound into one giant fucking mess:
//
//   1. WIREFRAME mode draws ALL THREE EDGES of EVERY TRIANGLE. Two adjacent
//      triangles share one edge, so that shared edge gets rasterized TWICE.
//      A typical mesh has ~3x more drawn edges than ACTUAL unique edges.
//      Triple the line density. Triple the visual noise. For a collision
//      hull with 5000 tris you draw 15000 edge segments when the geometry
//      has maybe 7500 unique edges. The whole screen turns into green soup.
//
//   2. No depth fade, no back-face dimming, no anti-aliasing. So far edges
//      are AS BRIGHT as near edges, edges on the back of the volume are AS
//      BRIGHT as edges on the front, and every line is a 1-pixel jaggy
//      that aliases against every other line at sub-pixel angles. The user
//      said they overhauled this six times and it still looked like trash.
//      No fucking wonder.
//
// The new approach builds a deduped EDGE LIST ONCE per collision-data load
// (cached until invalidated), then renders LINELIST instead of triangle
// wireframe. Each edge gets:
//   - Per-vertex color computed each frame from camera distance (depth fade)
//     AND average face-normal vs view direction (back-face dim)
//   - Alpha blending so dimmed edges actually blend against the model behind
//   - D3DRS_ANTIALIASEDLINEENABLE if the driver supports it
//
// Result: ~3x fewer lines drawn, near edges pop, far edges retreat, back
// edges fade into the background, the volume's shape becomes READABLE
// instead of a green Rorschach test. Like Blender. Like every editor that
// has ever shipped a wireframe overlay anyone could understand.
struct CollisionEdge {
    uint16_t a, b;          // vertex indices into g_mvCollVerts
    float    nx, ny, nz;    // average face normal of the (up to 2) faces sharing this edge
};
static std::vector<CollisionEdge> g_mvWireEdges;
static bool                       g_mvWireDirty = true;
static int                      g_savePakStatus = 0;    // 0=idle, 1-4=in progress, 5=done, -1=error
static bool                     g_pendingSceneReload = false; // set after SavePak to trigger full reload next frame
static std::string              g_savePakMessage;
static float                    g_savePakDoneTimer = 0; // seconds to show "done" message
static std::string              g_lastSavedPakPath;     // set after successful save so DLL updates its path field

// Cinematic playback state
static bool                     g_cineActive = false;
static int                      g_cineCamIdx = -1;
static float                    g_cineTime = 0.0f;
static float                    g_cineDuration = 0.0f;
static bool                     g_cinePanelOpen = false;
static int                      g_cinePanelSelCam = -1;
// Cinematic camera data arrays (populated from LevelScene each frame)
static std::vector<std::string> g_cineCamNameStrs;
static std::vector<const char*> g_cineCamNamePtrs;
static std::vector<uint32_t>    g_cineCamGuids;
static std::vector<float>       g_cineCamFovs;
static std::vector<float>       g_cineCamDurations;
static std::vector<float>       g_cineCamPosTimes;
static std::vector<float>       g_cineCamTgtTimes;

static bool                     g_mvViewerOpen = false; // host-side open state (persists across frames)
static float                    g_mvOrbitYaw = 0.0f;
static float                    g_mvOrbitPitch = 0.3f;
static float                    g_mvOrbitDist = 10.0f;
static bool                     g_mvWireframe = false;
static bool                     g_mvShowCollision = true;

// String pointer caches for ImGuiGlueFrameArgs (rebuilt when a new level is loaded)
static std::vector<const char*> g_pakModelNamePtrs;
static std::vector<const char*> g_pakTextureNamePtrs;
static std::vector<int>         g_pakTextureWidths;
static std::vector<int>         g_pakTextureHeights;
static std::vector<const char*> g_pakAnimNamePtrs;
static std::vector<const char*> g_pakEffectNamePtrs;
static std::vector<const char*> g_pakGfxBlockNamePtrs;
static std::vector<unsigned int>  g_pakModelCrcs;
static std::vector<int>           g_pakModelBoneCounts;
static std::vector<int>           g_pakModelMatCounts;
static std::vector<int>           g_pakModelGamemodeMasks;
static std::vector<int>           g_pakTextureDepths;
static std::vector<int>           g_pakTextureLevels;
static std::vector<unsigned int>  g_pakTextureFormats;
static std::vector<int>           g_pakTextureGamemodeMasks;
static std::vector<unsigned int>  g_pakAnimCrcs;
static std::vector<int>           g_pakAnimSizes;
static std::vector<int>           g_pakAnimSizesComp;
static std::vector<int>           g_pakAnimGamemodeMasks;
static std::vector<int>           g_pakEffectSizes;
static std::vector<int>           g_pakEffectGamemodeMasks;
static std::vector<int>           g_pakGfxSizes;
static std::vector<const char*>   g_binRawNamePtrs;
static std::vector<unsigned int>  g_binRawKinds;
static std::vector<int>           g_binRawSizes;
static std::vector<int>           g_binRawSizesComp;
static std::vector<std::string>   g_binRawNameStrings;
static std::vector<std::string>   g_subBlock1NameStrings;
static std::vector<const char*>   g_subBlock1NamePtrs;
static std::vector<std::string>   g_subBlock1TypeStrings;
static std::vector<const char*>   g_subBlock1TypePtrs;
static std::vector<int>           g_subBlock1Sizes;
static std::vector<std::string>   g_subBlock2NameStrings;
static std::vector<const char*>   g_subBlock2NamePtrs;
static std::vector<std::string>   g_subBlock2TypeStrings;
static std::vector<const char*>   g_subBlock2TypePtrs;
static std::vector<int>           g_subBlock2Sizes;
// ObjAs
static std::vector<std::string>   g_objANameStrings;
static std::vector<const char*>   g_objANamePtrs;
static std::vector<int>           g_objASizes;
static std::vector<int>           g_objASizesComp;
// Obj0s
static std::vector<std::string>   g_obj0NameStrings;
static std::vector<const char*>   g_obj0NamePtrs;
static std::vector<unsigned int>  g_obj0TypeCodes;
// Anim blocks
static std::vector<std::string>   g_animBlockNameStrings;
static std::vector<const char*>   g_animBlockNamePtrs;
static std::vector<std::string>   g_animBlockKeyNameStrings;
static std::vector<const char*>   g_animBlockKeyNamePtrs;
static std::vector<int>           g_animBlockSizes;
static std::vector<int>           g_animBlockSizesComp;
// Pak vals
static std::vector<std::string>   g_pakValNameStrings;
static std::vector<const char*>   g_pakValNamePtrs;
static std::vector<int>           g_pakValGamemodeMasks;
// Buffer infos
static std::vector<std::string>   g_bufInfoNameStrings;
static std::vector<const char*>   g_bufInfoNamePtrs;
static std::vector<unsigned int>  g_bufInfoExtra1, g_bufInfoExtra2;
// Mat1-4 + MatExtra
static std::vector<std::string>   g_mat1NameStrings;  static std::vector<const char*>   g_mat1NamePtrs;  static std::vector<unsigned int>  g_mat1Tex0, g_mat1Tex1;
static std::vector<std::string>   g_mat2NameStrings;  static std::vector<const char*>   g_mat2NamePtrs;  static std::vector<unsigned int>  g_mat2Tex0, g_mat2Tex1;
static std::vector<std::string>   g_mat3NameStrings;  static std::vector<const char*>   g_mat3NamePtrs;  static std::vector<unsigned int>  g_mat3Tex0, g_mat3Tex1;
static std::vector<std::string>   g_mat4NameStrings;  static std::vector<const char*>   g_mat4NamePtrs;  static std::vector<unsigned int>  g_mat4Tex0, g_mat4Tex1;
static std::vector<std::string>   g_matExtraNameStrings; static std::vector<const char*>   g_matExtraNamePtrs;
// Shape infos
static std::vector<std::string>   g_shapeInfoNameStrings;
static std::vector<const char*>   g_shapeInfoNamePtrs;
static std::vector<unsigned int>  g_shapeInfoOffsets, g_shapeInfoKinds;
// Havok shape infos
static std::vector<std::string>   g_hkShapeNameStrings;
static std::vector<const char*>   g_hkShapeNamePtrs;
static std::vector<unsigned int>  g_hkShapeKinds, g_hkShapeANums;
// Havok constraint datas
static std::vector<std::string>   g_hkConstraintDataNameStrings;
static std::vector<const char*>   g_hkConstraintDataNamePtrs;
static std::vector<unsigned int>  g_hkConstraintDataKinds;
// VBuff infos
static std::vector<std::string>   g_vbuffInfoNameStrings;
static std::vector<const char*>   g_vbuffInfoNamePtrs;
static std::vector<unsigned int>  g_vbuffInfoSizes, g_vbuffInfoOffsets;
// IBuff infos
static std::vector<std::string>   g_ibuffInfoNameStrings;
static std::vector<const char*>   g_ibuffInfoNamePtrs;
static std::vector<unsigned int>  g_ibuffInfoSizes, g_ibuffInfoFormats;
// Havok constraint infos
static std::vector<std::string>   g_hkConstraintInfoNameStrings;
static std::vector<const char*>   g_hkConstraintInfoNamePtrs;
static std::vector<unsigned int>  g_hkConstraintInfoKinds, g_hkConstraintInfoBoneParents;
// PField infos
static std::vector<std::string>   g_pFieldNameStrings;
static std::vector<const char*>   g_pFieldNamePtrs;
static std::vector<unsigned int>  g_pFieldGamemodeMasks, g_pFieldWidths;
// Radiosity infos
static std::vector<std::string>   g_radiosityNameStrings;
static std::vector<const char*>   g_radiosityNamePtrs;
static std::vector<unsigned int>  g_radiosityNums, g_radiosityOffsets;
// Foliage infos
static std::vector<std::string>   g_foliageNameStrings;
static std::vector<const char*>   g_foliageNamePtrs;
static std::vector<unsigned int>  g_foliageKinds;
// GameObjs (level entity placement)
static std::vector<std::string>   g_goNameStrings,  g_goTypeNameStrings, g_goMeshNameStrings;
static std::vector<const char*>   g_goNamePtrs,     g_goTypeNamePtrs,    g_goMeshNamePtrs;
static std::vector<unsigned int>  g_goGuids, g_goParentGuids, g_goLayerGuids;
static std::vector<int>           g_goGamemodeMasks;
static std::vector<int>           g_goTeams; // per-entity Team field (-1=unknown)
static std::vector<float>         g_goPosX, g_goPosY, g_goPosZ;
static std::vector<std::string>   g_goTypeDefNameStrings;
static std::vector<const char*>   g_goTypeDefNamePtrs;
static std::vector<int>           g_goTypeDefFieldCounts, g_goTypeDefInstanceCounts;
// GameObjs — event system & relationship fields
static std::vector<unsigned int>  g_goTargetGuids;
static std::vector<std::string>   g_goOutputEventStrings, g_goInputEventStrings, g_goModeNameStrings;
static std::vector<const char*>   g_goOutputEventPtrs, g_goInputEventPtrs, g_goModeNamePtrs;
static std::vector<float>         g_goDelays;
static std::vector<int>           g_goSticky;
// Packed GUID arrays
static std::vector<unsigned int>  g_goOutputsData, g_goLayersArrData, g_goNodesData;
static std::vector<int>           g_goOutputsOffsets, g_goOutputsCounts;
static std::vector<int>           g_goLayersArrOffsets, g_goLayersArrCounts;
static std::vector<int>           g_goNodesOffsets, g_goNodesCounts;
static std::vector<unsigned int>  g_goNode1Guids, g_goNode2Guids;
// Generic GUID references (ALL fields)
static std::vector<unsigned int>  g_goAllGuidRefData;
static std::vector<int>           g_goAllGuidRefCounts;
static std::vector<int>           g_goAllGuidRefOffsets;
// Editor visualization fields
static std::vector<std::string>   g_goEditorShapeStrings, g_goEditorTextureStrings;
static std::vector<const char*>   g_goEditorShapePtrs, g_goEditorTexturePtrs;
static std::vector<unsigned int>  g_goEditorColors;
static std::vector<float>         g_goEditorOuters, g_goEditorSizeX, g_goEditorSizeY, g_goEditorSizeZ;
static AssetBrowser* g_assetBrowser = NULL;
static AssetBrowser* g_gameFilesBrowser = NULL;
static HWND g_hwnd = NULL;
static HWND g_viewportHwnd = NULL;
static HWND g_timelineHud = NULL;
static bool g_isRunning = true;
static LARGE_INTEGER g_perfFreq;
static LARGE_INTEGER g_lastFrameTime;

// Performance metrics
static int g_frameCount = 0;
static float g_fps = 0.0f;
static float g_fpsUpdateTime = 0.0f;
static float g_guiUpdateAccum = 0.0f;
static bool g_imguiAvailable = false;
static bool g_imguiInitialized = false;
static bool g_uiWantCaptureMouse = false;
static bool g_uiWantCaptureKeyboard = false;
static char g_autoSpawnEffectName[128] = {0};
static bool g_autoSpawnEffectDone = false;

// 3dCrowd Editor — host-side persistent selected-item index. args
// gets memset each frame, so we preserve this across frames here.
// The pre-DrawFrame block seeds args from this; the post-DrawFrame
// block writes any DLL- or host-driven change back. -1 = no selection.
static int g_crowdEditorSelectedItem = -1;
static int g_crowdEditorSelInst      = -1;  // v1.4 in-panel per-instance edit

// Crowd Mesh Builder — host-side persistent picker state. Same pattern
// as g_crowdEditorSelectedItem: args gets memset each frame, this
// preserves the panel's "what's currently picked" across frames so the
// dropdowns don't snap back to -1 every redraw. The pre-DrawFrame block
// seeds args from these; the post-DrawFrame block reads back any DLL
// changes and stores them here for the next frame.
static int   g_crowdBuilderSelBaseIdx   = -1;
static int   g_crowdBuilderSelWeaponIdx = -1;
static int   g_crowdBuilderSelBoneIdx   = -1;
static char  g_crowdBuilderSelBaseName[96]   = {0};
static char  g_crowdBuilderSelWeaponName[96] = {0};
static char  g_crowdBuilderSelBoneName[64]   = {0};
static float g_crowdBuilderOffsetXYZ[3] = {0,0,0};
static float g_crowdBuilderOffsetYPR[3] = {0,0,0};
static char  g_crowdBuilderNewMeshName[96] = {0};
// Selection-change trackers — when any of these flip vs the live args
// values, the post-DrawFrame block clears + respawns the preview pair.
// Initialized to -2 so the very first frame with a valid pick is treated
// as a change (real selections are -1 / 0+).
static int   g_crowdBuilderLastPreviewBaseIdx   = -2;
static int   g_crowdBuilderLastPreviewWeaponIdx = -2;
static int   g_crowdBuilderLastPreviewBoneIdx   = -2;
// Tracks the weapon idx we last ran the "auto-pick attach bone" logic for.
// When the user changes weapon, we re-run it. Lets us only override the
// bone dropdown on weapon-change, not stomp the user's manual bone picks
// every frame. -2 = "never ran yet".
static int   g_crowdBuilderAutoPickedForWpIdx = -2;
// Embed mode toggle + UV target rect. Persistent across frames like
// everything else in this panel since args gets memset to zero.
static int   g_crowdBuilderEmbedMode = 0;
static float g_crowdBuilderEmbedUVRect[4] = {0,0,0,0};
// ============================================================
// AUTO-SPAWN STATE: THE FUCKING ANSWER TO "WHERE DID MY MESH GO?"
// ============================================================
// Before this flag the Bake button was a goddamn black hole. User
// picks meshes, dials offsets, hits BAKE, watches a JSON appear
// in models/ and then... nothing. No instances, no entities, no
// way to see the thing they just built without manually authoring
// a CrowdItem through the 3dCrowd Editor by hand like a peasant.
// I burned hours of feedback loops on people not understanding
// this. Fix: ON by default, position seeded once from the camera
// target so the mesh lands right where the user is fucking
// looking. The save flow picks it up via the sidecar and the
// Python side bolts a 3dCrowd entry onto the level. Reload the
// repack and the new mesh is standing there. Done. Eat it.
static int   g_crowdBuilderAutoSpawn       = 1;
static float g_crowdBuilderSpawnPos[3]     = {0.0f, 0.0f, 0.0f};
static int   g_crowdBuilderSpawnCount      = 3;
static int   g_crowdBuilderSpawnPosSeeded  = 0;  /* one-shot snap to cam target */

// ============================================================
// ADJUST BAKED WEAPON state. File-scope so it survives the args
// memset each frame. snapshot tracks WHICH weapon's verts are
// currently cached in g_bowAdjustOrigPositions so we can re-
// snapshot only when the user changes model or weapon dropdown.
// ============================================================
static int      g_bowAdjustSelModelIdx      = 0;
static int      g_bowAdjustSelWeaponIdx     = 0;  // semantically: index into mdl->bones
static float    g_bowAdjustDeltaXYZ[3]      = {0.0f, 0.0f, 0.0f};
static float    g_bowAdjustDeltaYPR[3]      = {0.0f, 0.0f, 0.0f};
static char     g_bowAdjustStatusText[160]  = {0};
static uint32_t g_bowAdjustSnapCrc          = 0;
static int      g_bowAdjustSnapWeaponIdx    = -1;
static std::vector<float>    g_bowAdjustOrigPositions;
// Index list of slot[0] verts weighted to the selected bone. Refreshed
// when the model/bone selection changes. The live VB writeback walks
// this list each frame, parallel to g_bowAdjustOrigPositions.
static std::vector<uint32_t> g_bowAdjustSelectedIndices;

// ============================================================
// TRIANGLE PICKER state. Embedded model viewer inside the Crowd
// Mesh Builder: render-to-texture each frame, mouse-pick triangles,
// drag XYZ sliders to move them. World-space deltas because the
// embedded viewer is a bind-pose render — what you see equals what
// gets committed to the model JSON.
// ============================================================
static int                 g_triPickerOpen        = 0;
static IDirect3DTexture9*  g_triPickerTex         = NULL;
static IDirect3DSurface9*  g_triPickerRT          = NULL;
static IDirect3DSurface9*  g_triPickerDS          = NULL;
static int                 g_triPickerW           = 640;
static int                 g_triPickerH           = 640;
static float               g_triPickerYaw         = 0.4f;
static float               g_triPickerPitch       = -0.25f;
static float               g_triPickerDist        = 2.5f;
static std::set<int>       g_triPickerSelTris;
static std::vector<uint32_t> g_triPickerSelVerts;
static std::vector<float>    g_triPickerSelOrigPos;
static uint32_t            g_triPickerSnapModelCrc = 0;
static float               g_triPickerDeltaXYZ[3] = {0.0f, 0.0f, 0.0f};
static char                g_triPickerStatusText[160] = {0};
// Cached camera matrices from the last RT render — used by click→ray.
static float               g_triPickerLastView[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static float               g_triPickerLastProj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static float               g_triPickerLastEye[3]   = {0.0f, 0.0f, -2.5f};
// Strip Baked Weapon sub-tool persistent state. Mirrors the same pattern
// as the merger fields. args gets memset each frame so we keep the latest
// picks here and re-seed args every pre-DrawFrame.
static int   g_stripSourceSelIdx = -1;
static char  g_stripSourceSelName[96] = {0};
static int   g_stripBoneSelected[128] = {0};
static char  g_stripNewName[96] = {0};
// Pending strip specs queued from "Strip & Queue" clicks. Each entry is a
// (new_name, source, bones[]) record. The save flow writes them all out as
// ze_strip_specs.json next to the PAK so the Python orchestrator can run
// apply_strip_specs before the merge step.
struct StripSpec {
    std::string new_name;
    std::string source;
    std::vector<std::string> bones;
};
static std::vector<StripSpec> s_pendingStripSpecs;
// Mesh Surgery panel persistent state. Same memset-survives-across-frames
// pattern as the other tool panels. The UV rect is what gets passed as the
// strip box when the user clicks Strip & Queue. Auto-find populates it via
// the spatial-outlier heuristic on the picked source model.
static int   g_meshSurgerySelSourceIdx = -1;
static char  g_meshSurgerySelSourceName[96] = {0};
static float g_meshSurgerySelectionUVRect[4] = {0,0,0,0};
static int   g_meshSurgerySelectedVertCount = 0;
static char  g_meshSurgeryNewName[96] = {0};
static char  g_meshSurgeryStatusText[256] = {0};

// Pending merge specs queued from "Bake" clicks. Each entry is one
// merged-character recipe; the save flow dumps the whole list out as
// ze_merged_models.json next to the PAK so the Python orchestrator can
// pick it up via --merged-models. Cleared after a successful save run.
struct CrowdBuilderMergeSpec {
    std::string newName;
    std::string baseName;
    std::string weaponName;
    std::string attachBone;
    float       offsetXYZ[3];
    float       offsetYPR[3];
    // Phase 2: embed-vs-newslot. embedMode=1 tells Python to bake the
    // weapon directly into base.slot[0] using embedUVRect as the atlas
    // region for the weapon's UVs. embedMode=0 = legacy new-slot path.
    int         embedMode;
    float       embedUVRect[4];
    // Phase 3: AUTO-SPAWN. THE FUCKING ANSWER. autoSpawn=1 tells the
    // Python pipeline to ALSO bolt a 3dCrowd item onto the level using
    // the baked model as key_main, dropping spawnCount instances around
    // spawnPos. Without this the bake is a write-only goddamn black
    // hole: JSON in models/, nothing in the world, user staring at the
    // viewport asking "where in the hell did my mesh go?" three times
    // an hour. With this checked the answer is "right where you were
    // looking, dickhead, hit reload". Default ON because the alternative
    // was years of confusion.
    int         autoSpawn;
    int         spawnCount;
    float       spawnPos[3];
};
static std::vector<CrowdBuilderMergeSpec> s_crowdBuilderMergeSpecs;

// IK chain UI caches (shared across command line parsing and UI build)
static std::vector<std::string> s_ikChainNamesStorage;
static std::vector<const char*> s_ikChainNames;
static std::vector<int> s_ikChainEnabledFlags;
static int s_ikSelectedChain = -1;

// Animation graph list (GameFiles/animations/graph_*.json)
static std::vector<std::string> s_graphListNamesStorage;
static std::vector<std::string> s_graphListPathsStorage;
static std::vector<const char*> s_graphListNames;
static std::vector<const char*> s_graphListPaths;

// Lua AnimTable list (GameFiles/lotrcparser)
static std::vector<std::string> s_luaListNamesStorage;
static std::vector<std::string> s_luaListPathsStorage;
static std::vector<const char*> s_luaListNames;
static std::vector<const char*> s_luaListPaths;

// Lua AnimationStatesTranslator list (GameFiles/lotrcparser)
static std::vector<std::string> s_luaStateListNamesStorage;
static std::vector<std::string> s_luaStateListPathsStorage;
static std::vector<const char*> s_luaStateListNames;
static std::vector<const char*> s_luaStateListPaths;

// Lua srclua browser (GameFiles/srclua - animation files)
static std::vector<std::string> s_luaSrcNamesStorage;
static std::vector<std::string> s_luaSrcPathsStorage;
static std::vector<std::string> s_luaSrcCatsStorage;
static std::vector<const char*> s_luaSrcNames;
static std::vector<const char*> s_luaSrcPaths;
static std::vector<const char*> s_luaSrcCats;

// Lua dependency tree (built when editor loads a file)
static std::vector<std::string> s_luaDepNames;
static std::vector<std::string> s_luaDepPaths;
static std::vector<std::string> s_luaDepTypes;
static std::vector<std::string> s_luaDepCats;
static std::vector<int>         s_luaDepParents;
static std::vector<int>         s_luaDepDepths;
static std::vector<int>         s_luaDepResolved;
static std::vector<const char*> s_luaDepNamesPtr;
static std::vector<const char*> s_luaDepPathsPtr;
static std::vector<const char*> s_luaDepTypesPtr;
static std::vector<const char*> s_luaDepCatsPtr;

// Lua AnimTable view data
static LuaAnimTableInfo s_luaAnimTableInfo;
static std::string s_luaAnimTablePath;
static std::vector<std::string> s_luaEntryKeysStorage;
static std::vector<std::string> s_luaEntryValuesStorage;
static std::vector<std::string> s_luaEntryPrimaryStorage;
static std::vector<std::string> s_luaEntrySourceStorage;
static std::vector<const char*> s_luaTableNames;
static std::vector<const char*> s_luaTableUsed;
static std::vector<const char*> s_luaEntryKeys;
static std::vector<const char*> s_luaEntryValues;
static std::vector<const char*> s_luaEntryPrimary;
static std::vector<const char*> s_luaEntrySources;
static std::vector<const char*> s_luaMissingClips;
static std::vector<const char*> s_luaMissingTables;
static std::string s_luaAnimTableLastError;

// Lua AnimationStatesTranslator view data
static LuaAnimStatesTranslatorInfo s_luaAnimStatesInfo;
static std::string s_luaAnimStatesPath;
static std::vector<std::string> s_luaStateKeysStorage;
static std::vector<std::string> s_luaStateNamesStorage;
static std::vector<std::string> s_luaStateStancesStorage;
static std::vector<std::string> s_luaStateActionsStorage;
static std::vector<const char*> s_luaStateKeys;
static std::vector<const char*> s_luaStateNames;
static std::vector<const char*> s_luaStateStances;
static std::vector<const char*> s_luaStateActions;
static std::string s_luaAnimStatesLastError;
static std::string s_luaEditorTargetPath;
static std::string s_luaEditorSourcePath;
static std::string s_luaEditorStatus;
static std::vector<char> s_luaEditorBuffer;
static int s_luaEditorBufferLen = 0;
static bool s_luaEditorOriginalBinary = false;
static std::string s_luaGuessedModelName;

static bool g_graphEnabledDesired = true;

// ─────────────────────────────────────────────────────────────────────────
//  RebuildCollisionWireEdges - the goddamn dedup pass
// ─────────────────────────────────────────────────────────────────────────
// Runs ONCE per collision-data load (or rebuild), gated behind g_mvWireDirty.
// Walks every triangle in g_mvCollIndices, hashes each of its three edges
// into a map keyed on (min(a,b), max(a,b)), and accumulates which faces
// share each edge. Output is g_mvWireEdges - one entry per UNIQUE edge
// with the average normal of its (up to two) adjacent faces baked in.
//
// Why we cache the average normal here instead of recomputing each frame:
// because the normal depends only on the geometry, not the camera, so
// it never fucking changes once the mesh is loaded. Computing it per
// frame would mean recomputing N triangle normals every render call for
// no reason whatsoever. Cache once, read forever.
//
// uint32 edge key encoding: (min_index << 16) | max_index. Works for
// meshes up to 65536 verts. Collision hulls in this game cap out around
// a few thousand verts so we are nowhere fucking near that limit. If
// some lunatic ever tries to load a 70k-vert collision mesh, the encoding
// will silently corrupt and the wireframe will look like an Escher
// drawing. At which point we have a bigger problem than wireframe quality.
static void RebuildCollisionWireEdges()
{
    g_mvWireEdges.clear();
    int numTris = (int)(g_mvCollIndices.size() / 3);
    if (numTris == 0) return;

    // Pre-compute per-triangle face normals. Once. Outside the hot loop.
    // Cross product of two triangle edges gives the unnormalized normal,
    // then normalize. Degenerate tris (zero area) get a zero normal which
    // contributes nothing useful to averaging - the other adjacent face
    // (if any) carries the back-face dim signal alone for that edge.
    std::vector<float> triNorms(numTris * 3, 0.0f);
    for (int t = 0; t < numTris; ++t) {
        uint16_t ia = g_mvCollIndices[t*3+0];
        uint16_t ib = g_mvCollIndices[t*3+1];
        uint16_t ic = g_mvCollIndices[t*3+2];
        const float* va = &g_mvCollVerts[ia*3];
        const float* vb = &g_mvCollVerts[ib*3];
        const float* vc = &g_mvCollVerts[ic*3];
        float ux = vb[0]-va[0], uy = vb[1]-va[1], uz = vb[2]-va[2];
        float vx = vc[0]-va[0], vy = vc[1]-va[1], vz = vc[2]-va[2];
        float nx = uy*vz - uz*vy;
        float ny = uz*vx - ux*vz;
        float nz = ux*vy - uy*vx;
        float len = sqrtf(nx*nx + ny*ny + nz*nz);
        if (len > 1e-6f) { nx/=len; ny/=len; nz/=len; }
        triNorms[t*3+0] = nx;
        triNorms[t*3+1] = ny;
        triNorms[t*3+2] = nz;
    }

    // Hash every edge. Map value = pair of face indices that share it.
    // -1 = no second face yet. After the loop, edges with f2=-1 are
    // boundary edges (cliff edges of the mesh) and only have one face's
    // normal contributing.
    typedef std::map<uint32_t, std::pair<int,int> > EdgeMap;
    EdgeMap edgeMap;
    for (int t = 0; t < numTris; ++t) {
        uint16_t i[3] = { g_mvCollIndices[t*3+0], g_mvCollIndices[t*3+1], g_mvCollIndices[t*3+2] };
        for (int e = 0; e < 3; ++e) {
            uint16_t a = i[e];
            uint16_t b = i[(e+1)%3];
            if (a == b) continue; // degenerate triangle, skip
            uint16_t lo = (a < b) ? a : b;
            uint16_t hi = (a < b) ? b : a;
            uint32_t key = ((uint32_t)lo << 16) | hi;
            EdgeMap::iterator it = edgeMap.find(key);
            if (it == edgeMap.end()) {
                edgeMap[key] = std::make_pair(t, -1);
            } else if (it->second.second == -1) {
                it->second.second = t;
            }
            // If both slots filled, this edge is shared by 3+ triangles
            // (non-manifold geometry, common in collision hulls). We just
            // drop the third+ contributor on the floor. The average of two
            // is plenty for back-face dim purposes.
        }
    }

    // Materialize the deduped edge list with averaged normals.
    g_mvWireEdges.reserve(edgeMap.size());
    for (EdgeMap::const_iterator it = edgeMap.begin(); it != edgeMap.end(); ++it) {
        CollisionEdge ce;
        ce.a = (uint16_t)(it->first >> 16);
        ce.b = (uint16_t)(it->first & 0xFFFF);
        int f1 = it->second.first;
        int f2 = it->second.second;
        float nx = triNorms[f1*3+0], ny = triNorms[f1*3+1], nz = triNorms[f1*3+2];
        if (f2 >= 0) {
            nx += triNorms[f2*3+0];
            ny += triNorms[f2*3+1];
            nz += triNorms[f2*3+2];
            float len = sqrtf(nx*nx + ny*ny + nz*nz);
            if (len > 1e-6f) { nx/=len; ny/=len; nz/=len; }
        }
        ce.nx = nx; ce.ny = ny; ce.nz = nz;
        g_mvWireEdges.push_back(ce);
    }
}

static void ParseAutoSpawnEffectFromCmdLine(const char* cmdLine)
{
    g_autoSpawnEffectName[0] = '\0';
    if (!cmdLine || !cmdLine[0])
        return;

    // Supported forms:
    //   -autospawnfx FX_AB_Captain_power_aura
    //   -autospawnfx=FX_AB_Captain_power_aura
    //   -autospawn_effect FX_AB_Captain_power_aura
    //   -autospawn_effect=FX_AB_Captain_power_aura
    // for test purposes
    const char* p = cmdLine;
    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            ++p;
        if (!*p) break;

        const char* tokenStart = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            ++p;
        const char* tokenEnd = p;
        const int tokenLen = (int)(tokenEnd - tokenStart);
        if (tokenLen <= 0)
            continue;

        const char* value = NULL;
        const int k1Len = 11; // "autospawnfx"
        const int k2Len = 14; // "autospawn_effect"

        // Normalize match: leading '-' then key then optional '='.
        if (tokenLen >= 1 + k1Len && tokenStart[0] == '-' && _strnicmp(tokenStart + 1, "autospawnfx", k1Len) == 0)
        {
            const char* after = tokenStart + 1 + k1Len;
            if (after < tokenEnd && *after == '=')
            {
                value = after + 1;
            }
            else if (after == tokenEnd)
            {
                // Value is next token.
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                    ++p;
                value = p;
            }
        }
        else if (tokenLen >= 1 + k2Len && tokenStart[0] == '-' && _strnicmp(tokenStart + 1, "autospawn_effect", k2Len) == 0)
        {
            const char* after = tokenStart + 1 + k2Len;
            if (after < tokenEnd && *after == '=')
            {
                value = after + 1;
            }
            else if (after == tokenEnd)
            {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                    ++p;
                value = p;
            }
        }

        if (value && value[0])
        {
            // Copy until whitespace or buffer end.
            int out = 0;
            while (value[out] && value[out] != ' ' && value[out] != '\t' && value[out] != '\r' && value[out] != '\n')
            {
                if (out >= (int)sizeof(g_autoSpawnEffectName) - 1)
                    break;
                g_autoSpawnEffectName[out] = value[out];
                out++;
            }
            g_autoSpawnEffectName[out] = '\0';
            return;
        }
    }

    // IK chain names/flags
    if (g_renderer)
    {
        int chainCount = g_renderer->getIKChainCount();
        if (chainCount > 0)
        {
            s_ikChainNamesStorage.reserve((size_t)chainCount);
            s_ikChainNames.reserve((size_t)chainCount);
            s_ikChainEnabledFlags.reserve((size_t)chainCount);
            for (int i = 0; i < chainCount; ++i)
            {
                const char* name = g_renderer->getIKChainName(i);
                s_ikChainNamesStorage.push_back(name ? name : "");
                s_ikChainEnabledFlags.push_back(g_renderer->getIKChainEnabled(i) ? 1 : 0);
            }
            for (size_t i = 0; i < s_ikChainNamesStorage.size(); ++i)
            {
                s_ikChainNames.push_back(s_ikChainNamesStorage[i].c_str());
            }
        }
    }
}

// Legacy Win32 UI toggle — kept around because ripping it out breaks 14 other things
static bool g_showLegacyUI = true; // F9 toggles legacy Win32 panels when ImGui is available.

// Camera state
static bool g_isResizing   = false;  // true while user is dragging the window border
static bool g_resizePending = false; // deferred resize to avoid double D3D9 Reset
static int  g_pendingResizeW = 0;
static int  g_pendingResizeH = 0;
static bool g_leftMouseDown = false;
static bool g_middleMouseDown = false;
static bool g_rightMouseDown = false;
static bool g_leftMouseClicked = false;  // set in WndProc, consumed in RenderFrame
static bool g_rightMouseClicked = false; // set in WndProc, consumed in RenderFrame (entity creation)
static int g_lastMouseX = 0;
static int g_lastMouseY = 0;
static bool g_boneDragActive = false;
static bool g_boneTranslateActive = false;
static bool g_editMode = false; // Phase 2: persistent bone edit mode
static int g_dragAxisLock = 0;  // 0=free,1=X,2=Y,3=Z (picked at drag start)

// Keyboard state
static bool g_keys[256] = {false};
static int g_gameAnimIndex = -1;
static int g_blendAnimIndex = -1; // Secondary JSON clip for pose blending (Phase 2)
static bool g_showBrowser = false;
static bool g_showAssetInspector = false; // F6: Asset Data Inspector panel
static int  g_assetInspCat = -1;  // cached DLL request: category to inspect
static int  g_assetInspIdx = -1;  // cached DLL request: index to inspect
static int  g_lastValidationErrors = 0;
static int  g_lastValidationWarnings = 0;
static bool g_showOverlay = true;
static bool g_showHelp = true;
static int g_browserMode = 0; // 0 = model (jmodel), 1 = animation
static int g_browserIndexModel = 0;
static int g_browserIndexAnim = 0;
static int g_browserIndexEffect = 0;
static char g_activeModelName[128] = "";
static char g_activeAnimName[128] = "";

// ---------------------------------------------------------------------------
// Persistent editor state (saved to Scene3DEditorState.ini next to the EXE)
// ---------------------------------------------------------------------------
static bool g_editorStateLoaded = false;
static char g_editorStateIniPath[MAX_PATH] = "";
static char g_imguiIniPath[MAX_PATH] = "";
static int  g_persistShowLegacyUI = -1; // -1 = not set in INI

static char g_persistModelName[128] = "";
static char g_persistAnimName[128] = "";
static char g_persistEffectName[128] = "";

static float g_persistPlaybackSpeed = 1.0f;
static int   g_persistLoopEnabled = 1;
static int   g_persistPaused = 0;
static float g_persistAnimTime = 0.0f;

static int   g_persistRecording = 0;
static int   g_persistInterpMode = 0;
static int   g_persistGizmoMode = 0;
static int   g_persistGizmoSpace = 0;
static int   g_persistRotSnapEnabled = 1;
static float g_persistRotSnapDegrees = 15.0f;
static int   g_persistMoveSnapEnabled = 1;
static float g_persistMoveSnapUnits = 0.05f;
static float g_persistEditorTimelineDuration = 0.0f;

// Theme colors (mutable for dark/light toggle)
static COLORREF kClrBg        = RGB(0x1a, 0x1d, 0x22);
static COLORREF kClrPanel     = RGB(0x20, 0x24, 0x2b);
static COLORREF kClrText      = RGB(0xe6, 0xea, 0xf0);
static COLORREF kClrTextDim   = RGB(0x9a, 0xa5, 0xb5);
static COLORREF kClrAccent    = RGB(0x4c, 0xd0, 0xff);
static COLORREF kClrBgTop     = RGB(0x1b, 0x1e, 0x24);
static COLORREF kClrBgBot     = RGB(0x16, 0x18, 0x1d);
static COLORREF kClrPanelTop  = RGB(0x21, 0x24, 0x2c);
static COLORREF kClrPanelBot  = RGB(0x1c, 0x20, 0x27);
static COLORREF kClrHighlight = RGB(0xff, 0xff, 0xff);
static HBRUSH g_brBg    = NULL;
static HBRUSH g_brPanel = NULL;
static HFONT g_uiFont   = NULL;
static bool g_darkTheme = true;
static void InvalidateAllTheme();
static void ApplyTheme(bool dark);
static float g_timelineZoom = 1.0f; // 1 = fit full duration
static int g_hoveredEventIndex = -1; // event index under mouse in timeline (-1 = none)
static int g_imguiViewportX = 0;
static int g_imguiViewportY = 0;
static int g_imguiViewportW = 0;
static int g_imguiViewportH = 0;

// GUI control IDs
#define IDC_LIST_MODELS     1001
#define IDC_LIST_ANIMS      1002
#define IDC_BTN_LOAD_MODEL  1003
#define IDC_BTN_LOAD_ANIM   1004
#define IDC_BTN_RESCAN      1005
#define IDC_BTN_PLAY        1006
#define IDC_BTN_STOP        1007
#define IDC_BTN_PREV        1008
#define IDC_BTN_NEXT        1009
#define IDC_TRACK_TIME      1010
#define IDC_STATIC_TIME     1011
#define IDC_STATIC_STATUS   1012
#define IDC_STATIC_MODEL    1013
#define IDC_STATIC_ANIM     1014
#define IDC_STATIC_MAT      1015
#define IDC_STATIC_TEX      1016
#define IDC_STATIC_WARN     1017
#define IDC_LIST_MATERIALS  1018
#define IDC_BTN_FRAME_PREV  1050
#define IDC_BTN_FRAME_NEXT  1051
#define IDC_BTN_FIRST_KEY   1052
#define IDC_BTN_LAST_KEY    1053
#define IDC_EDIT_MODEL_FILTER 1015
#define IDC_EDIT_ANIM_FILTER  1016
#define IDC_LIST_BONES        1017
#define IDC_CHECK_LOOP        1018
#define IDC_SLIDER_SPEED      1019
#define IDC_STATIC_SPEED      1020
#define IDC_BTN_PRESET_SET_A  1021
#define IDC_BTN_PRESET_LOAD_A 1022
#define IDC_BTN_PRESET_SET_B  1023
#define IDC_BTN_PRESET_LOAD_B 1024
#define IDC_BTN_RECORD        1025
#define IDC_EDIT_EXPORT       1026
#define IDC_BTN_EXPORT        1027
#define IDC_EDIT_TIMELINE_SEC 1028
#define IDC_BTN_TIMELINE_SET  1029
#define IDC_CHECK_ROT_SNAP    1030
#define IDC_EDIT_ROT_SNAP     1031
#define IDC_CHECK_MOVE_SNAP   1032
#define IDC_EDIT_MOVE_SNAP    1033
#define IDC_CHECK_INTERP_LIN  1034
#define IDC_BTN_EDIT_COMMIT   1035
#define IDC_BTN_EDIT_CANCEL   1036
#define IDC_EDIT_NUM_RX       1037
#define IDC_EDIT_NUM_RY       1038
#define IDC_EDIT_NUM_RZ       1039
#define IDC_EDIT_NUM_TX       1040
#define IDC_EDIT_NUM_TY       1041
#define IDC_EDIT_NUM_TZ       1042
#define IDC_BTN_NUM_READ      1043
#define IDC_BTN_NUM_KEY       1044
#define IDC_BTN_SET_KEY       1045
#define IDC_BTN_TOGGLE_LEFT   1060
#define IDC_BTN_TOGGLE_RIGHT  1061
#define IDC_BTN_TOGGLE_TL     1062
#define IDC_LIST_EFFECTS      1070
#define IDC_BTN_SPAWN_EFFECT  1071

// Timeline context menu IDs
#define IDM_ADD_SOUND_EVENT   2001
#define IDM_ADD_SOUND_CUE     2002
#define IDM_DELETE_EVENT      2003

// GUI handles
static HWND g_listModels = NULL;
static HWND g_listAnims = NULL;
static HWND g_btnLoadModel = NULL;
static HWND g_btnLoadAnim = NULL;
static HWND g_btnRescan = NULL;
static HWND g_btnPlay = NULL;
static HWND g_btnStop = NULL;
static HWND g_btnPrev = NULL;
static HWND g_btnNext = NULL;
static HWND g_btnFramePrev = NULL;
static HWND g_btnFrameNext = NULL;
static HWND g_btnFirstKey = NULL;
static HWND g_btnLastKey = NULL;
static HWND g_trackTime = NULL;
static HWND g_textTime = NULL;
static HWND g_textStatus = NULL;
static HWND g_textModel = NULL;
static HWND g_textAnim = NULL;
static HWND g_textMat = NULL;
static HWND g_textTex = NULL;
static HWND g_textWarn = NULL;
static HWND g_listMaterials = NULL;
static HWND g_labelModels = NULL;
static HWND g_labelAnims = NULL;
static HWND g_labelBones = NULL;
static HWND g_labelEffects = NULL;
static HWND g_listEffects = NULL;
static HWND g_btnSpawnEffect = NULL;
static bool g_trackDragging = false;
static std::vector<HTREEITEM> g_boneTreeItems;
static HWND g_editModelFilter = NULL;
static HWND g_editAnimFilter = NULL;
static HWND g_listBones = NULL;
static HWND g_checkLoop = NULL;
static HWND g_sliderSpeed = NULL;
static HWND g_textSpeed = NULL;
static HWND g_btnPresetSetA = NULL;
static HWND g_btnPresetLoadA = NULL;
static HWND g_btnPresetSetB = NULL;
static HWND g_btnPresetLoadB = NULL;
static HWND g_btnRecord = NULL;
static HWND g_editExportName = NULL;
static HWND g_btnExport = NULL;
static HWND g_editTimelineSec = NULL;
static HWND g_btnTimelineSet = NULL;
static HWND g_checkRotSnap = NULL;
static HWND g_editRotSnap = NULL;
static HWND g_checkMoveSnap = NULL;
static HWND g_editMoveSnap = NULL;
static HWND g_checkInterpLin = NULL;
static HWND g_btnEditCommit = NULL;
static HWND g_btnEditCancel = NULL;
static HWND g_editNumRx = NULL;
static HWND g_editNumRy = NULL;
static HWND g_editNumRz = NULL;
static HWND g_editNumTx = NULL;
static HWND g_editNumTy = NULL;
static HWND g_editNumTz = NULL;
static HWND g_btnNumRead = NULL;
static HWND g_btnNumKey = NULL;
static HWND g_btnSetKey = NULL;
static HWND g_btnToggleLeft = NULL;
static HWND g_btnToggleRight = NULL;
static HWND g_btnToggleTimeline = NULL;

static bool g_leftCollapsed = true;
static bool g_rightCollapsed = false;  // Changed to false so right panel is visible by default
static bool g_timelineCollapsed = true;

static char g_filterModel[64] = "";
static char g_filterAnim[64] = "";
static std::vector<int> g_modelListMap;
static std::vector<int> g_animListMap;
static std::vector<std::string> g_effectsList;
static int g_presetModel[2] = {-1, -1};
static int g_presetAnim[2] = {-1, -1};
static char g_exportName[64] = "Custom_Anim";
static char g_timelineSec[32] = "0";
static char g_rotSnapDegText[32] = "15";
static char g_moveSnapText[32] = "0.05";
static char g_numRxText[32] = "0";
static char g_numRyText[32] = "0";
static char g_numRzText[32] = "0";
static char g_numTxText[32] = "0";
static char g_numTyText[32] = "0";
static char g_numTzText[32] = "0";

static bool g_guiCreated = false;

static void SetLeftPanelVisible(bool visible)
{
    int cmd = visible ? SW_SHOW : SW_HIDE;
    HWND leftControls[] = {
        g_labelModels, g_editModelFilter, g_listModels, g_btnLoadModel,
        g_labelAnims, g_editAnimFilter, g_listAnims, g_btnLoadAnim, g_btnRescan,
        g_btnPresetSetA, g_btnPresetLoadA, g_btnPresetSetB, g_btnPresetLoadB,
        g_textMat, g_textTex, g_textWarn, g_listMaterials
    };
    for (size_t i = 0; i < sizeof(leftControls)/sizeof(leftControls[0]); ++i)
    {
        if (leftControls[i]) ShowWindow(leftControls[i], cmd);
    }
}

static void SetRightPanelVisible(bool visible)
{
    int cmd = visible ? SW_SHOW : SW_HIDE;
    HWND rightControls[] = {
        g_textModel, g_textAnim,
        g_btnFirstKey, g_btnFramePrev, g_btnPlay, g_btnStop, g_btnFrameNext, g_btnLastKey,
        g_btnRecord, g_btnSetKey, g_btnPrev, g_btnNext, g_checkLoop, g_textSpeed,
        g_trackTime, g_textTime, g_sliderSpeed,
        g_editTimelineSec, g_btnTimelineSet, g_editExportName, g_btnExport,
        g_checkRotSnap, g_editRotSnap, g_checkMoveSnap, g_editMoveSnap,
        g_checkInterpLin, g_btnEditCommit, g_btnEditCancel,
        g_editNumRx, g_editNumRy, g_editNumRz, g_editNumTx, g_editNumTy, g_editNumTz,
        g_btnNumRead, g_btnNumKey,
        g_labelBones, g_listBones
    };
    for (size_t i = 0; i < sizeof(rightControls)/sizeof(rightControls[0]); ++i)
    {
        if (rightControls[i]) ShowWindow(rightControls[i], cmd);
    }
}

//
// Forward Declarations
//

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK RenderProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TimelineProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static void CommitCurrentEdit();
static void CancelCurrentEdit();
void InitializeHavok();
void ShutdownHavok();
void InitializeScene();
void UpdateFrame(float deltaTime);
void RenderFrame(float deltaTime);
void UpdatePerformanceMetrics(float deltaTime);
void DrawPerformanceOverlay(HDC hdc);
void LoadGameFiles();
void SelectGameAnimation(int index);
void SelectBlendAnimation(int index);
JsonAnimClip* LoadJsonAnimClip(const char* path, const hkaSkeleton* skeleton, int decodeMode, int type2PackingMode);
void SelectGameModel(int index);
void DrawAssetBrowserOverlay(HDC hdc);
void UpdateBrowserSelection(int delta);
void CreateGui(HWND hwnd);
void LayoutGui(int width, int height);
void RefreshGameFilesUI();
void RefreshEffectsList();
static void RefreshGraphList();
static void RefreshLuaScriptList();
static void RefreshLuaStateScriptList();
static void RefreshLuaSrcFileList();
void UpdateGuiPlayback();
void SyncGuiSelectionFromState();
void CreateViewportWindow(HWND parent);
void RefreshBoneList();
void PreloadSkyboxes();
static void ApplyEnvMashPreset(int mashId);
LRESULT CALLBACK TimelineProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void BeginImGuiFrame();
void DrawDockspaceAndPanels();
void RenderImGuiOverlayPass();
  static bool BuildGraphFromCurrentAnimation(char* outPath, int outSize);
  static bool LoadLuaAnimTableFromPath(const char* path);
  static bool LoadLuaAnimStatesFromPath(const char* path);
  static bool GuessAndLoadModelForLuaTable();
static void PlayLuaAnimClipByName(const char* clipName);
static bool LoadLuaEditorFromPath(const char* path);
static bool SaveLuaEditorToTarget();
static void BuildLuaDepTree(const char* rootPath);

static void LoadEditorState();
static void SaveEditorState();
static void ResolvePersistedSelections();
static void ApplyPersistedRendererState();
static const char* GetImGuiIniPath();

static void DrawOverlayLine(HDC hdc, int x, int& y, int right, const char* text)
{
    RECT rect = {x, y, right, y + 18};
    DrawText(hdc, text, -1, &rect, DT_LEFT | DT_TOP);
    y += 18;
}

static const char* AxisLockLabel(int axis)
{
    if (axis == Scene3DRenderer::AXIS_X) return "X";
    if (axis == Scene3DRenderer::AXIS_Y) return "Y";
    if (axis == Scene3DRenderer::AXIS_Z) return "Z";
    return "Free";
}

static const char* GizmoModeLabel(int mode)
{
    return (mode == Scene3DRenderer::GIZMO_TRANSLATE) ? "Move" : "Rotate";
}

static const char* GizmoSpaceLabel(int space)
{
    return (space == Scene3DRenderer::GIZMO_WORLD) ? "World" : "Local";
}

//
// WinMain - Application Entry Point
//

// Debug log
static FILE* g_logFile = NULL;
void DebugLog(const char* msg) {
    if (!g_logFile) g_logFile = fopen("debug.log", "w");
    if (g_logFile) { fprintf(g_logFile, "%s\n", msg); fflush(g_logFile); }
}

// printf-style cousin. Adds a single newline. 1KB buffer, anything longer
// gets clipped, which is fine for diagnostic prints.
void DebugLogf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = 0;
    DebugLog(buf);
}

// ============================================================================
// Kit creation helpers (VS2005-safe — no lambdas)
// ============================================================================

// Context for kit creation helpers — avoids passing many parameters
struct KitCtx {
    ZeroEngine::LevelReader* reader;
    int gmm;
    uint32_t layGuid;
    std::string prefix;
};

// Helper: find a type def index by name (VS2005-safe, no lambda)
static int FindTypeDefByName(const ZeroEngine::LevelReader& reader, const char* name) {
    const std::vector<ZeroEngine::LevelGameObjTypeDef>& types = reader.GetGameObjTypes();
    for (int t = 0; t < (int)types.size(); ++t) {
        if (types[t].name == name) return t;
    }
    return -1;
}

static uint32_t KitAddEntity(KitCtx& ctx, ZeroEngine::PendingGameObj& e,
                             int tdefIdx, const std::string& name,
                             float px, float py, float pz) {
    e.type_def_index = tdefIdx;
    e.gamemodemask = ctx.gmm;
    e.layer_guid = ctx.layGuid;
    e.name_str = name;
    e.name_crc = ZeroEngine::LotrHashString(name.c_str());
    e.world_transform[0] = e.world_transform[5] = e.world_transform[10] = e.world_transform[15] = 1.0f;
    e.world_transform[12] = px; e.world_transform[13] = py; e.world_transform[14] = pz;
    ctx.reader->AddPendingEntity(e);
    const std::vector<ZeroEngine::LevelGameObjEntry>& gos = ctx.reader->GetGameObjs();
    return gos.empty() ? 0u : gos.back().guid;
}

static uint32_t KitMakeEntity(KitCtx& ctx, int tdefIdx, const std::string& name, float px, float py, float pz) {
    ZeroEngine::PendingGameObj e;
    return KitAddEntity(ctx, e, tdefIdx, name, px, py, pz);
}

static void KitCreateSpawnChain(KitCtx& ctx, int team, float sx, float sy, float sz) {
    int seIdx = FindTypeDefByName(*ctx.reader, "spawn_emitter");
    int scIdx = FindTypeDefByName(*ctx.reader, "spawn_class");
    int spIdx = FindTypeDefByName(*ctx.reader, "spawn_point");
    int snIdx = FindTypeDefByName(*ctx.reader, "spawn_node");
    int dcIdx = FindTypeDefByName(*ctx.reader, "demo_camera");
    char t[8]; _snprintf(t, sizeof(t), "T%d", team);
    std::vector<uint32_t> classGuids;
    std::vector<uint32_t> pointGuids;
    static const uint32_t kTeam1Classes[4] = { 55008412u, 55009003u, 55008642u, 55008752u };
    static const uint32_t kTeam2Classes[4] = { 55008415u, 55009007u, 55008643u, 55008753u };
    static const char* kClassNames[4] = { "Warrior", "Archer", "Scout", "Mage" };
    if (scIdx >= 0) {
        const uint32_t* cls = (team == 1) ? kTeam1Classes : kTeam2Classes;
        for (int ci = 0; ci < 4; ++ci) {
            ZeroEngine::PendingGameObj sc;
            sc.intOverrides[LC_FIELD_CHARACTERCLASS] = cls[ci];
            uint32_t cg = KitAddEntity(ctx, sc, scIdx,
                ctx.prefix + "_SC_" + t + "_" + kClassNames[ci], sx, sy, sz);
            if (cg != 0) classGuids.push_back(cg);
        }
    }
    if (spIdx >= 0) {
        ZeroEngine::PendingGameObj sp;
        sp.intOverrides[LC_FIELD_TEAM] = (uint32_t)team;
        uint32_t pg = KitAddEntity(ctx, sp, spIdx, ctx.prefix + "_SP_" + t, sx, sy, sz);
        if (pg != 0) pointGuids.push_back(pg);
    }
    if (snIdx >= 0) {
        float off[4][2] = {{-3,0},{3,0},{0,-3},{0,3}};
        for (int n = 0; n < 4; ++n) {
            char nb[64]; _snprintf(nb, sizeof(nb), "%s_SN_%s_%d", ctx.prefix.c_str(), t, n+1);
            ZeroEngine::PendingGameObj sn;
            sn.intOverrides[LC_FIELD_TEAM] = (uint32_t)team;
            KitAddEntity(ctx, sn, snIdx, nb, sx + off[n][0], sy, sz + off[n][1]);
        }
    }
    if (dcIdx >= 0) KitMakeEntity(ctx, dcIdx, ctx.prefix + "_CAM_" + t + "_Demo", sx, sy + 5.0f, sz);
    if (seIdx >= 0) {
        ZeroEngine::PendingGameObj se;
        se.intOverrides[LC_FIELD_TEAM] = (uint32_t)team;
        if (!pointGuids.empty()) se.listOverrides[LC_FIELD_SPAWN_POINTS] = pointGuids;
        if (!classGuids.empty()) se.listOverrides[LC_FIELD_SPAWN_CLASSES] = classGuids;
        KitAddEntity(ctx, se, seIdx, ctx.prefix + "_EMIT_" + t + "_Main", sx, sy, sz);
    }
}

static void KitCreateScoring(KitCtx& ctx, float sx, float sy, float sz) {
    int pmIdx = FindTypeDefByName(*ctx.reader, "PointManager");
    int leIdx = FindTypeDefByName(*ctx.reader, "logic_endgame");
    int gsIdx = FindTypeDefByName(*ctx.reader, "logic_gamestart");
    if (pmIdx >= 0) {
        KitMakeEntity(ctx, pmIdx, ctx.prefix + "_PM_Team1", sx, sy, sz);
        KitMakeEntity(ctx, pmIdx, ctx.prefix + "_PM_Team2", sx, sy, sz);
    }
    if (leIdx >= 0) KitMakeEntity(ctx, leIdx, ctx.prefix + "_EndGame", sx, sy, sz);
    if (gsIdx >= 0) KitMakeEntity(ctx, gsIdx, ctx.prefix + "_GameStart", sx, sy, sz);
}

static void KitCreateGamemode(KitCtx& ctx, int modeType) {
    int gmTIdx = FindTypeDefByName(*ctx.reader, "gamemode");
    if (gmTIdx < 0 || modeType < 0 || modeType >= LC_GamemodeTableCount) return;
    const LC_GamemodeDefaults& gd = LC_GamemodeTable[modeType];

    ZeroEngine::PendingGameObj gm;
    gm.type_def_index = gmTIdx;
    gm.parent_guid = 0;
    gm.gamemodemask = -1;
    gm.layer_guid = 0;  // gamemodes always sit at root (layer=0)
    std::string gmName = ctx.prefix + "_MODE_" + gd.modeName;
    for (size_t c = 0; c < gmName.size(); ++c) if (gmName[c]==' ') gmName[c]='_';
    gm.name_str = gmName;
    gm.name_crc = ZeroEngine::LotrHashString(gmName.c_str());
    gm.world_transform[0]=gm.world_transform[5]=gm.world_transform[10]=gm.world_transform[15]=1.0f;

    gm.stringOverrides[LC_GM_MODE] = gd.modeString;
    gm.stringOverrides[LC_GM_MODENAME] = gd.modeName;
    if (gd.matchStartMsg[0]) gm.stringOverrides[LC_GM_MATCHSTARTMSG] = gd.matchStartMsg;
    if (gd.matchStartVO[0])  gm.stringOverrides[LC_GM_MATCHSTARTVO] = gd.matchStartVO;
    if (gd.t1CPCompleteVO[0]) gm.stringOverrides[LC_GM_TEAM1CPCOMPLETEVO] = gd.t1CPCompleteVO;
    if (gd.t2CPCompleteVO[0]) gm.stringOverrides[LC_GM_TEAM2CPCOMPLETEVO] = gd.t2CPCompleteVO;
    if (gd.rbFrodoBirthEffect[0]) gm.stringOverrides[LC_GM_RB_FRODOBIRTHEFFECT] = gd.rbFrodoBirthEffect;
    gm.stringOverrides[LC_GM_INTRO] = "";
    gm.stringOverrides[LC_GM_OUTTRO] = "";
    gm.intOverrides[LC_GM_ATMOSPHERE] = 0;
    gm.intOverrides[LC_GM_ATMOSPHERE_LOW] = 0;
    gm.intOverrides[LC_GM_MAPNW] = 0;
    gm.intOverrides[LC_GM_MAPSE] = 0;
    gm.intOverrides[LC_GM_INTROCAMERA] = 0;
    gm.intOverrides[LC_GM_MUSIC] = 0;
    gm.intOverrides[LC_GM_MP_MINPLAYERCOUNT] = (uint32_t)gd.mpMinPlayerCount;
    gm.floatOverrides[LC_GM_MP_WARMUPTIME] = gd.mpWarmupTime;
    gm.floatOverrides[LC_GM_MP_SPAWNDELAY] = gd.mpSpawnDelay;
    gm.floatOverrides[LC_GM_MP_SPAWNWINDOW] = gd.mpSpawnWindow;
    gm.floatOverrides[LC_GM_CQ_UPDATEDELAY] = gd.cqUpdateDelay;
    gm.intOverrides[LC_GM_CQ_T1_TRICKLERATE] = (uint32_t)gd.cqT1TrickleRate;
    gm.intOverrides[LC_GM_CQ_T2_TRICKLERATE] = (uint32_t)gd.cqT2TrickleRate;
    gm.intOverrides[LC_GM_CQ_T1_PERPOINTRATE] = (uint32_t)gd.cqT1PerPointRate;
    gm.intOverrides[LC_GM_CQ_T2_PERPOINTRATE] = (uint32_t)gd.cqT2PerPointRate;
    gm.intOverrides[LC_GM_RB_VICTORYPOINTS] = (uint32_t)gd.rbVictoryPoints;
    gm.intOverrides[LC_GM_RB_POINTSPERSECOND] = (uint32_t)gd.rbPointsPerSecond;
    gm.intOverrides[LC_GM_RB_POINTSPERKILL] = (uint32_t)gd.rbPointsPerKill;
    gm.intOverrides[LC_GM_USESAI] = (uint32_t)gd.usesAI;
    gm.intOverrides[LC_GM_MAXAISPAWNTEAM1] = (uint32_t)gd.maxAITeam1;
    gm.intOverrides[LC_GM_MAXAISPAWNTEAM2] = (uint32_t)gd.maxAITeam2;
    gm.floatOverrides[LC_GM_AITEAM1DIFFICULTY] = gd.aiDifficulty;
    gm.floatOverrides[LC_GM_AITEAM2DIFFICULTY] = gd.aiDifficulty;
    gm.floatOverrides[LC_GM_AITEAM1DIFFINC] = 0.0f;
    gm.floatOverrides[LC_GM_AITEAM2DIFFINC] = 0.0f;
    gm.intOverrides[LC_GM_SHOWPOINTS] = (uint32_t)gd.showPoints;
    gm.intOverrides[LC_GM_AUTOSELECTSPAWNPOINT] = (uint32_t)gd.autoSelectSpawnPoint;
    gm.intOverrides[LC_GM_AUTOSELECTCLASS] = (uint32_t)gd.autoSelectClass;

    // Set Layers to include the kit's templateLayer
    if (ctx.layGuid != 0)
        gm.layerGuids.push_back(ctx.layGuid);

    ctx.reader->AddPendingEntity(gm);
}

// ============================================================================
// Forward declarations for functions defined below
static bool RunSubprocess(const char* cmdLine, int timeoutMs, const char* workDir = NULL);
static std::string FindLotrcRsExe();
static bool RustParserSanitize(const std::string& pakPath, const std::string& binPath);

// ================================================================
// RustParserSaveLevel — Full save flow through the Rust parser.
//   1. Dump the ORIGINAL (clean) PAK to JSON via lotrc_rs
//   2. Patch level.json with pending entities from C++
//   3. Compile patched JSON back to clean PAK via lotrc_rs
//   4. Copy output files to the _modified path
//
// This bypasses the C++ SavePak binary rebuilder entirely,
// producing game-ready PAK files through the Rust parser's
// proven compile path.
// ================================================================
// Write pending entities as JSON for level_patcher.py
static void WriteJsonString(FILE* f, const std::string& s)
{
    fputc('"', f);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\\': fputs("\\\\", f); break;
        case '"':  fputs("\\\"", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 32) fprintf(f, "\\u%04X", (unsigned)c);
            else fputc(c, f);
            break;
        }
    }
    fputc('"', f);
}

static bool WritePendingEntitiesJson(const std::string& path) {
    const std::vector<ZeroEngine::PendingGameObj>& pending = g_levelReader.GetPendingObjs();
    if (pending.empty()) return true; // nothing to write

    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;

    fprintf(f, "[\n");
    for (size_t pi = 0; pi < pending.size(); ++pi) {
        const ZeroEngine::PendingGameObj& p = pending[pi];
        // Get type name
        std::string typeName = "unknown";
        if (p.type_def_index >= 0 && p.type_def_index < (int)g_levelReader.GetGameObjTypes().size())
            typeName = g_levelReader.GetGameObjTypes()[p.type_def_index].name;

        if (pi > 0) fprintf(f, ",\n");
        fprintf(f, "  {\n");
        fprintf(f, "    \"type\": ");
        WriteJsonString(f, typeName);
        fprintf(f, ",\n");
        fprintf(f, "    \"layer\": %u,\n", p.layer_guid);
        fprintf(f, "    \"fields\": {\n");

        // Write fields from type definition, injecting PendingGameObj struct
        // values for standard fields and applying overrides
        if (p.type_def_index >= 0 && p.type_def_index < (int)g_levelReader.GetGameObjTypes().size()) {
            const ZeroEngine::LevelGameObjTypeDef& td = g_levelReader.GetGameObjTypes()[p.type_def_index];
            for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                const ZeroEngine::GOFieldDefEntry& fd = td.fields[fi];
                ZeroEngine::FieldKind fk = ZeroEngine::FieldKindFromCrc(fd.kind_crc);

                std::string fieldName = g_levelReader.ResolveCrc(fd.name_crc);
                if (fieldName.empty()) {
                    char fb[16]; sprintf(fb, "0x%08X", fd.name_crc);
                    fieldName = fb;
                }

                if (fi > 0) fprintf(f, ",\n");
                fprintf(f, "      ");
                WriteJsonString(f, fieldName);
                fprintf(f, ": ");

                // ── Check for standard field overrides from PendingGameObj struct ──
                // Use case-insensitive compare — type defs vary ("GUID" vs "guid", "Name" vs "name")
                bool handled = false;
                if (_stricmp(fieldName.c_str(), "GUID") == 0) {
                    fprintf(f, "%u", p.guid); handled = true;
                } else if (_stricmp(fieldName.c_str(), "ParentGUID") == 0) {
                    fprintf(f, "%u", p.parent_guid); handled = true;
                } else if (_stricmp(fieldName.c_str(), "GameModeMask") == 0) {
                    fprintf(f, "%d", p.gamemodemask); handled = true;
                } else if (_stricmp(fieldName.c_str(), "Name") == 0) {
                    std::string resolved = g_levelReader.ResolveCrc(p.name_crc);
                    if (!resolved.empty()) WriteJsonString(f, resolved);
                    else if (p.name_crc == 0) WriteJsonString(f, "");
                    else {
                        char nb[16];
                        sprintf_s(nb, "0x%08X", p.name_crc);
                        WriteJsonString(f, nb);
                    }
                    handled = true;
                } else if (_stricmp(fieldName.c_str(), "WorldTransform") == 0) {
                    fprintf(f, "[");
                    for (int mi = 0; mi < 16; ++mi)
                        fprintf(f, "%.7g%s", p.world_transform[mi], mi<15?", ":"");
                    fprintf(f, "]");
                    handled = true;
                } else if (_stricmp(fieldName.c_str(), "Transform") == 0) {
                    // Transform is PARENT-RELATIVE. Compute local = world × inverse(parent_world).
                    // For roots (parent_guid == 0) local == world, so fall through with world_transform.
                    float localT[16];
                    memcpy(localT, p.world_transform, 64);
                    if (p.parent_guid != 0) {
                        const float* P = NULL;
                        for (size_t pj = 0; pj < pending.size(); ++pj) {
                            if (pending[pj].guid == p.parent_guid) {
                                P = pending[pj].world_transform;
                                break;
                            }
                        }
                        if (P) {
                            // Rigid-transform inverse (row-major, t at indices 12-14):
                            //   M = [R | 0]
                            //       [t | 1]
                            //   M^-1 = [R^T       | 0]
                            //          [-t·R^T    | 1]
                            // So local_t = (t_child - t_parent) × R_parent^T
                            //    local_R = R_child × R_parent^T
                            const float* W = p.world_transform;
                            float dx = W[12] - P[12], dy = W[13] - P[13], dz = W[14] - P[14];
                            localT[12] = dx*P[0] + dy*P[1] + dz*P[2];
                            localT[13] = dx*P[4] + dy*P[5] + dz*P[6];
                            localT[14] = dx*P[8] + dy*P[9] + dz*P[10];
                            float out[9];
                            for (int ii = 0; ii < 3; ++ii) {
                                for (int jj = 0; jj < 3; ++jj) {
                                    float s = 0;
                                    for (int kk = 0; kk < 3; ++kk)
                                        s += W[ii*4 + kk] * P[jj*4 + kk];
                                    out[ii*3 + jj] = s;
                                }
                            }
                            localT[0] = out[0]; localT[1] = out[1]; localT[2]  = out[2];  localT[3]  = 0.0f;
                            localT[4] = out[3]; localT[5] = out[4]; localT[6]  = out[5];  localT[7]  = 0.0f;
                            localT[8] = out[6]; localT[9] = out[7]; localT[10] = out[8];  localT[11] = 0.0f;
                            localT[15] = 1.0f;
                        }
                    }
                    fprintf(f, "[");
                    for (int mi = 0; mi < 16; ++mi)
                        fprintf(f, "%.7g%s", localT[mi], mi<15?", ":"");
                    fprintf(f, "]");
                    handled = true;
                } else if (_stricmp(fieldName.c_str(), "Layers") == 0 && !p.layerGuids.empty()) {
                    fprintf(f, "[");
                    for (size_t li = 0; li < p.layerGuids.size(); ++li)
                        fprintf(f, "%s%u", li>0?", ":"", p.layerGuids[li]);
                    fprintf(f, "]");
                    handled = true;
                }
                if (!handled && ZeroEngine::FieldKindIsList(fk)) {
                    std::map<uint32_t, std::vector<uint32_t> >::const_iterator lIt = p.listOverrides.find(fd.name_crc);
                    if (lIt == p.listOverrides.end()) {
                        uint32_t altListCrc = fd.name.empty() ? fd.name_crc : ZeroEngine::LotrHashString(fd.name.c_str());
                        if (altListCrc != fd.name_crc)
                            lIt = p.listOverrides.find(altListCrc);
                    }
                    if (lIt != p.listOverrides.end()) {
                        fprintf(f, "[");
                        for (size_t li = 0; li < lIt->second.size(); ++li)
                            fprintf(f, "%s%u", li > 0 ? ", " : "", lIt->second[li]);
                        fprintf(f, "]");
                        handled = true;
                    }
                }
                // ── Check for generic overrides (intOverrides, floatOverrides, stringOverrides) ──
                // Try both fd.name_crc and canonical CRC (handles case mismatch)
                uint32_t altCrc = fd.name.empty() ? fd.name_crc : ZeroEngine::LotrHashString(fd.name.c_str());
                if (!handled) {
                    std::map<uint32_t, std::string>::const_iterator sIt = p.stringOverrides.find(fd.name_crc);
                    if (sIt == p.stringOverrides.end() && altCrc != fd.name_crc)
                        sIt = p.stringOverrides.find(altCrc);
                    if (sIt != p.stringOverrides.end()) {
                        WriteJsonString(f, sIt->second); handled = true;
                    }
                }
                if (!handled) {
                    std::map<uint32_t, uint32_t>::const_iterator iIt = p.intOverrides.find(fd.name_crc);
                    if (iIt == p.intOverrides.end() && altCrc != fd.name_crc)
                        iIt = p.intOverrides.find(altCrc);
                    if (iIt != p.intOverrides.end()) {
                        if (fk == ZeroEngine::FK_BOOL) fprintf(f, "%s", iIt->second ? "true" : "false");
                        else if (fk == ZeroEngine::FK_GUID) fprintf(f, "%u", iIt->second);
                        else fprintf(f, "%d", (int32_t)iIt->second);
                        handled = true;
                    }
                }
                if (!handled) {
                    std::map<uint32_t, float>::const_iterator fIt = p.floatOverrides.find(fd.name_crc);
                    if (fIt == p.floatOverrides.end() && altCrc != fd.name_crc)
                        fIt = p.floatOverrides.find(altCrc);
                    if (fIt != p.floatOverrides.end()) {
                        fprintf(f, "%.7g", fIt->second); handled = true;
                    }
                }
                if (handled) continue;

                // ── Read from fieldData (for donor-cloned entities with existing binary data) ──
                const uint8_t* fptr = (fd.offset < p.fieldData.size()) ? &p.fieldData[fd.offset] : NULL;
                uint32_t fsize = ZeroEngine::FieldKindFixedSize(fk);

                if (!fptr || fd.offset + fsize > (uint32_t)p.fieldData.size()) {
                    // Out of bounds — write default
                    if (fk == ZeroEngine::FK_FLOAT) fprintf(f, "0.0");
                    else if (fk == ZeroEngine::FK_BOOL) fprintf(f, "false");
                    else if (fk == ZeroEngine::FK_STRING || fk == ZeroEngine::FK_CRC) fprintf(f, "\"\"");
                    else if (fk == ZeroEngine::FK_MATRIX4X4) fprintf(f, "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]");
                    else if (fk == ZeroEngine::FK_VECTOR3) fprintf(f, "[0,0,0]");
                    else if (ZeroEngine::FieldKindIsList(fk)) fprintf(f, "[]");
                    else fprintf(f, "0");
                    continue;
                }

                switch (fk) {
                case ZeroEngine::FK_INT: {
                    int32_t v; memcpy(&v, fptr, 4);
                    fprintf(f, "%d", v);
                } break;
                case ZeroEngine::FK_GUID: {
                    uint32_t v; memcpy(&v, fptr, 4);
                    fprintf(f, "%u", v);
                } break;
                case ZeroEngine::FK_FLOAT: {
                    float v; memcpy(&v, fptr, 4);
                    fprintf(f, "%.7g", v);
                } break;
                case ZeroEngine::FK_BOOL: {
                    int32_t v; memcpy(&v, fptr, 4);
                    fprintf(f, "%s", v ? "true" : "false");
                } break;
                case ZeroEngine::FK_CRC: case ZeroEngine::FK_STRING: {
                    uint32_t v; memcpy(&v, fptr, 4);
                    std::string resolved = g_levelReader.ResolveCrc(v);
                    if (!resolved.empty())
                        WriteJsonString(f, resolved);
                    else if (v == 0)
                        WriteJsonString(f, "");
                    else {
                        char cb[16];
                        sprintf_s(cb, "0x%08X", v);
                        WriteJsonString(f, cb);
                    }
                } break;
                case ZeroEngine::FK_COLOR: {
                    uint32_t v; memcpy(&v, fptr, 4);
                    fprintf(f, "\"0x%08X\"", v);
                } break;
                case ZeroEngine::FK_VECTOR3: {
                    float x,y,z;
                    memcpy(&x, fptr, 4); memcpy(&y, fptr+4, 4); memcpy(&z, fptr+8, 4);
                    fprintf(f, "[%.7g, %.7g, %.7g]", x, y, z);
                } break;
                case ZeroEngine::FK_MATRIX4X4: {
                    fprintf(f, "[");
                    for (int mi = 0; mi < 16; ++mi) {
                        float v; memcpy(&v, fptr+mi*4, 4);
                        fprintf(f, "%.7g%s", v, mi<15?", ":"");
                    }
                    fprintf(f, "]");
                } break;
                default:
                    if (ZeroEngine::FieldKindIsList(fk)) {
                        uint16_t listNum, listOff;
                        memcpy(&listNum, fptr, 2);
                        memcpy(&listOff, fptr+2, 2);
                        fprintf(f, "[");
                        uint32_t dataStart = fd.offset + 4 + listOff;
                        for (int li = 0; li < listNum; ++li) {
                            uint32_t itemOff = dataStart + li * 4;
                            if (itemOff + 4 <= (uint32_t)p.fieldData.size()) {
                                uint32_t item; memcpy(&item, &p.fieldData[itemOff], 4);
                                if (fk == ZeroEngine::FK_CRCLIST || fk == ZeroEngine::FK_STRINGLIST) {
                                    std::string r = g_levelReader.ResolveCrc(item);
                                    if (li > 0) fprintf(f, ", ");
                                    if (!r.empty()) WriteJsonString(f, r);
                                    else fprintf(f, "%u", item);
                                } else {
                                    fprintf(f, "%s%u", li>0?", ":"", item);
                                }
                            }
                        }
                        fprintf(f, "]");
                    } else {
                        uint32_t v = 0;
                        memcpy(&v, fptr, 4);
                        fprintf(f, "%u", v);
                    }
                    break;
                }
            }
        }
        fprintf(f, "\n    }\n");
        fprintf(f, "  }");
    }
    fprintf(f, "\n]\n");
    fclose(f);
    return true;
}

// Collect all new strings that need to be in pak_strings.json
// Helper: append a string to the --strings list, quoting if it contains spaces
static void AppendNewString(std::string& result, const std::string& s, std::set<std::string>& seen) {
    if (s.empty() || seen.find(s) != seen.end()) return;
    if (!result.empty()) result += " ";
    if (s.find(' ') != std::string::npos)
        result += "\"" + s + "\"";
    else
        result += s;
    seen.insert(s);
}

static std::string CollectNewStrings() {
    std::string result;
    const std::vector<ZeroEngine::PendingGameObj>& pending = g_levelReader.GetPendingObjs();
    std::set<std::string> seen;
    for (size_t i = 0; i < pending.size(); ++i) {
        const ZeroEngine::PendingGameObj& p = pending[i];
        AppendNewString(result, p.name_str, seen);
        AppendNewString(result, p.output_event, seen);
        AppendNewString(result, p.input_action, seen);
        for (std::map<uint32_t, std::string>::const_iterator sit = p.stringOverrides.begin();
             sit != p.stringOverrides.end(); ++sit) {
            AppendNewString(result, sit->second, seen);
        }
    }
    // 3dCrowd Editor v1.5: custom CrowdItem `key` strings the user
    // invented via the rename / add-new / duplicate paths. Without
    // this, lotrc_rs -c writes the new PAK without the strings in
    // pak_strings, and the next -d dump shows raw 0x-hex CRCs instead
    // of the names the user typed. Queue lives on LevelReader; we
    // just funnel it into the same --strings arg.
    const std::vector<std::string>& queued = g_levelReader.GetQueuedCustomStrings();
    for (size_t i = 0; i < queued.size(); ++i) {
        AppendNewString(result, queued[i], seen);
    }
    return result;
}

static bool RustParserSaveLevel(
    const std::string& originalPakPath,
    const std::string& binPath,
    const std::string& outputPakPath)
{
    // Find lotrc_rs.exe
    std::string lotrcExe = FindLotrcRsExe();
    if (lotrcExe.empty()) {
        g_savePakMessage = "lotrc_rs.exe not found";
        g_savePakStatus = -1;
        return false;
    }

    // Find Python
    std::string pythonExe;
    {
        const char* pyCandidates[] = { "python", "python3", "py", NULL };
        for (int pi = 0; pyCandidates[pi]; ++pi) {
            char testCmd[256];
            sprintf_s(testCmd, "%s --version", pyCandidates[pi]);
            if (RunSubprocess(testCmd, 5000)) {
                pythonExe = pyCandidates[pi];
                break;
            }
        }
    }
    if (pythonExe.empty()) {
        g_savePakMessage = "Python not found — install Python and add to PATH";
        g_savePakStatus = -1;
        return false;
    }

    // Find level_patcher.py
    std::string patcherPy;
    {
        const char* candidates[] = {
            "Vespucci\\Tools\\Python\\level_patcher.py",
            "Scene3D\\Vespucci\\Tools\\Python\\level_patcher.py",
            "..\\Scene3D\\Vespucci\\Tools\\Python\\level_patcher.py",
            "level_patcher.py",
            "Scene3D\\level_patcher.py",
            "..\\Scene3D\\level_patcher.py",
            NULL
        };
        for (int pi = 0; candidates[pi]; ++pi) {
            if (GetFileAttributesA(candidates[pi]) != INVALID_FILE_ATTRIBUTES) {
                char absPath[MAX_PATH];
                GetFullPathNameA(candidates[pi], MAX_PATH, absPath, NULL);
                patcherPy = absPath;
                break;
            }
        }
    }
    if (patcherPy.empty()) {
        g_savePakMessage = "level_patcher.py not found";
        g_savePakStatus = -1;
        return false;
    }

    DebugLog("RustParserSaveLevel: START");

    // Build PAK directory path
    std::string pakDir;
    size_t lastSlash = originalPakPath.rfind('\\');
    if (lastSlash == std::string::npos) lastSlash = originalPakPath.rfind('/');
    if (lastSlash != std::string::npos)
        pakDir = originalPakPath.substr(0, lastSlash + 1);

    // No temp dirs needed — Python script handles everything

    // ── Write pending entities to JSON file ──
    std::string entitiesJsonPath = pakDir + "ze_pending_entities.json";
    {
        char logBuf[256];
        sprintf_s(logBuf, "RustParserSaveLevel: pendingCount=%d, gameObjCount=%d",
                  g_levelReader.GetPendingCount(), (int)g_levelReader.GetGameObjs().size());
        DebugLog(logBuf);
        OutputDebugStringA(logBuf); OutputDebugStringA("\n");
    }
    if (g_levelReader.GetPendingCount() > 0) {
        // Flush pending FieldEdits (gizmo drags + property panel edits made
        // since creation) back into PendingGameObj struct fields, override
        // maps, and fieldData. Without this, WritePendingEntitiesJson dumps
        // the entity at initial spawn coords with default fields and the
        // user has to relaunch + re-edit just to make the second save stick.
        g_levelReader.SyncPendingFromFieldEdits();
        if (!WritePendingEntitiesJson(entitiesJsonPath)) {
            g_savePakMessage = "Failed to write pending entities JSON";
            g_savePakStatus = -1;
            return false;
        }
    }

    // ── 3dCrowd diff: dump m_crowdItems to ze_crowd.json if dirty ──
    // The level_patcher.py picks this up via --crowd and overlays it onto
    // <dump>/sub_blocks2/3dcrowd.json after lotrc_rs -d, before lotrc_rs -c
    // repacks. No diff format — we just dump the whole crowd block over.
    // Crowd has no per-entity GUIDs and the data is small (a few KB per
    // level), so a full overwrite is simpler than a per-instance diff and
    // avoids the "did the patcher correctly merge field edits" class of bug
    // we've already burned hours on.
    std::string crowdJsonPath = pakDir + "ze_crowd.json";
    bool crowdDirty = false;
    {
        LevelScene* ls = g_renderer ? g_renderer->getLevelScene() : NULL;
        if (ls && ls->isCrowdDirty()) {
            if (ls->dumpCrowdAsJson(crowdJsonPath)) {
                crowdDirty = true;
                char logBuf[256];
                sprintf_s(logBuf, "RustParserSaveLevel: wrote ze_crowd.json (%d items)",
                          ls->getCrowdItemCount());
                DebugLog(logBuf);
            } else {
                DebugLog("RustParserSaveLevel: failed to write ze_crowd.json (continuing without)");
            }
        }
    }

    // ── Write deleted GUIDs file (if any entities were wiped) ──
    std::string deletedGuidsPath = pakDir + "ze_deleted_guids.json";
    {
        const std::set<uint32_t>& deleted = g_levelReader.GetDeletedGuids();
        if (!deleted.empty()) {
            FILE* df = fopen(deletedGuidsPath.c_str(), "w");
            if (df) {
                fprintf(df, "[");
                int di = 0;
                for (std::set<uint32_t>::const_iterator it = deleted.begin(); it != deleted.end(); ++it, ++di) {
                    fprintf(df, "%s%u", di > 0 ? ", " : "", *it);
                }
                fprintf(df, "]\n");
                fclose(df);
                char logBuf[128];
                sprintf_s(logBuf, "RustParserSaveLevel: %d deleted GUIDs written", (int)deleted.size());
                DebugLog(logBuf);
            }
        }
    }

    // ── Crowd Mesh Builder: write ze_merged_models.json sidecar ──
    // Every Bake click since the last successful save queued a spec
    // into s_crowdBuilderMergeSpecs. Dump the lot as a JSON array next
    // to the PAK; Python's level_patcher.py picks it up via
    // --merged-models and runs the actual mesh-merge math. Cleared
    // (not deleted-from-disk) on save success below — disk file stays
    // for debugging, in-memory list resets so the next save doesn't
    // re-emit the same bakes.
    std::string mergedModelsPath = pakDir + "ze_merged_models.json";
    bool mergedModelsWritten = false;
    if (!s_crowdBuilderMergeSpecs.empty()) {
        FILE* mf = fopen(mergedModelsPath.c_str(), "w");
        if (mf) {
            fprintf(mf, "[\n");
            for (size_t mi = 0; mi < s_crowdBuilderMergeSpecs.size(); ++mi) {
                const CrowdBuilderMergeSpec& s = s_crowdBuilderMergeSpecs[mi];
                if (mi > 0) fprintf(mf, ",\n");
                fprintf(mf, "  {\n");
                fprintf(mf, "    \"new_name\": ");
                WriteJsonString(mf, s.newName);
                fprintf(mf, ",\n    \"base\": ");
                WriteJsonString(mf, s.baseName);
                fprintf(mf, ",\n    \"weapons\": [\n");
                // Phase 4 sidecar v1 emits exactly one weapon per spec.
                // The JSON shape is an ARRAY so Phase 5+ can extend to
                // multi-weapon merges (bow + quiver, sword + shield)
                // without breaking the format.
                fprintf(mf, "      {\n");
                fprintf(mf, "        \"mesh\": ");
                WriteJsonString(mf, s.weaponName);
                fprintf(mf, ",\n        \"attach_bone\": ");
                WriteJsonString(mf, s.attachBone);
                fprintf(mf, ",\n        \"offset_xyz\": [%.7g, %.7g, %.7g]",
                        s.offsetXYZ[0], s.offsetXYZ[1], s.offsetXYZ[2]);
                fprintf(mf, ",\n        \"offset_ypr\": [%.7g, %.7g, %.7g]\n",
                        s.offsetYPR[0], s.offsetYPR[1], s.offsetYPR[2]);
                fprintf(mf, "      }\n");
                fprintf(mf, "    ],\n");
                // Embed mode tells Python to bake the weapon straight
                // into base.slot[0] instead of adding a new mesh slot.
                // embed_uv_rect picks the atlas region the weapon's UVs
                // map into. Set by the toggle + sliders in the Crowd
                // Mesh Builder panel.
                fprintf(mf, "    \"embed_mode\": %d,\n", s.embedMode);
                fprintf(mf, "    \"embed_uv_rect\": [%.7g, %.7g, %.7g, %.7g],\n",
                        s.embedUVRect[0], s.embedUVRect[1],
                        s.embedUVRect[2], s.embedUVRect[3]);
                /* AUTO-SPAWN PAYLOAD: Python takes this and slams a
                 * fresh 3dCrowd entry onto sub_blocks2/3dcrowd.json
                 * with new_name as key_main and spawn_count instances
                 * dropped around spawn_pos. Skip this and the bake
                 * writes a JSON that no entity in the level references,
                 * which is exactly the goddamn invisible-mesh problem
                 * that drove the user to scream "where in the hell are
                 * we going to put the mesh to?" at the panel. Not on
                 * my watch anymore. */
                fprintf(mf, "    \"auto_spawn\": %d,\n", s.autoSpawn);
                fprintf(mf, "    \"spawn_count\": %d,\n", s.spawnCount);
                fprintf(mf, "    \"spawn_pos\": [%.7g, %.7g, %.7g]\n",
                        s.spawnPos[0], s.spawnPos[1], s.spawnPos[2]);
                fprintf(mf, "  }");
            }
            fprintf(mf, "\n]\n");
            fclose(mf);
            mergedModelsWritten = true;
            char logBuf[256];
            sprintf_s(logBuf,
                "RustParserSaveLevel: wrote ze_merged_models.json (%d specs)",
                (int)s_crowdBuilderMergeSpecs.size());
            DebugLog(logBuf);
        } else {
            DebugLog("RustParserSaveLevel: failed to write ze_merged_models.json (continuing without)");
        }
    }

    // ── Strip Baked Weapon: write ze_strip_specs.json sidecar ──
    // Mirrors the merge-spec emit above. Python picks this up via
    // --strip-weapons and runs apply_strip_specs() BEFORE the merge step,
    // so a stripped CRD can serve as the base of a merge in the same save.
    std::string stripSpecsPath = pakDir + "ze_strip_specs.json";
    bool stripSpecsWritten = false;
    if (!s_pendingStripSpecs.empty()) {
        FILE* sf = fopen(stripSpecsPath.c_str(), "w");
        if (sf) {
            fprintf(sf, "[\n");
            for (size_t si = 0; si < s_pendingStripSpecs.size(); ++si) {
                const StripSpec& s = s_pendingStripSpecs[si];
                if (si > 0) fprintf(sf, ",\n");
                fprintf(sf, "  {\n");
                fprintf(sf, "    \"new_name\": ");
                WriteJsonString(sf, s.new_name);
                fprintf(sf, ",\n    \"source\": ");
                WriteJsonString(sf, s.source);
                fprintf(sf, ",\n    \"bones\": [");
                for (size_t bi = 0; bi < s.bones.size(); ++bi) {
                    if (bi > 0) fprintf(sf, ", ");
                    WriteJsonString(sf, s.bones[bi]);
                }
                fprintf(sf, "]\n  }");
            }
            fprintf(sf, "\n]\n");
            fclose(sf);
            stripSpecsWritten = true;
            char logBuf[256];
            sprintf_s(logBuf,
                "RustParserSaveLevel: wrote ze_strip_specs.json (%d specs)",
                (int)s_pendingStripSpecs.size());
            DebugLog(logBuf);
        } else {
            DebugLog("RustParserSaveLevel: failed to write ze_strip_specs.json (continuing without)");
        }
    }

    // ── Write field edits file (property edits to persist) ──
    std::string fieldEditsPath = pakDir + "ze_field_edits.json";
    {
        const std::vector<ZeroEngine::FieldEdit>& edits = g_levelReader.GetFieldEdits();
        if (!edits.empty()) {
            FILE* ef = fopen(fieldEditsPath.c_str(), "w");
            if (ef) {
                fprintf(ef, "[\n");
                for (size_t ei = 0; ei < edits.size(); ++ei) {
                    const ZeroEngine::FieldEdit& e = edits[ei];
                    if (ei > 0) fprintf(ef, ",\n");
                    fprintf(ef, "  {\"guid\": %u, \"field\": ", e.entityGuid);
                    WriteJsonString(ef, e.fieldName);
                    fprintf(ef, ", \"kind\": %d", e.kind);
                    switch (e.kind) {
                        case 0: fprintf(ef, ", \"value\": %d", e.intVal); break;
                        case 1: fprintf(ef, ", \"value\": %.7g", e.floatVal); break;
                        case 2: fprintf(ef, ", \"value\": %u", e.guidVal); break;
                        case 3: fprintf(ef, ", \"value\": [%.7g, %.7g, %.7g]", e.vec3Val[0], e.vec3Val[1], e.vec3Val[2]); break;
                        case 4: {
                            fprintf(ef, ", \"value\": [");
                            for (int mi = 0; mi < 16; ++mi)
                                fprintf(ef, "%.7g%s", e.matrixVal[mi], mi < 15 ? ", " : "");
                            fprintf(ef, "]");
                        } break;
                        case 5:
                            fprintf(ef, ", \"value\": ");
                            WriteJsonString(ef, e.stringVal);
                            break;
                        case 6: fprintf(ef, ", \"listIndex\": %d, \"value\": %u", e.listIndex, e.guidVal); break;
                        case 7: {
                            // Float array — write as array of sub-arrays (e.g. [[x,y,z,s], ...])
                            int stride = e.arrayStride > 0 ? e.arrayStride : 4;
                            int count = (int)e.arrayVal.size() / stride;
                            fprintf(ef, ", \"value\": [");
                            for (int ai = 0; ai < count; ++ai) {
                                if (ai > 0) fprintf(ef, ", ");
                                fprintf(ef, "[");
                                for (int si = 0; si < stride; ++si)
                                    fprintf(ef, "%.7g%s", e.arrayVal[ai * stride + si], si < stride-1 ? ", " : "");
                                fprintf(ef, "]");
                            }
                            fprintf(ef, "]");
                        } break;
                        case 8:
                            // List append — GUID pushed onto end of an objectlist field.
                            fprintf(ef, ", \"value\": %u", e.guidVal);
                            break;
                    }
                    fprintf(ef, "}");
                }
                fprintf(ef, "\n]\n");
                fclose(ef);
                char logBuf[128];
                sprintf_s(logBuf, "RustParserSaveLevel: %d field edits written", (int)edits.size());
                DebugLog(logBuf);
            }
        }
    }

    // ── Collect new strings ──
    std::string newStrings = CollectNewStrings();

    // ── Call Python: dump → patch → compile (single subprocess) ──
    g_savePakStatus = 1;
    g_savePakMessage = "Running level_patcher.py (dump + patch + compile)...";

    // Build command: python level_patcher.py <pak> <entities_json> <lotrc_exe> <output_pak> [--strings ...] [--deleted <path>]
    char saveCmd[8192];
    // Ensure entities file exists even if empty
    if (g_levelReader.GetPendingCount() == 0) {
        FILE* ef = fopen(entitiesJsonPath.c_str(), "w");
        if (ef) { fprintf(ef, "[]\n"); fclose(ef); }
    } else {
        // Already written above
    }
    // Build base command
    std::string cmd = "\"" + pythonExe + "\" \"" + patcherPy + "\" \"" + originalPakPath
                    + "\" \"" + entitiesJsonPath + "\" \"" + lotrcExe + "\" \"" + outputPakPath + "\"";
    // Append --strings if any
    if (!newStrings.empty())
        cmd += " --strings " + newStrings;
    // Append --deleted if any GUIDs were wiped
    if (!g_levelReader.GetDeletedGuids().empty())
        cmd += " --deleted \"" + deletedGuidsPath + "\"";
    // Append --edits if any property edits were made
    if (!g_levelReader.GetFieldEdits().empty())
        cmd += " --edits \"" + fieldEditsPath + "\"";
    // Append --crowd if the 3dCrowd block was edited this session.
    // Patcher overlays this onto the dump's sub_blocks2/3dcrowd.json
    // before the lotrc_rs -c repack pass.
    if (crowdDirty)
        cmd += " --crowd \"" + crowdJsonPath + "\"";
    // Append --merged-models if Crowd Mesh Builder queued any bakes
    // this session. Python's apply_merged_models() reads the spec
    // file, performs the JSON-level vertex/bone/material merge, and
    // writes the new <name>.json into the dump's models/ before
    // lotrc_rs -c repacks. The new model name is also already in
    // pak_strings via QueueCustomString (see consume block).
    if (mergedModelsWritten)
        cmd += " --merged-models \"" + mergedModelsPath + "\"";
    if (stripSpecsWritten)
        cmd += " --strip-weapons \"" + stripSpecsPath + "\"";
    // ── Adjust Baked Weapon / Move Triangles xforms ─────────────────
    // The panel's Apply button writes ze_embedded_weapon_xforms.json
    // next to the PAK (level_reader's dump-dir derivation). The
    // patcher unpacks the PAK to a TEMP directory, so it would never
    // see the file unless we hand it the path. Pass it as --xforms.
    // No file == no flag == no-op on the patcher side.
    {
        std::string xformsPath = g_levelReader.GetDumpDir();
        if (!xformsPath.empty()) {
            xformsPath += "/ze_embedded_weapon_xforms.json";
            FILE* probe = NULL; fopen_s(&probe, xformsPath.c_str(), "rb");
            if (probe) {
                fclose(probe);
                cmd += " --xforms \"" + xformsPath + "\"";
            }
        }
    }
    strncpy_s(saveCmd, sizeof(saveCmd), cmd.c_str(), _TRUNCATE);

    DebugLog(saveCmd);

    // Run Python with visible terminal window so user can see progress
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        // Show the terminal window so user sees Rust parser output
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        ZeroMemory(&pi, sizeof(pi));

        char cmdBuf[4096];
        strncpy_s(cmdBuf, sizeof(cmdBuf), saveCmd, _TRUNCATE);

        if (!CreateProcessA(NULL, cmdBuf, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL,
                            pakDir.c_str(), &si, &pi)) {
            DWORD err = GetLastError();
            char errBuf[256];
            sprintf_s(errBuf, "CreateProcess failed (error %u)", err);
            g_savePakMessage = errBuf;
            g_savePakStatus = -1;
            return false;
        }

        g_savePakStatus = 2;
        g_savePakMessage = "Python + Rust parser running (see terminal window)...";

        // Wait for completion (up to 10 minutes)
        DWORD wait = WaitForSingleObject(pi.hProcess, 600000);
        DWORD exitCode = 1;
        if (wait == WAIT_OBJECT_0)
            GetExitCodeProcess(pi.hProcess, &exitCode);
        else {
            TerminateProcess(pi.hProcess, 1);
            g_savePakMessage = "Save TIMEOUT — process killed after 10 minutes";
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // Cleanup entities JSON
        // Keep entities JSON for debugging (TODO: remove later)
        // DeleteFileA(entitiesJsonPath.c_str());
        DeleteFileA(deletedGuidsPath.c_str());
        DeleteFileA(fieldEditsPath.c_str());
        // Crowd diff cleanup. Mirror the entities JSON behavior — keep it
        // around for debugging so you can diff it against the dump dir to
        // see what the patcher overlayed. Delete-on-success can come later.
        // DeleteFileA(crowdJsonPath.c_str());

        if (wait == WAIT_OBJECT_0 && exitCode == 0) {
            // Check output file exists
            if (GetFileAttributesA(outputPakPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                // Save succeeded — crowd edits are persisted into the new
                // PAK. Clear the dirty flag so the next save won't redump
                // unchanged data. The next user mutation flips it back on.
                if (crowdDirty) {
                    LevelScene* lsClear = g_renderer ? g_renderer->getLevelScene() : NULL;
                    if (lsClear) lsClear->setCrowdDirty(false);
                }
                // Crowd Mesh Builder bakes are baked-in now (literally —
                // the merged models live inside the new PAK). Drop the
                // pending list so the NEXT save doesn't re-run the same
                // merges. The sidecar JSON file on disk stays for diff /
                // debugging, same policy as ze_crowd.json.
                if (mergedModelsWritten) {
                    s_crowdBuilderMergeSpecs.clear();
                    DebugLog("RustParserSaveLevel: cleared pending crowd-merge specs");
                }
                DebugLog("RustParserSaveLevel: SUCCESS");
                return true;
            } else {
                g_savePakMessage = "Python finished but output PAK not found: " + outputPakPath;
                g_savePakStatus = -1;
                return false;
            }
        } else {
            char errBuf[256];
            sprintf_s(errBuf, "Python exited with code %u", exitCode);
            g_savePakMessage = errBuf;
            g_savePakStatus = -1;
            return false;
        }
    }
}

// RustParserSanitize — dump-then-recompile a PAK/BIN through lotrc_rs.exe
//
// Our C++ SavePak writes structurally readable but sometimes broken PAK files.
// The Rust parser (lotrc_rs) can read them, normalize everything, and rewrite
// clean files from scratch. This function:
//   1. Runs lotrc_rs -d <pak> -o <tempdir>   (dump to editable format)
//   2. Runs lotrc_rs -c <tempdir> -o <outdir> (recompile from scratch)
//   3. Copies the recompiled PAK/BIN back over the originals
//
// Returns true on success. Sets g_savePakMessage on error.
// ============================================================================
static bool RunSubprocess(const char* cmdLine, int timeoutMs, const char* workDir)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    // CreateProcessA needs a mutable copy
    char cmdBuf[2048];
    strncpy_s(cmdBuf, sizeof(cmdBuf), cmdLine, _TRUNCATE);

    char logBuf[2200];
    sprintf_s(logBuf, "RunSubprocess: [%s] workDir=[%s]", cmdBuf, workDir ? workDir : "(inherit)");
    DebugLog(logBuf);

    if (!CreateProcessA(NULL, cmdBuf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, workDir, &si, &pi)) {
        DWORD err = GetLastError();
        sprintf_s(logBuf, "RunSubprocess: CreateProcessA FAILED, GetLastError=%u", err);
        DebugLog(logBuf);
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, (DWORD)timeoutMs);
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exitCode);
    else {
        TerminateProcess(pi.hProcess, 1);
        DebugLog("RunSubprocess: TIMEOUT — process killed");
    }

    sprintf_s(logBuf, "RunSubprocess: exitCode=%u wait=%s", exitCode,
        wait == WAIT_OBJECT_0 ? "OK" : "TIMEOUT");
    DebugLog(logBuf);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (wait == WAIT_OBJECT_0 && exitCode == 0);
}

static std::string FindLotrcRsExe()
{
    // Search for lotrc_rs.exe in common locations relative to the EXE
    const char* candidates[] = {
        "lotrc_rs.exe",
        "tools\\lotrc_rs.exe",
        "..\\tools\\lotrc_rs.exe",
        "..\\GameFiles\\lotrcparser\\lotrc_rs.exe",
        "..\\lotrc_rs.exe",
        NULL
    };
    for (int i = 0; candidates[i]; ++i) {
        if (GetFileAttributesA(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
            // Return absolute path to avoid CWD issues
            char absPath[MAX_PATH];
            if (GetFullPathNameA(candidates[i], MAX_PATH, absPath, NULL))
                return std::string(absPath);
            return std::string(candidates[i]);
        }
    }
    // Also check next to the EXE itself
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSep = strrchr(exePath, '\\');
    if (!lastSep) lastSep = strrchr(exePath, '/');
    if (lastSep) {
        *lastSep = '\0';
        char fullPath[MAX_PATH];
        sprintf_s(fullPath, "%s\\lotrc_rs.exe", exePath);
        if (GetFileAttributesA(fullPath) != INVALID_FILE_ATTRIBUTES)
            return std::string(fullPath);
    }
    return "";
}

static bool RustParserSanitize(const std::string& pakPath, const std::string& binPath)
{
    std::string lotrcExe = FindLotrcRsExe();
    if (lotrcExe.empty()) {
        g_savePakMessage = "lotrc_rs.exe not found — PAK saved but not sanitized. "
                           "Place lotrc_rs.exe next to ZeroEngine or in tools/";
        DebugLog("RustParserSanitize: lotrc_rs.exe not found, skipping sanitize");
        return false;
    }

    DebugLog("RustParserSanitize: START");

    // Build temp directory next to the PAK file
    std::string pakDir;
    size_t lastSlash = pakPath.rfind('\\');
    if (lastSlash == std::string::npos) lastSlash = pakPath.rfind('/');
    if (lastSlash != std::string::npos)
        pakDir = pakPath.substr(0, lastSlash + 1);

    std::string tempDump = pakDir + "ze_sanitize_tmp";
    std::string tempOut  = pakDir + "ze_sanitize_out";

    // Clean up any previous temp dirs
    char rmCmd[1024];
    sprintf_s(rmCmd, "cmd /c rmdir /s /q \"%s\" 2>nul & rmdir /s /q \"%s\" 2>nul",
              tempDump.c_str(), tempOut.c_str());
    RunSubprocess(rmCmd, 5000);

    // Create temp dirs
    CreateDirectoryA(tempDump.c_str(), NULL);
    CreateDirectoryA(tempOut.c_str(), NULL);

    // Step 1: Dump the (possibly broken) PAK to editable format
    g_savePakStatus = 2;
    g_savePakMessage = "Sanitizing: dumping via Rust parser...";

    char dumpCmd[2048];
    sprintf_s(dumpCmd, "\"%s\" -d \"%s\" -o \"%s\"",
              lotrcExe.c_str(), pakPath.c_str(), tempDump.c_str());
    DebugLog(dumpCmd);

    if (!RunSubprocess(dumpCmd, 60000)) {
        g_savePakMessage = "Rust parser dump failed — PAK saved but not sanitized";
        DebugLog("RustParserSanitize: dump FAILED");
        // Clean up
        sprintf_s(rmCmd, "cmd /c rmdir /s /q \"%s\" 2>nul & rmdir /s /q \"%s\" 2>nul",
                  tempDump.c_str(), tempOut.c_str());
        RunSubprocess(rmCmd, 5000);
        return false;
    }

    // Step 2: Recompile from dump into clean PAK/BIN
    g_savePakStatus = 3;
    g_savePakMessage = "Sanitizing: recompiling via Rust parser...";

    char compileCmd[2048];
    sprintf_s(compileCmd, "\"%s\" -c \"%s\" -o \"%s\"",
              lotrcExe.c_str(), tempDump.c_str(), tempOut.c_str());
    DebugLog(compileCmd);

    if (!RunSubprocess(compileCmd, 60000)) {
        g_savePakMessage = "Rust parser recompile failed — PAK saved but not sanitized";
        DebugLog("RustParserSanitize: compile FAILED");
        sprintf_s(rmCmd, "cmd /c rmdir /s /q \"%s\" 2>nul & rmdir /s /q \"%s\" 2>nul",
                  tempDump.c_str(), tempOut.c_str());
        RunSubprocess(rmCmd, 5000);
        return false;
    }

    // Step 3: Find the recompiled PAK/BIN in the output directory and copy back
    g_savePakStatus = 4;
    g_savePakMessage = "Sanitizing: copying clean files back...";

    // lotrc_rs outputs to <outdir>/Levels/<LevelName>.PAK and .BIN
    // or directly as <outdir>/<LevelName>.PAK depending on version
    // Search recursively for any .PAK file in the output
    WIN32_FIND_DATAA fd;
    char searchPat[1024];

    // Try flat output first
    std::string outPak, outBin;
    sprintf_s(searchPat, "%s\\*.PAK", tempOut.c_str());
    HANDLE hFind = FindFirstFileA(searchPat, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        outPak = tempOut + "\\" + fd.cFileName;
        FindClose(hFind);
    }
    sprintf_s(searchPat, "%s\\*.BIN", tempOut.c_str());
    hFind = FindFirstFileA(searchPat, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        outBin = tempOut + "\\" + fd.cFileName;
        FindClose(hFind);
    }

    // Try Levels/ subfolder
    if (outPak.empty()) {
        sprintf_s(searchPat, "%s\\Levels\\*.PAK", tempOut.c_str());
        hFind = FindFirstFileA(searchPat, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            outPak = tempOut + "\\Levels\\" + fd.cFileName;
            FindClose(hFind);
        }
    }
    if (outBin.empty()) {
        sprintf_s(searchPat, "%s\\Levels\\*.BIN", tempOut.c_str());
        hFind = FindFirstFileA(searchPat, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            outBin = tempOut + "\\Levels\\" + fd.cFileName;
            FindClose(hFind);
        }
    }

    bool copied = false;
    if (!outPak.empty()) {
        CopyFileA(outPak.c_str(), pakPath.c_str(), FALSE);
        copied = true;
        char logBuf[512];
        sprintf_s(logBuf, "RustParserSanitize: copied clean PAK from %s", outPak.c_str());
        DebugLog(logBuf);
    }
    if (!outBin.empty() && !binPath.empty()) {
        // Derive the modified BIN path to match the modified PAK
        std::string modBin = binPath;
        size_t bmod = modBin.find("_modified");
        if (bmod == std::string::npos) {
            size_t bdot = modBin.rfind('.');
            if (bdot != std::string::npos)
                modBin.insert(bdot, "_modified");
        }
        CopyFileA(outBin.c_str(), modBin.c_str(), FALSE);
        char logBuf[512];
        sprintf_s(logBuf, "RustParserSanitize: copied clean BIN from %s", outBin.c_str());
        DebugLog(logBuf);
    }

    // Clean up temp dirs
    sprintf_s(rmCmd, "cmd /c rmdir /s /q \"%s\" 2>nul & rmdir /s /q \"%s\" 2>nul",
              tempDump.c_str(), tempOut.c_str());
    RunSubprocess(rmCmd, 5000);

    if (copied) {
        DebugLog("RustParserSanitize: SUCCESS — files sanitized");
        return true;
    } else {
        g_savePakMessage = "Rust parser produced no output files";
        DebugLog("RustParserSanitize: no output files found");
        return false;
    }
}

static bool ContainsNoCaseSimple(const char* haystack, const char* needle)
{
    if (!needle || needle[0] == '\0')
    {
        return true;
    }
    if (!haystack)
    {
        return false;
    }
    size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; ++p)
    {
        if (_strnicmp(p, needle, nlen) == 0)
        {
            return true;
        }
    }
    return false;
}

static std::string BuildVersionedModifiedPakPath(const std::string& loadedPakPath)
{
    std::string basePath = loadedPakPath;
    size_t dotPos = basePath.rfind('.');
    size_t modPos = basePath.find("_modified");
    if (modPos != std::string::npos && (dotPos == std::string::npos || modPos < dotPos)) {
        size_t eraseEnd = (dotPos == std::string::npos) ? basePath.size() : dotPos;
        basePath.erase(modPos, eraseEnd - modPos);
        dotPos = basePath.rfind('.');
    }

    time_t now = time(NULL);
    struct tm tmNow;
    localtime_s(&tmNow, &now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmNow);

    std::string stem = basePath;
    std::string ext;
    dotPos = basePath.rfind('.');
    if (dotPos != std::string::npos) {
        stem = basePath.substr(0, dotPos);
        ext = basePath.substr(dotPos);
    }

    char suffix[64];
    sprintf_s(suffix, "_modified_%s", stamp);
    std::string candidate = stem + suffix + ext;
    for (int i = 2; GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; ++i) {
        char idx[16];
        sprintf_s(idx, "_%03d", i);
        candidate = stem + suffix + idx + ext;
    }
    return candidate;
}

static bool HasValidImGuiViewportRect()
{
    return (g_imguiViewportW > 1 && g_imguiViewportH > 1);
}

static void MapMouseToImGuiViewport(int& x, int& y)
{
    if (!g_imguiAvailable || g_showLegacyUI)
    {
        return;
    }
    if (!HasValidImGuiViewportRect())
    {
        return;
    }
    x -= g_imguiViewportX;
    y -= g_imguiViewportY;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > g_imguiViewportW) x = g_imguiViewportW;
    if (y > g_imguiViewportH) y = g_imguiViewportH;
}

static bool FileExistsA_Simple(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return false;
    return true;
}

static bool DirExistsA_Simple(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool EndsWithNoCase(const char* text, const char* suffix)
{
    if (!text || !suffix) return false;
    size_t textLen = strlen(text);
    size_t suffixLen = strlen(suffix);
    if (suffixLen == 0 || suffixLen > textLen) return false;
    return _stricmp(text + (textLen - suffixLen), suffix) == 0;
}

static bool StartsWithNoCase(const char* text, const char* prefix)
{
    if (!text || !prefix) return false;
    size_t textLen = strlen(text);
    size_t prefixLen = strlen(prefix);
    if (prefixLen == 0 || prefixLen > textLen) return false;
    return _strnicmp(text, prefix, prefixLen) == 0;
}

static const char* GetEditorStateIniPath()
{
    if (g_editorStateIniPath[0] != '\0')
    {
        return g_editorStateIniPath;
    }

    char exePath[MAX_PATH];
    exePath[0] = '\0';
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    exePath[MAX_PATH - 1] = '\0';

    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash)
    {
        lastSlash = strrchr(exePath, '/');
    }
    if (lastSlash)
    {
        *(lastSlash + 1) = '\0';
    }
    else
    {
        exePath[0] = '\0';
    }

    sprintf_s(g_editorStateIniPath, sizeof(g_editorStateIniPath),
              "%sScene3DEditorState.ini", exePath);
    return g_editorStateIniPath;
}

static const char* GetImGuiIniPath()
{
    if (g_imguiIniPath[0] != '\0')
    {
        return g_imguiIniPath;
    }

    // Allow overriding the ImGui .ini path via environment variable.
    // Useful for keeping separate layouts per project/workspace.
    DWORD n = GetEnvironmentVariableA("ZEROENGINE_IMGUI_INI", g_imguiIniPath, (DWORD)sizeof(g_imguiIniPath));
    if (n > 0 && n < (DWORD)sizeof(g_imguiIniPath))
    {
        return g_imguiIniPath;
    }
    g_imguiIniPath[0] = '\0';

    char exePath[MAX_PATH];
    exePath[0] = '\0';
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    exePath[MAX_PATH - 1] = '\0';

    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash)
    {
        lastSlash = strrchr(exePath, '/');
    }
    if (lastSlash)
    {
        *(lastSlash + 1) = '\0';
    }
    else
    {
        exePath[0] = '\0';
    }

    sprintf_s(g_imguiIniPath, sizeof(g_imguiIniPath),
              "%sScene3DEditor.ini", exePath);
    return g_imguiIniPath;
}

static float ReadIniFloatA(const char* section, const char* key, float defaultValue, const char* iniPath)
{
    char defBuf[64];
    char buf[64];
    sprintf_s(defBuf, sizeof(defBuf), "%.6g", defaultValue);
    GetPrivateProfileStringA(section, key, defBuf, buf, (DWORD)sizeof(buf), iniPath);
    return (float)atof(buf);
}

static void WriteIniIntA(const char* section, const char* key, int value, const char* iniPath)
{
    char buf[64];
    sprintf_s(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf, iniPath);
}

static void WriteIniFloatA(const char* section, const char* key, float value, const char* iniPath)
{
    char buf[64];
    sprintf_s(buf, sizeof(buf), "%.6g", value);
    WritePrivateProfileStringA(section, key, buf, iniPath);
}

static void WriteIniStringA(const char* section, const char* key, const char* value, const char* iniPath)
{
    WritePrivateProfileStringA(section, key, value ? value : "", iniPath);
}

static int FindAssetIndexByNameNoCase(const std::vector<AssetBrowser::AssetInfo>& assets, const char* name)
{
    if (!name || name[0] == '\0')
    {
        return -1;
    }
    for (size_t i = 0; i < assets.size(); ++i)
    {
        if (_stricmp(assets[i].name.c_str(), name) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static int FindStringIndexNoCase(const std::vector<std::string>& items, const char* name)
{
    if (!name || name[0] == '\0')
    {
        return -1;
    }
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (_stricmp(items[i].c_str(), name) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static void LoadEditorState()
{
    g_editorStateLoaded = false;

    const char* iniPath = GetEditorStateIniPath();
    if (!iniPath || iniPath[0] == '\0' || !FileExistsA_Simple(iniPath))
    {
        return;
    }

    g_editorStateLoaded = true;

    // Browser selections (store both index + name; name is more stable if asset ordering changes).
    g_browserIndexModel = GetPrivateProfileIntA("Browser", "ModelIndex", g_browserIndexModel, iniPath);
    g_gameAnimIndex = GetPrivateProfileIntA("Browser", "AnimIndex", g_gameAnimIndex, iniPath);
    g_browserIndexEffect = GetPrivateProfileIntA("Browser", "EffectIndex", g_browserIndexEffect, iniPath);

    GetPrivateProfileStringA("Browser", "ModelName", "", g_persistModelName, sizeof(g_persistModelName), iniPath);
    GetPrivateProfileStringA("Browser", "AnimName", "", g_persistAnimName, sizeof(g_persistAnimName), iniPath);
    GetPrivateProfileStringA("Browser", "EffectName", "", g_persistEffectName, sizeof(g_persistEffectName), iniPath);

    // View / UI
    g_showOverlay = (GetPrivateProfileIntA("View", "ShowOverlay", g_showOverlay ? 1 : 0, iniPath) != 0);
    g_showHelp = (GetPrivateProfileIntA("View", "ShowHelp", g_showHelp ? 1 : 0, iniPath) != 0);
    g_persistShowLegacyUI = GetPrivateProfileIntA("View", "ShowLegacyUI", -1, iniPath);

    // Theme
    g_darkTheme = (GetPrivateProfileIntA("Theme", "Dark", g_darkTheme ? 1 : 0, iniPath) != 0);

    // Playback
    g_persistPlaybackSpeed = ReadIniFloatA("Playback", "Speed", g_persistPlaybackSpeed, iniPath);
    g_persistLoopEnabled = GetPrivateProfileIntA("Playback", "Loop", g_persistLoopEnabled, iniPath);
    g_persistPaused = GetPrivateProfileIntA("Playback", "Paused", g_persistPaused, iniPath);
    g_persistAnimTime = ReadIniFloatA("Playback", "Time", g_persistAnimTime, iniPath);

    // Bone/editor panel (host-owned toggle + renderer-owned knobs)
    g_editMode = (GetPrivateProfileIntA("Bones", "EditMode", g_editMode ? 1 : 0, iniPath) != 0);
    GetPrivateProfileStringA("Bones", "ExportName", g_exportName, g_exportName, sizeof(g_exportName), iniPath);

    g_persistRecording = GetPrivateProfileIntA("Bones", "Recording", g_persistRecording, iniPath);
    g_persistInterpMode = GetPrivateProfileIntA("Bones", "InterpMode", g_persistInterpMode, iniPath);
    g_persistGizmoMode = GetPrivateProfileIntA("Bones", "GizmoMode", g_persistGizmoMode, iniPath);
    g_persistGizmoSpace = GetPrivateProfileIntA("Bones", "GizmoSpace", g_persistGizmoSpace, iniPath);
    g_persistRotSnapEnabled = GetPrivateProfileIntA("Bones", "RotSnapEnabled", g_persistRotSnapEnabled, iniPath);
    g_persistRotSnapDegrees = ReadIniFloatA("Bones", "RotSnapDegrees", g_persistRotSnapDegrees, iniPath);
    g_persistMoveSnapEnabled = GetPrivateProfileIntA("Bones", "MoveSnapEnabled", g_persistMoveSnapEnabled, iniPath);
    g_persistMoveSnapUnits = ReadIniFloatA("Bones", "MoveSnapUnits", g_persistMoveSnapUnits, iniPath);
    g_persistEditorTimelineDuration = ReadIniFloatA("Bones", "TimelineDuration", g_persistEditorTimelineDuration, iniPath);
}

static void SaveEditorState()
{
    const char* iniPath = GetEditorStateIniPath();
    if (!iniPath || iniPath[0] == '\0')
    {
        return;
    }

    // Browser selections
    WriteIniIntA("Browser", "ModelIndex", g_browserIndexModel, iniPath);
    WriteIniIntA("Browser", "AnimIndex", g_gameAnimIndex, iniPath);
    WriteIniIntA("Browser", "EffectIndex", g_browserIndexEffect, iniPath);
    WriteIniStringA("Browser", "ModelName", g_activeModelName, iniPath);
    WriteIniStringA("Browser", "AnimName", g_activeAnimName, iniPath);
    if (g_browserIndexEffect >= 0 && g_browserIndexEffect < (int)g_effectsList.size())
    {
        WriteIniStringA("Browser", "EffectName", g_effectsList[g_browserIndexEffect].c_str(), iniPath);
    }
    else
    {
        WriteIniStringA("Browser", "EffectName", "", iniPath);
    }

    // View / UI
    WriteIniIntA("View", "ShowOverlay", g_showOverlay ? 1 : 0, iniPath);
    WriteIniIntA("View", "ShowHelp", g_showHelp ? 1 : 0, iniPath);
    WriteIniIntA("View", "ShowLegacyUI", g_showLegacyUI ? 1 : 0, iniPath);

    // Theme
    WriteIniIntA("Theme", "Dark", g_darkTheme ? 1 : 0, iniPath);

    // Playback
    if (g_renderer)
    {
        WriteIniFloatA("Playback", "Speed", g_renderer->getPlaybackSpeed(), iniPath);
        WriteIniIntA("Playback", "Loop", g_renderer->getLoopAnimation() ? 1 : 0, iniPath);
        WriteIniIntA("Playback", "Paused", g_renderer->isJsonAnimationPaused() ? 1 : 0, iniPath);
        WriteIniFloatA("Playback", "Time", g_renderer->getJsonAnimationTime(), iniPath);
    }

    // Bone/editor panel
    WriteIniIntA("Bones", "EditMode", g_editMode ? 1 : 0, iniPath);
    WriteIniStringA("Bones", "ExportName", g_exportName, iniPath);
    if (g_renderer)
    {
        WriteIniIntA("Bones", "Recording", g_renderer->isEditorRecording() ? 1 : 0, iniPath);
        WriteIniIntA("Bones", "InterpMode", (int)g_renderer->getEditorInterpolationMode(), iniPath);
        WriteIniIntA("Bones", "GizmoMode", (int)g_renderer->getGizmoMode(), iniPath);
        WriteIniIntA("Bones", "GizmoSpace", (int)g_renderer->getGizmoSpace(), iniPath);
        WriteIniIntA("Bones", "RotSnapEnabled", g_renderer->getRotateSnapEnabled() ? 1 : 0, iniPath);
        WriteIniFloatA("Bones", "RotSnapDegrees", g_renderer->getRotateSnapDegrees(), iniPath);
        WriteIniIntA("Bones", "MoveSnapEnabled", g_renderer->getMoveSnapEnabled() ? 1 : 0, iniPath);
        WriteIniFloatA("Bones", "MoveSnapUnits", g_renderer->getMoveSnapUnits(), iniPath);
        WriteIniFloatA("Bones", "TimelineDuration", g_renderer->getEditorTimelineDuration(), iniPath);
    }
}

static void ResolvePersistedSelections()
{
    if (!g_editorStateLoaded || !g_gameFilesBrowser)
    {
        return;
    }

    const std::vector<AssetBrowser::AssetInfo>& jmodels = g_gameFilesBrowser->getJModels();
    const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();

    if (g_persistModelName[0] != '\0' && !jmodels.empty())
    {
        int idx = FindAssetIndexByNameNoCase(jmodels, g_persistModelName);
        if (idx >= 0) g_browserIndexModel = idx;
    }
    if (g_persistAnimName[0] != '\0' && !anims.empty())
    {
        int idx = FindAssetIndexByNameNoCase(anims, g_persistAnimName);
        if (idx >= 0) g_gameAnimIndex = idx;
    }
    if (g_persistEffectName[0] != '\0' && !g_effectsList.empty())
    {
        int idx = FindStringIndexNoCase(g_effectsList, g_persistEffectName);
        if (idx >= 0) g_browserIndexEffect = idx;
    }

    if (jmodels.empty())
    {
        g_browserIndexModel = 0;
    }
    else
    {
        if (g_browserIndexModel < 0) g_browserIndexModel = 0;
        if (g_browserIndexModel >= (int)jmodels.size()) g_browserIndexModel = (int)jmodels.size() - 1;
    }

    if (anims.empty())
    {
        g_gameAnimIndex = -1;
        g_browserIndexAnim = 0;
    }
    else
    {
        if (g_gameAnimIndex < 0) g_gameAnimIndex = 0;
        if (g_gameAnimIndex >= (int)anims.size()) g_gameAnimIndex = (int)anims.size() - 1;
        g_browserIndexAnim = g_gameAnimIndex;
    }

    if (g_effectsList.empty())
    {
        g_browserIndexEffect = 0;
    }
    else
    {
        if (g_browserIndexEffect < 0) g_browserIndexEffect = 0;
        if (g_browserIndexEffect >= (int)g_effectsList.size()) g_browserIndexEffect = (int)g_effectsList.size() - 1;
    }
}

static void ApplyPersistedRendererState()
{
    if (!g_editorStateLoaded || !g_renderer)
    {
        return;
    }

    g_renderer->setPlaybackSpeed(g_persistPlaybackSpeed);
    g_renderer->setLoopAnimation(g_persistLoopEnabled != 0);

    g_renderer->setEditorRecording(g_persistRecording != 0);
    g_renderer->setEditorInterpolationMode(g_persistInterpMode);
    g_renderer->setGizmoMode(g_persistGizmoMode);
    if (g_persistGizmoSpace != (int)g_renderer->getGizmoSpace())
    {
        g_renderer->toggleGizmoSpace();
    }
    g_renderer->setRotateSnapEnabled(g_persistRotSnapEnabled != 0);
    g_renderer->setRotateSnapDegrees(g_persistRotSnapDegrees);
    g_renderer->setMoveSnapEnabled(g_persistMoveSnapEnabled != 0);
    g_renderer->setMoveSnapUnits(g_persistMoveSnapUnits);
    g_renderer->setEditorTimelineDuration(g_persistEditorTimelineDuration);

    // Apply time/pause last; setJsonAnimationPath() resets pause.
    g_renderer->seekJsonAnimation(g_persistAnimTime);
    g_renderer->setJsonAnimationPaused(g_persistPaused != 0);

    // Keep legacy UI in sync if it's visible.
    UpdateGuiPlayback();
}

// -----------------------------------------------------------------------------
// ImGui glue wrappers (implemented in external DLL)
// -----------------------------------------------------------------------------

static HMODULE g_imguiModule = NULL;
static ImGuiGlue_Init_t      fpImGui_Init = NULL;
static ImGuiGlue_Shutdown_t  fpImGui_Shutdown = NULL;
static ImGuiGlue_WndProc_t   fpImGui_WndProc = NULL;
static ImGuiGlue_NewFrame_t  fpImGui_NewFrame = NULL;
static ImGuiGlue_DrawFrame_t fpImGui_DrawFrame = NULL;
static ImGuiGlue_Render_t    fpImGui_Render = NULL;
static ImGuiGlue_InvalidateDeviceObjects_t fpImGui_Invalidate = NULL;
static ImGuiGlue_CreateDeviceObjects_t     fpImGui_Create = NULL;

static bool LoadImGuiGlue()
{
    if (g_imguiAvailable) return true;

    g_imguiModule = LoadLibraryA("imgui_d3d9.dll");
    if (!g_imguiModule)
    {
        DebugLog("ImGui glue DLL not found (imgui_d3d9.dll). Docking UI disabled.");
        return false;
    }

    fpImGui_Init      = (ImGuiGlue_Init_t)     GetProcAddress(g_imguiModule, "ImGuiGlue_Init");
    fpImGui_Shutdown  = (ImGuiGlue_Shutdown_t) GetProcAddress(g_imguiModule, "ImGuiGlue_Shutdown");
    fpImGui_WndProc   = (ImGuiGlue_WndProc_t)  GetProcAddress(g_imguiModule, "ImGuiGlue_WndProcHandler");
    fpImGui_NewFrame  = (ImGuiGlue_NewFrame_t) GetProcAddress(g_imguiModule, "ImGuiGlue_NewFrame");
    fpImGui_DrawFrame = (ImGuiGlue_DrawFrame_t)GetProcAddress(g_imguiModule, "ImGuiGlue_DrawFrame");
    fpImGui_Render    = (ImGuiGlue_Render_t)   GetProcAddress(g_imguiModule, "ImGuiGlue_Render");
    // Optional helpers for DX9 Reset (fullscreen/resize).
    fpImGui_Invalidate = (ImGuiGlue_InvalidateDeviceObjects_t)GetProcAddress(g_imguiModule, "ImGuiGlue_InvalidateDeviceObjects");
    fpImGui_Create     = (ImGuiGlue_CreateDeviceObjects_t)    GetProcAddress(g_imguiModule, "ImGuiGlue_CreateDeviceObjects");

    if (!fpImGui_Init || !fpImGui_Shutdown || !fpImGui_WndProc ||
        !fpImGui_NewFrame || !fpImGui_DrawFrame || !fpImGui_Render)
    {
        DebugLog("ImGui glue DLL is missing required exports.");
        FreeLibrary(g_imguiModule);
        g_imguiModule = NULL;
        fpImGui_Invalidate = NULL;
        fpImGui_Create = NULL;
        return false;
    }

    g_imguiAvailable = true;
    return true;
}

// DX9 Reset helpers: called by the renderer around operations that may Reset the device.
void ImGui_PreReset()
{
    ReleaseModelViewerRenderTarget();
    if (g_imguiInitialized && fpImGui_Invalidate)
    {
        fpImGui_Invalidate();
    }
}

void ImGui_PostReset()
{
    if (g_imguiInitialized && fpImGui_Create)
    {
        fpImGui_Create();
    }
}

void BeginImGuiFrame()
{
    if (g_imguiAvailable && fpImGui_NewFrame)
    {
        fpImGui_NewFrame();
    }
}

/* Forward declarations for helper functions */
static void UpdateResolvedClipsForActiveState();
static void LoadGameFiles();
static void SelectGameAnimation();
static void SelectBlendAnimation();
static void SelectGameModel();
static void CreateGui();
static void LayoutGui();
static void RefreshGameFilesUI();
static void ApplyGuiFont();
static void ApplyTheme();
static void InvalidateAllTheme();
static void RefreshLuaScriptList();
static void RefreshLuaStateScriptList();
static void RefreshLuaSrcFileList();
static void ClearLuaAnimTableView();
static void BuildLuaAnimTableView();
static void ClearLuaAnimStatesView();
static void BuildLuaAnimStatesView();
static void LoadLuaAnimTableFromPath();
static void LoadLuaAnimStatesFromPath();
static void PlayLuaAnimClipByName();
static void ResolveLuaClipFromActiveTable();

void DrawDockspaceAndPanels()
{
    if (!g_imguiAvailable || !fpImGui_DrawFrame)
    {
        return;
    }

    // Build temporary name arrays for models/anims
    static std::vector<const char*> s_modelNames;
    static std::vector<const char*> s_animNames;
    static std::vector<const char*> s_boneNames;
    static std::vector<int> s_boneParents;
    static std::vector<float> s_eventTimes;
    static std::vector<int> s_eventCats;
    static std::vector<std::string> s_eventSummariesStorage;
    static std::vector<const char*> s_eventSummaries;
    static std::vector<const char*> s_effectNames;
    static std::vector<const char*> s_skyboxNames;
    static std::vector<std::string> s_materialLinesStorage;
    static std::vector<const char*> s_materialLines;
    static std::vector<std::string> s_graphStateNamesStorage;
    static std::vector<const char*> s_graphStateNames;
    static std::vector<std::string> s_graphParamNamesStorage;
    static std::vector<const char*> s_graphParamNames;
    static std::vector<int> s_graphParamTypes;
    static std::vector<float> s_graphParamFloats;
    static std::vector<int> s_graphParamInts;
    static std::vector<int> s_graphParamBools;
    static std::vector<float> s_graphParamMins;
    static std::vector<float> s_graphParamMaxs;
    static std::vector<int> s_graphParamHasRange;
    /* State machine visualization data */
    static std::vector<const char*> s_graphStateClips;
    static std::vector<int> s_graphStateLoopFlags;
    static std::vector<float> s_graphStateSpeeds;
    static std::vector<float> s_graphStateDurations;
    static std::vector<int> s_graphStateTransCounts;
    static std::vector<int> s_graphStateTransTargetsFlat;
    static std::vector<int> s_graphStateOnEnterCounts;
    static std::vector<int> s_graphStateOnExitCounts;
    static std::vector<const char*> s_graphStateOnEnterNames;
    static std::vector<const char*> s_graphStateOnExitNames;
    static std::vector<int> s_graphStateHasSubMachine;
    static std::vector<int> s_graphStateHasBlendGraph;
    static std::vector<const char*> s_graphStateSubMachineNames;
    static std::vector<const char*> s_graphStateBlendGraphNames;
    static std::vector<const char*> s_activeTransCondTexts;
    static std::vector<std::string> s_activeTransCondStorage;
    static std::vector<int> s_cvNamedIndices;
    static std::vector<const char*> s_cvNamedLabels;
    static std::vector<std::string> s_cvNamedLabelStorage;
    static char s_warningText[256] = "";
    s_modelNames.clear();
    s_animNames.clear();
    s_boneNames.clear();
    s_boneParents.clear();
    s_eventTimes.clear();
    s_eventCats.clear();
    s_eventSummariesStorage.clear();
    s_eventSummaries.clear();
    s_effectNames.clear();
    s_skyboxNames.clear();
    s_materialLinesStorage.clear();
    s_materialLines.clear();
    s_graphStateNamesStorage.clear();
    s_graphStateNames.clear();
    s_graphStateClips.clear();
    s_graphStateLoopFlags.clear();
    s_graphStateSpeeds.clear();
    s_graphStateDurations.clear();
    s_graphStateTransCounts.clear();
    s_graphStateTransTargetsFlat.clear();
    s_graphStateOnEnterCounts.clear();
    s_graphStateOnExitCounts.clear();
    s_graphStateOnEnterNames.clear();
    s_graphStateOnExitNames.clear();
    s_graphStateHasSubMachine.clear();
    s_graphStateHasBlendGraph.clear();
    s_graphStateSubMachineNames.clear();
    s_graphStateBlendGraphNames.clear();
    s_activeTransCondTexts.clear();
    s_activeTransCondStorage.clear();
    s_cvNamedIndices.clear();
    s_cvNamedLabels.clear();
    s_cvNamedLabelStorage.clear();
    s_graphParamNamesStorage.clear();
    s_graphParamNames.clear();
    s_graphParamTypes.clear();
    s_graphParamFloats.clear();
    s_graphParamInts.clear();
    s_graphParamBools.clear();
    s_graphParamMins.clear();
    s_graphParamMaxs.clear();
    s_graphParamHasRange.clear();
    s_ikChainNamesStorage.clear();
    s_ikChainNames.clear();
    s_ikChainEnabledFlags.clear();
    s_warningText[0] = '\0';
    if (g_gameFilesBrowser)
    {
        const std::vector<AssetBrowser::AssetInfo>& jm = g_gameFilesBrowser->getJModels();
        for (size_t i = 0; i < jm.size(); ++i)
        {
            s_modelNames.push_back(jm[i].name.c_str());
        }
        const std::vector<AssetBrowser::AssetInfo>& an = g_gameFilesBrowser->getAnimations();
        for (size_t i = 0; i < an.size(); ++i)
        {
            s_animNames.push_back(an[i].name.c_str());
        }
    }

    // Effects list (from RefreshEffectsList / g_effectsList)
    for (size_t i = 0; i < g_effectsList.size(); ++i)
    {
        s_effectNames.push_back(g_effectsList[i].c_str());
    }

    if (g_renderer)
    {
        // Skybox list (preloaded from GameFiles jmodels)
        int skyCount = g_renderer->getSkyboxCount();
        if (skyCount > 0)
        {
            s_skyboxNames.reserve((size_t)skyCount);
            for (int i = 0; i < skyCount; ++i)
            {
                s_skyboxNames.push_back(g_renderer->getSkyboxName(i));
            }
        }

        Scene3DRenderer::MaterialInfo mi;
        int matCount = g_renderer->getMaterialCount();
        if (matCount > 0)
        {
            s_materialLinesStorage.reserve((size_t)matCount);
            s_materialLines.reserve((size_t)matCount);
            for (int i = 0; i < matCount; ++i)
            {
                if (!g_renderer->getMaterialInfo(i, mi))
                {
                    continue;
                }
                char line[256];
                sprintf(line, "%d: %s  D:%s%s  UV:%s%s",
                        i,
                        mi.material[0] ? mi.material : "<unnamed>",
                        mi.diffuse[0] ? mi.diffuse : "<none>",
                        mi.hasDiffuse ? "" : " (missing)",
                        mi.hasTexcoord0 ? "yes" : "no",
                        mi.generatedUV ? " (gen)" : "");
                s_materialLinesStorage.push_back(line);
            }
            for (size_t i = 0; i < s_materialLinesStorage.size(); ++i)
            {
                s_materialLines.push_back(s_materialLinesStorage[i].c_str());
            }
        }

        char warnBuf[256];
        if (g_renderer->getValidationWarning(warnBuf, sizeof(warnBuf)))
        {
            strncpy(s_warningText, warnBuf, sizeof(s_warningText) - 1);
            s_warningText[sizeof(s_warningText) - 1] = '\0';
        }

        int boneCount = g_renderer->getSkeletonBoneCount();
        if (boneCount > 0)
        {
            s_boneNames.reserve((size_t)boneCount);
            s_boneParents.reserve((size_t)boneCount);
            for (int i = 0; i < boneCount; ++i)
            {
                s_boneNames.push_back(g_renderer->getSkeletonBoneName(i));
                s_boneParents.push_back(g_renderer->getSkeletonParentIndex(i));
            }
        }

        int evtCount = g_renderer->getJsonAnimEventCount();
        if (evtCount > 0)
        {
            s_eventTimes.reserve((size_t)evtCount);
            s_eventCats.reserve((size_t)evtCount);
            s_eventSummariesStorage.reserve((size_t)evtCount);
            s_eventSummaries.reserve((size_t)evtCount);

            for (int i = 0; i < evtCount; ++i)
            {
                s_eventTimes.push_back(g_renderer->getJsonAnimEventTime(i));
                s_eventCats.push_back(g_renderer->getJsonAnimEventCategory(i));
                char sum[256];
                g_renderer->getJsonAnimEventSummary(i, sum, sizeof(sum));
                s_eventSummariesStorage.push_back(sum);
            }
            for (size_t i = 0; i < s_eventSummariesStorage.size(); ++i)
            {
                s_eventSummaries.push_back(s_eventSummariesStorage[i].c_str());
            }
        }

        // Animation graph state/params
        int graphStateCount = g_renderer->getAnimationGraphStateCount();
        if (graphStateCount > 0)
        {
            s_graphStateNamesStorage.reserve((size_t)graphStateCount);
            s_graphStateNames.reserve((size_t)graphStateCount);
            for (int i = 0; i < graphStateCount; ++i)
            {
                const char* name = g_renderer->getAnimationGraphStateName(i);
                s_graphStateNamesStorage.push_back(name ? name : "");
            }
            for (size_t i = 0; i < s_graphStateNamesStorage.size(); ++i)
            {
                s_graphStateNames.push_back(s_graphStateNamesStorage[i].c_str());
            }

            /* Per-state data for state machine visualization */
            s_graphStateClips.reserve((size_t)graphStateCount);
            s_graphStateLoopFlags.reserve((size_t)graphStateCount);
            s_graphStateSpeeds.reserve((size_t)graphStateCount);
            s_graphStateDurations.reserve((size_t)graphStateCount);
            s_graphStateTransCounts.reserve((size_t)graphStateCount);
            s_graphStateOnEnterCounts.reserve((size_t)graphStateCount);
            s_graphStateOnExitCounts.reserve((size_t)graphStateCount);
            s_graphStateHasSubMachine.reserve((size_t)graphStateCount);
            s_graphStateHasBlendGraph.reserve((size_t)graphStateCount);
            s_graphStateSubMachineNames.reserve((size_t)graphStateCount);
            s_graphStateBlendGraphNames.reserve((size_t)graphStateCount);
            for (int i = 0; i < graphStateCount; ++i)
            {
                s_graphStateClips.push_back(g_renderer->getAnimationGraphStateClip(i));
                s_graphStateLoopFlags.push_back(g_renderer->getAnimationGraphStateLoop(i) ? 1 : 0);
                s_graphStateSpeeds.push_back(g_renderer->getAnimationGraphStateSpeed(i));
                s_graphStateDurations.push_back(g_renderer->getAnimationGraphStateStateDuration(i));
                int tc = g_renderer->getAnimationGraphStateTransitionCount(i);
                s_graphStateTransCounts.push_back(tc);
                for (int t = 0; t < tc; ++t)
                    s_graphStateTransTargetsFlat.push_back(g_renderer->getAnimationGraphStateTransitionTarget(i, t));
                int enterCount = g_renderer->getAnimationGraphStateOnEnterActionCount(i);
                int exitCount = g_renderer->getAnimationGraphStateOnExitActionCount(i);
                s_graphStateOnEnterCounts.push_back(enterCount);
                s_graphStateOnExitCounts.push_back(exitCount);
                for (int a = 0; a < enterCount; ++a)
                    s_graphStateOnEnterNames.push_back(g_renderer->getAnimationGraphStateOnEnterActionName(i, a));
                for (int a = 0; a < exitCount; ++a)
                    s_graphStateOnExitNames.push_back(g_renderer->getAnimationGraphStateOnExitActionName(i, a));
                s_graphStateHasSubMachine.push_back(g_renderer->getAnimationGraphStateHasSubMachine(i) ? 1 : 0);
                s_graphStateHasBlendGraph.push_back(g_renderer->getAnimationGraphStateHasBlendGraph(i) ? 1 : 0);
                s_graphStateSubMachineNames.push_back(g_renderer->getAnimationGraphStateSubMachineName(i));
                s_graphStateBlendGraphNames.push_back(g_renderer->getAnimationGraphStateBlendGraphName(i));
            }
        }

        int graphParamCount = g_renderer->getAnimationGraphParamCount();
        if (graphParamCount > 0)
        {
            s_graphParamNamesStorage.reserve((size_t)graphParamCount);
            s_graphParamNames.reserve((size_t)graphParamCount);
            s_graphParamTypes.reserve((size_t)graphParamCount);
            s_graphParamFloats.reserve((size_t)graphParamCount);
            s_graphParamInts.reserve((size_t)graphParamCount);
            s_graphParamBools.reserve((size_t)graphParamCount);
            s_graphParamMins.reserve((size_t)graphParamCount);
            s_graphParamMaxs.reserve((size_t)graphParamCount);
            s_graphParamHasRange.reserve((size_t)graphParamCount);

            for (int i = 0; i < graphParamCount; ++i)
            {
                const char* name = g_renderer->getAnimationGraphParamName(i);
                s_graphParamNamesStorage.push_back(name ? name : "");
                s_graphParamTypes.push_back(g_renderer->getAnimationGraphParamType(i));
                s_graphParamFloats.push_back(g_renderer->getAnimationGraphParamFloat(i));
                s_graphParamInts.push_back(g_renderer->getAnimationGraphParamInt(i));
                s_graphParamBools.push_back(g_renderer->getAnimationGraphParamBool(i) ? 1 : 0);
                s_graphParamMins.push_back(g_renderer->getAnimationGraphParamMin(i));
                s_graphParamMaxs.push_back(g_renderer->getAnimationGraphParamMax(i));
                s_graphParamHasRange.push_back(g_renderer->getAnimationGraphParamHasRange(i) ? 1 : 0);
            }
            for (size_t i = 0; i < s_graphParamNamesStorage.size(); ++i)
            {
                s_graphParamNames.push_back(s_graphParamNamesStorage[i].c_str());
            }
        }
    }

    ImGuiGlueFrameArgs args;
    memset(&args, 0, sizeof(args));
    // Size check — log once so we can compare with DLL side
    {
        static bool s_sizeLogged = false;
        if (!s_sizeLogged) {
            char logBuf[256];
            sprintf_s(logBuf, "EXE sizeof(ImGuiGlueFrameArgs) = %u", (unsigned)sizeof(args));
            DebugLog(logBuf);
            OutputDebugStringA(logBuf); OutputDebugStringA("\n");
            s_sizeLogged = true;
        }
    }
    args.mainHwnd     = g_hwnd;
    // When running the ImGui docking UI, the viewport renders behind the UI in the same backbuffer.
    // Passing NULL disables the child-HWND embedding path in the glue DLL.
    args.viewportHwnd = (g_showLegacyUI ? g_viewportHwnd : NULL);
    args.showOverlay  = g_showOverlay ? 1 : 0;
    args.fps          = g_fps;
    args.modelName    = (g_activeModelName[0] != '\0') ? g_activeModelName : "<none>";
    args.animName     = (g_activeAnimName[0] != '\0') ? g_activeAnimName : "<none>";
    args.modelNames   = s_modelNames.empty() ? NULL : &s_modelNames[0];
    args.modelCount   = (int)s_modelNames.size();
    args.selectedModel= g_browserIndexModel;
    args.animNames    = s_animNames.empty() ? NULL : &s_animNames[0];
    args.animCount    = (int)s_animNames.size();
    args.selectedAnim = g_browserIndexAnim;
    args.animTime     = g_renderer ? g_renderer->getJsonAnimationTime() : 0.0f;
    args.animDuration = g_renderer ? g_renderer->getJsonAnimationDuration() : 0.0f;
    args.playbackSpeed= g_renderer ? g_renderer->getPlaybackSpeed() : 1.0f;
    args.loopEnabled  = g_renderer ? (g_renderer->getLoopAnimation() ? 1 : 0) : 0;
    args.playing      = g_renderer ? (g_renderer->isJsonAnimationPaused() ? 0 : 1) : 0;
    args.rotInterpMode = g_renderer ? g_renderer->getRotInterpMode() : 0;
    args.animFixedEnabled = g_renderer ? (g_renderer->getAnimFixedStepEnabled() ? 1 : 0) : 0;
    args.animFixedStep = g_renderer ? g_renderer->getAnimFixedStep() : (1.0f / 30.0f);
    args.animFixedMaxSteps = g_renderer ? g_renderer->getAnimFixedMaxSteps() : 0;
    args.animFixedAccumulator = g_renderer ? g_renderer->getAnimFixedAccumulator() : 0.0f;
    args.animFixedStepsLast = g_renderer ? g_renderer->getAnimFixedStepsLast() : 0;
    args.timeWarpEnabled = g_renderer ? (g_renderer->getTimeWarpEnabled() ? 1 : 0) : 0;
    args.timeWarpType = g_renderer ? g_renderer->getTimeWarpEasingType() : 0;
    float tw1x = 0.0f, tw1y = 0.0f, tw2x = 1.0f, tw2y = 1.0f;
    if (g_renderer)
    {
        g_renderer->getTimeWarpControlPoints(tw1x, tw1y, tw2x, tw2y);
    }
    args.timeWarpCp1x = tw1x;
    args.timeWarpCp1y = tw1y;
    args.timeWarpCp2x = tw2x;
    args.timeWarpCp2y = tw2y;
    args.rootMotionMode = g_renderer ? (int)g_renderer->getRootMotionMode() : 0;
    args.rootMotionLockX = g_renderer ? (g_renderer->isRootMotionLockedX() ? 1 : 0) : 0;
    args.rootMotionLockY = g_renderer ? (g_renderer->isRootMotionLockedY() ? 1 : 0) : 0;
    args.rootMotionLockZ = g_renderer ? (g_renderer->isRootMotionLockedZ() ? 1 : 0) : 0;
    float rmx = 0.0f, rmy = 0.0f, rmz = 0.0f;
    if (g_renderer)
    {
        g_renderer->getRootMotionOffset(rmx, rmy, rmz);
    }
    args.rootMotionOffsetX = rmx;
    args.rootMotionOffsetY = rmy;
    args.rootMotionOffsetZ = rmz;
    args.rootMotionWarpEnabled = g_renderer ? (g_renderer->getRootMotionWarpEnabled() ? 1 : 0) : 0;
    args.rootMotionWarpMode = g_renderer ? g_renderer->getRootMotionWarpMode() : 0;
    float rwtX = 0.0f, rwtY = 0.0f, rwtZ = 0.0f;
    if (g_renderer)
    {
        g_renderer->getRootMotionWarpTarget(rwtX, rwtY, rwtZ);
    }
    args.rootMotionWarpTargetX = rwtX;
    args.rootMotionWarpTargetY = rwtY;
    args.rootMotionWarpTargetZ = rwtZ;
    args.compressAuto = g_renderer ? (g_renderer->getCompressionAuto() ? 1 : 0) : 0;
    args.compressQuantizeRot = g_renderer ? (g_renderer->getCompressionQuantizeRotations() ? 1 : 0) : 0;
    args.compressStripRot = g_renderer ? (g_renderer->getCompressionStripRotations() ? 1 : 0) : 0;
    float cPos = 0.0f, cRot = 0.0f, cScale = 0.0f, cRoot = 0.0f;
    if (g_renderer)
    {
        g_renderer->getCompressionTolerances(cPos, cRot, cScale, cRoot);
    }
    args.compressPosTol = cPos;
    args.compressRotTolDeg = cRot;
    args.compressScaleTol = cScale;
    args.compressRootTol = cRoot;
    args.compressStatsValid = g_renderer ? (g_renderer->hasLastCompressionStats() ? 1 : 0) : 0;
    if (g_renderer && args.compressStatsValid)
    {
        g_renderer->getLastCompressionStats(args.compressRotBefore, args.compressRotAfter,
                                             args.compressTransBefore, args.compressTransAfter,
                                             args.compressScaleBefore, args.compressScaleAfter,
                                             args.compressRootBefore, args.compressRootAfter);
    }
    else
    {
        args.compressRotBefore = args.compressRotAfter = 0;
        args.compressTransBefore = args.compressTransAfter = 0;
        args.compressScaleBefore = args.compressScaleAfter = 0;
        args.compressRootBefore = args.compressRootAfter = 0;
    }
    args.ikEnabled = g_renderer ? (g_renderer->getIKEnabled() ? 1 : 0) : 0;
    args.footIkEnabled = g_renderer ? (g_renderer->getFootIKEnabled() ? 1 : 0) : 0;
    args.lookAtEnabled = g_renderer ? (g_renderer->getLookAtIKEnabled() ? 1 : 0) : 0;
    args.aimEnabled = g_renderer ? (g_renderer->getAimIKEnabled() ? 1 : 0) : 0;
    args.lookAtBoneIndex = g_renderer ? g_renderer->getLookAtBoneIndex() : -1;
    args.aimBoneIndex = g_renderer ? g_renderer->getAimBoneIndex() : -1;
    float lkx = 0.0f, lky = 0.0f, lkz = 0.0f;
    float amx = 0.0f, amy = 0.0f, amz = 0.0f;
    if (g_renderer)
    {
        g_renderer->getLookAtTarget(lkx, lky, lkz);
        g_renderer->getAimTarget(amx, amy, amz);
    }
    args.lookAtTargetX = lkx;
    args.lookAtTargetY = lky;
    args.lookAtTargetZ = lkz;
    args.aimTargetX = amx;
    args.aimTargetY = amy;
    args.aimTargetZ = amz;
    args.lookAtWeight = g_renderer ? g_renderer->getLookAtWeight() : 1.0f;
    args.aimWeight = g_renderer ? g_renderer->getAimWeight() : 1.0f;
    args.ikChainCount = (int)s_ikChainNames.size();
    args.ikChainNames = s_ikChainNames.empty() ? NULL : &s_ikChainNames[0];
    args.ikChainEnabledFlags = s_ikChainEnabledFlags.empty() ? NULL : &s_ikChainEnabledFlags[0];
    if (s_ikSelectedChain >= args.ikChainCount)
        s_ikSelectedChain = -1;
    args.ikSelectedChain = s_ikSelectedChain;
    args.ikSelectedTargetX = 0.0f;
    args.ikSelectedTargetY = 0.0f;
    args.ikSelectedTargetZ = 0.0f;
    if (g_renderer && s_ikSelectedChain >= 0)
    {
        float tx = 0.0f, ty = 0.0f, tz = 0.0f;
        if (g_renderer->getIKChainTarget(s_ikSelectedChain, tx, ty, tz))
        {
            args.ikSelectedTargetX = tx;
            args.ikSelectedTargetY = ty;
            args.ikSelectedTargetZ = tz;
        }
    }
    args.physEnabled = g_renderer ? (g_renderer->getPhysicalAnimEnabled() ? 1 : 0) : 0;
    args.ragdollEnabled = g_renderer ? (g_renderer->getRagdollEnabled() ? 1 : 0) : 0;
    args.ragdollBlend = g_renderer ? g_renderer->getRagdollBlend() : 0.0f;
    args.physPosStiffness = g_renderer ? g_renderer->getPhysicalPosStiffness() : 0.0f;
    args.physPosDamping = g_renderer ? g_renderer->getPhysicalPosDamping() : 0.0f;
    args.physRotStiffness = g_renderer ? g_renderer->getPhysicalRotStiffness() : 0.0f;
    args.physRotDamping = g_renderer ? g_renderer->getPhysicalRotDamping() : 0.0f;
    args.ragdollGravity = g_renderer ? g_renderer->getRagdollGravity() : -9.81f;
    args.blendEnabled = g_renderer ? (g_renderer->getJsonBlendEnabled() ? 1 : 0) : 0;
    args.blendMode    = g_renderer ? g_renderer->getJsonBlendMode() : 0;
    args.blendRotMode = g_renderer ? g_renderer->getJsonBlendRotMode() : 0;
    args.blendAlpha   = g_renderer ? g_renderer->getJsonBlendAlpha() : 0.0f;
    args.blendAnimIndex = (g_blendAnimIndex >= 0 && g_blendAnimIndex < (int)s_animNames.size()) ? g_blendAnimIndex : -1;
    args.blendLayerRootBone = g_renderer ? g_renderer->getJsonBlendLayerRootBone() : -1;
    // Multi-clip blend layers
    static int s_blendLayerActive[16] = {};
    static float s_blendLayerWeights[16] = {};
    static int s_blendLayerClipIndices[16] = { -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1 };
    static int s_blendLayerLoops[16] = { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 };
    static unsigned char s_blendLayerBoneMasks[16 * 62] = {};
    if (g_renderer)
    {
        for (int li = 0; li < 16; ++li)
        {
            s_blendLayerActive[li] = g_renderer->m_blendLayers[li].active ? 1 : 0;
            s_blendLayerWeights[li] = g_renderer->m_blendLayers[li].weight;
            s_blendLayerLoops[li] = g_renderer->m_blendLayers[li].loop ? 1 : 0;
            memcpy(&s_blendLayerBoneMasks[li * 62], g_renderer->m_blendLayers[li].boneMask, 62);
        }
    }
    args.blendLayerActive = s_blendLayerActive;
    args.blendLayerWeights = s_blendLayerWeights;
    args.blendLayerClipIndices = s_blendLayerClipIndices;
    args.blendLayerLoops = s_blendLayerLoops;
    args.blendLayerBoneMasks = s_blendLayerBoneMasks;
    args.blendAutoNormalize = g_renderer ? (g_renderer->m_blendAutoNormalize ? 1 : 0) : 1;
    args.poseSnapshotValid = g_renderer ? (g_renderer->m_poseSnapshotValid ? 1 : 0) : 0;
    args.poseSnapshotBlendActive = g_renderer ? (g_renderer->m_poseSnapshotBlendActive ? 1 : 0) : 0;
    args.poseSnapshotBlendAlpha = g_renderer ? g_renderer->m_poseSnapshotBlendAlpha : 1.0f;
    args.rootPathEnabled = g_renderer ? (g_renderer->m_rootPathEnabled ? 1 : 0) : 0;
    args.rootPathCount = g_renderer ? g_renderer->m_rootPathCount : 0;

    args.loopRegionEnabled = g_renderer ? (g_renderer->m_loopRegionEnabled ? 1 : 0) : 0;
    args.loopRegionIn = g_renderer ? g_renderer->m_loopRegionIn : 0.0f;
    args.loopRegionOut = g_renderer ? g_renderer->m_loopRegionOut : 0.0f;
    args.blendIndependentTime = g_renderer ? (g_renderer->m_jsonBlendIndependentTime ? 1 : 0) : 1;
    args.blendLoopBlendClip = g_renderer ? (g_renderer->m_jsonBlendLoopBlendClip ? 1 : 0) : 1;
    args.blendFadeDuration = g_renderer ? g_renderer->m_jsonBlendFadeDuration : 0.0f;
    const int graphStateCount = g_renderer ? g_renderer->getAnimationGraphStateCount() : 0;
    const bool graphHasStates = (graphStateCount > 0);
    if (graphHasStates && g_renderer)
        g_graphEnabledDesired = g_renderer->getAnimationGraphEnabled();
    args.graphEnabled = graphHasStates ? (g_renderer->getAnimationGraphEnabled() ? 1 : 0) : (g_graphEnabledDesired ? 1 : 0);
    args.graphName = g_renderer ? g_renderer->getAnimationGraphName() : NULL;
    args.graphPath = g_renderer ? g_renderer->getAnimationGraphPath() : NULL;
    args.graphListCount = (int)s_graphListNames.size();
    args.graphListNames = s_graphListNames.empty() ? NULL : &s_graphListNames[0];
    args.graphListPaths = s_graphListPaths.empty() ? NULL : &s_graphListPaths[0];
    args.graphStateCount = (int)s_graphStateNames.size();
    args.graphStateNames = s_graphStateNames.empty() ? NULL : &s_graphStateNames[0];
    args.graphCurrentState = g_renderer ? g_renderer->getAnimationGraphCurrentState() : -1;
    args.graphNextState = g_renderer ? g_renderer->getAnimationGraphNextState() : -1;
    args.graphInTransition = g_renderer ? (g_renderer->isAnimationGraphInTransition() ? 1 : 0) : 0;
    args.graphStateTime = g_renderer ? g_renderer->getAnimationGraphStateTime() : 0.0f;
    args.graphStateDuration = g_renderer ? g_renderer->getAnimationGraphStateDuration() : 0.0f;
    args.graphTransitionTime = g_renderer ? g_renderer->getAnimationGraphTransitionTime() : 0.0f;
    args.graphTransitionDuration = g_renderer ? g_renderer->getAnimationGraphTransitionDuration() : 0.0f;
    args.graphParamCount = (int)s_graphParamNames.size();
    args.graphParamNames = s_graphParamNames.empty() ? NULL : &s_graphParamNames[0];
    args.graphParamTypes = s_graphParamTypes.empty() ? NULL : &s_graphParamTypes[0];
    args.graphParamFloats = s_graphParamFloats.empty() ? NULL : &s_graphParamFloats[0];
    args.graphParamInts = s_graphParamInts.empty() ? NULL : &s_graphParamInts[0];
    args.graphParamBools = s_graphParamBools.empty() ? NULL : &s_graphParamBools[0];
    args.graphParamMins = s_graphParamMins.empty() ? NULL : &s_graphParamMins[0];
    args.graphParamMaxs = s_graphParamMaxs.empty() ? NULL : &s_graphParamMaxs[0];
    args.graphParamHasRange = s_graphParamHasRange.empty() ? NULL : &s_graphParamHasRange[0];

    /* State machine visualization data */
    args.graphStateClips = s_graphStateClips.empty() ? NULL : &s_graphStateClips[0];
    args.graphStateLoopFlags = s_graphStateLoopFlags.empty() ? NULL : &s_graphStateLoopFlags[0];
    args.graphStateSpeeds = s_graphStateSpeeds.empty() ? NULL : &s_graphStateSpeeds[0];
    args.graphStateDurations = s_graphStateDurations.empty() ? NULL : &s_graphStateDurations[0];
    args.graphStateTransitionCounts = s_graphStateTransCounts.empty() ? NULL : &s_graphStateTransCounts[0];
    args.graphStateTransitionTargetsFlat = s_graphStateTransTargetsFlat.empty() ? NULL : &s_graphStateTransTargetsFlat[0];
    args.graphStateTransitionTotalCount = (int)s_graphStateTransTargetsFlat.size();
    args.graphStateOnEnterActionCounts = s_graphStateOnEnterCounts.empty() ? NULL : &s_graphStateOnEnterCounts[0];
    args.graphStateOnExitActionCounts = s_graphStateOnExitCounts.empty() ? NULL : &s_graphStateOnExitCounts[0];
    args.graphStateOnEnterActionNames = s_graphStateOnEnterNames.empty() ? NULL : &s_graphStateOnEnterNames[0];
    args.graphStateOnExitActionNames = s_graphStateOnExitNames.empty() ? NULL : &s_graphStateOnExitNames[0];
    args.graphStateOnEnterTotalCount = (int)s_graphStateOnEnterNames.size();
    args.graphStateOnExitTotalCount = (int)s_graphStateOnExitNames.size();
    args.graphStateHasSubMachine = s_graphStateHasSubMachine.empty() ? NULL : &s_graphStateHasSubMachine[0];
    args.graphStateHasBlendGraph = s_graphStateHasBlendGraph.empty() ? NULL : &s_graphStateHasBlendGraph[0];
    args.graphStateSubMachineNames = s_graphStateSubMachineNames.empty() ? NULL : &s_graphStateSubMachineNames[0];
    args.graphStateBlendGraphNames = s_graphStateBlendGraphNames.empty() ? NULL : &s_graphStateBlendGraphNames[0];

    /* Active transition detail */
    if (g_renderer)
    {
        int ati = g_renderer->getAnimationGraphActiveTransitionIndex();
        args.activeTransitionIndex = ati;
        if (ati >= 0)
        {
            args.activeTransitionFromState = g_renderer->getAnimationGraphTransitionFromState(ati);
            args.activeTransitionToState = g_renderer->getAnimationGraphTransitionToState(ati);
            float tTime = g_renderer->getAnimationGraphTransitionTime();
            float tDur = g_renderer->getAnimationGraphTransitionDuration();
            args.activeTransitionProgress = (tDur > 0.0f) ? (tTime / tDur) : 0.0f;
            args.activeTransitionConditionCount = g_renderer->getAnimationGraphTransitionConditionCount(ati);
        }
        else
        {
            args.activeTransitionFromState = -1;
            args.activeTransitionToState = -1;
            args.activeTransitionProgress = 0.0f;
            args.activeTransitionConditionCount = 0;
        }
        args.activeTransitionConditionTexts = NULL;
    }
    else
    {
        args.activeTransitionIndex = -1;
        args.activeTransitionFromState = -1;
        args.activeTransitionToState = -1;
        args.activeTransitionProgress = 0.0f;
        args.activeTransitionConditionCount = 0;
        args.activeTransitionConditionTexts = NULL;
    }

    /* Creature Variables */
    if (g_renderer)
    {
        for (int i = 0; i < 64; ++i)
            args.creatureVars[i] = g_renderer->getCreatureVar(i);
    }
    else
    {
        memset(args.creatureVars, 0, sizeof(args.creatureVars));
    }
    /* Named CV labels */
    {
        struct CVLabel { int idx; const char* label; };
        static const CVLabel cvLabels[] = {
            {0, "CV_0 MoveSpeed"}, {1, "CV_1 TurnRate"}, {2, "CV_2 Health"},
            {3, "CV_3 Mana"}, {4, "CV_4 Level"}, {5, "CV_5 Gender"},
            {43, "CV_43 Stance"}, {44, "CV_44 Action"},
            {45, "CV_45 CombatStyle"}, {46, "CV_46 WeaponType"},
            {47, "CV_47 MountType"}, {48, "CV_48 ShieldType"},
            {55, "CV_55 LookAtWeight"}, {56, "CV_56 AimWeight"},
            {59, "CV_59 Random"}, {60, "CV_60 TimeInState"},
            {61, "CV_61 AnimDrivenMoveX"}, {62, "CV_62 AnimDrivenMoveZ"},
            {63, "CV_63 AnimDrivenTurnY"}
        };
        for (int k = 0; k < (int)(sizeof(cvLabels)/sizeof(cvLabels[0])); ++k)
        {
            s_cvNamedIndices.push_back(cvLabels[k].idx);
            s_cvNamedLabelStorage.push_back(cvLabels[k].label);
        }
        for (size_t k = 0; k < s_cvNamedLabelStorage.size(); ++k)
            s_cvNamedLabels.push_back(s_cvNamedLabelStorage[k].c_str());
    }
    args.creatureVarNamedCount = (int)s_cvNamedLabels.size();
    args.creatureVarNamedIndices = s_cvNamedIndices.empty() ? NULL : &s_cvNamedIndices[0];
    args.creatureVarNamedLabels = s_cvNamedLabels.empty() ? NULL : &s_cvNamedLabels[0];

    args.luaTableListCount = (int)s_luaListNames.size();
    args.luaTableListNames = s_luaListNames.empty() ? NULL : &s_luaListNames[0];
    args.luaTableListPaths = s_luaListPaths.empty() ? NULL : &s_luaListPaths[0];
    args.luaTablePath = s_luaAnimTablePath.empty() ? NULL : s_luaAnimTablePath.c_str();
    args.luaTableName = s_luaAnimTableInfo.tableName.empty() ? NULL : s_luaAnimTableInfo.tableName.c_str();
    args.luaTableNameCount = (int)s_luaTableNames.size();
    args.luaTableNames = s_luaTableNames.empty() ? NULL : &s_luaTableNames[0];
    args.luaTableUsedCount = (int)s_luaTableUsed.size();
    args.luaTableUsedNames = s_luaTableUsed.empty() ? NULL : &s_luaTableUsed[0];
    args.luaTableEntryCount = (int)s_luaEntryKeys.size();
    args.luaTableEntryKeys = s_luaEntryKeys.empty() ? NULL : &s_luaEntryKeys[0];
    args.luaTableEntryValues = s_luaEntryValues.empty() ? NULL : &s_luaEntryValues[0];
    args.luaTableEntryPrimaryClips = s_luaEntryPrimary.empty() ? NULL : &s_luaEntryPrimary[0];
    args.luaTableEntrySources = s_luaEntrySources.empty() ? NULL : &s_luaEntrySources[0];
    args.luaTableMissingCount = (int)s_luaMissingClips.size();
    args.luaTableMissingNames = s_luaMissingClips.empty() ? NULL : &s_luaMissingClips[0];
    args.luaTableMissingTableCount = (int)s_luaMissingTables.size();
    args.luaTableMissingTableNames = s_luaMissingTables.empty() ? NULL : &s_luaMissingTables[0];
    args.luaTableError = s_luaAnimTableLastError.empty() ? NULL : s_luaAnimTableLastError.c_str();
    args.luaStatesListCount = (int)s_luaStateListNames.size();
    args.luaStatesListNames = s_luaStateListNames.empty() ? NULL : &s_luaStateListNames[0];
    args.luaStatesListPaths = s_luaStateListPaths.empty() ? NULL : &s_luaStateListPaths[0];
    args.luaStatesPath = s_luaAnimStatesPath.empty() ? NULL : s_luaAnimStatesPath.c_str();
    args.luaStatesEntryCount = (int)s_luaStateKeys.size();
    args.luaStatesEntryKeys = s_luaStateKeys.empty() ? NULL : &s_luaStateKeys[0];
    args.luaStatesEntryNames = s_luaStateNames.empty() ? NULL : &s_luaStateNames[0];
    args.luaStatesEntryStances = s_luaStateStances.empty() ? NULL : &s_luaStateStances[0];
    args.luaStatesEntryActions = s_luaStateActions.empty() ? NULL : &s_luaStateActions[0];
    args.luaStatesError = s_luaAnimStatesLastError.empty() ? NULL : s_luaAnimStatesLastError.c_str();
    
    /* Phase 2: Runtime state selector */
    if (g_renderer)
    {
        args.selectedAnimStateIndex = g_renderer->getAnimStatesTranslatorSelectedStateIndex();
        const LuaAnimStateEntry* activeState = g_renderer->getAnimStatesTranslatorActiveState();
        if (activeState)
        {
            args.selectedAnimStateKey = activeState->key.empty() ? NULL : activeState->key.c_str();
            args.selectedAnimStateName = activeState->name.empty() ? NULL : activeState->name.c_str();
        }
        else
        {
            args.selectedAnimStateKey = NULL;
            args.selectedAnimStateName = NULL;
        }
    }
    else
    {
        args.selectedAnimStateIndex = -1;
        args.selectedAnimStateKey = NULL;
        args.selectedAnimStateName = NULL;
    }
    
    /* Phase 2.5: ASM Parity Toggles */
    if (g_renderer)
    {
        args.animationDrivenEnabled = g_renderer->getAnimationDrivenEnabled() ? 1 : 0;
        args.rootMotionWarpEnabled = g_renderer->getRootMotionWarpEnabled() ? 1 : 0;
        args.animDrivenMode = (int)g_renderer->getAnimDrivenMode();
    }
    else
    {
        args.animationDrivenEnabled = 0;
        args.rootMotionWarpEnabled = 0;
        args.animDrivenMode = 0;
    }
    
    /* Phase 3 & 4: Resolved and Filtered Clips */
    if (g_renderer)
    {
        /* Phase 3: Resolved clips (unfiltered) */
        static std::vector<const char*> s_resolvedClipPtrs;
        s_resolvedClipPtrs.clear();
        int resolvedCount = g_renderer->getResolvedClipsForActiveStateCount();
        for (int i = 0; i < resolvedCount; ++i)
        {
            const char* clipPath = g_renderer->getResolvedClipsForActiveState(i);
            if (clipPath && clipPath[0])
                s_resolvedClipPtrs.push_back(clipPath);
        }
        args.resolvedClipsCount = (int)s_resolvedClipPtrs.size();
        args.resolvedClips = s_resolvedClipPtrs.empty() ? NULL : &s_resolvedClipPtrs[0];
        
        /* Phase 4: Filtered clips */
        static std::vector<const char*> s_filteredClipPtrs;
        s_filteredClipPtrs.clear();
        int filteredCount = g_renderer->getFilteredClipsForActiveStateCount();
        for (int i = 0; i < filteredCount; ++i)
        {
            const char* clipPath = g_renderer->getFilteredClipsForActiveState(i);
            if (clipPath && clipPath[0])
                s_filteredClipPtrs.push_back(clipPath);
        }
        args.filteredClipsCount = (int)s_filteredClipPtrs.size();
        args.filteredClips = s_filteredClipPtrs.empty() ? NULL : &s_filteredClipPtrs[0];
        args.selectedClipIndex = -1;  /* UI will set this when user selects a clip */
        
        /* ===== Phase 5: Graph State Synchronization ===== */
        if (g_renderer && g_renderer->getAnimationGraphEnabled())
        {
            /* Get matched graph state for active anim state */
            const char* graphStateName = g_renderer->getGraphStateNameForActiveAnimState();
            args.graphStateForActiveAnimState = graphStateName ? graphStateName : NULL;
            args.graphStateIndexForAnimState = g_renderer->getActiveGraphStateForAnimState();
            
            /* Get current graph state */
            args.currentGraphStateIndex = g_renderer->getAnimationGraphCurrentState();
            args.graphStateCount = g_renderer->getAnimationGraphStateCount();
            
            /* Build array of graph state names */
            static std::vector<const char*> s_graphStateNamePtrs;
            s_graphStateNamePtrs.clear();
            for (int i = 0; i < args.graphStateCount; ++i)
            {
                const char* stateName = g_renderer->getAnimationGraphStateName(i);
                if (stateName && stateName[0])
                    s_graphStateNamePtrs.push_back(stateName);
                else
                    s_graphStateNamePtrs.push_back("<unknown>");
            }
            args.graphStateNames = s_graphStateNamePtrs.empty() ? NULL : &s_graphStateNamePtrs[0];
            
            /* ===== Phase 6: Motion Matching Bridge State Sync ===== */
            args.motionMatchUseFilteredClips = g_renderer->getMotionMatchUseFilteredClips() ? 1 : 0;
            args.motionMatchFilteredClipCount = g_renderer->getMotionMatchFilteredClipCount();
            args.motionMatchFilteredDatabaseValid = (args.motionMatchFilteredClipCount > 0) ? 1 : 0;
        }
        else
        {
            args.graphStateForActiveAnimState = NULL;
            args.graphStateIndexForAnimState = -1;
            args.currentGraphStateIndex = -1;
            args.graphStateCount = 0;
            args.graphStateNames = NULL;
        }
    }
    else
    {
        args.resolvedClipsCount = 0;
        args.resolvedClips = NULL;
        args.filteredClipsCount = 0;
        args.filteredClips = NULL;
        args.selectedClipIndex = -1;
        args.graphStateForActiveAnimState = NULL;
        args.graphStateIndexForAnimState = -1;
        args.currentGraphStateIndex = -1;
        args.graphStateCount = 0;
        args.graphStateNames = NULL;
        
        /* Phase 6: Motion Matching Bridge state sync */
        args.motionMatchUseFilteredClips = 0;
        args.motionMatchFilteredClipCount = 0;
        args.motionMatchFilteredDatabaseValid = 0;
    }
    args.luaSrcFileCount = (int)s_luaSrcNames.size();
    args.luaSrcFileNames = s_luaSrcNames.empty() ? NULL : &s_luaSrcNames[0];
    args.luaSrcFilePaths = s_luaSrcPaths.empty() ? NULL : &s_luaSrcPaths[0];
    args.luaSrcFileCategories = s_luaSrcCats.empty() ? NULL : &s_luaSrcCats[0];
    args.luaDepNodeCount = (int)s_luaDepNamesPtr.size();
    args.luaDepNodeNames = s_luaDepNamesPtr.empty() ? NULL : &s_luaDepNamesPtr[0];
    args.luaDepNodePaths = s_luaDepPathsPtr.empty() ? NULL : &s_luaDepPathsPtr[0];
    args.luaDepNodeTypes = s_luaDepTypesPtr.empty() ? NULL : &s_luaDepTypesPtr[0];
    args.luaDepNodeCategories = s_luaDepCatsPtr.empty() ? NULL : &s_luaDepCatsPtr[0];
    args.luaDepNodeParents = s_luaDepParents.empty() ? NULL : &s_luaDepParents[0];
    args.luaDepNodeDepths = s_luaDepDepths.empty() ? NULL : &s_luaDepDepths[0];
    args.luaDepNodeResolved = s_luaDepResolved.empty() ? NULL : &s_luaDepResolved[0];
    args.luaEditorBuffer = s_luaEditorBuffer.empty() ? NULL : &s_luaEditorBuffer[0];
    args.luaEditorBufferSize = (int)s_luaEditorBuffer.size();
    args.luaEditorBufferLen = s_luaEditorBufferLen;
    args.luaEditorPath = s_luaEditorTargetPath.empty() ? NULL : s_luaEditorTargetPath.c_str();
    args.luaEditorSourcePath = s_luaEditorSourcePath.empty() ? NULL : s_luaEditorSourcePath.c_str();
    args.luaEditorStatus = s_luaEditorStatus.empty() ? NULL : s_luaEditorStatus.c_str();
    args.luaEditorIsBinary = s_luaEditorOriginalBinary ? 1 : 0;
    args.luaGuessedModelName = s_luaGuessedModelName.empty() ? NULL : s_luaGuessedModelName.c_str();
    args.motionMatchEnabled = g_renderer ? (g_renderer->getMotionMatchEnabled() ? 1 : 0) : 0;
    args.motionMatchClipCount = g_renderer ? g_renderer->getMotionMatchClipCount() : 0;
    args.motionMatchFrameCount = g_renderer ? g_renderer->getMotionMatchFrameCount() : 0;
    args.motionMatchDbValid = (args.motionMatchFrameCount > 0) ? 1 : 0;
    args.motionMatchSearchInterval = g_renderer ? g_renderer->getMotionMatchSearchInterval() : 0.15f;
    args.motionMatchBlendDuration = g_renderer ? g_renderer->getMotionMatchBlendDuration() : 0.12f;
    float mmvx = 0.0f, mmvz = 0.0f, mmfx = 0.0f, mmfz = 1.0f;
    if (g_renderer)
    {
        g_renderer->getMotionMatchTargetVelocity(mmvx, mmvz);
        g_renderer->getMotionMatchTargetFacing(mmfx, mmfz);
    }
    args.motionMatchTargetVelX = mmvx;
    args.motionMatchTargetVelZ = mmvz;
    args.motionMatchTargetFacingX = mmfx;
    args.motionMatchTargetFacingZ = mmfz;
    args.motionMatchLastScore = g_renderer ? g_renderer->getMotionMatchLastScore() : 0.0f;
    args.motionMatchCurrentClipKey = g_renderer ? g_renderer->getMotionMatchCurrentClipKey() : NULL;
    args.materialLines= s_materialLines.empty() ? NULL : &s_materialLines[0];
    args.materialCount= (int)s_materialLines.size();
    args.warningText  = (s_warningText[0] != '\0') ? s_warningText : NULL;
    args.effectNames  = s_effectNames.empty() ? NULL : &s_effectNames[0];
    args.effectCount  = (int)s_effectNames.size();
    args.selectedEffect = g_browserIndexEffect;

    args.skyboxNames = s_skyboxNames.empty() ? NULL : &s_skyboxNames[0];
    args.skyboxCount = (int)s_skyboxNames.size();
    args.activeSkyboxIndex = g_renderer ? g_renderer->getActiveSkyboxIndex() : -1;
    args.skyboxEnabled = g_renderer ? (g_renderer->getSkyboxEnabled() ? 1 : 0) : 0;
    args.skyRenderMode = g_renderer ? (int)g_renderer->getSkyRenderMode() : 0;
    args.cloudEnabled = g_renderer ? (g_renderer->getCloudLayerEnabled() ? 1 : 0) : 0;
    args.cloudSkyboxIndex = g_renderer ? g_renderer->getCloudSkyboxIndex() : -1;

    args.boneNames    = s_boneNames.empty() ? NULL : &s_boneNames[0];
    args.boneParents  = s_boneParents.empty() ? NULL : &s_boneParents[0];
    args.boneCount    = (int)s_boneNames.size();
    args.selectedBone = g_renderer ? g_renderer->getSelectedBoneIndex() : -1;
    args.editMode     = g_editMode ? 1 : 0;
    args.pendingEdit  = g_renderer ? (g_renderer->hasSelectedBonePendingEdit() ? 1 : 0) : 0;
    args.recording    = g_renderer ? (g_renderer->isEditorRecording() ? 1 : 0) : 0;
    args.interpMode   = g_renderer ? (int)g_renderer->getEditorInterpolationMode() : 0;

    // Easing defaults + active keyframe easing (selected bone at/behind playhead).
    args.activeRotKeyIndex = -1;
    args.activeTransKeyIndex = -1;
    args.activeScaleKeyIndex = -1;
    if (g_renderer)
    {
        g_renderer->initializeEditorKeyTimes();
        args.editorEasingType = g_renderer->getEditorDefaultEasingType();
        float dcp1x = 0.0f, dcp1y = 0.0f, dcp2x = 1.0f, dcp2y = 1.0f;
        g_renderer->getEditorDefaultEasingCP(dcp1x, dcp1y, dcp2x, dcp2y);
        args.editorEasingCp1x = dcp1x;
        args.editorEasingCp1y = dcp1y;
        args.editorEasingCp2x = dcp2x;
        args.editorEasingCp2y = dcp2y;

        int boneIdx = g_renderer->getSelectedBoneIndex();
        if (boneIdx >= 0)
        {
            int rotKeyIdx = g_renderer->findEditorRotKeyAtOrBeforeTime(boneIdx, args.animTime);
            if (rotKeyIdx >= 0)
            {
                args.activeRotKeyIndex = rotKeyIdx;
                args.activeRotKeyTimeMs = g_renderer->getEditorRotKeyTime(boneIdx, rotKeyIdx);
                int et = 0;
                float cp1x = 0.0f, cp1y = 0.0f, cp2x = 1.0f, cp2y = 1.0f;
                if (g_renderer->getEditorRotKeyEasing(boneIdx, rotKeyIdx, et, cp1x, cp1y, cp2x, cp2y))
                {
                    args.activeRotKeyEasingType = et;
                    args.activeRotKeyCp1x = cp1x;
                    args.activeRotKeyCp1y = cp1y;
                    args.activeRotKeyCp2x = cp2x;
                    args.activeRotKeyCp2y = cp2y;
                }
            }

            int transKeyIdx = g_renderer->findEditorTransKeyAtOrBeforeTime(boneIdx, args.animTime);
            if (transKeyIdx >= 0)
            {
                args.activeTransKeyIndex = transKeyIdx;
                args.activeTransKeyTimeMs = g_renderer->getEditorTransKeyTime(boneIdx, transKeyIdx);
                int et = 0;
                float cp1x = 0.0f, cp1y = 0.0f, cp2x = 1.0f, cp2y = 1.0f;
                if (g_renderer->getEditorTransKeyEasing(boneIdx, transKeyIdx, et, cp1x, cp1y, cp2x, cp2y))
                {
                    args.activeTransKeyEasingType = et;
                    args.activeTransKeyCp1x = cp1x;
                    args.activeTransKeyCp1y = cp1y;
                    args.activeTransKeyCp2x = cp2x;
                    args.activeTransKeyCp2y = cp2y;
                }

                args.activeTransKeyInterpMode = g_renderer->getEditorTransKeyInterpolationMode(boneIdx, transKeyIdx);
                float inX = 0.0f, outX = 0.0f;
                float inY = 0.0f, outY = 0.0f;
                float inZ = 0.0f, outZ = 0.0f;
                if (g_renderer->getEditorTransKeyTangents(boneIdx, transKeyIdx, inX, outX, inY, outY, inZ, outZ))
                {
                    args.activeTransKeyInTanX = inX;
                    args.activeTransKeyOutTanX = outX;
                    args.activeTransKeyInTanY = inY;
                    args.activeTransKeyOutTanY = outY;
                    args.activeTransKeyInTanZ = inZ;
                    args.activeTransKeyOutTanZ = outZ;
                }
            }

            int scaleKeyIdx = g_renderer->findEditorScaleKeyAtOrBeforeTime(boneIdx, args.animTime);
            if (scaleKeyIdx >= 0)
            {
                args.activeScaleKeyIndex = scaleKeyIdx;
                args.activeScaleKeyTimeMs = g_renderer->getEditorScaleKeyTime(boneIdx, scaleKeyIdx);
                int et = 0;
                float cp1x = 0.0f, cp1y = 0.0f, cp2x = 1.0f, cp2y = 1.0f;
                if (g_renderer->getEditorScaleKeyEasing(boneIdx, scaleKeyIdx, et, cp1x, cp1y, cp2x, cp2y))
                {
                    args.activeScaleKeyEasingType = et;
                    args.activeScaleKeyCp1x = cp1x;
                    args.activeScaleKeyCp1y = cp1y;
                    args.activeScaleKeyCp2x = cp2x;
                    args.activeScaleKeyCp2y = cp2y;
                }

                args.activeScaleKeyInterpMode = g_renderer->getEditorScaleKeyInterpolationMode(boneIdx, scaleKeyIdx);
                float inX = 0.0f, outX = 0.0f;
                float inY = 0.0f, outY = 0.0f;
                float inZ = 0.0f, outZ = 0.0f;
                if (g_renderer->getEditorScaleKeyTangents(boneIdx, scaleKeyIdx, inX, outX, inY, outY, inZ, outZ))
                {
                    args.activeScaleKeyInTanX = inX;
                    args.activeScaleKeyOutTanX = outX;
                    args.activeScaleKeyInTanY = inY;
                    args.activeScaleKeyOutTanY = outY;
                    args.activeScaleKeyInTanZ = inZ;
                    args.activeScaleKeyOutTanZ = outZ;
                }
            }
        }
    }
    else
    {
        args.editorEasingType = 0;
        args.editorEasingCp1x = 0.0f;
        args.editorEasingCp1y = 0.0f;
        args.editorEasingCp2x = 1.0f;
        args.editorEasingCp2y = 1.0f;
    }
    args.gizmoMode    = g_renderer ? (int)g_renderer->getGizmoMode() : 0;
    args.gizmoSpace   = g_renderer ? (int)g_renderer->getGizmoSpace() : 0;
    args.rotSnapEnabled = g_renderer ? (g_renderer->getRotateSnapEnabled() ? 1 : 0) : 0;
    args.rotSnapDegrees = g_renderer ? g_renderer->getRotateSnapDegrees() : 0.0f;
    args.moveSnapEnabled = g_renderer ? (g_renderer->getMoveSnapEnabled() ? 1 : 0) : 0;
    args.moveSnapUnits   = g_renderer ? g_renderer->getMoveSnapUnits() : 0.0f;
    args.editorTimelineDuration = g_renderer ? g_renderer->getEditorTimelineDuration() : 0.0f;
    args.exportName = g_exportName;
    args.eventCount = (int)s_eventTimes.size();
    args.eventTimes = s_eventTimes.empty() ? NULL : &s_eventTimes[0];
    args.eventCategories = s_eventCats.empty() ? NULL : &s_eventCats[0];
    args.eventSummaries = s_eventSummaries.empty() ? NULL : &s_eventSummaries[0];

    if (g_renderer)
    {
        IDirect3DTexture9* vpTex = g_renderer->getImGuiViewportTexture();
        int texW = 0;
        int texH = 0;
        g_renderer->getImGuiViewportTextureSize(texW, texH);
        args.viewportTexture = (void*)vpTex;
        args.viewportTextureWidth = texW;
        args.viewportTextureHeight = texH;
    }

    if (g_renderer)
    {
        float tx = 0.0f, ty = 0.0f, tz = 0.0f;
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        float sx = 1.0f, sy = 1.0f, sz = 1.0f;
        if (g_renderer->getSelectedBoneLocalTRSScale(tx, ty, tz, rx, ry, rz, sx, sy, sz))
        {
            args.hasBoneTRS = 1;
            args.boneTx = tx; args.boneTy = ty; args.boneTz = tz;
            args.boneRxDeg = rx; args.boneRyDeg = ry; args.boneRzDeg = rz;
            args.boneSx = sx; args.boneSy = sy; args.boneSz = sz;
        }
    }

    // ---- Level browser data ----
    args.pakModelCrcs = NULL; args.pakModelBoneCounts = NULL; args.pakModelMatCounts = NULL; args.pakModelGamemodeMasks = NULL;
    args.pakTextureDepths = NULL; args.pakTextureLevels = NULL; args.pakTextureFormats = NULL; args.pakTextureGamemodeMasks = NULL;
    args.pakAnimCrcs = NULL; args.pakAnimSizes = NULL; args.pakAnimSizesComp = NULL; args.pakAnimGamemodeMasks = NULL;
    args.pakEffectSizes = NULL; args.pakEffectGamemodeMasks = NULL; args.pakGfxSizes = NULL;
    args.binRawCount = 0; args.binRawNames = NULL; args.binRawKinds = NULL;
    args.binRawSizes = NULL; args.binRawSizesComp = NULL;
    args.pakSubBlock1Count = 0; args.pakSubBlock1Names = NULL;
    args.pakSubBlock1Types = NULL; args.pakSubBlock1Sizes = NULL;
    args.pakSubBlock2Count = 0; args.pakSubBlock2Names = NULL;
    args.pakSubBlock2Types = NULL; args.pakSubBlock2Sizes = NULL;
    args.pakObjACount = 0; args.pakObjANames = NULL; args.pakObjASizes = NULL; args.pakObjASizesComp = NULL;
    args.pakObj0Count = 0; args.pakObj0Names = NULL; args.pakObj0TypeCodes = NULL;
    args.pakAnimBlockCount = 0; args.pakAnimBlockNames = NULL; args.pakAnimBlockKeyNames = NULL;
    args.pakAnimBlockSizes = NULL; args.pakAnimBlockSizesComp = NULL;
    args.pakValCount = 0; args.pakValNames = NULL; args.pakValGamemodeMasks = NULL;
    args.pakBufferInfoCount = 0; args.pakBufferInfoNames = NULL; args.pakBufferInfoExtra1 = NULL; args.pakBufferInfoExtra2 = NULL;
    args.pakMat1Count = 0; args.pakMat1Names = NULL; args.pakMat1Tex0 = NULL; args.pakMat1Tex1 = NULL;
    args.pakMat2Count = 0; args.pakMat2Names = NULL; args.pakMat2Tex0 = NULL; args.pakMat2Tex1 = NULL;
    args.pakMat3Count = 0; args.pakMat3Names = NULL; args.pakMat3Tex0 = NULL; args.pakMat3Tex1 = NULL;
    args.pakMat4Count = 0; args.pakMat4Names = NULL; args.pakMat4Tex0 = NULL; args.pakMat4Tex1 = NULL;
    args.pakMatExtraCount = 0; args.pakMatExtraNames = NULL;
    args.pakShapeInfoCount = 0; args.pakShapeInfoNames = NULL; args.pakShapeInfoOffsets = NULL; args.pakShapeInfoKinds = NULL;
    args.pakHkShapeCount = 0; args.pakHkShapeNames = NULL; args.pakHkShapeKinds = NULL; args.pakHkShapeANums = NULL;
    args.pakHkConstraintDataCount = 0; args.pakHkConstraintDataNames = NULL; args.pakHkConstraintDataKinds = NULL;
    args.pakVBuffInfoCount = 0; args.pakVBuffInfoNames = NULL; args.pakVBuffInfoSizes = NULL; args.pakVBuffInfoOffsets = NULL;
    args.pakIBuffInfoCount = 0; args.pakIBuffInfoNames = NULL; args.pakIBuffInfoSizes = NULL; args.pakIBuffInfoFormats = NULL;
    args.pakHkConstraintInfoCount = 0; args.pakHkConstraintInfoNames = NULL; args.pakHkConstraintInfoKinds = NULL; args.pakHkConstraintInfoBoneParents = NULL;
    args.pakPFieldCount = 0; args.pakPFieldNames = NULL; args.pakPFieldGamemodeMasks = NULL; args.pakPFieldWidths = NULL;
    args.pakRadiosityCount = 0; args.pakRadiosityNames = NULL; args.pakRadiosityNums = NULL; args.pakRadiosityOffsets = NULL;
    args.pakFoliageCount = 0; args.pakFoliageNames = NULL; args.pakFoliageKinds = NULL;
    args.gameObjCount = 0; args.gameObjNames = NULL; args.gameObjTypeNames = NULL;
    args.gameObjGuids = NULL; args.gameObjParentGuids = NULL; args.gameObjGamemodeMasks = NULL;
    args.gameObjLayerGuids = NULL; args.gameObjMeshNames = NULL;
    args.gameObjEditorShapes = NULL; args.gameObjEditorColors = NULL; args.gameObjEditorTextures = NULL;
    args.gameObjEditorOuters = NULL; args.gameObjEditorSizeX = NULL; args.gameObjEditorSizeY = NULL; args.gameObjEditorSizeZ = NULL;
    args.sceneLoaded = 0; args.sceneInstanceCount = 0; args.sceneModelCount = 0;
    args.requestRenderScene = 0;
    args.gameObjPosX = NULL; args.gameObjPosY = NULL; args.gameObjPosZ = NULL;
    args.gameObjTypeDefCount = 0; args.gameObjTypeDefNames = NULL;
    args.gameObjTypeDefFieldCounts = NULL; args.gameObjTypeDefInstanceCounts = NULL;
    args.stringsLoaded = g_levelReader.GetCrcMapSize() > 0 ? 1 : 0;
    args.stringsCount  = (int)g_levelReader.GetCrcMapSize();
    args.requestLoadStrings = 0;
    args.requestedStringsPath[0] = '\0';
    args.levelLoaded       = g_levelReader.IsLoaded() ? 1 : 0;
    args.levelName         = g_levelReader.IsLoaded() ? g_levelReader.GetLevelName().c_str() : NULL;
    args.levelLoadError    = g_levelLoadError.empty() ? NULL : g_levelLoadError.c_str();
    args.pakModelCount     = (int)g_pakModelNamePtrs.size();
    args.pakModelNames     = g_pakModelNamePtrs.empty() ? NULL : &g_pakModelNamePtrs[0];
    args.pakTextureCount   = (int)g_pakTextureNamePtrs.size();
    args.pakTextureNames   = g_pakTextureNamePtrs.empty() ? NULL : &g_pakTextureNamePtrs[0];
    args.pakTextureWidths  = g_pakTextureWidths.empty()   ? NULL : &g_pakTextureWidths[0];
    args.pakTextureHeights = g_pakTextureHeights.empty()  ? NULL : &g_pakTextureHeights[0];
    args.pakAnimCount      = (int)g_pakAnimNamePtrs.size();
    args.pakAnimNames      = g_pakAnimNamePtrs.empty() ? NULL : &g_pakAnimNamePtrs[0];
    args.pakPakEffectCount = (int)g_pakEffectNamePtrs.size();
    args.pakPakEffectNames = g_pakEffectNamePtrs.empty() ? NULL : &g_pakEffectNamePtrs[0];
    args.pakGfxBlockCount  = (int)g_pakGfxBlockNamePtrs.size();
    args.pakGfxBlockNames  = g_pakGfxBlockNamePtrs.empty() ? NULL : &g_pakGfxBlockNamePtrs[0];
    args.pakModelCrcs           = g_pakModelCrcs.empty()          ? NULL : &g_pakModelCrcs[0];
    args.pakModelBoneCounts     = g_pakModelBoneCounts.empty()    ? NULL : &g_pakModelBoneCounts[0];
    args.pakModelMatCounts      = g_pakModelMatCounts.empty()     ? NULL : &g_pakModelMatCounts[0];
    args.pakModelGamemodeMasks  = g_pakModelGamemodeMasks.empty() ? NULL : &g_pakModelGamemodeMasks[0];
    args.pakTextureDepths       = g_pakTextureDepths.empty()      ? NULL : &g_pakTextureDepths[0];
    args.pakTextureLevels       = g_pakTextureLevels.empty()      ? NULL : &g_pakTextureLevels[0];
    args.pakTextureFormats      = g_pakTextureFormats.empty()     ? NULL : &g_pakTextureFormats[0];
    args.pakTextureGamemodeMasks= g_pakTextureGamemodeMasks.empty()? NULL : &g_pakTextureGamemodeMasks[0];
    args.pakAnimCrcs            = g_pakAnimCrcs.empty()           ? NULL : &g_pakAnimCrcs[0];
    args.pakAnimSizes           = g_pakAnimSizes.empty()          ? NULL : &g_pakAnimSizes[0];
    args.pakAnimSizesComp       = g_pakAnimSizesComp.empty()      ? NULL : &g_pakAnimSizesComp[0];
    args.pakAnimGamemodeMasks   = g_pakAnimGamemodeMasks.empty()  ? NULL : &g_pakAnimGamemodeMasks[0];
    args.pakEffectSizes         = g_pakEffectSizes.empty()        ? NULL : &g_pakEffectSizes[0];
    args.pakEffectGamemodeMasks = g_pakEffectGamemodeMasks.empty() ? NULL : &g_pakEffectGamemodeMasks[0];
    args.pakGfxSizes            = g_pakGfxSizes.empty()           ? NULL : &g_pakGfxSizes[0];
    args.binRawCount         = (int)g_binRawNamePtrs.size();
    args.binRawNames         = g_binRawNamePtrs.empty()    ? NULL : &g_binRawNamePtrs[0];
    args.binRawKinds         = g_binRawKinds.empty()       ? NULL : &g_binRawKinds[0];
    args.binRawSizes         = g_binRawSizes.empty()       ? NULL : &g_binRawSizes[0];
    args.binRawSizesComp     = g_binRawSizesComp.empty()   ? NULL : &g_binRawSizesComp[0];
    args.pakSubBlock1Count   = (int)g_subBlock1NamePtrs.size();
    args.pakSubBlock1Names   = g_subBlock1NamePtrs.empty() ? NULL : &g_subBlock1NamePtrs[0];
    args.pakSubBlock1Types   = g_subBlock1TypePtrs.empty() ? NULL : &g_subBlock1TypePtrs[0];
    args.pakSubBlock1Sizes   = g_subBlock1Sizes.empty()    ? NULL : &g_subBlock1Sizes[0];
    args.pakSubBlock2Count   = (int)g_subBlock2NamePtrs.size();
    args.pakSubBlock2Names   = g_subBlock2NamePtrs.empty() ? NULL : &g_subBlock2NamePtrs[0];
    args.pakSubBlock2Types   = g_subBlock2TypePtrs.empty() ? NULL : &g_subBlock2TypePtrs[0];
    args.pakSubBlock2Sizes   = g_subBlock2Sizes.empty()    ? NULL : &g_subBlock2Sizes[0];
    args.pakObjACount        = (int)g_objANamePtrs.size();
    args.pakObjANames        = g_objANamePtrs.empty()      ? NULL : &g_objANamePtrs[0];
    args.pakObjASizes        = g_objASizes.empty()         ? NULL : &g_objASizes[0];
    args.pakObjASizesComp    = g_objASizesComp.empty()     ? NULL : &g_objASizesComp[0];
    args.pakObj0Count        = (int)g_obj0NamePtrs.size();
    args.pakObj0Names        = g_obj0NamePtrs.empty()      ? NULL : &g_obj0NamePtrs[0];
    args.pakObj0TypeCodes    = g_obj0TypeCodes.empty()     ? NULL : &g_obj0TypeCodes[0];
    args.pakAnimBlockCount   = (int)g_animBlockNamePtrs.size();
    args.pakAnimBlockNames   = g_animBlockNamePtrs.empty()    ? NULL : &g_animBlockNamePtrs[0];
    args.pakAnimBlockKeyNames= g_animBlockKeyNamePtrs.empty() ? NULL : &g_animBlockKeyNamePtrs[0];
    args.pakAnimBlockSizes      = g_animBlockSizes.empty()     ? NULL : &g_animBlockSizes[0];
    args.pakAnimBlockSizesComp  = g_animBlockSizesComp.empty() ? NULL : &g_animBlockSizesComp[0];
    args.pakValCount         = (int)g_pakValNamePtrs.size();
    args.pakValNames         = g_pakValNamePtrs.empty()    ? NULL : &g_pakValNamePtrs[0];
    args.pakValGamemodeMasks = g_pakValGamemodeMasks.empty()? NULL : &g_pakValGamemodeMasks[0];
    args.pakBufferInfoCount  = (int)g_bufInfoNamePtrs.size();
    args.pakBufferInfoNames  = g_bufInfoNamePtrs.empty()   ? NULL : &g_bufInfoNamePtrs[0];
    args.pakBufferInfoExtra1 = g_bufInfoExtra1.empty()     ? NULL : &g_bufInfoExtra1[0];
    args.pakBufferInfoExtra2 = g_bufInfoExtra2.empty()     ? NULL : &g_bufInfoExtra2[0];
    args.pakMat1Count        = (int)g_mat1NamePtrs.size();
    args.pakMat1Names        = g_mat1NamePtrs.empty()      ? NULL : &g_mat1NamePtrs[0];
    args.pakMat1Tex0         = g_mat1Tex0.empty()          ? NULL : &g_mat1Tex0[0];
    args.pakMat1Tex1         = g_mat1Tex1.empty()          ? NULL : &g_mat1Tex1[0];
    args.pakMat2Count        = (int)g_mat2NamePtrs.size();
    args.pakMat2Names        = g_mat2NamePtrs.empty()      ? NULL : &g_mat2NamePtrs[0];
    args.pakMat2Tex0         = g_mat2Tex0.empty()          ? NULL : &g_mat2Tex0[0];
    args.pakMat2Tex1         = g_mat2Tex1.empty()          ? NULL : &g_mat2Tex1[0];
    args.pakMat3Count        = (int)g_mat3NamePtrs.size();
    args.pakMat3Names        = g_mat3NamePtrs.empty()      ? NULL : &g_mat3NamePtrs[0];
    args.pakMat3Tex0         = g_mat3Tex0.empty()          ? NULL : &g_mat3Tex0[0];
    args.pakMat3Tex1         = g_mat3Tex1.empty()          ? NULL : &g_mat3Tex1[0];
    args.pakMat4Count        = (int)g_mat4NamePtrs.size();
    args.pakMat4Names        = g_mat4NamePtrs.empty()      ? NULL : &g_mat4NamePtrs[0];
    args.pakMat4Tex0         = g_mat4Tex0.empty()          ? NULL : &g_mat4Tex0[0];
    args.pakMat4Tex1         = g_mat4Tex1.empty()          ? NULL : &g_mat4Tex1[0];
    args.pakMatExtraCount    = (int)g_matExtraNamePtrs.size();
    args.pakMatExtraNames    = g_matExtraNamePtrs.empty()  ? NULL : &g_matExtraNamePtrs[0];
    args.pakShapeInfoCount   = (int)g_shapeInfoNamePtrs.size();
    args.pakShapeInfoNames   = g_shapeInfoNamePtrs.empty() ? NULL : &g_shapeInfoNamePtrs[0];
    args.pakShapeInfoOffsets = g_shapeInfoOffsets.empty()  ? NULL : &g_shapeInfoOffsets[0];
    args.pakShapeInfoKinds   = g_shapeInfoKinds.empty()    ? NULL : &g_shapeInfoKinds[0];
    args.pakHkShapeCount     = (int)g_hkShapeNamePtrs.size();
    args.pakHkShapeNames     = g_hkShapeNamePtrs.empty()   ? NULL : &g_hkShapeNamePtrs[0];
    args.pakHkShapeKinds     = g_hkShapeKinds.empty()      ? NULL : &g_hkShapeKinds[0];
    args.pakHkShapeANums     = g_hkShapeANums.empty()      ? NULL : &g_hkShapeANums[0];
    args.pakHkConstraintDataCount = (int)g_hkConstraintDataNamePtrs.size();
    args.pakHkConstraintDataNames = g_hkConstraintDataNamePtrs.empty() ? NULL : &g_hkConstraintDataNamePtrs[0];
    args.pakHkConstraintDataKinds = g_hkConstraintDataKinds.empty()    ? NULL : &g_hkConstraintDataKinds[0];
    args.pakVBuffInfoCount   = (int)g_vbuffInfoNamePtrs.size();
    args.pakVBuffInfoNames   = g_vbuffInfoNamePtrs.empty() ? NULL : &g_vbuffInfoNamePtrs[0];
    args.pakVBuffInfoSizes   = g_vbuffInfoSizes.empty()    ? NULL : &g_vbuffInfoSizes[0];
    args.pakVBuffInfoOffsets = g_vbuffInfoOffsets.empty()  ? NULL : &g_vbuffInfoOffsets[0];
    args.pakIBuffInfoCount   = (int)g_ibuffInfoNamePtrs.size();
    args.pakIBuffInfoNames   = g_ibuffInfoNamePtrs.empty() ? NULL : &g_ibuffInfoNamePtrs[0];
    args.pakIBuffInfoSizes   = g_ibuffInfoSizes.empty()    ? NULL : &g_ibuffInfoSizes[0];
    args.pakIBuffInfoFormats = g_ibuffInfoFormats.empty()  ? NULL : &g_ibuffInfoFormats[0];
    args.pakHkConstraintInfoCount      = (int)g_hkConstraintInfoNamePtrs.size();
    args.pakHkConstraintInfoNames      = g_hkConstraintInfoNamePtrs.empty()   ? NULL : &g_hkConstraintInfoNamePtrs[0];
    args.pakHkConstraintInfoKinds      = g_hkConstraintInfoKinds.empty()      ? NULL : &g_hkConstraintInfoKinds[0];
    args.pakHkConstraintInfoBoneParents= g_hkConstraintInfoBoneParents.empty()? NULL : &g_hkConstraintInfoBoneParents[0];
    args.pakPFieldCount          = (int)g_pFieldNamePtrs.size();
    args.pakPFieldNames          = g_pFieldNamePtrs.empty()       ? NULL : &g_pFieldNamePtrs[0];
    args.pakPFieldGamemodeMasks  = g_pFieldGamemodeMasks.empty()  ? NULL : &g_pFieldGamemodeMasks[0];
    args.pakPFieldWidths         = g_pFieldWidths.empty()         ? NULL : &g_pFieldWidths[0];
    args.pakRadiosityCount       = (int)g_radiosityNamePtrs.size();
    args.pakRadiosityNames       = g_radiosityNamePtrs.empty()    ? NULL : &g_radiosityNamePtrs[0];
    args.pakRadiosityNums        = g_radiosityNums.empty()        ? NULL : &g_radiosityNums[0];
    args.pakRadiosityOffsets     = g_radiosityOffsets.empty()     ? NULL : &g_radiosityOffsets[0];
    args.pakFoliageCount         = (int)g_foliageNamePtrs.size();
    args.pakFoliageNames         = g_foliageNamePtrs.empty()      ? NULL : &g_foliageNamePtrs[0];
    args.pakFoliageKinds         = g_foliageKinds.empty()         ? NULL : &g_foliageKinds[0];
    args.gameObjCount            = (int)g_goNamePtrs.size();
    args.gameObjNames            = g_goNamePtrs.empty()           ? NULL : &g_goNamePtrs[0];
    args.gameObjTypeNames        = g_goTypeNamePtrs.empty()       ? NULL : &g_goTypeNamePtrs[0];
    args.gameObjGuids            = g_goGuids.empty()              ? NULL : &g_goGuids[0];
    args.gameObjParentGuids      = g_goParentGuids.empty()        ? NULL : &g_goParentGuids[0];
    args.gameObjGamemodeMasks    = g_goGamemodeMasks.empty()      ? NULL : &g_goGamemodeMasks[0];
    args.gameObjLayerGuids       = g_goLayerGuids.empty()         ? NULL : &g_goLayerGuids[0];
    args.gameObjMeshNames        = g_goMeshNamePtrs.empty()       ? NULL : &g_goMeshNamePtrs[0];
    args.gameObjPosX             = g_goPosX.empty()               ? NULL : &g_goPosX[0];
    args.gameObjPosY             = g_goPosY.empty()               ? NULL : &g_goPosY[0];
    args.gameObjPosZ             = g_goPosZ.empty()               ? NULL : &g_goPosZ[0];
    args.gameObjTypeDefCount     = (int)g_goTypeDefNamePtrs.size();
    args.gameObjTypeDefNames     = g_goTypeDefNamePtrs.empty()    ? NULL : &g_goTypeDefNamePtrs[0];
    args.gameObjTypeDefFieldCounts    = g_goTypeDefFieldCounts.empty()    ? NULL : &g_goTypeDefFieldCounts[0];
    args.gameObjTypeDefInstanceCounts = g_goTypeDefInstanceCounts.empty() ? NULL : &g_goTypeDefInstanceCounts[0];

    // Event system & relationship fields
    args.gameObjTargetGuids    = g_goTargetGuids.empty()       ? NULL : &g_goTargetGuids[0];
    args.gameObjOutputEvents   = g_goOutputEventPtrs.empty()   ? NULL : &g_goOutputEventPtrs[0];
    args.gameObjInputEvents    = g_goInputEventPtrs.empty()    ? NULL : &g_goInputEventPtrs[0];
    args.gameObjDelays         = g_goDelays.empty()            ? NULL : &g_goDelays[0];
    args.gameObjSticky         = g_goSticky.empty()            ? NULL : &g_goSticky[0];
    args.gameObjModeNames      = g_goModeNamePtrs.empty()      ? NULL : &g_goModeNamePtrs[0];
    // Packed arrays
    args.gameObjOutputsTotal   = (int)g_goOutputsData.size();
    args.gameObjOutputsData    = g_goOutputsData.empty()       ? NULL : &g_goOutputsData[0];
    args.gameObjOutputsOffsets = g_goOutputsOffsets.empty()    ? NULL : &g_goOutputsOffsets[0];
    args.gameObjOutputsCounts  = g_goOutputsCounts.empty()     ? NULL : &g_goOutputsCounts[0];
    args.gameObjLayersArrTotal = (int)g_goLayersArrData.size();
    args.gameObjLayersArrData  = g_goLayersArrData.empty()     ? NULL : &g_goLayersArrData[0];
    args.gameObjLayersArrOffsets= g_goLayersArrOffsets.empty()  ? NULL : &g_goLayersArrOffsets[0];
    args.gameObjLayersArrCounts= g_goLayersArrCounts.empty()   ? NULL : &g_goLayersArrCounts[0];
    args.gameObjNodesTotal     = (int)g_goNodesData.size();
    args.gameObjNodesData      = g_goNodesData.empty()         ? NULL : &g_goNodesData[0];
    args.gameObjNodesOffsets   = g_goNodesOffsets.empty()      ? NULL : &g_goNodesOffsets[0];
    args.gameObjNodesCounts    = g_goNodesCounts.empty()       ? NULL : &g_goNodesCounts[0];
    args.gameObjNode1Guids     = g_goNode1Guids.empty()        ? NULL : &g_goNode1Guids[0];
    args.gameObjNode2Guids     = g_goNode2Guids.empty()        ? NULL : &g_goNode2Guids[0];
    args.allGuidRefTotalPairs  = (int)(g_goAllGuidRefData.size() / 2);
    args.allGuidRefData        = g_goAllGuidRefData.empty()    ? NULL : &g_goAllGuidRefData[0];
    args.allGuidRefCounts      = g_goAllGuidRefCounts.empty()  ? NULL : &g_goAllGuidRefCounts[0];
    args.allGuidRefOffsets     = g_goAllGuidRefOffsets.empty()  ? NULL : &g_goAllGuidRefOffsets[0];
    args.gameObjEditorShapes   = g_goEditorShapePtrs.empty()   ? NULL : &g_goEditorShapePtrs[0];
    args.gameObjEditorColors   = g_goEditorColors.empty()      ? NULL : &g_goEditorColors[0];
    args.gameObjEditorTextures = g_goEditorTexturePtrs.empty() ? NULL : &g_goEditorTexturePtrs[0];
    args.gameObjEditorOuters   = g_goEditorOuters.empty()      ? NULL : &g_goEditorOuters[0];
    args.gameObjEditorSizeX    = g_goEditorSizeX.empty()       ? NULL : &g_goEditorSizeX[0];
    args.gameObjEditorSizeY    = g_goEditorSizeY.empty()       ? NULL : &g_goEditorSizeY[0];
    args.gameObjEditorSizeZ    = g_goEditorSizeZ.empty()       ? NULL : &g_goEditorSizeZ[0];

    args.viewportEnabled       = (g_renderer && g_renderer->getScene3dEnabled()) ? 1 : 0;
    args.requestToggleViewport = 0; // DLL will set to 1 if user clicks toggle
    args.requestLoadLevel  = 0; // DLL will set to 1 if user clicked Load
    args.requestLoadEntityMesh = 0;
    memset(args.requestedEntityMeshName, 0, sizeof(args.requestedEntityMeshName));
    args.requestedEntityPosX = 0; args.requestedEntityPosY = 0; args.requestedEntityPosZ = 0;
    // Level scene stats
    args.sceneLoaded        = (g_renderer && g_renderer->hasLevelScene()) ? 1 : 0;
    args.sceneInstanceCount = g_renderer ? g_renderer->levelSceneInstanceCount() : 0;
    args.sceneModelCount    = g_renderer ? g_renderer->levelSceneModelCount()    : 0;
    args.requestRenderScene = 0;

    // Level Inspector — fill selected instance details for ImGui DLL
    args.inspectorHoveredIdx  = -1;
    args.inspectorSelectedIdx = -1;
    args.inspectorSelName     = NULL;
    args.inspectorSelType     = NULL;
    args.inspectorSelMesh     = NULL;
    args.inspectorSelMatrix   = NULL;
    args.inspectorSelGuid     = 0;
    args.inspectorSelParentGuid = 0;
    args.inspectorSelGameModeMask = -1;
    args.inspectorSelPartCount = 0;
    args.inspectorPosChanged = 0;
    args.inspectorNewPos[0] = args.inspectorNewPos[1] = args.inspectorNewPos[2] = 0;
    args.inspectorResetRequested = 0;
    args.inspectorMatrixChanged = 0;
    memset(args.inspectorNewMatrix, 0, sizeof(args.inspectorNewMatrix));
    args.requestSavePak = 0;

    // Pending field-edit queue summary (Chain editor / staging UX).
    args.pendingFieldEditCount = (int)g_levelReader.GetFieldEdits().size();
    args.pendingFieldEditsCancelRequested = 0;
    args.pendingEntityCount = g_levelReader.GetPendingCount();
    args.pendingDeletedGuidCount = (int)g_levelReader.GetDeletedGuids().size();
    args.pendingCollisionCount = g_levelReader.GetPendingCollisionCount();
    // Build parallel arrays for per-edit revert UI. Statics so pointers stay valid.
    {
        static std::vector<unsigned int> s_peGuids;
        static std::vector<const char*>  s_peFieldPtrs;
        static std::vector<int>          s_peKinds;
        static std::vector<std::string>  s_peFieldStore;
        s_peGuids.clear();
        s_peFieldPtrs.clear();
        s_peKinds.clear();
        s_peFieldStore.clear();
        const std::vector<ZeroEngine::FieldEdit>& fes = g_levelReader.GetFieldEdits();
        s_peFieldStore.reserve(fes.size());
        for (size_t fei = 0; fei < fes.size(); ++fei) {
            s_peGuids.push_back(fes[fei].entityGuid);
            s_peFieldStore.push_back(fes[fei].fieldName);
            s_peKinds.push_back(fes[fei].kind);
        }
        for (size_t fei = 0; fei < s_peFieldStore.size(); ++fei)
            s_peFieldPtrs.push_back(s_peFieldStore[fei].c_str());
        args.pendingFieldEditEntityGuids = s_peGuids.empty() ? NULL : &s_peGuids[0];
        args.pendingFieldEditFieldNames  = s_peFieldPtrs.empty() ? NULL : &s_peFieldPtrs[0];
        args.pendingFieldEditKinds       = s_peKinds.empty()  ? NULL : &s_peKinds[0];
        args.pendingFieldEditRevertIndex = -1;
    }
    // Chain-editor: Add-connection result is STICKY across frames so the DLL
    // can show status. DLL clears it by setting chainAddResult = 0. Host resets
    // request flag only.
    args.chainDeleteConnectionRequested = 0;
    args.chainAddConnectionRequested = 0;
    // Outputs-list diagnostics for the currently-selected entity. Helps users
    // see whether they can Add without first deleting (needs a zero slot).
    args.selEntOutputsTotal = 0;
    args.selEntOutputsUsed = 0;
    args.selEntOutputsZeroSlots = 0;
    // Uses args.propEntityGuid from the previous frame (updated below). One frame
    // of staleness is imperceptible for a diagnostic counter.
    uint32_t selGuidForDiag = (uint32_t)args.propEntityGuid;
    if (g_levelReader.IsLoaded() && selGuidForDiag != 0) {
        const std::vector<ZeroEngine::LevelGameObjEntry>& gosSel = g_levelReader.GetGameObjs();
        const ZeroEngine::LevelGameObjEntry* selE = NULL;
        for (size_t ei = 0; ei < gosSel.size(); ++ei) {
            if (gosSel[ei].guid == selGuidForDiag) { selE = &gosSel[ei]; break; }
        }
        if (selE && selE->type_def_index >= 0 &&
            selE->type_def_index < (int)g_levelReader.GetGameObjTypes().size())
        {
            const ZeroEngine::LevelGameObjTypeDef& tdS = g_levelReader.GetGameObjTypes()[selE->type_def_index];
            const uint32_t CRC_OUTPUTS_S = 0x250BE3C1u;
            uint32_t outputsFieldOffS = 0;
            bool foundFieldS = false;
            for (size_t fi = 0; fi < tdS.fields.size(); ++fi) {
                if (tdS.fields[fi].name_crc == CRC_OUTPUTS_S) {
                    outputsFieldOffS = tdS.fields[fi].offset;
                    foundFieldS = true;
                    break;
                }
            }
            if (foundFieldS && selE->block1_obj_offset != 0) {
                uint32_t absOffS = selE->block1_obj_offset + outputsFieldOffS;
                uint16_t listNumS = 0, listDataOffS = 0;
                g_levelReader.GetFieldValue(absOffS, &listNumS, 2);
                g_levelReader.GetFieldValue(absOffS + 2, &listDataOffS, 2);
                args.selEntOutputsTotal = (int)listNumS;
                for (uint16_t li = 0; li < listNumS; ++li) {
                    uint32_t slotOff = absOffS + 4 + (uint32_t)listDataOffS + (uint32_t)li * 4;
                    uint32_t slotVal = 0;
                    g_levelReader.GetFieldValue(slotOff, &slotVal, 4);
                    if (slotVal == 0) args.selEntOutputsZeroSlots++;
                    else              args.selEntOutputsUsed++;
                }
            }
        }
    }

    // Save PAK status for progress display
    if (g_savePakStatus == 5 || g_savePakStatus == -1) {
        g_savePakDoneTimer -= 0.016f; // ~60fps estimate
        if (g_savePakDoneTimer <= 0) g_savePakStatus = 0;
    }
    args.savePakStatus = g_savePakStatus;
    args.savePakMessage = g_savePakMessage.empty() ? NULL : g_savePakMessage.c_str();
    // After successful save, tell DLL the saved path so it can update its file field
    if (!g_lastSavedPakPath.empty()) {
        strncpy_s(args.lastSavedPakPath, g_lastSavedPakPath.c_str(), 511);
        g_lastSavedPakPath.clear(); // one-shot: only send once
    }

    // Entity Creation state
    if (g_levelInspector.hasRightClickHit()) {
        args.createMenuRequested = 1;
        const float* wp = g_levelInspector.rightClickWorldPos();
        args.createWorldPos[0] = wp[0];
        args.createWorldPos[1] = wp[1];
        args.createWorldPos[2] = wp[2];
        g_levelInspector.clearRightClickHit();
    } else {
        args.createMenuRequested = 0;
        args.createWorldPos[0] = args.createWorldPos[1] = args.createWorldPos[2] = 0;
    }
    args.createEntityRequested = 0;
    memset(args.createEntityName, 0, sizeof(args.createEntityName));
    // Asset highlight output (reset each frame — DLL sets it)
    args.assetHighlightType = 0;
    args.assetHighlightCrc = 0;
    args.assetHighlightGameObjIdx = -1;
    // Collision visualization state
    args.collisionVisToggle = 0;
    // Phase 3-5 visualization states
    args.togglePathLinks = 0;
    args.toggleCaptureRadii = 0;
    args.toggleSpawnChains = 0;
    args.toggleAIGoals = 0;
    args.toggleSoundRadii = 0;
    args.requestValidateLevel = 0;
    args.requestNewLevelTemplate = 0;
    args.createKitRequested = 0;
    args.createGamemodeRequested = 0;
    if (g_renderer && g_renderer->hasLevelScene()) {
        LevelScene* ls = g_renderer->getLevelScene();
        args.collisionVisEnabled = ls ? (ls->collisionVisEnabled() ? 1 : 0) : 0;
        args.showPathLinks     = ls ? (ls->pathLinksVisible() ? 1 : 0) : 0;
        args.showCaptureRadii  = ls ? (ls->captureRadiiVisible() ? 1 : 0) : 0;
        args.showSpawnChains   = ls ? (ls->spawnChainsVisible() ? 1 : 0) : 0;
        args.showAIGoals       = ls ? (ls->aiGoalsVisible() ? 1 : 0) : 0;
        args.showSoundRadii    = ls ? (ls->soundRadiiVisible() ? 1 : 0) : 0;
        args.validationErrorCount = g_lastValidationErrors;
        args.validationWarningCount = g_lastValidationWarnings;
        // Count shapes for the selected model
        int selShapes = 0;
        if (ls) {
            int selIdx = ls->selectedInstance();
            if (selIdx < 0) selIdx = ls->hoveredInstance();
            if (selIdx >= 0) {
                const LevelInstance* inst = ls->getInstance(selIdx);
                // Count shapes matching this instance or its model
                for (int ci = 0; ci < ls->getCollisionShapeCount(); ++ci) {
                    const LevelCollisionShape* cs = ls->getCollisionShape(ci);
                    if (cs && cs->instanceIdx == selIdx) ++selShapes;
                }
                // If no direct match, count by model CRC
                if (selShapes == 0 && inst && inst->model) {
                    for (int ci = 0; ci < ls->getCollisionShapeCount(); ++ci) {
                        const LevelCollisionShape* cs = ls->getCollisionShape(ci);
                        if (!cs) continue;
                        // Match if same model
                        if (cs->instanceIdx >= 0 && cs->instanceIdx < ls->getInstanceCount()) {
                            const LevelInstance* csInst = ls->getInstance(cs->instanceIdx);
                            if (csInst && csInst->model == inst->model) { ++selShapes; break; }
                        }
                    }
                    // If found any, recount properly
                    if (selShapes > 0) {
                        selShapes = 0;
                        for (int ci = 0; ci < ls->getCollisionShapeCount(); ++ci) {
                            const LevelCollisionShape* cs = ls->getCollisionShape(ci);
                            if (!cs) continue;
                            if (cs->instanceIdx >= 0 && cs->instanceIdx < ls->getInstanceCount()) {
                                const LevelInstance* csInst = ls->getInstance(cs->instanceIdx);
                                if (csInst && csInst->model == inst->model) ++selShapes;
                            }
                        }
                    }
                }
            }
        }
        args.collisionShapeCount = selShapes;
    } else {
        args.collisionVisEnabled = 0;
        args.collisionShapeCount = 0;
    }

    // Asset Inspector — DLL requests, reset each frame
    args.assetInspectorToggle = g_showAssetInspector ? 1 : 0;
    // Consume the toggle — DLL maintains its own open/close state
    // We send 1 only on the frame F6 was pressed
    static bool s_prevShowAI = false;
    if (g_showAssetInspector != s_prevShowAI) {
        args.assetInspectorToggle = 1; // toggle event
        s_prevShowAI = g_showAssetInspector;
    } else {
        args.assetInspectorToggle = 0;
    }
    args.assetInspectorRequestCat = -1;
    args.assetInspectorRequestIdx = -1;
    args.assetInspectorEditRequested = 0;
    // Asset Inspector — supply raw Block1 data for previous frame's request
    // (DLL sets requestCat/Idx, host supplies data next frame via a static cache)
    {
        args.assetInspectorValid = 0;
        args.assetInspectorData = NULL;
        args.assetInspectorStride = 0;
        args.assetInspectorBlock1Off = 0;
        args.assetInspectorCat = -1;
        args.assetInspectorIdx = -1;
        if (g_assetInspCat >= 0 && g_assetInspIdx >= 0 && g_levelReader.IsLoaded() && !g_levelReader.GetBlock1().empty()) {
            const ZeroEngine::PakHeader& hdr = g_levelReader.GetPakHeader();
            uint32_t off = 0, stride = 0, count = 0;
            // Map category to PakHeader offset/size/count fields
            switch (g_assetInspCat) {
                case 12: off=hdr.buffer_info_offset; stride=hdr.buffer_info_size; count=hdr.buffer_info_num; break;
                case 13: off=hdr.mat1_offset;        stride=hdr.mat1_size;        count=hdr.mat1_num; break;
                case 14: off=hdr.mat2_offset;        stride=hdr.mat2_size;        count=hdr.mat2_num; break;
                case 15: off=hdr.mat3_offset;        stride=hdr.mat3_size;        count=hdr.mat3_num; break;
                case 16: off=hdr.mat4_offset;        stride=hdr.mat4_size;        count=hdr.mat4_num; break;
                case 17: off=hdr.mat_extra_offset;   stride=hdr.mat_extra_size;   count=hdr.mat_extra_num; break;
                case 18: off=hdr.shape_info_offset;  stride=hdr.shape_info_size;  count=hdr.shape_info_num; break;
                case 19: off=hdr.hk_shape_info_offset;stride=hdr.hk_shape_info_size;count=hdr.hk_shape_info_num;break;
                case 20: off=hdr.hk_constraint_data_offset;stride=hdr.hk_constraint_data_size;count=hdr.hk_constraint_data_num;break;
                case 21: off=hdr.vbuff_info_offset;  stride=hdr.vbuff_info_size;  count=hdr.vbuff_info_num; break;
                case 22: off=hdr.ibuff_info_offset;  stride=hdr.ibuff_info_size;  count=hdr.ibuff_info_num; break;
                case 23: off=hdr.hk_constraint_info_offset;stride=hdr.hk_constraint_info_size;count=hdr.hk_constraint_info_num;break;
                case 24: off=hdr.pfield_info_offset; stride=hdr.pfield_info_size; count=hdr.pfield_info_num; break;
                case 25: off=hdr.radiosity_vals_info_offset;stride=hdr.radiosity_vals_info_size;count=hdr.radiosity_vals_info_num;break;
                case 26: off=hdr.foliage_info_offset;stride=hdr.foliage_info_size;count=hdr.foliage_info_num;break;
                default: break;
            }
            if (stride > 0 && (uint32_t)g_assetInspIdx < count) {
                uint32_t entryOff = off + (uint32_t)g_assetInspIdx * stride;
                if (entryOff + stride <= (uint32_t)g_levelReader.GetBlock1().size()) {
                    args.assetInspectorValid = 1;
                    args.assetInspectorCat = g_assetInspCat;
                    args.assetInspectorIdx = g_assetInspIdx;
                    args.assetInspectorBlock1Off = entryOff;
                    args.assetInspectorStride = (int)stride;
                    args.assetInspectorData = &g_levelReader.GetBlock1()[entryOff];
                }
            }
        }
    }
    // Wire type defs for creation wizard
    {
        const std::vector<ZeroEngine::LevelGameObjTypeDef>& types = g_levelReader.GetGameObjTypes();
        static std::vector<const char*> s_typeDefNamePtrs;
        static std::vector<int> s_typeDefFieldCounts;
        static std::vector<int> s_typeDefInstanceCounts;
        int n = (int)types.size();
        s_typeDefNamePtrs.resize(n);
        s_typeDefFieldCounts.resize(n);
        s_typeDefInstanceCounts.resize(n);
        for (int i = 0; i < n; ++i) {
            s_typeDefNamePtrs[i] = types[i].name.c_str();
            s_typeDefFieldCounts[i] = (int)types[i].field_count;
            s_typeDefInstanceCounts[i] = (int)types[i].instance_count;
        }
        args.createTypeDefCount = n;
        args.createTypeDefNames = n > 0 ? &s_typeDefNamePtrs[0] : NULL;
        args.createTypeDefFieldCounts = n > 0 ? &s_typeDefFieldCounts[0] : NULL;
        args.createTypeDefInstanceCounts = n > 0 ? &s_typeDefInstanceCounts[0] : NULL;
    }
    // Wire model list for entity creation model picker
    {
        const std::vector<ZeroEngine::LevelModelEntry>& models = g_levelReader.GetModels();
        static std::vector<const char*> s_modelNamePtrs;
        static std::vector<unsigned int> s_modelCrcs;
        int nm = (int)models.size();
        s_modelNamePtrs.resize(nm);
        s_modelCrcs.resize(nm);
        for (int i = 0; i < nm; ++i) {
            s_modelNamePtrs[i] = models[i].name.c_str();
            s_modelCrcs[i] = models[i].crc;
        }
        args.createModelCount = nm;
        args.createModelNames = nm > 0 ? &s_modelNamePtrs[0] : NULL;
        args.createModelCrcs = nm > 0 ? &s_modelCrcs[0] : NULL;
        args.createEntityMeshCrc = 0;
    }

    // ---- Model Viewer state ----
    {
        IDirect3DDevice9* dev = g_renderer ? g_renderer->getD3DDevice() : NULL;

        // Create render target if needed
        if (dev && !g_mvRenderTarget) {
            if (SUCCEEDED(dev->CreateTexture(g_mvTexW, g_mvTexH, 1,
                    D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                    &g_mvRenderTarget, NULL))) {
                if (FAILED(g_mvRenderTarget->GetSurfaceLevel(0, &g_mvRTSurface))) {
                    ReleaseModelViewerRenderTarget();
                } else {
                    D3DFORMAT depthFmt = D3DFMT_D24S8;
                    IDirect3DSurface9* oldDepthForFmt = NULL;
                    if (SUCCEEDED(dev->GetDepthStencilSurface(&oldDepthForFmt)) && oldDepthForFmt) {
                        D3DSURFACE_DESC oldDepthDesc;
                        if (SUCCEEDED(oldDepthForFmt->GetDesc(&oldDepthDesc))) {
                            depthFmt = oldDepthDesc.Format;
                        }
                        oldDepthForFmt->Release();
                    }

                    HRESULT dsHr = dev->CreateDepthStencilSurface(g_mvTexW, g_mvTexH,
                        depthFmt, D3DMULTISAMPLE_NONE, 0, TRUE,
                        &g_mvDepthSurface, NULL);
                    if (FAILED(dsHr) || !g_mvDepthSurface) {
                        if (g_mvDepthSurface) { g_mvDepthSurface->Release(); g_mvDepthSurface = NULL; }
                        dsHr = dev->CreateDepthStencilSurface(g_mvTexW, g_mvTexH,
                            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
                            &g_mvDepthSurface, NULL);
                    }
                    if (FAILED(dsHr) || !g_mvDepthSurface) {
                        if (g_mvDepthSurface) { g_mvDepthSurface->Release(); g_mvDepthSurface = NULL; }
                        dsHr = dev->CreateDepthStencilSurface(g_mvTexW, g_mvTexH,
                            D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE,
                            &g_mvDepthSurface, NULL);
                    }
                    if (FAILED(dsHr) || !g_mvDepthSurface) {
                        if (g_mvDepthSurface) { g_mvDepthSurface->Release(); g_mvDepthSurface = NULL; }
                        dsHr = dev->CreateDepthStencilSurface(g_mvTexW, g_mvTexH,
                            D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE,
                            &g_mvDepthSurface, NULL);
                    }
                    if (FAILED(dsHr) || !g_mvDepthSurface) {
                        ReleaseModelViewerRenderTarget();
                    }
                }
            }
        }

        // Render model to texture if viewer is open and model selected
        if (dev && g_mvRenderTarget && g_mvRTSurface && g_mvDepthSurface &&
            g_mvModelCrc != 0 && g_mvViewerOpen &&
            g_renderer && g_renderer->hasLevelScene()) {
            LevelScene* ls = g_renderer->getLevelScene();
            LevelModel* mdl = ls ? ls->getModelByCrc(g_mvModelCrc) : NULL;
            if (mdl) {
                // Save current render target
                IDirect3DSurface9* oldRT = NULL;
                IDirect3DSurface9* oldDS = NULL;
                D3DVIEWPORT9 oldVP;
                memset(&oldVP, 0, sizeof(oldVP));
                bool haveOldVP = SUCCEEDED(dev->GetViewport(&oldVP));
                D3DMATRIX oldWorld, oldView, oldProj;
                memset(&oldWorld, 0, sizeof(oldWorld));
                memset(&oldView, 0, sizeof(oldView));
                memset(&oldProj, 0, sizeof(oldProj));
                bool haveOldWorld = SUCCEEDED(dev->GetTransform(D3DTS_WORLD, &oldWorld));
                bool haveOldView  = SUCCEEDED(dev->GetTransform(D3DTS_VIEW, &oldView));
                bool haveOldProj  = SUCCEEDED(dev->GetTransform(D3DTS_PROJECTION, &oldProj));
                DWORD oldFVF = 0;
                dev->GetFVF(&oldFVF);
                IDirect3DVertexShader9* oldVS = NULL;
                IDirect3DPixelShader9* oldPS = NULL;
                IDirect3DVertexDeclaration9* oldDecl = NULL;
                dev->GetVertexShader(&oldVS);
                dev->GetPixelShader(&oldPS);
                dev->GetVertexDeclaration(&oldDecl);
                DWORD oldFill = D3DFILL_SOLID, oldLighting = FALSE, oldCull = D3DCULL_CCW;
                DWORD oldZEnable = TRUE, oldZWrite = TRUE, oldAlpha = FALSE;
                DWORD oldSrcBlend = D3DBLEND_SRCALPHA, oldDstBlend = D3DBLEND_INVSRCALPHA;
                DWORD oldAALine = FALSE;
                DWORD oldColorWrite = 0xF, oldSrgbWrite = FALSE;
                dev->GetRenderState(D3DRS_FILLMODE, &oldFill);
                dev->GetRenderState(D3DRS_LIGHTING, &oldLighting);
                dev->GetRenderState(D3DRS_CULLMODE, &oldCull);
                dev->GetRenderState(D3DRS_ZENABLE, &oldZEnable);
                dev->GetRenderState(D3DRS_ZWRITEENABLE, &oldZWrite);
                dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlpha);
                dev->GetRenderState(D3DRS_SRCBLEND, &oldSrcBlend);
                dev->GetRenderState(D3DRS_DESTBLEND, &oldDstBlend);
                dev->GetRenderState(D3DRS_ANTIALIASEDLINEENABLE, &oldAALine);
                dev->GetRenderState(D3DRS_COLORWRITEENABLE, &oldColorWrite);
                dev->GetRenderState(D3DRS_SRGBWRITEENABLE, &oldSrgbWrite);
                IDirect3DBaseTexture9* oldTex0 = NULL;
                dev->GetTexture(0, &oldTex0);
                DWORD oldSampler0Srgb = FALSE;
                dev->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &oldSampler0Srgb);
                DWORD oldTss0ColorOp = D3DTOP_MODULATE;
                DWORD oldTss0ColorArg1 = D3DTA_TEXTURE, oldTss0ColorArg2 = D3DTA_DIFFUSE;
                DWORD oldTss1ColorOp = D3DTOP_DISABLE;
                dev->GetTextureStageState(0, D3DTSS_COLOROP, &oldTss0ColorOp);
                dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oldTss0ColorArg1);
                dev->GetTextureStageState(0, D3DTSS_COLORARG2, &oldTss0ColorArg2);
                dev->GetTextureStageState(1, D3DTSS_COLOROP, &oldTss1ColorOp);
                dev->GetRenderTarget(0, &oldRT);
                dev->GetDepthStencilSurface(&oldDS);

                // Set model viewer render target
                // ImGui may still have this texture bound from the previous
                // frame. Binding it as RT while it is also a sampler is D3D9
                // eating its own fucking tail: best case black preview, worst
                // case driver-dependent garbage. Pull the sampler plug first.
                dev->SetTexture(0, NULL);
                dev->SetRenderTarget(0, g_mvRTSurface);
                dev->SetDepthStencilSurface(g_mvDepthSurface);
                D3DVIEWPORT9 mvVP = { 0, 0, (DWORD)g_mvTexW, (DWORD)g_mvTexH, 0.0f, 1.0f };
                dev->SetViewport(&mvVP);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
                dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
                dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
                dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_ARGB(255, 40, 40, 50), 1.0f, 0);

                // Build orbit camera view + projection (use host-side values)
                float yaw   = g_mvOrbitYaw;
                float pitch = g_mvOrbitPitch;
                float dist  = g_mvOrbitDist;

                // Model center from bounding box
                float cx = 0, cy = 0, cz = 0;
                if (mdl->hasBounds) {
                    cx = (mdl->localMin[0] + mdl->localMax[0]) * 0.5f;
                    cy = (mdl->localMin[1] + mdl->localMax[1]) * 0.5f;
                    cz = (mdl->localMin[2] + mdl->localMax[2]) * 0.5f;
                }

                float cosP = cosf(pitch), sinP = sinf(pitch);
                float cosY = cosf(yaw),   sinY = sinf(yaw);
                float eyeX = cx + dist * cosP * sinY;
                float eyeY = cy + dist * sinP;
                float eyeZ = cz + dist * cosP * cosY;

                // Build view matrix (LookAt LH) manually — no D3DX dependency
                float zAx[3] = { cx - eyeX, cy - eyeY, cz - eyeZ }; // forward
                float zLen = sqrtf(zAx[0]*zAx[0]+zAx[1]*zAx[1]+zAx[2]*zAx[2]);
                if (zLen > 1e-6f) { zAx[0]/=zLen; zAx[1]/=zLen; zAx[2]/=zLen; }
                float upV[3] = {0,1,0};
                float xAx[3] = { upV[1]*zAx[2]-upV[2]*zAx[1],
                                  upV[2]*zAx[0]-upV[0]*zAx[2],
                                  upV[0]*zAx[1]-upV[1]*zAx[0] }; // right = up x forward
                float xLen = sqrtf(xAx[0]*xAx[0]+xAx[1]*xAx[1]+xAx[2]*xAx[2]);
                if (xLen > 1e-6f) { xAx[0]/=xLen; xAx[1]/=xLen; xAx[2]/=xLen; }
                float yAx[3] = { zAx[1]*xAx[2]-zAx[2]*xAx[1],
                                  zAx[2]*xAx[0]-zAx[0]*xAx[2],
                                  zAx[0]*xAx[1]-zAx[1]*xAx[0] }; // up = forward x right

                D3DMATRIX viewMat;
                memset(&viewMat, 0, sizeof(viewMat));
                viewMat._11=xAx[0]; viewMat._21=xAx[1]; viewMat._31=xAx[2];
                viewMat._12=yAx[0]; viewMat._22=yAx[1]; viewMat._32=yAx[2];
                viewMat._13=zAx[0]; viewMat._23=zAx[1]; viewMat._33=zAx[2];
                viewMat._41=-(xAx[0]*eyeX+xAx[1]*eyeY+xAx[2]*eyeZ);
                viewMat._42=-(yAx[0]*eyeX+yAx[1]*eyeY+yAx[2]*eyeZ);
                viewMat._43=-(zAx[0]*eyeX+zAx[1]*eyeY+zAx[2]*eyeZ);
                viewMat._44=1.0f;

                // Build perspective projection (LH)
                float fovY = 3.14159265f / 4.0f;
                float aspect = (float)g_mvTexW / (float)g_mvTexH;
                float zn = 0.1f, zf = 1000.0f;
                float yScale = 1.0f / tanf(fovY * 0.5f);
                float xScale = yScale / aspect;
                D3DMATRIX projMat;
                memset(&projMat, 0, sizeof(projMat));
                projMat._11 = xScale;
                projMat._22 = yScale;
                projMat._33 = zf / (zf - zn);
                projMat._34 = 1.0f;
                projMat._43 = -zn * zf / (zf - zn);

                D3DMATRIX worldMat;
                memset(&worldMat, 0, sizeof(worldMat));
                worldMat._11 = worldMat._22 = worldMat._33 = worldMat._44 = 1.0f;

                dev->SetTransform(D3DTS_VIEW, &viewMat);
                dev->SetTransform(D3DTS_PROJECTION, &projMat);
                dev->SetTransform(D3DTS_WORLD, &worldMat);

                // Wireframe / solid choice still applies to whichever path we take.
                dev->SetRenderState(D3DRS_FILLMODE,
                    g_mvWireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);

                // Prefer the shader path that LevelScene::render() uses for the
                // main viewport. drawSingleModelWithShaders runs the same
                // m_levelVS / m_levelPS pair the level instance loop runs, so
                // the popup picture matches the main viewport's material
                // output (per-pixel Lambert + ambient + secondary sun +
                // scatter inscatter). Falls through to the legacy fixed-
                // function diffuse-modulate path only when the shader
                // pipeline never initialized — in practice every device with
                // PS 3.0 support hits the shader path.
                bool drewWithShaders = ls->drawSingleModelWithShaders(
                    mdl,
                    (const float*)&worldMat,
                    (const float*)&viewMat,
                    (const float*)&projMat);

                if (!drewWithShaders) {
                    // FFP fallback. This is the original popup draw path and
                    // is intentionally minimal: one texture stage, FVF only,
                    // no lighting. Looks flat next to the main viewport but
                    // keeps the popup usable on any device that fails shader
                    // init for whatever reason.
                    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
                    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
                    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
                    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                    dev->SetVertexShader(NULL);
                    dev->SetPixelShader(NULL);
                    dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
                    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                    dev->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

                    for (int pi = 0; pi < (int)mdl->parts.size(); ++pi) {
                        const LevelMeshPart& part = mdl->parts[pi];
                        if (!part.vb || !part.ib || part.vertexCount == 0 || part.indexCount == 0) continue;
                        dev->SetTexture(0, part.diffuseTex);
                        dev->SetTextureStageState(0, D3DTSS_COLOROP,
                            part.diffuseTex ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
                        dev->SetTextureStageState(0, D3DTSS_COLORARG1,
                            part.diffuseTex ? D3DTA_TEXTURE : D3DTA_DIFFUSE);
                        dev->SetStreamSource(0, part.vb, 0, sizeof(LevelVertex));
                        dev->SetIndices(part.ib);
                        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                            part.vertexCount, 0, part.indexCount / 3);
                    }
                }

                // ═════════════════════════════════════════════════════════
                //  Collision wireframe overlay - the Blender-style rewrite
                // ═════════════════════════════════════════════════════════
                // Old code: D3DFILL_WIREFRAME on the triangle list. Drew
                // every edge of every triangle, so neighbors double-stamped
                // every shared edge. Triple density. Looked like green
                // fucking pasta. The user's words, repeatedly.
                //
                // New code: rebuild a deduped EDGE LIST once per collision
                // load, render LINELIST. Per-vertex color comes from depth
                // fade + back-face dim, all computed once per frame in CPU
                // because we are talking thousands of edges, not millions,
                // and a vertex shader for this would be more setup pain
                // than the cycles it would save. Don't optimize what does
                // not need optimizing.
                if (g_mvShowCollision && g_mvHasCollision && !g_mvCollVerts.empty()) {
                    // Lazy edge dedup. Runs ONCE per collision-data load.
                    // The dirty flag gets re-raised in the BuildCollision
                    // request handler and on Unload. If you find a path
                    // that mutates g_mvCollVerts / g_mvCollIndices and
                    // forgets to set g_mvWireDirty = true, you will spend
                    // a LONG time wondering why the wireframe shows the
                    // OLD geometry. Set the goddamn flag.
                    if (g_mvWireDirty) {
                        RebuildCollisionWireEdges();
                        g_mvWireDirty = false;
                    }

                    int numEdges = (int)g_mvWireEdges.size();
                    if (numEdges > 0) {
                        // ── Per-frame: build line-list verts with shaded colors ──
                        //
                        // For each edge we emit two verts (start, end). The
                        // color comes from THREE multiplied factors:
                        //
                        //   1. Base hue: green (0,1,0) by default. Could be
                        //      swapped to cyan for selected, orange for LOD,
                        //      whatever - the multiply chain stays the same.
                        //
                        //   2. Depth fade: 1.0 at the near plane, 0.4 at
                        //      the far plane (linear interp clamped to
                        //      [0.4, 1.0]). Far edges retreat into the
                        //      background, near edges pop forward. Without
                        //      this, the wireframe is depth-flat and you
                        //      cannot tell which way the volume curves.
                        //
                        //   3. Back-face dim: dot the edge's average face
                        //      normal with the view direction (camera ->
                        //      edge midpoint). If the dot is negative the
                        //      edge is on the back of the volume - alpha
                        //      it down to ~0.25 so it fades into the
                        //      background instead of fighting the front
                        //      edges for visual attention.
                        //
                        // Computed in CPU per frame because the line-list
                        // path uses DrawPrimitiveUP (system-memory verts)
                        // anyway, so we are already touching every vert.
                        // Vertex shader would need its own cbuffer of
                        // camera + fade params per draw, more plumbing
                        // than gain.
                        struct WireV { float x,y,z; DWORD col; };
                        std::vector<WireV> verts(numEdges * 2);

                        // Compute fade range from the model bounding box,
                        // not a hardcoded constant. A 5cm collision pickup
                        // and a 200m fortress wall both deserve sensible
                        // fade. Take the bounding diagonal as the "this is
                        // a far edge" benchmark.
                        float bbDiag = 1.0f;
                        if (mdl->hasBounds) {
                            float dx = mdl->localMax[0] - mdl->localMin[0];
                            float dy = mdl->localMax[1] - mdl->localMin[1];
                            float dz = mdl->localMax[2] - mdl->localMin[2];
                            bbDiag = sqrtf(dx*dx + dy*dy + dz*dz);
                            if (bbDiag < 0.1f) bbDiag = 0.1f;
                        }
                        float fadeNear = bbDiag * 0.05f;  // edges within 5% of bb-diag are full bright
                        float fadeFar  = bbDiag * 1.5f;   // edges beyond 1.5x bb-diag are fully faded
                        float fadeRange = fadeFar - fadeNear;
                        if (fadeRange < 1e-3f) fadeRange = 1.0f;

                        for (int e = 0; e < numEdges; ++e) {
                            const CollisionEdge& ce = g_mvWireEdges[e];
                            const float* pa = &g_mvCollVerts[ce.a*3];
                            const float* pb = &g_mvCollVerts[ce.b*3];

                            // Edge midpoint, vector camera->midpoint, normalized.
                            float mx = (pa[0]+pb[0])*0.5f - eyeX;
                            float my = (pa[1]+pb[1])*0.5f - eyeY;
                            float mz = (pa[2]+pb[2])*0.5f - eyeZ;
                            float mlen = sqrtf(mx*mx + my*my + mz*mz);
                            if (mlen < 1e-6f) mlen = 1.0f;
                            float vx = mx / mlen, vy = my / mlen, vz = mz / mlen;

                            // Back-face dim: dot of avg face normal with
                            // view direction. Front-facing (dot < 0 because
                            // normal points away from view) = full bright.
                            // Back-facing (dot > 0) = dimmed.
                            float dotN = ce.nx*vx + ce.ny*vy + ce.nz*vz;
                            float backDim = (dotN > 0.0f) ? 0.30f : 1.0f;

                            // Per-endpoint depth fade.
                            float dxA = pa[0]-eyeX, dyA = pa[1]-eyeY, dzA = pa[2]-eyeZ;
                            float dxB = pb[0]-eyeX, dyB = pb[1]-eyeY, dzB = pb[2]-eyeZ;
                            float dA = sqrtf(dxA*dxA + dyA*dyA + dzA*dzA);
                            float dB = sqrtf(dxB*dxB + dyB*dyB + dzB*dzB);
                            float fA = 1.0f - 0.6f * ((dA - fadeNear) / fadeRange);
                            float fB = 1.0f - 0.6f * ((dB - fadeNear) / fadeRange);
                            if (fA < 0.4f) fA = 0.4f; if (fA > 1.0f) fA = 1.0f;
                            if (fB < 0.4f) fB = 0.4f; if (fB > 1.0f) fB = 1.0f;

                            // Final composite. Green channel modulated by
                            // backDim * fade. Alpha follows the same curve
                            // so back+far edges blend out, front+near edges
                            // stay opaque against the model behind.
                            float gA = backDim * fA;
                            float gB = backDim * fB;
                            int   aA = (int)(255.0f * gA);
                            int   aB = (int)(255.0f * gB);
                            int   chA = (int)(255.0f * gA);
                            int   chB = (int)(255.0f * gB);
                            DWORD colA = D3DCOLOR_ARGB(aA, 0, chA, 0);
                            DWORD colB = D3DCOLOR_ARGB(aB, 0, chB, 0);

                            verts[e*2+0].x = pa[0]; verts[e*2+0].y = pa[1]; verts[e*2+0].z = pa[2]; verts[e*2+0].col = colA;
                            verts[e*2+1].x = pb[0]; verts[e*2+1].y = pb[1]; verts[e*2+1].z = pb[2]; verts[e*2+1].col = colB;
                        }

                        // ── Render state for the line pass ──
                        // FILLMODE back to SOLID because LINELIST is not
                        // affected by wireframe fillmode but other state
                        // resets care. Alpha blend ON so the dimmed edges
                        // actually fade against the model. Depth read ON
                        // so wireframe is occluded by SOLID geometry in
                        // FRONT of it (you can still see the wireframe of
                        // the collision hull through the model surface
                        // because we render it after the model and depth
                        // is the same - this is a feature not a bug, the
                        // user wants to see the collision shape clearly).
                        // Anti-aliased lines if the driver supports it -
                        // some old D3D9 drivers ignore this hint, in which
                        // case we get the jaggies anyway and there is
                        // nothing further we can do at this layer without
                        // a full geometry-shader-style line expansion.
                        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
                        dev->SetRenderState(D3DRS_ZENABLE, TRUE);
                        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                        dev->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE);
                        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                        dev->SetTexture(0, NULL);
                        dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
                        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);

                        dev->DrawPrimitiveUP(D3DPT_LINELIST, numEdges, &verts[0], sizeof(WireV));

                        // Restore state we touched. The outer Restore block
                        // below does FILLMODE + ZWRITE but does not know
                        // about ALPHABLEND / ANTIALIASEDLINE / CULLMODE -
                        // we have to put those back ourselves or the next
                        // model draw will be subtly wrong.
                        dev->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
                        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                    }
                }

                // Restore state
                dev->SetRenderTarget(0, oldRT);
                dev->SetDepthStencilSurface(oldDS);
                if (haveOldVP) dev->SetViewport(&oldVP);
                dev->SetTexture(0, oldTex0);
                if (oldTex0) oldTex0->Release();
                dev->SetTextureStageState(0, D3DTSS_COLOROP, oldTss0ColorOp);
                dev->SetTextureStageState(0, D3DTSS_COLORARG1, oldTss0ColorArg1);
                dev->SetTextureStageState(0, D3DTSS_COLORARG2, oldTss0ColorArg2);
                dev->SetTextureStageState(1, D3DTSS_COLOROP, oldTss1ColorOp);
                dev->SetRenderState(D3DRS_FILLMODE, oldFill);
                dev->SetRenderState(D3DRS_LIGHTING, oldLighting);
                dev->SetRenderState(D3DRS_CULLMODE, oldCull);
                dev->SetRenderState(D3DRS_ZENABLE, oldZEnable);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, oldZWrite);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlpha);
                dev->SetRenderState(D3DRS_SRCBLEND, oldSrcBlend);
                dev->SetRenderState(D3DRS_DESTBLEND, oldDstBlend);
                dev->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, oldAALine);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, oldColorWrite);
                dev->SetRenderState(D3DRS_SRGBWRITEENABLE, oldSrgbWrite);
                dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, oldSampler0Srgb);
                if (haveOldWorld) dev->SetTransform(D3DTS_WORLD, &oldWorld);
                if (haveOldView)  dev->SetTransform(D3DTS_VIEW, &oldView);
                if (haveOldProj)  dev->SetTransform(D3DTS_PROJECTION, &oldProj);
                dev->SetVertexShader(oldVS);
                dev->SetPixelShader(oldPS);
                if (oldDecl) dev->SetVertexDeclaration(oldDecl);
                else if (oldFVF != 0) dev->SetFVF(oldFVF);
                else dev->SetVertexDeclaration(NULL);
                if (oldVS) oldVS->Release();
                if (oldPS) oldPS->Release();
                if (oldDecl) oldDecl->Release();
                if (oldRT) oldRT->Release();
                if (oldDS) oldDS->Release();
            }
        }

        // Pass state to DLL
        args.modelViewerTexture = g_mvRenderTarget;
        args.modelViewerTexW = g_mvTexW;
        args.modelViewerTexH = g_mvTexH;
        args.modelViewerModelName = g_mvModelName.empty() ? NULL : g_mvModelName.c_str();
        args.modelViewerHasCollision = g_mvHasCollision ? 1 : 0;
        args.modelViewerCollisionTris = g_mvCollisionTris;
        args.modelViewerBuildProgress = g_mvBuildProgress;
        args.modelViewerRequestOpen = 0;
        args.modelViewerRequestCrc = 0;
        args.modelViewerBuildRequested = 0;
        args.modelViewerSaveRequested = 0;
        args.modelViewerOpen = 0;
    }

    // Mocap Studio state
    args.mocapState           = (int)g_mocapBridge.GetState();
    args.mocapProgress        = g_mocapBridge.GetProgress();
    args.mocapStatusMsg       = g_mocapBridge.GetStatusMsg();
    args.mocapErrorMsg        = g_mocapBridge.GetErrorMsg();
    args.mocapTotalFrames     = g_mocapBridge.GetTotalFrames();
    args.mocapReceivedFrames  = g_mocapBridge.GetReceivedFrames();
    args.mocapFps             = g_mocapBridge.GetFps();
    args.mocapSubjectCount    = g_mocapBridge.GetSubjectCount();
    args.mocapPlaybackTime    = g_mocapPlayTime;
    args.mocapDuration        = (g_mocapBridge.GetTotalFrames() > 1 && g_mocapBridge.GetFps() > 0.0f)
                                ? (float)(g_mocapBridge.GetTotalFrames() - 1) / g_mocapBridge.GetFps() : 0.0f;
    args.mocapPlaying         = g_mocapPlaying ? 1 : 0;
    args.mocapFingerCurlL     = g_mocapRetargeter.fingerCurl.leftGrip;
    args.mocapFingerCurlR     = g_mocapRetargeter.fingerCurl.rightGrip;
    args.mocapBonePositions   = NULL;
    args.mocapBonePositionCount = 0;
    args.mocapRequestLoadVideo    = 0;
    args.mocapRequestCancel       = 0;
    args.mocapRequestTogglePlay   = 0;
    args.mocapRequestSetTime      = 0;
    args.mocapRequestedTime       = 0.0f;
    args.mocapRequestConvert      = 0;
    args.mocapRequestApplyToModel = 0;
    args.mocapRequestSetFingerCurl = 0;

    args.selectedEditorObjIdx  = -1;
    args.selectedEditorObjGuid = 0;
    args.selectedEditorObjName = "";
    args.selectedEditorObjType = "";
    args.selectedEditorObjPos[0] = args.selectedEditorObjPos[1] = args.selectedEditorObjPos[2] = 0;
    args.editorObjFilterActive = 0;
    args.editorObjFilterGuidCount = 0;
    args.editorObjFilterGuids = 0;
    args.editorObjMaxDist = 0;
    args.editorObjFadeStart = 0;
    args.editorObjMinDist = 0;
    args.editorObjCategoryMask = 0x1FFFu;
    args.editorObjSearchMode = 0;
    args.editorObjSearchTerm[0] = '\0';
    args.editorObjLabelMaxCount = 0;
    args.requestSelectEditorObjGuid = 0;
    args.requestFocusEditorObjGuid = 0;
    args.gameObjTeams = g_goTeams.empty() ? NULL : &g_goTeams[0];
    // Phase 1 XSI: prime event-wire toggle state from current LevelScene
    // so the DLL's checkbox renders the actual state, not whatever was
    // on the heap. Default to "show all wires" if there's no scene yet.
    {
        LevelScene* lsW = (g_renderer && g_renderer->hasLevelScene()) ? g_renderer->getLevelScene() : NULL;
        args.eventWiresShow     = lsW ? (lsW->eventWiresVisible() ? 1 : 0) : 1;
        args.eventWireFocusGuid = lsW ? lsW->eventWireFocusGuid() : 0u;
    }
    // Phase 2 XSI: zero the reparent request before DrawFrame. DLL writes
    // it on a successful drop, host drains it after DrawFrame.
    args.requestReparentChildGuid     = 0;
    args.requestReparentNewParentGuid = 0;
    // Phase 3 XSI: layer manager output arrays start NULL/zero. The DLL
    // fills them inside DrawFrame from its INI-persisted sets. Host
    // reads them out post-DrawFrame and pushes into LevelScene.
    args.hiddenLayerCount  = 0;
    args.hiddenLayerGuids  = NULL;
    args.lockedLayerCount  = 0;
    args.lockedLayerGuids  = NULL;
    args.isolatedLayerGuid = 0;
    args.activeLayerGuid   = 0;
    // Phase 4 XSI: prime LEVEL gizmo state from current LevelScene so
    // the toolbar buttons paint the actual mode, drag flag tells the
    // DLL not to let the user change modes mid-drag. Field names are
    // levelGizmo* to avoid collision with the older Scene3DRenderer
    // bone gizmo fields earlier in the struct.
    {
        LevelScene* lsG = (g_renderer && g_renderer->hasLevelScene()) ? g_renderer->getLevelScene() : NULL;
        args.levelGizmoMode     = lsG ? lsG->gizmoMode()  : 0;
        args.levelGizmoSpace    = lsG ? lsG->gizmoSpace() : 0;
        args.levelGizmoSnap     = lsG ? lsG->gizmoSnap()  : 0.0f;
        args.levelGizmoDragging = (lsG && lsG->gizmoDragging()) ? 1 : 0;
    }
    // Phase 6 XSI: prime the spline-handles bridge — fully host-side.
    //
    // The user does NOT want a separate "Enable Spline Edit" toggle
    // floating around as another button to remember. If they have a
    // spline-bearing entity selected, edit handles light up. If they
    // don't, handles stay dark. Auto-on, auto-off, no extra switch
    // for the muscle memory to fight. The DLL panel could overlay a
    // hint if it wanted, but the BEHAVIOR is decided right here.
    //
    // Shift / Delete come from GetAsyncKeyState because the DLL would
    // need its own ImGui IO trip and we already do key polling host-
    // side for camera/edit shortcuts. Edge-detect Delete via a static
    // so a held key does not stutter-spam node deletions every frame
    // until the spline is one fucking point.
    {
        LevelScene* lsS = (g_renderer && g_renderer->hasLevelScene()) ? g_renderer->getLevelScene() : NULL;
        uint32_t splineGuid = lsS ? lsS->selectedSplineGuid() : 0u;
        args.splineHandlesFocusGuid = splineGuid;
        args.splineHandlesEnabled   = (splineGuid != 0) ? 1 : 0;
        args.splineHandleShiftHeld  = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;
        static bool s_delPrev = false;
        bool delNow = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        args.splineHandleDeletePressed = (delNow && !s_delPrev) ? 1 : 0;
        s_delPrev = delNow;
    }
    // Phase 9c: H = toggle layer hidden, Ctrl+D = duplicate entity.
    // Both edge-detected with a static so a held key fires once.
    // Guards: refuse when ImGui has keyboard capture (user typing in
    // a text field), refuse when no entity is selected, refuse H when
    // the entity has no layerGuid (terrain instance, crowd, etc.).
    args.hostRequestToggleLayerHiddenGuid = 0;
    args.hostRequestDuplicateEntityGuid   = 0;
    args.hostRequestDeleteEntityGuid             = 0;
    args.hostRequestCreateEntityFromTemplateGuid = 0;
    // 3dCrowd inspector request flags. Host clears each frame so the DLL
    // only fires once per button click. pendingCrowd* fields stay in args
    // each frame; the DLL writes a fresh value before raising the matching
    // *Requested flag.
    args.crowdDeleteRequested        = 0;
    args.crowdCloneRequested         = 0;
    args.crowdLodEditRequested       = 0;
    args.crowdPlaybackEditRequested  = 0;
    args.crowdPlaceRequested         = 0;
    args.pendingCrowdPlaceArchetypeItem = -1;
    args.crowdGenerateRequested      = 0;
    // crowdGeneratorOpen, pendingCrowdGen* are DLL-owned state — host
    // does NOT zero those each frame, the DLL window keeps its values
    // across frames via the args round-trip.
    {
        LevelScene* lsK = (g_renderer && g_renderer->hasLevelScene()) ? g_renderer->getLevelScene() : NULL;
        bool ctrlHeldK = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool altHeldK  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
        bool shiftHeldK= (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
        static bool s_hPrev = false, s_dPrev = false;
        bool hNow = (GetAsyncKeyState('H') & 0x8000) != 0;
        bool dNow = (GetAsyncKeyState('D') & 0x8000) != 0;
        bool hRising = (hNow && !s_hPrev);
        bool dRising = (dNow && !s_dPrev);
        s_hPrev = hNow;
        s_dPrev = dNow;

        if (lsK && lsK->isLoaded() && !g_uiWantCaptureKeyboard) {
            uint32_t selLayer = 0;
            uint32_t selEntity = 0;
            int eoSel = lsK->selectedEditorObj();
            if (eoSel >= 0) {
                const LevelEditorObj* eo = lsK->getEditorObj(eoSel);
                if (eo) { selLayer = eo->layerGuid; selEntity = eo->guid; }
            }
            if (selEntity == 0) {
                int iSel = lsK->selectedInstance();
                if (iSel >= 0) {
                    const LevelInstance* inst = lsK->getInstance(iSel);
                    if (inst) { selLayer = inst->layerGuid; selEntity = inst->guid; }
                }
            }

            // H = toggle layer hidden (requires no modifier).
            if (hRising && !ctrlHeldK && !altHeldK && !shiftHeldK && selLayer != 0) {
                args.hostRequestToggleLayerHiddenGuid = selLayer;
            }
            // Ctrl+D = duplicate entity (no shift, no alt).
            if (dRising && ctrlHeldK && !altHeldK && !shiftHeldK && selEntity != 0) {
                args.hostRequestDuplicateEntityGuid = selEntity;
            }
        }
    }
    // Counters for HUD display
    {
        LevelScene* ls2 = (g_renderer && g_renderer->hasLevelScene()) ? g_renderer->getLevelScene() : NULL;
        args.editorObjVisibleCount = ls2 ? ls2->editorObjVisibleCount() : 0;
        args.editorObjTotalCount = ls2 ? ls2->editorObjCount() : 0;
    }

    args.gameModeFilter = (g_renderer && g_renderer->hasLevelScene() && g_renderer->getLevelScene())
                          ? g_renderer->getLevelScene()->gameModeFilter() : -1;
    if (g_renderer && g_renderer->hasLevelScene())
    {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls && ls->isLoaded())
        {
            args.inspectorHoveredIdx  = ls->hoveredInstance();
            args.inspectorSelectedIdx = ls->selectedInstance();
            const LevelInstance* sel = ls->getInstance(ls->selectedInstance());
            if (sel)
            {
                args.inspectorSelName         = sel->objName.c_str();
                args.inspectorSelType         = sel->typeName.c_str();
                args.inspectorSelMesh         = sel->meshName.c_str();
                args.inspectorSelGuid         = sel->guid;
                args.inspectorSelParentGuid   = sel->parentGuid;
                args.inspectorSelGameModeMask = sel->gameModeMask;
                args.inspectorSelMatrix       = sel->mat;
                args.inspectorSelPartCount    = sel->model ? (int)sel->model->parts.size() : 0;
            }
            // Selected editor object
            const LevelEditorObj* selEO = ls->getEditorObj(ls->selectedEditorObj());
            if (selEO)
            {
                args.selectedEditorObjIdx  = ls->selectedEditorObj();
                args.selectedEditorObjGuid = selEO->guid;
                args.selectedEditorObjName = selEO->name.c_str();
                args.selectedEditorObjType = selEO->type.c_str();
                args.selectedEditorObjPos[0] = selEO->mat[12];
                args.selectedEditorObjPos[1] = selEO->mat[13];
                args.selectedEditorObjPos[2] = selEO->mat[14];
            }
        }
    }

    // ── Property Editor: populate field data for selected entity ──
    // Static storage (persists across frames, avoids alloc per frame)
    static std::vector<std::string>  s_propFieldNameStrs;
    static std::vector<std::string>  s_propFieldKindStrs;
    static std::vector<std::string>  s_propFieldStringStrs;
    static std::vector<const char*>  s_propFieldNamePtrs;
    static std::vector<const char*>  s_propFieldKindPtrs;
    static std::vector<const char*>  s_propFieldStringPtrs;
    static std::vector<uint32_t>     s_propFieldKindCrcs;
    static std::vector<uint32_t>     s_propFieldOffsets;
    static std::vector<int>          s_propFieldIntVals;
    static std::vector<float>        s_propFieldFloatVals;
    static std::vector<uint32_t>     s_propFieldGuidVals;
    static std::vector<float>        s_propFieldMatrixVals;
    static std::vector<float>        s_propFieldVec3Vals;
    static std::vector<uint32_t>     s_propListGuidData;
    static std::vector<int>          s_propListOffsets;
    static std::vector<int>          s_propListCounts;
    static int                       s_propLastEntityIdx = -1;
    static std::string               s_propEntityTypeStr;

    // KIND CRCs for dispatch
    static const uint32_t K_INT    = ZeroEngine::LotrHashString("Int");
    static const uint32_t K_FLOAT  = ZeroEngine::LotrHashString("Float");
    static const uint32_t K_BOOL   = ZeroEngine::LotrHashString("Bool");
    static const uint32_t K_CRC    = ZeroEngine::LotrHashString("CRC");
    static const uint32_t K_GUID   = ZeroEngine::LotrHashString("GUID");
    static const uint32_t K_MATRIX = ZeroEngine::LotrHashString("Matrix4x4");
    static const uint32_t K_VEC3   = ZeroEngine::LotrHashString("Vector3");
    static const uint32_t K_STRING = ZeroEngine::LotrHashString("String");
    static const uint32_t K_OBJLIST= ZeroEngine::LotrHashString("ObjectList");
    static const uint32_t K_NODELIST=ZeroEngine::LotrHashString("NodeList");
    static const uint32_t K_INTLIST= ZeroEngine::LotrHashString("IntList");
    static const uint32_t K_COLOR  = ZeroEngine::LotrHashString("Color");
    static const uint32_t K_COLOUR = ZeroEngine::LotrHashString("Colour");
    static const uint32_t K_QUAT   = ZeroEngine::LotrHashString("Quaternion");
    static const uint32_t K_ROT    = ZeroEngine::LotrHashString("Rotation");

    args.propEntityGuid = 0;
    args.propEntityType = NULL;
    args.propFieldCount = 0;
    args.propFieldNames = NULL;
    args.propFieldKindNames = NULL;
    args.propFieldKindCrcs = NULL;
    args.propFieldOffsets = NULL;
    args.propFieldIntVals = NULL;
    args.propFieldFloatVals = NULL;
    args.propFieldStringVals = NULL;
    args.propFieldGuidVals = NULL;
    args.propFieldMatrixVals = NULL;
    args.propFieldVec3Vals = NULL;
    args.propListTotalGuids = 0;
    args.propListGuidData = NULL;
    args.propListOffsets = NULL;
    args.propListCounts = NULL;
    args.propEditRequested = 0;

    // Find selected entity index — from scene_graph selection (s_lbSelIdx is DLL-side,
    // but we can use the inspectorSelectedIdx or selectedEditorObj to find the gameObj index)
    //
    // IMPORTANT: levelBlockSelGuid is written by the DLL DURING DrawFrame (line ~21480),
    // which runs AFTER this property population code. So we must carry the DLL's output
    // forward from the previous frame via a static variable.
    static uint32_t s_levelBlockSelGuid = 0;
    int propEntityIdx = -1;
    if (g_levelReader.IsLoaded() && g_levelReader.GetGameObjs().size() > 0) {
        const std::vector<ZeroEngine::LevelGameObjEntry>& objs = g_levelReader.GetGameObjs();
        // Priority 1: Level Blocks / scene_graph panel selection (most explicit user action)
        if (s_levelBlockSelGuid != 0) {
            for (int i = 0; i < (int)objs.size(); ++i) {
                if (objs[i].guid == s_levelBlockSelGuid) { propEntityIdx = i; break; }
            }
        }
        // Priority 2: 3D editor object (clicked wireframe sphere)
        if (propEntityIdx < 0 && args.selectedEditorObjGuid != 0) {
            for (int i = 0; i < (int)objs.size(); ++i) {
                if (objs[i].guid == args.selectedEditorObjGuid) { propEntityIdx = i; break; }
            }
        }
        // Priority 3: selected mesh instance
        if (propEntityIdx < 0 && args.inspectorSelectedIdx >= 0 && args.inspectorSelGuid != 0) {
            for (int i = 0; i < (int)objs.size(); ++i) {
                if (objs[i].guid == args.inspectorSelGuid) { propEntityIdx = i; break; }
            }
        }
    }

    if (propEntityIdx >= 0 && g_levelReader.IsLoaded()) {
        const std::vector<ZeroEngine::LevelGameObjEntry>& objs = g_levelReader.GetGameObjs();
        const std::vector<ZeroEngine::LevelGameObjTypeDef>& types = g_levelReader.GetGameObjTypes();
        const ZeroEngine::LevelGameObjEntry& ent = objs[propEntityIdx];

        if (ent.type_def_index >= 0 && ent.type_def_index < (int)types.size()) {
            const ZeroEngine::LevelGameObjTypeDef& td = types[ent.type_def_index];
            int fc = (int)td.fields.size();

            // Only rebuild if entity changed
            if (propEntityIdx != s_propLastEntityIdx) {
                s_propLastEntityIdx = propEntityIdx;
                s_propEntityTypeStr = td.name;

                s_propFieldNameStrs.resize(fc);
                s_propFieldKindStrs.resize(fc);
                s_propFieldStringStrs.resize(fc);
                s_propFieldNamePtrs.resize(fc);
                s_propFieldKindPtrs.resize(fc);
                s_propFieldStringPtrs.resize(fc);
                s_propFieldKindCrcs.resize(fc);
                s_propFieldOffsets.resize(fc);
                s_propFieldIntVals.resize(fc, 0);
                s_propFieldFloatVals.resize(fc, 0.0f);
                s_propFieldGuidVals.resize(fc, 0);
                s_propFieldMatrixVals.resize(fc * 16, 0.0f);
                s_propFieldVec3Vals.resize(fc * 3, 0.0f);
                s_propListGuidData.clear();
                s_propListOffsets.resize(fc, 0);
                s_propListCounts.resize(fc, 0);
            }

            // Read field values from Block1 every frame (values may have been edited)
            for (int fi = 0; fi < fc; ++fi) {
                const ZeroEngine::GOFieldDefEntry& fd = td.fields[fi];
                s_propFieldNameStrs[fi] = fd.name;
                s_propFieldKindStrs[fi] = fd.kind_name;
                s_propFieldKindCrcs[fi] = fd.kind_crc;
                s_propFieldOffsets[fi]  = fd.offset;
                s_propFieldNamePtrs[fi] = s_propFieldNameStrs[fi].c_str();
                s_propFieldKindPtrs[fi] = s_propFieldKindStrs[fi].c_str();
                s_propFieldStringStrs[fi] = "";
                s_propFieldStringPtrs[fi] = "";
                s_propFieldIntVals[fi] = 0;
                s_propFieldFloatVals[fi] = 0.0f;
                s_propFieldGuidVals[fi] = 0;
                s_propListOffsets[fi] = 0;
                s_propListCounts[fi] = 0;

                uint32_t absOff = ent.block1_obj_offset + fd.offset;
                uint32_t kc = fd.kind_crc;

                if (kc == K_INT || kc == K_BOOL || kc == K_COLOR || kc == K_COLOUR) {
                    int32_t v = 0;
                    g_levelReader.GetFieldValue(absOff, &v, 4);
                    s_propFieldIntVals[fi] = v;
                }
                else if (kc == K_FLOAT) {
                    float v = 0.0f;
                    g_levelReader.GetFieldValue(absOff, &v, 4);
                    s_propFieldFloatVals[fi] = v;
                }
                else if (kc == K_CRC || kc == K_STRING) {
                    uint32_t crc = 0;
                    g_levelReader.GetFieldValue(absOff, &crc, 4);
                    s_propFieldIntVals[fi] = (int)crc;
                    s_propFieldStringStrs[fi] = g_levelReader.ResolveCrc(crc);
                    s_propFieldStringPtrs[fi] = s_propFieldStringStrs[fi].c_str();
                }
                else if (kc == K_GUID) {
                    uint32_t v = 0;
                    g_levelReader.GetFieldValue(absOff, &v, 4);
                    s_propFieldGuidVals[fi] = v;
                }
                else if (kc == K_MATRIX) {
                    if (fd.offset + 64 <= ent.block1_obj_size)
                        g_levelReader.GetFieldValue(absOff, &s_propFieldMatrixVals[fi * 16], 64);
                }
                else if (kc == K_VEC3) {
                    if (fd.offset + 12 <= ent.block1_obj_size)
                        g_levelReader.GetFieldValue(absOff, &s_propFieldVec3Vals[fi * 3], 12);
                }
                else if (kc == K_QUAT || kc == K_ROT) {
                    // Quaternion: 16 bytes, store in matrix slot (only 4 used)
                    if (fd.offset + 16 <= ent.block1_obj_size)
                        g_levelReader.GetFieldValue(absOff, &s_propFieldMatrixVals[fi * 16], 16);
                }
                else if (kc == K_OBJLIST || kc == K_NODELIST || kc == K_INTLIST) {
                    // ObjectList: 2-byte count + 2-byte relative offset, then count*4 bytes of GUIDs
                    if (fd.offset + 4 <= ent.block1_obj_size) {
                        uint16_t listNum = 0, listOff = 0;
                        g_levelReader.GetFieldValue(absOff, &listNum, 2);
                        g_levelReader.GetFieldValue(absOff + 2, &listOff, 2);
                        s_propListOffsets[fi] = (int)s_propListGuidData.size();
                        s_propListCounts[fi]  = (int)listNum;
                        if (listNum > 0 && listNum < 10000) {
                            uint32_t dataStart = ent.block1_obj_offset + fd.offset + (uint32_t)listOff + 4;
                            for (uint16_t li = 0; li < listNum; ++li) {
                                uint32_t g = 0;
                                g_levelReader.GetFieldValue(dataStart + li * 4, &g, 4);
                                s_propListGuidData.push_back(g);
                            }
                        }
                    }
                }
            }

            // Set args pointers
            args.propEntityGuid = (int)ent.guid;
            args.propEntityType = s_propEntityTypeStr.c_str();
            args.propFieldCount = fc;
            args.propFieldNames     = fc > 0 ? &s_propFieldNamePtrs[0] : NULL;
            args.propFieldKindNames = fc > 0 ? &s_propFieldKindPtrs[0] : NULL;
            args.propFieldKindCrcs  = fc > 0 ? &s_propFieldKindCrcs[0] : NULL;
            args.propFieldOffsets   = fc > 0 ? &s_propFieldOffsets[0] : NULL;
            args.propFieldIntVals   = fc > 0 ? &s_propFieldIntVals[0] : NULL;
            args.propFieldFloatVals = fc > 0 ? &s_propFieldFloatVals[0] : NULL;
            args.propFieldStringVals= fc > 0 ? &s_propFieldStringPtrs[0] : NULL;
            args.propFieldGuidVals  = fc > 0 ? &s_propFieldGuidVals[0] : NULL;
            args.propFieldMatrixVals= !s_propFieldMatrixVals.empty() ? &s_propFieldMatrixVals[0] : NULL;
            args.propFieldVec3Vals  = !s_propFieldVec3Vals.empty() ? &s_propFieldVec3Vals[0] : NULL;
            args.propListTotalGuids = (int)s_propListGuidData.size();
            args.propListGuidData   = s_propListGuidData.empty() ? NULL : &s_propListGuidData[0];
            args.propListOffsets    = fc > 0 ? &s_propListOffsets[0] : NULL;
            args.propListCounts     = fc > 0 ? &s_propListCounts[0] : NULL;
        }
    } else {
        s_propLastEntityIdx = -1;
    }

    // ── Cinematic camera data for DLL ──
    if (g_renderer && g_renderer->hasLevelScene()) {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls) {
            const std::vector<LevelCinematicCamera>& cams = ls->getCinematicCameras();
            int nc = (int)cams.size();
            if ((int)g_cineCamNameStrs.size() != nc) {
                g_cineCamNameStrs.resize(nc);
                g_cineCamNamePtrs.resize(nc);
                g_cineCamGuids.resize(nc);
                g_cineCamFovs.resize(nc);
                g_cineCamDurations.resize(nc);
                g_cineCamPosTimes.resize(nc);
                g_cineCamTgtTimes.resize(nc);
                for (int ci = 0; ci < nc; ++ci) {
                    g_cineCamNameStrs[ci] = cams[ci].name;
                    g_cineCamNamePtrs[ci] = g_cineCamNameStrs[ci].c_str();
                    g_cineCamGuids[ci] = cams[ci].guid;
                    g_cineCamFovs[ci] = cams[ci].fov;
                    g_cineCamDurations[ci] = cams[ci].totalDuration;
                    g_cineCamPosTimes[ci] = cams[ci].positionTravelTime;
                    g_cineCamTgtTimes[ci] = cams[ci].targetTravelTime;
                }
            }
            args.cineCameraCount = nc;
            args.cineCameraNames = nc > 0 ? &g_cineCamNamePtrs[0] : NULL;
            args.cineCameraGuids = nc > 0 ? &g_cineCamGuids[0] : NULL;
            args.cineCameraFovs = nc > 0 ? &g_cineCamFovs[0] : NULL;
            args.cineCameraDurations = nc > 0 ? &g_cineCamDurations[0] : NULL;
            args.cineCameraPosTimes = nc > 0 ? &g_cineCamPosTimes[0] : NULL;
            args.cineCameraTgtTimes = nc > 0 ? &g_cineCamTgtTimes[0] : NULL;
            args.cineIsPlaying = g_cineActive ? 1 : 0;
            args.cinePlaybackTime = g_cineTime;
            args.cinePlaybackDuration = g_cineDuration;

            // Populate spline data for selected camera (from previous frame's DLL output)
            static std::string s_cinePosTrackName, s_cineTgtTrackName;

            if (g_cinePanelSelCam >= 0 && g_cinePanelSelCam < nc) {
                const LevelCinematicCamera& selCam = cams[g_cinePanelSelCam];
                if (selCam.positionTrack && !selCam.positionTrack->nodes.empty()) {
                    args.cineSelPosNodeCount = (int)selCam.positionTrack->nodes.size();
                    args.cineSelPosNodes = &selCam.positionTrack->nodes[0].x;
                    args.cineSelPosTrackGuid = selCam.positionTrackGuid;
                    s_cinePosTrackName = selCam.positionTrack->name;
                    args.cineSelPosTrackName = s_cinePosTrackName.c_str();
                }
                if (selCam.targetTrack && !selCam.targetTrack->nodes.empty()) {
                    args.cineSelTgtNodeCount = (int)selCam.targetTrack->nodes.size();
                    args.cineSelTgtNodes = &selCam.targetTrack->nodes[0].x;
                    args.cineSelTgtTrackGuid = selCam.targetTrackGuid;
                    s_cineTgtTrackName = selCam.targetTrack->name;
                    args.cineSelTgtTrackName = s_cineTgtTrackName.c_str();
                } else if (selCam.targetTrackGuid != 0) {
                    // Target is an entity (Locator/Creature), not a spline
                    args.cineSelTgtTrackGuid = selCam.targetTrackGuid;
                    s_cineTgtTrackName = "(entity target)";
                    args.cineSelTgtTrackName = s_cineTgtTrackName.c_str();
                }

                // Filter is handled DLL-side (set directly during DrawFrame)
            }

            // Populate spline list for track swap dropdowns
            static std::vector<std::string> s_splineNameStrs;
            static std::vector<const char*> s_splineNamePtrs;
            static std::vector<uint32_t> s_splineGuids;
            const std::vector<LevelSpline>& splines = ls->getSplines();
            if ((int)s_splineNameStrs.size() != (int)splines.size()) {
                s_splineNameStrs.resize(splines.size());
                s_splineNamePtrs.resize(splines.size());
                s_splineGuids.resize(splines.size());
                for (size_t si = 0; si < splines.size(); ++si) {
                    s_splineNameStrs[si] = splines[si].name;
                    s_splineNamePtrs[si] = s_splineNameStrs[si].c_str();
                    s_splineGuids[si] = splines[si].guid;
                }
            }
            args.cineSplineCount = (int)splines.size();
            args.cineSplineNames = splines.empty() ? NULL : &s_splineNamePtrs[0];
            args.cineSplineGuids = splines.empty() ? NULL : &s_splineGuids[0];

        }
    }

    // Collab READ half. MUST run BEFORE DrawFrame or the DLL reads stale
    // zeros because args gets memset(0) at frame start (line ~2595). Already
    // ate this bug once. Don't eat it twice.
    //
    // Pattern for every collab arg: STATUS fields (DLL reads) -> pump here,
    // before DrawFrame. REQUEST fields (DLL writes) -> read them AFTER
    // DrawFrame in the ACTION block. Reverse the order and your UI will
    // cheerfully lie that you're solo while the session hums along fine.
    {
        ZeroEngine::CollabSession& cs = ZeroEngine::CollabSession::Instance();
        static bool s_collabInited = false;
        if (!s_collabInited) {
            cs.Init();
            // Handshake the pointer ONCE right after Init. Without this,
            // Tick() drains the inbound queue, tries to apply edits, sees
            // m_impl->levelReader is NULL, and silently drops every
            // goddamn edit the other peer sent. The session looks perfect
            // from the outside - handshake green, stream finished, status
            // CONNECTED, peer count 1 - and nothing actually fucking
            // changes on your screen when they edit. Set the pointer.
            cs.SetLevelReader(&g_levelReader);
            s_collabInited = true;
        }
        cs.Tick();
        args.collabRole         = (int)cs.GetRole();
        args.collabStatus       = (int)cs.GetStatus();
        args.collabStatusText   = cs.GetStatusText();
        args.collabPeerCount    = cs.GetPeerCount();
        args.collabPort         = cs.GetPort();
        args.collabLocalIp      = cs.GetLocalIpText();
        args.collabMyGuidPrefix = cs.GetMyGuidPrefix();
        args.collabXferProgress = cs.GetXferProgress();
        args.collabXferDoneKB   = (unsigned int)(cs.GetXferDoneBytes() / 1024);
        args.collabXferTotalKB  = (unsigned int)(cs.GetXferTotalBytes() / 1024);

        // Keep the session's PAK identity in sync with whatever's actually
        // loaded RIGHT NOW. Originally this was only done on Host/Join click,
        // which meant if you hosted an empty viewport and then loaded a level,
        // the session still thought you had fuck-all to share and clients
        // asking for a stream got bounced with "nothing loaded." Now we
        // re-pump every frame. Cheap - just a fopen/fseek/fclose and a hash
        // of a short path string. Only actually changes when you swap levels.
        if (g_levelReader.IsLoaded()) {
            const std::string& pp = g_levelReader.GetPakPath();
            uint32_t h = ZeroEngine::LotrHashString(pp.c_str());
            uint64_t sz = 0;
            FILE* fp = NULL; fopen_s(&fp, pp.c_str(), "rb");
            if (fp) { fseek(fp, 0, SEEK_END); sz = (uint64_t)_ftelli64(fp); fclose(fp); }
            cs.SetLoadedPak(pp.c_str(), h, sz);
        }

        // Streamed PAK arrived from the host. DO NOT call g_levelReader.Load
        // here directly like a fucking amateur. That's what I did the first
        // time and the asset browser stayed a goddamn graveyard - models(0),
        // textures(0), anims(0), every category empty - while the 3D view
        // rendered Helm's Deep in all its motherfucking glory. Took a
        // screenshot from the other laptop to even spot it.
        //
        // The reason: there are TWO separate rebuilds in this file, and they
        // are NOT the same thing.
        //
        //   1. The 3D scene reload (unloadLevelScene / loadLevelScene).
        //      This is what g_pendingSceneReload triggers a few hundred
        //      lines down. It rebuilds the D3D mesh data only. THAT'S WHY
        //      THE 3D VIEW LOOKS FINE.
        //
        //   2. The asset browser cache rebuild. ~80 file-scope global
        //      vectors (g_pakModelNamePtrs, g_pakTextureNamePtrs,
        //      g_pakAnimNamePtrs, g_binRawNameStrings, g_subBlock1NamePtrs,
        //      g_goNamePtrs, g_goTypeDefNamePtrs, every fucking thing the
        //      left panel and Level Blocks read from) are .clear()'d and
        //      .push_back()'d in a 400-line block gated ENTIRELY on
        //      args.requestLoadLevel being set. THAT'S WHY THE LEFT PANEL
        //      IS A SHRINE TO ZERO when you skip this path.
        //
        // The clean fix is not "extract 400 lines into a helper" because the
        // helper would need 80 reference parameters or it'd be a free function
        // groping at file-scope globals - either way, a refactor that buys us
        // nothing at runtime and a thousand merge conflicts at design time.
        //
        // Instead: spoof the user click. Set requestLoadLevel = 1, copy the
        // streamed paths into the request buffers, and let the existing
        // handler at line ~4956 do its job exactly like the user just hit
        // "Load Level" with the streamed PAK selected. Both rebuilds run.
        // Asset tree fills. Everyone is happy.
        //
        // The DLL only WRITES requestLoadLevel = 1 on click - it never reads
        // it and never clears it. C++ owns the lifecycle (defaulted to 0
        // every frame at line ~3468, BEFORE this block runs). So setting it
        // here, BEFORE DrawFrame, survives DrawFrame untouched and gets
        // consumed by the handler well after. Verified in imgui_glue_dll.cpp
        // at the Load Level button - that is the only writer.
        //
        // If you ever add ANOTHER non-button code path that loads a level
        // (drag-drop, recent-files menu, command line arg, whatever): do
        // this same spoof. Don't reinvent the path. Don't be a hero.
        std::string rxPak, rxBin;
        if (cs.ConsumePakReady(rxPak, rxBin)) {
            DebugLog(("Collab: host's PAK finally crawled down the motherfucking wire - spoofing Load Level click for " + rxPak + ". If the asset tree stays at (0) after this, g_pendingSceneReload is the flag that's not getting set.").c_str());
            args.requestLoadLevel = 1;
            strncpy_s(args.requestedLevelPakPath, sizeof(args.requestedLevelPakPath), rxPak.c_str(), _TRUNCATE);
            strncpy_s(args.requestedLevelBinPath, sizeof(args.requestedLevelBinPath), rxBin.c_str(), _TRUNCATE);
        }
    }

    // ── 3dCrowd archetype enumeration ────────────────────────────────
    //    For the right-click "Place Crowd Character Here" submenu. Host
    //    owns the string storage (static vectors) — DLL only reads. Built
    //    fresh each frame so it tracks live edits (addCrowdItem grows the
    //    list mid-session).
    {
        static std::vector<std::string> s_archStrings;
        static std::vector<const char*> s_archPtrs;
        s_archStrings.clear();
        s_archPtrs.clear();
        LevelScene* lsA = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;
        if (lsA) {
            const std::vector<LevelCrowdItem>& items = lsA->getCrowdItems();
            for (size_t i = 0; i < items.size(); ++i) {
                const std::string& nm = items[i].modelKeyName;
                s_archStrings.push_back(nm.empty() ? std::string("(unresolved CRC)") : nm);
            }
            for (size_t i = 0; i < s_archStrings.size(); ++i)
                s_archPtrs.push_back(s_archStrings[i].c_str());
        }
        args.crowdArchetypeCount = (int)s_archPtrs.size();
        args.crowdArchetypeNames = s_archPtrs.empty() ? NULL : &s_archPtrs[0];
    }

    // ── 3dCrowd inspector: populate args from LevelScene if a crowd
    //    LevelInstance is currently selected. The DLL panel reads these
    //    fields to render the archetype info + LOD slider + Delete/Clone
    //    buttons. Default-zeroed by memset(&args, 0, ...) at top of frame,
    //    so the panel naturally hides when selInstIsCrowd == 0.
    {
        LevelScene* lsC = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;
        if (lsC) {
            int iSel = lsC->selectedInstance();
            const LevelInstance* inst = (iSel >= 0) ? lsC->getInstance(iSel) : NULL;
            if (inst && inst->crowdItemIdx >= 0 && inst->crowdValIdx >= 0) {
                const std::vector<LevelCrowdItem>& items = lsC->getCrowdItems();
                int ii = inst->crowdItemIdx;
                int vv = inst->crowdValIdx;
                if (ii >= 0 && ii < (int)items.size()
                    && vv >= 0 && vv < (int)items[ii].instances.size())
                {
                    const LevelCrowdItem& cit = items[ii];
                    const LevelCrowdInstance& cv = cit.instances[vv];
                    args.selInstIsCrowd       = 1;
                    args.selCrowdItemIdx      = ii;
                    args.selCrowdValIdx       = vv;
                    args.selCrowdAnimCount    = (int)cit.animationKeys.size();
                    args.selCrowdInstCount    = (int)cit.instances.size();
                    args.selCrowdPlaybackRate = cit.playbackRate;
                    args.selCrowdLod          = cv.lod;
                    args.selCrowdPosX         = cv.position[0];
                    args.selCrowdPosY         = cv.position[1];
                    args.selCrowdPosZ         = cv.position[2];
                    args.selCrowdRotation     = cv.rotation;
                    strncpy_s(args.selCrowdArchetype, sizeof(args.selCrowdArchetype),
                              cit.modelKeyName.c_str(), _TRUNCATE);
                    strncpy_s(args.selCrowdMeshName, sizeof(args.selCrowdMeshName),
                              cit.meshKeyName.c_str(), _TRUNCATE);
                    strncpy_s(args.selCrowdRightHand, sizeof(args.selCrowdRightHand),
                              cit.rightHandName.c_str(), _TRUNCATE);
                    strncpy_s(args.selCrowdLeftHand, sizeof(args.selCrowdLeftHand),
                              cit.leftHandName.c_str(), _TRUNCATE);
                }
            }
        }
    }

    // ── 3dCrowd Editor: populate panel display fields ───────────────
    // List every CrowdItem (key + mesh name + inst count), copy the
    // header of whatever item the DLL has selected, and build three
    // autocomplete lists: mesh-suitable models, WP_-prefixed weapon
    // models, and the union of all loaded crowd anim names. The DLL
    // panel reads these whenever crowdEditorOpen is non-zero.
    //
    // Selected-item state lives on the host via the file-scope static
    // g_crowdEditorSelectedItem (declared near the top of this file).
    // args gets memset'd each frame, so we seed args from the static
    // here, and the post-frame consume block writes any DLL-driven
    // change back into the static.
    //
    // Auto-sync from viewport: if the user has a crowd LevelInstance
    // currently selected (Inspector's selInstIsCrowd path populated
    // args.selCrowdItemIdx earlier in this same frame), promote that
    // to the Editor's selection. Viewport-click wins over panel-click
    // so clicking on a crowd member in 3D jumps the Editor's list to
    // that item. Also pulls the instance index in so v1.4's per-instance
    // edit pane focuses the picked one.
    if (args.selInstIsCrowd && args.selCrowdItemIdx >= 0) {
        g_crowdEditorSelectedItem = args.selCrowdItemIdx;
        if (args.selCrowdValIdx >= 0) {
            g_crowdEditorSelInst = args.selCrowdValIdx;
        }
    }
    args.crowdEditorSelectedItem = g_crowdEditorSelectedItem;
    args.crowdEditorSelInst      = g_crowdEditorSelInst;
    {
        LevelScene* lsE = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;
        if (lsE) {
            const std::vector<LevelCrowdItem>& items = lsE->getCrowdItems();
            int n = (int)items.size(); if (n > 64) n = 64;
            args.crowdEditorItemCount = n;
            for (int i = 0; i < n; ++i) {
                strncpy_s(args.crowdEditorItemKeys[i], 96,
                          items[i].meshKeyName.c_str(), _TRUNCATE);
                strncpy_s(args.crowdEditorItemMeshNames[i], 96,
                          items[i].modelKeyName.c_str(), _TRUNCATE);
                args.crowdEditorItemInstCounts[i] = (int)items[i].instances.size();
            }
            // Mirror live focus state from LevelScene back to args so the
            // DLL checkbox reflects reality after toggles + reloads.
            args.crowdEditorFocusModeOn = lsE->getCrowdFocusMode() ? 1 : 0;
            args.crowdEditorFocusedItem = lsE->getCrowdFocusedItem();

            // Selected item header
            int sel = args.crowdEditorSelectedItem;
            if (sel >= 0 && sel < n) {
                const LevelCrowdItem& it = items[sel];
                strncpy_s(args.crowdEditorSelKey,      96, it.meshKeyName.c_str(),   _TRUNCATE);
                strncpy_s(args.crowdEditorSelKeyMain,  96, it.modelKeyName.c_str(),  _TRUNCATE);
                strncpy_s(args.crowdEditorSelKeyRight, 96, it.rightHandName.c_str(), _TRUNCATE);
                strncpy_s(args.crowdEditorSelKeyLeft,  96, it.leftHandName.c_str(),  _TRUNCATE);
                args.crowdEditorSelPlaybackRate = it.playbackRate;
                int an = (int)it.animationNames.size(); if (an > 64) an = 64;
                args.crowdEditorSelAnimCount = an;
                for (int ai = 0; ai < an; ++ai) {
                    strncpy_s(args.crowdEditorSelAnimNames[ai], 96,
                              it.animationNames[ai].c_str(), _TRUNCATE);
                }
                args.crowdEditorSelInstCount = (int)it.instances.size();
                // v1.4: per-instance focus pose
                int vi = args.crowdEditorSelInst;
                if (vi >= 0 && vi < (int)it.instances.size()) {
                    const LevelCrowdInstance& cv = it.instances[vi];
                    args.crowdEditorSelInstPos[0] = cv.position[0];
                    args.crowdEditorSelInstPos[1] = cv.position[1];
                    args.crowdEditorSelInstPos[2] = cv.position[2];
                    args.crowdEditorSelInstRot    = cv.rotation;
                    args.crowdEditorSelInstLod    = cv.lod;
                }
            } else {
                args.crowdEditorSelKey[0] = 0;
                args.crowdEditorSelKeyMain[0] = 0;
                args.crowdEditorSelKeyRight[0] = 0;
                args.crowdEditorSelKeyLeft[0] = 0;
                args.crowdEditorSelPlaybackRate = 0.0f;
                args.crowdEditorSelAnimCount = 0;
                args.crowdEditorSelInstCount = 0;
            }

            // Autocomplete sources — pulled from m_crcMap (every name
            // the level knows about), NOT m_modelCache (only loaded
            // models). The cache only contains models referenced by
            // existing CrowdItems / instances; newly-baked Crowd Mesh
            // Builder outputs (e.g. crd_ch_haldir_TestShield) won't be
            // in the cache until something loads them — which can't
            // happen until the user picks them from THIS dropdown.
            // Chicken-and-egg fix: source from m_crcMap so the user can
            // pick anything they baked. setCrowdItemModelKey already
            // calls loadModelFromBinary on the pick to populate cache.
            static const char* kTexSkip[] = {
                "_D", "_N", "_S", "_GV", "_AO", "_E", NULL
            };
            std::vector<std::string> meshOpts;
            g_levelReader.GetStringsByPrefix("CRD_", meshOpts, kTexSkip);
            g_levelReader.GetStringsByPrefix("CH_",  meshOpts, kTexSkip);
            g_levelReader.GetStringsByPrefix("HMD_", meshOpts, kTexSkip);
            g_levelReader.GetStringsByPrefix("PR_",  meshOpts, kTexSkip);
            g_levelReader.GetStringsByPrefix("IGD_", meshOpts, kTexSkip);
            int mn = (int)meshOpts.size(); if (mn > 512) mn = 512;
            args.crowdEditorMeshOptionCount = mn;
            for (int i = 0; i < mn; ++i) {
                strncpy_s(args.crowdEditorMeshOptions[i], 96,
                          meshOpts[i].c_str(), _TRUNCATE);
            }

            std::vector<std::string> wpOpts;
            g_levelReader.GetStringsByPrefix("WP_", wpOpts, kTexSkip);
            g_levelReader.GetStringsByPrefix("PR_", wpOpts, kTexSkip);
            int wn = (int)wpOpts.size(); if (wn > 256) wn = 256;
            args.crowdEditorWeaponOptionCount = wn;
            for (int i = 0; i < wn; ++i) {
                strncpy_s(args.crowdEditorWeaponOptions[i], 96,
                          wpOpts[i].c_str(), _TRUNCATE);
            }

            // Animations: the level's BIN has a flat list of every anim
            // entry — exposed via LevelReader::GetAnimations(). Sort
            // them so SKELETON-COMPATIBLE ones (per the selected item's
            // mesh name) come FIRST in the list, with the count returned
            // in crowdEditorAnimCompatCount. DLL renders the first N in
            // a highlighted color so the user can spot them at a glance
            // but still has the full list available for manual picks.
            //
            // Compatibility heuristic: extract "distinctive" tokens from
            // the mesh name (drop generic prefixes like CRD_, CH_, mesh,
            // 01, etc.), then an anim is compatible if its lower-cased
            // name contains ANY surviving token. For
            // CRD_CH_urk_spr_all_01_mesh the only distinctive token is
            // 'spr', which matches RH6_Spr_Idle. For CH_elf_Haldir_01
            // 'haldir' matches RH6_Haldir_Wave. Imperfect but useful;
            // proper bone-set comparison needs skeleton metadata we
            // don't track yet.
            std::vector<std::string> skelTags;
            {
                // Build the ignore-token set once per session. These
                // are the generic prefixes/suffixes/numbers that show
                // up in every other mesh name and aren't useful for
                // disambiguating skeletons.
                static const char* kIgnore[] = {
                    "crd", "ch", "wp", "pr", "igd", "hmd", "gdr", "roh",
                    "hum", "orc", "fed", "osg", "plf", "wtp", "all",
                    "mesh", "model", NULL
                };
                int selForAnim = args.crowdEditorSelectedItem;
                if (selForAnim >= 0 && selForAnim < (int)items.size()) {
                    const std::string& mname = items[selForAnim].modelKeyName;
                    // Tokenize on underscore.
                    std::string cur;
                    for (size_t ci = 0; ci <= mname.size(); ++ci) {
                        char c = (ci < mname.size()) ? mname[ci] : '_';
                        if (c == '_' || c == '\0') {
                            if (cur.size() > 1) {
                                // Lowercase the token.
                                std::string lower = cur;
                                for (size_t li = 0; li < lower.size(); ++li) {
                                    char& lc = lower[li];
                                    if (lc >= 'A' && lc <= 'Z') lc += 32;
                                }
                                // Skip pure-digit tokens (01, 02, etc.)
                                bool allDigit = true;
                                for (size_t di = 0; di < lower.size(); ++di) {
                                    if (lower[di] < '0' || lower[di] > '9') {
                                        allDigit = false; break;
                                    }
                                }
                                // Skip ignore-list tokens.
                                bool ignored = allDigit;
                                if (!ignored) {
                                    for (int ii = 0; kIgnore[ii] && !ignored; ++ii) {
                                        if (lower == kIgnore[ii]) ignored = true;
                                    }
                                }
                                if (!ignored) skelTags.push_back(lower);
                            }
                            cur.clear();
                        } else {
                            cur.push_back(c);
                        }
                    }
                }
            }

            const std::vector<ZeroEngine::LevelAnimEntry>& levelAnims =
                g_levelReader.GetAnimations();

            // Two-pass write: compat anims first, then incompat. Counts
            // up to 256 total. Each anim's compat test: lower-case the
            // name, check if any tag is a substring.
            int compatN = 0;
            int totalN  = 0;
            if (!skelTags.empty()) {
                for (size_t i = 0; i < levelAnims.size() && totalN < 256; ++i) {
                    const std::string& n = levelAnims[i].name;
                    if (n.empty()) continue;
                    std::string lower = n;
                    for (size_t li = 0; li < lower.size(); ++li) {
                        char& lc = lower[li];
                        if (lc >= 'A' && lc <= 'Z') lc += 32;
                    }
                    bool match = false;
                    for (size_t ti = 0; ti < skelTags.size(); ++ti) {
                        if (lower.find(skelTags[ti]) != std::string::npos) {
                            match = true; break;
                        }
                    }
                    if (match) {
                        strncpy_s(args.crowdEditorAnimOptions[totalN], 96,
                                  n.c_str(), _TRUNCATE);
                        totalN++;
                        compatN++;
                    }
                }
            }
            // Second pass: non-compat anims fill the remainder, up to 256.
            for (size_t i = 0; i < levelAnims.size() && totalN < 256; ++i) {
                const std::string& n = levelAnims[i].name;
                if (n.empty()) continue;
                // Skip ones we already wrote in the compat pass.
                bool alreadyAdded = false;
                if (!skelTags.empty()) {
                    std::string lower = n;
                    for (size_t li = 0; li < lower.size(); ++li) {
                        char& lc = lower[li];
                        if (lc >= 'A' && lc <= 'Z') lc += 32;
                    }
                    for (size_t ti = 0; ti < skelTags.size() && !alreadyAdded; ++ti) {
                        if (lower.find(skelTags[ti]) != std::string::npos) alreadyAdded = true;
                    }
                }
                if (alreadyAdded) continue;
                strncpy_s(args.crowdEditorAnimOptions[totalN], 96,
                          n.c_str(), _TRUNCATE);
                totalN++;
            }
            args.crowdEditorAnimOptionCount = totalN;
            args.crowdEditorAnimCompatCount = skelTags.empty() ? -1 : compatN;
        }
    }

    // ── Crowd Mesh Builder: populate panel display fields ────────────
    // Enumerate available base meshes (CH_, CRD_) and weapons (WP_, PR_),
    // plus the bone list of whatever base mesh is currently selected.
    // The lifecycle around preview LevelInstances (spawn on first valid
    // pick, respawn on selection change, clear on panel close) is owned
    // by the post-frame consume block — this side is pure read-out.
    {
        LevelScene* lsB = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;
        if (lsB) {
            // Mesh lists — pulled from m_crcMap so we see EVERY level
            // asset name, not just the ones currently loaded into the
            // model cache. Case-insensitive prefix match. Texture
            // suffixes (_D/_N/_S/_GV) are filtered so the dropdown only
            // shows actual mesh names, not their material variants.
            //
            // Bases also include both 'CRD_' (sprite crowd meshes —
            // ideal for crowd use) and 'CH_' (hero rigs — need the
            // builder's bake to attach weapons properly). Weapons cover
            // 'WP_' plus 'PR_' since some hand-held items (banners,
            // pikes) live under PR_ in this level.
            static const char* kTexSkip[] = {
                "_D", "_N", "_S", "_GV", "_AO", "_E", NULL
            };
            std::vector<std::string> baseOpts;
            g_levelReader.GetStringsByPrefix("CH_",  baseOpts, kTexSkip);
            g_levelReader.GetStringsByPrefix("CRD_", baseOpts, kTexSkip);
            // Sort alphabetically. m_crcMap iterates in hash order which
            // looks random to the user. With hundreds of CH_/CRD_ names
            // the dropdown was unusable for finding a specific one like
            // CRD_CH_elf_ancn_bowtest_01 — they'd scroll past it without
            // realising. Alphabetical sort makes the whole "bowtest" /
            // "elf_ancn" cluster visible together.
            std::sort(baseOpts.begin(), baseOpts.end());
            int bn = (int)baseOpts.size(); if (bn > 512) bn = 512;
            args.crowdBuilderBaseMeshOptionCount = bn;
            for (int i = 0; i < bn; ++i) {
                strncpy_s(args.crowdBuilderBaseMeshOptions[i], 96,
                          baseOpts[i].c_str(), _TRUNCATE);
            }
            std::vector<std::string> wpOpts;
            g_levelReader.GetStringsByPrefix("WP_", wpOpts, kTexSkip);
            g_levelReader.GetStringsByPrefix("PR_", wpOpts, kTexSkip);
            int wn = (int)wpOpts.size(); if (wn > 256) wn = 256;
            args.crowdBuilderWeaponMeshOptionCount = wn;
            for (int i = 0; i < wn; ++i) {
                strncpy_s(args.crowdBuilderWeaponMeshOptions[i], 96,
                          wpOpts[i].c_str(), _TRUNCATE);
            }
            // Seed args from the file-scope statics — args got memset
            // a few thousand lines ago, so without this the dropdowns
            // would snap back to -1 every frame.
            args.crowdBuilderSelBaseIdx   = g_crowdBuilderSelBaseIdx;
            args.crowdBuilderSelWeaponIdx = g_crowdBuilderSelWeaponIdx;
            args.crowdBuilderSelBoneIdx   = g_crowdBuilderSelBoneIdx;
            strncpy_s(args.crowdBuilderSelBaseName,   96, g_crowdBuilderSelBaseName,   _TRUNCATE);
            strncpy_s(args.crowdBuilderSelWeaponName, 96, g_crowdBuilderSelWeaponName, _TRUNCATE);
            strncpy_s(args.crowdBuilderSelBoneName,   64, g_crowdBuilderSelBoneName,   _TRUNCATE);
            args.crowdBuilderOffsetXYZ[0] = g_crowdBuilderOffsetXYZ[0];
            args.crowdBuilderOffsetXYZ[1] = g_crowdBuilderOffsetXYZ[1];
            args.crowdBuilderOffsetXYZ[2] = g_crowdBuilderOffsetXYZ[2];
            args.crowdBuilderOffsetYawPitchRoll[0] = g_crowdBuilderOffsetYPR[0];
            args.crowdBuilderOffsetYawPitchRoll[1] = g_crowdBuilderOffsetYPR[1];
            args.crowdBuilderOffsetYawPitchRoll[2] = g_crowdBuilderOffsetYPR[2];
            strncpy_s(args.crowdBuilderNewMeshName, 96, g_crowdBuilderNewMeshName, _TRUNCATE);
            // Embed mode + UV target rect. Same memset-survives pattern
            // as everything else here.
            args.crowdBuilderEmbedMode = g_crowdBuilderEmbedMode;
            for (int i = 0; i < 4; ++i)
                args.crowdBuilderEmbedUVRect[i] = g_crowdBuilderEmbedUVRect[i];

            // ── AUTO-SPAWN PAYLOAD: seed the spawn pos to wherever the
            // user's looking so the default "where does the mesh go"
            // answer is "RIGHT FUCKING HERE, ASSHOLE" instead of "uh,
            // world origin, good luck finding it on a 2km terrain".
            // Seeding is one-shot. After the first frame the user owns
            // the value through the panel sliders. Don't keep snapping
            // their drag to the camera or they'll throw the monitor.
            args.crowdBuilderAutoSpawn  = g_crowdBuilderAutoSpawn;
            args.crowdBuilderSpawnCount = g_crowdBuilderSpawnCount;
            if (!g_crowdBuilderSpawnPosSeeded && g_renderer) {
                hkVector4 camTgt = g_renderer->getCameraTarget();
                g_crowdBuilderSpawnPos[0] = camTgt(0);
                g_crowdBuilderSpawnPos[1] = camTgt(1);
                g_crowdBuilderSpawnPos[2] = camTgt(2);
                g_crowdBuilderSpawnPosSeeded = 1;
            }
            args.crowdBuilderSpawnPos[0] = g_crowdBuilderSpawnPos[0];
            args.crowdBuilderSpawnPos[1] = g_crowdBuilderSpawnPos[1];
            args.crowdBuilderSpawnPos[2] = g_crowdBuilderSpawnPos[2];

            // ── BASE ATLAS POINTER: stop making the user type four
            // float coordinates blind like a goddamn assembly programmer
            // in 1987. We hand the panel the same D3D9 texture pointer
            // Mesh Surgery already shows, the panel draws it inline,
            // and the user click-drags a rectangle on the actual atlas
            // pixels. ensureModelLoaded forces a load on demand because
            // half the CRD picker entries aren't instanced on the current
            // map (cold cache). NULL falls through cleanly to a fallback
            // text. No crash if the user picks something exotic that
            // refuses to load.
            args.crowdBuilderBaseAtlasTexture = NULL;
            args.crowdBuilderBaseAtlasW = 0;
            args.crowdBuilderBaseAtlasH = 0;
            args.crowdBuilderBaseAtlasTextureName[0] = 0;
            if (args.crowdBuilderSelBaseIdx >= 0
             && args.crowdBuilderSelBaseIdx < bn) {
                std::string bName = baseOpts[args.crowdBuilderSelBaseIdx];
                uint32_t bCrc = ZeroEngine::LotrHashString(bName.c_str(), -1);
                LevelModel* bMdl = lsB->ensureModelLoaded(g_levelReader, bCrc, bName);
                if (bMdl && !bMdl->parts.empty() && bMdl->parts[0].diffuseTex) {
                    IDirect3DTexture9* btex = bMdl->parts[0].diffuseTex;
                    args.crowdBuilderBaseAtlasTexture = (void*)btex;
                    D3DSURFACE_DESC desc;
                    if (SUCCEEDED(btex->GetLevelDesc(0, &desc))) {
                        args.crowdBuilderBaseAtlasW = (int)desc.Width;
                        args.crowdBuilderBaseAtlasH = (int)desc.Height;
                    }
                    strncpy_s(args.crowdBuilderBaseAtlasTextureName, 96,
                              bName.c_str(), _TRUNCATE);
                }
            }

            // Bone list — populated from whichever base mesh is currently
            // picked. Reverse-resolve the name to a CRC because the model
            // cache is keyed by CRC, not name. If the model isn't cached
            // yet (rare — every loaded level entity already triggered a
            // load) the bone count just stays at 0 and the dropdown is
            // empty. Same for non-skeletal models (terrain, props): no
            // bones means an empty dropdown, by design.
            int bsel = args.crowdBuilderSelBaseIdx;
            if (bsel >= 0 && bsel < bn) {
                std::string baseName = baseOpts[bsel];
                uint32_t baseCrc = ZeroEngine::LotrHashString(baseName.c_str(), -1);
                // Force-load before reading bones. Picker lists every CH_/CRD_
                // name in the string table; many won't have a live instance on
                // the current map. Without this the bone dropdown stays empty
                // for any model the user picks that isn't already drawn.
                lsB->ensureModelLoaded(g_levelReader, baseCrc, baseName);
                std::vector<std::string> boneNames;
                if (lsB->getModelBoneNames(baseCrc, boneNames)) {
                    int boneN = (int)boneNames.size();
                    if (boneN > 128) boneN = 128;
                    args.crowdBuilderBoneOptionCount = boneN;
                    for (int i = 0; i < boneN; ++i) {
                        strncpy_s(args.crowdBuilderBoneOptions[i], 64,
                                  boneNames[i].c_str(), _TRUNCATE);
                    }

                    // Smart-default attach bone: when the user picks (or
                    // changes) the weapon, scan the weapon's own bones[]
                    // for any name that ALSO exists in this base mesh's
                    // bones[]. Weapon prefabs ship with the attach-point
                    // bones they were authored against (e.g. Bone_Lumbar3
                    // for back-sheathed bow, Bone_RHand_attach for swords).
                    // Auto-select the first match so the weapon snaps to a
                    // sensible attach point without the user manually
                    // hunting in the bone dropdown. Reset offsets to zero
                    // since the new pick has its own natural alignment.
                    // Only runs on weapon-change, so manual bone tweaks
                    // after the auto-pick are preserved.
                    if (args.crowdBuilderSelWeaponIdx >= 0
                     && args.crowdBuilderSelWeaponIdx < wn
                     && args.crowdBuilderSelWeaponIdx != g_crowdBuilderAutoPickedForWpIdx)
                    {
                        std::string wpName = wpOpts[args.crowdBuilderSelWeaponIdx];
                        uint32_t wpCrc = ZeroEngine::LotrHashString(wpName.c_str(), -1);
                        // Same lazy-load story as the base mesh above.
                        lsB->ensureModelLoaded(g_levelReader, wpCrc, wpName);
                        std::vector<std::string> wpBoneNames;
                        if (lsB->getModelBoneNames(wpCrc, wpBoneNames)) {
                            // Build a set of base bone names for fast lookup
                            // (boneN is small, but std::find is still O(N)
                            // per query so we'd do O(W*B) without a set).
                            std::set<std::string> baseBoneSet;
                            for (int i = 0; i < boneN; ++i)
                                baseBoneSet.insert(boneNames[i]);
                            int matchedIdx = -1;
                            for (size_t wi = 0; wi < wpBoneNames.size(); ++wi) {
                                const std::string& wn_ = wpBoneNames[wi];
                                if (wn_.empty()) continue;
                                // Skip the weapon's own root bone (named
                                // after the weapon itself) — it never
                                // collides with a character bone.
                                if (wn_ == wpName) continue;
                                if (baseBoneSet.count(wn_)) {
                                    // Find this bone's index in the base
                                    // bone list (the dropdown's index).
                                    for (int bi = 0; bi < boneN; ++bi) {
                                        if (boneNames[bi] == wn_) {
                                            matchedIdx = bi;
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                            if (matchedIdx >= 0) {
                                args.crowdBuilderSelBoneIdx = matchedIdx;
                                strncpy_s(args.crowdBuilderSelBoneName, 64,
                                          boneNames[matchedIdx].c_str(), _TRUNCATE);
                                g_crowdBuilderSelBoneIdx = matchedIdx;
                                strncpy_s(g_crowdBuilderSelBoneName, 64,
                                          boneNames[matchedIdx].c_str(), _TRUNCATE);
                                // Reset offsets — let the user adjust from
                                // a clean slate for the new weapon.
                                args.crowdBuilderOffsetXYZ[0] = 0.0f;
                                args.crowdBuilderOffsetXYZ[1] = 0.0f;
                                args.crowdBuilderOffsetXYZ[2] = 0.0f;
                                args.crowdBuilderOffsetYawPitchRoll[0] = 0.0f;
                                args.crowdBuilderOffsetYawPitchRoll[1] = 0.0f;
                                args.crowdBuilderOffsetYawPitchRoll[2] = 0.0f;
                                g_crowdBuilderOffsetXYZ[0] = 0.0f;
                                g_crowdBuilderOffsetXYZ[1] = 0.0f;
                                g_crowdBuilderOffsetXYZ[2] = 0.0f;
                                g_crowdBuilderOffsetYPR[0] = 0.0f;
                                g_crowdBuilderOffsetYPR[1] = 0.0f;
                                g_crowdBuilderOffsetYPR[2] = 0.0f;
                            }
                        }
                        g_crowdBuilderAutoPickedForWpIdx = args.crowdBuilderSelWeaponIdx;
                    }
                } else {
                    args.crowdBuilderBoneOptionCount = 0;
                }
            } else {
                args.crowdBuilderBoneOptionCount = 0;
            }

            // ── Strip Baked Weapon: populate source list (CRD_ only) ───
            // and the bone list of the currently picked source.
            {
                static const char* kTexSkip2[] = {
                    "_D", "_N", "_S", "_GV", "_AO", "_E", NULL
                };
                std::vector<std::string> crdOpts;
                g_levelReader.GetStringsByPrefix("CRD_", crdOpts, kTexSkip2);
                int cn = (int)crdOpts.size(); if (cn > 256) cn = 256;
                args.stripSourceCrdOptionCount = cn;
                for (int i = 0; i < cn; ++i) {
                    strncpy_s(args.stripSourceCrdOptions[i], 96,
                              crdOpts[i].c_str(), _TRUNCATE);
                }
                // Seed from persistent statics.
                args.stripSourceSelIdx = g_stripSourceSelIdx;
                strncpy_s(args.stripSourceSelName, 96, g_stripSourceSelName, _TRUNCATE);
                strncpy_s(args.stripNewName,       96, g_stripNewName,       _TRUNCATE);
                for (int b = 0; b < 128; ++b) {
                    args.stripBoneSelected[b] = g_stripBoneSelected[b];
                }

                // Populate strip-bone dropdown from the selected source's bones.
                int ssel = args.stripSourceSelIdx;
                if (ssel >= 0 && ssel < cn) {
                    std::string srcName = crdOpts[ssel];
                    uint32_t srcCrc = ZeroEngine::LotrHashString(srcName.c_str(), -1);
                    std::vector<std::string> stripBones;
                    if (lsB->getModelBoneNames(srcCrc, stripBones)) {
                        int stripN = (int)stripBones.size();
                        if (stripN > 128) stripN = 128;
                        args.stripBoneOptionCount = stripN;
                        for (int i = 0; i < stripN; ++i) {
                            strncpy_s(args.stripBoneOptions[i], 64,
                                      stripBones[i].c_str(), _TRUNCATE);
                        }
                    } else {
                        args.stripBoneOptionCount = 0;
                    }
                } else {
                    args.stripBoneOptionCount = 0;
                }
            }

            // ── Mesh Surgery: populate source list (CRD_ + CH_) and seed
            // args from persistent statics so UI state survives the memset.
            {
                static const char* kTexSkip3[] = {
                    "_D", "_N", "_S", "_GV", "_AO", "_E", NULL
                };
                std::vector<std::string> msOpts;
                g_levelReader.GetStringsByPrefix("CRD_", msOpts, kTexSkip3);
                g_levelReader.GetStringsByPrefix("CH_",  msOpts, kTexSkip3);
                int mn = (int)msOpts.size(); if (mn > 512) mn = 512;
                args.meshSurgerySourceOptionCount = mn;
                for (int i = 0; i < mn; ++i) {
                    strncpy_s(args.meshSurgerySourceOptions[i], 96,
                              msOpts[i].c_str(), _TRUNCATE);
                }
                args.meshSurgerySelSourceIdx = g_meshSurgerySelSourceIdx;
                strncpy_s(args.meshSurgerySelSourceName, 96,
                          g_meshSurgerySelSourceName, _TRUNCATE);
                strncpy_s(args.meshSurgeryNewName, 96,
                          g_meshSurgeryNewName, _TRUNCATE);
                strncpy_s(args.meshSurgeryStatusText, 256,
                          g_meshSurgeryStatusText, _TRUNCATE);
                for (int i = 0; i < 4; ++i) {
                    args.meshSurgerySelectionUVRect[i] = g_meshSurgerySelectionUVRect[i];
                }
                args.meshSurgerySelectedVertCount = g_meshSurgerySelectedVertCount;

                // Resolve atlas texture for the picked source. LevelModel's
                // parts[0] carries the diffuseTex from mat[0]'s tex0 — that's
                // the body atlas for CRD models. Query D3D9 surface desc
                // for width/height so the panel can label and lay out the
                // image properly.
                args.meshSurgeryAtlasTexture = NULL;
                args.meshSurgeryAtlasW = 0;
                args.meshSurgeryAtlasH = 0;
                args.meshSurgeryAtlasTextureName[0] = 0;
                if (args.meshSurgerySelSourceIdx >= 0
                 && args.meshSurgerySelSourceIdx < mn) {
                    std::string msName = msOpts[args.meshSurgerySelSourceIdx];
                    uint32_t msCrc = ZeroEngine::LotrHashString(msName.c_str(), -1);
                    // Force-load: the user picked a name from the string
                    // table, not from the live instance list, so the model
                    // may not be cached yet. ensureModelLoaded covers the
                    // cold-cache case the way the Crowd Editor's mesh
                    // swap does. Without this the panel reports "no UV
                    // and tex found" for any CRD the current map doesn't
                    // place (e.g. picking an elf CRD on Helm's Deep).
                    LevelModel* mdl = lsB->ensureModelLoaded(g_levelReader, msCrc, msName);
                    if (mdl && !mdl->parts.empty() && mdl->parts[0].diffuseTex) {
                        IDirect3DTexture9* tex = mdl->parts[0].diffuseTex;
                        args.meshSurgeryAtlasTexture = (void*)tex;
                        D3DSURFACE_DESC desc;
                        if (SUCCEEDED(tex->GetLevelDesc(0, &desc))) {
                            args.meshSurgeryAtlasW = (int)desc.Width;
                            args.meshSurgeryAtlasH = (int)desc.Height;
                        }
                        strncpy_s(args.meshSurgeryAtlasTextureName, 96,
                                  msName.c_str(), _TRUNCATE);
                    } else {
                        // Three failure modes, three confessions. Better the
                        // log screams which one tripped us than the user has
                        // to gdb-step through ensureModelLoaded at some ungodly
                        // hour while their soul slowly dies, wondering which
                        // fucking layer of this dead pipeline ate their pick.
                        if (!mdl) {
                            DebugLogf("[MeshSurgery] ensureModelLoaded('%s', crc=0x%08X) "
                                      "returned NULL. The BIN has no fucking entry for "
                                      "this name - either the PAK never packed it (string "
                                      "table lists it but no asset bytes exist) or some "
                                      "earlier load already failed and we cached the NULL.",
                                      msName.c_str(), msCrc);
                        } else if (mdl->parts.empty()) {
                            DebugLogf("[MeshSurgery] '%s' loaded but parts[] is goddamn "
                                      "empty. Model decoded, no draw call slots came out. "
                                      "Either the mesh has zero buffer_infos (broken on "
                                      "import) or every slot got filtered by the visibility "
                                      "mask. Either way, can't surgery what we can't draw.",
                                      msName.c_str());
                        } else if (!mdl->parts[0].diffuseTex) {
                            DebugLogf("[MeshSurgery] '%s' loaded with %d parts but "
                                      "parts[0].diffuseTex is NULL. mat[0].tex0 CRC didn't "
                                      "resolve to a real texture in the BIN. EA's pipeline "
                                      "tools occasionally shipped these phantom material "
                                      "refs and the PC port never caught them. Surgery off, "
                                      "user staring at a void.",
                                      msName.c_str(), (int)mdl->parts.size());
                        }
                    }
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────
    //  ADJUST MESH BY BONE — pre-frame: populate dropdowns + seed sliders
    // ─────────────────────────────────────────────────────────────────
    // The panel works on ANY skinned CRD/CH model — shipped or merged-by-
    // us. We list every model in the cache that has a non-empty bones
    // list, then for the picked model expose its bones as the "weapon"
    // dropdown (the field name is back-compat baggage; semantically it
    // selects the bone whose weighted verts will be moved).
    {
        LevelScene* lsBA0 = (g_renderer && g_renderer->hasLevelScene())
                            ? g_renderer->getLevelScene() : NULL;

        std::vector<std::string> modelNames;
        if (lsBA0) {
            // Pull all skinned models out of the cache. Anything with a
            // populated bones[] is fair game. Names sorted alphabetically
            // so the dropdown order is stable across loads.
            lsBA0->getSkinnedModelNames(modelNames);
            std::sort(modelNames.begin(), modelNames.end());
        }

        int mn = (int)modelNames.size(); if (mn > 256) mn = 256;
        args.bowAdjustModelOptionCount = mn;
        for (int i = 0; i < mn; ++i) {
            strncpy_s(args.bowAdjustModelOptions[i], 96,
                      modelNames[i].c_str(), _TRUNCATE);
        }

        if (g_bowAdjustSelModelIdx >= mn) g_bowAdjustSelModelIdx = 0;
        if (g_bowAdjustSelModelIdx < 0)   g_bowAdjustSelModelIdx = 0;
        args.bowAdjustSelModelIdx = g_bowAdjustSelModelIdx;
        if (mn > 0) {
            strncpy_s(args.bowAdjustSelModelName, 96,
                      modelNames[g_bowAdjustSelModelIdx].c_str(), _TRUNCATE);
        } else {
            args.bowAdjustSelModelName[0] = 0;
        }

        // Bone dropdown for the picked model. The "weapon" field name
        // stays for back-compat with the args struct; we treat it as a
        // bone-name slot from here on out.
        args.bowAdjustWeaponOptionCount = 0;
        if (mn > 0 && lsBA0) {
            const std::string& selName = modelNames[g_bowAdjustSelModelIdx];
            uint32_t selCrc = ZeroEngine::LotrHashString(selName.c_str(), -1);
            LevelModel* mdl = lsBA0->ensureModelLoaded(g_levelReader, selCrc, selName);
            if (mdl && !mdl->bones.empty()) {
                int bn = (int)mdl->bones.size(); if (bn > 128) bn = 128;
                args.bowAdjustWeaponOptionCount = bn;
                for (int i = 0; i < bn; ++i) {
                    strncpy_s(args.bowAdjustWeaponOptions[i], 64,
                              mdl->bones[i].c_str(), _TRUNCATE);
                }
            }
        }
        if (g_bowAdjustSelWeaponIdx >= args.bowAdjustWeaponOptionCount)
            g_bowAdjustSelWeaponIdx = 0;
        if (g_bowAdjustSelWeaponIdx < 0) g_bowAdjustSelWeaponIdx = 0;
        args.bowAdjustSelWeaponIdx = g_bowAdjustSelWeaponIdx;

        args.bowAdjustDeltaXYZ[0] = g_bowAdjustDeltaXYZ[0];
        args.bowAdjustDeltaXYZ[1] = g_bowAdjustDeltaXYZ[1];
        args.bowAdjustDeltaXYZ[2] = g_bowAdjustDeltaXYZ[2];
        args.bowAdjustDeltaYPR[0] = g_bowAdjustDeltaYPR[0];
        args.bowAdjustDeltaYPR[1] = g_bowAdjustDeltaYPR[1];
        args.bowAdjustDeltaYPR[2] = g_bowAdjustDeltaYPR[2];

        strncpy_s(args.bowAdjustStatusText, 160,
                  g_bowAdjustStatusText, _TRUNCATE);
    }

    // ─────────────────────────────────────────────────────────────────
    //  TRIANGLE PICKER — pre-frame: render picked model to embedded RT
    // ─────────────────────────────────────────────────────────────────
    // Compact version of the popup model viewer's render block. The
    // picked model goes into a dedicated 384x384 RT. Inside the panel
    // we draw it via ImGui::Image and capture clicks back to ray-pick
    // triangles. Camera = simple orbit around the model's bounds.
    args.triPickerOpen      = g_triPickerOpen;
    args.triPickerViewerTex = NULL;
    args.triPickerViewerW   = g_triPickerW;
    args.triPickerViewerH   = g_triPickerH;
    args.triPickerOrbitYaw  = g_triPickerYaw;
    args.triPickerOrbitPitch= g_triPickerPitch;
    args.triPickerOrbitDist = g_triPickerDist;
    args.triPickerDeltaXYZ[0] = g_triPickerDeltaXYZ[0];
    args.triPickerDeltaXYZ[1] = g_triPickerDeltaXYZ[1];
    args.triPickerDeltaXYZ[2] = g_triPickerDeltaXYZ[2];
    args.triPickerSelectedTriCount  = (int)g_triPickerSelTris.size();
    args.triPickerSelectedVertCount = (int)g_triPickerSelVerts.size();
    strncpy_s(args.triPickerStatusText, 160, g_triPickerStatusText, _TRUNCATE);

    if (g_triPickerOpen) {
        IDirect3DDevice9* devTP = NULL;
        if (g_renderer) devTP = g_renderer->getD3DDevice();
        LevelScene* lsTP = (g_renderer && g_renderer->hasLevelScene())
                            ? g_renderer->getLevelScene() : NULL;

        // Lazy-create the RT + DS surfaces at the target size.
        if (devTP && (!g_triPickerTex || !g_triPickerRT || !g_triPickerDS)) {
            if (g_triPickerRT) { g_triPickerRT->Release(); g_triPickerRT = NULL; }
            if (g_triPickerDS) { g_triPickerDS->Release(); g_triPickerDS = NULL; }
            if (g_triPickerTex){ g_triPickerTex->Release(); g_triPickerTex = NULL; }
            if (SUCCEEDED(devTP->CreateTexture(
                g_triPickerW, g_triPickerH, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_triPickerTex, NULL)) && g_triPickerTex)
            {
                g_triPickerTex->GetSurfaceLevel(0, &g_triPickerRT);
            }
            if (g_triPickerRT) {
                devTP->CreateDepthStencilSurface(g_triPickerW, g_triPickerH,
                    D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
                    &g_triPickerDS, NULL);
            }
        }

        // Active model = Crowd Mesh Builder's Base mesh selection if present,
        // else fall back to the Adjust Baked Weapon panel's picker. Lets the
        // user drive the picker from either entry point without juggling.
        std::string modelName = args.crowdBuilderSelBaseName;
        if (modelName.empty()) modelName = args.bowAdjustSelModelName;
        const uint32_t    modelCrc  = ZeroEngine::LotrHashString(modelName.c_str(), -1);
        LevelModel* mdl = (lsTP && !modelName.empty())
                          ? lsTP->ensureModelLoaded(g_levelReader, modelCrc, modelName)
                          : NULL;

        // Surface load failures to the panel status line — silent NULL was
        // the worst part of "viewer shows empty" because the user couldn't
        // tell whether they picked a real model or the loader had bailed.
        if (modelName.empty()) {
            sprintf_s(g_triPickerStatusText, 160,
                "Pick a model from the dropdown above (or the Base mesh combo).");
        } else if (!lsTP) {
            sprintf_s(g_triPickerStatusText, 160,
                "Level scene not ready yet. Reload the level.");
        } else if (!mdl) {
            sprintf_s(g_triPickerStatusText, 160,
                "Could not load '%s' (crc=0x%08X). Either not in BIN, "
                "or PAK was packed before this model was baked.",
                modelName.c_str(), modelCrc);
        } else if (mdl->parts.empty()) {
            sprintf_s(g_triPickerStatusText, 160,
                "'%s' loaded but has zero visible parts (occluder, "
                "collision-only, or broken material refs).",
                modelName.c_str());
        }

        if (devTP && lsTP && mdl && !mdl->parts.empty() && g_triPickerRT && g_triPickerDS) {
            // Save device state we'll touch.
            IDirect3DSurface9* oldRT = NULL; devTP->GetRenderTarget(0, &oldRT);
            IDirect3DSurface9* oldDS = NULL; devTP->GetDepthStencilSurface(&oldDS);
            D3DVIEWPORT9 oldVP; bool haveOldVP = SUCCEEDED(devTP->GetViewport(&oldVP));
            D3DMATRIX oldView, oldProj, oldWorld;
            devTP->GetTransform(D3DTS_VIEW,       &oldView);
            devTP->GetTransform(D3DTS_PROJECTION, &oldProj);
            devTP->GetTransform(D3DTS_WORLD,      &oldWorld);

            devTP->SetTexture(0, NULL);
            devTP->SetRenderTarget(0, g_triPickerRT);
            devTP->SetDepthStencilSurface(g_triPickerDS);
            D3DVIEWPORT9 vp = { 0, 0, (DWORD)g_triPickerW, (DWORD)g_triPickerH, 0.0f, 1.0f };
            devTP->SetViewport(&vp);
            devTP->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                         D3DCOLOR_ARGB(255, 40, 40, 50), 1.0f, 0);

            // Model center from local bounds.
            float cx = 0, cy = 0, cz = 0;
            if (mdl->hasBounds) {
                cx = (mdl->localMin[0] + mdl->localMax[0]) * 0.5f;
                cy = (mdl->localMin[1] + mdl->localMax[1]) * 0.5f;
                cz = (mdl->localMin[2] + mdl->localMax[2]) * 0.5f;
            }
            float cp = cosf(g_triPickerPitch), sp = sinf(g_triPickerPitch);
            float cyw = cosf(g_triPickerYaw),  syw = sinf(g_triPickerYaw);
            float eyeX = cx + g_triPickerDist * cp * syw;
            float eyeY = cy + g_triPickerDist * sp;
            float eyeZ = cz + g_triPickerDist * cp * cyw;
            g_triPickerLastEye[0] = eyeX;
            g_triPickerLastEye[1] = eyeY;
            g_triPickerLastEye[2] = eyeZ;

            // LookAt LH manually.
            float zAx[3] = { cx - eyeX, cy - eyeY, cz - eyeZ };
            float zLen = sqrtf(zAx[0]*zAx[0]+zAx[1]*zAx[1]+zAx[2]*zAx[2]);
            if (zLen > 1e-6f) { zAx[0]/=zLen; zAx[1]/=zLen; zAx[2]/=zLen; }
            float upV[3] = {0,1,0};
            float xAx[3] = { upV[1]*zAx[2]-upV[2]*zAx[1],
                              upV[2]*zAx[0]-upV[0]*zAx[2],
                              upV[0]*zAx[1]-upV[1]*zAx[0] };
            float xLen = sqrtf(xAx[0]*xAx[0]+xAx[1]*xAx[1]+xAx[2]*xAx[2]);
            if (xLen > 1e-6f) { xAx[0]/=xLen; xAx[1]/=xLen; xAx[2]/=xLen; }
            float yAx[3] = { zAx[1]*xAx[2]-zAx[2]*xAx[1],
                              zAx[2]*xAx[0]-zAx[0]*xAx[2],
                              zAx[0]*xAx[1]-zAx[1]*xAx[0] };
            float viewMat[16] = {
                xAx[0], yAx[0], zAx[0], 0,
                xAx[1], yAx[1], zAx[1], 0,
                xAx[2], yAx[2], zAx[2], 0,
                -(xAx[0]*eyeX+xAx[1]*eyeY+xAx[2]*eyeZ),
                -(yAx[0]*eyeX+yAx[1]*eyeY+yAx[2]*eyeZ),
                -(zAx[0]*eyeX+zAx[1]*eyeY+zAx[2]*eyeZ),
                1
            };
            float fovY = 3.14159265f / 4.0f;
            float aspect = (float)g_triPickerW / (float)g_triPickerH;
            float zn = 0.05f, zf = 1000.0f;
            float yScale = 1.0f / tanf(fovY * 0.5f);
            float xScale = yScale / aspect;
            float projMat[16] = {
                xScale, 0,      0,                  0,
                0,      yScale, 0,                  0,
                0,      0,      zf / (zf - zn),     1,
                0,      0,     -zn*zf / (zf - zn),  0
            };
            memcpy(g_triPickerLastView, viewMat, sizeof(viewMat));
            memcpy(g_triPickerLastProj, projMat, sizeof(projMat));

            D3DMATRIX viewD3D, projD3D, worldD3D;
            memcpy(&viewD3D, viewMat, sizeof(viewD3D));
            memcpy(&projD3D, projMat, sizeof(projD3D));
            memset(&worldD3D, 0, sizeof(worldD3D));
            worldD3D._11 = worldD3D._22 = worldD3D._33 = worldD3D._44 = 1.0f;
            devTP->SetTransform(D3DTS_VIEW,       &viewD3D);
            devTP->SetTransform(D3DTS_PROJECTION, &projD3D);
            devTP->SetTransform(D3DTS_WORLD,      &worldD3D);

            float worldId[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            lsTP->drawSingleModelWithShaders(mdl, worldId, viewMat, projMat);

            // Restore device state.
            devTP->SetRenderTarget(0, oldRT);
            devTP->SetDepthStencilSurface(oldDS);
            if (haveOldVP) devTP->SetViewport(&oldVP);
            devTP->SetTransform(D3DTS_VIEW,       &oldView);
            devTP->SetTransform(D3DTS_PROJECTION, &oldProj);
            devTP->SetTransform(D3DTS_WORLD,      &oldWorld);
            if (oldRT) oldRT->Release();
            if (oldDS) oldDS->Release();

            args.triPickerViewerTex = (void*)g_triPickerTex;
        }
    }

    // Publish the game shader name table to the DLL so the Render
    // Debug panel can populate its PS dropdown. We cache the
    // std::vector + char* table across frames — the strings stay
    // valid as long as the cache itself is loaded (which is "until
    // the level unloads"). We rebuild only when the cache pointer
    // or shader count changes, so this is effectively free per frame.
    static const GameShaderCache* s_psCacheLast = NULL;
    static int s_psCountLast = 0;
    static std::vector<std::string> s_psNames;
    static std::vector<const char*> s_psNamesC;
    {
        GameShaderCache* gsc = NULL;
        if (g_renderer && g_renderer->hasLevelScene()) {
            gsc = g_renderer->getLevelScene()->getGameShaders();
        }
        int curCount = gsc ? gsc->psCount() : 0;
        if (gsc != s_psCacheLast || curCount != s_psCountLast) {
            s_psNames.clear();
            s_psNamesC.clear();
            if (gsc) {
                gsc->getPSNames(s_psNames);
                s_psNamesC.reserve(s_psNames.size());
                for (size_t i = 0; i < s_psNames.size(); ++i) {
                    s_psNamesC.push_back(s_psNames[i].c_str());
                }
            }
            s_psCacheLast = gsc;
            s_psCountLast = curCount;
        }
        args.gameShaderNames     = s_psNamesC.empty() ? NULL : &s_psNamesC[0];
        args.gameShaderNameCount = (int)s_psNamesC.size();
    }

    // Publish frame stats + displayWorld object count to the DLL so
    // the Render Debug panel's Frame Stats table has real numbers.
    if (g_renderer) {
        for (int i = 0; i < 6; ++i) {
            const Scene3DRenderer::PassStats& s = g_renderer->getPassStats(i);
            args.passStatsDrawCalls[i] = s.drawCalls;
            args.passStatsTriangles[i] = s.triangles;
            args.passStatsMeshParts[i] = s.meshPartsDrawn;
        }
        args.displayWorldObjectCount = g_renderer->getDisplayWorldObjectCount();
    } else {
        for (int i = 0; i < 6; ++i) {
            args.passStatsDrawCalls[i] = 0;
            args.passStatsTriangles[i] = 0;
            args.passStatsMeshParts[i] = 0;
        }
        args.displayWorldObjectCount = 0;
    }

    // Publish loaded GameModel's meshPart summary table. Cached
    // across frames — rebuilt only when the model pointer or part
    // count changes. The strings are owned by the GameModel itself
    // (materialName / diffuseName are fixed char arrays inside
    // MeshPart) so they stay valid until the model is unloaded.
    static GameModel* s_gmCache = NULL;
    static int s_gmPartCountLast = 0;
    static std::vector<const char*> s_gmMatNames;
    static std::vector<const char*> s_gmDiffNames;
    static std::vector<int> s_gmVerts;
    static std::vector<int> s_gmIdx;
    static std::vector<int> s_gmLod;
    static std::vector<int> s_gmHasTex;
    {
        GameModel* gm = g_renderer ? g_renderer->getGameModel() : NULL;
        int curCount = gm ? gm->meshPartCount : 0;
        if (gm != s_gmCache || curCount != s_gmPartCountLast) {
            s_gmMatNames.clear();
            s_gmDiffNames.clear();
            s_gmVerts.clear();
            s_gmIdx.clear();
            s_gmLod.clear();
            s_gmHasTex.clear();
            if (gm) {
                for (int i = 0; i < gm->meshPartCount; ++i) {
                    const GameModel::MeshPart& p = gm->meshParts[i];
                    s_gmMatNames.push_back(p.materialName);
                    s_gmDiffNames.push_back(p.diffuseName);
                    s_gmVerts.push_back(p.vertexCount);
                    s_gmIdx.push_back(p.indexCount);
                    s_gmLod.push_back(p.lodLevel);
                    s_gmHasTex.push_back(p.diffuseTexture ? 1 : 0);
                }
            }
            s_gmCache = gm;
            s_gmPartCountLast = curCount;
        }
        args.gameModelPartCount = (int)s_gmMatNames.size();
        args.gameModelPartMatNames    = s_gmMatNames.empty()  ? NULL : &s_gmMatNames[0];
        args.gameModelPartDiffNames   = s_gmDiffNames.empty() ? NULL : &s_gmDiffNames[0];
        args.gameModelPartVertexCounts= s_gmVerts.empty()     ? NULL : &s_gmVerts[0];
        args.gameModelPartIndexCounts = s_gmIdx.empty()       ? NULL : &s_gmIdx[0];
        args.gameModelPartLodLevels   = s_gmLod.empty()       ? NULL : &s_gmLod[0];
        args.gameModelPartHasTexture  = s_gmHasTex.empty()    ? NULL : &s_gmHasTex[0];
    }

    fpImGui_DrawFrame(&args);

    // ─────────────────────────────────────────────────────────────────
    //  ADJUST MESH BY BONE — post-frame: bone-weight select + live VB
    // ─────────────────────────────────────────────────────────────────
    // Pivoted from the sidecar-only path so it works on every skinned
    // CRD/CH, baked or shipped. Selection mechanism:
    //   1. Look up the picked bone name in model->bones → bone_idx
    //   2. Scan model->skinOrder for bone_idx → palette_idx
    //   3. Scan part[0]'s cached BlendIndices/Weights → vert index list
    //   4. Snapshot positions for those indices on weapon/model change
    //   5. Every frame: build worldDelta = bind_world * delta_local *
    //      inv(bind_world), apply via applyDeltaByIndex
    //   6. Apply: persist with model name + bone name (bone-keyed); the
    //      Python helper re-runs the same bone-weight selection on the
    //      JSON to commit the move.
    {
        g_bowAdjustSelModelIdx  = args.bowAdjustSelModelIdx;
        g_bowAdjustSelWeaponIdx = args.bowAdjustSelWeaponIdx;
        g_bowAdjustDeltaXYZ[0]  = args.bowAdjustDeltaXYZ[0];
        g_bowAdjustDeltaXYZ[1]  = args.bowAdjustDeltaXYZ[1];
        g_bowAdjustDeltaXYZ[2]  = args.bowAdjustDeltaXYZ[2];
        g_bowAdjustDeltaYPR[0]  = args.bowAdjustDeltaYPR[0];
        g_bowAdjustDeltaYPR[1]  = args.bowAdjustDeltaYPR[1];
        g_bowAdjustDeltaYPR[2]  = args.bowAdjustDeltaYPR[2];

        if (args.bowAdjustResetRequested) {
            g_bowAdjustDeltaXYZ[0] = g_bowAdjustDeltaXYZ[1] = g_bowAdjustDeltaXYZ[2] = 0.0f;
            g_bowAdjustDeltaYPR[0] = g_bowAdjustDeltaYPR[1] = g_bowAdjustDeltaYPR[2] = 0.0f;
        }

        LevelScene* lsBA = (g_renderer && g_renderer->hasLevelScene())
                           ? g_renderer->getLevelScene() : NULL;
        if (lsBA && args.bowAdjustModelOptionCount > 0 &&
            args.bowAdjustWeaponOptionCount > 0)
        {
            const std::string modelName = args.bowAdjustSelModelName;
            const uint32_t    modelCrc  = ZeroEngine::LotrHashString(modelName.c_str(), -1);
            LevelModel* mdl = lsBA->ensureModelLoaded(g_levelReader, modelCrc, modelName);

            const int boneIdxUI = args.bowAdjustSelWeaponIdx;  // re-used field, holds bone index in mdl->bones
            if (mdl && boneIdxUI >= 0 && boneIdxUI < (int)mdl->bones.size())
            {
                const std::string boneName = mdl->bones[boneIdxUI];

                // Resolve to palette index (slot in skinOrder where the
                // bone lives). If the bone isn't in the palette, the
                // mesh isn't actually weighted to it — bail gracefully.
                uint32_t paletteIdx = (uint32_t)-1;
                for (uint32_t i = 0; i < mdl->skinOrder.size(); ++i) {
                    if ((int)mdl->skinOrder[i] == boneIdxUI) { paletteIdx = i; break; }
                }

                bool selChanged = (g_bowAdjustSnapCrc       != modelCrc ||
                                   g_bowAdjustSnapWeaponIdx != boneIdxUI);

                if (paletteIdx != (uint32_t)-1) {
                    // Re-select + re-snapshot on model/bone change.
                    if (selChanged || g_bowAdjustOrigPositions.empty()) {
                        g_bowAdjustSelectedIndices.clear();
                        lsBA->getVertsWeightedToBone(mdl, paletteIdx,
                                                     g_bowAdjustSelectedIndices);
                        if (!g_bowAdjustSelectedIndices.empty()) {
                            lsBA->snapshotVertsByIndex(mdl, g_bowAdjustSelectedIndices,
                                                       g_bowAdjustOrigPositions);
                            g_bowAdjustSnapCrc       = modelCrc;
                            g_bowAdjustSnapWeaponIdx = boneIdxUI;
                            sprintf_s(g_bowAdjustStatusText, 160,
                                "Selected %u vert(s) for bone='%s' palette_idx=%u",
                                (unsigned)g_bowAdjustSelectedIndices.size(),
                                boneName.c_str(), (unsigned)paletteIdx);
                        } else {
                            g_bowAdjustOrigPositions.clear();
                            sprintf_s(g_bowAdjustStatusText, 160,
                                "NO VERTS matched bone='%s' (palette_idx=%u). "
                                "Either the bone owns nothing in slot[0], or the "
                                "blend-byte pairing convention doesn't match. "
                                "Try a different bone.",
                                boneName.c_str(), (unsigned)paletteIdx);
                        }
                    }

                    if (!g_bowAdjustSelectedIndices.empty() &&
                        !g_bowAdjustOrigPositions.empty())
                    {
                        // bind_world for this palette slot.
                        float bindWorld[16];
                        if (lsBA->getBindWorldForPaletteIdx(mdl, paletteIdx, bindWorld))
                        {
                            const float deg2rad = 3.14159265358979f / 180.0f;
                            float yaw = g_bowAdjustDeltaYPR[0] * deg2rad;
                            float pit = g_bowAdjustDeltaYPR[1] * deg2rad;
                            float rol = g_bowAdjustDeltaYPR[2] * deg2rad;
                            float cy = cosf(yaw), sy = sinf(yaw);
                            float cp = cosf(pit), sp = sinf(pit);
                            float cr = cosf(rol), sr = sinf(rol);
                            float dL[16] = {
                                cy*cr + sy*sp*sr,   cp*sr,    -sy*cr + cy*sp*sr,  g_bowAdjustDeltaXYZ[0],
                                -cy*sr + sy*sp*cr,  cp*cr,     sy*sr + cy*sp*cr,  g_bowAdjustDeltaXYZ[1],
                                sy*cp,              -sp,       cy*cp,             g_bowAdjustDeltaXYZ[2],
                                0,                  0,         0,                 1
                            };
                            // Affine inverse of bind_world.
                            const float* B = bindWorld;
                            float Bi[16];
                            Bi[0]  = B[0];  Bi[1]  = B[4];  Bi[2]  = B[8];
                            Bi[4]  = B[1];  Bi[5]  = B[5];  Bi[6]  = B[9];
                            Bi[8]  = B[2];  Bi[9]  = B[6];  Bi[10] = B[10];
                            float btx = B[3], bty = B[7], btz = B[11];
                            Bi[3]  = -(Bi[0]*btx + Bi[1]*bty + Bi[2]*btz);
                            Bi[7]  = -(Bi[4]*btx + Bi[5]*bty + Bi[6]*btz);
                            Bi[11] = -(Bi[8]*btx + Bi[9]*bty + Bi[10]*btz);
                            Bi[12] = Bi[13] = Bi[14] = 0; Bi[15] = 1;
                            float BdL[16];
                            for (int r = 0; r < 4; ++r)
                                for (int c = 0; c < 4; ++c) {
                                    float s = 0;
                                    for (int k = 0; k < 4; ++k) s += B[r*4+k] * dL[k*4+c];
                                    BdL[r*4+c] = s;
                                }
                            float worldDelta[16];
                            for (int r = 0; r < 4; ++r)
                                for (int c = 0; c < 4; ++c) {
                                    float s = 0;
                                    for (int k = 0; k < 4; ++k) s += BdL[r*4+k] * Bi[k*4+c];
                                    worldDelta[r*4+c] = s;
                                }

                            lsBA->applyDeltaByIndex(mdl, g_bowAdjustSelectedIndices,
                                                    &g_bowAdjustOrigPositions[0],
                                                    worldDelta);
                        }
                    }
                } else {
                    // Bone isn't in this model's skin palette.
                    if (selChanged) {
                        g_bowAdjustSnapCrc       = modelCrc;
                        g_bowAdjustSnapWeaponIdx = boneIdxUI;
                        g_bowAdjustSelectedIndices.clear();
                        g_bowAdjustOrigPositions.clear();
                    }
                }

                // Apply: persist delta with bone NAME (bone-keyed).
                if (args.bowAdjustApplyRequested) {
                    std::string dumpDir = g_levelReader.GetDumpDir();
                    if (!dumpDir.empty()) {
                        std::string path = dumpDir + "/ze_embedded_weapon_xforms.json";
                        char buf[1024];
                        sprintf_s(buf, sizeof(buf),
                            "{\n"
                            " \"xforms\": [\n"
                            "  {\n"
                            "   \"model\": \"%s\",\n"
                            "   \"bone\": \"%s\",\n"
                            "   \"delta_xyz\": [%.6f, %.6f, %.6f],\n"
                            "   \"delta_ypr\": [%.4f, %.4f, %.4f]\n"
                            "  }\n"
                            " ]\n"
                            "}\n",
                            modelName.c_str(), boneName.c_str(),
                            g_bowAdjustDeltaXYZ[0], g_bowAdjustDeltaXYZ[1], g_bowAdjustDeltaXYZ[2],
                            g_bowAdjustDeltaYPR[0], g_bowAdjustDeltaYPR[1], g_bowAdjustDeltaYPR[2]);
                        FILE* f = NULL;
                        fopen_s(&f, path.c_str(), "wb");
                        if (f) {
                            fwrite(buf, 1, strlen(buf), f);
                            fclose(f);
                            sprintf_s(g_bowAdjustStatusText, 160,
                                "Persisted (%s, bone=%s). Save Level to commit.",
                                modelName.c_str(), boneName.c_str());
                        } else {
                            sprintf_s(g_bowAdjustStatusText, 160,
                                "ERROR: could not write %s", path.c_str());
                        }
                    } else {
                        sprintf_s(g_bowAdjustStatusText, 160,
                            "ERROR: no dump dir resolvable from pak path");
                    }
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────
    //  TRIANGLE PICKER — post-frame: click → ray-pick → selection state
    // ─────────────────────────────────────────────────────────────────
    {
        g_triPickerOpen        = args.triPickerOpen;
        g_triPickerYaw         = args.triPickerOrbitYaw;
        g_triPickerPitch       = args.triPickerOrbitPitch;
        g_triPickerDist        = args.triPickerOrbitDist;
        g_triPickerDeltaXYZ[0] = args.triPickerDeltaXYZ[0];
        g_triPickerDeltaXYZ[1] = args.triPickerDeltaXYZ[1];
        g_triPickerDeltaXYZ[2] = args.triPickerDeltaXYZ[2];

        if (args.triPickerResetSliders) {
            g_triPickerDeltaXYZ[0] = g_triPickerDeltaXYZ[1] = g_triPickerDeltaXYZ[2] = 0.0f;
        }
        if (args.triPickerClearSelection) {
            g_triPickerSelTris.clear();
            g_triPickerSelVerts.clear();
            g_triPickerSelOrigPos.clear();
            sprintf_s(g_triPickerStatusText, 160, "Selection cleared.");
        }

        LevelScene* lsTPp = (g_renderer && g_renderer->hasLevelScene())
                            ? g_renderer->getLevelScene() : NULL;
        std::string modelName = args.crowdBuilderSelBaseName;
        if (modelName.empty()) modelName = args.bowAdjustSelModelName;
        const uint32_t    modelCrc  = ZeroEngine::LotrHashString(modelName.c_str(), -1);
        LevelModel* mdl = (lsTPp && !modelName.empty())
                          ? lsTPp->ensureModelLoaded(g_levelReader, modelCrc, modelName)
                          : NULL;

        // Model changed → drop selection (the indices reference a different mesh).
        if (mdl && g_triPickerSnapModelCrc != modelCrc) {
            g_triPickerSelTris.clear();
            g_triPickerSelVerts.clear();
            g_triPickerSelOrigPos.clear();
            g_triPickerSnapModelCrc = modelCrc;
        }

        // Click → ray-pick.
        if (g_triPickerOpen && mdl && args.triPickerClickPending) {
            float u = ((float)args.triPickerClickX + 0.5f) / (float)g_triPickerW;
            float v = ((float)args.triPickerClickY + 0.5f) / (float)g_triPickerH;
            // Image-space (0..1) → clip-space (-1..+1), v flipped.
            float ndcX =  (u * 2.0f - 1.0f);
            float ndcY = -(v * 2.0f - 1.0f);
            // Unproject via inverse(view*proj). We stored row-major view+proj,
            // for D3D9-style perspective the simplest path is to build a ray
            // in view space then transform to world.
            // view-space ray dir (D3D LH, looking +z):
            float yScale = g_triPickerLastProj[5];
            float xScale = g_triPickerLastProj[0];
            float vdx = ndcX / xScale;
            float vdy = ndcY / yScale;
            float vdz = 1.0f;
            float vlen = sqrtf(vdx*vdx + vdy*vdy + vdz*vdz);
            if (vlen > 1e-6f) { vdx/=vlen; vdy/=vlen; vdz/=vlen; }
            // View matrix is world→view. To rotate a view-space direction back
            // to world space, multiply by the TRANSPOSE of view's 3x3 rotation.
            const float* V = g_triPickerLastView;
            // View row-major stored as columns of the rotation in V[0..10]
            // per the LookAt build above (xAx in col 0, yAx col 1, zAx col 2).
            float wdx = V[0]*vdx + V[1]*vdy + V[2]*vdz;
            float wdy = V[4]*vdx + V[5]*vdy + V[6]*vdz;
            float wdz = V[8]*vdx + V[9]*vdy + V[10]*vdz;
            float wlen = sqrtf(wdx*wdx + wdy*wdy + wdz*wdz);
            if (wlen > 1e-6f) { wdx/=wlen; wdy/=wlen; wdz/=wlen; }
            float rayOrigin[3] = { g_triPickerLastEye[0], g_triPickerLastEye[1], g_triPickerLastEye[2] };
            float rayDir[3]    = { wdx, wdy, wdz };

            int hitTri = -1; float hitT = 0.0f;
            if (lsTPp->pickTriangleOnModel(mdl, rayOrigin, rayDir, &hitTri, &hitT)) {
                if (!args.triPickerShiftHeld) g_triPickerSelTris.clear();
                g_triPickerSelTris.insert(hitTri);
                sprintf_s(g_triPickerStatusText, 160,
                    "Picked tri %d (t=%.3f). Selection: %u tri(s).",
                    hitTri, hitT, (unsigned)g_triPickerSelTris.size());
            } else {
                sprintf_s(g_triPickerStatusText, 160, "Click missed every triangle.");
            }
        }

        // Grow selection to connected triangles (one step — share a vert).
        if (g_triPickerOpen && mdl && args.triPickerGrowConnected
            && !g_triPickerSelTris.empty() && !mdl->parts.empty())
        {
            const LevelMeshPart& part = mdl->parts[0];
            if (!part.indicesCpu.empty()) {
                // Build vert→tris adjacency on demand. Cheap enough for ~4K tris.
                std::set<uint32_t> selVerts;
                for (std::set<int>::const_iterator it = g_triPickerSelTris.begin();
                     it != g_triPickerSelTris.end(); ++it)
                {
                    int t = *it;
                    if (t < 0) continue;
                    if ((size_t)(t*3 + 2) >= part.indicesCpu.size()) continue;
                    selVerts.insert(part.indicesCpu[t*3+0]);
                    selVerts.insert(part.indicesCpu[t*3+1]);
                    selVerts.insert(part.indicesCpu[t*3+2]);
                }
                const uint32_t triCount = (uint32_t)part.indicesCpu.size() / 3;
                int beforeCount = (int)g_triPickerSelTris.size();
                for (uint32_t t = 0; t < triCount; ++t) {
                    uint32_t a = part.indicesCpu[t*3+0];
                    uint32_t b = part.indicesCpu[t*3+1];
                    uint32_t c = part.indicesCpu[t*3+2];
                    if (selVerts.count(a) || selVerts.count(b) || selVerts.count(c))
                        g_triPickerSelTris.insert((int)t);
                }
                int afterCount = (int)g_triPickerSelTris.size();
                sprintf_s(g_triPickerStatusText, 160,
                    "Grow: %d -> %d tri(s) connected by shared vert.",
                    beforeCount, afterCount);
            }
        }

        // Rebuild unique vert list + snapshot when selection changes.
        if (mdl && !mdl->parts.empty()) {
            const LevelMeshPart& part = mdl->parts[0];
            std::set<uint32_t> uniq;
            for (std::set<int>::const_iterator it = g_triPickerSelTris.begin();
                 it != g_triPickerSelTris.end(); ++it)
            {
                int t = *it;
                if (t < 0) continue;
                if ((size_t)(t*3 + 2) >= part.indicesCpu.size()) continue;
                uniq.insert(part.indicesCpu[t*3+0]);
                uniq.insert(part.indicesCpu[t*3+1]);
                uniq.insert(part.indicesCpu[t*3+2]);
            }
            bool changed = uniq.size() != g_triPickerSelVerts.size();
            if (!changed) {
                size_t k = 0;
                for (std::set<uint32_t>::const_iterator it = uniq.begin(); it != uniq.end(); ++it, ++k) {
                    if (g_triPickerSelVerts[k] != *it) { changed = true; break; }
                }
            }
            if (changed) {
                g_triPickerSelVerts.assign(uniq.begin(), uniq.end());
                lsTPp->snapshotVertsByIndex(mdl, g_triPickerSelVerts, g_triPickerSelOrigPos);
            }
        }

        // Live VB delta — translation only (sliders are world XYZ).
        if (mdl && !g_triPickerSelVerts.empty() && !g_triPickerSelOrigPos.empty()) {
            float worldDelta[16] = {
                1, 0, 0, g_triPickerDeltaXYZ[0],
                0, 1, 0, g_triPickerDeltaXYZ[1],
                0, 0, 1, g_triPickerDeltaXYZ[2],
                0, 0, 0, 1
            };
            lsTPp->applyWorldTransformToVerts(mdl, g_triPickerSelVerts,
                                              &g_triPickerSelOrigPos[0], worldDelta);
        }

        // Apply — persist a vert-list xform sidecar.
        if (g_triPickerOpen && mdl && args.triPickerApplyRequested
            && !g_triPickerSelVerts.empty())
        {
            std::string dumpDir = g_levelReader.GetDumpDir();
            if (!dumpDir.empty()) {
                std::string path = dumpDir + "/ze_embedded_weapon_xforms.json";
                std::string body;
                body.reserve(64 + g_triPickerSelVerts.size() * 12);
                char head[512];
                sprintf_s(head, sizeof(head),
                    "{\n \"xforms\": [\n  {\n"
                    "   \"model\": \"%s\",\n"
                    "   \"delta_xyz\": [%.6f, %.6f, %.6f],\n"
                    "   \"verts\": [",
                    modelName.c_str(),
                    g_triPickerDeltaXYZ[0], g_triPickerDeltaXYZ[1], g_triPickerDeltaXYZ[2]);
                body = head;
                for (size_t i = 0; i < g_triPickerSelVerts.size(); ++i) {
                    char num[24];
                    sprintf_s(num, sizeof(num),
                        (i + 1 < g_triPickerSelVerts.size()) ? "%u, " : "%u",
                        (unsigned)g_triPickerSelVerts[i]);
                    body += num;
                }
                body += "]\n  }\n ]\n}\n";

                FILE* f = NULL; fopen_s(&f, path.c_str(), "wb");
                if (f) {
                    fwrite(body.data(), 1, body.size(), f);
                    fclose(f);
                    sprintf_s(g_triPickerStatusText, 160,
                        "Persisted %u verts (model=%s). Save Level to commit.",
                        (unsigned)g_triPickerSelVerts.size(), modelName.c_str());
                } else {
                    sprintf_s(g_triPickerStatusText, 160,
                        "ERROR: could not write %s", path.c_str());
                }
            } else {
                sprintf_s(g_triPickerStatusText, 160, "ERROR: no dump dir resolvable from pak path");
            }
        }
    }

    // ── Consume DLL→host crowd PLACE request ─────────────────────────
    // Right-click submenu "Place Crowd Character Here > <archetype>" fires
    // crowdPlaceRequested = 1 with pendingCrowdPlaceArchetypeItem set to
    // the index into m_crowdItems whose archetype should get a new
    // instance, and pendingCrowdPlacePos set to the world position. We
    // append the placement, mark dirty, and rebuild the LevelInstance list.
    {
        LevelScene* lsP = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;
        if (lsP && args.crowdPlaceRequested) {
            int archItem = args.pendingCrowdPlaceArchetypeItem;
            int total = lsP->getCrowdItemCount();
            if (archItem >= 0 && archItem < total) {
                // Y-snap: right-click raycast lands on Y=0 plane but
                // many levels have terrain at Y=20+ (Helm's Deep wall).
                // Place the new instance at the first existing
                // instance's Y so it lands on the same surface the
                // archetype already lives on, not buried underground.
                float placeX = args.pendingCrowdPlacePos[0];
                float placeY = args.pendingCrowdPlacePos[1];
                float placeZ = args.pendingCrowdPlacePos[2];
                const std::vector<LevelCrowdItem>& citems = lsP->getCrowdItems();
                if (archItem < (int)citems.size()
                    && !citems[archItem].instances.empty())
                {
                    placeY = citems[archItem].instances[0].position[1];
                }
                lsP->addCrowdInstanceToItem(archItem,
                    placeX, placeY, placeZ,
                    0.0f,        // default rotation = face -Z (engine convention)
                    10000.0f);   // default LOD = shipped game's most common value (100m)
                lsP->rebuildCrowdInstances();
            }
        }
    }

    // ── Consume DLL→host crowd GENERATE request ──────────────────────
    // Generator window's "Spawn N here" button fires this. We loop N times
    // sampling uniform points inside a disc of pendingCrowdGenRadius around
    // pendingCrowdGenPos, optionally jittering Y, optionally randomizing
    // yaw. Each result calls addCrowdInstanceToItem (which marks dirty);
    // ONE rebuildCrowdInstances() at the end means the LevelInstance vector
    // gets churned exactly once instead of N times.
    {
        LevelScene* lsG = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;
        if (lsG && args.crowdGenerateRequested) {
            int archItem = args.pendingCrowdGenArchetype;
            int total = lsG->getCrowdItemCount();
            int count = args.pendingCrowdGenCount;
            float radius = args.pendingCrowdGenRadius;
            float jitterY = args.pendingCrowdGenJitterY;
            bool randomYaw = (args.pendingCrowdGenRandomYaw != 0);

            if (archItem >= 0 && archItem < total && count > 0 && count <= 5000 && radius > 0.0f) {
                float cx = args.pendingCrowdGenPos[0];
                float cy = args.pendingCrowdGenPos[1];
                float cz = args.pendingCrowdGenPos[2];
                // Same Y-snap as the single-place flow: drop the
                // cluster center onto the archetype's existing
                // instance surface instead of the Y=0 raycast plane,
                // otherwise all 50 instances spawn buried underground.
                {
                    const std::vector<LevelCrowdItem>& citems = lsG->getCrowdItems();
                    if (archItem < (int)citems.size()
                        && !citems[archItem].instances.empty())
                    {
                        cy = citems[archItem].instances[0].position[1];
                    }
                }
                // We use rand() — not cryptographic, just need scatter. Seed
                // is whatever the CRT was initialized with; the user can
                // re-roll by clicking Spawn again. Uniform-area distribution
                // requires sqrt of the radial roll so density stays even
                // across the disc instead of clumping near center.
                for (int gi = 0; gi < count; ++gi) {
                    float rn  = (float)rand() / (float)RAND_MAX;
                    float ang = (float)rand() / (float)RAND_MAX * 6.2831853f;
                    float r   = sqrtf(rn) * radius;
                    float ox  = cosf(ang) * r;
                    float oz  = sinf(ang) * r;
                    float oy  = 0.0f;
                    if (jitterY > 0.0f) {
                        float jy = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                        oy = jy * jitterY;
                    }
                    float yaw = 0.0f;
                    if (randomYaw) {
                        yaw = (float)rand() / (float)RAND_MAX * 6.2831853f;
                    }
                    lsG->addCrowdInstanceToItem(archItem,
                        cx + ox, cy + oy, cz + oz,
                        yaw, 10000.0f);
                }
                lsG->rebuildCrowdInstances();
            }
        }

        // ── Path Place scatter ──────────────────────────────────────
        // DLL drew a polyline + tweaked spacing/rows/facing/jitter and
        // clicked Stamp. We sample the polyline at arc-length, replicate
        // per row with perpendicular offset, compute yaw per facing
        // mode, apply deterministic jitter, optionally snap Y to the
        // ground plane, and spawn one CrowdVal per resulting position.
        // Same scatter pipeline as the radial generator above, just
        // a different geometry source.
        if (lsG && args.pathPlaceRequested) {
            int archItem  = args.pathPlaceArchetype;
            int total     = lsG->getCrowdItemCount();
            int nodeCount = args.pathPlaceNodeCount;
            float spacing = args.pathPlaceSpacing;
            int rows      = args.pathPlaceRows;
            float rowGap  = args.pathPlaceRowGap;
            if (archItem >= 0 && archItem < total
                && nodeCount >= 2 && nodeCount <= 64
                && spacing > 0.01f && rows >= 1 && rows <= 8)
            {
                const float* nodes = args.pathPlaceNodes;
                // 1) total arc length of the polyline
                float totalLen = 0.0f;
                for (int i = 0; i + 1 < nodeCount; ++i) {
                    float dx = nodes[(i+1)*3+0] - nodes[i*3+0];
                    float dy = nodes[(i+1)*3+1] - nodes[i*3+1];
                    float dz = nodes[(i+1)*3+2] - nodes[i*3+2];
                    totalLen += sqrtf(dx*dx + dy*dy + dz*dz);
                }
                int N = (int)(totalLen / spacing) + 1;
                if (N < 1) N = 1;
                if (N > 1000) N = 1000;  // sanity cap

                // 2) sample positions + tangents at even arc-length
                struct Sample { float p[3]; float t[3]; };
                std::vector<Sample> samples;
                samples.reserve(N);
                for (int s = 0; s < N; ++s) {
                    float wantLen = (float)s * spacing;
                    // walk segments until we find the one containing wantLen
                    float accum = 0.0f;
                    int seg = 0;
                    while (seg + 1 < nodeCount) {
                        float dx = nodes[(seg+1)*3+0] - nodes[seg*3+0];
                        float dy = nodes[(seg+1)*3+1] - nodes[seg*3+1];
                        float dz = nodes[(seg+1)*3+2] - nodes[seg*3+2];
                        float segLen = sqrtf(dx*dx + dy*dy + dz*dz);
                        if (accum + segLen >= wantLen || seg + 2 >= nodeCount) {
                            float lt = (segLen > 0.001f)
                                       ? (wantLen - accum) / segLen
                                       : 0.0f;
                            if (lt < 0.0f) lt = 0.0f;
                            if (lt > 1.0f) lt = 1.0f;
                            Sample sm;
                            sm.p[0] = nodes[seg*3+0] + dx * lt;
                            sm.p[1] = nodes[seg*3+1] + dy * lt;
                            sm.p[2] = nodes[seg*3+2] + dz * lt;
                            float tlen = sqrtf(dx*dx + dz*dz);
                            if (tlen > 0.001f) {
                                sm.t[0] = dx / tlen;
                                sm.t[1] = 0.0f;
                                sm.t[2] = dz / tlen;
                            } else {
                                sm.t[0] = 1.0f; sm.t[1] = 0.0f; sm.t[2] = 0.0f;
                            }
                            samples.push_back(sm);
                            break;
                        }
                        accum += segLen;
                        ++seg;
                    }
                }

                // 3) multi-row replication: perpendicular offset
                unsigned int rngState = args.pathPlaceRngSeed;
                // simple xorshift PRNG; we want deterministic, not crypto
                #define PATH_RNG_NEXT() (rngState ^= rngState << 13, \
                                          rngState ^= rngState >> 17, \
                                          rngState ^= rngState <<  5, \
                                          rngState)
                int facingMode = args.pathPlaceFacingMode;
                float fixedYaw = args.pathPlaceFixedYaw;
                float laX = args.pathPlaceLookAt[0];
                float laZ = args.pathPlaceLookAt[2];
                float posJitter = args.pathPlacePosJitter;
                float yawJitter = args.pathPlaceYawJitter;
                bool snapGround = (args.pathPlaceSnapGround != 0);

                int spawned = 0;
                for (size_t si = 0; si < samples.size(); ++si) {
                    const Sample& sm = samples[si];
                    // perpendicular in XZ plane
                    float perpX = -sm.t[2];
                    float perpZ =  sm.t[0];
                    for (int r = 0; r < rows; ++r) {
                        float rowOff = ((float)r - (rows - 1) * 0.5f) * rowGap;
                        float px = sm.p[0] + perpX * rowOff;
                        float py = sm.p[1];
                        float pz = sm.p[2] + perpZ * rowOff;

                        // facing
                        float yaw = 0.0f;
                        switch (facingMode) {
                            case 0: // tangent
                                yaw = atan2f(sm.t[0], sm.t[2]);
                                break;
                            case 1: // perpendicular (rank-facing)
                                yaw = atan2f(perpX, perpZ);
                                break;
                            case 2: // look-at
                                yaw = atan2f(laX - px, laZ - pz);
                                break;
                            case 3: // fixed
                                yaw = fixedYaw;
                                break;
                            case 4: { // random
                                unsigned int rr = PATH_RNG_NEXT();
                                yaw = ((float)(rr & 0xFFFF) / 65535.0f)
                                       * 6.2831853f;
                                break;
                            }
                            default: yaw = 0.0f; break;
                        }

                        // jitter
                        if (posJitter > 0.0f) {
                            unsigned int jx = PATH_RNG_NEXT();
                            unsigned int jz = PATH_RNG_NEXT();
                            float fx = ((float)(jx & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
                            float fz = ((float)(jz & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
                            px += fx * posJitter;
                            pz += fz * posJitter;
                        }
                        if (yawJitter > 0.0f) {
                            unsigned int jy = PATH_RNG_NEXT();
                            float fy = ((float)(jy & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
                            yaw += fy * yawJitter;
                        }

                        // snap to ground: cheap Y=0 plane snap for now.
                        // (Proper terrain raycast can replace this later;
                        // most LOTR Conquest levels are roughly Y=0 ground.)
                        if (snapGround) py = 0.0f;

                        lsG->addCrowdInstanceToItem(archItem,
                            px, py, pz, yaw, 10000.0f);
                        ++spawned;
                    }
                }
                #undef PATH_RNG_NEXT

                if (spawned > 0) lsG->rebuildCrowdInstances();
            }
            // One-shot: zero the request flag so we don't re-spawn
            // every frame. The DLL sets it back to 1 on the next
            // Stamp click.
            args.pathPlaceRequested = 0;
        }
    }

    // ── Consume DLL→host crowd button clicks ─────────────────────────
    // Delete / Clone / LOD-commit / Playback-commit. All gated on a valid
    // (item, val) pair coming back from the DLL — if the user changed
    // selection mid-frame it might not be a crowd anymore, defend.
    {
        LevelScene* lsR = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;
        if (lsR && args.selInstIsCrowd) {
            int ii = args.selCrowdItemIdx;
            int vv = args.selCrowdValIdx;
            if (lsR->isValidCrowdInstance(ii, vv)) {
                if (args.crowdDeleteRequested) {
                    if (lsR->deleteCrowdInstance(ii, vv)) {
                        lsR->setSelectedInstance(-1);
                        lsR->rebuildCrowdInstances();
                    }
                } else if (args.crowdCloneRequested) {
                    // Same archetype, +1m on +X, same yaw + lod.
                    const std::vector<LevelCrowdItem>& items = lsR->getCrowdItems();
                    if (ii >= 0 && ii < (int)items.size()
                        && vv >= 0 && vv < (int)items[ii].instances.size())
                    {
                        const LevelCrowdInstance& src = items[ii].instances[vv];
                        lsR->addCrowdInstanceToItem(ii,
                            src.position[0] + 1.0f,
                            src.position[1],
                            src.position[2],
                            src.rotation,
                            src.lod);
                        lsR->rebuildCrowdInstances();
                    }
                } else if (args.crowdLodEditRequested) {
                    // Per-instance LOD. Use setCrowdInstancePosRot + manual
                    // lod patch — there's no setCrowdInstanceLod helper and
                    // adding one for one knob is overkill. Direct mut.
                    std::vector<LevelCrowdItem>& mitems = lsR->getCrowdItemsMut();
                    mitems[ii].instances[vv].lod = args.pendingCrowdLod;
                    lsR->setCrowdDirty(true);
                } else if (args.crowdPlaybackEditRequested) {
                    // Per-archetype playback rate — every instance in this
                    // CrowdItem shares the same speed. No rebuild needed
                    // because position/rotation didn't change.
                    std::vector<LevelCrowdItem>& mitems = lsR->getCrowdItemsMut();
                    mitems[ii].playbackRate = args.pendingCrowdPlayback;
                    lsR->setCrowdDirty(true);
                } else if (args.crowdPosRotEditRequested) {
                    // Manual XYZ / yaw edit from the Inspector text fields.
                    // setCrowdInstancePosRot handles the source-of-truth
                    // write + dirty flag; we just need to rebuild the
                    // LevelInstance vector so the world matrix + AABB
                    // reflect the new pose.
                    if (lsR->setCrowdInstancePosRot(ii, vv,
                            args.pendingCrowdPosX,
                            args.pendingCrowdPosY,
                            args.pendingCrowdPosZ,
                            args.pendingCrowdRotation))
                    {
                        lsR->rebuildCrowdInstances();
                    }
                }
            }
        }
    }

    // ── Consume 3dCrowd Editor request flags ─────────────────────────
    // Order matters: structural mutations (Delete/Duplicate/AddNew) run
    // BEFORE field edits in case the selected index moves under us. The
    // very end of the block kicks one rebuild if anything mutated, which
    // honors focus mode automatically.
    {
        LevelScene* lsE = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;
        if (lsE) {
            bool needRebuild = false;
            int sel = args.crowdEditorSelectedItem;

            // Focus toggle — flips on/off, mirrors the currently-selected
            // item. Toggling off restores all 7 on next rebuild.
            if (args.crowdEditorFocusToggleRequested) {
                bool on = !lsE->getCrowdFocusMode();
                lsE->setCrowdFocusMode(on, on ? sel : -1);
                needRebuild = true;
            }

            // Add a brand-new CrowdItem from the panel's bottom sub-form.
            // The mesh+key strings get CRC-hashed; the key string also
            // queues for pak_strings.json injection at save time so the
            // engine can resolve the user's invented name post-pack.
            if (args.crowdEditorAddNewItemRequested) {
                const char* newKey  = args.pendingCrowdEditorNewKey;
                const char* newMain = args.pendingCrowdEditorNewKeyMain;
                if (newKey[0] && newMain[0]) {
                    uint32_t keyCrc  = ZeroEngine::LotrHashString(newKey,  -1);
                    uint32_t meshCrc = ZeroEngine::LotrHashString(newMain, -1);
                    g_levelReader.QueueCustomString(newKey);
                    int idx = lsE->addCrowdItem(meshCrc, newMain,
                                                keyCrc, newKey,
                                                0, "", 0, "", 1.0f);
                    // Pre-populate model cache so the new item renders
                    // immediately. Mirror Pass-1 pattern.
                    lsE->setCrowdItemModelKey(idx, meshCrc, newMain, g_levelReader);
                    args.crowdEditorSelectedItem = idx;
                    needRebuild = true;
                }
            }

            // Duplicate selected — copies the header and instances, appends
            // "_copy" to the key. New item is at the end of m_crowdItems.
            if (args.crowdEditorDuplicateItemRequested && sel >= 0) {
                int idx = lsE->duplicateCrowdItem(sel);
                if (idx >= 0) {
                    // The new key string ("<oldname>_copy") needs to land
                    // in pak_strings.json too so the post-pack engine can
                    // resolve it. Hash it now and queue.
                    const std::vector<LevelCrowdItem>& items2 = lsE->getCrowdItems();
                    g_levelReader.QueueCustomString(items2[idx].meshKeyName);
                    args.crowdEditorSelectedItem = idx;
                    needRebuild = true;
                }
            }

            // Delete selected — gone-without-trace, selection clears.
            if (args.crowdEditorDeleteItemRequested && sel >= 0) {
                if (lsE->deleteCrowdItemAt(sel)) {
                    args.crowdEditorSelectedItem = -1;
                    needRebuild = true;
                }
            }

            // Rename `key` on selected. Hash + queue for pak_strings.
            if (args.crowdEditorRenameKeyRequested && sel >= 0) {
                const char* newName = args.pendingCrowdEditorKey;
                if (newName[0]) {
                    uint32_t crc = ZeroEngine::LotrHashString(newName, -1);
                    g_levelReader.QueueCustomString(newName);
                    lsE->renameCrowdItemKey(sel, crc, newName);
                    needRebuild = true;
                }
            }

            // key_main swap — autocomplete-fed, must be a real loaded
            // mesh. Hashing + Pass-1 cache load handled inside setter.
            if (args.crowdEditorSetKeyMainRequested && sel >= 0) {
                const char* newMain = args.pendingCrowdEditorKeyMain;
                if (newMain[0]) {
                    uint32_t crc = ZeroEngine::LotrHashString(newMain, -1);
                    lsE->setCrowdItemModelKey(sel, crc, newMain, g_levelReader);
                    needRebuild = true;
                }
            }

            // key_right / key_left — same shape, no model-cache load
            // needed because hand weapons are drawn by the engine via a
            // separate model lookup at render time, not Vespucci.
            if (args.crowdEditorSetKeyRightRequested && sel >= 0) {
                const char* n = args.pendingCrowdEditorKeyRight;
                uint32_t crc = n[0] ? ZeroEngine::LotrHashString(n, -1) : 0;
                lsE->setCrowdItemRightKey(sel, crc, n);
            }
            if (args.crowdEditorSetKeyLeftRequested && sel >= 0) {
                const char* n = args.pendingCrowdEditorKeyLeft;
                uint32_t crc = n[0] ? ZeroEngine::LotrHashString(n, -1) : 0;
                lsE->setCrowdItemLeftKey(sel, crc, n);
            }

            // Playback rate slider — no rebuild needed, draw state same.
            if (args.crowdEditorSetPlaybackRequested && sel >= 0) {
                lsE->setCrowdItemPlayback(sel, args.pendingCrowdEditorPlayback);
            }

            // Anim list — add/remove.
            if (args.crowdEditorAddAnimRequested && sel >= 0) {
                const char* n = args.pendingCrowdEditorAddAnim;
                if (n[0]) {
                    uint32_t crc = ZeroEngine::LotrHashString(n, -1);
                    lsE->addCrowdItemAnim(sel, crc, n);
                }
            }
            if (args.crowdEditorRemoveAnimRequested && sel >= 0) {
                lsE->removeCrowdItemAnim(sel, args.pendingCrowdEditorRemoveAnimIdx);
            }

            // Add instance — spawn at last instance + (+1, 0, 0) like Clone,
            // or at origin if the item has no instances yet. User can gizmo
            // it where they want. No camera-target raycast needed; matches
            // the existing crowdCloneRequested path.
            if (args.crowdEditorAddInstanceRequested && sel >= 0) {
                const std::vector<LevelCrowdItem>& items3 = lsE->getCrowdItems();
                if (sel < (int)items3.size()) {
                    float sx = 0.0f, sy = 0.0f, sz = 0.0f, sr = 0.0f;
                    if (!items3[sel].instances.empty()) {
                        const LevelCrowdInstance& last =
                            items3[sel].instances.back();
                        sx = last.position[0] + 1.0f;
                        sy = last.position[1];
                        sz = last.position[2];
                        sr = last.rotation;
                    }
                    lsE->addCrowdInstanceToItem(sel, sx, sy, sz, sr, 10000.0f);
                    needRebuild = true;
                }
            }

            // v1.4 per-instance edit + delete (in-panel, doesn't need
            // viewport pick). selInst is bounds-checked against the
            // current item's instance count before either action runs.
            if (sel >= 0) {
                int vi = args.crowdEditorSelInst;
                const std::vector<LevelCrowdItem>& items4 = lsE->getCrowdItems();
                if (sel < (int)items4.size()
                    && vi >= 0 && vi < (int)items4[sel].instances.size())
                {
                    if (args.crowdEditorInstEditRequested) {
                        if (lsE->setCrowdInstancePosRot(sel, vi,
                                args.pendingCrowdEditorInstPos[0],
                                args.pendingCrowdEditorInstPos[1],
                                args.pendingCrowdEditorInstPos[2],
                                args.pendingCrowdEditorInstRot))
                        {
                            // LOD lives on the instance too; direct mut
                            // since there's no setter for just the float.
                            std::vector<LevelCrowdItem>& mut4 =
                                lsE->getCrowdItemsMut();
                            mut4[sel].instances[vi].lod =
                                args.pendingCrowdEditorInstLod;
                            lsE->setCrowdDirty(true);
                            needRebuild = true;
                        }
                    }
                    if (args.crowdEditorDeleteInstRequested) {
                        if (lsE->deleteCrowdInstance(sel, vi)) {
                            // Adjusting selInst so we don't dangle: clamp
                            // to the new max or clear if list emptied.
                            int newCount =
                                (int)lsE->getCrowdItems()[sel].instances.size();
                            if (newCount == 0) g_crowdEditorSelInst = -1;
                            else if (vi >= newCount) g_crowdEditorSelInst = newCount - 1;
                            needRebuild = true;
                        }
                    }
                }
            }

            if (needRebuild) lsE->rebuildCrowdInstances();
        }
        // Persist the (possibly DLL-changed, possibly host-changed)
        // selection back into the file-scope static so the next
        // frame's pre-DrawFrame populate seeds args correctly.
        g_crowdEditorSelectedItem = args.crowdEditorSelectedItem;
        g_crowdEditorSelInst      = args.crowdEditorSelInst;
    }

    // ── Consume Crowd Mesh Builder state + requests ──────────────────
    // Three responsibilities every frame:
    //   1. Mirror DLL-side picker changes back into the host statics so
    //      the next pre-DrawFrame populate has the latest selection.
    //   2. Manage the preview LevelInstance lifecycle — spawn the base +
    //      weapon pair when the user has a valid full pick AND the panel
    //      is open AND something changed since last frame; clear them
    //      when the panel closes or the pick gets emptied.
    //   3. Consume the one-shot Bake request: build a CrowdBuilderMergeSpec
    //      record, push it onto the pending list, queue the new model
    //      name for pak_strings injection.
    {
        // (1) Persist DLL-driven picker changes.
        g_crowdBuilderSelBaseIdx   = args.crowdBuilderSelBaseIdx;
        g_crowdBuilderSelWeaponIdx = args.crowdBuilderSelWeaponIdx;
        g_crowdBuilderSelBoneIdx   = args.crowdBuilderSelBoneIdx;
        strncpy_s(g_crowdBuilderSelBaseName,   96, args.crowdBuilderSelBaseName,   _TRUNCATE);
        strncpy_s(g_crowdBuilderSelWeaponName, 96, args.crowdBuilderSelWeaponName, _TRUNCATE);
        strncpy_s(g_crowdBuilderSelBoneName,   64, args.crowdBuilderSelBoneName,   _TRUNCATE);
        g_crowdBuilderOffsetXYZ[0] = args.crowdBuilderOffsetXYZ[0];
        g_crowdBuilderOffsetXYZ[1] = args.crowdBuilderOffsetXYZ[1];
        g_crowdBuilderOffsetXYZ[2] = args.crowdBuilderOffsetXYZ[2];
        g_crowdBuilderOffsetYPR[0] = args.crowdBuilderOffsetYawPitchRoll[0];
        g_crowdBuilderOffsetYPR[1] = args.crowdBuilderOffsetYawPitchRoll[1];
        g_crowdBuilderOffsetYPR[2] = args.crowdBuilderOffsetYawPitchRoll[2];
        strncpy_s(g_crowdBuilderNewMeshName, 96, args.crowdBuilderNewMeshName, _TRUNCATE);
        // Embed-mode flags. Checkbox + UV target rect set by the user
        // in the Crowd Mesh Builder panel.
        g_crowdBuilderEmbedMode = args.crowdBuilderEmbedMode;
        for (int i = 0; i < 4; ++i)
            g_crowdBuilderEmbedUVRect[i] = args.crowdBuilderEmbedUVRect[i];

        // Auto-spawn state: rescue the user's checkbox, position drag,
        // and instance count from the goddamn per-frame memset of args.
        // Without this persistence the panel would forget every drag the
        // moment the next DrawFrame fires, which would feel like typing
        // into a void and is the exact horseshit we built the host-side
        // statics pattern to avoid for every other panel in this nightmare.
        g_crowdBuilderAutoSpawn  = args.crowdBuilderAutoSpawn;
        g_crowdBuilderSpawnCount = args.crowdBuilderSpawnCount > 0
                                   ? args.crowdBuilderSpawnCount : 3;
        g_crowdBuilderSpawnPos[0] = args.crowdBuilderSpawnPos[0];
        g_crowdBuilderSpawnPos[1] = args.crowdBuilderSpawnPos[1];
        g_crowdBuilderSpawnPos[2] = args.crowdBuilderSpawnPos[2];

        LevelScene* lsB = (g_renderer && g_renderer->hasLevelScene())
                          ? g_renderer->getLevelScene() : NULL;

        // (2) Preview lifecycle. Three states matter:
        //   - panel closed OR preview-flag off  -> clear, leave alone
        //   - panel open + preview-flag on, but no full pick -> clear
        //   - panel open + preview-flag on + full pick + selection
        //     changed since last frame -> clear-then-respawn
        // A selection is "full" when base+weapon+bone all have valid idxs.
        bool panelOpen      = (args.crowdBuilderOpen != 0);
        bool previewWanted  = panelOpen && (args.crowdBuilderPreviewActive != 0);
        bool fullPick       = (args.crowdBuilderSelBaseIdx   >= 0
                            && args.crowdBuilderSelWeaponIdx >= 0
                            && args.crowdBuilderSelBoneIdx   >= 0
                            && args.crowdBuilderSelBaseName[0]
                            && args.crowdBuilderSelWeaponName[0]
                            && args.crowdBuilderSelBoneName[0]);

        if (lsB) {
            if (!previewWanted || !fullPick) {
                // Clear if either the panel is closed or the pick isn't
                // ready. Cheap when no previews exist — clearPreviewMeshes
                // walks zero entries.
                lsB->clearPreviewMeshes();
                g_crowdBuilderLastPreviewBaseIdx   = -2;
                g_crowdBuilderLastPreviewWeaponIdx = -2;
                g_crowdBuilderLastPreviewBoneIdx   = -2;
            } else {
                // Did the user change anything that affects the preview
                // pair's identity (model swap, bone swap)? Offset changes
                // alone DON'T trigger a respawn — Phase 3 part B will
                // re-derive mat[] from offset each frame, so spawning
                // once and letting the user slide is the right shape.
                bool changed =
                    (args.crowdBuilderSelBaseIdx   != g_crowdBuilderLastPreviewBaseIdx)
                 || (args.crowdBuilderSelWeaponIdx != g_crowdBuilderLastPreviewWeaponIdx)
                 || (args.crowdBuilderSelBoneIdx   != g_crowdBuilderLastPreviewBoneIdx);

                if (changed) {
                    lsB->clearPreviewMeshes();
                    // Spawn base mesh at world origin as a non-following
                    // preview (parentInstIdx = -1 means it's its own
                    // anchor — Phase 3 part B's matrix recompute will
                    // treat any preview with parentInstIdx < 0 as a
                    // root). Then spawn weapon parented to base's bone.
                    uint32_t baseCrc =
                        ZeroEngine::LotrHashString(args.crowdBuilderSelBaseName, -1);
                    uint32_t wpCrc   =
                        ZeroEngine::LotrHashString(args.crowdBuilderSelWeaponName, -1);

                    // The base preview itself uses previewParentIdx = 0
                    // so it gets the preview-flag treatment (un-pickable,
                    // cleared on close) but no actual parent-follow math
                    // — Phase 3 part B's renderer code will branch on
                    // previewParentBoneIdx < 0 = anchor at identity.
                    // For now both spawn at identity which is fine.
                    float zeroOff[3] = {0,0,0};
                    int baseIdx = lsB->spawnPreviewMesh(baseCrc, /*parent*/-1,
                                                        /*bone*/-1,
                                                        zeroOff, zeroOff);
                    if (baseIdx >= 0) {
                        // baseIdx came back as a regular preview slot
                        // — flag it as anchor by leaving parent < 0,
                        // which the clear path already handles. The
                        // weapon previewParentIdx points at baseIdx.
                        int boneIdx = lsB->findBoneIndexByName(baseCrc,
                            std::string(args.crowdBuilderSelBoneName));
                        lsB->spawnPreviewMesh(wpCrc, baseIdx, boneIdx,
                                              g_crowdBuilderOffsetXYZ,
                                              g_crowdBuilderOffsetYPR);
                    }
                    g_crowdBuilderLastPreviewBaseIdx   = args.crowdBuilderSelBaseIdx;
                    g_crowdBuilderLastPreviewWeaponIdx = args.crowdBuilderSelWeaponIdx;
                    g_crowdBuilderLastPreviewBoneIdx   = args.crowdBuilderSelBoneIdx;
                }
            }
        }

        // Persist strip sub-panel state and consume Strip request.
        g_stripSourceSelIdx = args.stripSourceSelIdx;
        strncpy_s(g_stripSourceSelName, 96, args.stripSourceSelName, _TRUNCATE);
        strncpy_s(g_stripNewName,       96, args.stripNewName,       _TRUNCATE);
        for (int b = 0; b < 128; ++b) {
            g_stripBoneSelected[b] = args.stripBoneSelected[b];
        }
        // ── Mesh Surgery: persist DLL changes + handle requests ─────────
        g_meshSurgerySelSourceIdx = args.meshSurgerySelSourceIdx;
        strncpy_s(g_meshSurgerySelSourceName, 96, args.meshSurgerySelSourceName, _TRUNCATE);
        strncpy_s(g_meshSurgeryNewName, 96, args.meshSurgeryNewName, _TRUNCATE);
        for (int i = 0; i < 4; ++i) g_meshSurgerySelectionUVRect[i] = args.meshSurgerySelectionUVRect[i];

        if (args.meshSurgeryAutoFindRequested && args.meshSurgerySelSourceName[0]) {
            DebugLog("[MeshSurgery] Auto-find clicked. Spinning up the spatial-outlier sniffer.");
            // Phase 1 v2 — spawn Python in-process to actually run the
            // heuristic now. We invoke a one-liner that imports
            // auto_find_weapon_uv_box from level_patcher and prints the UV
            // bounds. Tiny synchronous call — no PAK dump needed since the
            // function reads the source JSON directly from the dump dir.
            std::string srcName = args.meshSurgerySelSourceName;
            DebugLogf("[MeshSurgery]   source = '%s'", srcName.c_str());
            // dump dir = loaded PAK path without the .PAK ext. Same shape
            // RustParserSaveLevel uses for its sidecars.
            std::string pakPath = g_levelReader.GetPakPath();
            DebugLogf("[MeshSurgery]   loaded PAK path = '%s'", pakPath.c_str());
            std::string dumpDir;
            if (!pakPath.empty()) {
                // Strip ".PAK" if present, that's the convention for the
                // dump-dir layout. e.g. ".../Helm'sDeep.PAK" -> ".../Helm'sDeep"
                dumpDir = pakPath;
                size_t pakExt = dumpDir.rfind(".PAK");
                if (pakExt == std::string::npos) pakExt = dumpDir.rfind(".pak");
                if (pakExt != std::string::npos) dumpDir = dumpDir.substr(0, pakExt);
            }
            std::string jsonPath = dumpDir + "/models/" + srcName + ".json";

            ImGuiGlueFrameArgs* mut = const_cast<ImGuiGlueFrameArgs*>(&args);
            // Sentinel-arm the selection rect (so a subsequent Strip click
            // routes through __AUTO__ in the spec):
            mut->meshSurgerySelectionUVRect[0] = -1.0f;
            mut->meshSurgerySelectionUVRect[1] = -1.0f;
            mut->meshSurgerySelectionUVRect[2] = -1.0f;
            mut->meshSurgerySelectionUVRect[3] = -1.0f;
            mut->meshSurgerySelectedVertCount  = -1;
            for (int i = 0; i < 4; ++i)
                g_meshSurgerySelectionUVRect[i] = -1.0f;
            g_meshSurgerySelectedVertCount = -1;

            // Run Python to compute the preview bbox so the user sees
            // what auto-find would catch BEFORE clicking Strip.
            std::string scene3dDir;
            {
                char exeDir[MAX_PATH] = {0};
                GetModuleFileNameA(NULL, exeDir, MAX_PATH);
                char* slash = strrchr(exeDir, '\\');
                if (slash) *slash = 0;
                scene3dDir = std::string(exeDir) + "\\..\\Scene3D\\Vespucci\\Tools\\Python";
            }
            char pyCmd[2048];
            _snprintf(pyCmd, sizeof(pyCmd),
                "python -c \"import sys, json; "
                "sys.path.insert(0, r'%s'); "
                "from level_patcher import auto_find_weapon_uv_box; "
                "j = json.load(open(r'%s','r')); "
                "b = auto_find_weapon_uv_box(j); "
                "print('%%g %%g %%g %%g' %% b if b else 'NONE')\"",
                scene3dDir.c_str(), jsonPath.c_str());
            pyCmd[sizeof(pyCmd)-1] = 0;
            FILE* pp = _popen(pyCmd, "r");
            float u0=0, v0=0, u1=0, v1=0; bool ok = false;
            if (pp) {
                char out[256] = {0};
                if (fgets(out, sizeof(out), pp)) {
                    if (sscanf(out, "%f %f %f %f", &u0, &v0, &u1, &v1) == 4)
                        ok = true;
                }
                _pclose(pp);
            }

            if (ok) {
                mut->meshSurgeryAutoFindPreview[0] = u0;
                mut->meshSurgeryAutoFindPreview[1] = v0;
                mut->meshSurgeryAutoFindPreview[2] = u1;
                mut->meshSurgeryAutoFindPreview[3] = v1;
                mut->meshSurgeryAutoFindPreviewValid = 1;
                _snprintf(g_meshSurgeryStatusText, 256,
                          "Auto-find sniffed out a weapon corpse at UV U[%.3f,%.3f] V[%.3f,%.3f] "
                          "on %s. Name the corpse, hit Strip & Queue, walk away.",
                          u0, u1, v0, v1, srcName.c_str());
            } else {
                mut->meshSurgeryAutoFindPreviewValid = 0;
                _snprintf(g_meshSurgeryStatusText, 256,
                          "Auto-find: %s has NO weapon outliers. Either the rig is clean "
                          "or this mesh hides its weapons inside the body like a sneaky "
                          "fucker. Draw a UV box by hand and try that.", srcName.c_str());
            }
            g_meshSurgeryStatusText[255] = 0;
            mut->meshSurgeryAutoFindRequested = 0;
        }

        if (args.meshSurgeryStripRequested && lsB) {
            DebugLogf("[MeshSurgery] Strip clicked: source='%s' out='%s' rect=[%.4f,%.4f,%.4f,%.4f]",
                      args.meshSurgerySelSourceName,
                      args.meshSurgeryNewName,
                      args.meshSurgerySelectionUVRect[0],
                      args.meshSurgerySelectionUVRect[1],
                      args.meshSurgerySelectionUVRect[2],
                      args.meshSurgerySelectionUVRect[3]);
            // Build a strip spec using the UV rect from the panel. Reuses
            // the existing s_pendingStripSpecs / ze_strip_specs.json pipeline
            // which already invokes apply_strip_specs at save time. The
            // function currently strips by BONE; UV-box selection in apply
            // is an enhancement we wire next (the Python side already
            // supports it conceptually — extend the spec schema with a
            // uv_box field).
            StripSpec sp;
            sp.new_name = args.meshSurgeryNewName;
            sp.source   = args.meshSurgerySelSourceName;
            // Auto mode is signaled by the sentinel rect (all -1) — pass
            // "__AUTO__" so Python runs auto_find_weapon_uv_box at save.
            // Otherwise pass the explicit UV bounds.
            char buf[64];
            bool isAuto = (args.meshSurgerySelectionUVRect[0] < 0
                        && args.meshSurgerySelectionUVRect[2] < 0);
            if (isAuto) {
                strncpy(buf, "__AUTO__", sizeof(buf));
            } else {
                _snprintf(buf, sizeof(buf), "__UV_BOX__%.4f,%.4f,%.4f,%.4f",
                          args.meshSurgerySelectionUVRect[0],
                          args.meshSurgerySelectionUVRect[1],
                          args.meshSurgerySelectionUVRect[2],
                          args.meshSurgerySelectionUVRect[3]);
            }
            buf[sizeof(buf)-1] = 0;
            sp.bones.push_back(std::string(buf));
            if (!sp.new_name.empty() && !sp.source.empty()) {
                s_pendingStripSpecs.push_back(sp);
                g_levelReader.QueueCustomString(sp.new_name);
                DebugLogf("[MeshSurgery]   queued spec #%d: %s -> %s, payload='%s'",
                          (int)s_pendingStripSpecs.size(), sp.source.c_str(),
                          sp.new_name.c_str(), sp.bones[0].c_str());
                _snprintf(g_meshSurgeryStatusText, 256,
                          "Strip spec queued: gutting %s -> %s on UV patch "
                          "U[%.3f,%.3f] V[%.3f,%.3f]. Hit Save Level so lotrc_rs "
                          "can finish the goddamn job.",
                          sp.source.c_str(), sp.new_name.c_str(),
                          args.meshSurgerySelectionUVRect[0],
                          args.meshSurgerySelectionUVRect[2],
                          args.meshSurgerySelectionUVRect[1],
                          args.meshSurgerySelectionUVRect[3]);
                g_meshSurgeryStatusText[255] = 0;
            } else {
                DebugLogf("[MeshSurgery]   spec REJECTED: empty new_name or source. The user clicked Strip with the picks half-filled. Button gate failed, we caught it here. Yell at the UI, not the pipeline.");
            }
            ImGuiGlueFrameArgs* mut = const_cast<ImGuiGlueFrameArgs*>(&args);
            mut->meshSurgeryStripRequested = 0;
        }

        if (args.stripRequested && lsB) {
            // Collect selected bone names by walking the populated option
            // list + the parallel selected flags. Same dance as the bake
            // request — we capture a spec and consume the one-shot flag.
            StripSpec sp;
            sp.new_name = args.stripNewName;
            sp.source   = args.stripSourceSelName;
            for (int b = 0; b < args.stripBoneOptionCount; ++b) {
                if (args.stripBoneSelected[b]) {
                    sp.bones.push_back(std::string(args.stripBoneOptions[b]));
                }
            }
            if (!sp.new_name.empty() && !sp.source.empty() && !sp.bones.empty()) {
                s_pendingStripSpecs.push_back(sp);
                // Queue the new name for pak_strings injection so the
                // recompiled PAK can resolve it.
                g_levelReader.QueueCustomString(sp.new_name);
                ImGuiGlueFrameArgs* mut = const_cast<ImGuiGlueFrameArgs*>(&args);
                mut->stripRequested = 0;
            } else {
                ImGuiGlueFrameArgs* mut = const_cast<ImGuiGlueFrameArgs*>(&args);
                mut->stripRequested = 0;
            }
        }

        // (3) Bake request — one-shot. Build a merge spec, queue the
        // new model's name for pak_strings, push the spec onto the
        // pending list. The actual sidecar file write happens at save
        // time so multiple bakes in one session batch into one JSON.
        if (args.crowdBuilderBakeRequested && fullPick
            && args.crowdBuilderNewMeshName[0])
        {
            CrowdBuilderMergeSpec spec;
            spec.newName    = args.crowdBuilderNewMeshName;
            spec.baseName   = args.crowdBuilderSelBaseName;
            spec.weaponName = args.crowdBuilderSelWeaponName;
            spec.attachBone = args.crowdBuilderSelBoneName;
            spec.offsetXYZ[0] = g_crowdBuilderOffsetXYZ[0];
            spec.offsetXYZ[1] = g_crowdBuilderOffsetXYZ[1];
            spec.offsetXYZ[2] = g_crowdBuilderOffsetXYZ[2];
            spec.offsetYPR[0] = g_crowdBuilderOffsetYPR[0];
            spec.offsetYPR[1] = g_crowdBuilderOffsetYPR[1];
            spec.offsetYPR[2] = g_crowdBuilderOffsetYPR[2];
            spec.embedMode    = g_crowdBuilderEmbedMode;
            spec.embedUVRect[0] = g_crowdBuilderEmbedUVRect[0];
            spec.embedUVRect[1] = g_crowdBuilderEmbedUVRect[1];
            spec.embedUVRect[2] = g_crowdBuilderEmbedUVRect[2];
            spec.embedUVRect[3] = g_crowdBuilderEmbedUVRect[3];
            spec.autoSpawn   = g_crowdBuilderAutoSpawn;
            spec.spawnCount  = g_crowdBuilderSpawnCount > 0 ? g_crowdBuilderSpawnCount : 3;
            spec.spawnPos[0] = g_crowdBuilderSpawnPos[0];
            spec.spawnPos[1] = g_crowdBuilderSpawnPos[1];
            spec.spawnPos[2] = g_crowdBuilderSpawnPos[2];
            DebugLogf("[CrowdBuilder] Bake clicked: new='%s' base='%s' wp='%s' bone='%s' "
                      "embed=%d uvRect=[%.4f,%.4f,%.4f,%.4f] offsetXYZ=[%.3f,%.3f,%.3f] YPR=[%.3f,%.3f,%.3f] "
                      "autoSpawn=%d count=%d pos=(%.2f,%.2f,%.2f)",
                      spec.newName.c_str(), spec.baseName.c_str(),
                      spec.weaponName.c_str(), spec.attachBone.c_str(),
                      spec.embedMode,
                      spec.embedUVRect[0], spec.embedUVRect[1], spec.embedUVRect[2], spec.embedUVRect[3],
                      spec.offsetXYZ[0], spec.offsetXYZ[1], spec.offsetXYZ[2],
                      spec.offsetYPR[0], spec.offsetYPR[1], spec.offsetYPR[2],
                      spec.autoSpawn, spec.spawnCount,
                      spec.spawnPos[0], spec.spawnPos[1], spec.spawnPos[2]);
            s_crowdBuilderMergeSpecs.push_back(spec);
            // The merged model name has to land in pak_strings.json so
            // post-pack ResolveCrc(merged_name_crc) can find it. The
            // engine looks every CRC up by hash; without the string
            // table entry the merged mesh would be a ghost.
            g_levelReader.QueueCustomString(spec.newName);
            char logBuf[256];
            sprintf_s(logBuf,
                "CrowdMeshBuilder: bake queued '%s' = %s + %s @ %s "
                "(pending=%d)",
                spec.newName.c_str(), spec.baseName.c_str(),
                spec.weaponName.c_str(), spec.attachBone.c_str(),
                (int)s_crowdBuilderMergeSpecs.size());
            DebugLog(logBuf);
        }
    }

    // Carry forward DLL's Level Blocks selection for next frame's property lookup.
    // The DLL writes levelBlockSelGuid during DrawFrame, but properties are populated
    // BEFORE DrawFrame. This static bridges the one-frame gap.
    s_levelBlockSelGuid = args.levelBlockSelGuid;

    // Carry forward cinematic panel state from DLL
    g_cinePanelOpen = (args.cinePanelOpen != 0);
    g_cinePanelSelCam = args.cinePanelSelCamera;

    // ── Cinematic playback control from DLL ──
    if (args.cinePlayRequested == 1 && g_renderer && g_renderer->hasLevelScene()) {
        // Start playback
        LevelScene* ls = g_renderer->getLevelScene();
        const std::vector<LevelCinematicCamera>& cams = ls->getCinematicCameras();
        int ci = args.cineCameraIndex;
        if (ci >= 0 && ci < (int)cams.size()) {
            g_cineActive = true;
            g_cineCamIdx = ci;
            g_cineTime = 0.0f;
            g_cineDuration = cams[ci].totalDuration;
        }
    } else if (args.cinePlayRequested == 2) {
        // Stop playback
        g_cineActive = false;
        g_cineCamIdx = -1;
        if (g_renderer) g_renderer->setCinematicOverride(false, 0,0,0, 0,0,0, 60);
    }

    // ── Cinematic property edit from DLL ──
    if (args.cineEditRequested && g_levelReader.IsLoaded()) {
        uint32_t editGuid = args.cineEditGuid;
        const char* editField = args.cineEditField;
        if (editGuid != 0 && editField[0]) {
            const std::vector<ZeroEngine::LevelGameObjEntry>& objs = g_levelReader.GetGameObjs();
            const std::vector<ZeroEngine::LevelGameObjTypeDef>& types = g_levelReader.GetGameObjTypes();
            for (int ei = 0; ei < (int)objs.size(); ++ei) {
                if (objs[ei].guid != editGuid) continue;
                const ZeroEngine::LevelGameObjEntry& ent = objs[ei];
                if (ent.type_def_index < 0 || ent.type_def_index >= (int)types.size()) break;
                const ZeroEngine::LevelGameObjTypeDef& td = types[ent.type_def_index];
                for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                    if (_stricmp(td.fields[fi].name.c_str(), editField) != 0) continue;
                    uint32_t absOff = ent.block1_obj_offset + td.fields[fi].offset;
                    ZeroEngine::FieldEdit fe;
                    memset(&fe.vec3Val, 0, sizeof(fe.vec3Val));
                    memset(&fe.matrixVal, 0, sizeof(fe.matrixVal));
                    fe.entityGuid = editGuid;
                    fe.fieldName = editField;
                    fe.kind = args.cineEditKind;
                    fe.intVal = args.cineEditIntVal;
                    fe.floatVal = args.cineEditFloatVal;
                    fe.guidVal = 0; fe.listIndex = 0; fe.arrayStride = 0;
                    switch (args.cineEditKind) {
                        case 0: g_levelReader.SetFieldValue(absOff, &args.cineEditIntVal, 4); break;
                        case 1: g_levelReader.SetFieldValue(absOff, &args.cineEditFloatVal, 4); break;
                        case 2:
                            fe.guidVal = args.cineEditGuidVal;
                            g_levelReader.SetFieldValue(absOff, &args.cineEditGuidVal, 4);
                            break;
                        case 4:
                            memcpy(fe.matrixVal, args.cineEditMatrixVal, 64);
                            g_levelReader.SetFieldValue(absOff, args.cineEditMatrixVal, 64);
                            break;
                        case 5: {
                            fe.stringVal = args.cineEditStringVal;
                            uint32_t crc = ZeroEngine::LotrHashString(args.cineEditStringVal);
                            g_levelReader.SetFieldValue(absOff, &crc, 4);
                        } break;
                    }
                    g_levelReader.AddFieldEdit(fe);
                    break;
                }
                break;
            }
        }
    }

    // ── SplineEditor (3D handles + FABRIK + curvature + extend arrows) ──
    // Runs BEFORE the cineNodeEditRequested handler below so any commit
    // SplineEditor makes flows through the existing AddFieldEdit writeback
    // path. Reuses cineNodeEditRequested / cineNodeEditData / cineNodeEditGuid.
    // Zero new persistence plumbing. The whole module is in SplineEditor.cpp
    // so the viewport stays the fuck out of the spline-editor business.
    if (g_renderer && g_renderer->hasLevelScene() && g_levelReader.IsLoaded()) {
        POINT spt; GetCursorPos(&spt);
        HWND splTarget = g_viewportHwnd ? g_viewportHwnd : g_hwnd;
        ScreenToClient(splTarget, &spt);
        RECT splRc; GetClientRect(splTarget, &splRc);
        int splVpW = splRc.right - splRc.left;
        int splVpH = splRc.bottom - splRc.top;
        SplineEditor::Update(args,
                             g_renderer->getLevelScene(),
                             &g_levelReader,
                             (float)spt.x, (float)spt.y,
                             splVpW, splVpH,
                             g_leftMouseDown,
                             g_leftMouseClicked);
        // Path Place tool. Same viewport input, different node array.
        // Click in empty space to add a node on ground plane, drag a
        // handle to move, right-click to pop the last node. Consume
        // the right-click ONLY when path-place is active and has
        // nodes to pop, so we don't steal RMB from the entity-create
        // context menu when the path tool isn't drawing.
        bool rmbForPathPlace = g_rightMouseClicked
                              && args.pathPlacePreviewActive
                              && args.pathPlaceNodeCount > 0;
        if (rmbForPathPlace) g_rightMouseClicked = false;
        SplineEditor::UpdateExternalPath(args,
                             g_renderer->getLevelScene(),
                             (float)spt.x, (float)spt.y,
                             splVpW, splVpH,
                             g_leftMouseDown,
                             g_leftMouseClicked,
                             rmbForPathPlace);
    }

    // ── Spline node edit from DLL ──
    if (args.cineNodeEditRequested && args.cineNodeEditGuid != 0 && args.cineNodeEditData &&
        g_levelReader.IsLoaded() && g_renderer && g_renderer->hasLevelScene())
    {
        uint32_t splGuid = args.cineNodeEditGuid;
        int newCount = args.cineNodeEditCount;
        const float* nodeData = args.cineNodeEditData;

        // Update LevelScene spline (viewport)
        LevelScene* ls = g_renderer->getLevelScene();
        std::vector<LevelSpline>& splines = ls->getSplinesMut();
        for (size_t si = 0; si < splines.size(); ++si) {
            if (splines[si].guid != splGuid) continue;
            splines[si].nodes.resize(newCount);
            for (int ni = 0; ni < newCount; ++ni) {
                splines[si].nodes[ni].x = nodeData[ni*4+0];
                splines[si].nodes[ni].y = nodeData[ni*4+1];
                splines[si].nodes[ni].z = nodeData[ni*4+2];
                splines[si].nodes[ni].s = nodeData[ni*4+3];
            }
            break;
        }

        // Update gameObj spline_nodes
        std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjsMut();
        for (size_t gi = 0; gi < gos.size(); ++gi) {
            if (gos[gi].guid != splGuid) continue;
            gos[gi].spline_nodes.resize(newCount);
            for (int ni = 0; ni < newCount; ++ni) {
                gos[gi].spline_nodes[ni].x = nodeData[ni*4+0];
                gos[gi].spline_nodes[ni].y = nodeData[ni*4+1];
                gos[gi].spline_nodes[ni].z = nodeData[ni*4+2];
                gos[gi].spline_nodes[ni].s = nodeData[ni*4+3];
            }
            break;
        }

        // Create FieldEdit for persistence
        ZeroEngine::FieldEdit fe;
        memset(&fe.vec3Val, 0, sizeof(fe.vec3Val));
        memset(&fe.matrixVal, 0, sizeof(fe.matrixVal));
        fe.entityGuid = splGuid;
        fe.fieldName = "Nodes";
        fe.kind = 7;
        fe.intVal = 0; fe.floatVal = 0; fe.guidVal = 0; fe.listIndex = 0;
        fe.arrayStride = 4;
        fe.arrayVal.resize(newCount * 4);
        memcpy(&fe.arrayVal[0], nodeData, newCount * 4 * sizeof(float));
        g_levelReader.AddFieldEdit(fe);
    }

    // ── Cinematic playback tick — interpolate splines, drive camera ──
    if (g_cineActive && g_renderer && g_renderer->hasLevelScene()) {
        LevelScene* ls = g_renderer->getLevelScene();
        const std::vector<LevelCinematicCamera>& cams = ls->getCinematicCameras();
        if (g_cineCamIdx >= 0 && g_cineCamIdx < (int)cams.size()) {
            const LevelCinematicCamera& cam = cams[g_cineCamIdx];
            g_cineTime += 0.016f; // ~60fps
            if (g_cineTime > cam.totalDuration) {
                g_cineActive = false;
                g_renderer->setCinematicOverride(false, 0,0,0, 0,0,0, 60);
            } else {
                // Interpolate position spline
                float eyeX = 0, eyeY = 0, eyeZ = 0;
                if (cam.positionTrack && !cam.positionTrack->nodes.empty()) {
                    float t = (cam.positionTravelTime > 0) ? g_cineTime / cam.positionTravelTime : 0;
                    if (t > 1.0f) t = 1.0f;
                    float fi = t * (cam.positionTrack->nodes.size() - 1);
                    int i0 = (int)fi;
                    float frac = fi - i0;
                    if (i0 >= (int)cam.positionTrack->nodes.size() - 1) {
                        i0 = (int)cam.positionTrack->nodes.size() - 1; frac = 0;
                    }
                    int i1 = i0 + 1;
                    if (i1 >= (int)cam.positionTrack->nodes.size()) i1 = i0;
                    const LevelSpline::Node& n0 = cam.positionTrack->nodes[i0];
                    const LevelSpline::Node& n1 = cam.positionTrack->nodes[i1];
                    eyeX = n0.x + (n1.x - n0.x) * frac;
                    eyeY = n0.y + (n1.y - n0.y) * frac;
                    eyeZ = n0.z + (n1.z - n0.z) * frac;
                }
                // Interpolate target spline
                float tgtX = eyeX, tgtY = eyeY, tgtZ = eyeZ + 10;
                if (cam.targetTrack && !cam.targetTrack->nodes.empty()) {
                    float t = (cam.targetTravelTime > 0) ? g_cineTime / cam.targetTravelTime : 0;
                    if (t > 1.0f) t = 1.0f;
                    float fi = t * (cam.targetTrack->nodes.size() - 1);
                    int i0 = (int)fi;
                    float frac = fi - i0;
                    if (i0 >= (int)cam.targetTrack->nodes.size() - 1) {
                        i0 = (int)cam.targetTrack->nodes.size() - 1; frac = 0;
                    }
                    int i1 = i0 + 1;
                    if (i1 >= (int)cam.targetTrack->nodes.size()) i1 = i0;
                    const LevelSpline::Node& n0 = cam.targetTrack->nodes[i0];
                    const LevelSpline::Node& n1 = cam.targetTrack->nodes[i1];
                    tgtX = n0.x + (n1.x - n0.x) * frac;
                    tgtY = n0.y + (n1.y - n0.y) * frac;
                    tgtZ = n0.z + (n1.z - n0.z) * frac;
                }
                g_renderer->setCinematicOverride(true, eyeX, eyeY, eyeZ, tgtX, tgtY, tgtZ, cam.fov);
            }
        }
    }

    // Handle property edit write-back
    if (args.propEditRequested && propEntityIdx >= 0 && g_levelReader.IsLoaded()) {
        const std::vector<ZeroEngine::LevelGameObjEntry>& objs = g_levelReader.GetGameObjs();
        const ZeroEngine::LevelGameObjEntry& ent = objs[propEntityIdx];
        int fi = args.propEditFieldIndex;
        if (fi >= 0 && fi < args.propFieldCount) {
            uint32_t absOff = ent.block1_obj_offset + args.propFieldOffsets[fi];

            // Track the edit for save persistence
            ZeroEngine::FieldEdit fe;
            memset(&fe, 0, sizeof(fe));
            fe.entityGuid = ent.guid;
            fe.fieldName = args.propFieldNames[fi] ? args.propFieldNames[fi] : "";
            fe.kind = args.propEditKind;

            switch (args.propEditKind) {
                case 0:
                    g_levelReader.SetFieldValue(absOff, &args.propEditIntVal, 4);
                    fe.intVal = args.propEditIntVal;
                    break;
                case 1:
                    g_levelReader.SetFieldValue(absOff, &args.propEditFloatVal, 4);
                    fe.floatVal = args.propEditFloatVal;
                    break;
                case 2:
                    g_levelReader.SetFieldValue(absOff, &args.propEditGuidVal, 4);
                    fe.guidVal = args.propEditGuidVal;
                    break;
                case 3:
                    g_levelReader.SetFieldValue(absOff, args.propEditVec3Val, 12);
                    memcpy(fe.vec3Val, args.propEditVec3Val, 12);
                    break;
                case 4:
                    g_levelReader.SetFieldValue(absOff, args.propEditMatrixVal, 64);
                    memcpy(fe.matrixVal, args.propEditMatrixVal, 64);
                    break;
                case 5: {
                    uint32_t crc = ZeroEngine::LotrHashString(args.propEditStringVal);
                    g_levelReader.SetFieldValue(absOff, &crc, 4);
                    fe.stringVal = args.propEditStringVal ? args.propEditStringVal : "";
                } break;
                case 6: {
                    uint16_t listCount = 0, listDataOff = 0;
                    g_levelReader.GetFieldValue(absOff, &listCount, 2);
                    g_levelReader.GetFieldValue(absOff + 2, &listDataOff, 2);
                    int listIdx = args.propEditIntVal;
                    if (listIdx >= 0 && listIdx < listCount) {
                        uint32_t itemOff = absOff + 4 + listDataOff + listIdx * 4;
                        g_levelReader.SetFieldValue(itemOff, &args.propEditGuidVal, 4);
                    }
                    fe.listIndex = args.propEditIntVal;
                    fe.guidVal = args.propEditGuidVal;
                } break;
            }
            g_levelReader.AddFieldEdit(fe);
        }
    }

    // Multi-select property edit: apply Team to every selected entity that has
    // a Team field. This reuses the normal FieldEdit path so save/collab see it.
    if (args.multiEditTeamRequested && g_levelReader.IsLoaded() &&
        args.multiEditGuidCount > 0 && args.multiEditGuids)
    {
        const std::vector<ZeroEngine::LevelGameObjEntry>& objs = g_levelReader.GetGameObjs();
        const std::vector<ZeroEngine::LevelGameObjTypeDef>& types = g_levelReader.GetGameObjTypes();
        int changed = 0;
        for (int gi = 0; gi < args.multiEditGuidCount; ++gi) {
            uint32_t guid = args.multiEditGuids[gi];
            const ZeroEngine::LevelGameObjEntry* ent = NULL;
            for (size_t oi = 0; oi < objs.size(); ++oi) {
                if (objs[oi].guid == guid) { ent = &objs[oi]; break; }
            }
            if (!ent || ent->block1_obj_offset == 0 ||
                ent->type_def_index < 0 || ent->type_def_index >= (int)types.size())
                continue;

            const ZeroEngine::LevelGameObjTypeDef& td = types[ent->type_def_index];
            uint32_t fieldOff = 0;
            bool foundTeam = false;
            for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                if (td.fields[fi].name_crc == LC_FIELD_TEAM ||
                    _stricmp(td.fields[fi].name.c_str(), "Team") == 0) {
                    fieldOff = td.fields[fi].offset;
                    foundTeam = true;
                    break;
                }
            }
            if (!foundTeam) continue;

            uint32_t absOff = ent->block1_obj_offset + fieldOff;
            int teamVal = args.multiEditTeamValue;
            if (!g_levelReader.SetFieldValue(absOff, &teamVal, 4)) continue;

            ZeroEngine::FieldEdit fe;
            memset(&fe, 0, sizeof(fe));
            fe.entityGuid = ent->guid;
            fe.fieldName = "Team";
            fe.kind = 0;
            fe.intVal = teamVal;
            g_levelReader.AddFieldEdit(fe);
            ++changed;
        }
        if (changed > 0) {
            char msg[160];
            sprintf_s(msg, "Set Team=%d on %d selected entities", args.multiEditTeamValue, changed);
            g_savePakMessage = msg;
            g_savePakStatus = 5;
            g_savePakDoneTimer = 3.0f;
        }
    }

    // ── Phase 2 XSI authoring: drag-reparent ────────────────────────────
    //
    // DLL drag-dropped one entity onto another in the outliner. Apply
    // it to Block1 (so the renderer's parent_guid cache picks it up on
    // next scene rebuild), update the runtime LevelGameObjEntry so the
    // outliner re-renders correctly THIS frame, push the parallel
    // g_goParentGuids slot, and log a FieldEdit so the change survives
    // SavePak.
    //
    // ParentGUID lives at offset 4 of every entity's field data per
    // EntityFieldDefs. That is a hard-coded universal across all 172
    // type definitions in the level.json analysis. If a future type
    // breaks that invariant we'll find it the moment a reparent on
    // that type fucks the layout, but right now it holds.
    if (args.requestReparentChildGuid != 0 && g_levelReader.IsLoaded())
    {
        uint32_t childGuid  = args.requestReparentChildGuid;
        uint32_t parentGuid = args.requestReparentNewParentGuid;

        // Refuse self-parent. The user shouldn't be able to do this from
        // the DLL but defense in depth is free.
        if (childGuid == parentGuid) {
            g_savePakMessage = "Reparent rejected: entity tried to be its OWN motherfucking parent. The hierarchy is a tree, not Greek mythology. Drop it on someone else.";
            g_savePakStatus = 4;
            g_savePakDoneTimer = 3.0f;
        } else {
            std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjsMut();
            int childIdx = -1;
            for (size_t gi = 0; gi < gos.size(); ++gi) {
                if (gos[gi].guid == childGuid) { childIdx = (int)gi; break; }
            }
            if (childIdx < 0) {
                g_savePakMessage = "Reparent failed: child GUID is fucking ghost, not in the loaded level. Did the level reload between drag and drop? Pick again.";
                g_savePakStatus = 4;
                g_savePakDoneTimer = 3.0f;
            } else {
                // Cycle check: walk the proposed new parent up its own
                // chain. If we hit childGuid, we'd be making child its
                // own ancestor and the engine would loop forever or
                // crash on the first traversal. Reject.
                bool cycle = false;
                if (parentGuid != 0) {
                    uint32_t walker = parentGuid;
                    int hops = 0;
                    while (walker != 0 && hops < 1024) {
                        if (walker == childGuid) { cycle = true; break; }
                        bool found = false;
                        for (size_t wi = 0; wi < gos.size(); ++wi) {
                            if (gos[wi].guid == walker) {
                                walker = gos[wi].parent_guid;
                                found = true; break;
                            }
                        }
                        if (!found) break;
                        ++hops;
                    }
                }
                if (cycle) {
                    g_savePakMessage = "Reparent rejected: that drop would put the child INSIDE its own ancestor chain. Infinite-loop bait. The engine traverses parent pointers in fifteen places, EVERY one of them would hang. Try again.";
                    g_savePakStatus = 4;
                    g_savePakDoneTimer = 3.0f;
                } else {
                    ZeroEngine::LevelGameObjEntry& child = gos[childIdx];
                    uint32_t absOff = child.block1_obj_offset + 4; // ParentGUID at offset 4
                    bool wrote = false;
                    if (child.block1_obj_offset != 0) {
                        wrote = g_levelReader.SetFieldValue(absOff, &parentGuid, 4);
                    }
                    // Update runtime cache regardless so the outliner
                    // tree re-flows next frame even if Block1 write
                    // somehow failed (e.g. pending entity not yet
                    // baked). Save pipeline rebuilds from FieldEdits.
                    child.parent_guid = parentGuid;
                    if ((size_t)childIdx < g_goParentGuids.size())
                        g_goParentGuids[childIdx] = parentGuid;

                    ZeroEngine::FieldEdit fe;
                    memset(&fe, 0, sizeof(fe));
                    fe.entityGuid = childGuid;
                    fe.fieldName  = "ParentGUID";
                    fe.kind       = 2; // GUID
                    fe.guidVal    = parentGuid;
                    g_levelReader.AddFieldEdit(fe);

                    char msg[160];
                    if (parentGuid == 0)
                        sprintf_s(msg, "Reparented 0x%08X -> top-level", childGuid);
                    else
                        sprintf_s(msg, "Reparented 0x%08X -> 0x%08X%s",
                                  childGuid, parentGuid, wrote ? "" : " (queued, not yet in Block1)");
                    g_savePakMessage = msg;
                    g_savePakStatus = 5;
                    g_savePakDoneTimer = 2.5f;
                }
            }
        }
    }

    // ── Phase 9c: Ctrl+D duplicate entity ─────────────────────────────
    //
    // Hot key from the Phase 7 hotkey block (post-pre-DrawFrame primer)
    // wrote the source entity's GUID into args.hostRequestDuplicateEntityGuid.
    // We resolve it through LevelReader::m_gameObjs, build a
    // PendingGameObj from the source's metadata + raw fieldData
    // copied out of the Block1 buffer (offset+size already cached on
    // the runtime entry), bump the world-transform Y by 2m so the
    // clone is visible without overlapping the original, and shove
    // it into the pending list via AddPendingEntity. The save
    // pipeline picks it up on next SavePak.
    //
    // Output entities are special-cased: their target_guid +
    // output_event + input_action carry into PendingGameObj's
    // dedicated fields so the wire pipeline sees them.
    if (args.hostRequestDuplicateEntityGuid != 0 && g_levelReader.IsLoaded())
    {
        uint32_t srcGuid = args.hostRequestDuplicateEntityGuid;
        const std::vector<ZeroEngine::LevelGameObjEntry>& gos =
            g_levelReader.GetGameObjs();
        int srcIdx = -1;
        for (size_t gi = 0; gi < gos.size(); ++gi) {
            if (gos[gi].guid == srcGuid) { srcIdx = (int)gi; break; }
        }
        if (srcIdx < 0) {
            g_savePakMessage = "Duplicate failed: source GUID is a fucking ghost, not in the loaded level. Picked the entity then it disappeared? Pick again and retry.";
            g_savePakStatus = 4;
            g_savePakDoneTimer = 3.0f;
        } else {
            const ZeroEngine::LevelGameObjEntry& src = gos[srcIdx];
            ZeroEngine::PendingGameObj pe;
            pe.guid           = g_levelReader.GenerateGuid();
            pe.parent_guid    = src.parent_guid;
            pe.layer_guid     = src.layer_guid;
            pe.name_crc       = src.name_crc;
            pe.type_crc       = src.type_crc;
            pe.gamemodemask   = src.gamemodemask;
            pe.type_def_index = src.type_def_index;
            pe.mesh_crc       = src.mesh_crc;
            memcpy(pe.world_transform, src.world_transform, sizeof(float) * 16);
            // 2m Y bump so the clone is visible.
            pe.world_transform[13] += 2.0f;
            pe.name_str       = src.name + "_dup";
            // Copy raw field data straight out of Block1.
            const std::vector<uint8_t>& block1 = g_levelReader.GetBlock1();
            if (src.block1_obj_offset != 0 && src.block1_obj_size > 0 &&
                src.block1_obj_offset + src.block1_obj_size <= block1.size())
            {
                pe.fieldData.resize(src.block1_obj_size);
                memcpy(&pe.fieldData[0],
                       &block1[src.block1_obj_offset],
                       src.block1_obj_size);

                // ── Sanitize unresolvable CRC fields ───────────────────────
                // Block1 bytes carry verbatim CRC32s for CRC/STRING fields
                // and CRCLIST/STRINGLIST entries. If a CRC doesn't resolve
                // in this level's pak_strings.json (e.g. the source entity
                // was imported from another level via a kit/template that
                // didn't bring the strings along), the engine's wide-string
                // parser FUN_0040b561 will eventually walk into junk and AV
                // at 0x0040B605. Symptom: site=B:0x878B5F calls with
                // buf=NULL count=~3564 in WideListParserLogger.txt.
                //
                // The sanitizer pass: for every CRC/STRING field in this
                // entity's type and every list entry, if the CRC is
                // non-zero and ResolveCrc() returns empty, zero it. "No
                // script" beats "ghost script that doesn't exist". The
                // user can fill them in via inspector afterwards.
                if (src.type_def_index < (int)g_levelReader.GetGameObjTypes().size()) {
                    const ZeroEngine::LevelGameObjTypeDef& td =
                        g_levelReader.GetGameObjTypes()[src.type_def_index];
                    for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                        const ZeroEngine::GOFieldDefEntry& fd = td.fields[fi];
                        ZeroEngine::FieldKind fk = ZeroEngine::FieldKindFromCrc(fd.kind_crc);
                        // Scalar CRC/STRING — 4-byte CRC at fd.offset.
                        if (fk == ZeroEngine::FK_CRC || fk == ZeroEngine::FK_STRING) {
                            if (fd.offset + 4 > pe.fieldData.size()) continue;
                            uint32_t v;
                            memcpy(&v, &pe.fieldData[fd.offset], 4);
                            if (v == 0) continue;
                            if (g_levelReader.ResolveCrc(v).empty()) {
                                uint32_t z = 0;
                                memcpy(&pe.fieldData[fd.offset], &z, 4);
                            }
                            continue;
                        }
                        // CRCLIST/STRINGLIST — 16-bit count + 16-bit offset
                        // at fd.offset, items at fd.offset+4+offset.
                        if (fk == ZeroEngine::FK_CRCLIST || fk == ZeroEngine::FK_STRINGLIST) {
                            if (fd.offset + 4 > pe.fieldData.size()) continue;
                            uint16_t listNum, listOff;
                            memcpy(&listNum, &pe.fieldData[fd.offset], 2);
                            memcpy(&listOff, &pe.fieldData[fd.offset + 2], 2);
                            uint32_t dataStart = fd.offset + 4u + listOff;
                            for (int li = 0; li < listNum; ++li) {
                                uint32_t itemOff = dataStart + (uint32_t)li * 4u;
                                if (itemOff + 4 > pe.fieldData.size()) break;
                                uint32_t item;
                                memcpy(&item, &pe.fieldData[itemOff], 4);
                                if (item == 0) continue;
                                if (g_levelReader.ResolveCrc(item).empty()) {
                                    uint32_t z = 0;
                                    memcpy(&pe.fieldData[itemOff], &z, 4);
                                }
                            }
                        }
                    }
                }
            }
            // Output entity wiring (only meaningful when type is Output).
            if (src.target_guid != 0) pe.target_guid = src.target_guid;
            if (!src.output_event.empty()) pe.output_event = src.output_event;
            if (!src.input_event.empty())  pe.input_action = src.input_event;
            pe.sticky = src.sticky ? 1 : 0;

            g_levelReader.AddPendingEntity(pe);

            char msg[200];
            sprintf_s(msg, "Duplicated 0x%08X -> 0x%08X (%s_dup, +2m Y). Save PAK to bake it.",
                      srcGuid, pe.guid, src.name.c_str());
            g_savePakMessage = msg;
            g_savePakStatus = 5;
            g_savePakDoneTimer = 3.5f;
        }
    }

    // ── Phase 10e: Delete entity ──────────────────────────────────────
    //
    // The Forge's Delete-key hotkey or right-click "Delete Entity" path
    // writes the target's GUID into args.hostRequestDeleteEntityGuid.
    // We mirror the wipe-layer pipeline: AddDeletedGuid on LevelReader
    // (so SavePak skips the entity), removeByGuid on LevelScene (so the
    // viewport drops its instance render), erase from the in-memory gos
    // list (so the iterator queries don't re-find it), and rebuild the
    // UI pointer arrays so the DLL's panels stop showing the corpse.
    //
    // Wires that POINTED AT the deleted entity will become broken refs
    // on next frame — Health mode catches them; the user can either
    // delete the dangling outputs or repoint them elsewhere. We do NOT
    // cascade-delete dependent Output entities here because that would
    // silently remove user wires the designer may want to redirect.
    if (args.hostRequestDeleteEntityGuid != 0 && g_levelReader.IsLoaded())
    {
        uint32_t delGuid = args.hostRequestDeleteEntityGuid;
        std::vector<ZeroEngine::LevelGameObjEntry>& gos =
            g_levelReader.GetGameObjsMut();
        int delIdx = -1;
        std::string delName;
        for (size_t gi = 0; gi < gos.size(); ++gi) {
            if (gos[gi].guid == delGuid) {
                delIdx = (int)gi;
                delName = gos[gi].name;
                break;
            }
        }
        if (delIdx < 0) {
            g_savePakMessage = "Delete failed: GUID is not in the loaded level. Already gone?";
            g_savePakStatus = 4;
            g_savePakDoneTimer = 3.0f;
        } else {
            g_levelReader.AddDeletedGuid(delGuid);
            if (g_renderer && g_renderer->hasLevelScene()) {
                LevelScene* ls = g_renderer->getLevelScene();
                if (ls) ls->removeByGuid(delGuid);
            }
            gos.erase(gos.begin() + delIdx);
            g_goNameStrings.clear(); g_goNamePtrs.clear();
            g_goTypeNameStrings.clear(); g_goTypeNamePtrs.clear();

            char msg[200];
            sprintf_s(msg, "Deleted 0x%08X (%s). Save PAK to bake it.",
                      delGuid, delName.c_str());
            g_savePakMessage = msg;
            g_savePakStatus = 5;
            g_savePakDoneTimer = 3.0f;
        }
    }

    // ── Phase 10e: Create entity from template ─────────────────────────
    //
    // The Forge's Create Entity wizard writes a TEMPLATE GUID into
    // args.hostRequestCreateEntityFromTemplateGuid. Same machinery as
    // duplicate but the new entity gets a fresh name based on the type
    // (rather than "_dup" suffix), is placed at the camera's look-at
    // point if we can derive one, falls back to template position with
    // a 2m bump otherwise. The wizard already filtered to a real
    // template, so resolution failures here mean the level got hot-
    // reloaded mid-click — surface the error like the duplicate path.
    if (args.hostRequestCreateEntityFromTemplateGuid != 0 && g_levelReader.IsLoaded())
    {
        uint32_t tplGuid = args.hostRequestCreateEntityFromTemplateGuid;
        const std::vector<ZeroEngine::LevelGameObjEntry>& gos =
            g_levelReader.GetGameObjs();
        int tplIdx = -1;
        for (size_t gi = 0; gi < gos.size(); ++gi) {
            if (gos[gi].guid == tplGuid) { tplIdx = (int)gi; break; }
        }
        if (tplIdx < 0) {
            g_savePakMessage = "Create failed: template GUID vanished. Pick the type again.";
            g_savePakStatus = 4;
            g_savePakDoneTimer = 3.0f;
        } else {
            const ZeroEngine::LevelGameObjEntry& tpl = gos[tplIdx];
            ZeroEngine::PendingGameObj pe;
            pe.guid           = g_levelReader.GenerateGuid();
            pe.parent_guid    = 0;                       // top-level by default
            pe.layer_guid     = tpl.layer_guid;          // inherit template's layer
            pe.name_crc       = tpl.name_crc;
            pe.type_crc       = tpl.type_crc;
            pe.gamemodemask   = tpl.gamemodemask;
            pe.type_def_index = tpl.type_def_index;
            pe.mesh_crc       = tpl.mesh_crc;
            memcpy(pe.world_transform, tpl.world_transform, sizeof(float) * 16);
            pe.world_transform[13] += 2.0f;              // 2m Y bump so it's not buried
            // Name = type_new (e.g., "trigger_volume_new") — designers
            // rename via the inspector; the placeholder is short enough
            // to spot in Outliner without scrolling.
            pe.name_str       = tpl.type_name + "_new";
            const std::vector<uint8_t>& block1 = g_levelReader.GetBlock1();
            if (tpl.block1_obj_offset != 0 && tpl.block1_obj_size > 0 &&
                tpl.block1_obj_offset + tpl.block1_obj_size <= block1.size())
            {
                pe.fieldData.resize(tpl.block1_obj_size);
                memcpy(&pe.fieldData[0],
                       &block1[tpl.block1_obj_offset],
                       tpl.block1_obj_size);

                // Same sanitizer pass as the donor-duplicate path above —
                // see that comment block for the full rationale. Short
                // version: zero any CRC/STRING field (and CRCLIST/
                // STRINGLIST entry) that doesn't resolve in this level's
                // pak_strings.json, otherwise the engine's wide-string
                // parser AVs at load.
                if (tpl.type_def_index < (int)g_levelReader.GetGameObjTypes().size()) {
                    const ZeroEngine::LevelGameObjTypeDef& td =
                        g_levelReader.GetGameObjTypes()[tpl.type_def_index];
                    for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                        const ZeroEngine::GOFieldDefEntry& fd = td.fields[fi];
                        ZeroEngine::FieldKind fk = ZeroEngine::FieldKindFromCrc(fd.kind_crc);
                        if (fk == ZeroEngine::FK_CRC || fk == ZeroEngine::FK_STRING) {
                            if (fd.offset + 4 > pe.fieldData.size()) continue;
                            uint32_t v;
                            memcpy(&v, &pe.fieldData[fd.offset], 4);
                            if (v == 0) continue;
                            if (g_levelReader.ResolveCrc(v).empty()) {
                                uint32_t z = 0;
                                memcpy(&pe.fieldData[fd.offset], &z, 4);
                            }
                            continue;
                        }
                        if (fk == ZeroEngine::FK_CRCLIST || fk == ZeroEngine::FK_STRINGLIST) {
                            if (fd.offset + 4 > pe.fieldData.size()) continue;
                            uint16_t listNum, listOff;
                            memcpy(&listNum, &pe.fieldData[fd.offset], 2);
                            memcpy(&listOff, &pe.fieldData[fd.offset + 2], 2);
                            uint32_t dataStart = fd.offset + 4u + listOff;
                            for (int li = 0; li < listNum; ++li) {
                                uint32_t itemOff = dataStart + (uint32_t)li * 4u;
                                if (itemOff + 4 > pe.fieldData.size()) break;
                                uint32_t item;
                                memcpy(&item, &pe.fieldData[itemOff], 4);
                                if (item == 0) continue;
                                if (g_levelReader.ResolveCrc(item).empty()) {
                                    uint32_t z = 0;
                                    memcpy(&pe.fieldData[itemOff], &z, 4);
                                }
                            }
                        }
                    }
                }
            }
            if (tpl.target_guid != 0) pe.target_guid = tpl.target_guid;
            if (!tpl.output_event.empty()) pe.output_event = tpl.output_event;
            if (!tpl.input_event.empty())  pe.input_action = tpl.input_event;
            pe.sticky = tpl.sticky ? 1 : 0;

            g_levelReader.AddPendingEntity(pe);

            char msg[220];
            sprintf_s(msg, "Created %s 0x%08X from template %s. Save PAK to bake.",
                      pe.name_str.c_str(), pe.guid, tpl.name.c_str());
            g_savePakMessage = msg;
            g_savePakStatus = 5;
            g_savePakDoneTimer = 3.5f;
        }
    }

    // ── Collab ACTION half — drain the DLL's request flags and actually fire
    //    HostStart / JoinStart / Stop on the session.
    //
    // This MUST run AFTER DrawFrame because the DLL writes the request flags
    // INSIDE DrawFrame (button click handlers). If you swap the order you'll
    // be checking flags from a frame ago and missing every click. The READ
    // half (sibling block above the DrawFrame call) does the opposite —
    // status into args BEFORE DrawFrame so the DLL can render the right menu.
    //
    // Don't fucking touch this ordering unless you also rewrite the args
    // memset-on-every-frame contract on line ~2595, which you won't, because
    // half the engine depends on it. ──
    {
        ZeroEngine::CollabSession& cs = ZeroEngine::CollabSession::Instance();
        if (args.collabStopRequested) {
            cs.Stop();
        }
        if (args.collabHostRequested) {
            unsigned short port = args.collabHostPort ? args.collabHostPort : (unsigned short)1818;
            if (g_levelReader.IsLoaded()) {
                const std::string& pp = g_levelReader.GetPakPath();
                uint32_t h = ZeroEngine::LotrHashString(pp.c_str());
                uint64_t sz = 0;
                FILE* fp = NULL; fopen_s(&fp, pp.c_str(), "rb");
                if (fp) { fseek(fp, 0, SEEK_END); sz = (uint64_t)_ftelli64(fp); fclose(fp); }
                cs.SetLoadedPak(pp.c_str(), h, sz);
            }
            cs.HostStart(port);
        }
        if (args.collabJoinRequested) {
            unsigned short port = args.collabJoinPort ? args.collabJoinPort : (unsigned short)1818;
            bool needStream = (args.collabJoinNoLocalPak != 0);
            // Only pass the local PAK identity when the user SAYS they have
            // one. If they ticked "stream from host" we send zeros; host
            // will skip the hash-match gate and ship its PAK over the wire.
            if (g_levelReader.IsLoaded() && !needStream) {
                const std::string& pp = g_levelReader.GetPakPath();
                uint32_t h = ZeroEngine::LotrHashString(pp.c_str());
                uint64_t sz = 0;
                FILE* fp = NULL; fopen_s(&fp, pp.c_str(), "rb");
                if (fp) { fseek(fp, 0, SEEK_END); sz = (uint64_t)_ftelli64(fp); fclose(fp); }
                cs.SetLoadedPak(pp.c_str(), h, sz);
            }
            cs.JoinStart(args.collabJoinIp, port, needStream);
        }
    }

    // Cancel all pending field edits if DLL requested it (Chain editor safety).
    if (args.pendingFieldEditsCancelRequested && g_levelReader.IsLoaded()) {
        g_levelReader.ClearFieldEdits();
    }
    // Revert a single pending field edit (Chain editor safety).
    // Exposed via GetFieldEdits() returning a const ref — we have to do an index-based
    // removal here because LevelReader doesn't expose EraseFieldEdit yet.
    if (args.pendingFieldEditRevertIndex >= 0 && g_levelReader.IsLoaded()) {
        int ridx = args.pendingFieldEditRevertIndex;
        // Rebuild m_fieldEdits without the reverted index. Clear then re-add.
        const std::vector<ZeroEngine::FieldEdit>& src = g_levelReader.GetFieldEdits();
        if (ridx < (int)src.size()) {
            std::vector<ZeroEngine::FieldEdit> kept;
            kept.reserve(src.size() - 1);
            for (int i = 0; i < (int)src.size(); ++i) {
                if (i != ridx) kept.push_back(src[i]);
            }
            g_levelReader.ClearFieldEdits();
            for (size_t i = 0; i < kept.size(); ++i) g_levelReader.AddFieldEdit(kept[i]);
        }
    }

    // Chain-editor: add one connection. Creates a new Output entity and writes
    // its GUID into a zero slot in source's Outputs[]. Fails if no zero slot
    // exists (until RebuildOutputsList lands). Soft-delete creates zero slots
    // that Add can consume — clean symmetry.
    if (args.chainAddConnectionRequested && g_levelReader.IsLoaded()) {
        // Default to bad-args; change to specific result below.
        int result = 4;
        uint32_t srcGuidA = args.chainAddSourceGuid;
        uint32_t tgtGuidA = args.chainAddTargetGuid;
        if (srcGuidA != 0 && tgtGuidA != 0) {
            // Locate source entity.
            const std::vector<ZeroEngine::LevelGameObjEntry>& gosA = g_levelReader.GetGameObjs();
            const ZeroEngine::LevelGameObjEntry* srcA = NULL;
            for (size_t ei = 0; ei < gosA.size(); ++ei) {
                if (gosA[ei].guid == srcGuidA) { srcA = &gosA[ei]; break; }
            }
            // Locate "Output" type def so we can create the new Output entity.
            int outputTypeIdx = FindTypeDefByName(g_levelReader, "Output");
            if (!srcA || srcA->type_def_index < 0) {
                result = 4;
            } else if (outputTypeIdx < 0) {
                result = 3;
            } else {
                // Find source's "Outputs" field and scan for zero slot.
                const ZeroEngine::LevelGameObjTypeDef& tdA =
                    g_levelReader.GetGameObjTypes()[srcA->type_def_index];
                const uint32_t CRC_OUTPUTS_A = 0x250BE3C1u;
                uint32_t outputsFieldOffA = 0;
                bool foundFieldA = false;
                for (size_t fi = 0; fi < tdA.fields.size(); ++fi) {
                    if (tdA.fields[fi].name_crc == CRC_OUTPUTS_A) {
                        outputsFieldOffA = tdA.fields[fi].offset;
                        foundFieldA = true;
                        break;
                    }
                }
                int zeroSlotIdx = -1;
                uint32_t zeroSlotAbsOff = 0;
                if (foundFieldA && srcA->block1_obj_offset != 0) {
                    uint32_t absOffA = srcA->block1_obj_offset + outputsFieldOffA;
                    uint16_t listNumA = 0, listDataOffA = 0;
                    g_levelReader.GetFieldValue(absOffA, &listNumA, 2);
                    g_levelReader.GetFieldValue(absOffA + 2, &listDataOffA, 2);
                    for (uint16_t li = 0; li < listNumA; ++li) {
                        uint32_t slotOff = absOffA + 4 + (uint32_t)listDataOffA + (uint32_t)li * 4;
                        uint32_t slotVal = 0;
                        g_levelReader.GetFieldValue(slotOff, &slotVal, 4);
                        if (slotVal == 0) { zeroSlotIdx = (int)li; zeroSlotAbsOff = slotOff; break; }
                    }
                }
                if (false) {  // old "fail on no zero slot" branch — kept disabled for reference
                    result = 2;
                } else {
                    // If no zero slot: we'll fall through to append via FieldEdit kind=8 (list
                    // append). The Python save pipeline grows the list JSON-side. In-memory
                    // Block1 isn't touched, so the new wire won't appear in the viewer until
                    // next reload — but the save is correct.
                    bool useListAppend = (zeroSlotIdx < 0);
                    // Create the new Output entity.
                    ZeroEngine::PendingGameObj e;
                    e.type_def_index = outputTypeIdx;
                    e.target_guid    = tgtGuidA;
                    e.output_event   = args.chainAddOutputEventName;
                    e.input_action   = args.chainAddInputActionName;
                    e.sticky         = args.chainAddSticky ? 1 : 0;
                    e.gamemodemask   = srcA->gamemodemask;
                    e.layer_guid     = srcA->layer_guid;
                    // Name helps the editor identify this in lists — mirror event/action.
                    char nameBuf[160];
                    _snprintf(nameBuf, sizeof(nameBuf), "Out_%s_%s",
                              args.chainAddOutputEventName, args.chainAddInputActionName);
                    e.name_str = nameBuf;
                    e.name_crc = ZeroEngine::LotrHashString(nameBuf);
                    // Delay + Parameter via generic override maps.
                    if (args.chainAddDelay != 0.0f) {
                        e.floatOverrides[0x33CED45Du /* Delay */] = args.chainAddDelay;
                    }
                    if (args.chainAddParameter[0] != '\0') {
                        e.stringOverrides[0xFFB8F4D3u /* Parameter */] = args.chainAddParameter;
                    }
                    // Zero-initialise transform (Output entities are invisible).
                    e.world_transform[0] = e.world_transform[5]
                        = e.world_transform[10] = e.world_transform[15] = 1.0f;
                    g_levelReader.AddPendingEntity(e);
                    // AddPendingEntity assigns GUID; the new entity is the last pushed.
                    const std::vector<ZeroEngine::PendingGameObj>& pend = g_levelReader.GetPendingObjs();
                    uint32_t newGuid = pend.empty() ? 0u : pend.back().guid;
                    if (newGuid != 0) {
                        ZeroEngine::FieldEdit fe;
                        memset(&fe, 0, sizeof(fe));
                        fe.entityGuid = srcGuidA;
                        fe.fieldName  = "Outputs";
                        fe.guidVal    = newGuid;
                        if (useListAppend) {
                            // kind=8 — Python pipeline appends to the JSON list during save.
                            fe.kind = 8;
                            fe.listIndex = -1;
                        } else {
                            // Zero slot exists — patch Block1 immediately so the new wire
                            // appears in the viewer right away.
                            g_levelReader.SetFieldValue(zeroSlotAbsOff, &newGuid, 4);
                            fe.kind = 6;
                            fe.listIndex = zeroSlotIdx;
                        }
                        g_levelReader.AddFieldEdit(fe);
                        args.chainAddLastNewGuid = newGuid;
                        result = useListAppend ? 5 : 1;  // 1=ok-in-place, 5=ok-append-on-save
                    } else {
                        result = 4;
                    }
                }
            }
        }
        args.chainAddResult = result;
    }

    // Chain-editor: delete one connection. Zeros the slot in source's Outputs[]
    // that holds outputGuid. Soft-delete; game iterators skip zero GUIDs.
    if (args.chainDeleteConnectionRequested && g_levelReader.IsLoaded()) {
        uint32_t srcGuid = args.chainDeleteSourceGuid;
        uint32_t outGuid = args.chainDeleteOutputGuid;
        if (srcGuid != 0 && outGuid != 0) {
            const std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjs();
            const ZeroEngine::LevelGameObjEntry* src = NULL;
            for (size_t ei = 0; ei < gos.size(); ++ei) {
                if (gos[ei].guid == srcGuid) { src = &gos[ei]; break; }
            }
            if (src && src->type_def_index >= 0 &&
                src->type_def_index < (int)g_levelReader.GetGameObjTypes().size())
            {
                const ZeroEngine::LevelGameObjTypeDef& td =
                    g_levelReader.GetGameObjTypes()[src->type_def_index];
                // Locate "Outputs" field by CRC.
                const uint32_t CRC_OUTPUTS_LOCAL = 0x250BE3C1u;
                uint32_t outputsFieldOff = 0;
                bool foundField = false;
                for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                    if (td.fields[fi].name_crc == CRC_OUTPUTS_LOCAL) {
                        outputsFieldOff = td.fields[fi].offset;
                        foundField = true;
                        break;
                    }
                }
                if (foundField && src->block1_obj_offset != 0) {
                    uint32_t absOff = src->block1_obj_offset + outputsFieldOff;
                    uint16_t listNum = 0, listDataOff = 0;
                    g_levelReader.GetFieldValue(absOff, &listNum, 2);
                    g_levelReader.GetFieldValue(absOff + 2, &listDataOff, 2);
                    // Walk GUIDs, find matching slot, zero it.
                    for (uint16_t li = 0; li < listNum; ++li) {
                        uint32_t slotOff = absOff + 4 + (uint32_t)listDataOff + (uint32_t)li * 4;
                        uint32_t slotVal = 0;
                        g_levelReader.GetFieldValue(slotOff, &slotVal, 4);
                        if (slotVal == outGuid) {
                            uint32_t zero = 0;
                            g_levelReader.SetFieldValue(slotOff, &zero, 4);
                            // Record as FieldEdit (kind=6, list item) so SavePak persists.
                            ZeroEngine::FieldEdit fe;
                            memset(&fe, 0, sizeof(fe));
                            fe.entityGuid = srcGuid;
                            fe.fieldName  = "Outputs";
                            fe.kind       = 6;
                            fe.listIndex  = (int)li;
                            fe.guidVal    = 0;
                            g_levelReader.AddFieldEdit(fe);
                            break;
                        }
                    }
                }
            }
        }
    }

    // Apply editor object relation filter from DLL
    if (g_renderer && g_renderer->hasLevelScene())
    {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls) {
            if (args.editorObjFilterActive && args.editorObjFilterGuidCount > 0 && args.editorObjFilterGuids)
                ls->setEditorObjFilter(args.editorObjFilterGuids, args.editorObjFilterGuidCount);
            else
                ls->clearEditorObjFilter();
            // Distance cull (Phase 1)
            ls->setEditorObjMaxDist(args.editorObjMaxDist);
            ls->setEditorObjFadeStart(args.editorObjFadeStart);
            ls->setEditorObjMinDist(args.editorObjMinDist);
            // Category filter (Phase 2)
            ls->setEditorObjCategoryMask(args.editorObjCategoryMask);
            // Search filter (Phase 3)
            ls->setEditorObjSearch(args.editorObjSearchMode, args.editorObjSearchTerm);
            // Label density cap (Phase 4)
            ls->setEditorObjLabelMaxCount(args.editorObjLabelMaxCount);
            // Level panel → editor obj selection sync
            if (args.requestSelectEditorObjGuid != 0) {
                int eoIdx = ls->findEditorObjByGuid(args.requestSelectEditorObjGuid);
                if (eoIdx >= 0) {
                    ls->setSelectedEditorObj(eoIdx);
                }
            }
            // Level panel → focus camera on editor obj (fly-to)
            if (args.requestFocusEditorObjGuid != 0 && g_renderer) {
                int foIdx = ls->findEditorObjByGuid(args.requestFocusEditorObjGuid);
                if (foIdx >= 0) {
                    ls->setSelectedEditorObj(foIdx);
                    const LevelEditorObj* feo = ls->getEditorObj(foIdx);
                    if (feo) {
                        // Set camera target to entity position, camera offset slightly above/behind
                        hkVector4 tgt; tgt.set(feo->mat[12], feo->mat[13], feo->mat[14]);
                        hkVector4 cam; cam.set(feo->mat[12] + 15.0f, feo->mat[13] + 20.0f, feo->mat[14] + 15.0f);
                        g_renderer->setCameraTarget(tgt);
                        g_renderer->setCameraPosition(cam);
                    }
                }
            }
            // Phase 1 XSI: copy event-wire toggle + focus chain back into
            // the renderer so the next frame draws (or hides) the wires.
            // Args came from the DLL; renderer holds the truth.
            ls->setEventWiresVisible(args.eventWiresShow != 0);
            ls->setEventWireFocusGuid(args.eventWireFocusGuid);
            // Phase 3 XSI: push the layer-manager state from the DLL's
            // sets into the renderer. setHiddenLayers/setLockedLayers
            // rebuild the std::set from the flat array each call. With
            // O(layers) per call and layer counts in the dozens this
            // costs nothing measurable.
            ls->setHiddenLayers(args.hiddenLayerGuids, args.hiddenLayerCount);
            ls->setLockedLayers(args.lockedLayerGuids, args.lockedLayerCount);
            ls->setIsolatedLayer(args.isolatedLayerGuid);
            ls->setActiveLayer(args.activeLayerGuid);
            // Phase 4 XSI: gizmo mode/space/snap from the toolbar.
            // Refuse to swap modes mid-drag, would crash the math.
            if (!ls->gizmoDragging()) {
                ls->setGizmoMode(args.levelGizmoMode);
                ls->setGizmoSpace(args.levelGizmoSpace);
            }
            ls->setGizmoSnap(args.levelGizmoSnap);
        }
    }

    // Handle viewport toggle request
    if (args.requestToggleViewport && g_renderer)
    {
        bool nowEnabled = !g_renderer->getScene3dEnabled();
        g_renderer->setScene3dEnabled(nowEnabled);
        if (!nowEnabled && g_viewportHwnd && g_hwnd)
        {
            // Fill full window so ImGui remains visible on the D3D surface
            RECT rc; GetClientRect(g_hwnd, &rc);
            MoveWindow(g_viewportHwnd, 0, 0, rc.right, rc.bottom, TRUE);
            g_resizePending = true;
            g_pendingResizeW = rc.right > 1 ? rc.right : 1;
            g_pendingResizeH = rc.bottom > 1 ? rc.bottom : 1;
        }
    }

    // Handle pending scene reload (after SavePak)
    // SavePak already reloaded LevelReader internally. We just need to
    // reload the 3D scene and rebuild UI arrays. Simulate a "Render Full Map" click
    // which calls loadLevelScene, then trigger the full UI rebuild by faking a load request.
    if (g_pendingSceneReload && g_levelReader.IsLoaded()) {
        g_pendingSceneReload = false;
        if (g_renderer) {
            g_renderer->unloadLevelScene();
            g_renderer->loadLevelScene(g_levelReader);
        }
    }

    // Handle level load request from DLL
    if (args.requestLoadLevel && args.requestedLevelPakPath[0] && args.requestedLevelBinPath[0])
    {
        g_levelLoadError.clear();
        g_levelReader.Unload();
        if (g_renderer) g_renderer->unloadLevelScene();
        g_pakModelNamePtrs.clear();
        g_pakTextureNamePtrs.clear();
        g_pakTextureWidths.clear();
        g_pakTextureHeights.clear();
        g_pakAnimNamePtrs.clear();
        g_pakEffectNamePtrs.clear();
        g_pakGfxBlockNamePtrs.clear();
        g_pakModelCrcs.clear();
        g_pakModelBoneCounts.clear();
        g_pakModelMatCounts.clear();
        g_pakModelGamemodeMasks.clear();
        g_pakTextureDepths.clear();
        g_pakTextureLevels.clear();
        g_pakTextureFormats.clear();
        g_pakTextureGamemodeMasks.clear();
        g_pakAnimCrcs.clear();
        g_pakAnimSizes.clear();
        g_pakAnimSizesComp.clear();
        g_pakAnimGamemodeMasks.clear();
        g_pakEffectSizes.clear();
        g_pakEffectGamemodeMasks.clear();
        g_pakGfxSizes.clear();
        g_binRawNamePtrs.clear();
        g_binRawKinds.clear();
        g_binRawSizes.clear();
        g_binRawSizesComp.clear();
        g_binRawNameStrings.clear();
        g_subBlock1NameStrings.clear(); g_subBlock1NamePtrs.clear();
        g_subBlock1TypeStrings.clear(); g_subBlock1TypePtrs.clear();
        g_subBlock1Sizes.clear();
        g_subBlock2NameStrings.clear(); g_subBlock2NamePtrs.clear();
        g_subBlock2TypeStrings.clear(); g_subBlock2TypePtrs.clear();
        g_subBlock2Sizes.clear();
        g_objANameStrings.clear(); g_objANamePtrs.clear();
        g_objASizes.clear(); g_objASizesComp.clear();
        g_obj0NameStrings.clear(); g_obj0NamePtrs.clear(); g_obj0TypeCodes.clear();
        g_animBlockNameStrings.clear(); g_animBlockNamePtrs.clear();
        g_animBlockKeyNameStrings.clear(); g_animBlockKeyNamePtrs.clear();
        g_animBlockSizes.clear(); g_animBlockSizesComp.clear();
        g_pakValNameStrings.clear(); g_pakValNamePtrs.clear(); g_pakValGamemodeMasks.clear();
        g_bufInfoNameStrings.clear(); g_bufInfoNamePtrs.clear(); g_bufInfoExtra1.clear(); g_bufInfoExtra2.clear();
        g_mat1NameStrings.clear(); g_mat1NamePtrs.clear(); g_mat1Tex0.clear(); g_mat1Tex1.clear();
        g_mat2NameStrings.clear(); g_mat2NamePtrs.clear(); g_mat2Tex0.clear(); g_mat2Tex1.clear();
        g_mat3NameStrings.clear(); g_mat3NamePtrs.clear(); g_mat3Tex0.clear(); g_mat3Tex1.clear();
        g_mat4NameStrings.clear(); g_mat4NamePtrs.clear(); g_mat4Tex0.clear(); g_mat4Tex1.clear();
        g_matExtraNameStrings.clear(); g_matExtraNamePtrs.clear();
        g_shapeInfoNameStrings.clear(); g_shapeInfoNamePtrs.clear(); g_shapeInfoOffsets.clear(); g_shapeInfoKinds.clear();
        g_hkShapeNameStrings.clear(); g_hkShapeNamePtrs.clear(); g_hkShapeKinds.clear(); g_hkShapeANums.clear();
        g_hkConstraintDataNameStrings.clear(); g_hkConstraintDataNamePtrs.clear(); g_hkConstraintDataKinds.clear();
        g_vbuffInfoNameStrings.clear(); g_vbuffInfoNamePtrs.clear(); g_vbuffInfoSizes.clear(); g_vbuffInfoOffsets.clear();
        g_ibuffInfoNameStrings.clear(); g_ibuffInfoNamePtrs.clear(); g_ibuffInfoSizes.clear(); g_ibuffInfoFormats.clear();
        g_hkConstraintInfoNameStrings.clear(); g_hkConstraintInfoNamePtrs.clear(); g_hkConstraintInfoKinds.clear(); g_hkConstraintInfoBoneParents.clear();
        g_pFieldNameStrings.clear(); g_pFieldNamePtrs.clear(); g_pFieldGamemodeMasks.clear(); g_pFieldWidths.clear();
        g_radiosityNameStrings.clear(); g_radiosityNamePtrs.clear(); g_radiosityNums.clear(); g_radiosityOffsets.clear();
        g_foliageNameStrings.clear(); g_foliageNamePtrs.clear(); g_foliageKinds.clear();
        g_goNameStrings.clear(); g_goNamePtrs.clear(); g_goTypeNameStrings.clear(); g_goTypeNamePtrs.clear();
        g_goMeshNameStrings.clear(); g_goMeshNamePtrs.clear();
        g_goGuids.clear(); g_goParentGuids.clear(); g_goLayerGuids.clear(); g_goGamemodeMasks.clear(); g_goTeams.clear();
        g_goPosX.clear(); g_goPosY.clear(); g_goPosZ.clear();
        g_goTypeDefNameStrings.clear(); g_goTypeDefNamePtrs.clear();
        g_goTypeDefFieldCounts.clear(); g_goTypeDefInstanceCounts.clear();
        g_goTargetGuids.clear(); g_goOutputEventStrings.clear(); g_goInputEventStrings.clear();
        g_goModeNameStrings.clear(); g_goOutputEventPtrs.clear(); g_goInputEventPtrs.clear();
        g_goModeNamePtrs.clear(); g_goDelays.clear(); g_goSticky.clear();
        g_goOutputsData.clear(); g_goOutputsOffsets.clear(); g_goOutputsCounts.clear();
        g_goLayersArrData.clear(); g_goLayersArrOffsets.clear(); g_goLayersArrCounts.clear();
        g_goNodesData.clear(); g_goNodesOffsets.clear(); g_goNodesCounts.clear();
        g_goNode1Guids.clear(); g_goNode2Guids.clear();
        g_goAllGuidRefData.clear(); g_goAllGuidRefCounts.clear(); g_goAllGuidRefOffsets.clear();
        g_goEditorShapeStrings.clear(); g_goEditorTextureStrings.clear();
        g_goEditorShapePtrs.clear(); g_goEditorTexturePtrs.clear();
        g_goEditorColors.clear(); g_goEditorOuters.clear();
        g_goEditorSizeX.clear(); g_goEditorSizeY.clear(); g_goEditorSizeZ.clear();

        if (g_levelReader.Load(args.requestedLevelPakPath, args.requestedLevelBinPath))
        {
            // Build pointer caches from loaded level
            const std::vector<ZeroEngine::LevelModelEntry>& mdls = g_levelReader.GetModels();
            for (size_t i = 0; i < mdls.size(); ++i)
                g_pakModelNamePtrs.push_back(mdls[i].name.c_str());

            const std::vector<ZeroEngine::LevelTextureEntry>& texs = g_levelReader.GetTextures();
            for (size_t i = 0; i < texs.size(); ++i)
            {
                g_pakTextureNamePtrs.push_back(texs[i].name.c_str());
                g_pakTextureWidths.push_back((int)texs[i].width);
                g_pakTextureHeights.push_back((int)texs[i].height);
            }

            const std::vector<ZeroEngine::LevelAnimEntry>& anims = g_levelReader.GetAnimations();
            for (size_t i = 0; i < anims.size(); ++i)
                g_pakAnimNamePtrs.push_back(anims[i].name.c_str());

            const std::vector<ZeroEngine::LevelEffectEntry>& effs = g_levelReader.GetEffects();
            for (size_t i = 0; i < effs.size(); ++i)
                g_pakEffectNamePtrs.push_back(effs[i].name.c_str());

            const std::vector<ZeroEngine::LevelGfxBlockEntry>& gbs = g_levelReader.GetGfxBlocks();
            for (size_t i = 0; i < gbs.size(); ++i)
                g_pakGfxBlockNamePtrs.push_back(gbs[i].name.c_str());

            const std::vector<ZeroEngine::LevelModelEntry>& mdlsD = g_levelReader.GetModels();
            for (size_t i = 0; i < mdlsD.size(); ++i) {
                g_pakModelCrcs.push_back((unsigned int)mdlsD[i].crc);
                g_pakModelBoneCounts.push_back((int)mdlsD[i].bones_num);
                g_pakModelMatCounts.push_back((int)mdlsD[i].mat_num);
                g_pakModelGamemodeMasks.push_back((int)mdlsD[i].gamemodemask);
            }

            const std::vector<ZeroEngine::LevelTextureEntry>& texsD = g_levelReader.GetTextures();
            for (size_t i = 0; i < texsD.size(); ++i) {
                g_pakTextureDepths.push_back((int)texsD[i].depth);
                g_pakTextureLevels.push_back((int)texsD[i].levels);
                g_pakTextureFormats.push_back((unsigned int)texsD[i].format);
                g_pakTextureGamemodeMasks.push_back((int)texsD[i].gamemodemask);
            }

            const std::vector<ZeroEngine::LevelAnimEntry>& animsD = g_levelReader.GetAnimations();
            for (size_t i = 0; i < animsD.size(); ++i) {
                g_pakAnimCrcs.push_back((unsigned int)animsD[i].crc);
                g_pakAnimSizes.push_back((int)animsD[i].size);
                g_pakAnimSizesComp.push_back((int)animsD[i].size_comp);
                g_pakAnimGamemodeMasks.push_back((int)animsD[i].gamemodemask);
            }

            const std::vector<ZeroEngine::LevelEffectEntry>& effsD = g_levelReader.GetEffects();
            for (size_t i = 0; i < effsD.size(); ++i) {
                g_pakEffectSizes.push_back((int)effsD[i].size);
                g_pakEffectGamemodeMasks.push_back((int)effsD[i].gamemodemask);
            }

            const std::vector<ZeroEngine::LevelGfxBlockEntry>& gbsD = g_levelReader.GetGfxBlocks();
            for (size_t i = 0; i < gbsD.size(); ++i)
                g_pakGfxSizes.push_back((int)gbsD[i].size);

            // BIN flat asset list
            const std::vector<ZeroEngine::BinFlatEntry>& binFlat = g_levelReader.GetAllBinAssets();
            g_binRawNameStrings.resize(binFlat.size());
            for (size_t i = 0; i < binFlat.size(); ++i) {
                g_binRawNameStrings[i] = binFlat[i].name;
                g_binRawKinds.push_back((unsigned int)binFlat[i].kind);
                g_binRawSizes.push_back((int)binFlat[i].size);
                g_binRawSizesComp.push_back((int)binFlat[i].size_comp);
            }
            for (size_t i = 0; i < g_binRawNameStrings.size(); ++i)
                g_binRawNamePtrs.push_back(g_binRawNameStrings[i].c_str());

            // Sub-blocks 1
            {
                const std::vector<ZeroEngine::SubBlockEntry>& sb1 = g_levelReader.GetSubBlocks1();
                g_subBlock1NameStrings.resize(sb1.size());
                g_subBlock1TypeStrings.resize(sb1.size());
                for (size_t i = 0; i < sb1.size(); ++i) {
                    g_subBlock1NameStrings[i] = sb1[i].name;
                    g_subBlock1TypeStrings[i] = sb1[i].type;
                    g_subBlock1Sizes.push_back((int)sb1[i].size);
                }
                for (size_t i = 0; i < g_subBlock1NameStrings.size(); ++i) {
                    g_subBlock1NamePtrs.push_back(g_subBlock1NameStrings[i].c_str());
                    g_subBlock1TypePtrs.push_back(g_subBlock1TypeStrings[i].c_str());
                }
            }
            // Sub-blocks 2
            {
                const std::vector<ZeroEngine::SubBlockEntry>& sb2 = g_levelReader.GetSubBlocks2();
                g_subBlock2NameStrings.resize(sb2.size());
                g_subBlock2TypeStrings.resize(sb2.size());
                for (size_t i = 0; i < sb2.size(); ++i) {
                    g_subBlock2NameStrings[i] = sb2[i].name;
                    g_subBlock2TypeStrings[i] = sb2[i].type;
                    g_subBlock2Sizes.push_back((int)sb2[i].size);
                }
                for (size_t i = 0; i < g_subBlock2NameStrings.size(); ++i) {
                    g_subBlock2NamePtrs.push_back(g_subBlock2NameStrings[i].c_str());
                    g_subBlock2TypePtrs.push_back(g_subBlock2TypeStrings[i].c_str());
                }
            }

            // ObjAs
            {
                const std::vector<ZeroEngine::LevelObjAEntry>& objas = g_levelReader.GetObjAs();
                g_objANameStrings.resize(objas.size());
                for (size_t i = 0; i < objas.size(); ++i) {
                    g_objANameStrings[i] = objas[i].name;
                    g_objASizes.push_back((int)objas[i].size);
                    g_objASizesComp.push_back((int)objas[i].size_comp);
                }
                for (size_t i = 0; i < g_objANameStrings.size(); ++i)
                    g_objANamePtrs.push_back(g_objANameStrings[i].c_str());
            }

            // Obj0s
            {
                const std::vector<ZeroEngine::LevelObj0Entry>& obj0s = g_levelReader.GetObj0s();
                g_obj0NameStrings.resize(obj0s.size());
                for (size_t i = 0; i < obj0s.size(); ++i) {
                    g_obj0NameStrings[i] = obj0s[i].name;
                    g_obj0TypeCodes.push_back((unsigned int)obj0s[i].type_code);
                }
                for (size_t i = 0; i < g_obj0NameStrings.size(); ++i)
                    g_obj0NamePtrs.push_back(g_obj0NameStrings[i].c_str());
            }

            // Animation blocks
            {
                const std::vector<ZeroEngine::LevelAnimBlockEntry>& ablks = g_levelReader.GetAnimBlocks();
                g_animBlockNameStrings.resize(ablks.size());
                g_animBlockKeyNameStrings.resize(ablks.size());
                for (size_t i = 0; i < ablks.size(); ++i) {
                    g_animBlockNameStrings[i]    = ablks[i].name;
                    g_animBlockKeyNameStrings[i] = ablks[i].key_name;
                    g_animBlockSizes.push_back((int)ablks[i].size);
                    g_animBlockSizesComp.push_back((int)ablks[i].size_comp);
                }
                for (size_t i = 0; i < g_animBlockNameStrings.size(); ++i) {
                    g_animBlockNamePtrs.push_back(g_animBlockNameStrings[i].c_str());
                    g_animBlockKeyNamePtrs.push_back(g_animBlockKeyNameStrings[i].c_str());
                }
            }

            // Pak vals
            {
                const std::vector<ZeroEngine::LevelPakValEntry>& pvals = g_levelReader.GetPakVals();
                g_pakValNameStrings.resize(pvals.size());
                for (size_t i = 0; i < pvals.size(); ++i) {
                    g_pakValNameStrings[i] = pvals[i].name;
                    g_pakValGamemodeMasks.push_back((int)pvals[i].gamemodemask);
                }
                for (size_t i = 0; i < g_pakValNameStrings.size(); ++i)
                    g_pakValNamePtrs.push_back(g_pakValNameStrings[i].c_str());
            }
            // Helper macro to populate a generic entry vector
            #define POPULATE_GENERIC(getter, nameStrVec, namePtrVec, e1vec, e2vec, hasE2) \
            { \
                const std::vector<ZeroEngine::LevelGenericEntry>& _v = g_levelReader.getter(); \
                nameStrVec.resize(_v.size()); \
                for (size_t _i = 0; _i < _v.size(); ++_i) { \
                    nameStrVec[_i] = _v[_i].name; \
                    e1vec.push_back(_v[_i].extra1); \
                    if (hasE2) e2vec.push_back(_v[_i].extra2); \
                } \
                for (size_t _i = 0; _i < nameStrVec.size(); ++_i) \
                    namePtrVec.push_back(nameStrVec[_i].c_str()); \
            }
            POPULATE_GENERIC(GetBufferInfos,      g_bufInfoNameStrings,           g_bufInfoNamePtrs,           g_bufInfoExtra1,              g_bufInfoExtra2,              true)
            POPULATE_GENERIC(GetMat1s,            g_mat1NameStrings,              g_mat1NamePtrs,              g_mat1Tex0,                   g_mat1Tex1,                   true)
            POPULATE_GENERIC(GetMat2s,            g_mat2NameStrings,              g_mat2NamePtrs,              g_mat2Tex0,                   g_mat2Tex1,                   true)
            POPULATE_GENERIC(GetMat3s,            g_mat3NameStrings,              g_mat3NamePtrs,              g_mat3Tex0,                   g_mat3Tex1,                   true)
            POPULATE_GENERIC(GetMat4s,            g_mat4NameStrings,              g_mat4NamePtrs,              g_mat4Tex0,                   g_mat4Tex1,                   true)
            POPULATE_GENERIC(GetShapeInfos,       g_shapeInfoNameStrings,         g_shapeInfoNamePtrs,         g_shapeInfoOffsets,           g_shapeInfoKinds,             true)
            POPULATE_GENERIC(GetHkShapeInfos,     g_hkShapeNameStrings,           g_hkShapeNamePtrs,           g_hkShapeKinds,               g_hkShapeANums,               true)
            POPULATE_GENERIC(GetHkConstraintDatas,g_hkConstraintDataNameStrings,  g_hkConstraintDataNamePtrs,  g_hkConstraintDataKinds,      g_hkConstraintDataKinds,      false)
            POPULATE_GENERIC(GetVBuffInfos,       g_vbuffInfoNameStrings,         g_vbuffInfoNamePtrs,         g_vbuffInfoSizes,             g_vbuffInfoOffsets,           true)
            POPULATE_GENERIC(GetIBuffInfos,       g_ibuffInfoNameStrings,         g_ibuffInfoNamePtrs,         g_ibuffInfoSizes,             g_ibuffInfoFormats,           true)
            POPULATE_GENERIC(GetHkConstraintInfos,g_hkConstraintInfoNameStrings,  g_hkConstraintInfoNamePtrs,  g_hkConstraintInfoKinds,      g_hkConstraintInfoBoneParents,true)
            POPULATE_GENERIC(GetPFieldInfos,      g_pFieldNameStrings,            g_pFieldNamePtrs,            g_pFieldGamemodeMasks,        g_pFieldWidths,               true)
            POPULATE_GENERIC(GetRadiosityInfos,   g_radiosityNameStrings,         g_radiosityNamePtrs,         g_radiosityNums,              g_radiosityOffsets,           true)
            POPULATE_GENERIC(GetFoliageInfos,     g_foliageNameStrings,           g_foliageNamePtrs,           g_foliageKinds,               g_foliageKinds,               false)
            // MatExtra has no extras
            {
                const std::vector<ZeroEngine::LevelGenericEntry>& _v = g_levelReader.GetMatExtras();
                g_matExtraNameStrings.resize(_v.size());
                for (size_t _i = 0; _i < _v.size(); ++_i)
                    g_matExtraNameStrings[_i] = _v[_i].name;
                for (size_t _i = 0; _i < g_matExtraNameStrings.size(); ++_i)
                    g_matExtraNamePtrs.push_back(g_matExtraNameStrings[_i].c_str());
            }
            #undef POPULATE_GENERIC

            // GameObjs — level entity placement data
            {
                const std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjs();
                size_t n = gos.size();
                g_goNameStrings.resize(n);
                g_goTypeNameStrings.resize(n);
                g_goMeshNameStrings.resize(n);
                g_goGuids.resize(n);
                g_goParentGuids.resize(n);
                g_goLayerGuids.resize(n);
                g_goGamemodeMasks.resize(n);
                g_goTeams.resize(n);
                g_goPosX.resize(n);
                g_goPosY.resize(n);
                g_goPosZ.resize(n);
                // CRC for "Team" field — computed once
                static uint32_t CRC_TEAM = ZeroEngine::LotrHashString("Team");
                for (size_t i = 0; i < n; ++i) {
                    g_goNameStrings[i]     = gos[i].name;
                    g_goTypeNameStrings[i] = gos[i].type_name;
                    g_goMeshNameStrings[i] = gos[i].mesh_name;
                    g_goGuids[i]           = gos[i].guid;
                    g_goParentGuids[i]     = gos[i].parent_guid;
                    g_goLayerGuids[i]      = gos[i].layer_guid;
                    g_goGamemodeMasks[i]   = gos[i].gamemodemask;
                    g_goPosX[i]            = gos[i].pos[0];
                    g_goPosY[i]            = gos[i].pos[1];
                    g_goPosZ[i]            = gos[i].pos[2];
                    // Extract Team from int_fields (if present)
                    std::map<uint32_t, uint32_t>::const_iterator tIt = gos[i].int_fields.find(CRC_TEAM);
                    g_goTeams[i] = (tIt != gos[i].int_fields.end()) ? (int)tIt->second : -1;
                }
                for (size_t i = 0; i < n; ++i) {
                    g_goNamePtrs.push_back(g_goNameStrings[i].c_str());
                    g_goTypeNamePtrs.push_back(g_goTypeNameStrings[i].c_str());
                    g_goMeshNamePtrs.push_back(g_goMeshNameStrings[i].c_str());
                }

                // Event system & relationship fields
                g_goTargetGuids.resize(n);
                g_goOutputEventStrings.resize(n);
                g_goInputEventStrings.resize(n);
                g_goModeNameStrings.resize(n);
                g_goDelays.resize(n);
                g_goSticky.resize(n);
                g_goOutputsOffsets.resize(n);
                g_goOutputsCounts.resize(n);
                g_goLayersArrOffsets.resize(n);
                g_goLayersArrCounts.resize(n);
                g_goNodesOffsets.resize(n);
                g_goNodesCounts.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    g_goNode1Guids.push_back(gos[i].node1_guid);
                    g_goNode2Guids.push_back(gos[i].node2_guid);
                    g_goTargetGuids[i]       = gos[i].target_guid;
                    g_goOutputEventStrings[i]= gos[i].output_event;
                    g_goInputEventStrings[i] = gos[i].input_event;
                    g_goModeNameStrings[i]   = gos[i].mode_name;
                    g_goDelays[i]            = gos[i].delay;
                    g_goSticky[i]            = gos[i].sticky ? 1 : 0;
                    // Pack Outputs array
                    g_goOutputsOffsets[i] = (int)g_goOutputsData.size();
                    g_goOutputsCounts[i]  = (int)gos[i].outputs.size();
                    for (size_t j = 0; j < gos[i].outputs.size(); ++j)
                        g_goOutputsData.push_back(gos[i].outputs[j]);
                    // Pack Layers array
                    g_goLayersArrOffsets[i] = (int)g_goLayersArrData.size();
                    g_goLayersArrCounts[i]  = (int)gos[i].layers.size();
                    for (size_t j = 0; j < gos[i].layers.size(); ++j)
                        g_goLayersArrData.push_back(gos[i].layers[j]);
                    // Pack Nodes array
                    g_goNodesOffsets[i] = (int)g_goNodesData.size();
                    g_goNodesCounts[i]  = (int)gos[i].nodes.size();
                    for (size_t j = 0; j < gos[i].nodes.size(); ++j)
                        g_goNodesData.push_back(gos[i].nodes[j]);
                }
                // Pack ALL generic GUID references
                g_goAllGuidRefCounts.resize(n);
                g_goAllGuidRefOffsets.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    g_goAllGuidRefOffsets[i] = (int)(g_goAllGuidRefData.size() / 2);
                    int pairCount = 0;
                    // Single GUID refs
                    for (std::map<uint32_t, uint32_t>::const_iterator it = gos[i].guid_refs.begin();
                         it != gos[i].guid_refs.end(); ++it) {
                        g_goAllGuidRefData.push_back(it->first);  // field CRC
                        g_goAllGuidRefData.push_back(it->second); // target GUID
                        pairCount++;
                    }
                    // Objectlist refs (each GUID in each list)
                    for (std::map<uint32_t, std::vector<uint32_t> >::const_iterator it = gos[i].list_refs.begin();
                         it != gos[i].list_refs.end(); ++it) {
                        for (size_t j = 0; j < it->second.size(); ++j) {
                            g_goAllGuidRefData.push_back(it->first);     // field CRC
                            g_goAllGuidRefData.push_back(it->second[j]); // target GUID
                            pairCount++;
                        }
                    }
                    g_goAllGuidRefCounts[i] = pairCount;
                }
                // Editor visualization fields
                g_goEditorShapeStrings.resize(n);
                g_goEditorTextureStrings.resize(n);
                g_goEditorColors.resize(n);
                g_goEditorOuters.resize(n);
                g_goEditorSizeX.resize(n); g_goEditorSizeY.resize(n); g_goEditorSizeZ.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    g_goEditorShapeStrings[i]  = gos[i].editor_shape;
                    g_goEditorTextureStrings[i]= gos[i].editor_texture;
                    g_goEditorColors[i]        = gos[i].editor_color;
                    g_goEditorOuters[i]        = gos[i].editor_outer;
                    g_goEditorSizeX[i]         = gos[i].editor_size[0];
                    g_goEditorSizeY[i]         = gos[i].editor_size[1];
                    g_goEditorSizeZ[i]         = gos[i].editor_size[2];
                }
                for (size_t i = 0; i < n; ++i) {
                    g_goOutputEventPtrs.push_back(g_goOutputEventStrings[i].c_str());
                    g_goInputEventPtrs.push_back(g_goInputEventStrings[i].c_str());
                    g_goModeNamePtrs.push_back(g_goModeNameStrings[i].c_str());
                    g_goEditorShapePtrs.push_back(g_goEditorShapeStrings[i].c_str());
                    g_goEditorTexturePtrs.push_back(g_goEditorTextureStrings[i].c_str());
                }

                // Type definitions
                const std::vector<ZeroEngine::LevelGameObjTypeDef>& tds = g_levelReader.GetGameObjTypes();
                g_goTypeDefNameStrings.resize(tds.size());
                for (size_t i = 0; i < tds.size(); ++i) {
                    g_goTypeDefNameStrings[i] = tds[i].name;
                    g_goTypeDefFieldCounts.push_back((int)tds[i].field_count);
                    g_goTypeDefInstanceCounts.push_back((int)tds[i].instance_count);
                }
                for (size_t i = 0; i < g_goTypeDefNameStrings.size(); ++i)
                    g_goTypeDefNamePtrs.push_back(g_goTypeDefNameStrings[i].c_str());
            }

            // ── Load full level scene from extracted directory ────────────────
            // Derive extracted dir from the PAK path:
            //   "C:/.../lotrcparser/BlackGates.PAK"  →  "C:/.../lotrcparser/BlackGates"
            if (g_renderer)
            {
                // Load level geometry directly from the in-memory LevelReader
                // (no pre-extracted JSON files required)
                g_renderer->loadLevelScene(g_levelReader);
            }
        }
        else
        {
            g_levelLoadError = g_levelReader.GetError();
        }
    }

    // Handle manual conquest_strings.txt load request
    if (args.requestLoadStrings && args.requestedStringsPath[0])
    {
        g_levelReader.LoadConquestStrings(args.requestedStringsPath);
    }

    // Handle "Render Full Map" button from UI
    if (args.requestRenderScene && g_renderer && g_levelReader.IsLoaded())
    {
        g_renderer->loadLevelScene(g_levelReader);
    }

    // Apply GameMode filter + position edits from UI
    if (g_renderer && g_renderer->hasLevelScene())
    {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls) {
            ls->setGameModeFilter(args.gameModeFilter);
            ls->setGameModeShowGlobals(args.gmfShowGlobals != 0);
            ls->setGameModeShowScripts(args.gmfShowScripts != 0);
            ls->setGameModeBitMask(args.gmfBitMask);
            if (args.inspectorPosChanged && ls->selectedInstance() >= 0) {
                int selIdx = ls->selectedInstance();
                ls->setInstancePosition(selIdx,
                    args.inspectorNewPos[0], args.inspectorNewPos[1], args.inspectorNewPos[2]);

                // Update Block1 buffer + track FieldEdit for Rust parser save
                const LevelInstance* inst = ls->getInstance(selIdx);
                if (inst && inst->block1WtOffset > 0) {
                    g_levelReader.SetWorldTransform(inst->block1WtOffset, inst->mat);
                }
                if (inst && inst->guid != 0) {
                    ZeroEngine::FieldEdit fe;
                    memset(&fe.vec3Val, 0, sizeof(fe.vec3Val));
                    memset(&fe.matrixVal, 0, sizeof(fe.matrixVal));
                    fe.entityGuid = inst->guid;
                    fe.fieldName = "WorldTransform";
                    fe.kind = 4;
                    memcpy(fe.matrixVal, inst->mat, 64);
                    fe.intVal = 0; fe.floatVal = 0; fe.guidVal = 0; fe.listIndex = 0;
                    g_levelReader.AddFieldEdit(fe);
                }
            }

            // Full matrix edit (rotation/scale changed)
            if (args.inspectorMatrixChanged && ls->selectedInstance() >= 0) {
                int selIdx = ls->selectedInstance();
                LevelInstance* inst = const_cast<LevelInstance*>(ls->getInstance(selIdx));
                if (inst) {
                    memcpy(inst->mat, args.inspectorNewMatrix, 64);
                    // Recompute world-space AABB
                    if (inst->model && inst->model->hasBounds) {
                        const float* lmin = inst->model->localMin;
                        const float* lmax = inst->model->localMax;
                        const float* m = inst->mat;
                        inst->bboxMin[0]=inst->bboxMin[1]=inst->bboxMin[2]= 1e30f;
                        inst->bboxMax[0]=inst->bboxMax[1]=inst->bboxMax[2]=-1e30f;
                        for (int ci = 0; ci < 8; ++ci) {
                            float cx = (ci & 1) ? lmax[0] : lmin[0];
                            float cy = (ci & 2) ? lmax[1] : lmin[1];
                            float cz = (ci & 4) ? lmax[2] : lmin[2];
                            float wx = cx*m[0]+cy*m[4]+cz*m[8]+m[12];
                            float wy = cx*m[1]+cy*m[5]+cz*m[9]+m[13];
                            float wz = cx*m[2]+cy*m[6]+cz*m[10]+m[14];
                            if (wx<inst->bboxMin[0]) inst->bboxMin[0]=wx;
                            if (wy<inst->bboxMin[1]) inst->bboxMin[1]=wy;
                            if (wz<inst->bboxMin[2]) inst->bboxMin[2]=wz;
                            if (wx>inst->bboxMax[0]) inst->bboxMax[0]=wx;
                            if (wy>inst->bboxMax[1]) inst->bboxMax[1]=wy;
                            if (wz>inst->bboxMax[2]) inst->bboxMax[2]=wz;
                        }
                    }
                    // Write to Block1
                    if (inst->block1WtOffset > 0)
                        g_levelReader.SetWorldTransform(inst->block1WtOffset, inst->mat);
                    // Also update the display entry
                    const std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjs();
                    for (size_t gi = 0; gi < gos.size(); ++gi) {
                        if (gos[gi].guid == inst->guid) {
                            ZeroEngine::LevelGameObjEntry& e = g_levelReader.GetGameObjsMut()[gi];
                            memcpy(e.world_transform, inst->mat, 64);
                            e.pos[0] = inst->mat[12];
                            e.pos[1] = inst->mat[13];
                            e.pos[2] = inst->mat[14];
                            break;
                        }
                    }
                    // Track FieldEdit for Rust parser save
                    if (inst->guid != 0) {
                        ZeroEngine::FieldEdit fe;
                        memset(&fe.vec3Val, 0, sizeof(fe.vec3Val));
                        memset(&fe.matrixVal, 0, sizeof(fe.matrixVal));
                        fe.entityGuid = inst->guid;
                        fe.fieldName = "WorldTransform";
                        fe.kind = 4;
                        memcpy(fe.matrixVal, inst->mat, 64);
                        fe.intVal = 0; fe.floatVal = 0; fe.guidVal = 0; fe.listIndex = 0;
                        g_levelReader.AddFieldEdit(fe);
                    }
                }
            }

            // Editor object position edit from ImGui DragFloat
            if (args.inspectorPosChanged && ls->selectedEditorObj() >= 0 && ls->selectedInstance() < 0) {
                int selEO = ls->selectedEditorObj();
                ls->setEditorObjPosition(selEO,
                    args.inspectorNewPos[0], args.inspectorNewPos[1], args.inspectorNewPos[2]);
                // Mark as moved for Block1 write-back below
                // (reuse the same write-back path)
            }

            // Editor object drag write-back (also handles ImGui edits since mat is already updated)
            bool eoNeedsWriteBack = g_levelInspector.editorObjMoved() ||
                (args.inspectorPosChanged && ls->selectedEditorObj() >= 0 && ls->selectedInstance() < 0);
            if (eoNeedsWriteBack && ls->selectedEditorObj() >= 0) {
                const LevelEditorObj* eo = ls->getEditorObj(ls->selectedEditorObj());
                if (eo) {
                    // Write WorldTransform to Block1
                    if (eo->block1WtOffset > 0) {
                        g_levelReader.SetWorldTransform(eo->block1WtOffset, eo->mat);
                    }

                    // For spawn_node: also recalculate and write local Transform
                    bool isSpawnNode = (eo->type.find("spawn_node") == 0);
                    if (isSpawnNode && eo->block1TfOffset > 0 && eo->parentGuid != 0) {
                        // Find parent spawn_point
                        int parentIdx = ls->findEditorObjByGuid(eo->parentGuid);
                        const LevelEditorObj* parent = ls->getEditorObj(parentIdx);
                        if (parent) {
                            float localTf[16];
                            const float* spMat = parent->mat;
                            const float* snMat = eo->mat;

                            // Check if parent has identity rotation
                            const float eps = 0.001f;
                            bool identRot = (fabsf(spMat[0]-1.0f) < eps && fabsf(spMat[1]) < eps && fabsf(spMat[2]) < eps &&
                                             fabsf(spMat[4]) < eps && fabsf(spMat[5]-1.0f) < eps && fabsf(spMat[6]) < eps &&
                                             fabsf(spMat[8]) < eps && fabsf(spMat[9]) < eps && fabsf(spMat[10]-1.0f) < eps);

                            if (identRot) {
                                // Pattern A: Identity rotation
                                // Transform[12] = WT[12] - SP[12]  (X)
                                // Transform[13] = SP[13] + WT[13]  (Z special)
                                // Transform[14] = WT[14] - SP[14]  (Y)
                                for (int k = 0; k < 12; ++k) localTf[k] = snMat[k]; // copy rotation
                                localTf[12] = snMat[12] - spMat[12];
                                localTf[13] = spMat[13] + snMat[13];
                                localTf[14] = snMat[14] - spMat[14];
                                localTf[15] = 1.0f;
                            } else {
                                // Pattern B: Non-identity rotation
                                // offset = WT_pos - SP_pos (with Z negation)
                                float ox = snMat[12] - spMat[12];
                                float oz = -(spMat[13] + snMat[13]);
                                float oy = snMat[14] - spMat[14];

                                // R_sp_inv = transpose(R_sp) — rotation part of parent
                                // local_offset = R_sp_inv * offset
                                float lx = spMat[0]*ox + spMat[4]*oz + spMat[8]*oy;
                                float lz = spMat[1]*ox + spMat[5]*oz + spMat[9]*oy;
                                float ly = spMat[2]*ox + spMat[6]*oz + spMat[10]*oy;

                                // R_local = R_sp_inv * R_sn
                                for (int r = 0; r < 3; ++r) {
                                    for (int c = 0; c < 3; ++c) {
                                        float sum = 0;
                                        for (int k = 0; k < 3; ++k)
                                            sum += spMat[k*4+r] * snMat[k*4+c]; // transpose(sp) * sn
                                        localTf[r*4+c] = sum;
                                    }
                                }
                                localTf[3] = 0; localTf[7] = 0; localTf[11] = 0;
                                localTf[12] = lx;
                                localTf[13] = lz;
                                localTf[14] = ly;
                                localTf[15] = 1.0f;
                            }
                            g_levelReader.SetWorldTransform(eo->block1TfOffset, localTf);
                        }
                    }
                    // Track FieldEdit for Rust parser save
                    if (eo->guid != 0) {
                        ZeroEngine::FieldEdit fe;
                        memset(&fe.vec3Val, 0, sizeof(fe.vec3Val));
                        memset(&fe.matrixVal, 0, sizeof(fe.matrixVal));
                        fe.entityGuid = eo->guid;
                        fe.fieldName = "WorldTransform";
                        fe.kind = 4;
                        memcpy(fe.matrixVal, eo->mat, 64);
                        fe.intVal = 0; fe.floatVal = 0; fe.guidVal = 0; fe.listIndex = 0;
                        g_levelReader.AddFieldEdit(fe);
                    }
                }
                g_levelInspector.clearEditorObjMoved();
            }
        }
    }

    // Handle Save PAK request
    static bool s_savingInProgress = false;
    if (args.requestSavePak && g_levelReader.IsLoaded() && !s_savingInProgress)
    {
        g_savePakStatus = 1;
        g_savePakMessage = "Starting Rust parser save pipeline...";

        // Save to a versioned _modified path so repeated saves do not overwrite
        // the last testable artifact.
        std::string savePath = BuildVersionedModifiedPakPath(g_levelReader.GetPakPath());

        // ── BUG FIX (cross-session edit-and-save was nuking previously-saved entities) ──
        //
        // The OLD behavior here walked the loaded path back to its unmodified
        // ancestor (HelmsDeep_modified_v3.PAK -> HelmsDeep.PAK) on the theory
        // that we could "always work from a clean base" while in-memory deltas
        // (m_pendingObjs, m_fieldEdits, m_deletedGuids) carried every change.
        //
        // That worked within ONE session and exploded across sessions. The
        // failure: m_pendingObjs is in-memory only. Process restart wipes it.
        // After a save+reload from a _modified PAK in a fresh session, the
        // previously-baked entities exist ONLY in the loaded _modified PAK,
        // NOT in m_pendingObjs. Save then dumped the unmodified original
        // (which had no record of those entities), applied the user's edit
        // to a GUID that didn't exist there, and produced an output PAK
        // that silently lost everything created in earlier sessions.
        //
        // Fix: dump from the LOADED PAK, period. Whatever the user has open
        // is the source of truth. In-session deltas apply on top. No
        // walk-back, no "_modified" suffix stripping. If you want to start
        // from a clean original, manually load the original yourself.
        //
        // Proper architectural fix (deferred): persist the deltas to disk on
        // every save so the "always from original" design survives process
        // restarts. That's the right answer long-term but is way more code.
        // This 2-line surgical fix solves the immediate user-facing bug.
        std::string originalPak = g_levelReader.GetPakPath();

        s_savingInProgress = true;

        // Unload 3D scene
        if (g_renderer) g_renderer->unloadLevelScene();

        // ── Primary path: Rust parser dump → patch → compile ──
        if (RustParserSaveLevel(originalPak, g_levelReader.GetBinPath(), savePath)) {
            g_savePakStatus = 5;
            g_savePakMessage = "Saved via Rust parser: " + savePath;
            g_savePakDoneTimer = 3.0f;
            g_lastSavedPakPath = savePath; // tell DLL to update its path field
            // NOTE: Do NOT clear pending state here. The save pipeline uses
            // "always from original + all accumulated deltas" design. Clearing
            // m_pendingObjs/m_deletedGuids/m_fieldEdits would cause subsequent
            // saves to lose previously baked changes, since the original PAK
            // is always used as the base.
        } else {
            // Rust parser path failed — C++ SavePak fallback DISABLED
            g_savePakStatus = -1;
            g_savePakMessage = "Save FAILED: Rust parser pipeline failed. Check lotrc_rs.exe is accessible.";
            g_savePakDoneTimer = 5.0f;
        }
        // Scene was unloaded before save — always reload it from current
        // LevelReader state so the user doesn't see a blank viewport
        g_pendingSceneReload = true;
        s_savingInProgress = false;
    }

    // Handle Asset Tree → Map Highlight
    if (g_renderer && g_renderer->hasLevelScene()) {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls) {
            if (args.assetHighlightType == 1 && args.assetHighlightCrc != 0) {
                // Model CRC → find all instances
                std::vector<int> indices;
                ls->findInstancesByModelCrc(args.assetHighlightCrc, indices);
                if (!indices.empty())
                    ls->setAssetHighlight(&indices[0], (int)indices.size());
                else
                    ls->clearAssetHighlight();
            }
            else if ((args.assetHighlightType == 2 || args.assetHighlightType == 4) && args.assetHighlightCrc != 0) {
                // Texture/Material tex0 CRC → find all instances using that texture
                std::vector<int> indices;
                ls->findInstancesByTextureCrc(args.assetHighlightCrc, indices);
                if (!indices.empty())
                    ls->setAssetHighlight(&indices[0], (int)indices.size());
                else
                    ls->clearAssetHighlight();
            }
            else if (args.assetHighlightType == 3 && args.assetHighlightGameObjIdx >= 0) {
                // GameObj index → select matching instance or editor obj by GUID
                ls->clearAssetHighlight();
                const std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjs();
                if (args.assetHighlightGameObjIdx < (int)gos.size()) {
                    uint32_t guid = gos[args.assetHighlightGameObjIdx].guid;
                    // Try to find as a mesh instance first
                    bool found = false;
                    for (int i = 0; i < ls->getInstanceCount(); ++i) {
                        const LevelInstance* inst = ls->getInstance(i);
                        if (inst && inst->guid == guid) {
                            ls->setSelectedInstance(i);
                            ls->setSelectedEditorObj(-1);
                            found = true;
                            break;
                        }
                    }
                    // If not a mesh instance, try as editor obj
                    if (!found) {
                        int eoIdx = ls->findEditorObjByGuid(guid);
                        if (eoIdx >= 0) {
                            ls->setSelectedEditorObj(eoIdx);
                            ls->setSelectedInstance(-1);
                        }
                    }
                }
            }
            else {
                ls->clearAssetHighlight();
            }
        }
    }

    // Handle collision vis toggle from DLL
    if (args.collisionVisToggle && g_renderer && g_renderer->hasLevelScene()) {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls) ls->toggleCollisionVis();
    }

    // Handle Phase 3-5 visualization toggles from DLL
    if (g_renderer && g_renderer->hasLevelScene()) {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls) {
            if (args.togglePathLinks)     ls->setPathLinksVisible(!ls->pathLinksVisible());
            if (args.toggleCaptureRadii)  ls->setCaptureRadiiVisible(!ls->captureRadiiVisible());
            if (args.toggleSpawnChains)   ls->setSpawnChainsVisible(!ls->spawnChainsVisible());
            if (args.toggleAIGoals)       ls->setAIGoalsVisible(!ls->aiGoalsVisible());
            if (args.toggleSoundRadii)    ls->setSoundRadiiVisible(!ls->soundRadiiVisible());
        }
    }

    // Handle Level Validation request from DLL
    if (args.requestValidateLevel && g_levelReader.IsLoaded()) {
        static std::vector<ValidationIssue> s_validationIssues;
        int errCount = ValidateLevel(g_levelReader, s_validationIssues);
        int warnCount = 0;
        for (size_t vi = 0; vi < s_validationIssues.size(); ++vi) {
            if (s_validationIssues[vi].severity == VSEV_WARNING) ++warnCount;
        }
        // Store for next frame's args (DLL reads these)
        g_lastValidationErrors = errCount;
        g_lastValidationWarnings = warnCount;
    }

    // Handle Level Template request from DLL
    if (args.requestNewLevelTemplate > 0 && g_levelReader.IsLoaded()) {
        float cx = 0.0f, cz = 0.0f;
        switch (args.requestNewLevelTemplate) {
        case 1: CreateConquestTemplate(g_levelReader, cx, cz); break;
        case 2: CreateTDMTemplate(g_levelReader, cx, cz); break;
        case 3: CreateCTRTemplate(g_levelReader, cx, cz); break;
        }
    }

    // Handle Asset Inspector — cache DLL's request for next frame + process edits
    g_assetInspCat = args.assetInspectorRequestCat;
    g_assetInspIdx = args.assetInspectorRequestIdx;
    // Process asset inspector edit
    if (args.assetInspectorEditRequested && g_levelReader.IsLoaded()) {
        if (args.assetInspectorEditSize == 4) {
            // Determine if float or int from context (DLL sets both, host picks the right one)
            g_levelReader.SetFieldValue(args.assetInspectorEditOffset,
                                        &args.assetInspectorEditInt, 4);
        } else if (args.assetInspectorEditSize == 2) {
            uint16_t v16 = (uint16_t)(args.assetInspectorEditInt & 0xFFFF);
            g_levelReader.SetFieldValue(args.assetInspectorEditOffset, &v16, 2);
        } else if (args.assetInspectorEditSize == 1) {
            uint8_t v8 = (uint8_t)(args.assetInspectorEditInt & 0xFF);
            g_levelReader.SetFieldValue(args.assetInspectorEditOffset, &v8, 1);
        } else if (args.assetInspectorEditSize == 8) {
            // 8-byte: use two consecutive 4-byte writes
            g_levelReader.SetFieldValue(args.assetInspectorEditOffset,
                                        &args.assetInspectorEditInt, 4);
        }
    }

    // ---- Handle Model Viewer requests ----
    // Sync state from DLL
    g_mvViewerOpen   = (args.modelViewerOpen != 0);
    g_mvOrbitYaw     = args.modelViewerOrbitYaw;
    g_mvOrbitPitch   = args.modelViewerOrbitPitch;
    g_mvOrbitDist    = args.modelViewerOrbitDist;
    g_mvWireframe    = (args.modelViewerWireframe != 0);
    g_mvShowCollision = (args.modelViewerShowCollision != 0);

    // Push LOD + render/shader toggles into Scene3DRenderer. These drive
    // the actual GameModel render path; nothing else reads them. Guarded
    // on g_renderer because this code runs before initialize() completes
    // on some startup paths.
    if (g_renderer) {
        for (int i = 0; i < 6; ++i) {
            g_renderer->setLodVisible(i, args.modelViewerLodVisible[i] != 0);
        }
        g_renderer->setModelRenderMode(args.modelViewerRenderMode);
        g_renderer->setModelShaderMode(args.modelViewerShaderMode);

        // Per-pass kill switches — the user toggles these from the
        // Render Debug window to hunt the "extra layer" overlay
        // (DisplayWorld is the prime suspect).
        g_renderer->setPassEnableSkeleton    (args.passEnableSkeleton     != 0);
        g_renderer->setPassEnableMesh        (args.passEnableMesh         != 0);
        g_renderer->setPassEnablePlayerChar  (args.passEnablePlayerChar   != 0);
        g_renderer->setPassEnableDisplayWorld(args.passEnableDisplayWorld != 0);
        g_renderer->setPassEnableFx          (args.passEnableFx           != 0);

        // GameModel shader path. Path 2 routes through GameShaderCache
        // with a specific PS name; Scene3DRenderer holds onto the
        // string and looks it up when binding for that pipeline.
        g_renderer->setGameModelShaderPath(args.gameModelShaderPath);
        g_renderer->setGameModelShaderPSName(args.gameModelShaderPSName);

        // Diagnostic toggles: tint, force-pass, part-visible overrides,
        // and the one-shot frame dump request. The renderer clears the
        // dump-requested flag after writing, so we set it here every
        // frame the user has the button still held — that's intended.
        g_renderer->setTintPasses(args.renderDebugTintPasses != 0);
        g_renderer->setForcePassOnly(args.renderDebugForcePass);
        for (int i = 0; i < 64; ++i) {
            g_renderer->setPartVisibleOverride(i, args.renderDebugPartVisible[i] != 0);
        }
        if (args.renderDebugDumpRequested) {
            g_renderer->requestFrameDump();
        }
    }

    // Open model viewer from asset tree or inspector
    if (args.modelViewerRequestCrc != 0) {
        g_mvViewerOpen = true;
        g_mvModelCrc = args.modelViewerRequestCrc;
        // Find model name
        const std::vector<ZeroEngine::LevelModelEntry>& mlist = g_levelReader.GetModels();
        g_mvModelName = "";
        for (int mi = 0; mi < (int)mlist.size(); ++mi) {
            if (mlist[mi].crc == g_mvModelCrc) {
                g_mvModelName = mlist[mi].name;
                break;
            }
        }
        // Reset collision state
        g_mvHasCollision = false;
        g_mvCollisionTris = 0;
        g_mvCollVerts.clear();
        g_mvCollIndices.clear();
        g_mvMoppData.clear();
        g_mvBuildProgress = -1;
        g_mvWireDirty = true;  // F4 wireframe edge cache must rebuild on next render

        // Check existing collision from all sources
        if (g_levelReader.ModelHasCollision(g_mvModelCrc)) {
            g_mvHasCollision = true;
            // Count triangles from HkShapeInfo BVTree entries
            const std::vector<uint8_t>& b1 = g_levelReader.GetBlock1();
            const ZeroEngine::PakHeader& ph = g_levelReader.GetPakHeader();
            uint32_t hkBase = ph.hk_shape_info_offset;
            uint32_t hkCount = ph.hk_shape_info_num;
            uint32_t hkStride = ph.hk_shape_info_size;
            if (hkStride == 0) hkStride = 80;
            int totalTris = 0;
            for (uint32_t hi = 0; hi < hkCount; ++hi) {
                uint32_t off = hkBase + hi * hkStride;
                if (off + hkStride > b1.size()) break;
                uint32_t kind, key, triNum;
                memcpy(&kind, &b1[off + 32], 4);
                memcpy(&key,  &b1[off + 36], 4);
                memcpy(&triNum, &b1[off + 72], 4);
                if (kind == 6 && key == g_mvModelCrc)
                    totalTris += (int)triNum;
            }
            // Also count from per-model ShapeInfo
            uint32_t miBase = ph.model_info_offset;
            for (uint32_t mi = 0; mi < ph.model_info_num; ++mi) {
                uint32_t off = miBase + mi * 256;
                if (off + 256 > b1.size()) break;
                uint32_t key, shapeNum;
                memcpy(&key, &b1[off], 4);
                if (key == g_mvModelCrc) {
                    memcpy(&shapeNum, &b1[off + 228], 4);
                    if (shapeNum > 0 && shapeNum < 10000)
                        totalTris += (int)shapeNum;
                    break;
                }
            }
            g_mvCollisionTris = totalTris;
        }
        // Auto-set orbit distance from model bounds
        if (g_renderer && g_renderer->hasLevelScene()) {
            LevelModel* mdl = g_renderer->getLevelScene()->getModelByCrc(g_mvModelCrc);
            if (mdl && mdl->hasBounds) {
                float dx = mdl->localMax[0] - mdl->localMin[0];
                float dy = mdl->localMax[1] - mdl->localMin[1];
                float dz = mdl->localMax[2] - mdl->localMin[2];
                float maxDim = dx; if (dy > maxDim) maxDim = dy; if (dz > maxDim) maxDim = dz;
                g_mvOrbitDist = maxDim * 1.5f;
                g_mvOrbitYaw = 0.0f;
                g_mvOrbitPitch = 0.3f;
            }
        }
    }

    // Build Collision request
    if (args.modelViewerBuildRequested && g_mvModelCrc != 0 &&
        g_renderer && g_renderer->hasLevelScene()) {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls) {
            g_mvBuildProgress = 0;
            g_mvCollVerts.clear();
            g_mvCollIndices.clear();
            g_mvMoppData.clear();
            g_mvWireDirty = true;  // F4 wireframe edge cache must rebuild on next render

            if (ls->getModelCollisionData(g_mvModelCrc, g_mvCollVerts, g_mvCollIndices)) {
                g_mvCollisionTris = (int)(g_mvCollIndices.size() / 3);
                g_mvHasCollision = true;
                // Generate MOPP BVTree bytecode via Havok
                g_mvBuildProgress = 50;
                g_mvMoppData.clear();
                {
                    unsigned char* moppBuf = NULL;
                    int moppSz = 0;
                    if (BuildMoppFromMesh(&g_mvCollVerts[0], (int)(g_mvCollVerts.size()/3),
                                          &g_mvCollIndices[0], g_mvCollisionTris,
                                          &moppBuf, &moppSz)) {
                        g_mvMoppData.assign(moppBuf, moppBuf + moppSz);
                        free(moppBuf);
                    }
                }
                g_mvBuildProgress = 100;
            } else {
                g_mvBuildProgress = -1;
                OutputDebugStringA("[MV_BUILD] getModelCollisionData FAILED\n");
            }
        }
    }

    // Save collision to PAK request — add pending collision AND trigger save
    // Debug: log every frame if save was requested
    if (args.modelViewerSaveRequested) {
        FILE* mvdbg = fopen("mv_save_debug.txt", "a");
        if (mvdbg) {
            fprintf(mvdbg, "SaveRequested: hasColl=%d crc=0x%08X loaded=%d verts=%d indices=%d mopp=%d\n",
                    g_mvHasCollision?1:0, g_mvModelCrc, g_levelReader.IsLoaded()?1:0,
                    (int)g_mvCollVerts.size(), (int)g_mvCollIndices.size(), (int)g_mvMoppData.size());
            fclose(mvdbg);
        }
    }
    if (args.modelViewerSaveRequested && g_mvHasCollision && g_mvModelCrc != 0 &&
        g_levelReader.IsLoaded()) {
        // ================================================================
        // Collision Save Pipeline (Python→Rust subprocess):
        //   1. Export collision_export.json (same as the Export JSON button)
        //   2. Spawn collision_repack.py which:
        //      a) Dumps the PAK via lotrc_rs -d
        //      b) Patches the model's shapes section with our collision data
        //      c) Recompiles via lotrc_rs -c
        //   3. Poll collision_progress.txt for status updates
        // ================================================================
        g_savePakStatus = 1;
        g_savePakMessage = "Exporting collision JSON...";

        // Derive output PAK path (_modified)
        std::string savePath = g_levelReader.GetPakPath();
        size_t modPos2 = savePath.find("_modified");
        if (modPos2 != std::string::npos)
            savePath = savePath.substr(0, modPos2) + savePath.substr(modPos2 + 9);
        size_t dotPos2 = savePath.rfind('.');
        if (dotPos2 != std::string::npos)
            savePath.insert(dotPos2, "_modified");
        else
            savePath += "_modified";

        // Step 1: Write collision_export.json (in current working directory, same as EXE)
        std::string collJsonPath = "collision_export.json";

        FILE* jf = fopen(collJsonPath.c_str(), "w");
        if (jf) {
            float minB[3] = {1e30f, 1e30f, 1e30f};
            float maxB[3] = {-1e30f, -1e30f, -1e30f};
            int numVerts = (int)(g_mvCollVerts.size() / 3);
            int numTris = (int)(g_mvCollIndices.size() / 3);
            for (int vi = 0; vi < numVerts; ++vi) {
                float vx = g_mvCollVerts[vi*3], vy = g_mvCollVerts[vi*3+1], vz = g_mvCollVerts[vi*3+2];
                if (vx < minB[0]) minB[0] = vx; if (vy < minB[1]) minB[1] = vy; if (vz < minB[2]) minB[2] = vz;
                if (vx > maxB[0]) maxB[0] = vx; if (vy > maxB[1]) maxB[1] = vy; if (vz > maxB[2]) maxB[2] = vz;
            }
            float maxExt = 0;
            for (int a = 0; a < 3; ++a) { float e = maxB[a]-minB[a]; if (e > maxExt) maxExt = e; }
            float treeScale = 254.f * 256.f * 256.f / (maxExt + 0.1f);

            fprintf(jf, "{\n  \"shapes\": [{\n");
            fprintf(jf, "    \"info\": {\n");
            fprintf(jf, "      \"offset\": 0, \"kind\": 1, \"unk_2\": 0,\n");
            fprintf(jf, "      \"unk_3\": 0.0, \"unk_4\": 0.0, \"unk_5\": 0.0,\n");
            fprintf(jf, "      \"translation\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0},\n");
            fprintf(jf, "      \"rotation\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0, \"w\": 1.0},\n");
            fprintf(jf, "      \"unk_13\": 1.0, \"unk_14\": 0.0, \"unk_15\": 0.0, \"unk_16\": 0.0,\n");
            fprintf(jf, "      \"unk_17\": 1.0, \"unk_18\": 0.0, \"unk_19\": 0.0, \"unk_20\": 0.0,\n");
            fprintf(jf, "      \"unk_21\": 1.0, \"unk_22\": 0.0, \"unk_23\": 0.5, \"unk_24\": 0.0,\n");
            fprintf(jf, "      \"unk_25\": 0.0, \"unk_26\": 0.0,\n");
            fprintf(jf, "      \"hk_shape_num\": 1, \"hk_shape_offset\": 0,\n");
            fprintf(jf, "      \"unk_29a\": 0, \"unk_29b\": 0, \"unk_29c\": 0, \"unk_29d\": 0,\n");
            fprintf(jf, "      \"unk_30\": -1.0\n");
            fprintf(jf, "    },\n");
            fprintf(jf, "    \"extra\": null,\n");
            fprintf(jf, "    \"hk_shapes\": [{\n");
            fprintf(jf, "      \"BVTreeMesh\": {\n");
            fprintf(jf, "        \"info\": {\n");
            fprintf(jf, "          \"translation\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0, \"w\": 0.0},\n");
            fprintf(jf, "          \"rotation\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0, \"w\": 1.0},\n");
            fprintf(jf, "          \"key\": \"\",\n");
            fprintf(jf, "          \"offset\": {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f},\n",
                    minB[0]-0.05f, minB[1]-0.05f, minB[2]-0.05f);
            fprintf(jf, "          \"tree_scale\": %.6f\n", treeScale);
            fprintf(jf, "        },\n");
            fprintf(jf, "        \"shape\": {\n");
            fprintf(jf, "          \"tree\": \"");
            for (size_t mi = 0; mi < g_mvMoppData.size(); ++mi)
                fprintf(jf, "%02x", g_mvMoppData[mi]);
            fprintf(jf, "\",\n");
            fprintf(jf, "          \"verts\": [\n");
            for (int vi = 0; vi < numVerts; ++vi) {
                fprintf(jf, "            {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f}%s\n",
                        g_mvCollVerts[vi*3], g_mvCollVerts[vi*3+1], g_mvCollVerts[vi*3+2],
                        (vi < numVerts-1) ? "," : "");
            }
            fprintf(jf, "          ],\n");
            fprintf(jf, "          \"inds\": [");
            for (int ii = 0; ii < numTris*3; ++ii) {
                fprintf(jf, "%d%s", g_mvCollIndices[ii], (ii < numTris*3-1) ? ", " : "");
            }
            fprintf(jf, "]\n");
            fprintf(jf, "        }\n      }\n    }]\n  }]\n}\n");
            fclose(jf);
        } else {
            g_savePakStatus = -1;
            g_savePakMessage = "Failed to write collision_export.json";
            g_savePakDoneTimer = 5.0f;
        }

        // Step 2: Find collision_repack.py and lotrc_rs.exe, spawn Python subprocess
        if (g_savePakStatus != -1) {
            g_savePakStatus = 2;
            g_savePakMessage = "Running collision repack pipeline...";

            // Find Python
            const char* pythonCandidates[] = { "python", "python3", "py", NULL };
            std::string pythonExe;
            for (int pi = 0; pythonCandidates[pi]; ++pi) {
                char testCmd[256];
                sprintf_s(testCmd, "%s --version", pythonCandidates[pi]);
                if (RunSubprocess(testCmd, 5000)) {
                    pythonExe = pythonCandidates[pi];
                    break;
                }
            }

            // Find collision_repack.py
            std::string repackPy;
            const char* pyCandidates[] = {
                "Vespucci\\Tools\\Python\\collision_repack.py",
                "Scene3D\\Vespucci\\Tools\\Python\\collision_repack.py",
                "..\\Scene3D\\Vespucci\\Tools\\Python\\collision_repack.py",
                "collision_repack.py",
                "Scene3D\\collision_repack.py",
                "..\\Scene3D\\collision_repack.py",
                NULL
            };
            for (int pi = 0; pyCandidates[pi]; ++pi) {
                if (GetFileAttributesA(pyCandidates[pi]) != INVALID_FILE_ATTRIBUTES) {
                    repackPy = pyCandidates[pi];
                    break;
                }
            }

            // Find lotrc_rs.exe
            std::string lotrcExe = FindLotrcRsExe();

            if (pythonExe.empty()) {
                g_savePakStatus = -1;
                g_savePakMessage = "Python not found — install Python and add to PATH";
                g_savePakDoneTimer = 5.0f;
            } else if (repackPy.empty()) {
                g_savePakStatus = -1;
                g_savePakMessage = "collision_repack.py not found";
                g_savePakDoneTimer = 5.0f;
            } else if (lotrcExe.empty()) {
                g_savePakStatus = -1;
                g_savePakMessage = "lotrc_rs.exe not found — place next to EXE or in tools/";
                g_savePakDoneTimer = 5.0f;
            } else {
                // Build command: python collision_repack.py <pak> <json> <model_name> <lotrc> <output>
                char repackCmd[4096];
                sprintf_s(repackCmd, "\"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"",
                          pythonExe.c_str(),
                          repackPy.c_str(),
                          g_levelReader.GetPakPath().c_str(),
                          collJsonPath.c_str(),
                          g_mvModelName.c_str(),
                          lotrcExe.c_str(),
                          savePath.c_str());

                DebugLog(repackCmd);

                g_savePakStatus = 3;
                g_savePakMessage = "Rust parser: dumping + patching + recompiling...";

                if (RunSubprocess(repackCmd, 180000)) { // 3 minute timeout
                    // Check if collision_progress.txt says DONE
                    // The script writes this next to itself, which is in CWD
                    FILE* pf = fopen("collision_progress.txt", "r");
                    if (pf) {
                        char line[512] = {0};
                        fgets(line, sizeof(line), pf);
                        fclose(pf);
                        if (strncmp(line, "DONE:", 5) == 0) {
                            g_mvHasCollision = true;
                            g_savePakStatus = 5;
                            g_savePakMessage = "Collision saved via Rust pipeline: " + savePath;
                            g_savePakDoneTimer = 3.0f;
                        } else if (strncmp(line, "ERROR:", 6) == 0) {
                            g_savePakStatus = -1;
                            g_savePakMessage = std::string("Repack error: ") + (line + 6);
                            g_savePakDoneTimer = 5.0f;
                        } else {
                            g_savePakStatus = 5;
                            g_savePakMessage = "Collision pipeline finished: " + savePath;
                            g_savePakDoneTimer = 3.0f;
                        }
                    } else {
                        g_savePakStatus = 5;
                        g_savePakMessage = "Collision pipeline finished (no progress file): " + savePath;
                        g_savePakDoneTimer = 3.0f;
                    }
                } else {
                    g_savePakStatus = -1;
                    g_savePakMessage = "Collision repack subprocess failed or timed out";
                    g_savePakDoneTimer = 5.0f;
                }
            }
        }
    }

    // Export Collision JSON — dumps MOPP + verts + indices in Rust parser format
    if (args.modelViewerBuildRequested == 2 && g_mvHasCollision && g_mvModelCrc != 0) {
        // Build the JSON matching the Rust parser's shapes format
        FILE* jf = fopen("collision_export.json", "w");
        if (jf) {
            // Compute aabb_min and tree_scale
            float minB[3] = {1e30f, 1e30f, 1e30f};
            float maxB[3] = {-1e30f, -1e30f, -1e30f};
            int numVerts = (int)(g_mvCollVerts.size() / 3);
            int numTris = (int)(g_mvCollIndices.size() / 3);
            for (int vi = 0; vi < numVerts; ++vi) {
                float vx = g_mvCollVerts[vi*3], vy = g_mvCollVerts[vi*3+1], vz = g_mvCollVerts[vi*3+2];
                if (vx < minB[0]) minB[0] = vx; if (vy < minB[1]) minB[1] = vy; if (vz < minB[2]) minB[2] = vz;
                if (vx > maxB[0]) maxB[0] = vx; if (vy > maxB[1]) maxB[1] = vy; if (vz > maxB[2]) maxB[2] = vz;
            }
            float maxExt = 0;
            for (int a = 0; a < 3; ++a) { float e = maxB[a]-minB[a]; if (e > maxExt) maxExt = e; }
            float treeScale = 254.f * 256.f * 256.f / (maxExt + 0.1f);

            fprintf(jf, "{\n  \"shapes\": [{\n");
            fprintf(jf, "    \"info\": {\n");
            fprintf(jf, "      \"offset\": 0, \"kind\": 1, \"unk_2\": 0,\n");
            fprintf(jf, "      \"unk_3\": 0.0, \"unk_4\": 0.0, \"unk_5\": 0.0,\n");
            fprintf(jf, "      \"translation\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0},\n");
            fprintf(jf, "      \"rotation\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0, \"w\": 1.0},\n");
            fprintf(jf, "      \"unk_13\": 1.0, \"unk_14\": 0.0, \"unk_15\": 0.0, \"unk_16\": 0.0,\n");
            fprintf(jf, "      \"unk_17\": 1.0, \"unk_18\": 0.0, \"unk_19\": 0.0, \"unk_20\": 0.0,\n");
            fprintf(jf, "      \"unk_21\": 1.0, \"unk_22\": 0.0, \"unk_23\": 0.5, \"unk_24\": 0.0,\n");
            fprintf(jf, "      \"unk_25\": 0.0, \"unk_26\": 0.0,\n");
            fprintf(jf, "      \"hk_shape_num\": 1, \"hk_shape_offset\": 0,\n");
            fprintf(jf, "      \"unk_29a\": 0, \"unk_29b\": 0, \"unk_29c\": 0, \"unk_29d\": 0,\n");
            fprintf(jf, "      \"unk_30\": -1.0\n");
            fprintf(jf, "    },\n");
            fprintf(jf, "    \"extra\": null,\n");
            fprintf(jf, "    \"hk_shapes\": [{\n");
            fprintf(jf, "      \"BVTreeMesh\": {\n");
            fprintf(jf, "        \"info\": {\n");
            fprintf(jf, "          \"translation\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0, \"w\": 0.0},\n");
            fprintf(jf, "          \"rotation\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0, \"w\": 1.0},\n");
            fprintf(jf, "          \"key\": \"\",\n");
            fprintf(jf, "          \"offset\": {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f},\n",
                    minB[0]-0.05f, minB[1]-0.05f, minB[2]-0.05f);
            fprintf(jf, "          \"tree_scale\": %.6f\n", treeScale);
            fprintf(jf, "        },\n");
            fprintf(jf, "        \"shape\": {\n");

            // MOPP tree as hex string
            fprintf(jf, "          \"tree\": \"");
            for (size_t mi = 0; mi < g_mvMoppData.size(); ++mi)
                fprintf(jf, "%02x", g_mvMoppData[mi]);
            fprintf(jf, "\",\n");

            // Vertices
            fprintf(jf, "          \"verts\": [\n");
            for (int vi = 0; vi < numVerts; ++vi) {
                fprintf(jf, "            {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f}%s\n",
                        g_mvCollVerts[vi*3], g_mvCollVerts[vi*3+1], g_mvCollVerts[vi*3+2],
                        (vi < numVerts-1) ? "," : "");
            }
            fprintf(jf, "          ],\n");

            // Indices
            fprintf(jf, "          \"inds\": [");
            for (int ii = 0; ii < numTris*3; ++ii) {
                fprintf(jf, "%d%s", g_mvCollIndices[ii], (ii < numTris*3-1) ? ", " : "");
            }
            fprintf(jf, "]\n");

            fprintf(jf, "        }\n");
            fprintf(jf, "      }\n");
            fprintf(jf, "    }]\n");
            fprintf(jf, "  }]\n");
            fprintf(jf, "}\n");
            fclose(jf);

            g_savePakStatus = 5;
            g_savePakMessage = "Collision exported to collision_export.json";
            g_savePakDoneTimer = 3.0f;
        }
    }

    // Handle Entity Creation request
    if (args.createEntityRequested && g_levelReader.IsLoaded())
    {
        ZeroEngine::PendingGameObj pgo;
        pgo.type_def_index = args.createEntityTypeDefIdx;
        pgo.parent_guid = args.createEntityParentGuid;
        pgo.gamemodemask = args.createEntityGameModeMask;
        pgo.layer_guid = args.createEntityLayerGuid;
        pgo.name_crc = ZeroEngine::LotrHashString(args.createEntityName);
        pgo.name_str = args.createEntityName;
        pgo.mesh_crc = args.createEntityMeshCrc;
        // Build identity matrix with the chosen position
        memset(pgo.world_transform, 0, sizeof(pgo.world_transform));
        pgo.world_transform[0] = pgo.world_transform[5] = pgo.world_transform[10] = pgo.world_transform[15] = 1.0f;
        pgo.world_transform[12] = args.createEntityPos[0];
        pgo.world_transform[13] = args.createEntityPos[1];
        pgo.world_transform[14] = args.createEntityPos[2];
        g_levelReader.AddPendingEntity(pgo);

        // ── Smart Chain Creation ──
        // Based on level.json analysis: auto-create required child entities
        // so the user gets a complete, functional chain instead of a single orphan.
        {
            std::string typeName;
            if (args.createEntityTypeDefIdx >= 0 &&
                args.createEntityTypeDefIdx < (int)g_levelReader.GetGameObjTypes().size()) {
                typeName = g_levelReader.GetGameObjTypes()[args.createEntityTypeDefIdx].name;
            }
            float px = args.createEntityPos[0], py = args.createEntityPos[1], pz = args.createEntityPos[2];
            int gmm = args.createEntityGameModeMask;
            uint32_t layGuid = args.createEntityLayerGuid;

            // Helper: find type def index by name (VS2005-safe, no lambda)
            #define FIND_TDEF(name) FindTypeDefByName(g_levelReader, name)

            if (typeName == "spawn_emitter") {
                // Auto-create: 2 spawn_class + 1 spawn_point + 4 spawn_nodes
                int scIdx = FIND_TDEF("spawn_class");
                int spIdx = FIND_TDEF("spawn_point");
                int snIdx = FIND_TDEF("spawn_node");
                if (scIdx >= 0) {
                    ZeroEngine::PendingGameObj sc;
                    sc.type_def_index = scIdx; sc.gamemodemask = gmm; sc.layer_guid = layGuid;
                    sc.name_str = std::string(args.createEntityName) + "_Warrior";
                    sc.name_crc = ZeroEngine::LotrHashString(sc.name_str.c_str());
                    memcpy(sc.world_transform, pgo.world_transform, 64);
                    g_levelReader.AddPendingEntity(sc);
                    sc.name_str = std::string(args.createEntityName) + "_Archer";
                    sc.name_crc = ZeroEngine::LotrHashString(sc.name_str.c_str());
                    g_levelReader.AddPendingEntity(sc);
                }
                if (spIdx >= 0) {
                    ZeroEngine::PendingGameObj sp;
                    sp.type_def_index = spIdx; sp.gamemodemask = gmm; sp.layer_guid = layGuid;
                    sp.name_str = std::string(args.createEntityName) + "_Point";
                    sp.name_crc = ZeroEngine::LotrHashString(sp.name_str.c_str());
                    memcpy(sp.world_transform, pgo.world_transform, 64);
                    g_levelReader.AddPendingEntity(sp);
                }
                if (snIdx >= 0) {
                    float offsets[4][2] = {{-3,0},{3,0},{0,-3},{0,3}};
                    for (int ni = 0; ni < 4; ++ni) {
                        ZeroEngine::PendingGameObj sn;
                        sn.type_def_index = snIdx; sn.gamemodemask = gmm; sn.layer_guid = layGuid;
                        char nbuf[128]; _snprintf(nbuf, sizeof(nbuf), "%s_Node%d", args.createEntityName, ni+1);
                        sn.name_str = nbuf;
                        sn.name_crc = ZeroEngine::LotrHashString(sn.name_str.c_str());
                        memcpy(sn.world_transform, pgo.world_transform, 64);
                        sn.world_transform[12] = px + offsets[ni][0];
                        sn.world_transform[14] = pz + offsets[ni][1];
                        g_levelReader.AddPendingEntity(sn);
                    }
                }
            }
            else if (typeName == "CapturePoint") {
                // Auto-create: trigger_radius (capture zone)
                int trIdx = FIND_TDEF("trigger_radius");
                if (trIdx >= 0) {
                    ZeroEngine::PendingGameObj tr;
                    tr.type_def_index = trIdx; tr.gamemodemask = gmm; tr.layer_guid = layGuid;
                    tr.name_str = std::string(args.createEntityName) + "_Area";
                    tr.name_crc = ZeroEngine::LotrHashString(tr.name_str.c_str());
                    memcpy(tr.world_transform, pgo.world_transform, 64);
                    g_levelReader.AddPendingEntity(tr);
                }
            }
            else if (typeName == "PathNetwork") {
                // Auto-create: 2 PathNodes + 1 PathLink
                int pnIdx = FIND_TDEF("PathNode");
                int plIdx = FIND_TDEF("PathLink");
                if (pnIdx >= 0) {
                    ZeroEngine::PendingGameObj pn;
                    pn.type_def_index = pnIdx; pn.gamemodemask = -1; pn.layer_guid = layGuid;
                    pn.name_str = std::string(args.createEntityName) + "_Node1";
                    pn.name_crc = ZeroEngine::LotrHashString(pn.name_str.c_str());
                    memcpy(pn.world_transform, pgo.world_transform, 64);
                    pn.world_transform[12] = px - 20;
                    g_levelReader.AddPendingEntity(pn);
                    pn.name_str = std::string(args.createEntityName) + "_Node2";
                    pn.name_crc = ZeroEngine::LotrHashString(pn.name_str.c_str());
                    pn.world_transform[12] = px + 20;
                    g_levelReader.AddPendingEntity(pn);
                }
                if (plIdx >= 0) {
                    ZeroEngine::PendingGameObj pl;
                    pl.type_def_index = plIdx; pl.gamemodemask = -1; pl.layer_guid = layGuid;
                    pl.name_str = std::string(args.createEntityName) + "_Link1";
                    pl.name_crc = ZeroEngine::LotrHashString(pl.name_str.c_str());
                    memcpy(pl.world_transform, pgo.world_transform, 64);
                    g_levelReader.AddPendingEntity(pl);
                }
            }
            else if (typeName == "PlayerRespawner") {
                // Auto-create: 2 spawn_emitters (Team1 + Team2) with their chains
                int seIdx = FIND_TDEF("spawn_emitter");
                int scIdx = FIND_TDEF("spawn_class");
                int spIdx = FIND_TDEF("spawn_point");
                int snIdx = FIND_TDEF("spawn_node");
                for (int team = 1; team <= 2; ++team) {
                    char teamLabel[16]; _snprintf(teamLabel, sizeof(teamLabel), "T%d", team);
                    if (seIdx >= 0) {
                        ZeroEngine::PendingGameObj se;
                        se.type_def_index = seIdx; se.gamemodemask = gmm; se.layer_guid = layGuid;
                        char nbuf[128]; _snprintf(nbuf, sizeof(nbuf), "%s_%s_Emitter", args.createEntityName, teamLabel);
                        se.name_str = nbuf; se.name_crc = ZeroEngine::LotrHashString(se.name_str.c_str());
                        memcpy(se.world_transform, pgo.world_transform, 64);
                        se.world_transform[14] = pz + (team == 1 ? -40.0f : 40.0f);
                        g_levelReader.AddPendingEntity(se);
                    }
                    if (scIdx >= 0) {
                        ZeroEngine::PendingGameObj sc;
                        sc.type_def_index = scIdx; sc.gamemodemask = gmm; sc.layer_guid = layGuid;
                        char nbuf[128]; _snprintf(nbuf, sizeof(nbuf), "%s_%s_Class", args.createEntityName, teamLabel);
                        sc.name_str = nbuf; sc.name_crc = ZeroEngine::LotrHashString(sc.name_str.c_str());
                        memcpy(sc.world_transform, pgo.world_transform, 64);
                        g_levelReader.AddPendingEntity(sc);
                    }
                    if (spIdx >= 0) {
                        ZeroEngine::PendingGameObj sp;
                            sp.type_def_index = spIdx; sp.gamemodemask = gmm; sp.layer_guid = layGuid;
                        char nbuf[128]; _snprintf(nbuf, sizeof(nbuf), "%s_%s_Point", args.createEntityName, teamLabel);
                        sp.name_str = nbuf; sp.name_crc = ZeroEngine::LotrHashString(sp.name_str.c_str());
                        memcpy(sp.world_transform, pgo.world_transform, 64);
                        sp.world_transform[14] = pz + (team == 1 ? -40.0f : 40.0f);
                        g_levelReader.AddPendingEntity(sp);
                    }
                    if (snIdx >= 0) {
                        for (int ni = 0; ni < 2; ++ni) {
                            ZeroEngine::PendingGameObj sn;
                                sn.type_def_index = snIdx; sn.gamemodemask = gmm; sn.layer_guid = layGuid;
                            char nbuf[128]; _snprintf(nbuf, sizeof(nbuf), "%s_%s_Node%d", args.createEntityName, teamLabel, ni+1);
                            sn.name_str = nbuf; sn.name_crc = ZeroEngine::LotrHashString(sn.name_str.c_str());
                            memcpy(sn.world_transform, pgo.world_transform, 64);
                            sn.world_transform[12] = px + (ni == 0 ? -3.0f : 3.0f);
                            sn.world_transform[14] = pz + (team == 1 ? -40.0f : 40.0f);
                            g_levelReader.AddPendingEntity(sn);
                        }
                    }
                }
            }
            else if (typeName == "PointManager") {
                // Auto-create: logic_endgame (victory target)
                int leIdx = FIND_TDEF("logic_endgame");
                if (leIdx >= 0) {
                    ZeroEngine::PendingGameObj le;
                    le.type_def_index = leIdx; le.gamemodemask = gmm; le.layer_guid = layGuid;
                    le.name_str = "logic_endgame";
                    le.name_crc = ZeroEngine::LotrHashString(le.name_str.c_str());
                    memcpy(le.world_transform, pgo.world_transform, 64);
                    g_levelReader.AddPendingEntity(le);
                }
            }
            #undef FIND_TDEF
        }

        // Add to 3D scene for immediate display
        if (g_renderer && g_renderer->hasLevelScene()) {
            LevelScene* ls = g_renderer->getLevelScene();
            if (ls) {
                uint32_t meshCrc = args.createEntityMeshCrc;
                LevelModel* model = meshCrc ? ls->getModelByCrc(meshCrc) : NULL;

                if (model) {
                    // Create a rendered 3D instance (existing model in cache)
                    LevelInstance inst;
                    inst.model = model;
                    memset(inst.mat, 0, sizeof(inst.mat));
                    inst.mat[0] = inst.mat[5] = inst.mat[10] = inst.mat[15] = 1.0f;
                    inst.mat[12] = args.createEntityPos[0];
                    inst.mat[13] = args.createEntityPos[1];
                    inst.mat[14] = args.createEntityPos[2];
                    inst.objName = args.createEntityName;
                    inst.guid = g_levelReader.GetGameObjs().back().guid;
                    inst.parentGuid = args.createEntityParentGuid;
                    inst.gameModeMask = args.createEntityGameModeMask;
                    inst.block1WtOffset = 0;
                    // Resolve mesh name and type name
                    const std::vector<ZeroEngine::LevelModelEntry>& models = g_levelReader.GetModels();
                    for (int mi = 0; mi < (int)models.size(); ++mi) {
                        if (models[mi].crc == meshCrc) {
                            inst.meshName = models[mi].name;
                            break;
                        }
                    }
                    if (args.createEntityTypeDefIdx >= 0 &&
                        args.createEntityTypeDefIdx < (int)g_levelReader.GetGameObjTypes().size()) {
                        inst.typeName = g_levelReader.GetGameObjTypes()[args.createEntityTypeDefIdx].name;
                    }
                    // Compute world-space AABB
                    if (model->hasBounds) {
                        const float* lmin = model->localMin;
                        const float* lmax = model->localMax;
                        const float* m = inst.mat;
                        inst.bboxMin[0]=inst.bboxMin[1]=inst.bboxMin[2]= 1e30f;
                        inst.bboxMax[0]=inst.bboxMax[1]=inst.bboxMax[2]=-1e30f;
                        for (int ci = 0; ci < 8; ++ci) {
                            float cx = (ci & 1) ? lmax[0] : lmin[0];
                            float cy = (ci & 2) ? lmax[1] : lmin[1];
                            float cz = (ci & 4) ? lmax[2] : lmin[2];
                            float wx = cx*m[0]+cy*m[4]+cz*m[8]+m[12];
                            float wy = cx*m[1]+cy*m[5]+cz*m[9]+m[13];
                            float wz = cx*m[2]+cy*m[6]+cz*m[10]+m[14];
                            if (wx<inst.bboxMin[0]) inst.bboxMin[0]=wx;
                            if (wy<inst.bboxMin[1]) inst.bboxMin[1]=wy;
                            if (wz<inst.bboxMin[2]) inst.bboxMin[2]=wz;
                            if (wx>inst.bboxMax[0]) inst.bboxMax[0]=wx;
                            if (wy>inst.bboxMax[1]) inst.bboxMax[1]=wy;
                            if (wz>inst.bboxMax[2]) inst.bboxMax[2]=wz;
                        }
                    } else {
                        float px = inst.mat[12], py = inst.mat[13], pz = inst.mat[14];
                        float r = 15.0f;
                        inst.bboxMin[0]=px-r; inst.bboxMin[1]=py-r; inst.bboxMin[2]=pz-r;
                        inst.bboxMax[0]=px+r; inst.bboxMax[1]=py+r; inst.bboxMax[2]=pz+r;
                    }
                    ls->addInstance(inst);
                } else {
                    // No mesh selected or model not in cache — add green editor sphere
                    LevelEditorObj eo;
                    memset(eo.mat, 0, sizeof(eo.mat));
                    eo.mat[0] = eo.mat[5] = eo.mat[10] = eo.mat[15] = 1.0f;
                    eo.mat[12] = args.createEntityPos[0];
                    eo.mat[13] = args.createEntityPos[1];
                    eo.mat[14] = args.createEntityPos[2];
                    eo.size[0] = eo.size[1] = eo.size[2] = 1.0f;
                    eo.outer = 2.0f;
                    eo.color = 0xFF00FF00; // green marker for new entities
                    eo.shape = "Sphere";
                    eo.name = args.createEntityName;
                    eo.guid = g_levelReader.GetGameObjs().back().guid;
                    eo.parentGuid = args.createEntityParentGuid;
                    eo.gameModeMask = args.createEntityGameModeMask;
                    eo.block1WtOffset = 0;
                    eo.block1TfOffset = 0;
                    if (args.createEntityTypeDefIdx >= 0 &&
                        args.createEntityTypeDefIdx < (int)g_levelReader.GetGameObjTypes().size()) {
                        eo.type = g_levelReader.GetGameObjTypes()[args.createEntityTypeDefIdx].name;
                    }
                    ls->addEditorObj(eo);
                }
            }
        }

        // Auto-collision on entity creation — DISABLED.
        // Collision is handled explicitly via Model Viewer "Build Collision" + "Save to PAK".
        // This auto-trigger was baking world-space positions into verts, causing Havok crashes.
        if (0 && args.createEntityMeshCrc != 0 &&
            !g_levelReader.ModelHasCollision(args.createEntityMeshCrc) &&
            g_renderer && g_renderer->hasLevelScene()) {
            LevelScene* cls = g_renderer->getLevelScene();
            if (cls) {
                std::vector<float> collVerts;
                std::vector<uint16_t> collIndices;
                if (cls->getModelCollisionData(args.createEntityMeshCrc, collVerts, collIndices)) {
                    ZeroEngine::PendingCollisionMesh pcm;
                    // Translation = (0,0,0) — we bake the instance position into vertices
                    // because model vertices may already be in world space (terrain)
                    pcm.translation[0] = 0.0f;
                    pcm.translation[1] = 0.0f;
                    pcm.translation[2] = 0.0f;
                    pcm.translation[3] = 0.0f;
                    pcm.rotation[0] = 0.0f;
                    pcm.rotation[1] = 0.0f;
                    pcm.rotation[2] = 0.0f;
                    pcm.rotation[3] = 1.0f;
                    pcm.key = args.createEntityMeshCrc;

                    // Use LOCAL-SPACE vertices — the game applies WorldTransform itself.
                    // Do NOT bake instance position into collision verts.
                    pcm.verts = collVerts;

                    pcm.indices = collIndices;
                    // Generate MOPP BVTree bytecode
                    {
                        unsigned char* moppBuf = NULL;
                        int moppSz = 0;
                        if (BuildMoppFromMesh(&pcm.verts[0], (int)(pcm.verts.size()/3),
                                              &pcm.indices[0], (int)(pcm.indices.size()/3),
                                              &moppBuf, &moppSz)) {
                            pcm.moppData.assign(moppBuf, moppBuf + moppSz);
                            free(moppBuf);
                        }
                    }
                    g_levelReader.AddPendingCollision(pcm);
                }
            }
        }
    } // close createEntityRequested

    // ================================================================
    // Construction Kit Creation Handler
    // ================================================================
    // Kit creation — read request from file (DLL writes ze_kit_request.txt)
    {
        float kx = 0, ky = 0, kz = 0;
        int gmm = -1;
        std::string prefix = "KIT";
        int cpCount = 4;
        int kitType = -1;
        int autoCreateLayer = 1;
        uint32_t existingLayerGuid = 0;
        char kitPath[MAX_PATH];
        {
            DWORD tplen = GetTempPathA(MAX_PATH, kitPath);
            if (tplen == 0 || tplen >= MAX_PATH) strcpy(kitPath, "C:\\tmp\\");
            strcat(kitPath, "ze_kit_request.txt");
        }
        FILE* kf = fopen(kitPath, "r");
        if (kf) {
            char line[256];
            while (fgets(line, sizeof(line), kf)) {
                if (sscanf(line, "type=%d", &kitType) == 1) continue;
                if (sscanf(line, "pos=%f %f %f", &kx, &ky, &kz) == 3) continue;
                if (sscanf(line, "gmm=%d", &gmm) == 1) continue;
                if (sscanf(line, "cpcount=%d", &cpCount) == 1) continue;
                if (sscanf(line, "autocreatelayer=%d", &autoCreateLayer) == 1) continue;
                if (sscanf(line, "layerguid=%u", &existingLayerGuid) == 1) continue;
                char pfx[64];
                if (sscanf(line, "prefix=%s", pfx) == 1) { prefix = pfx; continue; }
            }
            fclose(kf);
            DeleteFileA(kitPath);
        }
        bool kitRequested = (kitType >= 0);
    if (kitRequested && g_levelReader.IsLoaded())
    {
        uint32_t layGuid = 0;

        // Import any missing type definitions from reference levels
        // so kits can create types that don't exist in the current level
        {
            // Find lotrcparser directory relative to the PAK path
            std::string pakPath = g_levelReader.GetPakPath();
            std::string lotrcDir;
            // Try common relative paths from the PAK location
            size_t devPos = pakPath.find("\\dev\\");
            if (devPos != std::string::npos) {
                lotrcDir = pakPath.substr(0, devPos) + "\\dev\\Vespucci\\Project\\Final\\lotr\\ZeroEnginePrototype\\ZeroEngine\\GameFiles\\lotrcparser";
            }
            // Try relative to EXE
            if (lotrcDir.empty()) {
                char exePath[MAX_PATH];
                GetModuleFileNameA(NULL, exePath, MAX_PATH);
                std::string exeDir(exePath);
                size_t exeSlash = exeDir.rfind('\\');
                if (exeSlash != std::string::npos) exeDir = exeDir.substr(0, exeSlash);
                lotrcDir = exeDir + "\\..\\GameFiles\\lotrcparser";
            }
            g_levelReader.ImportAllMissingTypeDefs(lotrcDir);
        }

        #define FIND_TDEF_KIT(name) FindTypeDefByName(g_levelReader, name)

        // Remember entity count before kit creation
        int entCountBefore = (int)g_levelReader.GetGameObjs().size();

        // Initialize kit context for helper functions
        KitCtx kitCtx;
        kitCtx.reader = &g_levelReader;
        // ── Layer selection: auto-create or use existing ──
        uint32_t kitLayerGuid = 0;
        if (autoCreateLayer) {
            int tlIdx = FIND_TDEF_KIT("templateLayer");
            if (tlIdx >= 0) {
                std::string layerName = "MP_" + prefix;
                ZeroEngine::PendingGameObj tl;
                tl.type_def_index = tlIdx;
                tl.parent_guid = 0;
                tl.gamemodemask = gmm;
                tl.layer_guid = 0;
                tl.name_str = layerName;
                tl.name_crc = ZeroEngine::LotrHashString(layerName.c_str());
                tl.world_transform[0] = tl.world_transform[5] = tl.world_transform[10] = tl.world_transform[15] = 1.0f;
                g_levelReader.AddPendingEntity(tl);
                kitLayerGuid = g_levelReader.GetGameObjs().back().guid;
            }
        } else {
            kitLayerGuid = existingLayerGuid;
        }
        layGuid = kitLayerGuid;

        kitCtx.gmm = gmm;
        kitCtx.layGuid = layGuid;
        kitCtx.prefix = prefix;


        if (kitType == LC_KIT_CONQUEST_FULL) {
            // Root construct
            int ctIdx = FIND_TDEF_KIT("construct");
            if (ctIdx >= 0) KitMakeEntity(kitCtx,ctIdx, prefix + "_Construction_Kit", kx, ky, kz);

            // CP bundles
            int cpIdx = FIND_TDEF_KIT("CapturePoint");
            int trIdx = FIND_TDEF_KIT("trigger_radius");
            int toIdx = FIND_TDEF_KIT("ToggleObjective");
            int spIdx = FIND_TDEF_KIT("spawn_point");
            int dcIdx = FIND_TDEF_KIT("demo_camera");
            int rlIdx = FIND_TDEF_KIT("logic_relay");
            int agIdx = FIND_TDEF_KIT("AIGoal");
            int snIdx = FIND_TDEF_KIT("spawn_node");

            for (int c = 0; c < cpCount; ++c) {
                float cpx = kx + LC_DefaultCPLayouts[c % 4].cpOffset[0];
                float cpy = ky + LC_DefaultCPLayouts[c % 4].cpOffset[1];
                float cpz = kz + LC_DefaultCPLayouts[c % 4].cpOffset[2];
                char ci[8]; _snprintf(ci, sizeof(ci), "CP%d", c+1);

                if (ctIdx >= 0) KitMakeEntity(kitCtx,ctIdx, prefix + "_" + ci, cpx, cpy, cpz);
                if (cpIdx >= 0) KitMakeEntity(kitCtx,cpIdx, prefix + "_CNPT_" + ci, cpx, cpy, cpz);
                if (trIdx >= 0) KitMakeEntity(kitCtx,trIdx, prefix + "_CPTRIG_" + ci, cpx, cpy, cpz);
                if (toIdx >= 0) KitMakeEntity(kitCtx,toIdx, prefix + "_OBJ_" + ci, cpx, cpy, cpz);
                if (spIdx >= 0) {
                    float spx = kx + LC_DefaultCPLayouts[c % 4].spawnOffset[0];
                    float spz = kz + LC_DefaultCPLayouts[c % 4].spawnOffset[2];
                    KitMakeEntity(kitCtx,spIdx, prefix + "_SPWN_" + ci, spx, cpy, spz);
                    // spawn_nodes for CP spawn_point
                    if (snIdx >= 0) {
                        float off[4][2] = {{-2,0},{2,0},{0,-2},{0,2}};
                        for (int n = 0; n < 4; ++n) {
                            char nb[64]; _snprintf(nb, sizeof(nb), "%s_SN_%s_%d", prefix.c_str(), ci, n+1);
                            KitMakeEntity(kitCtx,snIdx, nb, spx + off[n][0], cpy, spz + off[n][1]);
                        }
                    }
                }
                if (dcIdx >= 0) {
                    float cx = kx + LC_DefaultCPLayouts[c % 4].camOffset[0];
                    float cz = kz + LC_DefaultCPLayouts[c % 4].camOffset[2];
                    KitMakeEntity(kitCtx,dcIdx, prefix + "_CAM_" + ci, cx, cpy + 5.0f, cz);
                }
                if (rlIdx >= 0) {
                    KitMakeEntity(kitCtx,rlIdx, prefix + "_RLY_" + ci + "_CAPTURED_TEAM1", cpx, cpy, cpz);
                    KitMakeEntity(kitCtx,rlIdx, prefix + "_RLY_" + ci + "_CAPTURED_TEAM2", cpx, cpy, cpz);
                }
                if (agIdx >= 0) {
                    KitMakeEntity(kitCtx,agIdx, prefix + "_AIGL_" + ci + "_ATTACK_TEAM1", cpx, cpy, cpz);
                    KitMakeEntity(kitCtx,agIdx, prefix + "_AIGL_" + ci + "_ATTACK_TEAM2", cpx, cpy, cpz);
                    KitMakeEntity(kitCtx,agIdx, prefix + "_AIGL_" + ci + "_DEFEND_TEAM1", cpx, cpy, cpz);
                    KitMakeEntity(kitCtx,agIdx, prefix + "_AIGL_" + ci + "_DEFEND_TEAM2", cpx, cpy, cpz);
                }
            }

            // GAMESCRIPT construct
            if (ctIdx >= 0) KitMakeEntity(kitCtx,ctIdx, prefix + "_GAMESCRIPT", kx, ky, kz);
            KitCreateScoring(kitCtx,kx, ky, kz);

            // Spawn system
            int prIdx = FIND_TDEF_KIT("PlayerRespawner");
            if (prIdx >= 0) KitMakeEntity(kitCtx,prIdx, prefix + "_PlayerRespawner", kx, ky, kz);
            KitCreateSpawnChain(kitCtx,1, kx + LC_SpawnTeamOffsets[0][0], ky, kz + LC_SpawnTeamOffsets[0][2]);
            KitCreateSpawnChain(kitCtx,2, kx + LC_SpawnTeamOffsets[1][0], ky, kz + LC_SpawnTeamOffsets[1][2]);

            // Auto-create gamemode entity with Conquest defaults
            KitCreateGamemode(kitCtx,LC_MODE_CONQUEST);
        }
        else if (kitType == LC_KIT_CONQUEST_CP) {
            int ctIdx = FIND_TDEF_KIT("construct");
            int cpIdx = FIND_TDEF_KIT("CapturePoint");
            int trIdx = FIND_TDEF_KIT("trigger_radius");
            int toIdx = FIND_TDEF_KIT("ToggleObjective");
            int spIdx = FIND_TDEF_KIT("spawn_point");
            int dcIdx = FIND_TDEF_KIT("demo_camera");
            int rlIdx = FIND_TDEF_KIT("logic_relay");
            int agIdx = FIND_TDEF_KIT("AIGoal");
            int snIdx = FIND_TDEF_KIT("spawn_node");

            if (ctIdx >= 0) KitMakeEntity(kitCtx,ctIdx, prefix + "_CP", kx, ky, kz);
            if (cpIdx >= 0) KitMakeEntity(kitCtx,cpIdx, prefix + "_CNPT", kx, ky, kz);
            if (trIdx >= 0) KitMakeEntity(kitCtx,trIdx, prefix + "_CPTRIG", kx, ky, kz);
            if (toIdx >= 0) KitMakeEntity(kitCtx,toIdx, prefix + "_OBJ", kx, ky, kz);
            if (spIdx >= 0) KitMakeEntity(kitCtx,spIdx, prefix + "_SPWN", kx - 5.0f, ky, kz - 5.0f);
            if (snIdx >= 0) {
                float off[4][2] = {{-2,0},{2,0},{0,-2},{0,2}};
                for (int n = 0; n < 4; ++n) {
                    char nb[64]; _snprintf(nb, sizeof(nb), "%s_SN_%d", prefix.c_str(), n+1);
                    KitMakeEntity(kitCtx,snIdx, nb, kx - 5.0f + off[n][0], ky, kz - 5.0f + off[n][1]);
                }
            }
            if (dcIdx >= 0) KitMakeEntity(kitCtx,dcIdx, prefix + "_CAM", kx, ky + 5.0f, kz + 10.0f);
            if (rlIdx >= 0) {
                KitMakeEntity(kitCtx,rlIdx, prefix + "_RLY_CAPTURED_TEAM1", kx, ky, kz);
                KitMakeEntity(kitCtx,rlIdx, prefix + "_RLY_CAPTURED_TEAM2", kx, ky, kz);
            }
            if (agIdx >= 0) {
                KitMakeEntity(kitCtx,agIdx, prefix + "_AIGL_ATTACK_TEAM1", kx, ky, kz);
                KitMakeEntity(kitCtx,agIdx, prefix + "_AIGL_ATTACK_TEAM2", kx, ky, kz);
                KitMakeEntity(kitCtx,agIdx, prefix + "_AIGL_DEFEND_TEAM1", kx, ky, kz);
                KitMakeEntity(kitCtx,agIdx, prefix + "_AIGL_DEFEND_TEAM2", kx, ky, kz);
            }
        }
        else if (kitType == LC_KIT_TDM) {
            int ctIdx = FIND_TDEF_KIT("construct");
            int goIdx = FIND_TDEF_KIT("GroupObject");
            int prIdx = FIND_TDEF_KIT("PlayerRespawner");

            if (ctIdx >= 0) KitMakeEntity(kitCtx,ctIdx, prefix + "_Construction_Kit", kx, ky, kz);
            if (goIdx >= 0) {
                KitMakeEntity(kitCtx,goIdx, prefix + "_GRP_Team1", kx, ky, kz);
                KitMakeEntity(kitCtx,goIdx, prefix + "_GRP_Team2", kx, ky, kz);
            }
            KitCreateScoring(kitCtx,kx, ky, kz);
            if (prIdx >= 0) KitMakeEntity(kitCtx,prIdx, prefix + "_PlayerRespawner", kx, ky, kz);
            KitCreateSpawnChain(kitCtx,1, kx + LC_SpawnTeamOffsets[0][0], ky, kz + LC_SpawnTeamOffsets[0][2]);
            KitCreateSpawnChain(kitCtx,2, kx + LC_SpawnTeamOffsets[1][0], ky, kz + LC_SpawnTeamOffsets[1][2]);

            // Auto-create gamemode entity with TDM defaults
            KitCreateGamemode(kitCtx,LC_MODE_TDM);
        }
        else if (kitType == LC_KIT_CTR) {
            int ctIdx  = FIND_TDEF_KIT("construct");
            int aqIdx  = FIND_TDEF_KIT("AcquireObject");
            int trIdx  = FIND_TDEF_KIT("trigger_radius");
            int prIdx  = FIND_TDEF_KIT("PlayerRespawner");

            if (ctIdx >= 0) KitMakeEntity(kitCtx,ctIdx, prefix + "_Construction_Kit", kx, ky, kz);
            if (aqIdx >= 0) KitMakeEntity(kitCtx,aqIdx, prefix + "_Ring", kx, ky, kz);
            if (trIdx >= 0) {
                KitMakeEntity(kitCtx,trIdx, prefix + "_CaptureZone_Team1", kx, ky, kz - 80.0f);
                KitMakeEntity(kitCtx,trIdx, prefix + "_CaptureZone_Team2", kx, ky, kz + 80.0f);
            }
            KitCreateScoring(kitCtx,kx, ky, kz);
            if (prIdx >= 0) KitMakeEntity(kitCtx,prIdx, prefix + "_PlayerRespawner", kx, ky, kz);
            KitCreateSpawnChain(kitCtx,1, kx + LC_SpawnTeamOffsets[0][0], ky, kz + LC_SpawnTeamOffsets[0][2]);
            KitCreateSpawnChain(kitCtx,2, kx + LC_SpawnTeamOffsets[1][0], ky, kz + LC_SpawnTeamOffsets[1][2]);

            // Auto-create gamemode entity with CTR defaults
            KitCreateGamemode(kitCtx,LC_MODE_CTR);
        }
        else if (kitType == LC_KIT_SPAWN_SYSTEM) {
            int prIdx = FIND_TDEF_KIT("PlayerRespawner");
            if (prIdx >= 0) KitMakeEntity(kitCtx,prIdx, prefix + "_PlayerRespawner", kx, ky, kz);
            KitCreateSpawnChain(kitCtx,1, kx + LC_SpawnTeamOffsets[0][0], ky, kz + LC_SpawnTeamOffsets[0][2]);
            KitCreateSpawnChain(kitCtx,2, kx + LC_SpawnTeamOffsets[1][0], ky, kz + LC_SpawnTeamOffsets[1][2]);
        }
        else if (kitType == LC_KIT_GAMESTART) {
            int gsIdx = FIND_TDEF_KIT("logic_gamestart");
            int rlIdx = FIND_TDEF_KIT("logic_relay");
            int outIdx = FIND_TDEF_KIT("Output");
            int tmIdx = FIND_TDEF_KIT("logic_timer");

            // ── Mode-specific gamestart ──
            if (gsIdx >= 0) {
                KitMakeEntity(kitCtx,gsIdx, prefix + "_GameStart", kx, ky, kz);
                uint32_t gsGuid = g_levelReader.GetGameObjs().back().guid;

                // Create the AI relay hub
                if (rlIdx >= 0) {
                    KitMakeEntity(kitCtx,rlIdx, prefix + "_RLY_ActivateAI", kx, ky, kz);
                    uint32_t relayGuid = g_levelReader.GetGameObjs().back().guid;

                    // Wire: GameStart.OnTrigger -> Relay.Trigger (Sticky)
                    if (outIdx >= 0) {
                        ZeroEngine::PendingGameObj out;
                        out.type_def_index = outIdx;
                        out.gamemodemask = gmm;
                        out.layer_guid = layGuid;
                        out.name_str = prefix + "_OUT_GS_ActivateAI";
                        out.name_crc = ZeroEngine::LotrHashString(out.name_str.c_str());
                        memset(out.world_transform, 0, sizeof(out.world_transform));
                        out.world_transform[0] = out.world_transform[5] = out.world_transform[10] = out.world_transform[15] = 1.0f;
                        // Set Output-specific fields: target, event, input
                        out.output_event = "OnTrigger";
                        out.input_action = "Trigger";
                        out.target_guid = relayGuid;
                        out.sticky = 1;
                        out.owner_guid = gsGuid;  // links into GameStart's Outputs[]
                        g_levelReader.AddPendingEntity(out);
                    }

                    // Wire: GameStart.OnTrigger -> each spawn_emitter.Activate
                    // Search existing entities for emitters matching our prefix
                    if (outIdx >= 0) {
                        const std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjs();
                        for (int gi = 0; gi < (int)gos.size(); ++gi) {
                            if (gos[gi].type_name != "spawn_emitter") continue;
                            if (gos[gi].name.find(prefix) == std::string::npos) continue;
                            if (gmm != -1 && gos[gi].gamemodemask != -1 && !(gos[gi].gamemodemask & gmm)) continue;

                            ZeroEngine::PendingGameObj out;
                                out.type_def_index = outIdx;
                            out.gamemodemask = gmm;
                            out.layer_guid = layGuid;
                            out.name_str = "OnTrigger";
                            out.name_crc = ZeroEngine::LotrHashString(out.name_str.c_str());
                            memset(out.world_transform, 0, sizeof(out.world_transform));
                            out.world_transform[0] = out.world_transform[5] = out.world_transform[10] = out.world_transform[15] = 1.0f;
                            out.output_event = "OnTrigger";
                            out.input_action = "Activate";
                            out.target_guid = gos[gi].guid;
                            out.sticky = 0;
                            out.owner_guid = gsGuid;
                            g_levelReader.AddPendingEntity(out);
                        }
                    }

                    // Wire relay -> each AIGoal.Activate matching our prefix
                    if (outIdx >= 0) {
                        const std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjs();
                        for (int gi = 0; gi < (int)gos.size(); ++gi) {
                            if (gos[gi].type_name != "AIGoal" &&
                                gos[gi].type_name != "aigoal_player" &&
                                gos[gi].type_name != "aigoal_multi_points") continue;
                            if (gos[gi].name.find(prefix) == std::string::npos) continue;
                            if (gmm != -1 && gos[gi].gamemodemask != -1 && !(gos[gi].gamemodemask & gmm)) continue;

                            ZeroEngine::PendingGameObj out;
                                out.type_def_index = outIdx;
                            out.gamemodemask = gmm;
                            out.layer_guid = layGuid;
                            out.name_str = "OnTrigger";
                            out.name_crc = ZeroEngine::LotrHashString(out.name_str.c_str());
                            memset(out.world_transform, 0, sizeof(out.world_transform));
                            out.world_transform[0] = out.world_transform[5] = out.world_transform[10] = out.world_transform[15] = 1.0f;
                            out.output_event = "OnTrigger";
                            out.input_action = "Activate";
                            out.target_guid = gos[gi].guid;
                            out.sticky = 1;
                            out.owner_guid = relayGuid;
                            g_levelReader.AddPendingEntity(out);
                        }
                    }
                }
            }

            // ── Global FX gamestart (GMM=-1) ──
            if (gsIdx >= 0) {
                ZeroEngine::PendingGameObj fxgs;
                fxgs.type_def_index = gsIdx;
                fxgs.gamemodemask = -1;  // all modes
                fxgs.layer_guid = layGuid;
                fxgs.name_str = "GameStart_FX";
                fxgs.name_crc = ZeroEngine::LotrHashString(fxgs.name_str.c_str());
                memset(fxgs.world_transform, 0, sizeof(fxgs.world_transform));
                fxgs.world_transform[0] = fxgs.world_transform[5] = fxgs.world_transform[10] = fxgs.world_transform[15] = 1.0f;
                g_levelReader.AddPendingEntity(fxgs);
                uint32_t fxgsGuid = g_levelReader.GetGameObjs().back().guid;

                // Create FX timer
                if (tmIdx >= 0) {
                    KitMakeEntity(kitCtx,tmIdx, prefix + "_Timer_FX", kx, ky, kz);
                    uint32_t timerGuid = g_levelReader.GetGameObjs().back().guid;

                    // Wire: GameStart_FX.OnTrigger -> Timer_FX.Start (Sticky)
                    if (outIdx >= 0) {
                        ZeroEngine::PendingGameObj out;
                        out.type_def_index = outIdx;
                        out.gamemodemask = -1;
                        out.layer_guid = layGuid;
                        out.name_str = "OnTrigger";
                        out.name_crc = ZeroEngine::LotrHashString(out.name_str.c_str());
                        memset(out.world_transform, 0, sizeof(out.world_transform));
                        out.world_transform[0] = out.world_transform[5] = out.world_transform[10] = out.world_transform[15] = 1.0f;
                        out.output_event = "OnTrigger";
                        out.input_action = "Start";
                        out.target_guid = timerGuid;
                        out.sticky = 1;
                        out.owner_guid = fxgsGuid;
                        g_levelReader.AddPendingEntity(out);
                    }
                }
            }
        }

        // Log what was created
        {
            int created = (int)g_levelReader.GetGameObjs().size() - entCountBefore;
            char logBuf[512];
            sprintf_s(logBuf, "KIT: created %d entities at pos (%.1f, %.1f, %.1f) prefix=%s",
                      created, kx, ky, kz, prefix.c_str());
            DebugLog(logBuf);
            OutputDebugStringA(logBuf);
            OutputDebugStringA("\n");
            // Also set status message so user can see it
            g_savePakMessage = logBuf;
            g_savePakStatus = 5;
            g_savePakDoneTimer = 5.0f;
        }

        // Add green editor spheres for all newly created kit entities
        if (g_renderer && g_renderer->hasLevelScene()) {
            LevelScene* ls = g_renderer->getLevelScene();
            if (ls) {
                const std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjs();
                // Add editor objects for ALL entities created by this kit
                for (int gi = entCountBefore; gi < (int)gos.size(); ++gi) {
                    LevelEditorObj eo;
                    memset(eo.mat, 0, sizeof(eo.mat));
                    eo.mat[0] = eo.mat[5] = eo.mat[10] = eo.mat[15] = 1.0f;
                    memcpy(eo.mat, gos[gi].world_transform, 64);
                    eo.size[0] = eo.size[1] = eo.size[2] = 1.0f;
                    eo.outer = 2.0f;
                    eo.color = 0xFF00FF00;
                    eo.shape = "Sphere";
                    eo.name = gos[gi].name;
                    eo.type = gos[gi].type_name;
                    eo.guid = gos[gi].guid;
                    eo.parentGuid = gos[gi].parent_guid;
                    eo.gameModeMask = gos[gi].gamemodemask;
                    eo.block1WtOffset = 0;
                    eo.block1TfOffset = 0;
                    ls->addEditorObj(eo);
                }
            }
        }

        #undef FIND_TDEF_KIT
    }
    } // close file-reading scope

    // ================================================================
    // Gamemode Wipe Handler — removes entities matching a gamemode bit
    // Keeps: gamemode entities, character_class, creature types
    // ================================================================
    {
        char wipePath[MAX_PATH];
        {
            DWORD tplen = GetTempPathA(MAX_PATH, wipePath);
            if (tplen == 0 || tplen >= MAX_PATH) strcpy(wipePath, "C:\\tmp\\");
            strcat(wipePath, "ze_wipe_request.txt");
        }
        FILE* wf = fopen(wipePath, "r");
        if (wf && g_levelReader.IsLoaded()) {
            uint32_t wipeLayerGuid = 0;
            char line[256];
            while (fgets(line, sizeof(line), wf)) {
                sscanf(line, "layer_guid=%u", &wipeLayerGuid);
            }
            fclose(wf);
            DeleteFileA(wipePath);

            if (wipeLayerGuid != 0) {
                // Types to keep (not wiped even if on the target layer)
                // Three categories of protection:
                //  1) Template/class hierarchy: engine uses these for type resolution
                //  2) Match infrastructure: engine NULL-derefs without these
                //     (confirmed: PointManager crash at 0x008C284E, drone init)
                //  3) Match flow: without these the match instant-wins or never starts
                static const char* keepTypes[] = {
                    // Template / class hierarchy
                    "gamemode", "templateLayer", "templateLevel", "templateGroup",
                    "character_class", "creature", "hero",
                    "rig", "rig_alt", "rig_mounted",
                    // Match infrastructure — engine crashes without these
                    "PointManager",         // crash: MgObjectivePointManager NULL deref in drone init
                    "PlayerRespawner",      // crash: spawn system init expects this
                    "spawn_emitter",        // crash: spawn system walks emitter list
                    "spawn_point",          // crash: emitters reference spawn points
                    // Match flow — instant-win / dead match without these
                    "logic_gamestart",      // match never starts (AI idle, no spawns)
                    "logic_endgame",        // match never ends properly
                    "GlobalGameEffects",    // engine effect system init
                    "MetaReward",           // progression/reward system
                    "GroupObject",          // team kill-tracking groups
                    "ResourceEmitter",      // Ent/Troll resource spawning
                    NULL
                };

                std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjsMut();
                int removed = 0;
                int kept = 0;

                // Find the layer name for logging
                std::string layerName = "?";
                for (int gi = 0; gi < (int)gos.size(); ++gi) {
                    if (gos[gi].guid == wipeLayerGuid) { layerName = gos[gi].name; break; }
                }

                // Remove entities that sit ON this layer
                for (int gi = (int)gos.size() - 1; gi >= 0; --gi) {
                    if (gos[gi].layer_guid != wipeLayerGuid) continue;

                    // Check if this type should be kept
                    bool keep = false;
                    for (int ki = 0; keepTypes[ki]; ++ki) {
                        if (gos[gi].type_name == keepTypes[ki]) {
                            keep = true;
                            break;
                        }
                    }
                    if (keep) { ++kept; continue; }

                    // Track for save exclusion
                    g_levelReader.AddDeletedGuid(gos[gi].guid);

                    // Remove from 3D scene
                    if (g_renderer && g_renderer->hasLevelScene()) {
                        LevelScene* ls = g_renderer->getLevelScene();
                        if (ls) ls->removeByGuid(gos[gi].guid);
                    }

                    gos.erase(gos.begin() + gi);
                    ++removed;
                }

                // Force full rebuild of UI pointer arrays
                g_goNameStrings.clear(); g_goNamePtrs.clear();
                g_goTypeNameStrings.clear(); g_goTypeNamePtrs.clear();
                g_goMeshNameStrings.clear(); g_goMeshNamePtrs.clear();
                g_goGuids.clear(); g_goParentGuids.clear(); g_goLayerGuids.clear();
                g_goGamemodeMasks.clear(); g_goTeams.clear();
                g_goPosX.clear(); g_goPosY.clear(); g_goPosZ.clear();
                g_goTargetGuids.clear();
                g_goOutputEventStrings.clear(); g_goOutputEventPtrs.clear();
                g_goInputEventStrings.clear(); g_goInputEventPtrs.clear();
                g_goModeNameStrings.clear(); g_goModeNamePtrs.clear();
                g_goEditorShapeStrings.clear(); g_goEditorShapePtrs.clear();
                g_goEditorTextureStrings.clear(); g_goEditorTexturePtrs.clear();
                g_goEditorColors.clear(); g_goEditorOuters.clear();
                g_goEditorSizeX.clear(); g_goEditorSizeY.clear(); g_goEditorSizeZ.clear();
                g_goDelays.clear(); g_goSticky.clear();
                g_goOutputsOffsets.clear(); g_goOutputsCounts.clear(); g_goOutputsData.clear();
                g_goLayersArrOffsets.clear(); g_goLayersArrCounts.clear(); g_goLayersArrData.clear();
                g_goNodesOffsets.clear(); g_goNodesCounts.clear(); g_goNodesData.clear();
                g_goNode1Guids.clear(); g_goNode2Guids.clear();
                g_goAllGuidRefOffsets.clear(); g_goAllGuidRefCounts.clear(); g_goAllGuidRefData.clear();

                char logBuf[512];
                sprintf_s(logBuf, "WIPE: removed %d entities from layer '%s' (guid=%u, kept %d protected)",
                          removed, layerName.c_str(), wipeLayerGuid, kept);
                DebugLog(logBuf);
                g_savePakMessage = logBuf;
                g_savePakStatus = 5;
                g_savePakDoneTimer = 5.0f;
            }
        } else if (wf) {
            fclose(wf);
        }
    }

    // ================================================================
    // Gamemode Entity Creation Handler
    // Creates a gamemode entity with all 49 fields set to correct defaults.
    // Uses donor cloning + generic field overrides for safety.
    // ================================================================
    if (args.createGamemodeRequested && g_levelReader.IsLoaded())
    {
        int nTypes = (int)g_levelReader.GetGameObjTypes().size();
        int gmTypeIdx = -1;
        for (int t = 0; t < nTypes; ++t) {
            if (g_levelReader.GetGameObjTypes()[t].name == "gamemode") {
                gmTypeIdx = t; break;
            }
        }

        if (gmTypeIdx >= 0 && args.createGamemodeType >= 0 && args.createGamemodeType < LC_GamemodeTableCount) {
            const LC_GamemodeDefaults& gmd = LC_GamemodeTable[args.createGamemodeType];

            ZeroEngine::PendingGameObj pgo;
            pgo.type_def_index = gmTypeIdx;
            pgo.parent_guid = 0;         // gamemodes never have parents
            pgo.gamemodemask = -1;        // gamemodes always have GMM=-1
            pgo.layer_guid = 0;          // gamemodes sit at root level (layer=0)
            pgo.name_str = args.createGmName;
            pgo.name_crc = ZeroEngine::LotrHashString(args.createGmName);
            pgo.mesh_crc = 0;
            // Identity matrix at origin — gamemodes are logical, not spatial
            memset(pgo.world_transform, 0, sizeof(pgo.world_transform));
            pgo.world_transform[0] = pgo.world_transform[5] = pgo.world_transform[10] = pgo.world_transform[15] = 1.0f;

            // ── Set all mode-specific fields via generic overrides ──
            // This writes into the cloned binary data at the correct offsets

            // Mode string (CRC of mode name)
            pgo.stringOverrides[LC_GM_MODE] = gmd.modeString;
            pgo.stringOverrides[LC_GM_MODENAME] = gmd.modeName;

            // GUID references
            pgo.intOverrides[LC_GM_ATMOSPHERE] = args.createGmAtmosphereGuid;
            pgo.intOverrides[LC_GM_ATMOSPHERE_LOW] = 0;
            pgo.intOverrides[LC_GM_MAPNW] = args.createGmMapNWGuid;
            pgo.intOverrides[LC_GM_MAPSE] = args.createGmMapSEGuid;
            pgo.intOverrides[LC_GM_INTROCAMERA] = 0;
            pgo.intOverrides[LC_GM_MUSIC] = 0;

            // Audio strings
            if (gmd.matchStartMsg[0])
                pgo.stringOverrides[LC_GM_MATCHSTARTMSG] = gmd.matchStartMsg;
            if (gmd.matchStartVO[0])
                pgo.stringOverrides[LC_GM_MATCHSTARTVO] = gmd.matchStartVO;
            // Campaign intro/outro — empty for multiplayer
            pgo.stringOverrides[LC_GM_INTRO] = "";
            pgo.stringOverrides[LC_GM_OUTTRO] = "";
            // CP complete VOs
            if (gmd.t1CPCompleteVO[0])
                pgo.stringOverrides[LC_GM_TEAM1CPCOMPLETEVO] = gmd.t1CPCompleteVO;
            if (gmd.t2CPCompleteVO[0])
                pgo.stringOverrides[LC_GM_TEAM2CPCOMPLETEVO] = gmd.t2CPCompleteVO;
            // Ringbearer effect
            if (gmd.rbFrodoBirthEffect[0])
                pgo.stringOverrides[LC_GM_RB_FRODOBIRTHEFFECT] = gmd.rbFrodoBirthEffect;

            // Multiplayer settings
            pgo.intOverrides[LC_GM_MP_MINPLAYERCOUNT] = (uint32_t)gmd.mpMinPlayerCount;
            pgo.floatOverrides[LC_GM_MP_WARMUPTIME] = gmd.mpWarmupTime;
            pgo.floatOverrides[LC_GM_MP_SPAWNDELAY] = gmd.mpSpawnDelay;
            pgo.floatOverrides[LC_GM_MP_SPAWNWINDOW] = gmd.mpSpawnWindow;

            // Conquest settings
            pgo.floatOverrides[LC_GM_CQ_UPDATEDELAY] = gmd.cqUpdateDelay;
            pgo.intOverrides[LC_GM_CQ_T1_TRICKLERATE] = (uint32_t)gmd.cqT1TrickleRate;
            pgo.intOverrides[LC_GM_CQ_T2_TRICKLERATE] = (uint32_t)gmd.cqT2TrickleRate;
            pgo.intOverrides[LC_GM_CQ_T1_PERPOINTRATE] = (uint32_t)gmd.cqT1PerPointRate;
            pgo.intOverrides[LC_GM_CQ_T2_PERPOINTRATE] = (uint32_t)gmd.cqT2PerPointRate;

            // Ringbearer settings
            pgo.intOverrides[LC_GM_RB_VICTORYPOINTS] = (uint32_t)gmd.rbVictoryPoints;
            pgo.intOverrides[LC_GM_RB_POINTSPERSECOND] = (uint32_t)gmd.rbPointsPerSecond;
            pgo.intOverrides[LC_GM_RB_POINTSPERKILL] = (uint32_t)gmd.rbPointsPerKill;

            // AI settings
            pgo.intOverrides[LC_GM_USESAI] = (uint32_t)gmd.usesAI;
            pgo.intOverrides[LC_GM_MAXAISPAWNTEAM1] = (uint32_t)gmd.maxAITeam1;
            pgo.intOverrides[LC_GM_MAXAISPAWNTEAM2] = (uint32_t)gmd.maxAITeam2;
            pgo.floatOverrides[LC_GM_AITEAM1DIFFICULTY] = gmd.aiDifficulty;
            pgo.floatOverrides[LC_GM_AITEAM2DIFFICULTY] = gmd.aiDifficulty;
            pgo.floatOverrides[LC_GM_AITEAM1DIFFINC] = 0.0f;
            pgo.floatOverrides[LC_GM_AITEAM2DIFFINC] = 0.0f;

            // Spawn/display settings
            pgo.intOverrides[LC_GM_SHOWPOINTS] = (uint32_t)gmd.showPoints;
            pgo.intOverrides[LC_GM_AUTOSELECTSPAWNPOINT] = (uint32_t)gmd.autoSelectSpawnPoint;
            pgo.intOverrides[LC_GM_AUTOSELECTCLASS] = (uint32_t)gmd.autoSelectClass;

            // Editor visualization (constant across all modes)
            pgo.stringOverrides[0x0EA9DC4Bu] = "Sphere_Wire"; // "Type" editor viz

            g_levelReader.AddPendingEntity(pgo);

            // Add editor object for immediate display
            if (g_renderer && g_renderer->hasLevelScene()) {
                LevelScene* ls = g_renderer->getLevelScene();
                if (ls) {
                    LevelEditorObj eo;
                    memset(eo.mat, 0, sizeof(eo.mat));
                    eo.mat[0] = eo.mat[5] = eo.mat[10] = eo.mat[15] = 1.0f;
                    eo.size[0] = eo.size[1] = eo.size[2] = 2.0f;
                    eo.outer = 3.0f;
                    eo.color = 0xFFAAFFAA; // green-tinted (matches real gamemodes)
                    eo.shape = "Sphere";
                    eo.name = args.createGmName;
                    eo.type = "gamemode";
                    eo.guid = g_levelReader.GetGameObjs().back().guid;
                    eo.parentGuid = 0;
                    eo.gameModeMask = -1;
                    eo.block1WtOffset = 0;
                    eo.block1TfOffset = 0;
                    ls->addEditorObj(eo);
                }
            }
        }
    }

        // Refresh gameObj pointer arrays so ALL new entities appear in UI this frame
        {
            const std::vector<ZeroEngine::LevelGameObjEntry>& gos = g_levelReader.GetGameObjs();
            size_t n = gos.size();
            size_t oldN = g_goNameStrings.size(); // how many we had before
            g_goNameStrings.resize(n);
            g_goTypeNameStrings.resize(n);
            g_goMeshNameStrings.resize(n);
            g_goGuids.resize(n);
            g_goParentGuids.resize(n);
            g_goLayerGuids.resize(n);
            g_goGamemodeMasks.resize(n);
            g_goTeams.resize(n);
            g_goPosX.resize(n);
            g_goPosY.resize(n);
            g_goPosZ.resize(n);
            // Fill ALL new entries (not just the last one — kits create many at once)
            static uint32_t CRC_TEAM_REFRESH = ZeroEngine::LotrHashString("Team");
            for (size_t i = oldN; i < n; ++i) {
                g_goNameStrings[i]     = gos[i].name;
                g_goTypeNameStrings[i] = gos[i].type_name;
                g_goMeshNameStrings[i] = gos[i].mesh_name;
                g_goGuids[i]           = gos[i].guid;
                g_goParentGuids[i]     = gos[i].parent_guid;
                g_goLayerGuids[i]      = gos[i].layer_guid;
                g_goGamemodeMasks[i]   = gos[i].gamemodemask;
                g_goPosX[i]            = gos[i].pos[0];
                g_goPosY[i]            = gos[i].pos[1];
                g_goPosZ[i]            = gos[i].pos[2];
                std::map<uint32_t, uint32_t>::const_iterator tIt2 = gos[i].int_fields.find(CRC_TEAM_REFRESH);
                g_goTeams[i] = (tIt2 != gos[i].int_fields.end()) ? (int)tIt2->second : -1;
                // Extend event/relationship arrays
                g_goTargetGuids.push_back(gos[i].target_guid);
                g_goOutputEventStrings.push_back(gos[i].output_event);
                g_goInputEventStrings.push_back(gos[i].input_event);
                g_goModeNameStrings.push_back("");
                g_goDelays.push_back(gos[i].delay);
                g_goSticky.push_back(gos[i].sticky ? 1 : 0);
                g_goOutputsOffsets.push_back((int)g_goOutputsData.size());
                g_goOutputsCounts.push_back(0);
                g_goLayersArrOffsets.push_back((int)g_goLayersArrData.size());
                g_goLayersArrCounts.push_back(0);
                g_goNodesOffsets.push_back((int)g_goNodesData.size());
                g_goNodesCounts.push_back(0);
                g_goNode1Guids.push_back(0);
                g_goNode2Guids.push_back(0);
                g_goAllGuidRefOffsets.push_back((int)(g_goAllGuidRefData.size() / 2));
                g_goAllGuidRefCounts.push_back(0);
                // Editor visualization
                g_goEditorShapeStrings.push_back(gos[i].editor_shape);
                g_goEditorTextureStrings.push_back(gos[i].editor_texture);
                g_goEditorColors.push_back(gos[i].editor_color);
                g_goEditorOuters.push_back(gos[i].editor_outer);
                g_goEditorSizeX.push_back(gos[i].editor_size[0]);
                g_goEditorSizeY.push_back(gos[i].editor_size[1]);
                g_goEditorSizeZ.push_back(gos[i].editor_size[2]);
            }
            // Rebuild all pointer arrays (c_str() pointers may have been invalidated by resize)
            g_goNamePtrs.resize(n);
            g_goTypeNamePtrs.resize(n);
            g_goMeshNamePtrs.resize(n);
            for (size_t j = 0; j < n; ++j) {
                g_goNamePtrs[j]     = g_goNameStrings[j].c_str();
                g_goTypeNamePtrs[j] = g_goTypeNameStrings[j].c_str();
                g_goMeshNamePtrs[j] = g_goMeshNameStrings[j].c_str();
            }
            // Rebuild event/editor pointer arrays
            g_goOutputEventPtrs.resize(n);
            g_goInputEventPtrs.resize(n);
            g_goModeNamePtrs.resize(n);
            g_goEditorShapePtrs.resize(n);
            g_goEditorTexturePtrs.resize(n);
            for (size_t j = 0; j < n; ++j) {
                g_goOutputEventPtrs[j] = g_goOutputEventStrings[j].c_str();
                g_goInputEventPtrs[j]  = g_goInputEventStrings[j].c_str();
                g_goModeNamePtrs[j]    = g_goModeNameStrings[j].c_str();
                g_goEditorShapePtrs[j] = g_goEditorShapeStrings[j].c_str();
                g_goEditorTexturePtrs[j]= g_goEditorTextureStrings[j].c_str();
            }
        }

    // ---- Mocap Studio request handling ----
    // Update mocap bridge (read pipe data from Python subprocess)
    {
        MocapState prevState = g_mocapBridge.GetState();
        g_mocapBridge.Update();
        MocapState curState = g_mocapBridge.GetState();
        // Re-enable 3D viewport when WHAM finishes or errors
        if (prevState != curState && (curState == MOCAP_DONE || curState == MOCAP_ERROR))
        {
            if (g_renderer) g_renderer->setScene3dEnabled(true);
        }
    }

    // Initialize retargeter — re-check skeleton each frame until we have one
    {
        static bool s_mocapRetargeterInit = false;
        if (!s_mocapRetargeterInit) {
            g_mocapRetargeter.Initialize();
            s_mocapRetargeterInit = true;
        }
        if (!g_mocapRetargeter.HasReferencePose() && g_renderer && g_renderer->getModelSkeleton()) {
            g_mocapRetargeter.SetReferencePose(g_renderer->getModelSkeleton());
        }
    }

    // Load Video button
    if (args.mocapRequestLoadVideo)
    {
        FILE* dbg = fopen("mocap_debug.log", "a");
        if (dbg) { fprintf(dbg, "[MOCAP] Load Video button pressed, g_hwnd=%p\n", (void*)g_hwnd); fclose(dbg); }

        OPENFILENAMEA ofn;
        char szFile[512] = "";
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = "Video Files\0*.mp4;*.mov;*.avi;*.mkv;*.wmv\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrTitle = "Select Video for Motion Capture";
        ofn.lpstrInitialDir = NULL;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        BOOL ok = GetOpenFileNameA(&ofn);
        if (dbg) { dbg = fopen("mocap_debug.log", "a"); }
        if (dbg) { fprintf(dbg, "[MOCAP] GetOpenFileName returned %d, CommDlgExtendedError=%lu, file='%s'\n", (int)ok, CommDlgExtendedError(), szFile); fclose(dbg); }
        if (ok)
        {
            g_mocapVideoPath = szFile;
            g_mocapPlaying = false;
            g_mocapPlayTime = 0.0f;
            g_mocapFrames.clear();

            // Build paths to WHAM python. Env-var overrides win; otherwise derive
            // from EXE location (Scene3D's parent is the ZeroEngine root, and WHAM
            // lives at <root>\Engine\source\Vendors\WHAM). Final fallback for the
            // interpreter is "python" so PATH lookup picks whatever's installed.
            std::string whamDir;
            std::string pythonExe;
            {
                char envBuf[1024] = {0};
                GetEnvironmentVariableA("VESPUCCI_WHAM_DIR", envBuf, sizeof(envBuf));
                if (envBuf[0]) {
                    whamDir = envBuf;
                } else {
                    char exeDir[MAX_PATH] = {0};
                    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
                    char* slash = strrchr(exeDir, '\\'); if (slash) *slash = 0;
                    whamDir = std::string(exeDir) + "\\..\\Engine\\source\\Vendors\\WHAM";
                }
                char pyBuf[1024] = {0};
                GetEnvironmentVariableA("VESPUCCI_WHAM_PYTHON", pyBuf, sizeof(pyBuf));
                pythonExe = pyBuf[0] ? pyBuf : "python";
            }

            // Disable 3D viewport to free VRAM for WHAM
            if (g_renderer) g_renderer->setScene3dEnabled(false);

            g_mocapBridge.StartProcessing(pythonExe.c_str(), whamDir.c_str(), szFile, 0);
        }
    }

    // Cancel button
    if (args.mocapRequestCancel)
    {
        g_mocapBridge.Cancel();
        g_mocapPlaying = false;
        if (g_renderer) g_renderer->setScene3dEnabled(true);
    }

    // Play/Pause toggle
    if (args.mocapRequestTogglePlay)
    {
        g_mocapPlaying = !g_mocapPlaying;
    }

    // Timeline scrub
    if (args.mocapRequestSetTime)
    {
        g_mocapPlayTime = args.mocapRequestedTime;
    }

    // Finger curl update
    if (args.mocapRequestSetFingerCurl)
    {
        g_mocapRetargeter.fingerCurl.leftGrip  = args.mocapRequestedFingerCurlL;
        g_mocapRetargeter.fingerCurl.rightGrip = args.mocapRequestedFingerCurlR;
    }

    // Advance mocap playback
    if (g_mocapPlaying && g_mocapBridge.GetState() == MOCAP_DONE)
    {
        float duration = (g_mocapBridge.GetTotalFrames() > 1 && g_mocapBridge.GetFps() > 0.0f)
                         ? (float)(g_mocapBridge.GetTotalFrames() - 1) / g_mocapBridge.GetFps() : 0.0f;
        g_mocapPlayTime += 1.0f / 30.0f; // approximate per-frame advance
        if (g_mocapPlayTime > duration)
            g_mocapPlayTime = 0.0f; // loop
    }

    // Convert button — retarget and export native Conquest JSON
    // Works in two modes:
    //   1. After in-app WHAM processing (bridge state == DONE)
    //   2. After external batch processing (bridge state == IDLE, find pkl on disk)
    if (args.mocapRequestConvert)
    {
        // baseDir = ZeroEngine root (Scene3D's parent). Env-var override wins.
        std::string baseDirStr;
        {
            char envBuf[1024] = {0};
            GetEnvironmentVariableA("VESPUCCI_ENGINE_ROOT", envBuf, sizeof(envBuf));
            if (envBuf[0]) {
                baseDirStr = envBuf;
            } else {
                char exeDir[MAX_PATH] = {0};
                GetModuleFileNameA(NULL, exeDir, MAX_PATH);
                char* slash = strrchr(exeDir, '\\'); if (slash) *slash = 0;
                baseDirStr = std::string(exeDir) + "\\..";
            }
        }
        const char* baseDir = baseDirStr.c_str();

        // Python interpreter for the standalone smpl_to_conquest.py converter.
        std::string convPythonExe;
        {
            char pyBuf[1024] = {0};
            GetEnvironmentVariableA("VESPUCCI_WHAM_PYTHON", pyBuf, sizeof(pyBuf));
            convPythonExe = pyBuf[0] ? pyBuf : "python";
        }

        bool converted = false;

        if (g_mocapBridge.GetState() == MOCAP_DONE && !g_mocapBridge.GetFrames().empty())
        {
            // Mode 1: In-app WHAM completed — retarget C++ frames
            g_mocapRetargeter.RetargetAll(g_mocapBridge, g_mocapFrames);

            char outputPath[1024];
            sprintf(outputPath, "%s\\GameFiles\\animations\\RH6_mocap_capture.json", baseDir);

            converted = MocapExporter::ExportConquestJSON(
                outputPath, "RH6_mocap_capture", g_mocapFrames,
                g_mocapBridge, g_mocapBridge.GetFps());
        }
        else
        {
            // Mode 2: External batch — find pkl and run Python converter
            std::string pklPath;
            // Check capture folder first
            char capturePkl[1024];
            sprintf(capturePkl, "%s\\Engine\\source\\Vendors\\WHAM\\output\\capture\\wham_output.pkl", baseDir);
            if (GetFileAttributesA(capturePkl) != INVALID_FILE_ATTRIBUTES)
                pklPath = capturePkl;

            // Check the current user's Downloads folder for a WHAM PKL dump.
            // Reads USERPROFILE so this works on any machine, not just dev.
            if (pklPath.empty())
            {
                char userProfile[MAX_PATH] = {0};
                GetEnvironmentVariableA("USERPROFILE", userProfile, sizeof(userProfile));
                if (userProfile[0]) {
                    char downloadsGlob[MAX_PATH];
                    sprintf(downloadsGlob, "%s\\Downloads\\*", userProfile);
                    WIN32_FIND_DATAA fd;
                    HANDLE hFind = FindFirstFileA(downloadsGlob, &fd);
                    if (hFind != INVALID_HANDLE_VALUE) {
                        do {
                            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && fd.cFileName[0] != '.') {
                                char candidate[1024];
                                sprintf(candidate, "%s\\Downloads\\%s\\wham_output.pkl", userProfile, fd.cFileName);
                                if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
                                    pklPath = candidate;
                                }
                            }
                        } while (FindNextFileA(hFind, &fd));
                        FindClose(hFind);
                    }
                }
            }

            if (!pklPath.empty())
            {
                char cmd[4096];
                sprintf(cmd,
                    "\"%s\" "
                    "\"%s\\Engine\\source\\Vendors\\WHAM\\smpl_to_conquest.py\" "
                    "--input \"%s\" --subject 0 --name \"RH6_mocap_capture\" "
                    "--output \"%s\\GameFiles\\animations\\RH6_mocap_capture.json\"",
                    convPythonExe.c_str(), baseDir, pklPath.c_str(), baseDir);
                system(cmd);
                converted = true;
            }
        }

        FILE* dbg = fopen("mocap_debug.log", "a");
        if (dbg) {
            fprintf(dbg, "[MOCAP] Convert: %s (bridge=%d, frames=%d)\n",
                    converted ? "SUCCESS" : "FAILED",
                    (int)g_mocapBridge.GetState(), (int)g_mocapFrames.size());
            fclose(dbg);
        }
    }

    // Capture flags for input gating (updated by the glue DLL).
    g_uiWantCaptureMouse = (args.wantCaptureMouse != 0);
    g_uiWantCaptureKeyboard = (args.wantCaptureKeyboard != 0);

    // Viewport content rect (for RT rendering + input mapping).
    if (!g_showLegacyUI && args.viewportW > 0 && args.viewportH > 0)
    {
        g_imguiViewportX = args.viewportX;
        g_imguiViewportY = args.viewportY;
        g_imguiViewportW = args.viewportW;
        g_imguiViewportH = args.viewportH;
    }
    if (g_renderer)
    {
        g_renderer->setImGuiViewportActive(!g_showLegacyUI);
        if (!g_showLegacyUI && HasValidImGuiViewportRect())
        {
            g_renderer->setImGuiViewportSize(g_imguiViewportW, g_imguiViewportH);
        }
    }

    // Handle UI requests coming back from ImGui glue
    if (args.requestRescan)
    {
        LoadGameFiles();
    }
    if (args.requestLoadModel && args.requestedModelIndex >= 0 && args.requestedModelIndex < (int)s_modelNames.size())
    {
        g_browserIndexModel = args.requestedModelIndex;
        SelectGameModel(g_browserIndexModel);
    }
    if (args.requestLoadAnim && args.requestedAnimIndex >= 0 && args.requestedAnimIndex < (int)s_animNames.size())
    {
        g_browserIndexAnim = args.requestedAnimIndex;
        SelectGameAnimation(g_browserIndexAnim);
    }
    // PAK binary animation loading — deferred, using JSON pipeline instead
    // See PakAnimLoader.h/cpp for future binary loading support
    if (args.requestSpawnEffect)
    {
        int idx = args.requestedEffectIndex;
        if (idx >= 0 && idx < (int)g_effectsList.size() && g_renderer)
        {
            g_browserIndexEffect = idx;
            std::string effectPath = "..\\GameFiles\\effects\\" + g_effectsList[idx] + ".json";
            g_renderer->loadTestEffect(effectPath.c_str(), "..\\GameFiles\\textures\\");
            hkVector4 spawnPos = g_renderer->getCameraTarget();
            g_renderer->spawnTestEffect(g_effectsList[idx].c_str(), spawnPos);
            if (g_textStatus)
            {
                char msg[256];
                sprintf(msg, "Spawned effect: %s", g_effectsList[idx].c_str());
                SetWindowText(g_textStatus, msg);
            }
        }
    }

    // Handle Level Blocks → Viewport: load entity mesh by name
    if (args.requestLoadEntityMesh && args.requestedEntityMeshName[0] && g_gameFilesBrowser)
    {
        const std::vector<AssetBrowser::AssetInfo>& jmodels = g_gameFilesBrowser->getJModels();
        int foundIdx = -1;
        for (int mi = 0; mi < (int)jmodels.size(); ++mi)
        {
            // Match: mesh name equals jmodel name (case-insensitive)
            if (_stricmp(jmodels[mi].name.c_str(), args.requestedEntityMeshName) == 0)
            {
                foundIdx = mi;
                break;
            }
        }
        if (foundIdx < 0)
        {
            // Try partial match (mesh name contained in jmodel name)
            std::string needle(args.requestedEntityMeshName);
            for (size_t ci = 0; ci < needle.size(); ++ci) needle[ci] = (char)tolower((unsigned char)needle[ci]);
            for (int mi = 0; mi < (int)jmodels.size(); ++mi)
            {
                std::string hay(jmodels[mi].name);
                for (size_t ci = 0; ci < hay.size(); ++ci) hay[ci] = (char)tolower((unsigned char)hay[ci]);
                if (hay.find(needle) != std::string::npos)
                {
                    foundIdx = mi;
                    break;
                }
            }
        }
        if (foundIdx >= 0)
        {
            // Enable viewport if disabled
            if (g_renderer && !g_renderer->getScene3dEnabled())
                g_renderer->setScene3dEnabled(true);
            SelectGameModel(foundIdx);
            DebugLog("Level Blocks: loaded entity mesh");
        }
        else
        {
            char msg[512];
            sprintf(msg, "Level Blocks: mesh '%s' not found in GameFiles", args.requestedEntityMeshName);
            DebugLog(msg);
            if (g_textStatus) SetWindowText(g_textStatus, msg);
        }
    }

    if (g_renderer)
    {
        if (args.requestSetSkyboxEnabled)
        {
            g_renderer->setSkyboxEnabled(args.requestedSkyboxEnabled != 0);
        }
        if (args.requestSetSkyRenderMode)
        {
            g_renderer->setSkyRenderMode(args.requestedSkyRenderMode);
        }
        if (args.requestSelectSkybox)
        {
            g_renderer->setActiveSkyboxIndex(args.requestedSkyboxIndex);
        }
        if (args.requestSetCloudEnabled)
        {
            g_renderer->setCloudLayerEnabled(args.requestedCloudEnabled != 0);
        }
        if (args.requestSelectCloudSkybox)
        {
            g_renderer->setCloudSkyboxIndex(args.requestedCloudSkyboxIndex);
        }
        if (args.requestApplyEnvMash)
        {
            ApplyEnvMashPreset(args.requestedEnvMashId);
        }
    }
    if (args.requestSetEditMode)
    {
        g_editMode = (args.requestedEditMode != 0);
    }
    if (g_renderer)
    {
        if (args.requestSelectBone)
        {
            g_renderer->setSelectedBoneIndex(args.requestedBoneIndex);
        }
        if (args.requestSetGizmoMode)
        {
            g_renderer->setGizmoMode(args.requestedGizmoMode);
        }
        if (args.requestSetGizmoSpace)
        {
            int desired = args.requestedGizmoSpace;
            if (desired != (int)g_renderer->getGizmoSpace())
            {
                g_renderer->toggleGizmoSpace();
            }
        }
        if (args.requestSetRecording)
        {
            g_renderer->setEditorRecording(args.requestedRecording != 0);
        }
        if (args.requestSetInterpMode)
        {
            g_renderer->setEditorInterpolationMode(args.requestedInterpMode);
        }
        if (args.requestSetEditorEasing)
        {
            g_renderer->setEditorDefaultEasing(
                args.requestedEditorEasingType,
                args.requestedEditorEasingCp1x, args.requestedEditorEasingCp1y,
                args.requestedEditorEasingCp2x, args.requestedEditorEasingCp2y);
        }
        if (args.requestSetActiveKeyEasing)
        {
            int boneIdx = g_renderer->getSelectedBoneIndex();
            if (boneIdx >= 0)
            {
                switch (args.requestedActiveKeyChannel)
                {
                case 1: // translation
                    g_renderer->setEditorTransKeyEasing(
                        boneIdx, args.requestedActiveKeyIndex, args.requestedActiveKeyEasingType,
                        args.requestedActiveKeyCp1x, args.requestedActiveKeyCp1y,
                        args.requestedActiveKeyCp2x, args.requestedActiveKeyCp2y);
                    break;
                case 2: // scale
                    g_renderer->setEditorScaleKeyEasing(
                        boneIdx, args.requestedActiveKeyIndex, args.requestedActiveKeyEasingType,
                        args.requestedActiveKeyCp1x, args.requestedActiveKeyCp1y,
                        args.requestedActiveKeyCp2x, args.requestedActiveKeyCp2y);
                    break;
                default: // rotation
                    g_renderer->setEditorRotKeyEasing(
                        boneIdx, args.requestedActiveKeyIndex, args.requestedActiveKeyEasingType,
                        args.requestedActiveKeyCp1x, args.requestedActiveKeyCp1y,
                        args.requestedActiveKeyCp2x, args.requestedActiveKeyCp2y);
                    break;
                }
            }
        }
        if (args.requestSetActiveKeyInterpolation)
        {
            int boneIdx = g_renderer->getSelectedBoneIndex();
            if (boneIdx >= 0)
            {
                switch (args.requestedActiveKeyChannel)
                {
                case 1: // translation
                    g_renderer->setEditorTransKeyInterpolationMode(
                        boneIdx, args.requestedActiveKeyIndex, args.requestedActiveKeyInterpMode);
                    g_renderer->setEditorTransKeyTangents(
                        boneIdx, args.requestedActiveKeyIndex,
                        args.requestedActiveKeyInTanX, args.requestedActiveKeyOutTanX,
                        args.requestedActiveKeyInTanY, args.requestedActiveKeyOutTanY,
                        args.requestedActiveKeyInTanZ, args.requestedActiveKeyOutTanZ);
                    break;
                case 2: // scale
                    g_renderer->setEditorScaleKeyInterpolationMode(
                        boneIdx, args.requestedActiveKeyIndex, args.requestedActiveKeyInterpMode);
                    g_renderer->setEditorScaleKeyTangents(
                        boneIdx, args.requestedActiveKeyIndex,
                        args.requestedActiveKeyInTanX, args.requestedActiveKeyOutTanX,
                        args.requestedActiveKeyInTanY, args.requestedActiveKeyOutTanY,
                        args.requestedActiveKeyInTanZ, args.requestedActiveKeyOutTanZ);
                    break;
                default: // rotation has no value curve interpolation
                    break;
                }
            }
        }
        if (args.requestSetRotSnapEnabled)
        {
            g_renderer->setRotateSnapEnabled(args.requestedRotSnapEnabled != 0);
        }
        if (args.requestSetRotSnapDegrees)
        {
            g_renderer->setRotateSnapDegrees(args.requestedRotSnapDegrees);
        }
        if (args.requestSetMoveSnapEnabled)
        {
            g_renderer->setMoveSnapEnabled(args.requestedMoveSnapEnabled != 0);
        }
        if (args.requestSetMoveSnapUnits)
        {
            g_renderer->setMoveSnapUnits(args.requestedMoveSnapUnits);
        }
        if (args.requestCommitEdit)
        {
            CommitCurrentEdit();
        }
        if (args.requestCancelEdit)
        {
            CancelCurrentEdit();
        }
        if (args.requestResetSelectedBone)
        {
            g_renderer->resetSelectedBoneToRest();
        }
        if (args.requestResetAllBones)
        {
            g_renderer->resetAllBonesToRest();
        }
        if (args.requestUndoPose)
        {
            g_renderer->undoPoseEdit();
        }
        if (args.requestRedoPose)
        {
            g_renderer->redoPoseEdit();
        }
        if (args.requestKeyBoneTRS)
        {
            bool keyRot = (args.requestedKeyRot != 0);
            bool keyTrans = (args.requestedKeyTrans != 0);
            bool keyScale = (args.requestedKeyScale != 0);
            g_renderer->keySelectedBoneLocalTRSScale(
                args.requestedBoneTx, args.requestedBoneTy, args.requestedBoneTz,
                args.requestedBoneRxDeg, args.requestedBoneRyDeg, args.requestedBoneRzDeg,
                args.requestedBoneSx, args.requestedBoneSy, args.requestedBoneSz,
                g_renderer->getJsonAnimationTime(),
                keyRot, keyTrans, keyScale);
        }
        if (args.requestSetTimelineDuration)
        {
            g_renderer->setEditorTimelineDuration(args.requestedTimelineDuration);
        }
        if (args.requestExportClip)
        {
            strncpy(g_exportName, args.requestedExportName, sizeof(g_exportName) - 1);
            g_exportName[sizeof(g_exportName) - 1] = '\0';

            char nameBuf[128];
            const char* base = g_exportName;
            if (!base || base[0] == '\0')
            {
                base = "Custom_Anim";
            }
            strcpy_s(nameBuf, base);
            char* dot = strrchr(nameBuf, '.');
            if (!dot || _stricmp(dot, ".json") != 0)
            {
                strcat_s(nameBuf, ".json");
            }

            char path[512];
            sprintf(path, "..\\GameFiles\\animations\\%s", nameBuf);
            if (g_renderer->exportEditorClip(path))
            {
                // After successful export, reload the exported animation so the
                // user immediately sees the result (including extended durations).
                g_renderer->setJsonAnimationPath(path);

                // Update the browser index to point to the exported file
                if (g_gameFilesBrowser)
                {
                    // Strip extension for name comparison
                    char exportedName[128];
                    strcpy_s(exportedName, nameBuf);
                    char* extDot = strrchr(exportedName, '.');
                    if (extDot) *extDot = '\0';

                    const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();
                    for (int ai = 0; ai < (int)anims.size(); ++ai)
                    {
                        if (_stricmp(anims[ai].name.c_str(), exportedName) == 0)
                        {
                            g_gameAnimIndex = ai;
                            g_browserIndexAnim = ai;
                            strcpy_s(g_activeAnimName, anims[ai].name.c_str());
                            break;
                        }
                    }
                }
            }
        }
        if (args.requestPlayToggle)
        {
            bool paused = g_renderer->isJsonAnimationPaused();
            g_renderer->setJsonAnimationPaused(!paused);
        }
        if (args.requestStop)
        {
            g_renderer->seekJsonAnimation(0.0f);
            g_renderer->setJsonAnimationPaused(true);
        }
        if (args.requestPrev)
        {
            g_renderer->seekJsonAnimation(0.0f);
        }
        if (args.requestNext)
        {
            float dur = g_renderer->getJsonAnimationDuration();
            if (dur > 0.0f) g_renderer->seekJsonAnimation(dur);
        }
        if (args.requestLoopToggle)
        {
            bool loop = g_renderer->getLoopAnimation();
            g_renderer->setLoopAnimation(!loop);
        }
        if (args.requestSetTime)
        {
            g_renderer->seekJsonAnimation(args.requestedTime);
        }
        if (args.requestSetSpeed)
        {
            g_renderer->setPlaybackSpeed(args.requestedSpeed);
        }
        // Handle animation event requests
        if (args.requestAddEvent)
        {
            // Use new generic event addition if event type is specified
            if (args.requestedAddEventType > 0)
            {
                std::vector<JsonAnimEventVal> vals;
                for (int i = 0; i < args.requestedAddEventParamCount && i < 9; ++i)
                {
                    JsonAnimEventVal v;
                    if (args.requestedAddEventParamTypes[i] == 1) // Float
                    {
                        v.type = JsonAnimEventVal::EVT_FLOAT;
                        v.floatVal = args.requestedAddEventParamFloats[i];
                    }
                    else if (args.requestedAddEventParamTypes[i] == 2) // CRC
                    {
                        v.type = JsonAnimEventVal::EVT_CRC;
                        strcpy_s(v.crcVal, sizeof(v.crcVal), args.requestedAddEventParamCrcs[i]);
                    }
                    else // Int (default)
                    {
                        v.type = JsonAnimEventVal::EVT_INT;
                        v.intVal = args.requestedAddEventParamInts[i];
                    }
                    vals.push_back(v);
                }
                g_renderer->addJsonAnimEventGeneric(args.requestedAddEventType,
                                                   args.requestedAddEventTime,
                                                   &vals);
            }
            else
            {
                // Fall back to legacy SoundEvent/SoundCue for backward compatibility
                bool isCue = (args.requestedAddEventIsSoundCue != 0);
                g_renderer->addJsonAnimEvent(isCue,
                                             args.requestedAddEventTime,
                                             args.requestedAddEventCrc0,
                                             args.requestedAddEventCrc1,
                                             args.requestedAddEventCrc2);
            }
            args.requestAddEvent = 0;  // Reset flag to prevent repeated additions
        }
        if (args.requestDeleteEvent)
        {
            g_renderer->removeJsonAnimEvent(args.requestedDeleteEventIndex);
            args.requestDeleteEvent = 0;  // Reset flag to prevent repeated deletions
        }
        if (args.requestSetRotInterpMode)
        {
            g_renderer->setRotInterpMode(args.requestedRotInterpMode);
        }
        if (args.requestSetIKEnabled)
        {
            g_renderer->setIKEnabled(args.requestedIKEnabled != 0);
        }
        if (args.requestSetFootIKEnabled)
        {
            g_renderer->setFootIKEnabled(args.requestedFootIKEnabled != 0);
        }
        if (args.requestSetLookAtEnabled)
        {
            g_renderer->setLookAtIKEnabled(args.requestedLookAtEnabled != 0);
        }
        if (args.requestSetAimEnabled)
        {
            g_renderer->setAimIKEnabled(args.requestedAimEnabled != 0);
        }
        if (args.requestSetLookAtBone)
        {
            g_renderer->setLookAtBoneIndex(args.requestedLookAtBone);
        }
        if (args.requestSetAimBone)
        {
            g_renderer->setAimBoneIndex(args.requestedAimBone);
        }
        if (args.requestSetLookAtTarget)
        {
            g_renderer->setLookAtTarget(args.requestedLookAtTargetX, args.requestedLookAtTargetY, args.requestedLookAtTargetZ);
        }
        if (args.requestSetAimTarget)
        {
            g_renderer->setAimTarget(args.requestedAimTargetX, args.requestedAimTargetY, args.requestedAimTargetZ);
        }
        if (args.requestSetLookAtWeight)
        {
            g_renderer->setLookAtWeight(args.requestedLookAtWeight);
        }
        if (args.requestSetAimWeight)
        {
            g_renderer->setAimWeight(args.requestedAimWeight);
        }
        if (args.requestSetIKSelectedChain)
        {
            s_ikSelectedChain = args.requestedIKSelectedChain;
        }
        if (args.requestSetIKChainEnabled)
        {
            g_renderer->setIKChainEnabled(args.requestedIKChainIndex, args.requestedIKChainEnabled != 0);
        }
        if (args.requestSetIKChainTarget)
        {
            g_renderer->setIKChainTarget(args.requestedIKChainTargetIndex,
                                         args.requestedIKChainTargetX,
                                         args.requestedIKChainTargetY,
                                         args.requestedIKChainTargetZ);
        }
        if (args.requestSetIKChainTargetBone)
        {
            g_renderer->setIKChainTargetBone(args.requestedIKChainTargetBoneIndex,
                                             args.requestedIKChainTargetBone);
        }
        if (args.requestRebuildIKChains)
        {
            g_renderer->rebuildDefaultIKChains();
        }
        if (args.requestSetPhysEnabled)
        {
            g_renderer->setPhysicalAnimEnabled(args.requestedPhysEnabled != 0);
        }
        if (args.requestSetRagdollEnabled)
        {
            g_renderer->setRagdollEnabled(args.requestedRagdollEnabled != 0);
        }
        if (args.requestSetRagdollBlend)
        {
            g_renderer->setRagdollBlend(args.requestedRagdollBlend);
        }
        if (args.requestSetPhysPosStiffness)
        {
            g_renderer->setPhysicalPosStiffness(args.requestedPhysPosStiffness);
        }
        if (args.requestSetPhysPosDamping)
        {
            g_renderer->setPhysicalPosDamping(args.requestedPhysPosDamping);
        }
        if (args.requestSetPhysRotStiffness)
        {
            g_renderer->setPhysicalRotStiffness(args.requestedPhysRotStiffness);
        }
        if (args.requestSetPhysRotDamping)
        {
            g_renderer->setPhysicalRotDamping(args.requestedPhysRotDamping);
        }
        if (args.requestSetRagdollGravity)
        {
            g_renderer->setRagdollGravity(args.requestedRagdollGravity);
        }
        if (args.requestResetPhysicsState)
        {
            g_renderer->resetPhysicsState();
        }
        if (args.requestApplyHitImpulse)
        {
            hkVector4 lin;
            lin.set(args.requestedHitLinearX, args.requestedHitLinearY, args.requestedHitLinearZ);
            hkVector4 ang;
            ang.set(args.requestedHitAngularX, args.requestedHitAngularY, args.requestedHitAngularZ);
            g_renderer->applyHitImpulse(args.requestedHitBoneIndex, lin, ang);
        }
        if (args.requestSetAnimFixedEnabled)
        {
            g_renderer->setAnimFixedStepEnabled(args.requestedAnimFixedEnabled != 0);
        }
        if (args.requestSetAnimFixedStep)
        {
            g_renderer->setAnimFixedStep(args.requestedAnimFixedStep);
        }
        if (args.requestSetAnimFixedMaxSteps)
        {
            g_renderer->setAnimFixedMaxSteps(args.requestedAnimFixedMaxSteps);
        }
        if (args.requestResetAnimFixedAccumulator)
        {
            g_renderer->resetAnimFixedAccumulator();
        }
        if (args.requestSetTimeWarpEnabled)
        {
            g_renderer->setTimeWarpEnabled(args.requestedTimeWarpEnabled != 0);
        }
        if (args.requestSetTimeWarpType)
        {
            g_renderer->setTimeWarpEasingType(args.requestedTimeWarpType);
        }
        if (args.requestSetTimeWarpCP)
        {
            g_renderer->setTimeWarpControlPoints(args.requestedTimeWarpCp1x,
                                                 args.requestedTimeWarpCp1y,
                                                 args.requestedTimeWarpCp2x,
                                                 args.requestedTimeWarpCp2y);
        }
        if (args.requestSetRootMotionMode)
        {
            g_renderer->setRootMotionMode(args.requestedRootMotionMode);
        }
        if (args.requestSetRootMotionLocks)
        {
            g_renderer->setRootMotionLock(args.requestedRootMotionLockX != 0,
                                          args.requestedRootMotionLockY != 0,
                                          args.requestedRootMotionLockZ != 0);
        }
        if (args.requestResetRootMotion)
        {
            g_renderer->resetRootMotionState();
        }
        if (args.requestSetRootMotionWarpEnabled)
        {
            g_renderer->setRootMotionWarpEnabled(args.requestedRootMotionWarpEnabled != 0);
        }
        if (args.requestSetRootMotionWarpMode)
        {
            g_renderer->setRootMotionWarpMode(args.requestedRootMotionWarpMode);
        }
        if (args.requestSetRootMotionWarpTarget)
        {
            g_renderer->setRootMotionWarpTarget(args.requestedRootMotionWarpTargetX,
                                                args.requestedRootMotionWarpTargetY,
                                                args.requestedRootMotionWarpTargetZ);
        }
        if (args.requestSetCompressAuto)
        {
            g_renderer->setCompressionAuto(args.requestedCompressAuto != 0);
        }
        if (args.requestSetCompressTolerances)
        {
            g_renderer->setCompressionTolerances(args.requestedCompressPosTol,
                                                 args.requestedCompressRotTolDeg,
                                                 args.requestedCompressScaleTol,
                                                 args.requestedCompressRootTol);
        }
        if (args.requestSetCompressQuantizeRot)
        {
            g_renderer->setCompressionQuantizeRotations(args.requestedCompressQuantizeRot != 0);
        }
        if (args.requestSetCompressStripRot)
        {
            g_renderer->setCompressionStripRotations(args.requestedCompressStripRot != 0);
        }
        if (args.requestOptimizeActiveClip)
        {
            g_renderer->optimizeActiveJsonClip();
        }
        if (args.requestSetBlendEnabled)
        {
            g_renderer->setJsonBlendEnabled(args.requestedBlendEnabled != 0);
        }
        if (args.requestSetBlendMode)
        {
            g_renderer->setJsonBlendMode(args.requestedBlendMode);
        }
        if (args.requestSetBlendRotMode)
        {
            g_renderer->setJsonBlendRotMode(args.requestedBlendRotMode);
        }
        if (args.requestSetBlendAlpha)
        {
            g_renderer->setJsonBlendAlpha(args.requestedBlendAlpha);
        }
        if (args.requestSetBlendLayerRootBone)
        {
            g_renderer->setJsonBlendLayerRootBone(args.requestedBlendLayerRootBone);
        }
        if (args.requestSetBlendAnimIndex)
        {
            SelectBlendAnimation(args.requestedBlendAnimIndex);
        }
        if (args.requestSetBlendIndependentTime)
        {
            g_renderer->m_jsonBlendIndependentTime = (args.requestedBlendIndependentTime != 0);
        }
        if (args.requestSetBlendLoopBlendClip)
        {
            g_renderer->m_jsonBlendLoopBlendClip = (args.requestedBlendLoopBlendClip != 0);
        }
        if (args.requestSetBlendFadeDuration)
        {
            g_renderer->m_jsonBlendFadeDuration = args.requestedBlendFadeDuration;
        }
        if (args.requestSetBlendLayerState)
        {
            int li = args.requestedBlendLayerIdx;
            if (li >= 0 && li < 16)
                g_renderer->m_blendLayers[li].active = (args.requestedBlendLayerActive != 0);
        }
        if (args.requestSetBlendLayerWeight)
        {
            int li = args.requestedBlendLayerIdx;
            if (li >= 0 && li < 16)
            {
                g_renderer->m_blendLayers[li].weight = args.requestedBlendLayerWeight;
                g_renderer->m_blendLayers[li].targetWeight = args.requestedBlendLayerWeight;
                // Auto-normalize (only non-exclusive full-body layers — exclusive per-bone layers are overrides)
                if (g_renderer->m_blendAutoNormalize)
                {
                    float total = 0.0f;
                    for (int i = 0; i < 16; ++i)
                    {
                        if (!g_renderer->m_blendLayers[i].active) continue;
                        if (g_renderer->m_blendLayers[i].exclusive) continue; // skip exclusive layers
                        total += g_renderer->m_blendLayers[i].weight;
                    }
                    if (total > 1.0f)
                    {
                        float inv = 1.0f / total;
                        for (int i = 0; i < 16; ++i)
                        {
                            if (!g_renderer->m_blendLayers[i].active) continue;
                            if (g_renderer->m_blendLayers[i].exclusive) continue;
                            g_renderer->m_blendLayers[i].weight *= inv;
                        }
                    }
                }
            }
        }
        if (args.requestSetBlendLayerClip)
        {
            int li = args.requestedBlendLayerIdx;
            int ci = args.requestedBlendLayerClipIdx;
            if (li >= 0 && li < 16 && g_gameFilesBrowser)
            {
                const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();
                if (ci >= 0 && ci < (int)anims.size())
                {
                    g_renderer->loadBlendLayerClip(li, anims[ci].path.c_str());
                    s_blendLayerClipIndices[li] = ci;
                }
                else
                {
                    // Clear the layer clip
                    s_blendLayerClipIndices[li] = -1;
                    if (g_renderer->m_blendLayers[li].clip)
                    {
                        delete g_renderer->m_blendLayers[li].clip;
                        g_renderer->m_blendLayers[li].clip = NULL;
                    }
                    g_renderer->m_blendLayers[li].path[0] = '\0';
                }
            }
        }
        if (args.requestSetBlendLayerLoop)
        {
            int li = args.requestedBlendLayerIdx;
            if (li >= 0 && li < 16)
                g_renderer->m_blendLayers[li].loop = (args.requestedBlendLayerLoop != 0);
        }
        if (args.requestSetBlendLayerBoneMask)
        {
            int li = args.requestedBlendLayerIdx;
            int bi = args.requestedBlendLayerBoneMaskBone;
            if (li >= 0 && li < 16 && bi >= 0 && bi < 62)
                g_renderer->m_blendLayers[li].boneMask[bi] = (unsigned char)(args.requestedBlendLayerBoneMaskValue ? 1 : 0);
        }
        if (args.requestSetBlendLayerBoneMaskAll)
        {
            int li = args.requestedBlendLayerIdx;
            if (li >= 0 && li < 16)
                memset(g_renderer->m_blendLayers[li].boneMask, (unsigned char)(args.requestedBlendLayerBoneMaskValue ? 1 : 0), 62);
        }
        if (args.requestSetBlendLayerBoneMaskPreset)
        {
            int li = args.requestedBlendLayerIdx;
            int preset = args.requestedBlendLayerBoneMaskPreset;
            if (li >= 0 && li < 16)
            {
                memset(g_renderer->m_blendLayers[li].boneMask, 0, 62);
                if (preset == 0) // Upper body: bones 3-51 (Lumbar1 through RThumb3)
                {
                    for (int bi = 3; bi <= 51 && bi < 62; ++bi)
                        g_renderer->m_blendLayers[li].boneMask[bi] = 1;
                }
                else if (preset == 1) // Lower body: bones 52-61 (LThigh through RFootBone3)
                {
                    for (int bi = 52; bi <= 61 && bi < 62; ++bi)
                        g_renderer->m_blendLayers[li].boneMask[bi] = 1;
                }
            }
        }
        if (args.requestSetBlendAutoNormalize)
        {
            g_renderer->m_blendAutoNormalize = (args.requestedBlendAutoNormalize != 0);
        }
        if (args.requestCapturePoseSnapshot)
        {
            // Capture current local pose into snapshot buffer
            int bc = g_renderer->getSkeletonBoneCount();
            if (bc > 0)
            {
                g_renderer->m_poseSnapshot.setSize(bc);
                // The current pose is in the hkaPose after buildPoseFromJson
                // We need to capture it during the render frame — set a flag
                g_renderer->m_poseSnapshotValid = true;
                // Actually capture: we need the pose. For now, mark as "capture next frame"
                // The renderer will capture in buildPoseFromJson when this flag is set
            }
        }
        if (args.requestSetPoseSnapshotBlendActive)
        {
            g_renderer->m_poseSnapshotBlendActive = (args.requestedPoseSnapshotBlendActive != 0);
        }
        if (args.requestSetRootPathEnabled)
        {
            g_renderer->m_rootPathEnabled = (args.requestedRootPathEnabled != 0);
        }
        if (args.requestClearRootPath)
        {
            g_renderer->m_rootPathCount = 0;
            g_renderer->m_rootPathHead = 0;
        }
        if (args.requestSetPoseSnapshotBlendAlpha)
        {
            g_renderer->m_poseSnapshotBlendAlpha = args.requestedPoseSnapshotBlendAlpha;
        }
        if (args.requestSetLoopRegion)
        {
            g_renderer->m_loopRegionEnabled = true;
            g_renderer->m_loopRegionIn = args.requestedLoopRegionIn;
            g_renderer->m_loopRegionOut = args.requestedLoopRegionOut;
        }
        if (args.requestClearLoopRegion)
        {
            g_renderer->m_loopRegionEnabled = false;
        }
        if (args.requestBlendFadeIn)
        {
            g_renderer->m_jsonBlendFadingIn = true;
            g_renderer->m_jsonBlendFadeElapsed = 0.0f;
            g_renderer->m_jsonBlendTime = 0.0f; // restart blend clip from beginning
        }
        if (args.requestBlendFadeOut)
        {
            g_renderer->m_jsonBlendFadingIn = false;
            g_renderer->m_jsonBlendFadeElapsed = 0.0f;
        }
        if (args.requestLoadGraph)
        {
            if (args.requestedGraphPath[0] != '\0')
            {
                bool loaded = false;
                if (EndsWithNoCase(args.requestedGraphPath, ".lua"))
                {
                    loaded = g_renderer->loadAnimationGraphLua(args.requestedGraphPath);
                }
                else
                {
                    loaded = g_renderer->loadAnimationGraph(args.requestedGraphPath);
                }
                if (loaded)
                {
                    g_renderer->setAnimationGraphEnabled(g_graphEnabledDesired);
                }
            }
        }
        if (args.requestClearGraph)
        {
            g_renderer->clearAnimationGraph();
        }
        if (args.requestSetGraphEnabled)
        {
            g_graphEnabledDesired = (args.requestedGraphEnabled != 0);
            g_renderer->setAnimationGraphEnabled(g_graphEnabledDesired);
        }
        if (args.requestSetGraphParam)
        {
            int idx = args.requestedGraphParamIndex;
            int type = args.requestedGraphParamType;
            if (type == IMGUI_GRAPH_PARAM_FLOAT)
            {
                g_renderer->setAnimationGraphParamFloat(idx, args.requestedGraphParamFloat);
            }
            else if (type == IMGUI_GRAPH_PARAM_INT)
            {
                g_renderer->setAnimationGraphParamInt(idx, args.requestedGraphParamInt);
            }
            else
            {
                g_renderer->setAnimationGraphParamBool(idx, args.requestedGraphParamBool != 0);
            }
        }
        if (args.requestFireGraphTrigger)
        {
            g_renderer->fireAnimationGraphTrigger(args.requestedGraphTriggerIndex);
        }
        if (args.requestSetGraphState)
        {
            g_renderer->setAnimationGraphCurrentState(args.requestedGraphStateIndex);
        }
        if (args.requestResetGraph)
        {
            g_renderer->resetAnimationGraph();
        }
        if (args.requestBuildGraphFromCurrentAnim)
        {
            char autoPath[512] = "";
            if (BuildGraphFromCurrentAnimation(autoPath, sizeof(autoPath)))
            {
                g_renderer->loadAnimationGraph(autoPath);
                RefreshGraphList();
            }
        }
        if (args.requestLoadLuaTable)
        {
            if (args.requestedLuaTablePath[0] != '\0')
            {
                if (LoadLuaAnimTableFromPath(args.requestedLuaTablePath))
                {
                    GuessAndLoadModelForLuaTable();
                }
            }
        }
        if (args.requestLoadLuaStates)
        {
            if (args.requestedLuaStatesPath[0] != '\0')
            {
                LoadLuaAnimStatesFromPath(args.requestedLuaStatesPath);
            }
        }
        /* Phase 2: Set AnimStatesTranslator selected state */
        if (args.requestSetAnimStatesSelectedIndex)
        {
            if (g_renderer)
            {
                g_renderer->setAnimStatesTranslatorSelectedStateIndex(args.requestedAnimStatesSelectedIndex);
                /* Phase 3: Resolve clips for this state */
                UpdateResolvedClipsForActiveState();
            }
        }
        /* Phase 2.5: ASM Parity Toggles */
        if (args.requestSetAnimationDriven)
        {
            if (g_renderer)
            {
                g_renderer->setAnimationDrivenEnabled(args.requestedAnimationDriven != 0);
            }
        }
        if (args.requestSetRootMotionWarp)
        {
            if (g_renderer)
            {
                g_renderer->setRootMotionWarpEnabled(args.requestedRootMotionWarp != 0);
            }
        }
        if (args.requestSetAnimDrivenMode)
        {
            if (g_renderer)
            {
                g_renderer->setAnimDrivenMode((Scene3DRenderer::AnimDrivenMode)args.requestedAnimDrivenMode);
            }
        }
        
        /* Phase 4: Filter Evaluation & Clip Selection */
        if (args.requestSetSelectedClipIndex)
        {
            /* Update selected clip index (UI feedback only, no playback) */
            /* This is handled by the renderer's internal state */
            if (g_renderer)
            {
                /* For now, just track the selection. Phase 5 will integrate this with playback */
            }
        }
        if (args.requestPlayFilteredClip)
        {
            if (g_renderer && args.requestedPlayClipIndex >= 0)
            {
                /* Load and play the filtered clip by index */
                const char* clipPath = g_renderer->getFilteredClipsForActiveState(args.requestedPlayClipIndex);
                if (clipPath && clipPath[0])
                {
                    g_renderer->setJsonAnimationPath(clipPath);
                }
            }
        }
        
        /* ===== PHASE 5: Graph State Transition Requests ===== */
        if (args.requestTransitionToGraphState)
        {
            if (g_renderer && args.requestedGraphStateIndex >= 0)
            {
                /* Transition animation graph to the requested state */
                g_renderer->transitionGraphToState(args.requestedGraphStateIndex);
            }
        }
        
        /* ===== PHASE 6: Motion Matching Bridge Requests ===== */
        if (args.requestSetMotionMatchFiltered)
        {
            if (g_renderer)
            {
                bool enableFiltered = (args.requestedMotionMatchFiltered != 0);
                g_renderer->setMotionMatchUseFilteredClips(enableFiltered);
            }
        }
        
        if (args.requestGuessLuaModel)
        {
            GuessAndLoadModelForLuaTable();
        }
        if (args.requestPlayLuaClip)
        {
            PlayLuaAnimClipByName(args.requestedLuaClipName);
        }
        if (args.requestOpenLuaEditor)
        {
            if (args.requestedLuaEditorPath[0] != '\0')
            {
                LoadLuaEditorFromPath(args.requestedLuaEditorPath);
                BuildLuaDepTree(args.requestedLuaEditorPath);
            }
        }
        if (args.requestReloadLuaEditor)
        {
            if (!s_luaEditorTargetPath.empty())
            {
                LoadLuaEditorFromPath(s_luaEditorTargetPath.c_str());
                BuildLuaDepTree(s_luaEditorTargetPath.c_str());
            }
        }
        if (args.requestSaveLuaEditor)
        {
            SaveLuaEditorToTarget();
        }
        if (args.requestSetMotionMatchEnabled)
        {
            g_renderer->setMotionMatchEnabled(args.requestedMotionMatchEnabled != 0);
        }
        if (args.requestRebuildMotionMatchDb)
        {
            g_renderer->rebuildMotionMatchDatabase();
        }
        if (args.requestClearMotionMatchDb)
        {
            g_renderer->clearMotionMatchDatabase();
        }
        if (args.requestSetMotionMatchTargetVelocity)
        {
            g_renderer->setMotionMatchTargetVelocity(args.requestedMotionMatchTargetVelX,
                                                     args.requestedMotionMatchTargetVelZ);
        }
        if (args.requestSetMotionMatchTargetFacing)
        {
            g_renderer->setMotionMatchTargetFacing(args.requestedMotionMatchTargetFacingX,
                                                   args.requestedMotionMatchTargetFacingZ);
        }
        if (args.requestSetMotionMatchSearchInterval)
        {
            g_renderer->setMotionMatchSearchInterval(args.requestedMotionMatchSearchInterval);
        }
        if (args.requestSetMotionMatchBlendDuration)
        {
            g_renderer->setMotionMatchBlendDuration(args.requestedMotionMatchBlendDuration);
        }
    }
}

void RenderImGuiOverlayPass()
{
    if (g_imguiAvailable && fpImGui_Render)
    {
        fpImGui_Render();
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // DPI awareness MUST be declared BEFORE any window or DC is created on
    // this process. Doing it later (like the DLL's ImGui_Init did) is a
    // no-op because Windows has already decided the process is "unaware"
    // and locked in DPI virtualization for the lifetime of the proc. On a
    // 1.5x scaled laptop that means the entire viewport gets rendered at
    // 2/3 native res and then upscaled by GDI bilinear before it hits the
    // screen, which produced exactly the "muddy textures on the other
    // computer" report we spent three iterations chasing in the GPU stack.
    // It was never the GPU. It was Windows lying about the screen size.
    //
    // Dynamic lookup order: SetProcessDpiAwarenessContext (Win10 1703+,
    // the only one that handles per-monitor DPI for laptops with docking
    // changes), SetProcessDpiAwareness (Win8.1+, system DPI only), and
    // SetProcessDPIAware (Vista+, the dumb-but-works fallback). First one
    // that succeeds wins. Logged via DebugLog so we can confirm in the
    // next renderer.log that it actually took effect.
    {
        HMODULE user32 = ::GetModuleHandleA("user32.dll");
        HMODULE shcore = ::LoadLibraryA("shcore.dll");
        bool dpiOk = false;
        const char* dpiPath = "none";
        if (user32) {
            typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
            PFN_SetProcessDpiAwarenessContext fn = (PFN_SetProcessDpiAwarenessContext)
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (fn) {
                if (fn((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) {
                    dpiOk = true; dpiPath = "SetProcessDpiAwarenessContext(PerMonV2)";
                } else if (fn((HANDLE)-3 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE */)) {
                    dpiOk = true; dpiPath = "SetProcessDpiAwarenessContext(PerMon)";
                }
            }
        }
        if (!dpiOk && shcore) {
            typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
            PFN_SetProcessDpiAwareness fn = (PFN_SetProcessDpiAwareness)
                ::GetProcAddress(shcore, "SetProcessDpiAwareness");
            if (fn && SUCCEEDED(fn(2 /* PROCESS_PER_MONITOR_DPI_AWARE */))) {
                dpiOk = true; dpiPath = "SetProcessDpiAwareness(PerMonitor)";
            }
        }
        if (!dpiOk && user32) {
            typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
            PFN_SetProcessDPIAware fn = (PFN_SetProcessDPIAware)
                ::GetProcAddress(user32, "SetProcessDPIAware");
            if (fn && fn()) { dpiOk = true; dpiPath = "SetProcessDPIAware(System)"; }
        }
        char dbg[160];
        sprintf_s(dbg, sizeof(dbg), "[DPI] awareness set via %s (%s)",
                  dpiPath, dpiOk ? "OK" : "FAILED");
        DebugLog(dbg);
    }

    DebugLog("=== Starting ===");
    ParseAutoSpawnEffectFromCmdLine(lpCmdLine);
    {
        char msg[512];
        sprintf_s(msg, "[CmdLine] %s", (lpCmdLine && lpCmdLine[0]) ? lpCmdLine : "(empty)");
        DebugLog(msg);
        sprintf_s(msg, "[CmdLine] autospawn='%s'", (g_autoSpawnEffectName[0] ? g_autoSpawnEffectName : "(none)"));
        DebugLog(msg);
    }

    // Initialize performance counter
    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_lastFrameTime);

    LoadEditorState();
    ApplyTheme(g_darkTheme);

    // Auto-discover conquest_strings.txt using EXE-absolute path (immune to cwd changes)
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* lastSep = strrchr(exePath, '\\');
        if (!lastSep) lastSep = strrchr(exePath, '/');
        if (lastSep) *lastSep = '\0';

        // Candidates relative to EXE directory
        const char* relPaths[] = {
            "conquest_strings.txt",
            "..\\GameFiles\\lotrcparser\\CoreScripts\\lotrc-rust\\lotrc\\res\\conquest_strings.txt",
            "..\\GameFiles\\lotrc\\lotrc-0.6.0\\lotrc-0.6.0\\lotrc\\res\\conquest_strings.txt",
            NULL
        };
        for (int i = 0; relPaths[i]; ++i) {
            char fullPath[MAX_PATH];
            sprintf_s(fullPath, sizeof(fullPath), "%s\\%s", exePath, relPaths[i]);
            if (GetFileAttributesA(fullPath) != INVALID_FILE_ATTRIBUTES) {
                g_levelReader.SetBaseStringsPath(std::string(fullPath));
                break;
            }
        }
    }
    
    // Register window class
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    // Use UI background color for host window (render viewport paints itself).
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "ZeroEngine3DViewport";

    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, "Window Registration Failed!", "Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    // Render child window class
    wc.lpfnWndProc = RenderProc;
    wc.lpszClassName = "ZeroEngine3DRender";
    wc.hbrBackground = NULL;
    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, "Render window registration failed!", "Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    // Timeline HUD class
    wc.lpfnWndProc = TimelineProc;
    wc.lpszClassName = "ZeroEngineTimelineHUD";
    wc.hbrBackground = NULL;
    RegisterClassEx(&wc);
    
    // Create window
    int windowWidth = 1280;
    int windowHeight = 720;

    RECT rect = {0, 0, windowWidth, windowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowEx(
        0,
        "ZeroEngine3DViewport",
        "ZeroEngine 3D Viewport - Havok v5.5.0 Rendering System",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwnd) {
        MessageBox(NULL, "Window Creation Failed!", "Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // Init common controls (trackbar, etc.)
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // Create GUI dock
    CreateGui(g_hwnd);

    // Create render child window (viewport)
    CreateViewportWindow(g_hwnd);
    {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        LayoutGui(rc.right, rc.bottom);
    }
    
    // Initialize Havok
    DebugLog("Initializing Havok...");
    InitializeHavok();
    DebugLog("Havok initialized");

    // Initialize renderer
    DebugLog("Creating renderer...");
    g_renderer = new Scene3DRenderer();
    DebugLog("Calling renderer->initialize()...");
    int viewWidth = windowWidth;
    int viewHeight = windowHeight;
    if (g_viewportHwnd)
    {
        RECT rc;
        GetClientRect(g_viewportHwnd, &rc);
        viewWidth = (rc.right > 1) ? rc.right : windowWidth;
        viewHeight = (rc.bottom > 1) ? rc.bottom : windowHeight;
    }
    if (!g_renderer->initialize(g_viewportHwnd ? g_viewportHwnd : g_hwnd, viewWidth, viewHeight)) {
        DebugLog("ERROR: Renderer initialization failed!");
        MessageBox(NULL, "Renderer Initialization Failed!", "Error", MB_ICONERROR | MB_OK);
        return 1;
    }
    DebugLog("Renderer initialized successfully");
    if (LoadImGuiGlue() && fpImGui_Init)
    {
        if (fpImGui_Init(g_hwnd, g_renderer->getD3DDevice(), GetImGuiIniPath()))
        {
             g_imguiAvailable = true;
             g_imguiInitialized = true;
             DebugLog("ImGui glue initialized");
             // Default to the new ImGui docking UI (legacy Win32 panels can be toggled with F9).
             g_showLegacyUI = (g_persistShowLegacyUI >= 0) ? (g_persistShowLegacyUI != 0) : false;
             {
                 RECT rc;
                 GetClientRect(g_hwnd, &rc);
                 LayoutGui(rc.right, rc.bottom);
             }

            // The legacy Win32 timeline HUD is superseded by the ImGui "Timeline" window.
            // Destroy it to avoid duplicate UI and stray hit-testing when embedding the viewport.
            if (g_timelineHud)
            {
                DestroyWindow(g_timelineHud);
                g_timelineHud = NULL;
            }
        }
        else
        {
            DebugLog("ImGui glue init failed");
        }
    }
    // ========================================

    // Initialize Wwise audio subsystem.
    // Skip entirely when VESPUCCI_NO_AUDIO=1 — lets us test whether the
    // editor-vs-game Wwise conflict is what freezes the editor.
    char vespucciNoAudio[8] = {0};
    GetEnvironmentVariableA("VESPUCCI_NO_AUDIO", vespucciNoAudio, sizeof(vespucciNoAudio));
    bool skipAudio = (vespucciNoAudio[0] == '1');
    if (skipAudio) {
        DebugLog("Audio init SKIPPED (VESPUCCI_NO_AUDIO=1)");
    } else
    DebugLog("Initializing audio (Wwise SDK)...");
    if (!skipAudio)
    {
        // Bank paths - use absolute paths to avoid MAX_PATH issues with long relative paths
        wchar_t bankRootPath[512];  // Increased buffer size
        wchar_t bankLangPath[512];
        wchar_t exeDir[512];
        GetModuleFileNameW(NULL, exeDir, 512);
        wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';

        // Build path and resolve .. to shorten it
        wchar_t tempPath[512];
        _snwprintf_s(tempPath, 512, _TRUNCATE,
            L"%s..\\Engine\\source\\WwiseV28\\english_us_\\", exeDir);
        GetFullPathNameW(tempPath, 512, bankLangPath, NULL);  // Resolve .. and get absolute path

        // Use same path for both (all banks are now in english_us_ folder)
        wcscpy_s(bankRootPath, 512, bankLangPath);

        if (AudioManager::Get().Initialize(g_hwnd, bankRootPath, bankLangPath))
        {
            DebugLog("Audio initialized - loading ALL banks with GUI progress...");
            AudioManager::Get().LoadAllBanks();
            {
                char dbgBuf[128];
                sprintf_s(dbgBuf, "Audio banks loaded (%d total)",
                          AudioManager::Get().GetLoadedBankCount());
                DebugLog(dbgBuf);
            }

            // Test sound: fire a known event to verify the full audio pipeline
            // "swing" is a known event in BaseCombat bank (from WWiseIDTable)
            DebugLog("Audio test: posting 'swing' event...");
            AkPlayingID testPid = AudioManager::Get().PlayCue("swing",
                                      AudioManager::DEFAULT_GAME_OBJECT);
            {
                char dbgBuf[128];
                sprintf_s(dbgBuf, "Audio test result: pid=%u (0=FAILED)", testPid);
                DebugLog(dbgBuf);
            }
        }
        else
        {
            DebugLog("WARNING: Audio initialization failed - continuing without sound");
        }
    }

    //   We beg, plead, and threaten the window to please remain a polite child window.
   //   Havok has a long and proud history of silently yeeting your WS_CHILD flag into the trash
  //   when it feels like re-parenting your HWND for fun. We fight the good fight here.
    if (g_viewportHwnd)
    {
        SetParent(g_viewportHwnd, g_hwnd);
        LONG style = GetWindowLong(g_viewportHwnd, GWL_STYLE);
        style &= ~WS_POPUP;
        style |= WS_CHILD | WS_VISIBLE;
        SetWindowLong(g_viewportHwnd, GWL_STYLE, style);

        RECT rc;
        GetClientRect(g_hwnd, &rc);
        LayoutGui(rc.right, rc.bottom);
    }

    // Scan for useless Training level assets
    DebugLog("Scanning Training level assets...");
    g_assetBrowser = new AssetBrowser();

    //   Path assumes the executable is cosplaying as a nesting doll three folders deep inside Scene3D.
   //   Yes we are doing ../../.. like it's 2003 and relative paths are still a personality trait.
  //   Change one folder name and the whole castle of cards collapses. Living on the edge.
    int numAssets = g_assetBrowser->scanAssets("..\\..\\..\\Training");
    DebugLog("Asset scan complete");
    printf("\n");
    g_assetBrowser->printAssetList();
    printf("\n");

    // Scan GameFiles assets (models/animations/textures)
    DebugLog("Scanning GameFiles assets...");
    LoadGameFiles();

    // Do not auto-load any model or animation on startup — user selects explicitly.
    RefreshBoneList();
    ApplyPersistedRendererState();

    if (!g_autoSpawnEffectDone && g_autoSpawnEffectName[0] != '\0' && g_renderer)
    {
        g_autoSpawnEffectDone = true;
        char msg[256];
        sprintf_s(msg, "[AutoSpawn] Loading/spawning effect '%s'", g_autoSpawnEffectName);
        DebugLog(msg);

        char effectPath[512];
        sprintf_s(effectPath, "..\\GameFiles\\effects\\%s.json", g_autoSpawnEffectName);
        g_renderer->loadTestEffect(effectPath, "..\\GameFiles\\textures\\");
        hkVector4 spawnPos = g_renderer->getCameraTarget();
        g_renderer->spawnTestEffect(g_autoSpawnEffectName, spawnPos);
    }

    // Main loop
    MSG msg = {0};
    while (g_isRunning) {
        // Process messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_isRunning = false;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!g_isRunning) break;

        // Calculate delta time
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        float deltaTime = (float)(currentTime.QuadPart - g_lastFrameTime.QuadPart) / (float)g_perfFreq.QuadPart;
        g_lastFrameTime = currentTime;

        // Clamp delta time (prevent huge jumps)
        if (deltaTime > 0.1f) deltaTime = 0.1f;

        // Apply deferred resize once per frame (avoids double D3D9 Reset from
        // LayoutGui + RenderProc both responding to the same MoveWindow/WM_SIZE).
        if (g_resizePending && g_renderer)
        {
            g_resizePending = false;
            g_renderer->resize(g_pendingResizeW, g_pendingResizeH);
        }

        BeginImGuiFrame();
        DrawDockspaceAndPanels();

        // Update and render
        UpdateFrame(deltaTime);
        RenderFrame(deltaTime);

        // Update Wwise listener position from camera, then pump audio
        if (g_renderer && g_renderer->getCamera() && AudioManager::Get().IsInitialized())
        {
            float pos[3], dir[3], up[3];
            g_renderer->getCamera()->getFrom(pos);
            g_renderer->getCamera()->getDir(dir);
            g_renderer->getCamera()->getUp(up);
            AudioManager::Get().UpdateListenerPosition(
                pos[0], pos[1], pos[2],   // position
                dir[0], dir[1], dir[2],   // forward
                up[0],  up[1],  up[2]);   // top
        }
        AudioManager::Get().RenderAudio();

        // Update performance metrics
        UpdatePerformanceMetrics(deltaTime);

        // Cooperative yield without forcing a fixed 30 FPS step
        Sleep(0);
    }
    
    SaveEditorState();

    // Cleanup
    if (g_character) {
        delete g_character;
        g_character = NULL;
    }

    if (g_assetBrowser) {
        delete g_assetBrowser;
        g_assetBrowser = NULL;
    }

    if (g_gameFilesBrowser) {
        delete g_gameFilesBrowser;
        g_gameFilesBrowser = NULL;
    }

    if (g_imguiAvailable && fpImGui_Shutdown)
    {
        fpImGui_Shutdown();
        g_imguiInitialized = false;
    }
    if (g_imguiModule)
    {
        FreeLibrary(g_imguiModule);
        g_imguiModule = NULL;
        fpImGui_Invalidate = NULL;
        fpImGui_Create = NULL;
    }

    if (g_renderer) {
        g_renderer->shutdown();
        delete g_renderer;
        g_renderer = NULL;
    }

    // Shutdown audio before Havok (Wwise uses its own memory)
    AudioManager::Get().Shutdown();

    ShutdownHavok();

    return (int)msg.wParam;
}

//
// InitializeHavok - Initialize Havok SDK
// Based on: havok/hk550/Demo/Demos/DemoCommon/DemoFramework/Init/PlatformInitDefault.cpp
//

void InitializeHavok()
{
    // Initialize memory system (EXACT same pattern as all Havok v5.5.0 demos)
    hkPoolMemory* memoryManager = new hkPoolMemory();
    hkThreadMemory* threadMemory = new hkThreadMemory(memoryManager, 16);
    hkBaseSystem::init(memoryManager, threadMemory, HK_NULL, HK_NULL);
    memoryManager->removeReference();

    // Initialize stack area for fast temporary memory
    char* stackBuffer = hkAllocate<char>(0x100000, HK_MEMORY_CLASS_BASE);
    hkThreadMemory::getInstance().setStackArea(stackBuffer, 0x100000);

    // Initialize file system (required for hkLoader to work)
    // Based on: hkDemoFramework.cpp lines 1110-1111
    hkNativeFileSystem* fileSystem = new hkNativeFileSystem();
    hkFileSystem::replaceInstance(fileSystem);
}

void PreloadSkyboxes()
{
    if (!g_renderer || !g_gameFilesBrowser)
    {
        return;
    }

    g_renderer->clearSkyboxes();
    const std::vector<AssetBrowser::AssetInfo>& jmodels = g_gameFilesBrowser->getJModels();
    int loaded = 0;
    for (size_t i = 0; i < jmodels.size(); ++i)
    {
        const AssetBrowser::AssetInfo& jm = jmodels[i];
        const bool isSkyLike =
            ContainsNoCaseSimple(jm.name.c_str(), "skydome") ||
            ContainsNoCaseSimple(jm.name.c_str(), "sky") ||
            ContainsNoCaseSimple(jm.name.c_str(), "cloud");
        if (!isSkyLike)
        {
            continue;
        }

        const AssetBrowser::AssetInfo* glb = g_gameFilesBrowser->findAsset(jm.name.c_str(), "model");
        if (!glb)
        {
            continue;
        }

        if (g_renderer->preloadSkyboxModel(jm.name.c_str(), jm.path.c_str(), glb->path.c_str(), "..\\GameFiles\\textures\\"))
        {
            loaded++;
        }
    }

    char msg[256];
    sprintf(msg, "Skybox preload complete (%d loaded, active=%s)", loaded, g_renderer->getActiveSkyboxName());
    DebugLog(msg);
    if (g_textStatus)
    {
        SetWindowText(g_textStatus, msg);
    }
}

static int FindEffectIndexByName(const char* effectName)
{
    if (!effectName || effectName[0] == '\0')
    {
        return -1;
    }
    for (size_t i = 0; i < g_effectsList.size(); ++i)
    {
        if (_stricmp(g_effectsList[i].c_str(), effectName) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static bool SpawnEffectByIndex(int idx)
{
    if (!g_renderer)
    {
        return false;
    }
    if (idx < 0 || idx >= (int)g_effectsList.size())
    {
        return false;
    }

    g_browserIndexEffect = idx;
    std::string effectPath = "..\\GameFiles\\effects\\" + g_effectsList[idx] + ".json";
    g_renderer->loadTestEffect(effectPath.c_str(), "..\\GameFiles\\textures\\");
    hkVector4 spawnPos = g_renderer->getCameraTarget();
    g_renderer->spawnTestEffect(g_effectsList[idx].c_str(), spawnPos);
    return true;
}

static bool SpawnEffectByName(const char* effectName)
{
    return SpawnEffectByIndex(FindEffectIndexByName(effectName));
}

static bool SelectSkyboxByName(const char* skyboxName)
{
    if (!g_renderer || !skyboxName || skyboxName[0] == '\0')
    {
        return false;
    }

    if (g_renderer->selectSkyboxByName(skyboxName))
    {
        return true;
    }

    // Fallback: substring match (helps when names differ by suffix).
    int count = g_renderer->getSkyboxCount();
    for (int i = 0; i < count; ++i)
    {
        const char* name = g_renderer->getSkyboxName(i);
        if (ContainsNoCaseSimple(name, skyboxName))
        {
            g_renderer->setActiveSkyboxIndex(i);
            return true;
        }
    }
    return false;
}

static void ApplyEnvMashPreset(int mashId)
{
    if (!g_renderer)
    {
        return;
    }

    // Reset ambient FX so presets are deterministic.
    g_renderer->clearAllEffects();

    const char* label = "<unknown>";
    switch (mashId)
    {
        case 0:
            label = "Clear Env FX";
            break;

        case 1: // Shire sky + Mordor ash
            label = "Shire Sky + Mordor Ash";
            g_renderer->setSkyboxEnabled(true);
            g_renderer->setSkyRenderMode(Scene3DRenderer::SKY_BACKDROP);
            SelectSkyboxByName("SHR_Skydome_01");
            SpawnEffectByName("FX_MOR_Env_Ash_Camera");
            break;

        case 2: // Mordor sky + Shire birds
            label = "Mordor Sky + Shire Birds";
            g_renderer->setSkyboxEnabled(true);
            g_renderer->setSkyRenderMode(Scene3DRenderer::SKY_BACKDROP);
            SelectSkyboxByName("MOR_Skydome_01");
            SpawnEffectByName("FX_SHR_Env_Birds");
            break;

        case 3: // Minas sky + Moria fog + bats
            label = "Minas Sky + Moria Fog + Bats";
            g_renderer->setSkyboxEnabled(true);
            g_renderer->setSkyRenderMode(Scene3DRenderer::SKY_BACKDROP);
            SelectSkyboxByName("MST_Skydome_01");
            SpawnEffectByName("FX_MGL_PR_AmbientFog_01");
            SpawnEffectByName("FX_MGL_Env_Bats");
            break;

        case 4: // Overcast clouds + Helm rain
            label = "Overcast Clouds + Helm Rain";
            g_renderer->setSkyboxEnabled(true);
            g_renderer->setSkyRenderMode(Scene3DRenderer::SKY_BACKDROP);
            SelectSkyboxByName("OSG_OvercastClouds_01");
            SpawnEffectByName("FX_HMD_PR_Rain_Camera");
            break;

        case 5: // Isengard sky + leaves + embers
            label = "Isengard Sky + Leaves + Embers";
            g_renderer->setSkyboxEnabled(true);
            g_renderer->setSkyRenderMode(Scene3DRenderer::SKY_BACKDROP);
            SelectSkyboxByName("IGD_Skydome_01");
            SpawnEffectByName("FX_MGL_Env_Leaves_Camera");
            SpawnEffectByName("FX_SHR_env_ember");
            break;

        default:
            label = "Unknown preset";
            break;
    }

    if (g_textStatus)
    {
        char msg[256];
        sprintf(msg, "Env Mash: %s", label);
        SetWindowText(g_textStatus, msg);
    }
}

void LoadGameFiles()
{
    if (!g_gameFilesBrowser)
    {
        g_gameFilesBrowser = new AssetBrowser();
    }

    int count = g_gameFilesBrowser->scanGameFiles("..\\GameFiles");
    char msg[128];
    sprintf(msg, "GameFiles scan complete (assets=%d)", count);
    DebugLog(msg);
    if (g_textStatus)
    {
        SetWindowText(g_textStatus, msg);
    }

    RefreshGameFilesUI();
    RefreshEffectsList();
    RefreshGraphList();
    RefreshLuaScriptList();
    RefreshLuaStateScriptList();
    RefreshLuaSrcFileList();
    ResolvePersistedSelections();
    PreloadSkyboxes();

    const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();
    if (!anims.empty())
    {
        if (g_gameAnimIndex < 0 || g_gameAnimIndex >= (int)anims.size())
        {
            g_gameAnimIndex = 0;
        }
        SelectGameAnimation(g_gameAnimIndex);
    }
}

void SelectGameAnimation(int index)
{
    if (!g_renderer || !g_gameFilesBrowser)
    {
        return;
    }
    const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();
    if (anims.empty())
    {
        return;
    }
    int count = (int)anims.size();
    if (count <= 0) return;

    // Remember the user's requested animation index even if it's not currently compatible.
    // This allows us to automatically retry after the skeleton/model changes.
    int requested = index;
    if (requested < 0) requested = 0;
    if (requested >= count) requested = count - 1;
    g_gameAnimIndex = requested;

    int tries = 0;
    int idx = requested;
    while (tries < count)
    {
        if (idx < 0) idx = count - 1;
        if (idx >= count) idx = 0;

        const AssetBrowser::AssetInfo& anim = anims[idx];
        if (g_renderer->setJsonAnimationPath(anim.path.c_str()))
        {
            g_gameAnimIndex = idx;
            g_browserIndexAnim = g_gameAnimIndex;
            char msg[512];
            sprintf(msg, "Selected JSON animation: %s", anim.name.c_str());
            DebugLog(msg);
            strcpy_s(g_activeAnimName, anim.name.c_str());
            if (g_textStatus)
            {
                SetWindowText(g_textStatus, msg);
            }
            SyncGuiSelectionFromState();
            UpdateGuiPlayback();
            return;
        }
        idx++;
        tries++;
    }

    DebugLog("No compatible JSON animation found for current skeleton.");
    g_activeAnimName[0] = '\0';
    if (g_textAnim)
    {
        SetWindowText(g_textAnim, "Anim: <none>");
    }
    if (g_textStatus)
    {
        SetWindowText(g_textStatus, "No compatible JSON animation found for current skeleton.");
    }
}

void SelectBlendAnimation(int index)
{
    if (!g_renderer || !g_gameFilesBrowser)
    {
        return;
    }

    const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();
    if (anims.empty())
    {
        return;
    }

    if (index < 0)
    {
        g_blendAnimIndex = -1;
        g_renderer->clearJsonBlendAnimation();
        return;
    }

    if (index >= (int)anims.size())
    {
        return;
    }

    const AssetBrowser::AssetInfo& anim = anims[index];
    if (g_renderer->setJsonBlendAnimationPath(anim.path.c_str()))
    {
        g_blendAnimIndex = index;
        char msg[512];
        sprintf(msg, "Blend JSON animation: %s", anim.name.c_str());
        DebugLog(msg);
    }
    else
    {
        DebugLog("Blend JSON animation rejected (incompatible rig/clip).");
    }
}

void SelectGameModel(int index)
{
    if (!g_renderer || !g_gameFilesBrowser)
    {
        return;
    }
    const std::vector<AssetBrowser::AssetInfo>& jmodels = g_gameFilesBrowser->getJModels();
    if (jmodels.empty())
    {
        return;
    }
    int count = (int)jmodels.size();
    if (index < 0) index = count - 1;
    if (index >= count) index = 0;
    g_browserIndexModel = index;

    const AssetBrowser::AssetInfo& jm = jmodels[g_browserIndexModel];
    const AssetBrowser::AssetInfo* glb = g_gameFilesBrowser->findAsset(jm.name.c_str(), "model");
    if (!glb)
    {
        char msg[512];
        sprintf(msg, "Model GLB not found for jmodel: %s", jm.name.c_str());
        DebugLog(msg);
        return;
    }

    if (g_renderer->loadGameModel(jm.path.c_str(), glb->path.c_str(), "..\\GameFiles\\textures\\"))
    {
        char msg[512];
        sprintf(msg, "Selected model: %s", jm.name.c_str());
        DebugLog(msg);
        strcpy_s(g_activeModelName, jm.name.c_str());
        if (g_textStatus)
        {
            SetWindowText(g_textStatus, msg);
        }
        RefreshBoneList();
        SyncGuiSelectionFromState();

        // If the user already chose an animation (even if it previously failed due to
        // skeleton incompatibility), retry now that the model/skeleton has changed.
        if (g_gameAnimIndex >= 0)
        {
            SelectGameAnimation(g_gameAnimIndex);
        }
        if (g_blendAnimIndex >= 0)
        {
            SelectBlendAnimation(g_blendAnimIndex);
        }

        UpdateGuiPlayback();
    }
}

static void ApplyGuiFont(HWND hwnd)
{
    if (!hwnd) return;
    if (!g_uiFont)
    {
        g_uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    SendMessage(hwnd, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
}

static void FillVerticalGradient(HDC hdc, const RECT& rc, COLORREF top, COLORREF bottom)
{
    TRIVERTEX verts[2];
    verts[0].x = rc.left;
    verts[0].y = rc.top;
    verts[0].Red   = (GetRValue(top)   << 8);
    verts[0].Green = (GetGValue(top)   << 8);
    verts[0].Blue  = (GetBValue(top)   << 8);
    verts[0].Alpha = 0xff00;

    verts[1].x = rc.right;
    verts[1].y = rc.bottom;
    verts[1].Red   = (GetRValue(bottom)   << 8);
    verts[1].Green = (GetGValue(bottom)   << 8);
    verts[1].Blue  = (GetBValue(bottom)   << 8);
    verts[1].Alpha = 0xff00;

    GRADIENT_RECT gRect = {0, 1};
    GradientFill(hdc, verts, 2, &gRect, 1, GRADIENT_FILL_RECT_V);
}

static void ApplyTheme(bool dark)
{
    g_darkTheme = dark;
    if (dark)
    {
        kClrBgTop    = RGB(0x1b, 0x1e, 0x24);
        kClrBgBot    = RGB(0x16, 0x18, 0x1d);
        kClrPanelTop = RGB(0x21, 0x24, 0x2c);
        kClrPanelBot = RGB(0x1c, 0x20, 0x27);
        kClrPanel    = kClrPanelTop;
        kClrBg       = kClrBgTop;
        kClrText     = RGB(0xe6, 0xea, 0xf0);
        kClrTextDim  = RGB(0x9a, 0xa5, 0xb5);
        kClrAccent   = RGB(0x4c, 0xd0, 0xff);
    }
    else
    {
        kClrBgTop    = RGB(0xf2, 0xf4, 0xf7);
        kClrBgBot    = RGB(0xe7, 0xea, 0xef);
        kClrPanelTop = RGB(0xf8, 0xf9, 0xfb);
        kClrPanelBot = RGB(0xee, 0xf0, 0xf5);
        kClrPanel    = kClrPanelTop;
        kClrBg       = kClrBgTop;
        kClrText     = RGB(0x16, 0x1a, 0x20);
        kClrTextDim  = RGB(0x56, 0x60, 0x70);
        kClrAccent   = RGB(0x1a, 0x73, 0xe8);
    }
    if (g_brBg) { DeleteObject(g_brBg); g_brBg = NULL; }
    if (g_brPanel) { DeleteObject(g_brPanel); g_brPanel = NULL; }
    g_brBg = CreateSolidBrush(kClrBgTop);
    g_brPanel = CreateSolidBrush(kClrPanelTop);

    // Refresh themed child controls immediately (e.g., tree view colors).
    if (g_listBones)
    {
        TreeView_SetBkColor(g_listBones, kClrPanel);
        TreeView_SetTextColor(g_listBones, kClrText);
    }
}

static void InvalidateAllTheme()
{
    if (g_hwnd)
    {
        RedrawWindow(g_hwnd, NULL, NULL,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    if (g_viewportHwnd) InvalidateRect(g_viewportHwnd, NULL, TRUE);
    if (g_timelineHud) InvalidateRect(g_timelineHud, NULL, TRUE);
    if (g_listModels) InvalidateRect(g_listModels, NULL, TRUE);
    if (g_listAnims) InvalidateRect(g_listAnims, NULL, TRUE);
    if (g_listMaterials) InvalidateRect(g_listMaterials, NULL, TRUE);
    if (g_listBones) InvalidateRect(g_listBones, NULL, TRUE);
}

void CreateGui(HWND hwnd)
{
    if (g_guiCreated || !hwnd)
    {
        return;
    }

    g_textStatus = CreateWindowEx(0, "STATIC", "Status: Ready",
                                  WS_CHILD | WS_VISIBLE,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_STATIC_STATUS, NULL, NULL);
    g_textModel = CreateWindowEx(0, "STATIC", "Model: <none>",
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_STATIC_MODEL, NULL, NULL);
    g_textAnim = CreateWindowEx(0, "STATIC", "Anim: <none>",
                                WS_CHILD | WS_VISIBLE,
                                0, 0, 10, 10, hwnd, (HMENU)IDC_STATIC_ANIM, NULL, NULL);
    g_textMat = CreateWindowEx(0, "STATIC", "Mat: <none>",
                                WS_CHILD | WS_VISIBLE,
                                0, 0, 10, 10, hwnd, (HMENU)IDC_STATIC_MAT, NULL, NULL);
    g_textTex = CreateWindowEx(0, "STATIC", "Tex: <none>",
                                WS_CHILD | WS_VISIBLE,
                                0, 0, 10, 10, hwnd, (HMENU)IDC_STATIC_TEX, NULL, NULL);
    g_textWarn = CreateWindowEx(0, "STATIC", "",
                                 WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_STATIC_WARN, NULL, NULL);
    g_btnToggleLeft = CreateWindowEx(0, "BUTTON", "<<",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_TOGGLE_LEFT, NULL, NULL);
    g_btnToggleRight = CreateWindowEx(0, "BUTTON", ">>",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_TOGGLE_RIGHT, NULL, NULL);
    g_btnToggleTimeline = CreateWindowEx(0, "BUTTON", "__",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_TOGGLE_TL, NULL, NULL);
    g_listMaterials = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_LIST_MATERIALS, NULL, NULL);

    g_labelEffects = CreateWindowEx(0, "STATIC", "Particle Effects",
                                    WS_CHILD | WS_VISIBLE,
                                    0, 0, 10, 10, hwnd, NULL, NULL, NULL);
    g_listEffects = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
                                   0, 0, 10, 10, hwnd, (HMENU)IDC_LIST_EFFECTS, NULL, NULL);
    g_btnSpawnEffect = CreateWindowEx(0, "BUTTON", "Spawn Effect",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_SPAWN_EFFECT, NULL, NULL);

    g_labelModels = CreateWindowEx(0, "STATIC", "Models",
                                   WS_CHILD | WS_VISIBLE,
                                   0, 0, 10, 10, hwnd, NULL, NULL, NULL);
    g_editModelFilter = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                       0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_MODEL_FILTER, NULL, NULL);
    g_listModels = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_LIST_MODELS, NULL, NULL);
    g_btnLoadModel = CreateWindowEx(0, "BUTTON", "Load Model",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_LOAD_MODEL, NULL, NULL);

    g_labelAnims = CreateWindowEx(0, "STATIC", "Animations",
                                  WS_CHILD | WS_VISIBLE,
                                  0, 0, 10, 10, hwnd, NULL, NULL, NULL);
    g_editAnimFilter = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_ANIM_FILTER, NULL, NULL);
    g_listAnims = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_LIST_ANIMS, NULL, NULL);
    g_btnLoadAnim = CreateWindowEx(0, "BUTTON", "Load Anim",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_LOAD_ANIM, NULL, NULL);
    g_btnRescan = CreateWindowEx(0, "BUTTON", "Rescan",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_RESCAN, NULL, NULL);
    g_btnPresetSetA = CreateWindowEx(0, "BUTTON", "Set A",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_PRESET_SET_A, NULL, NULL);
    g_btnPresetLoadA = CreateWindowEx(0, "BUTTON", "Load A",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_PRESET_LOAD_A, NULL, NULL);
    g_btnPresetSetB = CreateWindowEx(0, "BUTTON", "Set B",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_PRESET_SET_B, NULL, NULL);
    g_btnPresetLoadB = CreateWindowEx(0, "BUTTON", "Load B",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_PRESET_LOAD_B, NULL, NULL);

    g_labelBones = CreateWindowEx(0, "STATIC", "Skeleton",
                                  WS_CHILD | WS_VISIBLE,
                                  0, 0, 10, 10, hwnd, NULL, NULL, NULL);
    g_listBones = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, "",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
                                 TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_LIST_BONES, NULL, NULL);
    TreeView_SetBkColor(g_listBones, kClrPanel);
    TreeView_SetTextColor(g_listBones, kClrText);
    g_checkLoop = CreateWindowEx(0, "BUTTON", "Loop",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_CHECK_LOOP, NULL, NULL);
    g_sliderSpeed = CreateWindowEx(0, TRACKBAR_CLASS, "",
                                   WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                                   0, 0, 10, 10, hwnd, (HMENU)IDC_SLIDER_SPEED, NULL, NULL);
    g_textSpeed = CreateWindowEx(0, "STATIC", "Speed: 1.00x",
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_STATIC_SPEED, NULL, NULL);

    g_btnPrev = CreateWindowEx(0, "BUTTON", "<<",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_PREV, NULL, NULL);
    g_btnPlay = CreateWindowEx(0, "BUTTON", "Play",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_PLAY, NULL, NULL);
    g_btnStop = CreateWindowEx(0, "BUTTON", "Stop",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_STOP, NULL, NULL);
    g_btnFramePrev = CreateWindowEx(0, "BUTTON", "<F",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_FRAME_PREV, NULL, NULL);
    g_btnFrameNext = CreateWindowEx(0, "BUTTON", "F>",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_FRAME_NEXT, NULL, NULL);
    g_btnFirstKey = CreateWindowEx(0, "BUTTON", "|<",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_FIRST_KEY, NULL, NULL);
    g_btnLastKey = CreateWindowEx(0, "BUTTON", ">|",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_LAST_KEY, NULL, NULL);
    g_btnRecord = CreateWindowEx(0, "BUTTON", "AutoKey",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_RECORD, NULL, NULL);
    g_btnSetKey = CreateWindowEx(0, "BUTTON", "Set Key",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_SET_KEY, NULL, NULL);
    g_btnNext = CreateWindowEx(0, "BUTTON", ">>",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_NEXT, NULL, NULL);
    g_trackTime = CreateWindowEx(0, TRACKBAR_CLASS, "",
                                 WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_TRACK_TIME, NULL, NULL);
    g_textTime = CreateWindowEx(0, "STATIC", "0.00 / 0.00s",
                                WS_CHILD | WS_VISIBLE,
                                0, 0, 10, 10, hwnd, (HMENU)IDC_STATIC_TIME, NULL, NULL);

    g_editExportName = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_exportName,
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_EXPORT, NULL, NULL);
    g_btnExport = CreateWindowEx(0, "BUTTON", "Export",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_EXPORT, NULL, NULL);
    g_editTimelineSec = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_timelineSec,
                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                       0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_TIMELINE_SEC, NULL, NULL);
    g_btnTimelineSet = CreateWindowEx(0, "BUTTON", "Set Len(s)",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_TIMELINE_SET, NULL, NULL);
    g_checkRotSnap = CreateWindowEx(0, "BUTTON", "RotSnap",
                                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_CHECK_ROT_SNAP, NULL, NULL);
    g_editRotSnap = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_rotSnapDegText,
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_ROT_SNAP, NULL, NULL);
    g_checkMoveSnap = CreateWindowEx(0, "BUTTON", "MoveSnap",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_CHECK_MOVE_SNAP, NULL, NULL);
    g_editMoveSnap = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_moveSnapText,
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_MOVE_SNAP, NULL, NULL);
    g_checkInterpLin = CreateWindowEx(0, "BUTTON", "Interp Linear",
                                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_CHECK_INTERP_LIN, NULL, NULL);
    g_btnEditCommit = CreateWindowEx(0, "BUTTON", "Commit",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_EDIT_COMMIT, NULL, NULL);
    g_btnEditCancel = CreateWindowEx(0, "BUTTON", "Cancel",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_EDIT_CANCEL, NULL, NULL);
    g_editNumRx = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_numRxText,
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_NUM_RX, NULL, NULL);
    g_editNumRy = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_numRyText,
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_NUM_RY, NULL, NULL);
    g_editNumRz = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_numRzText,
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_NUM_RZ, NULL, NULL);
    g_editNumTx = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_numTxText,
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_NUM_TX, NULL, NULL);
    g_editNumTy = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_numTyText,
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_NUM_TY, NULL, NULL);
    g_editNumTz = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", g_numTzText,
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_NUM_TZ, NULL, NULL);
    g_btnNumRead = CreateWindowEx(0, "BUTTON", "Read",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_NUM_READ, NULL, NULL);
    g_btnNumKey = CreateWindowEx(0, "BUTTON", "Key TRS",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_NUM_KEY, NULL, NULL);

    ApplyGuiFont(g_textStatus);
    ApplyGuiFont(g_textModel);
    ApplyGuiFont(g_textAnim);
    ApplyGuiFont(g_labelModels);
    ApplyGuiFont(g_editModelFilter);
    ApplyGuiFont(g_listModels);
    ApplyGuiFont(g_btnLoadModel);
    ApplyGuiFont(g_labelAnims);
    ApplyGuiFont(g_editAnimFilter);
    ApplyGuiFont(g_listAnims);
    ApplyGuiFont(g_btnLoadAnim);
    ApplyGuiFont(g_btnRescan);
    ApplyGuiFont(g_btnPresetSetA);
    ApplyGuiFont(g_btnPresetLoadA);
    ApplyGuiFont(g_btnPresetSetB);
    ApplyGuiFont(g_btnPresetLoadB);
    ApplyGuiFont(g_labelBones);
    ApplyGuiFont(g_listBones);
    ApplyGuiFont(g_checkLoop);
    ApplyGuiFont(g_sliderSpeed);
    ApplyGuiFont(g_textSpeed);
    ApplyGuiFont(g_btnPrev);
    ApplyGuiFont(g_btnPlay);
    ApplyGuiFont(g_btnStop);
    ApplyGuiFont(g_btnFramePrev);
    ApplyGuiFont(g_btnFrameNext);
    ApplyGuiFont(g_btnFirstKey);
    ApplyGuiFont(g_btnLastKey);
    ApplyGuiFont(g_btnRecord);
    ApplyGuiFont(g_btnSetKey);
    ApplyGuiFont(g_btnNext);
    ApplyGuiFont(g_trackTime);
    ApplyGuiFont(g_textTime);
    ApplyGuiFont(g_editExportName);
    ApplyGuiFont(g_btnExport);
    ApplyGuiFont(g_editTimelineSec);
    ApplyGuiFont(g_btnTimelineSet);
    ApplyGuiFont(g_checkRotSnap);
    ApplyGuiFont(g_editRotSnap);
    ApplyGuiFont(g_checkMoveSnap);
    ApplyGuiFont(g_editMoveSnap);
    ApplyGuiFont(g_checkInterpLin);
    ApplyGuiFont(g_btnEditCommit);
    ApplyGuiFont(g_btnEditCancel);
    ApplyGuiFont(g_editNumRx);
    ApplyGuiFont(g_editNumRy);
    ApplyGuiFont(g_editNumRz);
    ApplyGuiFont(g_editNumTx);
    ApplyGuiFont(g_editNumTy);
    ApplyGuiFont(g_editNumTz);
    ApplyGuiFont(g_btnNumRead);
    ApplyGuiFont(g_btnNumKey);
    ApplyGuiFont(g_textMat);
    ApplyGuiFont(g_textTex);
    ApplyGuiFont(g_textWarn);
    ApplyGuiFont(g_listMaterials);
    ApplyGuiFont(g_btnToggleLeft);
    ApplyGuiFont(g_btnToggleRight);
    ApplyGuiFont(g_btnToggleTimeline);

    SendMessage(g_trackTime, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
    SendMessage(g_trackTime, TBM_SETPOS, TRUE, 0);
    SendMessage(g_trackTime, TBM_SETTICFREQ, 50, 0); // ticks every 5% for quick seek
    SendMessage(g_trackTime, TBM_SETPAGESIZE, 0, 50);

    SendMessage(g_sliderSpeed, TBM_SETRANGE, TRUE, MAKELONG(25, 200));
    SendMessage(g_sliderSpeed, TBM_SETPOS, TRUE, 100);
    SendMessage(g_checkLoop, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(g_checkRotSnap, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(g_checkMoveSnap, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(g_checkInterpLin, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessage(g_btnRecord, BM_SETCHECK, BST_UNCHECKED, 0);

    RECT rc;
    GetClientRect(hwnd, &rc);
    g_guiCreated = true;
    LayoutGui(rc.right, rc.bottom);
}

void LayoutGui(int width, int height)
{
    if (!g_guiCreated)
    {
        return;
    }

    // ImGui-first layout: hide all legacy Win32 panels and let the 3D viewport render behind ImGui.
    if (g_imguiAvailable && !g_showLegacyUI)
    {
        SetLeftPanelVisible(false);
        SetRightPanelVisible(false);
        if (g_textStatus) ShowWindow(g_textStatus, SW_HIDE);
        if (g_btnToggleLeft) ShowWindow(g_btnToggleLeft, SW_HIDE);
        if (g_btnToggleRight) ShowWindow(g_btnToggleRight, SW_HIDE);
        if (g_btnToggleTimeline) ShowWindow(g_btnToggleTimeline, SW_HIDE);
        if (g_timelineHud) ShowWindow(g_timelineHud, SW_HIDE);
        if (g_labelEffects) ShowWindow(g_labelEffects, SW_HIDE);
        if (g_listEffects) ShowWindow(g_listEffects, SW_HIDE);
        if (g_btnSpawnEffect) ShowWindow(g_btnSpawnEffect, SW_HIDE);

        if (g_viewportHwnd)
        {
            MoveWindow(g_viewportHwnd, 0, 0, width, height, TRUE);
            // Defer resize to once-per-frame to avoid double D3D9 Reset
            // (MoveWindow already triggers WM_SIZE in RenderProc)
            g_resizePending = true;
            g_pendingResizeW = width;
            g_pendingResizeH = height;
        }
        return;
    }

    const int margin = 6;
    const int colGap = 8;
    const int labelH = 16;
    const int editH = 20;
    const int buttonH = 22;
    const int statusH = 18;
    const int minCol = 350;  // Increased from 200 to fit all playback buttons
    const int minView = 320;

    // Column widths with collapsible panels
    int leftW  = g_leftCollapsed  ? 0 : (int)(width * 0.18f);
    int rightW = g_rightCollapsed ? 0 : (int)(width * 0.20f);
    if (!g_leftCollapsed  && leftW  < minCol) leftW  = minCol;
    if (!g_rightCollapsed && rightW < minCol) rightW = minCol;

    int gapL = g_leftCollapsed ? 0 : colGap;
    int gapR = g_rightCollapsed ? 0 : colGap;
    int centerW = width - margin * 2 - gapL - gapR - leftW - rightW;
    if (centerW < minView)
    {
        int deficit = minView - centerW;
        int adjust = (deficit + 1) / 2;
        if (!g_leftCollapsed)  leftW  = (leftW  - adjust < minCol) ? minCol : (leftW  - adjust);
        if (!g_rightCollapsed) rightW = (rightW - adjust < minCol) ? minCol : (rightW - adjust);
        centerW = width - margin * 2 - gapL - gapR - leftW - rightW;
        if (centerW < minView) centerW = minView;
    }

    // Toggle buttons and status at top
    int toggleW = 22;
    int yTop = margin;
    if (g_textStatus) ShowWindow(g_textStatus, SW_SHOW);
    if (g_btnToggleLeft) ShowWindow(g_btnToggleLeft, SW_SHOW);
    if (g_btnToggleRight) ShowWindow(g_btnToggleRight, SW_SHOW);
    if (g_btnToggleTimeline) ShowWindow(g_btnToggleTimeline, SW_SHOW);
    MoveWindow(g_btnToggleLeft, margin, yTop, toggleW, statusH, TRUE);
    MoveWindow(g_btnToggleRight, width - margin - toggleW, yTop, toggleW, statusH, TRUE);
    MoveWindow(g_textStatus, margin + toggleW + 4, yTop, width - margin * 2 - toggleW * 2 - 8, statusH, TRUE);
    yTop += statusH + colGap;

    int colHeight = height - yTop - margin;

    // Apply visibility toggles
    SetLeftPanelVisible(!g_leftCollapsed);
    SetRightPanelVisible(!g_rightCollapsed);

    // Left column (assets) or collapsed
    int leftX = margin;
    int leftInner = (leftW > 0) ? (leftW - colGap * 2) : 0;
    int y = yTop;
    if (!g_leftCollapsed)
    {
        MoveWindow(g_labelModels, leftX + colGap, y, leftInner, labelH, TRUE); y += labelH + 2;
        MoveWindow(g_editModelFilter, leftX + colGap, y, leftInner, editH, TRUE); y += editH + 4;
        int modelListH = (int)(colHeight * 0.38f);
        if (modelListH < 80) modelListH = 80;
        MoveWindow(g_listModels, leftX + colGap, y, leftInner, modelListH, TRUE); y += modelListH + 4;
        MoveWindow(g_btnLoadModel, leftX + colGap, y, leftInner, buttonH, TRUE); y += buttonH + colGap;

        MoveWindow(g_labelAnims, leftX + colGap, y, leftInner, labelH, TRUE); y += labelH + 2;
        MoveWindow(g_editAnimFilter, leftX + colGap, y, leftInner, editH, TRUE); y += editH + 4;
        int animListH = (int)(colHeight * 0.34f);
        if (animListH < 80) animListH = 80;
        MoveWindow(g_listAnims, leftX + colGap, y, leftInner, animListH, TRUE); y += animListH + 4;
        int halfW = (leftInner - colGap) / 2;
        MoveWindow(g_btnLoadAnim, leftX + colGap, y, halfW, buttonH, TRUE);
        MoveWindow(g_btnRescan,    leftX + colGap + halfW + colGap, y, leftInner - halfW - colGap, buttonH, TRUE);
        y += buttonH + 4;
        int quarterW = (leftInner - colGap * 3) / 4;
        MoveWindow(g_btnPresetSetA,  leftX + colGap, y, quarterW, buttonH, TRUE);
        MoveWindow(g_btnPresetLoadA, leftX + colGap + quarterW + colGap, y, quarterW, buttonH, TRUE);
        MoveWindow(g_btnPresetSetB,  leftX + colGap + (quarterW + colGap) * 2, y, quarterW, buttonH, TRUE);
        MoveWindow(g_btnPresetLoadB, leftX + colGap + (quarterW + colGap) * 3, y, quarterW, buttonH, TRUE);
        y += buttonH + colGap;

        // Material quick info (single line each)
        MoveWindow(g_textMat, leftX + colGap, y, leftInner, labelH, TRUE); y += labelH + 2;
        MoveWindow(g_textTex, leftX + colGap, y, leftInner, labelH, TRUE); y += labelH + 2;
        MoveWindow(g_textWarn, leftX + colGap, y, leftInner, labelH, TRUE); y += labelH + 2;
        ShowWindow(g_listMaterials, SW_HIDE); // keep list hidden for compact layout
    }

    // Center column (viewport) and bottom timeline
    int centerX = leftX + leftW + gapL;
    int timelineH = g_timelineCollapsed ? 24 : 120;
    int viewH = colHeight - timelineH - colGap;
    if (viewH < minView) viewH = minView;
    if (g_viewportHwnd)
    {
        MoveWindow(g_viewportHwnd, centerX, yTop, centerW, viewH, TRUE);
        // Defer resize to once-per-frame (MoveWindow triggers WM_SIZE in RenderProc too)
        if (!g_isResizing)
        {
            g_resizePending = true;
            g_pendingResizeW = centerW;
            g_pendingResizeH = viewH;
        }
        if (g_timelineHud)
        {
            MoveWindow(g_timelineHud, margin, yTop + viewH + colGap, width - margin * 2, timelineH, TRUE);
            ShowWindow(g_timelineHud, g_timelineCollapsed ? SW_HIDE : SW_SHOW);
            BringWindowToTop(g_timelineHud);
            // place collapse toggle near left of timeline
            MoveWindow(g_btnToggleTimeline, margin, yTop + viewH + colGap, toggleW, statusH, TRUE);
        }

        // Particle Effects panel in timeline area (bottom panel)
        int timelineY = yTop + viewH + colGap;
        int effectsX = margin + toggleW + 10; // Start after collapse button
        int effectsY = timelineY + 4; // Small padding from top
        int effectsAvailW = width - margin * 2 - toggleW - 20; // Available width

        if (!g_timelineCollapsed && effectsAvailW > 300)
        {
            // Layout: [Label] [Listbox] [Button] - all horizontal
            int labelW = 90;
            int buttonW = 90;
            int listW = effectsAvailW - labelW - buttonW - 20; // Remaining space for list
            if (listW < 150) listW = 150;

            int effectsH = timelineH - 8; // Use most of timeline height
            if (effectsH > 100) effectsH = 100;

            MoveWindow(g_labelEffects, effectsX, effectsY, labelW, labelH, TRUE);
            MoveWindow(g_listEffects, effectsX + labelW + 4, effectsY, listW, effectsH, TRUE);
            MoveWindow(g_btnSpawnEffect, effectsX + labelW + listW + 8, effectsY, buttonW, buttonH, TRUE);

            ShowWindow(g_labelEffects, SW_SHOW);
            ShowWindow(g_listEffects, SW_SHOW);
            ShowWindow(g_btnSpawnEffect, SW_SHOW);
        }
        else
        {
            // Hide effects panel when timeline is collapsed or not enough space
            ShowWindow(g_labelEffects, SW_HIDE);
            ShowWindow(g_listEffects, SW_HIDE);
            ShowWindow(g_btnSpawnEffect, SW_HIDE);
        }
    }

    // Right column (playback + editing) unless collapsed
    int rightX = centerX + centerW + gapR;
    int rightInner = (rightW > 0) ? (rightW - colGap * 2) : 0;
    y = yTop;
    if (!g_rightCollapsed)
    {
        // Model/Anim labels on top
        MoveWindow(g_textModel, rightX + colGap, y, rightInner, labelH, TRUE); y += labelH + 2;
        MoveWindow(g_textAnim,  rightX + colGap, y, rightInner, labelH, TRUE); y += labelH + colGap;

        // Playback row 1: << < Play Stop > >>
        int smallW = 40, midW = 52;
        int xBtn = rightX + colGap;
        MoveWindow(g_btnFirstKey,  xBtn, y, smallW, buttonH, TRUE); xBtn += smallW + colGap;
        MoveWindow(g_btnFramePrev, xBtn, y, smallW, buttonH, TRUE); xBtn += smallW + colGap;
        MoveWindow(g_btnPlay,      xBtn, y, midW, buttonH, TRUE);   xBtn += midW + colGap;
        MoveWindow(g_btnStop,      xBtn, y, midW, buttonH, TRUE);   xBtn += midW + colGap;
        MoveWindow(g_btnFrameNext, xBtn, y, smallW, buttonH, TRUE); xBtn += smallW + colGap;
        MoveWindow(g_btnLastKey,   xBtn, y, smallW, buttonH, TRUE);
        y += buttonH + 4;

        // Playback row 2: record/setkey/prev/next/loop/speed label
        xBtn = rightX + colGap;
        int longW = 68;
        MoveWindow(g_btnRecord, xBtn, y, longW, buttonH, TRUE); xBtn += longW + colGap;
        MoveWindow(g_btnSetKey, xBtn, y, longW, buttonH, TRUE); xBtn += longW + colGap;
        MoveWindow(g_btnPrev,   xBtn, y, smallW, buttonH, TRUE); xBtn += smallW + colGap;
        MoveWindow(g_btnNext,   xBtn, y, smallW, buttonH, TRUE); xBtn += smallW + colGap;
        MoveWindow(g_checkLoop, xBtn, y, 60, buttonH, TRUE);    xBtn += 60 + colGap;
        MoveWindow(g_textSpeed, xBtn, y, rightX + rightW - colGap - xBtn, buttonH, TRUE);
        y += buttonH + 4;

        // Time track + readout + speed slider
        int trackH = 26;
        MoveWindow(g_trackTime, rightX + colGap, y, rightInner, trackH, TRUE); y += trackH + 2;
        MoveWindow(g_textTime, rightX + colGap, y, rightInner, labelH, TRUE); y += labelH + 4;
        MoveWindow(g_sliderSpeed, rightX + colGap, y, rightInner, 22, TRUE); y += 22 + colGap;

        // Timeline seconds + export row
        int editW = rightInner - colGap - 74;
        if (editW < 80) editW = 80;
        MoveWindow(g_editTimelineSec, rightX + colGap, y, editW, buttonH, TRUE);
        MoveWindow(g_btnTimelineSet,  rightX + colGap + editW + colGap, y, rightInner - editW - colGap, buttonH, TRUE);
        y += buttonH + 4;
        editW = rightInner - colGap - 80;
        if (editW < 100) editW = 100;
        MoveWindow(g_editExportName, rightX + colGap, y, editW, buttonH, TRUE);
        MoveWindow(g_btnExport, rightX + colGap + editW + colGap, y, rightInner - editW - colGap, buttonH, TRUE);
        y += buttonH + colGap;

        // Snap row
        int checkW = 86;
        int snapEditW = 52;
        xBtn = rightX + colGap;
        MoveWindow(g_checkRotSnap, xBtn, y, checkW, buttonH, TRUE); xBtn += checkW + 2;
        MoveWindow(g_editRotSnap, xBtn, y, snapEditW, buttonH, TRUE); xBtn += snapEditW + colGap;
        MoveWindow(g_checkMoveSnap, xBtn, y, checkW, buttonH, TRUE); xBtn += checkW + 2;
        MoveWindow(g_editMoveSnap, xBtn, y, rightX + rightW - colGap - xBtn, buttonH, TRUE);
        y += buttonH + colGap;

        // Interp + commit/cancel
        int btnWide = (rightInner - checkW - colGap * 2) / 2;
        MoveWindow(g_checkInterpLin, rightX + colGap, y, checkW, buttonH, TRUE);
        MoveWindow(g_btnEditCommit, rightX + colGap + checkW + colGap, y, btnWide, buttonH, TRUE);
        MoveWindow(g_btnEditCancel, rightX + colGap + checkW + colGap + btnWide + colGap, y, rightInner - checkW - colGap * 2 - btnWide, buttonH, TRUE);
        y += buttonH + colGap;

        // Numeric TRS rows
        int nGap = 4;
        int nW = (rightInner - nGap * 2) / 3;
        MoveWindow(g_editNumRx, rightX + colGap, y, nW, buttonH, TRUE);
        MoveWindow(g_editNumRy, rightX + colGap + nW + nGap, y, nW, buttonH, TRUE);
        MoveWindow(g_editNumRz, rightX + colGap + (nW + nGap) * 2, y, rightInner - (nW + nGap) * 2, buttonH, TRUE);
        y += buttonH + 4;
        MoveWindow(g_editNumTx, rightX + colGap, y, nW, buttonH, TRUE);
        MoveWindow(g_editNumTy, rightX + colGap + nW + nGap, y, nW, buttonH, TRUE);
        MoveWindow(g_editNumTz, rightX + colGap + (nW + nGap) * 2, y, rightInner - (nW + nGap) * 2, buttonH, TRUE);
        y += buttonH + 4;
        int readW = 70;
        MoveWindow(g_btnNumRead, rightX + colGap, y, readW, buttonH, TRUE);
        MoveWindow(g_btnNumKey,  rightX + colGap + readW + colGap, y, rightInner - readW - colGap, buttonH, TRUE);
        y += buttonH + colGap;

        // Bones list
        MoveWindow(g_labelBones, rightX + colGap, y, rightInner, labelH, TRUE); y += labelH + 2;
        int bonesH = height - margin - y - timelineH;
        if (bonesH < 140) bonesH = 140;
        MoveWindow(g_listBones, rightX + colGap, y, rightInner, bonesH, TRUE); y += bonesH + 2;

        ShowWindow(g_textWarn, g_leftCollapsed ? SW_HIDE : SW_SHOW);
    }
    else
    {
        // Hide right controls when collapsed
        SetRightPanelVisible(false);
    }
}

void RefreshGameFilesUI()
{
    if (!g_guiCreated || !g_gameFilesBrowser)
    {
        return;
    }

    SendMessage(g_listModels, LB_RESETCONTENT, 0, 0);
    SendMessage(g_listAnims, LB_RESETCONTENT, 0, 0);

    g_modelListMap.clear();
    g_animListMap.clear();

    const std::vector<AssetBrowser::AssetInfo>& jmodels = g_gameFilesBrowser->getJModels();
    const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();

    for (size_t i = 0; i < jmodels.size(); i++)
    {
        if (!ContainsNoCaseSimple(jmodels[i].name.c_str(), g_filterModel))
        {
            continue;
        }
        g_modelListMap.push_back((int)i);
        SendMessage(g_listModels, LB_ADDSTRING, 0, (LPARAM)jmodels[i].name.c_str());
    }
    for (size_t i = 0; i < anims.size(); i++)
    {
        if (!ContainsNoCaseSimple(anims[i].name.c_str(), g_filterAnim))
        {
            continue;
        }
        g_animListMap.push_back((int)i);
        SendMessage(g_listAnims, LB_ADDSTRING, 0, (LPARAM)anims[i].name.c_str());
    }

    SyncGuiSelectionFromState();
}

static void EffectsLog(const char* msg)
{
    FILE* f = fopen("effects_debug.log", "a");
    if (f)
    {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static bool BufferContainsToken(const std::vector<unsigned char>& data, const char* token)
{
    if (!token || !token[0] || data.empty())
        return false;
    const size_t len = strlen(token);
    if (len == 0 || len > data.size())
        return false;
    for (size_t i = 0; i + len <= data.size(); ++i)
    {
        bool match = true;
        for (size_t j = 0; j < len; ++j)
        {
            if ((unsigned char)token[j] != data[i + j])
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

static bool FileLooksLikeAnimTableLua(const char* path)
{
    if (!path || !path[0])
        return false;

    FILE* f = fopen(path, "rb");
    if (!f)
        return false;

    std::vector<unsigned char> data;
    data.resize(64 * 1024);
    size_t read = fread(&data[0], 1, data.size(), f);
    fclose(f);
    if (read == 0)
        return false;
    data.resize(read);

    if (BufferContainsToken(data, "AnimTableName") ||
        BufferContainsToken(data, "AnimTableNames") ||
        BufferContainsToken(data, "AnimTableUsed") ||
        BufferContainsToken(data, "AnimTable"))
        return true;

    if (BufferContainsToken(data, "import(\"AT_") ||
        BufferContainsToken(data, "import('AT_") ||
        BufferContainsToken(data, "inherit(\"AT_") ||
        BufferContainsToken(data, "inherit('AT_"))
        return true;

    return false;
}

static bool FileLooksLikeAnimStatesLua(const char* path)
{
    if (!path || !path[0])
        return false;

    FILE* f = fopen(path, "rb");
    if (!f)
        return false;

    std::vector<unsigned char> data;
    data.resize(64 * 1024);
    size_t read = fread(&data[0], 1, data.size(), f);
    fclose(f);
    if (read == 0)
        return false;
    data.resize(read);

    if (BufferContainsToken(data, "AnimationStatesTranslator") ||
        BufferContainsToken(data, "Creature_ClassAnimationStatesTranslator") ||
        BufferContainsToken(data, "ActionsFilter") ||
        BufferContainsToken(data, "StancesFilter") ||
        BufferContainsToken(data, "State."))
        return true;

    return false;
}

static bool ReadFileBytes(const char* path, std::vector<unsigned char>& out)
{
    out.clear();
    if (!path || !path[0]) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(f);
        return false;
    }
    out.resize((size_t)size);
    size_t read = fread(&out[0], 1, out.size(), f);
    fclose(f);
    if (read != out.size())
        return false;
    return true;
}

static bool IsLuaBytecode(const std::vector<unsigned char>& data)
{
    if (data.size() < 4) return false;
    return (data[0] == 0x1B && data[1] == 'L' && data[2] == 'u' && data[3] == 'a');
}

static std::string GetFileNameOnly(const std::string& path)
{
    size_t cut = path.find_last_of("\\/");
    if (cut == std::string::npos) return path;
    return path.substr(cut + 1);
}

static bool FindDecompiledLuaByName(const std::string& fileName, std::string& outPath)
{
    outPath.clear();
    if (fileName.empty()) return false;

    std::vector<std::string> stack;
    stack.push_back("..\\GameFiles\\lotrcparser\\Unluac\\Output");

    while (!stack.empty())
    {
        std::string dir = stack.back();
        stack.pop_back();
        if (!DirExistsA_Simple(dir.c_str()))
            continue;

        std::string search = dir + "\\*";
        WIN32_FIND_DATA findData;
        HANDLE hFind = FindFirstFile(search.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            const char* name = findData.cFileName;
            if (!name || !name[0]) continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            std::string full = dir + "\\" + name;
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                stack.push_back(full);
            }
            else
            {
                if (_stricmp(name, fileName.c_str()) == 0)
                {
                    outPath = full;
                    FindClose(hFind);
                    return true;
                }
            }
        } while (FindNextFile(hFind, &findData));
        FindClose(hFind);
    }

    return false;
}

static bool EnsureLuaEditorBufferSize(size_t size)
{
    const size_t minSize = 1024;
    if (size < minSize) size = minSize;
    if (size > (size_t)1024 * 512) // 512 KB cap
        return false;
    s_luaEditorBuffer.resize(size + 1);
    return true;
}

static bool LoadLuaEditorFromPath(const char* path)
{
    s_luaEditorStatus.clear();
    s_luaEditorTargetPath.clear();
    s_luaEditorSourcePath.clear();
    s_luaEditorBufferLen = 0;
    s_luaEditorOriginalBinary = false;
    if (!path || !path[0])
    {
        s_luaEditorStatus = "No path provided.";
        return false;
    }

    s_luaEditorTargetPath = path;

    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path, bytes))
    {
        s_luaEditorStatus = "Failed to read file.";
        return false;
    }

    const bool isBinary = IsLuaBytecode(bytes);
    s_luaEditorOriginalBinary = isBinary;

    std::string sourcePath = path;
    std::vector<unsigned char> textBytes = bytes;

    if (isBinary)
    {
        std::string fileName = GetFileNameOnly(sourcePath);
        std::string decomp;
        if (FindDecompiledLuaByName(fileName, decomp))
        {
            sourcePath = decomp;
            if (!ReadFileBytes(sourcePath.c_str(), textBytes))
            {
                s_luaEditorStatus = "Failed to read decompiled source.";
                return false;
            }
        }
        else
        {
            s_luaEditorStatus = "Binary Lua. Decompile not found in Unluac Output.";
            return false;
        }
    }

    if (!EnsureLuaEditorBufferSize(textBytes.size() + 1))
    {
        s_luaEditorStatus = "File too large for editor buffer.";
        return false;
    }

    memcpy(&s_luaEditorBuffer[0], &textBytes[0], textBytes.size());
    s_luaEditorBuffer[textBytes.size()] = '\0';
    s_luaEditorBufferLen = (int)textBytes.size();
    s_luaEditorSourcePath = sourcePath;
    s_luaEditorStatus = isBinary ? "Loaded decompiled source (binary original)." : "Loaded source.";
    return true;
}

static bool SaveLuaEditorToTarget()
{
    if (s_luaEditorTargetPath.empty())
    {
        s_luaEditorStatus = "No target file to save.";
        return false;
    }

    const char* buffer = s_luaEditorBuffer.empty() ? "" : &s_luaEditorBuffer[0];
    size_t len = strlen(buffer);
    if (len == 0)
    {
        s_luaEditorStatus = "Editor buffer is empty.";
        return false;
    }

    if (s_luaEditorOriginalBinary)
    {
        std::string backup = s_luaEditorTargetPath + ".luac.bak";
        if (!CopyFileA(s_luaEditorTargetPath.c_str(), backup.c_str(), TRUE))
        {
            backup = s_luaEditorTargetPath + ".bak";
            CopyFileA(s_luaEditorTargetPath.c_str(), backup.c_str(), TRUE);
        }
    }

    FILE* f = fopen(s_luaEditorTargetPath.c_str(), "wb");
    if (!f)
    {
        s_luaEditorStatus = "Failed to write target file.";
        return false;
    }
    fwrite(buffer, 1, len, f);
    fclose(f);

    s_luaEditorStatus = "Saved Lua source to target path.";
    s_luaEditorOriginalBinary = false;
    return true;
}

static void AppendLuaScriptsFromSubBlocks(const char* rootLabel,
                                          const std::string& levelName,
                                          const std::string& subBlocksDir)
{
    if (!DirExistsA_Simple(subBlocksDir.c_str())) return;

    std::string search = subBlocksDir + "\\*.lua";
    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile(search.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        const char* fileName = findData.cFileName;
        if (!fileName || !fileName[0])
            continue;

        std::string path = subBlocksDir + "\\" + fileName;
        if (!FileLooksLikeAnimTableLua(path.c_str()))
            continue;

        std::string display = "[LUA] ";
        if (rootLabel && rootLabel[0])
        {
            display += rootLabel;
            display += "\\";
        }
        display += levelName + "\\" + fileName;
        s_luaListNamesStorage.push_back(display);
        s_luaListPathsStorage.push_back(path);
    } while (FindNextFile(hFind, &findData));

    FindClose(hFind);
}

static void AppendLuaScriptsFromRoot(const char* rootDir, const char* rootLabel)
{
    if (!rootDir || !rootDir[0]) return;
    if (!DirExistsA_Simple(rootDir)) return;

    std::string search = std::string(rootDir) + "\\*";
    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile(search.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;

        const char* levelName = findData.cFileName;
        if (!levelName || !levelName[0]) continue;
        if (strcmp(levelName, ".") == 0 || strcmp(levelName, "..") == 0) continue;

        std::string subBlocks1 = std::string(rootDir) + "\\" + levelName + "\\sub_blocks1";
        std::string subBlocks2 = std::string(rootDir) + "\\" + levelName + "\\sub_blocks2";
        AppendLuaScriptsFromSubBlocks(rootLabel, levelName, subBlocks1);
        AppendLuaScriptsFromSubBlocks(rootLabel, levelName, subBlocks2);
    } while (FindNextFile(hFind, &findData));

    FindClose(hFind);
}

static void RefreshLuaScriptList()
{
    s_luaListNamesStorage.clear();
    s_luaListPathsStorage.clear();
    s_luaListNames.clear();
    s_luaListPaths.clear();

    AppendLuaScriptsFromRoot("..\\GameFiles\\lotrcparser", "lotrcparser");

    for (size_t i = 0; i < s_luaListNamesStorage.size(); ++i)
        s_luaListNames.push_back(s_luaListNamesStorage[i].c_str());
    for (size_t i = 0; i < s_luaListPathsStorage.size(); ++i)
        s_luaListPaths.push_back(s_luaListPathsStorage[i].c_str());
}

static void AppendLuaStatesFromSubBlocks(const char* rootLabel,
                                         const std::string& levelName,
                                         const std::string& subBlocksDir)
{
    if (!DirExistsA_Simple(subBlocksDir.c_str())) return;

    std::string search = subBlocksDir + "\\*.lua";
    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile(search.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        const char* fileName = findData.cFileName;
        if (!fileName || !fileName[0])
            continue;

        std::string path = subBlocksDir + "\\" + fileName;
        if (!FileLooksLikeAnimStatesLua(path.c_str()))
            continue;

        std::string display = "[ANIMSTATES] ";
        if (rootLabel && rootLabel[0])
        {
            display += rootLabel;
            display += "\\";
        }
        display += levelName + "\\" + fileName;
        s_luaStateListNamesStorage.push_back(display);
        s_luaStateListPathsStorage.push_back(path);
    } while (FindNextFile(hFind, &findData));

    FindClose(hFind);
}

static void AppendLuaStatesFromRoot(const char* rootDir, const char* rootLabel)
{
    if (!rootDir || !rootDir[0]) return;
    if (!DirExistsA_Simple(rootDir)) return;

    std::string search = std::string(rootDir) + "\\*";
    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile(search.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;

        const char* levelName = findData.cFileName;
        if (!levelName || !levelName[0]) continue;
        if (strcmp(levelName, ".") == 0 || strcmp(levelName, "..") == 0) continue;

        std::string subBlocks1 = std::string(rootDir) + "\\" + levelName + "\\sub_blocks1";
        std::string subBlocks2 = std::string(rootDir) + "\\" + levelName + "\\sub_blocks2";
        AppendLuaStatesFromSubBlocks(rootLabel, levelName, subBlocks1);
        AppendLuaStatesFromSubBlocks(rootLabel, levelName, subBlocks2);
    } while (FindNextFile(hFind, &findData));

    FindClose(hFind);
}

static void RefreshLuaStateScriptList()
{
    s_luaStateListNamesStorage.clear();
    s_luaStateListPathsStorage.clear();
    s_luaStateListNames.clear();
    s_luaStateListPaths.clear();

    AppendLuaStatesFromRoot("..\\GameFiles\\lotrcparser", "lotrcparser");

    for (size_t i = 0; i < s_luaStateListNamesStorage.size(); ++i)
        s_luaStateListNames.push_back(s_luaStateListNamesStorage[i].c_str());
    for (size_t i = 0; i < s_luaStateListPathsStorage.size(); ++i)
        s_luaStateListPaths.push_back(s_luaStateListPathsStorage[i].c_str());
}

/* Classify a Lua filename into an animation category based on its prefix.
   Returns: "ANM", "AT", "ATK", "SM", "BC", "GL", or "Other". */
static const char* ClassifyLuaAnimFile(const char* filename)
{
    if (!filename || !filename[0]) return "Other";
    if (_strnicmp(filename, "ANM_", 4) == 0) return "ANM";
    if (_strnicmp(filename, "AT_", 3) == 0)  return "AT";
    if (_strnicmp(filename, "ATK_", 4) == 0) return "ATK";
    if (_strnicmp(filename, "SM_", 3) == 0)  return "SM";
    if (_strnicmp(filename, "BC_", 3) == 0)  return "BC";
    if (_strnicmp(filename, "GL_", 3) == 0)  return "GL";
    if (_strnicmp(filename, "Creature_", 9) == 0) return "SM";
    if (_strnicmp(filename, "Humanoid", 8) == 0) return "BC";
    if (_strnicmp(filename, "IM_", 3) == 0) return "SM";
    return "Other";
}

/* Returns true if a Lua filename looks like an animation-related file. */
static bool IsAnimationLuaFile(const char* filename)
{
    if (!filename || !filename[0]) return false;
    const char* cat = ClassifyLuaAnimFile(filename);
    return (strcmp(cat, "Other") != 0);
}

static void RefreshLuaSrcFileList()
{
    s_luaSrcNamesStorage.clear();
    s_luaSrcPathsStorage.clear();
    s_luaSrcCatsStorage.clear();
    s_luaSrcNames.clear();
    s_luaSrcPaths.clear();
    s_luaSrcCats.clear();

    const char* roots[] = { "..\\GameFiles\\lotrcparser", "..\\GameFiles\\srclua" };
    const char* rootLabels[] = { "lotrcparser", "srclua" };

    for (int r = 0; r < 2; ++r)
    {
        const char* rootDir = roots[r];
        if (!DirExistsA_Simple(rootDir)) continue;

        // Scan top-level subdirectories (level names)
        std::string searchDir = std::string(rootDir) + "\\*";
        WIN32_FIND_DATA dirData;
        HANDLE hDirFind = FindFirstFile(searchDir.c_str(), &dirData);
        if (hDirFind == INVALID_HANDLE_VALUE) continue;

        do
        {
            if (!(dirData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const char* levelName = dirData.cFileName;
            if (!levelName || !levelName[0]) continue;
            if (strcmp(levelName, ".") == 0 || strcmp(levelName, "..") == 0) continue;

            // Scan .lua files directly in this level folder
            std::string luaSearch = std::string(rootDir) + "\\" + levelName + "\\*.lua";
            WIN32_FIND_DATA luaData;
            HANDLE hLuaFind = FindFirstFile(luaSearch.c_str(), &luaData);
            if (hLuaFind != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (luaData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const char* fname = luaData.cFileName;
                    if (!fname || !fname[0]) continue;
                    if (!IsAnimationLuaFile(fname)) continue;

                    std::string fullPath = std::string(rootDir) + "\\" + levelName + "\\" + fname;
                    const char* cat = ClassifyLuaAnimFile(fname);
                    std::string display = std::string("[") + cat + "] " + levelName + "/" + fname;

                    s_luaSrcNamesStorage.push_back(display);
                    s_luaSrcPathsStorage.push_back(fullPath);
                    s_luaSrcCatsStorage.push_back(cat);
                } while (FindNextFile(hLuaFind, &luaData));
                FindClose(hLuaFind);
            }

            // Also scan sub_blocks1 and sub_blocks2
            const char* subDirs[] = { "sub_blocks1", "sub_blocks2" };
            for (int s = 0; s < 2; ++s)
            {
                std::string subDir = std::string(rootDir) + "\\" + levelName + "\\" + subDirs[s];
                if (!DirExistsA_Simple(subDir.c_str())) continue;

                std::string subSearch = subDir + "\\*.lua";
                HANDLE hSubFind = FindFirstFile(subSearch.c_str(), &luaData);
                if (hSubFind != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (luaData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        const char* fname = luaData.cFileName;
                        if (!fname || !fname[0]) continue;
                        if (!IsAnimationLuaFile(fname)) continue;

                        std::string fullPath = subDir + "\\" + fname;
                        const char* cat = ClassifyLuaAnimFile(fname);
                        std::string display = std::string("[") + cat + "] " + levelName + "/" + subDirs[s] + "/" + fname;

                        s_luaSrcNamesStorage.push_back(display);
                        s_luaSrcPathsStorage.push_back(fullPath);
                        s_luaSrcCatsStorage.push_back(cat);
                    } while (FindNextFile(hSubFind, &luaData));
                    FindClose(hSubFind);
                }
            }

        } while (FindNextFile(hDirFind, &dirData));
        FindClose(hDirFind);
    }

    // Build const char* pointer arrays
    for (size_t i = 0; i < s_luaSrcNamesStorage.size(); ++i)
        s_luaSrcNames.push_back(s_luaSrcNamesStorage[i].c_str());
    for (size_t i = 0; i < s_luaSrcPathsStorage.size(); ++i)
        s_luaSrcPaths.push_back(s_luaSrcPathsStorage[i].c_str());
    for (size_t i = 0; i < s_luaSrcCatsStorage.size(); ++i)
        s_luaSrcCats.push_back(s_luaSrcCatsStorage[i].c_str());
}

/* ---- Lua Dependency Tree Builder ---- */

/* Extract import("Name") and inherit("Name") from a Lua text buffer. */
static void ParseLuaImports(const char* text, size_t len,
                            std::vector<std::string>& outNames,
                            std::vector<std::string>& outTypes)
{
    outNames.clear();
    outTypes.clear();
    if (!text || len == 0) return;

    const char* p = text;
    const char* end = text + len;
    while (p < end)
    {
        // look for import( or inherit(
        const char* imp = NULL;
        const char* inh = NULL;
        const char* found = NULL;
        const char* foundType = NULL;

        // Find next occurrence of either keyword
        for (const char* s = p; s < end - 6; ++s)
        {
            if (strncmp(s, "import(", 7) == 0)
            {
                found = s + 7;
                foundType = "import";
                break;
            }
            if (s < end - 8 && strncmp(s, "inherit(", 8) == 0)
            {
                found = s + 8;
                foundType = "inherit";
                break;
            }
        }
        if (!found) break;

        // Skip whitespace
        while (found < end && (*found == ' ' || *found == '\t')) ++found;
        // Expect a quote
        if (found < end && (*found == '"' || *found == '\''))
        {
            char quote = *found;
            ++found;
            const char* nameStart = found;
            while (found < end && *found != quote) ++found;
            if (found < end)
            {
                std::string name(nameStart, found - nameStart);
                if (!name.empty())
                {
                    outNames.push_back(name);
                    outTypes.push_back(foundType);
                }
                ++found; // skip closing quote
            }
        }
        p = found;
    }
}

/* Extract animation clip references from AT_ Lua table entries.
   Scans for patterns like: Key = "ClipName" and returns clip names. */
static void ParseLuaAnimClipRefs(const char* text, size_t len,
                                  std::vector<std::string>& outClips)
{
    outClips.clear();
    if (!text || len == 0) return;

    const char* p = text;
    const char* end = text + len;
    while (p < end)
    {
        // Find '=' followed by whitespace then a quoted string
        const char* eq = NULL;
        for (const char* s = p; s < end; ++s)
        {
            if (*s == '=')
            {
                eq = s + 1;
                break;
            }
        }
        if (!eq) break;

        // Skip whitespace after =
        while (eq < end && (*eq == ' ' || *eq == '\t')) ++eq;
        // Expect a quote (but not { for table assignments)
        if (eq < end && (*eq == '"' || *eq == '\''))
        {
            char quote = *eq;
            ++eq;
            const char* nameStart = eq;
            while (eq < end && *eq != quote && *eq != '\n') ++eq;
            if (eq < end && *eq == quote)
            {
                std::string clip(nameStart, eq - nameStart);
                // Filter: only add if it looks like an animation name (not keywords/metadata)
                if (!clip.empty() && clip.find(' ') == std::string::npos &&
                    clip != "AnimTable" && clip.find("AT_") != 0)
                {
                    // Avoid duplicates
                    bool dup = false;
                    for (size_t i = 0; i < outClips.size(); ++i)
                        if (outClips[i] == clip) { dup = true; break; }
                    if (!dup)
                        outClips.push_back(clip);
                }
                ++eq;
            }
        }
        // Advance to next line
        while (eq < end && *eq != '\n') ++eq;
        if (eq < end) ++eq;
        p = eq;
    }
}

/* Get the level root directory from a Lua file path.
   e.g. .../lotrcparser/BlackGates/sub_blocks1/AT_FLY_Eagle.lua -> .../lotrcparser/BlackGates */
static std::string GetLevelRootDir(const std::string& luaFilePath)
{
    std::string dir = GetDirectoryFromPathAnim(luaFilePath.c_str());
    // If in sub_blocks*, go up one level
    std::string dirName = GetFileNameOnly(dir + "\\dummy");
    // Actually get the last folder name
    size_t lastSlash = dir.find_last_of("\\/");
    if (lastSlash != std::string::npos)
    {
        std::string folderName = dir.substr(lastSlash + 1);
        if (folderName.find("sub_blocks") == 0)
            return dir.substr(0, lastSlash);
    }
    return dir;
}

/* Recursively build the dependency tree. visited prevents cycles. */
static void BuildLuaDepTreeRecursive(
    const std::string& name,
    const std::string& path,
    const std::string& type,
    const std::string& cat,
    int parentIdx,
    int depth,
    const std::string& baseDir,
    std::vector<std::string>& visited,
    int maxDepth)
{
    int nodeIdx = (int)s_luaDepNames.size();
    s_luaDepNames.push_back(name);
    s_luaDepPaths.push_back(path);
    s_luaDepTypes.push_back(type);
    s_luaDepCats.push_back(cat);
    s_luaDepParents.push_back(parentIdx);
    s_luaDepDepths.push_back(depth);

    bool resolved = false;
    if (!path.empty())
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (f)
        {
            resolved = true;
            fclose(f);
        }
    }
    s_luaDepResolved.push_back(resolved ? 1 : 0);

    if (!resolved || depth >= maxDepth) return;

    // Check for cycle
    for (size_t i = 0; i < visited.size(); ++i)
    {
        if (visited[i] == path) return; // already visited
    }
    visited.push_back(path);

    // Read the file and parse imports
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path.c_str(), bytes)) return;
    if (bytes.empty()) return;

    // Handle binary Lua
    if (bytes.size() >= 4 && bytes[0] == 0x1B && bytes[1] == 'L' && bytes[2] == 'u' && bytes[3] == 'a')
    {
        // Try finding decompiled source
        std::string fileName = GetFileNameOnly(path);
        std::string decomp;
        if (FindDecompiledLuaByName(fileName, decomp))
        {
            bytes.clear();
            if (!ReadFileBytes(decomp.c_str(), bytes)) return;
        }
        else return; // can't parse binary
    }

    bytes.push_back(0); // null terminate
    const char* text = (const char*)&bytes[0];

    std::vector<std::string> importNames, importTypes;
    ParseLuaImports(text, bytes.size() - 1, importNames, importTypes);

    for (size_t i = 0; i < importNames.size(); ++i)
    {
        std::string childName = importNames[i];
        std::string childType = importTypes[i];

        // Try to resolve: look in same directory, then srclua sub-dirs
        std::string childPath;
        std::string tryPath = baseDir + "\\" + childName + ".lua";
        if (GetFileAttributesA(tryPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            childPath = tryPath;
        }
        else
        {
            // Try without extension (already has .lua?)
            tryPath = baseDir + "\\" + childName;
            if (GetFileAttributesA(tryPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                childPath = tryPath;
        }

        std::string childCat = ClassifyLuaAnimFile(childName.c_str());
        BuildLuaDepTreeRecursive(childName, childPath, childType, childCat,
                                 nodeIdx, depth + 1, baseDir, visited, maxDepth);
    }

    // --- JSON dependency nodes (leaf only, no recursion) ---
    std::string levelRoot = GetLevelRootDir(path);

    // For ANM_ files: check for corresponding animation_tables JSON
    if (_strnicmp(name.c_str(), "ANM_", 4) == 0)
    {
        std::string jsonPath = levelRoot + "\\animation_tables\\" + name + ".json";
        if (GetFileAttributesA(jsonPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            std::string jname = name + ".json";
            s_luaDepNames.push_back(jname);
            s_luaDepPaths.push_back(jsonPath);
            s_luaDepTypes.push_back("json");
            s_luaDepCats.push_back("JSON");
            s_luaDepParents.push_back(nodeIdx);
            s_luaDepDepths.push_back(depth + 1);
            s_luaDepResolved.push_back(1);
        }
    }

    // For AT_ files: parse animation clip references -> animations/*.json
    if (_strnicmp(name.c_str(), "AT_", 3) == 0)
    {
        std::vector<std::string> clips;
        ParseLuaAnimClipRefs(text, bytes.size() - 1, clips);
        std::string animDir = levelRoot + "\\animations\\";
        for (size_t c = 0; c < clips.size(); ++c)
        {
            std::string clipPath = animDir + clips[c] + ".json";
            bool clipExists = (GetFileAttributesA(clipPath.c_str()) != INVALID_FILE_ATTRIBUTES);
            std::string clipDisplayName = clips[c] + ".json";
            s_luaDepNames.push_back(clipDisplayName);
            s_luaDepPaths.push_back(clipExists ? clipPath : "");
            s_luaDepTypes.push_back("json");
            s_luaDepCats.push_back("JSON");
            s_luaDepParents.push_back(nodeIdx);
            s_luaDepDepths.push_back(depth + 1);
            s_luaDepResolved.push_back(clipExists ? 1 : 0);
        }
    }
}

static void BuildLuaDepTree(const char* rootPath)
{
    s_luaDepNames.clear();
    s_luaDepPaths.clear();
    s_luaDepTypes.clear();
    s_luaDepCats.clear();
    s_luaDepParents.clear();
    s_luaDepDepths.clear();
    s_luaDepResolved.clear();
    s_luaDepNamesPtr.clear();
    s_luaDepPathsPtr.clear();
    s_luaDepTypesPtr.clear();
    s_luaDepCatsPtr.clear();

    if (!rootPath || !rootPath[0]) return;

    std::string path(rootPath);
    std::string dir = GetDirectoryFromPathAnim(rootPath);
    std::string name = GetFileNameOnly(path);
    // Strip .lua extension for display
    if (name.size() > 4 && name.substr(name.size() - 4) == ".lua")
        name = name.substr(0, name.size() - 4);

    std::string cat = ClassifyLuaAnimFile(name.c_str());
    std::vector<std::string> visited;

    BuildLuaDepTreeRecursive(name, path, "root", cat, -1, 0, dir, visited, 6);

    // Build pointer arrays
    for (size_t i = 0; i < s_luaDepNames.size(); ++i)
        s_luaDepNamesPtr.push_back(s_luaDepNames[i].c_str());
    for (size_t i = 0; i < s_luaDepPaths.size(); ++i)
        s_luaDepPathsPtr.push_back(s_luaDepPaths[i].c_str());
    for (size_t i = 0; i < s_luaDepTypes.size(); ++i)
        s_luaDepTypesPtr.push_back(s_luaDepTypes[i].c_str());
    for (size_t i = 0; i < s_luaDepCats.size(); ++i)
        s_luaDepCatsPtr.push_back(s_luaDepCats[i].c_str());
}

static void ClearLuaAnimTableView()
{
    s_luaAnimTableInfo.tableName.clear();
    s_luaAnimTableInfo.tableNames.clear();
    s_luaAnimTableInfo.tableUsed.clear();
    s_luaAnimTableInfo.entries.clear();
    s_luaAnimTableInfo.missingClips.clear();
    s_luaAnimTableInfo.missingTables.clear();
    s_luaAnimTablePath.clear();

    s_luaEntryKeysStorage.clear();
    s_luaEntryValuesStorage.clear();
    s_luaEntryPrimaryStorage.clear();
    s_luaEntrySourceStorage.clear();
    s_luaTableNames.clear();
    s_luaTableUsed.clear();
    s_luaEntryKeys.clear();
    s_luaEntryValues.clear();
    s_luaEntryPrimary.clear();
    s_luaEntrySources.clear();
    s_luaMissingClips.clear();
    s_luaMissingTables.clear();
    s_luaAnimTableLastError.clear();
}

static void BuildLuaAnimTableView()
{
    s_luaEntryKeysStorage.clear();
    s_luaEntryValuesStorage.clear();
    s_luaEntryPrimaryStorage.clear();
    s_luaEntrySourceStorage.clear();
    s_luaTableNames.clear();
    s_luaTableUsed.clear();
    s_luaEntryKeys.clear();
    s_luaEntryValues.clear();
    s_luaEntryPrimary.clear();
    s_luaEntrySources.clear();
    s_luaMissingClips.clear();
    s_luaMissingTables.clear();

    for (size_t i = 0; i < s_luaAnimTableInfo.tableNames.size(); ++i)
        s_luaTableNames.push_back(s_luaAnimTableInfo.tableNames[i].c_str());
    for (size_t i = 0; i < s_luaAnimTableInfo.tableUsed.size(); ++i)
        s_luaTableUsed.push_back(s_luaAnimTableInfo.tableUsed[i].c_str());

    for (size_t i = 0; i < s_luaAnimTableInfo.entries.size(); ++i)
    {
        const LuaAnimTableEntry& entry = s_luaAnimTableInfo.entries[i];
        s_luaEntryKeysStorage.push_back(entry.key);
        s_luaEntrySourceStorage.push_back(entry.sourceTable);

        std::string joined;
        for (size_t j = 0; j < entry.clips.size(); ++j)
        {
            if (!joined.empty())
                joined += ", ";
            joined += entry.clips[j];
        }
        s_luaEntryValuesStorage.push_back(joined);
        s_luaEntryPrimaryStorage.push_back(entry.clips.empty() ? std::string() : entry.clips[0]);
    }

    for (size_t i = 0; i < s_luaEntryKeysStorage.size(); ++i)
        s_luaEntryKeys.push_back(s_luaEntryKeysStorage[i].c_str());
    for (size_t i = 0; i < s_luaEntryValuesStorage.size(); ++i)
        s_luaEntryValues.push_back(s_luaEntryValuesStorage[i].c_str());
    for (size_t i = 0; i < s_luaEntryPrimaryStorage.size(); ++i)
        s_luaEntryPrimary.push_back(s_luaEntryPrimaryStorage[i].c_str());
    for (size_t i = 0; i < s_luaEntrySourceStorage.size(); ++i)
        s_luaEntrySources.push_back(s_luaEntrySourceStorage[i].c_str());

    for (size_t i = 0; i < s_luaAnimTableInfo.missingClips.size(); ++i)
        s_luaMissingClips.push_back(s_luaAnimTableInfo.missingClips[i].c_str());
    for (size_t i = 0; i < s_luaAnimTableInfo.missingTables.size(); ++i)
        s_luaMissingTables.push_back(s_luaAnimTableInfo.missingTables[i].c_str());
}

static void ClearLuaAnimStatesView()
{
    s_luaAnimStatesInfo.tableName.clear();
    s_luaAnimStatesInfo.states.clear();
    s_luaAnimStatesPath.clear();

    s_luaStateKeysStorage.clear();
    s_luaStateNamesStorage.clear();
    s_luaStateStancesStorage.clear();
    s_luaStateActionsStorage.clear();
    s_luaStateKeys.clear();
    s_luaStateNames.clear();
    s_luaStateStances.clear();
    s_luaStateActions.clear();
    s_luaAnimStatesLastError.clear();
}

static void BuildLuaAnimStatesView()
{
    s_luaStateKeysStorage.clear();
    s_luaStateNamesStorage.clear();
    s_luaStateStancesStorage.clear();
    s_luaStateActionsStorage.clear();
    s_luaStateKeys.clear();
    s_luaStateNames.clear();
    s_luaStateStances.clear();
    s_luaStateActions.clear();

    for (size_t i = 0; i < s_luaAnimStatesInfo.states.size(); ++i)
    {
        const LuaAnimStateEntry& entry = s_luaAnimStatesInfo.states[i];
        s_luaStateKeysStorage.push_back(entry.key);
        s_luaStateNamesStorage.push_back(entry.name);

        std::string stances;
        for (size_t j = 0; j < entry.stances.size(); ++j)
        {
            if (!stances.empty())
                stances += ", ";
            stances += entry.stances[j];
        }
        s_luaStateStancesStorage.push_back(stances);

        std::string actions;
        for (size_t j = 0; j < entry.actions.size(); ++j)
        {
            if (!actions.empty())
                actions += ", ";
            actions += entry.actions[j];
        }
        s_luaStateActionsStorage.push_back(actions);
    }

    for (size_t i = 0; i < s_luaStateKeysStorage.size(); ++i)
        s_luaStateKeys.push_back(s_luaStateKeysStorage[i].c_str());
    for (size_t i = 0; i < s_luaStateNamesStorage.size(); ++i)
        s_luaStateNames.push_back(s_luaStateNamesStorage[i].c_str());
    for (size_t i = 0; i < s_luaStateStancesStorage.size(); ++i)
        s_luaStateStances.push_back(s_luaStateStancesStorage[i].c_str());
    for (size_t i = 0; i < s_luaStateActionsStorage.size(); ++i)
        s_luaStateActions.push_back(s_luaStateActionsStorage[i].c_str());
}

static bool LoadLuaAnimTableFromPath(const char* path)
{
    ClearLuaAnimTableView();
    if (!path || !path[0])
        return false;

    lua_State* L = luaL_newstate();
    if (!L)
    {
        s_luaAnimTableLastError = "Failed to create Lua state";
        DebugLog("LoadLuaAnimTable: failed to create Lua state");
        return false;
    }
    luaL_openlibs(L);

    LuaScriptContext ctx;
    ctx.baseDir = GetDirectoryFromPathAnim(path);
    ctx.gameFilesRoot = FindGameFilesRootFromPathAnim(path);
    ctx.renderer = g_renderer;

    LuaRegisterBindings(L, &ctx);

    if (!LuaLoadModule(L, &ctx, "MgBase"))
    {
        s_luaAnimTableLastError = "Failed to load MgBase (missing in script folder?)";
        DebugLog("LoadLuaAnimTable: failed to load MgBase");
        lua_close(L);
        return false;
    }

    if (!LuaRunFile(L, path, g_renderer))
    {
        s_luaAnimTableLastError = "Lua script error (see RendererLog)";
        DebugLog("LoadLuaAnimTable: failed to run script");
        lua_close(L);
        return false;
    }

    LuaAnimTableInfo info;
    if (!LuaExtractAnimTableInfo(L, info, ctx, g_renderer))
    {
        s_luaAnimTableLastError = "AnimTable missing or empty in this script";
        lua_close(L);
        return false;
    }

    lua_close(L);

    s_luaAnimTableInfo = info;
    s_luaAnimTablePath = path;
    s_luaAnimTableLastError.clear();
    BuildLuaAnimTableView();
    
    /* Phase 3: Pass AnimTable to renderer */
    if (g_renderer)
    {
        g_renderer->setAnimTable(info);
    }
    
    return true;
}

static bool LoadLuaAnimStatesFromPath(const char* path)
{
    ClearLuaAnimStatesView();
    if (!path || !path[0])
        return false;

    lua_State* L = luaL_newstate();
    if (!L)
    {
        s_luaAnimStatesLastError = "Failed to create Lua state";
        DebugLog("LoadLuaAnimStates: failed to create Lua state");
        return false;
    }
    luaL_openlibs(L);

    LuaScriptContext ctx;
    ctx.baseDir = GetDirectoryFromPathAnim(path);
    ctx.gameFilesRoot = FindGameFilesRootFromPathAnim(path);
    ctx.renderer = g_renderer;

    LuaRegisterBindings(L, &ctx);

    // Best-effort load of MgBase to match game environment (not required for all files).
    LuaLoadModule(L, &ctx, "MgBase");

    if (!LuaRunFile(L, path, g_renderer))
    {
        s_luaAnimStatesLastError = "Lua script error (see RendererLog)";
        DebugLog("LoadLuaAnimStates: failed to run script");
        lua_close(L);
        return false;
    }

    LuaAnimStatesTranslatorInfo info;
    if (!LuaExtractAnimStatesTranslatorInfo(L, info, g_renderer))
    {
        s_luaAnimStatesLastError = "AnimationStatesTranslator not found";
        DebugLog("LoadLuaAnimStates: missing AnimationStatesTranslator");
        lua_close(L);
        return false;
    }

    lua_close(L);

    s_luaAnimStatesInfo = info;
    s_luaAnimStatesPath = path;
    s_luaAnimStatesLastError.clear();
    BuildLuaAnimStatesView();
    
    /* Phase 2: Pass AnimStatesTranslator to renderer */
    if (g_renderer)
    {
        g_renderer->setAnimStatesTranslator(info);
    }
    
    return true;
}

static bool ResolveLuaClipFromActiveTable(const char* clipName, std::string& outPath)
{
    outPath.clear();
    if (!clipName || !clipName[0])
        return false;
    if (s_luaAnimTablePath.empty())
        return false;

    LuaScriptContext ctx;
    ctx.baseDir = GetDirectoryFromPathAnim(s_luaAnimTablePath.c_str());
    ctx.gameFilesRoot = FindGameFilesRootFromPathAnim(s_luaAnimTablePath.c_str());
    ctx.renderer = g_renderer;
    return ResolveLuaAnimClipPath(ctx, clipName, outPath);
}

static void PlayLuaAnimClipByName(const char* clipName)
{
    if (!g_renderer || !clipName || !clipName[0])
        return;
    std::string clipPath;
    if (!ResolveLuaClipFromActiveTable(clipName, clipPath))
    {
        char msg[256];
        sprintf(msg, "Lua clip missing: %s", clipName);
        DebugLog(msg);
        return;
    }
    g_renderer->setJsonAnimationPath(clipPath.c_str());
}

/* Phase 3: Resolve and update clips for active animation state */
static void UpdateResolvedClipsForActiveState()
{
    if (!g_renderer)
        return;

    const LuaAnimStateEntry* activeState = g_renderer->getAnimStatesTranslatorActiveState();
    if (!activeState)
        return;

    std::vector<std::string> clips = g_renderer->resolveStateToClips(activeState->key.c_str());
    
    /* If we have resolved clips, update renderer */
    if (!clips.empty() && g_renderer)
    {
        std::vector<std::string> resolvedPaths;
        LuaScriptContext ctx;
        if (!s_luaAnimTablePath.empty())
        {
            ctx.baseDir = GetDirectoryFromPathAnim(s_luaAnimTablePath.c_str());
            ctx.gameFilesRoot = FindGameFilesRootFromPathAnim(s_luaAnimTablePath.c_str());
            ctx.renderer = g_renderer;
        }

        for (size_t i = 0; i < clips.size(); ++i)
        {
            std::string resolvedPath;
            if (ResolveLuaAnimClipPath(ctx, clips[i], resolvedPath))
            {
                resolvedPaths.push_back(resolvedPath);
            }
        }

        /* Update renderer's resolved clips cache */
        if (!resolvedPaths.empty())
        {
            /* For now, just load the first clip - Phase 4 will add filter logic */
            g_renderer->setJsonAnimationPath(resolvedPaths[0].c_str());
            
            /* Phase 4: Apply filters to active state */
            g_renderer->applyFiltersToActiveState();
            
            /* Phase 5: Update matched graph state */
            g_renderer->updateGraphStateForActiveAnimState();
            
            /* Phase 6: Rebuild motion matching DB from filtered clips if enabled */
            if (g_renderer && g_renderer->getMotionMatchUseFilteredClips())
            {
                /* Get the filtered clips and rebuild the motion matching database */
                std::vector<std::string> filteredClipKeys;
                int filteredCount = g_renderer->getFilteredClipsForActiveStateCount();
                for (int i = 0; i < filteredCount; ++i)
                {
                    const char* clipPath = g_renderer->getFilteredClipsForActiveState(i);
                    if (clipPath && clipPath[0])
                        filteredClipKeys.push_back(clipPath);
                }
                g_renderer->rebuildMotionMatchDatabaseFromFilteredClips(filteredClipKeys);
            }
        }
    }
}

static void ToLowerInPlace(std::string& s)
{
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = (char)tolower((unsigned char)s[i]);
}

static bool IsStopToken(const std::string& t)
{
    const char* kStop[] =
    {
        "anim", "animtable", "table", "at", "anm", "bc",
        "unit", "class", "common", "base",
        NULL
    };
    for (int i = 0; kStop[i]; ++i)
    {
        if (t == kStop[i])
            return true;
    }
    return false;
}

static void CollectTokensFromName(const std::string& name, std::set<std::string>& out)
{
    std::string token;
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (isalnum((unsigned char)c))
        {
            token.push_back((char)tolower((unsigned char)c));
        }
        else
        {
            if (token.size() >= 3 && !IsStopToken(token))
                out.insert(token);
            token.clear();
        }
    }
    if (token.size() >= 3 && !IsStopToken(token))
        out.insert(token);
}

static std::string GetFileBaseNameNoExt(const char* path)
{
    if (!path || !path[0]) return std::string();
    const char* slash = strrchr(path, '/');
    const char* back = strrchr(path, '\\');
    const char* fileName = path;
    if (slash && back) fileName = (slash > back) ? (slash + 1) : (back + 1);
    else if (slash) fileName = slash + 1;
    else if (back) fileName = back + 1;

    std::string base = fileName;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos)
        base = base.substr(0, dot);
    return base;
}

static int ScoreModelNameForTokens(const std::string& modelLower, const std::vector<std::string>& tokens)
{
    int score = 0;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const std::string& t = tokens[i];
        if (t.empty()) continue;
        size_t pos = modelLower.find(t);
        if (pos != std::string::npos)
        {
            score += 5;
            if (pos == 0)
                score += 2;
            else if (pos > 0 && modelLower[pos - 1] == '_')
                score += 1;
        }
    }
    return score;
}

static int GuessModelIndexForLuaTable()
{
    if (!g_gameFilesBrowser)
        return -1;

    std::set<std::string> tokenSet;
    CollectTokensFromName(s_luaAnimTableInfo.tableName, tokenSet);
    CollectTokensFromName(GetFileBaseNameNoExt(s_luaAnimTablePath.c_str()), tokenSet);

    for (size_t i = 0; i < s_luaAnimTableInfo.tableUsed.size(); ++i)
        CollectTokensFromName(s_luaAnimTableInfo.tableUsed[i], tokenSet);
    for (size_t i = 0; i < s_luaAnimTableInfo.tableNames.size(); ++i)
        CollectTokensFromName(s_luaAnimTableInfo.tableNames[i], tokenSet);

    // Fallback: if no AnimTable data, use Lua editor path
    if (tokenSet.empty() && !s_luaEditorTargetPath.empty())
        CollectTokensFromName(GetFileBaseNameNoExt(s_luaEditorTargetPath.c_str()), tokenSet);

    if (tokenSet.empty())
        return -1;

    std::vector<std::string> tokens;
    for (std::set<std::string>::const_iterator it = tokenSet.begin(); it != tokenSet.end(); ++it)
        tokens.push_back(*it);

    const std::vector<AssetBrowser::AssetInfo>& jmodels = g_gameFilesBrowser->getJModels();
    int bestIndex = -1;
    int bestScore = 0;
    for (int i = 0; i < (int)jmodels.size(); ++i)
    {
        std::string nameLower = jmodels[i].name;
        ToLowerInPlace(nameLower);
        int score = ScoreModelNameForTokens(nameLower, tokens);
        if (score > bestScore)
        {
            bestScore = score;
            bestIndex = i;
        }
    }

    if (bestScore < 5)
        return -1;
    return bestIndex;
}

static bool GuessAndLoadModelForLuaTable()
{
    int idx = GuessModelIndexForLuaTable();
    if (idx < 0)
    {
        s_luaGuessedModelName.clear();
        return false;
    }
    g_browserIndexModel = idx;
    SelectGameModel(idx);
    const std::vector<AssetBrowser::AssetInfo>& jm = g_gameFilesBrowser->getJModels();
    if (idx >= 0 && idx < (int)jm.size())
        s_luaGuessedModelName = jm[idx].name;
    return true;
}

static void RefreshGraphList()
{
    s_graphListNamesStorage.clear();
    s_graphListPathsStorage.clear();
    s_graphListNames.clear();
    s_graphListPaths.clear();

    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile("..\\GameFiles\\animations\\graph_*.json", &findData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::string name = findData.cFileName;
                std::string path = std::string("..\\GameFiles\\animations\\") + name;
                s_graphListNamesStorage.push_back(name);
                s_graphListPathsStorage.push_back(path);
            }
        } while (FindNextFile(hFind, &findData));
        FindClose(hFind);
    }

    for (size_t i = 0; i < s_graphListNamesStorage.size(); ++i)
    {
        s_graphListNames.push_back(s_graphListNamesStorage[i].c_str());
    }
    for (size_t i = 0; i < s_graphListPathsStorage.size(); ++i)
    {
        s_graphListPaths.push_back(s_graphListPathsStorage[i].c_str());
    }
}

static bool BuildGraphFromCurrentAnimation(char* outPath, int outSize)
{
    if (!outPath || outSize <= 0) return false;
    outPath[0] = '\0';
    if (!g_renderer) return false;

    const char* animPath = g_renderer->getJsonAnimationPath();
    if (!animPath || !animPath[0])
        return false;

    // Extract filename and base name
    const char* slash = strrchr(animPath, '/');
    const char* back = strrchr(animPath, '\\');
    const char* fileName = animPath;
    if (slash && back)
        fileName = (slash > back) ? (slash + 1) : (back + 1);
    else if (slash)
        fileName = slash + 1;
    else if (back)
        fileName = back + 1;

    char baseName[256] = "";
    strncpy_s(baseName, sizeof(baseName), fileName, _TRUNCATE);
    char* dot = strrchr(baseName, '.');
    if (dot) *dot = '\0';

    char graphPath[512];
    sprintf_s(graphPath, "..\\GameFiles\\animations\\graph_auto_%s.json", baseName[0] ? baseName : "anim");

    FILE* f = fopen(graphPath, "w");
    if (!f)
        return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"Auto_%s\",\n", baseName[0] ? baseName : "Anim");
    fprintf(f, "  \"defaultState\": \"%s\",\n", baseName[0] ? baseName : "Anim");
    fprintf(f, "  \"states\": [\n");
    fprintf(f, "    { \"name\": \"%s\", \"clip\": \"%s\", \"loop\": true, \"speed\": 1.0 }\n",
            baseName[0] ? baseName : "Anim",
            fileName);
    fprintf(f, "  ],\n");
    fprintf(f, "  \"transitions\": []\n");
    fprintf(f, "}\n");
    fclose(f);

    strncpy_s(outPath, outSize, graphPath, _TRUNCATE);
    return true;
}

void RefreshEffectsList()
{
    EffectsLog("[RefreshEffectsList] Called");

    if (!g_guiCreated || !g_listEffects)
    {
        EffectsLog("[RefreshEffectsList] ERROR: GUI not created or listEffects is NULL");
        return;
    }

    SendMessage(g_listEffects, LB_RESETCONTENT, 0, 0);
    g_effectsList.clear();

    // Scan GameFiles/effects directory for .json files
    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile("..\\GameFiles\\effects\\*.json", &findData);

    EffectsLog("[RefreshEffectsList] Scanning: ..\\GameFiles\\effects\\*.json");

    if (hFind != INVALID_HANDLE_VALUE)
    {
        int count = 0;
        do
        {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::string effectName = findData.cFileName;
                char msg[256];
                sprintf(msg, "[RefreshEffectsList] Found file: %s", effectName.c_str());
                EffectsLog(msg);

                // Remove .json extension for display
                if (effectName.length() > 5 && effectName.substr(effectName.length() - 5) == ".json")
                {
                    effectName = effectName.substr(0, effectName.length() - 5);
                }
                g_effectsList.push_back(effectName);
                SendMessage(g_listEffects, LB_ADDSTRING, 0, (LPARAM)effectName.c_str());
                count++;
            }
        } while (FindNextFile(hFind, &findData));
        FindClose(hFind);

        char msg[256];
        sprintf(msg, "[RefreshEffectsList] Total effects found: %d", count);
        EffectsLog(msg);
    }
    else
    {
        EffectsLog("[RefreshEffectsList] ERROR: Could not find effects directory or no files found");
    }
}

void SyncGuiSelectionFromState()
{
    if (!g_guiCreated)
    {
        return;
    }

    if (g_gameAnimIndex >= 0)
    {
        for (size_t i = 0; i < g_animListMap.size(); i++)
        {
            if (g_animListMap[i] == g_gameAnimIndex)
            {
                SendMessage(g_listAnims, LB_SETCURSEL, (WPARAM)i, 0);
                break;
            }
        }
    }
    if (g_browserIndexModel >= 0)
    {
        for (size_t i = 0; i < g_modelListMap.size(); i++)
        {
            if (g_modelListMap[i] == g_browserIndexModel)
            {
                SendMessage(g_listModels, LB_SETCURSEL, (WPARAM)i, 0);
                break;
            }
        }
    }

    char text[256];
    sprintf(text, "Model: %s", (g_activeModelName[0] != '\0') ? g_activeModelName : "<none>");
    SetWindowText(g_textModel, text);
    sprintf(text, "Anim: %s", (g_activeAnimName[0] != '\0') ? g_activeAnimName : "<none>");
    SetWindowText(g_textAnim, text);
}

void UpdateGuiPlayback()
{
    if (!g_guiCreated || !g_renderer)
    {
        return;
    }

    float dur = g_renderer->getJsonAnimationDuration();
    float t = g_renderer->getJsonAnimationTime();
    if (!g_trackDragging && dur > 0.0f)
    {
        int pos = (int)((t / dur) * 1000.0f);
        if (pos < 0) pos = 0;
        if (pos > 1000) pos = 1000;
        SendMessage(g_trackTime, TBM_SETPOS, TRUE, pos);
    }

    char buffer[128];
    sprintf(buffer, "%.2f / %.2fs", t, dur);
    SetWindowText(g_textTime, buffer);

    // Material / texture info panel
    Scene3DRenderer::MaterialInfo mi;
    SendMessage(g_listMaterials, LB_RESETCONTENT, 0, 0);
    if (g_renderer->getPrimaryMaterialInfo(mi))
    {
        sprintf(buffer, "Mat: %s", mi.material[0] ? mi.material : "<unnamed>");
        SetWindowText(g_textMat, buffer);
        sprintf(buffer, "Tex: D:%s%s N:%s%s S:%s%s UV:%s%s",
                mi.diffuse[0] ? mi.diffuse : "<none>",
                mi.hasDiffuse ? "" : " (MISSING)",
                mi.normal[0] ? mi.normal : "<none>",
                mi.hasNormal ? "" : " (missing)",
                mi.specular[0] ? mi.specular : "<none>",
                mi.hasSpecular ? "" : " (missing)",
                mi.hasTexcoord0 ? "yes" : "no",
                mi.generatedUV ? " (generated)" : "");
        SetWindowText(g_textTex, buffer);
    }
    else
    {
        SetWindowText(g_textMat, "Mat: <none>");
        SetWindowText(g_textTex, "Tex: <none>");
    }
    int matCount = g_renderer->getMaterialCount();
    for (int i = 0; i < matCount; ++i)
    {
        if (g_renderer->getMaterialInfo(i, mi))
        {
            char line[256];
            sprintf(line, "%d: %s  D:%s%s  UV:%s%s",
                    i,
                    mi.material[0] ? mi.material : "<unnamed>",
                    mi.diffuse[0] ? mi.diffuse : "<none>",
                    mi.hasDiffuse ? "" : " (missing)",
                    mi.hasTexcoord0 ? "yes" : "no",
                    mi.generatedUV ? " (gen)" : "");
            SendMessage(g_listMaterials, LB_ADDSTRING, 0, (LPARAM)line);
        }
    }
    char warnBuf[256];
    if (g_renderer->getValidationWarning(warnBuf, sizeof(warnBuf)))
    {
        SetWindowText(g_textWarn, warnBuf);
    }
    else
    {
        SetWindowText(g_textWarn, "");
    }

    SetWindowText(g_btnPlay, g_renderer->isJsonAnimationPaused() ? "Play" : "Pause");
    bool rec = g_renderer->isEditorRecording();
    SendMessage(g_btnRecord, BM_SETCHECK, rec ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowText(g_btnRecord, rec ? "Auto*" : "AutoKey");

    float timelineLen = g_renderer->getEditorTimelineDuration();
    if (g_editTimelineSec && GetFocus() != g_editTimelineSec)
    {
        char lenBuf[32];
        if (timelineLen <= 0.0f)
        {
            strcpy_s(lenBuf, "0");
        }
        else
        {
            sprintf(lenBuf, "%.2f", timelineLen);
        }
        SetWindowText(g_editTimelineSec, lenBuf);
    }

    if (g_checkRotSnap)
    {
        SendMessage(g_checkRotSnap, BM_SETCHECK, g_renderer->getRotateSnapEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_checkMoveSnap)
    {
        SendMessage(g_checkMoveSnap, BM_SETCHECK, g_renderer->getMoveSnapEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_checkInterpLin)
    {
        SendMessage(g_checkInterpLin, BM_SETCHECK, g_renderer->getEditorInterpolationMode() == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_editRotSnap && GetFocus() != g_editRotSnap)
    {
        char snapBuf[32];
        sprintf(snapBuf, "%.2f", g_renderer->getRotateSnapDegrees());
        SetWindowText(g_editRotSnap, snapBuf);
    }
    if (g_editMoveSnap && GetFocus() != g_editMoveSnap)
    {
        char snapBuf[32];
        sprintf(snapBuf, "%.4f", g_renderer->getMoveSnapUnits());
        SetWindowText(g_editMoveSnap, snapBuf);
    }
    HWND f = GetFocus();
    bool editingNums = (f == g_editNumRx || f == g_editNumRy || f == g_editNumRz ||
                        f == g_editNumTx || f == g_editNumTy || f == g_editNumTz);
    if (!editingNums)
    {
        float tx = 0.0f, ty = 0.0f, tz = 0.0f;
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        if (g_renderer->getSelectedBoneLocalTRS(tx, ty, tz, rx, ry, rz))
        {
            char b[32];
            sprintf(b, "%.2f", rx); SetWindowText(g_editNumRx, b);
            sprintf(b, "%.2f", ry); SetWindowText(g_editNumRy, b);
            sprintf(b, "%.2f", rz); SetWindowText(g_editNumRz, b);
            sprintf(b, "%.4f", tx); SetWindowText(g_editNumTx, b);
            sprintf(b, "%.4f", ty); SetWindowText(g_editNumTy, b);
            sprintf(b, "%.4f", tz); SetWindowText(g_editNumTz, b);
        }
    }

    float speed = g_renderer->getPlaybackSpeed();
    sprintf(buffer, "Speed: %.2fx", speed);
    SetWindowText(g_textSpeed, buffer);
    int speedPos = (int)(speed * 100.0f);
    if (speedPos < 25) speedPos = 25;
    if (speedPos > 200) speedPos = 200;
    SendMessage(g_sliderSpeed, TBM_SETPOS, TRUE, speedPos);
    SendMessage(g_checkLoop, BM_SETCHECK, g_renderer->getLoopAnimation() ? BST_CHECKED : BST_UNCHECKED, 0);

    bool pending = g_renderer->hasSelectedBonePendingEdit();
    char status[384];
    sprintf(status, "Status: FPS %.1f | Anim %.2f/%.2fs | %s | Loop=%s | AutoKey=%s | Pending=%s | Len=%.2fs | Gizmo=%s/%s | Edit=%s | SnapR=%s(%.1f) SnapM=%s(%.3f) Interp=%s",
            g_fps,
            t,
            dur,
            g_renderer->isJsonAnimationPaused() ? "PAUSED" : "PLAY",
            g_renderer->getLoopAnimation() ? "ON" : "OFF",
            rec ? "ON" : "OFF",
            pending ? "YES" : "NO",
            timelineLen,
            GizmoModeLabel(g_renderer->getGizmoMode()),
            GizmoSpaceLabel(g_renderer->getGizmoSpace()),
            g_editMode ? "ON" : "OFF",
            g_renderer->getRotateSnapEnabled() ? "ON" : "OFF",
            g_renderer->getRotateSnapDegrees(),
            g_renderer->getMoveSnapEnabled() ? "ON" : "OFF",
            g_renderer->getMoveSnapUnits(),
            g_renderer->getEditorInterpolationMode() == 1 ? "LINEAR" : "HOLD");
    if (g_textStatus)
    {
        SetWindowText(g_textStatus, status);
    }
}

void CreateViewportWindow(HWND parent)
{
    if (!parent || g_viewportHwnd)
    {
        return;
    }

    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = RenderProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "ZeroEngine3DRender";

    if (!RegisterClassEx(&wc))
    {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS)
        {
            return;
        }
    }

    g_viewportHwnd = CreateWindowEx(
        0,
        "ZeroEngine3DRender",
        "",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        parent,
        NULL,
        wc.hInstance,
        NULL);

    g_timelineHud = CreateWindowEx(
        0,
        "ZeroEngineTimelineHUD",
        "",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 100, 32,
        g_viewportHwnd,
        NULL,
        wc.hInstance,
        NULL);
}

void RefreshBoneList()
{
    if (!g_guiCreated || !g_listBones)
    {
        return;
    }

    TreeView_DeleteAllItems(g_listBones);
    g_boneTreeItems.clear();

    if (!g_renderer)
    {
        return;
    }

    int count = g_renderer->getSkeletonBoneCount();
    g_boneTreeItems.resize(count, (HTREEITEM)NULL);

    TVINSERTSTRUCT insert = {0};
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;

    for (int i = 0; i < count; i++)
    {
        const char* name = g_renderer->getSkeletonBoneName(i);
        char label[256];
        if (name && name[0] != '\0')
        {
            sprintf(label, "%d: %s", i, name);
        }
        else
        {
            sprintf(label, "%d: <unnamed>", i);
        }

        int parentIdx = g_renderer->getSkeletonParentIndex(i);
        if (parentIdx >= 0 && parentIdx < (int)g_boneTreeItems.size() && g_boneTreeItems[parentIdx])
        {
            insert.hParent = g_boneTreeItems[parentIdx];
        }
        else
        {
            insert.hParent = TVI_ROOT;
        }

        insert.item.pszText = label;
        insert.item.lParam = (LPARAM)i;
        HTREEITEM hItem = (HTREEITEM)TreeView_InsertItem(g_listBones, &insert);
        g_boneTreeItems[i] = hItem;
    }

    if (count > 0 && g_boneTreeItems[0])
    {
        TreeView_SelectItem(g_listBones, g_boneTreeItems[0]);
        g_renderer->setSelectedBoneIndex(0);
    }
}

void UpdateBrowserSelection(int delta)
{
    if (!g_gameFilesBrowser)
    {
        return;
    }
    if (g_browserMode == 0)
    {
        const std::vector<AssetBrowser::AssetInfo>& jmodels = g_gameFilesBrowser->getJModels();
        if (jmodels.empty()) return;
        g_browserIndexModel += delta;
        if (g_browserIndexModel < 0) g_browserIndexModel = (int)jmodels.size() - 1;
        if (g_browserIndexModel >= (int)jmodels.size()) g_browserIndexModel = 0;
    }
    else
    {
        const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();
        if (anims.empty()) return;
        g_browserIndexAnim += delta;
        if (g_browserIndexAnim < 0) g_browserIndexAnim = (int)anims.size() - 1;
        if (g_browserIndexAnim >= (int)anims.size()) g_browserIndexAnim = 0;
    }
}

void DrawAssetBrowserOverlay(HDC hdc)
{
    if (!g_showBrowser || !g_gameFilesBrowser)
    {
        return;
    }

    const int baseX = 10;
    int y = 140;
    char line[512];

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(200, 220, 255));

    sprintf(line, "ASSET BROWSER (F2 toggle, TAB switch, ENTER load)");
    RECT rect = { baseX, y, 900, y + 20 };
    DrawText(hdc, line, -1, &rect, DT_LEFT | DT_TOP);
    y += 18;

    sprintf(line, "Mode: %s", (g_browserMode == 0) ? "Model (jmodel+glb)" : "Animation (json)");
    rect.top = y; rect.bottom = y + 20;
    DrawText(hdc, line, -1, &rect, DT_LEFT | DT_TOP);
    y += 18;

    if (g_browserMode == 0)
    {
        const std::vector<AssetBrowser::AssetInfo>& jmodels = g_gameFilesBrowser->getJModels();
        int count = (int)jmodels.size();
        int start = g_browserIndexModel - 6;
        if (start < 0) start = 0;
        int end = start + 12;
        if (end > count) end = count;

        for (int i = start; i < end; i++)
        {
            const AssetBrowser::AssetInfo& jm = jmodels[i];
            sprintf(line, "%s%s", (i == g_browserIndexModel) ? "> " : "  ", jm.name.c_str());
            rect.top = y; rect.bottom = y + 18;
            DrawText(hdc, line, -1, &rect, DT_LEFT | DT_TOP);
            y += 16;
        }
    }
    else
    {
        const std::vector<AssetBrowser::AssetInfo>& anims = g_gameFilesBrowser->getAnimations();
        int count = (int)anims.size();
        int start = g_browserIndexAnim - 6;
        if (start < 0) start = 0;
        int end = start + 12;
        if (end > count) end = count;

        for (int i = start; i < end; i++)
        {
            const AssetBrowser::AssetInfo& anim = anims[i];
            sprintf(line, "%s%s", (i == g_browserIndexAnim) ? "> " : "  ", anim.name.c_str());
            rect.top = y; rect.bottom = y + 18;
            DrawText(hdc, line, -1, &rect, DT_LEFT | DT_TOP);
            y += 16;
        }
    }
}

//
// ShutdownHavok - Shutdown Havok SDK
//

void ShutdownHavok()
{
    hkBaseSystem::quit();
}

//
// InitializeScene - Load and setup the scene
// Based on: SkinningDemo constructor, NormalBlendingDemo constructor
//

void InitializeScene()
{
    // Create animated character
    g_character = new AnimatedCharacter();

    // Load skeleton rig
    // NOTE: Replace with actual path to your .hkx rig file
    // For testing, you can use Havok demo assets:
    // "Resources/Animation/HavokGirl/hkRig.hkx"
    if (!g_character->loadSkeleton("Resources/Animation/HavokGirl/hkRig.hkx")) {
        MessageBox(g_hwnd, "Failed to load skeleton rig!", "Warning", MB_ICONWARNING | MB_OK);
        return;
    }

    // Load animations
    // NOTE: Replace with actual paths to your .hkx animation files
    g_character->loadAnimation("Resources/Animation/HavokGirl/hkWalk.hkx", "walk");
    g_character->loadAnimation("Resources/Animation/HavokGirl/hkRun.hkx", "run");
    g_character->loadAnimation("Resources/Animation/HavokGirl/hkIdle.hkx", "idle");

    // Start with idle animation
    g_character->playAnimation("idle", 1.0f, 1.0f);

    // Setup camera
    hkVector4 cameraPos;
    cameraPos.set(0.0f, 2.0f, 5.0f);
    g_renderer->setCameraPosition(cameraPos);

    hkVector4 cameraTarget;
    cameraTarget.set(0.0f, 1.0f, 0.0f);
    g_renderer->setCameraTarget(cameraTarget);

    // Load Training level scene (optional)
    // g_renderer->loadScene("Training/Art/BKG_BL_MountainSideEast_01.hkx");
}

//
// UpdateFrame - Update game logic
//

void UpdateFrame(float deltaTime)
{
    // Update character animation
    if (g_character) {
        g_character->update(deltaTime);
    }

    g_guiUpdateAccum += deltaTime;
    if (g_guiUpdateAccum >= 0.1f)
    {
        UpdateGuiPlayback();
        g_guiUpdateAccum = 0.0f;
    }
}

//
// RenderFrame - Render the scene
//

// Forward decl — defined in Scene3DCamera.cpp file scope.
// Pushes capture/cursor state to the camera input gate so WASD/Space/C
// stop reading the keyboard while ImGui owns focus OR the cursor is not
// over the 3D viewport child window. Mouse-held overrides the cursor
// check so a fly-around isn't cancelled mid-drag.
extern "C" void Scene3D_SetCameraInputGate(int uiKb, int cursorInVP, int mouseHeldFromVP);

void RenderFrame(float deltaTime)
{
    if (!g_renderer) return;

    // ── Camera input gate — feed it BEFORE stepAndRender so the camera's
    //    WASD poll inside updateCameraFromInput() sees the current state.
    {
        POINT cp; GetCursorPos(&cp);
        HWND tgt = g_viewportHwnd ? g_viewportHwnd : g_hwnd;
        ScreenToClient(tgt, &cp);
        RECT vrc; GetClientRect(tgt, &vrc);
        int vpW = vrc.right - vrc.left;
        int vpH = vrc.bottom - vrc.top;
        bool inVP = (cp.x >= 0 && cp.y >= 0 && cp.x < vpW && cp.y < vpH);
        bool mouseHeld = (g_leftMouseDown || g_rightMouseDown || g_middleMouseDown);
        Scene3D_SetCameraInputGate(g_uiWantCaptureKeyboard ? 1 : 0,
                                   inVP ? 1 : 0,
                                   mouseHeld ? 1 : 0);
    }

    // Render using Havok pipeline with actual deltaTime
    g_renderer->stepAndRender(deltaTime);

    // Level Inspector — pick objects under mouse cursor
    if (g_renderer->hasLevelScene())
    {
        LevelScene* ls = g_renderer->getLevelScene();
        if (ls && ls->isLoaded())
        {
            POINT pt;
            GetCursorPos(&pt);
            HWND target = g_viewportHwnd ? g_viewportHwnd : g_hwnd;
            ScreenToClient(target, &pt);
            RECT rc;
            GetClientRect(target, &rc);
            int vpW = rc.right - rc.left;
            int vpH = rc.bottom - rc.top;
            bool inVP = (pt.x >= 0 && pt.y >= 0 && pt.x < vpW && pt.y < vpH);
            bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool lmbClicked = g_leftMouseClicked;
            g_leftMouseClicked = false; // consume the click
            bool rmbClicked = g_rightMouseClicked;
            g_rightMouseClicked = false; // consume the click
            // Phase 4 XSI: viewport gizmo input. Non-Alt LMB drives the
            // manipulator when a mode is active and the click hits a
            // handle. We eat the click in that case so it does NOT
            // also flow into the Alt-pick path or any future right-
            // click handler. Mid-drag we keep stamping mouse-move into
            // gizmoUpdateDrag every frame and commit on release.
            bool gizmoEatClick = false;
            if (ls->gizmoMode() != 0 && !altHeld && inVP)
            {
                bool gizDragging = ls->gizmoDragging();
                if (lmbClicked && !gizDragging) {
                    float gOrigin[3], gDir[3];
                    ls->screenToRay(pt.x, pt.y, vpW, vpH, gOrigin, gDir);
                    if (ls->gizmoBeginDrag(gOrigin, gDir)) {
                        gizmoEatClick = true;
                    }
                } else if (gizDragging && g_leftMouseDown) {
                    float gOrigin[3], gDir[3];
                    ls->screenToRay(pt.x, pt.y, vpW, vpH, gOrigin, gDir);
                    ls->gizmoUpdateDrag(gOrigin, gDir);
                } else if (gizDragging && !g_leftMouseDown) {
                    // Drag released. Commit one FieldEdit through the
                    // reader so the change survives SavePak.
                    ls->gizmoEndDrag(&g_levelReader);
                }
            }
            // Cancel any active drag if user pressed Escape.
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) && ls->gizmoDragging())
                ls->gizmoCancelDrag();

            // ── Phase 7 XSI authoring: muscle-memory hotkeys ───────────
            //
            // Every poor bastard who has spent ten thousand hours in
            // Maya / XSI / Blender / 3ds Max already has W=move,
            // E=rotate, R=scale, F=frame-selection burned into their
            // motor cortex. Forcing them to mouse over to a toolbar
            // for that shit is industrial-grade disrespect. We poll
            // the keys here, edge-detect each one with a static so a
            // held key doesn't strobe-spam mode swaps, and route to
            // the gizmo state machine OR the camera-focus pipeline
            // directly. NO args round-trip: this code runs in
            // RenderFrame which does NOT have the ImGuiGlueFrameArgs
            // in scope, and reaching for it would be a one-liner
            // teaching kit on how to crash the editor.
            //
            // GUARD: skip if ImGui has keyboard capture (user typing
            // in a text field), if alt or ctrl is held (those drive
            // camera orbit / entity creation lanes that we are NOT
            // hijacking), or if the cursor is outside the viewport
            // rect.
            //
            // H (hide) and Ctrl+D (duplicate) are deferred. H needs
            // either a new args field or a public toggle on the
            // LevelScene hidden-layer set, neither of which I am
            // bolting on without coordinating with codex's panel.
            // Ctrl+D needs the entity-duplication save pipeline
            // which lives wholly in codex's lane.
            if (!g_uiWantCaptureKeyboard && !altHeld && !ctrlHeld && inVP)
            {
                static bool s_kW = false, s_kE = false, s_kR = false;
                static bool s_kF = false;
                bool nW = (GetAsyncKeyState('W') & 0x8000) != 0;
                bool nE = (GetAsyncKeyState('E') & 0x8000) != 0;
                bool nR = (GetAsyncKeyState('R') & 0x8000) != 0;
                bool nF = (GetAsyncKeyState('F') & 0x8000) != 0;

                // Mode swaps fire ONLY on the rising edge. A held 'W'
                // while painting around the level absolutely does
                // NOT toggle through translate/rotate/scale on every
                // 60Hz poll. Mid-drag the swap is also rejected
                // upstream by setGizmoMode logic, but we belt-and-
                // suspender it here.
                if (nW && !s_kW && !ls->gizmoDragging()) ls->setGizmoMode(1);
                if (nE && !s_kE && !ls->gizmoDragging()) ls->setGizmoMode(2);
                if (nR && !s_kR && !ls->gizmoDragging()) ls->setGizmoMode(3);

                // F = frame selected. Direct camera-fly call,
                // bypassing the args.requestFocusEditorObjGuid path
                // because we are already past DrawFrame this frame
                // and that args is gone. Same destination math as
                // the post-DrawFrame focus handler.
                if (nF && !s_kF && g_renderer) {
                    const float* mat = NULL;
                    int eoSel = ls->selectedEditorObj();
                    if (eoSel >= 0) {
                        const LevelEditorObj* eo = ls->getEditorObj(eoSel);
                        if (eo) mat = eo->mat;
                    }
                    if (mat == NULL) {
                        int iSel = ls->selectedInstance();
                        if (iSel >= 0) {
                            const LevelInstance* inst = ls->getInstance(iSel);
                            if (inst) mat = inst->mat;
                        }
                    }
                    if (mat) {
                        hkVector4 tgt; tgt.set(mat[12], mat[13], mat[14]);
                        hkVector4 cam; cam.set(mat[12] + 15.0f, mat[13] + 20.0f, mat[14] + 15.0f);
                        g_renderer->setCameraTarget(tgt);
                        g_renderer->setCameraPosition(cam);
                    }
                }

                s_kW = nW; s_kE = nE; s_kR = nR; s_kF = nF;
            }

            // ── 3dCrowd authoring hotkeys ──────────────────────────────
            // Delete   = yank the selected crowd member out of m_crowdItems
            //            and rebuild the LevelInstance list.
            // Shift+D  = clone the selected crowd member, offset +1m on
            //            +X so the clone isn't z-fighting with the source.
            //            Shift+D was picked over Ctrl+D because Ctrl+D is
            //            still earmarked for the entity-duplicate pipeline
            //            (see project_xsi_authoring_shell notes — that path
            //            wants the PendingGameObj clone + FieldEdit cascade
            //            and is NOT something this crowd code wants to step
            //            on). Shift+D is unowned. Crowds can have it.
            // Both gated on: ImGui doesn't own keyboard, cursor in VP, the
            // selected LevelInstance is actually a crowd member (the back-
            // ref index pair is non-negative).
            if (!g_uiWantCaptureKeyboard && inVP)
            {
                bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                static bool s_kDel = false;
                static bool s_kCloneD = false;
                bool nDel    = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
                bool nCloneD = (GetAsyncKeyState('D')        & 0x8000) != 0;

                int iSel = ls->selectedInstance();
                const LevelInstance* selInst = (iSel >= 0) ? ls->getInstance(iSel) : NULL;
                bool isCrowdSelected = (selInst != NULL && selInst->crowdItemIdx >= 0
                                         && selInst->crowdValIdx >= 0);

                // Delete on rising edge. No confirm dialog — the user can
                // re-add via the inspector (Phase 3) or by reloading the
                // level. Crowd state is in-memory until save, so this is
                // recoverable until the user hits SavePak.
                if (nDel && !s_kDel && isCrowdSelected && !altHeld && !ctrlHeld) {
                    int item = selInst->crowdItemIdx;
                    int val  = selInst->crowdValIdx;
                    if (ls->deleteCrowdInstance(item, val)) {
                        // The LevelInstance list is now stale — every crowd
                        // entry whose (item==item, val>val) just shifted down
                        // by one. Rebuild from the source of truth and clear
                        // the selection because the dead one is gone.
                        ls->setSelectedInstance(-1);
                        ls->rebuildCrowdInstances();
                    }
                }

                // Shift+D clones the selected member. Same archetype, same
                // animation set (because it stays in the same CrowdItem),
                // offset +1m along world +X so the clone is visible and
                // pickable instead of z-fighting with the source. Same LOD.
                if (nCloneD && !s_kCloneD && shiftHeld && isCrowdSelected
                    && !altHeld && !ctrlHeld) {
                    int item = selInst->crowdItemIdx;
                    int val  = selInst->crowdValIdx;
                    const std::vector<LevelCrowdItem>& items = ls->getCrowdItems();
                    if (item >= 0 && item < (int)items.size()
                        && val  >= 0 && val  < (int)items[item].instances.size())
                    {
                        const LevelCrowdInstance& src = items[item].instances[val];
                        ls->addCrowdInstanceToItem(item,
                            src.position[0] + 1.0f,
                            src.position[1],
                            src.position[2],
                            src.rotation,
                            src.lod);
                        ls->rebuildCrowdInstances();
                        // Don't try to re-select the new instance by index
                        // here — rebuildCrowdInstances() just churned the
                        // index space. The user can click the clone next
                        // frame; that's two clicks total. Beats sketchy
                        // index math on a partially-rebuilt list.
                    }
                }

                s_kDel    = nDel;
                s_kCloneD = nCloneD;
            }

            bool pickClick = altHeld && lmbClicked && !gizmoEatClick;
            bool dragging = altHeld && g_leftMouseDown && g_levelInspector.isEditorObjDragging();
            // Hover-pick when Alt is held (avoids constant ray-casting during normal mouse movement).
            // Always allow the click frame through so selection works.
            bool hoverActive = inVP && altHeld;
            if (pickClick) hoverActive = true; // never suppress the actual selection click
            g_levelInspector.update(ls, pt.x, pt.y, 0, 0, vpW, vpH, pickClick, hoverActive, dragging);
            // Ctrl+RMB = entity creation (right-click world position)
            if (ctrlHeld && rmbClicked && inVP)
                g_levelInspector.updateRightClick(ls, pt.x, pt.y, 0, 0, vpW, vpH);
        }
    }

    // Draw performance overlay (fallback) when ImGui glue is not active
    if (!g_imguiAvailable)
    {
        HWND overlayTarget = g_viewportHwnd ? g_viewportHwnd : g_hwnd;
        HDC hdc = GetDC(overlayTarget);
        DrawPerformanceOverlay(hdc);
        ReleaseDC(overlayTarget, hdc);
    }
}

//
// UpdatePerformanceMetrics - Calculate FPS and other metrics
//

void UpdatePerformanceMetrics(float deltaTime)
{
    g_frameCount++;
    g_fpsUpdateTime += deltaTime;

    if (g_fpsUpdateTime >= 1.0f) {
        g_fps = (float)g_frameCount / g_fpsUpdateTime;
        g_frameCount = 0;
        g_fpsUpdateTime = 0.0f;
    }
}

//
// DrawPerformanceOverlay - Draw FPS and other metrics
//

void DrawPerformanceOverlay(HDC hdc)
{
    if (!g_showOverlay)
    {
        return;
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 255, 0));
    SetTextColor(hdc, g_darkTheme ? RGB(0,255,0) : RGB(0,120,0));

    RECT clientRect;
    HWND overlayTarget = g_viewportHwnd ? g_viewportHwnd : g_hwnd;
    GetClientRect(overlayTarget, &clientRect);
    if (g_timelineHud)
    {
        clientRect.bottom -= 36;
    }
    int right = clientRect.right > 10 ? clientRect.right - 10 : 1200;

    char buffer[256];
    int y = 10;
    sprintf(buffer, "FPS: %.1f", g_fps);
    DrawOverlayLine(hdc, 10, y, right, buffer);
    DrawOverlayLine(hdc, 10, y, right, "Havok v5.5.0 Rendering System");
    DrawOverlayLine(hdc, 10, y, right, "Controls: Alt+LMB=Orbit Alt+MMB=Pan Alt+RMB=Dolly Wheel=Zoom WASD=Move");
    DrawOverlayLine(hdc, 10, y, right, "Arrow Up/Down = Blend Walk/Run");

    if (g_renderer)
    {
        const char* animPath = g_renderer->getJsonAnimationPath();
        const char* animName = animPath ? animPath : "<none>";
        if (animPath && animPath[0] != '\0')
        {
            const char* slash = strrchr(animPath, '\\');
            if (!slash) slash = strrchr(animPath, '/');
            if (slash && slash[1] != '\0')
            {
                animName = slash + 1;
            }
        }
        float t = g_renderer->getJsonAnimationTime();
        float dur = g_renderer->getJsonAnimationDuration();
        sprintf(buffer, "Anim: %s  [%.2fs / %.2fs] %s",
                animName, t, dur,
                g_renderer->isJsonAnimationPaused() ? "PAUSED" : "PLAY");
        DrawOverlayLine(hdc, 10, y, right, buffer);

        const char* modelLabel = (g_activeModelName[0] != '\0') ? g_activeModelName : "<none>";
        sprintf(buffer, "Model: %s", modelLabel);
        DrawOverlayLine(hdc, 10, y, right, buffer);

        sprintf(buffer, "Skybox: %s [%s] Mode=%s (%d total, F6 next, F4 toggle, F10 mode)",
                g_renderer->getActiveSkyboxName(),
                g_renderer->getSkyboxEnabled() ? "ON" : "OFF",
                g_renderer->getSkyRenderModeName(),
                g_renderer->getSkyboxCount());
        DrawOverlayLine(hdc, 10, y, right, buffer);

        sprintf(buffer,
                "Browser: F2=%s Tab=Mode Up/Down=Select Enter=Load Esc=Close F5=Rescan",
                g_showBrowser ? "ON" : "OFF");
        DrawOverlayLine(hdc, 10, y, right, buffer);
        DrawOverlayLine(hdc, 10, y, right, "Playback: F7/F8 Prev/Next  Space/Ins Play/Pause  Z/X Start/End  8/9 Seek");
        sprintf(buffer, "Editor: AutoKey=%s Pending=%s TimelineLen=%.2fs Gizmo=%s/%s RotAxis=%s MoveAxis=%s",
                g_renderer->isEditorRecording() ? "ON" : "OFF",
                g_renderer->hasSelectedBonePendingEdit() ? "YES" : "NO",
                g_renderer->getEditorTimelineDuration(),
                GizmoModeLabel(g_renderer->getGizmoMode()),
                GizmoSpaceLabel(g_renderer->getGizmoSpace()),
                AxisLockLabel(g_renderer->getEditorRotateAxisLock()),
                AxisLockLabel(g_renderer->getEditorTranslateAxisLock()));
        DrawOverlayLine(hdc, 10, y, right, buffer);
        sprintf(buffer, "Snap: Rot=%s(%.1f deg) Move=%s(%.4f units)",
                g_renderer->getRotateSnapEnabled() ? "ON" : "OFF",
                g_renderer->getRotateSnapDegrees(),
                g_renderer->getMoveSnapEnabled() ? "ON" : "OFF",
                g_renderer->getMoveSnapUnits());
        DrawOverlayLine(hdc, 10, y, right, buffer);
        sprintf(buffer, "Interpolation: %s", g_renderer->getEditorInterpolationMode() == 1 ? "LINEAR" : "HOLD");
        DrawOverlayLine(hdc, 10, y, right, buffer);
        sprintf(buffer, "EditMode: %s (B toggle)  Edit Drag: click ring/arrow first  Ctrl+drag fallback", g_editMode ? "ON" : "OFF");
        DrawOverlayLine(hdc, 10, y, right, buffer);

        const char* modeLabel =
            (g_renderer->getJsonDecodeMode() == 1) ? "legacyA" :
            (g_renderer->getJsonDecodeMode() == 2) ? "legacyB" :
            (g_renderer->getJsonDecodeMode() == 3) ? "rotvec" : "havok";
        const char* packLabel = (g_renderer->getType2PackingMode() == 1) ? "per-axis" : "interleaved";
        const char* axisLabel =
            (g_renderer->getRotAxisMode() == 1) ? "xzy" :
            (g_renderer->getRotAxisMode() == 2) ? "zyx" :
            (g_renderer->getRotAxisMode() == 3) ? "yxz" :
            (g_renderer->getRotAxisMode() == 4) ? "yzx" :
            (g_renderer->getRotAxisMode() == 5) ? "zxy" : "xyz";
        const char* applyLabel =
            (g_renderer->getRotApplyMode() == 2) ? "delta-only" :
            (g_renderer->getRotApplyMode() == 1) ? "delta*ref" : "ref*delta";

        const char* rootShort =
            (g_renderer->getRootMotionMode() == 3) ? "extract" :
            (g_renderer->getRootMotionMode() == 2) ? "OFF" :
            (g_renderer->getRootMotionMode() == 1) ? "clampY" : "full";
        const char* groundShort =
            (g_renderer->getGroundClampMode() == 2) ? "SNAP" :
            (g_renderer->getGroundClampMode() == 1) ? "BELOW" : "OFF";

        sprintf(buffer,
                "Modes: JSON=%s Decode=%s Type2=%s Axis=%s Sign=%d Apply=%s",
                g_renderer->getUseJsonAnim() ? "ON" : "OFF",
                modeLabel,
                packLabel,
                axisLabel,
                g_renderer->getRotSignMask(),
                applyLabel);
        DrawOverlayLine(hdc, 10, y, right, buffer);

        sprintf(buffer,
                "Modes2: PoseCorr=%s RefPose=%s Root=%s Ground=%s LogType2=%s",
                g_renderer->getApplyPoseCorrection() ? "ON" : "OFF",
                g_renderer->getForceReferencePose() ? "ON" : "OFF",
                rootShort,
                groundShort,
                g_renderer->getLogType2() ? "ON" : "OFF");
        DrawOverlayLine(hdc, 10, y, right, buffer);

        sprintf(buffer,
                "Ground: Offset=%.3f Base=%.3f FootMin=%.3f MeshMin=%.3f Contact=%.2f Release=%.2f Smooth=%.2f",
                g_renderer->getGroundOffsetY(),
                g_renderer->getModelBaseOffsetY(),
                g_renderer->getDebugFootMinY(),
                g_renderer->getDebugMeshMinY(),
                g_renderer->getGroundContactEps(),
                g_renderer->getGroundReleaseHeight(),
                g_renderer->getGroundSmoothFactor());
        DrawOverlayLine(hdc, 10, y, right, buffer);

        sprintf(buffer,
                "Overlay: H=%s  Help: F1=%s  Pause: %s",
                g_showOverlay ? "ON" : "OFF",
                g_showHelp ? "ON" : "OFF",
                g_renderer->isJsonAnimationPaused() ? "PAUSED" : "PLAY");
        DrawOverlayLine(hdc, 10, y, right, buffer);

        if (g_showBrowser)
        {
            sprintf(buffer, "Browser: ON  Mode=%s  Selection=%d",
                    (g_browserMode == 0) ? "Model" : "Anim",
                    (g_browserMode == 0) ? g_browserIndexModel : g_browserIndexAnim);
            DrawOverlayLine(hdc, 10, y, right, buffer);
        }
    }

    if (g_showHelp)
    {
        sprintf(buffer, "Help: F1/H overlay B edit-mode W/E move-rotate Q local-world V snap on/off N snap-step I interp Enter/K set-key Esc cancel Alt+mouse camera");
        DrawOverlayLine(hdc, 10, y, right, buffer);
    }

    DrawAssetBrowserOverlay(hdc);
}

//
// Shared editor helpers — deduplicated from WM_COMMAND, WM_KEYDOWN, RenderProc
//

static void CommitCurrentEdit()
{
    if (!g_renderer) return;
    g_renderer->editorCommitCurrent(g_renderer->getJsonAnimationTime());
    g_boneDragActive = false;
    g_boneTranslateActive = false;
    g_dragAxisLock = 0;
    UpdateGuiPlayback();
}

static void CancelCurrentEdit()
{
    if (!g_renderer) return;
    g_renderer->editorCancelCurrent();
    g_boneDragActive = false;
    g_boneTranslateActive = false;
    g_dragAxisLock = 0;
    UpdateGuiPlayback();
}

// Shared bone-edit entry for LMB / RMB in the render child window.
// Returns true if the click started a bone edit (caller should NOT set camera drag).
static bool BeginBoneEdit(HWND hwnd, LPARAM lParam, bool& outMouseFlag, const char* fallbackHint)
{
    bool ctrlDown = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
    bool editChord = ctrlDown;
    bool editRequested = g_editMode || editChord;
    if (!(editRequested && g_renderer && g_renderer->getSelectedBoneIndex() >= 0))
        return false;

    g_boneDragActive = false;
    g_boneTranslateActive = false;
    outMouseFlag = false;
    int mx = LOWORD(lParam);
    int my = HIWORD(lParam);
    MapMouseToImGuiViewport(mx, my);
    g_dragAxisLock = g_renderer->pickGizmoAxis(mx, my);
    bool axisHit = (g_dragAxisLock != 0);
    bool allowFreeAxis = (!axisHit && editChord);

    if (axisHit || allowFreeAxis)
    {
        g_renderer->setJsonAnimationPaused(true);
        if (allowFreeAxis) g_dragAxisLock = 0;

        if (g_renderer->getGizmoMode() == Scene3DRenderer::GIZMO_TRANSLATE)
        {
            g_boneTranslateActive = true;
            g_renderer->editorBeginTranslate();
            g_renderer->editorUpdateTranslate(0.0f, 0.0f, 0.0f, g_dragAxisLock);
        }
        else
        {
            g_boneDragActive = true;
            g_renderer->editorBeginDrag();
            g_renderer->editorUpdateDrag(0.0f, 0.0f, g_dragAxisLock);
        }
        return true;
    }

    if (g_textStatus)
        SetWindowText(g_textStatus, fallbackHint);
    return true; // consumed by edit path even if no axis hit
}

// Shared bone-edit release for LMB / RMB.
// Returns true if a pending edit was finalized (caller should NOT clear camera drag).
static bool EndBoneEdit()
{
    if (g_boneTranslateActive && g_renderer)
    {
        g_renderer->editorEndTranslate(g_renderer->getJsonAnimationTime());
        g_boneTranslateActive = false;
        g_dragAxisLock = 0;
        return true;
    }
    if (g_boneDragActive && g_renderer)
    {
        g_renderer->editorEndDrag(g_renderer->getJsonAnimationTime());
        g_boneDragActive = false;
        g_dragAxisLock = 0;
        return true;
    }
    return false;
}

//
// WindowProc - Window message handler
//

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (g_imguiAvailable && fpImGui_WndProc && fpImGui_WndProc(hwnd, uMsg, wParam, lParam))
    {
        return 1;
    }

    bool uiKeys = g_uiWantCaptureKeyboard;
    bool uiMouse = g_uiWantCaptureMouse;

    switch (uMsg) {
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            if (mmi)
            {
                mmi->ptMinTrackSize.x = 900;
                mmi->ptMinTrackSize.y = 600;
            }
            return 0;
        }
        case WM_DESTROY:
            ReleaseModelViewerRenderTarget();
            g_isRunning = false;
            PostQuitMessage(0);
            return 0;

        case WM_ENTERSIZEMOVE:
            g_isResizing = true;
            return 0;

        case WM_EXITSIZEMOVE:
        {
            g_isResizing = false;
            // Now do the deferred resize once, safely after dragging stops
            RECT rc;
            GetClientRect(hwnd, &rc);
            LayoutGui(rc.right, rc.bottom);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_SIZE:
        {
            if (wParam == SIZE_MINIMIZED)
            {
                return 0;
            }
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            // Renderer resize is driven by the render child window dimensions.
            LayoutGui(width, height);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == IDC_BTN_TOGGLE_LEFT && code == BN_CLICKED)
            {
                g_leftCollapsed = !g_leftCollapsed;
                RECT rc; GetClientRect(hwnd, &rc);
                LayoutGui(rc.right, rc.bottom);
                return 0;
            }
            if (id == IDC_BTN_TOGGLE_RIGHT && code == BN_CLICKED)
            {
                g_rightCollapsed = !g_rightCollapsed;
                RECT rc; GetClientRect(hwnd, &rc);
                LayoutGui(rc.right, rc.bottom);
                return 0;
            }
            if (id == IDC_BTN_TOGGLE_TL && code == BN_CLICKED)
            {
                g_timelineCollapsed = !g_timelineCollapsed;
                RECT rc; GetClientRect(hwnd, &rc);
                LayoutGui(rc.right, rc.bottom);
                return 0;
            }

            if (id == IDC_EDIT_MODEL_FILTER && code == EN_CHANGE)
            {
                GetWindowText(g_editModelFilter, g_filterModel, sizeof(g_filterModel));
                RefreshGameFilesUI();
                return 0;
            }
            if (id == IDC_EDIT_ANIM_FILTER && code == EN_CHANGE)
            {
                GetWindowText(g_editAnimFilter, g_filterAnim, sizeof(g_filterAnim));
                RefreshGameFilesUI();
                return 0;
            }
            if (id == IDC_EDIT_EXPORT && code == EN_CHANGE)
            {
                GetWindowText(g_editExportName, g_exportName, sizeof(g_exportName));
                return 0;
            }
            if (id == IDC_EDIT_TIMELINE_SEC && code == EN_CHANGE)
            {
                GetWindowText(g_editTimelineSec, g_timelineSec, sizeof(g_timelineSec));
                return 0;
            }
            if (id == IDC_EDIT_ROT_SNAP && code == EN_CHANGE)
            {
                GetWindowText(g_editRotSnap, g_rotSnapDegText, sizeof(g_rotSnapDegText));
                if (g_renderer)
                {
                    float d = (float)atof(g_rotSnapDegText);
                    if (d > 0.0f)
                    {
                        g_renderer->setRotateSnapDegrees(d);
                    }
                }
                return 0;
            }
            if (id == IDC_EDIT_MOVE_SNAP && code == EN_CHANGE)
            {
                GetWindowText(g_editMoveSnap, g_moveSnapText, sizeof(g_moveSnapText));
                if (g_renderer)
                {
                    float u = (float)atof(g_moveSnapText);
                    if (u > 0.0f)
                    {
                        g_renderer->setMoveSnapUnits(u);
                    }
                }
                return 0;
            }
            if ((id == IDC_EDIT_NUM_RX || id == IDC_EDIT_NUM_RY || id == IDC_EDIT_NUM_RZ ||
                 id == IDC_EDIT_NUM_TX || id == IDC_EDIT_NUM_TY || id == IDC_EDIT_NUM_TZ) && code == EN_CHANGE)
            {
                GetWindowText(g_editNumRx, g_numRxText, sizeof(g_numRxText));
                GetWindowText(g_editNumRy, g_numRyText, sizeof(g_numRyText));
                GetWindowText(g_editNumRz, g_numRzText, sizeof(g_numRzText));
                GetWindowText(g_editNumTx, g_numTxText, sizeof(g_numTxText));
                GetWindowText(g_editNumTy, g_numTyText, sizeof(g_numTyText));
                GetWindowText(g_editNumTz, g_numTzText, sizeof(g_numTzText));
                return 0;
            }
            if (id == IDC_LIST_MODELS)
            {
                int sel = (int)SendMessage(g_listModels, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR)
                {
                    if (sel >= 0 && sel < (int)g_modelListMap.size())
                    {
                        g_browserIndexModel = g_modelListMap[sel];
                    }
                }
                if (code == LBN_DBLCLK && sel != LB_ERR)
                {
                    if (sel >= 0 && sel < (int)g_modelListMap.size())
                    {
                        SelectGameModel(g_modelListMap[sel]);
                    }
                }
                return 0;
            }
            if (id == IDC_LIST_ANIMS)
            {
                int sel = (int)SendMessage(g_listAnims, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR)
                {
                    if (sel >= 0 && sel < (int)g_animListMap.size())
                    {
                        g_browserIndexAnim = g_animListMap[sel];
                    }
                }
                if (code == LBN_DBLCLK && sel != LB_ERR)
                {
                    if (sel >= 0 && sel < (int)g_animListMap.size())
                    {
                        SelectGameAnimation(g_animListMap[sel]);
                    }
                }
                return 0;
            }
            if (id == IDC_BTN_LOAD_MODEL && code == BN_CLICKED)
            {
                int sel = (int)SendMessage(g_listModels, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR)
                {
                    if (sel >= 0 && sel < (int)g_modelListMap.size())
                    {
                        SelectGameModel(g_modelListMap[sel]);
                    }
                }
                return 0;
            }
            if (id == IDC_BTN_LOAD_ANIM && code == BN_CLICKED)
            {
                int sel = (int)SendMessage(g_listAnims, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR)
                {
                    if (sel >= 0 && sel < (int)g_animListMap.size())
                    {
                        SelectGameAnimation(g_animListMap[sel]);
                    }
                }
                return 0;
            }
            if (id == IDC_BTN_RESCAN && code == BN_CLICKED)
            {
                LoadGameFiles();
                return 0;
            }
            if (id == IDC_BTN_SPAWN_EFFECT && code == BN_CLICKED)
            {
                EffectsLog("========================================");
                EffectsLog("[SPAWN EFFECT] Button clicked!");

                int sel = (int)SendMessage(g_listEffects, LB_GETCURSEL, 0, 0);
                char msg[256];
                sprintf(msg, "[SPAWN EFFECT] Selected index: %d, List size: %d", sel, (int)g_effectsList.size());
                EffectsLog(msg);

                if (sel != LB_ERR && sel >= 0 && sel < (int)g_effectsList.size())
                {
                    sprintf(msg, "[SPAWN EFFECT] Effect name: %s", g_effectsList[sel].c_str());
                    EffectsLog(msg);

                    if (g_renderer)
                    {
                        // Load the effect if not already loaded
                        std::string effectPath = "..\\GameFiles\\effects\\" + g_effectsList[sel] + ".json";
                        sprintf(msg, "[SPAWN EFFECT] Loading effect from: %s", effectPath.c_str());
                        EffectsLog(msg);

                        g_renderer->loadTestEffect(effectPath.c_str(), "..\\GameFiles\\textures\\");

                        // Spawn at camera target position (where camera is looking)
                        hkVector4 spawnPos = g_renderer->getCameraTarget();

                        sprintf(msg, "[SPAWN EFFECT] Spawning effect at camera target (%.2f, %.2f, %.2f)",
                                spawnPos(0), spawnPos(1), spawnPos(2));
                        EffectsLog(msg);
                        g_renderer->spawnTestEffect(g_effectsList[sel].c_str(), spawnPos);

                        sprintf(msg, "Spawned effect: %s at camera target", g_effectsList[sel].c_str());
                        SetWindowText(g_textStatus, msg);

                        sprintf(msg, "[SPAWN EFFECT] Status updated: %s", g_effectsList[sel].c_str());
                        EffectsLog(msg);
                        EffectsLog("========================================");
                    }
                    else
                    {
                        EffectsLog("[SPAWN EFFECT] ERROR: g_renderer is NULL!");
                    }
                }
                else
                {
                    EffectsLog("[SPAWN EFFECT] ERROR: No effect selected or invalid selection");
                }
                return 0;
            }
            if (id == IDC_BTN_PRESET_SET_A && code == BN_CLICKED)
            {
                g_presetModel[0] = g_browserIndexModel;
                g_presetAnim[0] = g_gameAnimIndex;
                if (g_textStatus)
                {
                    SetWindowText(g_textStatus, "Status: Preset A saved");
                }
                return 0;
            }
            if (id == IDC_BTN_PRESET_LOAD_A && code == BN_CLICKED)
            {
                if (g_presetModel[0] >= 0)
                {
                    SelectGameModel(g_presetModel[0]);
                }
                if (g_presetAnim[0] >= 0)
                {
                    SelectGameAnimation(g_presetAnim[0]);
                }
                return 0;
            }
            if (id == IDC_BTN_PRESET_SET_B && code == BN_CLICKED)
            {
                g_presetModel[1] = g_browserIndexModel;
                g_presetAnim[1] = g_gameAnimIndex;
                if (g_textStatus)
                {
                    SetWindowText(g_textStatus, "Status: Preset B saved");
                }
                return 0;
            }
            if (id == IDC_BTN_PRESET_LOAD_B && code == BN_CLICKED)
            {
                if (g_presetModel[1] >= 0)
                {
                    SelectGameModel(g_presetModel[1]);
                }
                if (g_presetAnim[1] >= 0)
                {
                    SelectGameAnimation(g_presetAnim[1]);
                }
                return 0;
            }
            if (id == IDC_CHECK_LOOP && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    bool loop = (SendMessage(g_checkLoop, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    g_renderer->setLoopAnimation(loop);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_CHECK_ROT_SNAP && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    bool on = (SendMessage(g_checkRotSnap, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    g_renderer->setRotateSnapEnabled(on);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_CHECK_MOVE_SNAP && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    bool on = (SendMessage(g_checkMoveSnap, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    g_renderer->setMoveSnapEnabled(on);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_CHECK_INTERP_LIN && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    bool on = (SendMessage(g_checkInterpLin, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    g_renderer->setEditorInterpolationMode(on ? 1 : 0);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_EDIT_COMMIT && code == BN_CLICKED)
            {
                CommitCurrentEdit();
                return 0;
            }
            if (id == IDC_BTN_EDIT_CANCEL && code == BN_CLICKED)
            {
                CancelCurrentEdit();
                return 0;
            }
            if (id == IDC_BTN_NUM_READ && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
                    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
                    if (g_renderer->getSelectedBoneLocalTRS(tx, ty, tz, rx, ry, rz))
                    {
                        char b[32];
                        sprintf(b, "%.2f", rx); SetWindowText(g_editNumRx, b);
                        sprintf(b, "%.2f", ry); SetWindowText(g_editNumRy, b);
                        sprintf(b, "%.2f", rz); SetWindowText(g_editNumRz, b);
                        sprintf(b, "%.4f", tx); SetWindowText(g_editNumTx, b);
                        sprintf(b, "%.4f", ty); SetWindowText(g_editNumTy, b);
                        sprintf(b, "%.4f", tz); SetWindowText(g_editNumTz, b);
                    }
                }
                return 0;
            }
            if (id == IDC_BTN_NUM_KEY && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    GetWindowText(g_editNumRx, g_numRxText, sizeof(g_numRxText));
                    GetWindowText(g_editNumRy, g_numRyText, sizeof(g_numRyText));
                    GetWindowText(g_editNumRz, g_numRzText, sizeof(g_numRzText));
                    GetWindowText(g_editNumTx, g_numTxText, sizeof(g_numTxText));
                    GetWindowText(g_editNumTy, g_numTyText, sizeof(g_numTyText));
                    GetWindowText(g_editNumTz, g_numTzText, sizeof(g_numTzText));
                    float rx = (float)atof(g_numRxText);
                    float ry = (float)atof(g_numRyText);
                    float rz = (float)atof(g_numRzText);
                    float tx = (float)atof(g_numTxText);
                    float ty = (float)atof(g_numTyText);
                    float tz = (float)atof(g_numTzText);
                    bool ok = g_renderer->keySelectedBoneLocalTRS(tx, ty, tz, rx, ry, rz,
                                                                  g_renderer->getJsonAnimationTime(),
                                                                  true, true);
                    if (g_textStatus)
                    {
                        SetWindowText(g_textStatus, ok ? "Status: Numeric TRS keyed on selected bone" : "Status: Numeric key failed (select a bone)");
                    }
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_PREV && code == BN_CLICKED)
            {
                SelectGameAnimation(g_gameAnimIndex - 1);
                return 0;
            }
            if (id == IDC_BTN_NEXT && code == BN_CLICKED)
            {
                SelectGameAnimation(g_gameAnimIndex + 1);
                return 0;
            }
            if (id == IDC_BTN_PLAY && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    // If we're at the end, rewind, then unpause
                    float dur = g_renderer->getJsonAnimationDuration();
                    float t   = g_renderer->getJsonAnimationTime();
                    if (dur > 0.0f && t >= dur)
                    {
                        g_renderer->seekJsonAnimation(0.0f);
                    }
                    g_renderer->setJsonAnimationPaused(false);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_STOP && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    g_renderer->seekJsonAnimation(0.0f);
                    g_renderer->setJsonAnimationPaused(true);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_FRAME_PREV && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    float ft = g_renderer->getJsonAnimationFrameTime();
                    if (ft <= 0.0f) ft = 1.0f / 30.0f;
                    float t = g_renderer->getJsonAnimationTime() - ft;
                    if (t < 0.0f) t = 0.0f;
                    g_renderer->seekJsonAnimation(t);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_FRAME_NEXT && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    float ft = g_renderer->getJsonAnimationFrameTime();
                    if (ft <= 0.0f) ft = 1.0f / 30.0f;
                    float t = g_renderer->getJsonAnimationTime() + ft;
                    float dur = g_renderer->getJsonAnimationDuration();
                    if (dur > 0.0f && t > dur) t = dur;
                    g_renderer->seekJsonAnimation(t);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_RECORD && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    bool rec = (SendMessage(g_btnRecord, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    g_renderer->setEditorRecording(rec);
                    if (g_textStatus)
                    {
                        SetWindowText(g_textStatus, rec ? "Status: AutoKey enabled (drag release writes keys)" : "Status: AutoKey disabled (use Set Key or Enter)");
                    }
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_SET_KEY && code == BN_CLICKED)
            {
                CommitCurrentEdit();
                if (g_textStatus)
                    SetWindowText(g_textStatus, "Status: Set Key committed pending transform at current frame");
                return 0;
            }
            if (id == IDC_BTN_FIRST_KEY && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    g_renderer->seekJsonAnimation(0.0f);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_LAST_KEY && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    float dur = g_renderer->getJsonAnimationDuration();
                    float ft = g_renderer->getJsonAnimationFrameTime();
                    // snap to last frame boundary if possible
                    if (dur > 0.0f && ft > 0.0f)
                    {
                        int frames = g_renderer->getJsonAnimationFrameCount();  //You asked for more frames per second
                        if (frames > 1)                                         //so we decided to leave it out to you :D
                        {
                            dur = ft * (float)(frames - 1);
                        }
                    }
                    g_renderer->seekJsonAnimation(dur);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if (id == IDC_BTN_EXPORT && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    GetWindowText(g_editExportName, g_exportName, sizeof(g_exportName));
                    char nameBuf[128];
                    const char* base = g_exportName;
                    if (!base || base[0] == '\0')
                    {
                        base = "Custom_Anim";
                    }
                    strcpy_s(nameBuf, base);
                    char* dot = strrchr(nameBuf, '.');
                    if (!dot || _stricmp(dot, ".json") != 0)
                    {
                        strcat_s(nameBuf, ".json");
                    }

                    char path[512];
                    sprintf(path, "..\\GameFiles\\animations\\%s", nameBuf);
                    bool ok = g_renderer->exportEditorClip(path);
                    if (g_textStatus)
                    {
                        if (ok)
                        {
                            char msg[600];
                            sprintf(msg, "Status: Exported %s", path);
                            SetWindowText(g_textStatus, msg);
                        }
                        else
                        {
                            SetWindowText(g_textStatus, "Status: Export failed (no keys?)");
                        }
                    }
                }
                return 0;
            }
            if (id == IDC_BTN_TIMELINE_SET && code == BN_CLICKED)
            {
                if (g_renderer)
                {
                    GetWindowText(g_editTimelineSec, g_timelineSec, sizeof(g_timelineSec));
                    float sec = (float)atof(g_timelineSec);
                    g_renderer->setEditorTimelineDuration(sec);
                    UpdateGuiPlayback();
                    if (g_textStatus)
                    {
                        char msg[256];
                        sprintf(msg, "Status: Timeline length set to %.2fs (0 = source)", g_renderer->getEditorTimelineDuration());
                        SetWindowText(g_textStatus, msg);
                    }
                }
                return 0;
            }
            break;
        }

        case WM_HSCROLL:
        {
            if ((HWND)lParam == g_trackTime && g_renderer)
            {
                int code = LOWORD(wParam);
                if (code == TB_THUMBTRACK)
                {
                    g_trackDragging = true;
                }
                else if (code == TB_ENDTRACK || code == TB_THUMBPOSITION)
                {
                    g_trackDragging = false;
                }

                int pos = (int)SendMessage(g_trackTime, TBM_GETPOS, 0, 0);
                float dur = g_renderer->getJsonAnimationDuration();
                if (dur > 0.0f)
                {
                    float t = (pos / 1000.0f) * dur;
                    g_renderer->seekJsonAnimation(t);
                    UpdateGuiPlayback();
                }
                return 0;
            }
            if ((HWND)lParam == g_sliderSpeed && g_renderer)
            {
                int pos = (int)SendMessage(g_sliderSpeed, TBM_GETPOS, 0, 0);
                float speed = pos / 100.0f;
                g_renderer->setPlaybackSpeed(speed);
                UpdateGuiPlayback();
                return 0;
            }
            break;
        }

        case WM_NOTIFY:
        {
            NMHDR* hdr = (NMHDR*)lParam;
            if (hdr && hdr->idFrom == IDC_LIST_BONES)
            {
                if (hdr->code == TVN_SELCHANGEDA || hdr->code == TVN_SELCHANGEDW)
                {
                    NMTREEVIEW* tv = (NMTREEVIEW*)lParam;
                    int boneIdx = (int)tv->itemNew.lParam;
                    if (g_renderer && boneIdx >= 0)
                    {
                        g_renderer->setSelectedBoneIndex(boneIdx);
                    }
                }
                return 0;
            }
            break;
        }

        case WM_LBUTTONDOWN:
            if (uiMouse) return 0;
            g_leftMouseDown = true;
            g_leftMouseClicked = true;
            g_lastMouseX = LOWORD(lParam);
            g_lastMouseY = HIWORD(lParam);
            SetCapture(hwnd);
            return 0;

        case WM_LBUTTONUP:
            if (uiMouse) return 0;
            g_leftMouseDown = false;
            ReleaseCapture();
            return 0;

        case WM_RBUTTONDOWN:
            if (uiMouse) return 0;
            g_rightMouseDown = true;
            g_rightMouseClicked = true;
            g_lastMouseX = LOWORD(lParam);
            g_lastMouseY = HIWORD(lParam);
            SetCapture(hwnd);
            return 0;

        case WM_RBUTTONUP:
            if (uiMouse) return 0;
            g_rightMouseDown = false;
            ReleaseCapture();
            return 0;

        case WM_MOUSEMOVE:
        {
            if (uiMouse) return 0;
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);

            if (g_renderer) {
                g_renderer->onMouseMove(x, y, g_leftMouseDown, g_rightMouseDown);
            }

            g_lastMouseX = x;
            g_lastMouseY = y;
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            if (uiMouse) return 0;
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);

            if (g_renderer) {
                g_renderer->onMouseWheel(delta);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, kClrText);
            SetBkColor(hdc, kClrPanelTop);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)g_brPanel;
        }

        case WM_KEYDOWN:
            if (uiKeys) return 0;
            if (wParam < 256) {
                g_keys[wParam] = true;

                // F9: toggle legacy Win32 panels (ImGui docking UI remains active).
                if (wParam == VK_F9 && g_imguiInitialized)
                {
                    g_showLegacyUI = !g_showLegacyUI;
                    RECT rc; GetClientRect(hwnd, &rc);
                    LayoutGui(rc.right, rc.bottom);
                    return 0;
                }

                // Ctrl+1/2/3: collapse/expand left, right, timeline
                if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
                {
                    if (wParam == '1')
                    {
                        g_leftCollapsed = !g_leftCollapsed;
                        RECT rc; GetClientRect(hwnd, &rc);
                        LayoutGui(rc.right, rc.bottom);
                        return 0;
                    }
                    if (wParam == '2')
                    {
                        g_rightCollapsed = !g_rightCollapsed;
                        RECT rc; GetClientRect(hwnd, &rc);
                        LayoutGui(rc.right, rc.bottom);
                        return 0;
                    }
                    if (wParam == '3')
                    {
                        g_timelineCollapsed = !g_timelineCollapsed;
                        RECT rc; GetClientRect(hwnd, &rc);
                        LayoutGui(rc.right, rc.bottom);
                        return 0;
                    }
                }

                if (wParam == VK_F11)
                {
                    ApplyTheme(!g_darkTheme);
                    InvalidateAllTheme();
                    return 0;
                }

                if (g_renderer) {
                    if (wParam == VK_F2)
                    {
                        g_showBrowser = !g_showBrowser;
                        return 0;
                    }
                    if (wParam == VK_F4)
                    {
                        // When a level is loaded, F4 toggles editor/logic wireframes.
                        // Otherwise, F4 toggles the skybox.
                        if (g_renderer->getLevelScene() && g_renderer->getLevelScene()->isLoaded())
                        {
                            g_renderer->getLevelScene()->toggleEditorObjs();
                            if (g_textStatus)
                            {
                                SetWindowText(g_textStatus,
                                    g_renderer->getLevelScene()->editorObjsVisible()
                                    ? "Status: Logic objects VISIBLE"
                                    : "Status: Logic objects HIDDEN");
                            }
                        }
                        else
                        {
                            bool enabled = !g_renderer->getSkyboxEnabled();
                            g_renderer->setSkyboxEnabled(enabled);
                            if (g_textStatus)
                            {
                                char status[256];
                                sprintf(status, "Status: Skybox %s (%s)", enabled ? "ON" : "OFF", g_renderer->getActiveSkyboxName());
                                SetWindowText(g_textStatus, status);
                            }
                        }
                        return 0;
                    }
                    if (wParam == VK_F6)
                    {
                        bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (shiftHeld) {
                            // Shift+F6 = next skybox (was plain F6 before)
                            g_renderer->selectNextSkybox();
                            if (g_textStatus) {
                                char status[256];
                                sprintf(status, "Status: Skybox -> %s", g_renderer->getActiveSkyboxName());
                                SetWindowText(g_textStatus, status);
                            }
                        } else {
                            // F6 = toggle Asset Data Inspector (set flag for DLL)
                            g_showAssetInspector = !g_showAssetInspector;
                        }

                        return 0;
                    }
                    if (wParam == VK_F10)
                    {
                        int mode = g_renderer->getSkyRenderMode();
                        mode = (mode + 1) % 3;
                        g_renderer->setSkyRenderMode(mode);
                        if (g_textStatus)
                        {
                            char status[256];
                            sprintf(status, "Status: Sky mode -> %s", g_renderer->getSkyRenderModeName());
                            SetWindowText(g_textStatus, status);
                        }
                        return 0;
                    }
                    if (wParam == VK_F12)
                    {
                        if (g_renderer && g_renderer->getLevelScene()) {
                            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                            if (ctrl) {
                                // Ctrl+F12: save screenshot
                                g_renderer->getLevelScene()->saveScreenshot("ze_screenshot.bmp");
                                if (g_textStatus) SetWindowText(g_textStatus, "Status: Screenshot saved to ze_screenshot.bmp");
                            } else {
                                // F12: hot-reload shaders from .hlsl files
                                g_renderer->getLevelScene()->hotReloadShaders();
                                if (g_textStatus) SetWindowText(g_textStatus, "Status: Shaders hot-reloaded from shaders/*.hlsl");
                            }
                        }
                        return 0;
                    }
                    if (wParam == VK_F1)
                    {
                        g_showHelp = !g_showHelp;
                        return 0;
                    }
                    if (wParam == VK_F3)
                    {
                        g_renderer->toggleFlyCamera();
                        if (g_textStatus)
                        {
                            SetWindowText(g_textStatus, g_renderer->isFlyCameraActive()
                                ? "Status: FLY CAMERA ON (WASD+Space/C move, RMB+drag look, F3 exit)"
                                : "Status: Fly camera OFF (orbit mode)");
                        }
                        return 0;
                    }
                    if (wParam == 'G' && g_renderer->isFlyCameraActive())
                    {
                        g_renderer->togglePhysicsPlaytest();
                        if (g_textStatus)
                        {
                            SetWindowText(g_textStatus, g_renderer->isPhysicsActive()
                                ? "Status: GRAVITY ON (WASD walk, Space jump, G toggle off)"
                                : "Status: Gravity OFF (free fly mode)");
                        }
                        return 0;
                    }
                    // (F4 editor objects toggle merged into the first F4 handler above)
                    if (wParam == 'H')
                    {
                        g_showOverlay = !g_showOverlay;
                        return 0;
                    }
                    if (wParam == 'I')
                    {
                        int mode = g_renderer->getEditorInterpolationMode();
                        g_renderer->setEditorInterpolationMode(mode == 1 ? 0 : 1);
                        UpdateGuiPlayback();
                        return 0;
                    }
                    if (wParam == 'B')
                    {
                        g_editMode = !g_editMode;
                        UpdateGuiPlayback();
                        return 0;
                    }
                    if (wParam == 'V')
                    {
                        if (g_renderer->getGizmoMode() == Scene3DRenderer::GIZMO_TRANSLATE)
                        {
                            g_renderer->setMoveSnapEnabled(!g_renderer->getMoveSnapEnabled());
                        }
                        else
                        {
                            g_renderer->setRotateSnapEnabled(!g_renderer->getRotateSnapEnabled());
                        }
                        UpdateGuiPlayback();
                        return 0;
                    }
                    if (wParam == 'F')
                    {
                        g_renderer->focusCameraOnModel();
                        UpdateGuiPlayback();
                        return 0;
                    }
                    // Camera presets: Ctrl+1/2/3
                    if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == '1' || wParam == '2' || wParam == '3'))
                    {
                        int preset = (wParam == '1') ? 1 : (wParam == '2') ? 2 : 3;
                        g_renderer->setCameraPreset(preset);
                        UpdateGuiPlayback();
                        return 0;
                    }
                    if (wParam == 'N')
                    {
                        if (g_renderer->getGizmoMode() == Scene3DRenderer::GIZMO_TRANSLATE)
                        {
                            float u = g_renderer->getMoveSnapUnits();
                            if (u < 0.015f) u = 0.05f;
                            else if (u < 0.075f) u = 0.10f;
                            else if (u < 0.15f) u = 0.25f;
                            else u = 0.01f;
                            g_renderer->setMoveSnapUnits(u);
                        }
                        else
                        {
                            float d = g_renderer->getRotateSnapDegrees();
                            if (d < 7.5f) d = 15.0f;
                            else if (d < 20.0f) d = 30.0f;
                            else if (d < 40.0f) d = 45.0f;
                            else d = 5.0f;
                            g_renderer->setRotateSnapDegrees(d);
                        }
                        UpdateGuiPlayback();
                        return 0;
                    }
                    if (wParam == VK_RETURN || wParam == 'K')
                    {
                        CommitCurrentEdit();
                        return 0;
                    }
                    if (g_showBrowser)
                    {
                        if (wParam == VK_TAB)
                        {
                            g_browserMode = (g_browserMode + 1) % 2;
                        }
                        else if (wParam == VK_UP)
                        {
                            UpdateBrowserSelection(-1);
                        }
                        else if (wParam == VK_DOWN)
                        {
                            UpdateBrowserSelection(1);
                        }
                        else if (wParam == VK_RETURN)
                        {
                            if (g_browserMode == 0)
                            {
                                SelectGameModel(g_browserIndexModel);
                            }
                            else
                            {
                                SelectGameAnimation(g_browserIndexAnim);
                            }
                        }
                        else if (wParam == VK_ESCAPE)
                        {
                            g_showBrowser = false;
                        }
                        return 0;
                    }
                    if (wParam == VK_F5)
                    {
                        LoadGameFiles();
                    }
                    else if (wParam == VK_F7 || wParam == VK_PRIOR)
                    {
                        SelectGameAnimation(g_gameAnimIndex - 1);
                    }
                    else if (wParam == VK_F8 || wParam == VK_NEXT)
                    {
                        SelectGameAnimation(g_gameAnimIndex + 1);
                    }
                    else if (wParam == VK_SPACE || wParam == VK_INSERT)
                    {
                        g_renderer->toggleJsonAnimationPaused();
                    }
                    else if (wParam == VK_HOME || wParam == VK_DELETE || wParam == 'Z')
                    {
                        g_renderer->seekJsonAnimation(0.0f);
                    }
                    else if (wParam == VK_END || wParam == 'X')
                    {
                        g_renderer->seekJsonAnimation(g_renderer->getJsonAnimationDuration());
                    }
                    else if (wParam == VK_OEM_4 || wParam == '8' || wParam == VK_NUMPAD8) // [ or 8
                    {
                        g_renderer->seekJsonAnimation(g_renderer->getJsonAnimationTime() - 0.25f);
                    }
                    else if (wParam == VK_OEM_6 || wParam == '9' || wParam == VK_NUMPAD9) // ] or 9
                    {
                        g_renderer->seekJsonAnimation(g_renderer->getJsonAnimationTime() + 0.25f);
                    }
                    g_renderer->onKeyDown((int)wParam);
                }
            }

            // ESC to exit (cancel active edit first, else quit)
            if (wParam == VK_ESCAPE) {
                if (g_boneDragActive || g_boneTranslateActive)
                {
                    CancelCurrentEdit();
                    return 0;
                }
                g_isRunning = false;
                PostQuitMessage(0);
            }
            return 0;
        case WM_KEYUP:
            if (uiKeys) return 0;
            if (wParam < 256) {
                g_keys[wParam] = false;
            }
            return 0;

        case WM_SYSKEYDOWN:
            // Prevent DX9/Havok fullscreen toggle (Alt+Enter) in docked GUI mode.
            if (wParam == VK_RETURN)
            {
                return 0;
            }
            break;

        case WM_ERASEBKGND:
        {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillVerticalGradient(hdc, rc, kClrBgTop, kClrBgBot);
            return 1;
        }

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK RenderProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (g_imguiAvailable && fpImGui_WndProc && fpImGui_WndProc(hwnd, uMsg, wParam, lParam))
    {
        return 1;
    }

    bool uiMouse = g_uiWantCaptureMouse;
    bool uiKeys  = g_uiWantCaptureKeyboard;

    switch (uMsg)
    {
        case WM_SIZE:
        {
            // Defer to once-per-frame resize to avoid double D3D9 Reset.
            // LayoutGui already sets g_resizePending after MoveWindow.
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            if (width > 0 && height > 0 && !g_isResizing)
            {
                g_resizePending = true;
                g_pendingResizeW = width;
                g_pendingResizeH = height;
            }
            return 0;
        }
        case WM_ERASEBKGND:
        {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillVerticalGradient(hdc, rc, kClrPanelTop, kClrPanelBot);
            // subtle top highlight
            RECT hl = rc;
            hl.bottom = hl.top + 1;
            HBRUSH hBrush = CreateSolidBrush(RGB(255,255,255));
            FillRect(hdc, &hl, hBrush);
            DeleteObject(hBrush);
            return 1;
        }
        case WM_LBUTTONDOWN:
        {
            if (uiMouse) { SetFocus(hwnd); return 0; }
            if (!BeginBoneEdit(hwnd, lParam, g_leftMouseDown,
                               "Status: Click a gizmo axis/ring to edit (Ctrl+LMB for free-axis fallback)"))
            {
                // Always mark the button DOWN at button-down time. The
                // "do not drag camera while in edit mode" enforcement lives
                // in WM_MOUSEMOVE (it forces the flags to false there for
                // the plain edit-drag path). Forcing them false HERE killed
                // the Alt+drag override AND the fly-cam look path, because
                // both check g_*MouseDown to know if a button is held.
                // Edit mode used to silently freeze the camera entirely.
                g_leftMouseDown = true;
            }
            g_leftMouseClicked = true;
            {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);
                MapMouseToImGuiViewport(x, y);
                g_lastMouseX = x;
                g_lastMouseY = y;
            }
            SetCapture(hwnd);
            SetFocus(hwnd);
            return 0;
        }

        case WM_LBUTTONUP:
            if (EndBoneEdit()) { ReleaseCapture(); return 0; }
            g_leftMouseDown = false;
            ReleaseCapture();
            return 0;

        case WM_RBUTTONDOWN:
        {
            if (uiMouse) { SetFocus(hwnd); return 0; }
            if (!BeginBoneEdit(hwnd, lParam, g_rightMouseDown,
                               "Status: Click a gizmo axis/ring to edit (Ctrl+RMB for free-axis fallback)"))
            {
                // Mark DOWN unconditionally. See the matching LMB comment.
                g_rightMouseDown = true;
            }
            g_rightMouseClicked = true;
            {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);
                MapMouseToImGuiViewport(x, y);
                g_lastMouseX = x;
                g_lastMouseY = y;
            }
            SetCapture(hwnd);
            SetFocus(hwnd);
            return 0;
        }

        case WM_RBUTTONUP:
            if (EndBoneEdit()) { ReleaseCapture(); return 0; }
            g_rightMouseDown = false;
            ReleaseCapture();
            return 0;

        case WM_MBUTTONDOWN:
            if (uiMouse) { SetFocus(hwnd); return 0; }
            g_middleMouseDown = true;
            {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);
                MapMouseToImGuiViewport(x, y);
                g_lastMouseX = x;
                g_lastMouseY = y;
            }
            SetCapture(hwnd);
            SetFocus(hwnd);
            return 0;

        case WM_MBUTTONUP:
            g_middleMouseDown = false;
            ReleaseCapture();
            return 0;

        case WM_MOUSEMOVE:
        {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            MapMouseToImGuiViewport(x, y);
            int dx = x - g_lastMouseX;
            int dy = y - g_lastMouseY;
            if (uiMouse && !g_leftMouseDown && !g_rightMouseDown && !g_middleMouseDown &&
                !g_boneDragActive && !g_boneTranslateActive)
            {
                g_lastMouseX = x;
                g_lastMouseY = y;
                return 0;
            }
            if (g_boneDragActive && g_renderer)
            {
                bool ctrlDown = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
                bool shiftDown = ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
                bool xDown = ((GetKeyState('X') & 0x8000) != 0);
                bool yDown = ((GetKeyState('Y') & 0x8000) != 0);
                bool zDown = ((GetKeyState('Z') & 0x8000) != 0);
                int keyLock = xDown ? 1 : (yDown ? 2 : (zDown ? 3 : 0));
                int axisLock = keyLock ? keyLock : g_dragAxisLock;
                float scale = 1.0f;
                if (ctrlDown) scale = 0.25f;
                else if (shiftDown) scale = 2.0f;
                g_renderer->editorUpdateDrag((float)dx * scale, (float)dy * scale, axisLock);
            }
            else if (g_boneTranslateActive && g_renderer)
            {
                bool shiftDown = ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
                bool ctrlDown = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
                bool xDown = ((GetKeyState('X') & 0x8000) != 0);
                bool yDown = ((GetKeyState('Y') & 0x8000) != 0);
                bool zDown = ((GetKeyState('Z') & 0x8000) != 0);
                int keyLock = xDown ? 1 : (yDown ? 2 : (zDown ? 3 : 0));
                int axisLock = keyLock ? keyLock : g_dragAxisLock;
                float scale = ctrlDown ? 0.25f : 1.0f;
                float tx = 0.0f;
                float ty = 0.0f;
                float tz = 0.0f;
                if (axisLock == 1)
                {
                    tx = (float)dx * scale;
                }
                else if (axisLock == 2)
                {
                    ty = -(float)dy * scale;
                }
                else if (axisLock == 3)
                {
                    tz = -(float)dy * scale;
                }
                else if (shiftDown)
                {
                    ty = -(float)dy * scale;
                }
                else
                {
                    tx = (float)dx * scale;
                    tz = -(float)dy * scale;
                }
                g_renderer->editorUpdateTranslate(tx, ty, tz, axisLock);
            }
            else if (g_renderer)
            {
                bool altDown = ((GetKeyState(VK_MENU) & 0x8000) != 0);
                bool flyMode = g_renderer->isFlyCameraActive();
                if (flyMode)
                {
                    // Fly camera: right-click drag = mouse look
                    if (g_rightMouseDown)
                    {
                        g_renderer->flyCameraMouseLook(dx, dy);
                    }
                }
                else if (altDown)
                {
                    if (g_leftMouseDown)
                    {
                        g_renderer->orbitCamera((float)dx, (float)dy);
                    }
                    else if (g_middleMouseDown)
                    {
                        g_renderer->panCamera((float)dx, (float)dy);
                    }
                    else if (g_rightMouseDown)
                    {
                        g_renderer->dollyCamera((float)dy * 4.0f);
                    }
                    g_renderer->onMouseMove(x, y, false, false);
                }
                else
                {
                    if (g_editMode)
                    {
                        // Professional edit behavior: no accidental camera drag while editing unless Alt is held.
                        g_renderer->onMouseMove(x, y, false, false);
                    }
                    else
                    {
                        g_renderer->onMouseMove(x, y, g_leftMouseDown, g_rightMouseDown);
                    }
                }
            }
            g_lastMouseX = x;
            g_lastMouseY = y;
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (g_renderer)
            {
                if (g_renderer->isFlyCameraActive())
                {
                    // In fly mode, scroll adjusts movement speed
                    float spd = g_renderer->getFlyCameraSpeed();
                    spd *= (delta > 0) ? 1.25f : 0.8f;
                    if (spd < 10.0f) spd = 10.0f;
                    if (spd > 5000.0f) spd = 5000.0f;
                    g_renderer->setFlyCameraSpeed(spd);
                }
                else
                {
                    g_renderer->onMouseWheel(delta);
                }
            }
            return 0;
        }

        case WM_KEYDOWN:
            if ((g_boneDragActive || g_boneTranslateActive) &&
                (wParam == 'X' || wParam == 'Y' || wParam == 'Z'))
            {
                return 0;
            }
            SendMessage(g_hwnd, WM_KEYDOWN, wParam, lParam);
            return 0;

        case WM_KEYUP:
            SendMessage(g_hwnd, WM_KEYUP, wParam, lParam);
            return 0;

        case WM_SYSKEYDOWN:
            if (wParam == VK_RETURN)
            {
                return 0;
            }
            break;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Timeline event editor — right-click context menu + add-event dialog
// ---------------------------------------------------------------------------

// Still experimental — touch at your own fucking risk
static float g_contextMenuTime = 0.0f; // time position where right-click occurred

struct AddEventDlgData
{
    bool isSoundCue;    // true = SoundCue, false = SoundEvent
    float time;
    char crc0[128];     // primary name
    char crc1[128];     // modifier / bone hint
    char crc2[128];     // sub-qualifier (SoundCue only)
    bool accepted;
};

static INT_PTR CALLBACK AddEventDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AddEventDlgData* data = (AddEventDlgData*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        data = (AddEventDlgData*)lParam;
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
        char title[128];
        sprintf_s(title, "Add %s at t=%.3fs", data->isSoundCue ? "SoundCue" : "SoundEvent", data->time);
        SetWindowText(hDlg, title);
        // Set label text for CRC[1]
        SetDlgItemText(hDlg, 10, data->isSoundCue ? "Event Name:" : "Category:");
        SetDlgItemText(hDlg, 12, data->isSoundCue ? "Modifier:" : "Bone Hint:");
        // Show/hide CRC[2] row
        ShowWindow(GetDlgItem(hDlg, 14), data->isSoundCue ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, 15), data->isSoundCue ? SW_SHOW : SW_HIDE);
        SetFocus(GetDlgItem(hDlg, 11));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            GetDlgItemText(hDlg, 11, data->crc0, sizeof(data->crc0));
            GetDlgItemText(hDlg, 13, data->crc1, sizeof(data->crc1));
            GetDlgItemText(hDlg, 15, data->crc2, sizeof(data->crc2));
            if (data->crc0[0] == '\0')
            {
                MessageBox(hDlg, "Event name cannot be empty.", "Error", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            data->accepted = true;
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            data->accepted = false;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        data->accepted = false;
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

// Build the dialog template in memory (no .rc file needed)
static bool ShowAddEventDialog(HWND parent, AddEventDlgData& data)
{
    // Dialog dimensions
    const int dlgW = 240, dlgH = 130;
    const int lblW = 70, editW = 150, editH = 14, gap = 4, startY = 8, lblX = 6;
    const int editX = lblX + lblW + gap;
    const int btnW = 50, btnH = 16;

    // Allocate buffer for DLGTEMPLATE + items
    // We need: 1 dialog + 8 controls (3 labels + 3 edits + 2 buttons)
    BYTE buf[2048];
    memset(buf, 0, sizeof(buf));
    BYTE* p = buf;

    // DLGTEMPLATE
    DLGTEMPLATE* dlg = (DLGTEMPLATE*)p;
    dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    dlg->cdit = 8; // 8 controls
    dlg->cx = (short)dlgW;
    dlg->cy = (short)dlgH;
    p += sizeof(DLGTEMPLATE);
    // menu, class, title (all empty WORDs)
    *(WORD*)p = 0; p += 2; // menu
    *(WORD*)p = 0; p += 2; // class
    *(WORD*)p = 0; p += 2; // title

    // Helper lambda-like macros replaced with inline code
    // Each DLGITEMTEMPLATE must be DWORD-aligned
    #define ALIGN4(ptr) ptr = (BYTE*)(((ULONG_PTR)ptr + 3) & ~3)

    // Control: Static label "Event Name:" (id=10)
    ALIGN4(p);
    { DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
      item->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
      item->x = (short)lblX; item->y = (short)startY; item->cx = (short)lblW; item->cy = (short)editH;
      item->id = 10;
      p += sizeof(DLGITEMTEMPLATE);
      *(WORD*)p = 0xFFFF; p += 2; *(WORD*)p = 0x0082; p += 2; // class = STATIC
      // text
      const wchar_t* txt = L"Event Name:";
      int len = (int)wcslen(txt) + 1;
      memcpy(p, txt, len * 2); p += len * 2;
      *(WORD*)p = 0; p += 2; // extra
    }

    // Control: Edit for crc0 (id=11)
    ALIGN4(p);
    { DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
      item->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
      item->x = (short)editX; item->y = (short)startY; item->cx = (short)editW; item->cy = (short)editH;
      item->id = 11;
      p += sizeof(DLGITEMTEMPLATE);
      *(WORD*)p = 0xFFFF; p += 2; *(WORD*)p = 0x0081; p += 2; // class = EDIT
      *(WORD*)p = 0; p += 2; // text (empty)
      *(WORD*)p = 0; p += 2; // extra
    }

    int row2Y = startY + editH + gap;
    // Control: Static label "Modifier:" (id=12)
    ALIGN4(p);
    { DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
      item->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
      item->x = (short)lblX; item->y = (short)row2Y; item->cx = (short)lblW; item->cy = (short)editH;
      item->id = 12;
      p += sizeof(DLGITEMTEMPLATE);
      *(WORD*)p = 0xFFFF; p += 2; *(WORD*)p = 0x0082; p += 2;
      const wchar_t* txt = L"Modifier:";
      int len = (int)wcslen(txt) + 1;
      memcpy(p, txt, len * 2); p += len * 2;
      *(WORD*)p = 0; p += 2;
    }

    // Control: Edit for crc1 (id=13)
    ALIGN4(p);
    { DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
      item->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
      item->x = (short)editX; item->y = (short)row2Y; item->cx = (short)editW; item->cy = (short)editH;
      item->id = 13;
      p += sizeof(DLGITEMTEMPLATE);
      *(WORD*)p = 0xFFFF; p += 2; *(WORD*)p = 0x0081; p += 2;
      *(WORD*)p = 0; p += 2;
      *(WORD*)p = 0; p += 2;
    }

    int row3Y = row2Y + editH + gap;
    // Control: Static label "Sub:" (id=14)
    ALIGN4(p);
    { DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
      item->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
      item->x = (short)lblX; item->y = (short)row3Y; item->cx = (short)lblW; item->cy = (short)editH;
      item->id = 14;
      p += sizeof(DLGITEMTEMPLATE);
      *(WORD*)p = 0xFFFF; p += 2; *(WORD*)p = 0x0082; p += 2;
      const wchar_t* txt = L"Sub-qualifier:";
      int len = (int)wcslen(txt) + 1;
      memcpy(p, txt, len * 2); p += len * 2;
      *(WORD*)p = 0; p += 2;
    }

    // Control: Edit for crc2 (id=15)
    ALIGN4(p);
    { DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
      item->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
      item->x = (short)editX; item->y = (short)row3Y; item->cx = (short)editW; item->cy = (short)editH;
      item->id = 15;
      p += sizeof(DLGITEMTEMPLATE);
      *(WORD*)p = 0xFFFF; p += 2; *(WORD*)p = 0x0081; p += 2;
      *(WORD*)p = 0; p += 2;
      *(WORD*)p = 0; p += 2;
    }

    int btnY = row3Y + editH + gap + 4;
    int btnOkX = dlgW / 2 - btnW - gap;
    int btnCancelX = dlgW / 2 + gap;

    // Control: OK button (IDOK)
    ALIGN4(p);
    { DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
      item->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP;
      item->x = (short)btnOkX; item->y = (short)btnY; item->cx = (short)btnW; item->cy = (short)btnH;
      item->id = IDOK;
      p += sizeof(DLGITEMTEMPLATE);
      *(WORD*)p = 0xFFFF; p += 2; *(WORD*)p = 0x0080; p += 2; // class = BUTTON
      const wchar_t* txt = L"OK";
      int len = (int)wcslen(txt) + 1;
      memcpy(p, txt, len * 2); p += len * 2;
      *(WORD*)p = 0; p += 2;
    }

    // Control: Cancel button (IDCANCEL)
    ALIGN4(p);
    { DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
      item->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
      item->x = (short)btnCancelX; item->y = (short)btnY; item->cx = (short)btnW; item->cy = (short)btnH;
      item->id = IDCANCEL;
      p += sizeof(DLGITEMTEMPLATE);
      *(WORD*)p = 0xFFFF; p += 2; *(WORD*)p = 0x0080; p += 2;
      const wchar_t* txt = L"Cancel";
      int len = (int)wcslen(txt) + 1;
      memcpy(p, txt, len * 2); p += len * 2;
      *(WORD*)p = 0; p += 2;
    }

    #undef ALIGN4

    data.accepted = false;
    DialogBoxIndirectParam(GetModuleHandle(NULL), (DLGTEMPLATE*)buf, parent, AddEventDlgProc, (LPARAM)&data);
    return data.accepted;
}

static float TimelinePixelToTime(HWND hwnd, int pixelX)
{
    if (!g_renderer) return 0.0f;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int margin = 8;
    int usableW = (rc.right - rc.left) - margin * 2;
    float dur = g_renderer->getJsonAnimationDuration();
    if (dur <= 0.0f || usableW <= 0) return 0.0f;
    float visible = dur / g_timelineZoom;
    if (visible <= 0.0f) visible = dur;
    float t = ((float)(pixelX - margin) / (float)usableW) * visible;
    if (t < 0.0f) t = 0.0f;
    if (t > dur) t = dur;
    return t;
}

//
// TimelineProc - custom HUD for scrub/markers
//
LRESULT CALLBACK TimelineProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (g_imguiAvailable && fpImGui_WndProc && fpImGui_WndProc(hwnd, uMsg, wParam, lParam))
    {
        return 1;
    }

    static bool dragging = false;
    switch (uMsg)
    {
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        {
            if (!g_renderer)
                break;
            if (uMsg == WM_LBUTTONDOWN)
            {
                dragging = true;
                SetCapture(hwnd);

                // NEW: Shift+Click to add keyframe at millisecond precision
                bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (shiftPressed && g_renderer->getSelectedBoneIndex() >= 0)
                {
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    int x = LOWORD(lParam);
                    int margin = 8;
                    int usableW = (rc.right - rc.left) - margin * 2;
                    float dur = g_renderer->getJsonAnimationDuration();
                    if (dur > 0.0f && usableW > 0)
                    {
                        float visible = dur / g_timelineZoom;
                        if (visible <= 0.0f) visible = dur;
                        float timeSeconds = ((float)(x - margin) / (float)usableW) * visible;
                        if (timeSeconds < 0.0f) timeSeconds = 0.0f;
                        if (timeSeconds > dur) timeSeconds = dur;

                        // Key the selected bone at this precise millisecond time
                        float tx, ty, tz, rxDeg, ryDeg, rzDeg;
                        if (g_renderer->getSelectedBoneLocalTRS(tx, ty, tz, rxDeg, ryDeg, rzDeg))
                        {
                            g_renderer->keySelectedBoneLocalTRS(tx, ty, tz, rxDeg, ryDeg, rzDeg,
                                                                timeSeconds, true, true);
                            g_renderer->seekJsonAnimation(timeSeconds);
                            UpdateGuiPlayback();

                            // Visual feedback
                            char msg[128];
                            sprintf_s(msg, "Keyframe added at %.2fms", timeSeconds * 1000.0f);
                            if (g_textStatus) SetWindowText(g_textStatus, msg);
                        }
                    }
                    // Don't drag after shift-click key
                    dragging = false;
                    ReleaseCapture();
                }
            }
            if (dragging)
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int x = LOWORD(lParam);
                int margin = 8;
                int usableW = (rc.right - rc.left) - margin * 2;
                float dur = g_renderer->getJsonAnimationDuration();
                if (dur > 0.0f && usableW > 0)
                {
                    float visible = dur / g_timelineZoom;
                    if (visible <= 0.0f) visible = dur;
                    float t = ((float)(x - margin) / (float)usableW) * visible;
                    if (t < 0.0f) t = 0.0f;
                    if (t > dur) t = dur;
                    g_renderer->seekJsonAnimation(t);
                    UpdateGuiPlayback();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            else if (uMsg == WM_MOUSEMOVE)
            {
                // Hover detection and millisecond time display
                RECT rc;
                GetClientRect(hwnd, &rc);
                int mx = (short)LOWORD(lParam);
                int margin = 8;
                int barW = (rc.right - rc.left) - margin * 2;
                float dur = g_renderer->getJsonAnimationDuration();
                int prev = g_hoveredEventIndex;
                g_hoveredEventIndex = -1;
                if (dur > 0.0f && barW > 0)
                {
                    float visible = dur / g_timelineZoom;
                    if (visible <= 0.0f) visible = dur;
                    float scale = (float)barW / visible;
                    int barLeft = rc.left + margin;

                    // NEW: Calculate and display millisecond time under cursor
                    float hoverTimeSeconds = ((float)(mx - barLeft) / (float)barW) * visible;
                    if (hoverTimeSeconds < 0.0f) hoverTimeSeconds = 0.0f;
                    if (hoverTimeSeconds > dur) hoverTimeSeconds = dur;

                    // Set tooltip with millisecond precision
                    static HWND hToolTip = NULL;
                    static char tooltipBuf[64] = "";
                    char newTooltip[64];
                    sprintf_s(newTooltip, sizeof(newTooltip), "%.2fms", hoverTimeSeconds * 1000.0f);

                    if (strcmp(newTooltip, tooltipBuf) != 0)
                    {
                        strcpy_s(tooltipBuf, newTooltip);
                        if (g_textStatus) SetWindowText(g_textStatus, newTooltip);
                    }

                    int evtCount = g_renderer->getJsonAnimEventCount();
                    int bestDist = 7; // max pixel proximity
                    for (int i = 0; i < evtCount; ++i)
                    {
                        float et = g_renderer->getJsonAnimEventTime(i);
                        int ex = barLeft + (int)(et * scale);
                        int d = abs(mx - ex);
                        if (d < bestDist) { bestDist = d; g_hoveredEventIndex = i; }
                    }
                }
                if (g_hoveredEventIndex != prev)
                    InvalidateRect(hwnd, NULL, FALSE);
            }
            if (uMsg == WM_LBUTTONUP)
            {
                dragging = false;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) g_timelineZoom *= 0.9f;
            else g_timelineZoom *= 1.1f;
            if (g_timelineZoom < 0.0625f) g_timelineZoom = 0.0625f;  // 1/16x
            if (g_timelineZoom > 16.0f) g_timelineZoom = 16.0f;       // 16x for millisecond precision
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_RBUTTONDOWN:
        {
            if (!g_renderer || g_renderer->getJsonAnimationDuration() <= 0.0f)
                break;
            int mx = (short)LOWORD(lParam);
            g_contextMenuTime = TimelinePixelToTime(hwnd, mx);

            HMENU hMenu = CreatePopupMenu();
            char label[128];
            sprintf_s(label, "Add SoundEvent at %.3fs", g_contextMenuTime);
            AppendMenu(hMenu, MF_STRING, IDM_ADD_SOUND_EVENT, label);
            sprintf_s(label, "Add SoundCue at %.3fs", g_contextMenuTime);
            AppendMenu(hMenu, MF_STRING, IDM_ADD_SOUND_CUE, label);
            if (g_hoveredEventIndex >= 0)
            {
                AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
                char evtBuf[256];
                g_renderer->getJsonAnimEventSummary(g_hoveredEventIndex, evtBuf, sizeof(evtBuf));
                sprintf_s(label, "Delete: %s", evtBuf);
                // Truncate if too long for menu
                if (strlen(label) > 80) { label[77] = '.'; label[78] = '.'; label[79] = '.'; label[80] = '\0'; }
                AppendMenu(hMenu, MF_STRING, IDM_DELETE_EVENT, label);
            }

            POINT pt = { mx, (short)HIWORD(lParam) };
            ClientToScreen(hwnd, &pt);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);

            if (cmd == IDM_ADD_SOUND_EVENT || cmd == IDM_ADD_SOUND_CUE)
            {
                AddEventDlgData dlgData;
                memset(&dlgData, 0, sizeof(dlgData));
                dlgData.isSoundCue = (cmd == IDM_ADD_SOUND_CUE);
                dlgData.time = g_contextMenuTime;
                if (ShowAddEventDialog(hwnd, dlgData))
                {
                    g_renderer->addJsonAnimEvent(dlgData.isSoundCue, dlgData.time,
                                                 dlgData.crc0, dlgData.crc1, dlgData.crc2);
                    InvalidateRect(hwnd, NULL, FALSE);
                    if (g_textStatus)
                    {
                        char msg[256];
                        sprintf_s(msg, "Status: Added %s \"%s\" at t=%.3fs",
                                  dlgData.isSoundCue ? "SoundCue" : "SoundEvent",
                                  dlgData.crc0, dlgData.time);
                        SetWindowText(g_textStatus, msg);
                    }
                }
            }
            else if (cmd == IDM_DELETE_EVENT && g_hoveredEventIndex >= 0)
            {
                g_renderer->removeJsonAnimEvent(g_hoveredEventIndex);
                g_hoveredEventIndex = -1;
                InvalidateRect(hwnd, NULL, FALSE);
                if (g_textStatus)
                    SetWindowText(g_textStatus, "Status: Event deleted");
            }
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillVerticalGradient(hdc, rc, kClrPanelTop, kClrPanelBot);
            // top highlight for glass effect
            RECT hl = rc; hl.bottom = hl.top + 1;
            HBRUSH hBrush = CreateSolidBrush(RGB(255,255,255));
            FillRect(hdc, &hl, hBrush);
            DeleteObject(hBrush);

            if (g_renderer)
            {
                float dur = g_renderer->getJsonAnimationDuration();
                float t = g_renderer->getJsonAnimationTime();
                int frames = g_renderer->getJsonAnimationFrameCount();
                int margin = 8;
                int barH = (rc.bottom - rc.top) - 14;
                RECT bar = { rc.left + margin, rc.top + 6, rc.right - margin, rc.top + 6 + barH };
                HBRUSH barBrush = CreateSolidBrush(RGB(40, 46, 54));
                FillRect(hdc, &bar, barBrush);
                DeleteObject(barBrush);

                if (dur > 0.0f)
                {
                    float visible = dur / g_timelineZoom;
                    if (visible <= 0.0f) visible = dur;
                    float scale = (float)(bar.right - bar.left) / visible;
                    // ticks
                    if (frames > 0 && frames < 400)
                    {
                        HPEN penTick = CreatePen(PS_SOLID, 1, RGB(80, 90, 104));
                        HPEN old = (HPEN)SelectObject(hdc, penTick);
                        for (int i = 0; i < frames; ++i)
                        {
                            float ft = (dur / (float)(frames - 1)) * (float)i;
                            int x = bar.left + (int)(ft * scale);
                            MoveToEx(hdc, x, bar.top, NULL);
                            LineTo(hdc, x, bar.top + 6);
                        }
                        SelectObject(hdc, old);
                        DeleteObject(penTick);
                    }

                    // Draw keyframe indicators if selected bone exists
                    if (g_renderer && g_renderer->getSelectedBoneIndex() >= 0)
                    {
                        int boneIdx = g_renderer->getSelectedBoneIndex();
                        int rotKeyCount = g_renderer->getEditorRotKeyCount(boneIdx);
                        int transKeyCount = g_renderer->getEditorTransKeyCount(boneIdx);

                        // Draw rotation keyframe markers (small squares)
                        if (rotKeyCount > 0)
                        {
                            HPEN penKeyRot = CreatePen(PS_SOLID, 1, RGB(200, 100, 100));
                            HPEN old = (HPEN)SelectObject(hdc, penKeyRot);
                            for (int i = 0; i < rotKeyCount; ++i)
                            {
                                float keyTimeMs = g_renderer->getEditorRotKeyTime(boneIdx, i);
                                float keyTimeS = keyTimeMs / 1000.0f;
                                if (keyTimeS >= 0.0f && keyTimeS <= dur)
                                {
                                    int kx = bar.left + (int)(keyTimeS * scale);
                                    int ky = (bar.top + bar.bottom) / 2 - 3;
                                    RECT krect = { kx - 2, ky - 2, kx + 2, ky + 2 };
                                    HBRUSH kBrush = CreateSolidBrush(RGB(200, 100, 100));
                                    FillRect(hdc, &krect, kBrush);
                                    DeleteObject(kBrush);
                                }
                            }
                            SelectObject(hdc, old);
                            DeleteObject(penKeyRot);
                        }

                        // Draw translation keyframe markers (small diamonds)
                        if (transKeyCount > 0)
                        {
                            HPEN penKeyTrans = CreatePen(PS_SOLID, 1, RGB(100, 200, 100));
                            for (int i = 0; i < transKeyCount; ++i)
                            {
                                float keyTimeMs = g_renderer->getEditorTransKeyTime(boneIdx, i);
                                float keyTimeS = keyTimeMs / 1000.0f;
                                if (keyTimeS >= 0.0f && keyTimeS <= dur)
                                {
                                    int kx = bar.left + (int)(keyTimeS * scale);
                                    int ky = (bar.top + bar.bottom) / 2 + 3;
                                    POINT kpts[4] = {
                                        { kx, ky - 2 },
                                        { kx + 2, ky },
                                        { kx, ky + 2 },
                                        { kx - 2, ky }
                                    };
                                    HBRUSH kBrush = CreateSolidBrush(RGB(100, 200, 100));
                                    HPEN old = (HPEN)SelectObject(hdc, penKeyTrans);
                                    Polygon(hdc, kpts, 4);
                                    SelectObject(hdc, old);
                                    DeleteObject(kBrush);
                                }
                            }
                            DeleteObject(penKeyTrans);
                        }
                    }

                    // millisecond grid and labels when zoomed in
                    if (g_timelineZoom >= 2.0f)
                    {
                        HPEN penMs = CreatePen(PS_SOLID, 1, RGB(60, 70, 84));
                        HPEN old = (HPEN)SelectObject(hdc, penMs);

                        // Determine millisecond interval based on zoom level
                        float msInterval = 10.0f;  // Start with 10ms
                        if (g_timelineZoom < 4.0f) msInterval = 50.0f;
                        else if (g_timelineZoom < 8.0f) msInterval = 20.0f;
                        else if (g_timelineZoom >= 8.0f) msInterval = 5.0f;
                        else if (g_timelineZoom >= 12.0f) msInterval = 1.0f;  // At 12x+ zoom, show every millisecond

                        // Draw millisecond grid lines
                        float currentMs = 0.0f;
                        while (currentMs <= dur * 1000.0f)
                        {
                            float timeS = currentMs / 1000.0f;
                            if (timeS <= dur)
                            {
                                int x = bar.left + (int)(timeS * scale);
                                MoveToEx(hdc, x, bar.top + 2, NULL);
                                LineTo(hdc, x, bar.top + 4);
                            }
                            currentMs += msInterval;
                        }

                        SelectObject(hdc, old);
                        DeleteObject(penMs);

                        // Draw millisecond labels for major intervals (even more detailed when zoomed)
                        if (g_timelineZoom >= 8.0f)
                        {
                            SetTextColor(hdc, RGB(160, 170, 180));
                            SetBkMode(hdc, TRANSPARENT);

                            // At very high zoom (12+), show every 10ms
                            float labelInterval = (g_timelineZoom >= 12.0f) ? 10.0f : (msInterval * 10.0f);
                            float currentMs2 = 0.0f;

                            while (currentMs2 <= dur * 1000.0f)
                            {
                                float timeS = currentMs2 / 1000.0f;
                                if (timeS <= dur)
                                {
                                    int x = bar.left + (int)(timeS * scale);
                                    char msLabel[32];

                                    // Format: show decimals for sub-10ms precision
                                    if (labelInterval < 10.0f)
                                        sprintf_s(msLabel, sizeof(msLabel), "%.1fms", currentMs2);
                                    else
                                        sprintf_s(msLabel, sizeof(msLabel), "%.0fms", currentMs2);

                                    TextOut(hdc, x - 20, bar.bottom + 2, msLabel, (int)strlen(msLabel));
                                }
                                currentMs2 += labelInterval;
                            }

                            // At extreme zoom, also show frame boundaries
                            if (g_timelineZoom >= 12.0f && frames > 0)
                            {
                                HPEN penFrame = CreatePen(PS_DOT, 1, RGB(100, 120, 140));
                                HPEN oldPen = (HPEN)SelectObject(hdc, penFrame);
                                SetTextColor(hdc, RGB(120, 140, 160));

                                for (int i = 0; i < frames && i < 100; ++i)  // Limit to avoid clutter
                                {
                                    float ft = (dur / (float)(frames - 1)) * (float)i;
                                    int x = bar.left + (int)(ft * scale);
                                    MoveToEx(hdc, x, bar.top, NULL);
                                    LineTo(hdc, x, bar.bottom);

                                    char frameLabel[32];
                                    sprintf_s(frameLabel, sizeof(frameLabel), "F%d", i);
                                    TextOut(hdc, x + 2, bar.top - 12, frameLabel, (int)strlen(frameLabel));
                                }

                                SelectObject(hdc, oldPen);
                                DeleteObject(penFrame);
                            }
                        }
                    }
                    // event markers
                    {
                        static const COLORREF catColors[9] = {
                            RGB(0xE0, 0x40, 0x40), // 0 damage  - red
                            RGB(0x40, 0xE0, 0xC0), // 1 trail   - cyan
                            RGB(0xE0, 0xD0, 0x40), // 2 sound   - yellow
                            RGB(0xB0, 0x60, 0xE0), // 3 camera  - purple
                            RGB(0x60, 0xC0, 0x60), // 4 state   - green
                            RGB(0xE0, 0x90, 0x30), // 5 projectile - orange
                            RGB(0xE0, 0x70, 0xA0), // 6 throw   - pink
                            RGB(0x90, 0x70, 0x40), // 7 bow     - brown
                            RGB(0x90, 0x90, 0x90), // 8 controller - gray
                        };
                        int evtCount = g_renderer->getJsonAnimEventCount();
                        int midY = (bar.top + bar.bottom) / 2;
                        for (int i = 0; i < evtCount; ++i)
                        {
                            float et = g_renderer->getJsonAnimEventTime(i);
                            int ex = bar.left + (int)(et * scale);
                            int cat = g_renderer->getJsonAnimEventCategory(i);
                            if (cat < 0 || cat > 8) cat = 4;
                            COLORREF clr = catColors[cat];
                            bool hovered = (i == g_hoveredEventIndex);
                            int sz = hovered ? 5 : 3;
                            // draw diamond
                            POINT pts[4] = {
                                { ex, midY - sz },
                                { ex + sz, midY },
                                { ex, midY + sz },
                                { ex - sz, midY }
                            };
                            HBRUSH eBrush = CreateSolidBrush(clr);
                            HPEN ePen = CreatePen(PS_SOLID, 1, hovered ? RGB(255,255,255) : clr);
                            HBRUSH oldBr = (HBRUSH)SelectObject(hdc, eBrush);
                            HPEN oldPn = (HPEN)SelectObject(hdc, ePen);
                            Polygon(hdc, pts, 4);
                            SelectObject(hdc, oldBr);
                            SelectObject(hdc, oldPn);
                            DeleteObject(eBrush);
                            DeleteObject(ePen);
                        }
                    }

                    // playhead
                    int px = bar.left + (int)(t * scale);
                    HPEN penHead = CreatePen(PS_SOLID, 2, kClrAccent);
                    HPEN old = (HPEN)SelectObject(hdc, penHead);
                    MoveToEx(hdc, px, bar.top, NULL);
                    LineTo(hdc, px, bar.bottom);
                    SelectObject(hdc, old);
                    DeleteObject(penHead);

                    // info text + hovered event tooltip
                    char info[256];
                    float tMs = t * 1000.0f;
                    float durMs = dur * 1000.0f;
                    if (g_hoveredEventIndex >= 0)
                    {
                        char evtSummary[256];
                        g_renderer->getJsonAnimEventSummary(g_hoveredEventIndex, evtSummary, sizeof(evtSummary));
                        sprintf(info, "t=%.1fms (%.3fs) / %.1fms  zoom=%.2fx  | %s", tMs, t, durMs, g_timelineZoom, evtSummary);
                    }
                    else
                    {
                        sprintf(info, "t=%.1fms (%.3fs) / %.1fms  zoom=%.2fx  [%d events]", tMs, t, durMs, g_timelineZoom,
                                g_renderer->getJsonAnimEventCount());
                    }
                    SetTextColor(hdc, g_hoveredEventIndex >= 0 ? kClrAccent : kClrText);
                    SetBkMode(hdc, TRANSPARENT);
                    TextOut(hdc, bar.left, bar.bottom + 1, info, (int)strlen(info));
                }
                else
                {
                    const char* msg = "No animation loaded";
                    SetTextColor(hdc, kClrTextDim);
                    SetBkMode(hdc, TRANSPARENT);
                    TextOut(hdc, rc.left + margin, rc.top + 6, msg, (int)strlen(msg));
                }
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
