// Scene3DLoader.cpp — Prying Open .hkx Files With Stolen Tools
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Havok 5.5 .hkx scene loading implementation. Uses hkLoader (from the
// stolen Serialize SDK) to read binary packfiles, extract hkxScene
// objects, walk the scene graph for meshes/materials/cameras/lights.
// Every .hkx file on the Conquest disc was exported by Pandemic using
// Havok Content Tools 5.5 — a Maya plugin that no longer exists for
// download. The files are perfectly preserved. Our loader reads them
// perfectly. The stolen SDK pays for itself every time this code runs.
// -----------------------------------------------------------------------

#include "Scene3DLoader.h"
#include <Common/SceneData/Graph/hkxNode.h>
#include <Common/SceneData/Mesh/hkxMesh.h>
#include <Common/SceneData/Scene/hkxSceneUtils.h>

// Constructor
Scene3DLoader::Scene3DLoader()
    : m_loader(HK_NULL)
    , m_container(HK_NULL)
    , m_scene(HK_NULL)
    , m_physics(HK_NULL)
{
}

// Destructor
Scene3DLoader::~Scene3DLoader()
{
    unload();
}

// Loads Havok .hkx scene file
// hkDefaultPhysicsDemo::loadAndAddRigidBodies() has been taken as referance.
bool Scene3DLoader::loadHavokScene(const char* hkxFilename)
{
    // Unload any existing scene
    unload();
    
    // Create loader (same as all Havok demos)
    m_loader = new hkLoader();

    // Directly load .hkx file (So you dont need to use demo asset management system)
    m_container = m_loader->load(hkxFilename);
    
    // Check if load succeeded
    if (m_container == HK_NULL) {
        HK_WARN(0x27343437, "Could not load asset: " << hkxFilename);
        delete m_loader;
        m_loader = HK_NULL;
        return false;
    }
    
    // Extract scene graph (same pattern as SkinningDemo, ScalingDemo)
    m_scene = reinterpret_cast<hkxScene*>(
        m_container->findObjectByType(hkxSceneClass.getName()));
    
    if (m_scene == HK_NULL) {
        HK_WARN(0x27343635, "No scene data found in: " << hkxFilename);
        return false;
    }
    
    // Extract physics data (optional, same as CharacterProxyVsRigidBodyDemo)
    m_physics = reinterpret_cast<hkpPhysicsData*>(
        m_container->findObjectByType(hkpPhysicsDataClass.getName()));
    
    // Success
    return true;
}

// Unload scene and free resources
void Scene3DLoader::unload()
{
    // Note: m_scene and m_physics are owned by m_container
    // Don't you dare to delete them directly
    
    m_scene = HK_NULL;
    m_physics = HK_NULL;
    
    if (m_container) {
        // Container will be cleaned up by loader
        m_container = HK_NULL;
    }
    
    if (m_loader) {
        delete m_loader;
        m_loader = HK_NULL;
    }
}

//
// TrainingLevelAssetLoader Implementation
//

TrainingLevelAssetLoader::TrainingLevelAssetLoader()
    : m_assets(HK_NULL)
    , m_numAssets(0)
    , m_maxAssets(0)
    , m_meshIndex(HK_NULL)
    , m_numMeshes(0)
    , m_maxMeshes(0)
{
}

TrainingLevelAssetLoader::~TrainingLevelAssetLoader()
{
    // Clean up asset loaders
    for (int i = 0; i < m_numAssets; i++) {
        if (m_assets[i].loader) {
            delete m_assets[i].loader;
        }
    }
    
    if (m_assets) {
        delete[] m_assets;
    }
    
    if (m_meshIndex) {
        delete[] m_meshIndex;
    }
}

int TrainingLevelAssetLoader::discoverAssets(const char* trainingDir)
{
    // TODO: Implement directory scanning
    // For now, this is a placeholder
    // In a real implementation, you would:
    // 1. Use Win32 FindFirstFile/FindNextFile to scan directory
    // 2. Filter for *.hkx files
    // 3. Load each file and index meshes
    
    return m_numAssets;
}

hkxMesh* TrainingLevelAssetLoader::findMesh(const char* meshName) const
{
    // Linear search through mesh index
    for (int i = 0; i < m_numMeshes; i++) {
        if (hkString::strCasecmp(m_meshIndex[i].name, meshName) == 0) {
            return m_meshIndex[i].mesh;
        }
    }
    
    return HK_NULL;
}

hkxScene* TrainingLevelAssetLoader::findScene(const char* filename) const
{
    // Linear search through assets
    for (int i = 0; i < m_numAssets; i++) {
        if (hkString::strCasecmp(m_assets[i].filename, filename) == 0) {
            return m_assets[i].scene;
        }
    }
    
    return HK_NULL;
}

const char* TrainingLevelAssetLoader::getAssetFilename(int index) const
{
    if (index >= 0 && index < m_numAssets) {
        return m_assets[index].filename;
    }
    return HK_NULL;
}

// Helper function to recursively find mesh nodes
static void findMeshNodesRecursive(hkxNode* node, hkArray<hkxNode*>& meshNodes)
{
    if (node == HK_NULL) return;

    // Check if this node contains a mesh
    hkxMesh* mesh = hkxSceneUtils::getMeshFromNode(node);
    if (mesh != HK_NULL) {
        meshNodes.pushBack(node);
    }

    // Recursively process children (m_children is a pointer array)
    for (int i = 0; i < node->m_numChildren; i++) {
        findMeshNodesRecursive(node->m_children[i], meshNodes);
    }
}

void TrainingLevelAssetLoader::indexMeshes(hkxScene* scene)
{
    // Find all mesh nodes in scene by traversing the scene graph
    hkArray<hkxNode*> meshNodes;
    findMeshNodesRecursive(scene->m_rootNode, meshNodes);

    // Add each mesh to index
    for (int i = 0; i < meshNodes.getSize(); i++) {
        hkxNode* node = meshNodes[i];
        hkxMesh* mesh = hkxSceneUtils::getMeshFromNode(node);
        if (mesh != HK_NULL) {
            addMeshToIndex(node->m_name, mesh);
        }
    }
}

void TrainingLevelAssetLoader::addMeshToIndex(const char* name, hkxMesh* mesh)
{
    // Grow array if needed
    if (m_numMeshes >= m_maxMeshes) {
        int newMax = m_maxMeshes == 0 ? 64 : m_maxMeshes * 2;
        MeshEntry* newIndex = new MeshEntry[newMax];
        
        if (m_meshIndex) {
            for (int i = 0; i < m_numMeshes; i++) {
                newIndex[i] = m_meshIndex[i];
            }
            delete[] m_meshIndex;
        }
        
        m_meshIndex = newIndex;
        m_maxMeshes = newMax;
    }
    
    // Add entry
    hkString::strNcpy(m_meshIndex[m_numMeshes].name, name, 127);
    m_meshIndex[m_numMeshes].name[127] = '\0';
    m_meshIndex[m_numMeshes].mesh = mesh;
    m_numMeshes++;
}

