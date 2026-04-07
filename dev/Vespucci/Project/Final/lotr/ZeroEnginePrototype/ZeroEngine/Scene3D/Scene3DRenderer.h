// Scene3DRenderer.h — The Beating Heart of This Whole Goddamn Operation
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Hours lost to this file alone: incalculable. Maybe 400. Maybe more.
// Cigarettes smoked while debugging stepAndRender(): entire cartons.
//
// This is the main rendering pipeline for ZeroEngine — a reverse-engineered
// reimplementation of LOTR: Conquest's display system, built on top of a
// Havok 5.5.0 SDK that was obtained through... let's call it "digital
// archaeology." The kind where you dig through dead torrents at 3 AM and
// pray the ZIP isn't corrupted. Havok 5.5.0 hasn't been officially
// distributed since like 2009. Microsoft bought Havok and buried every
// old version in a shallow grave. We dug it back up. Sorry not sorry.
//
// The rendering pattern here is copied from Havok's own demo framework
// (hkDemo.h, PyramidDemo, SkinningDemo, etc.) because Pandemic Studios
// used the exact same pattern in Conquest. Those poor bastards at Pandemic
// built this engine knowing EA was about to shut them down. Every function
// in here carries their fingerprints — the naming conventions, the way
// the camera orbits, the goddamn light manager setup. All Pandemic DNA.
// They called it "Magellan" internally. We call it Vespucci now because
// we're rebuilding it from zero, from the ashes of a studio that EA
// murdered in 2009.
//
// This file is 1400+ lines because Scene3DRenderer does EVERYTHING:
// rendering, animation, camera, physics playtest, skeletal editing,
// motion matching, IK, particle effects, level loading, skyboxes...
// It's a god class. I know. Pandemic made it this way. I inherited the
// sin. Refactoring it would break every .obj file we've compiled and
// I'd rather eat glass than recompile Scene3D from scratch.
//
// Based on: havok/hk550/Demo/Demos/DemoCommon/DemoFramework/hkDemo.h
// -----------------------------------------------------------------------

#ifndef SCENE3D_RENDERER_H
#define SCENE3D_RENDERER_H

// MOPP BVTree builder — Havok's midphase collision acceleration structure.
// Pandemic used MOPP trees everywhere for level collision in Conquest.
// This function takes raw triangle soup and produces a MOPP bytecode blob
// that Havok's broadphase can chew on. Implemented in Scene3DCamera.cpp
// because that's where the physics playtest lives, and honestly by the time
// I realized it didn't belong there, 14 other files were already including
// this header. Moving it would break the stolen Havok 5.5 link chain.
// outMoppBytes/outMoppSize: caller provides pointer, function allocates with malloc().
// Caller must free() the returned buffer.
bool BuildMoppFromMesh(const float* vertXYZ, int numVerts,
                       const unsigned short* indices, int numTris,
                       unsigned char** outMoppBytes, int* outMoppSize);

// Windows API — because of course we need the entire fucking Win32 surface
#include <windows.h>

// Havok v5.5.0 Core — the SDK that no longer exists on any official server.
// Downloaded from a torrent that had 2 seeders in 2019. One of them was
// probably a ghost. The other was probably me from a parallel timeline.
// If Havok/Microsoft ever comes knocking: this is for preservation purposes
// of a dead game from a dead studio. Take it up with EA.
#include <Common/Base/hkBase.h>
#include <Common/Base/Container/Array/hkArray.h>
#include <Common/Base/Container/Array/hkObjectArray.h>
#include <Common/Visualize/hkDebugDisplay.h>
#include <vector>
#include <set>
#include <map>

// Havok Graphics Bridge (v5.5.0) — the rendering layer Havok themselves abandoned.
// No documentation survived. The only examples were buried in demo code that
// Pandemic's engineers somehow made work. These headers pull in the entire
// hkg (Havok Graphics) subsystem which wraps D3D9. Pandemic used this exact
// bridge for Conquest's renderer. We're using their stolen playbook.
#include <Graphics/Bridge/System/hkgSystem.h>
#include <Graphics/Common/Window/hkgWindow.h>
#include <Graphics/Common/DisplayWorld/hkgDisplayWorld.h>
#include <Graphics/Common/DisplayContext/hkgDisplayContext.h>
#include <Graphics/Common/Camera/hkgCamera.h>
#include <Graphics/Common/Light/hkgLightManager.h>
#include <Graphics/Common/DisplayObject/hkgDisplayObject.h>
#include "InverseKinematics.h"
#include "LuaAnimationRuntime.h"
// NOTE: Scene3DRendererInternal.h moved to end of file (after class definition)
// to break circular include dependency

// Forward declare D3D9 device — DirectX 9, the API that refuses to die.
// Pandemic shipped Conquest on D3D9 in 2009. Here we are in 2026, still
// talking to the same goddamn interface. Microsoft deprecated it. NVIDIA
// keeps it on life support. We keep using it because the stolen Havok 5.5
// graphics bridge only speaks D3D9 and rewriting it for D3D11 would take
// longer than the time I have left on this earth.
struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
struct IDirect3DPixelShader9;
class hkaSkeleton;

// Forward declare DirectX 9 PC window class (fallback if hkgWindow::create is NULL)
class hkgWindowDX9PC {
public:
    static hkgWindow* createWindowDX9PC();
};

// Forward declarations
class Scene3DLoader;
class HavokToDisplayConverter;
class AnimatedCharacter;
class LevelScene;
namespace ZeroEngine { class LevelReader; }
class hkaPose;
struct JsonAnimClip;
struct AnimationGraphRuntime;
struct JsonAnimEvent;
struct JsonAnimEventVal;
enum AnimEventType;
struct GraphState;
struct BlendGraphRuntime;
struct JsonAnimEvent;
class hkgTexture;
struct GameModel;
struct ParticleEmitter;
class EffectManager;
class MgPackedParticleShaders;

// One-shot trigger reference for the animation graph state machine.
// When a graph transition fires a trigger, this struct remembers which
// graph and which parameter so we can reset it next frame. Pandemic's
// original Lua animation system did the same thing — triggers are
// consumed once then cleared, like a mousetrap. Except this mousetrap
// is inside a state machine inside a blend graph inside my nightmares
// and this shit does not even work properly and needs more work.
struct GraphTriggerRef
{
    AnimationGraphRuntime* graph;
    int paramIndex;

    GraphTriggerRef()
        : graph(NULL),
          paramIndex(-1)
    {}
};

struct CameraBookmark
{
    hkVector4 target;
    float yaw;
    float pitch;
    float roll;
    float distance;
    bool  valid;
};

// ===================================================================
// Scene3DRenderer — THE God Class. 1400+ lines of everything.
//
// This motherfucker IS the engine. Rendering, animation, camera,
// physics playtest, bone editing, IK, motion matching, particle
// effects, level loading, skyboxes, mocap bridge — it's all here
// in one class because that's how Pandemic's engineers at the old
// Los Angeles studio structured their demo framework, and I was
// too deep in by the time I realized what I'd inherited.
//
// Faithfully recreates the rendering loop from Havok's demo framework
// (the one Pandemic copied for Conquest). Every stepAndRender() call
// follows the exact same step→render→present sequence that shipped
// on the Xbox 360 disc in January 2009. Those Pandemic devs knew
// what they were doing. They just didn't know EA was about to put
// a bullet in the studio 10 months later.
//
// The Havok 5.5 SDK we're linking against? Obtained through means
// that would make a lawyer yap. The Wwise SDK references? Same
// story. This entire codebase is held together by stolen middleware
// from dead companies and the desperate prayers of one developer
// who hasn't slept properly since 2019.
//
// If you're reading this and you worked at Pandemic: I'm sorry and
// thank you. Your code was better than you knew. Someone remembered.
// ===================================================================
/**
 * Scene3DRenderer - Havok v5.5.0 rendering pipeline
 *
 * This class faithfully recreates the rendering loop pattern used across pretty much
 * every Havok demo from 2008–2010. I had to read through PyramidDemo, ReachingDemo,
 * SkinningDemo, CharacterProxyVsRigidBodyDemo… and then rebuild half of it because
 * the original demo framework was never meant to live this long.
 * 
 * Why so many demos? Because Havok never shipped a single coherent renderer example.
 * So we copy and paste pieces from all of them and pretend it’s “consistent”.
 * 
 *   Rendering Loop:
 *   1. stepDemo() - Update animations/physics
 *   2. render() - Render display world
 *   3. swapBuffers() - Present frame
 * 
 *  Real-world usage (the sequence I type in my sleep now):
 * 
 *   Scene3DRenderer renderer;                          // new renderer, same suffering
 *   renderer.initialize(hwnd, width, height);          // set up window, context, Havok display world
 *   renderer.loadScene("Training/Art/mesh.hkx");       // load whatever .hkx scene survived the import
 *   
 *   In the message loop / game tick:
 *   renderer.stepAndRender(deltaTime);                 // one function to rule them all
 *
 *    Why stepAndRender() exists:
 *    I got tired of calling three functions manually every frame and debugging
 *    stepAndRender() is just stepDemo() + render() + swapBuffers() in one call
 *    because splitting them made debugging three times harder
 *    If initialize() fails → usually bad HWND or driver hates us
 *    If loadScene() crashes → welcome to the wonderful world of malformed .hkx files
 */
class Scene3DRenderer {
public:
    // ---------------------------------------------------------------
    // Enums — named constants so we stop guessing what 0, 1, 2 means
    // at 4 AM. Every one of these replaced a magic integer that some
    // Pandemic engineer hardcoded in 2007 and left for us to decipher.
    // ---------------------------------------------------------------
    enum GizmoMode      { GIZMO_ROTATE    = 0, GIZMO_TRANSLATE  = 1 };
    enum GizmoSpace     { GIZMO_LOCAL     = 0, GIZMO_WORLD      = 1 };
    enum AxisLock       { AXIS_FREE = 0, AXIS_X = 1, AXIS_Y = 2, AXIS_Z = 3 };
    enum InterpMode     { INTERP_HOLD     = 0, INTERP_LINEAR    = 1 };
    enum SkyRenderMode  { SKY_BACKDROP    = 0, SKY_MESH         = 1, SKY_HYBRID = 2 };
    enum RootMotionMode { ROOT_MOTION_FULL = 0, ROOT_MOTION_CLAMP_Y = 1, ROOT_MOTION_OFF = 2, ROOT_MOTION_EXTRACT = 3 };
    enum RootMotionWarpMode { ROOT_WARP_NONE = 0, ROOT_WARP_SCALE = 1, ROOT_WARP_REDIRECT = 2, ROOT_WARP_SCALE_REDIRECT = 3 };
    enum GroundClampMode{ GROUND_CLAMP_OFF = 0, GROUND_CLAMP_BELOW = 1, GROUND_CLAMP_SNAP = 2 };
    enum RotApplyMode   { ROT_REF_DELTA   = 0, ROT_DELTA_REF    = 1, ROT_DELTA_ONLY = 2 };
    enum JsonBlendMode  { JSON_BLEND_CROSSFADE = 0, JSON_BLEND_LAYERED = 1, JSON_BLEND_ADDITIVE = 2 };
    enum JsonBlendRotMode { JSON_BLEND_ROT_SLERP = 0, JSON_BLEND_ROT_NLERP = 1 };
    enum RotInterpMode { ROT_INTERP_SLERP = 0, ROT_INTERP_NLERP = 1 };
    enum GraphParamType { GRAPH_PARAM_FLOAT = 0, GRAPH_PARAM_INT = 1, GRAPH_PARAM_BOOL = 2, GRAPH_PARAM_TRIGGER = 3 };

    // ---------------------------------------------------------------
    // Editor keyframe structs — the bone-level animation editing system.
    // This is a full keyframe editor bolted onto a reverse-engineered
    // game engine. Pandemic had their own DCC tools at the studio.
    // We have this. It works. It took mass amounts of caffeine to build.
    // Rotation keys store quaternions (x,y,z,w) unaligned because
    // std::vector and hkQuaternion's 16-byte alignment don't mix.
    // ---------------------------------------------------------------
    struct EditorKey
    {
        int frame;
        float timeMs;     // Exact millisecond time for sub-frame precision
        float rot[4]; // x,y,z,w (avoids aligned hkQuaternion in std::vector)
        int easingType;   // EasingType from AnimationCurve.h
        float easingCp1x; // Bezier control point 1 X (for custom Bezier curves)
        float easingCp1y; // Bezier control point 1 Y
        float easingCp2x; // Bezier control point 2 X
        float easingCp2y; // Bezier control point 2 Y
    };

    // Per-channel (1D) key for editor curves (position/scale channels).
    // Tangents are slopes in value-per-second, matching common DCC conventions.
    enum EditorCurveInterpMode { CURVE_CONSTANT = 0, CURVE_LINEAR = 1, CURVE_CUBIC = 2 };
    struct EditorFloatKey
    {
        int frame;
        float timeMs;     // Exact millisecond time for sub-frame precision
        float value;
        float inTangent;
        float outTangent;
        int interpMode;   // EditorCurveInterpMode (mode for segment starting at this key)
        int easingType;   // EasingType from AnimationCurve.h
        float easingCp1x; // Bezier control point 1 X
        float easingCp1y; // Bezier control point 1 Y
        float easingCp2x; // Bezier control point 2 X
        float easingCp2y; // Bezier control point 2 Y
    };
    struct EditorTransOverride
    {
        float x, y, z;
    };
    struct PoseRot
    {
        float x, y, z, w;
    };
    struct PoseSnapshot
    {
        std::vector<PoseRot> rot;
        std::vector<EditorTransOverride> trans;
    };
    struct PoseLibraryEntry
    {
        bool valid;
        char name[64];
        PoseSnapshot pose;
    };
    // ---------------------------------------------------------------
    // Motion Matching — runtime pose search database.
    // Pandemic didn't ship motion matching in Conquest (they used
    // traditional state machines), but their animation system was
    // structured in a way that made it possible to bolt on. So we did.
    // Each frame in the database stores root position/velocity, foot
    // positions, and future trajectory points. At runtime we search
    // for the best matching frame and crossfade into it. The search
    // is brute-force linear because the database is small enough and
    // I'm too fucking tired to implement a KD-tree at this hour.
    // ---------------------------------------------------------------
    struct MotionMatchFeature
    {
        float rootPosX, rootPosZ;
        float rootVelX, rootVelZ;
        float facingX, facingZ;
        float leftFootX, leftFootY, leftFootZ;
        float rightFootX, rightFootY, rightFootZ;
        float trajX1, trajZ1;
        float trajX2, trajZ2;
        float trajX3, trajZ3;
    };
    struct MotionMatchFrame
    {
        const JsonAnimClip* clip;
        int clipIndex;
        int frameIndex;
        float time;
        MotionMatchFeature feature;
    };
    struct MotionMatchClipInfo
    {
        JsonAnimClip* clip;
        int startIndex;
        int frameCount;
        float frameTime;
        float duration;
    };
    struct MotionMatchDatabase
    {
        bool valid;
        int rootBoneIndex;
        int leftFootBoneIndex;
        int rightFootBoneIndex;
        std::vector<MotionMatchFrame> frames;
        std::vector<MotionMatchClipInfo> clips;
        MotionMatchDatabase()
            : valid(false),
              rootBoneIndex(-1),
              leftFootBoneIndex(-1),
              rightFootBoneIndex(-1)
        {}
    };
    // ---------------------------------------------------------------
    // Physical Animation / Ragdoll state.
    // Each bone gets its own physics state — position, velocity,
    // rotation, angular velocity. When ragdoll kicks in, the
    // skeleton stops being driven by animation and starts being
    // driven by gravity and whatever impulse you hit it with.
    // Pandemic used Havok Physics 5.5 for their ragdolls (same SDK
    // we grave-robbed). The constraint setup was buried so deep in
    // the .exe that I had to trace through 47 fucking call levels
    // in Ghidra just to find how they initialized bone limits.
    // Some of those constraints still don't match the original game.
    // The ragdoll sometimes folds in ways that would make an
    // orthopedic surgeon scream. But it works. Mostly. At 5 AM.
    // ---------------------------------------------------------------
    struct PhysBoneState
    {
        hkVector4 pos;
        hkVector4 vel;
        hkQuaternion rot;
        hkVector4 angVel;
        float lengthToParent;
        bool valid;
        PhysBoneState()
            : lengthToParent(0.0f), valid(false)
        {
            pos.setZero4();
            vel.setZero4();
            angVel.setZero4();
            rot.setIdentity();
        }
    };
    struct PendingImpulse
    {
        int boneIndex;
        hkVector4 linear;
        hkVector4 angular;
        bool valid;
        PendingImpulse() : boneIndex(-1), valid(false)
        {
            linear.setZero4();
            angular.setZero4();
        }
    };
    Scene3DRenderer();
    ~Scene3DRenderer();
    
    /**
     * Initialize the renderer
     * 
     * @param hwnd - Win32 window handle
     * @param width - Window width
     * @param height - Window height
     * @return true on success, false on failure
     * 
     * This creates:
     * - hkgDisplayContext (graphics context)
     * - hkgDisplayWorld (scene container)
     * - hkgCamera (perspective camera)
     * - hkgLightManager (lighting system)
     */
    bool initialize(HWND hwnd, int width, int height);
    
    /**
     * Shutdown the renderer and free resources
     */
    void shutdown();
    
    /**
     * Load a Havok scene file
     *
     * @param hkxFilename - Path to .hkx file
     * @return true on success, false on failure
     */
    bool loadScene(const char* hkxFilename);

/**
     * CreateTestScene – The procedural cube everyone keep coming back to
     *
     * When the entire Havok scene pipeline decides it hates us today,
     * we generate a simple cube by hand no .hkx required. so we have at least one triangle
     * 
     * Use when you just need to see if the renderer is alive.
     * If this doesn’t render → the problem is deeper than files.
     * If it does → we buy ourselves five more minutes before going back to the real pain.
     */
    void createTestScene();
    
    /**
     * Main rendering loop (same as all Havok demos)
     * 
     * @param deltaTime - How much time passed (we hope it's not negative this time)
     * 
     * * Follows the sacred Havok demo pattern to the letter, because deviating from it
     * usually means segfaults in places we can't debug without attaching at 4 a.m.
     * 
     * This performs:
     * 1. Update camera from input
     * 2. Clear framebuffer
     * 3. Render display world
     * 4. Swap buffers
     * Why it's one function now: splitting step/render/present made error tracking hell.
     * One function = one place to breakpoint when everything inevitably goes wrong again.
     */
    void stepAndRender(float deltaTime);
    
    /**
     * Handle window resize
     */
    void resize(int width, int height);
    
    /**
     * Get the display world
     */
    hkgDisplayWorld* getDisplayWorld() { return m_displayWorld; }
    
    /**
     * Get the display context
     */
    hkgDisplayContext* getDisplayContext() { return m_context; }
    
    /**
     * Get the camera
     */
    hkgCamera* getCamera() { return m_camera; }
    
    /**
     * Get the converter
     */
    HavokToDisplayConverter* getConverter() { return m_converter; }

    /**
     * Get the Direct3D 9 device (for texture loading / custom rendering)
     */
    IDirect3DDevice9* getD3DDevice() { return m_d3dDevice; }

    // Viewport 3D rendering toggle. When false, only clear+ImGui+Present run each frame.
    void setScene3dEnabled(bool v) { m_scene3dEnabled = v; }
    bool getScene3dEnabled() const { return m_scene3dEnabled; }

    // ImGui viewport (render-to-texture) support. When enabled, the 3D scene is rendered into
    // an offscreen D3D9 render target texture and displayed via ImGui::Image in the docking UI.
    void setImGuiViewportActive(bool active);
    void setImGuiViewportSize(int width, int height); // pixels (content region)
    IDirect3DTexture9* getImGuiViewportTexture();     // may create lazily
    void getImGuiViewportTextureSize(int& outW, int& outH) const;

    /**
     * Set the Direct3D 9 device (optional; useful when hk550 headers don't expose it)
     */
    void setD3DDevice(IDirect3DDevice9* device) { m_d3dDevice = device; }
    
    /**
     * Camera controls (same as Havok demos)
     */
    void setCameraPosition(const hkVector4& position);
    void setCameraTarget(const hkVector4& target);
    void setCameraUp(const hkVector4& up);
    void setCameraFOV(float fovDegrees);
    void setCameraNearFar(float nearPlane, float farPlane);
    
    /**
     * Mouse/keyboard input for camera control
     */
    void onMouseMove(int x, int y, bool leftButton, bool rightButton);
    void onMouseWheel(int delta);
    void orbitCamera(float dx, float dy);
    void panCamera(float dx, float dy);
    void dollyCamera(float delta);
    void focusSelection();
    void homeCamera();
    void setNavigationMode(int mode); // 1=Maya,2=Blender,3=Unreal-ish
    int  getNavigationMode() const { return m_navMode; }
    const char* getNavigationModeName() const;
    bool loadCameraIni(const char* path = "camera_nav.ini");
    void applyOrthoView(int viewId, bool invert = false); // 1=Front, 2=Side, 3=Top
    void setOrthoEnabled(bool ortho);
    bool getOrthoEnabled() const { return m_cameraOrtho; }
    void saveCameraBookmark(int slot); // slot 1..5
    bool loadCameraBookmark(int slot); // slot 1..5
    void toggleHorizonLock();
    bool getHorizonLock() const { return m_horizonLock; }
    void rollCamera(float deltaRadians);
    void onKeyDown(int keyCode);

    // Animation/pose utilities
    bool jumpToKeyOnSelectedBone(bool forward);
    void resetSelectedBoneToRest();
    void resetAllBonesToRest();
    void undoPoseEdit();
    void redoPoseEdit();
    void pushUndoSnapshot();
    void savePoseSlot(int slot, const char* name);
    bool applyPoseSlot(int slot, bool mirrorX);
    void getSelectedBoneKeyFrames(std::vector<int>& rotFrames, std::vector<int>& transFrames) const;

    // ---------------------------------------------------------------
    // Level Scene — loads an entire LOTR:Conquest multiplayer/campaign
    // map from the extracted PAK/BIN data. Thousands of mesh instances,
    // collision geometry, spawn points, the whole goddamn thing.
    // Pandemic packed these levels with a custom tool they called
    // "lotrcparser" (we found the string in the .exe). The level data
    // has been dead for 14 years and we brought it back to life.
    // Every time I load Minas Tirith I swear I can hear Pandemic's
    // level designers whispering from beyond the grave: "OH GOD WHICH WAY
    // WAS UP???" the transforms are wrong.
    // They are. Some objects are 40000 units underground.
    // I've been fighting it for months. It might be haunted.
    // ---------------------------------------------------------------
    bool loadLevelScene(const ZeroEngine::LevelReader& reader);
    void unloadLevelScene();
    bool hasLevelScene() const;
    int  levelSceneInstanceCount() const;
    int  levelSceneModelCount() const;
    int  levelSceneDrawCalls() const;
    LevelScene* getLevelScene() const { return m_levelScene; }

    // Fly camera — first-person noclip for exploring the dead levels of Conquest.
    // Walk through Minas Tirith at your own pace. Nobody's shooting at you.
    // Nobody's even here anymore. Just you and the ghosts of Pandemic's art team
    // who placed every single fucking barrel and torch by hand in 2008.
    void toggleFlyCamera();
    bool isFlyCameraActive() const { return m_flyCameraActive; }
    void setFlyCameraSpeed(float unitsPerSec) { m_flyCamSpeed = unitsPerSec; }
    float getFlyCameraSpeed() const { return m_flyCamSpeed; }
    void flyCameraMouseLook(int dx, int dy);

    // Physics playtest — walk around in the level with gravity and collision.
    // atleast thats what it tries to do.
    // Uses hkpCharacterProxy from the stolen Havok 5.5 Physics SDK.
    // Pandemic used the same'ish character controller in Conquest for all
    // playable heroes (Aragorn, Gandalf, etc). We're using it to walk
    // a camera through their dead levels. It's like visiting a crime scene
    // 14 years later. Everything is where they left it. Nothing has moved.
    // The collision meshes still work. The MOPP trees still accelerate.
    // Havok doesn't care that the company that licensed it is gone.
    bool initPhysicsPlaytest();
    void shutdownPhysicsPlaytest();
    void togglePhysicsPlaytest();
    bool isPhysicsActive() const { return m_physicsActive; }

    // Player character model (third-person playtest)
    bool loadPlayerModel(const char* jmodelPath, const char* glbPath, const char* textureDir);
    void renderPlayerCharacter();
    void buildPlayerAnimationPose(hkaPose& outPose);

    // Load a new GameFiles model (jmodel + glb) at runtime
    bool loadGameModel(const char* jmodelPath, const char* glbPath, const char* textureDir);
    bool loadGroundModel(const char* jmodelPath, const char* glbPath, const char* textureDir, float extraYOffset = 0.0f);
    void clearGroundModel();
    bool hasGroundModel() const { return m_groundModel != NULL; }
    void setGroundModelVisible(bool visible) { m_groundModelVisible = visible; }
    bool loadGroundTexture(const char* texturePath, float tileRepeat = 8.0f);
    void setGroundSize(float meters);
    float getGroundSize() const { return m_groundSize; }
    float getGroundTextureRepeat() const { return m_groundTextureRepeat; }
    // Materials / rendering
    bool reloadMaterialTextures(int partIndex); // -1 = all
    void setGammaEnabled(bool enabled);
    bool getGammaEnabled() const { return m_gammaEnabled; }
    void setAnisotropy(int maxAniso);
    int  getAnisotropy() const { return m_maxAnisotropy; }
    void setMipBias(float bias);
    float getMipBias() const { return m_mipBias; }
    void setD3DPerfMarkersEnabled(bool enabled) { m_d3dPerfMarkersEnabled = enabled; }
    bool getD3DPerfMarkersEnabled() const { return m_d3dPerfMarkersEnabled; }
    IDirect3DTexture9* getGroundTextureD3D() const { return m_groundTextureD3D; }
    void toggleRimLight();
    bool getRimLightEnabled() const { return m_rimLightEnabled; }
    const char* getPackedShaderRoot() const { return m_packedShaderRoot; }
    bool preloadSkyboxModel(const char* skyboxName, const char* jmodelPath, const char* glbPath, const char* textureDir);
    void clearSkyboxes();
    int getSkyboxCount() const { return (int)m_skyboxes.size(); }
    int getActiveSkyboxIndex() const { return m_activeSkyboxIndex; }
    const char* getSkyboxName(int index) const;
    const char* getActiveSkyboxName() const;
    void setActiveSkyboxIndex(int index);
    bool selectSkyboxByName(const char* name);
    void selectNextSkybox();
    void setSkyboxEnabled(bool enabled) { m_skyboxEnabled = enabled; }
    bool getSkyboxEnabled() const { return m_skyboxEnabled; }
    void setSkyRenderMode(int mode);
    SkyRenderMode getSkyRenderMode() const { return m_skyRenderMode; }
    const char* getSkyRenderModeName() const;

    // Optional extra sky layer (typically clouds). Uses another preloaded skybox model as an overlay.
    void setCloudLayerEnabled(bool enabled) { m_cloudLayerEnabled = enabled; }
    bool getCloudLayerEnabled() const { return m_cloudLayerEnabled; }
    void setCloudSkyboxIndex(int index);
    int  getCloudSkyboxIndex() const { return m_cloudSkyboxIndex; }
    const char* getCloudSkyboxName() const;

    // ---------------------------------------------------------------
    // JSON Animation — the animation pipeline I built after ripping
    // Conquest's .ANM binary format apart byte by motherfucking byte.
    //
    // Here's the sick joke: Pandemic stored animations as packed binary
    // blobs with variable-length quaternion compression that NOBODY
    // documented. No headers. No magic numbers. Just raw compressed
    // bone rotations with 3 different encoding schemes depending on
    // which goddamn version of their internal tool exported it.
    // I sat in Ghidra for WEEKS tracing the decode path through
    // the .exe. Function 0x006B2A10. (ConquestLLC.exe <- get this shit in dc) 
    // That address is burned into my retinas.
    // I see it when I close my eyes. I see it when I shit.
    //
    // So we decode those binary nightmares into JSON (human-readable,
    // debuggable) and play them back here. I should've stick with the
    // haighcam's parser but god fucking no.
    // Every getter below feeds the ImGui timeline panel.
    // Every setter touches state that 14 other systems depend on.
    // Change one value wrong and the skeleton does a fucking backflip
    // into the shadow realm.
    // ---------------------------------------------------------------
    bool setJsonAnimationPath(const char* path);
    const char* getJsonAnimationPath() const;
    float getJsonAnimationTime() const;
    float getJsonAnimationDuration() const;
    float getJsonAnimationFrameTime() const;
    int   getJsonAnimationFrameCount() const;

    // JSON animation blending (pose A ↔ pose B)
    bool  setJsonBlendAnimationPath(const char* path);
    void  clearJsonBlendAnimation();
    const char* getJsonBlendAnimationPath() const;
    bool  getJsonBlendEnabled() const { return m_jsonBlendEnabled; }
    void  setJsonBlendEnabled(bool enabled) { m_jsonBlendEnabled = enabled; }
    float getJsonBlendAlpha() const { return m_jsonBlendAlpha; }
    void  setJsonBlendAlpha(float alpha);
    int   getJsonBlendMode() const { return m_jsonBlendMode; }
    void  setJsonBlendMode(int mode);
    int   getJsonBlendRotMode() const { return m_jsonBlendRotMode; }
    void  setJsonBlendRotMode(int mode);
    int   getRotInterpMode() const { return m_rotInterpMode; }
    void  setRotInterpMode(int mode);
    int   getJsonBlendLayerRootBone() const { return m_jsonBlendLayerRootBone; }
    void  setJsonBlendLayerRootBone(int boneIndex);
    bool  loadBlendLayerClip(int layerIndex, const char* path);

    // ---------------------------------------------------------------
    // Animation Graph — Pandemic's Lua-driven state machine system.
    //
    // In the original game, animation states were controlled by Lua
    // scripts that called into C++ through a binding layer we found
    // references to in the .exe strings: "AnimStateMachine",
    // "PlayClip", "SetTransition", "WeightDamp"... the whole thing.
    // Those Pandemic motherfuckers built an ENTIRE animation state
    // machine in Lua in 2007-2008 and it was actually good. Like,
    // really fucking good. Transitions, blend trees, parametric
    // blending, trigger-based state changes — all from script.
    //
    // We rebuilt it (Kind of and still needs more work).
    // Took months. The Lua→clip resolution is still
    // partially broken because their AnimTable format uses CRC32
    // hashes as keys and some of the original string mappings are
    // lost to time. When a clip can't resolve, the character just
    // T-poses and stares at you like you personally killed Pandemic.
    // ---------------------------------------------------------------
    bool  loadAnimationGraph(const char* path);
    bool  loadAnimationGraphLua(const char* path);
    void  clearAnimationGraph();
    const char* getAnimationGraphPath() const;
    const char* getAnimationGraphName() const;
    bool  getAnimationGraphEnabled() const;
    void  setAnimationGraphEnabled(bool enabled);
    int   getAnimationGraphStateCount() const;
    const char* getAnimationGraphStateName(int index) const;
    int   getAnimationGraphCurrentState() const;
    int   getAnimationGraphNextState() const;
    bool  isAnimationGraphInTransition() const;
    float getAnimationGraphStateTime() const;
    float getAnimationGraphStateDuration() const;
    float getAnimationGraphTransitionTime() const;
    float getAnimationGraphTransitionDuration() const;
    bool  setAnimationGraphCurrentState(int index);
    bool  setAnimationGraphCurrentStateByName(const char* name);
    void  resetAnimationGraph();

    int   getAnimationGraphParamCount() const;
    const char* getAnimationGraphParamName(int index) const;
    int   getAnimationGraphParamType(int index) const;
    float getAnimationGraphParamFloat(int index) const;
    int   getAnimationGraphParamInt(int index) const;
    bool  getAnimationGraphParamBool(int index) const;
    float getAnimationGraphParamMin(int index) const;
    float getAnimationGraphParamMax(int index) const;
    bool  getAnimationGraphParamHasRange(int index) const;

    bool  setAnimationGraphParamFloat(int index, float value);
    bool  setAnimationGraphParamInt(int index, int value);
    bool  setAnimationGraphParamBool(int index, bool value);
    bool  fireAnimationGraphTrigger(int index);

    // Per-state data accessors (for state machine visualization)
    const char* getAnimationGraphStateClip(int index) const;
    bool  getAnimationGraphStateLoop(int index) const;
    float getAnimationGraphStateSpeed(int index) const;
    float getAnimationGraphStateStateDuration(int index) const;
    int   getAnimationGraphStateTransitionCount(int stateIndex) const;
    int   getAnimationGraphStateTransitionTarget(int stateIndex, int transIndex) const;
    int   getAnimationGraphStateOnEnterActionCount(int index) const;
    int   getAnimationGraphStateOnExitActionCount(int index) const;
    const char* getAnimationGraphStateOnEnterActionName(int stateIndex, int actionIndex) const;
    const char* getAnimationGraphStateOnExitActionName(int stateIndex, int actionIndex) const;
    bool  getAnimationGraphStateHasSubMachine(int index) const;
    bool  getAnimationGraphStateHasBlendGraph(int index) const;
    const char* getAnimationGraphStateSubMachineName(int index) const;
    const char* getAnimationGraphStateBlendGraphName(int index) const;
    int   getAnimationGraphActiveTransitionIndex() const;
    int   getAnimationGraphTransitionFromState(int transIndex) const;
    int   getAnimationGraphTransitionToState(int transIndex) const;
    int   getAnimationGraphTransitionConditionCount(int transIndex) const;
    int   getAnimationGraphTransitionCount() const;

    // Motion matching controls
    bool  getMotionMatchEnabled() const;
    void  setMotionMatchEnabled(bool enabled);
    bool  rebuildMotionMatchDatabase();
    void  clearMotionMatchDatabase();
    void  setMotionMatchTargetVelocity(float x, float z);
    void  setMotionMatchTargetFacing(float x, float z);
    void  getMotionMatchTargetVelocity(float& x, float& z) const;
    void  getMotionMatchTargetFacing(float& x, float& z) const;
    void  setMotionMatchSearchInterval(float seconds);
    float getMotionMatchSearchInterval() const;
    void  setMotionMatchBlendDuration(float seconds);
    float getMotionMatchBlendDuration() const;
    int   getMotionMatchFrameCount() const;
    int   getMotionMatchClipCount() const;
    float getMotionMatchLastScore() const;
    const char* getMotionMatchCurrentClipKey() const;
    
    /* Phase 6: Motion Matching Bridge - Filter-Based Integration */
    bool  rebuildMotionMatchDatabaseFromFilteredClips(const std::vector<std::string>& filteredClipKeys);
    void  setMotionMatchUseFilteredClips(bool enabled);
    bool  getMotionMatchUseFilteredClips() const;
    int   getMotionMatchFilteredClipCount() const;

    // Animation States Translator runtime interface (Phase 2)
    const LuaAnimStatesTranslatorInfo& getAnimStatesTranslatorInfo() const;
    void setAnimStatesTranslator(const LuaAnimStatesTranslatorInfo& info);
    void setAnimStatesTranslatorSelectedStateIndex(int index);
    int  getAnimStatesTranslatorSelectedStateIndex() const;
    const LuaAnimStateEntry* getAnimStatesTranslatorActiveState() const;

    // ASM Parity Toggles (Phase 2.5)
    enum AnimDrivenMode { ADM_Default = 0, ADM_ClipDriven = 1, ADM_MotionDriven = 2 };
    
    bool getAnimationDrivenEnabled() const;
    void setAnimationDrivenEnabled(bool enabled);
    bool getRootMotionWarpEnabled() const;
    void setRootMotionWarpEnabled(bool enabled);
    AnimDrivenMode getAnimDrivenMode() const;
    void setAnimDrivenMode(AnimDrivenMode mode);

    // AnimTable & State→Clip Resolution (Phase 3)
    const LuaAnimTableInfo& getAnimTableInfo() const;
    void setAnimTable(const LuaAnimTableInfo& info);
    std::vector<std::string> resolveStateToClips(const char* stateKey) const;
    int  getResolvedClipsForActiveStateCount() const;
    const char* getResolvedClipsForActiveState(int clipIndex) const;
    bool loadResolvedClip(int clipIndex);
    JsonAnimClip* getJsonAnimClipByKey(const char* key);

    // Filter Evaluation (Phase 4)
    std::vector<std::string> resolveStateToClipsWithFilters(const char* stateKey, 
                                                             const std::vector<std::string>& stancesFilter,
                                                             const std::vector<std::string>& actionsFilter) const;
    std::vector<std::string> getFilteredClipsForActiveState() const;
    int  getFilteredClipsForActiveStateCount() const;
    const char* getFilteredClipsForActiveState(int clipIndex) const;
    void applyFiltersToActiveState();

    // Graph State Machine Integration (Phase 5)
    bool findGraphStateMatchingAnimState(const char* animStateKey, int& outGraphStateIndex) const;
    void transitionGraphToState(int graphStateIndex);
    const char* getGraphStateNameForActiveAnimState() const;
    bool isGraphTransitioningToAnimState() const;
    int  getActiveGraphStateForAnimState() const;
    void updateGraphStateForActiveAnimState();  /* Updates cached graph state match */

    // ---------------------------------------------------------------
    // Animation Events — the shit that makes the game FEEL like a game.
    //
    // When Aragorn swings his sword in Conquest, it's not just an
    // animation playing. At frame 12, a DamageEvent fires. At frame 8,
    // a TrailEvent starts drawing the weapon arc. At frame 3, a
    // SoundEvent triggers a whoosh through Wwise (another stolen SDK —
    // Audiokinetic's Wwise, ripped from a dev portal that no longer
    // exists for this version). Frame 15, CameraShake. Frame 18,
    // ControllerRumble. All of this was packed into Pandemic's .ANM
    // binary as event tracks with CRC32-hashed type names.
    //
    // I reverse-engineered every single event type by watching the
    // game in slow motion and cross-referencing the binary data with
    // what happened on screen. Some events I STILL don't fully
    // understand — there's a "Charge" event type that does something
    // to the character controller that I can't reproduce because the
    // function that handles it in the .exe calls into 6 layers of
    // virtual dispatch that Ghidra can't fully resolve. Whatever.
    // It ships without it. Nobody will notice. I hope.
    // ---------------------------------------------------------------
    int   getJsonAnimEventCount() const;
    float getJsonAnimEventTime(int index) const;
    const char* getJsonAnimEventTypeName(int index) const;
    int   getJsonAnimEventCategory(int index) const; // 0=damage,1=trail,2=sound,3=camera,4=state,5=projectile,6=throw,7=bow,8=controller
    void  getJsonAnimEventSummary(int index, char* buf, int bufSize) const;

    // Event mutation for timeline editor
    // Returns index of newly inserted event, or -1 on failure.
    // Generic event addition - uses int for AnimEventType and void* for parameter vector
    int   addJsonAnimEventGeneric(int eventType, float timeSeconds, void* pvals);
    // Typed overload used internally by event system
    int   addJsonAnimEvent(AnimEventType eventType, float timeSeconds,
                           const std::vector<JsonAnimEventVal>& vals);
    // Legacy function for backward compatibility with SoundEvent/SoundCue
    int   addJsonAnimEvent(bool isSoundCue, float timeSeconds,
                           const char* crc0, const char* crc1, const char* crc2);
    bool  removeJsonAnimEvent(int index);

    void  focusCameraOnModel();
    void  setCameraPreset(int presetId); // 1=Maya,2=Blender,3=Unreal-ish

    struct MaterialInfo
    {
        char material[128];
        char diffuse[128];
        char normal[128];
        char specular[128];
        bool hasDiffuse;
        bool hasNormal;
        bool hasSpecular;
        bool hasTexcoord0;
        bool generatedUV;
    };
    bool getPrimaryMaterialInfo(MaterialInfo& outInfo) const;
    int  getMaterialCount() const;
    bool getMaterialInfo(int idx, MaterialInfo& outInfo) const;
    bool getValidationWarning(char* outText, int maxLen) const;
    void setEditorTimelineDuration(float seconds);
    float getEditorTimelineDuration() const;
    bool isJsonAnimationPaused() const;
    void setJsonAnimationPaused(bool paused);
    void toggleJsonAnimationPaused();
    void seekJsonAnimation(float timeSeconds);
    void setStrictRigCoverage(bool strict);
    bool getStrictRigCoverage() const { return m_strictRigCoverage; }
    RootMotionMode getRootMotionMode() const;
    void setRootMotionMode(int mode);
    void resetRootMotionState();
    bool getRootMotionEnabled() const { return m_rootMotionEnabled; }
    void setRootMotionEnabled(bool enabled);
    void setRootMotionLockAxis(int axis, bool locked);
    void setRootMotionLock(bool lockX, bool lockY, bool lockZ);
    void getRootMotionOffset(float& outX, float& outY, float& outZ) const;
    int getRootMotionWarpMode() const;
    void setRootMotionWarpMode(int mode);
    void setRootMotionWarpTarget(float x, float y, float z);
    void getRootMotionWarpTarget(float& outX, float& outY, float& outZ) const;
    GroundClampMode getGroundClampMode() const;
    int getJsonDecodeMode() const;
    int getType2PackingMode() const;
    int getRotAxisMode() const;
    int getRotSignMask() const;
    RotApplyMode getRotApplyMode() const;
    bool getApplyPoseCorrection() const;
    bool getForceReferencePose() const;
    bool getUseJsonAnim() const;
    bool getLogType2() const;
    void setPlaybackSpeed(float speed);
    float getPlaybackSpeed() const;
    void setLoopAnimation(bool loop);
    bool getLoopAnimation() const;
    void setAnimFixedStepEnabled(bool enabled);
    bool getAnimFixedStepEnabled() const;
    void setAnimFixedStep(float stepSeconds);
    float getAnimFixedStep() const;
    void setAnimFixedMaxSteps(int maxSteps);
    int  getAnimFixedMaxSteps() const;
    float getAnimFixedAccumulator() const;
    int   getAnimFixedStepsLast() const;
    void  resetAnimFixedAccumulator();
    void setTimeWarpEnabled(bool enabled);
    bool getTimeWarpEnabled() const;
    void setTimeWarpEasingType(int easingType);
    int  getTimeWarpEasingType() const;
    void setTimeWarpControlPoints(float cp1x, float cp1y, float cp2x, float cp2y);
    void getTimeWarpControlPoints(float& cp1x, float& cp1y, float& cp2x, float& cp2y) const;
    void setCompressionAuto(bool enabled);
    bool getCompressionAuto() const;
    void setCompressionTolerances(float posTol, float rotTolDeg, float scaleTol, float rootTol);
    void getCompressionTolerances(float& posTol, float& rotTolDeg, float& scaleTol, float& rootTol) const;
    void setCompressionQuantizeRotations(bool enabled);
    bool getCompressionQuantizeRotations() const;
    void setCompressionStripRotations(bool enabled);
    bool getCompressionStripRotations() const;
    bool optimizeActiveJsonClip();
    void getLastCompressionStats(int& rotBefore, int& rotAfter,
                                 int& transBefore, int& transAfter,
                                 int& scaleBefore, int& scaleAfter,
                                 int& rootBefore, int& rootAfter) const;
    bool hasLastCompressionStats() const;
    void setSelectedBoneIndex(int index);
    int getSelectedBoneIndex() const;
    int getSkeletonBoneCount() const;
    const char* getSkeletonBoneName(int index) const;
    int getSkeletonParentIndex(int index) const;
    const hkaSkeleton* getModelSkeleton() const;
    float getCreatureVar(int index, int source = 0) const;
    void setCreatureVar(int index, int source, float value);
    void setCreatureDataNamed(const char* name, float value);
    float getCreatureDataNamed(const char* name) const;
    float getLocalVar(const char* name) const;
    void setLocalVar(const char* name, float value);
    void setEditorRecording(bool recording);
    bool isEditorRecording() const;
    void editorBeginDrag();
    void editorUpdateDrag(float dx, float dy, int axisLock);
    void editorEndDrag(float timeSeconds);
    void editorBeginTranslate();
    void editorUpdateTranslate(float dx, float dy, float dz, int axisLock);
    void editorEndTranslate(float timeSeconds);
    bool exportEditorClip(const char* path);
    AxisLock getEditorRotateAxisLock() const { return m_editorRotateAxisLock; }
    AxisLock getEditorTranslateAxisLock() const { return m_editorTranslateAxisLock; }
    void setGizmoMode(int mode);
    GizmoMode getGizmoMode() const { return m_gizmoMode; }
    void toggleGizmoSpace();
    GizmoSpace getGizmoSpace() const { return m_gizmoSpace; }
    AxisLock pickGizmoAxis(int mouseX, int mouseY) const;
    void setRotateSnapEnabled(bool enabled);
    bool getRotateSnapEnabled() const { return m_rotateSnapEnabled; }
    void setMoveSnapEnabled(bool enabled);
    bool getMoveSnapEnabled() const { return m_moveSnapEnabled; }
    void setRotateSnapDegrees(float degrees);
    float getRotateSnapDegrees() const { return m_rotateSnapDegrees; }
    void setMoveSnapUnits(float units);
    float getMoveSnapUnits() const { return m_moveSnapUnits; }
    float getDebugFootMinY() const { return m_debugFootMinY; }
    float getDebugMeshMinY() const { return m_debugMeshMinY; }
    bool isRootMotionLockedX() const { return m_rootMotionLock[0]; }
    bool isRootMotionLockedY() const { return m_rootMotionLock[1]; }
    bool isRootMotionLockedZ() const { return m_rootMotionLock[2]; }
    void setEditorInterpolationMode(int mode);
    InterpMode getEditorInterpolationMode() const { return m_editorInterpolationMode; }

    // ---------------------------------------------------------------
    // Physical Animation / Ragdoll — let the bones obey physics.
    //
    // Stiffness and damping control how hard the skeleton fights
    // gravity. High stiffness = bones snap to animation pose like
    // nothing happened. Low stiffness = your character melts into
    // the fucking floor like a bag of wet meat. Pandemic tuned these
    // values per-character: Gandalf was stiffer (robes), Gimli was
    // heavier (stocky dwarf physics), Legolas was lighter. We found
    // the tuning constants in a data block at 0x00B8C400 in the .exe.
    // Raw floats. No labels. Just vibes and experimental tweaking
    // by some poor Pandemic physics programmer who probably wrote
    // "TODO: clean this up" and then got laid off before they could.
    // ---------------------------------------------------------------
    void setPhysicalAnimEnabled(bool enabled) { m_physicalAnimEnabled = enabled; }
    bool getPhysicalAnimEnabled() const { return m_physicalAnimEnabled; }
    void setPhysicalPosStiffness(float k) { m_physPosStiffness = k; }
    float getPhysicalPosStiffness() const { return m_physPosStiffness; }
    void setPhysicalPosDamping(float d) { m_physPosDamping = d; }
    float getPhysicalPosDamping() const { return m_physPosDamping; }
    void setPhysicalRotStiffness(float k) { m_physRotStiffness = k; }
    float getPhysicalRotStiffness() const { return m_physRotStiffness; }
    void setPhysicalRotDamping(float d) { m_physRotDamping = d; }
    float getPhysicalRotDamping() const { return m_physRotDamping; }
    void setRagdollEnabled(bool enabled) { m_ragdollEnabled = enabled; }
    bool getRagdollEnabled() const { return m_ragdollEnabled; }
    void setRagdollBlend(float w);
    float getRagdollBlend() const { return m_ragdollBlend; }
    void setRagdollGravity(float g) { m_ragdollGravity.set(0.0f, g, 0.0f); }
    float getRagdollGravity() const { return m_ragdollGravity(1); }
    void resetPhysicsState();
    void applyHitImpulse(int boneIndex, const hkVector4& linearImpulse, const hkVector4& angularImpulse);

    // ---------------------------------------------------------------
    // Inverse Kinematics — making feet touch the goddamn ground.
    //
    // You'd think "put foot on floor" would be simple. You'd be
    // wrong, nigga. You'd be so catastrophically fucking wrong.
    // IK is where math goes to die screaming. Two-bone IK for legs,
    // look-at IK for heads, aim IK for weapons — each one is a
    // separate chain of sin-cos-atan2 bullshit that has to solve
    // in the right order or the skeleton turns into a pretzel made
    // of nightmares. Pandemic had this shit handled through Havok's
    // built-in IK solvers, but those solvers live in hkaIk.h which
    // is part of the Animation SDK we stole, and half the functions
    // are inlined template garbage that only compiles if you set up
    // the include paths EXACTLY right. One wrong path and you get
    // 847 template instantiation errors that scroll for 10 minutes.
    //
    // The foot IK alone took 3 weeks. THREE WEEKS for feet. FEET.
    // I have dreams about feet now. Not the good kind. The kind
    // where the feet are backwards and clipping through the earth
    // and the ground plane is at Y=-infinity and I'm falling too.
    // ---------------------------------------------------------------
    void setIKEnabled(bool enabled) { m_ikEnabled = enabled; }
    bool getIKEnabled() const { return m_ikEnabled; }
    void setFootIKEnabled(bool enabled) { m_footIkEnabled = enabled; }
    bool getFootIKEnabled() const { return m_footIkEnabled; }
    void setLookAtIKEnabled(bool enabled) { m_lookAtEnabled = enabled; }
    bool getLookAtIKEnabled() const { return m_lookAtEnabled; }
    void setAimIKEnabled(bool enabled) { m_aimEnabled = enabled; }
    bool getAimIKEnabled() const { return m_aimEnabled; }

    int  getIKChainCount() const { return m_ikChains.getSize(); }
    const char* getIKChainName(int index) const;
    bool getIKChainEnabled(int index) const;
    void setIKChainEnabled(int index, bool enabled);
    bool getIKChainTarget(int index, float& outX, float& outY, float& outZ) const;
    void setIKChainTarget(int index, float x, float y, float z);
    void setIKChainTargetBone(int index, int boneIndex);
    void setIKChainUseGround(int index, bool enabled);
    void setIKChainGroundOffset(int index, float offset);
    void setIKChainUseTwoBone(int index, bool enabled);
    void rebuildDefaultIKChains();

    void setLookAtBoneIndex(int boneIndex) { m_lookAtBoneIndex = boneIndex; }
    int  getLookAtBoneIndex() const { return m_lookAtBoneIndex; }
    void setAimBoneIndex(int boneIndex) { m_aimBoneIndex = boneIndex; }
    int  getAimBoneIndex() const { return m_aimBoneIndex; }
    void setLookAtTarget(float x, float y, float z);
    void setAimTarget(float x, float y, float z);
    void getLookAtTarget(float& outX, float& outY, float& outZ) const;
    void getAimTarget(float& outX, float& outY, float& outZ) const;
    void setLookAtWeight(float w);
    void setAimWeight(float w);
    float getLookAtWeight() const { return m_lookAtWeight; }
    float getAimWeight() const { return m_aimWeight; }

    // Default easing used for newly recorded editor keys (and as UI default).
    void setEditorDefaultEasing(int easingType, float cp1x, float cp1y, float cp2x, float cp2y);
    int  getEditorDefaultEasingType() const { return m_editorDefaultEasingType; }
    void getEditorDefaultEasingCP(float& cp1x, float& cp1y, float& cp2x, float& cp2y) const;

    // Editor keyframe easing accessors (used by UI).
    int  findEditorRotKeyAtOrBeforeTime(int boneIndex, float timeSeconds) const;
    int  findEditorTransKeyAtOrBeforeTime(int boneIndex, float timeSeconds) const;
    int  findEditorScaleKeyAtOrBeforeTime(int boneIndex, float timeSeconds) const;
    bool getEditorRotKeyEasing(int boneIndex, int keyIndex, int& outType, float& outCp1x, float& outCp1y, float& outCp2x, float& outCp2y) const;
    bool getEditorTransKeyEasing(int boneIndex, int keyIndex, int& outType, float& outCp1x, float& outCp1y, float& outCp2x, float& outCp2y) const;
    bool getEditorScaleKeyEasing(int boneIndex, int keyIndex, int& outType, float& outCp1x, float& outCp1y, float& outCp2x, float& outCp2y) const;
    bool setEditorRotKeyEasing(int boneIndex, int keyIndex, int type, float cp1x, float cp1y, float cp2x, float cp2y);
    bool setEditorTransKeyEasing(int boneIndex, int keyIndex, int type, float cp1x, float cp1y, float cp2x, float cp2y);
    bool setEditorScaleKeyEasing(int boneIndex, int keyIndex, int type, float cp1x, float cp1y, float cp2x, float cp2y);

    // Per-key translation curve params (shared key index across X/Y/Z channels).
    bool getEditorTransKeyTangents(int boneIndex, int keyIndex,
                                   float& outInX, float& outOutX,
                                   float& outInY, float& outOutY,
                                   float& outInZ, float& outOutZ) const;
    bool setEditorTransKeyTangents(int boneIndex, int keyIndex,
                                   float inX, float outX,
                                   float inY, float outY,
                                   float inZ, float outZ);
    bool getEditorScaleKeyTangents(int boneIndex, int keyIndex,
                                   float& outInX, float& outOutX,
                                   float& outInY, float& outOutY,
                                   float& outInZ, float& outOutZ) const;
    bool setEditorScaleKeyTangents(int boneIndex, int keyIndex,
                                   float inX, float outX,
                                   float inY, float outY,
                                   float inZ, float outZ);
    int  getEditorTransKeyInterpolationMode(int boneIndex, int keyIndex) const;
    bool setEditorTransKeyInterpolationMode(int boneIndex, int keyIndex, int interpMode);
    int  getEditorScaleKeyInterpolationMode(int boneIndex, int keyIndex) const;
    bool setEditorScaleKeyInterpolationMode(int boneIndex, int keyIndex, int interpMode);
    void editorCommitCurrent(float timeSeconds);
    void editorCancelCurrent();
    bool hasSelectedBonePendingEdit() const;
    bool getSelectedBoneLocalTRS(float& tx, float& ty, float& tz, float& rxDeg, float& ryDeg, float& rzDeg) const;
    bool getSelectedBoneLocalTRSScale(float& tx, float& ty, float& tz,
                                      float& rxDeg, float& ryDeg, float& rzDeg,
                                      float& sx, float& sy, float& sz) const;
    bool keySelectedBoneLocalTRS(float tx, float ty, float tz, float rxDeg, float ryDeg, float rzDeg, float timeSeconds, bool keyRot, bool keyTrans);
    bool keySelectedBoneLocalTRSScale(float tx, float ty, float tz,
                                      float rxDeg, float ryDeg, float rzDeg,
                                      float sx, float sy, float sz,
                                      float timeSeconds, bool keyRot, bool keyTrans, bool keyScale);

    // Editor keyframe accessors for timeline visualization
    int getEditorRotKeyCount(int boneIndex) const;
    int getEditorTransKeyCount(int boneIndex) const;
    int getEditorScaleKeyCount(int boneIndex) const;
    float getEditorRotKeyTime(int boneIndex, int keyIndex) const;
    float getEditorTransKeyTime(int boneIndex, int keyIndex) const;
    float getEditorScaleKeyTime(int boneIndex, int keyIndex) const;
    void initializeEditorKeyTimes();

    // Particle effects rendering (public for EffectManager)
    void renderParticleEmitter(ParticleEmitter* emitter);
    void renderRefractParticleEmitter(ParticleEmitter* emitter);
    bool prepareRefractionSceneCopy();

    // Particle effects testing
    void spawnTestEffect(const char* effectName, const hkVector4& position);
    void loadTestEffect(const char* jsonPath, const char* textureDir);
    void clearAllEffects();

private:
    // ===============================================================
    // PRIVATE SECTION — ABANDON ALL HOPE, YE WHO SCROLL PAST HERE
    //
    // Everything below this line is the internal organ meat of the
    // renderer. 500+ member variables. Helper functions that call
    // helper functions that call Havok functions that call D3D9
    // functions that call into the GPU driver that calls into the
    // void. This is where the real suffering lives. Every pointer
    // down here is a potential null deref. Every float is a potential
    // NaN. Every hkArray is a potential buffer overrun that Havok's
    // debug allocator will catch in debug builds but SILENTLY CORRUPT
    // in release. I found that out the hard way. Three times.
    //
    // If you're modifying anything below: ADD YOUR NEW SHIT AT THE
    // END. The .obj files from Scene3D are pre-compiled against this
    // exact layout. Move a member variable and the struct offset
    // shifts and everything compiled against the old layout will
    // access the wrong memory and you'll get crashes so mysterious
    // that you'll start believing in demons. I already do.
    // ===============================================================

    // Rendering helpers — each one is a mini-ritual of D3D9 state
    void renderMesh(const hkaPose* pose);
    void renderSkyboxMesh();
    void renderSkeletonFromPose(const hkaPose& pose);
    void renderFloorGrid();
    void renderGroundModel();
    void renderBoneGizmo(const hkaPose& pose);

    void releaseImGuiViewportResources();
    bool ensureImGuiViewportResources(int desiredW, int desiredH);
    void releaseRefractionResources();
    bool ensureRefractionSceneCopyResources(int desiredW, int desiredH, int desiredFmt);
    bool ensureRefractionWhiteTexture();
    bool ensureRefractionShader();

    // JSON animation helpers (pose sampling + blending caches)
    void applyJsonClipToLocalPose(const JsonAnimClip* clip, hkQsTransform* local, float timeSeconds);
    void applyJsonBlendToLocalPose(hkQsTransform* local, float timeSeconds);
    void applyEditorOverridesToLocalPose(hkQsTransform* local, int boneCount, float frame);
    void buildPoseFromAnimGraph(hkaPose& pose);
    bool applyIKToPose(hkaPose& pose);
    float applyTimeWarpToClipTime(const JsonAnimClip* clip, float timeSeconds) const;
    bool applyRootMotionToPose(hkaPose& pose, const JsonAnimClip* clip, float timeSeconds);
    bool applyPhysicsToPose(hkaPose& pose);
    void stepAnimation(float deltaTime);
    void updateAnimationGraph(float deltaTime);
    const JsonAnimClip* getActiveJsonClipForUI() const;
    JsonAnimClip* getActiveJsonClipForEdit();
    bool OptimizeJsonClipInternal(JsonAnimClip* clip);
    void processAnimEventsForClip(const JsonAnimClip* clip, float prevTime, float currTime, unsigned int clipKey);
    void resetEventDedupForClip(unsigned int clipKey);
public:
    void queueGraphOneShotTrigger(AnimationGraphRuntime* graph, int paramIndex);
    void clearGraphOneShotTriggers();
private:
    void rebuildJsonBlendMask();
    void ensureJsonBlendAdditiveRefPose();
    void ensureGraphStateAdditiveRef(int stateIndex);
    hkQsTransform* getGraphScratchLocal(int boneCount, hkQsTransform* avoid);
    hkQsTransform* getJsonBlendScratchLocal(int boneCount);

    // ---------------------------------------------------------------
    // Win32 window — the physical hole in the screen where the
    // suffering becomes visible to human eyes
    // ---------------------------------------------------------------
    HWND m_hwnd;
    int m_windowWidth;
    int m_windowHeight;
    
    // Havok rendering components — the same setup from every single
    // Havok 5.5 demo ever written. Pandemic copied it. I copied
    // Pandemic. We're all just copying dead people's homework at
    // this point. The whole chain of custody is a fucking graveyard.
    hkgWindow* m_window;                   // Havok window wrapper. Internally creates the D3D9 device. If this returns NULL your GPU driver hates you personally.
    hkgDisplayContext* m_context;          // Graphics context — the thing that allocates vertex buffers, textures, shaders. Every resource in the renderer flows through this motherfucker.
    hkgDisplayWorld* m_displayWorld;       // Scene container — all display objects live here. Remove one wrong and the render loop crashes with no stack trace. Ask me how I know.
    hkgCamera* m_camera;                   // Camera. Havok's camera class. Stores view/proj matrices. Simple in theory. In practice it fights with D3D9's left-handed coordinate system every single frame.
    hkgLightManager* m_lightManager;       // Lighting system. Pandemic used 3 directional lights for most levels. We found the light directions hardcoded at 0x00A4E6F0 in the .exe. Three float3 vectors. Raw. Unlabeled. Classic Pandemic.
    
    // Scene loading and conversion
    Scene3DLoader* m_loader;
    HavokToDisplayConverter* m_converter;

    // ---------------------------------------------------------------
    // Animation system — THE SINGLE LARGEST BLOCK OF STATE IN THIS
    // CLASS. 100+ variables for animation alone. Playback time,
    // blend weights, graph state, motion matching database, decode
    // mode flags, compression settings, editor keyframes, undo stack,
    // pose library, root motion tracking, event deduplication...
    //
    // This is what happens when you try to reverse-engineer an entire
    // animation pipeline from a dead game's .exe and you keep adding
    // "just one more feature" for 3 years straight. Every variable
    // here has a story. Most of those stories end with me staring at
    // a debugger at 4 AM going "why the fuck is this NaN."
    //
    // The decode modes (m_jsonDecodeMode, m_type2PackingMode,
    // m_rotAxisMode, m_rotSignMask) exist because Pandemic used AT
    // LEAST 3 different quaternion compression schemes across
    // different animation files and I had to reverse all of them.
    // ---------------------------------------------------------------
    AnimatedCharacter* m_animatedCharacter;
    float m_animationTime;
    JsonAnimClip* m_jsonAnim;
    JsonAnimClip* m_jsonBlendAnim;
    AnimationGraphRuntime* m_animGraph;
    float m_jsonAnimTime;
    float m_lastEventTime;
    int m_lastEventIndex;
    bool m_useJsonAnim;
    bool m_useJsonBlendAnim;
    int m_jsonDecodeMode; // 0=auto, 1=force A, 2=force B, 3=rotvec
    int m_type2PackingMode; // 0=interleaved per-frame, 1=per-axis blocks
    bool m_logType2;
    int m_lastType2LogFrame;
    int m_rotAxisMode; // 0=xyz, 1=xzy, 2=zyx, 3=yxz, 4=yzx, 5=zxy
    int m_rotSignMask; // bit0=flipX, bit1=flipY, bit2=flipZ
    RotApplyMode m_rotApplyMode;
    bool m_applyPoseCorrection;
    bool m_forceReferencePose;
    bool m_jsonAnimPaused;
    bool m_jsonBlendEnabled;
    bool m_loopAnimation;
    float m_playbackSpeed;
    bool m_animFixedStepEnabled;
    float m_animFixedStep;
    float m_animFixedAccumulator;
    int m_animFixedMaxSteps;
    int m_animFixedStepsLast;
    bool m_timeWarpEnabled;
    int m_timeWarpEasingType;
    float m_timeWarpCp1x;
    float m_timeWarpCp1y;
    float m_timeWarpCp2x;
    float m_timeWarpCp2y;
    int m_jsonBlendMode;
    int m_jsonBlendRotMode;
    int m_rotInterpMode;
    float m_jsonBlendAlpha;
    int m_jsonBlendLayerRootBone;
    std::vector<float> m_jsonBlendMask;
    hkArray<hkQsTransform> m_jsonBlendAdditiveRefLocal;
    bool m_jsonBlendAdditiveRefValid;
    hkArray<hkQsTransform> m_graphScratchLocalA;
    hkArray<hkQsTransform> m_graphScratchLocalB;
    hkArray<hkQsTransform> m_jsonBlendScratchLocal;
    bool m_motionMatchEnabled;
    bool m_motionMatchDatabaseValid;
    float m_motionMatchSearchInterval;
    float m_motionMatchSearchTimer;
    float m_motionMatchBlendDuration;
    float m_motionMatchBlendTime;
    bool m_motionMatchBlendActive;
    bool m_motionMatchRequestBlend;
    hkVector4 m_motionMatchTargetVel;
    hkVector4 m_motionMatchTargetFacing;
    float m_motionMatchWeightVel;
    float m_motionMatchWeightFacing;
    float m_motionMatchWeightFeet;
    float m_motionMatchWeightTraj;
    float m_motionMatchTrajT1;
    float m_motionMatchTrajT2;
    float m_motionMatchTrajT3;
    float m_motionMatchLastScore;
    int m_motionMatchCurrentFrameIndex;
    int m_motionMatchCurrentClipIndex;
    float m_motionMatchTime;
    float m_motionMatchPrevTime;
    MotionMatchDatabase m_motionMatchDb;
    MotionMatchFeature m_motionMatchLastFeature;
    bool m_motionMatchHasLastFeature;
    std::vector<JsonAnimClip*> m_motionMatchOwnedClips;
    hkArray<hkQsTransform> m_motionMatchLastLocalPose;
    hkArray<hkQsTransform> m_motionMatchBlendFrom;
    hkArray<hkQsTransform> m_motionMatchBlendTemp;
    bool m_motionMatchHasLastPose;
    
    /* Animation States Translator (Phase 2) */
    LuaAnimStatesTranslatorInfo m_animStatesTranslator;
    
    /* ASM Parity Toggles (Phase 2.5) */
    bool m_useAnimationDriven;
    bool m_rootMotionWarpEnabled;
    AnimDrivenMode m_animDrivenMode;
    
    /* AnimTable & Clip Resolution (Phase 3) */
    LuaAnimTableInfo m_animTable;
    std::vector<std::string> m_resolvedClipsForActiveState;
    
    /* Filter Evaluation (Phase 4) */
    std::vector<std::string> m_filteredClipsForActiveState;
    
    /* Graph State Machine Integration (Phase 5) */
    int m_graphStateForActiveAnimState;       /* Matched graph state index (-1 if no match) */
    bool m_isTransitioningToAnimState;        /* True when transition in progress */
    
    /* Motion Matching Bridge (Phase 6) */
    bool m_motionMatchUseFilteredClips;       /* When true, motion match only searches filtered clips */
    bool m_motionMatchFilteredDatabaseValid;  /* True when filtered DB is valid for current state */
    int m_motionMatchFilteredClipCount;       /* Count of clips in filtered motion match DB */
    
    bool m_ikEnabled;
    bool m_footIkEnabled;
    bool m_lookAtEnabled;
    bool m_aimEnabled;
    int m_lookAtBoneIndex;
    int m_aimBoneIndex;
    float m_lookAtWeight;
    float m_aimWeight;
    hkVector4 m_lookAtTarget;
    hkVector4 m_aimTarget;
    hkObjectArray<IKChain> m_ikChains;
    bool m_physicalAnimEnabled;
    bool m_ragdollEnabled;
    float m_ragdollBlend;
    float m_physPosStiffness;
    float m_physPosDamping;
    float m_physRotStiffness;
    float m_physRotDamping;
    hkVector4 m_ragdollGravity;
    float m_lastAnimDeltaTime;
    hkObjectArray<PhysBoneState> m_physState;
    PendingImpulse m_pendingImpulse;
    RootMotionMode m_rootMotionMode;
    GroundClampMode m_groundClampMode;
    float m_groundOffsetY;
    bool m_animationDrivenEnabled;
    bool m_rootMotionEnabled;
    float m_groundContactEps;
    float m_groundReleaseHeight;
    float m_groundSmoothFactor;
    bool  m_groundFillEnabled;
    hkgTexture* m_groundTexture;
    IDirect3DTexture9* m_groundTextureD3D;
    char m_groundTexturePath[512];
    float m_groundSize;
    float m_groundTextureRepeat;
    float m_groundHeight;
    struct GameModel* m_groundModel;
    bool  m_groundModelVisible;
    float m_groundModelOffsetY;
    char  m_groundJPath[512];
    char  m_groundGlbPath[512];
    char  m_groundTexDir[512];
    bool  m_gammaEnabled;
    int   m_maxAnisotropy;
    float m_mipBias;
    bool  m_d3dPerfMarkersEnabled;
    bool  m_rimLightEnabled;
    IDirect3D9* m_d3d9;
    unsigned int m_d3dAdapterIndex;
    bool m_d3dAdapterValid;
    unsigned int m_d3dVendorId;
    char m_d3dAdapterDesc[128];
    char m_packedShaderRoot[260];
    float m_cameraRoll;
    bool  m_horizonLock;
    bool  m_cameraOrtho;
    int   m_navMode; // 1=Maya, 2=Blender, 3=Unreal-ish
    char  m_navModeLabels[4][16];
    CameraBookmark m_camBookmarks[5];
    float m_debugMeshMinY;
    float m_debugFootMinY;
    float m_modelBaseOffsetY;
    bool m_modelBaseComputed;
    int m_selectedBoneIndex;
    bool m_editorRecording;
    bool m_editorDragging;
    bool m_editorDraggingTrans;
    AxisLock m_editorRotateAxisLock;
    AxisLock m_editorTranslateAxisLock;
    GizmoMode m_gizmoMode;
    GizmoSpace m_gizmoSpace;
    bool m_rotateSnapEnabled;
    bool m_moveSnapEnabled;
    float m_rotateSnapDegrees;
    float m_moveSnapUnits;
    InterpMode m_editorInterpolationMode;
    int m_editorDefaultEasingType;
    float m_editorDefaultEasingCp1x;
    float m_editorDefaultEasingCp1y;
    float m_editorDefaultEasingCp2x;
    float m_editorDefaultEasingCp2y;
    float m_rotateSnapAccum;
    float m_rotateSnapApplied;
    int m_rotateSnapAxis;
    float m_moveSnapAccum[3];
    float m_moveSnapApplied[3];
    int m_moveSnapAxis;
    float m_cameraFovDegrees;
    bool m_gizmoCacheValid;
    hkVector4 m_gizmoCachePos;
    hkVector4 m_gizmoCacheAxisX;
    hkVector4 m_gizmoCacheAxisY;
    hkVector4 m_gizmoCacheAxisZ;
    float m_gizmoCacheScale;
    float m_editorFrameTime;
    float m_editorTimelineDuration;
    hkVector4 m_rootMotionOffset;
    hkVector4 m_rootMotionPrevPos;
    float m_rootMotionPrevTime;
    bool m_rootMotionPrevValid;
    int  m_rootMotionWarpMode;
    hkVector4 m_rootMotionWarpTarget;
    bool m_compressAuto;
    bool m_compressQuantizeRot;
    bool m_compressStripRot;
    float m_compressPosTol;
    float m_compressRotTolDeg;
    float m_compressScaleTol;
    float m_compressRootTol;
    bool m_compressStatsValid;
    int m_compressRotBefore;
    int m_compressRotAfter;
    int m_compressTransBefore;
    int m_compressTransAfter;
    int m_compressScaleBefore;
    int m_compressScaleAfter;
    int m_compressRootBefore;
    int m_compressRootAfter;
    std::vector< std::vector<EditorKey> > m_editorRotKeys;
    std::vector< std::vector<EditorFloatKey> > m_editorPosKeysX;
    std::vector< std::vector<EditorFloatKey> > m_editorPosKeysY;
    std::vector< std::vector<EditorFloatKey> > m_editorPosKeysZ;
    std::vector< std::vector<EditorFloatKey> > m_editorScaleKeysX;
    std::vector< std::vector<EditorFloatKey> > m_editorScaleKeysY;
    std::vector< std::vector<EditorFloatKey> > m_editorScaleKeysZ;
    hkArray<hkQuaternion> m_editorOverrideRot;
    hkArray<hkQsTransform> m_editorLastLocalPose;
    std::vector<EditorTransOverride> m_editorOverrideTrans;
    std::vector<PoseSnapshot> m_undoStack;
    std::vector<PoseSnapshot> m_redoStack;
    PoseLibraryEntry m_poseLibrary[5];
    bool m_rootMotionLock[3]; // X,Y,Z
    hkArray<hkVector4> m_rootTrail;
    bool m_showRootTrail;
    float m_creatureVars[64];
    std::map<std::string, float> m_creatureData;
    int m_inputButtonState[32];
    bool m_inputButtonDown[32];
    unsigned int m_graphRngState;
    std::map<std::string, float> m_localVars;
    std::vector<GraphTriggerRef> m_graphOneShotTriggers;

public:
    float getGroundOffsetY() const { return m_groundOffsetY; }
    float getModelBaseOffsetY() const { return m_modelBaseOffsetY; }
    float getGroundContactEps() const { return m_groundContactEps; }
    float getGroundReleaseHeight() const { return m_groundReleaseHeight; }
    float getGroundSmoothFactor() const { return m_groundSmoothFactor; }
    hkVector4 getCameraTarget() const { return m_cameraTarget; }
    hkVector4 getCameraPosition() const;
    char m_jsonAnimPath[512];
    char m_animGraphPath[512];

private:
    char m_jsonBlendAnimPath[512];

    // Game model (for mesh rendering)
    struct GameModel* m_gameModel;
    struct SkyboxEntry
    {
        char name[128];
        GameModel* model;
    };
    std::vector<SkyboxEntry> m_skyboxes;
    int m_activeSkyboxIndex;
    bool m_skyboxEnabled;
    SkyRenderMode m_skyRenderMode;
    bool m_cloudLayerEnabled;
    int  m_cloudSkyboxIndex;

    // Direct3D device (exposed for texture loading / custom rendering)
    IDirect3DDevice9* m_d3dDevice;
    bool m_strictRigCoverage;

    // Viewport visibility: when false, skip all 3D rendering (ImGui+Present still run)
    bool m_scene3dEnabled;

    // ImGui viewport (render-to-texture)
    bool m_imguiViewportActive;
    int  m_imguiViewportW;
    int  m_imguiViewportH;
    IDirect3DTexture9* m_imguiViewportTex;
    IDirect3DSurface9* m_imguiViewportSurf;
    IDirect3DSurface9* m_imguiViewportDepth;
    int  m_imguiViewportTexW;
    int  m_imguiViewportTexH;

    // Refraction (heat haze) scene copy + shader
    IDirect3DTexture9* m_refractionSceneTex;
    IDirect3DSurface9* m_refractionSceneSurf;
    int  m_refractionSceneW;
    int  m_refractionSceneH;
    int  m_refractionSceneFmt;
    IDirect3DPixelShader9* m_refractionPS;
    IDirect3DTexture9* m_refractionWhiteTex;

    // Level scene (full level loaded from extracted lotrcparser directory)
    LevelScene* m_levelScene;

    // Particle effects system
    EffectManager* m_effectManager;

    // Packed D3D9 game shaders (GameFiles/lotrcparser/Shaders_PC_nvidia) for closer-to-1:1 FX.
    bool m_usePackedParticleShaders;
    MgPackedParticleShaders* m_packedParticleShaders;

    // Camera control state
    float m_cameraDistance;
    float m_cameraYaw;
    float m_cameraPitch;
    hkVector4 m_cameraTarget;
    int m_lastMouseX;
    int m_lastMouseY;

    // Rendering state
    bool m_initialized;
    
    // Internal methods
    void updateCameraFromInput(float deltaTime);
    void setupDefaultLighting();
    bool loadJsonAnimation(const char* path);
    void buildPoseFromJson(hkaPose& pose, float timeSeconds);
    void buildPoseFromMotionMatch(hkaPose& pose);
    void updateMotionMatching(float deltaTime);
    bool buildMotionMatchDatabaseFromGraph();
    bool buildMotionMatchDatabaseFromClips(const std::vector<JsonAnimClip*>& clips);
    void updateMotionMatchQueryFromPose(const hkaPose& pose);
    void buildMotionMatchQuery(MotionMatchFeature& outQuery) const;
    int findBestMotionMatchFrame(const MotionMatchFeature& query, float& outScore) const;
    void buildPoseFromGraphMachine(AnimationGraphRuntime& graph, hkQsTransform* local, int boneCount);
    void evaluateGraphStatePose(AnimationGraphRuntime& graph, GraphState& state, hkQsTransform* local, int boneCount, float timeSeconds);
    void evaluateBlendGraphNode(AnimationGraphRuntime& graph, BlendGraphRuntime& blendGraph, int nodeIndex, hkQsTransform* local, int boneCount, float timeSeconds);
    void computeModelBaseOffsetFromReference();
    void autoFrameLoadedModel();
    void ensureEditorArrays();
    void recordEditorKey(int boneIndex, float timeSeconds, const hkQuaternion& rot);
    void recordEditorTransKey(int boneIndex, float timeSeconds, const hkVector4& t);
    void recordEditorScaleKey(int boneIndex, float timeSeconds, const hkVector4& s);
    void processAnimEvents(float prevTime, float currTime);
    void handleAnimEvent(const JsonAnimEvent& evt);
    void handleSoundCueEvent(const JsonAnimEvent& evt);
    void handleSoundEvent(const JsonAnimEvent& evt);
    void handleDamageEvent(const JsonAnimEvent& evt);      /* All damage/combat events */
    void handleTrailEvent(const JsonAnimEvent& evt);       /* Weapon trails on/off */
    void handleParticleEvent(const JsonAnimEvent& evt);    /* Particle/VFX spawning */
    void handleCameraEvent(const JsonAnimEvent& evt);      /* Camera shake/effects */
    void handleStateEvent(const JsonAnimEvent& evt);       /* Game state changes */
    void handleProjectileEvent(const JsonAnimEvent& evt);  /* Projectile firing/ready */
    void handleThrowEvent(const JsonAnimEvent& evt);       /* Throw with torque */
    void handleBowEvent(const JsonAnimEvent& evt);         /* Bow string animations */
    void handleControllerEvent(const JsonAnimEvent& evt);  /* Controller rumble */
    void handleGrabEvent(const JsonAnimEvent& evt);        /* Grab/melee damage areas */
    void handleChargeEvent(const JsonAnimEvent& evt);      /* Charge attack start/stop */
    void resetEventDedup();
    bool initD3D9AdapterInfo();

    // Event deduplication — without this, every animation loop boundary
    // fires the same SoundEvent twice and you get double sword clangs
    // that sound like the game is having a fucking seizure. Pandemic
    // had this same bug in early builds — you can hear it in leaked
    // 2008 footage if you listen carefully. They fixed it the same
    // way: track what already fired, skip duplicates. Some problems
    // are eternal.
    std::set<unsigned long long> m_firedEventIndices;

    // Fly camera state — first-person free-movement through dead worlds.
    // MUST stay at end of class or the .obj layout shifts and every
    // pre-compiled file starts reading garbage. I learned this lesson
    // through a crash so violent it corrupted my FUCKING PDB file.
    // HOW DOES A PDB FILE GETS CORRUPTED IN THE FIRST PLACE?
    // TOOK MY DAYS TO FIGURE OUT THAT IT WAS CORRUPTING THE vc80.pdb
    // Never again.
    bool  m_flyCameraActive;
    float m_flyCamPos[3];   // world position
    float m_flyCamYaw;      // horizontal look (radians, same convention as orbit)
    float m_flyCamPitch;    // vertical look (radians, same convention as orbit)
    float m_flyCamSpeed;    // base movement speed (units/sec)

    // Physics playtest state — character controller with gravity.
    // Uses void* to avoid including Havok Physics headers because
    // those headers pull in 200+ transitive includes and add 45
    // seconds to compile time. The stolen Havok 5.5 Physics SDK
    // was designed for full game projects, not surgical extraction.
    // But here we are, casting void* to hkpWorld* like animals.
    bool  m_physicsActive;
    void* m_physWorld;       // hkpWorld*
    void* m_physCharProxy;   // hkpCharacterProxy*
    void* m_physCharCtx;     // hkpCharacterContext*
    float m_physGroundY;     // ground plane height
    float m_physAccum;       // fixed-step accumulator

    // Level collision — raw triangle soup ripped from Pandemic's level
    // meshes and shoved into Havok's MOPP builder. 16-byte aligned
    // because Havok's SIMD math will shit itself on unaligned memory
    // and the error message is just "hkBaseAssert failed" with no
    // context. THANKS HAVOK. REAL FUCKING HELPFUL.
    void* m_physLevelVerts;    // float[numVerts*4], 16-byte aligned
    void* m_physLevelIndices;  // uint16_t[numIndices]
    int   m_physLevelVertCount;
    int   m_physLevelIdxCount;

    // Player character — the poor bastard we're puppeteering through
    // Pandemic's dead levels. Third-person camera follows behind.
    // Walk speed, yaw, animation state blending... Pandemic had all
    // this wired through their Lua gameplay scripts. We have raw C++
    // and a prayer. The animation state machine below (IDLE→WALK→RUN)
    // is a baby version of what Pandemic had. Theirs supported 47
    // different animation states per character class. Ours supports 3.
    // But ours compiles. Theirs requires a build system that died
    // with the studio. Progress is relative when you're walking
    // through a graveyard.
    GameModel* m_playerModel;
    AnimatedCharacter* m_playerAnimChar;
    float m_playerYaw;       // character facing direction (radians)
    float m_tpCamDistance;   // third-person camera distance behind
    float m_tpCamHeight;     // third-person camera height above

    // Player animation state (idle/walk/run blending from JSON clips)
    enum PlayerAnimState { PLAYER_ANIM_IDLE = 0, PLAYER_ANIM_WALK = 1, PLAYER_ANIM_RUN = 2 };
    PlayerAnimState m_playerAnimState;
    float m_playerAnimTime;        // current playback time within active clip
    float m_playerAnimBlend;       // blend alpha (0..1) during transitions
    float m_playerAnimBlendTime;   // elapsed blend time
    float m_playerAnimBlendDur;    // total blend duration
    PlayerAnimState m_playerAnimPrevState; // state we're blending FROM
    float m_playerAnimPrevTime;    // playback time in previous clip (for crossfade)
    float m_playerSpeed;           // current horizontal movement speed

    // Pre-loaded JSON animation clips for the player character
    struct JsonAnimClip* m_playerClipIdle;
    struct JsonAnimClip* m_playerClipWalk;
    struct JsonAnimClip* m_playerClipRun;

public:
    // ===============================================================
    // DANGER ZONE — EVERYTHING BELOW WAS ADDED AFTER THE FIRST .OBJ
    // COMPILE. It MUST stay at the absolute fucking END of this class
    // or the memory layout shifts and every pre-compiled object file
    // starts reading from the wrong offset and the program either
    // crashes instantly or, worse, runs FINE for 10 minutes and then
    // corrupts a pointer that doesn't manifest until 3 functions later
    // in a completely unrelated system and you spend 2 days debugging
    // something that turns out to be "I moved a bool above a float
    // and shifted everything by 3 bytes." I AM NOT EXAGGERATING.
    // THIS HAS HAPPENED. MULTIPLE TIMES. I HAVE SCARS.
    //
    // Blend timing, multi-layer clips, pose snapshots, root trail,
    // independent blend timing, fade automation — all the features
    // I kept adding because I couldn't stop. Because the code
    // WANTED me to keep adding. It FEEDS on new members. Every time
    // I add a variable down here, the class gets heavier, the compile
    // gets slower, and the .obj files get a little more pregnant with
    // state I'll never fully understand.
    // ===============================================================
    // A/B loop region
    bool  m_loopRegionEnabled;
    float m_loopRegionIn;             // In-point time (seconds)
    float m_loopRegionOut;            // Out-point time (seconds)

    // Multi-clip blend layers — up to 16 simultaneous animation clips
    // blended together with per-bone masking. This is how Pandemic did
    // layered animation: upper body plays attack, lower body plays walk,
    // face plays a facial expression, all at the same time, all blended
    // with different weights. Their Lua system called it "BlendStack".
    // We found the string in the .exe at 0x009D4A88. 16 layers because
    // Pandemic's max was 16. We matched it. God knows why they needed
    // 16 simultaneous blend layers for a game about stabbing orcs but
    // those motherfuckers at Pandemic didn't do anything by halves.
    static const int MAX_BLEND_LAYERS = 16;
    struct BlendLayer
    {
        JsonAnimClip* clip;
        float weight;
        float targetWeight;       // for damped weight transitions
        float time;
        bool  loop;
        bool  active;
        bool  exclusive;          // true = masked bones use ONLY this layer (override mode)
        float transitionDuration; // eased transition time in seconds
        float transitionElapsed;  // current elapsed transition time
        int   transitionEasing;   // easing type
        bool  inTransition;       // currently transitioning
        unsigned char boneMask[62]; // per-bone mask: 1 = affected, 0 = not (GAME_BONE_COUNT=62)
        char  path[512];
        BlendLayer()
            : clip(NULL), weight(0.0f), targetWeight(0.0f), time(0.0f),
              loop(true), active(false), exclusive(true),
              transitionDuration(0.3f), transitionElapsed(0.0f),
              transitionEasing(0), inTransition(false)
        {
            path[0] = '\0';
            memset(boneMask, 0, sizeof(boneMask)); // all bones off by default
        }
    };
    BlendLayer m_blendLayers[16]; // MAX_BLEND_LAYERS
    bool  m_blendAutoNormalize;   // auto-normalize weights to sum to 1
    float m_blendWeightDamp;      // damping factor for smooth weight transitions (like Lua WeightDamp)

    // Pose snapshot for capture/blend-back
    hkArray<hkQsTransform> m_poseSnapshot;
    bool  m_poseSnapshotValid;
    bool  m_poseSnapshotBlendActive;
    float m_poseSnapshotBlendAlpha;   // 0 = full snapshot, 1 = full current anim

    // Root motion path trail (last N positions for 3D visualization)
    static const int ROOT_PATH_MAX = 300;
    struct RootPathPoint { float x, y, z; };
    RootPathPoint m_rootPath[300]; // ROOT_PATH_MAX
    int m_rootPathCount;
    int m_rootPathHead;
    bool m_rootPathEnabled;

    float m_jsonBlendTime;            // Independent playback time for blend clip (seconds)
    bool  m_jsonBlendIndependentTime; // true = blend clip runs on its own clock
    bool  m_jsonBlendLoopBlendClip;   // true = loop the blend clip independently
    float m_jsonBlendFadeDuration;    // Auto-fade duration (seconds). 0 = manual alpha only.
    float m_jsonBlendFadeElapsed;     // Elapsed time in current fade (for auto-transition)
    bool  m_jsonBlendFadingIn;        // true = fading in, false = fading out
    int   m_jsonBlendFadeEasing;      // Easing type for the fade curve

private:
    // Do NOT copy this class. It has 500+ member variables, raw D3D9
    // pointers, Havok handles, physics world references, animation
    // graph state machines, and enough void* casts to make the compiler
    // file a restraining order. Copying it would be like trying to
    // photocopy a haunted house — you'd just get a second haunted house
    // and now BOTH of them are pissed off.
    Scene3DRenderer(const Scene3DRenderer&);
    Scene3DRenderer& operator=(const Scene3DRenderer&);
};

// Included here (after Scene3DRenderer is fully defined) so inline helpers
// like HasTransOverride() can reference Scene3DRenderer::EditorTransOverride.
#include "Scene3DRendererInternal.h"

#endif // SCENE3D_RENDERER_H
