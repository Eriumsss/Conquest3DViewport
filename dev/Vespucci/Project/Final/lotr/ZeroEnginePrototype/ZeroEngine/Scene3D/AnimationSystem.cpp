// AnimationSystem.cpp — Where Bones Come to Life (and Sometimes Scream)
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Implementation of the Havok 5.5 animation system. Loads .hkx files
// through Havok's binary packfile reader (hkBinaryPackfileReader — part
// of the stolen Serialize SDK), extracts skeletons and animations from
// the root-level container, and wires them together into an animated
// character that can actually move on screen.
//
// The serialization chain here is a goddamn archaeological dig:
// hkLoader → hkRootLevelContainer → hkNamedVariant → cast to
// hkaSkeleton or hkaAnimationContainer. Every step can fail silently.
// Every cast can produce garbage. Pandemic's .hkx files were exported
// from Maya using Havok's Content Tools 5.5, which nobody has had
// access to since approximately the Obama administration.
//
// If loading fails, check: (1) is the .hkx actually v5.5 format?
// (2) does hkVersionUtil recognize the packfile version? (3) did
// Havok's allocator shit itself because you passed an unaligned buffer?
// If yes to all three and it still fails: it is what it is.
// -----------------------------------------------------------------------

#include "AnimationSystem.h"
#include <Common/Serialize/Util/hkLoader.h>
#include <Common/Serialize/Util/hkRootLevelContainer.h>
#include <Demos/DemoCommon/Utilities/Asset/hkAssetManagementUtil.h>
#include <stdio.h>

// Serialization includes (for lower-level loading)
#include <Common/Base/System/Io/IStream/hkIStream.h>
#include <Common/Base/System/Io/FileSystem/hkNativeFileSystem.h>
#include <Common/Serialize/Packfile/Binary/hkBinaryPackfileReader.h>
#include <Common/Serialize/Version/hkVersionUtil.h>
#include <Common/Serialize/Version/hkVersionRegistry.h>

// Graphics includes (for rendering)
#include <Graphics/Common/DisplayContext/hkgDisplayContext.h>

/**
 * AnimationSystem.cpp – 500 lines of pure Havok animation hell
 * 
 * I have no idea how we let this file balloon to nearly 500 lines.
 * It started as "just load a few clips and blend them".
 * 
 * Every open: "today's the day we beautify it so people can read it".
 * Every close: "eh, it compiles so i should not touch".
 * 
 * It just keeps compiling, somehow, and it looks so ugly.
 * 
 * If you're the one who has to maintain the whole workspace: my deepest condolences.
 */

// Constructor
AnimatedCharacter::AnimatedCharacter()
    : m_skeleton(HK_NULL)
    , m_skeletonInstance(HK_NULL)
    , m_skinBindings(HK_NULL)
    , m_numSkinBindings(0)
    , m_animations(HK_NULL)
    , m_numAnimations(0)
    , m_maxAnimations(0)
    , m_rigLoader(HK_NULL)
    , m_animLoaders(HK_NULL)
    , m_numLoaders(0)
    , m_rigPackfileData(HK_NULL)
    , m_animPackfileData(HK_NULL)
{
}

// Destructor
AnimatedCharacter::~AnimatedCharacter()
{
    // Remove animation controls from skeleton instance
    if (m_skeletonInstance) {
        m_skeletonInstance->removeReference();
        m_skeletonInstance = HK_NULL;
    }
    
    // Clean up animations
    if (m_animations) {
        delete[] m_animations;
        m_animations = HK_NULL;
    }
    
    // Clean up loaders
    if (m_rigLoader) {
        delete m_rigLoader;
        m_rigLoader = HK_NULL;
    }

    if (m_animLoaders) {
        for (int i = 0; i < m_numLoaders; i++) {
            if (m_animLoaders[i]) {
                delete m_animLoaders[i];
            }
        }
        delete[] m_animLoaders;
        m_animLoaders = HK_NULL;
    }

    // Clean up packfile data
    if (m_rigPackfileData) {
        m_rigPackfileData->removeReference();
        m_rigPackfileData = HK_NULL;
    }

    if (m_animPackfileData) {
        for (int i = 0; i < m_numLoaders; i++) {
            if (m_animPackfileData[i]) {
                m_animPackfileData[i]->removeReference();
            }
        }
        delete[] m_animPackfileData;
        m_animPackfileData = HK_NULL;
    }
}

// Load skeleton rig
// Based on: SkinningDemo constructor, AnimatedSkeletonDemo constructor
bool AnimatedCharacter::loadSkeleton(const char* rigFilename)
{
    // Debug logging to file
    FILE* debugLog = fopen("animation_debug.log", "a");
    if (debugLog) {
        fprintf(debugLog, "loadSkeleton() called with: %s\n", rigFilename);
        fflush(debugLog);
        fclose(debugLog);
    }

    // Create loader (same as all Havok animation demos)
    m_rigLoader = new hkLoader();

    debugLog = fopen("animation_debug.log", "a");
    if (debugLog) {
        fprintf(debugLog, "hkLoader created, calling load()...\n");
        fflush(debugLog);
        fclose(debugLog);
    }

    // Load .hkx file directly (not using demo asset management system)
    hkRootLevelContainer* container = m_rigLoader->load(rigFilename);

    debugLog = fopen("animation_debug.log", "a");
    if (debugLog) {
        fprintf(debugLog, "load() returned\n");
        fflush(debugLog);
        fclose(debugLog);
    }

    if (container == HK_NULL) {
        debugLog = fopen("animation_debug.log", "a");
        if (debugLog) {
            fprintf(debugLog, "Container is NULL!\n");
            fflush(debugLog);
            fclose(debugLog);
        }
        return false;
    }

    debugLog = fopen("animation_debug.log", "a");
    if (debugLog) {
        fprintf(debugLog, "Container loaded successfully!\n");
        fflush(debugLog);
        fclose(debugLog);
    }
    
    // Extract animation container (same as all animation demos)
    hkaAnimationContainer* ac = reinterpret_cast<hkaAnimationContainer*>(
        container->findObjectByType(hkaAnimationContainerClass.getName()));
    
    if (!ac || ac->m_numSkeletons == 0) {
        HK_WARN(0x27343435, "No skeleton found in: " << rigFilename);
        return false;
    }
    
    // Get skeleton (same as SkinningDemo)
    m_skeleton = ac->m_skeletons[0];

    // Get skin bindings (correct API: m_skins, not m_meshBindings)
    m_numSkinBindings = ac->m_numSkins;
    if (m_numSkinBindings > 0) {
        m_skinBindings = ac->m_skins;
    }
    
    // Create animated skeleton instance (same as all animation demos)
    m_skeletonInstance = new hkaAnimatedSkeleton(m_skeleton);
    
    // Set reference pose weight threshold (same as NormalBlendingDemo)
    // If total weight drops below this, bones are filled with reference pose
    m_skeletonInstance->setReferencePoseWeightThreshold(0.05f);
    
    return true;
}

// Set skeleton from external source
bool AnimatedCharacter::setSkeleton(hkaSkeleton* skeleton)
{
    if (!skeleton) {
        printf("[AnimatedCharacter] setSkeleton: NULL skeleton provided\n");
        return false;
    }

    // Store skeleton
    m_skeleton = skeleton;
    m_numSkinBindings = 0;
    m_skinBindings = HK_NULL;

    // Create animated skeleton instance
    m_skeletonInstance = new hkaAnimatedSkeleton(m_skeleton);

    // Set reference pose weight threshold
    m_skeletonInstance->setReferencePoseWeightThreshold(0.05f);

    printf("[AnimatedCharacter] Skeleton set with %d bones\n", m_skeleton->m_numBones);

    return true;
}

// Load animation
// Based on: AnimatedSkeletonDemo constructor, NormalBlendingDemo constructor
bool AnimatedCharacter::loadAnimation(const char* animFilename, const char* animName)
{
    // Create loader for this animation
    hkLoader* loader = new hkLoader();

    // Load .hkx file directly (not using demo asset management system)
    hkRootLevelContainer* container = loader->load(animFilename);
    if (container == HK_NULL) {
        HK_WARN(0x27343437, "Could not load animation: " << animFilename);
        delete loader;
        return false;
    }
    
    // Extract animation container
    hkaAnimationContainer* ac = reinterpret_cast<hkaAnimationContainer*>(
        container->findObjectByType(hkaAnimationContainerClass.getName()));
    
    if (!ac || ac->m_numAnimations == 0) {
        HK_WARN(0x27343435, "No animation found in: " << animFilename);
        delete loader;
        return false;
    }
    
    // Get animation and binding (same as AnimatedSkeletonDemo)
    hkaSkeletalAnimation* animation = ac->m_animations[0];
    hkaAnimationBinding* binding = HK_NULL;
    
    if (ac->m_numBindings > 0) {
        binding = ac->m_bindings[0];
    } else {
        HK_WARN(0x27343435, "No binding found in: " << animFilename);
        delete loader;
        return false;
    }
    
    // Add to animation list
    addAnimationEntry(animName, animation, binding);
    
    // Store loader for cleanup
    if (m_numLoaders == 0) {
        m_animLoaders = new hkLoader*[16];  // Initial capacity
    }
    m_animLoaders[m_numLoaders++] = loader;

    return true;
}

// Play animation
// Based on: NormalBlendingDemo::stepDemo()
bool AnimatedCharacter::playAnimation(const char* animName, float weight, float playbackSpeed)
{
    int index = findAnimation(animName);
    if (index < 0) {
        HK_WARN(0x27343436, "Animation not found: " << animName);
        return false;
    }

    AnimationEntry& entry = m_animations[index];

    // Set animation control parameters (permissive)
    if (entry.control) {
        entry.control->setMasterWeight(weight);
        entry.control->setPlaybackSpeed(playbackSpeed);
    }

    return true;
}

// Blend to animation
bool AnimatedCharacter::blendToAnimation(const char* animName, float targetWeight, float blendDuration)
{
    int index = findAnimation(animName);
    if (index < 0) {
        return false;
    }

    AnimationEntry& entry = m_animations[index];
    // Always allow blending; we keep permissive bindings.
    entry.targetWeight = targetWeight;
    entry.blendDuration = blendDuration;
    entry.blendTime = 0.0f;

    return true;
}

// Update animation state
// Based on: AnimatedSkeletonDemo::stepDemo(), NormalBlendingDemo::stepDemo()
void AnimatedCharacter::update(float deltaTime)
{
    if (!m_skeletonInstance) {
        return;
    }

    // Update blend weights
    for (int i = 0; i < m_numAnimations; i++) {
        AnimationEntry& entry = m_animations[i];

        if (entry.blendDuration > 0.0f && entry.control) {
            entry.blendTime += deltaTime;

            float t = entry.blendTime / entry.blendDuration;
            if (t >= 1.0f) {
                t = 1.0f;
                entry.blendDuration = 0.0f;  // Blend complete
            }

            float currentWeight = entry.control->getMasterWeight();
            float newWeight = currentWeight + (entry.targetWeight - currentWeight) * t;
            entry.control->setMasterWeight(newWeight);
        }
    }

    // Advance all active animations (same as all Havok demos)
    m_skeletonInstance->stepDeltaTime(deltaTime);
}

// Get current pose
// Based on: AnimatedSkeletonDemo::stepDemo(), NormalBlendingDemo::stepDemo()
void AnimatedCharacter::getPose(hkaPose& outPose)
{
    if (!m_skeletonInstance) {
        outPose.setToReferencePose();
        return;
    }

    // Sample and combine all active animations (EXACT same API as all Havok demos)
    m_skeletonInstance->sampleAndCombineAnimations(
        outPose.writeAccessPoseLocalSpace().begin(),
        outPose.writeAccessFloatSlotValues().begin()
    );
}

// Find animation by name
int AnimatedCharacter::findAnimation(const char* animName) const
{
    for (int i = 0; i < m_numAnimations; i++) {
        if (hkString::strCasecmp(m_animations[i].name, animName) == 0) {
            return i;
        }
    }
    return -1;
}

// Add animation entry
// Based on: NormalBlendingDemo constructor
void AnimatedCharacter::addAnimationEntry(const char* name,
                                           hkaSkeletalAnimation* anim,
                                           hkaAnimationBinding* binding)
{
    // Grow array if needed
    if (m_numAnimations >= m_maxAnimations) {
        int newMax = m_maxAnimations == 0 ? 8 : m_maxAnimations * 2;
        AnimationEntry* newArray = new AnimationEntry[newMax];

        if (m_animations) {
            for (int i = 0; i < m_numAnimations; i++) {
                newArray[i] = m_animations[i];
            }
            delete[] m_animations;
        }

        m_animations = newArray;
        m_maxAnimations = newMax;
    }

    // Add entry
    AnimationEntry& entry = m_animations[m_numAnimations];
    hkString::strNcpy(entry.name, name, 63);
    entry.name[63] = '\0';
    entry.animation = anim;
    entry.binding = binding;
    entry.targetWeight = 0.0f;
    entry.blendDuration = 0.0f;
    entry.blendTime = 0.0f;
    entry.bindingBoneCount = binding ? binding->m_numTransformTrackToBoneIndices : 0;
    // Be permissive: allow playing even when binding track count doesn't match skeleton.
    entry.compatible = true;

    // Create animation control (same as NormalBlendingDemo)
    entry.control = new hkaDefaultAnimationControl(binding);
    entry.control->setMasterWeight(0.0f);
    entry.control->setPlaybackSpeed(1.0f);

    // Add control to animated skeleton (same as all animation demos)
    if (m_skeletonInstance) {
        m_skeletonInstance->addAnimationControl(entry.control);

        // The animated skeleton now owns the control (same as all demos)
        entry.control->removeReference();
    }

    m_numAnimations++;
}

// Render skeleton bones for visualization
// Based on: Havok demos skeleton visualization
void AnimatedCharacter::renderSkeleton(hkgDisplayContext* context)
{
    if (!m_skeleton || !m_skeletonInstance || !context) {
        return;
    }

    // Sample current pose (v5.5.0 API: accessPoseLocalSpace + writeAccessFloatSlotValues)
    hkaPose pose(m_skeleton);
    m_skeletonInstance->sampleAndCombineAnimations(pose.accessPoseLocalSpace().begin(), pose.writeAccessFloatSlotValues().begin());

    // Sync to model space (convert local transforms to world space)
    pose.syncModelSpace();

    // Get skeleton data
    const hkInt16* parentIndices = m_skeleton->m_parentIndices;
    int numBones = m_skeleton->m_numBones;

    // Draw bones as lines from child to parent
    context->beginGroup(HKG_IMM_LINES);

    float boneColor[4] = {1.0f, 1.0f, 0.0f, 1.0f}; // Yellow bones
    context->setCurrentColor4(boneColor);

    for (int i = 0; i < numBones; i++) {
        int parentIdx = parentIndices[i];

        // Skip root bone (has no parent)
        if (parentIdx < 0) {
            continue;
        }

        // Get bone positions (v5.5.0 API: getBoneModelSpace)
        const hkVector4& childPos = pose.getBoneModelSpace(i).getTranslation();
        const hkVector4& parentPos = pose.getBoneModelSpace(parentIdx).getTranslation();

        // Draw line from child to parent
        float childPosArray[3];
        childPos.store3(childPosArray);
        context->setCurrentPosition(childPosArray);

        float parentPosArray[3];
        parentPos.store3(parentPosArray);
        context->setCurrentPosition(parentPosArray);
    }

    context->endGroup();

    // Draw joint spheres
    context->beginGroup(HKG_IMM_POINTS);

    float jointColor[4] = {1.0f, 0.0f, 0.0f, 1.0f}; // Red joints
    context->setCurrentColor4(jointColor);

    for (int i = 0; i < numBones; i++) {
        const hkVector4& pos = pose.getBoneModelSpace(i).getTranslation();
        float posArray[3];
        pos.store3(posArray);
        context->setCurrentPosition(posArray);
    }

    context->endGroup();
    context->flush();
}
