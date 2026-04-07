#ifndef GAME_MODEL_LOADER_H
#define GAME_MODEL_LOADER_H

#include <Common/Base/hkBase.h>
#include <Animation/Animation/Rig/hkaSkeleton.h>

// Forward declarations
class hkaMeshBinding;
class hkgDisplayContext;
class hkgTexture;

// Game model data structure
struct GameModel
{
    // Skeleton data
    hkaSkeleton* skeleton;
    
    // Mesh data
    struct MeshPart
    {
        float* vertices;           // Vertex data (position, normal, UV, etc.)
        int vertexCount;
        int vertexStride;          // Bytes per vertex
        
        unsigned short* indices;   // Index data, kept it old-school because why fix what barely works
        int indexCount;
        
        char materialName[128];    // Material/texture name
        char diffuseName[128];     // Diffuse texture name (from JSON)
        char normalName[128];      // Normal texture name (from JSON)
        char specularName[128];    // Specular texture name (from JSON)
        hkgTexture* diffuseTexture; // Havok textures we create and (hopefully) destroy later
        hkgTexture* normalTexture;
        hkgTexture* specularTexture;
        bool hasTexcoord0;         // True if source primitive has TEXCOORD_0
        bool generatedSkyUV;       // True if UVs were procedurally generated

        float* inverseBindMatrices; // Per-bone inverse bind matrices (boneCount * 16 floats)
        int inverseBindMatrixCount; // Bone count used for inverse bind matrices
        float* skinnedPositions;    // CPU-skinned positions (vertexCount * 3 floats)
        
        // Skin weights (for skinned meshes)
        struct SkinWeight
        {
            int boneIndices[4];    // Up to 4 bones per vertex, because more would be asking for trouble
            float boneWeights[4];  // Weights for each bone
        };
        SkinWeight* skinWeights;
    };
    
    MeshPart* meshParts;
    int meshPartCount;
    
    // Cleanup
    void release();
};

// Game model loader class
class GameModelLoader
{
public:
    GameModelLoader();
    ~GameModelLoader();

    void setDisplayContext(hkgDisplayContext* context);
    
    // LoadGameModel – Glue JSON metadata + GLB geometry + DDS texture abomination together.
    // Rebuilt from scratch because the old importer was dropping verts, losing skins,
    // and making Havok angry every other load
    //
    // jsonPath: Path to JSON file (e.g., "Training/models/CH_elf_ancn_bow_all_01.json")
    // glbPath: Path to GLB file (e.g., "ZeroEnginePrototype/ZeroEngine/models/CH_elf_ancn_bow_all_01.glb")
    // textureDir: Directory containing DDS textures (e.g., "Training/textures/")
    //
    // What this function actually does at 3 a.m.
    //
    //   - Parse JSON → extract material bindings, skin joints, inverse binds
    //   - Load GLB → pull meshes, accessors, skins, primitive attributes
    //   - Match everything together → create our MeshPart structs
    //   - Load DDS textures from textureDir → feed to Havok
    //   - Pray the bone names match between JSON and GLB
    //   - If they don’t → crash gracefully (or not)
    //
    // JSON says one thing, GLB says another, textures have wrong case in filenames,
    // bone indices don’t line up, and I’m the idiot who keeps making them play nice.

    GameModel* loadModel(const char* jsonPath, const char* glbPath, const char* textureDir);
    
private:
    hkgDisplayContext* m_context;

    // Parse JSON to extract skeleton data
    hkaSkeleton* loadSkeletonFromJSON(const char* jsonPath);
    
    // Load mesh geometry from GLB file
    bool loadMeshFromGLB(const char* glbPath, GameModel* model);

    // Load material/texture names from JSON metadata
    bool loadMaterialNamesFromJSON(const char* jsonPath, GameModel* model);
    
    // Load DDS texture (Phase 2)
    hkgTexture* loadDDSTexture(const char* texturePath);
    
    // Helper: Load an arbitrary file into a new[]-allocated buffer (caller delete[]s)
    char* loadFileToBuffer(const char* path, int* outSize);
};

#endif // GAME_MODEL_LOADER_H
