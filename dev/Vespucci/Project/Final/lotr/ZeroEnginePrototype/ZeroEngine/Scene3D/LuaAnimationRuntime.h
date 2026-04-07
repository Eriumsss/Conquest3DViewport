// LuaAnimationRuntime.h — Pandemic's Animation Brain, Resurrected
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Runtime data structures for the Lua-based animation system. This is the
// C++ side that Pandemic's Lua scripts drove at runtime — AnimTable lookups,
// state-to-clip resolution, filter evaluation, stance/action filtering.
// The whole pipeline: Lua defines the graph → parser fills these structs →
// runtime evaluates them frame by frame → poses come out.
//
// "Fall seven times, stand up eight." — Japanese proverb
// The number of times the Lua clip resolution produced a null pointer
// because a CRC hash couldn't resolve to a clip name: hundreds. Each
// time I traced the failure, added a fallback, tried again. The system
// stands now. Wobbly as fuck, but standing.
//
// AnimStatesTranslator, AnimTable, AnimStateEntry — these are OUR names
// for concepts that Pandemic named internally but we only know through
// string fragments found during .exe analysis. Their naming was probably
// different. Their architecture was definitely the same.
// -----------------------------------------------------------------------
#ifndef LUA_ANIMATION_RUNTIME_H
#define LUA_ANIMATION_RUNTIME_H

#include <string>
#include <set>
#include <map>
#include <vector>

struct lua_State;
class Scene3DRenderer;
struct JsonAnimClip;
class hkaSkeleton;

struct LuaScriptContext
{
    std::string baseDir;
    std::string gameFilesRoot;
    std::set<std::string> loadedModules;
    std::set<std::string> warnedApis;
    unsigned int rngState;
    Scene3DRenderer* renderer;

    LuaScriptContext();
};

struct LuaAnimTableEntry
{
    std::string key;
    std::vector<std::string> clips;
    std::string sourceTable;
};

struct LuaAnimTableInfo
{
    std::string tableName;                 /* AnimTableName */
    std::vector<std::string> tableNames;   /* AnimTableNames */
    std::vector<std::string> tableUsed;    /* AnimTableUsed */
    std::vector<LuaAnimTableEntry> entries;/* merged entries (last table wins) */
    std::vector<std::string> missingClips;
    std::vector<std::string> missingTables;
};

struct LuaAnimStateEntry
{
    std::string key;
    std::string name;
    std::vector<std::string> stances;
    std::vector<std::string> actions;
};

struct LuaAnimStatesTranslatorInfo
{
    std::string tableName;
    std::vector<LuaAnimStateEntry> states;
    int selectedStateIndex;  /* -1 = none, else index into states array */
    
    LuaAnimStatesTranslatorInfo() : selectedStateIndex(-1) {}
};

std::string GetDirectoryFromPathAnim(const char* path);
std::string FindGameFilesRootFromPathAnim(const char* path);

bool LuaRunFile(lua_State* L, const char* path, Scene3DRenderer* renderer);
bool LuaLoadModule(lua_State* L, LuaScriptContext* ctx, const char* moduleName);
void LuaRegisterBindings(lua_State* L, LuaScriptContext* ctx);
bool LuaExtractAnimTable(lua_State* L, std::map<std::string, std::string>& outMap, std::string& outTableName, Scene3DRenderer* renderer);
bool LuaExtractAnimTableInfo(lua_State* L, LuaAnimTableInfo& out, const LuaScriptContext& ctx, Scene3DRenderer* renderer);
bool LuaExtractAnimStatesTranslatorInfo(lua_State* L, LuaAnimStatesTranslatorInfo& out, Scene3DRenderer* renderer);
bool ResolveLuaAnimClipPath(const LuaScriptContext& ctx, const std::string& clipName, std::string& outPath);
JsonAnimClip* LoadLuaAnimClipChecked(const std::string& clipPath,
                                     const hkaSkeleton* skeleton,
                                     int jsonDecodeMode,
                                     int type2PackingMode,
                                     bool strictRigCoverage);

int LuaAbsIndex(lua_State* L, int idx);
bool LuaGetFieldString(lua_State* L, int idx, const char* name, std::string& out);
bool LuaGetFieldNumber(lua_State* L, int idx, const char* name, float& out);
bool LuaGetFieldBool(lua_State* L, int idx, const char* name, bool& out);
bool LuaGetFieldTable(lua_State* L, int idx, const char* name);
bool LuaGetStringFromValue(lua_State* L, int idx, std::string& out);
bool LuaGetNumberFromValue(lua_State* L, int idx, float& out);

#endif
