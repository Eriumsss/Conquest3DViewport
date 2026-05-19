// Scene3DEffects.h — Fire, Smoke, and Other Things That Shouldn't Exist
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Particle effects system — emitters, particles, billboards, velocity fields,
// color gradients, size curves, rotation rates. Every explosion, every torch
// flame, every magic spell effect in Conquest was built from these primitives.
// Pandemic's effects artists defined them in their internal effect editor
// and exported them as binary blobs in the PAK files.
//
// We reverse-engineered the effect format from hex dumps of the EffectInfo
// entries in Block1. Each effect is a tree of emitters, each emitter spawns
// particles with configurable lifetime, velocity, color ramp, texture
// animation, and blend mode. The blend modes map directly to D3D9
// render states (SRCBLEND, DESTBLEND) — Pandemic stored the actual D3D9
// enum values in the effect data. No abstraction layer. Just raw D3D9
// constants baked into the art assets. Bold move, Pandemic. Fucking bold.
//
// "Memento mori." — Remember you will die. And so will these particles.
// Every frame. Thousands of them. Born, rendered, dead. Like everything else.
// -----------------------------------------------------------------------

#ifndef SCENE3D_EFFECTS_H
#define SCENE3D_EFFECTS_H

#include <Common/Base/hkBase.h>
#include <Common/Base/Math/Vector/hkVector4.h>

// Forward declarations
class hkgTexture;
class hkgDisplayContext;

// Color helper (matches game's ARGB format)
struct EffectColor
{
    unsigned char a, r, g, b;
    
    EffectColor() : a(255), r(255), g(255), b(255) {}
    EffectColor(unsigned char _a, unsigned char _r, unsigned char _g, unsigned char _b)
        : a(_a), r(_r), g(_g), b(_b) {}
    
    // Parse from hex string like "0xFFFFFFFF" (ARGB)
    static EffectColor fromHex(const char* hex);
    
    // Interpolate between two colors
    static EffectColor lerp(const EffectColor& a, const EffectColor& b, float t);
};

// Single particle instance (like a vertex in a mesh)
struct Particle
{
    hkVector4 position;           // World position
    hkVector4 velocity;           // Movement velocity
    EffectColor color;            // Current color (interpolated)
    EffectColor startColor;       // Per-particle start color (randomized)
    EffectColor endColor;         // Per-particle end color (randomized)
    float colorMultiplier;        // HDR-ish multiplier for RGB (not alpha)
    float width;                  // Current width (horizontal extent)
    float height;                 // Current height (vertical extent)
    float startWidth;             // Authored start width (game units, typically cm)
    float startHeight;            // Authored start height (game units, typically cm)
    float endWidth;               // Authored end width (game units, typically cm)
    float endHeight;              // Authored end height (game units, typically cm)
    float pivotX;                 // Billboard pivot (0=left, 1=right)
    float pivotY;                 // Billboard pivot (0=bottom, 1=top)
    hkVector4 rotation;           // Euler rotation in radians (X,Y,Z)
    hkVector4 angularVelStart;    // Angular velocity at spawn (radians/sec)
    hkVector4 angularVelEnd;      // Angular velocity at death (radians/sec)
    float lifetime;               // Time alive (seconds)
    float maxLifetime;            // Total lifetime (seconds)
    int currentFrame;             // Current texture atlas frame
    float animationTime;          // Animation playback time
    int alphaTestStart;           // Per-particle alpha test ref at spawn (0..255)
    int alphaTestEnd;             // Per-particle alpha test ref at death (0..255)
    
    Particle();
    void update(float deltaTime);
    float getLifetimeRatio() const { return lifetime / maxLifetime; }
};

// UV rectangle for a texture within the shared FX atlas
struct AtlasUVRect
{
    float u0, v0, u1, v1;  // Left, top, right, bottom
    AtlasUVRect() : u0(0), v0(0), u1(1), v1(1) {}
    AtlasUVRect(float _u0, float _v0, float _u1, float _v1)
        : u0(_u0), v0(_v0), u1(_u1), v1(_v1) {}
};

// Controller-driven spawn rate segment (30 FPS fixed-step frames)
struct RateSegment
{
    int startFrame;
    int endFrame;
    int fadeInFrames;
    int fadeOutFrames;
    float rate; // Particles per frame at 30 FPS

    RateSegment()
        : startFrame(0)
        , endFrame(0)
        , fadeInFrames(0)
        , fadeOutFrames(0)
        , rate(0.0f)
    {
    }
};

// Generic float controller segment (30 FPS fixed-step frames)
struct FloatSegment
{
    int startFrame;
    int endFrame;
    int fadeInFrames;
    int fadeOutFrames;
    float value;

    FloatSegment()
        : startFrame(0)
        , endFrame(0)
        , fadeInFrames(0)
        , fadeOutFrames(0)
        , value(0.0f)
    {
    }
};

// Force/collision effector volume referenced by emitters (Effectors / EmitterEffectors arrays).
// The JSON stores a subtype in the "Type" field as a CRC (e.g. 0x7C34BCCE, 0x796690EA).
struct EffectEffector
{
    int guid;                     // GUID from JSON
    unsigned int typeId;          // CRC/type id from JSON "Type"
    bool containing;              // "Containing"
    bool isEmitterEffector;       // True if object type was "EmitterEffector"
    float force;                  // "force"
    float damping;                // "Damping"
    float bounce;                 // "Bounce"
    float height;                 // "Height"
    float constrict;              // "Constrict"

    hkVector4 position;           // From Transform/WorldTransform translation
    hkVector4 axisY;              // Local Y axis in world space (normalized, best-effort)
    float transformScale;         // Uniform-ish scale extracted from matrix (X axis length)

    int controllerGuids[16];      // Controller GUIDs that animate this effector (usually "force")
    int controllerCount;
    FloatSegment forceSegments[16];
    int forceSegmentCount;

    EffectEffector();
};

// Particle emitter (like a mesh part with spawn rules)
struct ParticleEmitter
{
    // Emitter properties from JSON
    char name[64];
    char textureName[128];
    hkgTexture* texture;          // Loaded texture (like diffuseTexture in MeshPart)
    bool usesSharedAtlas;         // True if texture is the shared atlas (don't release it)
    AtlasUVRect atlasUVRect;      // UV rect within the atlas for this emitter's texture

    // Refraction / heat-haze (special shader in the shipped game).
    bool refract;                 // "Refract"
    float refractionScale;        // "RefractionScale"
    char refractionTextureName[128]; // "RefractionTexture"
    hkgTexture* refractionTexture;   // Refraction normal/noise map (usually *_N, often atlas_2)
    bool usesSharedAtlasRefraction;  // True if refractionTexture is a shared atlas
    AtlasUVRect refractionAtlasUVRect; // UV rect for refractionTexture within its atlas

    // Texture atlas settings
    bool useAtlas;
    int frameCount;
    int minFrame;
    int maxFrame;
    float playbackTime;           // Flipbook loop length (game frames at 30 FPS)
    bool loopTextureAnim;         // Loop flipbook animation
    bool randomizeStartFrame;

    // Computed flipbook layout (after atlas UV + texture dimensions are known)
    int uvColumns;                // Columns in the flipbook grid
    int uvRows;                   // Rows in the flipbook grid
    int animFrameMin;             // Inclusive start frame index for animation/random selection
    int animFrameMax;             // Inclusive end frame index for animation/random selection
    
    // Particle type
    enum ParticleType {
        BILLBOARD,                // Camera-facing quads (like textured plane)
        MESH                      // Geometry instances (like rendering multiple models)
    };
    ParticleType particleType;

    // Orientation mode (affects how quads are oriented)
    enum OrientMode
    {
        ORIENT_NONE,
        ORIENT_Z_TO_MOVEMENT_DIRECTION // "orientZToMovementDirection" and hashed equivalents
    };
    OrientMode orientMode;
    
    // Blend mode
    enum BlendMode {
        NORMAL,                   // Alpha blending
        ADDITIVE                  // Additive blending (for fire/lightning)
    };
    BlendMode blendMode;
    
    // Spawn settings
    float rateMin, rateMax;       // Particles per second
    int lifespanMin, lifespanMax; // Particle lifetime (frames)
    int fadeInFrames;             // Fade in time in frames (0 = none)
    float initialVelocityContribution; // 0..1 mix between random dir and emitter axis dir
    bool sortParticles;           // Sort back-to-front for correct blending

    // Optional controller-driven rate segments (used when RateMin/RateMax are 0)
    hkArray<RateSegment> rateSegments;
    
    // Size animation
    float startWidthMin, startWidthMax;
    float startHeightMin, startHeightMax;
    float endWidthMin, endWidthMax;
    float endHeightMin, endHeightMax;

    // Pivot point (where the particle position lies within the quad)
    float pivotXMin, pivotXMax;
    float pivotYMin, pivotYMax;
    
    // Color animation
    EffectColor startColorMin, startColorMax;
    EffectColor endColorMin, endColorMax;
    float startColorMultiplier;
    float endColorMultiplier;

    // Alpha test (0..255). Many effects rely on this to avoid bright "sheets" from low-alpha pixels.
    int startAlphaTestMin;
    int startAlphaTestMax;
    int endAlphaTestMin;
    int endAlphaTestMax;
    
    // Rotation
    hkVector4 initialOrientationMin;
    hkVector4 initialOrientationMax;
    hkVector4 startRotationVelocityMin;
    hkVector4 startRotationVelocityMax;
    hkVector4 endRotationVelocityMin;
    hkVector4 endRotationVelocityMax;
    
    // Physics
    float speedMin, speedMax;
    float acceleration;
    hkVector4 externalAcceleration;    // From Effectors[] (world units per second^2)
    float externalDamping;             // From Effectors[] (per-second damping coefficient)

    // Effectors attached to this emitter (by GUID in the JSON)
    hkArray<EffectEffector*> effectors;        // "Effectors"
    hkArray<EffectEffector*> emitterEffectors; // "EmitterEffectors"
    
    // Emitter shape
    enum EmitterType {
        POINT,
        SPHERE,
        DOME,
        CONE,
        CIRCLE,
        CYLINDER,
        POLYGON,
        DIRECTION,
        MESH_EMITTER
    };
    EmitterType emitterType;
    bool volumeEmit;              // Emit from volume or surface
    float height;                 // For cone/cylinder
    float angle;                  // For cone
    hkVector4 axisY;              // Emitter's local Y axis in world space (normalized, best-effort)
    
    // Transform
    hkVector4 position;
    hkVector4 rotation;
    hkVector4 localOffset;             // Local translation from Transform/WorldTransform (scaled to renderer units)
    float transformScale;             // Scale from WorldTransform matrix (default 1.0)
    
    // Runtime state
    hkArray<Particle> particles;  // Active particles (like vertices in a mesh)
    float spawnAccumulator;       // Time until next spawn
    
    ParticleEmitter();
    ~ParticleEmitter();
    
    void spawn(int count);
    void update(float deltaTime, float effectiveRatePerFrame, float effectTimeSeconds);
    void removeDeadParticles();
};

// Effect light (dynamic point light attached to effect)
struct EffectLight
{
    hkVector4 position;
    EffectColor color;
    float colorScale;
    float radius;
    float innerRadius;
    
    EffectLight();
};

// Complete effect definition (like a GameModel)
struct EffectDefinition
{
    char name[128];
    int duration;                 // Effect duration (frames)
    bool looping;
    bool worldSpace;              // World-space or local-space particles
    bool sortParticles;           // Sort particles back-to-front for correct blending
    
    hkArray<ParticleEmitter*> emitters;
    hkArray<EffectLight*> lights;
    hkArray<EffectEffector*> effectors;
    
    EffectDefinition();
    ~EffectDefinition();
    
    void release();
};

#endif // SCENE3D_EFFECTS_H
