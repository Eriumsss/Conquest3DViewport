// LuaAnimationRuntime.cpp — The Seance. Executing Dead Code.
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Lua runtime foundation: initializes lua_State with our custom bindings,
// registers the AnimStateMachine/AnimTable/Filter API functions, and
// provides the bridge between Lua execution and C++ state. When Pandemic's
// Lua scripts call "AnimStateMachine.new()" or "state:setClip()", they
// land HERE, in OUR C++ implementations of functions that Pandemic wrote
// but whose source code died with the studio. We rebuilt the bindings
// from string analysis of the .exe and trial-and-error against the
// actual Lua bytecode. It's like building a Ouija board that actually
// works and using it to talk to a dead game engine.
// -----------------------------------------------------------------------

#include "Scene3DRendererInternal.h"
#include "LuaAnimationRuntime.h"
#include <windows.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <set>
#include <ctype.h>
#include <stdio.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

JsonAnimClip* LoadJsonAnimClip(const char* path,
                               const hkaSkeleton* skeleton,
                               int decodeMode,
                               int type2PackingMode);

LuaScriptContext::LuaScriptContext()
    : rngState(0x12345678u),
      renderer(NULL)
{}

static int CountCoveredSkeletonBones_Local(const JsonAnimClip* clip, int skeletonBoneCount)
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

JsonAnimClip* LoadLuaAnimClipChecked(const std::string& clipPath,
                                     const hkaSkeleton* skeleton,
                                     int jsonDecodeMode,
                                     int type2PackingMode,
                                     bool strictRigCoverage)
{
    if (clipPath.empty() || !skeleton)
        return NULL;

    JsonAnimClip* clip = LoadJsonAnimClip(clipPath.c_str(), skeleton, jsonDecodeMode, type2PackingMode);
    if (clip && clip->tracks.empty())
    {
        delete clip;
        clip = NULL;
    }

    if (clip && strictRigCoverage)
    {
        const int skeletonBones = skeleton->m_numBones;
        const int coveredBones = CountCoveredSkeletonBones_Local(clip, skeletonBones);
        int minCovered = (int)(skeletonBones * 0.25f);
        if (minCovered < 4) minCovered = 4;

        if (coveredBones < minCovered)
        {
            delete clip;
            clip = NULL;
        }
    }

    return clip;
}

static float Vec3DistXZ(const JsonVec3& a, const JsonVec3& b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return (float)sqrt(dx * dx + dz * dz);
}

static bool CollectRootTranslations(const JsonAnimClip* clip, std::vector<JsonVec3>& out)
{
    out.clear();
    if (!clip) return false;

    if (clip->rootBoneIndex >= 0 && !clip->translationTracks.empty())
    {
        for (size_t i = 0; i < clip->translationTracks.size(); ++i)
        {
            const JsonTranslationTrack& track = clip->translationTracks[i];
            if (track.boneIndex == clip->rootBoneIndex && !track.translations.empty())
            {
                out = track.translations;
                return out.size() >= 2;
            }
        }
    }

    if (!clip->rootTranslations.empty())
    {
        out = clip->rootTranslations;
        return out.size() >= 2;
    }

    return false;
}

static float GetClipDurationFallback(const JsonAnimClip* clip)
{
    if (!clip) return 0.0f;
    if (clip->duration > 0.0f) return clip->duration;
    if (clip->frameTime > 0.0f && clip->frameCount > 1)
        return (clip->frameCount - 1) * clip->frameTime;
    return 0.0f;
}

static float ComputeRootSpeedFromClip(const JsonAnimClip* clip)
{
    if (!clip) return 0.0f;
    std::vector<JsonVec3> samples;
    if (!CollectRootTranslations(clip, samples))
        return 0.0f;

    float total = 0.0f;
    for (size_t i = 1; i < samples.size(); ++i)
        total += Vec3DistXZ(samples[i - 1], samples[i]);

    float duration = GetClipDurationFallback(clip);
    if (duration <= 0.0f)
        return 0.0f;
    return total / duration;
}

static void LuaLogStubOnce(LuaScriptContext* ctx, const char* module, const char* func)
{
    if (!ctx || !module || !func) return;
    std::string key = std::string(module) + "." + func;
    if (ctx->warnedApis.find(key) != ctx->warnedApis.end())
        return;
    ctx->warnedApis.insert(key);
    char buf[512];
    sprintf_s(buf, "Lua API stub called: %s.%s", module, func);
    RendererLog(buf);
}

static int LuaStubFunction(lua_State* L)
{
    LuaScriptContext* ctx = (LuaScriptContext*)lua_touserdata(L, lua_upvalueindex(1));
    const char* module = lua_tostring(L, lua_upvalueindex(2));
    const char* func = lua_tostring(L, lua_upvalueindex(3));
    LuaLogStubOnce(ctx, module, func);
    lua_pushnil(L);
    return 1;
}

static int LuaStubModuleIndex(lua_State* L)
{
    if (!lua_isstring(L, 2))
    {
        lua_pushnil(L);
        return 1;
    }
    const char* func = lua_tostring(L, 2);
    const char* module = lua_tostring(L, lua_upvalueindex(1));
    LuaScriptContext* ctx = (LuaScriptContext*)lua_touserdata(L, lua_upvalueindex(2));

    lua_pushlightuserdata(L, ctx);
    lua_pushstring(L, module ? module : "");
    lua_pushstring(L, func ? func : "");
    lua_pushcclosure(L, LuaStubFunction, 3);
    return 1;
}

static void LuaAttachStubMeta(lua_State* L, const char* moduleName, LuaScriptContext* ctx)
{
    lua_newtable(L);
    lua_pushstring(L, moduleName ? moduleName : "");
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaStubModuleIndex, 2);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
}

static void LuaRegisterModule(lua_State* L, const char* moduleName, const luaL_Reg* funcs, LuaScriptContext* ctx)
{
    lua_newtable(L);
    if (funcs)
        luaL_register(L, NULL, funcs);
    LuaAttachStubMeta(L, moduleName, ctx);
    lua_setglobal(L, moduleName);
}

static bool FileExistsA_SimpleAnim(const char* path);

static void TrimWhitespace(std::string& text)
{
    size_t start = 0;
    while (start < text.size() && isspace((unsigned char)text[start]))
        ++start;
    size_t end = text.size();
    while (end > start && isspace((unsigned char)text[end - 1]))
        --end;
    text = text.substr(start, end - start);
}

static bool ParseRegistryLine(const std::string& line, std::string& outName)
{
    std::string text = line;
    size_t cut = std::string::npos;
    size_t hashPos = text.find('#');
    size_t semiPos = text.find(';');
    size_t slashPos = text.find("//");
    if (hashPos != std::string::npos) cut = hashPos;
    if (semiPos != std::string::npos && (cut == std::string::npos || semiPos < cut)) cut = semiPos;
    if (slashPos != std::string::npos && (cut == std::string::npos || slashPos < cut)) cut = slashPos;
    if (cut != std::string::npos)
        text = text.substr(0, cut);
    TrimWhitespace(text);
    if (text.empty())
        return false;
    outName = text;
    return true;
}

static bool ReadTextFileLines(const char* path, std::vector<std::string>& outLines)
{
    outLines.clear();
    if (!path || !path[0]) return false;
    FILE* file = NULL;
    fopen_s(&file, path, "rb");
    if (!file) return false;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), file))
        outLines.push_back(std::string(buffer));
    fclose(file);
    return true;
}

static bool LuaHasGlobalTable(lua_State* L, const char* name)
{
    if (!name || !name[0]) return false;
    lua_getglobal(L, name);
    bool ok = lua_istable(L, -1);
    lua_pop(L, 1);
    return ok;
}

static bool LuaRegisterStubModuleIfMissing(lua_State* L, const char* moduleName, LuaScriptContext* ctx)
{
    if (!moduleName || !moduleName[0]) return false;
    if (LuaHasGlobalTable(L, moduleName))
        return false;
    LuaRegisterModule(L, moduleName, NULL, ctx);
    return true;
}

static std::string GetExecutableDirectoryAnim()
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return std::string(".");
    return GetDirectoryFromPathAnim(path);
}

static void LuaRegisterStubModulesFromRegistry(lua_State* L, LuaScriptContext* ctx)
{
    std::string exeDir = GetExecutableDirectoryAnim();
    std::string path = exeDir + "\\LuaApiRegistry.txt";
    if (!FileExistsA_SimpleAnim(path.c_str()))
    {
        RendererLog("Lua API registry missing: LuaApiRegistry.txt");
        return;
    }

    std::vector<std::string> lines;
    if (!ReadTextFileLines(path.c_str(), lines))
    {
        RendererLog("Lua API registry read failed.");
        return;
    }

    int total = 0;
    int added = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string name;
        if (!ParseRegistryLine(lines[i], name))
            continue;
        ++total;
        if (LuaRegisterStubModuleIfMissing(L, name.c_str(), ctx))
            ++added;
    }

    char buf[256];
    sprintf_s(buf, "Lua API registry loaded: %d entries, %d new stubs.", total, added);
    RendererLog(buf);
}

static bool FileExistsA_SimpleAnim(const char* path)
{
    if (!path || !path[0]) return false;
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return false;
    return true;
}

static bool EndsWithNoCaseAnim(const char* text, const char* suffix)
{
    if (!text || !suffix) return false;
    size_t textLen = strlen(text);
    size_t suffixLen = strlen(suffix);
    if (suffixLen == 0 || suffixLen > textLen) return false;
    return _stricmp(text + (textLen - suffixLen), suffix) == 0;
}

static bool IsAbsolutePathAnim(const char* path)
{
    if (!path || !path[0]) return false;
    if (path[0] == '\\' || path[0] == '/') return true;
    if (path[1] == ':') return true;
    return false;
}

std::string GetDirectoryFromPathAnim(const char* path)
{
    if (!path) return std::string();
    std::string p(path);
    size_t cut = p.find_last_of("\\/");
    if (cut == std::string::npos) return std::string(".");
    return p.substr(0, cut);
}

std::string FindGameFilesRootFromPathAnim(const char* path)
{
    const char* markerA = "\\GameFiles\\";
    const char* markerB = "/GameFiles/";
    if (!path) return std::string("..\\GameFiles\\");
    std::string p(path);
    size_t pos = p.find(markerA);
    if (pos == std::string::npos)
    {
        pos = p.find(markerB);
    }
    if (pos != std::string::npos)
    {
        size_t markerLen = strlen(markerA);
        return p.substr(0, pos + markerLen);
    }
    return std::string("..\\GameFiles\\");
}

static void LuaLogMessage(Scene3DRenderer* renderer, const char* prefix, const char* message)
{
    if (!renderer && !message) return;
    char buf[1024];
    if (prefix && message)
        sprintf_s(buf, "%s%s", prefix, message);
    else if (message)
        sprintf_s(buf, "%s", message);
    else
        sprintf_s(buf, "%s", prefix ? prefix : "");
    RendererLog(buf);
}

bool LuaRunFile(lua_State* L, const char* path, Scene3DRenderer* renderer)
{
    if (!path || !path[0]) return false;
    int status = luaL_loadfile(L, path);
    if (status != 0)
    {
        const char* err = lua_tostring(L, -1);
        LuaLogMessage(renderer, "Lua load error: ", err ? err : path);
        lua_pop(L, 1);
        return false;
    }
    status = lua_pcall(L, 0, 0, 0);
    if (status != 0)
    {
        const char* err = lua_tostring(L, -1);
        LuaLogMessage(renderer, "Lua run error: ", err ? err : path);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

static std::string NormalizeModuleName(const char* module)
{
    std::string out = module ? module : "";
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (out[i] == '/') out[i] = '\\';
    }
    return out;
}

static std::string GetLevelRootFromBaseDir(const std::string& baseDir);

static std::string GetLevelNameFromBaseDir(const std::string& baseDir)
{
    if (baseDir.empty()) return std::string();
    std::string lower = baseDir;
    for (size_t i = 0; i < lower.size(); ++i)
        lower[i] = (char)tolower(lower[i]);
    const char* k1 = "\\lotrcparser\\";
    const char* k2 = "\\srcjson\\";
    const char* k3 = "\\srclua\\";
    size_t pos = lower.find(k1);
    size_t markerLen = strlen(k1);
    if (pos == std::string::npos) { pos = lower.find(k2); markerLen = strlen(k2); }
    if (pos == std::string::npos) { pos = lower.find(k3); markerLen = strlen(k3); }
    if (pos == std::string::npos)
        return std::string();

    size_t start = pos + markerLen;
    size_t end = lower.find('\\', start);
    if (end == std::string::npos)
        end = lower.find('/', start);
    if (end == std::string::npos)
        end = baseDir.size();
    if (end <= start)
        return std::string();
    return baseDir.substr(start, end - start);
}

static bool TryModuleFromDir(const std::string& dir, const std::string& moduleName, std::string& outPath)
{
    if (dir.empty()) return false;
    std::string path = dir;
    if (!path.empty() && path[path.size() - 1] != '\\' && path[path.size() - 1] != '/')
        path += "\\";
    path += moduleName;
    if (!EndsWithNoCaseAnim(path.c_str(), ".lua"))
        path += ".lua";
    if (FileExistsA_SimpleAnim(path.c_str()))
    {
        outPath = path;
        return true;
    }
    return false;
}

static bool TryLoadModuleFromDir(lua_State* L,
                                 Scene3DRenderer* renderer,
                                 const std::string& dir,
                                 const std::string& moduleName,
                                 bool& outTried)
{
    std::string path;
    if (!TryModuleFromDir(dir, moduleName, path))
        return false;
    outTried = true;
    if (LuaRunFile(L, path.c_str(), renderer))
        return true;
    return false;
}

bool LuaLoadModule(lua_State* L, LuaScriptContext* ctx, const char* moduleName)
{
    if (!ctx || !moduleName || !moduleName[0]) return false;
    std::string name = NormalizeModuleName(moduleName);
    if (ctx->loadedModules.find(name) != ctx->loadedModules.end())
        return true;
    ctx->loadedModules.insert(name);

    bool triedAny = false;
    std::string path;
    if (TryLoadModuleFromDir(L, ctx->renderer, ctx->baseDir, name, triedAny))
        return true;

    const std::string levelName = GetLevelNameFromBaseDir(ctx->baseDir);
    const std::string levelRoot = GetLevelRootFromBaseDir(ctx->baseDir);

    if (!levelRoot.empty() && TryLoadModuleFromDir(L, ctx->renderer, levelRoot, name, triedAny))
        return true;

    if (!ctx->gameFilesRoot.empty())
    {
        const std::string gf = ctx->gameFilesRoot;
        if (!levelName.empty())
        {
            if (TryLoadModuleFromDir(L, ctx->renderer, gf + "srclua\\" + levelName, name, triedAny))
                return true;
            if (TryLoadModuleFromDir(L, ctx->renderer, gf + "lotrcparser\\Unluac\\Output", name, triedAny))
                return true;
            if (TryLoadModuleFromDir(L, ctx->renderer, gf + "lotrcparser\\" + levelName + "\\sub_blocks1", name, triedAny))
                return true;
            if (TryLoadModuleFromDir(L, ctx->renderer, gf + "lotrcparser\\" + levelName + "\\sub_blocks2", name, triedAny))
                return true;
            if (TryLoadModuleFromDir(L, ctx->renderer, gf + "srcjson\\" + levelName + "\\sub_blocks1", name, triedAny))
                return true;
            if (TryLoadModuleFromDir(L, ctx->renderer, gf + "srcjson\\" + levelName + "\\sub_blocks2", name, triedAny))
                return true;
        }
        if (TryLoadModuleFromDir(L, ctx->renderer, gf + "lotrcparser\\Unluac\\Output", name, triedAny))
            return true;
    }

    if (triedAny)
    {
        LuaLogMessage(ctx->renderer, "Lua import failed: ", name.c_str());
        return false;
    }

    // Keep the old message, but mention the first attempted location.
    std::string missing = ctx->baseDir;
    if (!missing.empty() && missing[missing.size() - 1] != '\\' && missing[missing.size() - 1] != '/')
        missing += "\\";
    missing += name;
    if (!EndsWithNoCaseAnim(missing.c_str(), ".lua"))
        missing += ".lua";
    LuaLogMessage(ctx->renderer, "Lua import missing: ", missing.c_str());
    return false;
}

static LuaScriptContext* LuaGetContext(lua_State* L)
{
    return (LuaScriptContext*)lua_touserdata(L, lua_upvalueindex(1));
}

static const char* LuaGetModuleArg(lua_State* L)
{
    int top = lua_gettop(L);
    if (top >= 2 && lua_isstring(L, 2))
        return lua_tostring(L, 2);
    if (top >= 1 && lua_isstring(L, 1))
        return lua_tostring(L, 1);
    return NULL;
}

static int LuaSysImport(lua_State* L)
{
    LuaScriptContext* ctx = LuaGetContext(L);
    const char* module = LuaGetModuleArg(L);
    if (module && ctx)
    {
        LuaLoadModule(L, ctx, module);
    }
    return 0;
}

static int LuaSysInherit(lua_State* L)
{
    LuaScriptContext* ctx = LuaGetContext(L);
    const char* module = LuaGetModuleArg(L);
    if (module && ctx)
    {
        LuaLoadModule(L, ctx, module);
    }
    return 0;
}

static int LuaSysPrint(lua_State* L)
{
    LuaScriptContext* ctx = LuaGetContext(L);
    const char* msg = lua_tostring(L, 1);
    if (!msg) msg = "";
    LuaLogMessage(ctx ? ctx->renderer : NULL, "Lua: ", msg);
    return 0;
}

static int LuaSysPrintError(lua_State* L)
{
    LuaScriptContext* ctx = LuaGetContext(L);
    const char* msg = lua_tostring(L, 1);
    if (!msg) msg = "";
    LuaLogMessage(ctx ? ctx->renderer : NULL, "Lua ERROR: ", msg);
    return 0;
}

static int LuaSysModuleIndex(lua_State* L)
{
    if (!lua_istable(L, 1) || !lua_isstring(L, 2))
    {
        lua_pushnil(L);
        return 1;
    }

    lua_pushvalue(L, 2);
    lua_gettable(L, 1);
    if (!lua_isnil(L, -1))
    {
        return 1;
    }
    lua_pop(L, 1);

    lua_getglobal(L, "SYSTEM_MODULES");
    if (lua_istable(L, -1))
    {
        lua_pushvalue(L, 2);
        lua_gettable(L, -2);
        return 1;
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    return 1;
}

static int LuaSysCreateSystemModule(lua_State* L)
{
    const char* name = lua_tostring(L, 1);
    if (!name) return 0;
    lua_getglobal(L, "SYSTEM_MODULES");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setglobal(L, "SYSTEM_MODULES");
        lua_getglobal(L, "SYSTEM_MODULES");
    }
    lua_newtable(L);
    LuaScriptContext* ctx = (LuaScriptContext*)lua_touserdata(L, lua_upvalueindex(1));
    if (ctx)
        LuaAttachStubMeta(L, name, ctx);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 0;
}

static int LuaScriptAssert(lua_State* L)
{
    LuaScriptContext* ctx = LuaGetContext(L);
    int ok = lua_toboolean(L, 1);
    if (!ok)
    {
        const char* msg = lua_tostring(L, 2);
        LuaLogMessage(ctx ? ctx->renderer : NULL, "Lua Assert: ", msg ? msg : "failed");
        return luaL_error(L, msg ? msg : "Lua Assert failed");
    }
    return 0;
}

static int LuaScriptGetRandomNumber(lua_State* L)
{
    LuaScriptContext* ctx = LuaGetContext(L);
    if (!ctx)
    {
        lua_pushnumber(L, 0.5f);
        return 1;
    }
    ctx->rngState = (ctx->rngState * 1664525u) + 1013904223u;
    unsigned int v = (ctx->rngState >> 8) & 0x00FFFFFFu;
    float f = (float)v / 16777216.0f;
    lua_pushnumber(L, f);
    return 1;
}

static int LuaAnimGetRootSpeed(lua_State* L)
{
    LuaScriptContext* ctx = LuaGetContext(L);
    const char* name = lua_tostring(L, 1);
    if (!ctx || !name || !name[0])
    {
        lua_pushnumber(L, 0.0f);
        return 1;
    }

    std::string clipPath;
    if (!ResolveLuaAnimClipPath(*ctx, std::string(name), clipPath))
    {
        lua_pushnumber(L, 0.0f);
        return 1;
    }

    const hkaSkeleton* skeleton = NULL;
    int decodeMode = 0;
    int type2PackingMode = 0;
    if (ctx->renderer)
    {
        skeleton = ctx->renderer->getModelSkeleton();
        decodeMode = ctx->renderer->getJsonDecodeMode();
        type2PackingMode = ctx->renderer->getType2PackingMode();
    }

    JsonAnimClip* clip = LoadJsonAnimClip(clipPath.c_str(), skeleton, decodeMode, type2PackingMode);
    if (!clip)
    {
        lua_pushnumber(L, 0.0f);
        return 1;
    }

    float speed = ComputeRootSpeedFromClip(clip);
    delete clip;

    lua_pushnumber(L, speed);
    return 1;
}

static bool LuaGetTableIntField(lua_State* L, int idx, const char* name, int& out)
{
    idx = LuaAbsIndex(L, idx);
    lua_getfield(L, idx, name);
    if (lua_isnumber(L, -1))
    {
        out = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);
        return true;
    }
    lua_pop(L, 1);
    return false;
}

static int LuaCreatureEnvGetVariable(lua_State* L)
{
    LuaScriptContext* ctx = LuaGetContext(L);
    if (lua_isnumber(L, 1))
    {
        lua_pushnumber(L, lua_tonumber(L, 1));
        return 1;
    }

    if (lua_istable(L, 1))
    {
        int idx = -1;
        int src = 0;
        if (LuaGetTableIntField(L, 1, "Index", idx))
        {
            LuaGetTableIntField(L, 1, "Source", src);
            if (ctx && ctx->renderer)
            {
                float v = ctx->renderer->getCreatureVar(idx, src);
                lua_pushnumber(L, v);
                return 1;
            }
            // CV_RANDOM is typically index 59; return deterministic random.
            if (idx == 59 && ctx)
            {
                ctx->rngState = (ctx->rngState * 1664525u) + 1013904223u;
                unsigned int v = (ctx->rngState >> 8) & 0x00FFFFFFu;
                float f = (float)v / 16777216.0f;
                lua_pushnumber(L, f);
                return 1;
            }
        }
    }

    lua_pushnumber(L, 0.0f);
    return 1;
}

void LuaRegisterBindings(lua_State* L, LuaScriptContext* ctx)
{
    lua_newtable(L);
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaSysImport, 1);
    lua_setfield(L, -2, "SYS_IMPORT");
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaSysInherit, 1);
    lua_setfield(L, -2, "SYS_INHERIT");
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaSysPrint, 1);
    lua_setfield(L, -2, "SYS_PRINT");
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaSysPrintError, 1);
    lua_setfield(L, -2, "SYS_PRINTERROR");
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaSysModuleIndex, 1);
    lua_setfield(L, -2, "ModuleIndex");
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaSysCreateSystemModule, 1);
    lua_setfield(L, -2, "CreateSystemModule");
    LuaAttachStubMeta(L, "MgSys", ctx);
    lua_setglobal(L, "MgSys");

    lua_newtable(L);
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaScriptAssert, 1);
    lua_setfield(L, -2, "Assert");
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, LuaScriptGetRandomNumber, 1);
    lua_setfield(L, -2, "GetRandomNumber");
    LuaAttachStubMeta(L, "MgScript", ctx);
    lua_setglobal(L, "MgScript");

    const luaL_Reg mgAnimFuncs[] =
    {
        {"GetRootSpeed", LuaAnimGetRootSpeed},
        {NULL, NULL}
    };
    LuaRegisterModule(L, "MgAnim", mgAnimFuncs, ctx);

    const luaL_Reg mgCreatureFuncs[] =
    {
        {"GetVariable", LuaCreatureEnvGetVariable},
        {NULL, NULL}
    };
    LuaRegisterModule(L, "MgCreatureEnv", mgCreatureFuncs, ctx);

    const char* kStubModules[] =
    {
        "MgCreature",
        "MgScriptObject",
        "MgScriptObjectFilter",
        "MgScriptTimer",
        "MgScriptVoiceOverObject",
        "MgScriptAIObject",
        "MgScriptAITarget",
        "MgScriptAIEventBroadcaster",
        "MgScriptAIObjectTask",
        "MgScriptNPCIntention",
        "MgScriptSplineCreature",
        "MgScriptMovieObject",
        "MgScriptUIInterfaceObject",
        "MgScriptAIGoal",
        "MgScriptAIPatrol",
        "MgScriptAIFortificationExtended",
        "MgScriptAIFortification",
        "MgScriptAITargetArea",
        "MgGameObject",
        "MgAnimation",
        "MgAnimProp",
        "MgAnimationController",
        "MgCreatureAnimController",
        "MgPropAnimController",
        "MgAnimationNode",
        "MgAnimationNodeBlend",
        "MgAnimationNodeDrivenRagdoll",
        "MgAnimationNodeEffect",
        "MgAnimationNodeFootPlacement",
        "MgAnimationNodeGlobalSRTFixup",
        "MgAnimationNodeInstance",
        "MgAnimationNodeLookAt",
        "MgAnimationNodePoseMatcher",
        "MgAnimationNodePoseMatcherInstance",
        "MgAnimationNodeProp",
        "MgAnimationNodePropInstance",
        "MgAnimationNodeResult",
        "MgAnimationNodeSampler",
        "MgAnimationNodeSamplerInstance",
        "MgAnimationNodeSelector",
        "MgAnimationNodeSelectorInstance",
        "MgAnimationNodeStateMachine",
        "MgAnimationNodeTwoJointsIk",
        "MgAnimationTwoJointsIkSolver",
        "MgAnimationFootIkSolver",
        "MgAnimationLookAtSolver",
        NULL
    };

    for (int i = 0; kStubModules[i]; ++i)
        LuaRegisterModule(L, kStubModules[i], NULL, ctx);

    LuaRegisterStubModulesFromRegistry(L, ctx);
}

bool LuaExtractAnimTable(lua_State* L, std::map<std::string, std::string>& outMap, std::string& outTableName, Scene3DRenderer* renderer)
{
    outMap.clear();
    outTableName.clear();

    lua_getglobal(L, "AnimTableName");
    if (lua_isstring(L, -1))
    {
        outTableName = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    lua_getglobal(L, "AnimTable");
    if (lua_istable(L, -1))
    {
        size_t len = (size_t)lua_objlen(L, -1);
        if (len > 0)
        {
            for (size_t i = 1; i <= len; ++i)
            {
                lua_rawgeti(L, -1, (int)i);
                if (lua_istable(L, -1))
                {
                    lua_pushnil(L);
                    while (lua_next(L, -2) != 0)
                    {
                        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING)
                        {
                            const char* k = lua_tostring(L, -2);
                            const char* v = lua_tostring(L, -1);
                            if (k && v)
                            {
                                outMap[std::string(k)] = std::string(v);
                            }
                        }
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
        }
        else
        {
            // Some tables are a flat map (string -> string).
            lua_pushnil(L);
            while (lua_next(L, -2) != 0)
            {
                if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING)
                {
                    const char* k = lua_tostring(L, -2);
                    const char* v = lua_tostring(L, -1);
                    if (k && v)
                    {
                        outMap[std::string(k)] = std::string(v);
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    else
    {
        lua_pop(L, 1);

        // Fallback: AnimTableNames lists global table names (common in AT_* scripts).
        lua_getglobal(L, "AnimTableNames");
        if (lua_istable(L, -1))
        {
            size_t len = (size_t)lua_objlen(L, -1);
            for (size_t i = 1; i <= len; ++i)
            {
                lua_rawgeti(L, -1, (int)i);
                if (lua_isstring(L, -1))
                {
                    const char* name = lua_tostring(L, -1);
                    if (name && name[0])
                    {
                        lua_getglobal(L, name);
                        if (lua_istable(L, -1))
                        {
                            lua_pushnil(L);
                            while (lua_next(L, -2) != 0)
                            {
                                if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING)
                                {
                                    const char* k = lua_tostring(L, -2);
                                    const char* v = lua_tostring(L, -1);
                                    if (k && v)
                                    {
                                        outMap[std::string(k)] = std::string(v);
                                    }
                                }
                                lua_pop(L, 1);
                            }
                        }
                        lua_pop(L, 1);
                        if (outTableName.empty())
                            outTableName = name;
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    if (outMap.empty())
    {
        LuaLogMessage(renderer, "Lua AnimTable missing or empty: ", "AnimTable");
        return false;
    }

    return true;
}

static void LuaCollectStringArray(lua_State* L, int idx, std::vector<std::string>& out)
{
    out.clear();
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx)) return;

    size_t len = (size_t)lua_objlen(L, idx);
    if (len > 0)
    {
        for (size_t i = 1; i <= len; ++i)
        {
            lua_rawgeti(L, idx, (int)i);
            if (lua_isstring(L, -1))
            {
                const char* s = lua_tostring(L, -1);
                if (s && s[0])
                    out.push_back(std::string(s));
            }
            lua_pop(L, 1);
        }
        return;
    }

    lua_pushnil(L);
    while (lua_next(L, idx) != 0)
    {
        if (lua_isstring(L, -1))
        {
            const char* s = lua_tostring(L, -1);
            if (s && s[0])
                out.push_back(std::string(s));
        }
        lua_pop(L, 1);
    }
}

static void LuaMergeAnimTable(lua_State* L,
                              int tableIdx,
                              const std::string& sourceName,
                              std::map<std::string, LuaAnimTableEntry>& out)
{
    tableIdx = LuaAbsIndex(L, tableIdx);
    if (!lua_istable(L, tableIdx))
        return;

    const std::string src = sourceName.empty() ? std::string("<table>") : sourceName;

    lua_pushnil(L);
    while (lua_next(L, tableIdx) != 0)
    {
        if (lua_type(L, -2) == LUA_TSTRING)
        {
            const char* key = lua_tostring(L, -2);
            if (key && key[0])
            {
                std::vector<std::string> clips;
                if (lua_isstring(L, -1))
                {
                    const char* v = lua_tostring(L, -1);
                    if (v && v[0])
                        clips.push_back(std::string(v));
                }
                else if (lua_istable(L, -1))
                {
                    LuaCollectStringArray(L, -1, clips);
                }

                if (!clips.empty())
                {
                    LuaAnimTableEntry entry;
                    entry.key = key;
                    entry.clips = clips;
                    entry.sourceTable = src;
                    out[entry.key] = entry; // last table wins
                }
            }
        }
        lua_pop(L, 1);
    }
}

static void LuaCollectGlobalTableNameMap(lua_State* L,
                                         const std::vector<std::string>& names,
                                         std::map<const void*, std::string>& out)
{
    for (size_t i = 0; i < names.size(); ++i)
    {
        const std::string& name = names[i];
        if (name.empty()) continue;
        lua_getglobal(L, name.c_str());
        if (lua_istable(L, -1))
        {
            const void* ptr = lua_topointer(L, -1);
            if (ptr)
                out[ptr] = name;
        }
        lua_pop(L, 1);
    }
}

static bool LuaMergeNamedTable(lua_State* L,
                               const char* name,
                               std::map<std::string, LuaAnimTableEntry>& out,
                               std::set<std::string>& missingTables)
{
    if (!name || !name[0])
        return false;
    lua_getglobal(L, name);
    if (lua_istable(L, -1))
    {
        LuaMergeAnimTable(L, -1, name, out);
        lua_pop(L, 1);
        return true;
    }
    lua_pop(L, 1);
    missingTables.insert(std::string(name));
    return false;
}

bool LuaExtractAnimTableInfo(lua_State* L, LuaAnimTableInfo& out, const LuaScriptContext& ctx, Scene3DRenderer* renderer)
{
    out.tableName.clear();
    out.tableNames.clear();
    out.tableUsed.clear();
    out.entries.clear();
    out.missingClips.clear();
    out.missingTables.clear();

    lua_getglobal(L, "AnimTableName");
    if (lua_isstring(L, -1))
        out.tableName = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getglobal(L, "AnimTableNames");
    if (lua_istable(L, -1))
        LuaCollectStringArray(L, -1, out.tableNames);
    lua_pop(L, 1);

    lua_getglobal(L, "AnimTableUsed");
    if (lua_istable(L, -1))
        LuaCollectStringArray(L, -1, out.tableUsed);
    lua_pop(L, 1);

    std::map<std::string, LuaAnimTableEntry> merged;
    std::set<std::string> missingTables;

    std::map<const void*, std::string> tableNameByPtr;
    LuaCollectGlobalTableNameMap(L, out.tableNames, tableNameByPtr);
    LuaCollectGlobalTableNameMap(L, out.tableUsed, tableNameByPtr);

    bool hasAnimTable = false;
    lua_getglobal(L, "AnimTable");
    if (lua_istable(L, -1))
    {
        size_t len = (size_t)lua_objlen(L, -1);
        if (len > 0)
        {
            hasAnimTable = true;
            for (size_t i = 1; i <= len; ++i)
            {
                lua_rawgeti(L, -1, (int)i);
                if (lua_isstring(L, -1))
                {
                    const char* name = lua_tostring(L, -1);
                    LuaMergeNamedTable(L, name, merged, missingTables);
                }
                else if (lua_istable(L, -1))
                {
                    const void* ptr = lua_topointer(L, -1);
                    std::string tname;
                    if (ptr)
                    {
                        std::map<const void*, std::string>::const_iterator it = tableNameByPtr.find(ptr);
                        if (it != tableNameByPtr.end())
                            tname = it->second;
                    }
                    if (tname.empty())
                    {
                        char tmp[64];
                        sprintf_s(tmp, "AnimTable[%u]", (unsigned)i);
                        tname = tmp;
                    }
                    LuaMergeAnimTable(L, -1, tname, merged);
                }
                lua_pop(L, 1);
            }
        }
        else
        {
            hasAnimTable = true;
            std::string name = out.tableName.empty() ? std::string("AnimTable") : out.tableName;
            LuaMergeAnimTable(L, -1, name, merged);
        }
    }
    lua_pop(L, 1);

    if (!hasAnimTable)
    {
        const std::vector<std::string>* list = !out.tableUsed.empty() ? &out.tableUsed : &out.tableNames;
        if (list)
        {
            for (size_t i = 0; i < list->size(); ++i)
            {
                LuaMergeNamedTable(L, (*list)[i].c_str(), merged, missingTables);
            }
        }
    }

    struct LuaAnimTableEntryKeyLess
    {
        bool operator()(const LuaAnimTableEntry& a, const LuaAnimTableEntry& b) const
        {
            return a.key < b.key;
        }
    };

    out.entries.reserve(merged.size());
    for (std::map<std::string, LuaAnimTableEntry>::const_iterator it = merged.begin(); it != merged.end(); ++it)
    {
        out.entries.push_back(it->second);
    }
    std::sort(out.entries.begin(), out.entries.end(), LuaAnimTableEntryKeyLess());

    if (!missingTables.empty())
    {
        for (std::set<std::string>::const_iterator it = missingTables.begin(); it != missingTables.end(); ++it)
            out.missingTables.push_back(*it);
    }

    std::set<std::string> missingClips;
    for (size_t i = 0; i < out.entries.size(); ++i)
    {
        const LuaAnimTableEntry& entry = out.entries[i];
        for (size_t j = 0; j < entry.clips.size(); ++j)
        {
            const std::string& clipName = entry.clips[j];
            std::string clipPath;
            if (!ResolveLuaAnimClipPath(ctx, clipName, clipPath))
            {
                missingClips.insert(clipName);
            }
        }
    }
    if (!missingClips.empty())
    {
        for (std::set<std::string>::const_iterator it = missingClips.begin(); it != missingClips.end(); ++it)
            out.missingClips.push_back(*it);
    }

    if (out.entries.empty())
    {
        LuaLogMessage(renderer, "Lua AnimTable missing or empty: ", "AnimTable");
        return false;
    }

    return true;
}

static bool LuaReadStringArray(lua_State* L, int idx, std::vector<std::string>& out)
{
    out.clear();
    if (!lua_istable(L, idx))
        return false;

    idx = LuaAbsIndex(L, idx);
    int len = (int)lua_objlen(L, idx);
    if (len <= 0)
    {
        lua_pushnil(L);
        while (lua_next(L, idx) != 0)
        {
            std::string value;
            if (LuaGetStringFromValue(L, -1, value))
                out.push_back(value);
            lua_pop(L, 1);
        }
        return !out.empty();
    }

    out.reserve(len);
    for (int i = 1; i <= len; ++i)
    {
        lua_rawgeti(L, idx, i);
        std::string value;
        if (LuaGetStringFromValue(L, -1, value))
            out.push_back(value);
        lua_pop(L, 1);
    }
    return !out.empty();
}

bool LuaExtractAnimStatesTranslatorInfo(lua_State* L, LuaAnimStatesTranslatorInfo& out, Scene3DRenderer* renderer)
{
    out.tableName.clear();
    out.states.clear();
    (void)renderer;

    int tableIndex = 0;
    lua_getglobal(L, "AnimationStatesTranslator");
    if (lua_istable(L, -1))
    {
        out.tableName = "AnimationStatesTranslator";
        tableIndex = lua_gettop(L);
    }
    else
    {
        lua_pop(L, 1);
        lua_getglobal(L, "Creature_ClassAnimationStatesTranslator");
        if (lua_istable(L, -1))
        {
            out.tableName = "Creature_ClassAnimationStatesTranslator";
            tableIndex = lua_gettop(L);
        }
        else
        {
            lua_pop(L, 1);
            return false;
        }
    }

    if (!LuaGetFieldTable(L, tableIndex, "States"))
    {
        lua_pop(L, 1); // translator table
        return false;
    }
    int statesTable = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, statesTable) != 0)
    {
        LuaAnimStateEntry entry;

        if (lua_isstring(L, -2))
            entry.key = lua_tostring(L, -2);
        else
            entry.key = "<state>";

        if (lua_istable(L, -1))
        {
            std::string name;
            if (LuaGetFieldString(L, -1, "Name", name))
                entry.name = name;
            else
                entry.name = entry.key;

            if (LuaGetFieldTable(L, -1, "StancesFilter"))
            {
                LuaReadStringArray(L, -1, entry.stances);
                lua_pop(L, 1);
            }
            if (LuaGetFieldTable(L, -1, "ActionsFilter"))
            {
                LuaReadStringArray(L, -1, entry.actions);
                lua_pop(L, 1);
            }
        }
        else
        {
            entry.name = entry.key;
        }

        out.states.push_back(entry);
        lua_pop(L, 1);
    }

    lua_pop(L, 2); // states table + translator table
    return !out.states.empty();
}

static bool TryAnimPath(const std::string& dir, const std::string& name, std::string& outPath)
{
    if (dir.empty()) return false;
    std::string path = dir;
    if (!path.empty() && path[path.size() - 1] != '\\' && path[path.size() - 1] != '/')
        path += "\\";
    path += name;
    if (FileExistsA_SimpleAnim(path.c_str()))
    {
        outPath = path;
        return true;
    }
    return false;
}

static std::string GetLevelRootFromBaseDir(const std::string& baseDir)
{
    if (baseDir.empty()) return std::string();
    std::string lower = baseDir;
    for (size_t i = 0; i < lower.size(); ++i)
        lower[i] = (char)tolower(lower[i]);
    const char* markerA = "\\sub_blocks1";
    const char* markerB = "/sub_blocks1";
    const char* markerC = "\\sub_blocks2";
    const char* markerD = "/sub_blocks2";
    size_t pos = lower.find(markerA);
    if (pos == std::string::npos) pos = lower.find(markerB);
    if (pos == std::string::npos) pos = lower.find(markerC);
    if (pos == std::string::npos) pos = lower.find(markerD);
    if (pos == std::string::npos) return std::string();
    return baseDir.substr(0, pos);
}

bool ResolveLuaAnimClipPath(const LuaScriptContext& ctx, const std::string& clipName, std::string& outPath)
{
    std::string name = clipName;
    if (name.empty()) return false;
    if (!EndsWithNoCaseAnim(name.c_str(), ".json"))
        name += ".json";

    if (IsAbsolutePathAnim(name.c_str()))
    {
        if (FileExistsA_SimpleAnim(name.c_str()))
        {
            outPath = name;
            return true;
        }
        return false;
    }

    // Global GameFiles paths
    if (TryAnimPath(ctx.gameFilesRoot + "animations", name, outPath)) return true;
    if (TryAnimPath(ctx.gameFilesRoot + "srcjson", name, outPath)) return true;

    // Level-relative paths (common for parsed PAKs)
    std::string levelRoot = GetLevelRootFromBaseDir(ctx.baseDir);
    if (!levelRoot.empty())
    {
        if (TryAnimPath(levelRoot + "\\animations", name, outPath)) return true;
        if (TryAnimPath(levelRoot + "\\srcjson", name, outPath)) return true;
    }

    // As a last resort, try next to the Lua file.
    if (TryAnimPath(ctx.baseDir, name, outPath)) return true;
    return false;
}

int LuaAbsIndex(lua_State* L, int idx)
{
    if (idx < 0)
        idx = lua_gettop(L) + idx + 1;
    return idx;
}

bool LuaGetFieldString(lua_State* L, int idx, const char* name, std::string& out)
{
    idx = LuaAbsIndex(L, idx);
    lua_getfield(L, idx, name);
    if (lua_isstring(L, -1))
    {
        out = lua_tostring(L, -1);
        lua_pop(L, 1);
        return true;
    }
    lua_pop(L, 1);
    return false;
}

bool LuaGetFieldNumber(lua_State* L, int idx, const char* name, float& out)
{
    idx = LuaAbsIndex(L, idx);
    lua_getfield(L, idx, name);
    if (lua_isnumber(L, -1))
    {
        out = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        return true;
    }
    lua_pop(L, 1);
    return false;
}

bool LuaGetFieldBool(lua_State* L, int idx, const char* name, bool& out)
{
    idx = LuaAbsIndex(L, idx);
    lua_getfield(L, idx, name);
    if (lua_isboolean(L, -1))
    {
        out = (lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);
        return true;
    }
    lua_pop(L, 1);
    return false;
}

bool LuaGetFieldTable(lua_State* L, int idx, const char* name)
{
    idx = LuaAbsIndex(L, idx);
    lua_getfield(L, idx, name);
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool LuaGetStringFromValue(lua_State* L, int idx, std::string& out)
{
    idx = LuaAbsIndex(L, idx);
    if (lua_isstring(L, idx))
    {
        out = lua_tostring(L, idx);
        return true;
    }
    if (lua_istable(L, idx))
    {
        lua_rawgeti(L, idx, 1);
        if (lua_isstring(L, -1))
        {
            out = lua_tostring(L, -1);
            lua_pop(L, 1);
            return true;
        }
        lua_pop(L, 1);
    }
    return false;
}

bool LuaGetNumberFromValue(lua_State* L, int idx, float& out)
{
    idx = LuaAbsIndex(L, idx);
    if (lua_isnumber(L, idx))
    {
        out = (float)lua_tonumber(L, idx);
        return true;
    }
    if (lua_istable(L, idx))
    {
        lua_rawgeti(L, idx, 1);
        if (lua_isnumber(L, -1))
        {
            out = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            return true;
        }
        lua_pop(L, 1);
    }
    return false;
}
