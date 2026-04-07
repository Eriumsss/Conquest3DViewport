// AnimationSystem.h — Havok 5.5.0 Skeletal Animation System
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// "Life is what happens when you’re busy making other plans." — John Lennon
// I planned to reverse-engineer one animation format. ONE. Now I have a
// full skeletal animation pipeline with blending, IK, ragdoll, motion
// matching, state machines, and a bone editor. Life happened. Hard.
//
// This is the Havok animation wrapper — loads skeletons, loads .hkx
// animation clips, creates bindings (maps animation tracks to skeleton
// bones), controls playback (play/pause/blend), samples poses, and
// feeds the result to the skinned mesh renderer. Pulled straight from
// Havok’s SkinningDemo, NormalBlendingDemo, AnimatedSkeletonDemo —
// the same demo code that Pandemic’s engineers at the LA studio copied
// into Conquest’s animation system back in 2007-2008.
//
// The stolen Havok 5.5 Animation SDK is doing all the heavy lifting here.
// hkaAnimatedSkeleton, hkaAnimationControl, hkaAnimationBinding — these
// classes are the backbone (literally) of every character in Conquest.
// We just had to figure out HOW Pandemic wired them together, which
// meant tracing the .exe’s animation init path through 30+ functions
// in Ghidra until the pattern matched the demo code.
//
// Pipeline:
// 1. Load skeleton (.hkx rig file)                    — the bone hierarchy
// 2. Load animations (.hkx clips)                     — the motion data
// 3. Create bindings (track→bone mapping)             — the glue
// 4. Create controls (playback state)                 — play/pause/blend
// 5. Create animated skeleton (combines everything)   — the final boss
// 6. Sample pose (bone transforms for this frame)     — the output
// 7. Apply pose to mesh (render the fucking thing)    — the payoff
// -----------------------------------------------------------------------

#ifndef ANIMATION_SYSTEM_H
#define ANIMATION_SYSTEM_H

// Havok v5.5.0 Animation Includes
#include <Common/Base/hkBase.h>
#include <Animation/Animation/hkaAnimationContainer.h>
#include <Animation/Animation/Rig/hkaSkeleton.h>
#include <Animation/Animation/hkaAnimation.h>
#include <Animation/Animation/Animation/hkaAnimationBinding.h>
#include "Animation/Animation/Playback/Control/Default/hkaDefaultAnimationControl.h"
#include <Animation/Animation/Playback/hkaAnimatedSkeleton.h>
#include <Animation/Animation/Rig/hkaPose.h>
#include <Animation/Animation/Mapper/hkaSkeletonMapper.h>

// Havok Scene Data
#include <Common/SceneData/Mesh/hkxMesh.h>
#include <Common/SceneData/Mesh/hkxMeshSection.h>
#include <Animation/Animation/Deform/Skinning/hkaMeshBinding.h>

// Havok Serialization
#include <Common/Serialize/Packfile/hkPackfileData.h>

// Forward declarations
class hkLoader;
class hkRootLevelContainer;

/**
 * AnimatedCharacter - Character animation we got from havok without consent
 * 
 * Not the exact copy of Pandemic's Havok system from LOTR : Conquest, but we got it by pirating the SDK.
 * The support team is long dead i think they can't complain anymore.
 * 
 * Sourced from antique demos we snagged without permission: SkinningDemo and friends.
 * Those poor files. They deserve retirement. Instead, they're enslaved in our codebase.
 * 
 *   The pirated path:
 * 
 *   hkaSkeleton
 *       ↓
 *   hkaAnimatedSkeleton (instance we hotfixed from assembly dumps)
 *       ↓
 *   hkaDefaultAnimationControl[] (controls for multiple unlicensed animations)
 *       ↓
 *   sampleAndCombineAnimations() → hkaPose
 *       ↓
 *   Apply to skinned mesh → Render
 * 
 *   How to use:
 *   AnimatedCharacter character;
 *   character.loadSkeleton("Resources/Animation/HavokGirl/hkRig.hkx");
 *   character.loadAnimation("Resources/Animation/HavokGirl/hkWalk.hkx", "walk");
 *   character.loadAnimation("Resources/Animation/HavokGirl/hkRun.hkx", "run");
 *   character.playAnimation("walk", 1.0f);
 *   character.blendToAnimation("run", 0.5f, 1.0f);
 *   
 *   // In game loop:
 *   character.update(deltaTime);
 *   hkaPose pose = character.getPose();
 *   // Apply pose... render... act surprised if caught
 */
class AnimatedCharacter {
public:
    AnimatedCharacter();
    ~AnimatedCharacter();
    
    /**
     * Load skeleton rig from .hkx file
     * 
     * @param rigFilename - Path to .hkx rig file (e.g., "Resources/Animation/HavokGirl/hkRig.hkx")
     * @return true on success, false on failure
     * 
     * The rig file contains:
     * - hkaSkeleton (bone hierarchy, reference pose, parent indices)
     * - hkaMeshBinding[] (skin bindings for mesh deformation)
     */
    bool loadSkeleton(const char* rigFilename);
    
    /**
     * Set skeleton from external source (e.g., game model loader)
     *
     * @param skeleton - Pointer to hkaSkeleton (ownership transferred to AnimatedCharacter)
     * @return true on success, false on failure
     *
     * Use this when loading skeletons from game models instead of .hkx files.
     */
    bool setSkeleton(hkaSkeleton* skeleton);

    /**
     * Load animation from .hkx file
     *
     * @param animFilename - Path to .hkx animation file
     * @param animName - Friendly name for this animation (e.g., "walk", "run", "idle")
     * @return true on success, false on failure
     *
     * The animation file contains:
     * - hkaSkeletalAnimation (keyframe data)
     * - hkaAnimationBinding (maps animation tracks to skeleton bones)
     *
     * Multiple animations can be loaded and blended together.
     */
    bool loadAnimation(const char* animFilename, const char* animName);
    
    /**
     * Play an animation
     * 
     * @param animName - Name of animation to play
     * @param weight - Blend weight (0.0 = invisible, 1.0 = full strength)
     * @param playbackSpeed - Playback speed multiplier (1.0 = normal, 2.0 = double speed)
     * @return true if animation found, false otherwise
     */
    bool playAnimation(const char* animName, float weight = 1.0f, float playbackSpeed = 1.0f);
    
    /**
     * Blend to another animation over time
     * 
     * @param animName - Target animation name
     * @param targetWeight - Target blend weight
     * @param blendDuration - Time to blend (in seconds)
     * @return true if animation found, false otherwise
     */
    bool blendToAnimation(const char* animName, float targetWeight, float blendDuration);
    
    /**
     * Update animation state
     * 
     * @param deltaTime - Time since last frame (in seconds)
     * 
     * This advances all active animations and updates blend weights.
     */
    void update(float deltaTime);
    
    /**
     * Get the current pose
     * 
     * @param outPose - Output pose (must be pre-allocated with correct skeleton)
     * 
     * This samples all active animations and combines them into a single pose.
     * Same API as used in all Havok demos: sampleAndCombineAnimations()
     */
    void getPose(hkaPose& outPose);
    
    /**
     * Get the skeleton
     */
    hkaSkeleton* getSkeleton() const { return m_skeleton; }
    
    /**
     * Get the animated skeleton instance
     */
    hkaAnimatedSkeleton* getAnimatedSkeleton() const { return m_skeletonInstance; }
    
    /**
     * Get skin bindings (for mesh deformation)
     */
    hkaMeshBinding** getSkinBindings() const { return m_skinBindings; }
    int getNumSkinBindings() const { return m_numSkinBindings; }

    /**
     * Render skeleton bones for visualization
     * Based on: Havok demos skeleton visualization
     */
    void renderSkeleton(class hkgDisplayContext* context);

private:
    // Skeleton data
    hkaSkeleton* m_skeleton;                    // Bone hierarchy and reference pose
    hkaAnimatedSkeleton* m_skeletonInstance;    // Animated skeleton instance
    hkaMeshBinding** m_skinBindings;            // Skin bindings for mesh deformation
    int m_numSkinBindings;                      // Number of skin bindings
    
    // Animation data
    struct AnimationEntry {
        char name[64];                          // Friendly name
        hkaSkeletalAnimation* animation;        // Animation data
        hkaAnimationBinding* binding;           // Binding to skeleton
        hkaDefaultAnimationControl* control;    // Playback control
        float targetWeight;                     // Target blend weight
        float blendDuration;                    // Blend duration
        float blendTime;                        // Current blend time
        int bindingBoneCount;                   // Track count in binding
        bool compatible;                        // True if binding matches skeleton
    };
    
    AnimationEntry* m_animations;               // Array of loaded animations
    int m_numAnimations;                        // Number of loaded animations
    int m_maxAnimations;                        // Capacity
    
    // Loaders (for resource management)
    hkLoader* m_rigLoader;                      // Loader for rig file
    hkLoader** m_animLoaders;                   // Loaders for animation files
    int m_numLoaders;                           // Number of loaders

    // Packfile data (for memory management)
    hkPackfileData* m_rigPackfileData;          // Packfile data for rig
    hkPackfileData** m_animPackfileData;        // Packfile data for animations

    // Internal methods
    int findAnimation(const char* animName) const;
    void addAnimationEntry(const char* name, hkaSkeletalAnimation* anim, 
                           hkaAnimationBinding* binding);
    
    // Disable copy constructor and assignment
    AnimatedCharacter(const AnimatedCharacter&);
    AnimatedCharacter& operator=(const AnimatedCharacter&);
};

#endif // ANIMATION_SYSTEM_H
