// Scene3DEffectManager.h — Herding a Thousand Dying Particles Per Frame
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Manages the lifecycle of all active particle effects: spawning,
// updating, rendering, killing. Each effect is a hierarchy of emitters
// that produce particles. The manager ticks every emitter, ages every
// particle, removes the dead ones, and feeds the survivors to the
// renderer. At peak load during a Conquest battle, there could be
// 50+ simultaneous effects (fire, smoke, sword trails, magic blasts,
// blood splatter, dust clouds). This manager handles all of them
// without skipping a goddamn frame.
//
// "This too shall pass." — Persian proverb. So will every particle.
// Birth → update → render → death. 0.3 seconds average lifetime.
// Thousands per frame. A constant churn of creation and destruction.
// Just like this project. Just like everything.
// -----------------------------------------------------------------------

#ifndef SCENE3D_EFFECT_MANAGER_H
#define SCENE3D_EFFECT_MANAGER_H

#include "Scene3DEffects.h"
#include "Scene3DEffectLoader.h"
#include <Common/Base/Container/Array/hkArray.h>
#include <Common/Base/Math/Vector/hkVector4.h>

// Forward declarations
class hkgDisplayContext;
class hkgTexture;
class Scene3DRenderer;
class hkaPose;

// Active effect instance
struct ActiveEffect
{
    EffectDefinition* definition;  // Shared effect definition
    hkArray<ParticleEmitter*> emitters; // Per-instance emitters (runtime state)
    hkArray<EffectLight*> lights;        // Per-instance lights (future use)
    hkVector4 position;             // World position
    hkVector4 rotation;             // Rotation (euler angles)
    hkVector4 lastPosition;         // Previous frame position (for local-space follow)
    bool hasLastPosition;           // True once lastPosition is initialized
    float time;                     // Current playback time
    bool finished;                  // True when effect is done
    int boneIndex;                  // Bone to attach to (-1 = world space)
    int debugFramesLeft;            // Debug: per-frame logging window after spawn

    ActiveEffect();
};

// Atlas UV entry — maps a texture name to its UV rectangle in the atlas
struct AtlasUVEntry
{
    char name[128];
    AtlasUVRect rect;
};

// Atlas set for a specific level (atlas texture + per-sprite UV table)
struct AtlasSet
{
    char levelName[64];                   // e.g. "Moria", "Helm'sDeep"
    int atlasIndex;                       // 1 or 2 (atlas_1 / atlas_2)
    hkgTexture* atlasTexture;             // blocks/<Level>/atlas_1.dds
    hkArray<AtlasUVEntry> entries;        // srcjson/<Level>/sub_blocks1/atlas_1.json
    bool uvAttempted;
    bool uvLoaded;
    bool textureAttempted;

    AtlasSet();
};

// Effect manager - manages spawning, updating, and cleanup of effects
class EffectManager
{
public:
    EffectManager();
    ~EffectManager();

    // Initialize with display context and load atlas data
    void initialize(hkgDisplayContext* context);

    // Load effect definition from JSON file
    EffectDefinition* loadEffectDefinition(const char* jsonPath, const char* textureDir);

    // Spawn effect at world position
    ActiveEffect* spawnEffect(const char* effectName, const hkVector4& position);

    // Spawn effect attached to bone
    ActiveEffect* spawnEffectOnBone(const char* effectName, int boneIndex);

    // Update all active effects
    void update(float deltaTime, Scene3DRenderer* renderer, const hkaPose* pose);

    // Render all active effects
    void render(Scene3DRenderer* renderer);

    // Clear all active effects
    void clearAll();

    // Get number of active effects
    int getActiveEffectCount() const { return m_activeEffects.getSize(); }

    // Debug/diagnostics helpers
    bool hasActiveEffectNamed(const char* effectName) const;

private:
    EffectLoader m_loader;
    hkgDisplayContext* m_context;
    hkArray<EffectDefinition*> m_definitions;  // Loaded effect definitions
    hkArray<ActiveEffect*> m_activeEffects;    // Active effect instances

    // Level atlas registry (loaded on demand)
    hkArray<AtlasSet*> m_atlasSets;

    // Atlas helpers
    static void buildGameFilesDirFromTextureDir(const char* textureDir, char* outDir, int outDirSize);
    static const char* detectPreferredAtlasLevelFromEffectName(const char* effectName);

    AtlasSet* getOrCreateAtlasSet(const char* levelName, int atlasIndex);
    bool ensureAtlasUVLoaded(AtlasSet* set, const char* gameFilesDir);
    bool ensureAtlasTextureLoaded(AtlasSet* set, const char* gameFilesDir);

    static bool findAtlasUVInEntries(const hkArray<AtlasUVEntry>& entries, const char* textureName, AtlasUVRect* outRect);
    void parseAtlasJSON(const char* jsonPath, hkArray<AtlasUVEntry>& outEntries);

    bool resolveAtlasForTexture(const char* effectName,
                               const char* textureName,
                               const char* textureDir,
                               AtlasSet** outSet,
                               AtlasUVRect* outRect);

    // Patch emitter textures to use atlas
    void patchEmittersWithAtlas(EffectDefinition* def, const char* textureDir);

    // Find loaded definition by name
    EffectDefinition* findDefinition(const char* name);

    // Remove finished effects
    void removeFinishedEffects();
};

#endif // SCENE3D_EFFECT_MANAGER_H
