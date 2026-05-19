// Scene3DEffectManager.cpp — The Particle Reaper. Birth, Render, Death.
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Manages the lifecycle of all active effects. SpawnEffect() creates,
// Update() ticks every emitter and ages every particle, Render() draws
// the survivors, and the dead get compacted out of the array. At peak
// combat in Conquest there could be 50+ simultaneous effects. This
// manager handles all of them without breaking a sweat. The particles
// might die but the manager lives forever. Unlike Pandemic.
// -----------------------------------------------------------------------

#include "Scene3DEffectManager.h"
#include "Scene3DRenderer.h"
#include "Scene3DRendererInternal.h"
#include <Graphics/Common/Texture/hkgTexture.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#ifdef _MSC_VER
#include <excpt.h>
#endif

static bool g_fxDisabledAfterException = false;

AtlasSet::AtlasSet()
    : atlasTexture(HK_NULL)
    , atlasIndex(1)
    , uvAttempted(false)
    , uvLoaded(false)
    , textureAttempted(false)
{
    levelName[0] = '\0';
}

// ActiveEffect implementation
ActiveEffect::ActiveEffect()
{
    definition = HK_NULL;
    emitters.clear();
    lights.clear();
    position.setZero4();
    rotation.setZero4();
    lastPosition.setZero4();
    hasLastPosition = false;
    time = 0.0f;
    finished = false;
    boneIndex = -1;
    debugFramesLeft = 0;
}

// Case-insensitive string comparison helper
static int stricmp_local(const char* a, const char* b)
{
    while (*a && *b)
    {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static ParticleEmitter* CloneEmitterTemplate(const ParticleEmitter* src)
{
    if (!src)
        return HK_NULL;

    ParticleEmitter* dst = new ParticleEmitter();

    strncpy_s(dst->name, src->name, _TRUNCATE);
    strncpy_s(dst->textureName, src->textureName, _TRUNCATE);

    dst->texture = src->texture;
    dst->usesSharedAtlas = src->usesSharedAtlas;
    dst->atlasUVRect = src->atlasUVRect;

    dst->refract = src->refract;
    dst->refractionScale = src->refractionScale;
    strncpy_s(dst->refractionTextureName, src->refractionTextureName, _TRUNCATE);
    dst->refractionTexture = src->refractionTexture;
    dst->usesSharedAtlasRefraction = src->usesSharedAtlasRefraction;
    dst->refractionAtlasUVRect = src->refractionAtlasUVRect;

    dst->useAtlas = src->useAtlas;
    dst->frameCount = src->frameCount;
    dst->minFrame = src->minFrame;
    dst->maxFrame = src->maxFrame;
    dst->playbackTime = src->playbackTime;
    dst->loopTextureAnim = src->loopTextureAnim;
    dst->randomizeStartFrame = src->randomizeStartFrame;

    dst->uvColumns = src->uvColumns;
    dst->uvRows = src->uvRows;
    dst->animFrameMin = src->animFrameMin;
    dst->animFrameMax = src->animFrameMax;

    dst->particleType = src->particleType;
    dst->orientMode = src->orientMode;
    dst->blendMode = src->blendMode;

    dst->rateMin = src->rateMin;
    dst->rateMax = src->rateMax;
    dst->lifespanMin = src->lifespanMin;
    dst->lifespanMax = src->lifespanMax;
    dst->fadeInFrames = src->fadeInFrames;
    dst->initialVelocityContribution = src->initialVelocityContribution;
    dst->sortParticles = src->sortParticles;

    dst->rateSegments.clear();
    for (int i = 0; i < src->rateSegments.getSize(); ++i)
        dst->rateSegments.pushBack(src->rateSegments[i]);

    dst->startWidthMin = src->startWidthMin;
    dst->startWidthMax = src->startWidthMax;
    dst->startHeightMin = src->startHeightMin;
    dst->startHeightMax = src->startHeightMax;
    dst->endWidthMin = src->endWidthMin;
    dst->endWidthMax = src->endWidthMax;
    dst->endHeightMin = src->endHeightMin;
    dst->endHeightMax = src->endHeightMax;

    dst->pivotXMin = src->pivotXMin;
    dst->pivotXMax = src->pivotXMax;
    dst->pivotYMin = src->pivotYMin;
    dst->pivotYMax = src->pivotYMax;

    dst->startColorMin = src->startColorMin;
    dst->startColorMax = src->startColorMax;
    dst->endColorMin = src->endColorMin;
    dst->endColorMax = src->endColorMax;
    dst->startColorMultiplier = src->startColorMultiplier;
    dst->endColorMultiplier = src->endColorMultiplier;

    dst->startAlphaTestMin = src->startAlphaTestMin;
    dst->startAlphaTestMax = src->startAlphaTestMax;
    dst->endAlphaTestMin = src->endAlphaTestMin;
    dst->endAlphaTestMax = src->endAlphaTestMax;

    dst->initialOrientationMin = src->initialOrientationMin;
    dst->initialOrientationMax = src->initialOrientationMax;
    dst->startRotationVelocityMin = src->startRotationVelocityMin;
    dst->startRotationVelocityMax = src->startRotationVelocityMax;
    dst->endRotationVelocityMin = src->endRotationVelocityMin;
    dst->endRotationVelocityMax = src->endRotationVelocityMax;

    dst->speedMin = src->speedMin;
    dst->speedMax = src->speedMax;
    dst->acceleration = src->acceleration;
    dst->externalAcceleration = src->externalAcceleration;
    dst->externalDamping = src->externalDamping;

    dst->effectors.clear();
    for (int i = 0; i < src->effectors.getSize(); ++i)
        dst->effectors.pushBack(src->effectors[i]);

    dst->emitterEffectors.clear();
    for (int i = 0; i < src->emitterEffectors.getSize(); ++i)
        dst->emitterEffectors.pushBack(src->emitterEffectors[i]);

    dst->emitterType = src->emitterType;
    dst->volumeEmit = src->volumeEmit;
    dst->height = src->height;
    dst->angle = src->angle;
    dst->axisY = src->axisY;

    dst->rotation = src->rotation;
    dst->localOffset = src->localOffset;
    dst->transformScale = src->transformScale;

    // Runtime state should never be inherited from the template.
    dst->position.setZero4();
    dst->particles.clear();
    dst->spawnAccumulator = 0.0f;

    // Share the texture object; bump refcount when this emitter owns it.
    if (dst->texture && !dst->usesSharedAtlas)
        dst->texture->addReference();
    if (dst->refractionTexture && !dst->usesSharedAtlasRefraction && dst->refractionTexture != dst->texture)
        dst->refractionTexture->addReference();

    return dst;
}

static EffectLight* CloneLightTemplate(const EffectLight* src)
{
    if (!src)
        return HK_NULL;

    EffectLight* dst = new EffectLight();
    dst->position = src->position;
    dst->color = src->color;
    dst->colorScale = src->colorScale;
    dst->radius = src->radius;
    dst->innerRadius = src->innerRadius;
    return dst;
}

static void DestroyActiveEffect(ActiveEffect* e)
{
    if (!e)
        return;

    for (int i = 0; i < e->emitters.getSize(); ++i)
        delete e->emitters[i];
    e->emitters.clear();

    for (int i = 0; i < e->lights.getSize(); ++i)
        delete e->lights[i];
    e->lights.clear();

    delete e;
}

// Load text file helper
static char* LoadTextFileEM(const char* path, int* outSize)
{
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    int size = (int)ftell(file);
    fseek(file, 0, SEEK_SET);
    char* buffer = (char*)malloc(size + 1);
    if (!buffer) { fclose(file); return NULL; }
    int bytesRead = (int)fread(buffer, 1, size, file);
    buffer[bytesRead] = '\0';
    fclose(file);
    if (outSize) *outSize = bytesRead;
    return buffer;
}

// EffectManager implementation
EffectManager::EffectManager()
    : m_context(HK_NULL)
{
}

EffectManager::~EffectManager()
{
    clearAll();

    // Release all loaded definitions
    for (int i = 0; i < m_definitions.getSize(); i++)
    {
        delete m_definitions[i];
    }
    m_definitions.clear();

    // Release atlas sets (textures + UV tables)
    for (int i = 0; i < m_atlasSets.getSize(); i++)
    {
        AtlasSet* set = m_atlasSets[i];
        if (!set) continue;
        if (set->atlasTexture)
        {
            set->atlasTexture->removeReference();
            set->atlasTexture = HK_NULL;
        }
        delete set;
    }
    m_atlasSets.clear();
}

void EffectManager::initialize(hkgDisplayContext* context)
{
    m_context = context;
    m_loader.setDisplayContext(context);
}

void EffectManager::parseAtlasJSON(const char* jsonPath, hkArray<AtlasUVEntry>& outEntries)
{
    outEntries.clear();

    int jsonSize = 0;
    char* json = LoadTextFileEM(jsonPath, &jsonSize);
    if (!json)
    {
        char msg[256];
        sprintf_s(msg, "[EffectManager] Failed to load atlas JSON: %s", jsonPath);
        RendererLog(msg);
        return;
    }

    // Parse JSON array of {key, vals: {x, y, z, w}}
    // Simple manual parsing - find each "key" : "name" and "vals" : {x, y, z, w}
    const char* p = json;
    const char* end = json + jsonSize;

    while (p < end)
    {
        // Find next "key"
        const char* keyStr = strstr(p, "\"key\"");
        if (!keyStr || keyStr >= end) break;

        // Extract the key value (texture name)
        // "key" is 5 chars ("key"), skip past closing quote with +5
        const char* nameStart = strchr(keyStr + 5, '\"');
        if (!nameStart || nameStart >= end) break;
        nameStart++; // skip opening quote
        const char* nameEnd = strchr(nameStart, '\"');
        if (!nameEnd || nameEnd >= end) break;

        // Find "vals" after this key
        const char* valsStr = strstr(nameEnd, "\"vals\"");
        if (!valsStr || valsStr >= end) break;

        // Find the opening brace of vals
        const char* braceStart = strchr(valsStr, '{');
        if (!braceStart || braceStart >= end) break;
        const char* braceEnd = strchr(braceStart, '}');
        if (!braceEnd || braceEnd >= end) break;

        // Extract x, y, z, w from the vals object
        float x = 0, y = 0, z = 0, w = 0;
        const char* xStr = strstr(braceStart, "\"x\"");
        const char* yStr = strstr(braceStart, "\"y\"");
        const char* zStr = strstr(braceStart, "\"z\"");
        const char* wStr = strstr(braceStart, "\"w\"");

        if (xStr && xStr < braceEnd) { const char* colon = strchr(xStr, ':'); if (colon) x = (float)atof(colon + 1); }
        if (yStr && yStr < braceEnd) { const char* colon = strchr(yStr, ':'); if (colon) y = (float)atof(colon + 1); }
        if (zStr && zStr < braceEnd) { const char* colon = strchr(zStr, ':'); if (colon) z = (float)atof(colon + 1); }
        if (wStr && wStr < braceEnd) { const char* colon = strchr(wStr, ':'); if (colon) w = (float)atof(colon + 1); }

        // Create atlas entry
        AtlasUVEntry entry;
        int nameLen = (int)(nameEnd - nameStart);
        if (nameLen >= (int)sizeof(entry.name)) nameLen = sizeof(entry.name) - 1;
        memcpy(entry.name, nameStart, nameLen);
        entry.name[nameLen] = '\0';
        entry.rect = AtlasUVRect(x, y, z, w);

        outEntries.pushBack(entry);

        p = braceEnd + 1;
    }

    free(json);

    char msg[256];
    sprintf_s(msg, "[EffectManager] Parsed %d atlas UV entries from %s", outEntries.getSize(), jsonPath);
    RendererLog(msg);
}

// Normalize textureDir (..\GameFiles\textures\) -> gameFilesDir (..\GameFiles\)
void EffectManager::buildGameFilesDirFromTextureDir(const char* textureDir, char* outDir, int outDirSize)
{
    if (!outDir || outDirSize <= 0)
        return;
    outDir[0] = '\0';
    if (!textureDir || textureDir[0] == '\0')
        return;

    strncpy_s(outDir, outDirSize, textureDir, _TRUNCATE);

    // Normalize slashes to backslashes
    for (char* p = outDir; *p; ++p)
    {
        if (*p == '/') *p = '\\';
    }

    // Strip trailing slashes
    int len = (int)strlen(outDir);
    while (len > 0 && (outDir[len - 1] == '\\' || outDir[len - 1] == '/'))
    {
        outDir[len - 1] = '\0';
        len--;
    }

    // If the last path segment is "textures", strip it
    char* lastSlash = strrchr(outDir, '\\');
    if (lastSlash && _stricmp(lastSlash + 1, "textures") == 0)
    {
        *(lastSlash + 1) = '\0'; // keep trailing slash
        return;
    }

    // Otherwise, re-append a trailing slash
    len = (int)strlen(outDir);
    if (len > 0 && len < outDirSize - 1 && outDir[len - 1] != '\\')
    {
        outDir[len] = '\\';
        outDir[len + 1] = '\0';
    }
}

static bool startsWithCI(const char* s, const char* prefix)
{
    if (!s || !prefix) return false;
    size_t n = strlen(prefix);
    return _strnicmp(s, prefix, n) == 0;
}

const char* EffectManager::detectPreferredAtlasLevelFromEffectName(const char* effectName)
{
    if (!effectName) return HK_NULL;

    if (startsWithCI(effectName, "FX_HMD_"))  return "Helm'sDeep";
    if (startsWithCI(effectName, "FX_IGD_"))  return "Isengard";
    if (startsWithCI(effectName, "FX_MOR_"))  return "Moria";
    if (startsWithCI(effectName, "FX_DOM_"))  return "Mount_Doom";
    if (startsWithCI(effectName, "FX_MGL_"))  return "Minas_Morgul";
    if (startsWithCI(effectName, "FX_MSTT_")) return "MinasTirith_Top";
    if (startsWithCI(effectName, "FX_MST_"))  return "MinasTirith";
    if (startsWithCI(effectName, "FX_OSG_"))  return "Osgiliath";
    if (startsWithCI(effectName, "FX_PLN_"))  return "PelennorFields";
    if (startsWithCI(effectName, "FX_RVD_"))  return "Rivendell";
    if (startsWithCI(effectName, "FX_SHR_"))  return "Shire";
    if (startsWithCI(effectName, "FX_TRN_"))  return "Training";
    if (startsWithCI(effectName, "FX_WTP_"))  return "Weathertop";
    if (startsWithCI(effectName, "FX_BKG_"))  return "BlackGates";

    return HK_NULL;
}

AtlasSet* EffectManager::getOrCreateAtlasSet(const char* levelName, int atlasIndex)
{
    if (!levelName || levelName[0] == '\0')
        return HK_NULL;

    if (atlasIndex != 2)
        atlasIndex = 1;

    for (int i = 0; i < m_atlasSets.getSize(); i++)
    {
        AtlasSet* set = m_atlasSets[i];
        if (set && set->atlasIndex == atlasIndex && _stricmp(set->levelName, levelName) == 0)
            return set;
    }

    AtlasSet* set = new AtlasSet();
    strncpy_s(set->levelName, levelName, _TRUNCATE);
    set->atlasIndex = atlasIndex;
    m_atlasSets.pushBack(set);
    return set;
}

static bool fileExistsLocal(const char* path)
{
    if (!path || path[0] == '\0')
        return false;

    FILE* f = fopen(path, "rb");
    if (f)
    {
        fclose(f);
        return true;
    }
    return false;
}

static bool uvLooksHalfTexel(float u0, float u1, unsigned int texSize)
{
    if (texSize == 0)
        return false;

    float a = u0 * (float)texSize;
    float b = u1 * (float)texSize;

    float fracA = fabsf((a - floorf(a)) - 0.5f);
    float fracB = fabsf((b - floorf(b)) - 0.5f);

    // Atlas UV tables from the game are stored with half-texel inset (pixel-center bounds).
    // Default 0..1 rects and some standalone textures use edge UVs instead.
    return (fracA < 0.01f && fracB < 0.01f);
}

static void computeEmitterFlipbookLayout(ParticleEmitter* emitter)
{
    if (!emitter)
        return;

    // Choose which texture drives flipbook layout:
    // - Most emitters animate their diffuse/mask texture.
    // - Refraction-only ("heat haze") emitters have empty Texture and animate RefractionTexture instead.
    hkgTexture* layoutTex = emitter->texture ? emitter->texture : emitter->refractionTexture;
    const AtlasUVRect& layoutRect = emitter->texture ? emitter->atlasUVRect : emitter->refractionAtlasUVRect;

    // Default: single frame
    emitter->uvColumns = 1;
    emitter->uvRows = 1;
    emitter->animFrameMin = 0;
    emitter->animFrameMax = 0;

    if (!layoutTex)
        return;

    const int jsonCols = emitter->frameCount;
    if (jsonCols <= 1)
        return;

    // If the texture animation does not loop and does not randomize a start frame,
    // treat it as a static sprite (ignore flipbook slicing).
    if (!emitter->loopTextureAnim && !emitter->randomizeStartFrame)
        return;

    const unsigned int texW = layoutTex->getWidth();
    const unsigned int texH = layoutTex->getHeight();
    if (texW == 0 || texH == 0)
        return;

    float u0 = layoutRect.u0;
    float v0 = layoutRect.v0;
    float u1 = layoutRect.u1;
    float v1 = layoutRect.v1;

    bool halfU = uvLooksHalfTexel(u0, u1, texW);
    bool halfV = uvLooksHalfTexel(v0, v1, texH);

    float x0 = halfU ? (u0 * (float)texW - 0.5f) : (u0 * (float)texW);
    float x1 = halfU ? (u1 * (float)texW + 0.5f) : (u1 * (float)texW);
    float y0 = halfV ? (v0 * (float)texH - 0.5f) : (v0 * (float)texH);
    float y1 = halfV ? (v1 * (float)texH + 0.5f) : (v1 * (float)texH);

    float rectW = x1 - x0;
    float rectH = y1 - y0;
    if (rectW <= 1.0f || rectH <= 1.0f)
        return;

    int cols = jsonCols;
    if (cols < 1) cols = 1;

    int minF = emitter->minFrame;
    int maxF = emitter->maxFrame;
    if (minF < 0) minF = 0;
    if (maxF < minF) maxF = minF;

    // Some exported effects use 1-based frame indices (min=1) for single-row strips.
    if (minF == 1)
    {
        if (maxF <= cols || (cols > 0 && (maxF % cols) == 0))
        {
            minF -= 1;
            maxF -= 1;
            if (minF < 0) minF = 0;
            if (maxF < 0) maxF = 0;
        }
    }

    // Assume frame indices are row-major within a grid of 'cols' columns.
    int rows = (maxF / cols) + 1;
    if (rows < 1) rows = 1;

    float frameW = rectW / (float)cols;
    float frameH = rectH / (float)rows;

    // If the inferred grid would create sub-2px tiles, fall back to a 1-row strip.
    const float kMinTilePx = 2.0f;
    if (frameW < kMinTilePx || frameH < kMinTilePx)
    {
        rows = 1;

        if (maxF >= cols) maxF = cols - 1;
        if (minF >= cols) minF = 0;
        if (maxF < minF) { minF = 0; maxF = cols - 1; }

        frameW = rectW / (float)cols;
        frameH = rectH;

        // Still too small? Treat as a single sprite.
        if (frameW < kMinTilePx || frameH < kMinTilePx)
        {
            cols = 1;
            rows = 1;
            minF = 0;
            maxF = 0;
        }
    }

    // Clamp animation range to the flipbook capacity.
    int capacity = cols * rows;
    if (capacity < 1) capacity = 1;
    if (maxF >= capacity) maxF = capacity - 1;
    if (minF >= capacity) minF = 0;
    if (maxF < minF) { minF = 0; maxF = capacity - 1; }

    emitter->uvColumns = cols;
    emitter->uvRows = rows;
    emitter->animFrameMin = minF;
    emitter->animFrameMax = maxF;
}

bool EffectManager::ensureAtlasUVLoaded(AtlasSet* set, const char* gameFilesDir)
{
    if (!set || !gameFilesDir || gameFilesDir[0] == '\0')
        return false;

    if (set->uvAttempted)
        return set->uvLoaded;

    set->uvAttempted = true;

    char jsonPath[512];
    sprintf_s(jsonPath, "%ssrcjson\\%s\\sub_blocks1\\atlas_%d.json", gameFilesDir, set->levelName, set->atlasIndex);

    parseAtlasJSON(jsonPath, set->entries);
    set->uvLoaded = (set->entries.getSize() > 0);
    return set->uvLoaded;
}

bool EffectManager::ensureAtlasTextureLoaded(AtlasSet* set, const char* gameFilesDir)
{
    if (!set || !gameFilesDir || gameFilesDir[0] == '\0' || !m_context)
        return false;

    if (set->textureAttempted)
        return (set->atlasTexture != HK_NULL);

    set->textureAttempted = true;

    char ddsPath[512];
    sprintf_s(ddsPath, "%sblocks\\%s\\atlas_%d.dds", gameFilesDir, set->levelName, set->atlasIndex);

    if (!fileExistsLocal(ddsPath))
    {
        char msg[256];
        sprintf_s(msg, "[EffectManager] WARNING: atlas texture missing: %s", ddsPath);
        RendererLog(msg);
        return false;
    }

    set->atlasTexture = hkgTexture::create(m_context);
    if (!set->atlasTexture || !set->atlasTexture->loadFromFile(ddsPath))
    {
        if (set->atlasTexture)
        {
            set->atlasTexture->removeReference();
            set->atlasTexture = HK_NULL;
        }
        char msg[256];
        sprintf_s(msg, "[EffectManager] ERROR: Failed to load atlas texture: %s", ddsPath);
        RendererLog(msg);
        return false;
    }

    // DDS atlases already contain mipmaps; don't rebuild them (can corrupt packed sprites).
    set->atlasTexture->setAutoMipMaps(false);
    set->atlasTexture->setAutoCompression(false);
    set->atlasTexture->setFilterMode(
        (HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));
    set->atlasTexture->setTextureWrapModeU(HKG_TEXTURE_CLAMP);
    set->atlasTexture->setTextureWrapModeV(HKG_TEXTURE_CLAMP);
    set->atlasTexture->realize(false);
    set->atlasTexture->setFilterMode(
        (HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));

    {
        char msg[256];
        sprintf_s(msg, "[EffectManager] Loaded atlas texture for %s: %s", set->levelName, ddsPath);
        RendererLog(msg);
    }

    return true;
}

static const char* atlasLookupAlias(const char* textureName)
{
    if (!textureName) return HK_NULL;

    // Known authoring aliases: some effects reference older texture keys that are packed in the atlas
    // under a slightly different name.
    if (stricmp_local(textureName, "FX_SHR_env_ember") == 0 ||
        stricmp_local(textureName, "FX_SHR_env_ember_fire") == 0)
    {
        return "FX_Ember";
    }

    return HK_NULL;
}

bool EffectManager::findAtlasUVInEntries(const hkArray<AtlasUVEntry>& entries, const char* textureName, AtlasUVRect* outRect)
{
    if (!textureName || !outRect)
        return false;

    const char* alias = atlasLookupAlias(textureName);

    for (int pass = 0; pass < 2; pass++)
    {
        const char* lookupName = (pass == 0) ? textureName : alias;
        if (!lookupName) continue;

        for (int i = 0; i < entries.getSize(); i++)
        {
            if (stricmp_local(entries[i].name, lookupName) == 0)
            {
                *outRect = entries[i].rect;
                return true;
            }
        }
    }

    return false;
}

bool EffectManager::resolveAtlasForTexture(const char* effectName,
                                          const char* textureName,
                                          const char* textureDir,
                                          AtlasSet** outSet,
                                          AtlasUVRect* outRect)
{
    if (!effectName || !textureName || !textureDir || !outSet || !outRect)
        return false;

    char gameFilesDir[512];
    buildGameFilesDirFromTextureDir(textureDir, gameFilesDir, sizeof(gameFilesDir));
    if (gameFilesDir[0] == '\0')
        return false;

    const char* preferredLevel = detectPreferredAtlasLevelFromEffectName(effectName);

    const bool preferAtlas2 = (strstr(textureName, "_N") != NULL || strstr(textureName, "_n") != NULL);
    const int atlasOrder[2] = { preferAtlas2 ? 2 : 1, preferAtlas2 ? 1 : 2 };

    // Fast-path: use preferred level atlases when available (most env/camera effects)
    if (preferredLevel)
    {
        for (int k = 0; k < 2; k++)
        {
            AtlasSet* preferred = getOrCreateAtlasSet(preferredLevel, atlasOrder[k]);
            if (preferred && ensureAtlasUVLoaded(preferred, gameFilesDir))
            {
                AtlasUVRect rect;
                if (findAtlasUVInEntries(preferred->entries, textureName, &rect) &&
                    ensureAtlasTextureLoaded(preferred, gameFilesDir) &&
                    preferred->atlasTexture)
                {
                    *outSet = preferred;
                    *outRect = rect;
                    return true;
                }
            }
        }
    }

    // Fallback: search every known level UV table for a match (with deterministic tie-breaker).
    static const char* kLevels[] = {
        "Helm'sDeep",
        "Isengard",
        "Minas_Morgul",
        "MinasTirith",
        "MinasTirith_Top",
        "Moria",
        "Mount_Doom",
        "Osgiliath",
        "PelennorFields",
        "Rivendell",
        "Shire",
        "Training",
        "Weathertop",
        "BlackGates",
    };

    for (int i = 0; i < (int)(sizeof(kLevels) / sizeof(kLevels[0])); i++)
    {
        const char* level = kLevels[i];
        if (preferredLevel && _stricmp(level, preferredLevel) == 0)
            continue;

        for (int k = 0; k < 2; k++)
        {
            AtlasSet* set = getOrCreateAtlasSet(level, atlasOrder[k]);
            if (!set || !ensureAtlasUVLoaded(set, gameFilesDir))
                continue;

            AtlasUVRect rect;
            if (!findAtlasUVInEntries(set->entries, textureName, &rect))
                continue;

            if (!ensureAtlasTextureLoaded(set, gameFilesDir) || !set->atlasTexture)
                continue;

            *outSet = set;
            *outRect = rect;
            return true;
        }
    }

    return false;
}

void EffectManager::patchEmittersWithAtlas(EffectDefinition* def, const char* textureDir)
{
    char msg[512];

    if (!def)
    {
        RendererLog("[patchAtlas] def is NULL, skipping");
        return;
    }

    sprintf_s(msg, "[patchAtlas] Patching %d emitters for effect '%s'",
              def->emitters.getSize(), def->name);
    RendererLog(msg);

    for (int i = 0; i < def->emitters.getSize(); i++)
    {
        ParticleEmitter* emitter = def->emitters[i];
        if (!emitter)
        {
            sprintf_s(msg, "[patchAtlas] Emitter[%d] is NULL", i);
            RendererLog(msg);
            continue;
        }

        // Patch diffuse/mask texture (Texture)
        if (emitter->textureName[0] != '\0')
        {
            sprintf_s(msg, "[patchAtlas] Emitter[%d] '%s' textureName='%s' currentTex=%p",
                      i, emitter->name, emitter->textureName, emitter->texture);
            RendererLog(msg);

            AtlasSet* set = HK_NULL;
            AtlasUVRect rect;
            if (resolveAtlasForTexture(def->name, emitter->textureName, textureDir, &set, &rect))
            {
                // Release any individually loaded texture
                if (emitter->texture && !emitter->usesSharedAtlas)
                {
                    emitter->texture->removeReference();
                }

                // Assign shared atlas texture and UV rect
                emitter->texture = set->atlasTexture;
                emitter->usesSharedAtlas = true;
                emitter->atlasUVRect = rect;

                sprintf_s(msg, "[patchAtlas] PATCHED emitter '%s' tex '%s' -> %s/atlas_%d UV (%.4f, %.4f, %.4f, %.4f)",
                          emitter->name, emitter->textureName,
                          set ? set->levelName : "?", set ? set->atlasIndex : 0,
                          rect.u0, rect.v0, rect.u1, rect.v1);
                RendererLog(msg);
            }
            else
            {
                sprintf_s(msg, "[patchAtlas] NO MATCH for emitter '%s' textureName='%s' in any level atlas",
                          emitter->name, emitter->textureName);
                RendererLog(msg);
            }
        }
        else
        {
            sprintf_s(msg, "[patchAtlas] Emitter[%d] '%s' has empty textureName", i, emitter->name);
            RendererLog(msg);
        }

        // Patch refraction texture (RefractionTexture)
        if (emitter->refractionTextureName[0] != '\0')
        {
            sprintf_s(msg, "[patchAtlas] Emitter[%d] '%s' refractionTextureName='%s' currentRefractTex=%p",
                      i, emitter->name, emitter->refractionTextureName, emitter->refractionTexture);
            RendererLog(msg);

            AtlasSet* set = HK_NULL;
            AtlasUVRect rect;
            if (resolveAtlasForTexture(def->name, emitter->refractionTextureName, textureDir, &set, &rect))
            {
                // Release any individually loaded refraction texture
                if (emitter->refractionTexture && !emitter->usesSharedAtlasRefraction)
                {
                    emitter->refractionTexture->removeReference();
                }

                emitter->refractionTexture = set->atlasTexture;
                emitter->usesSharedAtlasRefraction = true;
                emitter->refractionAtlasUVRect = rect;

                sprintf_s(msg, "[patchAtlas] PATCHED emitter '%s' refract '%s' -> %s/atlas_%d UV (%.4f, %.4f, %.4f, %.4f)",
                          emitter->name, emitter->refractionTextureName,
                          set ? set->levelName : "?", set ? set->atlasIndex : 0,
                          rect.u0, rect.v0, rect.u1, rect.v1);
                RendererLog(msg);
            }
            else
            {
                sprintf_s(msg, "[patchAtlas] NO MATCH for emitter '%s' refractionTextureName='%s' in any level atlas",
                          emitter->name, emitter->refractionTextureName);
                RendererLog(msg);
            }
        }

        // Compute flipbook layout once the primary animated texture and rect are known.
        computeEmitterFlipbookLayout(emitter);
    }
}

EffectDefinition* EffectManager::loadEffectDefinition(const char* jsonPath, const char* textureDir)
{
    if (!jsonPath || !textureDir)
        return HK_NULL;

    // Load the effect
    EffectDefinition* def = m_loader.loadEffect(jsonPath, textureDir);
    if (def)
    {
        // Patch emitters to use atlas textures where available
        patchEmittersWithAtlas(def, textureDir);

        m_definitions.pushBack(def);
    }

    return def;
}

EffectDefinition* EffectManager::findDefinition(const char* name)
{
    if (!name)
        return HK_NULL;
    
    for (int i = 0; i < m_definitions.getSize(); i++)
    {
        if (strcmp(m_definitions[i]->name, name) == 0)
            return m_definitions[i];
    }
    
    return HK_NULL;
}

ActiveEffect* EffectManager::spawnEffect(const char* effectName, const hkVector4& position)
{
    EffectDefinition* def = findDefinition(effectName);
    if (!def)
    {
        char msg[256];
        sprintf_s(msg, "[EffectManager] Effect not found: %s", effectName);
        RendererLog(msg);
        return HK_NULL;
    }

    ActiveEffect* effect = new ActiveEffect();
    effect->definition = def;
    effect->position = position;
    effect->lastPosition = position;
    effect->hasLastPosition = true;
    effect->time = 0.0f;
    effect->finished = false;
    effect->boneIndex = -1;
    effect->debugFramesLeft = (effectName && stricmp_local(effectName, "FX_AB_Captain_power_aura") == 0) ? 120 : 0;

    // Instance emitters/lights so multiple active effects can coexist.
    for (int j = 0; j < def->emitters.getSize(); j++)
    {
        ParticleEmitter* inst = CloneEmitterTemplate(def->emitters[j]);
        if (inst)
            effect->emitters.pushBack(inst);
    }
    for (int j = 0; j < def->lights.getSize(); j++)
    {
        EffectLight* inst = CloneLightTemplate(def->lights[j]);
        if (inst)
            effect->lights.pushBack(inst);
    }

    m_activeEffects.pushBack(effect);
    
    char msg[256];
    sprintf_s(msg, "[EffectManager] Spawned effect: %s at (%.2f, %.2f, %.2f)",
              effectName, position(0), position(1), position(2));
    RendererLog(msg);
    
    return effect;
}

ActiveEffect* EffectManager::spawnEffectOnBone(const char* effectName, int boneIndex)
{
    EffectDefinition* def = findDefinition(effectName);
    if (!def)
    {
        char msg[256];
        sprintf_s(msg, "[EffectManager] Effect not found: %s", effectName);
        RendererLog(msg);
        return HK_NULL;
    }

    ActiveEffect* effect = new ActiveEffect();
    effect->definition = def;
    effect->position.setZero4();
    effect->time = 0.0f;
    effect->finished = false;
    effect->boneIndex = boneIndex;
    effect->debugFramesLeft = (effectName && stricmp_local(effectName, "FX_AB_Captain_power_aura") == 0) ? 120 : 0;

    for (int j = 0; j < def->emitters.getSize(); j++)
    {
        ParticleEmitter* inst = CloneEmitterTemplate(def->emitters[j]);
        if (inst)
            effect->emitters.pushBack(inst);
    }
    for (int j = 0; j < def->lights.getSize(); j++)
    {
        EffectLight* inst = CloneLightTemplate(def->lights[j]);
        if (inst)
            effect->lights.pushBack(inst);
    }

    m_activeEffects.pushBack(effect);
    
    char msg[256];
    sprintf_s(msg, "[EffectManager] Spawned effect: %s on bone %d", effectName, boneIndex);
    RendererLog(msg);
    
    return effect;
}

bool EffectManager::hasActiveEffectNamed(const char* effectName) const
{
    if (!effectName || !effectName[0])
        return false;

    for (int i = 0; i < m_activeEffects.getSize(); i++)
    {
        const ActiveEffect* effect = m_activeEffects[i];
        if (!effect || !effect->definition)
            continue;
        if (effect->definition->name[0] == '\0')
            continue;
        if (stricmp_local(effect->definition->name, effectName) == 0)
            return true;
    }
    return false;
}

void EffectManager::update(float deltaTime, Scene3DRenderer* renderer, const hkaPose* pose)
{
    if (g_fxDisabledAfterException)
        return;

    // Debug: Print active effect count
    static float debugTimer = 0.0f;
    debugTimer += deltaTime;
    if (debugTimer >= 1.0f)
    {
        printf("[EffectManager] Active effects: %d, deltaTime: %.4f\n", m_activeEffects.getSize(), deltaTime);
        debugTimer = 0.0f;
    }

    for (int i = 0; i < m_activeEffects.getSize(); i++)
    {
        ActiveEffect* effect = m_activeEffects[i];
        if (!effect || !effect->definition)
            continue;

        const bool isLocalSpace = !effect->definition->worldSpace;

        // Update effect time
        effect->time += deltaTime;

        // Stop spawning once the effect duration has elapsed (non-looping).
        // Keep updating existing particles until they die.
        bool stopSpawning = false;
        if (!effect->definition->looping && effect->definition->duration > 0)
        {
            float durationSeconds = (float)effect->definition->duration / 30.0f; // game runs at 30fps
            if (effect->time >= durationSeconds)
            {
                stopSpawning = true;
            }
        }

        // Attachment modes:
        //  - boneIndex == -1: world space (position set at spawn)
        //  - boneIndex == -2: follow camera
        //  - boneIndex == -3: follow camera target
        if (pose && renderer && effect->boneIndex >= 0)
        {
            const int boneCount = renderer->getSkeletonBoneCount();
            if (effect->boneIndex < boneCount)
            {
                hkVector4 bonePos = pose->getBoneModelSpace(effect->boneIndex).getTranslation();
                float rox = 0.0f, roy = 0.0f, roz = 0.0f;
                renderer->getRootMotionOffset(rox, roy, roz);
                bonePos(0) += rox;
                bonePos(1) += roy;
                bonePos(2) += roz;
                effect->position = bonePos;
            }
        }
        else if (renderer)
        {
            if (effect->boneIndex == -2)
            {
                effect->position = renderer->getCameraPosition();
            }
            else if (effect->boneIndex == -3)
            {
                effect->position = renderer->getCameraTarget();
            }
        }

        // If this effect is local-space, shift existing particles by the effect's movement
        // before we spawn new ones at the updated position.
        if (isLocalSpace)
        {
            if (effect->hasLastPosition)
            {
                float dx = effect->position(0) - effect->lastPosition(0);
                float dy = effect->position(1) - effect->lastPosition(1);
                float dz = effect->position(2) - effect->lastPosition(2);

                if (fabsf(dx) > 1e-6f || fabsf(dy) > 1e-6f || fabsf(dz) > 1e-6f)
                {
                    hkVector4 delta;
                    delta.set(dx, dy, dz, 0.0f);
                    for (int j = 0; j < effect->emitters.getSize(); j++)
                    {
                        ParticleEmitter* e = effect->emitters[j];
                        if (!e) continue;
                        for (int p = 0; p < e->particles.getSize(); p++)
                        {
                            e->particles[p].position.add4(delta);
                        }
                    }
                }
            }
            effect->lastPosition = effect->position;
            effect->hasLastPosition = true;
        }

        // Update all emitters in this effect instance
        for (int j = 0; j < effect->emitters.getSize(); j++)
        {
            ParticleEmitter* emitter = effect->emitters[j];
            if (emitter)
            {
                if (effect->debugFramesLeft > 0)
                {
                    char dbg[512];
                    sprintf_s(dbg, "[FXDBG] update effect='%s' emitter='%s' particles=%d orient=%d refract=%d tex=%p refrTex=%p",
                              effect->definition ? effect->definition->name : "?",
                              emitter->name,
                              emitter->particles.getSize(),
                              (int)emitter->orientMode,
                              emitter->refract ? 1 : 0,
                              emitter->texture,
                              emitter->refractionTexture);
                    RendererLog(dbg);
                }

                // Set emitter position (world space or bone-attached)
                emitter->position.setAdd4(effect->position, emitter->localOffset);

                // Evaluate effective spawn rate for this frame.
                // Many emitters author RateMin/RateMax = 0 and drive emission via controller segments.
                float ratePerFrame = stopSpawning ? 0.0f
                                                  : (emitter->rateMin + (emitter->rateMax - emitter->rateMin) * 0.5f);
                if (ratePerFrame < 0.0f) ratePerFrame = 0.0f;

                if (!stopSpawning && emitter->rateSegments.getSize() > 0)
                {
                    float timeFrames = effect->time * 30.0f; // effect timeline is in 30 FPS frames
                    float best = 0.0f;
                    for (int s = 0; s < emitter->rateSegments.getSize(); s++)
                    {
                        const RateSegment& seg = emitter->rateSegments[s];
                        if (timeFrames < (float)seg.startFrame || timeFrames > (float)seg.endFrame)
                            continue;

                        float factor = 1.0f;
                        if (seg.fadeInFrames > 0)
                        {
                            float fin = (timeFrames - (float)seg.startFrame) / (float)seg.fadeInFrames;
                            if (fin < 0.0f) fin = 0.0f;
                            if (fin > 1.0f) fin = 1.0f;
                            if (fin < factor) factor = fin;
                        }
                        if (seg.fadeOutFrames > 0)
                        {
                            float fout = ((float)seg.endFrame - timeFrames) / (float)seg.fadeOutFrames;
                            if (fout < 0.0f) fout = 0.0f;
                            if (fout > 1.0f) fout = 1.0f;
                            if (fout < factor) factor = fout;
                        }

                        float r = seg.rate * factor;
                        if (r > best) best = r;
                    }

                    if (best > ratePerFrame)
                        ratePerFrame = best;
                }

                // Update emitter
#ifdef _MSC_VER
                __try
                {
                    emitter->update(deltaTime, ratePerFrame, effect->time);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    const unsigned int code = (unsigned int)GetExceptionCode();
                    char msg[512];
                    sprintf_s(msg, "[FXCRASH] SEH 0x%08X in ParticleEmitter::update effect='%s' emitter='%s'",
                              code,
                              effect->definition ? effect->definition->name : "?",
                              emitter->name);
                    RendererLog(msg);
                    g_fxDisabledAfterException = true;
                    return;
                }
#else
                emitter->update(deltaTime, ratePerFrame, effect->time);
#endif

                // Debug: Print emitter particle count
                if (debugTimer >= 0.99f && j == 0) // Only print for first emitter
                {
                    printf("[EffectManager] Emitter '%s' has %d particles\n",
                        emitter->name, emitter->particles.getSize());
                }
            }
        }

        if (effect->debugFramesLeft > 0)
            effect->debugFramesLeft--;

        // Now that we've advanced particle simulation, decide if this non-looping effect is done.
        if (stopSpawning)
        {
            bool anyAlive = false;
            for (int j = 0; j < effect->emitters.getSize(); j++)
            {
                ParticleEmitter* e = effect->emitters[j];
                if (e && e->particles.getSize() > 0)
                {
                    anyAlive = true;
                    break;
                }
            }
            if (!anyAlive)
            {
                effect->finished = true;
            }
        }
    }

    // Remove finished effects
    removeFinishedEffects();
}

void EffectManager::render(Scene3DRenderer* renderer)
{
    if (!renderer)
        return;
    if (g_fxDisabledAfterException)
        return;

#ifdef _MSC_VER
    __try
    {
#endif

    static int frameCount = 0;
    frameCount++;

    // If any active effect requested a debug window, log a lightweight breadcrumb at render entry.
    for (int i = 0; i < m_activeEffects.getSize(); i++)
    {
        ActiveEffect* effect = m_activeEffects[i];
        if (effect && effect->debugFramesLeft > 0)
        {
            char msg[128];
            sprintf_s(msg, "[FXDBG] EffectManager::render begin (effects=%d)", m_activeEffects.getSize());
            RendererLog(msg);
            break;
        }
    }

    // Log every 60 frames (once per second at 60fps)
    if (frameCount % 60 == 0 && m_activeEffects.getSize() > 0)
    {
        char msg[256];
        sprintf_s(msg, "[EffectManager] Active effects: %d", m_activeEffects.getSize());
        RendererLog(msg);

        int totalParticles = 0;
        for (int i = 0; i < m_activeEffects.getSize(); i++)
        {
            ActiveEffect* effect = m_activeEffects[i];
            if (effect && effect->definition)
            {
                for (int j = 0; j < effect->emitters.getSize(); j++)
                {
                    ParticleEmitter* emitter = effect->emitters[j];
                    if (emitter)
                    {
                        totalParticles += emitter->particles.getSize();
                    }
                }
            }
        }
        sprintf_s(msg, "[EffectManager] Total particles: %d", totalParticles);
        RendererLog(msg);
    }

    // Refraction ("heat haze") particles need a scene copy and a custom shader.
    // Render them first so they distort only the opaque scene, then draw regular particles on top.
    bool hasRefract = false;
    for (int i = 0; i < m_activeEffects.getSize() && !hasRefract; i++)
    {
        ActiveEffect* effect = m_activeEffects[i];
        if (!effect || !effect->definition)
            continue;

        for (int j = 0; j < effect->emitters.getSize(); j++)
        {
            ParticleEmitter* emitter = effect->emitters[j];
            // Only consider refraction if there are actually particles to draw;
            // avoids unnecessary scene copies and reduces the chance of hitting
            // device state edge-cases on frames with zero refract quads.
            if (emitter && emitter->refract && emitter->particles.getSize() > 0)
            {
                hasRefract = true;
                break;
            }
        }
    }

    bool refractReady = false;
    if (hasRefract)
    {
#ifdef _MSC_VER
        // Debug breadcrumb: refraction scene copy tends to be the first GPU-touching operation for these FX.
        for (int i = 0; i < m_activeEffects.getSize(); i++)
        {
            ActiveEffect* effect = m_activeEffects[i];
            if (effect && effect->debugFramesLeft > 0)
            {
                RendererLog("[FXDBG] prepareRefractionSceneCopy begin");
                break;
            }
        }
#endif
#ifdef _MSC_VER
        __try
        {
            refractReady = renderer->prepareRefractionSceneCopy();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            const unsigned int code = (unsigned int)GetExceptionCode();
            char msg[256];
            sprintf_s(msg, "[FXCRASH] SEH 0x%08X in prepareRefractionSceneCopy()", code);
            RendererLog(msg);
            g_fxDisabledAfterException = true;
            return;
        }
#else
        refractReady = renderer->prepareRefractionSceneCopy();
#endif

        for (int i = 0; i < m_activeEffects.getSize(); i++)
        {
            ActiveEffect* effect = m_activeEffects[i];
            if (effect && effect->debugFramesLeft > 0)
            {
                char msg[128];
                sprintf_s(msg, "[FXDBG] prepareRefractionSceneCopy result=%d", refractReady ? 1 : 0);
                RendererLog(msg);
                break;
            }
        }
    }

    if (refractReady)
    {
        for (int i = 0; i < m_activeEffects.getSize(); i++)
        {
            ActiveEffect* effect = m_activeEffects[i];
            if (!effect || !effect->definition)
                continue;

            for (int j = 0; j < effect->emitters.getSize(); j++)
            {
                ParticleEmitter* emitter = effect->emitters[j];
                if (emitter && emitter->refract)
                {
                    if (effect->debugFramesLeft > 0)
                    {
                        char dbg[512];
                        sprintf_s(dbg, "[FXDBG] renderRefract effect='%s' emitter='%s' particles=%d tex=%p refrTex=%p",
                                  effect->definition ? effect->definition->name : "?",
                                  emitter->name,
                                  emitter->particles.getSize(),
                                  emitter->texture,
                                  emitter->refractionTexture);
                        RendererLog(dbg);
                    }

#ifdef _MSC_VER
                    __try
                    {
                        renderer->renderRefractParticleEmitter(emitter);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        const unsigned int code = (unsigned int)GetExceptionCode();
                        char msg[512];
                        sprintf_s(msg, "[FXCRASH] SEH 0x%08X in renderRefractParticleEmitter effect='%s' emitter='%s'",
                                  code,
                                  effect->definition ? effect->definition->name : "?",
                                  emitter->name);
                        RendererLog(msg);
                        g_fxDisabledAfterException = true;
                        return;
                    }
#else
                    renderer->renderRefractParticleEmitter(emitter);
#endif
                }
            }
        }
    }

    for (int i = 0; i < m_activeEffects.getSize(); i++)
    {
        ActiveEffect* effect = m_activeEffects[i];
        if (!effect || !effect->definition)
            continue;

        // Render all emitters in this effect instance
        for (int j = 0; j < effect->emitters.getSize(); j++)
        {
            ParticleEmitter* emitter = effect->emitters[j];
            if (emitter && (!emitter->refract || !refractReady))
            {
                if (effect->debugFramesLeft > 0)
                {
                    char dbg[512];
                    sprintf_s(dbg, "[FXDBG] render effect='%s' emitter='%s' particles=%d orient=%d tex=%p",
                              effect->definition ? effect->definition->name : "?",
                              emitter->name,
                              emitter->particles.getSize(),
                              (int)emitter->orientMode,
                              emitter->texture);
                    RendererLog(dbg);
                }

#ifdef _MSC_VER
                __try
                {
                    renderer->renderParticleEmitter(emitter);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    const unsigned int code = (unsigned int)GetExceptionCode();
                    char msg[512];
                    sprintf_s(msg, "[FXCRASH] SEH 0x%08X in renderParticleEmitter effect='%s' emitter='%s'",
                              code,
                              effect->definition ? effect->definition->name : "?",
                              emitter->name);
                    RendererLog(msg);
                    g_fxDisabledAfterException = true;
                    return;
                }
#else
                renderer->renderParticleEmitter(emitter);
#endif
            }
        }
    }
#ifdef _MSC_VER
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        const unsigned int code = (unsigned int)GetExceptionCode();
        char msg[256];
        sprintf_s(msg, "[FXCRASH] SEH 0x%08X in EffectManager::render()", code);
        RendererLog(msg);
        g_fxDisabledAfterException = true;
        return;
    }
#endif
}

void EffectManager::clearAll()
{
    for (int i = 0; i < m_activeEffects.getSize(); i++)
    {
        DestroyActiveEffect(m_activeEffects[i]);
    }
    m_activeEffects.clear();
}

void EffectManager::removeFinishedEffects()
{
    for (int i = m_activeEffects.getSize() - 1; i >= 0; i--)
    {
        if (m_activeEffects[i]->finished)
        {
            DestroyActiveEffect(m_activeEffects[i]);
            m_activeEffects.removeAt(i);
        }
    }
}
