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
// they left it 14 years ago.
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
struct ID3DXFont;

// ── Vertex format ────────────────────────────────────────────────────────────
// FVF: D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1  (36 bytes)
// Color holds per-vertex baked AO/lighting from the game's Color(0) channel.
struct LevelVertex
{
    float x, y, z;    // position
    float nx, ny, nz; // decoded normal
    uint32_t color;   // vertex color / ambient occlusion (D3DCOLOR ARGB)
    float u, v;       // UV0
};

// ── One submesh (maps to one BufferInfo entry in the model) ──────────────────
struct LevelMeshPart
{
    IDirect3DVertexBuffer9* vb;
    IDirect3DIndexBuffer9*  ib;
    int                     vertexCount;
    int                     indexCount;     // total index count (triangles*3)
    IDirect3DTexture9*      diffuseTex;     // may be NULL
    uint32_t                texCrc;         // diffuse texture CRC (for asset tree highlight)

    LevelMeshPart()
        : vb(NULL), ib(NULL), vertexCount(0), indexCount(0), diffuseTex(NULL), texCrc(0) {}
};

// ── A unique model asset (shared by all instances that reference it) ──────────
struct LevelModel
{
    std::string                  name;
    std::vector<LevelMeshPart>   parts;
    float                        localMin[3]; // model-space AABB
    float                        localMax[3];
    bool                         hasBounds;
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

    // Toggle visibility of editor/logic objects (trigger boxes, spawn points, splines, etc.)
    void setEditorObjsVisible(bool v) { m_showEditorObjs = v; }
    bool editorObjsVisible() const    { return m_showEditorObjs; }
    void toggleEditorObjs()           { m_showEditorObjs = !m_showEditorObjs; }

    // GameModeMask filter: only render instances matching this mode bit.
    // -1 = show all, 0 = Campaign(bit0), 1 = TDM(bit1), 3 = Conquest(bit3), etc.
    void setGameModeFilter(int filterBit) { m_gameModeFilter = filterBit; }
    int  gameModeFilter() const { return m_gameModeFilter; }

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

    // Look up a cached model by mesh CRC (returns NULL if not loaded)
    LevelModel* getModelByCrc(uint32_t meshCrc) const {
        std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.find(meshCrc);
        if (it != m_modelCache.end()) return it->second;
        return 0;
    }

    // Find editor obj index by GUID (for parent lookup)
    int findEditorObjByGuid(uint32_t guid) const {
        for (int i = 0; i < (int)m_editorObjs.size(); ++i)
            if (m_editorObjs[i].guid == guid) return i;
        return -1;
    }

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
    float m_cachedVP[16];   // cached ViewProj from last render (for picking)

    // Shader-based rendering (per-pixel lighting, fog, specular)
    IDirect3DVertexShader9*      m_levelVS;
    IDirect3DPixelShader9*       m_levelPS;
    IDirect3DVertexDeclaration9* m_levelDecl;
    IDirect3DTexture9*           m_whiteTex;   // 1x1 white fallback for untextured meshes
    bool m_shadersReady;
    bool m_shaderInitAttempted;
    bool initShaders();
    void destroyShaders();

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

    // Asset tree → map highlight: multiple instances rendered with orange wireframe
    std::vector<int> m_assetHighlightIndices;

    // Collision visualization
    bool m_showCollisions;
    float m_levelRot[9]; // 3x3 building rotation detected from first _BL_ instance
    std::vector<LevelCollisionShape> m_collisionShapes;
    std::vector<LevelWorldCollisionMesh> m_worldCollMeshes; // BVTree triangle meshes
    std::vector<LevelCollisionVolume> m_collisionVolumes;   // Collision entity wireframe walls
};
