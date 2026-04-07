// GameModelLoader.cpp — The Unholy Trinity: JSON + GLB + DDS = One Fucking Model
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Stitches JSON metadata, GLB geometry, and DDS textures into a GameModel
// that Havok and D3D9 can render. Three formats that hate each other,
// forced to cooperate at gunpoint. Bone names mismatch? We fix it.
// Texture case wrong? We fix it. Skin weights don't sum to 1.0? We
// normalize them and pretend it was always fine. This file is 90% error
// handling and 10% actual model loading. That ratio tells you everything.
// -----------------------------------------------------------------------
#include "GameModelLoader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <vector>
#include <string>
#include <d3d9.h>
#include <math.h>
#include <new>
#include "ZeroMath.h"

#include <Graphics/Common/DisplayContext/hkgDisplayContext.h>
#include <Graphics/Common/Texture/hkgTexture.h>
#include <Common/Base/Math/Matrix/hkRotation.h>
#include <Common/Base/Math/Quaternion/hkQuaternion.h>

/**
 * GameModelLoader.cpp – 2100+ lines of "Dante's hell"
 * 
 * Don't scroll too fast.
 * Around line 1335 the computer sometimes lags by one frame.  
 * and its because of this file
 * 
 * If you breakpoint inside the file around the said line,
 * the debugger shows a call stack with your username in one of the frames.  
 * 
 * This file is a black hole.  
 * It appeared fully formed one night, already monstrous.  
 * You know how some files just feel wrong?
 * This one is one of them
 * Scroll past line 55 at your own risk.  
 * 
 * the scariest part? it still loads everything
 * so dont you dare to touch it
 * because the one time i tried to clean it up,
 * the characters turned into scary abominations, something like skinwalkers.
 * 
 * If you ever have to work in here:
 * close every other tab first.
 * change one tiny thing
 * test immediately
 * 
 * do NOT try to understand the whole file because nobody can.
 * 
 * it doesn't like being understood.
 * 
 * also please don't rename anything.  
 * Please don't add any hurtful comment because as you can see I am trying to be 
 * as polite and careful as possible here, so you can probably guess that something 
 * is very wrong with this file.
 * 
 */ 

// External D3D device (will be needed for Phase 2: texture loading)
// extern IDirect3DDevice9* g_pd3dDevice;

// Simple logging helper
static bool g_gameModelLogInitialized = false;
static void GameModelLog(const char* format, ...)
{
    FILE* logFile = fopen("gamemodel.log", g_gameModelLogInitialized ? "a" : "w");
    if (logFile)
    {
        g_gameModelLogInitialized = true;
        va_list args;
        va_start(args, format);
        vfprintf(logFile, format, args);
        va_end(args);
        fprintf(logFile, "\n");
        fclose(logFile);
    }
}

// ------------------------------------------------------------
// Minimal JSON helpers (C++03, no external deps)
// ------------------------------------------------------------
static const char* SkipWhitespace(const char* p)
{
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        ++p;
    }
    return p;
}

static const char* FindKey(const char* json, const char* key)
{
    if (!json || !key) return NULL;
    char pattern[128];
    sprintf_s(pattern, "\"%s\"", key);
    return strstr(json, pattern);
}

static const char* FindKeyInRange(const char* start, const char* end, const char* key)
{
    if (!start || !end || !key) return NULL;
    char pattern[128];
    sprintf_s(pattern, "\"%s\"", key);
    const size_t patLen = strlen(pattern);
    for (const char* p = start; p + patLen <= end; ++p)
    {
        if (*p == '"' && (size_t)(end - p) >= patLen)
        {
            if (memcmp(p, pattern, patLen) == 0)
            {
                return p;
            }
        }
    }
    return NULL;
}

static const char* FindMatching(const char* start, char openCh, char closeCh)
{
    if (!start || *start != openCh) return NULL;
    int depth = 0;
    bool inString = false;
    for (const char* p = start; *p; ++p)
    {
        if (*p == '"' && (p == start || p[-1] != '\\')) {
            inString = !inString;
        }
        if (inString) continue;
        if (*p == openCh) depth++;
        else if (*p == closeCh) {
            depth--;
            if (depth == 0) return p;
        }
    }
    return NULL;
}

static bool ExtractIntValueRange(const char* start, const char* end, const char* key, int* out)
{
    if (!start || !end || !key || !out) return false;
    const char* k = FindKeyInRange(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, ':');
    if (!p || p >= end) return false;
    p = SkipWhitespace(p + 1);
    *out = (int)strtol(p, NULL, 10);
    return true;
}

static bool ExtractStringValueRange(const char* start, const char* end, const char* key, char* out, int outSize)
{
    if (!start || !end || !key || !out || outSize <= 0) return false;
    const char* k = FindKeyInRange(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, ':');
    if (!p || p >= end) return false;
    p = SkipWhitespace(p + 1);
    if (*p != '"') return false;
    p++;
    const char* strEnd = strchr(p, '"');
    if (!strEnd || strEnd > end) return false;
    int len = (int)(strEnd - p);
    if (len >= outSize) len = outSize - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool ExtractIntValue(const char* json, const char* key, int* out)
{
    if (!json) return false;
    return ExtractIntValueRange(json, json + strlen(json), key, out);
}

static bool ExtractStringValue(const char* json, const char* key, char* out, int outSize)
{
    if (!json) return false;
    return ExtractStringValueRange(json, json + strlen(json), key, out, outSize);
}

static bool ParseIntArray(const char* json, const char* key, int* outValues, int maxValues, int* outCount)
{
    if (!json || !key || !outValues || maxValues <= 0) return false;
    const char* k = FindKey(json, key);
    if (!k) return false;
    const char* p = strchr(k, '[');
    if (!p) return false;
    const char* end = FindMatching(p, '[', ']');
    if (!end) return false;

    int count = 0;
    for (const char* c = p + 1; c < end && count < maxValues; )
    {
        c = SkipWhitespace(c);
        if (c >= end) break;
        if (*c == '-' || (*c >= '0' && *c <= '9'))
        {
            outValues[count++] = (int)strtol(c, (char**)&c, 10);
        }
        else
        {
            c++;
        }
    }
    if (outCount) *outCount = count;
    return count > 0;
}

static bool ParseIntArrayRange(const char* start, const char* end, const char* key, std::vector<int>& outValues)
{
    if (!start || !end || !key) return false;
    const char* k = FindKeyInRange(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, '[');
    if (!p || p >= end) return false;
    const char* arrEnd = FindMatching(p, '[', ']');
    if (!arrEnd || arrEnd > end) return false;

    for (const char* c = p + 1; c < arrEnd; )
    {
        c = SkipWhitespace(c);
        if (c >= arrEnd) break;
        if (*c == '-' || (*c >= '0' && *c <= '9'))
        {
            int val = (int)strtol(c, (char**)&c, 10);
            outValues.push_back(val);
        }
        else
        {
            c++;
        }
    }
    return !outValues.empty();
}

static bool ParseStringArrayRange(const char* start, const char* end, const char* key, std::vector<std::string>& outValues)
{
    if (!start || !end || !key) return false;
    const char* k = FindKeyInRange(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, '[');
    if (!p || p >= end) return false;
    const char* arrEnd = FindMatching(p, '[', ']');
    if (!arrEnd || arrEnd > end) return false;

    for (const char* c = p + 1; c < arrEnd; )
    {
        c = SkipWhitespace(c);
        if (c >= arrEnd) break;
        if (*c == '"')
        {
            c++;
            const char* strEnd = c;
            while (strEnd < arrEnd)
            {
                if (*strEnd == '"' && (strEnd == c || strEnd[-1] != '\\'))
                {
                    break;
                }
                strEnd++;
            }
            if (strEnd >= arrEnd) break;
            outValues.push_back(std::string(c, strEnd - c));
            c = strEnd + 1;
        }
        else
        {
            c++;
        }
    }
    return !outValues.empty();
}

static bool HasDdsExtension(const char* name)
{
    if (!name) return false;
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return (_stricmp(dot, ".dds") == 0);
}

static bool ParseBoneTransformObject(const char* objStart, const char* objEnd, hkQsTransform& out)
{
    if (!objStart || !objEnd) return false;
    float vals[16];
    int count = 0;

    for (const char* p = objStart; p < objEnd && count < 16; )
    {
        if (*p == '-' || (*p >= '0' && *p <= '9'))
        {
            char* endPtr = NULL;
            double v = strtod(p, &endPtr);
            vals[count++] = (float)v;
            p = endPtr ? endPtr : (p + 1);
        }
        else
        {
            ++p;
        }
    }

    if (count < 16)
    {
        return false;
    }

    hkVector4 col0(vals[0], vals[1], vals[2]);
    hkVector4 col1(vals[4], vals[5], vals[6]);
    hkVector4 col2(vals[8], vals[9], vals[10]);

    hkRotation rot;
    rot.setCols(col0, col1, col2);

    hkQuaternion q;
    q.set(rot);

    hkVector4 translation(vals[12], vals[13], vals[14]);

    out.setRotation(q);
    out.setTranslation(translation);
    out.setScale(hkVector4(1.0f, 1.0f, 1.0f));
    return true;
}
/** 
 * Sometimes, after a long session, the file re-opens itself on the same line you were reading.
 * Not Visual Studio. The file.  
 * The scroll position is exact. The caret is blinking exactly where you left it.*/
static void CopyBaseMaterialName(const char* textureName, char* out, int outSize)
{
    if (!textureName || !out || outSize <= 0) return;
    out[0] = '\0';

    char temp[256];
    strcpy_s(temp, textureName);

    // Strip extension
    char* dot = strrchr(temp, '.');
    if (dot && _stricmp(dot, ".dds") == 0) {
        *dot = '\0';
    }

    // Strip common suffixes
    size_t len = strlen(temp);
    if (len > 2) {
        const char* suffix = temp + len - 2;
        if (_stricmp(suffix, "_D") == 0 || _stricmp(suffix, "_N") == 0 || _stricmp(suffix, "_S") == 0) {
            temp[len - 2] = '\0';
        }
    }

    strcpy_s(out, outSize, temp);
}

static const char* SafeStr(const char* s)
{
    return (s && s[0] != '\0') ? s : "<none>";
}

static bool ContainsNoCase(const char* haystack, const char* needle)
{
    if (!haystack || !needle || needle[0] == '\0')
    {
        return false;
    }
    const size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; ++p)
    {
        if (_strnicmp(p, needle, nlen) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool MeshPartHasMeaningfulUVs(const GameModel::MeshPart& part)
{
    if (!part.vertices || part.vertexCount <= 0 || part.vertexStride < 20)
    {
        return false;
    }

    const int strideFloats = part.vertexStride / (int)sizeof(float);
    if (strideFloats < 5)
    {
        return false;
    }

    const float* v = (const float*)part.vertices;
    float minU = v[3];
    float maxU = v[3];
    float minV = v[4];
    float maxV = v[4];

    for (int i = 1; i < part.vertexCount; ++i)
    {
        const float* p = v + i * strideFloats;
        if (p[3] < minU) minU = p[3];
        if (p[3] > maxU) maxU = p[3];
        if (p[4] < minV) minV = p[4];
        if (p[4] > maxV) maxV = p[4];
    }

    const float spanU = maxU - minU;
    const float spanV = maxV - minV;
    return (spanU > 0.0005f || spanV > 0.0005f);
}

static void GenerateSphericalUVs(GameModel::MeshPart& part)
{
    if (!part.vertices || part.vertexCount <= 0 || part.vertexStride < 20)
    {
        return;
    }

    const int strideFloats = part.vertexStride / (int)sizeof(float);
    if (strideFloats < 5)
    {
        return;
    }

    const float invTwoPi = 1.0f / (2.0f * 3.14159265f);
    const float invPi = 1.0f / 3.14159265f;

    float* v = (float*)part.vertices;
    for (int i = 0; i < part.vertexCount; ++i)
    {
        float* p = v + i * strideFloats;
        float x = p[0];
        float y = p[1];
        float z = p[2];

        float len = ZLength3f(x, y, z);
        if (len < 1e-6f)
        {
            p[3] = 0.0f;
            p[4] = 0.0f;
            continue;
        }

        ZNormalize3f(x, y, z);

        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;

        p[3] = 0.5f + atan2f(z, x) * invTwoPi;
        p[4] = 0.5f - asinf(y) * invPi;
    }
}

static int FindBoneIndexByName(const hkaSkeleton* skeleton, const char* name)
{
    if (!skeleton || !name || name[0] == '\0') return -1;
    for (int i = 0; i < skeleton->m_numBones; i++)
    {
        if (skeleton->m_bones[i] && skeleton->m_bones[i]->m_name)
        {
            if (strcmp(skeleton->m_bones[i]->m_name, name) == 0)
            {
                return i;
            }
        }
    }
    return -1;
}

static int FindBoneIndexByNameCaseInsensitive(const hkaSkeleton* skeleton, const char* name)
{
    if (!skeleton || !name || name[0] == '\0') return -1;
    for (int i = 0; i < skeleton->m_numBones; i++)
    {
        if (skeleton->m_bones[i] && skeleton->m_bones[i]->m_name)
        {
            if (_stricmp(skeleton->m_bones[i]->m_name, name) == 0)
            {
                return i;
            }
        }
    }
    return -1;
}

static void StripNamespaceToken(const char* name, char* out, int outSize)
{
    if (!out || outSize <= 0) return;
    out[0] = '\0';
    if (!name || name[0] == '\0') return;

    const char* start = name;
    const char* colon = strrchr(name, ':');
    const char* bar = strrchr(name, '|');
    if (colon && (!bar || colon > bar)) start = colon + 1;
    else if (bar) start = bar + 1;

    while (*start == ' ' || *start == '\t') start++;

    size_t len = strlen(start);
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
    {
        len--;
    }

    if ((int)len >= outSize) len = outSize - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

static int FindBoneIndexByNameFlexible(const hkaSkeleton* skeleton, const char* name, bool* outUsedAlias)
{
    if (outUsedAlias) *outUsedAlias = false;
    if (!skeleton || !name || name[0] == '\0') return -1;

    int idx = FindBoneIndexByName(skeleton, name);
    if (idx >= 0) return idx;
    idx = FindBoneIndexByNameCaseInsensitive(skeleton, name);
    if (idx >= 0) return idx;

    char stripped[128];
    StripNamespaceToken(name, stripped, sizeof(stripped));
    if (stripped[0] != '\0' && strcmp(stripped, name) != 0)
    {
        idx = FindBoneIndexByName(skeleton, stripped);
        if (idx >= 0) return idx;
        idx = FindBoneIndexByNameCaseInsensitive(skeleton, stripped);
        if (idx >= 0) return idx;
    }

    // Try "Bone_" prefix if missing
    const char* baseName = (stripped[0] != '\0') ? stripped : name;
    if (_strnicmp(baseName, "Bone_", 5) != 0)
    {
        char prefixed[140];
        sprintf_s(prefixed, "Bone_%s", baseName);
        idx = FindBoneIndexByName(skeleton, prefixed);
        if (idx >= 0) return idx;
        idx = FindBoneIndexByNameCaseInsensitive(skeleton, prefixed);
        if (idx >= 0) return idx;
    }

    // Simple alias table for common humanoid rigs
    struct Alias { const char* from; const char* to; };
    static const Alias aliases[] =
    {
        { "Root", "Bone_Root" },
        { "root", "Bone_Root" },
        { "Hips", "Bone_Root" },
        { "Pelvis", "Bone_Root" },
        { "Spine", "Bone_Lumbar1" },
        { "Spine1", "Bone_Lumbar2" },
        { "Spine2", "Bone_Lumbar3" },
        { "Neck", "Bone_Neck" },
        { "Head", "Bone_Head" },
        { "Jaw", "Bone_jaw" },
        { "LeftShoulder", "Bone_LShoulder" },
        { "RightShoulder", "Bone_RShoulder" },
        { "LeftArm", "Bone_LBicep" },
        { "RightArm", "Bone_RBicep" },
        { "LeftForeArm", "Bone_LForearm" },
        { "RightForeArm", "Bone_RForearm" },
        { "LeftHand", "Bone_LHand" },
        { "RightHand", "Bone_RHand" },
        { "LeftUpLeg", "Bone_LThigh" },
        { "RightUpLeg", "Bone_RThigh" },
        { "LeftLeg", "Bone_LShin" },
        { "RightLeg", "Bone_RShin" },
        { "LeftFoot", "Bone_LFootBone1" },
        { "RightFoot", "Bone_RFootBone1" }
    };

    for (int i = 0; i < (int)(sizeof(aliases) / sizeof(aliases[0])); i++)
    {
        if (_stricmp(baseName, aliases[i].from) == 0)
        {
            idx = FindBoneIndexByName(skeleton, aliases[i].to);
            if (idx < 0)
            {
                idx = FindBoneIndexByNameCaseInsensitive(skeleton, aliases[i].to);
            }
            if (idx >= 0)
            {
                if (outUsedAlias) *outUsedAlias = true;
                return idx;
            }
        }
    }

    return -1;
}

static void SetIdentityMatrix(float* out)
{
    if (!out) return;
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

// ------------------------------------------------------------
// glTF (GLB) parsing helpers
// ------------------------------------------------------------
struct GlbAccessor
{
    int bufferView;
    int byteOffset;
    int componentType;
    int count;
    char type[8];
};

struct GlbBufferView
{
    int byteOffset;
    int byteLength;
    int byteStride;
};

struct GlbPrimitive
{
    int positionAccessor;
    int normalAccessor;
    int texcoordAccessor;
    int indexAccessor;
    int jointsAccessor;
    int weightsAccessor;
    int meshIndex;
    int skinIndex;
};

struct GlbNode
{
    char name[128];
    int mesh;
    int skin;
};

struct GlbSkin
{
    std::vector<int> joints;
    int inverseBindAccessor;
};

static int TypeComponentCount(const char* type)
{
    if (!type) return 0;
    if (strcmp(type, "SCALAR") == 0) return 1;
    if (strcmp(type, "VEC2") == 0) return 2;
    if (strcmp(type, "VEC3") == 0) return 3;
    if (strcmp(type, "VEC4") == 0) return 4;
    if (strcmp(type, "MAT4") == 0) return 16;
    return 0;
}

static int ComponentTypeSize(int componentType)
{
    switch (componentType)
    {
    case 5120: // BYTE
    case 5121: // UNSIGNED_BYTE
        return 1;
    case 5122: // SHORT
    case 5123: // UNSIGNED_SHORT
        return 2;
    case 5125: // UNSIGNED_INT
    case 5126: // FLOAT
        return 4;
    default:
        return 0;
    }
}

// ------------------------------------------------------------
// GLB sub-parsers – each extracts one top-level JSON array
// ------------------------------------------------------------

static void ParseGlbBufferViews(const char* json, std::vector<GlbBufferView>& out)
{
    const char* key = FindKey(json, "bufferViews");
    if (!key) return;
    const char* arr = strchr(key, '[');
    const char* arrEnd = FindMatching(arr, '[', ']');
    const char* p = (arr && arrEnd) ? (arr + 1) : NULL;
    while (p && arrEnd && p < arrEnd)
    {
        const char* objStart = strchr(p, '{');
        if (!objStart || objStart >= arrEnd) break;
        const char* objEnd = FindMatching(objStart, '{', '}');
        if (!objEnd || objEnd > arrEnd) break;

        GlbBufferView bv;
        bv.byteOffset = 0;
        bv.byteLength = 0;
        bv.byteStride = 0;
        ExtractIntValueRange(objStart, objEnd, "byteOffset", &bv.byteOffset);
        ExtractIntValueRange(objStart, objEnd, "byteLength", &bv.byteLength);
        ExtractIntValueRange(objStart, objEnd, "byteStride", &bv.byteStride);
        out.push_back(bv);
        p = objEnd + 1;
    }
}

static void ParseGlbAccessors(const char* json, std::vector<GlbAccessor>& out)
{
    const char* key = FindKey(json, "accessors");
    if (!key) return;
    const char* arr = strchr(key, '[');
    const char* arrEnd = FindMatching(arr, '[', ']');
    const char* p = (arr && arrEnd) ? (arr + 1) : NULL;
    while (p && arrEnd && p < arrEnd)
    {
        const char* objStart = strchr(p, '{');
        if (!objStart || objStart >= arrEnd) break;
        const char* objEnd = FindMatching(objStart, '{', '}');
        if (!objEnd || objEnd > arrEnd) break;

        GlbAccessor acc;
        acc.bufferView = -1;
        acc.byteOffset = 0;
        acc.componentType = 0;
        acc.count = 0;
        acc.type[0] = '\0';
        ExtractIntValueRange(objStart, objEnd, "bufferView", &acc.bufferView);
        ExtractIntValueRange(objStart, objEnd, "byteOffset", &acc.byteOffset);
        ExtractIntValueRange(objStart, objEnd, "componentType", &acc.componentType);
        ExtractIntValueRange(objStart, objEnd, "count", &acc.count);
        ExtractStringValueRange(objStart, objEnd, "type", acc.type, sizeof(acc.type));
        out.push_back(acc);
        p = objEnd + 1;
    }
}

static void ParseGlbNodes(const char* json, std::vector<GlbNode>& out)
{
    const char* key = FindKey(json, "nodes");
    if (!key) return;
    const char* arr = strchr(key, '[');
    const char* arrEnd = FindMatching(arr, '[', ']');
    const char* p = (arr && arrEnd) ? (arr + 1) : NULL;
    while (p && arrEnd && p < arrEnd)
    {
        const char* objStart = strchr(p, '{');
        if (!objStart || objStart >= arrEnd) break;
        const char* objEnd = FindMatching(objStart, '{', '}');
        if (!objEnd || objEnd > arrEnd) break;

        GlbNode node;
        node.name[0] = '\0';
        node.mesh = -1;
        node.skin = -1;
        ExtractStringValueRange(objStart, objEnd, "name", node.name, sizeof(node.name));
        ExtractIntValueRange(objStart, objEnd, "mesh", &node.mesh);
        ExtractIntValueRange(objStart, objEnd, "skin", &node.skin);
        out.push_back(node);
        p = objEnd + 1;
    }
}

static void ParseGlbSkins(const char* json, std::vector<GlbSkin>& out)
{
    const char* key = FindKey(json, "skins");
    if (!key) return;
    const char* arr = strchr(key, '[');
    const char* arrEnd = FindMatching(arr, '[', ']');
    const char* p = (arr && arrEnd) ? (arr + 1) : NULL;
    while (p && arrEnd && p < arrEnd)
    {
        const char* objStart = strchr(p, '{');
        if (!objStart || objStart >= arrEnd) break;
        const char* objEnd = FindMatching(objStart, '{', '}');
        if (!objEnd || objEnd > arrEnd) break;

        GlbSkin skin;
        skin.inverseBindAccessor = -1;
        ExtractIntValueRange(objStart, objEnd, "inverseBindMatrices", &skin.inverseBindAccessor);
        ParseIntArrayRange(objStart, objEnd, "joints", skin.joints);
        out.push_back(skin);
        p = objEnd + 1;
    }
}

static int ParseGlbMeshPrimitives(const char* json, std::vector<GlbPrimitive>& out)
{
    int meshCount = 0;
    const char* key = FindKey(json, "meshes");
    if (!key) return 0;

    const char* arr = strchr(key, '[');
    const char* arrEnd = FindMatching(arr, '[', ']');
    const char* m = (arr && arrEnd) ? (arr + 1) : NULL;
    int meshIndex = 0;

    while (m && arrEnd && m < arrEnd)
    {
        const char* meshStart = strchr(m, '{');
        if (!meshStart || meshStart >= arrEnd) break;
        const char* meshEnd = FindMatching(meshStart, '{', '}');
        if (!meshEnd || meshEnd > arrEnd) break;

        const char* primKey = FindKeyInRange(meshStart, meshEnd, "primitives");
        if (primKey)
        {
            const char* primArr = strchr(primKey, '[');
            const char* primArrEnd = FindMatching(primArr, '[', ']');
            const char* p = (primArr && primArrEnd) ? (primArr + 1) : NULL;
            while (p && primArrEnd && p < primArrEnd)
            {
                const char* primStart = strchr(p, '{');
                if (!primStart || primStart >= primArrEnd) break;
                const char* primObjEnd = FindMatching(primStart, '{', '}');
                if (!primObjEnd || primObjEnd > primArrEnd) break;

                GlbPrimitive prim;
                prim.positionAccessor = -1;
                prim.normalAccessor = -1;
                prim.texcoordAccessor = -1;
                prim.indexAccessor = -1;
                prim.jointsAccessor = -1;
                prim.weightsAccessor = -1;
                prim.meshIndex = meshIndex;
                prim.skinIndex = -1;

                ExtractIntValueRange(primStart, primObjEnd, "indices", &prim.indexAccessor);

                const char* attrKey = FindKeyInRange(primStart, primObjEnd, "attributes");
                if (attrKey)
                {
                    const char* attrStart = strchr(attrKey, '{');
                    const char* attrEnd = FindMatching(attrStart, '{', '}');
                    if (attrStart && attrEnd)
                    {
                        ExtractIntValueRange(attrStart, attrEnd, "POSITION", &prim.positionAccessor);
                        ExtractIntValueRange(attrStart, attrEnd, "NORMAL", &prim.normalAccessor);
                        ExtractIntValueRange(attrStart, attrEnd, "TEXCOORD_0", &prim.texcoordAccessor);
                        ExtractIntValueRange(attrStart, attrEnd, "JOINTS_0", &prim.jointsAccessor);
                        ExtractIntValueRange(attrStart, attrEnd, "WEIGHTS_0", &prim.weightsAccessor);
                    }
                }

                if (prim.positionAccessor >= 0)
                {
                    out.push_back(prim);
                }
                p = primObjEnd + 1;
            }
        }

        meshIndex++;
        m = meshEnd + 1;
    }

    return meshIndex;
}

GameModelLoader::GameModelLoader()
    : m_context(NULL)
{
}

GameModelLoader::~GameModelLoader()
{
}

void GameModelLoader::setDisplayContext(hkgDisplayContext* context)
{
    m_context = context;
}

GameModel* GameModelLoader::loadModel(const char* jsonPath, const char* glbPath, const char* textureDir)
{
    GameModel* model = new GameModel();
    memset(model, 0, sizeof(GameModel));
    
    // Step 1: Load skeleton from JSON
    model->skeleton = loadSkeletonFromJSON(jsonPath);
    if (!model->skeleton)
    {
        printf("[GameModelLoader] Failed to load skeleton from %s\n", jsonPath);
        delete model;
        return NULL;
    }
    
    printf("[GameModelLoader] Loaded skeleton with %d bones\n", model->skeleton->m_numBones);

    // Step 2: Load mesh from GLB
    if (!loadMeshFromGLB(glbPath, model))
    {
        printf("[GameModelLoader] Warning: Failed to load mesh from %s\n", glbPath);
        // Continue anyway - we can still render the skeleton
    }

    // Step 3: Load material/texture names from JSON metadata
    loadMaterialNamesFromJSON(jsonPath, model);

    // Step 4: Load textures (DDS)
    if (textureDir && textureDir[0] != '\0' && model->meshParts)
    {
        for (int i = 0; i < model->meshPartCount; i++)
        {
            GameModel::MeshPart& part = model->meshParts[i];

            char path[512];

            // Diffuse
            if (part.diffuseName[0] != '\0')
            {
                if (HasDdsExtension(part.diffuseName))
                    sprintf_s(path, "%s%s", textureDir, part.diffuseName);
                else
                    sprintf_s(path, "%s%s.dds", textureDir, part.diffuseName);
                part.diffuseTexture = loadDDSTexture(path);
            }

            // Normal
            if (part.normalName[0] != '\0')
            {
                if (HasDdsExtension(part.normalName))
                    sprintf_s(path, "%s%s", textureDir, part.normalName);
                else
                    sprintf_s(path, "%s%s.dds", textureDir, part.normalName);
                part.normalTexture = loadDDSTexture(path);
            }

            // Specular
            if (part.specularName[0] != '\0')
            {
                if (HasDdsExtension(part.specularName))
                    sprintf_s(path, "%s%s", textureDir, part.specularName);
                else
                    sprintf_s(path, "%s%s.dds", textureDir, part.specularName);
                part.specularTexture = loadDDSTexture(path);
            }

            // Fallback: standard LOTR naming based on materialName
            if (!part.diffuseTexture && part.materialName[0] != '\0')
            {
                sprintf_s(path, "%s%s_D.dds", textureDir, part.materialName);
                part.diffuseTexture = loadDDSTexture(path);
            }
            if (!part.normalTexture && part.materialName[0] != '\0')
            {
                sprintf_s(path, "%s%s_N.dds", textureDir, part.materialName);
                part.normalTexture = loadDDSTexture(path);
            }
            if (!part.specularTexture && part.materialName[0] != '\0')
            {
                sprintf_s(path, "%s%s_S.dds", textureDir, part.materialName);
                part.specularTexture = loadDDSTexture(path);
            }

            // Fallback: try "<Material>.dds" if diffuse wasn't found
            if (!part.diffuseTexture && part.materialName[0] != '\0')
            {
                sprintf_s(path, "%s%s.dds", textureDir, part.materialName);
                part.diffuseTexture = loadDDSTexture(path);
            }

            GameModelLog("[GameModelLoader] Part %d material='%s' diffuse='%s' (%s) normal='%s' (%s) specular='%s' (%s)",
                i,
                SafeStr(part.materialName),
                SafeStr(part.diffuseName), part.diffuseTexture ? "loaded" : "missing",
                SafeStr(part.normalName), part.normalTexture ? "loaded" : "missing",
                SafeStr(part.specularName), part.specularTexture ? "loaded" : "missing");
        }

        // Material fallback:
        // Some character assets split one material across multiple mesh parts but only one part
        // carries explicit diffuse metadata. Reuse by material name for those assets only.
        // Skydomes must NOT use this fallback (it creates an opaque/flat inner dome artifact).
        const bool isSkyDomeAsset =
            ContainsNoCase(jsonPath, "skydome") ||
            ContainsNoCase(glbPath, "skydome");

        if (!isSkyDomeAsset)
        {
            for (int i = 0; i < model->meshPartCount; i++)
            {
                GameModel::MeshPart& dst = model->meshParts[i];
                if (dst.diffuseTexture || dst.materialName[0] == '\0')
                {
                    continue;
                }

                for (int j = 0; j < model->meshPartCount; j++)
                {
                    if (i == j)
                    {
                        continue;
                    }

                    GameModel::MeshPart& src = model->meshParts[j];
                    if (!src.diffuseTexture || src.materialName[0] == '\0')
                    {
                        continue;
                    }

                    if (_stricmp(dst.materialName, src.materialName) == 0)
                    {
                        dst.diffuseTexture = src.diffuseTexture;
                        dst.diffuseTexture->addReference();
                        if (dst.diffuseName[0] == '\0' && src.diffuseName[0] != '\0')
                        {
                            strcpy_s(dst.diffuseName, src.diffuseName);
                        }
                        GameModelLog("[GameModelLoader] Part %d reused diffuse from part %d by material '%s'",
                            i, j, SafeStr(dst.materialName));
                        break;
                    }
                }
            }
        }
        else
        {
            GameModelLog("[GameModelLoader] Skydome path: enabling controlled sky fallback for untextured parts: %s",
                SafeStr(jsonPath));

            int primaryPart = -1;
            for (int i = 0; i < model->meshPartCount; i++)
            {
                if (model->meshParts[i].diffuseTexture)
                {
                    primaryPart = i;
                    break;
                }
            }

            if (primaryPart >= 0)
            {
                GameModel::MeshPart& primary = model->meshParts[primaryPart];

                for (int i = 0; i < model->meshPartCount; i++)
                {
                    GameModel::MeshPart& dst = model->meshParts[i];
                    if (dst.diffuseTexture)
                    {
                        continue;
                    }

                    if (!MeshPartHasMeaningfulUVs(dst))
                    {
                        GenerateSphericalUVs(dst);
                        dst.generatedSkyUV = true;
                        GameModelLog("[GameModelLoader] Skydome part %d had no UV span; generated spherical UVs", i);
                    }

                    dst.diffuseTexture = primary.diffuseTexture;
                    if (dst.diffuseTexture)
                    {
                        dst.diffuseTexture->addReference();
                    }
                    if (dst.diffuseName[0] == '\0' && primary.diffuseName[0] != '\0')
                    {
                        strcpy_s(dst.diffuseName, primary.diffuseName);
                    }
                    GameModelLog("[GameModelLoader] Skydome part %d borrowed diffuse from part %d ('%s')",
                        i, primaryPart, SafeStr(primary.diffuseName));
                }
            }
            else
            {
                GameModelLog("[GameModelLoader] Skydome fallback skipped: no primary textured part found");
            }
        }
    }

    return model;
}

hkaSkeleton* GameModelLoader::loadSkeletonFromJSON(const char* jsonPath)
{
    if (!jsonPath || jsonPath[0] == '\0')
    {
        printf("[GameModelLoader] loadSkeletonFromJSON: invalid path\n");
        return NULL;
    }

    int jsonSize = 0;
    char* json = loadFileToBuffer(jsonPath, &jsonSize);
    if (!json)
    {
        printf("[GameModelLoader] loadSkeletonFromJSON: failed to read %s\n", jsonPath);
        return NULL;
    }

    std::vector<std::string> boneNames;
    std::vector<int> parentIndices;

    ParseStringArrayRange(json, json + jsonSize, "bones", boneNames);
    ParseIntArrayRange(json, json + jsonSize, "bone_parents", parentIndices);

    if (boneNames.empty())
    {
        printf("[GameModelLoader] loadSkeletonFromJSON: no bones found in %s\n", jsonPath);
        delete[] json;
        return NULL;
    }

    int boneCount = (int)boneNames.size();

    // Create skeleton
    hkaSkeleton* skeleton = new hkaSkeleton();
    skeleton->m_numBones = boneCount;
    skeleton->m_numParentIndices = boneCount;
    skeleton->m_numReferencePose = boneCount;
    skeleton->m_numFloatSlots = 0;
    skeleton->m_floatSlots = NULL;

    // Optional skeleton name from info.key
    char skelName[128];
    skelName[0] = '\0';
    const char* infoKey = FindKey(json, "info");
    if (infoKey)
    {
        const char* infoStart = strchr(infoKey, '{');
        const char* infoEnd = FindMatching(infoStart, '{', '}');
        if (infoStart && infoEnd)
        {
            ExtractStringValueRange(infoStart, infoEnd, "key", skelName, sizeof(skelName));
        }
    }
    if (skelName[0] != '\0')
    {
        size_t nameLen = strlen(skelName);
        char* nameCopy = new char[nameLen + 1];
        strcpy_s(nameCopy, nameLen + 1, skelName);
        skeleton->m_name = nameCopy;
    }
    else
    {
        skeleton->m_name = NULL;
    }

    // Allocate bone array (hkaBone** is array of pointers)
    skeleton->m_bones = new hkaBone*[boneCount];
    for (int i = 0; i < boneCount; i++)
    {
        skeleton->m_bones[i] = new hkaBone();
        skeleton->m_bones[i]->m_lockTranslation = false;

        const std::string& srcName = boneNames[i];
        size_t nameLen = srcName.size();
        char* nameCopy = new char[nameLen + 1];
        if (nameLen > 0)
        {
            memcpy(nameCopy, srcName.c_str(), nameLen);
        }
        nameCopy[nameLen] = '\0';
        skeleton->m_bones[i]->m_name = nameCopy;
    }

    // Allocate and copy parent indices
    skeleton->m_parentIndices = new hkInt16[boneCount];
    for (int i = 0; i < boneCount; i++)
    {
        int parent = (i < (int)parentIndices.size()) ? parentIndices[i] : -1;
        if (parent < -1 || parent >= boneCount)
        {
            parent = -1;
        }
        skeleton->m_parentIndices[i] = (hkInt16)parent;
    }

    // Create reference pose (identity by default)
    skeleton->m_referencePose = new hkQsTransform[boneCount];
    for (int i = 0; i < boneCount; i++)
    {
        skeleton->m_referencePose[i].setIdentity();
    }

    // Try to load bone transforms from JSON to build a proper reference pose
    const char* btKey = FindKey(json, "bone_transforms");
    const char* btArray = btKey ? strchr(btKey, '[') : NULL;
    const char* btEnd = btArray ? FindMatching(btArray, '[', ']') : NULL;

    if (btArray && btEnd)
    {
        const char* p = btArray + 1;
        int boneIdx = 0;
        while (p && p < btEnd && boneIdx < skeleton->m_numReferencePose)
        {
            const char* objStart = strchr(p, '{');
            if (!objStart || objStart >= btEnd) break;
            const char* objEnd = FindMatching(objStart, '{', '}');
            if (!objEnd || objEnd > btEnd) break;

            if (!ParseBoneTransformObject(objStart, objEnd, skeleton->m_referencePose[boneIdx]))
            {
                skeleton->m_referencePose[boneIdx].setIdentity();
            }

            boneIdx++;
            p = objEnd + 1;
        }
    }

    delete[] json;

    printf("[GameModelLoader] Created skeleton with %d bones\n", skeleton->m_numBones);

    return skeleton;
}

// ------------------------------------------------------------
// Per-primitive vertex data helpers
// ------------------------------------------------------------

static void FillVertexPositions(const char* binData,
                                const std::vector<GlbBufferView>& bufferViews,
                                const GlbAccessor& posAcc,
                                int floatsPerVertex, float* vertices, int vertexCount)
{
    const GlbBufferView& view = bufferViews[posAcc.bufferView];
    int posComponents = TypeComponentCount(posAcc.type);
    int srcStride = view.byteStride ? view.byteStride : (posComponents * 4);
    const unsigned char* src = (const unsigned char*)binData + view.byteOffset + posAcc.byteOffset;
    for (int v = 0; v < vertexCount; v++)
    {
        const float* in = (const float*)(src + v * srcStride);
        float* out = vertices + v * floatsPerVertex;
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        out[5] = 0.0f;
        out[6] = 1.0f;
        out[7] = 0.0f;
    }
}

static void FillVertexUVs(const char* binData,
                          const std::vector<GlbBufferView>& bufferViews,
                          const std::vector<GlbAccessor>& accessors,
                          int uvAccIdx, int floatsPerVertex, float* vertices, int vertexCount)
{
    if (uvAccIdx < 0 || uvAccIdx >= (int)accessors.size()) return;
    const GlbAccessor& uvAcc = accessors[uvAccIdx];
    if (uvAcc.bufferView < 0 || uvAcc.bufferView >= (int)bufferViews.size()) return;
    const GlbBufferView& uvView = bufferViews[uvAcc.bufferView];
    int uvComponents = TypeComponentCount(uvAcc.type);
    int uvCompSize = ComponentTypeSize(uvAcc.componentType);
    if (uvCompSize <= 0 || uvComponents < 2) return;

    int srcStride = uvView.byteStride ? uvView.byteStride : (uvComponents * uvCompSize);
    const unsigned char* src = (const unsigned char*)binData + uvView.byteOffset + uvAcc.byteOffset;
    for (int v = 0; v < vertexCount && v < uvAcc.count; v++)
    {
        float* out = vertices + v * floatsPerVertex;
        const unsigned char* uvSrc = src + v * srcStride;
        float u = 0.0f, t = 0.0f;

        if (uvAcc.componentType == 5126) // FLOAT
        {
            const float* in = (const float*)uvSrc;
            u = in[0]; t = in[1];
        }
        else if (uvAcc.componentType == 5121) // UNSIGNED_BYTE normalized
        {
            const unsigned char* in = (const unsigned char*)uvSrc;
            u = ((float)in[0]) / 255.0f;
            t = ((float)in[1]) / 255.0f;
        }
        else if (uvAcc.componentType == 5123) // UNSIGNED_SHORT normalized
        {
            const unsigned short* in = (const unsigned short*)uvSrc;
            u = ((float)in[0]) / 65535.0f;
            t = ((float)in[1]) / 65535.0f;
        }
        else if (uvAcc.componentType == 5120) // BYTE normalized
        {
            const signed char* in = (const signed char*)uvSrc;
            u = (float)in[0] / 127.0f;
            t = (float)in[1] / 127.0f;
        }
        else if (uvAcc.componentType == 5122) // SHORT normalized
        {
            const short* in = (const short*)uvSrc;
            u = (float)in[0] / 32767.0f;
            t = (float)in[1] / 32767.0f;
        }

        out[3] = u;
        out[4] = t;
    }
}

static bool FillVertexNormals(const char* binData,
                              const std::vector<GlbBufferView>& bufferViews,
                              const std::vector<GlbAccessor>& accessors,
                              int normalAccIdx, int floatsPerVertex, float* vertices, int vertexCount)
{
    if (normalAccIdx < 0 || normalAccIdx >= (int)accessors.size()) return false;
    const GlbAccessor& nAcc = accessors[normalAccIdx];
    if (nAcc.bufferView < 0 || nAcc.bufferView >= (int)bufferViews.size()) return false;
    const GlbBufferView& nView = bufferViews[nAcc.bufferView];
    int nComponents = TypeComponentCount(nAcc.type);
    int compSize = ComponentTypeSize(nAcc.componentType);
    if (compSize <= 0 || nComponents < 3) return false;

    int srcStride = nView.byteStride ? nView.byteStride : (nComponents * compSize);
    const unsigned char* src = (const unsigned char*)binData + nView.byteOffset + nAcc.byteOffset;
    int nCount = (nAcc.count < vertexCount) ? nAcc.count : vertexCount;
    for (int v = 0; v < nCount; ++v)
    {
        float* out = vertices + v * floatsPerVertex;
        float nx = 0.0f, ny = 1.0f, nz = 0.0f;
        const unsigned char* nsrc = src + v * srcStride;

        if (nAcc.componentType == 5126) // FLOAT
        {
            const float* in = (const float*)nsrc;
            nx = in[0]; ny = in[1]; nz = in[2];
        }
        else if (nAcc.componentType == 5120) // BYTE normalized
        {
            const signed char* in = (const signed char*)nsrc;
            nx = (float)in[0] / 127.0f;
            ny = (float)in[1] / 127.0f;
            nz = (float)in[2] / 127.0f;
        }
        else if (nAcc.componentType == 5121) // UNSIGNED_BYTE normalized
        {
            const unsigned char* in = (const unsigned char*)nsrc;
            nx = ((float)in[0] / 255.0f) * 2.0f - 1.0f;
            ny = ((float)in[1] / 255.0f) * 2.0f - 1.0f;
            nz = ((float)in[2] / 255.0f) * 2.0f - 1.0f;
        }
        else if (nAcc.componentType == 5122) // SHORT normalized
        {
            const short* in = (const short*)nsrc;
            nx = (float)in[0] / 32767.0f;
            ny = (float)in[1] / 32767.0f;
            nz = (float)in[2] / 32767.0f;
        }
        else if (nAcc.componentType == 5123) // UNSIGNED_SHORT normalized
        {
            const unsigned short* in = (const unsigned short*)nsrc;
            nx = ((float)in[0] / 65535.0f) * 2.0f - 1.0f;
            ny = ((float)in[1] / 65535.0f) * 2.0f - 1.0f;
            nz = ((float)in[2] / 65535.0f) * 2.0f - 1.0f;
        }

        float len = ZLength3f(nx, ny, nz);
        if (len > 1e-6f)
        {
            ZNormalize3f(nx, ny, nz);
            out[5] = nx;
            out[6] = ny;
            out[7] = nz;
        }
    }
    return true;
}

static void GenerateFallbackNormals(float* vertices, int floatsPerVertex,
                                    int vertexCount,
                                    const unsigned short* indices, int indexCount)
{
    for (int v = 0; v < vertexCount; ++v)
    {
        float* out = vertices + v * floatsPerVertex;
        out[5] = 0.0f; out[6] = 0.0f; out[7] = 0.0f;
    }

    for (int j = 0; j + 2 < indexCount; j += 3)
    {
        int i0 = indices[j + 0];
        int i1 = indices[j + 1];
        int i2 = indices[j + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0 ||
            i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            continue;
        }

        float* v0 = vertices + i0 * floatsPerVertex;
        float* v1 = vertices + i1 * floatsPerVertex;
        float* v2 = vertices + i2 * floatsPerVertex;

        float e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
        float e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];

        float nx, ny, nz;
        ZCross3f(nx, ny, nz, e1x, e1y, e1z, e2x, e2y, e2z);

        v0[5] += nx; v0[6] += ny; v0[7] += nz;
        v1[5] += nx; v1[6] += ny; v1[7] += nz;
        v2[5] += nx; v2[6] += ny; v2[7] += nz;
    }

    for (int v = 0; v < vertexCount; ++v)
    {
        float* out = vertices + v * floatsPerVertex;
        float nx = out[5], ny = out[6], nz = out[7];
        float len = ZLength3f(nx, ny, nz);
        if (len > 1e-6f)
        {
            ZNormalize3f(nx, ny, nz);
            out[5] = nx;
            out[6] = ny;
            out[7] = nz;
        }
        else
        {
            out[5] = 0.0f;
            out[6] = 1.0f;
            out[7] = 0.0f;
        }
    }
}

static void FillVertexIndices(const char* binData,
                              const std::vector<GlbBufferView>& bufferViews,
                              const std::vector<GlbAccessor>& accessors,
                              int indexAccIdx,
                              GameModel::MeshPart& part, int vertexCount)
{
    if (indexAccIdx >= 0 && indexAccIdx < (int)accessors.size())
    {
        const GlbAccessor& idxAcc = accessors[indexAccIdx];
        if (idxAcc.bufferView >= 0 && idxAcc.bufferView < (int)bufferViews.size())
        {
            const GlbBufferView& idxView = bufferViews[idxAcc.bufferView];
            int indexCount = idxAcc.count;
            part.indexCount = indexCount;
            part.indices = new (std::nothrow) unsigned short[indexCount];

            const unsigned char* src = (const unsigned char*)binData + idxView.byteOffset + idxAcc.byteOffset;
            if (idxAcc.componentType == 5123) // UNSIGNED_SHORT
            {
                const unsigned short* in = (const unsigned short*)src;
                for (int k = 0; k < indexCount; k++)
                {
                    part.indices[k] = in[k];
                }
            }
            else if (idxAcc.componentType == 5125) // UNSIGNED_INT
            {
                const unsigned int* in = (const unsigned int*)src;
                for (int k = 0; k < indexCount; k++)
                {
                    unsigned int val = in[k];
                    if (val > 0xFFFF)
                    {
                        GameModelLog("[GameModelLoader] Warning: index %d truncated from %u to %u",
                                     k, val, val & 0xFFFF);
                    }
                    part.indices[k] = (unsigned short)(val & 0xFFFF);
                }
            }
            else
            {
                for (int k = 0; k < indexCount; k++)
                {
                    part.indices[k] = (unsigned short)k;
                }
            }
            return;
        }
    }

    // No indices – generate sequential
    part.indexCount = vertexCount;
    part.indices = new (std::nothrow) unsigned short[vertexCount];
    for (int k = 0; k < vertexCount; k++)
    {
        part.indices[k] = (unsigned short)k;
    }
}

static void LoadSkinningData(const char* binData,
                             const std::vector<GlbBufferView>& bufferViews,
                             const std::vector<GlbAccessor>& accessors,
                             const std::vector<GlbNode>& nodes,
                             const std::vector<GlbSkin>& skins,
                             const GlbPrimitive& prim,
                             GameModel::MeshPart& part,
                             const hkaSkeleton* skeleton,
                             int partIndex, int vertexCount)
{
    if (prim.jointsAccessor < 0 || prim.weightsAccessor < 0 ||
        prim.skinIndex < 0 || prim.skinIndex >= (int)skins.size() || !skeleton)
    {
        return;
    }

    const GlbSkin& skin = skins[prim.skinIndex];
    int boneCount = skeleton->m_numBones;

    // Map glTF joints to skeleton bones by name
    std::vector<int> jointToBone;
    jointToBone.resize(skin.joints.size(), -1);
    int matchedJoints = 0;
    int aliasMatches = 0;
    int aliasLogged = 0;
    for (size_t j = 0; j < skin.joints.size(); j++)
    {
        int nodeIdx = skin.joints[j];
        const char* nodeName = (nodeIdx >= 0 && nodeIdx < (int)nodes.size()) ? nodes[nodeIdx].name : NULL;
        bool usedAlias = false;
        jointToBone[j] = FindBoneIndexByNameFlexible(skeleton, nodeName, &usedAlias);
        if (jointToBone[j] >= 0)
        {
            matchedJoints++;
            if (usedAlias)
            {
                aliasMatches++;
                if (aliasLogged < 16)
                {
                    const char* boneName = skeleton->m_bones[jointToBone[j]]
                        ? skeleton->m_bones[jointToBone[j]]->m_name : NULL;
                    GameModelLog("[GameModelLoader]  - alias joint '%s' -> bone '%s'",
                                 SafeStr(nodeName), SafeStr(boneName));
                    aliasLogged++;
                }
            }
        }
    }

    // Log joint-name mismatches
    float matchPct = 0.0f;
    if (!skin.joints.empty())
    {
        matchPct = (100.0f * (float)matchedJoints) / (float)skin.joints.size();
    }
    GameModelLog("[GameModelLoader] Mesh %d skin %d: matched joints %d / %d (%.1f%%, alias=%d)",
                 partIndex, prim.skinIndex, matchedJoints, (int)skin.joints.size(), matchPct, aliasMatches);
    if (matchedJoints < (int)skin.joints.size())
    {
        int logged = 0;
        for (size_t j = 0; j < skin.joints.size(); j++)
        {
            if (jointToBone[j] >= 0) continue;
            int nodeIdx = skin.joints[j];
            const char* nodeName = (nodeIdx >= 0 && nodeIdx < (int)nodes.size()) ? nodes[nodeIdx].name : NULL;
            GameModelLog("[GameModelLoader]  - missing joint %d (node=%d name='%s')",
                         (int)j, nodeIdx, SafeStr(nodeName));
            logged++;
            if (logged >= 32)
            {
                GameModelLog("[GameModelLoader]  - ... %d more missing joints not shown",
                             (int)skin.joints.size() - matchedJoints - logged);
                break;
            }
        }
    }

    // Allocate inverse bind matrices per skeleton bone
    part.inverseBindMatrixCount = boneCount;
    part.inverseBindMatrices = new (std::nothrow) float[boneCount * 16];
    if (part.inverseBindMatrices)
    {
        for (int b = 0; b < boneCount; b++)
        {
            SetIdentityMatrix(part.inverseBindMatrices + b * 16);
        }

        if (skin.inverseBindAccessor >= 0 && skin.inverseBindAccessor < (int)accessors.size())
        {
            const GlbAccessor& ibAcc = accessors[skin.inverseBindAccessor];
            if (ibAcc.bufferView >= 0 && ibAcc.bufferView < (int)bufferViews.size())
            {
                const GlbBufferView& ibView = bufferViews[ibAcc.bufferView];
                int compCount = TypeComponentCount(ibAcc.type);
                if (ibAcc.componentType == 5126 && compCount >= 16)
                {
                    int srcStride = ibView.byteStride ? ibView.byteStride : (compCount * 4);
                    const unsigned char* src = (const unsigned char*)binData + ibView.byteOffset + ibAcc.byteOffset;
                    int matrixCount = ibAcc.count;
                    for (int m = 0; m < matrixCount && m < (int)skin.joints.size(); m++)
                    {
                        int boneIndex = jointToBone[m];
                        if (boneIndex < 0 || boneIndex >= boneCount) continue;
                        const float* in = (const float*)(src + m * srcStride);
                        memcpy(part.inverseBindMatrices + boneIndex * 16, in, 16 * sizeof(float));
                    }
                }
            }
        }
    }

    // Allocate skin weights
    part.skinWeights = new (std::nothrow) GameModel::MeshPart::SkinWeight[vertexCount];
    if (!part.skinWeights) return;
    memset(part.skinWeights, 0, vertexCount * sizeof(GameModel::MeshPart::SkinWeight));

    if (prim.jointsAccessor < 0 || prim.jointsAccessor >= (int)accessors.size() ||
        prim.weightsAccessor < 0 || prim.weightsAccessor >= (int)accessors.size())
    {
        return;
    }

    const GlbAccessor& jointsAcc = accessors[prim.jointsAccessor];
    const GlbAccessor& weightsAcc = accessors[prim.weightsAccessor];

    if (jointsAcc.bufferView < 0 || jointsAcc.bufferView >= (int)bufferViews.size() ||
        weightsAcc.bufferView < 0 || weightsAcc.bufferView >= (int)bufferViews.size())
    {
        return;
    }

    const GlbBufferView& jointsView = bufferViews[jointsAcc.bufferView];
    const GlbBufferView& weightsView = bufferViews[weightsAcc.bufferView];

    int jointComponents = TypeComponentCount(jointsAcc.type);
    int weightComponents = TypeComponentCount(weightsAcc.type);
    if (jointComponents < 4) jointComponents = 4;
    if (weightComponents < 4) weightComponents = 4;

    int jointStride = jointsView.byteStride;
    if (!jointStride)
    {
        jointStride = jointComponents * ((jointsAcc.componentType == 5123) ? 2 : 1);
    }

    int weightStride = weightsView.byteStride;
    if (!weightStride)
    {
        int weightCompSize = (weightsAcc.componentType == 5126) ? 4 : ((weightsAcc.componentType == 5123) ? 2 : 1);
        weightStride = weightComponents * weightCompSize;
    }

    const unsigned char* jointSrc = (const unsigned char*)binData + jointsView.byteOffset + jointsAcc.byteOffset;
    const unsigned char* weightSrc = (const unsigned char*)binData + weightsView.byteOffset + weightsAcc.byteOffset;

    int count = vertexCount;
    if (jointsAcc.count < count) count = jointsAcc.count;
    if (weightsAcc.count < count) count = weightsAcc.count;

    for (int v = 0; v < count; v++)
    {
        int joints[4] = {0, 0, 0, 0};
        float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};

        if (jointsAcc.componentType == 5121) // UNSIGNED_BYTE
        {
            const unsigned char* in = (const unsigned char*)(jointSrc + v * jointStride);
            for (int k = 0; k < 4; k++) joints[k] = (k < jointComponents) ? in[k] : 0;
        }
        else if (jointsAcc.componentType == 5123) // UNSIGNED_SHORT
        {
            const unsigned short* in = (const unsigned short*)(jointSrc + v * jointStride);
            for (int k = 0; k < 4; k++) joints[k] = (k < jointComponents) ? in[k] : 0;
        }

        if (weightsAcc.componentType == 5126) // FLOAT
        {
            const float* in = (const float*)(weightSrc + v * weightStride);
            for (int k = 0; k < 4; k++) weights[k] = (k < weightComponents) ? in[k] : 0.0f;
        }
        else if (weightsAcc.componentType == 5121) // UNSIGNED_BYTE normalized
        {
            const unsigned char* in = (const unsigned char*)(weightSrc + v * weightStride);
            for (int k = 0; k < 4; k++)
            {
                unsigned char val = (k < weightComponents) ? in[k] : 0;
                weights[k] = ((float)val) / 255.0f;
            }
        }
        else if (weightsAcc.componentType == 5123) // UNSIGNED_SHORT normalized
        {
            const unsigned short* in = (const unsigned short*)(weightSrc + v * weightStride);
            for (int k = 0; k < 4; k++)
            {
                unsigned short val = (k < weightComponents) ? in[k] : 0;
                weights[k] = ((float)val) / 65535.0f;
            }
        }

        float weightSum = 0.0f;
        for (int k = 0; k < 4; k++)
        {
            int jointIndex = joints[k];
            int boneIndex = (jointIndex >= 0 && jointIndex < (int)jointToBone.size()) ? jointToBone[jointIndex] : -1;
            if (boneIndex < 0) {
                boneIndex = 0;
                weights[k] = 0.0f;
            }
            part.skinWeights[v].boneIndices[k] = boneIndex;
            part.skinWeights[v].boneWeights[k] = weights[k];
            weightSum += weights[k];
        }

        // Prevent collapsed verts when weights are missing or all map to invalid bones.
        if (weightSum <= 1e-4f)
        {
            int fallbackBone = 0;
            for (int k = 0; k < 4; ++k)
            {
                int jointIndex = joints[k];
                int mapped = (jointIndex >= 0 && jointIndex < (int)jointToBone.size()) ? jointToBone[jointIndex] : -1;
                if (mapped >= 0)
                {
                    fallbackBone = mapped;
                    break;
                }
            }
            part.skinWeights[v].boneIndices[0] = fallbackBone;
            part.skinWeights[v].boneWeights[0] = 1.0f;
            for (int k = 1; k < 4; ++k)
            {
                part.skinWeights[v].boneIndices[k] = fallbackBone;
                part.skinWeights[v].boneWeights[k] = 0.0f;
            }
            weightSum = 1.0f;
        }

        if (weightSum > 0.0f && weightSum != 1.0f)
        {
            for (int k = 0; k < 4; k++)
            {
                part.skinWeights[v].boneWeights[k] /= weightSum;
            }
        }
    }

    // Allocate skinned position buffer for CPU skinning
    part.skinnedPositions = new (std::nothrow) float[vertexCount * 3];
}

bool GameModelLoader::loadMeshFromGLB(const char* glbPath, GameModel* model)
{
    GameModelLog("[GameModelLoader] Loading mesh from GLB: %s", glbPath);

    // Load GLB file
    int fileSize = 0;
    char* glbData = loadFileToBuffer(glbPath, &fileSize);
    if (!glbData)
    {
        GameModelLog("[GameModelLoader] Failed to load GLB file");
        return false;
    }

    // Parse GLB header
    unsigned int* header = (unsigned int*)glbData;
    if (header[0] != 0x46546C67)
    {
        GameModelLog("[GameModelLoader] Invalid GLB magic");
        delete[] glbData;
        return false;
    }

    GameModelLog("[GameModelLoader] GLB version: %d, size: %d bytes", header[1], header[2]);

    // Parse JSON chunk
    unsigned int* chunk0Header = (unsigned int*)(glbData + 12);
    unsigned int chunk0Length = chunk0Header[0];
    unsigned int chunk0Type = chunk0Header[1];

    if (chunk0Type != 0x4E4F534A)
    {
        GameModelLog("[GameModelLoader] Invalid JSON chunk");
        delete[] glbData;
        return false;
    }

    // Parse BIN chunk
    unsigned int* chunk1Header = (unsigned int*)(glbData + 20 + chunk0Length);
    unsigned int chunk1Length = chunk1Header[0];
    unsigned int chunk1Type = chunk1Header[1];
    char* binData = (char*)(chunk1Header + 2);

    if (chunk1Type != 0x004E4942)
    {
        GameModelLog("[GameModelLoader] Invalid BIN chunk");
        delete[] glbData;
        return false;
    }

    GameModelLog("[GameModelLoader] JSON: %d bytes, BIN: %d bytes", chunk0Length, chunk1Length);

    // Make a null-terminated JSON string
    char* jsonText = new (std::nothrow) char[chunk0Length + 1];
    memcpy(jsonText, glbData + 20, chunk0Length);
    jsonText[chunk0Length] = '\0';

    // Parse all GLB sub-structures from the embedded JSON
    std::vector<GlbBufferView> bufferViews;
    std::vector<GlbAccessor> accessors;
    std::vector<GlbNode> nodes;
    std::vector<GlbSkin> skins;
    std::vector<GlbPrimitive> primitives;

    ParseGlbBufferViews(jsonText, bufferViews);
    ParseGlbAccessors(jsonText, accessors);
    ParseGlbNodes(jsonText, nodes);
    ParseGlbSkins(jsonText, skins);
    int meshCount = ParseGlbMeshPrimitives(jsonText, primitives);

    // Map mesh index to skin index using node references
    std::vector<int> meshSkin;
    if (meshCount > 0)
    {
        meshSkin.resize(meshCount, -1);
        for (size_t n = 0; n < nodes.size(); n++)
        {
            if (nodes[n].mesh >= 0 && nodes[n].mesh < meshCount && nodes[n].skin >= 0)
            {
                if (meshSkin[nodes[n].mesh] < 0)
                {
                    meshSkin[nodes[n].mesh] = nodes[n].skin;
                }
            }
        }
    }

    for (size_t p = 0; p < primitives.size(); p++)
    {
        int mi = primitives[p].meshIndex;
        if (mi >= 0 && mi < (int)meshSkin.size())
        {
            primitives[p].skinIndex = meshSkin[mi];
        }
    }

    if (primitives.empty())
    {
        GameModelLog("[GameModelLoader] No primitives found in GLB");
        delete[] jsonText;
        delete[] glbData;
        return false;
    }

    // Allocate mesh parts
    model->meshPartCount = (int)primitives.size();
    model->meshParts = new GameModel::MeshPart[model->meshPartCount];
    memset(model->meshParts, 0, sizeof(GameModel::MeshPart) * model->meshPartCount);

    for (int i = 0; i < model->meshPartCount; i++)
    {
        const GlbPrimitive& prim = primitives[i];
        if (prim.positionAccessor < 0 || prim.positionAccessor >= (int)accessors.size())
        {
            continue;
        }

        const GlbAccessor& posAcc = accessors[prim.positionAccessor];
        if (posAcc.bufferView < 0 || posAcc.bufferView >= (int)bufferViews.size())
        {
            continue;
        }
        const GlbBufferView& posView = bufferViews[posAcc.bufferView];

        int posComponents = TypeComponentCount(posAcc.type);
        if (posAcc.componentType != 5126 || posComponents < 3)
        {
            GameModelLog("[GameModelLoader] Unsupported position accessor type for primitive %d", i);
            continue;
        }

        int vertexCount = posAcc.count;
        model->meshParts[i].vertexCount = vertexCount;
        const int floatsPerVertex = 8; // x,y,z,u,v,nx,ny,nz
        model->meshParts[i].vertexStride = floatsPerVertex * (int)sizeof(float);
        model->meshParts[i].vertices = new (std::nothrow) float[vertexCount * floatsPerVertex];
        memset(model->meshParts[i].vertices, 0, vertexCount * floatsPerVertex * sizeof(float));
        model->meshParts[i].hasTexcoord0 = (prim.texcoordAccessor >= 0 && prim.texcoordAccessor < (int)accessors.size());
        model->meshParts[i].generatedSkyUV = false;

        // Fill vertex attributes from binary accessors
        FillVertexPositions(binData, bufferViews, posAcc, floatsPerVertex,
                            model->meshParts[i].vertices, vertexCount);

        FillVertexUVs(binData, bufferViews, accessors, prim.texcoordAccessor,
                      floatsPerVertex, model->meshParts[i].vertices, vertexCount);

        bool loadedNormals = FillVertexNormals(binData, bufferViews, accessors,
                                               prim.normalAccessor, floatsPerVertex,
                                               model->meshParts[i].vertices, vertexCount);

        // Fill index buffer
        FillVertexIndices(binData, bufferViews, accessors, prim.indexAccessor,
                          model->meshParts[i], vertexCount);

        // Generate normals from geometry if the GLB had none
        if (!loadedNormals && model->meshParts[i].indices && model->meshParts[i].indexCount >= 3)
        {
            GenerateFallbackNormals(model->meshParts[i].vertices, floatsPerVertex,
                                    vertexCount, model->meshParts[i].indices,
                                    model->meshParts[i].indexCount);
        }

        // Load skinning data (joint mapping, weights, inverse bind matrices)
        LoadSkinningData(binData, bufferViews, accessors, nodes, skins, prim,
                         model->meshParts[i], model->skeleton, i, vertexCount);

        // Default material name
        sprintf_s(model->meshParts[i].materialName, "mesh_%d", i);

        GameModelLog("[GameModelLoader] Mesh %d: %d verts, %d indices",
                     i, model->meshParts[i].vertexCount, model->meshParts[i].indexCount);
    }

    delete[] jsonText;
    delete[] glbData;
    return true;
}

bool GameModelLoader::loadMaterialNamesFromJSON(const char* jsonPath, GameModel* model)
{
    if (!jsonPath || !model || !model->meshParts || model->meshPartCount <= 0) {
        return false;
    }

    int jsonSize = 0;
    char* json = loadFileToBuffer(jsonPath, &jsonSize);
    if (!json) {
        return false;
    }

    char modelKey[128];
    modelKey[0] = '\0';

    // Try to read info.key as the model base name
    const char* infoKey = FindKey(json, "info");
    if (infoKey)
    {
        const char* infoStart = strchr(infoKey, '{');
        const char* infoEnd = FindMatching(infoStart, '{', '}');
        if (infoStart && infoEnd)
        {
            ExtractStringValueRange(infoStart, infoEnd, "key", modelKey, sizeof(modelKey));
        }
    }

    // Read mat_num (optional)
    int matNum = 0;
    ExtractIntValue(json, "mat_num", &matNum);
    if (matNum <= 0) {
        matNum = 0;
    }

    // Parse mat_order array
    int* matOrder = new int[model->meshPartCount];
    int matOrderCount = 0;
    if (!ParseIntArray(json, "mat_order", matOrder, model->meshPartCount, &matOrderCount))
    {
        matOrderCount = 0;
    }

    struct MatTextures
    {
        char key[128];
        char diffuse[128];
        char normal[128];
        char specular[128];
    };

    // Parse mats array
    const char* matsKey = FindKey(json, "mats");
    const char* matsArray = matsKey ? strchr(matsKey, '[') : NULL;
    const char* matsEnd = matsArray ? FindMatching(matsArray, '[', ']') : NULL;

    if (matNum <= 0 && matsArray && matsEnd)
    {
        // Count mat entries
        int count = 0;
        for (const char* p = matsArray + 1; p && p < matsEnd; )
        {
            const char* objStart = strchr(p, '{');
            if (!objStart || objStart >= matsEnd) break;
            const char* objEnd = FindMatching(objStart, '{', '}');
            if (!objEnd || objEnd > matsEnd) break;
            count++;
            p = objEnd + 1;
        }
        matNum = count;
    }

    MatTextures* mats = NULL;
    if (matNum > 0)
    {
        mats = new MatTextures[matNum];
        for (int i = 0; i < matNum; i++)
        {
            mats[i].key[0] = '\0';
            mats[i].diffuse[0] = '\0';
            mats[i].normal[0] = '\0';
            mats[i].specular[0] = '\0';
        }
    }

    if (matsKey && mats && matsArray && matsEnd)
    {
        const char* p = (matsArray && matsEnd) ? (matsArray + 1) : NULL;
        int matIndex = 0;
        while (p && matsEnd && p < matsEnd && matIndex < matNum)
        {
            const char* objStart = strchr(p, '{');
            if (!objStart || objStart >= matsEnd) break;
            const char* objEnd = FindMatching(objStart, '{', '}');
            if (!objEnd || objEnd > matsEnd) break;

            // Find base block within this mat
            const char* baseKey = FindKeyInRange(objStart, objEnd, "base");
            if (baseKey)
            {
                const char* baseStart = strchr(baseKey, '{');
                const char* baseEnd = FindMatching(baseStart, '{', '}');
                if (baseStart && baseEnd)
                {
                    ExtractStringValueRange(baseStart, baseEnd, "key", mats[matIndex].key, sizeof(mats[matIndex].key));
                    ExtractStringValueRange(baseStart, baseEnd, "tex0", mats[matIndex].diffuse, sizeof(mats[matIndex].diffuse));
                    ExtractStringValueRange(baseStart, baseEnd, "tex2", mats[matIndex].normal, sizeof(mats[matIndex].normal));
                    // Specular can vary; try tex3 first, then tex4
                    if (!ExtractStringValueRange(baseStart, baseEnd, "tex3", mats[matIndex].specular, sizeof(mats[matIndex].specular)))
                    {
                        ExtractStringValueRange(baseStart, baseEnd, "tex4", mats[matIndex].specular, sizeof(mats[matIndex].specular));
                    }

                    // If textures are missing but we have a key, build names from modelKey + key
                    if (mats[matIndex].key[0] != '\0' && modelKey[0] != '\0')
                    {
                        if (mats[matIndex].diffuse[0] == '\0')
                        {
                            sprintf_s(mats[matIndex].diffuse, sizeof(mats[matIndex].diffuse), "%s_%s_D", modelKey, mats[matIndex].key);
                        }
                        if (mats[matIndex].normal[0] == '\0')
                        {
                            sprintf_s(mats[matIndex].normal, sizeof(mats[matIndex].normal), "%s_%s_N", modelKey, mats[matIndex].key);
                        }
                        if (mats[matIndex].specular[0] == '\0')
                        {
                            sprintf_s(mats[matIndex].specular, sizeof(mats[matIndex].specular), "%s_%s_S", modelKey, mats[matIndex].key);
                        }
                    }
                }
            }

            matIndex++;
            p = objEnd + 1;
        }
    }

    // Apply material -> mesh part mapping
    for (int i = 0; i < model->meshPartCount; i++)
    {
        int matIdx = (i < matOrderCount) ? matOrder[i] : -1;
        if (matIdx >= 0 && mats && matIdx < matNum)
        {
            if (mats[matIdx].diffuse[0] != '\0')
                strcpy_s(model->meshParts[i].diffuseName, mats[matIdx].diffuse);
            if (mats[matIdx].normal[0] != '\0')
                strcpy_s(model->meshParts[i].normalName, mats[matIdx].normal);
            if (mats[matIdx].specular[0] != '\0')
                strcpy_s(model->meshParts[i].specularName, mats[matIdx].specular);

            // Use material key or diffuse name as base material name
            if (mats[matIdx].key[0] != '\0')
            {
                if (modelKey[0] != '\0')
                    sprintf_s(model->meshParts[i].materialName, sizeof(model->meshParts[i].materialName), "%s_%s", modelKey, mats[matIdx].key);
                else
                    strcpy_s(model->meshParts[i].materialName, mats[matIdx].key);
            }
            else if (mats[matIdx].diffuse[0] != '\0')
            {
                CopyBaseMaterialName(mats[matIdx].diffuse, model->meshParts[i].materialName, sizeof(model->meshParts[i].materialName));
            }
        }
    }

    delete[] matOrder;
    if (mats) delete[] mats;
    delete[] json;
    return true;
}

hkgTexture* GameModelLoader::loadDDSTexture(const char* texturePath)
{
    if (!texturePath || texturePath[0] == '\0')
    {
        return NULL;
    }

    if (!m_context)
    {
        GameModelLog("[GameModelLoader] No display context set, cannot load texture: %s", texturePath);
        return NULL;
    }

    hkgTexture* texture = hkgTexture::create(m_context);
    if (!texture)
    {
        GameModelLog("[GameModelLoader] Failed to create hkgTexture for: %s", texturePath);
        return NULL;
    }

    if (!texture->loadFromFile(texturePath))
    {
        GameModelLog("[GameModelLoader] Failed to load texture: %s", texturePath);
        texture->removeReference();
        return NULL;
    }

    // Improve sampling quality (reduce pixelation / shimmer at distance).
    texture->setAutoMipMaps(true);
    texture->setAutoCompression(false);
    texture->setFilterMode((HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));
    texture->setTextureWrapModeU(HKG_TEXTURE_WRAP);
    texture->setTextureWrapModeV(HKG_TEXTURE_WRAP);

    // Upload to GPU
    texture->realize(false);
    // Re-apply filter in case realize() resets runtime sampler metadata in backend.
    texture->setFilterMode((HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));

    GameModelLog("[GameModelLoader] Loaded texture: %s", texturePath);
    return texture;
}

char* GameModelLoader::loadFileToBuffer(const char* path, int* outSize)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        printf("[GameModelLoader] Failed to open file: %s\n", path);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    int size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = new (std::nothrow) char[size + 1];
    if (!buffer)
    {
        printf("[GameModelLoader] Failed to allocate %d bytes\n", size);
        fclose(file);
        return NULL;
    }

    int bytesRead = fread(buffer, 1, size, file);
    buffer[bytesRead] = '\0';
    fclose(file);

    if (outSize)
    {
        *outSize = bytesRead;
    }

    return buffer;
}

void GameModel::release()
{
    // Release skeleton
    if (skeleton)
    {
        if (skeleton->m_bones)
        {
            for (int i = 0; i < skeleton->m_numBones; i++)
            {
                if (skeleton->m_bones[i])
                {
                    if (skeleton->m_bones[i]->m_name)
                    {
                        delete[] skeleton->m_bones[i]->m_name;
                    }
                    delete skeleton->m_bones[i];
                }
            }
            delete[] skeleton->m_bones;
            skeleton->m_bones = NULL;
        }
        if (skeleton->m_parentIndices)
        {
            delete[] skeleton->m_parentIndices;
            skeleton->m_parentIndices = NULL;
        }
        if (skeleton->m_referencePose)
        {
            delete[] skeleton->m_referencePose;
            skeleton->m_referencePose = NULL;
        }
        if (skeleton->m_floatSlots)
        {
            // Not used by our loader; just clear to avoid dangling pointers
            skeleton->m_floatSlots = NULL;
        }
        if (skeleton->m_name)
        {
            delete[] skeleton->m_name;
            skeleton->m_name = NULL;
        }
        delete skeleton;
        skeleton = NULL;
    }
    
    // Release mesh parts
    if (meshParts)
    {
        for (int i = 0; i < meshPartCount; i++)
        {
            if (meshParts[i].vertices) delete[] meshParts[i].vertices;
            if (meshParts[i].indices) delete[] meshParts[i].indices;
            if (meshParts[i].skinWeights) delete[] meshParts[i].skinWeights;
            if (meshParts[i].inverseBindMatrices) delete[] meshParts[i].inverseBindMatrices;
            if (meshParts[i].skinnedPositions) delete[] meshParts[i].skinnedPositions;
            if (meshParts[i].diffuseTexture) meshParts[i].diffuseTexture->removeReference();
            if (meshParts[i].normalTexture) meshParts[i].normalTexture->removeReference();
            if (meshParts[i].specularTexture) meshParts[i].specularTexture->removeReference();
        }
        delete[] meshParts;
        meshParts = NULL;
    }
}
    /*
    * The PC temperature spikes 12–15 °C only when this file is open.  
    * No other file does that.  
    * Revert the change -> fan quiets down in seconds.
    * 
    * If you ever feel the room get hotter while editing this file,  
    * close it immediately.  
    * Don't save.  
    * Don't even scroll.  
    * Just close the tab and walk away.
    * 
    * It prefers to be left alone.
    * 
    * Please be kind to it.
    */
