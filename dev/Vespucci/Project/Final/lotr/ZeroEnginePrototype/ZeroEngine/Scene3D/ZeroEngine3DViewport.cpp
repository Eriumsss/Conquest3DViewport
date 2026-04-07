// ZeroEngine3DViewport.cpp — The Main Window. The Portal. Ground Fucking Zero.
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// This is the entry point for the entire 3D viewport — the WinMain,
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
// Pandemic's original viewer was called "ZeroEdit" — an internal tool
// built on top of their Magellan engine. We found "ZeroEdit.exe" in
// the .exe's import table and "ZeroEditor.log" in the string table.
// This file is our version. Built on their corpse. Using their SDK.
// Running their shaders. Loading their levels. We didn't just reverse-
// engineer their game — we rebuilt their fucking TOOLS.
//
// "The two most important days in your life are the day you were born
// and the day you find out why." — Mark Twain. I found out why on the
// day this viewport first rendered a Conquest level at 60fps. That was
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
#include "LevelInspector.h"
#include "Scene3DLoader.h"
#include "HavokToDisplayConverter.h"
#include "AnimationSystem.h"
#include "AssetBrowser.h"
#include "LuaAnimationRuntime.h"
#include "LevelReader.h"
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
static ZeroEngine::LevelReader g_levelReader;
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
static int                      g_savePakStatus = 0;    // 0=idle, 1-4=in progress, 5=done, -1=error
static bool                     g_pendingSceneReload = false; // set after SavePak to trigger full reload next frame
static std::string              g_savePakMessage;
static float                    g_savePakDoneTimer = 0; // seconds to show "done" message

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

// ============================================================================
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
static bool RunSubprocess(const char* cmdLine, int timeoutMs = 30000)
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

    if (!CreateProcessA(NULL, cmdBuf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, (DWORD)timeoutMs);
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exitCode);

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
        if (GetFileAttributesA(candidates[i]) != INVALID_FILE_ATTRIBUTES)
            return std::string(candidates[i]);
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

    // Save PAK status for progress display
    if (g_savePakStatus == 5 || g_savePakStatus == -1) {
        g_savePakDoneTimer -= 0.016f; // ~60fps estimate
        if (g_savePakDoneTimer <= 0) g_savePakStatus = 0;
    }
    args.savePakStatus = g_savePakStatus;
    args.savePakMessage = g_savePakMessage.empty() ? NULL : g_savePakMessage.c_str();

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
    if (g_renderer && g_renderer->hasLevelScene()) {
        LevelScene* ls = g_renderer->getLevelScene();
        args.collisionVisEnabled = ls ? (ls->collisionVisEnabled() ? 1 : 0) : 0;
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
                g_mvRenderTarget->GetSurfaceLevel(0, &g_mvRTSurface);
                dev->CreateDepthStencilSurface(g_mvTexW, g_mvTexH,
                    D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
                    &g_mvDepthSurface, NULL);
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
                dev->GetRenderTarget(0, &oldRT);
                dev->GetDepthStencilSurface(&oldDS);

                // Set model viewer render target
                dev->SetRenderTarget(0, g_mvRTSurface);
                dev->SetDepthStencilSurface(g_mvDepthSurface);
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

                // Render mode
                if (g_mvWireframe) {
                    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
                } else {
                    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
                }
                dev->SetRenderState(D3DRS_LIGHTING, FALSE);
                dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
                dev->SetRenderState(D3DRS_ZENABLE, TRUE);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

                // Draw each mesh part
                dev->SetVertexShader(NULL);
                dev->SetPixelShader(NULL);
                dev->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

                for (int pi = 0; pi < (int)mdl->parts.size(); ++pi) {
                    const LevelMeshPart& part = mdl->parts[pi];
                    if (!part.vb || !part.ib || part.vertexCount == 0 || part.indexCount == 0) continue;
                    dev->SetTexture(0, part.diffuseTex);
                    dev->SetStreamSource(0, part.vb, 0, sizeof(LevelVertex));
                    dev->SetIndices(part.ib);
                    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                        part.vertexCount, 0, part.indexCount / 3);
                }

                // Draw collision wireframe overlay if enabled
                if (g_mvShowCollision && g_mvHasCollision && !g_mvCollVerts.empty()) {
                    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
                    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
                    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                    dev->SetTexture(0, NULL);
                    dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
                    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);

                    struct CWireVert { float x,y,z; DWORD col; };
                    int numVerts = (int)(g_mvCollVerts.size() / 3);
                    std::vector<CWireVert> wireVerts(numVerts);
                    for (int vi = 0; vi < numVerts; ++vi) {
                        wireVerts[vi].x = g_mvCollVerts[vi*3+0];
                        wireVerts[vi].y = g_mvCollVerts[vi*3+1];
                        wireVerts[vi].z = g_mvCollVerts[vi*3+2];
                        wireVerts[vi].col = 0xFF00FF00; // green wireframe
                    }
                    dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, numVerts,
                        (int)(g_mvCollIndices.size() / 3), &g_mvCollIndices[0],
                        D3DFMT_INDEX16, &wireVerts[0], sizeof(CWireVert));
                }

                // Restore state
                dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
                dev->SetRenderTarget(0, oldRT);
                dev->SetDepthStencilSurface(oldDS);
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
    int propEntityIdx = -1;
    if (g_levelReader.IsLoaded() && g_levelReader.GetGameObjs().size() > 0) {
        // Try selected editor object first
        if (args.selectedEditorObjGuid != 0) {
            const std::vector<ZeroEngine::LevelGameObjEntry>& objs = g_levelReader.GetGameObjs();
            for (int i = 0; i < (int)objs.size(); ++i) {
                if (objs[i].guid == args.selectedEditorObjGuid) { propEntityIdx = i; break; }
            }
        }
        // Fallback: selected mesh instance
        if (propEntityIdx < 0 && args.inspectorSelectedIdx >= 0 && args.inspectorSelGuid != 0) {
            const std::vector<ZeroEngine::LevelGameObjEntry>& objs = g_levelReader.GetGameObjs();
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

    fpImGui_DrawFrame(&args);

    // Handle property edit write-back
    if (args.propEditRequested && propEntityIdx >= 0 && g_levelReader.IsLoaded()) {
        const std::vector<ZeroEngine::LevelGameObjEntry>& objs = g_levelReader.GetGameObjs();
        const ZeroEngine::LevelGameObjEntry& ent = objs[propEntityIdx];
        int fi = args.propEditFieldIndex;
        if (fi >= 0 && fi < args.propFieldCount) {
            uint32_t absOff = ent.block1_obj_offset + args.propFieldOffsets[fi];
            switch (args.propEditKind) {
                case 0: g_levelReader.SetFieldValue(absOff, &args.propEditIntVal, 4); break;
                case 1: g_levelReader.SetFieldValue(absOff, &args.propEditFloatVal, 4); break;
                case 2: g_levelReader.SetFieldValue(absOff, &args.propEditGuidVal, 4); break;
                case 3: g_levelReader.SetFieldValue(absOff, args.propEditVec3Val, 12); break;
                case 4: g_levelReader.SetFieldValue(absOff, args.propEditMatrixVal, 64); break;
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
        g_goGuids.clear(); g_goParentGuids.clear(); g_goLayerGuids.clear(); g_goGamemodeMasks.clear();
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
                g_goPosX.resize(n);
                g_goPosY.resize(n);
                g_goPosZ.resize(n);
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
            if (args.inspectorPosChanged && ls->selectedInstance() >= 0) {
                int selIdx = ls->selectedInstance();
                ls->setInstancePosition(selIdx,
                    args.inspectorNewPos[0], args.inspectorNewPos[1], args.inspectorNewPos[2]);

                // Also update Block1 buffer so Save writes the new position
                const LevelInstance* inst = ls->getInstance(selIdx);
                if (inst && inst->block1WtOffset > 0) {
                    g_levelReader.SetWorldTransform(inst->block1WtOffset, inst->mat);
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
        g_savePakMessage = "Rebuilding Block1...";

        // Always save to the same _modified path (derived from original level name)
        // This avoids _modified_modified_modified... chains
        std::string savePath = g_levelReader.GetPakPath();
        // Strip existing _modified suffix to get base name
        size_t modPos = savePath.find("_modified");
        if (modPos != std::string::npos) {
            savePath = savePath.substr(0, modPos) + savePath.substr(modPos + 9); // remove "_modified"
        }
        size_t dotPos = savePath.rfind('.');
        if (dotPos != std::string::npos)
            savePath.insert(dotPos, "_modified");
        else
            savePath += "_modified";

        s_savingInProgress = true;

        // Unload 3D scene BEFORE save (SavePak does Unload+Load internally)
        if (g_renderer) g_renderer->unloadLevelScene();

        if (g_levelReader.SavePak(savePath)) {
            // C++ wrote the PAK — now sanitize through Rust parser (dump+recompile)
            // This fixes any structural issues our C++ rebuilder produces
            if (RustParserSanitize(savePath, g_levelReader.GetBinPath())) {
                g_savePakStatus = 5;
                g_savePakMessage = "Saved & sanitized: " + savePath;
                g_savePakDoneTimer = 3.0f;
            } else {
                // Sanitize failed but C++ save succeeded — file exists, just not cleaned
                if (g_savePakStatus != -1) { // don't overwrite error status
                    g_savePakStatus = 5;
                    g_savePakMessage = "Saved (no sanitize): " + savePath;
                    g_savePakDoneTimer = 3.0f;
                }
            }
        } else {
            g_savePakStatus = -1;
            g_savePakMessage = "Save FAILED: " + g_levelReader.GetError();
            g_savePakDoneTimer = 5.0f;
        }
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

        // Refresh gameObj pointer arrays so the new entity appears in UI this frame
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
            g_goPosX.resize(n);
            g_goPosY.resize(n);
            g_goPosZ.resize(n);
            // Only fill the last entry (new entity)
            size_t i = n - 1;
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
            // Rebuild all pointer arrays (c_str() pointers may have been invalidated by resize)
            g_goNamePtrs.resize(n);
            g_goTypeNamePtrs.resize(n);
            g_goMeshNamePtrs.resize(n);
            for (size_t j = 0; j < n; ++j) {
                g_goNamePtrs[j]     = g_goNameStrings[j].c_str();
                g_goTypeNamePtrs[j] = g_goTypeNameStrings[j].c_str();
                g_goMeshNamePtrs[j] = g_goMeshNameStrings[j].c_str();
            }
            // Extend event/relationship arrays with defaults for the new entity
            g_goTargetGuids.push_back(0);
            g_goOutputEventStrings.push_back("");
            g_goInputEventStrings.push_back("");
            g_goModeNameStrings.push_back("");
            g_goDelays.push_back(0);
            g_goSticky.push_back(0);
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

            // Build paths to WHAM python (absolute paths to avoid CWD issues)
            std::string whamDir = "C:\\Users\\Yusuf\\Desktop\\Oyun\\The.Lord.of.the.Rings.Conquest"
                "\\The Lord of the Rings - Conquest\\dev\\Vespucci\\Project\\Final\\lotr"
                "\\ZeroEnginePrototype\\ZeroEngine\\Engine\\source\\Vendors\\WHAM";
            std::string pythonExe = "C:\\Users\\Yusuf\\Miniconda3\\envs\\wham311\\python.exe";

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
        static const char* baseDir =
            "C:\\Users\\Yusuf\\Desktop\\Oyun\\The.Lord.of.the.Rings.Conquest"
            "\\The Lord of the Rings - Conquest\\dev\\Vespucci\\Project\\Final"
            "\\lotr\\ZeroEnginePrototype\\ZeroEngine";

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

            // Check Downloads folder
            if (pklPath.empty())
            {
                WIN32_FIND_DATAA fd;
                HANDLE hFind = FindFirstFileA("C:\\Users\\Yusuf\\Downloads\\*", &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && fd.cFileName[0] != '.') {
                            char candidate[1024];
                            sprintf(candidate, "C:\\Users\\Yusuf\\Downloads\\%s\\wham_output.pkl", fd.cFileName);
                            if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
                                pklPath = candidate;
                            }
                        }
                    } while (FindNextFileA(hFind, &fd));
                    FindClose(hFind);
                }
            }

            if (!pklPath.empty())
            {
                char cmd[4096];
                sprintf(cmd,
                    "\"C:\\Users\\Yusuf\\Miniconda3\\envs\\wham311\\python.exe\" "
                    "\"%s\\Engine\\source\\Vendors\\WHAM\\smpl_to_conquest.py\" "
                    "--input \"%s\" --subject 0 --name \"RH6_mocap_capture\" "
                    "--output \"%s\\GameFiles\\animations\\RH6_mocap_capture.json\"",
                    baseDir, pklPath.c_str(), baseDir);
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

    // Initialize Wwise audio subsystem
    DebugLog("Initializing audio (Wwise SDK)...");
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

void RenderFrame(float deltaTime)
{
    if (!g_renderer) return;

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
            bool pickClick = altHeld && lmbClicked;
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
                g_leftMouseDown = !g_editMode;
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
                g_rightMouseDown = !g_editMode;
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
