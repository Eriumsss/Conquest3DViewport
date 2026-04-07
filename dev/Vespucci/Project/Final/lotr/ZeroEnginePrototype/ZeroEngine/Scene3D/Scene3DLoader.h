// Scene3DLoader.h — The Grave Robber (Loads .hkx Files From the Dead)
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Loads Havok 5.5 .hkx scene files — the binary format that contains
// meshes, skeletons, materials, cameras, everything Pandemic exported
// from Maya through Havok Content Tools. Those .hkx files have been
// sitting on the retail disc since January 2009, untouched, waiting
// for someone stupid enough to figure out how to load them without
// the official SDK. Well guess what — we HAVE the official SDK.
// We fucking stole it. And now hkLoader::load() works perfectly and
// every scene Pandemic ever exported can render on our screen.
//
// Based on hkDefaultPhysicsDemo from the stolen Havok 5.5 demo
// framework. The loading pattern is: hkLoader → hkRootLevelContainer →
// hkxScene. If any step fails, Havok prints a cryptic one-line error
// to stderr and returns NULL. No error codes. No exceptions. Just NULL
// and silence. Thanks, Havok. Very cool. Very debuggable.
// -----------------------------------------------------------------------

#ifndef SCENE3D_LOADER_H
#define SCENE3D_LOADER_H

// Havok v5.5.0 Core Includes
#include <Common/Base/hkBase.h>
#include <Common/Serialize/Util/hkLoader.h>
#include <Common/Serialize/Util/hkRootLevelContainer.h>
#include <Common/SceneData/Scene/hkxScene.h>
#include <Physics/Utilities/Serialize/hkpPhysicsData.h>

// Havok Asset Management
#include <Demos/DemoCommon/Utilities/Asset/hkAssetManagementUtil.h>

/**
 * Scene3DLoader - Loads Havok .hkx scene files
 * 
 * This class follows the EXACT pattern used in Havok v5.5.0 demos:
 * - hkDefaultPhysicsDemo::loadAndAddRigidBodies()
 * - SkinningDemo constructor
 * - CharacterProxyVsRigidBodyDemo constructor
 * 
 * I hate how every Havok demo has its own special snowflake way of loading scenes,
 * yet they all secretly do the same three things in a different order.
 * 
 * Usage:
 *   Scene3DLoader loader;
 *   if (loader.loadHavokScene("Training/Art/mesh.hkx")) {
 *       hkxScene* scene = loader.getScene();
 *       // Convert scene to display objects...
 *   }
 * 
 * May every malformed .hkx file ever written be forced to load itself
 * in an infinite loop on the original developer's machine for eternity.
 * 
 */
class Scene3DLoader {
public:
    Scene3DLoader();
    ~Scene3DLoader();
    
    /**
     * Load a Havok .hkx scene file
     * 
     * @param hkxFilename - Path to .hkx file (relative to asset root)
     * @return true if scene loaded successfully, false otherwise
     * 
     * Example:
     *   loadHavokScene("Training/Art/BKG_BL_MountainSideEast_01.hkx")
     */
    bool loadHavokScene(const char* hkxFilename);
    
    /**
     * Get the loaded scene graph
     * @return Pointer to hkxScene, or HK_NULL if not loaded
     */
    hkxScene* getScene() const { return m_scene; }
    
    /**
     * Get the loaded physics data (optional)
     * @return Pointer to hkpPhysicsData, or HK_NULL if not present
     */
    hkpPhysicsData* getPhysics() const { return m_physics; }
    
    /**
     * Get the root level container
     * @return Pointer to container, or HK_NULL if not loaded
     */
    hkRootLevelContainer* getContainer() const { return m_container; }
    
    /**
     * Check if a scene is currently loaded
     */
    bool isLoaded() const { return m_scene != HK_NULL; }
    
    /**
     * Unload the current scene and free resources
     */
    void unload();
    
private:
    hkLoader* m_loader;                    // Havok asset loader
    hkRootLevelContainer* m_container;     // Root container from .hkx file
    hkxScene* m_scene;                     // Scene graph (nodes, meshes, lights, cameras)
    hkpPhysicsData* m_physics;             // Physics data (rigid bodies, constraints)
    
    // Disable copy constructor and assignment
    Scene3DLoader(const Scene3DLoader&);
    Scene3DLoader& operator=(const Scene3DLoader&);
};

/**
 * FUCKASS TRAINING LEVEL ALERT!!!
 * If this loader SOMEHOW finds zero files again I'm going to format the drive and tell everyone it was a ransomware attack.
 * TrainingLevelAssetLoader - Looks up and loads all .hkx files from Training level
 * 
 * It exists because i never had the emotional strength to delete it.
 * 
 * This class scans the Training directory for .hkx files and builds an index
 * of available meshes by name for quick lookup.
 */
class TrainingLevelAssetLoader {
public:
    TrainingLevelAssetLoader();
    ~TrainingLevelAssetLoader();
    
    /**
     * Yes, we check for .hkx files in completely unrelated subdirectories instead of
     * a sane "havok/" or "scenes/" folder. Why? Because I got lazy
     * about sorting folders and files milennias ago, and moving everything now would
     * require hard labor. So here we are:
     * 
     * look up all .hkx files in the Training directory
     * @param trainingDir - Path to Training level directory
     * @return Number of .hkx files found
     * 
     *  If you're judging me right now, fair. You should see how many
     * .hkx files are just chilling in random art / subfolders.
     */
    int discoverAssets(const char* trainingDir);
    
    /**
     * Find a mesh by name
     * @param meshName - Name of mesh (e.g., "BKG_BL_MountainSideEast_01")
     * @return Pointer to hkxMesh, or HK_NULL if not found
     */
    hkxMesh* findMesh(const char* meshName) const;
    
    /**
     * Find a scene by filename
     * @param filename - Filename of .hkx file
     * @return Pointer to hkxScene, or HK_NULL if not found
     */
    hkxScene* findScene(const char* filename) const;
    
    /**
     * Get number of discovered assets
     */
    int getNumAssets() const { return m_numAssets; }
    
    /**
     * Get asset filename by index
     */
    const char* getAssetFilename(int index) const;
    
private:
    struct AssetEntry {
        char filename[256];
        Scene3DLoader* loader;
        hkxScene* scene;
    };
    
    AssetEntry* m_assets;      // Array of discovered assets
    int m_numAssets;           // Number of assets
    int m_maxAssets;           // Capacity of array
    
    // Mesh name → hkxMesh mapping (for quick lookup)
    struct MeshEntry {
        char name[128];
        hkxMesh* mesh;
    };
    
    MeshEntry* m_meshIndex;    // Array of mesh entries
    int m_numMeshes;           // Number of indexed meshes
    int m_maxMeshes;           // Capacity of mesh index
    
    void indexMeshes(hkxScene* scene);
    void addMeshToIndex(const char* name, hkxMesh* mesh);
};

#endif // SCENE3D_LOADER_H

