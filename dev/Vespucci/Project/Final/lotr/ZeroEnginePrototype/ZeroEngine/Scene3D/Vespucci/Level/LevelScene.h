// LevelScene.h — Raising Pandemic's Dead Worlds From Their Binary Graves
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// "We are all visitors to this time, this place. We are just passing
// through." — Aboriginal proverb. Pandemic's level designers were
// passing through when they built Minas Tirith, Helm's Deep, Osgiliath,
// the Black Gate. They placed every barrel, every torch, every fucking
// cobblestone by hand in their internal editor. Then EA shut the studio
// and all that work became 0s and 1s on a disc nobody plays anymore.
//
// This class loads their ENTIRE levels back into existence. Thousands
// of mesh instances, each with a world transform, material references,
// LOD ranges, collision shapes. We read the PAK/BIN binary, resolve
// every CRC hash to a model name, extract vertex/index buffers from
// Block2, create D3D9 geometry, and render it all at 60fps. Their
// levels are ALIVE again. Every goddamn cobblestone, exactly where
// they left it 17 years ago.
// -----------------------------------------------------------------------
#pragma once

// LevelScene.h
// Loads and renders a full LOTRC level directly from the in-memory LevelReader
// data (no pre-extracted JSON files required).
//
// Data pipeline:
//   LevelReader::GetGameObjs()    -> per-instance mesh CRC + full 4x4 WorldTransform
//   LevelReader::GetBlock1()      -> ModelInfo / VBuffInfo / IBuffInfo structs
//   LevelReader::GetBlock2()      -> raw vertex and index data
//   LevelReader::GetBinAssetData() -> raw DDS bytes for textures
//
// Rendering uses D3D9 VB/IB with fixed-function pipeline.

#include <string>
#include <vector>
#include <map>
#include <set>
#include <stdint.h>

class GameShaderCache;

// Forward-declare LevelReader (full definition in LevelReader.h, included by .cpp)
namespace ZeroEngine {
    class LevelReader;
}

struct IDirect3DDevice9;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;
struct IDirect3DTexture9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DVertexDeclaration9;
struct IDirect3DSurface9;
struct IDirect3DCubeTexture9;
struct ID3DXFont;

// ── Vertex format ────────────────────────────────────────────────────────────
// FVF: D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1  (36 bytes)
// Color holds per-vertex baked AO/lighting from the game's Color(0) channel.
struct LevelVertex
{
    float x, y, z;        // position (12 bytes)
    float nx, ny, nz;     // decoded normal (12 bytes)
    uint32_t color;       // vertex color / AO (4 bytes)
    float u, v;           // UV0 (8 bytes)
    float tx, ty, tz, tw; // tangent xyz + handedness w (16 bytes)
    float slr, slg, slb, slw; // static lighting RGBA (16 bytes) — game texcoord5
};  // Total: 68 bytes

// ── One submesh (maps to one BufferInfo entry in the model) ──────────────────
struct LevelMeshPart
{
    IDirect3DVertexBuffer9* vb;
    IDirect3DIndexBuffer9*  ib;
    int                     vertexCount;
    int                     indexCount;     // total index count (triangles*3)
    IDirect3DTexture9*      diffuseTex;     // may be NULL — tex0 (_D diffuse/albedo)
    IDirect3DTexture9*      normalTex;      // may be NULL — tex2 or tex4 (_N normal map)
    IDirect3DTexture9*      specularTex;    // may be NULL — tex3 (_S specular map)
    IDirect3DTexture9*      detailTex;      // may be NULL — tex1 (_GV detail/grunge)
    IDirect3DTexture9*      emissiveTex;    // may be NULL — tex5 (emissive glow)
    // Terrain multi-layer (Mat4, kind=1)
    IDirect3DTexture9*      terrainLayer2;  // tex2 — second terrain diffuse layer
    IDirect3DTexture9*      terrainLayer3;  // tex4 — third terrain diffuse layer
    IDirect3DTexture9*      terrainMask0;   // mask0 — blend weight texture
    IDirect3DTexture9*      terrainMask1;   // mask1
    IDirect3DTexture9*      terrainMask2;   // mask2
    uint32_t                texCrc;         // diffuse texture CRC (for asset tree highlight)
    float                   matColor[4];    // per-material tint (game VS c197 → COLOR0)

    LevelMeshPart()
        : vb(NULL), ib(NULL), vertexCount(0), indexCount(0),
          diffuseTex(NULL), normalTex(NULL), specularTex(NULL), detailTex(NULL),
          emissiveTex(NULL), terrainLayer2(NULL), terrainLayer3(NULL),
          terrainMask0(NULL), terrainMask1(NULL), terrainMask2(NULL), texCrc(0)
    { matColor[0]=matColor[1]=matColor[2]=matColor[3]=1.0f; }

    // Position-only CPU mirror of the VB, populated at load time.
    // Three floats per vert, sized 3 * vertexCount. Used by the
    // embedded-weapon editor as the "original baseline" for live
    // preview transforms — the VB itself is D3DUSAGE_WRITEONLY so we
    // can't read positions back from it. Empty for models that were
    // never set up by loadModelFromBinary (defensive). Member appended
    // AT END per the struct-layout commandment.
    std::vector<float>           positionsCpu;

    // ── Skinning CPU mirror (END of struct per layout rule) ────────────
    // BlendIndices + BlendWeight are not part of LevelVertex, so the GPU
    // never sees them — but the bone-weight selection mode of the Adjust
    // Baked Weapon panel needs to ask "which slot[0] verts are weighted to
    // Bone_LHand?" without re-parsing the BIN. Cached here as one u32 per
    // vert per stream (4 packed u8 bytes each). Empty on parts that don't
    // carry skin data (props, terrain). 8 bytes/vert ≈ 27 KB on a 3260-
    // vert CRD; negligible.
    std::vector<uint32_t>        blendIndicesCpu;  // each entry = 4 palette indices packed u8.u8.u8.u8
    std::vector<uint32_t>        blendWeightsCpu;  // each entry = 4 weights packed u8.u8.u8.u8 (0..255)

    // Index buffer CPU mirror — populated at load time. Lets the triangle
    // picker do ray-vs-triangle tests without locking the GPU IB (which
    // was created D3DUSAGE_WRITEONLY). Each entry is one u32 index;
    // size = indexCount. Triangle T uses indices [T*3], [T*3+1], [T*3+2].
    // END of struct per layout commandment.
    std::vector<uint32_t>        indicesCpu;
};

// ── A unique model asset (shared by all instances that reference it) ──────────
struct LevelModel
{
    std::string                  name;
    std::vector<LevelMeshPart>   parts;
    float                        localMin[3]; // model-space AABB
    float                        localMax[3];
    bool                         hasBounds;
    // Crowd Mesh Builder support — resolved bone names from the model's
    // bone CRC array (ModelInfo.bones_offset). Indexed the same way the
    // skeleton palette is indexed at runtime, so findBoneIndexByName
    // returns something useful for "where do I parent this weapon to".
    // Empty for models that have no skin/bone data (props, terrain).
    // APPENDED AT END per the struct-layout-end commandment.
    std::vector<std::string>     bones;
    // Rest-pose WORLD-space bone matrices, 16 floats per bone, row-major
    // (D3D9 compatible). bones.size() entries × 16. Populated at load time
    // by cascading the model's bone_transforms through bone_parents. Used
    // by the Crowd Mesh Builder preview to attach a weapon to a bone:
    //   weapon.mat = parent.mat × restBoneWorld[boneIdx] × offsetTransform
    // Empty for models that have no bone data. APPENDED AT END.
    std::vector<float>           restBoneWorld;

    // ============================================================
    // Skin palette: the AUTHORITATIVE bind matrices Pandemic shipped.
    // ============================================================
    // The cascade product in restBoneWorld is WRONG for some bones.
    // It drifts from Pandemic's authored bind by 10+ degrees of rotation
    // and significant translation. Why? Because the engine's actual
    // bind-pose convention has subtleties our cascade misses (origin
    // pose adjustments baked into skin_binds at content build time).
    //
    // The PAK ships an INVERSE-bind matrix per palette entry in
    // skin_binds, paired with skin_order which maps bind_idx -> bone_idx.
    // For ANY bone that's in the palette, the authoritative bind is
    // inverse(skin_binds[bind_idx]). For everything else, we fall back
    // to restBoneWorld and hope the cascade matches.
    //
    // The Crowd Mesh Builder preview MUST use the authoritative bind
    // or it lies to the user about where the weapon will land at bake
    // time. Lying preview = trial-and-error tuning hell forever.
    //
    // skinOrder[bind_idx] = bone_idx (uint32_t per entry)
    // skinBindsRowMajor[bind_idx * 16 .. bind_idx*16+15] = inverse bind
    //   matrix for that palette slot, row-major 4x4, D3D9 compatible.
    // Both are EMPTY for models with no skin palette (props, terrain).
    // APPENDED AT END.
    std::vector<uint32_t>        skinOrder;
    std::vector<float>           skinBindsRowMajor;

    // Helper: look up the authoritative bind-pose world matrix for a
    // bone. Returns 16 floats (row-major) if the bone is in the skin
    // palette; falls back to restBoneWorld[boneIdx] if not. Returns
    // identity if neither exists. Pass-by-output to avoid heap thrash.
    bool authoritativeBindWorld(int boneIdx, float outMat16[16]) const;

    void release(); // releases D3D9 resources
};

// ── A placed instance in the level ───────────────────────────────────────────
struct LevelInstance
{
    LevelModel* model;      // non-owning; owned by LevelScene::m_modelCache
    float       mat[16];    // row-major world matrix (D3D9 compatible)
    // Object metadata (for inspector)
    std::string objName;
    std::string typeName;
    std::string meshName;
    uint32_t    guid;
    uint32_t    parentGuid;
    int32_t     gameModeMask;
    uint32_t    block1WtOffset;  // byte offset in decompressed block1 for binary write-back
    // World-space AABB (computed from model AABB + world matrix)
    float       bboxMin[3];
    float       bboxMax[3];
    // Phase 3 XSI: which templateLayer this instance belongs to (0 = none).
    // APPENDED AT END per the struct-layout-end rule. Used by the layer
    // manager to hide/lock/isolate whole layers without touching the
    // GameModeMask path or the per-entity selection lists.
    uint32_t    layerGuid;
    // 3dCrowd back-reference. crowdItemIdx >= 0 means this LevelInstance was
    // pushed by the crowd loader and points into LevelScene::m_crowdItems.
    // Gizmo drags on this instance write back to m_crowdItems[i].instances[v]
    // instead of going through the GameObj FieldEdit pipeline (because
    // crowd data lives in Block2, not Block1, and is NOT addressable by a
    // single block1WtOffset). -1/-1 for everything that isn't a crowd member.
    // APPENDED AT END per the struct-layout-end rule. Read the rule, lazy fuck.
    int         crowdItemIdx;
    int         crowdValIdx;
    // Crowd Mesh Builder preview — when previewParentIdx >= 0, this
    // instance's mat[] is recomputed each frame from
    // m_instances[previewParentIdx]'s bone matrix at previewParentBoneIdx,
    // multiplied by previewOffset transform. Used only by the Crowd Mesh
    // Builder panel for visual offset tuning; not saved, not pickable for
    // normal selection. APPENDED AT END per the struct-layout-end rule.
    int         previewParentIdx;     // -1 = not preview
    int         previewParentBoneIdx;
    float       previewOffset[6];     // XYZ + Yaw/Pitch/Roll (radians)

    // Default-init the crowd back-reference fields to -1 so legacy push
    // sites (regular entities, kit templates, anywhere a LevelInstance
    // is constructed without explicitly assigning every field) don't
    // leak garbage values into the gizmo writeback path. Body init only —
    // initializer list would force declaration-order warnings on the
    // existing field cluster above. The other fields can stay
    // uninitialized; every existing call site overwrites them.
    // Preview fields default to "not a preview" so every existing push
    // site keeps working without manual zeroing.
    LevelInstance()
    {
        crowdItemIdx = -1;
        crowdValIdx  = -1;
        previewParentIdx     = -1;
        previewParentBoneIdx = -1;
        previewOffset[0] = previewOffset[1] = previewOffset[2] = 0.0f;
        previewOffset[3] = previewOffset[4] = previewOffset[5] = 0.0f;
    }
};

// ── A logic/editor object rendered as a wireframe shape ──────────────────────
// For game objects that have no visual mesh: spawn points, triggers, cameras,
// emitters, capture points, etc.  Shape is drawn as a colored wireframe.
struct LevelEditorObj
{
    float       mat[16];      // world transform (position + orientation)
    float       size[3];      // half-extents (box) or [radius,0,0] (sphere)
    float       outer;        // outer radius (sphere/capsule)
    uint32_t    color;        // D3D ARGB (from editor_color, or type-derived)
    std::string type;         // type_name (e.g. "spawn_point", "FED_camera")
    std::string shape;        // "Sphere", "Box", "Billboard", etc.
    uint32_t    guid;         // object GUID (for linking to Event Graph)
    uint32_t    parentGuid;  // parent object GUID (for spawn_node → spawn_point)
    std::string name;         // object name
    int32_t     gameModeMask; // gamemode visibility mask (-1=all)
    uint32_t    block1WtOffset; // byte offset of WorldTransform in Block1 (0=unknown)
    uint32_t    block1TfOffset; // byte offset of Transform (local) in Block1 (0=unknown)
    // Phase 3 XSI: which templateLayer owns this editor obj (0=none).
    // Appended at END. Same struct-layout-end commandment as the
    // instance struct above. Read it.
    uint32_t    layerGuid;
};

// ── 3dCrowd ───────────────────────────────────────────────────────────────────
// One crowd member placed in the world. Mirrors the binary CrowdVal record
// (position, Y-axis rotation, squared-distance LOD cutoff). Pandemic packs
// HUNDREDS of these into one item because they all share an archetype.
struct LevelCrowdInstance
{
    float    position[3];   // world-space xyz
    float    rotation;      // yaw in radians (single float — crowds stand upright, no pitch/roll)
    float    lod;           // squared cutoff distance — engine compares dist² to this
};

// One crowd archetype + its full instance list. Maps 1:1 to the binary
// CrowdItem (CrowdHeader 28B + Crc[anim_num] + CrowdVal[inst_num]).
// "key" is a static pose-snapshot mesh used as a cheap LOD; "key_main" is
// the master rig (the one our viewer loadModelFromBinary actually loads).
// "key_right" / "key_left" are weapon model CRCs that the original game
// parents to skeleton slot bones — our viewer doesn't attach them yet.
// playbackRate is the unk_4 field — animation time scale (0.0=static prop,
// 0.2-0.4=slow idle breathing, 1.0=normal, 5.0=combat frenzy).
struct LevelCrowdItem
{
    uint32_t    meshKey;            // CrowdHeader.key — static LOD pose mesh CRC
    uint32_t    modelKey;           // CrowdHeader.key_main — master rig CRC
    uint32_t    rightHandKey;       // CrowdHeader.key_right — right-hand weapon CRC (0=empty)
    uint32_t    leftHandKey;        // CrowdHeader.key_left — left-hand weapon CRC (0=empty)
    float       playbackRate;       // CrowdHeader.unk_4 — animation time scale
    std::string meshKeyName;        // resolved string for inspector display
    std::string modelKeyName;
    std::string rightHandName;
    std::string leftHandName;
    std::vector<uint32_t>    animationKeys;    // CRCs from animations[] — clip names
    std::vector<std::string> animationNames;   // resolved strings, same indexing
    std::vector<LevelCrowdInstance> instances; // per-placement xyz/yaw/lod
};

// ── A spline path (position or target track) ─────────────────────────────────
struct LevelSpline
{
    uint32_t    guid;
    std::string name;
    struct Node { float x, y, z, s; }; // x,y,z = position, s = arc-length
    std::vector<Node> nodes;
};

// ── A cinematic camera referencing two splines ───────────────────────────────
struct LevelCinematicCamera
{
    uint32_t    guid;
    std::string name;
    float       mat[16];          // WorldTransform
    float       fov;
    float       totalDuration;
    float       positionTravelTime;
    float       targetTravelTime;
    uint32_t    positionTrackGuid;
    uint32_t    targetTrackGuid;
    LevelSpline* positionTrack;   // resolved pointer (may be NULL)
    LevelSpline* targetTrack;     // resolved pointer (may be NULL)
};

// ── A world collision mesh (BVTree triangle data rendered as wireframe) ──────
struct LevelWorldCollisionMesh
{
    IDirect3DVertexBuffer9* vb;
    IDirect3DIndexBuffer9*  ib;
    int                     vertCount;
    int                     triCount;
    float                   translation[3]; // world offset

    LevelWorldCollisionMesh() : vb(NULL), ib(NULL), vertCount(0), triCount(0)
    { translation[0]=translation[1]=translation[2]=0; }
    void release(); // implemented in LevelScene.cpp (needs full D3D9 type definitions)
};

// ── A collision shape visualized on the map ──────────────────────────────────
struct LevelCollisionShape
{
    float       worldMat[16];   // combined instance + shape local transform
    float       halfExt[3];     // half-extents (box) or {radius,0,0} (sphere)
    float       radius;         // sphere/capsule radius
    float       pt1[3], pt2[3]; // capsule/cylinder endpoints (local)
    int         kind;           // 0=generic,1=box,2=sphere,3=capsule,4=cylinder,5=convex,6=bvtree
    int         instanceIdx;    // which instance this belongs to (-1 if global)
    uint32_t    modelCrc;       // model CRC for identification
    uint32_t    block1Offset;   // absolute Block1 offset for editing
};

// ── A node-based collision volume (Collision entity rendered as wireframe wall)
struct LevelCollisionVolume
{
    struct Node { float x, y, z; }; // local-space node position
    std::vector<Node> nodes;
    float       height;         // extrusion height
    float       worldPos[3];    // WorldTransform translation
    uint32_t    guid;
    std::string name;
    int32_t     gameModeMask;
    bool        closed;         // closed loop or open path
    uint32_t    collFlags;      // collision flag profile (LC_COLL_*)
};

// ── The scene ─────────────────────────────────────────────────────────────────
class LevelScene
{
public:
    LevelScene();
    ~LevelScene();

    // Must be called before load().
    void setDevice(IDirect3DDevice9* device);

    // Load from the already-parsed LevelReader.  No file I/O is performed here;
    // all data is read from the in-memory block1/block2 buffers.
    // Returns true on success.  Partial loads (some models missing) are allowed.
    bool load(const ZeroEngine::LevelReader& reader);
    void unload();

    bool        isLoaded()      const { return m_loaded; }
    int         instanceCount() const { return (int)m_instances.size(); }
    int         modelCount()    const { return (int)m_modelCache.size(); }
    int         drawCallCount() const { return m_drawCallsLast; }
    const std::string& levelName() const { return m_levelName; }

    // Axis-aligned bounding box of all loaded geometry (world space).
    // Only valid when isLoaded() && hasBounds().
    bool hasBounds() const { return m_boundsValid; }
    void getBounds(float outMin[3], float outMax[3]) const
    {
        outMin[0]=m_boundsMin[0]; outMin[1]=m_boundsMin[1]; outMin[2]=m_boundsMin[2];
        outMax[0]=m_boundsMax[0]; outMax[1]=m_boundsMax[1]; outMax[2]=m_boundsMax[2];
    }

    // Render all instances.
    // Call after the camera/view matrices have been pushed to D3D9.
    void render();

    // Hot-reload shaders from external .hlsl files (F12 key).
    // Returns true if any shaders were recompiled.
    bool hotReloadShaders();

    // Save a screenshot of the backbuffer to a BMP file (Ctrl+F12).
    bool saveScreenshot(const char* path);

    // Release all D3DPOOL_DEFAULT resources before device Reset (fullscreen toggle).
    // Must be called before IDirect3DDevice9::Reset(), otherwise Reset fails.
    void releaseDefaultPoolResources();

    // Toggle visibility of editor/logic objects (trigger boxes, spawn points, splines, etc.)
    void setEditorObjsVisible(bool v) { m_showEditorObjs = v; }
    bool editorObjsVisible() const    { return m_showEditorObjs; }
    void toggleEditorObjs()           { m_showEditorObjs = !m_showEditorObjs; }

    // PathLink / CapturePoint / SpawnChain visualization toggles
    void setPathLinksVisible(bool v)    { m_showPathLinks = v; }
    bool pathLinksVisible() const       { return m_showPathLinks; }
    void setCaptureRadiiVisible(bool v) { m_showCaptureRadii = v; }
    bool captureRadiiVisible() const    { return m_showCaptureRadii; }
    void setSpawnChainsVisible(bool v)  { m_showSpawnChains = v; }
    bool spawnChainsVisible() const     { return m_showSpawnChains; }
    void setAIGoalsVisible(bool v)      { m_showAIGoals = v; }
    bool aiGoalsVisible() const         { return m_showAIGoals; }
    void setSoundRadiiVisible(bool v)   { m_showSoundRadii = v; }
    bool soundRadiiVisible() const      { return m_showSoundRadii; }

    // GameModeMask filter: only render instances matching this mode bit.
    // -1 = show all, 0 = Campaign(bit0), 1 = TDM(bit1), 3 = Conquest(bit3), etc.
    void setGameModeFilter(int filterBit) { m_gameModeFilter = filterBit; }
    void setGameModeShowGlobals(bool show) { m_gmfShowGlobals = show; }
    void setGameModeShowScripts(bool show) { m_gmfShowScripts = show; }
    void setGameModeBitMask(unsigned int mask) { m_gmfBitMask = mask; }
    int  gameModeFilter() const { return m_gameModeFilter; }

    // Distance cull settings
    void setEditorObjMaxDist(float d)   { m_editorObjMaxDist = d; }
    void setEditorObjFadeStart(float d) { m_editorObjFadeStart = d; }
    void setEditorObjMinDist(float d)   { m_editorObjMinDist = d; }
    int  editorObjVisibleCount() const  { return m_editorObjVisibleCount; }
    int  editorObjCount() const         { return (int)m_editorObjs.size(); }
    int  findEditorObjByGuid(uint32_t guid) const {
        for (int i = 0; i < (int)m_editorObjs.size(); ++i)
            if (m_editorObjs[i].guid == guid) return i;
        return -1;
    }
    void removeByGuid(uint32_t guid) {
        // Remove from editor objects
        for (int i = (int)m_editorObjs.size() - 1; i >= 0; --i)
            if (m_editorObjs[i].guid == guid) { m_editorObjs.erase(m_editorObjs.begin() + i); break; }
        // Remove from instances
        for (int i = (int)m_instances.size() - 1; i >= 0; --i)
            if (m_instances[i].guid == guid) { m_instances.erase(m_instances.begin() + i); break; }
    }
    void setEditorObjCategoryMask(unsigned int m) { m_editorObjCategoryMask = m; }
    void setEditorObjLabelMaxCount(int n) { m_editorObjLabelMaxCount = n; }
    // Phase 1 XSI authoring: F4 event-wire toggle + isolate-chain getters
    // and setters. The host writes these from imgui_glue args, the render
    // pass reads them. No magic, just a plain bridge.
    bool        eventWiresVisible()  const { return m_showEventWires; }
    uint32_t    eventWireFocusGuid() const { return m_eventWireFocusGuid; }
    void        setEventWiresVisible(bool b)        { m_showEventWires = b; }
    void        setEventWireFocusGuid(uint32_t g)   { m_eventWireFocusGuid = g; }
    // Phase 3 XSI: layer manager bridge.
    // Pass GUID arrays + counts because the host has them packed flat
    // already (see imgui_glue ArgsBuilder block in ZeroEngine3DViewport.cpp).
    // We rebuild the sets each call. With layer counts in the dozens at
    // most, this is fine.
    void setHiddenLayers(const uint32_t* guids, int count) {
        m_hiddenLayers.clear();
        for (int i = 0; i < count && guids; ++i) m_hiddenLayers.insert(guids[i]);
    }
    void setLockedLayers(const uint32_t* guids, int count) {
        m_lockedLayers.clear();
        for (int i = 0; i < count && guids; ++i) m_lockedLayers.insert(guids[i]);
    }
    void     setIsolatedLayer(uint32_t g) { m_isolatedLayer = g; }
    void     setActiveLayer(uint32_t g)   { m_activeLayer = g; }
    uint32_t isolatedLayer() const        { return m_isolatedLayer; }
    uint32_t activeLayer()   const        { return m_activeLayer; }
    bool     layerHidden(uint32_t lg) const { return m_hiddenLayers.find(lg) != m_hiddenLayers.end(); }
    bool     layerLocked(uint32_t lg) const { return m_lockedLayers.find(lg) != m_lockedLayers.end(); }
    // Single visibility predicate. Centralizes the "should anything on
    // this layer render right now" decision so the instance pass and
    // the editor-obj pass can't drift apart.
    bool layerVisibleForRender(uint32_t lg) const {
        if (m_isolatedLayer != 0 && lg != m_isolatedLayer) return false;
        if (m_hiddenLayers.find(lg) != m_hiddenLayers.end()) return false;
        return true;
    }
    // Phase 4 XSI: viewport gizmo control surface.
    // Renderer owns the math and the state. Host calls gizmoBeginDrag
    // on LMB-down (over the viewport, not over ImGui), gizmoUpdateDrag
    // on each WM_MOUSEMOVE while gizmoDragging() is true, gizmoEndDrag
    // on LMB-up. Suppress instance-pick while a gizmo drag is live.
    int   gizmoMode()       const { return m_gizmoMode; }
    int   gizmoSpace()      const { return m_gizmoSpace; }
    bool  gizmoDragging()   const { return m_gizmoDragging; }
    float gizmoSnap()       const { return m_gizmoSnap; }
    void  setGizmoMode(int m)   { m_gizmoMode = (m < 0 || m > 3) ? 0 : m; }
    void  setGizmoSpace(int s)  { m_gizmoSpace = (s == 1) ? 1 : 0; }
    void  setGizmoSnap(float s) { m_gizmoSnap = (s < 0.0f) ? 0.0f : s; }
    bool     gizmoBeginDrag(const float rayOrigin[3], const float rayDir[3]);
    void     gizmoUpdateDrag(const float rayOrigin[3], const float rayDir[3]);
    uint32_t gizmoEndDrag(ZeroEngine::LevelReader* reader);
    void     gizmoCancelDrag();
    // Where the gizmo currently anchors. Returns true if there is a
    // valid selection and writes a 4x4 row-major matrix into outMat.
    // The renderer uses this to draw the gizmo, the host can use it
    // to know whether to even ask for a drag.
    bool     gizmoGetTargetMatrix(float outMat[16]) const;
    void setEditorObjSearch(int mode, const char* term) {
        m_editorObjSearchMode = mode;
        if (term) { strncpy(m_editorObjSearchTerm, term, 127); m_editorObjSearchTerm[127] = '\0'; }
        else m_editorObjSearchTerm[0] = '\0';
    }

    // Centralized editor object visibility check.
    // Returns: 0.0 = skip, 1.0 = full draw, 0.0-1.0 = fade zone.
    // Also outputs the resolved label and color via lbl/fixedCol.
    // All filtering (relation, gamemode, distance, category, search) goes here.
    float shouldDrawEditorObj(int idx, const float camPos[3],
                              char lbl[8], unsigned long& fixedCol) const;

    // Object picking — cast ray from camera, return instance index or -1.
    int pickInstance(const float rayOrigin[3], const float rayDir[3]) const;

    // Hover/selection state for inspector highlight
    void setHoveredInstance(int idx) { m_hoveredIdx = idx; }
    void setSelectedInstance(int idx) { m_selectedIdx = idx; }
    int  hoveredInstance()  const { return m_hoveredIdx; }
    int  selectedInstance() const { return m_selectedIdx; }
    const LevelInstance* getInstance(int idx) const {
        if (idx < 0 || idx >= (int)m_instances.size()) return 0;
        return &m_instances[idx];
    }
    // Move a selected instance to a new position (updates mat[12..14])
    void setInstancePosition(int idx, float x, float y, float z) {
        if (idx < 0 || idx >= (int)m_instances.size()) return;
        m_instances[idx].mat[12] = x;
        m_instances[idx].mat[13] = y;
        m_instances[idx].mat[14] = z;
    }
    int getInstanceCount() const { return (int)m_instances.size(); }

    // Editor object picking — cast ray, return editor obj index or -1.
    // outDist receives the ray hit distance (for priority comparison).
    int pickEditorObj(const float rayOrigin[3], const float rayDir[3], float* outDist = 0) const;

    // Editor object hover/selection
    void setHoveredEditorObj(int idx)  { m_hoveredEditorIdx = idx; }
    void setSelectedEditorObj(int idx) { m_selectedEditorIdx = idx; }
    int  hoveredEditorObj()  const { return m_hoveredEditorIdx; }
    int  selectedEditorObj() const { return m_selectedEditorIdx; }
    const LevelEditorObj* getEditorObj(int idx) const {
        if (idx < 0 || idx >= (int)m_editorObjs.size()) return 0;
        return &m_editorObjs[idx];
    }
    int getEditorObjCount() const { return (int)m_editorObjs.size(); }

    // Move a selected editor object to a new world position
    void setEditorObjPosition(int idx, float x, float y, float z) {
        if (idx < 0 || idx >= (int)m_editorObjs.size()) return;
        m_editorObjs[idx].mat[12] = x;
        m_editorObjs[idx].mat[13] = y;
        m_editorObjs[idx].mat[14] = z;
    }

    // Add a new editor object at runtime (for entity creation)
    void addEditorObj(const LevelEditorObj& eo) { m_editorObjs.push_back(eo); }

    // Add a new rendered instance at runtime (for entity creation with mesh)
    void addInstance(const LevelInstance& inst) { m_instances.push_back(inst); }

    // ── 3dCrowd authoring API ────────────────────────────────────────────
    // The crowd block in sub_blocks2 is now a first-class mutable thing,
    // not a dead read-only blob. Every LevelInstance that came from a
    // CrowdVal carries its (item, val) back-reference, so a gizmo drag
    // on a crowd member reaches back through getCrowdItemsMut() and
    // writes the new pos/rot into the source-of-truth array. Save time
    // dumps the whole thing back over sub_blocks2/3dcrowd.json and
    // lotrc_rs -c repacks it via Crowd::dump_bytes. Clean round-trip,
    // no in-place byte shifting bullshit.
    const std::vector<LevelCrowdItem>& getCrowdItems() const { return m_crowdItems; }
    std::vector<LevelCrowdItem>&       getCrowdItemsMut()    { return m_crowdItems; }
    bool   isCrowdDirty()  const { return m_crowdDirty; }
    void   setCrowdDirty(bool v) { m_crowdDirty = v; }
    int    getCrowdItemCount() const { return (int)m_crowdItems.size(); }
    int    getCrowdInstanceCount(int item) const {
        if (item < 0 || item >= (int)m_crowdItems.size()) return 0;
        return (int)m_crowdItems[item].instances.size();
    }
    // Returns true if the (item,val) pair points to a live crowd member.
    bool   isValidCrowdInstance(int item, int val) const {
        if (item < 0 || item >= (int)m_crowdItems.size()) return false;
        if (val  < 0 || val  >= (int)m_crowdItems[item].instances.size()) return false;
        return true;
    }
    // Mutate one crowd placement. Marks dirty. Returns false if (item,val)
    // is out of range. Use this from the gizmo writeback path.
    bool   setCrowdInstancePosRot(int item, int val,
                                   float x, float y, float z, float rotY);
    // Append a new instance to an existing crowd item. Returns the new val
    // index (== old instance count) or -1 if item is out of range.
    int    addCrowdInstanceToItem(int item, float x, float y, float z, float rotY, float lod);
    // Remove one instance. Subsequent instances shift down by one — any
    // LevelInstance with crowdValIdx > val for the same crowdItemIdx will
    // be re-tagged by the caller (or just rebuild the LevelInstances).
    bool   deleteCrowdInstance(int item, int val);
    // Append a brand-new crowd item with a template archetype. Returns
    // the new item index, or -1 on failure.
    int    addCrowdItem(uint32_t modelCrc, const std::string& modelName,
                        uint32_t meshCrc,  const std::string& meshName,
                        uint32_t rightCrc, const std::string& rightName,
                        uint32_t leftCrc,  const std::string& leftName,
                        float playbackRate);

    // ── 3dCrowd Editor panel authoring helpers ──────────────────────────
    // Direct header mutators called by the editor. Each marks crowd dirty.
    // setCrowdItemModelKey + addCrowdItemAnim also touch the model cache
    // (Pass-1 pattern) so the viewport reflects the change immediately
    // without needing a level reload. Returns false if 'item' is out of
    // range; addCrowdItemAnim returns false if anim already in the list.
    bool   renameCrowdItemKey   (int item, uint32_t newCrc, const std::string& name);
    bool   setCrowdItemModelKey (int item, uint32_t newCrc, const std::string& name,
                                  const ZeroEngine::LevelReader& reader);
    bool   setCrowdItemMeshKey  (int item, uint32_t newCrc, const std::string& name);
    bool   setCrowdItemRightKey (int item, uint32_t newCrc, const std::string& name);
    bool   setCrowdItemLeftKey  (int item, uint32_t newCrc, const std::string& name);
    bool   setCrowdItemPlayback (int item, float rate);
    bool   addCrowdItemAnim     (int item, uint32_t crc, const std::string& name);
    bool   removeCrowdItemAnim  (int item, int animIdx);
    int    duplicateCrowdItem   (int item);     // new index, key suffix "_copy"
    bool   deleteCrowdItemAt    (int item);

    // Autocomplete sources — read-only enumeration over the model cache
    // and the union of all currently-loaded crowd anim lists. Output is
    // appended to 'out' (not cleared) so callers can union multiple
    // prefixes. Names are returned in iteration order (unsorted).
    void   getModelNamesByPrefix(const char* prefix, std::vector<std::string>& out) const;
    void   getAllModelNames     (std::vector<std::string>& out) const;
    void   getAllCrowdAnimNames (std::vector<std::string>& out) const;
    // Skinned-only filter: every loaded model whose bones[] is non-empty.
    // Used by the Adjust Mesh by Bone panel — cold-cache models are
    // missing from this list, but ensureModelLoaded brings them in when
    // crowds reference them, which covers every CRD/CH the level uses.
    // Output appended (not cleared) so callers can union if needed.
    void   getSkinnedModelNames (std::vector<std::string>& out) const;

    // Crowd Mesh Builder support — enumerate bones of a cached model, find
    // a bone by name, spawn/clear preview instances that follow a parent
    // bone. The preview path is a visual-only thing for the offset-tuning
    // UI; it never round-trips through save and the per-frame mat[]
    // recompute (parent bone palette × offsetXYZ/YPR) is wired in a
    // later phase. Out-of-cache models return false / -1 / empty.
    bool getModelBoneNames(uint32_t modelCrc, std::vector<std::string>& out) const;
    int  findBoneIndexByName(uint32_t modelCrc, const std::string& boneName) const;
    int  spawnPreviewMesh(uint32_t childModelCrc,
                           int parentInstIdx,
                           int parentBoneIdx,
                           const float offsetXYZ[3],
                           const float offsetYPR[3]);
    void clearPreviewMeshes();

    // Focus mode — editor-only viewport gating. When on, rebuildCrowd-
    // Instances() skips every CrowdItem whose index != getCrowdFocusedItem().
    // Off restores the full set on next rebuild.
    void   setCrowdFocusMode    (bool on, int focusedItem);
    bool   getCrowdFocusMode    () const { return m_crowdFocusModeOn; }
    int    getCrowdFocusedItem  () const { return m_crowdFocusedItem; }

    // After mutating m_crowdItems, the LevelInstance list pushed by the
    // crowd loader is stale (its mat[] reflects the OLD position). Call
    // this to rebuild only the crowd-sourced LevelInstance entries from
    // current m_crowdItems data. Non-crowd LevelInstances are untouched.
    void   rebuildCrowdInstances();

    // Dump m_crowdItems as the same JSON shape the Rust parser emits
    // (an array of {header:{...}, animations:[...], instances:[...]}).
    // Writes to the given path, overwriting whatever is there. Used by
    // the save pipeline to replace the post-lotrc_rs-dump 3dcrowd.json
    // before the -c repack step reads it.
    bool   dumpCrowdAsJson(const std::string& path) const;

    // Look up a cached model by mesh CRC (returns NULL if not loaded)
    LevelModel* getModelByCrc(uint32_t meshCrc) const {
        std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.find(meshCrc);
        if (it != m_modelCache.end()) return it->second;
        return 0;
    }

    // Same as getModelByCrc except it actually fucking works for
    // authoring tools. Cache miss falls through to loadModelFromBinary
    // so the user can pick ANY CRD/CH name in the string table, not
    // just the handful some entity already happens to render. Returns
    // NULL only when the BIN itself has no bytes for the CRC; caches
    // NULL on failure so the UI thread doesn't burn cycles retrying
    // the same dead lookup every frame.
    LevelModel* ensureModelLoaded(const ZeroEngine::LevelReader& reader,
                                  uint32_t                       meshCrc,
                                  const std::string&             modelName);

    // Render one model using the same custom-shader path render() uses
    // for level instances. The caller is responsible for the render
    // target, depth buffer, viewport, and clearing. We push shader binds,
    // VS/PS constants (viewProj, camPos, lighting), per-part textures,
    // and DrawIndexedPrimitive. We do NOT save/restore device state — the
    // caller already does that around its RT swap. Returns false if the
    // shader pipeline is not ready (caller should fall back to FFP).
    //
    // Why this exists: the Model Viewer popup used fixed-function with
    // one texture stage. Looked nothing like the main viewport because
    // the main path is per-pixel Lambert + ambient + scattering. We
    // can't sensibly mirror that with FFP register combiners, so the
    // popup now calls into this method and gets identical material
    // output for a single isolated model in its own RT.
    bool drawSingleModelWithShaders(LevelModel*  model,
                                    const float  worldMat[16],
                                    const float  viewMat[16],
                                    const float  projMat[16]);

    // Embedded-weapon vertex editor support.
    //
    // The Crowd Mesh Builder bakes weapons by appending their verts to
    // slot[0] of a merged CRD. The vert range is recorded in
    // ze_embedded_weapons.json. These helpers let the popup mesh editor
    // adjust the weapon's position WITHOUT re-baking the whole model.
    //
    // snapshotEmbeddedWeaponVerts: locks slot[0]'s VB, copies the raw
    // positions for verts [vertFirst, vertFirst+vertCount) into outOrig
    // (size = vertCount*3 floats). Call once when the editor opens for
    // a given weapon; reuse the snapshot for every slider change.
    //
    // applyEmbeddedWeaponDelta: locks slot[0]'s VB and rewrites the same
    // vert range as worldDelta * orig_pos. worldDelta is the 4x4 (row-
    // major) bone-local transform conjugated back to world space, ie
    //   worldDelta = bind_world * delta_local * inv(bind_world)
    // where delta_local = T(xyz) * R(ypr). The caller builds it because
    // it owns the bind_world matrix (from the sidecar tag) and the user
    // XYZ/YPR slider values. We just push the transformed positions.
    // Both methods return false on bad model / out-of-range / lock fail.
    bool snapshotEmbeddedWeaponVerts(LevelModel* model,
                                     uint32_t    vertFirst,
                                     uint32_t    vertCount,
                                     std::vector<float>& outOrig);
    bool applyEmbeddedWeaponDelta(LevelModel*  model,
                                  uint32_t     vertFirst,
                                  uint32_t     vertCount,
                                  const float* origPositions,  // vertCount*3 floats
                                  const float  worldDelta[16]);

    // Bone-weight selection — works on ANY skinned mesh, baked-by-us or
    // shipped-by-Pandemic. Scans slot[0]'s cached BlendIndices /
    // BlendWeight (mirror populated at load) and returns every vert idx
    // where any of the 4 bone slots maps to skinOrder[paletteIdx] AND
    // its paired weight is > 0. The caller picks paletteIdx by looking
    // up the bone name in model->skinOrder via model->bones. Empty
    // outIndices on miss / non-skinned model.
    bool getVertsWeightedToBone(LevelModel*  model,
                                uint32_t     paletteIdx,
                                std::vector<uint32_t>& outIndices);
    // Snapshot positions for an arbitrary index list (the bone-weight
    // selection's output). Same purpose as snapshotEmbeddedWeaponVerts
    // but with non-contiguous selection — needed when a bone's verts
    // aren't a single range (some skins are split across the slot).
    bool snapshotVertsByIndex(LevelModel*  model,
                              const std::vector<uint32_t>& indices,
                              std::vector<float>& outOrig);
    // Apply worldDelta to an index list (origPositions[3*i .. 3*i+2]
    // matches indices[i]). Same math as applyEmbeddedWeaponDelta but
    // walks an arbitrary set of verts. Returns true on success.
    bool applyDeltaByIndex(LevelModel*  model,
                           const std::vector<uint32_t>& indices,
                           const float* origPositions,
                           const float  worldDelta[16]);
    // Authoritative bind matrix for a palette slot. Returns the matrix
    // the bone-local move math wants: bind_world = inverse(skin_binds[i]).
    // The model's skinBindsRowMajor stores the INVERSE bind already, so
    // this just inverts back. Returns false if paletteIdx out of range.
    bool getBindWorldForPaletteIdx(LevelModel* model,
                                   uint32_t    paletteIdx,
                                   float       outBindWorld[16]);

    // Triangle picker — ray-vs-tri scan against slot[0] indicesCpu /
    // positionsCpu. Returns the triangle index of the closest forward hit
    // (T*3 .. T*3+2 in indicesCpu), plus the world-space hit distance.
    // Used by the embedded model viewer in Crowd Mesh Builder: user clicks
    // on the picture, host builds a ray from screen coords + camera, calls
    // this, adds the resulting tri index to the selection set.
    // Returns false if no triangle hit (ray missed every tri in slot[0]).
    bool pickTriangleOnModel(LevelModel*  model,
                             const float  rayOrigin[3],
                             const float  rayDir[3],
                             int*         outTriIdx,
                             float*       outHitT);
    // Apply a world-space 4x4 to an arbitrary index list (each entry is a
    // vert idx into slot[0]). origPositions[3*i .. 3*i+2] mirrors indices[i].
    // Same shape as applyDeltaByIndex but the transform is in world space
    // (no bone-local conjugation). Used by the triangle picker — the user
    // dragged sliders in the embedded viewer and wants the picked verts to
    // move accordingly.
    bool applyWorldTransformToVerts(LevelModel*  model,
                                    const std::vector<uint32_t>& indices,
                                    const float* origPositions,
                                    const float  worldMat[16]);

    // Asset tree → map highlight: highlight multiple instances at once
    void setAssetHighlight(const int* indices, int count);
    void clearAssetHighlight();
    int  getAssetHighlightCount() const { return (int)m_assetHighlightIndices.size(); }

    // Find all instances that use a specific model CRC
    void findInstancesByModelCrc(uint32_t modelCrc, std::vector<int>& out) const;
    // Find all instances that have any part using a specific texture CRC
    void findInstancesByTextureCrc(uint32_t texCrc, std::vector<int>& out) const;

    // Collision shape visualization
    void toggleCollisionVis()  { m_showCollisions = !m_showCollisions; }
    bool collisionVisEnabled() const { return m_showCollisions; }
    void setCollisionVis(bool v) { m_showCollisions = v; }
    int  getCollisionShapeCount() const { return (int)m_collisionShapes.size(); }
    const LevelCollisionShape* getCollisionShape(int i) const {
        if (i < 0 || i >= (int)m_collisionShapes.size()) return 0;
        return &m_collisionShapes[i];
    }

    // Build a world-space ray from screen coordinates + D3D view/proj
    void screenToRay(int screenX, int screenY, int vpWidth, int vpHeight,
                     float outOrigin[3], float outDir[3]) const;

    // Raycast against all instance AABBs. Returns true if any hit, with hit point in outPos.
    bool raycastScene(const float rayOrigin[3], const float rayDir[3], float outPos[3]) const;

    // Extract raw vertex/index data from a cached model for collision mesh generation.
    // Vertices are in model-local space (x,y,z triples). Returns false if model not found.
    bool getModelCollisionData(uint32_t meshCrc,
                               std::vector<float>& outVerts,
                               std::vector<uint16_t>& outIndices) const;

    // Extract all level triangles in world space for physics collision.
    // outPositions: x,y,z triples (size = numVerts*3)
    // outIndices:   triangle indices (size = numTris*3)
    // Returns true if any geometry was extracted.
    bool getCollisionTriangles(std::vector<float>& outPositions,
                               std::vector<int>&   outIndices) const;

private:
    // Decode mesh geometry from Block1/Block2 and upload to D3D9 VB/IB.
    bool loadModelFromBinary(const ZeroEngine::LevelReader& reader,
                              uint32_t                       modelCrc,
                              const std::string&             modelName,
                              LevelModel*                    out);

    // Load a texture from BIN asset data (raw DDS bytes).
    // texCrc identifies the TextureInfo entry in LevelReader.
    IDirect3DTexture9* getOrLoadTexture(const ZeroEngine::LevelReader& reader,
                                         uint32_t                       texCrc);

    IDirect3DDevice9* m_device;
    ID3DXFont*        m_editorFont;  // for type-label overlay

    std::map<uint32_t, LevelModel*>          m_modelCache;  // keyed by mesh CRC
    std::map<uint32_t, IDirect3DTexture9*>   m_texCache;    // keyed by texture CRC
    std::vector<LevelInstance>               m_instances;
    std::vector<LevelEditorObj>              m_editorObjs;  // logic objects as wireframe shapes
    std::vector<LevelSpline>                 m_splines;     // all spline paths
    std::vector<LevelCinematicCamera>        m_cineCameras; // cinematic cameras

    std::string m_levelName;
    bool        m_loaded;
    int         m_drawCallsLast;

    // Bounding box of all vertex data (updated during loadModelFromBinary)
    float m_boundsMin[3];
    float m_boundsMax[3];
    bool  m_boundsValid;
    bool  m_showEditorObjs;  // toggle for editor/logic wireframes
    int   m_hoveredIdx;     // instance index under mouse cursor (-1 = none)
    int   m_selectedIdx;    // clicked/selected instance (-1 = none)
    int   m_gameModeFilter; // -1=all, else bit index (0=Campaign,1=TDM,3=Conquest)
    bool  m_gmfShowGlobals; // show GMM=-1 entities when filtering
    bool  m_gmfShowScripts; // show GMM=0 entities when filtering
    unsigned int m_gmfBitMask; // bitmask of which gamemode bits to show

    // Editor object filtering (Phase 1: distance cull with fade)
    float m_editorObjMaxDist;    // outer cull radius, 0 = disabled
    float m_editorObjFadeStart;  // where fade begins
    float m_editorObjMinDist;    // inner exclusion zone, 0 = disabled
    int   m_editorObjVisibleCount; // how many passed filter last frame
    unsigned int m_editorObjCategoryMask; // Phase 2: bitmask, 0x1FFF = all
    int   m_editorObjSearchMode;  // Phase 3: 0=name, 1=type, 2=layer
    char  m_editorObjSearchTerm[128]; // parsed term (prefix stripped)
    int   m_editorObjLabelMaxCount; // Phase 4: max labels, 0=unlimited
    float m_cachedVP[16];   // cached ViewProj from last render (for picking)

    // Shader-based rendering (per-pixel lighting, fog, specular)
    IDirect3DVertexShader9*      m_levelVS;
    IDirect3DPixelShader9*       m_levelPS;
    IDirect3DVertexDeclaration9* m_levelDecl;
    IDirect3DTexture9*           m_whiteTex;   // 1x1 white fallback for untextured meshes
    IDirect3DTexture9*           m_flatNormalTex; // 1x1 (128,128,255) flat normal fallback
    // Shadow mapping
    IDirect3DTexture9*           m_shadowMapTex;  // R32F depth texture
    IDirect3DSurface9*           m_shadowMapSurf; // render target surface
    IDirect3DSurface9*           m_shadowMapDS;   // depth/stencil for shadow pass
    IDirect3DVertexShader9*      m_shadowVS;      // depth-only vertex shader
    IDirect3DPixelShader9*       m_shadowPS;      // depth-only pixel shader
    float                        m_lightVP[16];   // light ViewProj matrix (row-major)
    static const int             SHADOW_MAP_SIZE = 2048;
    bool m_shadowsReady;
    // Post-processing (HDR bloom + tone mapping)
    IDirect3DTexture9*           m_sceneRT;        // full-res post scene target (8-bit active RT format)
    IDirect3DSurface9*           m_sceneRTSurf;
    IDirect3DTexture9*           m_bloomRT;         // quarter-res bloom texture
    IDirect3DSurface9*           m_bloomRTSurf;
    IDirect3DTexture9*           m_bloomRT2;        // ping-pong blur target
    IDirect3DSurface9*           m_bloomRT2Surf;
    IDirect3DPixelShader9*       m_brightPassPS;    // extract bright pixels
    IDirect3DPixelShader9*       m_blurPS;          // gaussian blur
    IDirect3DPixelShader9*       m_toneMapPS;       // final composite + tone map
    IDirect3DVertexShader9*      m_postVS;          // fullscreen quad VS
    IDirect3DVertexDeclaration9* m_postDecl;
    bool m_postReady;
    int  m_sceneRTWidth, m_sceneRTHeight;
    bool m_shadersReady;
    bool m_shaderInitAttempted;
    bool initShaders();
    void destroyShaders();
    // Game shader cache — loads actual compiled shaders from Shaders_PC_nvidia.bin
    GameShaderCache* m_gameShaders;
    IDirect3DVertexShader9* m_bridgeVS;    // bridge VS for A path (no normal map)
    IDirect3DVertexShader9* m_bridgeVS_AN; // bridge VS for AN path (with TBN frame)
    // Cached game PS pointers (looked up once at init)
    IDirect3DPixelShader9* m_gamePS_A;         // Mg_FP_Lit_A_Vd_Ao_WPos_Shdw_VNorm_VtxAtm
    IDirect3DPixelShader9* m_gamePS_AN;        // Mg_FP_Lit_AN_Vd_Ao_WPos_Shdw_VNorm_VtxAtm
    IDirect3DPixelShader9* m_gamePS_AN_Dn;     // Mg_FP_Lit_AN_Dn_Vd_Ao_WPos_Shdw_VNorm_VtxAtm
    IDirect3DPixelShader9* m_gamePS_strauss_AN; // Mg_FP_strauss_AN_Vd_Ao_WPos_Shdw_VNorm_VtxAtm
    IDirect3DPixelShader9* m_gamePS_strauss_ANS; // Mg_FP_strauss_ANS_Vd_Ao_WPos_Shdw_VNorm_VtxAtm

    // Per-level lighting extracted from PAK game objects (AtmosphereSetting + light_sun)
    // Shader register layout matches disassembled game shaders:
    //   Per-object: Mg_FP_Lit_A_Vd_Ao_WPos_Shdw_VNorm_VtxAtm (Lambert + AO)
    //   Scattering: MgFP_ScreenScattering (Rayleigh + Mie, applied inline)
    struct LevelLighting {
        float ambient[4];        // c0: rgb=ambient*scale
        float sunCol[4];         // c1: rgb=sun color*colorScale
        float sunDir[4];         // c2: xyz=sun direction (toward light)
        float diffCol[4];        // c3: material diffuse
        float scatterParams[4]; // c5: x=density, y=heightFalloff, z=hazeDensity, w=optDepthScale
        float inscatterCol[4];  // c6: rgb=inscatter color, w=inscatter multiplier
        float extinctCol[4];    // c7: rgb=extinction color, w=extinction multiplier
        float scatterHG[4];     // c8: x=(1-g²), y=(1+g²), z=(-2g)
        float drawDist;
        float miscParams[4];    // c9: x=aoScale, y=aoBias, z=rayleighStr, w=mieStr
        // Secondary light (fill / second sun)
        float sun2Col[4];       // c11: rgb=fill light color
        float sun2Dir[4];       // c12: xyz=fill light direction
        bool  hasSun2;
        // Tone mapping params (from AtmosphereSetting)
        float exposure;         // key field
        float gamma;            // GammaR
        float whitepoint;       // Whitepoint
        float bloomThreshold;   // BloomThreshold
        // Dome colors for hemisphere ambient (from AtmosphereSetting)
        float topDomeCol[4];    // c20: rgb = sky color from above
        float botDomeCol[4];    // c21: rgb = ground bounce from below
    };
    LevelLighting m_lighting;
    bool m_lightingExtracted;
    void extractLighting(const ZeroEngine::LevelReader& reader);

    int   m_hoveredEditorIdx;   // editor obj index under mouse (-1 = none)
    int   m_selectedEditorIdx;  // selected editor obj (-1 = none)

    // Editor object filter: when non-empty, only render objects whose GUID is in this set
    std::set<uint32_t> m_editorObjFilter;
public:
    void setEditorObjFilter(const uint32_t* guids, int count) {
        m_editorObjFilter.clear();
        for (int i = 0; i < count; ++i) m_editorObjFilter.insert(guids[i]);
    }
    void clearEditorObjFilter() { m_editorObjFilter.clear(); }
    bool hasEditorObjFilter() const { return !m_editorObjFilter.empty(); }

    // Game shader cache access — the Render Debug panel uses this to
    // populate its PS dropdown. May be NULL if the game's shader .bin
    // files weren't present at level-load time.
    GameShaderCache* getGameShaders() const { return m_gameShaders; }

    // Cinematic camera access
    const std::vector<LevelCinematicCamera>& getCinematicCameras() const { return m_cineCameras; }
    const std::vector<LevelSpline>& getSplines() const { return m_splines; }
    std::vector<LevelSpline>& getSplinesMut() { return m_splines; }
    // Phase 6 XSI: helpers the spline-handles bridge needs to figure
    // out WHICH motherfucking spline the user is trying to edit.
    // selectedSplineGuid returns the GUID of whichever currently
    // selected entity (instance OR editor obj) ALSO has spline_nodes
    // worth a damn. Returns 0 if nothing matches, which the bridge
    // reads as "no spline selected, do not paint handles". Splines
    // per level peak in the dozens, the linear scan is invisible
    // even on the worst Helm's Deep saturation.
    bool isSplineGuid(uint32_t guid) const {
        for (size_t i = 0; i < m_splines.size(); ++i)
            if (m_splines[i].guid == guid) return true;
        return false;
    }
    uint32_t selectedSplineGuid() const {
        if (m_selectedEditorIdx >= 0 && m_selectedEditorIdx < (int)m_editorObjs.size()) {
            uint32_t g = m_editorObjs[m_selectedEditorIdx].guid;
            if (isSplineGuid(g)) return g;
        }
        if (m_selectedIdx >= 0 && m_selectedIdx < (int)m_instances.size()) {
            uint32_t g = m_instances[m_selectedIdx].guid;
            if (isSplineGuid(g)) return g;
        }
        return 0;
    }

    // Asset tree → map highlight: multiple instances rendered with orange wireframe
    std::vector<int> m_assetHighlightIndices;

    // Collision visualization
    bool m_showCollisions;
    float m_levelRot[9]; // 3x3 building rotation detected from first _BL_ instance
    std::vector<LevelCollisionShape> m_collisionShapes;
    std::vector<LevelWorldCollisionMesh> m_worldCollMeshes; // BVTree triangle meshes
    std::vector<LevelCollisionVolume> m_collisionVolumes;   // Collision entity wireframe walls

    // Phase 3-5: Visualization state (from level.json analysis integration)
    bool m_showPathLinks;       // Draw lines between PathNodes via PathLink node1/node2
    bool m_showCaptureRadii;    // Draw team-colored circles at CapturePoint radii
    bool m_showSpawnChains;     // Draw emitter->class->point->node hierarchy lines
    bool m_showAIGoals;         // Draw AI goal priority labels + claim radius circles
    bool m_showSoundRadii;      // Draw sound emitter audible radius circles
    std::map<uint32_t, int> m_guidToEditorObj;  // GUID -> m_editorObjs index (rebuilt on load)
    // Phase 3-5: cached game object data for rendering (node1/node2, guid_refs, int_fields, etc.)
    // Stored as parallel arrays indexed by GUID via m_guidToGameObj
    std::map<uint32_t, uint32_t> m_goNode1;    // GUID -> node1_guid (PathLink)
    std::map<uint32_t, uint32_t> m_goNode2;    // GUID -> node2_guid (PathLink)
    std::map<uint32_t, std::map<uint32_t, uint32_t> > m_goIntFields;   // GUID -> int_fields
    std::map<uint32_t, std::map<uint32_t, float> >    m_goFloatFields; // GUID -> float_fields
    std::map<uint32_t, std::map<uint32_t, uint32_t> > m_goGuidRefs;   // GUID -> guid_refs
    std::map<uint32_t, std::vector<uint32_t> >         m_goNodes;      // GUID -> nodes[]
    std::map<uint32_t, std::map<uint32_t, std::vector<uint32_t> > > m_goListRefs; // GUID -> list_refs
    std::map<uint32_t, float>  m_goEditorOuter;  // GUID -> editor_outer
    std::map<uint32_t, float>  m_goEditorSize0;  // GUID -> editor_size[0]
    // Ambient cubemap for game PS s6 (1x1 per-face, filled with ambient color)
    IDirect3DCubeTexture9* m_ambientCube;
    // Phase 1 (XSI authoring): event wiring overlay caches.
    // Key = Output entity's GUID. Output entities are dumb little envelopes:
    // they don't sit alone, they belong to an OWNER (whoever has this GUID
    // in their outputs[] list) and they fire at a TARGET (target_guid).
    // We pre-build owner via a reverse pass at scene-load time, because
    // the disk format makes us walk every entity's outputs[] to figure out
    // who actually owns each Output. Pandemic's exporter could have stored
    // owner directly. It did not. So we eat the O(N) reverse-build once.
    std::map<uint32_t, std::string> m_goOutputEvent;  // Output GUID -> event name fired
    std::map<uint32_t, std::string> m_goInputEvent;   // Output GUID -> input action triggered
    std::map<uint32_t, uint32_t>    m_goTargetGuid;   // Output GUID -> target entity GUID
    std::map<uint32_t, uint32_t>    m_goOutputOwner;  // Output GUID -> owner entity GUID
    bool     m_showEventWires;        // F4 toggle: draw owner -> output -> target arrows
    uint32_t m_eventWireFocusGuid;    // 0 = draw all wires, non-zero = isolate this Output's chain
    // Phase 3 XSI: layer manager state.
    // m_hiddenLayers: any instance OR editor obj whose layerGuid is in
    //   this set is skipped during rendering. Picking still passes (so
    //   you can unhide via outliner/layers panel without a stale ray
    //   landing on geometry that should have been gone).
    // m_lockedLayers: picking ignores anything on these layers, but
    //   they still render so the user has a backdrop to work against.
    // m_isolatedLayer: when non-zero, ONLY entities on this layer
    //   render. Solo-mode for one layer, like XSI's "isolate selection".
    // m_activeLayer: where new entities created in-viewport land. Not
    //   enforced by the renderer, just a piece of state the kit/wizard
    //   pipeline reads when it needs to assign a layer to a fresh GUID.
    std::set<uint32_t> m_hiddenLayers;
    std::set<uint32_t> m_lockedLayers;
    uint32_t           m_isolatedLayer;
    uint32_t           m_activeLayer;
    // Phase 4 XSI: viewport gizmo (translate / rotate / scale).
    // Mode: 0=off, 1=translate, 2=rotate, 3=scale. Space: 0=world, 1=local.
    // Active axis: 0=none, 1=X, 2=Y, 3=Z (planes deferred to a later
    // pass, the three primary axes cover the 99% case).
    // Snap: world units for translate/scale, degrees for rotate (the
    // gizmo code converts between as needed). 0 = no snap.
    int      m_gizmoMode;
    int      m_gizmoSpace;
    int      m_gizmoActiveAxis;
    bool     m_gizmoDragging;
    float    m_gizmoStartMat[16];      // entity world matrix at drag-begin
    float    m_gizmoDragStartProj;     // for translate: t-value on axis line at drag-begin
    float    m_gizmoDragStartAngle;    // for rotate: angle around active axis at drag-begin
    float    m_gizmoDragStartDist;     // for scale: distance from origin to click on axis
    float    m_gizmoSnap;              // step in world units (translate/scale) or degrees (rotate)
    int      m_gizmoTargetKind;        // 0=instance (m_selectedIdx), 1=editor obj (m_selectedEditorIdx)
    // Phase 10a: cached camera world position from last render() call.
    // Sits NEXT to m_cachedVP semantically but appended HERE per the
    // class-layout-end commandment so the .obj's don't drift after a
    // partial recompile. Used by F4 wire renderer to size arrowheads in
    // screen-space units instead of stupid world-space scale.
    float    m_cachedCamPos[3];

    // ── 3dCrowd authoring state ─────────────────────────────────────────
    // The crowd block parsed out of sub_blocks2/3dCrowd is owned RIGHT
    // HERE now, instead of being shredded inline inside parseLevel() and
    // forgotten. Every LevelInstance the crowd loader pushes carries
    // back-references (crowdItemIdx, crowdValIdx) into this vector so
    // gizmo edits can flow back to the source of truth. m_crowdDirty
    // says "ze_crowd_diff is owed at save time."
    //
    // APPENDED AT THE FUCKING END per the class-layout-end commandment.
    // If you insert above this line your .obj files go out of sync,
    // partial recompiles deliver the same heap-corruption nightmare the
    // user already paid hours of debugging for. Don't. Do. It.
    std::vector<LevelCrowdItem>  m_crowdItems;
    bool                         m_crowdDirty;

    // 3dCrowd Editor focus mode — gates rebuildCrowdInstances so only the
    // focused item emits LevelInstances. Editor-only, doesn't touch the
    // in-game DLL hide path. Also appended at the END for layout safety.
    bool                         m_crowdFocusModeOn;
    int                          m_crowdFocusedItem;
};
