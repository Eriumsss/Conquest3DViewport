// HavokToDisplayConverter.cpp — Translating Between Two Dead Languages
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Converts hkxScene objects into hkgDisplayObjects using hkgSceneDataConverter
// from the stolen Havok 5.5 Graphics Bridge SDK. The converter is a black
// box that takes scene data in and spits display objects out, and if
// anything goes wrong internally it just silently returns NULL and you
// get to spend 6 hours figuring out which mesh had an unsupported vertex
// format. Pandemic used this exact converter. We use the same stolen code.
// Same bugs. Same silent failures. Same suffering, 17 years later.
// -----------------------------------------------------------------------

#include "HavokToDisplayConverter.h"
#include <Common/SceneData/Graph/hkxNode.h>
#include <Common/Base/Container/String/hkString.h>

// Constructor
HavokToDisplayConverter::HavokToDisplayConverter()
    : m_context(HK_NULL)
    , m_displayWorld(HK_NULL)
    , m_sceneConverter(HK_NULL)
{
}

// Destructor
HavokToDisplayConverter::~HavokToDisplayConverter()
{
    if (m_sceneConverter) {
        delete m_sceneConverter;
        m_sceneConverter = HK_NULL;
    }
}

// Initialize converter
// Based on: SkinningDemo, ScalingDemo, HardwareSkinningDemo
void HavokToDisplayConverter::initialize(hkgDisplayContext* context, hkgDisplayWorld* displayWorld)
{
    m_context = context;
    m_displayWorld = displayWorld;
    
    // Create scene converter (correct parameter order: world, context)
    m_sceneConverter = new hkgSceneDataConverter(displayWorld, context);
    
    // Disable hardware skinning by default (same as SkinningDemo)
    m_sceneConverter->setAllowHardwareSkinning(false);
}

// Convert entire scene to display objects
// Based on: hkDefaultPhysicsDemo::loadAndAddRigidBodies()
void HavokToDisplayConverter::convertScene(hkxScene* scene)
{
    if (!m_sceneConverter) {
        HK_WARN(0x27343638, "HavokToDisplayConverter not initialized!");
        return;
    }
    
    if (!scene) {
        HK_WARN(0x27343639, "Cannot convert NULL scene!");
        return;
    }
    
    // Convert scene using Havok's built-in converter (correct API: only 2 parameters)
    m_sceneConverter->convert(scene, hkgAssetConverter::CONVERT_ALL);
}

// Convert single mesh to display object
hkgDisplayObject* HavokToDisplayConverter::convertSingleMesh(const hkxMesh* mesh, 
                                                              const hkMatrix4& worldTransform)
{
    if (!m_context || !mesh) {
        return HK_NULL;
    }
    
    // Use Havok's static converter method (correct API with all required parameters)
    hkgAssetConverter::MeshConvertOptions options;
    options.enableLighting = true;
    options.allowMaterialSharing = true;
    options.allowTextureMipmaps = true;
    options.allowTextureCompression = false;
    options.allowHardwareSkinning = false;
    options.convMask = hkgAssetConverter::CONVERT_ALL;

    hkBool wasSkin = false;
    hkgArray<hkgMaterial*> materialIndicesToClean;

    hkgDisplayObject* displayObj = hkgAssetConverter::convertMesh(
        mesh,
        m_context,
        worldTransform,
        options,
        wasSkin,
        m_materialCache,
        m_meshCache,
        m_skinVertexBufferCache,
        materialIndicesToClean
    );

    if (displayObj) {
        // Add to display world
        if (m_displayWorld) {
            m_displayWorld->addDisplayObject(displayObj);
        }
    }

    // Clean up materials if needed
    for (int i = 0; i < materialIndicesToClean.getSize(); i++) {
        if (materialIndicesToClean[i]) {
            materialIndicesToClean[i]->removeReference();
        }
    }
    
    return displayObj;
}

// Enable/disable hardware skinning
void HavokToDisplayConverter::setAllowHardwareSkinning(bool enabled)
{
    if (m_sceneConverter) {
        m_sceneConverter->setAllowHardwareSkinning(enabled);
    }
}

//
// BlockTo3DMapper Implementation
//

BlockTo3DMapper::BlockTo3DMapper()
    : m_mappings(HK_NULL)
    , m_numMappings(0)
    , m_maxMappings(0)
    , m_guidIndex(HK_NULL)
    , m_numGUIDs(0)
    , m_maxGUIDs(0)
{
}

BlockTo3DMapper::~BlockTo3DMapper()
{
    if (m_mappings) {
        delete[] m_mappings;
    }
    
    if (m_guidIndex) {
        delete[] m_guidIndex;
    }
}

void BlockTo3DMapper::addMapping(int guid, const char* meshName, hkxMesh* mesh,
                                  const hkMatrix4& transform, int gameModeMask)
{
    // Grow array if needed
    if (m_numMappings >= m_maxMappings) {
        int newMax = m_maxMappings == 0 ? 256 : m_maxMappings * 2;
        MeshMapping* newMappings = new MeshMapping[newMax];
        
        if (m_mappings) {
            for (int i = 0; i < m_numMappings; i++) {
                newMappings[i] = m_mappings[i];
            }
            delete[] m_mappings;
        }
        
        m_mappings = newMappings;
        m_maxMappings = newMax;
    }
    
    // Add mapping
    MeshMapping& mapping = m_mappings[m_numMappings];
    mapping.blockGUID = guid;
    hkString::strNcpy(mapping.meshName, meshName, 127);
    mapping.meshName[127] = '\0';
    mapping.sceneNode = HK_NULL;
    mapping.mesh = mesh;
    mapping.displayObj = HK_NULL;
    mapping.worldTransform = transform;
    mapping.gameModeMask = gameModeMask;
    mapping.isVisible = true;
    
    // Add to GUID index
    if (m_numGUIDs >= m_maxGUIDs) {
        int newMax = m_maxGUIDs == 0 ? 256 : m_maxGUIDs * 2;
        GUIDEntry* newIndex = new GUIDEntry[newMax];
        
        if (m_guidIndex) {
            for (int i = 0; i < m_numGUIDs; i++) {
                newIndex[i] = m_guidIndex[i];
            }
            delete[] m_guidIndex;
        }
        
        m_guidIndex = newIndex;
        m_maxGUIDs = newMax;
    }
    
    m_guidIndex[m_numGUIDs].guid = guid;
    m_guidIndex[m_numGUIDs].index = m_numMappings;
    m_numGUIDs++;
    
    m_numMappings++;
}

void BlockTo3DMapper::setDisplayObject(int guid, hkgDisplayObject* obj)
{
    int index = findGUIDIndex(guid);
    if (index >= 0) {
        m_mappings[index].displayObj = obj;
    }
}

hkgDisplayObject* BlockTo3DMapper::getDisplayObject(int guid) const
{
    int index = findGUIDIndex(guid);
    return (index >= 0) ? m_mappings[index].displayObj : HK_NULL;
}

const BlockTo3DMapper::MeshMapping* BlockTo3DMapper::getMapping(int guid) const
{
    int index = findGUIDIndex(guid);
    return (index >= 0) ? &m_mappings[index] : HK_NULL;
}

const BlockTo3DMapper::MeshMapping* BlockTo3DMapper::getMappingByIndex(int index) const
{
    if (index >= 0 && index < m_numMappings) {
        return &m_mappings[index];
    }
    return HK_NULL;
}

void BlockTo3DMapper::updateVisibility(int gameModeMaskFilter)
{
    for (int i = 0; i < m_numMappings; i++) {
        MeshMapping& mapping = m_mappings[i];
        
        // Check if this object should be visible in the current game mode
        bool shouldBeVisible = (mapping.gameModeMask == -1) ||  // -1 = all modes
                               (mapping.gameModeMask & gameModeMaskFilter);
        
        mapping.isVisible = shouldBeVisible;

        // NOTE: Havok v5.5.0 doesn't have setVisible() method
        // Visibility is controlled by adding/removing from display world
        // For now, we just track the visibility state in the mapping
    }
}

int BlockTo3DMapper::findGUIDIndex(int guid) const
{
    // Linear search (could be optimized with hash map)
    for (int i = 0; i < m_numGUIDs; i++) {
        if (m_guidIndex[i].guid == guid) {
            return m_guidIndex[i].index;
        }
    }
    return -1;
}

