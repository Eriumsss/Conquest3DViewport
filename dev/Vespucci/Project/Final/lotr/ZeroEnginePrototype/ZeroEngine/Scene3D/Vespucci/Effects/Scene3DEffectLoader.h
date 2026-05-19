// Scene3DEffectLoader.h — Parsing Dead Effects Back Into Existence
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Loads particle effect definitions from JSON files (our decoded
// representation of Pandemic's binary effect format). Follows the same
// pattern as GameModelLoader — read JSON metadata, extract emitter
// parameters, load textures, wire everything together into a
// renderable EffectDefinition.
//
// Pandemic's effect artists used a visual editor to build these effects.
// We use a JSON file that's been hex-dumped, decoded, annotated, and
// prayed over. Same result. Very different vibes.
// -----------------------------------------------------------------------

#ifndef SCENE3D_EFFECT_LOADER_H
#define SCENE3D_EFFECT_LOADER_H

#include "Scene3DEffects.h"

// Forward declarations
class hkgDisplayContext;

// Effect loader class (like GameModelLoader)
class EffectLoader
{
public:
    EffectLoader();
    ~EffectLoader();
    
    // Set display context for texture loading
    void setDisplayContext(hkgDisplayContext* context);
    
    // Load effect from JSON file
    EffectDefinition* loadEffect(const char* jsonPath, const char* textureDir);
    
private:
    hkgDisplayContext* m_context;
    
    // JSON parsing helpers (manual parsing like Scene3DAnimation.cpp)
    static const char* findKey(const char* start, const char* end, const char* key);
    static const char* findMatchingBrace(const char* start, char open, char close);
    static bool extractString(const char* start, const char* end, const char* key, char* out, int outSize);
    static bool extractInt(const char* start, const char* end, const char* key, int* out);
    static bool extractFloat(const char* start, const char* end, const char* key, float* out);
    static bool extractBool(const char* start, const char* end, const char* key, bool* out);
    static bool extractColor(const char* start, const char* end, const char* key, EffectColor* out);
    static bool extractVector3(const char* start, const char* end, const char* key, hkVector4* out);
    static bool extractTransformScale(const char* start, const char* end, const char* key, float* outScale);
    static bool extractTransformTranslation(const char* start, const char* end, const char* key, hkVector4* outTranslation);
    
    // Parse emitter from JSON object
    ParticleEmitter* parseEmitter(const char* objStart, const char* objEnd, const char* textureDir);
    
    // Parse effect light from JSON object
    EffectLight* parseEffectLight(const char* objStart, const char* objEnd);
    
    // Load texture (like GameModelLoader::loadDDSTexture)
    hkgTexture* loadTexture(const char* textureName, const char* textureDir);
};

#endif // SCENE3D_EFFECT_LOADER_H
