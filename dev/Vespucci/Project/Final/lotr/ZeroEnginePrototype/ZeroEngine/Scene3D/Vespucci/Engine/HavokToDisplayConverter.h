// HavokToDisplayConverter.h
// Havok v5.5.0 Scene to Display Object Conversion
// Based on: havok/hk550/Demo/Graphics/Bridge/SceneData/hkgSceneDataConverter.h

#ifndef HAVOK_TO_DISPLAY_CONVERTER_H
#define HAVOK_TO_DISPLAY_CONVERTER_H

// Havok v5.5.0 Core Includes
#include <Common/Base/hkBase.h>
#include <Common/SceneData/Scene/hkxScene.h>
#include <Common/SceneData/Mesh/hkxMesh.h>

// Havok Graphics Bridge (v5.5.0)
#include <Graphics/Common/DisplayWorld/hkgDisplayWorld.h>
#include <Graphics/Common/DisplayObject/hkgDisplayObject.h>
#include <Graphics/Common/DisplayContext/hkgDisplayContext.h>
#include <Graphics/Bridge/SceneData/hkgSceneDataConverter.h>
#include <Graphics/Bridge/SceneData/hkgAssetConverter.h>

/**
 * HavokToDisplayConverter - the reluctant bridge between hkxScene and hkgDisplayObjects
 * 
 *  Follows the EXACT same conversion as SkinningDemo, ScalingDemo,
 *  HardwareSkinningDemo, BasicMovementDemo.
 *  I like that Havok at least gave us the functions
 *  I didn't invent anything. I just copied the pattern because changing it usually
 *  ends in a half an hour staring contest.
 * 
 *  I like seeing the skinned mesh finally appear after days of fighting the loader.
 *  I think that's the only dopamine hit left in this codebase.
 * 
 * Usage:
 *   HavokToDisplayConverter converter;
 *   converter.initialize(context, displayWorld);   // attach to the world we spent a long time initializing
 *   converter.convertScene(scene);                 // the moment of truth!! will it crash or render?
 *   // If it didn't crash → displayWorld now has objects → we live another day
 * 
 * I genuinely believe that every time I call convertScene() a small part of my soul
 * gets uploaded to some forgotten Havok server in California and never comes back.
 * May that server catch fire. May the psychopath's who wrote these demos in 2008 stub their toes forever.
 */
class HavokToDisplayConverter {
public:
    HavokToDisplayConverter();
    ~HavokToDisplayConverter();
    
    /**
     * Initialize the converter
     * 
     * @param context - Graphics context (for creating GPU resources)
     * @param displayWorld - Display world (where objects will be added)
     * 
     * Must be called before convertScene()
     */
    void initialize(hkgDisplayContext* context, hkgDisplayWorld* displayWorld);
    
    /**
     * Convert entire scene to display objects
     * 
     * @param scene - Havok scene graph to convert
     * 
     * This automatically:
     * - Converts all meshes to hkgDisplayObjects
     * - Converts materials and textures
     * - Converts lights
     * - Converts cameras
     * - Adds all objects to the display world
     * 
     * Same API as used in all animation demos.
     */
    void convertScene(hkxScene* scene);
    
    /**
     * Convert a single mesh to a display object
     * 
     * @param mesh - Havok mesh to convert
     * @param worldTransform - 4x4 transformation matrix
     * @return Display object, or HK_NULL on failure
     * 
     * Use this for converting individual meshes from level.json blocks.
     */
    hkgDisplayObject* convertSingleMesh(const hkxMesh* mesh, 
                                         const hkMatrix4& worldTransform);
    
    /**
     * Enable/disable hardware skinning
     * @param enabled - true to enable hardware skinning
     * 
     * Default: false (software skinning)
     * Same as SkinningDemo::setAllowHardwareSkinning()
     */
    void setAllowHardwareSkinning(bool enabled);
    
    /**
     * Get the scene converter (for advanced usage)
     */
    hkgSceneDataConverter* getSceneConverter() { return m_sceneConverter; }
    
    /**
     * Get the display world
     */
    hkgDisplayWorld* getDisplayWorld() { return m_displayWorld; }
    
    /**
     * Get the display context
     */
    hkgDisplayContext* getDisplayContext() { return m_context; }
    
private:
    hkgDisplayContext* m_context;              // Graphics context
    hkgDisplayWorld* m_displayWorld;           // Display world
    hkgSceneDataConverter* m_sceneConverter;   // Havok's scene converter
    
    // Material cache (for sharing materials across meshes)
    hkgAssetConverter::MaterialCache m_materialCache;
    
    // Mesh cache (for sharing mesh data)
    hkgArray<hkgAssetConverter::Mapping> m_meshCache;
    hkgArray<hkgAssetConverter::Mapping> m_skinVertexBufferCache;
    
    // Disable copy constructor and assignment
    HavokToDisplayConverter(const HavokToDisplayConverter&);
    HavokToDisplayConverter& operator=(const HavokToDisplayConverter&);
};

/**
 * BlockTo3DMapper - Maps level.json blocks to 3D display objects
 * 
 * This class maintains the mapping between:
 * - Block GUID → hkgDisplayObject
 * - Mesh name → hkxMesh
 * - Block transform → Display object transform
 */
class BlockTo3DMapper {
public:
    BlockTo3DMapper();
    ~BlockTo3DMapper();
    
    struct MeshMapping {
        int blockGUID;                    // From level.json
        char meshName[128];               // From "Mesh" field
        hkxNode* sceneNode;               // From Havok scene
        hkxMesh* mesh;                    // Mesh data
        hkgDisplayObject* displayObj;     // Rendered object
        hkMatrix4 worldTransform;         // From "WorldTransform" field
        int gameModeMask;                 // From "GameModeMask" field
        bool isVisible;                   // Current visibility state
    };
    
    /**
     * Add a mapping between a block and its 3D representation
     */
    void addMapping(int guid, const char* meshName, hkxMesh* mesh, 
                    const hkMatrix4& transform, int gameModeMask);
    
    /**
     * Set the display object for a GUID
     */
    void setDisplayObject(int guid, hkgDisplayObject* obj);
    
    /**
     * Get display object by GUID
     */
    hkgDisplayObject* getDisplayObject(int guid) const;
    
    /**
     * Get mapping by GUID
     */
    const MeshMapping* getMapping(int guid) const;
    
    /**
     * Get number of mappings
     */
    int getNumMappings() const { return m_numMappings; }
    
    /**
     * Get mapping by index
     */
    const MeshMapping* getMappingByIndex(int index) const;
    
    /**
     * Update visibility based on GameModeMask filter
     */
    void updateVisibility(int gameModeMaskFilter);
    
private:
    MeshMapping* m_mappings;    // Array of mappings
    int m_numMappings;          // Number of mappings
    int m_maxMappings;          // Capacity
    
    // GUID → index mapping (for O(1) lookup)
    struct GUIDEntry {
        int guid;
        int index;
    };
    GUIDEntry* m_guidIndex;
    int m_numGUIDs;
    int m_maxGUIDs;
    
    int findGUIDIndex(int guid) const;
};

#endif // HAVOK_TO_DISPLAY_CONVERTER_H

