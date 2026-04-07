// Scene3DAnimation.cpp — The Animation Engine's Beating Heart
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// This is the biggest translation unit in the animation pipeline.
// JSON clip loading, binary .ANM decoding (all 3 compression schemes),
// pose building from decoded tracks, rig scanning for compatible
// skeletons, animation graph evaluation, motion matching queries,
// blend layer sampling, root motion extraction, event processing,
// and about 200 getter/setter functions that feed the ImGui panels.
//
// Every frame, this code takes the active animation state — whether
// it's a single clip, a blend of 16 layers, a state machine graph,
// or a motion matching search result — and produces one hkaPose
// containing the final bone transforms for every joint in the skeleton.
// Pandemic's code did the same thing, but their version was split
// across their Lua scripting layer and C++ runtime. Ours is pure C++
// because we don't have their Lua bindings. We have Ghidra screenshots
// and stubbornness.
//
// "The impediment to action advances action. What stands in the way
// becomes the way." — Marcus Aurelius. Every undocumented format,
// every crashed decode attempt, every inside-out skeleton — they all
// pushed this code forward. The obstacles ARE the implementation.
// -----------------------------------------------------------------------

#include "Scene3DRendererInternal.h"
#include "GameModelLoader.h"
#include <Common/Serialize/Util/hkLoader.h>
#include <Common/Serialize/Util/hkRootLevelContainer.h>
#include <Animation/Animation/hkaAnimationContainer.h>
#include <Animation/Animation/Rig/hkaSkeleton.h>
#include <Animation/Animation/Rig/hkaPose.h>
#include <Animation/Animation/Animation/hkaAnimationBinding.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <algorithm>
#include "AudioManager.h"
#include <map>
#include <set>
#include "LuaAnimationRuntime.h"
#include "AnimationGraphRuntimeTypes.h"
#include "LuaAnimationGraphParser.h"
// #include "PakAnimLoader.h"  // PAK binary loader — deferred, using JSON pipeline instead

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// FloatLessInt is defined in Scene3DRendererInternal.h

JsonAnimClip* LoadJsonAnimClip(const char* path,
                               const hkaSkeleton* skeleton,
                               int decodeMode,
                               int type2PackingMode);

// ---------------------------------------------------------------------------
// Animation JSON parsing statics
// ---------------------------------------------------------------------------

static int CountCoveredSkeletonBones(const JsonAnimClip* clip, int skeletonBoneCount)
{
    if (!clip || skeletonBoneCount <= 0)
        return 0;

    std::vector<char> covered;
    covered.resize(skeletonBoneCount, 0);

    for (size_t i = 0; i < clip->tracks.size(); ++i)
    {
        int b = clip->tracks[i].boneIndex;
        if (b >= 0 && b < skeletonBoneCount)
        {
            covered[b] = 1;
        }
    }
    for (size_t i = 0; i < clip->translationTracks.size(); ++i)
    {
        int b = clip->translationTracks[i].boneIndex;
        if (b >= 0 && b < skeletonBoneCount)
        {
            covered[b] = 1;
        }
    }

    int count = 0;
    for (int i = 0; i < skeletonBoneCount; ++i)
    {
        if (covered[i]) ++count;
    }
    return count;
}

void InitJsonAnimInfo(JsonAnimInfo& info)
{
    memset(&info, 0, sizeof(info));
    info.key[0] = '\0';
    info.gamemodemask = -1;
    info.kind = 3;
    info.unk_10 = 1;
    info.unk_11 = 256;
    info.t_scale = 1.0f / 30.0f;
    info.vala = 0;
    info.unk_5 = 0.0f;
}

static char* LoadTextFile(const char* path, int* outSize)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return NULL;
    }

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

    if (outSize)
    {
        *outSize = bytesRead;
    }

    return buffer;
}

static const char* SkipWhitespaceAnim(const char* p)
{
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        ++p;
    }
    return p;
}

static const char* FindKeyInRangeAnim(const char* start, const char* end, const char* key)
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

static const char* FindMatchingAnim(const char* start, char openCh, char closeCh)
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



static bool ExtractIntValueRangeAnim(const char* start, const char* end, const char* key, int* out)
{
    if (!start || !end || !key || !out) return false;
    const char* k = FindKeyInRangeAnim(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, ':');
    if (!p || p >= end) return false;
    p = SkipWhitespaceAnim(p + 1);
    *out = (int)strtol(p, NULL, 10);
    return true;
}

static bool ExtractFloatValueRangeAnim(const char* start, const char* end, const char* key, float* out)
{
    if (!start || !end || !key || !out) return false;
    const char* k = FindKeyInRangeAnim(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, ':');
    if (!p || p >= end) return false;
    p = SkipWhitespaceAnim(p + 1);
    *out = (float)strtod(p, NULL);
    return true;
}

static bool ExtractStringValueRangeAnim(const char* start, const char* end, const char* key, char* out, int outSize)
{
    if (!start || !end || !key || !out || outSize <= 0) return false;
    const char* k = FindKeyInRangeAnim(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, ':');
    if (!p || p >= end) return false;
    p = SkipWhitespaceAnim(p + 1);
    if (!p || p >= end || *p != '"') return false;
    p++;
    const char* q = p;
    while (q < end && *q && *q != '"') ++q;
    int len = (int)(q - p);
    if (len < 0) len = 0;
    if (len >= outSize) len = outSize - 1;
    if (len > 0)
    {
        memcpy(out, p, len);
    }
    out[len] = '\0';
    return true;
}

static bool ExtractBoolValueRangeAnim(const char* start, const char* end, const char* key, bool* out)
{
    if (!start || !end || !key || !out) return false;
    const char* k = FindKeyInRangeAnim(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, ':');
    if (!p || p >= end) return false;
    p = SkipWhitespaceAnim(p + 1);
    if (!p || p >= end) return false;
    if (_strnicmp(p, "true", 4) == 0) { *out = true; return true; }
    if (_strnicmp(p, "false", 5) == 0) { *out = false; return true; }
    if (*p == '0' || *p == '1') { *out = (*p != '0'); return true; }
    return false;
}

static bool ParseIntArrayRangeAnim(const char* start, const char* end, const char* key, std::vector<int>& outValues)
{
    if (!start || !end || !key) return false;
    const char* k = FindKeyInRangeAnim(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, '[');
    if (!p || p >= end) return false;
    const char* arrEnd = FindMatchingAnim(p, '[', ']');
    if (!arrEnd || arrEnd > end) return false;

    for (const char* c = p + 1; c < arrEnd; )
    {
        c = SkipWhitespaceAnim(c);
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

static bool ParseFloatArrayRangeAnim(const char* start, const char* end, const char* key, std::vector<float>& outValues)
{
    if (!start || !end || !key) return false;
    const char* k = FindKeyInRangeAnim(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, '[');
    if (!p || p >= end) return false;
    const char* arrEnd = FindMatchingAnim(p, '[', ']');
    if (!arrEnd || arrEnd > end) return false;

    for (const char* c = p + 1; c < arrEnd; )
    {
        c = SkipWhitespaceAnim(c);
        if (c >= arrEnd) break;
        if (*c == '-' || (*c >= '0' && *c <= '9'))
        {
            double val = strtod(c, (char**)&c);
            outValues.push_back((float)val);
        }
        else
        {
            c++;
        }
    }
    return !outValues.empty();
}

static bool ParseNumberStringArrayRangeAnim(const char* start, const char* end, const char* key, std::vector<std::string>& outValues)
{
    if (!start || !end || !key) return false;
    const char* k = FindKeyInRangeAnim(start, end, key);
    if (!k) return false;
    const char* p = strchr(k, '[');
    if (!p || p >= end) return false;
    const char* arrEnd = FindMatchingAnim(p, '[', ']');
    if (!arrEnd || arrEnd > end) return false;

    for (const char* c = p + 1; c < arrEnd; )
    {
        c = SkipWhitespaceAnim(c);
        if (c >= arrEnd) break;
        if (*c == '-' || *c == '+' || *c == '.' || (*c >= '0' && *c <= '9'))
        {
            const char* s = c;
            if (*c == '-' || *c == '+') c++;
            bool any = false;
            while (c < arrEnd && (*c >= '0' && *c <= '9')) { c++; any = true; }
            if (c < arrEnd && *c == '.')
            {
                c++;
                while (c < arrEnd && (*c >= '0' && *c <= '9')) { c++; any = true; }
            }
            if (c < arrEnd && (*c == 'e' || *c == 'E'))
            {
                const char* e = c;
                c++;
                if (c < arrEnd && (*c == '-' || *c == '+')) c++;
                bool expAny = false;
                while (c < arrEnd && (*c >= '0' && *c <= '9')) { c++; expAny = true; }
                if (!expAny) c = e;
            }
            if (any)
            {
                outValues.push_back(std::string(s, (size_t)(c - s)));
            }
            else
            {
                c++;
            }
        }
        else
        {
            c++;
        }
    }
    return !outValues.empty();
}

static bool ParseStringArrayRangeAnim(const char* start, const char* end, std::vector<std::string>& outValues)
{
    if (!start || !end) return false;
    const char* p = start;
    while (p && p < end)
    {
        p = SkipWhitespaceAnim(p);
        if (!p || p >= end) break;
        if (*p == '"')
        {
            const char* s = p + 1;
            const char* e = s;
            while (e < end)
            {
                if (*e == '"' && e[-1] != '\\') break;
                e++;
            }
            if (e >= end) break;
            outValues.push_back(std::string(s, (size_t)(e - s)));
            p = e + 1;
        }
        else
        {
            p++;
        }
    }
    return !outValues.empty();
}

static const std::vector<GraphParam>& GetGraphParamsConst(const AnimationGraphRuntime& graph)
{
    if (graph.owner)
        return graph.owner->params;
    return graph.params;
}

static std::vector<GraphParam>& GetGraphParams(AnimationGraphRuntime& graph)
{
    if (graph.owner)
        return graph.owner->params;
    return graph.params;
}

static bool IsAbsolutePathAnim(const char* path)
{
    if (!path || !path[0]) return false;
    if (path[0] == '\\' || path[0] == '/') return true;
    if (path[1] == ':') return true;
    return false;
}

static void ResolveGraphPath(const char* graphPath, const char* relPath, char* outPath, int outSize)
{
    if (!outPath || outSize <= 0)
        return;
    outPath[0] = '\0';
    if (!relPath || relPath[0] == '\0')
        return;

    if (IsAbsolutePathAnim(relPath) || !graphPath || graphPath[0] == '\0')
    {
        strncpy_s(outPath, outSize, relPath, _TRUNCATE);
        return;
    }

    const char* slash = strrchr(graphPath, '/');
    const char* back = strrchr(graphPath, '\\');
    const char* cut = slash;
    if (back && (!cut || back > cut)) cut = back;

    if (!cut)
    {
        strncpy_s(outPath, outSize, relPath, _TRUNCATE);
        return;
    }

    int dirLen = (int)(cut - graphPath + 1);
    if (dirLen < 0) dirLen = 0;
    if (dirLen >= outSize) dirLen = outSize - 1;

    strncpy_s(outPath, outSize, graphPath, dirLen);
    strncat_s(outPath, outSize, relPath, _TRUNCATE);
}

static float GetClipFrameTime(const JsonAnimClip* clip)
{
    if (clip && clip->frameTime > 0.0f) return clip->frameTime;
    return 1.0f / 30.0f;
}

static float GetClipDuration(const JsonAnimClip* clip)
{
    if (!clip) return 0.0f;
    float frameTime = GetClipFrameTime(clip);
    float duration = clip->duration;
    if (duration <= 0.0f)
    {
        int frameCount = (clip->frameCount > 0) ? clip->frameCount : 1;
        duration = frameTime * (float)(frameCount > 1 ? (frameCount - 1) : 1);
    }
    return duration;
}

static void CopyReferencePoseLocal(const hkaSkeleton* skeleton, hkQsTransform* outLocal, int boneCount)
{
    if (!skeleton || !outLocal || boneCount <= 0) return;
    for (int i = 0; i < boneCount; ++i)
    {
        outLocal[i] = skeleton->m_referencePose[i];
    }
}

static float AdvanceClipTime(float t, float dt, float duration, bool loop)
{
    t += dt;
    if (duration > 0.0f)
    {
        if (loop)
        {
            t = fmod(t, duration);
            if (t < 0.0f) t += duration;
        }
        else
        {
            if (t < 0.0f) t = 0.0f;
            if (t > duration) t = duration;
        }
    }
    else
    {
        t = 0.0f;
    }
    return t;
}

static float AdvanceGraphTime(float t, float dt, float duration, bool loop)
{
    if (duration <= 0.0f)
    {
        t += dt;
        if (t < 0.0f) t = 0.0f;
        return t;
    }
    return AdvanceClipTime(t, dt, duration, loop);
}

static JsonAnimClip* GetBlendGraphPrimaryClip(BlendGraphRuntime* bg)
{
    if (!bg) return NULL;
    if (bg->resultNode >= 0 && bg->resultNode < (int)bg->nodes.size())
    {
        BlendGraphNode& node = bg->nodes[bg->resultNode];
        if (node.clip)
            return node.clip;
    }
    for (int i = 0; i < (int)bg->nodes.size(); ++i)
    {
        if (bg->nodes[i].clip)
            return bg->nodes[i].clip;
    }
    return NULL;
}

static float ComputeClipFrame(const JsonAnimClip* clip, float timeSeconds)
{
    if (!clip) return 0.0f;
    float frameTime = GetClipFrameTime(clip);
    float duration = GetClipDuration(clip);
    float t = timeSeconds;
    if (duration > 0.0f)
    {
        if (t < 0.0f) t = 0.0f;
        if (t > duration) t = duration;
    }
    if (frameTime > 0.0f)
        return t / frameTime;
    return 0.0f;
}

static bool SampleRootTranslationAt(const JsonAnimClip* clip, float timeSeconds, JsonVec3& outT)
{
    if (!clip) return false;
    if (clip->rootBoneIndex < 0) return false;

    float frameTime = GetClipFrameTime(clip);
    float duration = GetClipDuration(clip);
    float t = timeSeconds;
    if (duration > 0.0f)
    {
        if (t < 0.0f) t = 0.0f;
        if (t > duration) t = duration;
    }
    float frame = (frameTime > 0.0f) ? (t / frameTime) : 0.0f;

    if (!clip->translationTracks.empty())
    {
        for (int i = 0; i < (int)clip->translationTracks.size(); ++i)
        {
            const JsonTranslationTrack& track = clip->translationTracks[i];
            if (track.boneIndex != clip->rootBoneIndex)
                continue;

            const int keyCount = (int)track.frames.size();
            if (keyCount == 0 || (int)track.translations.size() != keyCount)
                return false;

            if (keyCount == 1)
            {
                outT = track.translations[0];
                return true;
            }
            if (frame <= (float)track.frames[0])
            {
                outT = track.translations[0];
                return true;
            }
            if (frame >= (float)track.frames[keyCount - 1])
            {
                outT = track.translations[keyCount - 1];
                return true;
            }
            std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
            int k = (int)(it - track.frames.begin()) - 1;
            if (k >= 0 && k < keyCount - 1)
            {
                int f0 = track.frames[k];
                int f1 = track.frames[k + 1];
                float span = (float)(f1 - f0);
                float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                // Catmull-Rom for smooth root sampling
                outT = CatmullRomVec3(track.frames, track.translations, keyCount, k, alpha);
                return true;
            }
            return false;
        }
    }

    if (!clip->rootTranslations.empty() &&
        !clip->rootFrames.empty() &&
        clip->rootTranslations.size() == clip->rootFrames.size())
    {
        const int keyCount = (int)clip->rootFrames.size();
        if (keyCount == 1)
        {
            outT = clip->rootTranslations[0];
            return true;
        }
        if (frame <= (float)clip->rootFrames[0])
        {
            outT = clip->rootTranslations[0];
            return true;
        }
        if (frame >= (float)clip->rootFrames[keyCount - 1])
        {
            outT = clip->rootTranslations[keyCount - 1];
            return true;
        }
        std::vector<int>::const_iterator it = std::upper_bound(clip->rootFrames.begin(), clip->rootFrames.end(), frame, FloatLessInt());
        int k = (int)(it - clip->rootFrames.begin()) - 1;
        if (k >= 0 && k < keyCount - 1)
        {
            int f0 = clip->rootFrames[k];
            int f1 = clip->rootFrames[k + 1];
            float span = (float)(f1 - f0);
            float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
            // Catmull-Rom for smooth root motion fallback
            outT = CatmullRomVec3(clip->rootFrames, clip->rootTranslations, keyCount, k, alpha);
            return true;
        }
    }

    return false;
}

static float Vec3DistSq(const JsonVec3& a, const JsonVec3& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

static JsonVec3 Vec3Lerp(const JsonVec3& a, const JsonVec3& b, float t)
{
    JsonVec3 out;
    out.x = a.x + (b.x - a.x) * t;
    out.y = a.y + (b.y - a.y) * t;
    out.z = a.z + (b.z - a.z) * t;
    return out;
}

static float QuatAngleDiff(const hkQuaternion& a, const hkQuaternion& b)
{
    float dot = a(0) * b(0) + a(1) * b(1) + a(2) * b(2) + a(3) * b(3);
    if (dot < 0.0f) dot = -dot;
    if (dot > 1.0f) dot = 1.0f;
    return 2.0f * (float)acos(dot);
}

static void ReduceVec3Track(std::vector<int>& frames, std::vector<JsonVec3>& values, float tolerance)
{
    if (frames.size() <= 2 || values.size() != frames.size())
        return;
    const float tolSq = tolerance * tolerance;

    std::vector<int> outFrames;
    std::vector<JsonVec3> outVals;
    outFrames.reserve(frames.size());
    outVals.reserve(values.size());

    outFrames.push_back(frames[0]);
    outVals.push_back(values[0]);

    for (size_t i = 1; i + 1 < frames.size(); ++i)
    {
        const int f0 = outFrames.back();
        const int f1 = frames[i];
        const int f2 = frames[i + 1];
        if (f2 <= f0)
        {
            outFrames.push_back(frames[i]);
            outVals.push_back(values[i]);
            continue;
        }

        const float alpha = (float)(f1 - f0) / (float)(f2 - f0);
        JsonVec3 predicted = Vec3Lerp(outVals.back(), values[i + 1], alpha);
        float err = Vec3DistSq(predicted, values[i]);
        if (err <= tolSq)
        {
            continue;
        }

        outFrames.push_back(frames[i]);
        outVals.push_back(values[i]);
    }

    outFrames.push_back(frames.back());
    outVals.push_back(values.back());

    frames.swap(outFrames);
    values.swap(outVals);
}

static void ReduceQuatTrack(JsonTrack& track, float toleranceDeg)
{
    if (track.frames.size() <= 2 || track.rotations.size() != track.frames.size())
        return;
    const float tolRad = toleranceDeg * 3.1415926535f / 180.0f;
    // Acceleration tolerance: keep keys where removing them would change
    // the angular acceleration by more than this threshold.  This prevents
    // removing keys at curvature inflection points (e.g. impact hits,
    // direction reversals) that SLERP-based error alone can miss.
    const float accelTolRad = tolRad * 2.0f;

    std::vector<int> outFrames;
    std::vector<JsonTrack::Quat4> outRots;
    outFrames.reserve(track.frames.size());
    outRots.reserve(track.rotations.size());

    outFrames.push_back(track.frames[0]);
    outRots.push_back(track.rotations[0]);

    for (size_t i = 1; i + 1 < track.frames.size(); ++i)
    {
        const int f0 = outFrames.back();
        const int f1 = track.frames[i];
        const int f2 = track.frames[i + 1];
        if (f2 <= f0)
        {
            outFrames.push_back(track.frames[i]);
            outRots.push_back(track.rotations[i]);
            continue;
        }
        const float alpha = (float)(f1 - f0) / (float)(f2 - f0);
        hkQuaternion q0 = MakeQuaternion(outRots.back());
        hkQuaternion q2 = MakeQuaternion(track.rotations[i + 1]);
        hkQuaternion pred = QuatBlend(q0, q2, alpha, false);
        hkQuaternion q1 = MakeQuaternion(track.rotations[i]);
        float err = QuatAngleDiff(pred, q1);

        // Check angular acceleration change (second derivative).
        // If key i is at a curvature inflection point, the angular
        // velocity changes direction — removing it would smooth over
        // an impact or direction reversal.
        bool accelSignificant = false;
        if (i >= 2 && i + 2 < track.frames.size())
        {
            hkQuaternion qPrev = MakeQuaternion(track.rotations[i - 1]);
            hkQuaternion qNext = MakeQuaternion(track.rotations[i + 1]);
            // Angular velocity approximation: angle(q[i-1], q[i]) and angle(q[i], q[i+1])
            float v0 = QuatAngleDiff(qPrev, q1);
            float v1 = QuatAngleDiff(q1, qNext);
            // Acceleration = change in velocity
            float accel = fabsf(v1 - v0);
            if (accel > accelTolRad)
                accelSignificant = true;
        }

        if (err <= tolRad && !accelSignificant)
        {
            continue;
        }
        outFrames.push_back(track.frames[i]);
        outRots.push_back(track.rotations[i]);
    }

    outFrames.push_back(track.frames.back());
    outRots.push_back(track.rotations.back());

    track.frames.swap(outFrames);
    track.rotations.swap(outRots);
}

struct TrackBoneLess
{
    bool operator()(const JsonTrack& a, const JsonTrack& b) const { return a.boneIndex < b.boneIndex; }
};
struct TransBoneLess
{
    bool operator()(const JsonTranslationTrack& a, const JsonTranslationTrack& b) const { return a.boneIndex < b.boneIndex; }
};
struct ScaleBoneLess
{
    bool operator()(const JsonScaleTrack& a, const JsonScaleTrack& b) const { return a.boneIndex < b.boneIndex; }
};

static void SortTracksByBoneIndex(JsonAnimClip* clip)
{
    if (!clip) return;
    std::stable_sort(clip->tracks.begin(), clip->tracks.end(), TrackBoneLess());
    std::stable_sort(clip->translationTracks.begin(), clip->translationTracks.end(), TransBoneLess());
    std::stable_sort(clip->scaleTracks.begin(), clip->scaleTracks.end(), ScaleBoneLess());
}

static int CountQuatKeys(const JsonAnimClip* clip)
{
    if (!clip) return 0;
    int total = 0;
    for (size_t i = 0; i < clip->tracks.size(); ++i)
        total += (int)clip->tracks[i].frames.size();
    return total;
}

static int CountVec3Keys(const std::vector<JsonTranslationTrack>& tracks)
{
    int total = 0;
    for (size_t i = 0; i < tracks.size(); ++i)
        total += (int)tracks[i].frames.size();
    return total;
}

static int CountScaleKeys(const std::vector<JsonScaleTrack>& tracks)
{
    int total = 0;
    for (size_t i = 0; i < tracks.size(); ++i)
        total += (int)tracks[i].frames.size();
    return total;
}

static int FindGraphParamIndex(const AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return -1;
    const std::vector<GraphParam>& params = GetGraphParamsConst(graph);
    for (int i = 0; i < (int)params.size(); ++i)
    {
        if (_stricmp(params[i].name.c_str(), name) == 0)
            return i;
    }
    return -1;
}

static int FindGraphStateIndex(const AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)graph.states.size(); ++i)
    {
        if (_stricmp(graph.states[i].name.c_str(), name) == 0)
            return i;
    }
    return -1;
}

static AnimationGraphRuntime* FindStateMachineByName(AnimationGraphRuntime& graph, const char* name);

static unsigned int HashStringLowerRuntime(const char* text)
{
    if (!text) return 0;
    unsigned int hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p)
    {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

static bool ParseCreatureVarName(const char* name, int& index, int& source)
{
    if (!name) return false;
    if (_strnicmp(name, "CV_", 3) != 0) return false;
    const char* p = name + 3;
    if (!*p) return false;
    char* endPtr = NULL;
    long idx = strtol(p, &endPtr, 10);
    if (endPtr == p) return false;
    index = (int)idx;
    source = 0;
    if (endPtr && endPtr[0] == '_' && (endPtr[1] == 'S' || endPtr[1] == 's'))
    {
        char* endPtr2 = NULL;
        long src = strtol(endPtr + 2, &endPtr2, 10);
        if (endPtr2 != endPtr + 2)
            source = (int)src;
    }
    return true;
}

static bool ParseIndexedSuffix(const char* name, const char* prefix, int& index)
{
    if (!name || !prefix) return false;
    size_t len = strlen(prefix);
    if (_strnicmp(name, prefix, len) != 0) return false;
    const char* p = name + len;
    if (!*p) return false;
    char* endPtr = NULL;
    long idx = strtol(p, &endPtr, 10);
    if (endPtr == p) return false;
    index = (int)idx;
    return true;
}

static const char* GetSuffixAfterPrefix(const char* name, const char* prefix)
{
    if (!name || !prefix) return NULL;
    size_t len = strlen(prefix);
    if (_strnicmp(name, prefix, len) != 0) return NULL;
    return name + len;
}

static void SetGraphParamValue(GraphParam& p, float f, int i, bool b)
{
    switch (p.type)
    {
    case Scene3DRenderer::GRAPH_PARAM_INT:
        p.i = i;
        p.f = (float)i;
        p.b = (i != 0);
        break;
    case Scene3DRenderer::GRAPH_PARAM_BOOL:
        p.b = b;
        p.i = b ? 1 : 0;
        p.f = b ? 1.0f : 0.0f;
        break;
    case Scene3DRenderer::GRAPH_PARAM_TRIGGER:
        p.triggered = b;
        p.b = b;
        p.i = b ? 1 : 0;
        p.f = b ? 1.0f : 0.0f;
        break;
    case Scene3DRenderer::GRAPH_PARAM_FLOAT:
    default:
        p.f = f;
        p.i = (int)f;
        p.b = (f != 0.0f);
        break;
    }
}

static void SetGraphParamFloat(GraphParam& p, float v)
{
    SetGraphParamValue(p, v, (int)v, (v != 0.0f));
}

static void SetGraphParamInt(GraphParam& p, int v)
{
    SetGraphParamValue(p, (float)v, v, (v != 0));
}

static void SetGraphParamBool(GraphParam& p, bool v)
{
    SetGraphParamValue(p, v ? 1.0f : 0.0f, v ? 1 : 0, v);
}

struct RuntimeGraphInputs
{
    float moveX;
    float moveZ;
    float speed;
    int stanceHash;
    int actionHash;
    float randomValue;
    bool sprinting;
};

static void UpdateInputButtons(int* buttonState, bool* buttonDown)
{
    static const int kButtonCount = 32;
    static const int kButtonKeys[kButtonCount] =
    {
        VK_RBUTTON, 'X', 'Y', 'B',
        VK_LBUTTON, 'C', VK_MBUTTON, 'G',
        VK_RBUTTON, VK_LBUTTON, 'F', 'E',
        'Q', '1', '2', '3',
        VK_SPACE, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };

    for (int i = 0; i < kButtonCount; ++i)
    {
        int key = kButtonKeys[i];
        bool down = (key != 0) ? ((GetAsyncKeyState(key) & 0x8000) != 0) : false;
        int state = 0;
        if (down && !buttonDown[i]) state = 2;
        else if (down && buttonDown[i]) state = 3;
        else if (!down && buttonDown[i]) state = 1;
        buttonState[i] = state;
        buttonDown[i] = down;
    }
}

static bool StrStartsWithNoCase(const char* s, const char* prefix)
{
    if (!s || !prefix) return false;
    size_t len = strlen(prefix);
    return _strnicmp(s, prefix, len) == 0;
}

static bool StrEqualsNoCase(const char* s, const char* other)
{
    if (!s || !other) return false;
    return _stricmp(s, other) == 0;
}

static bool MatchInputEventToButton(const char* name, int& outButton, int& outState)
{
    if (!name || !name[0]) return false;

    if (StrEqualsNoCase(name, "Start_Jump") || StrEqualsNoCase(name, "StartJump"))
    {
        outButton = 16; // VK_SPACE mapped in UpdateInputButtons
        outState = 2;   // down
        return true;
    }

    // Attack buttons (1/2/3)
    if (StrStartsWithNoCase(name, "Attack1"))
    {
        outButton = 13;
    }
    else if (StrStartsWithNoCase(name, "Attack2"))
    {
        outButton = 14;
    }
    else if (StrStartsWithNoCase(name, "Attack3"))
    {
        outButton = 15;
    }
    else if (StrStartsWithNoCase(name, "Special1"))
    {
        outButton = 1; // X
    }
    else if (StrStartsWithNoCase(name, "Special2"))
    {
        outButton = 2; // Y
    }
    else if (StrStartsWithNoCase(name, "Special3"))
    {
        outButton = 3; // B
    }
    else if (StrStartsWithNoCase(name, "Block"))
    {
        outButton = 0;
    }
    else if (StrStartsWithNoCase(name, "Throw"))
    {
        outButton = 7;
    }
    else if (StrStartsWithNoCase(name, "DrawWeapon") || StrStartsWithNoCase(name, "ChargingWeapon") || StrStartsWithNoCase(name, "FireWeapon"))
    {
        outButton = 4;
    }
    else if (StrStartsWithNoCase(name, "Snipe"))
    {
        outButton = 0;
    }
    else if (StrStartsWithNoCase(name, "Modifier"))
    {
        outButton = 1;
    }
    else if (StrStartsWithNoCase(name, "Dodge") || StrStartsWithNoCase(name, "Evade"))
    {
        outButton = 9;
    }
    else
    {
        return false;
    }

    // Map suffix to button edge.
    if (strstr(name, "Tapped") || strstr(name, "_Tapped") || strstr(name, "Down"))
    {
        outState = 2; // down
    }
    else if (strstr(name, "Released") || strstr(name, "_Released") || strstr(name, "Up"))
    {
        outState = 1; // up
    }
    else if (strstr(name, "Held") || strstr(name, "Charging"))
    {
        outState = 3; // held
    }
    else
    {
        // Default to Down for ambiguous events (e.g. "ThrowReady", "DrawWeapon").
        outState = 2;
    }
    return true;
}

static void UpdateInputEventTriggers(AnimationGraphRuntime& graph, const int* buttonState)
{
    std::vector<GraphParam>& params = GetGraphParams(graph);
    for (int i = 0; i < (int)params.size(); ++i)
    {
        GraphParam& p = params[i];
        if (p.type != Scene3DRenderer::GRAPH_PARAM_TRIGGER)
            continue;

        int button = -1;
        int state = 0;
        if (!MatchInputEventToButton(p.name.c_str(), button, state))
            continue;

        bool triggered = false;
        if (button >= 0 && button < 32)
        {
            triggered = (buttonState[button] == state);
        }
        p.triggered = triggered;
    }
}

static RuntimeGraphInputs BuildRuntimeInputs(const int* buttonState, unsigned int& rngState)
{
    RuntimeGraphInputs inputs;
    inputs.moveX = 0.0f;
    inputs.moveZ = 0.0f;
    inputs.speed = 0.0f;
    inputs.sprinting = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    if ((GetAsyncKeyState('A') & 0x8000) != 0 || (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0)
        inputs.moveX -= 1.0f;
    if ((GetAsyncKeyState('D') & 0x8000) != 0 || (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0)
        inputs.moveX += 1.0f;
    if ((GetAsyncKeyState('W') & 0x8000) != 0 || (GetAsyncKeyState(VK_UP) & 0x8000) != 0)
        inputs.moveZ += 1.0f;
    if ((GetAsyncKeyState('S') & 0x8000) != 0 || (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0)
        inputs.moveZ -= 1.0f;

    float lenSq = inputs.moveX * inputs.moveX + inputs.moveZ * inputs.moveZ;
    if (lenSq > 1.0f)
    {
        float invLen = 1.0f / sqrtf(lenSq);
        inputs.moveX *= invLen;
        inputs.moveZ *= invLen;
        lenSq = 1.0f;
    }
    inputs.speed = (lenSq > 0.0f) ? sqrtf(lenSq) : 0.0f;

    int actionHash = (int)HashStringLowerRuntime("Unknown");
    int stanceHash = (int)HashStringLowerRuntime(inputs.speed > 0.01f ? "Move" : "Idle");
    bool attack = (buttonState[13] >= 2 || buttonState[14] >= 2 || buttonState[15] >= 2);
    bool fire = (buttonState[4] >= 2 || buttonState[9] >= 2);
    bool block = (buttonState[0] >= 2 || buttonState[8] >= 2);
    bool modifier = (buttonState[1] >= 2);
    bool throwBtn = (buttonState[7] >= 2);
    bool techRoll = (buttonState[8] >= 2);
    bool evade = (buttonState[9] >= 2);

    if (attack)
        actionHash = (int)HashStringLowerRuntime("Attack");
    else if (throwBtn)
        actionHash = (int)HashStringLowerRuntime("Throw");
    else if (fire && modifier)
        actionHash = (int)HashStringLowerRuntime("Aiming");
    else if (fire)
        actionHash = (int)HashStringLowerRuntime("Firing");
    else if (block && modifier)
        actionHash = (int)HashStringLowerRuntime("Sniping");
    else if (techRoll)
        actionHash = (int)HashStringLowerRuntime("Dodge");
    else if (evade)
        actionHash = (int)HashStringLowerRuntime("Evade");
    else if (block)
        actionHash = (int)HashStringLowerRuntime((inputs.speed > 0.1f) ? "Dodge" : "Block");

    rngState = (rngState * 1664525u) + 1013904223u;
    unsigned int rv = (rngState >> 8) & 0x00FFFFFFu;
    inputs.randomValue = (float)rv / 16777216.0f;

    inputs.actionHash = actionHash;
    inputs.stanceHash = stanceHash;
    return inputs;
}

static void UpdateCreatureVarsFromInputs(Scene3DRenderer& renderer, const RuntimeGraphInputs& inputs)
{
    const float moveX = inputs.moveX;
    const float moveZ = inputs.moveZ;
    const float speed = inputs.speed;

    renderer.setCreatureVar(1, 0, speed);
    renderer.setCreatureVar(2, 0, speed);
    renderer.setCreatureVar(4, 0, speed);
    renderer.setCreatureVar(5, 0, speed);
    renderer.setCreatureVar(7, 0, moveX);
    renderer.setCreatureVar(9, 0, moveZ);
    renderer.setCreatureVar(10, 0, moveX);
    renderer.setCreatureVar(11, 0, moveZ);
    renderer.setCreatureVar(19, 0, speed);
    renderer.setCreatureVar(20, 0, speed);
    renderer.setCreatureVar(22, 0, speed);
    renderer.setCreatureVar(23, 0, speed);
    renderer.setCreatureVar(25, 0, moveX);
    renderer.setCreatureVar(27, 0, moveZ);
    renderer.setCreatureVar(28, 0, moveX);
    renderer.setCreatureVar(29, 0, moveZ);
    renderer.setCreatureVar(43, 0, (float)inputs.stanceHash);
    renderer.setCreatureVar(44, 0, (float)inputs.actionHash);
    renderer.setCreatureVar(48, 0, 1.0f);
    renderer.setCreatureVar(49, 0, inputs.sprinting ? 1.0f : 0.0f);
    renderer.setCreatureVar(59, 0, inputs.randomValue);
}

static float GetTimeInStateByName(AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return 0.0f;
    AnimationGraphRuntime* sm = FindStateMachineByName(graph, name);
    if (sm)
        return sm->stateTime;
    int idx = FindGraphStateIndex(graph, name);
    if (idx < 0) return 0.0f;
    if (idx == graph.currentState) return graph.stateTime;
    if (graph.inTransition && idx == graph.nextState) return graph.nextTime;
    return 0.0f;
}

static bool IsStateActiveByName(AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return false;
    AnimationGraphRuntime* sm = FindStateMachineByName(graph, name);
    if (sm && sm->currentState >= 0 && sm->currentState < (int)sm->states.size())
        return true;
    int idx = FindGraphStateIndex(graph, name);
    if (idx < 0) return false;
    if (idx == graph.currentState) return true;
    if (graph.inTransition && idx == graph.nextState) return true;
    return false;
}

static void UpdateGraphParamsForMachine(Scene3DRenderer& renderer, AnimationGraphRuntime& graph, const RuntimeGraphInputs& inputs, const int* buttonState)
{
    std::vector<GraphParam>& params = GetGraphParams(graph);
    for (int i = 0; i < (int)params.size(); ++i)
    {
        GraphParam& p = params[i];
        const char* name = p.name.c_str();
        int idx = -1;
        int src = 0;
        if (ParseCreatureVarName(name, idx, src))
        {
            // Read from m_creatureVars for ALL CVs, including 43/44.
            // This ensures that values written by SetCreatureData actions
            // (mirrored to CV_43/CV_44) are visible to graph conditions.
            float v = renderer.getCreatureVar(idx, src);
            SetGraphParamValue(p, v, (int)v, (v != 0.0f));
            continue;
        }

        if (ParseIndexedSuffix(name, "ButtonState:", idx))
        {
            int state = (idx >= 0 && idx < 32) ? buttonState[idx] : 0;
            SetGraphParamValue(p, (float)state, state, (state != 0));
            continue;
        }

        const char* timeSuffix = GetSuffixAfterPrefix(name, "TimeInState:");
        if (timeSuffix)
        {
            float t = GetTimeInStateByName(graph, timeSuffix);
            SetGraphParamFloat(p, t);
            continue;
        }

        const char* activeSuffix = GetSuffixAfterPrefix(name, "ActiveState:");
        if (activeSuffix)
        {
            bool active = IsStateActiveByName(graph, activeSuffix);
            SetGraphParamBool(p, active);
            continue;
        }

        const char* localSuffix = GetSuffixAfterPrefix(name, "GetLocal:");
        if (localSuffix)
        {
            float v = renderer.getLocalVar(localSuffix);
            SetGraphParamValue(p, v, (int)v, (v != 0.0f));
            continue;
        }

        const char* queueSuffix = GetSuffixAfterPrefix(name, "_TransQueueCount:");
        if (queueSuffix)
        {
            SetGraphParamInt(p, 0);
            continue;
        }

        if (_stricmp(name, "speed") == 0 || _stricmp(name, "move_speed") == 0 || _stricmp(name, "CV_PHYSICAL_MOVESPEED_XZ") == 0)
        {
            SetGraphParamFloat(p, inputs.speed);
            continue;
        }
        if (_stricmp(name, "local_move_x") == 0 || _stricmp(name, "move_x") == 0 || _stricmp(name, "CV_PHYSICAL_LOCAL_MOVE_X") == 0)
        {
            SetGraphParamFloat(p, inputs.moveX);
            continue;
        }
        if (_stricmp(name, "local_move_z") == 0 || _stricmp(name, "move_z") == 0 || _stricmp(name, "CV_PHYSICAL_LOCAL_MOVE_Z") == 0)
        {
            SetGraphParamFloat(p, inputs.moveZ);
            continue;
        }
    }
}

static void UpdateGraphRuntimeParams(Scene3DRenderer& renderer, AnimationGraphRuntime& graph, int* buttonState, bool* buttonDown, unsigned int& rngState)
{
    UpdateInputButtons(buttonState, buttonDown);
    RuntimeGraphInputs inputs = BuildRuntimeInputs(buttonState, rngState);
    UpdateCreatureVarsFromInputs(renderer, inputs);
    UpdateGraphParamsForMachine(renderer, graph, inputs, buttonState);
    UpdateInputEventTriggers(graph, buttonState);
    for (int i = 0; i < (int)graph.subMachines.size(); ++i)
    {
        AnimationGraphRuntime* sm = graph.subMachines[i];
        if (sm)
        {
            UpdateGraphParamsForMachine(renderer, *sm, inputs, buttonState);
            UpdateInputEventTriggers(*sm, buttonState);
        }
    }
}

static int ParseGraphParamType(const char* typeStr)
{
    if (!typeStr || !typeStr[0]) return Scene3DRenderer::GRAPH_PARAM_FLOAT;
    if (_stricmp(typeStr, "float") == 0) return Scene3DRenderer::GRAPH_PARAM_FLOAT;
    if (_stricmp(typeStr, "int") == 0 || _stricmp(typeStr, "integer") == 0) return Scene3DRenderer::GRAPH_PARAM_INT;
    if (_stricmp(typeStr, "bool") == 0 || _stricmp(typeStr, "boolean") == 0) return Scene3DRenderer::GRAPH_PARAM_BOOL;
    if (_stricmp(typeStr, "trigger") == 0) return Scene3DRenderer::GRAPH_PARAM_TRIGGER;
    return Scene3DRenderer::GRAPH_PARAM_FLOAT;
}

static int ParseGraphBlendMode(const char* str)
{
    if (!str || !str[0]) return Scene3DRenderer::JSON_BLEND_CROSSFADE;
    if (_stricmp(str, "crossfade") == 0 || _stricmp(str, "blend") == 0)
        return Scene3DRenderer::JSON_BLEND_CROSSFADE;
    if (_stricmp(str, "layered") == 0 || _stricmp(str, "mask") == 0 || _stricmp(str, "masked") == 0)
        return Scene3DRenderer::JSON_BLEND_LAYERED;
    if (_stricmp(str, "additive") == 0)
        return Scene3DRenderer::JSON_BLEND_ADDITIVE;
    return Scene3DRenderer::JSON_BLEND_CROSSFADE;
}

static int ParseGraphRotMode(const char* str)
{
    if (!str || !str[0]) return Scene3DRenderer::JSON_BLEND_ROT_SLERP;
    if (_stricmp(str, "nlerp") == 0)
        return Scene3DRenderer::JSON_BLEND_ROT_NLERP;
    return Scene3DRenderer::JSON_BLEND_ROT_SLERP;
}

static int FindMaskIndexByName(const AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)graph.masks.size(); ++i)
    {
        if (_stricmp(graph.masks[i].name.c_str(), name) == 0)
            return i;
    }
    return -1;
}

static int FindBlendGraphIndexByName(const AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)graph.blendGraphs.size(); ++i)
    {
        BlendGraphRuntime* bg = graph.blendGraphs[i];
        if (bg && _stricmp(bg->name.c_str(), name) == 0)
            return i;
    }
    return -1;
}

static int FindBlendGraphNodeIndex(const BlendGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)graph.nodes.size(); ++i)
    {
        if (_stricmp(graph.nodes[i].name.c_str(), name) == 0)
            return i;
    }
    return -1;
}

static int FindSkeletonBoneIndexCI(const hkaSkeleton* skeleton, const char* name)
{
    if (!skeleton || !name || !name[0]) return -1;
    for (int i = 0; i < skeleton->m_numBones; ++i)
    {
        const hkaBone* bone = skeleton->m_bones[i];
        if (bone && bone->m_name && _stricmp(bone->m_name, name) == 0)
            return i;
    }
    return -1;
}

static int FindFirstBoneByAliases(const hkaSkeleton* skeleton, const char** names, int count)
{
    if (!skeleton || !names || count <= 0) return -1;
    for (int i = 0; i < count; ++i)
    {
        int idx = FindSkeletonBoneIndexCI(skeleton, names[i]);
        if (idx >= 0) return idx;
    }
    return -1;
}

static bool ParseFloatArrayByKey(const char* start, const char* end, const char* key, std::vector<float>& outValues)
{
    const char* keyPos = FindKeyInRangeAnim(start, end, key);
    if (!keyPos) return false;
    const char* arrStart = strchr(keyPos, '[');
    if (!arrStart || arrStart >= end) return false;
    const char* arrEnd = FindMatchingAnim(arrStart, '[', ']');
    if (!arrEnd || arrEnd > end) return false;

    outValues.clear();
    const char* p = arrStart + 1;
    while (p && p < arrEnd)
    {
        p = SkipWhitespaceAnim(p);
        if (!p || p >= arrEnd) break;
        char* endPtr = NULL;
        double v = strtod(p, &endPtr);
        if (endPtr != p)
        {
            outValues.push_back((float)v);
            p = endPtr;
        }
        else
        {
            p++;
        }
    }
    return !outValues.empty();
}

static float GetGraphParamFloatByName(const AnimationGraphRuntime& graph, const char* name, float defaultValue)
{
    int idx = FindGraphParamIndex(graph, name);
    if (idx < 0) return defaultValue;
    const std::vector<GraphParam>& params = GetGraphParamsConst(graph);
    if (idx < 0 || idx >= (int)params.size()) return defaultValue;
    return params[idx].f;
}

static float GetGraphParamFloatByAliases(const AnimationGraphRuntime& graph, const char** names, int count, float defaultValue)
{
    for (int i = 0; i < count; ++i)
    {
        float v = GetGraphParamFloatByName(graph, names[i], defaultValue);
        if (FindGraphParamIndex(graph, names[i]) >= 0)
            return v;
    }
    return defaultValue;
}

static float EvalGraphWeightName(const char* name, const AnimationGraphRuntime& graph, const GraphWeight& weight)
{
    if (!name || !name[0]) return 0.0f;

    if (_stricmp(name, "Wt_AimSweep") == 0 || _stricmp(name, "AimSweep") == 0)
    {
        const char* names[] = { "aim_sweep", "AimSweep", "CV_AIM_LOCAL_PITCH" };
        float t = GetGraphParamFloatByAliases(graph, names, 3, 0.0f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return t;
    }
    if (_stricmp(name, "Wt_AimSweepLow") == 0)
    {
        const char* names[] = { "aim_sweep", "AimSweep", "CV_AIM_LOCAL_PITCH" };
        float t = GetGraphParamFloatByAliases(graph, names, 3, 0.0f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float v = 1.0f - (t * 2.0f);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return v;
    }
    if (_stricmp(name, "Wt_AimSweepMid") == 0)
    {
        const char* names[] = { "aim_sweep", "AimSweep", "CV_AIM_LOCAL_PITCH" };
        float t = GetGraphParamFloatByAliases(graph, names, 3, 0.0f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float v = 1.0f - fabsf((t * 2.0f) - 1.0f);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return v;
    }
    if (_stricmp(name, "Wt_AimSweepHigh") == 0)
    {
        const char* names[] = { "aim_sweep", "AimSweep", "CV_AIM_LOCAL_PITCH" };
        float t = GetGraphParamFloatByAliases(graph, names, 3, 0.0f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float v = (t * 2.0f) - 1.0f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return v;
    }

    bool isWalk = (strstr(name, "Walk") != NULL || strstr(name, "walk") != NULL);
    bool isRun = (strstr(name, "Run") != NULL || strstr(name, "run") != NULL);

    const char* speedNames[] = { "speed", "move_speed", "CV_PHYSICAL_MOVESPEED_XZ" };
    const char* moveXNames[] = { "local_move_x", "move_x", "CV_PHYSICAL_LOCAL_MOVE_X" };
    const char* moveZNames[] = { "local_move_z", "move_z", "CV_PHYSICAL_LOCAL_MOVE_Z" };

    float speed = GetGraphParamFloatByAliases(graph, speedNames, 3, 0.0f);
    float moveX = GetGraphParamFloatByAliases(graph, moveXNames, 3, 0.0f);
    float moveZ = GetGraphParamFloatByAliases(graph, moveZNames, 3, 0.0f);

    float absX = fabsf(moveX);
    float absZ = fabsf(moveZ);
    float dirSum = absX + absZ;
    float dirX = (dirSum > 0.0f) ? (absX / dirSum) : 0.5f;
    float dirZ = (dirSum > 0.0f) ? (absZ / dirSum) : 0.5f;

    float walkSpeed = (weight.argCount > 0) ? weight.args[0] : 0.0f;
    float runSpeed = (weight.argCount > 1) ? weight.args[1] : walkSpeed;
    if (runSpeed <= walkSpeed) runSpeed = walkSpeed + 0.001f;

    float walkFactor = 1.0f;
    if (runSpeed > walkSpeed)
    {
        walkFactor = (runSpeed - speed) / (runSpeed - walkSpeed);
        if (walkFactor < 0.0f) walkFactor = 0.0f;
        if (walkFactor > 1.0f) walkFactor = 1.0f;
    }
    float runFactor = 1.0f - walkFactor;

    float speedFactor = 1.0f;
    if (isWalk) speedFactor = walkFactor;
    else if (isRun) speedFactor = runFactor;

    char last = name[(int)strlen(name) - 1];
    float dirFactor = 1.0f;
    if (last == 'N' || last == 'n' || last == 'S' || last == 's')
        dirFactor = dirZ;
    else if (last == 'E' || last == 'e' || last == 'W' || last == 'w')
        dirFactor = dirX;

    return dirFactor * speedFactor;
}

static float EvalGraphWeight(const GraphWeight& weight, const AnimationGraphRuntime& graph)
{
    if (weight.isConst)
        return weight.constant;
    if (weight.name.empty())
        return 0.0f;

    int idx = FindGraphParamIndex(graph, weight.name.c_str());
    if (idx >= 0)
    {
        const std::vector<GraphParam>& params = GetGraphParamsConst(graph);
        if (idx < (int)params.size())
            return params[idx].f;
    }
    return EvalGraphWeightName(weight.name.c_str(), graph, weight);
}

static void ParseGraphMasksRange(const char* json, const char* jsonEnd, AnimationGraphRuntime& graph, const hkaSkeleton* skeleton)
{
    if (!json || !jsonEnd || !skeleton) return;
    const char* masksKey = FindKeyInRangeAnim(json, jsonEnd, "masks");
    const char* masksArray = masksKey ? strchr(masksKey, '[') : NULL;
    const char* masksEnd = masksArray ? FindMatchingAnim(masksArray, '[', ']') : NULL;
    if (!masksArray || !masksEnd) return;

    const int boneCount = skeleton->m_numBones;
    const char* p = masksArray + 1;
    while (p && p < masksEnd)
    {
        p = SkipWhitespaceAnim(p);
        if (!p || p >= masksEnd) break;
        if (*p == '{')
        {
            const char* objStart = p;
            const char* objEnd = FindMatchingAnim(objStart, '{', '}');
            if (!objEnd || objEnd > masksEnd) break;

            char name[128] = "";
            if (ExtractStringValueRangeAnim(objStart, objEnd, "name", name, sizeof(name)))
            {
                GraphMask mask;
                mask.name = name;
                mask.weights.assign(boneCount, 0.0f);

                const char* bonesKey = FindKeyInRangeAnim(objStart, objEnd, "bones");
                const char* bonesArray = bonesKey ? strchr(bonesKey, '[') : NULL;
                const char* bonesEnd = bonesArray ? FindMatchingAnim(bonesArray, '[', ']') : NULL;
                if (bonesArray && bonesEnd)
                {
                    const char* b = bonesArray + 1;
                    while (b && b < bonesEnd)
                    {
                        b = SkipWhitespaceAnim(b);
                        if (!b || b >= bonesEnd) break;
                        if (*b == '{')
                        {
                            const char* boneStart = b;
                            const char* boneEnd = FindMatchingAnim(boneStart, '{', '}');
                            if (!boneEnd || boneEnd > bonesEnd) break;

                            char boneName[128] = "";
                            float weight = 1.0f;
                            if (ExtractStringValueRangeAnim(boneStart, boneEnd, "bone", boneName, sizeof(boneName)) ||
                                ExtractStringValueRangeAnim(boneStart, boneEnd, "name", boneName, sizeof(boneName)))
                            {
                                ExtractFloatValueRangeAnim(boneStart, boneEnd, "weight", &weight);
                                int idx = FindSkeletonBoneIndexCI(skeleton, boneName);
                                if (idx >= 0 && idx < boneCount)
                                {
                                    mask.weights[idx] = weight;
                                }
                            }
                            b = boneEnd + 1;
                            continue;
                        }
                        b++;
                    }
                }
                graph.masks.push_back(mask);
            }

            p = objEnd + 1;
            continue;
        }
        p++;
    }
}

static int ParseBlendGraphNodeType(const char* typeStr)
{
    if (!typeStr || !typeStr[0]) return BLEND_NODE_SAMPLER;
    if (_stricmp(typeStr, "sampler") == 0) return BLEND_NODE_SAMPLER;
    if (_stricmp(typeStr, "blender") == 0 || _stricmp(typeStr, "blend") == 0) return BLEND_NODE_BLENDER;
    if (_stricmp(typeStr, "statemachine") == 0 || _stricmp(typeStr, "stateMachine") == 0) return BLEND_NODE_STATEMACHINE;
    if (_stricmp(typeStr, "subgraph") == 0) return BLEND_NODE_SUBGRAPH;
    if (_stricmp(typeStr, "reference") == 0 || _stricmp(typeStr, "ref") == 0) return BLEND_NODE_REFERENCE;
    return BLEND_NODE_SAMPLER;
}

static void BuildMaskFromBlendParts(const AnimationGraphRuntime& graph, const std::vector<int>& maskIndices, const std::vector<float>& maskWeights, std::vector<float>& outMask)
{
    outMask.clear();
    if (maskIndices.empty() || maskWeights.empty())
        return;
    int boneCount = 0;
    for (int i = 0; i < (int)maskIndices.size(); ++i)
    {
        int idx = maskIndices[i];
        if (idx >= 0 && idx < (int)graph.masks.size())
        {
            int count = (int)graph.masks[idx].weights.size();
            if (count > boneCount) boneCount = count;
        }
    }
    if (boneCount <= 0) return;

    outMask.assign(boneCount, 0.0f);
    for (int i = 0; i < (int)maskIndices.size(); ++i)
    {
        int idx = maskIndices[i];
        if (idx < 0 || idx >= (int)graph.masks.size()) continue;
        const std::vector<float>& src = graph.masks[idx].weights;
        float w = (i < (int)maskWeights.size()) ? maskWeights[i] : 1.0f;
        for (int b = 0; b < boneCount && b < (int)src.size(); ++b)
        {
            outMask[b] += src[b] * w;
        }
    }
    for (int b = 0; b < boneCount; ++b)
    {
        if (outMask[b] > 1.0f) outMask[b] = 1.0f;
        if (outMask[b] < 0.0f) outMask[b] = 0.0f;
    }
}

static BlendGraphRuntime* ParseBlendGraphObject(const char* objStart, const char* objEnd,
                                               const char* graphPath,
                                               const hkaSkeleton* skeleton,
                                               int jsonDecodeMode,
                                               int type2PackingMode,
                                               bool strictRigCoverage,
                                               AnimationGraphRuntime& graph)
{
    if (!objStart || !objEnd) return NULL;

    char name[128] = "";
    if (!ExtractStringValueRangeAnim(objStart, objEnd, "name", name, sizeof(name)))
        return NULL;

    BlendGraphRuntime* bg = new BlendGraphRuntime();
    bg->name = name;

    char resultName[128] = "";
    if (ExtractStringValueRangeAnim(objStart, objEnd, "result", resultName, sizeof(resultName)) ||
        ExtractStringValueRangeAnim(objStart, objEnd, "Result", resultName, sizeof(resultName)))
    {
        bg->resultNodeName = resultName;
    }

    const char* nodesKey = FindKeyInRangeAnim(objStart, objEnd, "nodes");
    const char* nodesArray = nodesKey ? strchr(nodesKey, '[') : NULL;
    const char* nodesEnd = nodesArray ? FindMatchingAnim(nodesArray, '[', ']') : NULL;
    if (nodesArray && nodesEnd)
    {
        const char* p = nodesArray + 1;
        while (p && p < nodesEnd)
        {
            p = SkipWhitespaceAnim(p);
            if (!p || p >= nodesEnd) break;
            if (*p == '{')
            {
                const char* nodeStart = p;
                const char* nodeEnd = FindMatchingAnim(nodeStart, '{', '}');
                if (!nodeEnd || nodeEnd > nodesEnd) break;

                char nodeName[128] = "";
                if (ExtractStringValueRangeAnim(nodeStart, nodeEnd, "name", nodeName, sizeof(nodeName)))
                {
                    BlendGraphNode node;
                    node.name = nodeName;

                    char typeStr[64] = "";
                    if (ExtractStringValueRangeAnim(nodeStart, nodeEnd, "type", typeStr, sizeof(typeStr)))
                    {
                        node.type = ParseBlendGraphNodeType(typeStr);
                    }

                    if (node.type == BLEND_NODE_SAMPLER)
                    {
                        char clipPath[512] = "";
                        if (ExtractStringValueRangeAnim(nodeStart, nodeEnd, "clip", clipPath, sizeof(clipPath)) ||
                            ExtractStringValueRangeAnim(nodeStart, nodeEnd, "animation", clipPath, sizeof(clipPath)))
                        {
                            node.clipPath = clipPath;
                            char resolved[512] = "";
                            ResolveGraphPath(graphPath, clipPath, resolved, sizeof(resolved));
                            JsonAnimClip* clip = LoadJsonAnimClip(resolved, skeleton, jsonDecodeMode, type2PackingMode);
                            if (clip && clip->tracks.empty())
                            {
                                delete clip;
                                clip = NULL;
                            }

                            if (clip)
                            {
                                if (strictRigCoverage)
                                {
                                    const int skeletonBones = skeleton->m_numBones;
                                    const int coveredBones = CountCoveredSkeletonBones(clip, skeletonBones);
                                    int minCovered = (int)(skeletonBones * 0.25f);
                                    if (minCovered < 4) minCovered = 4;
                                    if (coveredBones < minCovered)
                                    {
                                        delete clip;
                                        clip = NULL;
                                    }
                                }
                            }

                            node.clip = clip;
                            if (clip)
                            {
                                float dur = GetClipDuration(clip);
                                if (dur > bg->duration)
                                    bg->duration = dur;
                            }
                        }

                        bool loop = true;
                        if (ExtractBoolValueRangeAnim(nodeStart, nodeEnd, "loop", &loop))
                            node.loop = loop;
                        float rate = 1.0f;
                        if (ExtractFloatValueRangeAnim(nodeStart, nodeEnd, "rate", &rate))
                            node.rate = rate;
                        bool syncVel = false;
                        if (ExtractBoolValueRangeAnim(nodeStart, nodeEnd, "syncToVelocity", &syncVel))
                            node.syncToVelocity = syncVel;
                        float rootSpeed = 0.0f;
                        if (ExtractFloatValueRangeAnim(nodeStart, nodeEnd, "rootSpeed", &rootSpeed))
                            node.rootSpeed = rootSpeed;

                        char progressParam[128] = "";
                        if (ExtractStringValueRangeAnim(nodeStart, nodeEnd, "progressParam", progressParam, sizeof(progressParam)) ||
                            ExtractStringValueRangeAnim(nodeStart, nodeEnd, "progress", progressParam, sizeof(progressParam)))
                        {
                            node.progressParamIndex = FindGraphParamIndex(graph, progressParam);
                            if (node.progressParamIndex >= 0)
                                node.useProgressParam = true;
                        }
                    }
                    else if (node.type == BLEND_NODE_BLENDER)
                    {
                        bool normalize = false;
                        if (ExtractBoolValueRangeAnim(nodeStart, nodeEnd, "normalizeWeights", &normalize))
                            node.normalizeWeights = normalize;
                        bool syncTracks = false;
                        if (ExtractBoolValueRangeAnim(nodeStart, nodeEnd, "syncTracks", &syncTracks))
                            node.syncTracks = syncTracks;
                        bool syncVel = false;
                        if (ExtractBoolValueRangeAnim(nodeStart, nodeEnd, "syncToVelocity", &syncVel))
                            node.blenderSyncToVelocity = syncVel;
                        float weightDamp = 0.0f;
                        if (ExtractFloatValueRangeAnim(nodeStart, nodeEnd, "weightDamp", &weightDamp))
                            node.weightDamp = weightDamp;
                        float syncedRate = 1.0f;
                        if (ExtractFloatValueRangeAnim(nodeStart, nodeEnd, "syncedRate", &syncedRate))
                            node.syncedRate = syncedRate;

                        const char* tracksKey = FindKeyInRangeAnim(nodeStart, nodeEnd, "tracks");
                        const char* tracksArray = tracksKey ? strchr(tracksKey, '[') : NULL;
                        const char* tracksEnd = tracksArray ? FindMatchingAnim(tracksArray, '[', ']') : NULL;
                        if (tracksArray && tracksEnd)
                        {
                            const char* t = tracksArray + 1;
                            while (t && t < tracksEnd)
                            {
                                t = SkipWhitespaceAnim(t);
                                if (!t || t >= tracksEnd) break;
                                if (*t == '{')
                                {
                                    const char* trackStart = t;
                                    const char* trackEnd = FindMatchingAnim(trackStart, '{', '}');
                                    if (!trackEnd || trackEnd > tracksEnd) break;

                                    BlendGraphTrack track;
                                    char inputName[128] = "";
                                    if (ExtractStringValueRangeAnim(trackStart, trackEnd, "input", inputName, sizeof(inputName)))
                                    {
                                        track.inputName = inputName;
                                    }

                                    char weightName[128] = "";
                                    float weightVal = 0.0f;
                                    if (ExtractStringValueRangeAnim(trackStart, trackEnd, "weight", weightName, sizeof(weightName)))
                                    {
                                        track.weight.isConst = false;
                                        track.weight.name = weightName;
                                    }
                                    else if (ExtractFloatValueRangeAnim(trackStart, trackEnd, "weight", &weightVal))
                                    {
                                        track.weight.isConst = true;
                                        track.weight.constant = weightVal;
                                    }

                                    std::vector<float> args;
                                    if (ParseFloatArrayByKey(trackStart, trackEnd, "args", args) ||
                                        ParseFloatArrayByKey(trackStart, trackEnd, "weightArgs", args))
                                    {
                                        track.weight.argCount = (int)args.size();
                                        for (int ai = 0; ai < track.weight.argCount && ai < 4; ++ai)
                                            track.weight.args[ai] = args[ai];
                                    }

                                    char maskName[128] = "";
                                    if (ExtractStringValueRangeAnim(trackStart, trackEnd, "mask", maskName, sizeof(maskName)))
                                    {
                                        int maskIdx = FindMaskIndexByName(graph, maskName);
                                        if (maskIdx >= 0 && maskIdx < (int)graph.masks.size())
                                            track.mask = graph.masks[maskIdx].weights;
                                    }
                                    else
                                    {
                                        const char* partsKey = FindKeyInRangeAnim(trackStart, trackEnd, "blendParts");
                                        const char* partsArray = partsKey ? strchr(partsKey, '[') : NULL;
                                        const char* partsEnd = partsArray ? FindMatchingAnim(partsArray, '[', ']') : NULL;
                                        if (partsArray && partsEnd)
                                        {
                                            std::vector<int> partMasks;
                                            std::vector<float> partWeights;
                                            const char* bp = partsArray + 1;
                                            while (bp && bp < partsEnd)
                                            {
                                                bp = SkipWhitespaceAnim(bp);
                                                if (!bp || bp >= partsEnd) break;
                                                if (*bp == '{')
                                                {
                                                    const char* partStart = bp;
                                                    const char* partEnd = FindMatchingAnim(partStart, '{', '}');
                                                    if (!partEnd || partEnd > partsEnd) break;
                                                    char partMaskName[128] = "";
                                                    float partWeight = 1.0f;
                                                    if (ExtractStringValueRangeAnim(partStart, partEnd, "mask", partMaskName, sizeof(partMaskName)))
                                                    {
                                                        ExtractFloatValueRangeAnim(partStart, partEnd, "weight", &partWeight);
                                                        int midx = FindMaskIndexByName(graph, partMaskName);
                                                        if (midx >= 0)
                                                        {
                                                            partMasks.push_back(midx);
                                                            partWeights.push_back(partWeight);
                                                        }
                                                    }
                                                    bp = partEnd + 1;
                                                    continue;
                                                }
                                                bp++;
                                            }
                                            BuildMaskFromBlendParts(graph, partMasks, partWeights, track.mask);
                                        }
                                    }

                                    node.tracks.push_back(track);

                                    t = trackEnd + 1;
                                    continue;
                                }
                                t++;
                            }
                        }
                    }
                    else if (node.type == BLEND_NODE_REFERENCE)
                    {
                        char refName[128] = "";
                        if (ExtractStringValueRangeAnim(nodeStart, nodeEnd, "ref", refName, sizeof(refName)) ||
                            ExtractStringValueRangeAnim(nodeStart, nodeEnd, "node", refName, sizeof(refName)))
                        {
                            node.refName = refName;
                        }
                    }
                    else if (node.type == BLEND_NODE_SUBGRAPH)
                    {
                        char subName[128] = "";
                        if (ExtractStringValueRangeAnim(nodeStart, nodeEnd, "graph", subName, sizeof(subName)) ||
                            ExtractStringValueRangeAnim(nodeStart, nodeEnd, "subgraph", subName, sizeof(subName)))
                        {
                            node.subgraphName = subName;
                        }
                    }
                    else if (node.type == BLEND_NODE_STATEMACHINE)
                    {
                        char smName[128] = "";
                        if (ExtractStringValueRangeAnim(nodeStart, nodeEnd, "machine", smName, sizeof(smName)) ||
                            ExtractStringValueRangeAnim(nodeStart, nodeEnd, "stateMachine", smName, sizeof(smName)))
                        {
                            node.stateMachineName = smName;
                        }
                    }

                    bg->nodes.push_back(node);
                }

                p = nodeEnd + 1;
                continue;
            }
            p++;
        }
    }

    return bg;
}

static AnimationGraphRuntime* FindStateMachineByName(AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return NULL;
    if (!graph.name.empty() && _stricmp(graph.name.c_str(), name) == 0)
        return &graph;
    for (int i = 0; i < (int)graph.subMachines.size(); ++i)
    {
        AnimationGraphRuntime* sm = graph.subMachines[i];
        if (sm && !sm->name.empty() && _stricmp(sm->name.c_str(), name) == 0)
            return sm;
    }
    return NULL;
}

static void ResolveBlendGraphLinks(AnimationGraphRuntime& graph)
{
    for (int i = 0; i < (int)graph.blendGraphs.size(); ++i)
    {
        BlendGraphRuntime* bg = graph.blendGraphs[i];
        if (!bg) continue;
        if (!bg->resultNodeName.empty())
        {
            bg->resultNode = FindBlendGraphNodeIndex(*bg, bg->resultNodeName.c_str());
        }
        for (int n = 0; n < (int)bg->nodes.size(); ++n)
        {
            BlendGraphNode& node = bg->nodes[n];
            if (!node.refName.empty())
            {
                node.referenceNode = FindBlendGraphNodeIndex(*bg, node.refName.c_str());
            }
            if (!node.subgraphName.empty())
            {
                int idx = FindBlendGraphIndexByName(graph, node.subgraphName.c_str());
                if (idx >= 0 && idx < (int)graph.blendGraphs.size())
                    node.subgraph = graph.blendGraphs[idx];
            }
            if (!node.stateMachineName.empty())
            {
                node.stateMachine = FindStateMachineByName(graph, node.stateMachineName.c_str());
            }
            if (node.type == BLEND_NODE_BLENDER)
            {
                if (node.smoothedWeights.size() != node.tracks.size())
                    node.smoothedWeights.assign(node.tracks.size(), 0.0f);
                for (int t = 0; t < (int)node.tracks.size(); ++t)
                {
                    BlendGraphTrack& track = node.tracks[t];
                    if (track.inputNode < 0 && !track.inputName.empty())
                    {
                        track.inputNode = FindBlendGraphNodeIndex(*bg, track.inputName.c_str());
                    }
                }
            }
        }
    }
}

static void ResolveGraphStateLinks(AnimationGraphRuntime& graph)
{
    ResolveBlendGraphLinks(graph);

    for (int i = 0; i < (int)graph.states.size(); ++i)
    {
        GraphState& st = graph.states[i];
        if (!st.blendGraphName.empty())
        {
            int idx = FindBlendGraphIndexByName(graph, st.blendGraphName.c_str());
            if (idx >= 0 && idx < (int)graph.blendGraphs.size())
            {
                st.blendGraph = graph.blendGraphs[idx];
                if (st.duration <= 0.0f && st.blendGraph)
                    st.duration = st.blendGraph->duration;
            }
        }
        if (!st.stateMachineName.empty())
        {
            AnimationGraphRuntime* sm = FindStateMachineByName(graph, st.stateMachineName.c_str());
            if (sm)
                st.subMachine = sm;
        }
    }
}


static GraphConditionOp ParseConditionOp(const char* str)
{
    if (!str || !str[0]) return GRAPH_COND_EQUAL;
    if (strcmp(str, ">") == 0 || _stricmp(str, "gt") == 0 || _stricmp(str, "greater") == 0) return GRAPH_COND_GREATER;
    if (strcmp(str, "<") == 0 || _stricmp(str, "lt") == 0 || _stricmp(str, "less") == 0) return GRAPH_COND_LESS;
    if (strcmp(str, ">=") == 0 || _stricmp(str, "gte") == 0) return GRAPH_COND_GREATER_EQUAL;
    if (strcmp(str, "<=") == 0 || _stricmp(str, "lte") == 0) return GRAPH_COND_LESS_EQUAL;
    if (strcmp(str, "==") == 0 || _stricmp(str, "eq") == 0) return GRAPH_COND_EQUAL;
    if (strcmp(str, "!=") == 0 || _stricmp(str, "neq") == 0) return GRAPH_COND_NOT_EQUAL;
    if (_stricmp(str, "true") == 0) return GRAPH_COND_TRUE;
    if (_stricmp(str, "false") == 0) return GRAPH_COND_FALSE;
    return GRAPH_COND_EQUAL;
}

static int FindEasingTypeByName(const char* name)
{
    if (!name || !name[0]) return EASING_LINEAR;
    for (int i = 0; i < EASING_COUNT; ++i)
    {
        const char* n = GetEasingName(i);
        if (n && _stricmp(n, name) == 0)
            return i;
    }
    if (_stricmp(name, "bezier") == 0 || _stricmp(name, "cubicbezier") == 0)
        return EASING_BEZIER_CUBIC;
    if (_stricmp(name, "linear") == 0)
        return EASING_LINEAR;
    return EASING_LINEAR;
}

static void BuildSubtreeMask(const hkaSkeleton* skeleton, int root, std::vector<float>& outMask)
{
    outMask.clear();
    if (!skeleton || skeleton->m_numBones <= 0)
        return;

    const int boneCount = skeleton->m_numBones;
    outMask.assign(boneCount, 1.0f);
    if (root < 0 || root >= boneCount || !skeleton->m_parentIndices)
        return;

    outMask.assign(boneCount, 0.0f);
    const hkInt16* parents = skeleton->m_parentIndices;
    for (int i = 0; i < boneCount; ++i)
    {
        int p = i;
        while (p >= 0 && p < boneCount)
        {
            if (p == root)
            {
                outMask[i] = 1.0f;
                break;
            }
            p = (int)parents[p];
        }
    }
}

static bool EvalGraphCondition(const GraphCondition& cond, const GraphParam& param)
{
    switch (param.type)
    {
    case Scene3DRenderer::GRAPH_PARAM_FLOAT:
    {
        float v = param.f;
        switch (cond.op)
        {
        case GRAPH_COND_GREATER:        return v > cond.f;
        case GRAPH_COND_LESS:           return v < cond.f;
        case GRAPH_COND_GREATER_EQUAL:  return v >= cond.f;
        case GRAPH_COND_LESS_EQUAL:     return v <= cond.f;
        case GRAPH_COND_NOT_EQUAL:      return v != cond.f;
        case GRAPH_COND_TRUE:           return v != 0.0f;
        case GRAPH_COND_FALSE:          return v == 0.0f;
        case GRAPH_COND_TEST_BITS_TRUE:  return (((int)v) & cond.i) != 0;
        case GRAPH_COND_TEST_BITS_FALSE: return (((int)v) & cond.i) == 0;
        case GRAPH_COND_EQUAL:
        default:                        return v == cond.f;
        }
    }
    case Scene3DRenderer::GRAPH_PARAM_INT:
    {
        int v = param.i;
        switch (cond.op)
        {
        case GRAPH_COND_GREATER:        return v > cond.i;
        case GRAPH_COND_LESS:           return v < cond.i;
        case GRAPH_COND_GREATER_EQUAL:  return v >= cond.i;
        case GRAPH_COND_LESS_EQUAL:     return v <= cond.i;
        case GRAPH_COND_NOT_EQUAL:      return v != cond.i;
        case GRAPH_COND_TRUE:           return v != 0;
        case GRAPH_COND_FALSE:          return v == 0;
        case GRAPH_COND_TEST_BITS_TRUE:  return (v & cond.i) != 0;
        case GRAPH_COND_TEST_BITS_FALSE: return (v & cond.i) == 0;
        case GRAPH_COND_EQUAL:
        default:                        return v == cond.i;
        }
    }
    case Scene3DRenderer::GRAPH_PARAM_BOOL:
    case Scene3DRenderer::GRAPH_PARAM_TRIGGER:
    default:
    {
        bool v = (param.type == Scene3DRenderer::GRAPH_PARAM_TRIGGER) ? param.triggered : param.b;
        switch (cond.op)
        {
        case GRAPH_COND_NOT_EQUAL:      return v != cond.b;
        case GRAPH_COND_FALSE:          return !v;
        case GRAPH_COND_TRUE:           return v;
        case GRAPH_COND_EQUAL:
        default:                        return v == cond.b;
        }
    }
    }
}

static bool GraphTransitionConditionsMet(const GraphTransition& tr, const AnimationGraphRuntime& graph)
{
    const std::vector<GraphParam>& params = GetGraphParamsConst(graph);
    for (size_t i = 0; i < tr.conditions.size(); ++i)
    {
        const GraphCondition& cond = tr.conditions[i];
        if (cond.paramIndex < 0 || cond.paramIndex >= (int)params.size())
            return false;
        if (!EvalGraphCondition(cond, params[cond.paramIndex]))
            return false;
    }
    return true;
}

static void ConsumeGraphTransitionTriggers(const GraphTransition& tr, AnimationGraphRuntime& graph)
{
    std::vector<GraphParam>& params = GetGraphParams(graph);
    for (size_t i = 0; i < tr.conditions.size(); ++i)
    {
        const GraphCondition& cond = tr.conditions[i];
        if (cond.paramIndex < 0 || cond.paramIndex >= (int)params.size())
            continue;
        GraphParam& p = params[cond.paramIndex];
        if (p.type == Scene3DRenderer::GRAPH_PARAM_TRIGGER)
        {
            p.triggered = false;
        }
    }
}

static bool GraphConditionSetMet(const std::vector<GraphCondition>& conds, const AnimationGraphRuntime& graph)
{
    const std::vector<GraphParam>& params = GetGraphParamsConst(graph);
    for (size_t i = 0; i < conds.size(); ++i)
    {
        const GraphCondition& cond = conds[i];
        if (cond.paramIndex < 0 || cond.paramIndex >= (int)params.size())
            return false;
        if (!EvalGraphCondition(cond, params[cond.paramIndex]))
            return false;
    }
    return true;
}

static bool GraphConditionSetsMet(const std::vector< std::vector<GraphCondition> >& sets, const AnimationGraphRuntime& graph)
{
    if (sets.empty())
        return true;
    for (size_t i = 0; i < sets.size(); ++i)
    {
        if (GraphConditionSetMet(sets[i], graph))
            return true;
    }
    return false;
}

static void SelectGraphStatePickConditional(AnimationGraphRuntime& graph, GraphState& state)
{
    if (state.pickAnims.empty())
        return;

    int pickIndex = state.pickDefault;
    for (size_t i = 0; i < state.pickCandidates.size(); ++i)
    {
        const GraphPickCandidate& cand = state.pickCandidates[i];
        if (cand.pickIndex < 0 || cand.pickIndex >= (int)state.pickAnims.size())
            continue;
        if (GraphConditionSetsMet(cand.conditionSets, graph))
        {
            pickIndex = cand.pickIndex;
            break;
        }
    }

    if (pickIndex < 0 || pickIndex >= (int)state.pickAnims.size())
        pickIndex = 0;

    GraphPickAnim& anim = state.pickAnims[pickIndex];
    state.pickSelected = pickIndex;
    state.clipPath = anim.clipPath;
    state.clip = anim.clip;
    state.duration = anim.duration;
    state.frameTime = anim.frameTime;
    state.loop = anim.loop;
    state.speed = anim.speed;
    state.doneEvent = anim.doneEvent;
}

static int FindBestGraphTransition(const AnimationGraphRuntime& graph, int fromState, float stateTime)
{
    int bestIdx = -1;
    int bestPriority = -2147483647;
    float duration = 0.0f;
    if (fromState >= 0 && fromState < (int)graph.states.size())
    {
        duration = graph.states[fromState].duration;
    }
    float norm = (duration > 0.0f) ? (stateTime / duration) : 0.0f;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;

    for (int i = 0; i < (int)graph.transitions.size(); ++i)
    {
        const GraphTransition& tr = graph.transitions[i];
        if (!tr.anyState)
        {
            if (tr.fromState != fromState)
                continue;
        }

        if (tr.toState < 0 || tr.toState >= (int)graph.states.size())
            continue;

        if (tr.exitTime >= 0.0f && duration > 0.0f && norm < tr.exitTime)
            continue;
        if (tr.maxTime >= 0.0f && duration > 0.0f && norm > tr.maxTime)
            continue;

        if (!GraphTransitionConditionsMet(tr, graph))
            continue;

        if (tr.priority > bestPriority)
        {
            bestPriority = tr.priority;
            bestIdx = i;
        }
    }
    return bestIdx;
}

static void StartGraphTransition(AnimationGraphRuntime& graph, int transitionIndex)
{
    if (transitionIndex < 0 || transitionIndex >= (int)graph.transitions.size())
        return;
    GraphTransition& tr = graph.transitions[transitionIndex];
    if (tr.toState < 0 || tr.toState >= (int)graph.states.size())
        return;

    graph.activeTransition = transitionIndex;
    graph.nextState = tr.toState;
    graph.transitionDuration = tr.duration;
    graph.transitionTime = 0.0f;
    graph.inTransition = (tr.duration > 0.0f);

    float nextDuration = graph.states[graph.nextState].duration;
    if ((tr.syncTime || tr.syncHint != 0) && graph.currentState >= 0 && graph.currentState < (int)graph.states.size())
    {
        float curDuration = graph.states[graph.currentState].duration;
        float norm = (curDuration > 0.0f) ? (graph.stateTime / curDuration) : 0.0f;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        if (tr.syncHint == 2)
            graph.nextTime = (1.0f - norm) * nextDuration;
        else
            graph.nextTime = norm * nextDuration;
    }
    else
    {
        graph.nextTime = 0.0f;
    }
    graph.lastNextEventTime = graph.nextTime;

    if (!graph.inTransition)
    {
        graph.currentState = graph.nextState;
        graph.stateTime = graph.nextTime;
        graph.nextState = -1;
        graph.activeTransition = -1;
        graph.transitionDuration = 0.0f;
        graph.transitionTime = 0.0f;
        graph.lastEventTime = graph.stateTime;
        graph.lastNextEventTime = 0.0f;
    }
}

static bool IsGraphEventTriggered(AnimationGraphRuntime& graph, const char* name)
{
    int idx = FindGraphParamIndex(graph, name);
    if (idx < 0)
        return false;
    std::vector<GraphParam>& params = GetGraphParams(graph);
    if (idx >= (int)params.size())
        return false;
    GraphParam& p = params[idx];
    if (p.type == Scene3DRenderer::GRAPH_PARAM_TRIGGER)
        return p.triggered;
    return p.b;
}

static void TriggerGraphEventRecursive(Scene3DRenderer& renderer, AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0])
        return;
    int idx = FindGraphParamIndex(graph, name);
    if (idx >= 0)
    {
        std::vector<GraphParam>& params = GetGraphParams(graph);
        if (idx >= 0 && idx < (int)params.size())
        {
            GraphParam& p = params[idx];
            if (p.type == Scene3DRenderer::GRAPH_PARAM_TRIGGER)
            {
                p.triggered = true;
                renderer.queueGraphOneShotTrigger(&graph, idx);
            }
        }
    }

    for (int i = 0; i < (int)graph.subMachines.size(); ++i)
    {
        AnimationGraphRuntime* sm = graph.subMachines[i];
        if (sm)
            TriggerGraphEventRecursive(renderer, *sm, name);
    }
}

static bool GetActionArgAsString(const GraphActionArg& arg, std::string& out)
{
    if (arg.type != GRAPH_ACT_ARG_STRING)
        return false;
    out = arg.s;
    return true;
}

static bool GetActionArgAsFloat(const GraphActionArg& arg, float& out)
{
    if (arg.type == GRAPH_ACT_ARG_FLOAT || arg.type == GRAPH_ACT_ARG_INT)
    {
        out = arg.f;
        return true;
    }
    if (arg.type == GRAPH_ACT_ARG_BOOL)
    {
        out = arg.b ? 1.0f : 0.0f;
        return true;
    }
    if (arg.type == GRAPH_ACT_ARG_STRING)
    {
        out = (float)(int)HashStringLowerRuntime(arg.s.c_str());
        return true;
    }
    return false;
}

static void ExecuteGraphAction(Scene3DRenderer& renderer, const GraphAction& action)
{
    if (_stricmp(action.name.c_str(), "SetLocal") == 0)
    {
        if (action.args.size() < 2)
            return;
        std::string name;
        if (!GetActionArgAsString(action.args[0], name))
            return;
        float value = 0.0f;
        if (!GetActionArgAsFloat(action.args[1], value))
            return;
        renderer.setLocalVar(name.c_str(), value);
        return;
    }
    if (_stricmp(action.name.c_str(), "AddLocal") == 0)
    {
        if (action.args.size() < 2)
            return;
        std::string name;
        if (!GetActionArgAsString(action.args[0], name))
            return;
        float value = 0.0f;
        if (!GetActionArgAsFloat(action.args[1], value))
            return;
        float cur = renderer.getLocalVar(name.c_str());
        renderer.setLocalVar(name.c_str(), cur + value);
        return;
    }
    if (_stricmp(action.name.c_str(), "SetCreatureData") == 0)
    {
        if (action.args.size() < 2)
            return;
        std::string fieldName;
        if (!GetActionArgAsString(action.args[0], fieldName))
            return;

        // Get the value — for string args this hashes via FNV-1a (same as
        // the condition parser), for numeric args it's the raw float.
        float value = 0.0f;
        GetActionArgAsFloat(action.args[1], value);

        // Store in the named creature-data map
        renderer.setCreatureDataNamed(fieldName.c_str(), value);

        // Mirror well-known fields into the CV_* float array so that
        // animation-graph conditions like e_Equal(CV_ACTION, "Attack") work.
        if (_stricmp(fieldName.c_str(), "Action") == 0)
            renderer.setCreatureVar(44, 0, value);   // CV_ACTION
        else if (_stricmp(fieldName.c_str(), "Stance") == 0)
            renderer.setCreatureVar(43, 0, value);   // CV_STANCE
        return;
    }
}

static void ExecuteGraphActions(Scene3DRenderer& renderer, const std::vector<GraphAction>& actions)
{
    for (size_t i = 0; i < actions.size(); ++i)
        ExecuteGraphAction(renderer, actions[i]);
}

static void DispatchGraphStateEvents(Scene3DRenderer& renderer, AnimationGraphRuntime& graph, GraphState& state, int eventType)
{
    for (size_t i = 0; i < state.events.size(); ++i)
    {
        GraphStateEvent& evt = state.events[i];
        if (evt.type != eventType)
            continue;
        if (!evt.postEvent.empty())
            TriggerGraphEventRecursive(renderer, graph, evt.postEvent.c_str());
    }
}

static void HandleGraphStateEnter(Scene3DRenderer& renderer, AnimationGraphRuntime& graph, int stateIndex)
{
    if (stateIndex < 0 || stateIndex >= (int)graph.states.size())
        return;
    GraphState& state = graph.states[stateIndex];
    SelectGraphStatePickConditional(graph, state);
    ExecuteGraphActions(renderer, state.onEnterActions);
    DispatchGraphStateEvents(renderer, graph, state, 0);
}

static void HandleGraphStateExit(Scene3DRenderer& renderer, AnimationGraphRuntime& graph, int stateIndex)
{
    if (stateIndex < 0 || stateIndex >= (int)graph.states.size())
        return;
    GraphState& state = graph.states[stateIndex];
    ExecuteGraphActions(renderer, state.onExitActions);
    DispatchGraphStateEvents(renderer, graph, state, 1);
}

static void ProcessGraphStateOnEventHandlers(Scene3DRenderer& renderer, AnimationGraphRuntime& graph, int stateIndex)
{
    if (stateIndex < 0 || stateIndex >= (int)graph.states.size())
        return;
    GraphState& state = graph.states[stateIndex];

    for (size_t i = 0; i < state.events.size(); ++i)
    {
        GraphStateEvent& evt = state.events[i];
        if (evt.type != 3 || evt.onEvent.empty() || evt.postEvent.empty())
            continue;
        if (IsGraphEventTriggered(graph, evt.onEvent.c_str()))
            TriggerGraphEventRecursive(renderer, graph, evt.postEvent.c_str());
    }

    for (size_t i = 0; i < state.onEventActions.size(); ++i)
    {
        GraphStateEventAction& action = state.onEventActions[i];
        if (action.eventName.empty())
            continue;
        if (IsGraphEventTriggered(graph, action.eventName.c_str()))
            ExecuteGraphActions(renderer, action.actions);
    }
}

static float GetGraphStateDuration(const GraphState& state);

static void ProcessGraphStateTimedEvents(Scene3DRenderer& renderer,
                                         AnimationGraphRuntime& graph,
                                         int stateIndex,
                                         float prevTime,
                                         float currTime,
                                         bool loop)
{
    if (stateIndex < 0 || stateIndex >= (int)graph.states.size())
        return;
    GraphState& state = graph.states[stateIndex];
    if (state.events.empty())
        return;

    float duration = GetGraphStateDuration(state);
    bool wrapped = loop && (currTime + 1e-4f < prevTime);

    for (size_t i = 0; i < state.events.size(); ++i)
    {
        GraphStateEvent& evt = state.events[i];
        if (evt.type != 2 || evt.postEvent.empty())
            continue;
        float t = evt.time;
        if (t < 0.0f)
            continue;

        bool fire = false;
        if (!wrapped)
        {
            if (t > prevTime && t <= currTime)
                fire = true;
        }
        else
        {
            if (t > prevTime && t <= duration)
                fire = true;
            else if (t >= 0.0f && t <= currTime)
                fire = true;
        }

        if (fire)
            TriggerGraphEventRecursive(renderer, graph, evt.postEvent.c_str());
    }
}

static void ProcessGraphStateDoneEvent(Scene3DRenderer& renderer,
                                       AnimationGraphRuntime& graph,
                                       int stateIndex,
                                       float prevTime,
                                       float currTime,
                                       bool loop)
{
    if (stateIndex < 0 || stateIndex >= (int)graph.states.size())
        return;
    GraphState& state = graph.states[stateIndex];
    if (state.doneEvent.empty())
        return;
    float duration = GetGraphStateDuration(state);
    if (duration <= 0.0f)
        return;

    bool wrapped = loop && (currTime + 1e-4f < prevTime);
    if (!wrapped)
    {
        if (prevTime < duration && currTime >= duration)
            TriggerGraphEventRecursive(renderer, graph, state.doneEvent.c_str());
    }
    else
    {
        TriggerGraphEventRecursive(renderer, graph, state.doneEvent.c_str());
    }
}

static bool ParseThreeComp40ArrayRange(const char* start, const char* end, std::vector<ThreeComp40>& outValues)
{
    if (!start || !end) return false;
    const char* key = FindKeyInRangeAnim(start, end, "ThreeComp40");
    if (!key) return false;
    const char* arrStart = strchr(key, '[');
    if (!arrStart || arrStart >= end) return false;
    const char* arrEnd = FindMatchingAnim(arrStart, '[', ']');
    if (!arrEnd || arrEnd > end) return false;

    const char* p = arrStart + 1;
    while (p && p < arrEnd)
    {
        p = SkipWhitespaceAnim(p);
        if (!p || p >= arrEnd) break;
        if (*p == '{')
        {
            const char* objEnd = FindMatchingAnim(p, '{', '}');
            if (!objEnd || objEnd > arrEnd) break;

            ThreeComp40 v;
            v.a = v.b = v.c = v.d = v.e = 0;
            ExtractIntValueRangeAnim(p, objEnd, "a", &v.a);
            ExtractIntValueRangeAnim(p, objEnd, "b", &v.b);
            ExtractIntValueRangeAnim(p, objEnd, "c", &v.c);
            ExtractIntValueRangeAnim(p, objEnd, "d", &v.d);
            ExtractIntValueRangeAnim(p, objEnd, "e", &v.e);
            outValues.push_back(v);

            p = objEnd + 1;
        }
        else
        {
            p++;
        }
    }

    return !outValues.empty();
}

// ---------------------------------------------------------------------------
// Type2 translation track decoding
// ---------------------------------------------------------------------------

static bool DecodeType2TranslationTrack(const std::vector<int>& type2Vals,
                                        const std::vector<int>& frameData,
                                        const std::vector<float>& valsA,
                                        int flags,
                                        int s1,
                                        const hkVector4& refT,
                                        int packingMode,
                                        std::vector<int>& outFrames,
                                        std::vector<JsonVec3>& outTranslations)
{
    if (type2Vals.empty())
        return false;

    int axisMask = flags & 0x70; // 0x10=X, 0x20=Y, 0x40=Z
    int animatedAxes[3] = { 0, 0, 0 };
    int animatedCount = 0;
    if (axisMask & 0x10) animatedAxes[animatedCount++] = 0;
    if (axisMask & 0x20) animatedAxes[animatedCount++] = 1;
    if (axisMask & 0x40) animatedAxes[animatedCount++] = 2;

    int frameCount = (s1 >= 0) ? (s1 + 1) : 0;
    if (frameCount <= 0 && frameData.size() >= 4)
        frameCount = (int)frameData.size() - 4;
    if (frameCount <= 0)
        frameCount = (int)type2Vals.size();

    int componentCount = (frameCount > 0) ? (int)(type2Vals.size() / frameCount) : 0;
    if (componentCount <= 0)
        return false;

    if (animatedCount == 0)
    {
        animatedCount = componentCount;
        for (int i = 0; i < componentCount && i < 3; i++)
        {
            animatedAxes[i] = i;
        }
    }

    if (componentCount != animatedCount)
    {
        if (componentCount < animatedCount)
        {
            animatedCount = componentCount;
        }
        else
        {
            for (int ax = 0; ax < 3 && animatedCount < componentCount; ++ax)
            {
                bool already = false;
                for (int k = 0; k < animatedCount; ++k)
                {
                    if (animatedAxes[k] == ax) { already = true; break; }
                }
                if (!already)
                {
                    animatedAxes[animatedCount++] = ax;
                }
            }
        }
    }

    int totalVals = (int)valsA.size();
    int constantsProvided = totalVals - 2 * animatedCount;
    if (constantsProvided < 0) constantsProvided = 0;

    bool constAxis[3] = { false, false, false };
    int remainingConsts = constantsProvided;
    const int priorityOrder[3] = { 1, 2, 0 }; // prefer Y, then Z, then X
    for (int p = 0; p < 3 && remainingConsts > 0; ++p)
    {
        int ax = priorityOrder[p];
        bool animated = false;
        for (int k = 0; k < animatedCount; ++k)
        {
            if (animatedAxes[k] == ax) { animated = true; break; }
        }
        if (!animated)
        {
            constAxis[ax] = true;
            remainingConsts--;
        }
    }

    float minVal[3] = { 0.0f, 0.0f, 0.0f };
    float maxVal[3] = { 0.0f, 0.0f, 0.0f };
    float constVal[3] = { refT(0), refT(1), refT(2) };

    int vIdx = 0;
    for (int ax = 0; ax < 3; ++ax)
    {
        bool animated = false;
        for (int k = 0; k < animatedCount; ++k)
        {
            if (animatedAxes[k] == ax) { animated = true; break; }
        }

        if (animated)
        {
            if (vIdx + 1 < totalVals)
            {
                minVal[ax] = valsA[vIdx++];
                maxVal[ax] = valsA[vIdx++];
            }
        }
        else if (constAxis[ax])
        {
            if (vIdx < totalVals)
            {
                constVal[ax] = valsA[vIdx++];
            }
        }
    }

    outFrames.clear();
    outTranslations.clear();
    outFrames.reserve(frameCount);
    outTranslations.reserve(frameCount);

    for (int f = 0; f < frameCount; ++f)
    {
        int frameIdx = f;
        if ((int)frameData.size() >= frameCount + 4)
        {
            frameIdx = frameData[4 + f];
        }

        float compVals[3] = { constVal[0], constVal[1], constVal[2] };

        for (int c = 0; c < animatedCount; ++c)
        {
            int ax = animatedAxes[c];
        int idx = 0;
        if (packingMode == 1)
        {
            idx = c * frameCount + f;
        }
        else
        {
            idx = f * componentCount + c;
        }
            if (idx < 0 || idx >= (int)type2Vals.size()) continue;

            float norm = (float)type2Vals[idx] / 65535.0f;
            float v = minVal[ax] + (maxVal[ax] - minVal[ax]) * norm;
            compVals[ax] = v;
        }

        JsonVec3 t;
        t.x = compVals[0];
        t.y = compVals[1];
        t.z = compVals[2];

        outFrames.push_back(frameIdx);
        outTranslations.push_back(t);
    }

    return !outFrames.empty();
}

// ---------------------------------------------------------------------------
// String-to-enum mapping for animation event types
// ---------------------------------------------------------------------------

static AnimEventType AnimEventTypeFromString(const char* name)
{
    if (!name || !name[0]) return ANIM_EVT_UNKNOWN;

    // Damage / Combat
    if (strcmp(name, "ApplyDamage") == 0)                return ANIM_EVT_APPLY_DAMAGE;
    if (strcmp(name, "ApplyDamageToTarget") == 0)        return ANIM_EVT_APPLY_DAMAGE_TO_TARGET;
    if (strcmp(name, "DamageOBBArea") == 0)              return ANIM_EVT_DAMAGE_OBB_AREA;
    if (strcmp(name, "DamageOBBArea_LeftHand") == 0)     return ANIM_EVT_DAMAGE_OBB_AREA_LEFT_HAND;
    if (strcmp(name, "DamageOBBArea_RightHand") == 0)    return ANIM_EVT_DAMAGE_OBB_AREA_RIGHT_HAND;
    if (strcmp(name, "DamageCylinderArea") == 0)         return ANIM_EVT_DAMAGE_CYLINDER_AREA;
    if (strcmp(name, "DamageCylinderArea_RightHand") == 0) return ANIM_EVT_DAMAGE_CYLINDER_AREA_RIGHT_HAND;
    if (strcmp(name, "FFOnlyDamageOBBArea") == 0)        return ANIM_EVT_FF_ONLY_DAMAGE_OBB_AREA;
    if (strcmp(name, "FFDamageOBBArea_RightHand") == 0)  return ANIM_EVT_FF_DAMAGE_OBB_AREA_RIGHT_HAND;
    if (strcmp(name, "ClearHitList") == 0)               return ANIM_EVT_CLEAR_HIT_LIST;
    if (strcmp(name, "FlyerClearHitList") == 0)          return ANIM_EVT_FLYER_CLEAR_HIT_LIST;
    if (strcmp(name, "FlyerDamageOBBArea") == 0)         return ANIM_EVT_FLYER_DAMAGE_OBB_AREA;
    if (strcmp(name, "GrabOBBArea_LeftHand") == 0)       return ANIM_EVT_GRAB_OBB_AREA_LEFT_HAND;
    if (strcmp(name, "GrabOBBArea_RightHand") == 0)      return ANIM_EVT_GRAB_OBB_AREA_RIGHT_HAND;
    if (strcmp(name, "NewHit2") == 0)                    return ANIM_EVT_NEW_HIT2;
    if (strcmp(name, "NewHit3") == 0)                    return ANIM_EVT_NEW_HIT3;
    if (strcmp(name, "NewHit4") == 0)                    return ANIM_EVT_NEW_HIT4;
    if (strcmp(name, "NewHit5") == 0)                    return ANIM_EVT_NEW_HIT5;

    // Trails / Visual FX
    if (strcmp(name, "TrailOn_Left") == 0)               return ANIM_EVT_TRAIL_ON_LEFT;
    if (strcmp(name, "TrailOn_Right") == 0)              return ANIM_EVT_TRAIL_ON_RIGHT;
    if (strcmp(name, "TrailOn_Right_Split") == 0)        return ANIM_EVT_TRAIL_ON_RIGHT_SPLIT;
    if (strcmp(name, "TrailOff") == 0)                   return ANIM_EVT_TRAIL_OFF;
    if (strcmp(name, "TrailOff_Left") == 0)              return ANIM_EVT_TRAIL_OFF_LEFT;
    if (strcmp(name, "TrailOff_Right") == 0)             return ANIM_EVT_TRAIL_OFF_RIGHT;
    if (strcmp(name, "ActivateAttachedParticleEffect") == 0) return ANIM_EVT_ACTIVATE_ATTACHED_PARTICLE_EFFECT;
    if (strcmp(name, "ActivateParticleEffect") == 0)     return ANIM_EVT_ACTIVATE_PARTICLE_EFFECT;

    // Sound
    if (strcmp(name, "SoundCue") == 0)                   return ANIM_EVT_SOUND_CUE;
    if (strcmp(name, "SoundEvent") == 0)                 return ANIM_EVT_SOUND_EVENT;
    if (strcmp(name, "SetSoundOverride") == 0)           return ANIM_EVT_SET_SOUND_OVERRIDE;

    // Camera
    if (strcmp(name, "Camera") == 0)                     return ANIM_EVT_CAMERA;
    if (strcmp(name, "CameraEffect") == 0)               return ANIM_EVT_CAMERA_EFFECT;
    if (strcmp(name, "CameraEffectAlways") == 0)         return ANIM_EVT_CAMERA_EFFECT_ALWAYS;

    // State / Logic
    if (strcmp(name, "StateChange1") == 0)               return ANIM_EVT_STATE_CHANGE1;
    if (strcmp(name, "StateChange2") == 0)               return ANIM_EVT_STATE_CHANGE2;
    if (strcmp(name, "BeginFaceTarget") == 0)            return ANIM_EVT_BEGIN_FACE_TARGET;
    if (strcmp(name, "EndFaceTarget") == 0)              return ANIM_EVT_END_FACE_TARGET;
    if (strcmp(name, "PostGenericEvent") == 0)           return ANIM_EVT_POST_GENERIC_EVENT;
    if (strcmp(name, "Jump") == 0)                       return ANIM_EVT_JUMP;
    if (strcmp(name, "Destroy") == 0)                    return ANIM_EVT_DESTROY;
    if (strcmp(name, "SHInvincible") == 0)               return ANIM_EVT_SH_INVINCIBLE;
    if (strcmp(name, "SelectInventoryLoadout") == 0)     return ANIM_EVT_SELECT_INVENTORY_LOADOUT;
    if (strcmp(name, "EnableDrone") == 0)                 return ANIM_EVT_ENABLE_DRONE;
    if (strcmp(name, "AbilityActivationEvent") == 0)     return ANIM_EVT_ABILITY_ACTIVATION_EVENT;
    if (strcmp(name, "SetRotationRateScale") == 0)       return ANIM_EVT_SET_ROTATION_RATE_SCALE;
    if (strcmp(name, "StartCharge") == 0)                return ANIM_EVT_START_CHARGE;
    if (strcmp(name, "StopCharge") == 0)                 return ANIM_EVT_STOP_CHARGE;

    // Projectile
    if (strcmp(name, "FireProjectile") == 0)             return ANIM_EVT_FIRE_PROJECTILE;
    if (strcmp(name, "FireProjectile_LeftHand") == 0)    return ANIM_EVT_FIRE_PROJECTILE_LEFT_HAND;
    if (strcmp(name, "FireProjectile_RightHand") == 0)   return ANIM_EVT_FIRE_PROJECTILE_RIGHT_HAND;
    if (strcmp(name, "FireProjectile_Visual") == 0)      return ANIM_EVT_FIRE_PROJECTILE_VISUAL;
    if (strcmp(name, "FireProjectileVisual_LeftHand") == 0) return ANIM_EVT_FIRE_PROJECTILE_VISUAL_LEFT_HAND;
    if (strcmp(name, "ReadyProjectile") == 0)            return ANIM_EVT_READY_PROJECTILE;
    if (strcmp(name, "ReadyProjectile_LeftHand") == 0)   return ANIM_EVT_READY_PROJECTILE_LEFT_HAND;
    if (strcmp(name, "ReadyProjectile_RightHand") == 0)  return ANIM_EVT_READY_PROJECTILE_RIGHT_HAND;
    if (strcmp(name, "UnreadyProjectile_LeftHand") == 0) return ANIM_EVT_UNREADY_PROJECTILE_LEFT_HAND;
    if (strcmp(name, "UnreadyProjectile_RightHand") == 0) return ANIM_EVT_UNREADY_PROJECTILE_RIGHT_HAND;
    if (strcmp(name, "EmbedLaunchPoint") == 0)           return ANIM_EVT_EMBED_LAUNCH_POINT;

    // Throw
    if (strcmp(name, "Throw_LeftHand") == 0)             return ANIM_EVT_THROW_LEFT_HAND;
    if (strcmp(name, "Throw_LeftTorque") == 0)           return ANIM_EVT_THROW_LEFT_TORQUE;
    if (strcmp(name, "Throw_RightHand") == 0)            return ANIM_EVT_THROW_RIGHT_HAND;
    if (strcmp(name, "Throw_RightTorque") == 0)          return ANIM_EVT_THROW_RIGHT_TORQUE;

    // Bow
    if (strcmp(name, "GrabStringL") == 0)                return ANIM_EVT_GRAB_STRING_L;
    if (strcmp(name, "ReleaseStringL") == 0)             return ANIM_EVT_RELEASE_STRING_L;

    // Controller
    if (strcmp(name, "RumblePlay") == 0)                 return ANIM_EVT_RUMBLE_PLAY;

    return ANIM_EVT_UNKNOWN;
}

// ---------------------------------------------------------------------------
// JSON animation clip loader
// ---------------------------------------------------------------------------

JsonAnimClip* LoadJsonAnimClip(const char* path,
                               const hkaSkeleton* skeleton,
                               int decodeMode,
                               int type2PackingMode)
{
    if (!path || !skeleton) return NULL;

    int jsonSize = 0;
    char* json = LoadTextFile(path, &jsonSize);
    if (!json) return NULL;

    JsonAnimClip* clip = new JsonAnimClip();
    InitJsonAnimInfo(clip->info);
    clip->frameTime = 1.0f / 30.0f;
    clip->duration = 0.0f;
    clip->frameCount = 0;
    clip->maxFramesPerBlock = 256;
    clip->decodeCount = 0;
    clip->altDecodeCount = 0;
    clip->rootBoneIndex = -1;

    // Parse info block
    const char* infoKey = FindKeyInRangeAnim(json, json + jsonSize, "info");
    if (infoKey)
    {
        const char* infoStart = strchr(infoKey, '{');
        const char* infoEnd = infoStart ? FindMatchingAnim(infoStart, '{', '}') : NULL;
        if (infoStart && infoEnd)
        {
            ExtractStringValueRangeAnim(infoStart, infoEnd, "key", clip->info.key, sizeof(clip->info.key));
            ExtractIntValueRangeAnim(infoStart, infoEnd, "gamemodemask", &clip->info.gamemodemask);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "offset", &clip->info.offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "size", &clip->info.size);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "kind", &clip->info.kind);
            ExtractFloatValueRangeAnim(infoStart, infoEnd, "unk_5", &clip->info.unk_5);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "vals_num", &clip->info.vals_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "vals2_num", &clip->info.vals2_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "unk_8", &clip->info.unk_8);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "vala", &clip->info.vala);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "unk_10", &clip->info.unk_10);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "unk_11", &clip->info.unk_11);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "data_offset", &clip->info.data_offset);
            ExtractFloatValueRangeAnim(infoStart, infoEnd, "unk_13", &clip->info.unk_13);
            ExtractFloatValueRangeAnim(infoStart, infoEnd, "unk_14", &clip->info.unk_14);
            ExtractFloatValueRangeAnim(infoStart, infoEnd, "t_scale", &clip->info.t_scale);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "block_starts_offset", &clip->info.block_starts_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "block_starts_num", &clip->info.block_starts_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "block_starts2_offset", &clip->info.block_starts2_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "block_starts2_num", &clip->info.block_starts2_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj_c3_offset", &clip->info.obj_c3_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj_c3_num", &clip->info.obj_c3_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj_c4_offset", &clip->info.obj_c4_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj_c4_num", &clip->info.obj_c4_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "block_offset", &clip->info.block_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "block_size", &clip->info.block_size);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj3_num", &clip->info.obj3_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj3_offset", &clip->info.obj3_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "bones_num1", &clip->info.bones_num1);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "unk_29", &clip->info.unk_29);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj1_num", &clip->info.obj1_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "bones_offset", &clip->info.bones_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "unk_32", &clip->info.unk_32);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj1_offset", &clip->info.obj1_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj2_offset", &clip->info.obj2_offset);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj2_num", &clip->info.obj2_num);
            ExtractIntValueRangeAnim(infoStart, infoEnd, "obj5_offset", &clip->info.obj5_offset);

            clip->frameTime = (clip->info.t_scale > 0.0f) ? clip->info.t_scale : clip->frameTime;
            clip->duration = (clip->info.unk_5 > 0.0f) ? clip->info.unk_5 : clip->duration;
            clip->frameCount = (clip->info.vala > 0) ? clip->info.vala : clip->frameCount;
            clip->maxFramesPerBlock = (clip->info.unk_11 > 0) ? clip->info.unk_11 : clip->maxFramesPerBlock;
        }
    }
    if (clip->maxFramesPerBlock <= 0)
    {
        clip->maxFramesPerBlock = 256;
    }

    // Parse bone names
    std::vector<std::string> animBoneNames;
    const char* bonesKey = FindKeyInRangeAnim(json, json + jsonSize, "bones");
    const char* bonesArray = bonesKey ? strchr(bonesKey, '[') : NULL;
    const char* bonesEnd = bonesArray ? FindMatchingAnim(bonesArray, '[', ']') : NULL;
    if (bonesArray && bonesEnd)
    {
        ParseStringArrayRangeAnim(bonesArray + 1, bonesEnd, animBoneNames);
    }
    clip->boneNames = animBoneNames;
    std::vector<int> animToSkeleton;
    animToSkeleton.assign(animBoneNames.size(), -1);
    for (size_t i = 0; i < animBoneNames.size(); ++i)
    {
        const std::string& name = animBoneNames[i];
        if (!name.empty())
        {
            int skelIdx = FindSkeletonBoneIndex(skeleton, name.c_str());
            animToSkeleton[i] = skelIdx;
            if (skelIdx < 0)
            {
                char msg[256];
                sprintf_s(msg, "WARN: Anim bone '%s' not found in skeleton", name.c_str());
                RendererLog(msg);
            }
        }
    }

    std::vector<int> rotIndexMap;
    std::vector<int> transIndexMap;
    rotIndexMap.assign(skeleton->m_numBones, -1);
    transIndexMap.assign(skeleton->m_numBones, -1);

    // Parse all blocks
    const char* blocksKey = FindKeyInRangeAnim(json, json + jsonSize, "blocks");
    const char* blocksArray = blocksKey ? strchr(blocksKey, '[') : NULL;
    const char* blocksEnd = blocksArray ? FindMatchingAnim(blocksArray, '[', ']') : NULL;
    {
        FILE* dfp = fopen("mocap_parse_debug.log", "a");
        if (dfp) {
            fprintf(dfp, "[PARSE] file=%s bones=%d blocksKey=%s blocksArray=%s blocksEnd=%s\n",
                    path, (int)animBoneNames.size(),
                    blocksKey ? "FOUND" : "NULL",
                    blocksArray ? "FOUND" : "NULL",
                    blocksEnd ? "FOUND" : "NULL");
            fclose(dfp);
        }
    }
    if (blocksArray && blocksEnd)
    {
        const char* p = blocksArray + 1;
        int blockIndex = 0;
        while (p && p < blocksEnd)
        {
            p = SkipWhitespaceAnim(p);
            if (!p || p >= blocksEnd) break;
            if (*p != '[')
            {
                p++;
                continue;
            }

            const char* blockStart = p;
            const char* blockEnd = FindMatchingAnim(blockStart, '[', ']');
            if (!blockEnd || blockEnd > blocksEnd) break;

            int blockBase = 0;
            if (clip->maxFramesPerBlock > 0)
            {
                blockBase = blockIndex * clip->maxFramesPerBlock;
            }

            const char* bonesList = NULL;
            const char* bonesListEnd = NULL;
            const char* q = blockStart + 1;
            while (q && q < blockEnd)
            {
                q = SkipWhitespaceAnim(q);
                if (!q || q >= blockEnd) break;
                if (*q == '[')
                {
                    bonesList = q;
                    bonesListEnd = FindMatchingAnim(bonesList, '[', ']');
                    break;
                }
                q++;
            }

            if (bonesList && bonesListEnd)
            {
                if ((int)clip->type2Obj0.size() <= blockIndex)
                {
                    clip->type2Obj0.resize(blockIndex + 1);
                    clip->type2Obj2.resize(blockIndex + 1);
                    clip->rotObj1.resize(blockIndex + 1);
                    clip->type2Obj0[blockIndex].resize(animBoneNames.size());
                    clip->type2Obj2[blockIndex].resize(animBoneNames.size());
                    clip->rotObj1[blockIndex].resize(animBoneNames.size());
                }

                int boneIndex = 0;
                const char* b = bonesList + 1;
                while (b && b < bonesListEnd)
                {
                    b = SkipWhitespaceAnim(b);
                    if (!b || b >= bonesListEnd) break;
                    if (*b == ',') { b++; continue; }
                    if (*b != '[') { b++; continue; }

                    const char* boneStart = b;
                    const char* boneEnd = FindMatchingAnim(boneStart, '[', ']');
                    if (!boneEnd || boneEnd > bonesListEnd) break;

                    int skeletonBoneIndex = -1;
                    if (boneIndex >= 0 && boneIndex < (int)animBoneNames.size())
                    {
                        skeletonBoneIndex = animToSkeleton[boneIndex];
                    }

                    const char* objStart = boneStart;
                    const char* objEnd = NULL;
                    for (int objIdx = 0; objIdx < 3; objIdx++)
                    {
                        objStart = strchr(objStart, '{');
                        if (!objStart || objStart >= boneEnd) break;
                        objEnd = FindMatchingAnim(objStart, '{', '}');
                        if (!objEnd || objEnd > boneEnd) break;

                        if (objIdx == 0 || objIdx == 2)
                        {
                            std::vector<int> type2Vals;
                            std::vector<int> frameData;
                            std::vector<float> valsA;
                            int flags = 0;
                            int s1 = 0;
                            int s2 = 0;
                            int nbytes = 0;

                            ParseIntArrayRangeAnim(objStart, objEnd, "Type2", type2Vals);
                            ParseIntArrayRangeAnim(objStart, objEnd, "data", frameData);
                            ParseFloatArrayRangeAnim(objStart, objEnd, "vals_a", valsA);
                            std::vector<std::string> valsAStr;
                            ParseNumberStringArrayRangeAnim(objStart, objEnd, "vals_a", valsAStr);
                            ExtractIntValueRangeAnim(objStart, objEnd, "flags", &flags);
                            ExtractIntValueRangeAnim(objStart, objEnd, "s1", &s1);
                            ExtractIntValueRangeAnim(objStart, objEnd, "s2", &s2);
                            ExtractIntValueRangeAnim(objStart, objEnd, "nbytes", &nbytes);

                            JsonType2Block tblock;
                            tblock.nbytes = nbytes;
                            tblock.flags = flags;
                            tblock.s1 = s1;
                            tblock.s2 = s2;
                            tblock.data = frameData;
                            tblock.valsA = valsA;
                            tblock.valsAStr = valsAStr;
                            tblock.valsType2 = type2Vals;
                            tblock.valid = (nbytes != 0 || flags != 0 || s1 != 0 || s2 != 0 ||
                                            !frameData.empty() || !valsA.empty() || !type2Vals.empty());

                            if (boneIndex >= 0 && boneIndex < (int)animBoneNames.size())
                            {
                                if (objIdx == 0)
                                    clip->type2Obj0[blockIndex][boneIndex] = tblock;
                                else
                                    clip->type2Obj2[blockIndex][boneIndex] = tblock;
                            }

                            if (objIdx == 0 && skeletonBoneIndex >= 0)
                            {
                                if (!type2Vals.empty() && !valsA.empty())
                                {
                                    std::vector<int> tFrames;
                                    std::vector<JsonVec3> tVals;
                                    hkVector4 refT = skeleton->m_referencePose[skeletonBoneIndex].getTranslation();
                                    if (DecodeType2TranslationTrack(type2Vals,
                                                                    frameData,
                                                                    valsA,
                                                                    flags,
                                                                    s1,
                                                                    refT,
                                                                    type2PackingMode,
                                                                    tFrames,
                                                                    tVals))
                                    {
                                        int tIndex = transIndexMap[skeletonBoneIndex];
                                        if (tIndex < 0)
                                        {
                                            JsonTranslationTrack newTrack;
                                            newTrack.boneIndex = skeletonBoneIndex;
                                            clip->translationTracks.push_back(newTrack);
                                            tIndex = (int)clip->translationTracks.size() - 1;
                                            transIndexMap[skeletonBoneIndex] = tIndex;
                                        }

                                        JsonTranslationTrack& tTrack = clip->translationTracks[tIndex];
                                        for (int t = 0; t < (int)tFrames.size() && t < (int)tVals.size(); ++t)
                                        {
                                            tTrack.frames.push_back(tFrames[t] + blockBase);
                                            tTrack.translations.push_back(tVals[t]);
                                        }
                                    }
                                }
                            }
                        }
                        else if (objIdx == 1)
                        {
                            std::vector<ThreeComp40> rawVals;
                            std::vector<int> frameData;
                            int flags = 0;
                            int s1 = 0;
                            int s2 = 0;
                            int nbytes = 0;
                            ParseThreeComp40ArrayRange(objStart, objEnd, rawVals);
                            ParseIntArrayRangeAnim(objStart, objEnd, "data", frameData);
                            ExtractIntValueRangeAnim(objStart, objEnd, "flags", &flags);
                            ExtractIntValueRangeAnim(objStart, objEnd, "s1", &s1);
                            ExtractIntValueRangeAnim(objStart, objEnd, "s2", &s2);
                            ExtractIntValueRangeAnim(objStart, objEnd, "nbytes", &nbytes);

                            if (boneIndex >= 0 && boneIndex < (int)animBoneNames.size())
                            {
                                JsonRotBlock rblock;
                                rblock.nbytes = nbytes;
                                rblock.flags = flags;
                                rblock.s1 = s1;
                                rblock.s2 = s2;
                                rblock.data = frameData;
                                rblock.vals = rawVals;
                                rblock.valid = (nbytes != 0 || flags != 0 || s1 != 0 || s2 != 0 ||
                                                !frameData.empty() || !rawVals.empty());
                                clip->rotObj1[blockIndex][boneIndex] = rblock;
                            }

                            {
                                static int s_dbgCount = 0;
                                if (s_dbgCount < 5) {
                                    FILE* dfp = fopen("mocap_parse_debug.log", "a");
                                    if (dfp) {
                                        fprintf(dfp, "[PARSE] bone=%d skelBone=%d rawVals=%d frameData=%d nbytes=%d flags=%d s1=%d\n",
                                                boneIndex, skeletonBoneIndex, (int)rawVals.size(), (int)frameData.size(), nbytes, flags, s1);
                                        fclose(dfp);
                                    }
                                    s_dbgCount++;
                                }
                            }
                            if (!rawVals.empty() && skeletonBoneIndex >= 0)
                            {
                                int trackIndex = rotIndexMap[skeletonBoneIndex];
                                if (trackIndex < 0)
                                    {
                                        JsonTrack newTrack;
                                        newTrack.boneIndex = skeletonBoneIndex;
                                        clip->tracks.push_back(newTrack);
                                        trackIndex = (int)clip->tracks.size() - 1;
                                        rotIndexMap[skeletonBoneIndex] = trackIndex;
                                    }

                                    JsonTrack& track = clip->tracks[trackIndex];

                                    int valueCount = (int)rawVals.size();
                                    int frameStart = 0;
                                    if ((int)frameData.size() >= valueCount + 4)
                                    {
                                        frameStart = 4;
                                    }
                                    else if ((int)frameData.size() >= valueCount)
                                    {
                                        frameStart = 0;
                                    }

                                    for (int v = 0; v < valueCount; v++)
                                    {
                                        int frameIdx = v;
                                        if ((int)frameData.size() >= valueCount + frameStart)
                                        {
                                            frameIdx = frameData[frameStart + v];
                                        }
                                        frameIdx += blockBase;
                                        track.frames.push_back(frameIdx);
                                        switch (decodeMode)
                                        {
                                        case 1:  track.rotations.push_back(DecodeThreeComp40A(rawVals[v]));      break;
                                        case 2:  track.rotations.push_back(DecodeThreeComp40B(rawVals[v]));      break;
                                        case 3:  track.rotations.push_back(DecodeThreeComp40RotVec(rawVals[v])); break;
                                        default: track.rotations.push_back(DecodeThreeComp40Havok(rawVals[v]));  break;
                                        }
                                        clip->decodeCount++;
                                    }
                                }
                            }

                        objStart = objEnd + 1;
                    }

                    boneIndex++;
                    b = boneEnd + 1;
                }
            }

            blockIndex++;
            p = blockEnd + 1;
        }
    }

    // -----------------------------------------------------------------------
    // Parse obj2 (root translation per frame)
    // -----------------------------------------------------------------------
    const char* obj2Key = FindKeyInRangeAnim(json, json + jsonSize, "obj2");
    const char* obj2Array = obj2Key ? strchr(obj2Key, '[') : NULL;
    const char* obj2End = obj2Array ? FindMatchingAnim(obj2Array, '[', ']') : NULL;
    if (obj2Array && obj2End)
    {
        const char* op = obj2Array + 1;
        int frameIdx = 0;
        while (op && op < obj2End)
        {
            op = SkipWhitespaceAnim(op);
            if (!op || op >= obj2End) break;
            if (*op == '{')
            {
                const char* objE = FindMatchingAnim(op, '{', '}');
                if (!objE || objE > obj2End) break;
                float x = 0, y = 0, z = 0;
                ExtractFloatValueRangeAnim(op, objE, "x", &x);
                ExtractFloatValueRangeAnim(op, objE, "y", &y);
                ExtractFloatValueRangeAnim(op, objE, "z", &z);
                clip->rootFrames.push_back(frameIdx);
                JsonVec3 v; v.x = x; v.y = y; v.z = z;
                clip->rootTranslations.push_back(v);
                frameIdx++;
                op = objE + 1;
            }
            else
            {
                op++;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Parse events
    // -----------------------------------------------------------------------
    const char* eventsKey = FindKeyInRangeAnim(json, json + jsonSize, "events");
    const char* eventsArray = eventsKey ? strchr(eventsKey, '[') : NULL;
    const char* eventsEnd = eventsArray ? FindMatchingAnim(eventsArray, '[', ']') : NULL;
    if (eventsArray && eventsEnd)
    {
        const char* ep = eventsArray + 1;
        while (ep && ep < eventsEnd)
        {
            ep = SkipWhitespaceAnim(ep);
            if (!ep || ep >= eventsEnd) break;
            if (*ep == '{')
            {
                const char* evtEnd = FindMatchingAnim(ep, '{', '}');
                if (!evtEnd || evtEnd > eventsEnd) break;

                JsonAnimEvent evt;
                ExtractStringValueRangeAnim(ep, evtEnd, "event", evt.event, sizeof(evt.event));
                ExtractFloatValueRangeAnim(ep, evtEnd, "t", &evt.t);
                evt.eventType = AnimEventTypeFromString(evt.event);

                // Parse vals array
                const char* valsKey = FindKeyInRangeAnim(ep, evtEnd, "vals");
                const char* valsArr = valsKey ? strchr(valsKey, '[') : NULL;
                const char* valsEnd2 = valsArr ? FindMatchingAnim(valsArr, '[', ']') : NULL;
                if (valsArr && valsEnd2)
                {
                    const char* vp = valsArr + 1;
                    while (vp && vp < valsEnd2)
                    {
                        vp = SkipWhitespaceAnim(vp);
                        if (!vp || vp >= valsEnd2) break;
                        if (*vp == '{')
                        {
                            const char* valObjEnd = FindMatchingAnim(vp, '{', '}');
                            if (!valObjEnd || valObjEnd > valsEnd2) break;

                            JsonAnimEventVal val;
                            float fv = 0;
                            int iv = 0;
                            char cv[128] = {0};
                            if (ExtractFloatValueRangeAnim(vp, valObjEnd, "Float", &fv))
                            {
                                val.type = JsonAnimEventVal::EVT_FLOAT;
                                val.floatVal = fv;
                            }
                            else if (ExtractStringValueRangeAnim(vp, valObjEnd, "CRC", cv, sizeof(cv)))
                            {
                                val.type = JsonAnimEventVal::EVT_CRC;
                                strcpy_s(val.crcVal, cv);
                            }
                            else if (ExtractIntValueRangeAnim(vp, valObjEnd, "Int", &iv))
                            {
                                val.type = JsonAnimEventVal::EVT_INT;
                                val.intVal = iv;
                            }
                            evt.vals.push_back(val);
                            vp = valObjEnd + 1;
                        }
                        else
                        {
                            vp++;
                        }
                    }
                }
                clip->events.push_back(evt);
                ep = evtEnd + 1;
            }
            else
            {
                ep++;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Parse obj5_a and obj5_b
    // -----------------------------------------------------------------------
    const char* obj5aNames[2] = { "obj5_a", "obj5_b" };
    std::vector<JsonObj5Entry>* obj5Targets[2] = { &clip->obj5a, &clip->obj5b };
    for (int oi = 0; oi < 2; oi++)
    {
        const char* o5Key = FindKeyInRangeAnim(json, json + jsonSize, obj5aNames[oi]);
        const char* o5Arr = o5Key ? strchr(o5Key, '[') : NULL;
        const char* o5End = o5Arr ? FindMatchingAnim(o5Arr, '[', ']') : NULL;
        if (o5Arr && o5End)
        {
            const char* o5p = o5Arr + 1;
            while (o5p && o5p < o5End)
            {
                o5p = SkipWhitespaceAnim(o5p);
                if (!o5p || o5p >= o5End) break;
                if (*o5p == '{')
                {
                    const char* o5ObjEnd = FindMatchingAnim(o5p, '{', '}');
                    if (!o5ObjEnd || o5ObjEnd > o5End) break;
                    JsonObj5Entry entry;
                    ExtractFloatValueRangeAnim(o5p, o5ObjEnd, "unk_0", &entry.unk_0);
                    ExtractFloatValueRangeAnim(o5p, o5ObjEnd, "unk_1", &entry.unk_1);
                    ExtractFloatValueRangeAnim(o5p, o5ObjEnd, "unk_2", &entry.unk_2);
                    ExtractFloatValueRangeAnim(o5p, o5ObjEnd, "unk_3", &entry.unk_3);
                    ExtractFloatValueRangeAnim(o5p, o5ObjEnd, "unk_4", &entry.unk_4);
                    ExtractFloatValueRangeAnim(o5p, o5ObjEnd, "unk_5", &entry.unk_5);
                    ExtractFloatValueRangeAnim(o5p, o5ObjEnd, "unk_6", &entry.unk_6);
                    obj5Targets[oi]->push_back(entry);
                    o5p = o5ObjEnd + 1;
                }
                else
                {
                    o5p++;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Decode sub-block 2 (scale) into scale tracks
    // -----------------------------------------------------------------------
    std::vector<int> scaleIndexMap;
    scaleIndexMap.assign(skeleton->m_numBones, -1);
    for (int bi = 0; bi < (int)clip->type2Obj2.size(); bi++)
    {
        int blockBase = bi * clip->maxFramesPerBlock;
        for (int boneIdx = 0; boneIdx < (int)clip->type2Obj2[bi].size(); boneIdx++)
        {
            const JsonType2Block& sb = clip->type2Obj2[bi][boneIdx];
            if (!sb.valid) continue;
            if (sb.valsType2.empty() && sb.valsA.empty()) continue;

            int skeletonBoneIndex = -1;
            if (boneIdx >= 0 && boneIdx < (int)animBoneNames.size())
                skeletonBoneIndex = animToSkeleton[boneIdx];
            if (skeletonBoneIndex < 0) continue;

            // For scale, static-only blocks (flags & 0x70 == 0) have constant values in valsA
            // Keyframed blocks use the same Type2 decode as translation
            if (!sb.valsType2.empty())
            {
                std::vector<int> sFrames;
                std::vector<JsonVec3> sVals;
                hkVector4 refS;
                refS.set(1.0f, 1.0f, 1.0f);
                if (DecodeType2TranslationTrack(sb.valsType2,
                                                sb.data,
                                                sb.valsA,
                                                sb.flags,
                                                sb.s1,
                                                refS,
                                                type2PackingMode,
                                                sFrames,
                                                sVals))
                {
                    int sIndex = scaleIndexMap[skeletonBoneIndex];
                    if (sIndex < 0)
                    {
                        JsonScaleTrack newTrack;
                        newTrack.boneIndex = skeletonBoneIndex;
                        clip->scaleTracks.push_back(newTrack);
                        sIndex = (int)clip->scaleTracks.size() - 1;
                        scaleIndexMap[skeletonBoneIndex] = sIndex;
                    }
                    JsonScaleTrack& sTrack = clip->scaleTracks[sIndex];
                    for (int t = 0; t < (int)sFrames.size() && t < (int)sVals.size(); ++t)
                    {
                        sTrack.frames.push_back(sFrames[t] + blockBase);
                        sTrack.scales.push_back(sVals[t]);
                    }
                }
            }
            else if (!sb.valsA.empty())
            {
                // Static scale: extract constant values from valsA based on flags
                int staticMask = sb.flags & 0x07;
                float sx = 1.0f, sy = 1.0f, sz = 1.0f;
                int vIdx = 0;
                if ((staticMask & 1) && vIdx < (int)sb.valsA.size()) sx = sb.valsA[vIdx++];
                if ((staticMask & 2) && vIdx < (int)sb.valsA.size()) sy = sb.valsA[vIdx++];
                if ((staticMask & 4) && vIdx < (int)sb.valsA.size()) sz = sb.valsA[vIdx++];

                int sIndex = scaleIndexMap[skeletonBoneIndex];
                if (sIndex < 0)
                {
                    JsonScaleTrack newTrack;
                    newTrack.boneIndex = skeletonBoneIndex;
                    clip->scaleTracks.push_back(newTrack);
                    sIndex = (int)clip->scaleTracks.size() - 1;
                    scaleIndexMap[skeletonBoneIndex] = sIndex;
                }
                JsonScaleTrack& sTrack = clip->scaleTracks[sIndex];
                sTrack.frames.push_back(blockBase);
                JsonVec3 sv; sv.x = sx; sv.y = sy; sv.z = sz;
                sTrack.scales.push_back(sv);
            }
        }
    }

    // Cache root bone index so buildPoseFromJson avoids per-frame strcmp
    for (int i = 0; i < skeleton->m_numBones; i++)
    {
        const char* name = skeleton->m_bones[i] ? skeleton->m_bones[i]->m_name : NULL;
        if (name && (strcmp(name, "bone_globalsrt") == 0 || strcmp(name, "Bone_Root") == 0))
        {
            clip->rootBoneIndex = i;
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Quaternion neighborhood correction (double-cover fix)
    // Each ThreeComp40 decode produces an arbitrary-sign quaternion.
    // Walk each rotation track and flip any quat whose dot with its
    // predecessor is negative, so the entire track lives in one hemisphere.
    // This prevents SLERP from taking the long arc (360-degree spin) between
    // adjacent keys that happen to land on opposite sides of the hypersphere.
    // Must happen once at load time, NOT per-sample.
    // -----------------------------------------------------------------------
    for (size_t ti = 0; ti < clip->tracks.size(); ++ti)
    {
        JsonTrack& track = clip->tracks[ti];
        const int keyCount = (int)track.rotations.size();
        for (int k = 1; k < keyCount; ++k)
        {
            const JsonTrack::Quat4& prev = track.rotations[k - 1];
            JsonTrack::Quat4& curr = track.rotations[k];
            float dot = prev.x * curr.x + prev.y * curr.y
                      + prev.z * curr.z + prev.w * curr.w;
            if (dot < 0.0f)
            {
                curr.x = -curr.x;
                curr.y = -curr.y;
                curr.z = -curr.z;
                curr.w = -curr.w;
            }
        }
    }

    free(json);
    return clip;
}

// ---------------------------------------------------------------------------
// Rig scanning utilities
// ---------------------------------------------------------------------------

struct RigMatch
{
    char path[MAX_PATH];
    int boneCount;
    int matchCount;
    float matchRatio;
};

static int CountMatchingBones(const hkaSkeleton* target, const hkaSkeleton* candidate)
{
    if (!target || !candidate) return 0;
    int matches = 0;
    for (int i = 0; i < target->m_numBones; i++)
    {
        const char* name = target->m_bones[i] ? target->m_bones[i]->m_name : NULL;
        if (!name || name[0] == '\0') continue;
        for (int j = 0; j < candidate->m_numBones; j++)
        {
            const char* other = candidate->m_bones[j] ? candidate->m_bones[j]->m_name : NULL;
            if (other && strcmp(name, other) == 0)
            {
                matches++;
                break;
            }
        }
    }
    return matches;
}

static bool HasHkxExtension(const char* path)
{
    if (!path) return false;
    const char* dot = strrchr(path, '.');
    return dot && (_stricmp(dot, ".hkx") == 0);
}

static void ScanHkxFile(const char* filePath, const hkaSkeleton* target, std::vector<RigMatch>& matches, FILE* logFile)
{
    if (!filePath || !target) return;

    hkLoader* loader = new hkLoader();
    hkRootLevelContainer* container = loader->load(filePath);
    if (container == HK_NULL)
    {
        delete loader;
        return;
    }

    hkaAnimationContainer* ac = reinterpret_cast<hkaAnimationContainer*>(
        container->findObjectByType(hkaAnimationContainerClass.getName()));

    if (ac)
    {
        for (int i = 0; i < ac->m_numSkeletons; i++)
        {
            hkaSkeleton* skel = ac->m_skeletons[i];
            int matchCount = CountMatchingBones(target, skel);
            if (matchCount > 0)
            {
                RigMatch rm;
                strcpy_s(rm.path, filePath);
                rm.boneCount = skel ? skel->m_numBones : 0;
                rm.matchCount = matchCount;
                rm.matchRatio = (target->m_numBones > 0) ? (float)matchCount / (float)target->m_numBones : 0.0f;
                matches.push_back(rm);

                if (logFile)
                {
                    fprintf(logFile, "Skeleton match: %s | bones=%d | matches=%d | ratio=%.2f\n",
                        filePath, rm.boneCount, rm.matchCount, rm.matchRatio);
                }
            }
        }

        for (int i = 0; i < ac->m_numBindings; i++)
        {
            hkaAnimationBinding* binding = ac->m_bindings[i];
            if (binding && binding->m_numTransformTrackToBoneIndices == target->m_numBones)
            {
                if (logFile)
                {
                    fprintf(logFile, "Binding track-count match: %s | tracks=%d\n",
                        filePath, binding->m_numTransformTrackToBoneIndices);
                }
            }
        }
    }

    delete loader;
}

static void ScanDirectoryForRigs(const char* dir, const hkaSkeleton* target, std::vector<RigMatch>& matches, FILE* logFile)
{
    if (!dir || !target) return;

    char searchPath[MAX_PATH];
    sprintf_s(searchPath, "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        char fullPath[MAX_PATH];
        sprintf_s(fullPath, "%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            ScanDirectoryForRigs(fullPath, target, matches, logFile);
        }
        else if (HasHkxExtension(fd.cFileName))
        {
            ScanHkxFile(fullPath, target, matches, logFile);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

void ScanForMatchingRigs(const hkaSkeleton* target)
{
    if (!target) return;

    FILE* logFile = fopen("rig_scan.log", "w");
    if (logFile)
    {
        fprintf(logFile, "Rig scan for target bones=%d\n", target->m_numBones);
    }

    std::vector<RigMatch> matches;
    ScanDirectoryForRigs("..\\Engine\\source\\havok\\hk550\\Demo\\Demos\\Resources\\Animation", target, matches, logFile);
    ScanDirectoryForRigs("..\\Engine\\source\\havok\\hk550\\Demo\\Demos\\Animation", target, matches, logFile);

    if (logFile)
    {
        if (matches.empty())
        {
            fprintf(logFile, "No skeleton name matches found.\n");
        }
        else
        {
            struct MatchSort
            {
                bool operator()(const RigMatch& a, const RigMatch& b) const
                {
                    return a.matchCount > b.matchCount;
                }
            } sorter;

            std::sort(matches.begin(), matches.end(), sorter);

            fprintf(logFile, "Top matches:\n");
            int top = (matches.size() < 10) ? (int)matches.size() : 10;
            for (int i = 0; i < top; i++)
            {
                fprintf(logFile, "  %d) %s | bones=%d | matches=%d | ratio=%.2f\n",
                    i + 1, matches[i].path, matches[i].boneCount, matches[i].matchCount, matches[i].matchRatio);
            }
        }
        fflush(logFile);
        fclose(logFile);
    }
}

// ---------------------------------------------------------------------------
// Scene3DRenderer member functions — animation loading
// ---------------------------------------------------------------------------

bool Scene3DRenderer::loadJsonAnimation(const char* path)
{
    if (!path || !m_gameModel || !m_gameModel->skeleton)
    {
        return false;
    }

    // Reset editor state so a new clip doesn't inherit prior pose edits.
    m_editorRotKeys.clear();
    m_editorPosKeysX.clear();
    m_editorPosKeysY.clear();
    m_editorPosKeysZ.clear();
    m_editorScaleKeysX.clear();
    m_editorScaleKeysY.clear();
    m_editorScaleKeysZ.clear();
    m_editorOverrideRot.clear();
    m_editorOverrideTrans.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_editorLastLocalPose.clear();
    m_editorRecording = false;
    m_editorDragging = false;
    m_editorDraggingTrans = false;

    if (m_jsonAnim)
    {
        delete m_jsonAnim;
        m_jsonAnim = NULL;
    }

    // Reset mode first so failed early-outs cannot keep stale animation state.
    m_useJsonAnim = false;
    m_jsonAnimTime = 0.0f;
    m_lastEventTime = 0.0f;
    m_lastEventIndex = 0;
    resetEventDedup();
    resetRootMotionState();

    // Allow broader rig compatibility (match the older, permissive behavior).
    // We keep only a loose coverage check below; no family token hard-rejects.

    m_jsonAnim = LoadJsonAnimClip(path, m_gameModel->skeleton, m_jsonDecodeMode, m_type2PackingMode);
    if (m_jsonAnim && m_jsonAnim->tracks.empty())
    {
        delete m_jsonAnim;
        m_jsonAnim = NULL;
    }

    // Guard against loading clearly incompatible class/rig animations.
    if (m_jsonAnim && m_gameModel && m_gameModel->skeleton)
    {
        const int skeletonBones = m_gameModel->skeleton->m_numBones;
        const int coveredBones = CountCoveredSkeletonBones(m_jsonAnim, skeletonBones);
        int minCovered = (int)(skeletonBones * 0.25f);
        if (minCovered < 4) minCovered = 4;

        if (m_strictRigCoverage && coveredBones < minCovered)
        {
            char msg[256];
            sprintf_s(msg,
                      "Rejected JSON animation (incompatible rig): %s (coveredBones=%d/%d, min=%d)",
                      path, coveredBones, skeletonBones, minCovered);
            RendererLog(msg);
            delete m_jsonAnim;
            m_jsonAnim = NULL;
        }
        else
        {
            char msg[256];
            sprintf_s(msg,
                      "%s JSON rig coverage: %d/%d (min hint %d) -- %s",
                      (coveredBones < minCovered) ? "Low" : "OK",
                      coveredBones, skeletonBones, minCovered,
                      m_strictRigCoverage ? "kept (strict off)" : "kept");
            RendererLog(msg);
        }
    }

    m_useJsonAnim = (m_jsonAnim != NULL);

    if (m_jsonAnim && m_compressAuto)
    {
        OptimizeJsonClipInternal(m_jsonAnim);
    }

    if (m_jsonAnim)
    {
        m_editorFrameTime = (m_jsonAnim->frameTime > 0.0f) ? m_jsonAnim->frameTime : (1.0f / 30.0f);
        float baseDuration = m_jsonAnim->duration;
        if (baseDuration <= 0.0f)
        {
            int frameCount = (m_jsonAnim->frameCount > 0) ? m_jsonAnim->frameCount : 1;
            baseDuration = m_editorFrameTime * (float)(frameCount > 1 ? (frameCount - 1) : 1);
        }
        m_editorTimelineDuration = baseDuration;

        // Initialize millisecond times for all keyframes
        initializeEditorKeyTimes();

        char msg[256];
        const char* modeLabel =
            (m_jsonDecodeMode == 1) ? "legacyA" :
            (m_jsonDecodeMode == 2) ? "legacyB" :
            (m_jsonDecodeMode == 3) ? "rotvec" : "havok";
        sprintf_s(msg, "Loaded JSON animation: %s (tracks=%d, translations=%d, altDecode=%d/%d, mode=%s, type2=%s, rot=%s, flip=%d, order=%s)",
                  path,
                  (int)m_jsonAnim->tracks.size(),
                  (int)m_jsonAnim->translationTracks.size(),
                  m_jsonAnim->altDecodeCount,
                  m_jsonAnim->decodeCount,
                  modeLabel,
                  (m_type2PackingMode == 1) ? "per-axis" : "interleaved",
                  (m_rotAxisMode == 1) ? "xzy" :
                  (m_rotAxisMode == 2) ? "zyx" :
                  (m_rotAxisMode == 3) ? "yxz" :
                  (m_rotAxisMode == 4) ? "yzx" :
                  (m_rotAxisMode == 5) ? "zxy" : "xyz",
                  m_rotSignMask,
                  (m_rotApplyMode == ROT_DELTA_ONLY) ? "delta-only" :
                  (m_rotApplyMode == ROT_DELTA_REF) ? "delta*ref" : "ref*delta");
        RendererLog(msg);
    }
    // Retarget sanity: log any bone-name mismatches and rest-pose length deviations.
    if (m_jsonAnim && m_gameModel && m_gameModel->skeleton)
    {
        int skelBones = m_gameModel->skeleton->m_numBones;
        int mismatchNames = 0;
        for (size_t i = 0; i < m_jsonAnim->boneNames.size(); ++i)
        {
            const char* animName = m_jsonAnim->boneNames[i].c_str();
            int idx = FindSkeletonBoneIndex(m_gameModel->skeleton, animName);
            if (idx < 0) mismatchNames++;
        }
        if (mismatchNames > 0)
        {
            char msg[256];
            sprintf_s(msg, "Retarget check: %d anim bones missing in skeleton (out of %d)", mismatchNames, (int)m_jsonAnim->boneNames.size());
            RendererLog(msg);
        }
    }
    else
    {
        char msg[256];
        sprintf_s(msg, "Failed to load JSON animation: %s", path);
        RendererLog(msg);
    }

    return m_useJsonAnim;
}

// ---------------------------------------------------------------------------
// PAK binary animation loader — DEFERRED (using JSON pipeline instead)
// The PakAnimLoader.h/cpp files contain the binary parser for future use.
// ---------------------------------------------------------------------------
#if 0  // Disabled — PAK binary loading deferred
bool Scene3DRenderer::loadPakAnimation(const void* animInfoPtr, const unsigned char* block1,
                                       size_t block1Size, const char* animName)
{
    if (!animInfoPtr || !block1 || block1Size == 0 || !m_gameModel || !m_gameModel->skeleton)
    {
        return false;
    }
    const ZeroEngine::AnimationInfo& animInfo = *(const ZeroEngine::AnimationInfo*)animInfoPtr;

    // Reset editor state (same as loadJsonAnimation)
    m_editorRotKeys.clear();
    m_editorPosKeysX.clear();
    m_editorPosKeysY.clear();
    m_editorPosKeysZ.clear();
    m_editorScaleKeysX.clear();
    m_editorScaleKeysY.clear();
    m_editorScaleKeysZ.clear();
    m_editorOverrideRot.clear();
    m_editorOverrideTrans.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_editorLastLocalPose.clear();
    m_editorRecording = false;
    m_editorDragging = false;
    m_editorDraggingTrans = false;

    if (m_jsonAnim)
    {
        delete m_jsonAnim;
        m_jsonAnim = NULL;
    }

    m_useJsonAnim = false;
    m_jsonAnimTime = 0.0f;
    m_lastEventTime = 0.0f;
    m_lastEventIndex = 0;
    resetEventDedup();
    resetRootMotionState();

    // Parse the binary animation data from PAK Block1
    m_jsonAnim = new JsonAnimClip();
    if (!ParsePakAnimation(animInfo, block1, block1Size, *m_jsonAnim))
    {
        RendererLog("loadPakAnimation: ParsePakAnimation failed");
        delete m_jsonAnim;
        m_jsonAnim = NULL;
        return false;
    }

    // Store animation name
    if (animName)
    {
        strncpy(m_jsonAnim->info.key, animName, 127);
        m_jsonAnim->info.key[127] = '\0';
        sprintf_s(m_jsonAnimPath, "PAK:%s", animName);
    }

    // Validate: need at least some tracks
    if (m_jsonAnim->tracks.empty())
    {
        RendererLog("loadPakAnimation: no tracks decoded");
        delete m_jsonAnim;
        m_jsonAnim = NULL;
        return false;
    }

    // Rig compatibility check
    if (m_gameModel && m_gameModel->skeleton)
    {
        const int skeletonBones = m_gameModel->skeleton->m_numBones;
        const int coveredBones = CountCoveredSkeletonBones(m_jsonAnim, skeletonBones);
        char msg[256];
        sprintf_s(msg, "PAK animation rig coverage: %d/%d bones",
                  coveredBones, skeletonBones);
        RendererLog(msg);
    }

    m_useJsonAnim = true;

    // Set up timing (same as loadJsonAnimation)
    m_editorFrameTime = (m_jsonAnim->frameTime > 0.0f) ? m_jsonAnim->frameTime : (1.0f / 30.0f);
    float baseDuration = m_jsonAnim->duration;
    if (baseDuration <= 0.0f)
    {
        int frameCount = (m_jsonAnim->frameCount > 0) ? m_jsonAnim->frameCount : 1;
        baseDuration = m_editorFrameTime * (float)(frameCount > 1 ? (frameCount - 1) : 1);
    }
    m_editorTimelineDuration = baseDuration;
    initializeEditorKeyTimes();

    char msg[256];
    sprintf_s(msg, "Loaded PAK animation: %s (tracks=%d, frames=%d, duration=%.3fs, bones=%d)",
              animName ? animName : "?",
              (int)m_jsonAnim->tracks.size(),
              m_jsonAnim->frameCount,
              baseDuration,
              (int)m_jsonAnim->boneNames.size());
    RendererLog(msg);

    return true;
}
#endif  // Disabled — PAK binary loading deferred

// ---------------------------------------------------------------------------
// Animation parameter getters / setters
// ---------------------------------------------------------------------------

bool Scene3DRenderer::setJsonAnimationPath(const char* path)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }
    m_jsonAnimPaused = false;

    if (m_gameModel && m_gameModel->skeleton)
    {
        if (loadJsonAnimation(path))
        {
            strcpy_s(m_jsonAnimPath, path);
            return true;
        }

        // Keep UI/runtime consistent when loading fails or is rejected.
        m_jsonAnimPath[0] = '\0';
        m_useJsonAnim = false;
        m_jsonAnimTime = 0.0f;
        RendererLog("setJsonAnimationPath: failed/rejected; animation state reset.");
        return false;
    }
    strcpy_s(m_jsonAnimPath, path);
    return true;
}

const char* Scene3DRenderer::getJsonAnimationPath() const
{
    return m_jsonAnimPath;
}

float Scene3DRenderer::getJsonAnimationTime() const
{
    if (m_animGraph && m_animGraph->enabled &&
        m_animGraph->currentState >= 0 &&
        m_animGraph->currentState < (int)m_animGraph->states.size())
    {
        return m_animGraph->stateTime;
    }
    return m_jsonAnimTime;
}

float Scene3DRenderer::getJsonAnimationDuration() const
{
    const JsonAnimClip* clip = getActiveJsonClipForUI();
    if (!clip)
    {
        return 0.0f;
    }
    float frameTime = GetClipFrameTime(clip);
    float duration = GetClipDuration(clip);
    if (m_editorTimelineDuration > duration)
    {
        duration = m_editorTimelineDuration;
    }
    return duration;
}

float Scene3DRenderer::getJsonAnimationFrameTime() const
{
    const JsonAnimClip* clip = getActiveJsonClipForUI();
    if (clip && clip->frameTime > 0.0f)
    {
        return clip->frameTime;
    }
    if (m_editorFrameTime > 0.0f)
    {
        return m_editorFrameTime;
    }
    return 1.0f / 30.0f;
}

int Scene3DRenderer::getJsonAnimationFrameCount() const
{
    const JsonAnimClip* clip = getActiveJsonClipForUI();
    if (clip && clip->frameCount > 0)
    {
        return clip->frameCount;
    }
    float ft = getJsonAnimationFrameTime();
    float dur = getJsonAnimationDuration();
    if (ft > 0.0f && dur > 0.0f)
    {
        int frames = (int)floor((dur / ft) + 0.5f) + 1;
        if (frames < 1) frames = 1;
        return frames;
    }
    return 0;
}

const JsonAnimClip* Scene3DRenderer::getActiveJsonClipForUI() const
{
    if (m_animGraph && m_animGraph->enabled &&
        m_animGraph->currentState >= 0 &&
        m_animGraph->currentState < (int)m_animGraph->states.size())
    {
        return m_animGraph->states[m_animGraph->currentState].clip;
    }
    return m_jsonAnim;
}

JsonAnimClip* Scene3DRenderer::getActiveJsonClipForEdit()
{
    if (m_animGraph && m_animGraph->enabled &&
        m_animGraph->currentState >= 0 &&
        m_animGraph->currentState < (int)m_animGraph->states.size())
    {
        return m_animGraph->states[m_animGraph->currentState].clip;
    }
    return m_jsonAnim;
}

// ---------------------------------------------------------------------------
// Animation graph controls (state machine / anim graph)
// ---------------------------------------------------------------------------

bool Scene3DRenderer::loadAnimationGraph(const char* path)
{
    if (!path || !path[0])
    {
        RendererLog("loadAnimationGraph: empty path");
        return false;
    }
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        RendererLog("loadAnimationGraph: no model/skeleton loaded");
        return false;
    }

    clearAnimationGraph();
    resetRootMotionState();

    int jsonSize = 0;
    char* json = LoadTextFile(path, &jsonSize);
    if (!json || jsonSize <= 0)
    {
        if (json) free(json);
        char msg[512];
        sprintf_s(msg, "loadAnimationGraph: failed to read %s", path);
        RendererLog(msg);
        return false;
    }

    AnimationGraphRuntime* graph = new AnimationGraphRuntime();
    graph->owner = graph;

    char graphName[128] = "";
    if (ExtractStringValueRangeAnim(json, json + jsonSize, "name", graphName, sizeof(graphName)))
    {
        graph->name = graphName;
    }
    else if (ExtractStringValueRangeAnim(json, json + jsonSize, "graphName", graphName, sizeof(graphName)))
    {
        graph->name = graphName;
    }

    // Parse parameters
    const char* paramsKey = FindKeyInRangeAnim(json, json + jsonSize, "parameters");
    if (!paramsKey) paramsKey = FindKeyInRangeAnim(json, json + jsonSize, "params");
    const char* paramsArray = paramsKey ? strchr(paramsKey, '[') : NULL;
    const char* paramsEnd = paramsArray ? FindMatchingAnim(paramsArray, '[', ']') : NULL;
    if (paramsArray && paramsEnd)
    {
        const char* p = paramsArray + 1;
        while (p && p < paramsEnd)
        {
            p = SkipWhitespaceAnim(p);
            if (!p || p >= paramsEnd) break;
            if (*p == '{')
            {
                const char* objStart = p;
                const char* objEnd = FindMatchingAnim(objStart, '{', '}');
                if (!objEnd || objEnd > paramsEnd) break;

                char name[128] = "";
                if (ExtractStringValueRangeAnim(objStart, objEnd, "name", name, sizeof(name)))
                {
                    GraphParam param;
                    param.name = name;

                    char typeStr[64] = "";
                    if (ExtractStringValueRangeAnim(objStart, objEnd, "type", typeStr, sizeof(typeStr)))
                    {
                        param.type = ParseGraphParamType(typeStr);
                    }

                    float minVal = 0.0f, maxVal = 0.0f;
                    bool hasMin = ExtractFloatValueRangeAnim(objStart, objEnd, "min", &minVal);
                    bool hasMax = ExtractFloatValueRangeAnim(objStart, objEnd, "max", &maxVal);
                    if (hasMin || hasMax)
                    {
                        param.hasRange = true;
                        if (hasMin) param.minValue = minVal;
                        if (hasMax) param.maxValue = maxVal;
                    }

                    if (param.type == Scene3DRenderer::GRAPH_PARAM_FLOAT)
                    {
                        float fVal = 0.0f;
                        if (ExtractFloatValueRangeAnim(objStart, objEnd, "value", &fVal) ||
                            ExtractFloatValueRangeAnim(objStart, objEnd, "default", &fVal))
                        {
                            param.f = fVal;
                            param.i = (int)fVal;
                            param.b = (fVal != 0.0f);
                        }
                    }
                    else if (param.type == Scene3DRenderer::GRAPH_PARAM_INT)
                    {
                        int iVal = 0;
                        if (ExtractIntValueRangeAnim(objStart, objEnd, "value", &iVal) ||
                            ExtractIntValueRangeAnim(objStart, objEnd, "default", &iVal))
                        {
                            param.i = iVal;
                            param.f = (float)iVal;
                            param.b = (iVal != 0);
                        }
                    }
                    else
                    {
                        bool bVal = false;
                        if (ExtractBoolValueRangeAnim(objStart, objEnd, "value", &bVal) ||
                            ExtractBoolValueRangeAnim(objStart, objEnd, "default", &bVal))
                        {
                            param.b = bVal;
                            param.f = bVal ? 1.0f : 0.0f;
                            param.i = bVal ? 1 : 0;
                        }
                    }

                    if (param.type == Scene3DRenderer::GRAPH_PARAM_TRIGGER)
                    {
                        param.triggered = false;
                    }
                    graph->params.push_back(param);
                }

                p = objEnd + 1;
                continue;
            }
            p++;
        }
    }

    ParseGraphMasksRange(json, json + jsonSize, *graph, m_gameModel->skeleton);

    const char* bgKey = FindKeyInRangeAnim(json, json + jsonSize, "blendGraphs");
    const char* bgArray = bgKey ? strchr(bgKey, '[') : NULL;
    const char* bgEnd = bgArray ? FindMatchingAnim(bgArray, '[', ']') : NULL;
    if (bgArray && bgEnd)
    {
        const char* p = bgArray + 1;
        while (p && p < bgEnd)
        {
            p = SkipWhitespaceAnim(p);
            if (!p || p >= bgEnd) break;
            if (*p == '{')
            {
                const char* objStart = p;
                const char* objEnd = FindMatchingAnim(objStart, '{', '}');
                if (!objEnd || objEnd > bgEnd) break;

                BlendGraphRuntime* bg = ParseBlendGraphObject(objStart, objEnd, path,
                                                             m_gameModel->skeleton,
                                                             m_jsonDecodeMode,
                                                             m_type2PackingMode,
                                                             m_strictRigCoverage,
                                                             *graph);
                if (bg)
                {
                    graph->blendGraphs.push_back(bg);
                }

                p = objEnd + 1;
                continue;
            }
            p++;
        }
    }

    // Parse states
    const char* statesKey = FindKeyInRangeAnim(json, json + jsonSize, "states");
    const char* statesArray = statesKey ? strchr(statesKey, '[') : NULL;
    const char* statesEnd = statesArray ? FindMatchingAnim(statesArray, '[', ']') : NULL;
    if (statesArray && statesEnd)
    {
        const char* p = statesArray + 1;
        while (p && p < statesEnd)
        {
            p = SkipWhitespaceAnim(p);
            if (!p || p >= statesEnd) break;
            if (*p == '{')
            {
                const char* objStart = p;
                const char* objEnd = FindMatchingAnim(objStart, '{', '}');
                if (!objEnd || objEnd > statesEnd) break;

                char name[128] = "";
                char clipPath[512] = "";
                char blendGraphName[128] = "";
                char stateMachineName[128] = "";
                bool hasName = ExtractStringValueRangeAnim(objStart, objEnd, "name", name, sizeof(name));
                bool hasClip = (ExtractStringValueRangeAnim(objStart, objEnd, "clip", clipPath, sizeof(clipPath)) ||
                                ExtractStringValueRangeAnim(objStart, objEnd, "path", clipPath, sizeof(clipPath)));
                bool hasBlendGraph = (ExtractStringValueRangeAnim(objStart, objEnd, "blendGraph", blendGraphName, sizeof(blendGraphName)) ||
                                      ExtractStringValueRangeAnim(objStart, objEnd, "graph", blendGraphName, sizeof(blendGraphName)));
                bool hasStateMachine = (ExtractStringValueRangeAnim(objStart, objEnd, "stateMachine", stateMachineName, sizeof(stateMachineName)) ||
                                        ExtractStringValueRangeAnim(objStart, objEnd, "machine", stateMachineName, sizeof(stateMachineName)));
                if (hasName && (hasClip || hasBlendGraph || hasStateMachine))
                {
                    GraphState state;
                    state.name = name;
                    if (hasClip) state.clipPath = clipPath;
                    if (hasBlendGraph) state.blendGraphName = blendGraphName;
                    if (hasStateMachine) state.stateMachineName = stateMachineName;

                    bool loop = true;
                    if (ExtractBoolValueRangeAnim(objStart, objEnd, "loop", &loop))
                        state.loop = loop;

                    float speed = 1.0f;
                    if (ExtractFloatValueRangeAnim(objStart, objEnd, "speed", &speed))
                        state.speed = speed;

                    if (hasClip)
                    {
                        char resolved[512] = "";
                        ResolveGraphPath(path, clipPath, resolved, sizeof(resolved));
                        JsonAnimClip* clip = LoadJsonAnimClip(resolved, m_gameModel->skeleton, m_jsonDecodeMode, m_type2PackingMode);
                        if (clip && clip->tracks.empty())
                        {
                            delete clip;
                            clip = NULL;
                        }

                        if (clip)
                        {
                            const int skeletonBones = m_gameModel->skeleton->m_numBones;
                            const int coveredBones = CountCoveredSkeletonBones(clip, skeletonBones);
                            int minCovered = (int)(skeletonBones * 0.25f);
                            if (minCovered < 4) minCovered = 4;

                            if (m_strictRigCoverage && coveredBones < minCovered)
                            {
                                char msg[256];
                                sprintf_s(msg,
                                          "Rejected graph state '%s' (coveredBones=%d < min=%d): %s",
                                          name, coveredBones, minCovered, resolved);
                                RendererLog(msg);
                                delete clip;
                                clip = NULL;
                            }
                        }

                        if (clip)
                        {
                            state.clip = clip;
                            state.duration = GetClipDuration(clip);
                            state.frameTime = GetClipFrameTime(clip);
                        }
                    }

                    if (state.clip || hasBlendGraph || hasStateMachine)
                    {
                        graph->states.push_back(state);
                    }
                }

                p = objEnd + 1;
                continue;
            }
            p++;
        }
    }

    // Default state
    char defStateName[128] = "";
    if (ExtractStringValueRangeAnim(json, json + jsonSize, "defaultState", defStateName, sizeof(defStateName)))
    {
        graph->defaultState = FindGraphStateIndex(*graph, defStateName);
    }
    if (graph->defaultState < 0 && !graph->states.empty())
    {
        graph->defaultState = 0;
    }

    // Parse transitions
    const char* transKey = FindKeyInRangeAnim(json, json + jsonSize, "transitions");
    const char* transArray = transKey ? strchr(transKey, '[') : NULL;
    const char* transEnd = transArray ? FindMatchingAnim(transArray, '[', ']') : NULL;
    if (transArray && transEnd)
    {
        const char* p = transArray + 1;
        while (p && p < transEnd)
        {
            p = SkipWhitespaceAnim(p);
            if (!p || p >= transEnd) break;
            if (*p == '{')
            {
                const char* objStart = p;
                const char* objEnd = FindMatchingAnim(objStart, '{', '}');
                if (!objEnd || objEnd > transEnd) break;

                GraphTransition tr;
                char fromName[128] = "";
                char toName[128] = "";
                if (!ExtractStringValueRangeAnim(objStart, objEnd, "from", fromName, sizeof(fromName)))
                {
                    ExtractStringValueRangeAnim(objStart, objEnd, "source", fromName, sizeof(fromName));
                }
                if (!ExtractStringValueRangeAnim(objStart, objEnd, "to", toName, sizeof(toName)))
                {
                    ExtractStringValueRangeAnim(objStart, objEnd, "target", toName, sizeof(toName));
                }

                if (toName[0] == '\0')
                {
                    p = objEnd + 1;
                    continue;
                }

                if (fromName[0] == '\0' || _stricmp(fromName, "*") == 0 || _stricmp(fromName, "any") == 0 || _stricmp(fromName, "anyState") == 0)
                {
                    tr.anyState = true;
                }
                else
                {
                    tr.fromState = FindGraphStateIndex(*graph, fromName);
                    if (tr.fromState < 0)
                    {
                        p = objEnd + 1;
                        continue;
                    }
                }

                tr.toState = FindGraphStateIndex(*graph, toName);
                if (tr.toState < 0)
                {
                    p = objEnd + 1;
                    continue;
                }

                float duration = 0.0f;
                if (ExtractFloatValueRangeAnim(objStart, objEnd, "duration", &duration) ||
                    ExtractFloatValueRangeAnim(objStart, objEnd, "blendTime", &duration))
                {
                    tr.duration = duration;
                }

                float exitTime = -1.0f;
                if (ExtractFloatValueRangeAnim(objStart, objEnd, "exitTime", &exitTime))
                {
                    if (exitTime < 0.0f) exitTime = 0.0f;
                    if (exitTime > 1.0f) exitTime = 1.0f;
                    tr.exitTime = exitTime;
                }

                int prio = 0;
                if (ExtractIntValueRangeAnim(objStart, objEnd, "priority", &prio))
                {
                    tr.priority = prio;
                }

                char easingName[64] = "";
                if (ExtractStringValueRangeAnim(objStart, objEnd, "easing", easingName, sizeof(easingName)) ||
                    ExtractStringValueRangeAnim(objStart, objEnd, "easingType", easingName, sizeof(easingName)))
                {
                    tr.easingType = FindEasingTypeByName(easingName);
                }
                else
                {
                    int easingType = 0;
                    if (ExtractIntValueRangeAnim(objStart, objEnd, "easingType", &easingType))
                        tr.easingType = easingType;
                }

                ExtractFloatValueRangeAnim(objStart, objEnd, "cp1x", &tr.cp1x);
                ExtractFloatValueRangeAnim(objStart, objEnd, "cp1y", &tr.cp1y);
                ExtractFloatValueRangeAnim(objStart, objEnd, "cp2x", &tr.cp2x);
                ExtractFloatValueRangeAnim(objStart, objEnd, "cp2y", &tr.cp2y);

                char blendStr[64] = "";
                if (ExtractStringValueRangeAnim(objStart, objEnd, "blendMode", blendStr, sizeof(blendStr)))
                {
                    tr.blendMode = ParseGraphBlendMode(blendStr);
                }
                else
                {
                    int blendInt = 0;
                    if (ExtractIntValueRangeAnim(objStart, objEnd, "blendMode", &blendInt))
                        tr.blendMode = blendInt;
                }

                char rotStr[64] = "";
                if (ExtractStringValueRangeAnim(objStart, objEnd, "rotMode", rotStr, sizeof(rotStr)) ||
                    ExtractStringValueRangeAnim(objStart, objEnd, "rotationMode", rotStr, sizeof(rotStr)))
                {
                    tr.rotMode = ParseGraphRotMode(rotStr);
                }
                else
                {
                    int rotInt = 0;
                    if (ExtractIntValueRangeAnim(objStart, objEnd, "rotMode", &rotInt))
                        tr.rotMode = rotInt;
                }

                bool allowInterrupt = true;
                if (ExtractBoolValueRangeAnim(objStart, objEnd, "allowInterrupt", &allowInterrupt) ||
                    ExtractBoolValueRangeAnim(objStart, objEnd, "interrupt", &allowInterrupt))
                {
                    tr.allowInterrupt = allowInterrupt;
                }

                bool syncTime = false;
                if (ExtractBoolValueRangeAnim(objStart, objEnd, "sync", &syncTime))
                {
                    tr.syncTime = syncTime;
                }

                char layerRootName[128] = "";
                if (ExtractStringValueRangeAnim(objStart, objEnd, "layerRoot", layerRootName, sizeof(layerRootName)) ||
                    ExtractStringValueRangeAnim(objStart, objEnd, "maskRoot", layerRootName, sizeof(layerRootName)))
                {
                    tr.layerRoot = FindSkeletonBoneIndex(m_gameModel->skeleton, layerRootName);
                }
                else
                {
                    int layerRootIdx = -1;
                    if (ExtractIntValueRangeAnim(objStart, objEnd, "layerRoot", &layerRootIdx))
                        tr.layerRoot = layerRootIdx;
                }

                if ((tr.blendMode == Scene3DRenderer::JSON_BLEND_LAYERED ||
                     tr.blendMode == Scene3DRenderer::JSON_BLEND_ADDITIVE) &&
                    tr.layerRoot >= 0)
                {
                    BuildSubtreeMask(m_gameModel->skeleton, tr.layerRoot, tr.mask);
                }

                // Parse conditions array
                const char* condKey = FindKeyInRangeAnim(objStart, objEnd, "conditions");
                const char* condArray = condKey ? strchr(condKey, '[') : NULL;
                const char* condEnd = condArray ? FindMatchingAnim(condArray, '[', ']') : NULL;
                if (condArray && condEnd)
                {
                    const char* c = condArray + 1;
                    while (c && c < condEnd)
                    {
                        c = SkipWhitespaceAnim(c);
                        if (!c || c >= condEnd) break;
                        if (*c == '{')
                        {
                            const char* cStart = c;
                            const char* cEnd = FindMatchingAnim(cStart, '{', '}');
                            if (!cEnd || cEnd > condEnd) break;

                            char paramName[128] = "";
                            if (ExtractStringValueRangeAnim(cStart, cEnd, "param", paramName, sizeof(paramName)) ||
                                ExtractStringValueRangeAnim(cStart, cEnd, "parameter", paramName, sizeof(paramName)))
                            {
                                int pIndex = FindGraphParamIndex(*graph, paramName);
                                if (pIndex >= 0)
                                {
                                    GraphCondition cond;
                                    cond.paramIndex = pIndex;

                                    char opStr[16] = "";
                                    if (ExtractStringValueRangeAnim(cStart, cEnd, "op", opStr, sizeof(opStr)))
                                    {
                                        cond.op = ParseConditionOp(opStr);
                                    }

                                    const GraphParam& param = graph->params[pIndex];
                                    if (param.type == Scene3DRenderer::GRAPH_PARAM_FLOAT)
                                    {
                                        float v = 0.0f;
                                        if (ExtractFloatValueRangeAnim(cStart, cEnd, "value", &v))
                                            cond.f = v;
                                    }
                                    else if (param.type == Scene3DRenderer::GRAPH_PARAM_INT)
                                    {
                                        int v = 0;
                                        if (ExtractIntValueRangeAnim(cStart, cEnd, "value", &v))
                                            cond.i = v;
                                    }
                                    else
                                    {
                                        bool v = false;
                                        if (ExtractBoolValueRangeAnim(cStart, cEnd, "value", &v))
                                            cond.b = v;
                                    }

                                    tr.conditions.push_back(cond);
                                }
                            }

                            c = cEnd + 1;
                            continue;
                        }
                        c++;
                    }
                }

                graph->transitions.push_back(tr);

                p = objEnd + 1;
                continue;
            }
            p++;
        }
    }

    ResolveGraphStateLinks(*graph);

    if (graph->states.empty())
    {
        RendererLog("loadAnimationGraph: no valid states (graph rejected)");
        delete graph;
        free(json);
        return false;
    }

    if (graph->defaultState < 0)
    {
        graph->defaultState = 0;
    }
    graph->currentState = graph->defaultState;
    graph->stateTime = 0.0f;
    graph->nextState = -1;
    graph->activeTransition = -1;
    graph->inTransition = false;
    graph->transitionTime = 0.0f;
    graph->transitionDuration = 0.0f;
    graph->lastEventTime = 0.0f;
    graph->lastNextEventTime = 0.0f;
    graph->valid = true;
    graph->enabled = true;

    m_animGraph = graph;
    strcpy_s(m_animGraphPath, path);

    free(json);
    return true;
}

bool Scene3DRenderer::loadAnimationGraphLua(const char* path)
{
    if (!path || !path[0])
    {
        RendererLog("loadAnimationGraphLua: empty path");
        return false;
    }
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        RendererLog("loadAnimationGraphLua: no model/skeleton loaded");
        return false;
    }

    clearAnimationGraph();
    resetRootMotionState();

    lua_State* L = luaL_newstate();
    if (!L)
    {
        RendererLog("loadAnimationGraphLua: failed to create Lua state");
        return false;
    }
    luaL_openlibs(L);

    LuaScriptContext ctx;
    ctx.baseDir = GetDirectoryFromPathAnim(path);
    ctx.gameFilesRoot = FindGameFilesRootFromPathAnim(path);
    ctx.renderer = this;

    LuaRegisterBindings(L, &ctx);

    if (!LuaLoadModule(L, &ctx, "MgBase"))
    {
        RendererLog("loadAnimationGraphLua: failed to load MgBase");
        lua_close(L);
        return false;
    }

    if (!LuaRunFile(L, path, this))
    {
        RendererLog("loadAnimationGraphLua: failed to run script");
        lua_close(L);
        return false;
    }

    std::map<std::string, std::string> animMap;
    std::string graphName;
    if (!LuaExtractAnimTable(L, animMap, graphName, this))
    {
        RendererLog("loadAnimationGraphLua: AnimTable missing or empty");
        lua_close(L);
        return false;
    }

    AnimationGraphRuntime* graph = new AnimationGraphRuntime();
    graph->owner = graph;

    bool built = BuildGraphFromLuaAnimationGraph(L, *graph, animMap, ctx,
                                                 m_gameModel->skeleton, m_jsonDecodeMode,
                                                 m_type2PackingMode, m_strictRigCoverage);

    if (!built)
    {
        std::vector<std::string> keys;
        keys.reserve(animMap.size());
        for (std::map<std::string, std::string>::const_iterator it = animMap.begin(); it != animMap.end(); ++it)
        {
            keys.push_back(it->first);
        }
        std::sort(keys.begin(), keys.end());

        for (size_t i = 0; i < keys.size(); ++i)
        {
            const std::string& stateName = keys[i];
            const std::string& clipName = animMap[stateName];

            std::string clipPath;
            if (!ResolveLuaAnimClipPath(ctx, clipName, clipPath))
            {
                char msg[512];
                sprintf_s(msg, "loadAnimationGraphLua: missing clip %s", clipName.c_str());
                RendererLog(msg);
                continue;
            }

            JsonAnimClip* clip = LoadLuaAnimClipChecked(clipPath,
                                                        m_gameModel->skeleton,
                                                        m_jsonDecodeMode,
                                                        m_type2PackingMode,
                                                        m_strictRigCoverage);
            if (!clip)
            {
                continue;
            }

            GraphState state;
            state.name = stateName;
            state.clipPath = clipPath;
            state.clip = clip;
            state.loop = true;
            state.speed = 1.0f;
            state.duration = GetClipDuration(clip);
            state.frameTime = GetClipFrameTime(clip);
            graph->states.push_back(state);
        }
    }

    lua_close(L);

    if (graph->states.empty())
    {
        RendererLog("loadAnimationGraphLua: no valid states (graph rejected)");
        delete graph;
        return false;
    }

    if (graph->name.empty())
    {
        if (!graphName.empty())
        {
            graph->name = graphName;
        }
        else
        {
            char baseName[256] = "";
            const char* slash = strrchr(path, '/');
            const char* back = strrchr(path, '\\');
            const char* fileName = path;
            if (slash && back) fileName = (slash > back) ? (slash + 1) : (back + 1);
            else if (slash) fileName = slash + 1;
            else if (back) fileName = back + 1;
            strncpy_s(baseName, sizeof(baseName), fileName, _TRUNCATE);
            char* dot = strrchr(baseName, '.');
            if (dot) *dot = '\0';
            graph->name = baseName;
        }
    }

    if (graph->defaultState < 0)
    {
        for (size_t i = 0; i < graph->states.size(); ++i)
        {
            if (graph->states[i].name == "StandIdle")
            {
                graph->defaultState = (int)i;
                break;
            }
        }
    }
    if (graph->defaultState < 0)
    {
        for (size_t i = 0; i < graph->states.size(); ++i)
        {
            if (graph->states[i].name == "Idle")
            {
                graph->defaultState = (int)i;
                break;
            }
        }
    }
    if (graph->defaultState < 0)
    {
        graph->defaultState = 0;
    }

    ResolveBlendGraphLinks(*graph);
    ResolveGraphStateLinks(*graph);

    graph->currentState = graph->defaultState;
    graph->stateTime = 0.0f;
    graph->nextState = -1;
    graph->activeTransition = -1;
    graph->inTransition = false;
    graph->transitionTime = 0.0f;
    graph->transitionDuration = 0.0f;
    graph->lastEventTime = 0.0f;
    graph->lastNextEventTime = 0.0f;
    graph->valid = true;
    graph->enabled = true;

    m_animGraph = graph;
    strcpy_s(m_animGraphPath, path);
    return true;
}

static void DestroyBlendGraphRuntime(BlendGraphRuntime* bg)
{
    if (!bg) return;
    for (int i = 0; i < (int)bg->nodes.size(); ++i)
    {
        BlendGraphNode& node = bg->nodes[i];
        if (node.clip)
        {
            delete node.clip;
            node.clip = NULL;
        }
        node.stateMachine = NULL;
        node.subgraph = NULL;
    }
    delete bg;
}

static void DestroyGraphMachine(AnimationGraphRuntime* graph)
{
    if (!graph) return;
    for (size_t i = 0; i < graph->states.size(); ++i)
    {
        GraphState& st = graph->states[i];
        if (st.clip)
        {
            delete st.clip;
            st.clip = NULL;
        }
        st.additiveRefLocal.clear();
        st.additiveRefValid = false;
        st.blendGraph = NULL;
        st.subMachine = NULL;
    }

    for (size_t i = 0; i < graph->blendGraphs.size(); ++i)
    {
        DestroyBlendGraphRuntime(graph->blendGraphs[i]);
    }
    graph->blendGraphs.clear();

    for (size_t i = 0; i < graph->subMachines.size(); ++i)
    {
        DestroyGraphMachine(graph->subMachines[i]);
    }
    graph->subMachines.clear();

    delete graph;
}

void Scene3DRenderer::clearAnimationGraph()
{
    if (!m_animGraph)
    {
        m_animGraphPath[0] = '\0';
        return;
    }

    DestroyGraphMachine(m_animGraph);
    m_animGraph = NULL;
    m_animGraphPath[0] = '\0';
    clearMotionMatchDatabase();
}

const char* Scene3DRenderer::getAnimationGraphPath() const
{
    return m_animGraphPath;
}

const char* Scene3DRenderer::getAnimationGraphName() const
{
    if (m_animGraph && !m_animGraph->name.empty())
        return m_animGraph->name.c_str();
    return NULL;
}

bool Scene3DRenderer::getAnimationGraphEnabled() const
{
    return (m_animGraph && m_animGraph->enabled);
}

void Scene3DRenderer::setAnimationGraphEnabled(bool enabled)
{
    if (!m_animGraph) return;
    if (m_animGraph->enabled == enabled) return;
    m_animGraph->enabled = enabled;
    if (enabled)
    {
        resetAnimationGraph();
    }
}

int Scene3DRenderer::getAnimationGraphStateCount() const
{
    if (!m_animGraph) return 0;
    return (int)m_animGraph->states.size();
}

const char* Scene3DRenderer::getAnimationGraphStateName(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return NULL;
    return m_animGraph->states[index].name.c_str();
}

int Scene3DRenderer::getAnimationGraphCurrentState() const
{
    if (!m_animGraph) return -1;
    return m_animGraph->currentState;
}

int Scene3DRenderer::getAnimationGraphNextState() const
{
    if (!m_animGraph) return -1;
    return m_animGraph->nextState;
}

bool Scene3DRenderer::isAnimationGraphInTransition() const
{
    if (!m_animGraph) return false;
    return m_animGraph->inTransition;
}

float Scene3DRenderer::getAnimationGraphStateTime() const
{
    if (!m_animGraph) return 0.0f;
    return m_animGraph->stateTime;
}

float Scene3DRenderer::getAnimationGraphStateDuration() const
{
    if (!m_animGraph) return 0.0f;
    if (m_animGraph->currentState < 0 || m_animGraph->currentState >= (int)m_animGraph->states.size())
        return 0.0f;
    return m_animGraph->states[m_animGraph->currentState].duration;
}

float Scene3DRenderer::getAnimationGraphTransitionTime() const
{
    if (!m_animGraph) return 0.0f;
    return m_animGraph->transitionTime;
}

float Scene3DRenderer::getAnimationGraphTransitionDuration() const
{
    if (!m_animGraph) return 0.0f;
    return m_animGraph->transitionDuration;
}

bool Scene3DRenderer::setAnimationGraphCurrentState(int index)
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return false;
    int prevState = m_animGraph->currentState;
    if (prevState != index)
    {
        HandleGraphStateExit(*this, *m_animGraph, prevState);
        HandleGraphStateEnter(*this, *m_animGraph, index);
    }
    m_animGraph->currentState = index;
    m_animGraph->stateTime = 0.0f;
    m_animGraph->nextState = -1;
    m_animGraph->activeTransition = -1;
    m_animGraph->inTransition = false;
    m_animGraph->transitionTime = 0.0f;
    m_animGraph->transitionDuration = 0.0f;
    m_animGraph->lastEventTime = 0.0f;
    m_animGraph->lastNextEventTime = 0.0f;
    resetEventDedup();
    return true;
}

void Scene3DRenderer::resetAnimationGraph()
{
    if (!m_animGraph) return;
    int prevState = m_animGraph->currentState;
    int def = m_animGraph->defaultState;
    if (def < 0 && !m_animGraph->states.empty())
        def = 0;
    if (def >= 0 && def < (int)m_animGraph->states.size())
    {
        if (prevState != def)
        {
            HandleGraphStateExit(*this, *m_animGraph, prevState);
            HandleGraphStateEnter(*this, *m_animGraph, def);
        }
        m_animGraph->currentState = def;
    }
    m_animGraph->stateTime = 0.0f;
    m_animGraph->nextState = -1;
    m_animGraph->activeTransition = -1;
    m_animGraph->inTransition = false;
    m_animGraph->transitionTime = 0.0f;
    m_animGraph->transitionDuration = 0.0f;
    m_animGraph->lastEventTime = 0.0f;
    m_animGraph->lastNextEventTime = 0.0f;
    resetEventDedup();
}

int Scene3DRenderer::getAnimationGraphParamCount() const
{
    if (!m_animGraph) return 0;
    return (int)GetGraphParamsConst(*m_animGraph).size();
}

const char* Scene3DRenderer::getAnimationGraphParamName(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return NULL;
    return GetGraphParamsConst(*m_animGraph)[index].name.c_str();
}

int Scene3DRenderer::getAnimationGraphParamType(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return GRAPH_PARAM_FLOAT;
    return GetGraphParamsConst(*m_animGraph)[index].type;
}

float Scene3DRenderer::getAnimationGraphParamFloat(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return 0.0f;
    return GetGraphParamsConst(*m_animGraph)[index].f;
}

int Scene3DRenderer::getAnimationGraphParamInt(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return 0;
    return GetGraphParamsConst(*m_animGraph)[index].i;
}

bool Scene3DRenderer::getAnimationGraphParamBool(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return false;
    const GraphParam& p = GetGraphParamsConst(*m_animGraph)[index];
    if (p.type == GRAPH_PARAM_TRIGGER) return p.triggered;
    return p.b;
}

float Scene3DRenderer::getAnimationGraphParamMin(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return 0.0f;
    return GetGraphParamsConst(*m_animGraph)[index].minValue;
}

float Scene3DRenderer::getAnimationGraphParamMax(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return 0.0f;
    return GetGraphParamsConst(*m_animGraph)[index].maxValue;
}

bool Scene3DRenderer::getAnimationGraphParamHasRange(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return false;
    return GetGraphParamsConst(*m_animGraph)[index].hasRange;
}

bool Scene3DRenderer::setAnimationGraphParamFloat(int index, float value)
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return false;
    GraphParam& p = GetGraphParams(*m_animGraph)[index];
    p.f = value;
    p.i = (int)value;
    p.b = (value != 0.0f);
    return true;
}

bool Scene3DRenderer::setAnimationGraphParamInt(int index, int value)
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return false;
    GraphParam& p = GetGraphParams(*m_animGraph)[index];
    p.i = value;
    p.f = (float)value;
    p.b = (value != 0);
    return true;
}

bool Scene3DRenderer::setAnimationGraphParamBool(int index, bool value)
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return false;
    GraphParam& p = GetGraphParams(*m_animGraph)[index];
    if (p.type == GRAPH_PARAM_TRIGGER)
    {
        p.triggered = value;
    }
    else
    {
        p.b = value;
        p.f = value ? 1.0f : 0.0f;
        p.i = value ? 1 : 0;
    }
    return true;
}

bool Scene3DRenderer::fireAnimationGraphTrigger(int index)
{
    if (!m_animGraph || index < 0 || index >= (int)GetGraphParamsConst(*m_animGraph).size())
        return false;
    GraphParam& p = GetGraphParams(*m_animGraph)[index];
    if (p.type != GRAPH_PARAM_TRIGGER)
        return false;
    p.triggered = true;
    return true;
}

// ---------------------------------------------------------------------------
// Per-state data accessors (for state machine visualization)
// ---------------------------------------------------------------------------

const char* Scene3DRenderer::getAnimationGraphStateClip(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return NULL;
    const std::string& cp = m_animGraph->states[index].clipPath;
    return cp.empty() ? NULL : cp.c_str();
}

bool Scene3DRenderer::getAnimationGraphStateLoop(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return false;
    return m_animGraph->states[index].loop;
}

float Scene3DRenderer::getAnimationGraphStateSpeed(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return 1.0f;
    return m_animGraph->states[index].speed;
}

float Scene3DRenderer::getAnimationGraphStateStateDuration(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return 0.0f;
    return m_animGraph->states[index].duration;
}

int Scene3DRenderer::getAnimationGraphStateTransitionCount(int stateIndex) const
{
    if (!m_animGraph) return 0;
    int count = 0;
    for (size_t i = 0; i < m_animGraph->transitions.size(); ++i)
    {
        const GraphTransition& t = m_animGraph->transitions[i];
        if (t.fromState == stateIndex || t.anyState)
            ++count;
    }
    return count;
}

int Scene3DRenderer::getAnimationGraphStateTransitionTarget(int stateIndex, int transIndex) const
{
    if (!m_animGraph) return -1;
    int count = 0;
    for (size_t i = 0; i < m_animGraph->transitions.size(); ++i)
    {
        const GraphTransition& t = m_animGraph->transitions[i];
        if (t.fromState == stateIndex || t.anyState)
        {
            if (count == transIndex)
                return t.toState;
            ++count;
        }
    }
    return -1;
}

int Scene3DRenderer::getAnimationGraphStateOnEnterActionCount(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return 0;
    return (int)m_animGraph->states[index].onEnterActions.size();
}

int Scene3DRenderer::getAnimationGraphStateOnExitActionCount(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return 0;
    return (int)m_animGraph->states[index].onExitActions.size();
}

const char* Scene3DRenderer::getAnimationGraphStateOnEnterActionName(int stateIndex, int actionIndex) const
{
    if (!m_animGraph || stateIndex < 0 || stateIndex >= (int)m_animGraph->states.size())
        return NULL;
    const GraphState& s = m_animGraph->states[stateIndex];
    if (actionIndex < 0 || actionIndex >= (int)s.onEnterActions.size())
        return NULL;
    return s.onEnterActions[actionIndex].name.c_str();
}

const char* Scene3DRenderer::getAnimationGraphStateOnExitActionName(int stateIndex, int actionIndex) const
{
    if (!m_animGraph || stateIndex < 0 || stateIndex >= (int)m_animGraph->states.size())
        return NULL;
    const GraphState& s = m_animGraph->states[stateIndex];
    if (actionIndex < 0 || actionIndex >= (int)s.onExitActions.size())
        return NULL;
    return s.onExitActions[actionIndex].name.c_str();
}

bool Scene3DRenderer::getAnimationGraphStateHasSubMachine(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return false;
    return m_animGraph->states[index].subMachine != NULL;
}

bool Scene3DRenderer::getAnimationGraphStateHasBlendGraph(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return false;
    return m_animGraph->states[index].blendGraph != NULL;
}

const char* Scene3DRenderer::getAnimationGraphStateSubMachineName(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return NULL;
    const std::string& n = m_animGraph->states[index].stateMachineName;
    return n.empty() ? NULL : n.c_str();
}

const char* Scene3DRenderer::getAnimationGraphStateBlendGraphName(int index) const
{
    if (!m_animGraph || index < 0 || index >= (int)m_animGraph->states.size())
        return NULL;
    const std::string& n = m_animGraph->states[index].blendGraphName;
    return n.empty() ? NULL : n.c_str();
}

int Scene3DRenderer::getAnimationGraphActiveTransitionIndex() const
{
    if (!m_animGraph) return -1;
    return m_animGraph->activeTransition;
}

int Scene3DRenderer::getAnimationGraphTransitionFromState(int transIndex) const
{
    if (!m_animGraph || transIndex < 0 || transIndex >= (int)m_animGraph->transitions.size())
        return -1;
    return m_animGraph->transitions[transIndex].fromState;
}

int Scene3DRenderer::getAnimationGraphTransitionToState(int transIndex) const
{
    if (!m_animGraph || transIndex < 0 || transIndex >= (int)m_animGraph->transitions.size())
        return -1;
    return m_animGraph->transitions[transIndex].toState;
}

int Scene3DRenderer::getAnimationGraphTransitionConditionCount(int transIndex) const
{
    if (!m_animGraph || transIndex < 0 || transIndex >= (int)m_animGraph->transitions.size())
        return 0;
    return (int)m_animGraph->transitions[transIndex].conditions.size();
}

int Scene3DRenderer::getAnimationGraphTransitionCount() const
{
    if (!m_animGraph) return 0;
    return (int)m_animGraph->transitions.size();
}

// ---------------------------------------------------------------------------
// JSON animation blending controls (pose A ↔ pose B)
// ---------------------------------------------------------------------------

bool Scene3DRenderer::setJsonBlendAnimationPath(const char* path)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }

    if (m_gameModel && m_gameModel->skeleton)
    {
        JsonAnimClip* next = LoadJsonAnimClip(path, m_gameModel->skeleton, m_jsonDecodeMode, m_type2PackingMode);
        if (next && next->tracks.empty())
        {
            delete next;
            next = NULL;
        }

        if (next && m_gameModel && m_gameModel->skeleton)
        {
            const int skeletonBones = m_gameModel->skeleton->m_numBones;
            const int coveredBones = CountCoveredSkeletonBones(next, skeletonBones);
            int minCovered = (int)(skeletonBones * 0.25f);
            if (minCovered < 4) minCovered = 4;

            if (m_strictRigCoverage && coveredBones < minCovered)
            {
                char msg[256];
                sprintf_s(msg,
                          "Rejected blend JSON anim: coveredBones=%d < min=%d (%s)",
                          coveredBones, minCovered, path);
                RendererLog(msg);
                delete next;
                next = NULL;
            }
        }

        if (!next)
        {
            RendererLog("setJsonBlendAnimationPath: failed/rejected; keeping previous blend clip.");
            return false;
        }

        if (m_jsonBlendAnim)
        {
            delete m_jsonBlendAnim;
            m_jsonBlendAnim = NULL;
        }
        m_jsonBlendAnim = next;
        m_useJsonBlendAnim = true;
        strcpy_s(m_jsonBlendAnimPath, path);

        // Blend caches depend on clip + skeleton.
        m_jsonBlendMask.clear();
        m_jsonBlendAdditiveRefLocal.clear();
        m_jsonBlendAdditiveRefValid = false;

        return true;
    }

    // Skeleton not ready yet; stash the path so the host can retry after model load.
    strcpy_s(m_jsonBlendAnimPath, path);
    return true;
}

void Scene3DRenderer::clearJsonBlendAnimation()
{
    if (m_jsonBlendAnim)
    {
        delete m_jsonBlendAnim;
        m_jsonBlendAnim = NULL;
    }
    m_useJsonBlendAnim = false;
    m_jsonBlendAnimPath[0] = '\0';
    m_jsonBlendMask.clear();
    m_jsonBlendAdditiveRefLocal.clear();
    m_jsonBlendAdditiveRefValid = false;
}

const char* Scene3DRenderer::getJsonBlendAnimationPath() const
{
    return m_jsonBlendAnimPath;
}

void Scene3DRenderer::setJsonBlendAlpha(float alpha)
{
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    m_jsonBlendAlpha = alpha;
}

void Scene3DRenderer::setJsonBlendMode(int mode)
{
    if (mode < JSON_BLEND_CROSSFADE) mode = JSON_BLEND_CROSSFADE;
    if (mode > JSON_BLEND_ADDITIVE) mode = JSON_BLEND_ADDITIVE;
    if (m_jsonBlendMode == mode) return;
    m_jsonBlendMode = mode;

    // Mode changes can require different caches.
    m_jsonBlendAdditiveRefValid = false;
}

void Scene3DRenderer::setJsonBlendRotMode(int mode)
{
    if (mode < JSON_BLEND_ROT_SLERP) mode = JSON_BLEND_ROT_SLERP;
    if (mode > JSON_BLEND_ROT_NLERP) mode = JSON_BLEND_ROT_NLERP;
    m_jsonBlendRotMode = mode;
}

bool Scene3DRenderer::loadBlendLayerClip(int layerIndex, const char* path)
{
    if (layerIndex < 0 || layerIndex >= MAX_BLEND_LAYERS || !path || path[0] == '\0')
        return false;
    if (!m_gameModel || !m_gameModel->skeleton)
        return false;

    BlendLayer& layer = m_blendLayers[layerIndex];
    if (layer.clip)
    {
        delete layer.clip;
        layer.clip = NULL;
    }
    layer.clip = LoadJsonAnimClip(path, m_gameModel->skeleton, m_jsonDecodeMode, m_type2PackingMode);
    if (layer.clip && layer.clip->tracks.empty())
    {
        delete layer.clip;
        layer.clip = NULL;
    }
    strncpy_s(layer.path, sizeof(layer.path), path, _TRUNCATE);
    layer.time = 0.0f;
    return (layer.clip != NULL);
}

void Scene3DRenderer::setJsonBlendLayerRootBone(int boneIndex)
{
    m_jsonBlendLayerRootBone = boneIndex;
    rebuildJsonBlendMask();
}

void Scene3DRenderer::setEditorTimelineDuration(float seconds)
{
    if (seconds <= 0.0f)
    {
        m_editorTimelineDuration = 0.0f;
        return;
    }
    if (seconds < 0.033f)
    {
        seconds = 0.033f;
    }
    m_editorTimelineDuration = seconds;
}

float Scene3DRenderer::getEditorTimelineDuration() const
{
    return m_editorTimelineDuration;
}

bool Scene3DRenderer::isJsonAnimationPaused() const
{
    return m_jsonAnimPaused;
}

void Scene3DRenderer::setJsonAnimationPaused(bool paused)
{
    m_jsonAnimPaused = paused;
}

void Scene3DRenderer::toggleJsonAnimationPaused()
{
    m_jsonAnimPaused = !m_jsonAnimPaused;
}

void Scene3DRenderer::setStrictRigCoverage(bool strict)
{
    m_strictRigCoverage = strict;
}

void Scene3DRenderer::seekJsonAnimation(float timeSeconds)
{
    float duration = getJsonAnimationDuration();
    if (duration > 0.0f)
    {
        if (timeSeconds < 0.0f) timeSeconds = 0.0f;
        if (timeSeconds > duration) timeSeconds = duration;
    }
    if (m_animGraph && m_animGraph->enabled)
    {
        m_animGraph->stateTime = timeSeconds;
        m_animGraph->lastEventTime = timeSeconds;
        if (m_animGraph->inTransition)
        {
            m_animGraph->nextTime = timeSeconds;
            m_animGraph->lastNextEventTime = timeSeconds;
        }
        resetEventDedup();
    }
    else
    {
        m_jsonAnimTime = timeSeconds;
        m_lastEventTime = timeSeconds;
        m_lastEventIndex = 0;
        resetEventDedup();
    }

    resetRootMotionState();
    if (m_rootMotionMode == ROOT_MOTION_EXTRACT && m_jsonAnim)
    {
        JsonVec3 startT, currT;
        float sampleTime = applyTimeWarpToClipTime(m_jsonAnim, timeSeconds);
        if (SampleRootTranslationAt(m_jsonAnim, 0.0f, startT) &&
            SampleRootTranslationAt(m_jsonAnim, sampleTime, currT))
        {
            m_rootMotionOffset.set(currT.x - startT.x,
                                   currT.y - startT.y,
                                   currT.z - startT.z);
        }
    }
}

Scene3DRenderer::RootMotionMode Scene3DRenderer::getRootMotionMode() const
{
    return m_rootMotionMode;
}

void Scene3DRenderer::setRootMotionMode(int mode)
{
    if (mode < ROOT_MOTION_FULL) mode = ROOT_MOTION_FULL;
    if (mode > ROOT_MOTION_EXTRACT) mode = ROOT_MOTION_EXTRACT;
    if (m_rootMotionMode == (RootMotionMode)mode)
        return;
    m_rootMotionMode = (RootMotionMode)mode;
    resetRootMotionState();
}

void Scene3DRenderer::resetRootMotionState()
{
    m_rootMotionOffset.setZero4();
    m_rootMotionPrevPos.setZero4();
    m_rootMotionPrevTime = 0.0f;
    m_rootMotionPrevValid = false;
    m_rootTrail.clear();
}

void Scene3DRenderer::setRootMotionLockAxis(int axis, bool locked)
{
    if (axis < 0 || axis > 2) return;
    m_rootMotionLock[axis] = locked;
}

void Scene3DRenderer::setRootMotionLock(bool lockX, bool lockY, bool lockZ)
{
    m_rootMotionLock[0] = lockX;
    m_rootMotionLock[1] = lockY;
    m_rootMotionLock[2] = lockZ;
}

void Scene3DRenderer::getRootMotionOffset(float& outX, float& outY, float& outZ) const
{
    if (m_rootMotionMode == ROOT_MOTION_EXTRACT)
    {
        outX = m_rootMotionOffset(0);
        outY = m_rootMotionOffset(1);
        outZ = m_rootMotionOffset(2);
    }
    else
    {
        outX = outY = outZ = 0.0f;
    }
}

bool Scene3DRenderer::getRootMotionWarpEnabled() const
{
    return m_rootMotionWarpEnabled;
}

void Scene3DRenderer::setRootMotionWarpEnabled(bool enabled)
{
    m_rootMotionWarpEnabled = enabled;
}

int Scene3DRenderer::getRootMotionWarpMode() const
{
    return m_rootMotionWarpMode;
}

void Scene3DRenderer::setRootMotionWarpMode(int mode)
{
    if (mode < ROOT_WARP_NONE) mode = ROOT_WARP_NONE;
    if (mode > ROOT_WARP_SCALE_REDIRECT) mode = ROOT_WARP_SCALE_REDIRECT;
    m_rootMotionWarpMode = mode;
}

void Scene3DRenderer::setRootMotionWarpTarget(float x, float y, float z)
{
    m_rootMotionWarpTarget.set(x, y, z);
}

void Scene3DRenderer::getRootMotionWarpTarget(float& outX, float& outY, float& outZ) const
{
    outX = m_rootMotionWarpTarget(0);
    outY = m_rootMotionWarpTarget(1);
    outZ = m_rootMotionWarpTarget(2);
}

Scene3DRenderer::GroundClampMode Scene3DRenderer::getGroundClampMode() const
{
    return m_groundClampMode;
}

int Scene3DRenderer::getJsonDecodeMode() const
{
    return m_jsonDecodeMode;
}

int Scene3DRenderer::getType2PackingMode() const
{
    return m_type2PackingMode;
}

int Scene3DRenderer::getRotAxisMode() const
{
    return m_rotAxisMode;
}

int Scene3DRenderer::getRotSignMask() const
{
    return m_rotSignMask;
}

Scene3DRenderer::RotApplyMode Scene3DRenderer::getRotApplyMode() const
{
    return m_rotApplyMode;
}

bool Scene3DRenderer::getApplyPoseCorrection() const
{
    return m_applyPoseCorrection;
}

bool Scene3DRenderer::getForceReferencePose() const
{
    return m_forceReferencePose;
}

bool Scene3DRenderer::getUseJsonAnim() const
{
    return m_useJsonAnim;
}

bool Scene3DRenderer::getLogType2() const
{
    return m_logType2;
}

void Scene3DRenderer::setPlaybackSpeed(float speed)
{
    if (speed < 0.05f) speed = 0.05f;
    if (speed > 4.0f) speed = 4.0f;
    m_playbackSpeed = speed;
}

float Scene3DRenderer::getPlaybackSpeed() const
{
    return m_playbackSpeed;
}

void Scene3DRenderer::setRotInterpMode(int mode)
{
    if (mode < ROT_INTERP_SLERP) mode = ROT_INTERP_SLERP;
    if (mode > ROT_INTERP_NLERP) mode = ROT_INTERP_NLERP;
    m_rotInterpMode = mode;
}

const char* Scene3DRenderer::getIKChainName(int index) const
{
    if (index < 0 || index >= m_ikChains.getSize())
        return NULL;
    return m_ikChains[index].name.c_str();
}

bool Scene3DRenderer::getIKChainEnabled(int index) const
{
    if (index < 0 || index >= m_ikChains.getSize())
        return false;
    return m_ikChains[index].enabled;
}

void Scene3DRenderer::setIKChainEnabled(int index, bool enabled)
{
    if (index < 0 || index >= m_ikChains.getSize())
        return;
    m_ikChains[index].enabled = enabled;
}

bool Scene3DRenderer::getIKChainTarget(int index, float& outX, float& outY, float& outZ) const
{
    if (index < 0 || index >= m_ikChains.getSize())
        return false;
    const hkVector4& t = m_ikChains[index].targetPosition;
    outX = t(0); outY = t(1); outZ = t(2);
    return true;
}

void Scene3DRenderer::setIKChainTarget(int index, float x, float y, float z)
{
    if (index < 0 || index >= m_ikChains.getSize())
        return;
    m_ikChains[index].targetPosition.set(x, y, z);
    m_ikChains[index].targetBoneIndex = -1;
}

void Scene3DRenderer::setIKChainTargetBone(int index, int boneIndex)
{
    if (index < 0 || index >= m_ikChains.getSize())
        return;
    m_ikChains[index].targetBoneIndex = boneIndex;
}

void Scene3DRenderer::setIKChainUseGround(int index, bool enabled)
{
    if (index < 0 || index >= m_ikChains.getSize())
        return;
    m_ikChains[index].useGround = enabled;
}

void Scene3DRenderer::setIKChainGroundOffset(int index, float offset)
{
    if (index < 0 || index >= m_ikChains.getSize())
        return;
    m_ikChains[index].groundOffset = offset;
}

void Scene3DRenderer::setIKChainUseTwoBone(int index, bool enabled)
{
    if (index < 0 || index >= m_ikChains.getSize())
        return;
    m_ikChains[index].useTwoBone = enabled;
}

void Scene3DRenderer::setLookAtTarget(float x, float y, float z)
{
    m_lookAtTarget.set(x, y, z);
}

void Scene3DRenderer::setAimTarget(float x, float y, float z)
{
    m_aimTarget.set(x, y, z);
}

void Scene3DRenderer::getLookAtTarget(float& outX, float& outY, float& outZ) const
{
    outX = m_lookAtTarget(0);
    outY = m_lookAtTarget(1);
    outZ = m_lookAtTarget(2);
}

void Scene3DRenderer::getAimTarget(float& outX, float& outY, float& outZ) const
{
    outX = m_aimTarget(0);
    outY = m_aimTarget(1);
    outZ = m_aimTarget(2);
}

void Scene3DRenderer::setLookAtWeight(float w)
{
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    m_lookAtWeight = w;
}

void Scene3DRenderer::setAimWeight(float w)
{
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    m_aimWeight = w;
}

void Scene3DRenderer::setRagdollBlend(float w)
{
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    m_ragdollBlend = w;
}

void Scene3DRenderer::resetPhysicsState()
{
    m_physState.clear();
    m_pendingImpulse.valid = false;
}

void Scene3DRenderer::applyHitImpulse(int boneIndex, const hkVector4& linearImpulse, const hkVector4& angularImpulse)
{
    if (boneIndex < 0)
        return;
    if (boneIndex < m_physState.getSize() && m_physState[boneIndex].valid)
    {
        m_physState[boneIndex].vel.add4(linearImpulse);
        m_physState[boneIndex].angVel.add4(angularImpulse);
        return;
    }
    m_pendingImpulse.boneIndex = boneIndex;
    m_pendingImpulse.linear = linearImpulse;
    m_pendingImpulse.angular = angularImpulse;
    m_pendingImpulse.valid = true;
}

void Scene3DRenderer::rebuildDefaultIKChains()
{
    m_ikChains.clear();
    if (!m_gameModel || !m_gameModel->skeleton)
        return;

    const hkaSkeleton* skeleton = m_gameModel->skeleton;
    const int boneCount = skeleton->m_numBones;
    if (boneCount <= 0)
        return;

    hkaPose refPose(skeleton);
    refPose.setToReferencePose();
    refPose.syncModelSpace();
    hkArray<hkVector4> positions;
    positions.setSize(boneCount);
    for (int i = 0; i < boneCount; ++i)
    {
        positions[i] = refPose.getBoneModelSpace(i).getTranslation();
    }

    IKSolver solver;

    // LOTR:C bone names first, then generic fallbacks
    const char* leftHipNames[]   = { "Bone_LThigh", "Hip_L", "L_Hip", "LeftUpLeg", "Thigh_L", "UpperLeg_L", "Bip01 L Thigh" };
    const char* leftKneeNames[]  = { "Bone_LShin", "Knee_L", "L_Knee", "LeftLeg", "Calf_L", "LowerLeg_L", "Bip01 L Calf" };
    const char* leftAnkleNames[] = { "Bone_LFootBone1", "Ankle_L", "L_Ankle", "Foot_L", "LeftFoot", "Bip01 L Foot" };

    const char* rightHipNames[]   = { "Bone_RThigh", "Hip_R", "R_Hip", "RightUpLeg", "Thigh_R", "UpperLeg_R", "Bip01 R Thigh" };
    const char* rightKneeNames[]  = { "Bone_RShin", "Knee_R", "R_Knee", "RightLeg", "Calf_R", "LowerLeg_R", "Bip01 R Calf" };
    const char* rightAnkleNames[] = { "Bone_RFootBone1", "Ankle_R", "R_Ankle", "Foot_R", "RightFoot", "Bip01 R Foot" };

    const char* leftShoulderNames[] = { "Bone_LShoulder", "Shoulder_L", "L_Shoulder", "Clavicle_L", "LeftShoulder", "Bip01 L Clavicle" };
    const char* leftElbowNames[]    = { "Bone_LForearm", "Elbow_L", "L_Elbow", "Forearm_L", "LowerArm_L", "Bip01 L Forearm" };
    const char* leftWristNames[]    = { "Bone_LHand", "Wrist_L", "L_Wrist", "Hand_L", "LeftHand", "Bip01 L Hand" };

    const char* rightShoulderNames[] = { "Bone_RShoulder", "Shoulder_R", "R_Shoulder", "Clavicle_R", "RightShoulder", "Bip01 R Clavicle" };
    const char* rightElbowNames[]    = { "Bone_RForearm", "Elbow_R", "R_Elbow", "Forearm_R", "LowerArm_R", "Bip01 R Forearm" };
    const char* rightWristNames[]    = { "Bone_RHand", "Wrist_R", "R_Wrist", "Hand_R", "RightHand", "Bip01 R Hand" };

    const char* headNames[] = { "Bone_Head", "Head", "Head_JNT", "Bip01 Head", "Head1" };
    const char* neckNames[] = { "Bone_Neck", "Neck", "Neck1", "Bip01 Neck" };
    const char* spineNames[] = { "Bone_UpperBody", "Bone_Lumbar3", "Spine2", "Spine3", "Spine1", "Chest", "Bip01 Spine2", "Bip01 Spine1" };

    int lHip = FindFirstBoneByAliases(skeleton, leftHipNames, sizeof(leftHipNames) / sizeof(leftHipNames[0]));
    int lKnee = FindFirstBoneByAliases(skeleton, leftKneeNames, sizeof(leftKneeNames) / sizeof(leftKneeNames[0]));
    int lAnkle = FindFirstBoneByAliases(skeleton, leftAnkleNames, sizeof(leftAnkleNames) / sizeof(leftAnkleNames[0]));
    if (lHip >= 0 && lKnee >= 0 && lAnkle >= 0)
    {
        std::vector<int> bones;
        bones.push_back(lHip);
        bones.push_back(lKnee);
        bones.push_back(lAnkle);
        IKChain chain;
        if (solver.CreateChain("Left Leg", bones, positions, chain))
        {
            chain.enabled = m_footIkEnabled;
            chain.useGround = true;
            chain.useTwoBone = (bones.size() == 3);
            chain.targetBoneIndex = bones.back();
            chain.targetPosition = positions[bones.back()];
            m_ikChains.pushBack(chain);
        }
    }

    int rHip = FindFirstBoneByAliases(skeleton, rightHipNames, sizeof(rightHipNames) / sizeof(rightHipNames[0]));
    int rKnee = FindFirstBoneByAliases(skeleton, rightKneeNames, sizeof(rightKneeNames) / sizeof(rightKneeNames[0]));
    int rAnkle = FindFirstBoneByAliases(skeleton, rightAnkleNames, sizeof(rightAnkleNames) / sizeof(rightAnkleNames[0]));
    if (rHip >= 0 && rKnee >= 0 && rAnkle >= 0)
    {
        std::vector<int> bones;
        bones.push_back(rHip);
        bones.push_back(rKnee);
        bones.push_back(rAnkle);
        IKChain chain;
        if (solver.CreateChain("Right Leg", bones, positions, chain))
        {
            chain.enabled = m_footIkEnabled;
            chain.useGround = true;
            chain.useTwoBone = (bones.size() == 3);
            chain.targetBoneIndex = bones.back();
            chain.targetPosition = positions[bones.back()];
            m_ikChains.pushBack(chain);
        }
    }

    int lSh = FindFirstBoneByAliases(skeleton, leftShoulderNames, sizeof(leftShoulderNames) / sizeof(leftShoulderNames[0]));
    int lEl = FindFirstBoneByAliases(skeleton, leftElbowNames, sizeof(leftElbowNames) / sizeof(leftElbowNames[0]));
    int lWr = FindFirstBoneByAliases(skeleton, leftWristNames, sizeof(leftWristNames) / sizeof(leftWristNames[0]));
    if (lSh >= 0 && lEl >= 0 && lWr >= 0)
    {
        std::vector<int> bones;
        bones.push_back(lSh);
        bones.push_back(lEl);
        bones.push_back(lWr);
        IKChain chain;
        if (solver.CreateChain("Left Arm", bones, positions, chain))
        {
            chain.enabled = false;
            chain.useGround = false;
            chain.useTwoBone = (bones.size() == 3);
            chain.targetBoneIndex = bones.back();
            chain.targetPosition = positions[bones.back()];
            m_ikChains.pushBack(chain);
        }
    }

    int rSh = FindFirstBoneByAliases(skeleton, rightShoulderNames, sizeof(rightShoulderNames) / sizeof(rightShoulderNames[0]));
    int rEl = FindFirstBoneByAliases(skeleton, rightElbowNames, sizeof(rightElbowNames) / sizeof(rightElbowNames[0]));
    int rWr = FindFirstBoneByAliases(skeleton, rightWristNames, sizeof(rightWristNames) / sizeof(rightWristNames[0]));
    if (rSh >= 0 && rEl >= 0 && rWr >= 0)
    {
        std::vector<int> bones;
        bones.push_back(rSh);
        bones.push_back(rEl);
        bones.push_back(rWr);
        IKChain chain;
        if (solver.CreateChain("Right Arm", bones, positions, chain))
        {
            chain.enabled = false;
            chain.useGround = false;
            chain.useTwoBone = (bones.size() == 3);
            chain.targetBoneIndex = bones.back();
            chain.targetPosition = positions[bones.back()];
            m_ikChains.pushBack(chain);
        }
    }

    if (m_lookAtBoneIndex < 0)
    {
        m_lookAtBoneIndex = FindFirstBoneByAliases(skeleton, headNames, sizeof(headNames) / sizeof(headNames[0]));
        if (m_lookAtBoneIndex < 0)
            m_lookAtBoneIndex = FindFirstBoneByAliases(skeleton, neckNames, sizeof(neckNames) / sizeof(neckNames[0]));
        if (m_lookAtBoneIndex < 0)
            m_lookAtBoneIndex = FindFirstBoneByAliases(skeleton, spineNames, sizeof(spineNames) / sizeof(spineNames[0]));
    }

    if (m_aimBoneIndex < 0)
    {
        m_aimBoneIndex = FindFirstBoneByAliases(skeleton, spineNames, sizeof(spineNames) / sizeof(spineNames[0]));
        if (m_aimBoneIndex < 0)
            m_aimBoneIndex = FindFirstBoneByAliases(skeleton, rightWristNames, sizeof(rightWristNames) / sizeof(rightWristNames[0]));
    }

    if (m_lookAtBoneIndex >= 0)
    {
        hkVector4 pos = refPose.getBoneModelSpace(m_lookAtBoneIndex).getTranslation();
        hkRotation r; r.set(refPose.getBoneModelSpace(m_lookAtBoneIndex).getRotation());
        hkVector4 fwd = r.getColumn(2);
        fwd.normalize3();
        hkVector4 target;
        target.setMul4(1.0f, fwd);
        target.add4(pos);
        m_lookAtTarget = target;
    }

    if (m_aimBoneIndex >= 0)
    {
        hkVector4 pos = refPose.getBoneModelSpace(m_aimBoneIndex).getTranslation();
        hkRotation r; r.set(refPose.getBoneModelSpace(m_aimBoneIndex).getRotation());
        hkVector4 fwd = r.getColumn(2);
        fwd.normalize3();
        hkVector4 target;
        target.setMul4(1.0f, fwd);
        target.add4(pos);
        m_aimTarget = target;
    }
}

static void ApplyLookAtToBone(const hkaSkeleton* skeleton,
                              hkaPose& pose,
                              int boneIndex,
                              const hkVector4& target,
                              float weight)
{
    if (!skeleton || boneIndex < 0 || boneIndex >= skeleton->m_numBones)
        return;
    if (weight <= 0.0f)
        return;

    pose.syncModelSpace();
    const hkQsTransform& modelT = pose.getBoneModelSpace(boneIndex);
    hkVector4 pos = modelT.getTranslation();
    hkVector4 toTarget;
    toTarget.setSub4(target, pos);
    float len = toTarget.length3();
    if (len < 1e-6f)
        return;
    toTarget.normalize3();

    hkRotation r;
    r.set(modelT.getRotation());
    hkVector4 forward = r.getColumn(2);
    forward.normalize3();

    hkQuaternion delta = QuatFromTo(forward, toTarget);
    hkQuaternion desiredModel;
    desiredModel.setMul(delta, modelT.getRotation());
    desiredModel.normalize();

    hkQuaternion finalModel = QuatBlend(modelT.getRotation(), desiredModel, weight, false);

    hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();
    int parent = skeleton->m_parentIndices[boneIndex];
    if (parent >= 0)
    {
        hkQuaternion parentRot = pose.getBoneModelSpace(parent).getRotation();
        hkQuaternion parentInv;
        parentInv.setInverse(parentRot);
        hkQuaternion localRot;
        localRot.setMul(parentInv, finalModel);
        localRot.normalize();
        local[boneIndex].setRotation(localRot);
    }
    else
    {
        local[boneIndex].setRotation(finalModel);
    }
}

bool Scene3DRenderer::applyIKToPose(hkaPose& pose)
{
    if (!m_gameModel || !m_gameModel->skeleton)
        return false;

    const bool anyIK = (m_ikEnabled && !m_ikChains.isEmpty());
    if (!anyIK && !m_lookAtEnabled && !m_aimEnabled)
        return false;

    const hkaSkeleton* skeleton = m_gameModel->skeleton;
    const int boneCount = skeleton->m_numBones;
    hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();

    bool applied = false;

    if (anyIK)
    {
        IKSolver solver;
        const float groundY = m_groundHeight - (m_modelBaseOffsetY + m_groundOffsetY);

        for (int c = 0; c < m_ikChains.getSize(); ++c)
        {
            IKChain& chain = m_ikChains[c];
            if (!chain.enabled)
                continue;
            if (chain.useGround && !m_footIkEnabled)
                continue;
            if (chain.boneIndices.size() < 2)
                continue;

            pose.syncModelSpace();

            const int jointCount = (int)chain.boneIndices.size();
            hkArray<hkVector4> joints;
            joints.reserve(jointCount);
            bool valid = true;
            for (int j = 0; j < jointCount; ++j)
            {
                int b = chain.boneIndices[j];
                if (b < 0 || b >= boneCount)
                {
                    valid = false;
                    break;
                }
                joints.pushBack(pose.getBoneModelSpace(b).getTranslation());
            }
            if (!valid || joints.getSize() != jointCount)
                continue;

            hkVector4 target = chain.targetPosition;
            int effBone = chain.boneIndices[jointCount - 1];
            if (chain.targetBoneIndex >= 0 && chain.targetBoneIndex < boneCount)
            {
                target = pose.getBoneModelSpace(chain.targetBoneIndex).getTranslation();
            }
            else if (effBone >= 0 && effBone < boneCount && chain.useGround)
            {
                target = pose.getBoneModelSpace(effBone).getTranslation();
            }
            if (chain.useGround)
            {
                target(1) = groundY + chain.groundOffset;
            }
            chain.targetPosition = target;

            IKChain temp = chain;
            temp.targetPosition = target;
            hkArray<hkQuaternion> dummy;
            if (!solver.SolveChain(temp, joints, dummy))
                continue;

            for (int j = 0; j + 1 < jointCount; ++j)
            {
                int boneIdx = chain.boneIndices[j];
                int childIdx = chain.boneIndices[j + 1];
                if (boneIdx < 0 || boneIdx >= boneCount || childIdx < 0 || childIdx >= boneCount)
                    continue;

                hkVector4 curPos = pose.getBoneModelSpace(boneIdx).getTranslation();
                hkVector4 curChild = pose.getBoneModelSpace(childIdx).getTranslation();
                hkVector4 desiredPos = joints[j];
                hkVector4 desiredChild = joints[j + 1];

                hkVector4 fromDir;
                fromDir.setSub4(curChild, curPos);
                hkVector4 toDir;
                toDir.setSub4(desiredChild, desiredPos);
                if (fromDir.length3() < 1e-6f || toDir.length3() < 1e-6f)
                    continue;

                hkQuaternion delta = QuatFromTo(fromDir, toDir);
                hkQuaternion desiredModel;
                desiredModel.setMul(delta, pose.getBoneModelSpace(boneIdx).getRotation());
                desiredModel.normalize();

                int parent = skeleton->m_parentIndices[boneIdx];
                if (parent >= 0)
                {
                    hkQuaternion parentRot = pose.getBoneModelSpace(parent).getRotation();
                    hkQuaternion parentInv;
                    parentInv.setInverse(parentRot);
                    hkQuaternion localRot;
                    localRot.setMul(parentInv, desiredModel);
                    localRot.normalize();
                    local[boneIdx].setRotation(localRot);
                }
                else
                {
                    local[boneIdx].setRotation(desiredModel);
                }
            }

            applied = true;
        }
    }

    if (m_lookAtEnabled && m_lookAtBoneIndex >= 0)
    {
        ApplyLookAtToBone(skeleton, pose, m_lookAtBoneIndex, m_lookAtTarget, m_lookAtWeight);
        applied = true;
    }
    if (m_aimEnabled && m_aimBoneIndex >= 0)
    {
        ApplyLookAtToBone(skeleton, pose, m_aimBoneIndex, m_aimTarget, m_aimWeight);
        applied = true;
    }

    pose.syncModelSpace();
    return applied;
}

bool Scene3DRenderer::applyPhysicsToPose(hkaPose& pose)
{
    if (!m_gameModel || !m_gameModel->skeleton)
        return false;
    if (!m_physicalAnimEnabled && !m_ragdollEnabled)
        return false;

    const hkaSkeleton* skeleton = m_gameModel->skeleton;
    const int boneCount = skeleton->m_numBones;
    if (boneCount <= 0)
        return false;

    float dt = m_lastAnimDeltaTime;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (dt > 0.25f) dt = 0.25f;

    pose.syncModelSpace();
    hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();

    if (m_physState.getSize() != boneCount)
    {
        m_physState.clear();
        m_physState.setSize(boneCount);
    }

    for (int i = 0; i < boneCount; ++i)
    {
        PhysBoneState& st = m_physState[i];
        if (!st.valid)
        {
            const hkQsTransform& ms = pose.getBoneModelSpace(i);
            st.pos = ms.getTranslation();
            st.rot = ms.getRotation();
            st.vel.setZero4();
            st.angVel.setZero4();
            int parent = skeleton->m_parentIndices[i];
            if (parent >= 0)
            {
                hkVector4 dp;
                dp.setSub4(st.pos, pose.getBoneModelSpace(parent).getTranslation());
                st.lengthToParent = dp.length3();
            }
            else
            {
                st.lengthToParent = 0.0f;
            }
            st.valid = true;
        }
    }

    if (m_pendingImpulse.valid)
    {
        int idx = m_pendingImpulse.boneIndex;
        if (idx >= 0 && idx < boneCount)
        {
            m_physState[idx].vel.add4(m_pendingImpulse.linear);
            m_physState[idx].angVel.add4(m_pendingImpulse.angular);
        }
        m_pendingImpulse.valid = false;
    }

    const bool ragdoll = m_ragdollEnabled;
    const float posK = ragdoll ? 0.0f : m_physPosStiffness;
    const float posD = m_physPosDamping;
    const float rotK = ragdoll ? 0.0f : m_physRotStiffness;
    const float rotD = m_physRotDamping;
    const hkVector4 gravity = m_ragdollGravity;

    // Integrate physical state toward target pose
    for (int i = 0; i < boneCount; ++i)
    {
        PhysBoneState& st = m_physState[i];
        const hkQsTransform& target = pose.getBoneModelSpace(i);
        hkVector4 targetPos = target.getTranslation();
        hkQuaternion targetRot = target.getRotation();

        hkVector4 acc;
        if (posK > 0.0f)
        {
            hkVector4 error;
            error.setSub4(targetPos, st.pos);
            hkVector4 kTerm;
            kTerm.setMul4(posK, error);
            hkVector4 dTerm;
            dTerm.setMul4(-posD, st.vel);
            acc.setAdd4(kTerm, dTerm);
        }
        else
        {
            hkVector4 dTerm;
            dTerm.setMul4(-posD, st.vel);
            acc.setAdd4(gravity, dTerm);
        }

        hkVector4 dv;
        dv.setMul4(dt, acc);
        st.vel.add4(dv);
        hkVector4 dp;
        dp.setMul4(dt, st.vel);
        st.pos.add4(dp);

        // Angular PD toward target rotation
        if (rotK > 0.0f)
        {
            hkQuaternion inv;
            inv.setInverse(st.rot);
            hkQuaternion delta;
            delta.setMul(targetRot, inv);
            delta.normalize();
            hkVector4 axis;
            float angle = 0.0f;
            QuatToAxisAngle(delta, axis, angle);
            hkVector4 angError;
            angError.setMul4(angle, axis);
            hkVector4 kTerm;
            kTerm.setMul4(rotK, angError);
            hkVector4 dTerm;
            dTerm.setMul4(-rotD, st.angVel);
            hkVector4 angAcc;
            angAcc.setAdd4(kTerm, dTerm);
            hkVector4 dang;
            dang.setMul4(dt, angAcc);
            st.angVel.add4(dang);
        }
        else
        {
            hkVector4 dTerm;
            dTerm.setMul4(-rotD, st.angVel);
            hkVector4 dang;
            dang.setMul4(dt, dTerm);
            st.angVel.add4(dang);
        }

        float angSpeed = st.angVel.length3();
        if (angSpeed > 1e-6f)
        {
            hkVector4 axis;
            axis.setMul4(1.0f / angSpeed, st.angVel);
            hkQuaternion dq(axis, angSpeed * dt);
            hkQuaternion next;
            next.setMul(dq, st.rot);
            next.normalize();
            st.rot = next;
        }
    }

    // Enforce parent-child length constraints
    for (int i = 0; i < boneCount; ++i)
    {
        int parent = skeleton->m_parentIndices[i];
        if (parent < 0)
            continue;
        PhysBoneState& st = m_physState[i];
        PhysBoneState& p = m_physState[parent];
        float rest = st.lengthToParent;
        if (rest <= 1e-6f)
            continue;
        hkVector4 dir;
        dir.setSub4(st.pos, p.pos);
        float len = dir.length3();
        if (len > 1e-6f)
        {
            dir.setMul4(1.0f / len, dir);
            hkVector4 newPos;
            newPos.setMul4(rest, dir);
            newPos.add4(p.pos);
            st.pos = newPos;
        }
    }

    hkArray<hkQsTransform> origLocal;
    origLocal.setSize(boneCount);
    for (int i = 0; i < boneCount; ++i)
    {
        origLocal[i] = local[i];
    }

    // Write physical pose in local space
    for (int i = 0; i < boneCount; ++i)
    {
        PhysBoneState& st = m_physState[i];
        int parent = skeleton->m_parentIndices[i];
        hkQuaternion modelRot = st.rot;
        hkVector4 modelPos = st.pos;
        hkQuaternion localRot;
        hkVector4 localPos;
        if (parent < 0)
        {
            localRot = modelRot;
            localPos = modelPos;
        }
        else
        {
            hkQuaternion parentRot = m_physState[parent].rot;
            hkQuaternion parentInv;
            parentInv.setInverse(parentRot);
            localRot.setMul(parentInv, modelRot);
            localRot.normalize();
            hkVector4 diff;
            diff.setSub4(modelPos, m_physState[parent].pos);
            localPos = RotateVectorByQuat(parentInv, diff);
        }

        local[i].setRotation(localRot);
        local[i].setTranslation(localPos);
        local[i].setScale(origLocal[i].getScale());
    }

    if (ragdoll)
    {
        float w = m_ragdollBlend;
        if (w < 0.0f) w = 0.0f;
        if (w > 1.0f) w = 1.0f;
        if (w < 1.0f)
        {
            const bool useNlerp = (m_rotInterpMode == ROT_INTERP_NLERP);
            for (int i = 0; i < boneCount; ++i)
            {
                hkVector4 t0 = origLocal[i].getTranslation();
                hkVector4 t1 = local[i].getTranslation();
                hkVector4 t;
                t.set(t0(0) + (t1(0) - t0(0)) * w,
                      t0(1) + (t1(1) - t0(1)) * w,
                      t0(2) + (t1(2) - t0(2)) * w);

                hkQuaternion r0 = origLocal[i].getRotation();
                hkQuaternion r1 = local[i].getRotation();
                hkQuaternion r = QuatBlend(r0, r1, w, useNlerp);

                hkVector4 s0 = origLocal[i].getScale();
                hkVector4 s1 = local[i].getScale();
                hkVector4 s;
                s.set(s0(0) + (s1(0) - s0(0)) * w,
                      s0(1) + (s1(1) - s0(1)) * w,
                      s0(2) + (s1(2) - s0(2)) * w);

                local[i].setTranslation(t);
                local[i].setRotation(r);
                local[i].setScale(s);
            }
        }
    }

    pose.syncModelSpace();
    return true;
}

void Scene3DRenderer::setLoopAnimation(bool loop)
{
    m_loopAnimation = loop;
}

bool Scene3DRenderer::getLoopAnimation() const
{
    return m_loopAnimation;
}

void Scene3DRenderer::setAnimFixedStepEnabled(bool enabled)
{
    m_animFixedStepEnabled = enabled;
    if (enabled)
    {
        m_animFixedAccumulator = 0.0f;
        m_animFixedStepsLast = 0;
    }
}

bool Scene3DRenderer::getAnimFixedStepEnabled() const
{
    return m_animFixedStepEnabled;
}

void Scene3DRenderer::setAnimFixedStep(float stepSeconds)
{
    const float minStep = 1.0f / 240.0f;
    const float maxStep = 1.0f / 10.0f;
    if (stepSeconds < minStep) stepSeconds = minStep;
    if (stepSeconds > maxStep) stepSeconds = maxStep;
    m_animFixedStep = stepSeconds;
}

float Scene3DRenderer::getAnimFixedStep() const
{
    return m_animFixedStep;
}

void Scene3DRenderer::setAnimFixedMaxSteps(int maxSteps)
{
    if (maxSteps < 1) maxSteps = 1;
    if (maxSteps > 32) maxSteps = 32;
    m_animFixedMaxSteps = maxSteps;
}

int Scene3DRenderer::getAnimFixedMaxSteps() const
{
    return m_animFixedMaxSteps;
}

float Scene3DRenderer::getAnimFixedAccumulator() const
{
    return m_animFixedAccumulator;
}

int Scene3DRenderer::getAnimFixedStepsLast() const
{
    return m_animFixedStepsLast;
}

void Scene3DRenderer::resetAnimFixedAccumulator()
{
    m_animFixedAccumulator = 0.0f;
    m_animFixedStepsLast = 0;
}

void Scene3DRenderer::setTimeWarpEnabled(bool enabled)
{
    m_timeWarpEnabled = enabled;
}

bool Scene3DRenderer::getTimeWarpEnabled() const
{
    return m_timeWarpEnabled;
}

void Scene3DRenderer::setTimeWarpEasingType(int easingType)
{
    if (easingType < 0) easingType = 0;
    if (easingType >= EASING_COUNT) easingType = EASING_COUNT - 1;
    m_timeWarpEasingType = easingType;
}

int Scene3DRenderer::getTimeWarpEasingType() const
{
    return m_timeWarpEasingType;
}

void Scene3DRenderer::setTimeWarpControlPoints(float cp1x, float cp1y, float cp2x, float cp2y)
{
    if (cp1x < 0.0f) cp1x = 0.0f;
    if (cp1x > 1.0f) cp1x = 1.0f;
    if (cp2x < 0.0f) cp2x = 0.0f;
    if (cp2x > 1.0f) cp2x = 1.0f;
    if (cp1y < 0.0f) cp1y = 0.0f;
    if (cp1y > 1.0f) cp1y = 1.0f;
    if (cp2y < 0.0f) cp2y = 0.0f;
    if (cp2y > 1.0f) cp2y = 1.0f;
    if (cp1x > cp2x)
    {
        float tmp = cp1x;
        cp1x = cp2x;
        cp2x = tmp;
    }
    m_timeWarpCp1x = cp1x;
    m_timeWarpCp1y = cp1y;
    m_timeWarpCp2x = cp2x;
    m_timeWarpCp2y = cp2y;
}

void Scene3DRenderer::getTimeWarpControlPoints(float& cp1x, float& cp1y, float& cp2x, float& cp2y) const
{
    cp1x = m_timeWarpCp1x;
    cp1y = m_timeWarpCp1y;
    cp2x = m_timeWarpCp2x;
    cp2y = m_timeWarpCp2y;
}

void Scene3DRenderer::setCompressionAuto(bool enabled)
{
    m_compressAuto = enabled;
}

bool Scene3DRenderer::getCompressionAuto() const
{
    return m_compressAuto;
}

void Scene3DRenderer::setCompressionTolerances(float posTol, float rotTolDeg, float scaleTol, float rootTol)
{
    if (posTol < 0.0f) posTol = 0.0f;
    if (rotTolDeg < 0.0f) rotTolDeg = 0.0f;
    if (scaleTol < 0.0f) scaleTol = 0.0f;
    if (rootTol < 0.0f) rootTol = 0.0f;
    m_compressPosTol = posTol;
    m_compressRotTolDeg = rotTolDeg;
    m_compressScaleTol = scaleTol;
    m_compressRootTol = rootTol;
}

void Scene3DRenderer::getCompressionTolerances(float& posTol, float& rotTolDeg, float& scaleTol, float& rootTol) const
{
    posTol = m_compressPosTol;
    rotTolDeg = m_compressRotTolDeg;
    scaleTol = m_compressScaleTol;
    rootTol = m_compressRootTol;
}

void Scene3DRenderer::setCompressionQuantizeRotations(bool enabled)
{
    m_compressQuantizeRot = enabled;
    if (!enabled)
    {
        m_compressStripRot = false;
    }
}

bool Scene3DRenderer::getCompressionQuantizeRotations() const
{
    return m_compressQuantizeRot;
}

void Scene3DRenderer::setCompressionStripRotations(bool enabled)
{
    if (!m_compressQuantizeRot)
        enabled = false;
    m_compressStripRot = enabled;
}

bool Scene3DRenderer::getCompressionStripRotations() const
{
    return m_compressStripRot;
}

bool Scene3DRenderer::OptimizeJsonClipInternal(JsonAnimClip* clip)
{
    if (!clip)
        return false;

    m_compressRotBefore = CountQuatKeys(clip);
    m_compressTransBefore = CountVec3Keys(clip->translationTracks);
    m_compressScaleBefore = CountScaleKeys(clip->scaleTracks);
    m_compressRootBefore = (int)clip->rootFrames.size();

    for (size_t i = 0; i < clip->tracks.size(); ++i)
    {
        ReduceQuatTrack(clip->tracks[i], m_compressRotTolDeg);
    }
    for (size_t i = 0; i < clip->translationTracks.size(); ++i)
    {
        ReduceVec3Track(clip->translationTracks[i].frames, clip->translationTracks[i].translations, m_compressPosTol);
    }
    for (size_t i = 0; i < clip->scaleTracks.size(); ++i)
    {
        ReduceVec3Track(clip->scaleTracks[i].frames, clip->scaleTracks[i].scales, m_compressScaleTol);
    }
    if (!clip->rootFrames.empty() && !clip->rootTranslations.empty() &&
        clip->rootFrames.size() == clip->rootTranslations.size())
    {
        ReduceVec3Track(clip->rootFrames, clip->rootTranslations, m_compressRootTol);
    }

    if (m_compressQuantizeRot)
    {
        for (size_t i = 0; i < clip->tracks.size(); ++i)
        {
            JsonTrack& tr = clip->tracks[i];
            tr.rotationsPacked.clear();
            tr.rotationsPackedValid = false;
            if (tr.rotations.empty())
                continue;
            tr.rotationsPacked.resize(tr.rotations.size());
            for (size_t k = 0; k < tr.rotations.size(); ++k)
            {
                hkQuaternion q = MakeQuaternion(tr.rotations[k]);
                tr.rotationsPacked[k] = PackQuat16(q);
            }
            tr.rotationsPackedValid = true;
            if (m_compressStripRot)
            {
                tr.rotations.clear();
            }
        }
    }
    else
    {
        for (size_t i = 0; i < clip->tracks.size(); ++i)
        {
            clip->tracks[i].rotationsPacked.clear();
            clip->tracks[i].rotationsPackedValid = false;
        }
    }

    SortTracksByBoneIndex(clip);

    m_compressRotAfter = CountQuatKeys(clip);
    m_compressTransAfter = CountVec3Keys(clip->translationTracks);
    m_compressScaleAfter = CountScaleKeys(clip->scaleTracks);
    m_compressRootAfter = (int)clip->rootFrames.size();
    m_compressStatsValid = true;

    char msg[256];
    sprintf_s(msg,
              "Compression: rot %d->%d, pos %d->%d, scale %d->%d, root %d->%d",
              m_compressRotBefore, m_compressRotAfter,
              m_compressTransBefore, m_compressTransAfter,
              m_compressScaleBefore, m_compressScaleAfter,
              m_compressRootBefore, m_compressRootAfter);
    RendererLog(msg);

    return true;
}

bool Scene3DRenderer::optimizeActiveJsonClip()
{
    JsonAnimClip* clip = getActiveJsonClipForEdit();
    return OptimizeJsonClipInternal(clip);
}

void Scene3DRenderer::getLastCompressionStats(int& rotBefore, int& rotAfter,
                                              int& transBefore, int& transAfter,
                                              int& scaleBefore, int& scaleAfter,
                                              int& rootBefore, int& rootAfter) const
{
    rotBefore = m_compressRotBefore;
    rotAfter = m_compressRotAfter;
    transBefore = m_compressTransBefore;
    transAfter = m_compressTransAfter;
    scaleBefore = m_compressScaleBefore;
    scaleAfter = m_compressScaleAfter;
    rootBefore = m_compressRootBefore;
    rootAfter = m_compressRootAfter;
}

bool Scene3DRenderer::hasLastCompressionStats() const
{
    return m_compressStatsValid;
}

float Scene3DRenderer::applyTimeWarpToClipTime(const JsonAnimClip* clip, float timeSeconds) const
{
    if (!m_timeWarpEnabled || !clip)
        return timeSeconds;
    float duration = GetClipDuration(clip);
    if (duration <= 0.0f)
        return timeSeconds;
    float t = timeSeconds;
    if (t < 0.0f) t = 0.0f;
    if (t > duration) t = duration;
    float norm = t / duration;
    int type = m_timeWarpEasingType;
    if (type < 0) type = 0;
    if (type >= EASING_COUNT) type = EASING_COUNT - 1;
    float warped = EvaluateEasing(norm, type, m_timeWarpCp1x, m_timeWarpCp1y, m_timeWarpCp2x, m_timeWarpCp2y);
    if (warped < 0.0f) warped = 0.0f;
    if (warped > 1.0f) warped = 1.0f;
    return warped * duration;
}

bool Scene3DRenderer::applyRootMotionToPose(hkaPose& pose, const JsonAnimClip* clip, float timeSeconds)
{
    if (!m_gameModel || !m_gameModel->skeleton)
        return false;

    const hkaSkeleton* skeleton = m_gameModel->skeleton;
    const int boneCount = skeleton->m_numBones;
    if (boneCount <= 0)
        return false;

    int rootIdx = 0;
    if (clip && clip->rootBoneIndex >= 0 && clip->rootBoneIndex < boneCount)
        rootIdx = clip->rootBoneIndex;

    // Always keep prev position updated so mode switches don't jump.
    pose.syncModelSpace();
    hkVector4 rootPos = pose.getBoneModelSpace(rootIdx).getTranslation();

    hkVector4 delta;
    delta.setZero4();
    bool hasDelta = false;

    if (m_rootMotionPrevValid)
    {
        bool wrapped = (timeSeconds + 1e-4f < m_rootMotionPrevTime);
        if (wrapped && clip)
        {
            JsonVec3 startT, endT;
            if (SampleRootTranslationAt(clip, 0.0f, startT) &&
                SampleRootTranslationAt(clip, GetClipDuration(clip), endT))
            {
                hkVector4 startV;
                hkVector4 endV;
                startV.set(startT.x, startT.y, startT.z);
                endV.set(endT.x, endT.y, endT.z);
                hkVector4 d1;
                d1.setSub4(endV, m_rootMotionPrevPos);
                hkVector4 d2;
                d2.setSub4(rootPos, startV);
                delta.setAdd4(d1, d2);
                hasDelta = true;
            }
        }
        if (wrapped && !clip)
        {
            m_rootMotionPrevPos = rootPos;
            m_rootMotionPrevTime = timeSeconds;
            m_rootMotionPrevValid = true;
            return false;
        }
        if (!hasDelta)
        {
            delta.setSub4(rootPos, m_rootMotionPrevPos);
            hasDelta = true;
        }
    }

    m_rootMotionPrevPos = rootPos;
    m_rootMotionPrevTime = timeSeconds;
    m_rootMotionPrevValid = true;

    if (!hasDelta)
        return false;

    // Locks/clamps
    if (m_rootMotionLock[0]) delta(0) = 0.0f;
    if (m_rootMotionLock[1]) delta(1) = 0.0f;
    if (m_rootMotionLock[2]) delta(2) = 0.0f;
    if (m_rootMotionMode == ROOT_MOTION_CLAMP_Y)
        delta(1) = 0.0f;

    // Motion warping (applies when extracting root motion)
    if (m_rootMotionMode == ROOT_MOTION_EXTRACT &&
        m_rootMotionWarpEnabled &&
        m_rootMotionWarpMode != ROOT_WARP_NONE)
    {
        float tx = m_rootMotionWarpTarget(0);
        float tz = m_rootMotionWarpTarget(2);
        float targetLen = sqrtf(tx * tx + tz * tz);

        hkVector4 srcTotal;
        srcTotal.set(delta(0), 0.0f, delta(2));
        if (clip)
        {
            JsonVec3 startT, endT;
            if (SampleRootTranslationAt(clip, 0.0f, startT) &&
                SampleRootTranslationAt(clip, GetClipDuration(clip), endT))
            {
                srcTotal.set(endT.x - startT.x, 0.0f, endT.z - startT.z);
            }
        }

        float sx = srcTotal(0);
        float sz = srcTotal(2);
        float srcLen = sqrtf(sx * sx + sz * sz);

        float scale = 1.0f;
        if ((m_rootMotionWarpMode == ROOT_WARP_SCALE || m_rootMotionWarpMode == ROOT_WARP_SCALE_REDIRECT) &&
            srcLen > 1e-5f && targetLen > 1e-5f)
        {
            scale = targetLen / srcLen;
        }

        hkQuaternion yawRot;
        yawRot.setIdentity();
        if ((m_rootMotionWarpMode == ROOT_WARP_REDIRECT || m_rootMotionWarpMode == ROOT_WARP_SCALE_REDIRECT) &&
            srcLen > 1e-5f && targetLen > 1e-5f)
        {
            float srcYaw = atan2f(sz, sx);
            float dstYaw = atan2f(tz, tx);
            float yaw = dstYaw - srcYaw;
            hkVector4 axis;
            axis.set(0.0f, 1.0f, 0.0f);
            yawRot.setAxisAngle(axis, yaw);
        }

        hkVector4 warped = delta;
        if (m_rootMotionWarpMode == ROOT_WARP_REDIRECT || m_rootMotionWarpMode == ROOT_WARP_SCALE_REDIRECT)
        {
            warped = RotateVectorByQuat(yawRot, warped);
        }
        if (m_rootMotionWarpMode == ROOT_WARP_SCALE || m_rootMotionWarpMode == ROOT_WARP_SCALE_REDIRECT)
        {
            hkVector4 scaled;
            scaled.setMul4(scale, warped);
            warped = scaled;
        }
        delta = warped;
    }

    if (m_rootMotionMode == ROOT_MOTION_EXTRACT)
    {
        m_rootMotionOffset.add4(delta);
    }

    if (m_rootMotionMode == ROOT_MOTION_OFF || m_rootMotionMode == ROOT_MOTION_EXTRACT)
    {
        hkVector4 refT = skeleton->m_referencePose[rootIdx].getTranslation();
        hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();
        local[rootIdx].setTranslation(refT);
        pose.syncModelSpace();
        return true;
    }
    if (m_rootMotionMode == ROOT_MOTION_CLAMP_Y)
    {
        hkVector4 refT = skeleton->m_referencePose[rootIdx].getTranslation();
        hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();
        hkVector4 curT = local[rootIdx].getTranslation();
        curT(1) = refT(1);
        if (m_rootMotionLock[0]) curT(0) = refT(0);
        if (m_rootMotionLock[2]) curT(2) = refT(2);
        local[rootIdx].setTranslation(curT);
        pose.syncModelSpace();
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Pose building from JSON animation data
// ---------------------------------------------------------------------------

void Scene3DRenderer::applyJsonClipToLocalPose(const JsonAnimClip* clip, hkQsTransform* local, float timeSeconds)
{
    if (!clip || !local || !m_gameModel || !m_gameModel->skeleton)
    {
        return;
    }

    const bool useNlerpRot = (m_rotInterpMode == ROT_INTERP_NLERP);

    float frameTime = (clip->frameTime > 0.0f) ? clip->frameTime : (1.0f / 30.0f);
    float duration = clip->duration;
    if (duration <= 0.0f)
    {
        int frameCount = (clip->frameCount > 0) ? clip->frameCount : 1;
        duration = frameTime * (float)(frameCount > 1 ? (frameCount - 1) : 1);
    }

    float t = timeSeconds;
    if (duration > 0.0f)
    {
        if (t < 0.0f) t = 0.0f;
        if (t > duration) t = duration;
    }
    t = applyTimeWarpToClipTime(clip, t);

    float frame = (frameTime > 0.0f) ? (t / frameTime) : 0.0f;

    // Optional logging (only for primary clip to avoid double-logging during blending).
    bool logThisFrame = false;
    int logFrameIndex = 0;
    if (clip == m_jsonAnim && m_logType2)
    {
        logFrameIndex = (int)(frame + 0.5f);
        if (logFrameIndex != m_lastType2LogFrame)
        {
            logThisFrame = true;
            m_lastType2LogFrame = logFrameIndex;
        }
    }

    // Rotation tracks
    for (int i = 0; i < (int)clip->tracks.size(); i++)
    {
        const JsonTrack& track = clip->tracks[i];
        if (track.boneIndex < 0 || track.boneIndex >= m_gameModel->skeleton->m_numBones)
        {
            continue;
        }

        const int keyCount = (int)track.frames.size();
        if (keyCount == 0 ||
            (int)track.rotations.size() != keyCount)
        {
            continue;
        }

        hkQuaternion q;
        q.setIdentity();
        if (keyCount == 1)
        {
            JsonTrack::Quat4 qa = ApplyQuatAxisMap(SelectQuatForMode(track, 0, m_jsonDecodeMode),
                                                   m_rotAxisMode,
                                                   m_rotSignMask);
            q = MakeQuaternion(qa);
        }
        else if (frame <= (float)track.frames[0])
        {
            JsonTrack::Quat4 qa = ApplyQuatAxisMap(SelectQuatForMode(track, 0, m_jsonDecodeMode),
                                                   m_rotAxisMode,
                                                   m_rotSignMask);
            q = MakeQuaternion(qa);
        }
        else if (frame >= (float)track.frames[keyCount - 1])
        {
            JsonTrack::Quat4 qa = ApplyQuatAxisMap(SelectQuatForMode(track, keyCount - 1, m_jsonDecodeMode),
                                                   m_rotAxisMode,
                                                   m_rotSignMask);
            q = MakeQuaternion(qa);
        }
        else
        {
            std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
            int k = (int)(it - track.frames.begin()) - 1;
            if (k >= 0 && k < keyCount - 1)
            {
                int f0 = track.frames[k];
                int f1 = track.frames[k + 1];
                float span = (float)(f1 - f0);
                float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                if (alpha < 0.0f) alpha = 0.0f;
                if (alpha > 1.0f) alpha = 1.0f;
                JsonTrack::Quat4 q0a = ApplyQuatAxisMap(SelectQuatForMode(track, k, m_jsonDecodeMode),
                                                        m_rotAxisMode,
                                                        m_rotSignMask);
                JsonTrack::Quat4 q1a = ApplyQuatAxisMap(SelectQuatForMode(track, k + 1, m_jsonDecodeMode),
                                                        m_rotAxisMode,
                                                        m_rotSignMask);
                hkQuaternion q0 = MakeQuaternion(q0a);
                hkQuaternion q1 = MakeQuaternion(q1a);
                q = QuatBlend(q0, q1, alpha, useNlerpRot);
            }
        }

        if (m_applyPoseCorrection && m_rotApplyMode == ROT_DELTA_ONLY)
        {
            hkQuaternion ref = local[track.boneIndex].getRotation();
            JsonTrack::Quat4 q0a = ApplyQuatAxisMap(SelectQuatForMode(track, 0, m_jsonDecodeMode),
                                                    m_rotAxisMode,
                                                    m_rotSignMask);
            hkQuaternion q0 = MakeQuaternion(q0a);
            hkQuaternion inv;
            inv.setInverse(q0);
            hkQuaternion corr;
            corr.setMul(ref, inv);
            corr.normalize();
            hkQuaternion corrected;
            corrected.setMul(corr, q);
            corrected.normalize();
            q = corrected;
        }

        if (m_jsonDecodeMode == 3)
        {
            hkQuaternion ref = local[track.boneIndex].getRotation();
            hkQuaternion combined;
            if (m_rotApplyMode == ROT_DELTA_ONLY)
            {
                local[track.boneIndex].setRotation(q);
            }
            else
            {
                if (m_rotApplyMode == ROT_DELTA_REF)
                {
                    combined.setMul(q, ref);
                }
                else
                {
                    combined.setMul(ref, q);
                }
                combined.normalize();
                local[track.boneIndex].setRotation(combined);
            }
        }
        else
        {
            local[track.boneIndex].setRotation(q);
        }
    }

    // Translation tracks
    if (!clip->translationTracks.empty())
    {
        for (int i = 0; i < (int)clip->translationTracks.size(); i++)
        {
            const JsonTranslationTrack& track = clip->translationTracks[i];
            if (track.boneIndex < 0 || track.boneIndex >= m_gameModel->skeleton->m_numBones)
            {
                continue;
            }

            const int keyCount = (int)track.frames.size();
            if (keyCount == 0 || (int)track.translations.size() != keyCount)
            {
                continue;
            }

            JsonVec3 tt = track.translations[0];
            if (keyCount == 1)
            {
                tt = track.translations[0];
            }
            else if (frame <= (float)track.frames[0])
            {
                tt = track.translations[0];
            }
            else if (frame >= (float)track.frames[keyCount - 1])
            {
                tt = track.translations[keyCount - 1];
            }
            else
            {
                std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
                int k = (int)(it - track.frames.begin()) - 1;
                if (k >= 0 && k < keyCount - 1)
                {
                    int f0 = track.frames[k];
                    int f1 = track.frames[k + 1];
                    float span = (float)(f1 - f0);
                    float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                    if (alpha < 0.0f) alpha = 0.0f;
                    if (alpha > 1.0f) alpha = 1.0f;
                    // Catmull-Rom for C1-continuous translation
                    tt = CatmullRomVec3(track.frames, track.translations, keyCount, k, alpha);
                }
            }

            if (logThisFrame)
            {
                const hkaBone* bone = m_gameModel->skeleton->m_bones[track.boneIndex];
                const char* name = bone ? bone->m_name : NULL;
                if (name && (strcmp(name, "bone_globalsrt") == 0 || strcmp(name, "Bone_Root") == 0))
                {
                    char msg[256];
                    sprintf_s(msg,
                              "Type2Trans frame=%d bone=%s t=(%.4f, %.4f, %.4f)",
                              logFrameIndex,
                              name,
                              tt.x, tt.y, tt.z);
                    RendererLog(msg);
                }
            }

            hkVector4 v;
            v.set(tt.x, tt.y, tt.z);
            local[track.boneIndex].setTranslation(v);
        }
    }

    // Root translations
    if (clip->rootBoneIndex >= 0 &&
        clip->rootBoneIndex < m_gameModel->skeleton->m_numBones &&
        !clip->rootTranslations.empty() &&
        !clip->rootFrames.empty() &&
        (int)clip->rootFrames.size() == (int)clip->rootTranslations.size())
    {
        int keyCount = (int)clip->rootFrames.size();
        JsonVec3 tt = clip->rootTranslations[0];

        if (keyCount == 1)
        {
            tt = clip->rootTranslations[0];
        }
        else if (frame <= (float)clip->rootFrames[0])
        {
            tt = clip->rootTranslations[0];
        }
        else if (frame >= (float)clip->rootFrames[keyCount - 1])
        {
            tt = clip->rootTranslations[keyCount - 1];
        }
        else
        {
            std::vector<int>::const_iterator it = std::upper_bound(clip->rootFrames.begin(), clip->rootFrames.end(), frame, FloatLessInt());
            int k = (int)(it - clip->rootFrames.begin()) - 1;
            if (k >= 0 && k < keyCount - 1)
            {
                int f0 = clip->rootFrames[k];
                int f1 = clip->rootFrames[k + 1];
                float span = (float)(f1 - f0);
                float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                // Catmull-Rom for smooth root motion
                tt = CatmullRomVec3(clip->rootFrames, clip->rootTranslations, keyCount, k, alpha);
            }
        }

        hkVector4 v;
        v.set(tt.x, tt.y, tt.z);
        local[clip->rootBoneIndex].setTranslation(v);
    }

    // Scale tracks
    if (!clip->scaleTracks.empty())
    {
        for (int i = 0; i < (int)clip->scaleTracks.size(); i++)
        {
            const JsonScaleTrack& track = clip->scaleTracks[i];
            if (track.boneIndex < 0 || track.boneIndex >= m_gameModel->skeleton->m_numBones)
                continue;

            const int keyCount = (int)track.frames.size();
            if (keyCount == 0 || (int)track.scales.size() != keyCount)
                continue;

            JsonVec3 ss = track.scales[0];
            if (keyCount == 1)
            {
                ss = track.scales[0];
            }
            else if (frame <= (float)track.frames[0])
            {
                ss = track.scales[0];
            }
            else if (frame >= (float)track.frames[keyCount - 1])
            {
                ss = track.scales[keyCount - 1];
            }
            else
            {
                std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
                int k = (int)(it - track.frames.begin()) - 1;
                if (k >= 0 && k < keyCount - 1)
                {
                    int f0 = track.frames[k];
                    int f1 = track.frames[k + 1];
                    float span = (float)(f1 - f0);
                    float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                    if (alpha < 0.0f) alpha = 0.0f;
                    if (alpha > 1.0f) alpha = 1.0f;
                    const JsonVec3& a = track.scales[k];
                    const JsonVec3& b = track.scales[k + 1];
                    ss.x = a.x + (b.x - a.x) * alpha;
                    ss.y = a.y + (b.y - a.y) * alpha;
                    ss.z = a.z + (b.z - a.z) * alpha;
                }
            }

            hkVector4 sv;
            sv.set(ss.x, ss.y, ss.z);
            local[track.boneIndex].setScale(sv);
        }
    }
}

void Scene3DRenderer::rebuildJsonBlendMask()
{
    m_jsonBlendMask.clear();
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return;
    }

    const int boneCount = m_gameModel->skeleton->m_numBones;
    if (boneCount <= 0)
    {
        return;
    }

    const hkInt16* parents = m_gameModel->skeleton->m_parentIndices;

    // Default: full-body influence.
    m_jsonBlendMask.assign(boneCount, 1.0f);

    const int root = m_jsonBlendLayerRootBone;
    if (!parents || root < 0 || root >= boneCount)
    {
        return;
    }

    // Layered: only the subtree under root has weight 1.
    m_jsonBlendMask.assign(boneCount, 0.0f);
    for (int i = 0; i < boneCount; ++i)
    {
        int p = i;
        while (p >= 0 && p < boneCount)
        {
            if (p == root)
            {
                m_jsonBlendMask[i] = 1.0f;
                break;
            }
            p = (int)parents[p];
        }
    }
}

void Scene3DRenderer::ensureJsonBlendAdditiveRefPose()
{
    m_jsonBlendAdditiveRefValid = false;
    m_jsonBlendAdditiveRefLocal.clear();

    if (!m_gameModel || !m_gameModel->skeleton || !m_jsonBlendAnim)
    {
        return;
    }

    const int boneCount = m_gameModel->skeleton->m_numBones;
    if (boneCount <= 0)
    {
        return;
    }

    m_jsonBlendAdditiveRefLocal.setSize(boneCount);

    hkaPose refPose(m_gameModel->skeleton);
    refPose.setToReferencePose();
    hkQsTransform* local = refPose.writeAccessPoseLocalSpace().begin();
    applyJsonClipToLocalPose(m_jsonBlendAnim, local, 0.0f);

    for (int i = 0; i < boneCount; ++i)
    {
        m_jsonBlendAdditiveRefLocal[i] = local[i];
    }
    m_jsonBlendAdditiveRefValid = true;
}

void Scene3DRenderer::ensureGraphStateAdditiveRef(int stateIndex)
{
    if (!m_animGraph || !m_gameModel || !m_gameModel->skeleton)
        return;
    if (stateIndex < 0 || stateIndex >= (int)m_animGraph->states.size())
        return;

    GraphState& st = m_animGraph->states[stateIndex];
    if (!st.clip)
        return;

    const int boneCount = m_gameModel->skeleton->m_numBones;
    if (boneCount <= 0)
        return;

    if (st.additiveRefValid && st.additiveRefLocal.getSize() == boneCount)
        return;

    st.additiveRefLocal.setSize(boneCount);

    hkaPose refPose(m_gameModel->skeleton);
    refPose.setToReferencePose();
    hkQsTransform* local = refPose.writeAccessPoseLocalSpace().begin();
    applyJsonClipToLocalPose(st.clip, local, 0.0f);

    for (int i = 0; i < boneCount; ++i)
    {
        st.additiveRefLocal[i] = local[i];
    }
    st.additiveRefValid = true;
}

hkQsTransform* Scene3DRenderer::getGraphScratchLocal(int boneCount, hkQsTransform* avoid)
{
    if (boneCount <= 0)
        return NULL;

    if (m_graphScratchLocalA.getSize() != boneCount)
        m_graphScratchLocalA.setSize(boneCount);
    if (m_graphScratchLocalB.getSize() != boneCount)
        m_graphScratchLocalB.setSize(boneCount);

    hkQsTransform* a = m_graphScratchLocalA.begin();
    hkQsTransform* b = m_graphScratchLocalB.begin();

    if (avoid == a) return b;
    if (avoid == b) return a;
    return a;
}

hkQsTransform* Scene3DRenderer::getJsonBlendScratchLocal(int boneCount)
{
    if (boneCount <= 0)
        return NULL;

    if (m_jsonBlendScratchLocal.getSize() != boneCount)
        m_jsonBlendScratchLocal.setSize(boneCount);

    return m_jsonBlendScratchLocal.begin();
}

void Scene3DRenderer::applyJsonBlendToLocalPose(hkQsTransform* local, float timeSeconds)
{
    if (!local || !m_gameModel || !m_gameModel->skeleton)
        return;

    if (!(m_jsonBlendEnabled && m_useJsonBlendAnim && m_jsonBlendAnim))
        return;

    // Compute effective alpha (manual alpha * auto-fade weight)
    float effectiveAlpha = m_jsonBlendAlpha;

    // Auto-fade transition: smoothly ramp alpha over m_jsonBlendFadeDuration
    if (m_jsonBlendFadeDuration > 0.0f)
    {
        float fadeT = (m_jsonBlendFadeElapsed < m_jsonBlendFadeDuration)
                      ? (m_jsonBlendFadeElapsed / m_jsonBlendFadeDuration)
                      : 1.0f;
        // Apply easing to the fade curve
        float fadeFactor = EvaluateEasing(fadeT, m_jsonBlendFadeEasing, 0.25f, 0.0f, 0.75f, 1.0f);
        if (!m_jsonBlendFadingIn)
            fadeFactor = 1.0f - fadeFactor; // fade out = reverse
        effectiveAlpha *= fadeFactor;
    }

    if (effectiveAlpha <= 0.0f)
        return;

    const int boneCount = m_gameModel->skeleton->m_numBones;
    if (boneCount <= 0)
        return;

    // Use independent blend time when enabled, otherwise sync to primary clip
    float blendSampleTime = timeSeconds;
    if (m_jsonBlendIndependentTime)
    {
        blendSampleTime = m_jsonBlendTime;

        // Handle looping for independent blend clip
        if (m_jsonBlendLoopBlendClip)
        {
            float blendDur = GetClipDuration(m_jsonBlendAnim);
            if (blendDur > 0.0f && blendSampleTime > blendDur)
            {
                blendSampleTime = fmodf(blendSampleTime, blendDur);
                if (blendSampleTime < 0.0f) blendSampleTime += blendDur;
            }
        }
    }

    hkQsTransform* blendLocal = getJsonBlendScratchLocal(boneCount);
    if (!blendLocal)
        return;
    CopyReferencePoseLocal(m_gameModel->skeleton, blendLocal, boneCount);
    applyJsonClipToLocalPose(m_jsonBlendAnim, blendLocal, blendSampleTime);

    const bool useNlerp = (m_jsonBlendRotMode == JSON_BLEND_ROT_NLERP);

    const float* mask = NULL;
    if (m_jsonBlendMode == JSON_BLEND_LAYERED || m_jsonBlendMode == JSON_BLEND_ADDITIVE)
    {
        if ((int)m_jsonBlendMask.size() != boneCount)
        {
            rebuildJsonBlendMask();
        }
        if (!m_jsonBlendMask.empty())
        {
            mask = &m_jsonBlendMask[0];
        }
    }

    if (m_jsonBlendMode == JSON_BLEND_ADDITIVE)
    {
        if (!m_jsonBlendAdditiveRefValid || m_jsonBlendAdditiveRefLocal.getSize() != boneCount)
        {
            ensureJsonBlendAdditiveRefPose();
        }
        if (m_jsonBlendAdditiveRefValid && m_jsonBlendAdditiveRefLocal.getSize() == boneCount)
        {
            ApplyAdditiveLocalPose(local, blendLocal, m_jsonBlendAdditiveRefLocal.begin(), boneCount, effectiveAlpha, mask, useNlerp);
        }
    }
    else
    {
        BlendLocalPose(local, blendLocal, boneCount, effectiveAlpha,
                      (m_jsonBlendMode == JSON_BLEND_LAYERED) ? mask : NULL,
                      useNlerp);
    }

    // Multi-clip blend layers (up to 16 additional clips layered on top)
    for (int li = 0; li < MAX_BLEND_LAYERS; ++li)
    {
        const BlendLayer& layer = m_blendLayers[li];
        if (!layer.active || !layer.clip || layer.weight <= 0.0f)
            continue;

        // Compute effective weight with eased transition
        float layerWeight = layer.weight;
        if (layer.inTransition && layer.transitionDuration > 0.0f)
        {
            float t = layer.transitionElapsed / layer.transitionDuration;
            if (t > 1.0f) t = 1.0f;
            // Apply easing (default: ease-in-out cubic)
            float eased = EvaluateEasing(t, layer.transitionEasing, 0.42f, 0.0f, 0.58f, 1.0f);
            layerWeight *= eased;
        }
        if (layerWeight <= 0.0f) continue;

        float layerTime = layer.time;
        if (layer.loop)
        {
            float layerDur = GetClipDuration(layer.clip);
            if (layerDur > 0.0f && layerTime > layerDur)
            {
                layerTime = fmodf(layerTime, layerDur);
                if (layerTime < 0.0f) layerTime += layerDur;
            }
        }

        // Sample the layer clip into scratch buffer
        CopyReferencePoseLocal(m_gameModel->skeleton, blendLocal, boneCount);
        applyJsonClipToLocalPose(layer.clip, blendLocal, layerTime);

        // Apply per-bone: check the 62-bone checkbox mask
        bool hasMask = false;
        for (int bi = 0; bi < boneCount && bi < 62; ++bi)
        {
            if (layer.boneMask[bi]) { hasMask = true; break; }
        }

        if (hasMask)
        {
            // Per-bone application with exclusive/override mode
            for (int bi = 0; bi < boneCount && bi < 62; ++bi)
            {
                if (!layer.boneMask[bi]) continue;

                if (layer.exclusive)
                {
                    // EXCLUSIVE: masked bones use ONLY the layer's clip pose
                    // Overwrite base animation entirely for this bone
                    if (layerWeight >= 1.0f)
                    {
                        local[bi] = blendLocal[bi];
                    }
                    else
                    {
                        // Blend between base and layer, but at full weight = pure layer
                        hkQuaternion qBase = local[bi].getRotation();
                        hkQuaternion qLayer = blendLocal[bi].getRotation();
                        hkQuaternion qResult = useNlerp
                            ? QuatNlerpShortest(qBase, qLayer, layerWeight)
                            : QuatSlerpShortest(qBase, qLayer, layerWeight);
                        local[bi].setRotation(qResult);

                        hkVector4 tResult = Vec3Lerp(local[bi].getTranslation(),
                                                     blendLocal[bi].getTranslation(), layerWeight);
                        local[bi].setTranslation(tResult);

                        hkVector4 sResult = Vec3Lerp(local[bi].getScale(),
                                                     blendLocal[bi].getScale(), layerWeight);
                        local[bi].setScale(sResult);
                    }
                }
                else
                {
                    // ADDITIVE: blend the layer on top
                    hkQuaternion qBase = local[bi].getRotation();
                    hkQuaternion qLayer = blendLocal[bi].getRotation();
                    hkQuaternion qResult = useNlerp
                        ? QuatNlerpShortest(qBase, qLayer, layerWeight)
                        : QuatSlerpShortest(qBase, qLayer, layerWeight);
                    local[bi].setRotation(qResult);

                    hkVector4 tResult = Vec3Lerp(local[bi].getTranslation(),
                                                 blendLocal[bi].getTranslation(), layerWeight);
                    local[bi].setTranslation(tResult);

                    hkVector4 sResult = Vec3Lerp(local[bi].getScale(),
                                                 blendLocal[bi].getScale(), layerWeight);
                    local[bi].setScale(sResult);
                }
            }
        }
        else
        {
            // No mask = full body blend (legacy behavior)
            BlendLocalPose(local, blendLocal, boneCount, layerWeight, NULL, useNlerp);
        }
    }
}

void Scene3DRenderer::applyEditorOverridesToLocalPose(hkQsTransform* local, int boneCount, float frame)
{
    if (!local || boneCount <= 0)
        return;

    // Editor overrides / recorded keys
    ensureEditorArrays();
    for (int i = 0; i < boneCount; i++)
    {
        if (i >= (int)m_editorRotKeys.size()) break;
        hkQuaternion sample;
        if (SampleEditorKey(m_editorRotKeys[i], frame, m_editorInterpolationMode,
                            m_rotInterpMode == ROT_INTERP_NLERP, sample))
        {
            local[i].setRotation(sample);
        }
        if (i < (int)m_editorPosKeysX.size())
        {
            hkVector4 sampleT;
            if (SampleEditorTransKey(m_editorPosKeysX[i], m_editorPosKeysY[i], m_editorPosKeysZ[i], frame, m_editorInterpolationMode, sampleT))
            {
                local[i].setTranslation(sampleT);
            }
        }
        if (i < (int)m_editorScaleKeysX.size())
        {
            hkVector4 sampleS;
            if (SampleEditorScaleKey(m_editorScaleKeysX[i], m_editorScaleKeysY[i], m_editorScaleKeysZ[i], frame, m_editorInterpolationMode, sampleS))
            {
                local[i].setScale(sampleS);
            }
        }
    }

    if (m_editorLastLocalPose.getSize() == boneCount)
    {
        for (int i = 0; i < boneCount; i++)
        {
            m_editorLastLocalPose[i] = local[i];
        }
    }

    for (int i = 0; i < boneCount && i < m_editorOverrideRot.getSize(); ++i)
    {
        if (!IsIdentityQuatApprox(m_editorOverrideRot[i]))
        {
            hkQuaternion base = local[i].getRotation();
            hkQuaternion finalRot;
            finalRot.setMul(base, m_editorOverrideRot[i]);
            finalRot.normalize();
            local[i].setRotation(finalRot);
        }
    }

    for (int i = 0; i < boneCount && i < (int)m_editorOverrideTrans.size(); ++i)
    {
        if (HasTransOverride(m_editorOverrideTrans[i]))
        {
            hkVector4 base = local[i].getTranslation();
            base(0) += m_editorOverrideTrans[i].x;
            base(1) += m_editorOverrideTrans[i].y;
            base(2) += m_editorOverrideTrans[i].z;
            local[i].setTranslation(base);
        }
    }
}

void Scene3DRenderer::buildPoseFromJson(hkaPose& pose, float timeSeconds)
{
    if (!m_jsonAnim || !m_gameModel || !m_gameModel->skeleton)
    {
        pose.setToReferencePose();
        pose.syncModelSpace();
        return;
    }

    if (m_forceReferencePose)
    {
        pose.setToReferencePose();
        pose.syncModelSpace();
        return;
    }

    // CRITICAL: Initialize all bones to reference pose first
    // This ensures bones without animation data don't get corrupted
    pose.setToReferencePose();

    // Defensive: Verify skeleton is valid
    if (!m_gameModel->skeleton || m_gameModel->skeleton->m_numBones <= 0)
    {
        pose.syncModelSpace();
        return;
    }

    float frameTime = (m_jsonAnim->frameTime > 0.0f) ? m_jsonAnim->frameTime : (1.0f / 30.0f);
    float duration = m_jsonAnim->duration;
    if (duration <= 0.0f)
    {
        int frameCount = (m_jsonAnim->frameCount > 0) ? m_jsonAnim->frameCount : 1;
        duration = frameTime * (float)(frameCount > 1 ? (frameCount - 1) : 1);
    }

    float t = timeSeconds;
    if (duration > 0.0f)
    {
        if (t < 0.0f)
        {
            t = 0.0f;
        }
        if (t > duration)
        {
            t = duration;
        }
    }
    t = applyTimeWarpToClipTime(m_jsonAnim, t);

    float frame = (frameTime > 0.0f) ? (t / frameTime) : 0.0f;

    hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();

    const bool useNlerpRot = (m_rotInterpMode == ROT_INTERP_NLERP);

    bool logThisFrame = false;
    int logFrameIndex = 0;
    if (m_logType2)
    {
        logFrameIndex = (int)(frame + 0.5f);
        if (logFrameIndex != m_lastType2LogFrame)
        {
            logThisFrame = true;
            m_lastType2LogFrame = logFrameIndex;
        }
    }

    for (int i = 0; i < (int)m_jsonAnim->tracks.size(); i++)
    {
        const JsonTrack& track = m_jsonAnim->tracks[i];
        // Skip invalid bone indices
        if (track.boneIndex < 0 || track.boneIndex >= m_gameModel->skeleton->m_numBones)
        {
            continue;
        }

        const int keyCount = (int)track.frames.size();
        if (keyCount == 0 ||
            (int)track.rotations.size() != keyCount)
        {
            continue;
        }

        hkQuaternion q;
        q.setIdentity();
        if (keyCount == 1)
        {
            JsonTrack::Quat4 qa = ApplyQuatAxisMap(SelectQuatForMode(track, 0, m_jsonDecodeMode),
                                                   m_rotAxisMode,
                                                   m_rotSignMask);
            q = MakeQuaternion(qa);
        }
        else if (frame <= (float)track.frames[0])
        {
            JsonTrack::Quat4 qa = ApplyQuatAxisMap(SelectQuatForMode(track, 0, m_jsonDecodeMode),
                                                   m_rotAxisMode,
                                                   m_rotSignMask);
            q = MakeQuaternion(qa);
        }
        else if (frame >= (float)track.frames[keyCount - 1])
        {
            JsonTrack::Quat4 qa = ApplyQuatAxisMap(SelectQuatForMode(track, keyCount - 1, m_jsonDecodeMode),
                                                   m_rotAxisMode,
                                                   m_rotSignMask);
            q = MakeQuaternion(qa);
        }
        else
        {
            // Binary search: find first frame > current frame
            std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
            int k = (int)(it - track.frames.begin()) - 1;
            if (k >= 0 && k < keyCount - 1)
            {
                int f0 = track.frames[k];
                int f1 = track.frames[k + 1];
                float span = (float)(f1 - f0);
                float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                if (alpha < 0.0f) alpha = 0.0f;
                if (alpha > 1.0f) alpha = 1.0f;
                JsonTrack::Quat4 q0a = ApplyQuatAxisMap(SelectQuatForMode(track, k, m_jsonDecodeMode),
                                                        m_rotAxisMode,
                                                        m_rotSignMask);
                JsonTrack::Quat4 q1a = ApplyQuatAxisMap(SelectQuatForMode(track, k + 1, m_jsonDecodeMode),
                                                        m_rotAxisMode,
                                                        m_rotSignMask);
                hkQuaternion q0 = MakeQuaternion(q0a);
                hkQuaternion q1 = MakeQuaternion(q1a);
                q = QuatBlend(q0, q1, alpha, useNlerpRot);
            }
        }

        if (m_applyPoseCorrection && m_rotApplyMode == ROT_DELTA_ONLY)
        {
            hkQuaternion ref = local[track.boneIndex].getRotation();
            JsonTrack::Quat4 q0a = ApplyQuatAxisMap(SelectQuatForMode(track, 0, m_jsonDecodeMode),
                                                    m_rotAxisMode,
                                                    m_rotSignMask);
            hkQuaternion q0 = MakeQuaternion(q0a);
            hkQuaternion inv;
            inv.setInverse(q0);
            hkQuaternion corr;
            corr.setMul(ref, inv);
            corr.normalize();
            hkQuaternion corrected;
            corrected.setMul(corr, q);
            corrected.normalize();
            q = corrected;
        }

        if (m_jsonDecodeMode == 3)
        {
            hkQuaternion ref = local[track.boneIndex].getRotation();
            hkQuaternion combined;
            if (m_rotApplyMode == ROT_DELTA_ONLY)
            {
                local[track.boneIndex].setRotation(q);
            }
            else
            {
                if (m_rotApplyMode == ROT_DELTA_REF)
                {
                    combined.setMul(q, ref);
                }
                else
                {
                    combined.setMul(ref, q);
                }
                combined.normalize();
                local[track.boneIndex].setRotation(combined);
            }
        }
        else
        {
            local[track.boneIndex].setRotation(q);
        }
    }

    if (!m_jsonAnim->translationTracks.empty())
    {
        for (int i = 0; i < (int)m_jsonAnim->translationTracks.size(); i++)
        {
            const JsonTranslationTrack& track = m_jsonAnim->translationTracks[i];
            if (track.boneIndex < 0 || track.boneIndex >= m_gameModel->skeleton->m_numBones)
            {
                continue;
            }

            const int keyCount = (int)track.frames.size();
            if (keyCount == 0 || (int)track.translations.size() != keyCount)
            {
                continue;
            }

            JsonVec3 t = track.translations[0];
            if (keyCount == 1)
            {
                t = track.translations[0];
            }
            else if (frame <= (float)track.frames[0])
            {
                t = track.translations[0];
            }
            else if (frame >= (float)track.frames[keyCount - 1])
            {
                t = track.translations[keyCount - 1];
            }
            else
            {
                // Binary search: find first frame > current frame
                std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
                int k = (int)(it - track.frames.begin()) - 1;
                if (k >= 0 && k < keyCount - 1)
                {
                    int f0 = track.frames[k];
                    int f1 = track.frames[k + 1];
                    float span = (float)(f1 - f0);
                    float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                    if (alpha < 0.0f) alpha = 0.0f;
                    if (alpha > 1.0f) alpha = 1.0f;
                    // Catmull-Rom for C1-continuous translation (no velocity pops at keys)
                    t = CatmullRomVec3(track.frames, track.translations, keyCount, k, alpha, m_loopAnimation);
                }
            }

            if (logThisFrame)
            {
                const hkaBone* bone = m_gameModel->skeleton->m_bones[track.boneIndex];
                const char* name = bone ? bone->m_name : NULL;
                if (name && (strcmp(name, "bone_globalsrt") == 0 || strcmp(name, "Bone_Root") == 0))
                {
                    char msg[256];
                    sprintf_s(msg,
                              "Type2Trans frame=%d bone=%s t=(%.4f, %.4f, %.4f) mode=%s type2=%s",
                              logFrameIndex,
                              name,
                              t.x, t.y, t.z,
                              (m_jsonDecodeMode == 1) ? "legacyA" :
                              (m_jsonDecodeMode == 2) ? "legacyB" :
                              (m_jsonDecodeMode == 3) ? "rotvec" : "havok",
                              (m_type2PackingMode == 1) ? "per-axis" : "interleaved");
                    RendererLog(msg);
                }
            }

            hkVector4 v;
            v.set(t.x, t.y, t.z);
            local[track.boneIndex].setTranslation(v);
        }
    }
    else if (m_jsonAnim->rootBoneIndex >= 0 &&
             m_jsonAnim->rootBoneIndex < m_gameModel->skeleton->m_numBones &&
             !m_jsonAnim->rootFrames.empty() &&
             m_jsonAnim->rootFrames.size() == m_jsonAnim->rootTranslations.size())
    {
        const int keyCount = (int)m_jsonAnim->rootFrames.size();
        JsonVec3 t = m_jsonAnim->rootTranslations[0];

        if (keyCount == 1)
        {
            t = m_jsonAnim->rootTranslations[0];
        }
        else if (frame <= (float)m_jsonAnim->rootFrames[0])
        {
            t = m_jsonAnim->rootTranslations[0];
        }
        else if (frame >= (float)m_jsonAnim->rootFrames[keyCount - 1])
        {
            t = m_jsonAnim->rootTranslations[keyCount - 1];
        }
        else
        {
            // Binary search: find first frame > current frame
            std::vector<int>::const_iterator it = std::upper_bound(m_jsonAnim->rootFrames.begin(), m_jsonAnim->rootFrames.end(), frame, FloatLessInt());
            int k = (int)(it - m_jsonAnim->rootFrames.begin()) - 1;
            if (k >= 0 && k < keyCount - 1)
            {
                int f0 = m_jsonAnim->rootFrames[k];
                int f1 = m_jsonAnim->rootFrames[k + 1];
                float span = (float)(f1 - f0);
                float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                // Catmull-Rom for smooth root motion
                t = CatmullRomVec3(m_jsonAnim->rootFrames, m_jsonAnim->rootTranslations, keyCount, k, alpha, m_loopAnimation);
            }
        }

        hkVector4 v;
        v.set(t.x, t.y, t.z);
        local[m_jsonAnim->rootBoneIndex].setTranslation(v);
    }

    // Apply scale tracks
    if (!m_jsonAnim->scaleTracks.empty())
    {
        for (int i = 0; i < (int)m_jsonAnim->scaleTracks.size(); i++)
        {
            const JsonScaleTrack& track = m_jsonAnim->scaleTracks[i];
            if (track.boneIndex < 0 || track.boneIndex >= m_gameModel->skeleton->m_numBones)
                continue;

            const int keyCount = (int)track.frames.size();
            if (keyCount == 0 || (int)track.scales.size() != keyCount)
                continue;

            JsonVec3 s = track.scales[0];
            if (keyCount == 1)
            {
                s = track.scales[0];
            }
            else if (frame <= (float)track.frames[0])
            {
                s = track.scales[0];
            }
            else if (frame >= (float)track.frames[keyCount - 1])
            {
                s = track.scales[keyCount - 1];
            }
            else
            {
                // Binary search: find first frame > current frame
                std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
                int k = (int)(it - track.frames.begin()) - 1;
                if (k >= 0 && k < keyCount - 1)
                {
                    int f0 = track.frames[k];
                    int f1 = track.frames[k + 1];
                    float span = (float)(f1 - f0);
                    float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                    if (alpha < 0.0f) alpha = 0.0f;
                    if (alpha > 1.0f) alpha = 1.0f;
                    const JsonVec3& a = track.scales[k];
                    const JsonVec3& b = track.scales[k + 1];
                    s.x = a.x + (b.x - a.x) * alpha;
                    s.y = a.y + (b.y - a.y) * alpha;
                    s.z = a.z + (b.z - a.z) * alpha;
                }
            }

            hkVector4 sv;
            sv.set(s.x, s.y, s.z);
            local[track.boneIndex].setScale(sv);
        }
    }

    // Optional pose blending (Phase 2): blend a secondary JSON clip into the base pose.
    // This happens BEFORE editor overrides, so edits apply on top of the blended result.
    applyJsonBlendToLocalPose(local, timeSeconds);

    // Editor overrides / recorded keys
    const int boneCount = m_gameModel->skeleton->m_numBones;
    applyEditorOverridesToLocalPose(local, boneCount, frame);

    // Pose snapshot capture (captures the current posed state)
    if (m_poseSnapshotValid && m_poseSnapshot.getSize() != boneCount)
    {
        // First time or bone count changed — actually capture
        m_poseSnapshot.setSize(boneCount);
        for (int i = 0; i < boneCount; ++i)
            m_poseSnapshot[i] = local[i];
    }

    // Pose snapshot blend-back
    if (m_poseSnapshotBlendActive && m_poseSnapshotValid &&
        m_poseSnapshot.getSize() == boneCount && m_poseSnapshotBlendAlpha < 1.0f)
    {
        BlendLocalPose(local, m_poseSnapshot.begin(), boneCount,
                      1.0f - m_poseSnapshotBlendAlpha, NULL, false);
    }

    bool rootApplied = applyRootMotionToPose(pose, m_jsonAnim, timeSeconds);
    bool ikApplied = applyIKToPose(pose);
    bool physApplied = applyPhysicsToPose(pose);
    if (!ikApplied && !physApplied && !rootApplied)
        pose.syncModelSpace();

    // Record root bone position for path trail
    if (m_rootPathEnabled && m_jsonAnim && m_jsonAnim->rootBoneIndex >= 0 &&
        m_jsonAnim->rootBoneIndex < boneCount)
    {
        hkVector4 rootPos = pose.getBoneModelSpace(m_jsonAnim->rootBoneIndex).getTranslation();
        m_rootPath[m_rootPathHead % ROOT_PATH_MAX].x = rootPos(0);
        m_rootPath[m_rootPathHead % ROOT_PATH_MAX].y = rootPos(1);
        m_rootPath[m_rootPathHead % ROOT_PATH_MAX].z = rootPos(2);
        m_rootPathHead++;
        if (m_rootPathCount < ROOT_PATH_MAX) m_rootPathCount++;
    }
}

static float GetGraphStateDuration(const GraphState& state)
{
    if (state.clip)
        return state.duration;
    if (state.blendGraph)
        return state.blendGraph->duration;
    if (state.subMachine && state.subMachine->currentState >= 0 && state.subMachine->currentState < (int)state.subMachine->states.size())
        return state.subMachine->states[state.subMachine->currentState].duration;
    return state.duration;
}

void Scene3DRenderer::evaluateBlendGraphNode(AnimationGraphRuntime& graph,
                                             BlendGraphRuntime& blendGraph,
                                             int nodeIndex,
                                             hkQsTransform* outLocal,
                                             int boneCount,
                                             float timeSeconds)
{
    if (!m_gameModel || !m_gameModel->skeleton || boneCount <= 0)
        return;
    if (nodeIndex < 0 || nodeIndex >= (int)blendGraph.nodes.size())
    {
        CopyReferencePoseLocal(m_gameModel->skeleton, outLocal, boneCount);
        return;
    }

    BlendGraphNode& node = blendGraph.nodes[nodeIndex];
    if (node.type == BLEND_NODE_REFERENCE && node.referenceNode >= 0)
    {
        evaluateBlendGraphNode(graph, blendGraph, node.referenceNode, outLocal, boneCount, timeSeconds);
        return;
    }
    if (node.type == BLEND_NODE_SUBGRAPH && node.subgraph)
    {
        evaluateBlendGraphNode(graph, *node.subgraph, node.subgraph->resultNode, outLocal, boneCount, timeSeconds);
        return;
    }
    if (node.type == BLEND_NODE_STATEMACHINE && node.stateMachine)
    {
        evaluateGraphStatePose(*node.stateMachine, node.stateMachine->states[node.stateMachine->currentState], outLocal, boneCount,
                               node.stateMachine->stateTime);
        return;
    }

    if (node.type == BLEND_NODE_SAMPLER)
    {
        CopyReferencePoseLocal(m_gameModel->skeleton, outLocal, boneCount);
        if (!node.clip) return;

        float t = timeSeconds * node.rate;
        if (node.useProgressParam && node.progressParamIndex >= 0)
        {
            const std::vector<GraphParam>& params = GetGraphParamsConst(graph);
            if (node.progressParamIndex < (int)params.size())
            {
                float p = params[node.progressParamIndex].f;
                if (p < 0.0f) p = 0.0f;
                if (p > 1.0f) p = 1.0f;
                float dur = GetClipDuration(node.clip);
                t = p * dur;
            }
        }

        if (node.syncToVelocity && node.rootSpeed > 0.0f)
        {
            const char* speedNames[] = { "speed", "move_speed", "CV_PHYSICAL_MOVESPEED_XZ" };
            float speed = GetGraphParamFloatByAliases(graph, speedNames, 3, 0.0f);
            t *= (speed / node.rootSpeed);
        }

        applyJsonClipToLocalPose(node.clip, outLocal, t);
        return;
    }

    if (node.type == BLEND_NODE_BLENDER)
    {
        if (node.tracks.empty())
        {
            CopyReferencePoseLocal(m_gameModel->skeleton, outLocal, boneCount);
            return;
        }

        std::vector<float>& weights = node.tmpWeights;
        if (weights.size() != node.tracks.size())
            weights.assign(node.tracks.size(), 0.0f);
        else
            std::fill(weights.begin(), weights.end(), 0.0f);
        for (int i = 0; i < (int)node.tracks.size(); ++i)
        {
            weights[i] = EvalGraphWeight(node.tracks[i].weight, graph);
        }

        if (node.weightDamp > 0.0f && graph.lastDeltaTime > 0.0f)
        {
            float k = node.weightDamp * graph.lastDeltaTime;
            if (k < 0.0f) k = 0.0f;
            if (k > 1.0f) k = 1.0f;
            if (node.smoothedWeights.size() != weights.size())
                node.smoothedWeights.assign(weights.size(), 0.0f);
            for (int i = 0; i < (int)weights.size(); ++i)
            {
                node.smoothedWeights[i] = node.smoothedWeights[i] + (weights[i] - node.smoothedWeights[i]) * k;
                weights[i] = node.smoothedWeights[i];
            }
        }

        float total = 0.0f;
        for (int i = 0; i < (int)weights.size(); ++i)
            total += weights[i];
        if (node.normalizeWeights && total > 0.0f)
        {
            for (int i = 0; i < (int)weights.size(); ++i)
                weights[i] /= total;
        }

        int first = -1;
        for (int i = 0; i < (int)weights.size(); ++i)
        {
            if (weights[i] > 0.0001f)
            {
                first = i;
                break;
            }
        }

        if (first < 0)
        {
            CopyReferencePoseLocal(m_gameModel->skeleton, outLocal, boneCount);
            return;
        }

        hkQsTransform* temp = getGraphScratchLocal(boneCount, outLocal);
        if (!temp)
        {
            CopyReferencePoseLocal(m_gameModel->skeleton, outLocal, boneCount);
            return;
        }

        float localTime = timeSeconds * node.syncedRate;
        evaluateBlendGraphNode(graph, blendGraph, node.tracks[first].inputNode, outLocal, boneCount, localTime);
        float accum = weights[first];

        for (int i = first + 1; i < (int)node.tracks.size(); ++i)
        {
            float w = weights[i];
            if (w <= 0.0001f) continue;
            evaluateBlendGraphNode(graph, blendGraph, node.tracks[i].inputNode, temp, boneCount, localTime);
            float alpha = (accum + w > 0.0f) ? (w / (accum + w)) : 0.0f;
            const float* mask = node.tracks[i].mask.empty() ? NULL : &node.tracks[i].mask[0];
            BlendLocalPose(outLocal, temp, boneCount, alpha, mask, false);
            accum += w;
        }
        return;
    }

    CopyReferencePoseLocal(m_gameModel->skeleton, outLocal, boneCount);
}

void Scene3DRenderer::evaluateGraphStatePose(AnimationGraphRuntime& graph,
                                             GraphState& state,
                                             hkQsTransform* outLocal,
                                             int boneCount,
                                             float timeSeconds)
{
    if (!m_gameModel || !m_gameModel->skeleton || boneCount <= 0)
        return;

    if (state.clip)
    {
        CopyReferencePoseLocal(m_gameModel->skeleton, outLocal, boneCount);
        applyJsonClipToLocalPose(state.clip, outLocal, timeSeconds);
        return;
    }
    if (state.blendGraph)
    {
        int rootNode = state.blendGraph->resultNode;
        evaluateBlendGraphNode(graph, *state.blendGraph, rootNode, outLocal, boneCount, timeSeconds);
        return;
    }
    if (state.subMachine && state.subMachine->currentState >= 0 &&
        state.subMachine->currentState < (int)state.subMachine->states.size())
    {
        GraphState& subState = state.subMachine->states[state.subMachine->currentState];
        evaluateGraphStatePose(*state.subMachine, subState, outLocal, boneCount, state.subMachine->stateTime);
        return;
    }

    CopyReferencePoseLocal(m_gameModel->skeleton, outLocal, boneCount);
}

void Scene3DRenderer::buildPoseFromGraphMachine(AnimationGraphRuntime& graph, hkQsTransform* local, int boneCount)
{
    if (graph.currentState < 0 || graph.currentState >= (int)graph.states.size())
    {
        CopyReferencePoseLocal(m_gameModel->skeleton, local, boneCount);
        return;
    }

    GraphState& curState = graph.states[graph.currentState];
    evaluateGraphStatePose(graph, curState, local, boneCount, graph.stateTime);

    if (graph.inTransition &&
        graph.activeTransition >= 0 &&
        graph.activeTransition < (int)graph.transitions.size() &&
        graph.nextState >= 0 &&
        graph.nextState < (int)graph.states.size())
    {
        GraphTransition& tr = graph.transitions[graph.activeTransition];
        GraphState& nextState = graph.states[graph.nextState];

        hkQsTransform* blendLocal = getGraphScratchLocal(boneCount, local);
        if (!blendLocal)
        {
            CopyReferencePoseLocal(m_gameModel->skeleton, local, boneCount);
            return;
        }
        evaluateGraphStatePose(graph, nextState, blendLocal, boneCount, graph.nextTime);

        float alpha = 1.0f;
        if (tr.duration > 0.0f)
        {
            alpha = graph.transitionTime / tr.duration;
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;
        }
        alpha = EvaluateEasing(alpha, tr.easingType, tr.cp1x, tr.cp1y, tr.cp2x, tr.cp2y);

        const bool useNlerp = (tr.rotMode == JSON_BLEND_ROT_NLERP);
        const float* mask = NULL;
        if ((tr.blendMode == JSON_BLEND_LAYERED || tr.blendMode == JSON_BLEND_ADDITIVE) && !tr.mask.empty())
        {
            mask = &tr.mask[0];
        }

        if (tr.blendMode == JSON_BLEND_ADDITIVE)
        {
            ensureGraphStateAdditiveRef(graph.nextState);
            if (nextState.additiveRefValid && nextState.additiveRefLocal.getSize() == boneCount)
            {
                ApplyAdditiveLocalPose(local, blendLocal, nextState.additiveRefLocal.begin(), boneCount, alpha, mask, useNlerp);
            }
        }
        else
        {
            BlendLocalPose(local, blendLocal, boneCount, alpha,
                          (tr.blendMode == JSON_BLEND_LAYERED) ? mask : NULL,
                          useNlerp);
        }
    }

    applyJsonBlendToLocalPose(local, graph.stateTime);

    float frame = 0.0f;
    if (curState.clip)
    {
        float warpedTime = applyTimeWarpToClipTime(curState.clip, graph.stateTime);
        frame = ComputeClipFrame(curState.clip, warpedTime);
    }
    applyEditorOverridesToLocalPose(local, boneCount, frame);
}

void Scene3DRenderer::buildPoseFromAnimGraph(hkaPose& pose)
{
    if (!m_animGraph || !m_animGraph->valid || !m_gameModel || !m_gameModel->skeleton)
    {
        pose.setToReferencePose();
        pose.syncModelSpace();
        return;
    }

    if (m_forceReferencePose)
    {
        pose.setToReferencePose();
        pose.syncModelSpace();
        return;
    }

    pose.setToReferencePose();

    const int boneCount = m_gameModel->skeleton->m_numBones;
    if (boneCount <= 0)
    {
        pose.syncModelSpace();
        return;
    }

    hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();
    buildPoseFromGraphMachine(*m_animGraph, local, boneCount);

    JsonAnimClip* curClip = NULL;
    if (m_animGraph->currentState >= 0 && m_animGraph->currentState < (int)m_animGraph->states.size())
        curClip = m_animGraph->states[m_animGraph->currentState].clip;
    bool rootApplied = applyRootMotionToPose(pose, curClip, m_animGraph->stateTime);
    bool ikApplied = applyIKToPose(pose);
    bool physApplied = applyPhysicsToPose(pose);
    if (!ikApplied && !physApplied && !rootApplied)
        pose.syncModelSpace();
}

void Scene3DRenderer::updateAnimationGraph(float deltaTime)
{
    clearGraphOneShotTriggers();
    if (!m_animGraph || !m_animGraph->valid || !m_animGraph->enabled)
        return;

    AnimationGraphRuntime& graph = *m_animGraph;

    UpdateGraphRuntimeParams(*this, graph, m_inputButtonState, m_inputButtonDown, m_graphRngState);

    if (graph.currentState < 0 || graph.currentState >= (int)graph.states.size())
    {
        resetAnimationGraph();
    }
    if (graph.currentState < 0 || graph.currentState >= (int)graph.states.size())
        return;

    if (m_jsonAnimPaused)
        return;

    float prevStateTime = graph.stateTime;
    float prevNextTime = graph.nextTime;

    float dt = deltaTime * m_playbackSpeed;
    graph.lastDeltaTime = deltaTime;
    GraphState& curState = graph.states[graph.currentState];
    float curDuration = GetGraphStateDuration(curState);
    const bool curLoop = curState.loop && m_loopAnimation;

    if (graph.inTransition &&
        graph.activeTransition >= 0 &&
        graph.activeTransition < (int)graph.transitions.size() &&
        graph.nextState >= 0 &&
        graph.nextState < (int)graph.states.size())
    {
        GraphTransition& tr = graph.transitions[graph.activeTransition];
        GraphState& nextState = graph.states[graph.nextState];
        float nextDuration = GetGraphStateDuration(nextState);
        const bool nextLoop = nextState.loop && m_loopAnimation;

        graph.stateTime = AdvanceGraphTime(graph.stateTime, dt * curState.speed, curDuration, curLoop);
        graph.nextTime = AdvanceGraphTime(graph.nextTime, dt * nextState.speed, nextDuration, nextLoop);
        graph.transitionTime += dt;

        ProcessGraphStateDoneEvent(*this, graph, graph.currentState, prevStateTime, graph.stateTime, curLoop);
        ProcessGraphStateDoneEvent(*this, graph, graph.nextState, prevNextTime, graph.nextTime, nextLoop);

        // Event processing (supports blended transitions) before OnEvent handlers/transition checks.
        if (graph.currentState >= 0 && graph.currentState < (int)graph.states.size())
        {
            GraphState& curEvtState = graph.states[graph.currentState];
            unsigned int curKey = (unsigned int)(graph.currentState + 1);
            bool curLoopEvt = curEvtState.loop && m_loopAnimation;
            ProcessGraphStateTimedEvents(*this, graph, graph.currentState, graph.lastEventTime, graph.stateTime, curLoopEvt);
            if (curEvtState.clip)
                processAnimEventsForClip(curEvtState.clip, graph.lastEventTime, graph.stateTime, curKey);
            graph.lastEventTime = graph.stateTime;
        }

        if (graph.nextState >= 0 && graph.nextState < (int)graph.states.size())
        {
            GraphState& nextEvtState = graph.states[graph.nextState];
            unsigned int nextKey = (unsigned int)(graph.nextState + 1);
            bool nextLoopEvt = nextEvtState.loop && m_loopAnimation;
            ProcessGraphStateTimedEvents(*this, graph, graph.nextState, graph.lastNextEventTime, graph.nextTime, nextLoopEvt);
            if (nextEvtState.clip)
                processAnimEventsForClip(nextEvtState.clip, graph.lastNextEventTime, graph.nextTime, nextKey);
            graph.lastNextEventTime = graph.nextTime;
        }

        ProcessGraphStateOnEventHandlers(*this, graph, graph.currentState);
        ProcessGraphStateOnEventHandlers(*this, graph, graph.nextState);

        if (tr.allowInterrupt)
        {
            int nextTr = FindBestGraphTransition(graph, graph.nextState, graph.nextTime);
            if (nextTr >= 0)
            {
                // Promote next state to current before starting a new transition.
                graph.currentState = graph.nextState;
                graph.stateTime = graph.nextTime;
                graph.nextState = -1;
                graph.activeTransition = -1;
                graph.inTransition = false;
                graph.transitionTime = 0.0f;
                graph.transitionDuration = 0.0f;
                graph.lastEventTime = graph.stateTime;
                graph.lastNextEventTime = 0.0f;
                resetEventDedup();

                GraphTransition& tr2 = graph.transitions[nextTr];
                if (tr2.toState != graph.currentState)
                {
                    HandleGraphStateExit(*this, graph, graph.currentState);
                    HandleGraphStateEnter(*this, graph, tr2.toState);
                    StartGraphTransition(graph, nextTr);
                    ConsumeGraphTransitionTriggers(tr2, graph);
                }
            }
        }

        if (graph.inTransition)
        {
            if (graph.transitionDuration <= 0.0f || graph.transitionTime >= graph.transitionDuration)
            {
                // Finish transition.
                graph.currentState = graph.nextState;
                graph.stateTime = graph.nextTime;
                graph.nextState = -1;
                graph.activeTransition = -1;
                graph.inTransition = false;
                graph.transitionTime = 0.0f;
                graph.transitionDuration = 0.0f;
                graph.lastEventTime = graph.stateTime;
                graph.lastNextEventTime = 0.0f;
                resetEventDedup();
            }
        }
    }
    else
    {
        graph.stateTime = AdvanceGraphTime(graph.stateTime, dt * curState.speed, curDuration, curLoop);

        ProcessGraphStateDoneEvent(*this, graph, graph.currentState, prevStateTime, graph.stateTime, curLoop);

        // Event processing before OnEvent handlers/transition checks.
        if (graph.currentState >= 0 && graph.currentState < (int)graph.states.size())
        {
            GraphState& curEvtState = graph.states[graph.currentState];
            unsigned int curKey = (unsigned int)(graph.currentState + 1);
            bool curLoopEvt = curEvtState.loop && m_loopAnimation;
            ProcessGraphStateTimedEvents(*this, graph, graph.currentState, graph.lastEventTime, graph.stateTime, curLoopEvt);
            if (curEvtState.clip)
                processAnimEventsForClip(curEvtState.clip, graph.lastEventTime, graph.stateTime, curKey);
            graph.lastEventTime = graph.stateTime;
        }

        ProcessGraphStateOnEventHandlers(*this, graph, graph.currentState);

        int trIdx = FindBestGraphTransition(graph, graph.currentState, graph.stateTime);
        if (trIdx >= 0)
        {
            GraphTransition& tr = graph.transitions[trIdx];
            if (tr.toState != graph.currentState)
            {
                HandleGraphStateExit(*this, graph, graph.currentState);
                HandleGraphStateEnter(*this, graph, tr.toState);
                StartGraphTransition(graph, trIdx);
                ConsumeGraphTransitionTriggers(tr, graph);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Animation event dispatch
// ---------------------------------------------------------------------------

static unsigned long long MakeEventKey(unsigned int clipKey, int eventIndex)
{
    return ((unsigned long long)clipKey << 32) | (unsigned int)eventIndex;
}

void Scene3DRenderer::processAnimEventsForClip(const JsonAnimClip* clip, float prevTime, float currTime, unsigned int clipKey)
{
    if (!clip || clip->events.empty())
        return;

    const std::vector<JsonAnimEvent>& events = clip->events;
    const int count = (int)events.size();
    const bool wrapped = (currTime < prevTime);
    float prevSample = applyTimeWarpToClipTime(clip, prevTime);
    float currSample = applyTimeWarpToClipTime(clip, currTime);

    if (!wrapped)
    {
        // Normal forward playback (no loop wrap)
        for (int i = 0; i < count; ++i)
        {
            float t = events[i].t;
            if (t > prevSample && t <= currSample)
            {
                // Dedup: skip if this event index was already fired this cycle
                unsigned long long key = MakeEventKey(clipKey, i);
                if (m_firedEventIndices.find(key) != m_firedEventIndices.end())
                    continue;
                m_firedEventIndices.insert(key);
                handleAnimEvent(events[i]);
            }
        }
    }
    else
    {
        // Loop wraparound: fire events in [prevTime, duration] then [0, currTime]
        // Clear dedup for this clip at loop boundary so events can fire again.
        resetEventDedupForClip(clipKey);

        float duration = GetClipDuration(clip);
        for (int i = 0; i < count; ++i)
        {
            float t = events[i].t;
            if (t > prevSample && t <= duration)
            {
                unsigned long long key = MakeEventKey(clipKey, i);
                m_firedEventIndices.insert(key);
                handleAnimEvent(events[i]);
            }
            else if (t >= 0.0f && t <= currSample)
            {
                unsigned long long key = MakeEventKey(clipKey, i);
                if (m_firedEventIndices.find(key) != m_firedEventIndices.end())
                    continue;
                m_firedEventIndices.insert(key);
                handleAnimEvent(events[i]);
            }
        }
    }
}

void Scene3DRenderer::processAnimEvents(float prevTime, float currTime)
{
    processAnimEventsForClip(m_jsonAnim, prevTime, currTime, 0);
}

// ---------------------------------------------------------------------------
// Animation event handler — logs events, extensible for gameplay dispatch
// ---------------------------------------------------------------------------

static const char* AnimEventTypeName(AnimEventType type)
{
    switch (type)
    {
    case ANIM_EVT_APPLY_DAMAGE:                     return "ApplyDamage";
    case ANIM_EVT_APPLY_DAMAGE_TO_TARGET:            return "ApplyDamageToTarget";
    case ANIM_EVT_DAMAGE_OBB_AREA:                   return "DamageOBBArea";
    case ANIM_EVT_DAMAGE_OBB_AREA_LEFT_HAND:         return "DamageOBBArea_LeftHand";
    case ANIM_EVT_DAMAGE_OBB_AREA_RIGHT_HAND:        return "DamageOBBArea_RightHand";
    case ANIM_EVT_DAMAGE_CYLINDER_AREA:              return "DamageCylinderArea";
    case ANIM_EVT_DAMAGE_CYLINDER_AREA_RIGHT_HAND:   return "DamageCylinderArea_RightHand";
    case ANIM_EVT_FF_ONLY_DAMAGE_OBB_AREA:           return "FFOnlyDamageOBBArea";
    case ANIM_EVT_FF_DAMAGE_OBB_AREA_RIGHT_HAND:     return "FFDamageOBBArea_RightHand";
    case ANIM_EVT_CLEAR_HIT_LIST:                    return "ClearHitList";
    case ANIM_EVT_FLYER_CLEAR_HIT_LIST:              return "FlyerClearHitList";
    case ANIM_EVT_FLYER_DAMAGE_OBB_AREA:             return "FlyerDamageOBBArea";
    case ANIM_EVT_GRAB_OBB_AREA_LEFT_HAND:           return "GrabOBBArea_LeftHand";
    case ANIM_EVT_GRAB_OBB_AREA_RIGHT_HAND:          return "GrabOBBArea_RightHand";
    case ANIM_EVT_NEW_HIT2:                          return "NewHit2";
    case ANIM_EVT_NEW_HIT3:                          return "NewHit3";
    case ANIM_EVT_NEW_HIT4:                          return "NewHit4";
    case ANIM_EVT_NEW_HIT5:                          return "NewHit5";
    case ANIM_EVT_TRAIL_ON_LEFT:                     return "TrailOn_Left";
    case ANIM_EVT_TRAIL_ON_RIGHT:                    return "TrailOn_Right";
    case ANIM_EVT_TRAIL_ON_RIGHT_SPLIT:              return "TrailOn_Right_Split";
    case ANIM_EVT_TRAIL_OFF:                         return "TrailOff";
    case ANIM_EVT_TRAIL_OFF_LEFT:                    return "TrailOff_Left";
    case ANIM_EVT_TRAIL_OFF_RIGHT:                   return "TrailOff_Right";
    case ANIM_EVT_ACTIVATE_ATTACHED_PARTICLE_EFFECT: return "ActivateAttachedParticleEffect";
    case ANIM_EVT_ACTIVATE_PARTICLE_EFFECT:          return "ActivateParticleEffect";
    case ANIM_EVT_SOUND_CUE:                         return "SoundCue";
    case ANIM_EVT_SOUND_EVENT:                       return "SoundEvent";
    case ANIM_EVT_SET_SOUND_OVERRIDE:                return "SetSoundOverride";
    case ANIM_EVT_CAMERA:                            return "Camera";
    case ANIM_EVT_CAMERA_EFFECT:                     return "CameraEffect";
    case ANIM_EVT_CAMERA_EFFECT_ALWAYS:              return "CameraEffectAlways";
    case ANIM_EVT_STATE_CHANGE1:                     return "StateChange1";
    case ANIM_EVT_STATE_CHANGE2:                     return "StateChange2";
    case ANIM_EVT_BEGIN_FACE_TARGET:                 return "BeginFaceTarget";
    case ANIM_EVT_END_FACE_TARGET:                   return "EndFaceTarget";
    case ANIM_EVT_POST_GENERIC_EVENT:                return "PostGenericEvent";
    case ANIM_EVT_JUMP:                              return "Jump";
    case ANIM_EVT_DESTROY:                           return "Destroy";
    case ANIM_EVT_SH_INVINCIBLE:                     return "SHInvincible";
    case ANIM_EVT_SELECT_INVENTORY_LOADOUT:          return "SelectInventoryLoadout";
    case ANIM_EVT_ENABLE_DRONE:                      return "EnableDrone";
    case ANIM_EVT_ABILITY_ACTIVATION_EVENT:          return "AbilityActivationEvent";
    case ANIM_EVT_SET_ROTATION_RATE_SCALE:           return "SetRotationRateScale";
    case ANIM_EVT_START_CHARGE:                      return "StartCharge";
    case ANIM_EVT_STOP_CHARGE:                       return "StopCharge";
    case ANIM_EVT_FIRE_PROJECTILE:                   return "FireProjectile";
    case ANIM_EVT_FIRE_PROJECTILE_LEFT_HAND:         return "FireProjectile_LeftHand";
    case ANIM_EVT_FIRE_PROJECTILE_RIGHT_HAND:        return "FireProjectile_RightHand";
    case ANIM_EVT_FIRE_PROJECTILE_VISUAL:            return "FireProjectile_Visual";
    case ANIM_EVT_FIRE_PROJECTILE_VISUAL_LEFT_HAND:  return "FireProjectileVisual_LeftHand";
    case ANIM_EVT_READY_PROJECTILE:                  return "ReadyProjectile";
    case ANIM_EVT_READY_PROJECTILE_LEFT_HAND:        return "ReadyProjectile_LeftHand";
    case ANIM_EVT_READY_PROJECTILE_RIGHT_HAND:       return "ReadyProjectile_RightHand";
    case ANIM_EVT_UNREADY_PROJECTILE_LEFT_HAND:      return "UnreadyProjectile_LeftHand";
    case ANIM_EVT_UNREADY_PROJECTILE_RIGHT_HAND:     return "UnreadyProjectile_RightHand";
    case ANIM_EVT_EMBED_LAUNCH_POINT:                return "EmbedLaunchPoint";
    case ANIM_EVT_THROW_LEFT_HAND:                   return "Throw_LeftHand";
    case ANIM_EVT_THROW_LEFT_TORQUE:                 return "Throw_LeftTorque";
    case ANIM_EVT_THROW_RIGHT_HAND:                  return "Throw_RightHand";
    case ANIM_EVT_THROW_RIGHT_TORQUE:                return "Throw_RightTorque";
    case ANIM_EVT_GRAB_STRING_L:                     return "GrabStringL";
    case ANIM_EVT_RELEASE_STRING_L:                  return "ReleaseStringL";
    case ANIM_EVT_RUMBLE_PLAY:                       return "RumblePlay";
    default:                                         return "Unknown";
    }
}

// Category: 0=damage, 1=trail/vfx, 2=sound, 3=camera, 4=state, 5=projectile, 6=throw, 7=bow, 8=controller
static int AnimEventCategory(AnimEventType type)
{
    switch (type)
    {
    case ANIM_EVT_APPLY_DAMAGE:
    case ANIM_EVT_APPLY_DAMAGE_TO_TARGET:
    case ANIM_EVT_DAMAGE_OBB_AREA:
    case ANIM_EVT_DAMAGE_OBB_AREA_LEFT_HAND:
    case ANIM_EVT_DAMAGE_OBB_AREA_RIGHT_HAND:
    case ANIM_EVT_DAMAGE_CYLINDER_AREA:
    case ANIM_EVT_DAMAGE_CYLINDER_AREA_RIGHT_HAND:
    case ANIM_EVT_FF_ONLY_DAMAGE_OBB_AREA:
    case ANIM_EVT_FF_DAMAGE_OBB_AREA_RIGHT_HAND:
    case ANIM_EVT_CLEAR_HIT_LIST:
    case ANIM_EVT_FLYER_CLEAR_HIT_LIST:
    case ANIM_EVT_FLYER_DAMAGE_OBB_AREA:
    case ANIM_EVT_GRAB_OBB_AREA_LEFT_HAND:
    case ANIM_EVT_GRAB_OBB_AREA_RIGHT_HAND:
    case ANIM_EVT_NEW_HIT2:
    case ANIM_EVT_NEW_HIT3:
    case ANIM_EVT_NEW_HIT4:
    case ANIM_EVT_NEW_HIT5:
        return 0;
    case ANIM_EVT_TRAIL_ON_LEFT:
    case ANIM_EVT_TRAIL_ON_RIGHT:
    case ANIM_EVT_TRAIL_ON_RIGHT_SPLIT:
    case ANIM_EVT_TRAIL_OFF:
    case ANIM_EVT_TRAIL_OFF_LEFT:
    case ANIM_EVT_TRAIL_OFF_RIGHT:
    case ANIM_EVT_ACTIVATE_ATTACHED_PARTICLE_EFFECT:
    case ANIM_EVT_ACTIVATE_PARTICLE_EFFECT:
        return 1;
    case ANIM_EVT_SOUND_CUE:
    case ANIM_EVT_SOUND_EVENT:
    case ANIM_EVT_SET_SOUND_OVERRIDE:
        return 2;
    case ANIM_EVT_CAMERA:
    case ANIM_EVT_CAMERA_EFFECT:
    case ANIM_EVT_CAMERA_EFFECT_ALWAYS:
        return 3;
    case ANIM_EVT_STATE_CHANGE1:
    case ANIM_EVT_STATE_CHANGE2:
    case ANIM_EVT_BEGIN_FACE_TARGET:
    case ANIM_EVT_END_FACE_TARGET:
    case ANIM_EVT_POST_GENERIC_EVENT:
    case ANIM_EVT_JUMP:
    case ANIM_EVT_DESTROY:
    case ANIM_EVT_SH_INVINCIBLE:
    case ANIM_EVT_SELECT_INVENTORY_LOADOUT:
    case ANIM_EVT_ENABLE_DRONE:
    case ANIM_EVT_ABILITY_ACTIVATION_EVENT:
    case ANIM_EVT_SET_ROTATION_RATE_SCALE:
    case ANIM_EVT_START_CHARGE:
    case ANIM_EVT_STOP_CHARGE:
        return 4;
    case ANIM_EVT_FIRE_PROJECTILE:
    case ANIM_EVT_FIRE_PROJECTILE_LEFT_HAND:
    case ANIM_EVT_FIRE_PROJECTILE_RIGHT_HAND:
    case ANIM_EVT_FIRE_PROJECTILE_VISUAL:
    case ANIM_EVT_FIRE_PROJECTILE_VISUAL_LEFT_HAND:
    case ANIM_EVT_READY_PROJECTILE:
    case ANIM_EVT_READY_PROJECTILE_LEFT_HAND:
    case ANIM_EVT_READY_PROJECTILE_RIGHT_HAND:
    case ANIM_EVT_UNREADY_PROJECTILE_LEFT_HAND:
    case ANIM_EVT_UNREADY_PROJECTILE_RIGHT_HAND:
    case ANIM_EVT_EMBED_LAUNCH_POINT:
        return 5;
    case ANIM_EVT_THROW_LEFT_HAND:
    case ANIM_EVT_THROW_LEFT_TORQUE:
    case ANIM_EVT_THROW_RIGHT_HAND:
    case ANIM_EVT_THROW_RIGHT_TORQUE:
        return 6;
    case ANIM_EVT_GRAB_STRING_L:
    case ANIM_EVT_RELEASE_STRING_L:
        return 7;
    case ANIM_EVT_RUMBLE_PLAY:
        return 8;
    default:
        return 4; // unknown → state/misc
    }
}

// ---------------------------------------------------------------------------
// Event inspection getters (for UI / timeline)
// ---------------------------------------------------------------------------

int Scene3DRenderer::getJsonAnimEventCount() const
{
    const JsonAnimClip* clip = getActiveJsonClipForUI();
    if (!clip) return 0;
    return (int)clip->events.size();
}

float Scene3DRenderer::getJsonAnimEventTime(int index) const
{
    const JsonAnimClip* clip = getActiveJsonClipForUI();
    if (!clip || index < 0 || index >= (int)clip->events.size())
        return 0.0f;
    return clip->events[index].t;
}

const char* Scene3DRenderer::getJsonAnimEventTypeName(int index) const
{
    const JsonAnimClip* clip = getActiveJsonClipForUI();
    if (!clip || index < 0 || index >= (int)clip->events.size())
        return "Unknown";
    return AnimEventTypeName(clip->events[index].eventType);
}

int Scene3DRenderer::getJsonAnimEventCategory(int index) const
{
    const JsonAnimClip* clip = getActiveJsonClipForUI();
    if (!clip || index < 0 || index >= (int)clip->events.size())
        return 4;
    return AnimEventCategory(clip->events[index].eventType);
}

void Scene3DRenderer::getJsonAnimEventSummary(int index, char* buf, int bufSize) const
{
    if (!buf || bufSize <= 0) return;
    buf[0] = '\0';
    const JsonAnimClip* clip = getActiveJsonClipForUI();
    if (!clip || index < 0 || index >= (int)clip->events.size())
        return;

    const JsonAnimEvent& evt = clip->events[index];
    int written = sprintf_s(buf, bufSize, "@%.3fs %s", evt.t, AnimEventTypeName(evt.eventType));
    for (size_t i = 0; i < evt.vals.size() && written < bufSize - 32; ++i)
    {
        const JsonAnimEventVal& v = evt.vals[i];
        switch (v.type)
        {
        case JsonAnimEventVal::EVT_INT:
            written += sprintf_s(buf + written, bufSize - written, " [%d]", v.intVal);
            break;
        case JsonAnimEventVal::EVT_FLOAT:
            written += sprintf_s(buf + written, bufSize - written, " [%.2f]", v.floatVal);
            break;
        case JsonAnimEventVal::EVT_CRC:
            if (v.crcVal[0])
                written += sprintf_s(buf + written, bufSize - written, " [%s]", v.crcVal);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Event mutation (add / remove) for timeline editor
// ---------------------------------------------------------------------------

// Generic entry point callable from the host exe (void* avoids exposing vector type)
int Scene3DRenderer::addJsonAnimEventGeneric(int eventType, float timeSeconds, void* pvals)
{
    if (!pvals)
        return -1;
    std::vector<JsonAnimEventVal>* valsPtr = static_cast<std::vector<JsonAnimEventVal>*>(pvals);
    return addJsonAnimEvent(static_cast<AnimEventType>(eventType), timeSeconds, *valsPtr);
}

// New generic event addition function that supports all event types
int Scene3DRenderer::addJsonAnimEvent(AnimEventType eventType, float timeSeconds,
                                      const std::vector<JsonAnimEventVal>& vals)
{
    JsonAnimClip* clip = getActiveJsonClipForEdit();
    if (!clip)
        return -1;

    JsonAnimEvent evt;
    evt.eventType = eventType;
    evt.t = timeSeconds;
    strcpy_s(evt.event, AnimEventTypeName(eventType));
    evt.vals = vals;

    // Ensure we have 9 values (pad with zeros if needed)
    while (evt.vals.size() < 9)
    {
        JsonAnimEventVal v;
        v.type = JsonAnimEventVal::EVT_INT;
        v.intVal = 0;
        evt.vals.push_back(v);
    }

    // Insert sorted by time
    std::vector<JsonAnimEvent>& events = clip->events;
    int insertIdx = (int)events.size();
    for (int i = 0; i < (int)events.size(); ++i)
    {
        if (events[i].t > timeSeconds)
        {
            insertIdx = i;
            break;
        }
    }
    events.insert(events.begin() + insertIdx, evt);
    resetEventDedup();

    char msg[256];
    sprintf_s(msg, sizeof(msg), "Added %s at t=%.3fs (index %d)",
              AnimEventTypeName(eventType), timeSeconds, insertIdx);
    RendererLog(msg);
    return insertIdx;
}

// Legacy function for backward compatibility with SoundEvent/SoundCue
int Scene3DRenderer::addJsonAnimEvent(bool isSoundCue, float timeSeconds,
                                      const char* crc0, const char* crc1, const char* crc2)
{
    if (!crc0 || crc0[0] == '\0')
        return -1;

    std::vector<JsonAnimEventVal> vals;

    // CRC[0]
    {
        JsonAnimEventVal v;
        v.type = JsonAnimEventVal::EVT_CRC;
        strcpy_s(v.crcVal, crc0);
        vals.push_back(v);
    }
    // CRC[1]
    {
        JsonAnimEventVal v;
        v.type = JsonAnimEventVal::EVT_CRC;
        if (crc1 && crc1[0] != '\0')
            strcpy_s(v.crcVal, crc1);
        else
            v.crcVal[0] = '\0';
        vals.push_back(v);
    }
    // CRC[2] (SoundCue only)
    if (isSoundCue)
    {
        JsonAnimEventVal v;
        v.type = JsonAnimEventVal::EVT_CRC;
        if (crc2 && crc2[0] != '\0')
            strcpy_s(v.crcVal, crc2);
        else
            v.crcVal[0] = '\0';
        vals.push_back(v);
    }

    AnimEventType eventType = isSoundCue ? ANIM_EVT_SOUND_CUE : ANIM_EVT_SOUND_EVENT;
    return addJsonAnimEventGeneric(eventType, timeSeconds, &vals);
}

bool Scene3DRenderer::removeJsonAnimEvent(int index)
{
    JsonAnimClip* clip = getActiveJsonClipForEdit();
    if (!clip || index < 0 || index >= (int)clip->events.size())
        return false;

    char msg[256];
    sprintf_s(msg, sizeof(msg), "Removed event %d (%s at t=%.3fs)",
              index, AnimEventTypeName(clip->events[index].eventType),
              clip->events[index].t);
    RendererLog(msg);

    clip->events.erase(clip->events.begin() + index);
    resetEventDedup();
    return true;
}

void Scene3DRenderer::handleAnimEvent(const JsonAnimEvent& evt)
{
    // Build a log line: "AnimEvent @0.350s: DamageOBBArea [CRC=slash_heavy, Int=25, ...]"
    char msg[512];
    const char* typeName = AnimEventTypeName(evt.eventType);
    int written = sprintf_s(msg, sizeof(msg), "AnimEvent @%.3fs: %s", evt.t, typeName);

    // Append parameter summary
    for (size_t i = 0; i < evt.vals.size() && written < 480; ++i)
    {
        const JsonAnimEventVal& v = evt.vals[i];
        switch (v.type)
        {
        case JsonAnimEventVal::EVT_INT:
            written += sprintf_s(msg + written, sizeof(msg) - written, " [Int=%d]", v.intVal);
            break;
        case JsonAnimEventVal::EVT_FLOAT:
            written += sprintf_s(msg + written, sizeof(msg) - written, " [Float=%.3f]", v.floatVal);
            break;
        case JsonAnimEventVal::EVT_CRC:
            if (v.crcVal[0])
                written += sprintf_s(msg + written, sizeof(msg) - written, " [CRC=%s]", v.crcVal);
            break;
        }
    }

    RendererLog(msg);

    // ---------------------------------------------------------------------------
    // Per-type dispatch — expand each case as gameplay systems are implemented
    // ---------------------------------------------------------------------------
    switch (evt.eventType)
    {
    // Damage / Combat
    case ANIM_EVT_APPLY_DAMAGE:
    case ANIM_EVT_APPLY_DAMAGE_TO_TARGET:
    case ANIM_EVT_DAMAGE_OBB_AREA:
    case ANIM_EVT_DAMAGE_OBB_AREA_LEFT_HAND:
    case ANIM_EVT_DAMAGE_OBB_AREA_RIGHT_HAND:
    case ANIM_EVT_DAMAGE_CYLINDER_AREA:
    case ANIM_EVT_DAMAGE_CYLINDER_AREA_RIGHT_HAND:
    case ANIM_EVT_FF_ONLY_DAMAGE_OBB_AREA:
    case ANIM_EVT_FF_DAMAGE_OBB_AREA_RIGHT_HAND:
    case ANIM_EVT_GRAB_OBB_AREA_LEFT_HAND:
    case ANIM_EVT_GRAB_OBB_AREA_RIGHT_HAND:
    case ANIM_EVT_NEW_HIT2:
    case ANIM_EVT_NEW_HIT3:
    case ANIM_EVT_NEW_HIT4:
    case ANIM_EVT_NEW_HIT5:
        handleDamageEvent(evt);
        break;

    case ANIM_EVT_CLEAR_HIT_LIST:
    case ANIM_EVT_FLYER_CLEAR_HIT_LIST:
    case ANIM_EVT_FLYER_DAMAGE_OBB_AREA:
    {
        // Log hit list clear for debug
        RendererLog("  [COMBAT] Cleared hit tracking list (prevents double-hits)");
        break;
    }

    // Trails / Visual FX
    case ANIM_EVT_TRAIL_ON_LEFT:
    case ANIM_EVT_TRAIL_ON_RIGHT:
    case ANIM_EVT_TRAIL_ON_RIGHT_SPLIT:
    case ANIM_EVT_TRAIL_OFF:
    case ANIM_EVT_TRAIL_OFF_LEFT:
    case ANIM_EVT_TRAIL_OFF_RIGHT:
        handleTrailEvent(evt);
        break;

    case ANIM_EVT_ACTIVATE_ATTACHED_PARTICLE_EFFECT:
    case ANIM_EVT_ACTIVATE_PARTICLE_EFFECT:
        handleParticleEvent(evt);
        break;

    // -----------------------------------------------------------------------
    // Sound — separated SoundCue and SoundEvent handling
    // -----------------------------------------------------------------------
    case ANIM_EVT_SOUND_CUE:
        handleSoundCueEvent(evt);
        break;

    case ANIM_EVT_SOUND_EVENT:
        handleSoundEvent(evt);
        break;
    case ANIM_EVT_SET_SOUND_OVERRIDE:
    {
        // vals[0].crcVal = switch group name, vals[1].crcVal = switch value
        if (evt.vals.size() >= 2
            && evt.vals[0].type == JsonAnimEventVal::EVT_CRC && evt.vals[0].crcVal[0]
            && evt.vals[1].type == JsonAnimEventVal::EVT_CRC && evt.vals[1].crcVal[0])
        {
            AudioManager::Get().SetSwitch(evt.vals[0].crcVal, evt.vals[1].crcVal,
                                          AudioManager::DEFAULT_GAME_OBJECT);
        }
        break;
    }

    // Camera
    case ANIM_EVT_CAMERA:
    case ANIM_EVT_CAMERA_EFFECT:
    case ANIM_EVT_CAMERA_EFFECT_ALWAYS:
        handleCameraEvent(evt);
        break;

    // State / Logic
    case ANIM_EVT_STATE_CHANGE1:
    case ANIM_EVT_STATE_CHANGE2:
    case ANIM_EVT_BEGIN_FACE_TARGET:
    case ANIM_EVT_END_FACE_TARGET:
    case ANIM_EVT_POST_GENERIC_EVENT:
    case ANIM_EVT_JUMP:
    case ANIM_EVT_DESTROY:
    case ANIM_EVT_SH_INVINCIBLE:
    case ANIM_EVT_SELECT_INVENTORY_LOADOUT:
    case ANIM_EVT_ENABLE_DRONE:
    case ANIM_EVT_ABILITY_ACTIVATION_EVENT:
    case ANIM_EVT_SET_ROTATION_RATE_SCALE:
    case ANIM_EVT_START_CHARGE:
    case ANIM_EVT_STOP_CHARGE:
        handleStateEvent(evt);
        break;

    // Projectile
    case ANIM_EVT_FIRE_PROJECTILE:
    case ANIM_EVT_FIRE_PROJECTILE_LEFT_HAND:
    case ANIM_EVT_FIRE_PROJECTILE_RIGHT_HAND:
    case ANIM_EVT_FIRE_PROJECTILE_VISUAL:
    case ANIM_EVT_FIRE_PROJECTILE_VISUAL_LEFT_HAND:
    case ANIM_EVT_READY_PROJECTILE:
    case ANIM_EVT_READY_PROJECTILE_LEFT_HAND:
    case ANIM_EVT_READY_PROJECTILE_RIGHT_HAND:
    case ANIM_EVT_UNREADY_PROJECTILE_LEFT_HAND:
    case ANIM_EVT_UNREADY_PROJECTILE_RIGHT_HAND:
    case ANIM_EVT_EMBED_LAUNCH_POINT:
        handleProjectileEvent(evt);
        break;

    // Throw
    case ANIM_EVT_THROW_LEFT_HAND:
    case ANIM_EVT_THROW_LEFT_TORQUE:
    case ANIM_EVT_THROW_RIGHT_HAND:
    case ANIM_EVT_THROW_RIGHT_TORQUE:
        handleThrowEvent(evt);
        break;

    // Bow
    case ANIM_EVT_GRAB_STRING_L:
    case ANIM_EVT_RELEASE_STRING_L:
        handleBowEvent(evt);
        break;

    // Controller
    case ANIM_EVT_RUMBLE_PLAY:
        handleControllerEvent(evt);
        break;

    case ANIM_EVT_UNKNOWN:
    default:
        // Unrecognized event — already logged above
        break;
    }

    if (m_animGraph && m_animGraph->valid)
    {
        TriggerGraphEventRecursive(*this, *m_animGraph, typeName);
    }
}

// ---------------------------------------------------------------------------
// SoundEvent handler — context-dependent generic audio categories
//
// SoundEvent structure (from 15,166 instances across 11,726 animation files):
//   vals[0] CRC = category name (e.g. "footstep_walk", "swing", "attack_vocal")
//   vals[1] CRC = bone attachment hint (e.g. "LeftHand", "FootLeftFront") — for
//                 3D spatial positioning.  Usually empty.
//   vals[2..8] Int = always zero (unused)
//
// Resolution: The category name is posted directly as a Wwise event.  Wwise
// resolves the actual sound variation through switch containers driven by the
// creature/weapon/material switches already set on the game object (via
// SetSoundOverride events or AudioManager::SetCreatureSwitch, etc.).
//
// Example flow for "footstep_walk" on an Orc character:
//   1. SetSoundOverride previously set creature switch to "orc"
//   2. PostEvent("footstep_walk") → Wwise selects orc footstep variation
//   3. If bone hint is "FootLeftFront", sound is positioned at that bone
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleSoundEvent(const JsonAnimEvent& evt)
{
    if (evt.vals.empty() || evt.vals[0].type != JsonAnimEventVal::EVT_CRC
        || evt.vals[0].crcVal[0] == '\0')
    {
        return;
    }

    const char* category = evt.vals[0].crcVal;  // e.g. "footstep_walk"

    // CRC[1] = bone name hint for 3D positioning (if present)
    const char* boneHint = NULL;
    if (evt.vals.size() > 1
        && evt.vals[1].type == JsonAnimEventVal::EVT_CRC
        && evt.vals[1].crcVal[0] != '\0')
    {
        boneHint = evt.vals[1].crcVal;
    }

    // Post the category as a Wwise event.
    // Wwise resolves the sound variation via creature/weapon/material switches.
    // In the viewer we use DEFAULT_GAME_OBJECT (single entity).
    // In-game, this would use the entity's registered game object ID, and
    // the bone hint would place the sound at the bone's world-space position.
    AkPlayingID pid = AudioManager::Get().PlayCue(
        category, AudioManager::DEFAULT_GAME_OBJECT);

    if (boneHint)
    {
        char dbg[256];
        sprintf_s(dbg, sizeof(dbg),
                  "  SoundEvent: category=\"%s\" bone=\"%s\" pid=%u",
                  category, boneHint, (unsigned)pid);
        RendererLog(dbg);
    }
}

// ---------------------------------------------------------------------------
// SoundCue handler — direct Wwise event names with optional switch modifiers
//
// SoundCue structure (from 7,378 instances across 11,726 animation files):
//   vals[0] CRC = primary Wwise event name (e.g. "Foley_Jump", "Hand_Grab",
//                 "Sword_Stab", "Troll_Attack", "fellbeast_wing_flap")
//   vals[1] CRC = optional modifier  → set as "anim_modifier" switch value
//                 before posting (e.g. "wing_flap", "Warg", "skid")
//   vals[2] CRC = optional sub-qualifier → set as "anim_sub" switch value
//                 before posting (e.g. "downstroke", "upstroke", "turn_180")
//   vals[3..8] Int = always zero (unused)
//
// The modifier/sub-qualifier switches tell Wwise which variation of the
// sound to play.  For example, "fellbeast_wing_flap" with modifier="wing_flap"
// and sub="downstroke" selects the downstroke wing flap variation.
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleSoundCueEvent(const JsonAnimEvent& evt)
{
    if (evt.vals.empty() || evt.vals[0].type != JsonAnimEventVal::EVT_CRC
        || evt.vals[0].crcVal[0] == '\0')
    {
        return;
    }

    const char* cueName = evt.vals[0].crcVal;  // e.g. "Foley_Jump"
    const AkGameObjectID gameObj = AudioManager::DEFAULT_GAME_OBJECT;

    // Set optional modifier switch (CRC[1]) before posting
    // This allows Wwise to select the correct variation through switch containers.
    if (evt.vals.size() > 1
        && evt.vals[1].type == JsonAnimEventVal::EVT_CRC
        && evt.vals[1].crcVal[0] != '\0')
    {
        AudioManager::Get().SetSwitch("anim_modifier", evt.vals[1].crcVal, gameObj);
    }

    // Set optional sub-qualifier switch (CRC[2]) before posting
    if (evt.vals.size() > 2
        && evt.vals[2].type == JsonAnimEventVal::EVT_CRC
        && evt.vals[2].crcVal[0] != '\0')
    {
        AudioManager::Get().SetSwitch("anim_sub", evt.vals[2].crcVal, gameObj);
    }

    // Post the primary event name directly to Wwise
    AkPlayingID pid = AudioManager::Get().PlayCue(cueName, gameObj);

    // Debug logging for compound cues
    if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_CRC
        && evt.vals[1].crcVal[0] != '\0')
    {
        char dbg[256];
        const char* sub = (evt.vals.size() > 2
                           && evt.vals[2].type == JsonAnimEventVal::EVT_CRC
                           && evt.vals[2].crcVal[0] != '\0')
                          ? evt.vals[2].crcVal : "";
        sprintf_s(dbg, sizeof(dbg),
                  "  SoundCue: event=\"%s\" modifier=\"%s\" sub=\"%s\" pid=%u",
                  cueName, evt.vals[1].crcVal, sub, (unsigned)pid);
        RendererLog(dbg);
    }
}

// ---------------------------------------------------------------------------
// Damage Event Handler - Handles all damage/combat events
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleDamageEvent(const JsonAnimEvent& evt)
{
    // Extract damage parameters
    float damageAmount = 1.0f;  // Default damage
    if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_FLOAT)
        damageAmount = evt.vals[0].floatVal;
    else if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_INT)
        damageAmount = (float)evt.vals[0].intVal;
    
    float radius = 2.0f;  // Default OBB/cylinder radius
    if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_FLOAT)
        radius = evt.vals[1].floatVal;
    else if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_INT)
        radius = (float)evt.vals[1].intVal;
    
    char dbg[256];
    const char* eventName = AnimEventTypeName(evt.eventType);
    sprintf_s(dbg, sizeof(dbg), "  [DAMAGE] %s: %.1f damage, radius=%.2f", 
              eventName, damageAmount, radius);
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // 1. Get attacker position from character root bone
    // 2. Create damage volume (OBB or cylinder) based on event type
    // 3. Query entities within volume
    // 4. Apply damage to valid targets (track hit list to prevent double-hits)
    // 5. Spawn damage number/hit reaction on targets
}

// ---------------------------------------------------------------------------
// Trail Event Handler - Handles weapon/effect trails
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleTrailEvent(const JsonAnimEvent& evt)
{
    const char* eventName = AnimEventTypeName(evt.eventType);
    bool isTrailOn = (evt.eventType >= ANIM_EVT_TRAIL_ON_LEFT && evt.eventType <= ANIM_EVT_TRAIL_ON_RIGHT_SPLIT);
    
    const char* trailName = "Default";  // Can be customized via event params
    if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_CRC && evt.vals[0].crcVal[0])
        trailName = evt.vals[0].crcVal;
    
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), "  [TRAIL] %s: %s trail '%s'", 
              eventName, isTrailOn ? "Enabled" : "Disabled", trailName);
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // 1. Get weapon bone (left/right hand, sword, etc.)
    // 2. If ON: Start trail rendering with color/texture from trailName
    // 3. If OFF: Stop trail rendering and flush particles
}

// ---------------------------------------------------------------------------
// Particle Event Handler - Handles VFX/particle effects
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleParticleEvent(const JsonAnimEvent& evt)
{
    const char* particleName = "default";  // Default particle system
    if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_CRC && evt.vals[0].crcVal[0])
        particleName = evt.vals[0].crcVal;
    
    const char* boneName = "root";  // Attachment bone
    if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_CRC && evt.vals[1].crcVal[0])
        boneName = evt.vals[1].crcVal;
    
    float scale = 1.0f;  // Effect scale
    if (evt.vals.size() > 2 && evt.vals[2].type == JsonAnimEventVal::EVT_FLOAT)
        scale = evt.vals[2].floatVal;
    else if (evt.vals.size() > 2 && evt.vals[2].type == JsonAnimEventVal::EVT_INT)
        scale = (float)evt.vals[2].intVal;
    
    bool attached = (evt.eventType == ANIM_EVT_ACTIVATE_ATTACHED_PARTICLE_EFFECT);
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), "  [PARTICLE] %s '%s' at bone '%s' scale=%.2f", 
              attached ? "Attached" : "World", particleName, boneName, scale);
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // 1. Load particle system template by name
    // 2. If attached: Get bone position/orientation, spawn and track effect
    // 3. If world: Spawn at character position
    // 4. Set effect scale/color/intensity from event parameters
    // 5. Let effect run to completion then destroy
}

// ---------------------------------------------------------------------------
// Camera Event Handler - Handles camera effects/shakes
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleCameraEvent(const JsonAnimEvent& evt)
{
    float intensity = 1.0f;
    if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_FLOAT)
        intensity = evt.vals[0].floatVal;
    else if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_INT)
        intensity = (float)evt.vals[0].intVal;
    
    float duration = 0.2f;
    if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_FLOAT)
        duration = evt.vals[1].floatVal;
    else if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_INT)
        duration = (float)evt.vals[1].intVal;
    
    const char* effectType = "shake";
    if (evt.vals.size() > 2 && evt.vals[2].type == JsonAnimEventVal::EVT_CRC && evt.vals[2].crcVal[0])
        effectType = evt.vals[2].crcVal;
    
    const char* eventName = AnimEventTypeName(evt.eventType);
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), "  [CAMERA] %s: intensity=%.2f duration=%.3fs type='%s'", 
              eventName, intensity, duration, effectType);
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // 1. Get current camera position/target
    // 2. Add shake/blur/zoom based on effectType
    // 3. Ramping function: intensity increases then fades over duration
}

// ---------------------------------------------------------------------------
// State Event Handler - Handles game state changes/logic
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleStateEvent(const JsonAnimEvent& evt)
{
    const char* stateValue = "";
    if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_CRC && evt.vals[0].crcVal[0])
        stateValue = evt.vals[0].crcVal;
    else if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_INT)
    {
        static char intBuf[32];
        sprintf_s(intBuf, sizeof(intBuf), "%d", evt.vals[0].intVal);
        stateValue = intBuf;
    }
    
    const char* eventName = AnimEventTypeName(evt.eventType);
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), "  [STATE] %s: value='%s'", eventName, stateValue);
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // ANIM_EVT_STATE_CHANGE1/2: Update character state machine
    // ANIM_EVT_BEGIN_FACE_TARGET: Lock character rotation to face target
    // ANIM_EVT_END_FACE_TARGET: Release rotation lock
    // ANIM_EVT_JUMP: Apply jump velocity impulse
    // ANIM_EVT_DESTROY: Mark character for removal
    // ANIM_EVT_SH_INVINCIBLE: Toggle invincibility frames
    // ANIM_EVT_ABILITY_ACTIVATION: Trigger special ability logic
    // ANIM_EVT_START_CHARGE/STOP_CHARGE: Begin/end ability charge-up
}

// ---------------------------------------------------------------------------
// Projectile Event Handler - Handles projectile firing/readying
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleProjectileEvent(const JsonAnimEvent& evt)
{
    const char* projectileClass = "arrow";
    if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_CRC && evt.vals[0].crcVal[0])
        projectileClass = evt.vals[0].crcVal;
    
    hkVector4 velocity(1, 0, 0, 0);  // Default forward direction
    if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_FLOAT)
        velocity(0) = evt.vals[1].floatVal;
    if (evt.vals.size() > 2 && evt.vals[2].type == JsonAnimEventVal::EVT_FLOAT)
        velocity(1) = evt.vals[2].floatVal;
    if (evt.vals.size() > 3 && evt.vals[3].type == JsonAnimEventVal::EVT_FLOAT)
        velocity(2) = evt.vals[3].floatVal;
    
    velocity.normalize3();
    {
        hkVector4 scaled;
        scaled.setMul4(25.0f, velocity);
        velocity = scaled;  // Default projectile speed
    }
    
    const char* eventName = AnimEventTypeName(evt.eventType);
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), 
              "  [PROJECTILE] %s: class='%s' velocity=(%.1f,%.1f,%.1f)", 
              eventName, projectileClass, velocity(0), velocity(1), velocity(2));
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // FIRE_PROJECTILE variants: Instantiate projectile, apply velocity, add gravity
    // READY_PROJECTILE variants: Draw/nock projectile, play draw animation
    // UNREADY_PROJECTILE variants: Sheath projectile
    // EMBED_LAUNCH_POINT: Set projectile launch offset for weapon
}

// ---------------------------------------------------------------------------
// Throw Event Handler - Handles throw actions with torque
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleThrowEvent(const JsonAnimEvent& evt)
{
    hkVector4 throwVel(0, 1, 0, 0);
    if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_FLOAT)
        throwVel(0) = evt.vals[0].floatVal;
    if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_FLOAT)
        throwVel(1) = evt.vals[1].floatVal;
    if (evt.vals.size() > 2 && evt.vals[2].type == JsonAnimEventVal::EVT_FLOAT)
        throwVel(2) = evt.vals[2].floatVal;
    
    float torque = 0.0f;
    if (evt.vals.size() > 3 && evt.vals[3].type == JsonAnimEventVal::EVT_FLOAT)
        torque = evt.vals[3].floatVal;
    
    bool hasTorque = (evt.eventType == ANIM_EVT_THROW_LEFT_TORQUE || 
                      evt.eventType == ANIM_EVT_THROW_RIGHT_TORQUE);
    
    const char* eventName = AnimEventTypeName(evt.eventType);
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), 
              "  [THROW] %s: velocity=(%.1f,%.1f,%.1f) torque=%.1f", 
              eventName, throwVel(0), throwVel(1), throwVel(2), torque);
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // 1. Get character hand bone position (left/right based on event type)
    // 2. Create throwable object (stone, spear, fireball, etc.)
    // 3. Set velocity and apply torque rotation if requested
    // 4. Add gravity and collision to projectile
    // 5. Handle impact damage when projectile hits something
}

// ---------------------------------------------------------------------------
// Bow Event Handler - Handles bow string animations
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleBowEvent(const JsonAnimEvent& evt)
{
    bool isGrab = (evt.eventType == ANIM_EVT_GRAB_STRING_L);
    
    const char* eventName = AnimEventTypeName(evt.eventType);
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), "  [BOW] %s: %s", 
              eventName, isGrab ? "Nocking string" : "Releasing shot");
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // GRAB_STRING_L: Snap bowstring back (load animation)
    // RELEASE_STRING_L: Release arrow and play bowstring snap forward
    // Coordinate with READY/UNREADY_PROJECTILE events for full bow animation
}

// ---------------------------------------------------------------------------
// Controller Event Handler - Handles rumble/haptic feedback
// ---------------------------------------------------------------------------
void Scene3DRenderer::handleControllerEvent(const JsonAnimEvent& evt)
{
    float strength = 1.0f;
    if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_FLOAT)
        strength = evt.vals[0].floatVal;
    else if (evt.vals.size() > 0 && evt.vals[0].type == JsonAnimEventVal::EVT_INT)
        strength = (float)evt.vals[0].intVal;
    
    float duration = 0.1f;
    if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_FLOAT)
        duration = evt.vals[1].floatVal;
    else if (evt.vals.size() > 1 && evt.vals[1].type == JsonAnimEventVal::EVT_INT)
        duration = (float)evt.vals[1].intVal;
    
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), "  [CONTROLLER] RUMBLE_PLAY: strength=%.2f duration=%.3fs", 
              strength, duration);
    RendererLog(dbg);
    
    // TODO: In-game integration:
    // 1. Query for connected game controller (XInput API or similar)
    // 2. Set vibration motors to strength * 65535 (max value)
    // 3. Schedule vibration stop after duration milliseconds
    // 4. Support dual-motor (left/right) control for directional feedback
}

// ---------------------------------------------------------------------------
// Reset event deduplication state — call on animation change, seek, or
// when a full loop cycle completes.
// ---------------------------------------------------------------------------
void Scene3DRenderer::resetEventDedupForClip(unsigned int clipKey)
{
    if (m_firedEventIndices.empty())
        return;
    std::set<unsigned long long>::iterator it = m_firedEventIndices.begin();
    while (it != m_firedEventIndices.end())
    {
        unsigned long long key = *it;
        unsigned int k = (unsigned int)(key >> 32);
        if (k == clipKey)
        {
            std::set<unsigned long long>::iterator kill = it;
            ++it;
            m_firedEventIndices.erase(kill);
        }
        else
        {
            ++it;
        }
    }
}

void Scene3DRenderer::resetEventDedup()
{
    m_firedEventIndices.clear();
}

void Scene3DRenderer::queueGraphOneShotTrigger(AnimationGraphRuntime* graph, int paramIndex)
{
    if (!graph || paramIndex < 0)
        return;
    GraphTriggerRef ref;
    ref.graph = graph;
    ref.paramIndex = paramIndex;
    m_graphOneShotTriggers.push_back(ref);
}

void Scene3DRenderer::clearGraphOneShotTriggers()
{
    if (m_graphOneShotTriggers.empty())
        return;
    for (size_t i = 0; i < m_graphOneShotTriggers.size(); ++i)
    {
        GraphTriggerRef& ref = m_graphOneShotTriggers[i];
        if (!ref.graph || ref.paramIndex < 0)
            continue;
        std::vector<GraphParam>& params = GetGraphParams(*ref.graph);
        if (ref.paramIndex >= (int)params.size())
            continue;
        GraphParam& p = params[ref.paramIndex];
        if (p.type == Scene3DRenderer::GRAPH_PARAM_TRIGGER)
            p.triggered = false;
    }
    m_graphOneShotTriggers.clear();
}

// ---------------------------------------------------------------------------
// Motion matching (Phase 9)
// ---------------------------------------------------------------------------

static void NormalizeXZ(float& x, float& z)
{
    float len = sqrtf(x * x + z * z);
    if (len > 1e-6f)
    {
        x /= len;
        z /= len;
    }
    else
    {
        x = 0.0f;
        z = 1.0f;
    }
}

static void CollectClipsFromGraph(AnimationGraphRuntime* graph, std::set<JsonAnimClip*>& out);

static void CollectClipsFromBlendGraph(BlendGraphRuntime* bg, std::set<JsonAnimClip*>& out)
{
    if (!bg) return;
    for (int i = 0; i < (int)bg->nodes.size(); ++i)
    {
        BlendGraphNode& node = bg->nodes[i];
        if (node.clip) out.insert(node.clip);
        if (node.subgraph) CollectClipsFromBlendGraph(node.subgraph, out);
        if (node.stateMachine)
        {
            CollectClipsFromGraph(node.stateMachine, out);
        }
    }
}

static void CollectClipsFromGraph(AnimationGraphRuntime* graph, std::set<JsonAnimClip*>& out)
{
    if (!graph) return;
    for (int i = 0; i < (int)graph->states.size(); ++i)
    {
        GraphState& st = graph->states[i];
        if (st.clip) out.insert(st.clip);
        if (st.blendGraph) CollectClipsFromBlendGraph(st.blendGraph, out);
        if (st.subMachine) CollectClipsFromGraph(st.subMachine, out);
    }
    for (int i = 0; i < (int)graph->blendGraphs.size(); ++i)
    {
        CollectClipsFromBlendGraph(graph->blendGraphs[i], out);
    }
    for (int i = 0; i < (int)graph->subMachines.size(); ++i)
    {
        CollectClipsFromGraph(graph->subMachines[i], out);
    }
}

bool Scene3DRenderer::getMotionMatchEnabled() const
{
    return m_motionMatchEnabled;
}

void Scene3DRenderer::setMotionMatchEnabled(bool enabled)
{
    m_motionMatchEnabled = enabled ? true : false;
    if (m_motionMatchEnabled && !m_motionMatchDatabaseValid)
    {
        rebuildMotionMatchDatabase();
    }
}

bool Scene3DRenderer::rebuildMotionMatchDatabase()
{
    if (!m_gameModel || !m_gameModel->skeleton)
        return false;
    if (m_animGraph && m_animGraph->valid)
    {
        return buildMotionMatchDatabaseFromGraph();
    }
    if (m_jsonAnim)
    {
        std::vector<JsonAnimClip*> clips;
        clips.push_back(m_jsonAnim);
        return buildMotionMatchDatabaseFromClips(clips);
    }
    return false;
}

void Scene3DRenderer::clearMotionMatchDatabase()
{
    for (size_t i = 0; i < m_motionMatchOwnedClips.size(); ++i)
    {
        delete m_motionMatchOwnedClips[i];
    }
    m_motionMatchOwnedClips.clear();
    m_motionMatchDb.frames.clear();
    m_motionMatchDb.clips.clear();
    m_motionMatchDb.valid = false;
    m_motionMatchDatabaseValid = false;
    m_motionMatchCurrentFrameIndex = -1;
    m_motionMatchCurrentClipIndex = -1;
    m_motionMatchTime = 0.0f;
    m_motionMatchPrevTime = 0.0f;
    m_motionMatchBlendTime = 0.0f;
    m_motionMatchBlendActive = false;
    m_motionMatchRequestBlend = false;
    m_motionMatchHasLastPose = false;
    m_motionMatchHasLastFeature = false;
    memset(&m_motionMatchLastFeature, 0, sizeof(m_motionMatchLastFeature));
}

void Scene3DRenderer::setMotionMatchTargetVelocity(float x, float z)
{
    m_motionMatchTargetVel.set(x, 0.0f, z, 0.0f);
}

void Scene3DRenderer::setMotionMatchTargetFacing(float x, float z)
{
    m_motionMatchTargetFacing.set(x, 0.0f, z, 0.0f);
}

void Scene3DRenderer::getMotionMatchTargetVelocity(float& x, float& z) const
{
    x = m_motionMatchTargetVel(0);
    z = m_motionMatchTargetVel(2);
}

void Scene3DRenderer::getMotionMatchTargetFacing(float& x, float& z) const
{
    x = m_motionMatchTargetFacing(0);
    z = m_motionMatchTargetFacing(2);
}

void Scene3DRenderer::setMotionMatchSearchInterval(float seconds)
{
    if (seconds < 0.01f) seconds = 0.01f;
    if (seconds > 2.0f) seconds = 2.0f;
    m_motionMatchSearchInterval = seconds;
}

float Scene3DRenderer::getMotionMatchSearchInterval() const
{
    return m_motionMatchSearchInterval;
}

void Scene3DRenderer::setMotionMatchBlendDuration(float seconds)
{
    if (seconds < 0.0f) seconds = 0.0f;
    if (seconds > 1.0f) seconds = 1.0f;
    m_motionMatchBlendDuration = seconds;
}

float Scene3DRenderer::getMotionMatchBlendDuration() const
{
    return m_motionMatchBlendDuration;
}

int Scene3DRenderer::getMotionMatchFrameCount() const
{
    return (int)m_motionMatchDb.frames.size();
}

int Scene3DRenderer::getMotionMatchClipCount() const
{
    return (int)m_motionMatchDb.clips.size();
}

float Scene3DRenderer::getMotionMatchLastScore() const
{
    return m_motionMatchLastScore;
}

const char* Scene3DRenderer::getMotionMatchCurrentClipKey() const
{
    if (m_motionMatchCurrentClipIndex >= 0 &&
        m_motionMatchCurrentClipIndex < (int)m_motionMatchDb.clips.size())
    {
        JsonAnimClip* clip = m_motionMatchDb.clips[m_motionMatchCurrentClipIndex].clip;
        if (clip && clip->info.key[0] != '\0')
            return clip->info.key;
    }
    return "";
}

/* Animation States Translator (Phase 2) */
const LuaAnimStatesTranslatorInfo& Scene3DRenderer::getAnimStatesTranslatorInfo() const
{
    return m_animStatesTranslator;
}

void Scene3DRenderer::setAnimStatesTranslator(const LuaAnimStatesTranslatorInfo& info)
{
    m_animStatesTranslator = info;
}

void Scene3DRenderer::setAnimStatesTranslatorSelectedStateIndex(int index)
{
    if (index >= -1 && index < (int)m_animStatesTranslator.states.size())
    {
        m_animStatesTranslator.selectedStateIndex = index;
    }
}

int Scene3DRenderer::getAnimStatesTranslatorSelectedStateIndex() const
{
    return m_animStatesTranslator.selectedStateIndex;
}

const LuaAnimStateEntry* Scene3DRenderer::getAnimStatesTranslatorActiveState() const
{
    int idx = m_animStatesTranslator.selectedStateIndex;
    if (idx >= 0 && idx < (int)m_animStatesTranslator.states.size())
    {
        return &m_animStatesTranslator.states[idx];
    }
    return NULL;
}

/* ASM Parity Toggles (Phase 2.5) */
bool Scene3DRenderer::getAnimationDrivenEnabled() const
{
    return m_useAnimationDriven;
}

void Scene3DRenderer::setAnimationDrivenEnabled(bool enabled)
{
    m_useAnimationDriven = enabled;
}

// NOTE: getRootMotionWarpEnabled / setRootMotionWarpEnabled defined earlier (root motion section)

Scene3DRenderer::AnimDrivenMode Scene3DRenderer::getAnimDrivenMode() const
{
    return m_animDrivenMode;
}

void Scene3DRenderer::setAnimDrivenMode(AnimDrivenMode mode)
{
    m_animDrivenMode = mode;
}

/* AnimTable & State→Clip Resolution (Phase 3) */
const LuaAnimTableInfo& Scene3DRenderer::getAnimTableInfo() const
{
    return m_animTable;
}

void Scene3DRenderer::setAnimTable(const LuaAnimTableInfo& info)
{
    m_animTable = info;
}

std::vector<std::string> Scene3DRenderer::resolveStateToClips(const char* stateKey) const
{
    std::vector<std::string> result;
    if (!stateKey || !stateKey[0])
        return result;

    /* Search AnimTable entries for state key */
    for (size_t i = 0; i < m_animTable.entries.size(); ++i)
    {
        const LuaAnimTableEntry& entry = m_animTable.entries[i];
        /* Match if entry key matches state key exactly or contains it */
        if (entry.key == stateKey || 
            (entry.key.find(stateKey) != std::string::npos && 
             entry.key.find("_") != std::string::npos))  /* Allow partial match with underscore */
        {
            for (size_t j = 0; j < entry.clips.size(); ++j)
            {
                result.push_back(entry.clips[j]);
            }
        }
    }
    return result;
}

int Scene3DRenderer::getResolvedClipsForActiveStateCount() const
{
    return (int)m_resolvedClipsForActiveState.size();
}

const char* Scene3DRenderer::getResolvedClipsForActiveState(int clipIndex) const
{
    if (clipIndex >= 0 && clipIndex < (int)m_resolvedClipsForActiveState.size())
    {
        return m_resolvedClipsForActiveState[clipIndex].c_str();
    }
    return "";
}

bool Scene3DRenderer::loadResolvedClip(int clipIndex)
{
    if (clipIndex >= 0 && clipIndex < (int)m_resolvedClipsForActiveState.size())
    {
        setJsonAnimationPath(m_resolvedClipsForActiveState[clipIndex].c_str());
        return true;
    }
    return false;
}

/* Helper: case-insensitive substring match against a list of filter keywords */
static bool MatchesFilterKeyword(const std::string& key, const std::vector<std::string>& filters)
{
    if (filters.empty())
        return true;  /* No filter = match all */

    for (size_t fi = 0; fi < filters.size(); ++fi)
    {
        if (filters[fi].empty())
            continue;
        /* Case-insensitive substring match */
        std::string lowerKey = key;
        std::string lowerFilter = filters[fi];
        for (size_t ci = 0; ci < lowerKey.size(); ++ci)
            lowerKey[ci] = (char)tolower((unsigned char)lowerKey[ci]);
        for (size_t ci = 0; ci < lowerFilter.size(); ++ci)
            lowerFilter[ci] = (char)tolower((unsigned char)lowerFilter[ci]);

        if (lowerKey.find(lowerFilter) != std::string::npos)
            return true;
    }
    return false;
}

/* Filter Evaluation (Phase 4) */
std::vector<std::string> Scene3DRenderer::resolveStateToClipsWithFilters(
    const char* stateKey,
    const std::vector<std::string>& stancesFilter,
    const std::vector<std::string>& actionsFilter) const
{
    std::vector<std::string> result;
    if (!stateKey || !stateKey[0])
        return result;

    /* Search AnimTable entries matching state key AND filters */
    for (size_t i = 0; i < m_animTable.entries.size(); ++i)
    {
        const LuaAnimTableEntry& entry = m_animTable.entries[i];

        /* Check state key match (as in Phase 3) */
        bool keyMatch = (entry.key == stateKey ||
            (entry.key.find(stateKey) != std::string::npos &&
             entry.key.find("_") != std::string::npos));

        if (!keyMatch)
            continue;

        /* Check filter match */
        bool stanceMatch = MatchesFilterKeyword(entry.key, stancesFilter);
        bool actionMatch = MatchesFilterKeyword(entry.key, actionsFilter);

        /* Both filters must match (AND logic) */
        if (stanceMatch && actionMatch)
        {
            for (size_t j = 0; j < entry.clips.size(); ++j)
            {
                result.push_back(entry.clips[j]);
            }
        }
    }
    return result;
}

std::vector<std::string> Scene3DRenderer::getFilteredClipsForActiveState() const
{
    return m_filteredClipsForActiveState;
}

int Scene3DRenderer::getFilteredClipsForActiveStateCount() const
{
    return (int)m_filteredClipsForActiveState.size();
}

const char* Scene3DRenderer::getFilteredClipsForActiveState(int clipIndex) const
{
    if (clipIndex >= 0 && clipIndex < (int)m_filteredClipsForActiveState.size())
    {
        return m_filteredClipsForActiveState[clipIndex].c_str();
    }
    return "";
}

void Scene3DRenderer::applyFiltersToActiveState()
{
    const LuaAnimStateEntry* activeState = getAnimStatesTranslatorActiveState();
    if (!activeState)
    {
        m_filteredClipsForActiveState.clear();
        return;
    }

    /* Get filtered clips using state's stance/action filters */
    m_filteredClipsForActiveState = resolveStateToClipsWithFilters(
        activeState->key.c_str(),
        activeState->stances,
        activeState->actions
    );
    
    /* Auto-load first filtered clip if available */
    if (!m_filteredClipsForActiveState.empty())
    {
        setJsonAnimationPath(m_filteredClipsForActiveState[0].c_str());
    }
}

/* =========================================================================
   Phase 5: Graph State Machine Integration
   ========================================================================= */

bool Scene3DRenderer::findGraphStateMatchingAnimState(const char* animStateKey, int& outGraphStateIndex) const
{
    outGraphStateIndex = -1;
    
    if (!animStateKey || !animStateKey[0] || !m_animGraph || !m_animGraph->valid)
        return false;
    
    /* Search for exact and substring matches in graph states */
    int bestIdx = -1;
    bool exactMatch = false;
    
    for (int i = 0; i < (int)m_animGraph->states.size(); ++i)
    {
        const GraphState& graphState = m_animGraph->states[i];
        
        /* Exact match (best case) */
        if (_stricmp(graphState.name.c_str(), animStateKey) == 0)
        {
            outGraphStateIndex = i;
            return true;
        }
        
        /* Substring match (fallback): anim state key contained in graph state name */
        if (!exactMatch && graphState.name.find(animStateKey) != std::string::npos)
        {
            bestIdx = i;
            exactMatch = false;
        }
    }
    
    if (bestIdx >= 0)
    {
        outGraphStateIndex = bestIdx;
        return true;
    }
    
    return false;
}

void Scene3DRenderer::transitionGraphToState(int graphStateIndex)
{
    if (!m_animGraph || !m_animGraph->valid)
        return;
    
    if (graphStateIndex < 0 || graphStateIndex >= (int)m_animGraph->states.size())
        return;
    
    int prevState = m_animGraph->currentState;
    int prevNext = m_animGraph->nextState;
    bool wasInTransition = m_animGraph->inTransition;

    /* Check if same state (no transition needed) */
    if (!wasInTransition && prevState == graphStateIndex)
        return;

    if (wasInTransition && prevNext >= 0 && prevNext != graphStateIndex)
        HandleGraphStateExit(*this, *m_animGraph, prevNext);
    if (prevState != graphStateIndex)
        HandleGraphStateExit(*this, *m_animGraph, prevState);

    if (prevState != graphStateIndex && !(wasInTransition && prevNext == graphStateIndex))
        HandleGraphStateEnter(*this, *m_animGraph, graphStateIndex);

    /* Execute transition: set currentState directly (no transition blend) */
    m_animGraph->currentState = graphStateIndex;
    m_animGraph->stateTime = 0.0f;
    m_animGraph->nextState = -1;
    m_animGraph->activeTransition = -1;
    m_animGraph->inTransition = false;
    m_animGraph->transitionTime = 0.0f;
    m_animGraph->transitionDuration = 0.0f;
    m_animGraph->lastEventTime = 0.0f;
    m_animGraph->lastNextEventTime = 0.0f;
    m_isTransitioningToAnimState = false;
    resetEventDedup();
    
    char msg[256];
    sprintf_s(msg, "Phase 5: Transitioned graph to state '%s'",
              m_animGraph->states[graphStateIndex].name.c_str());
    RendererLog(msg);
}

const char* Scene3DRenderer::getGraphStateNameForActiveAnimState() const
{
    if (m_graphStateForActiveAnimState < 0 || !m_animGraph || !m_animGraph->valid)
        return NULL;
    
    if (m_graphStateForActiveAnimState >= (int)m_animGraph->states.size())
        return NULL;
    
    return m_animGraph->states[m_graphStateForActiveAnimState].name.c_str();
}

bool Scene3DRenderer::isGraphTransitioningToAnimState() const
{
    return m_isTransitioningToAnimState;
}

int Scene3DRenderer::getActiveGraphStateForAnimState() const
{
    return m_graphStateForActiveAnimState;
}

void Scene3DRenderer::updateGraphStateForActiveAnimState()
{
    m_graphStateForActiveAnimState = -1;
    m_isTransitioningToAnimState = false;
    
    const LuaAnimStateEntry* activeState = getAnimStatesTranslatorActiveState();
    if (!activeState || !m_animGraph || !m_animGraph->valid)
        return;
    
    /* Find matching graph state for active animation state */
    int graphStateIdx = -1;
    if (findGraphStateMatchingAnimState(activeState->key.c_str(), graphStateIdx))
    {
        m_graphStateForActiveAnimState = graphStateIdx;
        m_isTransitioningToAnimState = false;
    }
}

/* ==================== PHASE 6: MOTION MATCHING BRIDGE ==================== */

JsonAnimClip* Scene3DRenderer::getJsonAnimClipByKey(const char* key)
{
    if (!key || key[0] == '\0')
        return NULL;

    // Check primary clip
    if (m_jsonAnim && m_jsonAnim->info.key[0] != '\0' && strcmp(m_jsonAnim->info.key, key) == 0)
        return m_jsonAnim;

    // Check blend clip
    if (m_jsonBlendAnim && m_jsonBlendAnim->info.key[0] != '\0' && strcmp(m_jsonBlendAnim->info.key, key) == 0)
        return m_jsonBlendAnim;

    // Check animation graph state clips
    if (m_animGraph)
    {
        for (int i = 0; i < (int)m_animGraph->states.size(); ++i)
        {
            JsonAnimClip* clip = m_animGraph->states[i].clip;
            if (clip && clip->info.key[0] != '\0' && strcmp(clip->info.key, key) == 0)
                return clip;
        }
    }

    // Check motion match owned clips
    for (size_t i = 0; i < m_motionMatchOwnedClips.size(); ++i)
    {
        JsonAnimClip* clip = m_motionMatchOwnedClips[i];
        if (clip && clip->info.key[0] != '\0' && strcmp(clip->info.key, key) == 0)
            return clip;
    }

    return NULL;
}

bool Scene3DRenderer::rebuildMotionMatchDatabaseFromFilteredClips(const std::vector<std::string>& filteredClipKeys)
{
    if (filteredClipKeys.empty())
    {
        m_motionMatchFilteredDatabaseValid = false;
        m_motionMatchFilteredClipCount = 0;
        return false;
    }
    
    /* Build list of JsonAnimClip* from filtered clip keys */
    std::vector<JsonAnimClip*> filteredClips;
    for (size_t ki = 0; ki < filteredClipKeys.size(); ++ki)
    {
        JsonAnimClip* clip = getJsonAnimClipByKey(filteredClipKeys[ki].c_str());
        if (clip)
            filteredClips.push_back(clip);
    }
    
    if (filteredClips.empty())
    {
        m_motionMatchFilteredDatabaseValid = false;
        m_motionMatchFilteredClipCount = 0;
        return false;
    }
    
    /* Use existing motion match DB building but only from filtered clips */
    m_motionMatchFilteredClipCount = (int)filteredClips.size();
    
    /* If motion matching from filtered clips is enabled, rebuild the main DB */
    if (m_motionMatchUseFilteredClips)
    {
        bool success = buildMotionMatchDatabaseFromClips(filteredClips);
        m_motionMatchFilteredDatabaseValid = success;
        return success;
    }
    
    m_motionMatchFilteredDatabaseValid = true;
    return true;
}

void Scene3DRenderer::setMotionMatchUseFilteredClips(bool enabled)
{
    m_motionMatchUseFilteredClips = enabled;
    
    /* If disabling, restore full database */
    if (!enabled && m_jsonAnim)
    {
        rebuildMotionMatchDatabase();
    }
}

bool Scene3DRenderer::getMotionMatchUseFilteredClips() const
{
    return m_motionMatchUseFilteredClips;
}

int Scene3DRenderer::getMotionMatchFilteredClipCount() const
{
    return m_motionMatchFilteredClipCount;
}

bool Scene3DRenderer::buildMotionMatchDatabaseFromGraph()
{
    if (!m_animGraph || !m_animGraph->valid)
        return false;
    std::set<JsonAnimClip*> unique;
    CollectClipsFromGraph(m_animGraph, unique);
    std::vector<JsonAnimClip*> clips;
    for (std::set<JsonAnimClip*>::iterator it = unique.begin(); it != unique.end(); ++it)
    {
        clips.push_back(*it);
    }
    if (clips.empty() && m_jsonAnim)
        clips.push_back(m_jsonAnim);
    return buildMotionMatchDatabaseFromClips(clips);
}

bool Scene3DRenderer::buildMotionMatchDatabaseFromClips(const std::vector<JsonAnimClip*>& clips)
{
    if (!m_gameModel || !m_gameModel->skeleton)
        return false;

    clearMotionMatchDatabase();
    if (clips.empty())
        return false;

    const hkaSkeleton* skeleton = m_gameModel->skeleton;
    const char* leftFootAliases[] = { "LeftFoot", "Left_Foot", "L_Foot", "Foot_L", "foot_l", "Bip01 L Foot", "Bip01 L Toe0", "LeftToe", "Toe_L" };
    const char* rightFootAliases[] = { "RightFoot", "Right_Foot", "R_Foot", "Foot_R", "foot_r", "Bip01 R Foot", "Bip01 R Toe0", "RightToe", "Toe_R" };

    int rootIdx = 0;
    if (clips[0] && clips[0]->rootBoneIndex >= 0 && clips[0]->rootBoneIndex < skeleton->m_numBones)
        rootIdx = clips[0]->rootBoneIndex;
    int leftFoot = FindFirstBoneByAliases(skeleton, leftFootAliases, (int)(sizeof(leftFootAliases) / sizeof(leftFootAliases[0])));
    int rightFoot = FindFirstBoneByAliases(skeleton, rightFootAliases, (int)(sizeof(rightFootAliases) / sizeof(rightFootAliases[0])));

    m_motionMatchDb.rootBoneIndex = rootIdx;
    m_motionMatchDb.leftFootBoneIndex = leftFoot;
    m_motionMatchDb.rightFootBoneIndex = rightFoot;

    bool prevWarp = m_timeWarpEnabled;
    m_timeWarpEnabled = false;

    for (size_t ci = 0; ci < clips.size(); ++ci)
    {
        JsonAnimClip* clip = clips[ci];
        if (!clip)
            continue;

        float frameTime = (clip->frameTime > 0.0f) ? clip->frameTime : (1.0f / 30.0f);
        float duration = GetClipDuration(clip);
        int frameCount = clip->frameCount;
        if (frameCount <= 0)
        {
            if (frameTime <= 0.0f) frameTime = 1.0f / 30.0f;
            frameCount = (duration > 0.0f) ? (int)(duration / frameTime + 0.5f) + 1 : 1;
        }
        if (frameCount < 1)
            continue;

        MotionMatchClipInfo info;
        info.clip = clip;
        info.startIndex = (int)m_motionMatchDb.frames.size();
        info.frameCount = frameCount;
        info.frameTime = frameTime;
        info.duration = duration;
        m_motionMatchDb.clips.push_back(info);
        int clipIndex = (int)m_motionMatchDb.clips.size() - 1;

        hkArray<hkVector4> rootPos;
        hkArray<hkVector4> faceDir;
        hkArray<hkVector4> leftPos;
        hkArray<hkVector4> rightPos;
        rootPos.setSize(frameCount);
        faceDir.setSize(frameCount);
        if (leftFoot >= 0) leftPos.setSize(frameCount);
        if (rightFoot >= 0) rightPos.setSize(frameCount);

        hkaPose pose(skeleton);
        hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();

        for (int f = 0; f < frameCount; ++f)
        {
            float t = frameTime * (float)f;
            pose.setToReferencePose();
            applyJsonClipToLocalPose(clip, local, t);
            pose.syncModelSpace();

            const hkQsTransform& root = pose.getBoneModelSpace(rootIdx);
            rootPos[f] = root.getTranslation();

            hkVector4 base;
            base.set(0.0f, 0.0f, 1.0f, 0.0f);
            faceDir[f] = RotateVectorByQuat(root.getRotation(), base);

            if (leftFoot >= 0)
                leftPos[f] = pose.getBoneModelSpace(leftFoot).getTranslation();
            if (rightFoot >= 0)
                rightPos[f] = pose.getBoneModelSpace(rightFoot).getTranslation();
        }

        int off1 = (int)(m_motionMatchTrajT1 / frameTime + 0.5f);
        int off2 = (int)(m_motionMatchTrajT2 / frameTime + 0.5f);
        int off3 = (int)(m_motionMatchTrajT3 / frameTime + 0.5f);
        if (off1 < 1) off1 = 1;
        if (off2 < 1) off2 = 1;
        if (off3 < 1) off3 = 1;

        for (int f = 0; f < frameCount; ++f)
        {
            int prev = (f > 0) ? (f - 1) : f;
            int next = (f < frameCount - 1) ? (f + 1) : f;
            float dt = (float)(next - prev) * frameTime;
            hkVector4 vel;
            vel.setZero4();
            if (dt > 1e-6f)
            {
                hkVector4 diff;
                diff.setSub4(rootPos[next], rootPos[prev]);
                vel.setMul4(1.0f / dt, diff);
            }

            MotionMatchFeature feat;
            feat.rootPosX = rootPos[f](0);
            feat.rootPosZ = rootPos[f](2);
            feat.rootVelX = vel(0);
            feat.rootVelZ = vel(2);

            float fx = faceDir[f](0);
            float fz = faceDir[f](2);
            NormalizeXZ(fx, fz);
            feat.facingX = fx;
            feat.facingZ = fz;

            if (leftFoot >= 0)
            {
                hkVector4 rel;
                rel.setSub4(leftPos[f], rootPos[f]);
                feat.leftFootX = rel(0);
                feat.leftFootY = rel(1);
                feat.leftFootZ = rel(2);
            }
            else
            {
                feat.leftFootX = feat.leftFootY = feat.leftFootZ = 0.0f;
            }

            if (rightFoot >= 0)
            {
                hkVector4 rel;
                rel.setSub4(rightPos[f], rootPos[f]);
                feat.rightFootX = rel(0);
                feat.rightFootY = rel(1);
                feat.rightFootZ = rel(2);
            }
            else
            {
                feat.rightFootX = feat.rightFootY = feat.rightFootZ = 0.0f;
            }

            int i1 = f + off1; if (i1 >= frameCount) i1 = frameCount - 1;
            int i2 = f + off2; if (i2 >= frameCount) i2 = frameCount - 1;
            int i3 = f + off3; if (i3 >= frameCount) i3 = frameCount - 1;

            hkVector4 d1; d1.setSub4(rootPos[i1], rootPos[f]);
            hkVector4 d2; d2.setSub4(rootPos[i2], rootPos[f]);
            hkVector4 d3; d3.setSub4(rootPos[i3], rootPos[f]);
            feat.trajX1 = d1(0); feat.trajZ1 = d1(2);
            feat.trajX2 = d2(0); feat.trajZ2 = d2(2);
            feat.trajX3 = d3(0); feat.trajZ3 = d3(2);

            MotionMatchFrame fr;
            fr.clip = clip;
            fr.clipIndex = clipIndex;
            fr.frameIndex = f;
            fr.time = frameTime * (float)f;
            fr.feature = feat;
            m_motionMatchDb.frames.push_back(fr);
        }
    }

    m_timeWarpEnabled = prevWarp;

    m_motionMatchDb.valid = !m_motionMatchDb.frames.empty();
    m_motionMatchDatabaseValid = m_motionMatchDb.valid;
    if (m_motionMatchDb.valid)
    {
        m_motionMatchCurrentClipIndex = 0;
        m_motionMatchCurrentFrameIndex = m_motionMatchDb.clips[0].startIndex;
        m_motionMatchTime = 0.0f;
        m_motionMatchPrevTime = 0.0f;
        m_motionMatchSearchTimer = 0.0f;
    }
    return m_motionMatchDb.valid;
}

void Scene3DRenderer::buildMotionMatchQuery(MotionMatchFeature& outQuery) const
{
    if (m_motionMatchHasLastFeature)
    {
        outQuery = m_motionMatchLastFeature;
    }
    else
    {
        memset(&outQuery, 0, sizeof(outQuery));
        outQuery.facingX = 0.0f;
        outQuery.facingZ = 1.0f;
    }

    float tvx = m_motionMatchTargetVel(0);
    float tvz = m_motionMatchTargetVel(2);
    if (fabsf(tvx) > 1e-4f || fabsf(tvz) > 1e-4f)
    {
        outQuery.rootVelX = tvx;
        outQuery.rootVelZ = tvz;
        outQuery.trajX1 = tvx * m_motionMatchTrajT1;
        outQuery.trajZ1 = tvz * m_motionMatchTrajT1;
        outQuery.trajX2 = tvx * m_motionMatchTrajT2;
        outQuery.trajZ2 = tvz * m_motionMatchTrajT2;
        outQuery.trajX3 = tvx * m_motionMatchTrajT3;
        outQuery.trajZ3 = tvz * m_motionMatchTrajT3;
    }

    float fx = m_motionMatchTargetFacing(0);
    float fz = m_motionMatchTargetFacing(2);
    if (fabsf(fx) > 1e-4f || fabsf(fz) > 1e-4f)
    {
        NormalizeXZ(fx, fz);
        outQuery.facingX = fx;
        outQuery.facingZ = fz;
    }
}

int Scene3DRenderer::findBestMotionMatchFrame(const MotionMatchFeature& query, float& outScore) const
{
    if (!m_motionMatchDb.valid || m_motionMatchDb.frames.empty())
        return -1;

    float bestScore = 1e30f;
    int bestIndex = -1;

    for (int i = 0; i < (int)m_motionMatchDb.frames.size(); ++i)
    {
        const MotionMatchFeature& f = m_motionMatchDb.frames[i].feature;

        float cost = 0.0f;
        float dvx = f.rootVelX - query.rootVelX;
        float dvz = f.rootVelZ - query.rootVelZ;
        cost += m_motionMatchWeightVel * (dvx * dvx + dvz * dvz);

        float dfx = f.facingX - query.facingX;
        float dfz = f.facingZ - query.facingZ;
        cost += m_motionMatchWeightFacing * (dfx * dfx + dfz * dfz);

        float footCost = 0.0f;
        if (m_motionMatchDb.leftFootBoneIndex >= 0)
        {
            float dx = f.leftFootX - query.leftFootX;
            float dy = f.leftFootY - query.leftFootY;
            float dz = f.leftFootZ - query.leftFootZ;
            footCost += dx * dx + dy * dy + dz * dz;
        }
        if (m_motionMatchDb.rightFootBoneIndex >= 0)
        {
            float dx = f.rightFootX - query.rightFootX;
            float dy = f.rightFootY - query.rightFootY;
            float dz = f.rightFootZ - query.rightFootZ;
            footCost += dx * dx + dy * dy + dz * dz;
        }
        cost += m_motionMatchWeightFeet * footCost;

        float tx1 = f.trajX1 - query.trajX1;
        float tz1 = f.trajZ1 - query.trajZ1;
        float tx2 = f.trajX2 - query.trajX2;
        float tz2 = f.trajZ2 - query.trajZ2;
        float tx3 = f.trajX3 - query.trajX3;
        float tz3 = f.trajZ3 - query.trajZ3;
        float trajCost = (tx1 * tx1 + tz1 * tz1) +
                         (tx2 * tx2 + tz2 * tz2) +
                         (tx3 * tx3 + tz3 * tz3);
        cost += m_motionMatchWeightTraj * trajCost;

        if (m_motionMatchCurrentFrameIndex >= 0 &&
            m_motionMatchCurrentFrameIndex < (int)m_motionMatchDb.frames.size())
        {
            int delta = abs(m_motionMatchDb.frames[i].frameIndex - m_motionMatchDb.frames[m_motionMatchCurrentFrameIndex].frameIndex);
            if (m_motionMatchDb.frames[i].clipIndex != m_motionMatchCurrentClipIndex)
                delta += 30;
            cost += 0.0005f * (float)delta;
        }

        if (cost < bestScore)
        {
            bestScore = cost;
            bestIndex = i;
        }
    }

    outScore = bestScore;
    return bestIndex;
}

void Scene3DRenderer::updateMotionMatchQueryFromPose(const hkaPose& pose)
{
    if (!m_gameModel || !m_gameModel->skeleton)
        return;

    const hkaSkeleton* skeleton = m_gameModel->skeleton;
    int rootIdx = (m_motionMatchDb.rootBoneIndex >= 0) ? m_motionMatchDb.rootBoneIndex : 0;
    int leftFoot = m_motionMatchDb.leftFootBoneIndex;
    int rightFoot = m_motionMatchDb.rightFootBoneIndex;

    const hkQsTransform& root = pose.getBoneModelSpace(rootIdx);
    hkVector4 rootPos = root.getTranslation();

    MotionMatchFeature f;
    f.rootPosX = rootPos(0);
    f.rootPosZ = rootPos(2);

    float dt = (m_lastAnimDeltaTime > 1e-5f) ? m_lastAnimDeltaTime : (1.0f / 60.0f);
    if (m_motionMatchHasLastFeature)
    {
        f.rootVelX = (f.rootPosX - m_motionMatchLastFeature.rootPosX) / dt;
        f.rootVelZ = (f.rootPosZ - m_motionMatchLastFeature.rootPosZ) / dt;
    }
    else
    {
        f.rootVelX = 0.0f;
        f.rootVelZ = 0.0f;
    }

    hkVector4 base;
    base.set(0.0f, 0.0f, 1.0f, 0.0f);
    hkVector4 fwd = RotateVectorByQuat(root.getRotation(), base);
    f.facingX = fwd(0);
    f.facingZ = fwd(2);
    NormalizeXZ(f.facingX, f.facingZ);

    if (leftFoot >= 0 && leftFoot < skeleton->m_numBones)
    {
        hkVector4 pos = pose.getBoneModelSpace(leftFoot).getTranslation();
        hkVector4 rel;
        rel.setSub4(pos, rootPos);
        f.leftFootX = rel(0);
        f.leftFootY = rel(1);
        f.leftFootZ = rel(2);
    }
    else
    {
        f.leftFootX = f.leftFootY = f.leftFootZ = 0.0f;
    }

    if (rightFoot >= 0 && rightFoot < skeleton->m_numBones)
    {
        hkVector4 pos = pose.getBoneModelSpace(rightFoot).getTranslation();
        hkVector4 rel;
        rel.setSub4(pos, rootPos);
        f.rightFootX = rel(0);
        f.rightFootY = rel(1);
        f.rightFootZ = rel(2);
    }
    else
    {
        f.rightFootX = f.rightFootY = f.rightFootZ = 0.0f;
    }

    f.trajX1 = f.rootVelX * m_motionMatchTrajT1;
    f.trajZ1 = f.rootVelZ * m_motionMatchTrajT1;
    f.trajX2 = f.rootVelX * m_motionMatchTrajT2;
    f.trajZ2 = f.rootVelZ * m_motionMatchTrajT2;
    f.trajX3 = f.rootVelX * m_motionMatchTrajT3;
    f.trajZ3 = f.rootVelZ * m_motionMatchTrajT3;

    m_motionMatchLastFeature = f;
    m_motionMatchHasLastFeature = true;
}

void Scene3DRenderer::updateMotionMatching(float deltaTime)
{
    if (!m_motionMatchEnabled)
        return;
    if (!m_motionMatchDatabaseValid)
    {
        if (!rebuildMotionMatchDatabase())
            return;
    }

    if (m_motionMatchBlendActive)
    {
        m_motionMatchBlendTime += deltaTime;
        if (m_motionMatchBlendDuration > 0.0f && m_motionMatchBlendTime >= m_motionMatchBlendDuration)
        {
            m_motionMatchBlendActive = false;
        }
    }

    if (m_motionMatchDb.clips.empty())
        return;

    if (m_motionMatchCurrentClipIndex < 0 || m_motionMatchCurrentClipIndex >= (int)m_motionMatchDb.clips.size())
    {
        m_motionMatchCurrentClipIndex = 0;
        m_motionMatchTime = 0.0f;
        m_motionMatchPrevTime = 0.0f;
    }

    MotionMatchClipInfo& curInfo = m_motionMatchDb.clips[m_motionMatchCurrentClipIndex];
    JsonAnimClip* curClip = curInfo.clip;
    if (!curClip)
        return;

    if (!m_jsonAnimPaused)
    {
        float dt = deltaTime * m_playbackSpeed;
        float dur = curInfo.duration;
        if (dur <= 0.0f)
            dur = GetClipDuration(curClip);
        const bool loop = m_loopAnimation;
        m_motionMatchTime = AdvanceGraphTime(m_motionMatchTime, dt, dur, loop);
    }

    if (curInfo.frameTime > 0.0f)
    {
        int frame = (int)(m_motionMatchTime / curInfo.frameTime + 0.5f);
        if (frame < 0) frame = 0;
        if (frame >= curInfo.frameCount) frame = curInfo.frameCount - 1;
        m_motionMatchCurrentFrameIndex = curInfo.startIndex + frame;
    }

    m_motionMatchSearchTimer -= deltaTime;
    if (m_motionMatchSearchTimer <= 0.0f)
    {
        MotionMatchFeature query;
        buildMotionMatchQuery(query);
        float score = 0.0f;
        int best = findBestMotionMatchFrame(query, score);
        if (best >= 0)
        {
            if (best != m_motionMatchCurrentFrameIndex)
            {
                m_motionMatchRequestBlend = true;
            }
            m_motionMatchCurrentFrameIndex = best;
            const MotionMatchFrame& fr = m_motionMatchDb.frames[best];
            m_motionMatchCurrentClipIndex = fr.clipIndex;
            m_motionMatchTime = fr.time;
            m_motionMatchPrevTime = fr.time;
            m_motionMatchLastScore = score;
        }
        m_motionMatchSearchTimer = m_motionMatchSearchInterval;
    }

    if (!m_jsonAnimPaused &&
        m_motionMatchCurrentClipIndex >= 0 &&
        m_motionMatchCurrentClipIndex < (int)m_motionMatchDb.clips.size())
    {
        MotionMatchClipInfo& finalInfo = m_motionMatchDb.clips[m_motionMatchCurrentClipIndex];
        if (finalInfo.clip)
        {
            processAnimEventsForClip(finalInfo.clip, m_motionMatchPrevTime, m_motionMatchTime,
                                     (unsigned int)m_motionMatchCurrentClipIndex);
            m_motionMatchPrevTime = m_motionMatchTime;
        }
    }
}

void Scene3DRenderer::buildPoseFromMotionMatch(hkaPose& pose)
{
    if (!m_motionMatchEnabled || !m_motionMatchDatabaseValid || !m_gameModel || !m_gameModel->skeleton)
    {
        pose.setToReferencePose();
        pose.syncModelSpace();
        return;
    }

    if (m_forceReferencePose)
    {
        pose.setToReferencePose();
        pose.syncModelSpace();
        return;
    }

    pose.setToReferencePose();
    const int boneCount = m_gameModel->skeleton->m_numBones;
    if (boneCount <= 0)
    {
        pose.syncModelSpace();
        return;
    }

    if (m_motionMatchCurrentClipIndex < 0 || m_motionMatchCurrentClipIndex >= (int)m_motionMatchDb.clips.size())
    {
        pose.syncModelSpace();
        return;
    }

    JsonAnimClip* clip = m_motionMatchDb.clips[m_motionMatchCurrentClipIndex].clip;
    if (!clip)
    {
        pose.syncModelSpace();
        return;
    }

    hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();
    applyJsonClipToLocalPose(clip, local, m_motionMatchTime);

    if (m_motionMatchRequestBlend && m_motionMatchHasLastPose &&
        m_motionMatchLastLocalPose.getSize() == boneCount)
    {
        m_motionMatchBlendFrom.setSize(boneCount);
        for (int i = 0; i < boneCount; ++i)
            m_motionMatchBlendFrom[i] = m_motionMatchLastLocalPose[i];
        m_motionMatchBlendTime = 0.0f;
        m_motionMatchBlendActive = true;
        m_motionMatchRequestBlend = false;
    }

    if (m_motionMatchBlendActive && m_motionMatchBlendDuration > 0.0f &&
        m_motionMatchBlendFrom.getSize() == boneCount)
    {
        float alpha = m_motionMatchBlendTime / m_motionMatchBlendDuration;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        m_motionMatchBlendTemp.setSize(boneCount);
        for (int i = 0; i < boneCount; ++i)
            m_motionMatchBlendTemp[i] = m_motionMatchBlendFrom[i];
        const bool useNlerp = (m_rotInterpMode == ROT_INTERP_NLERP);
        BlendLocalPose(m_motionMatchBlendTemp.begin(), local, boneCount, alpha, NULL, useNlerp);
        for (int i = 0; i < boneCount; ++i)
            local[i] = m_motionMatchBlendTemp[i];
        if (alpha >= 1.0f)
            m_motionMatchBlendActive = false;
    }

    applyJsonBlendToLocalPose(local, m_motionMatchTime);

    float frame = ComputeClipFrame(clip, m_motionMatchTime);
    applyEditorOverridesToLocalPose(local, boneCount, frame);

    bool rootApplied = applyRootMotionToPose(pose, clip, m_motionMatchTime);
    bool ikApplied = applyIKToPose(pose);
    bool physApplied = applyPhysicsToPose(pose);
    if (!ikApplied && !physApplied && !rootApplied)
        pose.syncModelSpace();

    pose.syncModelSpace();
    updateMotionMatchQueryFromPose(pose);

    if (m_motionMatchLastLocalPose.getSize() != boneCount)
        m_motionMatchLastLocalPose.setSize(boneCount);
    const hkQsTransform* poseLocal = pose.accessPoseLocalSpace().begin();
    for (int i = 0; i < boneCount; ++i)
        m_motionMatchLastLocalPose[i] = poseLocal[i];
    m_motionMatchHasLastPose = true;
}
