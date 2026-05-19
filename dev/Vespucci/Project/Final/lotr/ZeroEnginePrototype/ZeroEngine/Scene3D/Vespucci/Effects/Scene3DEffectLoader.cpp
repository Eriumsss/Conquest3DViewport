// Scene3DEffectLoader.cpp — Resurrecting Pandemic's Fire and Smoke From JSON
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Parses effect JSON files (decoded from Pandemic's binary EffectInfo
// format) into EffectDefinition structs. Each effect is a tree of emitters
// with spawn rates, velocity fields, color gradients, texture references,
// and D3D9 blend mode constants. Pandemic stored the raw D3D9 enum values
// in the art data. We read them and pass them straight to SetRenderState().
// No abstraction. Just trust the dead data. It knows what it wants.
// -----------------------------------------------------------------------

#include "Scene3DEffectLoader.h"
#include "Scene3DRendererInternal.h"
#include "ZeroMath.h"
#include <Graphics/Common/Texture/hkgTexture.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <math.h>

// Constructor
EffectLoader::EffectLoader()
    : m_context(HK_NULL)
{
}

// Destructor
EffectLoader::~EffectLoader()
{
}

// Set display context
void EffectLoader::setDisplayContext(hkgDisplayContext* context)
{
    m_context = context;
}

// Load text file (same as Scene3DAnimation.cpp)
static char* LoadTextFile(const char* path, int* outSize)
{
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    
    fseek(file, 0, SEEK_END);
    int size = (int)ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* buffer = (char*)malloc(size + 1);
    if (!buffer)
    {
        fclose(file);
        return NULL;
    }
    
    int bytesRead = (int)fread(buffer, 1, size, file);
    buffer[bytesRead] = '\0';
    fclose(file);
    
    if (outSize) *outSize = bytesRead;
    return buffer;
}

// Extract int array value (array like [1, 2, 3])
static int ExtractIntArray(const char* start, const char* end, const char* key, int* outValues, int maxValues)
{
    if (!start || !end || !key || !outValues || maxValues <= 0)
        return 0;

    // Reuse EffectLoader::findKey semantics by duplicating a minimal scan here.
    const int keyLen = (int)strlen(key);
    const char* p = start;

    const char* valueStart = HK_NULL;
    while (p < end)
    {
        const char* quote1 = (const char*)memchr(p, '\"', end - p);
        if (!quote1 || quote1 >= end) break;

        const char* quote2 = (const char*)memchr(quote1 + 1, '\"', end - (quote1 + 1));
        if (!quote2 || quote2 >= end) break;

        int len = (int)(quote2 - quote1 - 1);
        if (len == keyLen && memcmp(quote1 + 1, key, keyLen) == 0)
        {
            const char* colon = (const char*)memchr(quote2, ':', end - quote2);
            if (colon && colon < end)
                valueStart = colon + 1;
            break;
        }

        p = quote2 + 1;
    }

    if (!valueStart)
        return 0;

    while (valueStart < end && (*valueStart == ' ' || *valueStart == '\t' || *valueStart == '\n' || *valueStart == '\r'))
        valueStart++;

    while (valueStart < end && *valueStart != '[')
        valueStart++;

    if (valueStart >= end || *valueStart != '[')
        return 0;

    valueStart++; // after '['

    int count = 0;
    const char* s = valueStart;
    while (s < end && *s && *s != ']' && count < maxValues)
    {
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == ','))
            s++;

        if (s >= end || *s == ']' || *s == '\0')
            break;

        char* next = HK_NULL;
        long v = strtol(s, &next, 10);
        if (next == s)
            break;

        outValues[count++] = (int)v;
        s = next;
    }

    return count;
}

static unsigned int ParseTypeIdString(const char* s)
{
    if (!s || s[0] == '\0')
        return 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (unsigned int)strtoul(s + 2, NULL, 16);

    return (unsigned int)strtoul(s, NULL, 10);
}

static EffectEffector* FindEffectorByGuid(const hkArray<EffectEffector*>& list, int guid)
{
    for (int i = 0; i < list.getSize(); ++i)
    {
        EffectEffector* e = list[i];
        if (e && e->guid == guid)
            return e;
    }
    return HK_NULL;
}

// Find key in JSON range
const char* EffectLoader::findKey(const char* start, const char* end, const char* key)
{
    if (!start || !end || !key) return NULL;
    
    int keyLen = (int)strlen(key);
    const char* p = start;
    
    while (p < end)
    {
        // Look for "key":
        const char* quote1 = (const char*)memchr(p, '\"', end - p);
        if (!quote1 || quote1 >= end) break;
        
        const char* quote2 = (const char*)memchr(quote1 + 1, '\"', end - (quote1 + 1));
        if (!quote2 || quote2 >= end) break;
        
        int len = (int)(quote2 - quote1 - 1);
        if (len == keyLen && memcmp(quote1 + 1, key, keyLen) == 0)
        {
            // Found key, look for colon
            const char* colon = (const char*)memchr(quote2, ':', end - quote2);
            if (colon && colon < end)
                return colon + 1;
        }
        
        p = quote2 + 1;
    }
    
    return NULL;
}

// Find matching brace/bracket
const char* EffectLoader::findMatchingBrace(const char* start, char open, char close)
{
    if (!start || *start != open) return NULL;
    
    int depth = 1;
    const char* p = start + 1;
    
    while (*p && depth > 0)
    {
        if (*p == open) depth++;
        else if (*p == close) depth--;
        if (depth == 0) return p;
        p++;
    }
    
    return NULL;
}

// Extract string value
bool EffectLoader::extractString(const char* start, const char* end, const char* key, char* out, int outSize)
{
    const char* valueStart = findKey(start, end, key);
    if (!valueStart) return false;
    
    // Skip whitespace
    while (valueStart < end && (*valueStart == ' ' || *valueStart == '\t' || *valueStart == '\n' || *valueStart == '\r'))
        valueStart++;
    
    if (valueStart >= end || *valueStart != '\"') return false;
    
    const char* strStart = valueStart + 1;
    const char* strEnd = (const char*)memchr(strStart, '\"', end - strStart);
    if (!strEnd) return false;
    
    int len = (int)(strEnd - strStart);
    if (len >= outSize) len = outSize - 1;
    
    memcpy(out, strStart, len);
    out[len] = '\0';
    return true;
}

// Extract int value
bool EffectLoader::extractInt(const char* start, const char* end, const char* key, int* out)
{
    const char* valueStart = findKey(start, end, key);
    if (!valueStart) return false;
    
    *out = atoi(valueStart);
    return true;
}

// Extract float value
bool EffectLoader::extractFloat(const char* start, const char* end, const char* key, float* out)
{
    const char* valueStart = findKey(start, end, key);
    if (!valueStart) return false;
    
    *out = (float)atof(valueStart);
    return true;
}

// Extract bool value
bool EffectLoader::extractBool(const char* start, const char* end, const char* key, bool* out)
{
    const char* valueStart = findKey(start, end, key);
    if (!valueStart) return false;

    // Skip whitespace
    while (valueStart < end && (*valueStart == ' ' || *valueStart == '\t'))
        valueStart++;

    if (valueStart < end && (valueStart[0] == 't' || valueStart[0] == 'T'))
        *out = true;
    else
        *out = false;

    return true;
}

// Extract color value (hex string like "0xFFFFFFFF")
bool EffectLoader::extractColor(const char* start, const char* end, const char* key, EffectColor* out)
{
    char hexStr[32];
    if (!extractString(start, end, key, hexStr, sizeof(hexStr)))
        return false;

    *out = EffectColor::fromHex(hexStr);
    return true;
}

// Extract vector3 value (array like [x, y, z])
bool EffectLoader::extractVector3(const char* start, const char* end, const char* key, hkVector4* out)
{
    const char* valueStart = findKey(start, end, key);
    if (!valueStart) return false;

    // Skip to opening bracket
    while (valueStart < end && *valueStart != '[')
        valueStart++;

    if (valueStart >= end) return false;

    float x = 0.0f, y = 0.0f, z = 0.0f;
    sscanf_s(valueStart, "[%f,%f,%f]", &x, &y, &z);

    out->set(x, y, z, 0.0f);
    return true;
}

// Extract scale from a 4x4 transform matrix stored as a 16-element JSON array
// Scale = length of the first column vector (m[0], m[1], m[2])
bool EffectLoader::extractTransformScale(const char* start, const char* end, const char* key, float* outScale)
{
    const char* valueStart = findKey(start, end, key);
    if (!valueStart) return false;

    // Skip to opening bracket
    while (valueStart < end && *valueStart != '[')
        valueStart++;

    if (valueStart >= end) return false;

    float m[16];
    int count = sscanf_s(valueStart, "[%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f]",
                         &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7],
                         &m[8], &m[9], &m[10], &m[11], &m[12], &m[13], &m[14], &m[15]);

    if (count < 16) return false;

    // Scale = length of first row vector (assumes row-major: m[0],m[1],m[2] is X axis)
    float sx = ZLength3f(m[0], m[1], m[2]);
    if (sx < 0.0001f) sx = 1.0f; // Avoid zero scale
    *outScale = sx;
    return true;
}

// Extract translation (position) from a 4x4 transform matrix stored as a 16-element JSON array
// Translation = last row (m[12], m[13], m[14]) in the game's row-major transforms.
bool EffectLoader::extractTransformTranslation(const char* start, const char* end, const char* key, hkVector4* outTranslation)
{
    if (!outTranslation) return false;

    const char* valueStart = findKey(start, end, key);
    if (!valueStart) return false;

    // Skip to opening bracket
    while (valueStart < end && *valueStart != '[')
        valueStart++;

    if (valueStart >= end) return false;

    float m[16];
    int count = sscanf_s(valueStart, "[%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f]",
                         &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7],
                         &m[8], &m[9], &m[10], &m[11], &m[12], &m[13], &m[14], &m[15]);
    if (count < 16) return false;

    outTranslation->set(m[12], m[13], m[14], 0.0f);
    return true;
}

// Extract the Y axis (row 1) from a 4x4 transform matrix stored as a 16-element JSON array.
// The game's transforms are row-major; row 1 (m[4],m[5],m[6]) corresponds to the local Y axis in world space.
static bool ExtractTransformAxisY(const char* start, const char* end, const char* key, float* outX, float* outY, float* outZ)
{
    if (!outX || !outY || !outZ) return false;
    *outX = *outY = *outZ = 0.0f;

    // Minimal local key scan (can't call EffectLoader::findKey here because it's private).
    const int keyLen = (int)strlen(key);
    const char* valueStart = NULL;
    const char* p = start;
    while (p < end)
    {
        const char* quote1 = (const char*)memchr(p, '\"', end - p);
        if (!quote1 || quote1 >= end) break;

        const char* quote2 = (const char*)memchr(quote1 + 1, '\"', end - (quote1 + 1));
        if (!quote2 || quote2 >= end) break;

        int len = (int)(quote2 - quote1 - 1);
        if (len == keyLen && memcmp(quote1 + 1, key, keyLen) == 0)
        {
            const char* colon = (const char*)memchr(quote2, ':', end - quote2);
            if (colon && colon < end)
                valueStart = colon + 1;
            break;
        }

        p = quote2 + 1;
    }

    if (!valueStart) return false;

    while (valueStart < end && *valueStart != '[')
        valueStart++;

    if (valueStart >= end) return false;

    float m[16];
    int count = sscanf_s(valueStart, "[%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f]",
                         &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7],
                         &m[8], &m[9], &m[10], &m[11], &m[12], &m[13], &m[14], &m[15]);
    if (count < 16) return false;

    *outX = m[4];
    *outY = m[5];
    *outZ = m[6];
    return true;
}

// Load texture (same as GameModelLoader::loadDDSTexture)
hkgTexture* EffectLoader::loadTexture(const char* textureName, const char* textureDir)
{
    if (!textureName || textureName[0] == '\0' || !m_context)
        return HK_NULL;

    char path[512];
    char logMsg[512];

    if (strstr(textureName, ".dds"))
        sprintf_s(path, "%s%s", textureDir, textureName);
    else
        sprintf_s(path, "%s%s.dds", textureDir, textureName);

    sprintf_s(logMsg, "[loadTexture] Attempting to load: '%s' from path: '%s'", textureName, path);
    RendererLog(logMsg);

    // Check if file exists
    FILE* testFile = fopen(path, "rb");
    if (testFile)
    {
        fclose(testFile);
        RendererLog("[loadTexture] File exists!");
    }
    else
    {
        sprintf_s(logMsg, "[loadTexture] ERROR: File does NOT exist: '%s'", path);
        RendererLog(logMsg);
        return HK_NULL;
    }

    hkgTexture* texture = hkgTexture::create(m_context);
    if (!texture)
    {
        RendererLog("[loadTexture] ERROR: Failed to create hkgTexture");
        return HK_NULL;
    }

    if (!texture->loadFromFile(path))
    {
        sprintf_s(logMsg, "[loadTexture] ERROR: hkgTexture::loadFromFile() failed for: '%s'", path);
        RendererLog(logMsg);
        texture->removeReference();
        return HK_NULL;
    }

    sprintf_s(logMsg, "[loadTexture] SUCCESS: Loaded texture '%s'", textureName);
    RendererLog(logMsg);

    // Set texture properties (same as GameModelLoader)
    // DDS files already contain mipmaps; avoid rebuilding them for atlases.
    texture->setAutoMipMaps(false);
    texture->setAutoCompression(false);
    texture->setFilterMode((HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));
    // Particle atlases must clamp to avoid UV bleed/wrap artifacts.
    texture->setTextureWrapModeU(HKG_TEXTURE_CLAMP);
    texture->setTextureWrapModeV(HKG_TEXTURE_CLAMP);
    texture->realize(false);
    texture->setFilterMode((HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));

    char msg[256];
    sprintf_s(msg, "[EffectLoader] Loaded texture: %s", path);
    RendererLog(msg);

    return texture;
}

// Parse emitter from JSON object
ParticleEmitter* EffectLoader::parseEmitter(const char* objStart, const char* objEnd, const char* textureDir)
{
    if (!objStart || !objEnd) return HK_NULL;

    ParticleEmitter* emitter = new ParticleEmitter();

    // Extract basic properties
    extractString(objStart, objEnd, "Name", emitter->name, sizeof(emitter->name));

    // Extract diffuse texture.
    // NOTE: Some emitters use RefractionTexture/NormalTexture for special shaders (heat haze).
    // Treating those as a fallback diffuse produces ugly "colored wall" quads.
    char texName[128] = { 0 };
    extractString(objStart, objEnd, "Texture", texName, sizeof(texName));
    if (texName[0] != '\0')
    {
        strncpy_s(emitter->textureName, texName, _TRUNCATE);
        emitter->texture = loadTexture(texName, textureDir);
    }

    // Extract refraction (heat haze) settings.
    extractBool(objStart, objEnd, "Refract", &emitter->refract);
    extractFloat(objStart, objEnd, "RefractionScale", &emitter->refractionScale);

    char refrName[128] = { 0 };
    extractString(objStart, objEnd, "RefractionTexture", refrName, sizeof(refrName));
    if (refrName[0] != '\0')
    {
        strncpy_s(emitter->refractionTextureName, refrName, _TRUNCATE);

        // Most refraction textures live in the shared atlas_2 and won't exist as standalone DDS files.
        // Avoid spamming "file does not exist" logs here; atlas patching will resolve them later.
        char refrPath[512];
        if (strstr(refrName, ".dds"))
            sprintf_s(refrPath, "%s%s", textureDir, refrName);
        else
            sprintf_s(refrPath, "%s%s.dds", textureDir, refrName);

        FILE* testFile = fopen(refrPath, "rb");
        if (testFile)
        {
            fclose(testFile);
            emitter->refractionTexture = loadTexture(refrName, textureDir);
        }
    }

    // Extract texture atlas settings
    char textureUsage[32];
    if (extractString(objStart, objEnd, "TextureUsage", textureUsage, sizeof(textureUsage)))
    {
        emitter->useAtlas = (_stricmp(textureUsage, "animated") == 0 ||
                              _stricmp(textureUsage, "normal") == 0 ||
                              _stricmp(textureUsage, "atlas") == 0);
    }

    extractInt(objStart, objEnd, "frames", &emitter->frameCount);
    extractInt(objStart, objEnd, "MinFrame", &emitter->minFrame);
    extractInt(objStart, objEnd, "MaxFrame", &emitter->maxFrame);
    extractFloat(objStart, objEnd, "PlaybackTime", &emitter->playbackTime);
    extractBool(objStart, objEnd, "LoopTextureAnim", &emitter->loopTextureAnim);
    extractBool(objStart, objEnd, "RandomizeTextureAnimStartFrame", &emitter->randomizeStartFrame);

    // Extract particle type
    char particleType[32];
    if (extractString(objStart, objEnd, "ParticleType", particleType, sizeof(particleType)))
    {
        if (_stricmp(particleType, "billboard") == 0 ||
            _stricmp(particleType, "quad") == 0 ||
            _stricmp(particleType, "sprite") == 0)
            emitter->particleType = ParticleEmitter::BILLBOARD;
        else if (_stricmp(particleType, "mesh") == 0)
            emitter->particleType = ParticleEmitter::MESH;
    }

    // Extract orientation mode (used for streak/rain/beams)
    char orientMode[64];
    if (extractString(objStart, objEnd, "OrientMode", orientMode, sizeof(orientMode)))
    {
        if (_stricmp(orientMode, "orientZToMovementDirection") == 0 ||
            _stricmp(orientMode, "0x5C2DF50F") == 0)
        {
            emitter->orientMode = ParticleEmitter::ORIENT_Z_TO_MOVEMENT_DIRECTION;
        }
        else
        {
            emitter->orientMode = ParticleEmitter::ORIENT_NONE;
        }
    }

    // Extract blend mode
    char blendMode[32];
    if (extractString(objStart, objEnd, "BlendMode", blendMode, sizeof(blendMode)))
    {
        if (_stricmp(blendMode, "additive") == 0)
            emitter->blendMode = ParticleEmitter::ADDITIVE;
        else
            emitter->blendMode = ParticleEmitter::NORMAL;
    }

    // Extract spawn settings
    extractFloat(objStart, objEnd, "RateMin", &emitter->rateMin);
    extractFloat(objStart, objEnd, "RateMax", &emitter->rateMax);
    extractInt(objStart, objEnd, "LifeSpanMin", &emitter->lifespanMin);
    extractInt(objStart, objEnd, "LifeSpanMax", &emitter->lifespanMax);
    extractInt(objStart, objEnd, "FadeInTime", &emitter->fadeInFrames);
    extractFloat(objStart, objEnd, "InitialVelocityContribution", &emitter->initialVelocityContribution);

    // Extract size animation
    extractFloat(objStart, objEnd, "StartWidthMin", &emitter->startWidthMin);
    extractFloat(objStart, objEnd, "StartWidthMax", &emitter->startWidthMax);
    extractFloat(objStart, objEnd, "StartHeightMin", &emitter->startHeightMin);
    extractFloat(objStart, objEnd, "StartHeightMax", &emitter->startHeightMax);
    extractFloat(objStart, objEnd, "EndWidthMin", &emitter->endWidthMin);
    extractFloat(objStart, objEnd, "EndWidthMax", &emitter->endWidthMax);
    extractFloat(objStart, objEnd, "EndHeightMin", &emitter->endHeightMin);
    extractFloat(objStart, objEnd, "EndHeightMax", &emitter->endHeightMax);

    // Extract pivot point (where the particle position lies within the quad)
    extractFloat(objStart, objEnd, "PivotPointXMin", &emitter->pivotXMin);
    extractFloat(objStart, objEnd, "PivotPointXMax", &emitter->pivotXMax);
    extractFloat(objStart, objEnd, "PivotPointYMin", &emitter->pivotYMin);
    extractFloat(objStart, objEnd, "PivotPointYMax", &emitter->pivotYMax);

    // Extract color animation
    extractColor(objStart, objEnd, "StartColorMin", &emitter->startColorMin);
    extractColor(objStart, objEnd, "StartColorMax", &emitter->startColorMax);
    extractColor(objStart, objEnd, "EndColorMin", &emitter->endColorMin);
    extractColor(objStart, objEnd, "EndColorMax", &emitter->endColorMax);
    extractFloat(objStart, objEnd, "StartColorMultiplier", &emitter->startColorMultiplier);
    extractFloat(objStart, objEnd, "EndColorMultiplier", &emitter->endColorMultiplier);

    // Extract alpha test (0..255). Used heavily by smoke/embers to avoid bright full-sheet quads.
    extractInt(objStart, objEnd, "StartAlphaTestMin", &emitter->startAlphaTestMin);
    extractInt(objStart, objEnd, "StartAlphaTestMax", &emitter->startAlphaTestMax);
    extractInt(objStart, objEnd, "EndAlphaTestMin", &emitter->endAlphaTestMin);
    extractInt(objStart, objEnd, "EndAlphaTestMax", &emitter->endAlphaTestMax);

    // Extract rotation
    extractVector3(objStart, objEnd, "InitialOrientationMin", &emitter->initialOrientationMin);
    extractVector3(objStart, objEnd, "InitialOrientationMax", &emitter->initialOrientationMax);
    extractVector3(objStart, objEnd, "StartRotationVelocityMin", &emitter->startRotationVelocityMin);
    extractVector3(objStart, objEnd, "StartRotationVelocityMax", &emitter->startRotationVelocityMax);
    extractVector3(objStart, objEnd, "EndRotationVelocityMin", &emitter->endRotationVelocityMin);
    extractVector3(objStart, objEnd, "EndRotationVelocityMax", &emitter->endRotationVelocityMax);

    // Extract physics
    extractFloat(objStart, objEnd, "SpeedMin", &emitter->speedMin);
    extractFloat(objStart, objEnd, "SpeedMax", &emitter->speedMax);
    extractFloat(objStart, objEnd, "Acceleration", &emitter->acceleration);

    // Extract emitter type
    char emitterType[32];
    if (extractString(objStart, objEnd, "Type", emitterType, sizeof(emitterType)))
    {
        if (_stricmp(emitterType, "sphere") == 0)
            emitter->emitterType = ParticleEmitter::SPHERE;
        else if (_stricmp(emitterType, "dome") == 0)
            emitter->emitterType = ParticleEmitter::DOME;
        else if (_stricmp(emitterType, "cone") == 0)
            emitter->emitterType = ParticleEmitter::CONE;
        else if (_stricmp(emitterType, "circle") == 0)
            emitter->emitterType = ParticleEmitter::CIRCLE;
        else if (_stricmp(emitterType, "cylinder") == 0)
            emitter->emitterType = ParticleEmitter::CYLINDER;
        else if (_stricmp(emitterType, "polygon") == 0)
            emitter->emitterType = ParticleEmitter::POLYGON;
        else if (_stricmp(emitterType, "direction") == 0)
            emitter->emitterType = ParticleEmitter::DIRECTION;
        else if (_stricmp(emitterType, "mesh") == 0)
            emitter->emitterType = ParticleEmitter::MESH_EMITTER;
        else
            emitter->emitterType = ParticleEmitter::POINT;
    }

    extractBool(objStart, objEnd, "Volume", &emitter->volumeEmit);
    extractFloat(objStart, objEnd, "Height", &emitter->height);
    extractFloat(objStart, objEnd, "Angle", &emitter->angle);

    // Extract transform scale from WorldTransform (or Transform as fallback)
    if (!extractTransformScale(objStart, objEnd, "WorldTransform", &emitter->transformScale))
    {
        extractTransformScale(objStart, objEnd, "Transform", &emitter->transformScale);
    }

    // Extract local translation from WorldTransform (or Transform as fallback)
    hkVector4 translation;
    bool hasTranslation = extractTransformTranslation(objStart, objEnd, "WorldTransform", &translation);
    if (!hasTranslation)
        hasTranslation = extractTransformTranslation(objStart, objEnd, "Transform", &translation);

    if (hasTranslation)
    {
        // NOTE: Effect transform translations are already in world units (match camera/model space).
        // Do NOT apply cm->m scaling here or multi-emitter grids (rain, ash) collapse to the origin.
        emitter->localOffset.set(translation(0), translation(1), translation(2), 0.0f);
    }
    else
    {
        emitter->localOffset.setZero4();
    }

    // Extract emitter orientation axis (best-effort). Used for directional emission and velocity bias.
    {
        float ax = 0.0f, ay = 1.0f, az = 0.0f;
        if (!ExtractTransformAxisY(objStart, objEnd, "WorldTransform", &ax, &ay, &az))
            ExtractTransformAxisY(objStart, objEnd, "Transform", &ax, &ay, &az);

        float len = ZLength3f(ax, ay, az);
        if (len > 0.0001f)
        {
            ZNormalize3f(ax, ay, az);
        }
        emitter->axisY.set(ax, ay, az, 0.0f);
    }

    {
        char msg[256];
        sprintf_s(msg, "[EffectLoader] Emitter '%s' transformScale=%.4f offset=(%.3f,%.3f,%.3f)",
                  emitter->name, emitter->transformScale,
                  emitter->localOffset(0), emitter->localOffset(1), emitter->localOffset(2));
        RendererLog(msg);
    }

    return emitter;
}

// Parse effect light from JSON object
EffectLight* EffectLoader::parseEffectLight(const char* objStart, const char* objEnd)
{
    if (!objStart || !objEnd) return HK_NULL;

    EffectLight* light = new EffectLight();

    extractColor(objStart, objEnd, "Color", &light->color);
    extractFloat(objStart, objEnd, "ColorScale", &light->colorScale);
    extractFloat(objStart, objEnd, "Radius", &light->radius);
    extractFloat(objStart, objEnd, "InnerRadius", &light->innerRadius);

    return light;
}

// Load effect from JSON file
EffectDefinition* EffectLoader::loadEffect(const char* jsonPath, const char* textureDir)
{
    if (!jsonPath || !textureDir)
    {
        RendererLog("[EffectLoader] Invalid parameters");
        return HK_NULL;
    }

    int jsonSize = 0;
    char* json = LoadTextFile(jsonPath, &jsonSize);
    if (!json)
    {
        char msg[256];
        sprintf_s(msg, "[EffectLoader] Failed to load: %s", jsonPath);
        RendererLog(msg);
        return HK_NULL;
    }

    EffectDefinition* effect = new EffectDefinition();

    struct ControllerRateEntry
    {
        int guid;
        int start;
        int end;
        int fadeIn;
        int fadeOut;
        float rate;
    };

    struct ControllerForceEntry
    {
        int guid;
        int start;
        int end;
        int fadeIn;
        int fadeOut;
        float force;
    };

    struct EmitterControllersEntry
    {
        ParticleEmitter* emitter;
        int controllerGuids[32];
        int controllerCount;
        int effectorGuids[32];
        int effectorCount;
        int emitterEffectorGuids[32];
        int emitterEffectorCount;
    };

    std::vector<ControllerRateEntry> controllerRates;
    std::vector<ControllerForceEntry> controllerForces;
    std::vector<EmitterControllersEntry> emitterControllers;

    // Parse "objs" array
    const char* objsKey = findKey(json, json + jsonSize, "objs");
    if (!objsKey)
    {
        free(json);
        delete effect;
        return HK_NULL;
    }

    const char* objsArray = strchr(objsKey, '[');
    const char* objsEnd = objsArray ? findMatchingBrace(objsArray, '[', ']') : NULL;

    if (!objsArray || !objsEnd)
    {
        free(json);
        delete effect;
        return HK_NULL;
    }

    // Parse each object in the array
    const char* p = objsArray + 1;
    int objCount = 0;
    while (p < objsEnd)
    {
        // Find next object
        const char* objStart = strchr(p, '{');
        if (!objStart || objStart >= objsEnd) break;

        const char* objEnd = findMatchingBrace(objStart, '{', '}');
        if (!objEnd || objEnd >= objsEnd) break;

        objCount++;

        // Extract object type
        char objType[64];
        if (extractString(objStart, objEnd, "type", objType, sizeof(objType)))
        {
            char msg[256];
            sprintf_s(msg, "[EffectLoader] Found object type: %s", objType);
            RendererLog(msg);

            // Cache common controller data (GUID -> Rate) for later emitter patching.
            // Many emitters author RateMin/RateMax=0 and rely on a controller object that provides "Rate".
            int guid = 0;
            float controllerRate = 0.0f;
            if (extractInt(objStart, objEnd, "GUID", &guid) && guid != 0 &&
                extractFloat(objStart, objEnd, "Rate", &controllerRate))
            {
                ControllerRateEntry e;
                e.guid = guid;
                e.start = 0;
                e.end = 0;
                e.fadeIn = 0;
                e.fadeOut = 0;
                extractInt(objStart, objEnd, "Start", &e.start);
                extractInt(objStart, objEnd, "end", &e.end);
                extractInt(objStart, objEnd, "FadeIn", &e.fadeIn);
                extractInt(objStart, objEnd, "FadeOut", &e.fadeOut);
                e.rate = controllerRate;
                controllerRates.push_back(e);
            }

            // Cache effector force controllers (GUID -> force), used by Effectors/EmitterEffectors.
            // These controller objects look like:
            //   Start/FadeIn/FadeOut/end/track + "force"
            int forceGuid = 0;
            int track = 0;
            float controllerForce = 0.0f;
            if (extractInt(objStart, objEnd, "GUID", &forceGuid) && forceGuid != 0 &&
                extractInt(objStart, objEnd, "track", &track) &&
                extractFloat(objStart, objEnd, "force", &controllerForce))
            {
                ControllerForceEntry e;
                e.guid = forceGuid;
                e.start = 0;
                e.end = 0;
                e.fadeIn = 0;
                e.fadeOut = 0;
                extractInt(objStart, objEnd, "Start", &e.start);
                extractInt(objStart, objEnd, "end", &e.end);
                extractInt(objStart, objEnd, "FadeIn", &e.fadeIn);
                extractInt(objStart, objEnd, "FadeOut", &e.fadeOut);
                e.force = controllerForce;
                controllerForces.push_back(e);
            }

            if (strcmp(objType, "templateEffect") == 0)
            {
                // Parse root effect properties
                extractString(objStart, objEnd, "Name", effect->name, sizeof(effect->name));
                extractInt(objStart, objEnd, "Duration", &effect->duration);
                extractBool(objStart, objEnd, "Looping", &effect->looping);
                extractBool(objStart, objEnd, "WorldSpace", &effect->worldSpace);
                extractBool(objStart, objEnd, "SortParticles", &effect->sortParticles);

                sprintf_s(msg, "[EffectLoader] templateEffect: name=%s, duration=%d, looping=%d",
                          effect->name, effect->duration, effect->looping);
                RendererLog(msg);
            }
            else if (strcmp(objType, "Emitter") == 0)
            {
                // Collect controller GUID list before parsing emitter (so we can patch rates later).
                int controllerGuids[32];
                int controllerCount = ExtractIntArray(objStart, objEnd, "Controllers", controllerGuids, 32);
                int effectorGuids[32];
                int effectorCount = ExtractIntArray(objStart, objEnd, "Effectors", effectorGuids, 32);
                int emitterEffectorGuids[32];
                int emitterEffectorCount = ExtractIntArray(objStart, objEnd, "EmitterEffectors", emitterEffectorGuids, 32);

                // Parse emitter
                ParticleEmitter* emitter = parseEmitter(objStart, objEnd, textureDir);
                if (emitter)
                {
                    effect->emitters.pushBack(emitter);

                    EmitterControllersEntry ec;
                    ec.emitter = emitter;
                    ec.controllerCount = controllerCount;
                    if (ec.controllerCount > 32) ec.controllerCount = 32;
                    for (int k = 0; k < ec.controllerCount; k++)
                        ec.controllerGuids[k] = controllerGuids[k];

                    ec.effectorCount = effectorCount;
                    if (ec.effectorCount > 32) ec.effectorCount = 32;
                    for (int k = 0; k < ec.effectorCount; k++)
                        ec.effectorGuids[k] = effectorGuids[k];

                    ec.emitterEffectorCount = emitterEffectorCount;
                    if (ec.emitterEffectorCount > 32) ec.emitterEffectorCount = 32;
                    for (int k = 0; k < ec.emitterEffectorCount; k++)
                        ec.emitterEffectorGuids[k] = emitterEffectorGuids[k];

                    emitterControllers.push_back(ec);

                    sprintf_s(msg, "[EffectLoader] Added emitter: %s (rate=%.1f-%.1f, life=%d-%d)",
                              emitter->name, emitter->rateMin, emitter->rateMax,
                              emitter->lifespanMin, emitter->lifespanMax);
                    RendererLog(msg);
                }
                else
                {
                    RendererLog("[EffectLoader] Failed to parse emitter");
                }
            }
            else if (strcmp(objType, "EffectLight") == 0)
            {
                // Parse effect light
                EffectLight* light = parseEffectLight(objStart, objEnd);
                if (light)
                {
                    effect->lights.pushBack(light);
                    sprintf_s(msg, "[EffectLoader] Added light: radius=%.2f, scale=%.2f",
                              light->radius, light->colorScale);
                    RendererLog(msg);
                }
            }
            else if (strcmp(objType, "Effector") == 0 || strcmp(objType, "EmitterEffector") == 0)
            {
                int guid = 0;
                if (extractInt(objStart, objEnd, "GUID", &guid) && guid != 0)
                {
                    EffectEffector* ef = new EffectEffector();
                    ef->guid = guid;
                    ef->isEmitterEffector = (strcmp(objType, "EmitterEffector") == 0);

                    char typeStr[64];
                    if (extractString(objStart, objEnd, "Type", typeStr, sizeof(typeStr)))
                        ef->typeId = ParseTypeIdString(typeStr);

                    extractBool(objStart, objEnd, "Containing", &ef->containing);
                    extractFloat(objStart, objEnd, "force", &ef->force);
                    extractFloat(objStart, objEnd, "Damping", &ef->damping);
                    extractFloat(objStart, objEnd, "Bounce", &ef->bounce);
                    extractFloat(objStart, objEnd, "Height", &ef->height);
                    extractFloat(objStart, objEnd, "Constrict", &ef->constrict);

                    int controllerGuids[16];
                    int controllerCount = ExtractIntArray(objStart, objEnd, "Controllers", controllerGuids, 16);
                    ef->controllerCount = controllerCount;
                    if (ef->controllerCount > 16) ef->controllerCount = 16;
                    for (int k = 0; k < ef->controllerCount; ++k)
                        ef->controllerGuids[k] = controllerGuids[k];

                    hkVector4 pos;
                    pos.setZero4();
                    if (!extractTransformTranslation(objStart, objEnd, "WorldTransform", &pos))
                        extractTransformTranslation(objStart, objEnd, "Transform", &pos);
                    ef->position = pos;

                    float scale = 1.0f;
                    if (!extractTransformScale(objStart, objEnd, "WorldTransform", &scale))
                        extractTransformScale(objStart, objEnd, "Transform", &scale);
                    ef->transformScale = scale;

                    float ax = 0.0f, ay = 0.0f, az = 0.0f;
                    if (!ExtractTransformAxisY(objStart, objEnd, "WorldTransform", &ax, &ay, &az))
                        ExtractTransformAxisY(objStart, objEnd, "Transform", &ax, &ay, &az);
                    float len = ZLength3f(ax, ay, az);
                    if (len > 0.0001f)
                    {
                        ZNormalize3f(ax, ay, az);
                    }
                    else
                    {
                        ax = 0.0f; ay = 1.0f; az = 0.0f;
                    }
                    ef->axisY.set(ax, ay, az, 0.0f);

                    effect->effectors.pushBack(ef);
                }
            }
        }
        else
        {
            char msg[256];
            sprintf_s(msg, "[EffectLoader] Object %d has no type field", objCount);
            RendererLog(msg);
        }

        p = objEnd + 1;
    }

    // Attach controller-driven spawn rate segments to emitters.
    for (int i = 0; i < effect->emitters.getSize(); ++i)
    {
        if (effect->emitters[i])
            effect->emitters[i]->sortParticles = effect->sortParticles;
    }

    for (size_t i = 0; i < emitterControllers.size(); i++)
    {
        ParticleEmitter* emitter = emitterControllers[i].emitter;
        if (!emitter)
            continue;

        int added = 0;
        for (int j = 0; j < emitterControllers[i].controllerCount; j++)
        {
            int cg = emitterControllers[i].controllerGuids[j];
            for (size_t c = 0; c < controllerRates.size(); c++)
            {
                if (controllerRates[c].guid == cg)
                {
                    RateSegment seg;
                    seg.startFrame = controllerRates[c].start;
                    seg.endFrame = controllerRates[c].end;
                    seg.fadeInFrames = controllerRates[c].fadeIn;
                    seg.fadeOutFrames = controllerRates[c].fadeOut;
                    seg.rate = controllerRates[c].rate;
                    emitter->rateSegments.pushBack(seg);
                    added++;
                }
            }
        }

        if (added > 0)
        {
            char msg[256];
            sprintf_s(msg, "[EffectLoader] Attached %d controller rate segments to emitter '%s'",
                      added, emitter->name);
            RendererLog(msg);
        }
    }

    // Attach controller-driven force segments to effectors.
    for (int i = 0; i < effect->effectors.getSize(); ++i)
    {
        EffectEffector* ef = effect->effectors[i];
        if (!ef)
            continue;

        ef->forceSegmentCount = 0;
        for (int j = 0; j < ef->controllerCount; ++j)
        {
            int cg = ef->controllerGuids[j];
            for (size_t c = 0; c < controllerForces.size(); ++c)
            {
                if (controllerForces[c].guid != cg)
                    continue;

                if (ef->forceSegmentCount >= 16)
                    break;

                FloatSegment seg;
                seg.startFrame = controllerForces[c].start;
                seg.endFrame = controllerForces[c].end;
                seg.fadeInFrames = controllerForces[c].fadeIn;
                seg.fadeOutFrames = controllerForces[c].fadeOut;
                seg.value = controllerForces[c].force;
                ef->forceSegments[ef->forceSegmentCount++] = seg;
            }
        }
    }

    // Attach effector definitions (Effectors / EmitterEffectors) to emitters.
    for (size_t i = 0; i < emitterControllers.size(); i++)
    {
        ParticleEmitter* emitter = emitterControllers[i].emitter;
        if (!emitter)
            continue;

        emitter->effectors.clear();
        emitter->emitterEffectors.clear();

        for (int j = 0; j < emitterControllers[i].effectorCount; ++j)
        {
            EffectEffector* ef = FindEffectorByGuid(effect->effectors, emitterControllers[i].effectorGuids[j]);
            if (ef)
                emitter->effectors.pushBack(ef);
        }
        for (int j = 0; j < emitterControllers[i].emitterEffectorCount; ++j)
        {
            EffectEffector* ef = FindEffectorByGuid(effect->effectors, emitterControllers[i].emitterEffectorGuids[j]);
            if (ef)
                emitter->emitterEffectors.pushBack(ef);
        }

        // Legacy approximation: treat all attached effectors as uniform directional acceleration.
        float accX = 0.0f, accY = 0.0f, accZ = 0.0f;
        float damp = 0.0f;
        const float kForceScale = 0.01f; // authored force is typically cm/s^2

        for (int j = 0; j < emitter->effectors.getSize(); ++j)
        {
            EffectEffector* ef = emitter->effectors[j];
            if (!ef) continue;
            float f = ef->force * kForceScale;
            accX += ef->axisY(0) * f;
            accY += ef->axisY(1) * f;
            accZ += ef->axisY(2) * f;
            if (ef->damping > damp) damp = ef->damping;
        }
        for (int j = 0; j < emitter->emitterEffectors.getSize(); ++j)
        {
            EffectEffector* ef = emitter->emitterEffectors[j];
            if (!ef) continue;
            float f = ef->force * kForceScale;
            accX += ef->axisY(0) * f;
            accY += ef->axisY(1) * f;
            accZ += ef->axisY(2) * f;
            if (ef->damping > damp) damp = ef->damping;
        }

        if ((accX*accX + accY*accY + accZ*accZ) > 0.000001f)
            emitter->externalAcceleration.set(accX, accY, accZ, 0.0f);
        else
            emitter->externalAcceleration.setZero4();

        emitter->externalDamping = damp;
    }

    {
        char msg[256];
        sprintf_s(msg, "[EffectLoader] Parsed %d objects from objs array", objCount);
        RendererLog(msg);
    }

    free(json);

    {
        char msg[256];
        sprintf_s(msg, "[EffectLoader] Loaded effect: %s (emitters=%d, lights=%d)",
                  jsonPath, effect->emitters.getSize(), effect->lights.getSize());
        RendererLog(msg);
    }

    return effect;
}
