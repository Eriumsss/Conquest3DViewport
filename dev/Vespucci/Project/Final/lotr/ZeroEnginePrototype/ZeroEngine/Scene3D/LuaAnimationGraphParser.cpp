// LuaAnimationGraphParser.cpp — Summoning Pandemic's Lua Ghosts
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Parses Conquest's Lua animation graph scripts into C++ runtime structs.
// Opens a lua_State, loads the bytecode from the PAK sub-block, executes
// it in a sandboxed environment, and walks the resulting Lua tables to
// extract states, transitions, conditions, blend nodes, actions. Every
// piece of Pandemic's animation logic that was defined in Lua gets
// translated into our AnimationGraphRuntime representation.
//
// The parsing is fragile as fuck because Pandemic's Lua scripts reference
// clip names by CRC hash, and if we can't resolve the hash to a string,
// the state gets a null clip and the character T-poses. About 15% of
// clips still can't resolve. The CRC→string dictionary is incomplete
// because some animation names only existed in Pandemic's internal
// build tools and never made it to the shipped disc.
// -----------------------------------------------------------------------

#include "Scene3DRendererInternal.h"
#include "LuaAnimationRuntime.h"
#include "AnimationGraphRuntimeTypes.h"
#include "LuaAnimationGraphParser.h"
#include <string.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

JsonAnimClip* LoadJsonAnimClip(const char* path,
                               const hkaSkeleton* skeleton,
                               int decodeMode,
                               int type2PackingMode);

static int FindGraphParamIndexLocal(const AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)graph.params.size(); ++i)
    {
        if (_stricmp(graph.params[i].name.c_str(), name) == 0)
            return i;
    }
    return -1;
}

static int FindGraphStateIndexLocal(const AnimationGraphRuntime& graph, const char* name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)graph.states.size(); ++i)
    {
        if (_stricmp(graph.states[i].name.c_str(), name) == 0)
            return i;
    }
    return -1;
}

static int FindSkeletonBoneIndexCI_Local(const hkaSkeleton* skeleton, const char* name)
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

static float GetClipFrameTimeLocal(const JsonAnimClip* clip)
{
    if (clip && clip->frameTime > 0.0f) return clip->frameTime;
    return 1.0f / 30.0f;
}

static float GetClipDurationLocal(const JsonAnimClip* clip)
{
    if (!clip) return 0.0f;
    float frameTime = GetClipFrameTimeLocal(clip);
    float duration = clip->duration;
    if (duration <= 0.0f)
    {
        int frameCount = (clip->frameCount > 0) ? clip->frameCount : 1;
        duration = frameTime * (float)(frameCount > 1 ? (frameCount - 1) : 1);
    }
    return duration;
}

static void EnsureGraphParamLocal(AnimationGraphRuntime& graph, const std::string& name, int type, float defaultValue)
{
    if (name.empty()) return;
    int idx = FindGraphParamIndexLocal(graph, name.c_str());
    if (idx >= 0) return;
    GraphParam p;
    p.name = name;
    p.type = type;
    p.f = defaultValue;
    p.i = (int)defaultValue;
    p.b = (defaultValue != 0.0f);
    graph.params.push_back(p);
}

static bool LuaGetFieldIntLocal(lua_State* L, int idx, const char* name, int& out)
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

static unsigned int HashStringLowerLocal(const char* text)
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

static std::string BuildGraphParamNameFromVar(lua_State* L, int idx)
{
    idx = LuaAbsIndex(L, idx);
    if (lua_isstring(L, idx))
        return std::string(lua_tostring(L, idx));

    if (!lua_istable(L, idx))
        return std::string();

    int cvIndex = -1;
    int cvSource = -1;
    if (LuaGetFieldIntLocal(L, idx, "Index", cvIndex))
    {
        LuaGetFieldIntLocal(L, idx, "Source", cvSource);
        char buf[64];
        if (cvSource >= 0)
            sprintf_s(buf, "CV_%d_S%d", cvIndex, cvSource);
        else
            sprintf_s(buf, "CV_%d", cvIndex);
        return std::string(buf);
    }

    lua_rawgeti(L, idx, 1);
    std::string base;
    if (lua_isstring(L, -1))
        base = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (base.empty())
        return std::string();

    std::string name = base;
    for (int i = 2; i <= 4; ++i)
    {
        lua_rawgeti(L, idx, i);
        if (lua_isstring(L, -1))
        {
            name += ":";
            name += lua_tostring(L, -1);
        }
        else if (lua_isnumber(L, -1))
        {
            char nbuf[64];
            sprintf_s(nbuf, "%d", (int)lua_tonumber(L, -1));
            name += ":";
            name += nbuf;
        }
        lua_pop(L, 1);
    }
    return name;
}

static bool LuaValueToFloat(lua_State* L, int idx, float& out)
{
    idx = LuaAbsIndex(L, idx);
    if (lua_isnumber(L, idx))
    {
        out = (float)lua_tonumber(L, idx);
        return true;
    }
    if (lua_isboolean(L, idx))
    {
        out = lua_toboolean(L, idx) ? 1.0f : 0.0f;
        return true;
    }
    return false;
}

static bool LuaBuildArgFromValue(lua_State* L, int idx, GraphActionArg& out)
{
    idx = LuaAbsIndex(L, idx);
    if (lua_isnumber(L, idx))
    {
        float v = (float)lua_tonumber(L, idx);
        out.type = GRAPH_ACT_ARG_FLOAT;
        out.f = v;
        out.i = (int)v;
        out.b = (v != 0.0f);
        return true;
    }
    if (lua_isboolean(L, idx))
    {
        bool b = lua_toboolean(L, idx) != 0;
        out.type = GRAPH_ACT_ARG_BOOL;
        out.b = b;
        out.f = b ? 1.0f : 0.0f;
        out.i = b ? 1 : 0;
        return true;
    }
    if (lua_isstring(L, idx))
    {
        out.type = GRAPH_ACT_ARG_STRING;
        out.s = lua_tostring(L, idx);
        return true;
    }
    return false;
}

static bool LuaParseActionTable(lua_State* L, int idx, GraphAction& out)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return false;

    lua_rawgeti(L, idx, 1);
    if (!lua_isstring(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }
    out.name = lua_tostring(L, -1);
    lua_pop(L, 1);

    int len = (int)lua_objlen(L, idx);
    for (int i = 2; i <= len; ++i)
    {
        lua_rawgeti(L, idx, i);
        GraphActionArg arg;
        if (LuaBuildArgFromValue(L, -1, arg))
            out.args.push_back(arg);
        lua_pop(L, 1);
    }

    return !out.name.empty();
}

static void LuaParseActionList(lua_State* L, int idx, std::vector<GraphAction>& out)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return;

    lua_rawgeti(L, idx, 1);
    bool firstIsString = lua_isstring(L, -1);
    lua_pop(L, 1);

    if (firstIsString)
    {
        GraphAction act;
        if (LuaParseActionTable(L, idx, act))
            out.push_back(act);
        return;
    }

    int len = (int)lua_objlen(L, idx);
    for (int i = 1; i <= len; ++i)
    {
        lua_rawgeti(L, idx, i);
        if (lua_istable(L, -1))
            LuaParseActionList(L, -1, out);
        lua_pop(L, 1);
    }
}

static bool LuaParseEventTarget(lua_State* L, int idx, std::string& name, std::vector<GraphActionArg>& args)
{
    idx = LuaAbsIndex(L, idx);
    name.clear();
    args.clear();

    if (lua_isstring(L, idx))
    {
        name = lua_tostring(L, idx);
        return !name.empty();
    }
    if (lua_isnumber(L, idx))
    {
        char buf[64];
        sprintf_s(buf, "%d", (int)lua_tonumber(L, idx));
        name = buf;
        return true;
    }
    if (!lua_istable(L, idx))
        return false;

    lua_rawgeti(L, idx, 1);
    if (lua_isstring(L, -1))
    {
        name = lua_tostring(L, -1);
    }
    else if (lua_isnumber(L, -1))
    {
        char buf[64];
        sprintf_s(buf, "%d", (int)lua_tonumber(L, -1));
        name = buf;
    }
    lua_pop(L, 1);
    if (name.empty())
        return false;

    int len = (int)lua_objlen(L, idx);
    for (int i = 2; i <= len; ++i)
    {
        lua_rawgeti(L, idx, i);
        GraphActionArg arg;
        if (LuaBuildArgFromValue(L, -1, arg))
            args.push_back(arg);
        lua_pop(L, 1);
    }
    return true;
}

static void LuaParseOnEventActions(lua_State* L, int idx, AnimationGraphRuntime& graph, GraphState& state)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return;

    int len = (int)lua_objlen(L, idx);
    for (int i = 1; i <= len; ++i)
    {
        lua_rawgeti(L, idx, i);
        if (lua_istable(L, -1))
        {
            int entry = LuaAbsIndex(L, -1);
            std::string eventName;
            std::vector<GraphActionArg> eventArgs;
            lua_rawgeti(L, entry, 1);
            bool firstIsString = lua_isstring(L, -1);
            bool firstIsNumber = lua_isnumber(L, -1);
            bool gotEvent = false;
            if (firstIsString || firstIsNumber)
                gotEvent = LuaParseEventTarget(L, -1, eventName, eventArgs);
            lua_pop(L, 1);

            if (gotEvent && !eventName.empty())
            {
                EnsureGraphParamLocal(graph, eventName, Scene3DRenderer::GRAPH_PARAM_TRIGGER, 0.0f);
                std::vector<GraphAction> actions;
                lua_rawgeti(L, entry, 2);
                if (lua_istable(L, -1))
                    LuaParseActionList(L, -1, actions);
                lua_pop(L, 1);

                if (!actions.empty())
                {
                    GraphStateEventAction ea;
                    ea.eventName = eventName;
                    ea.actions = actions;
                    state.onEventActions.push_back(ea);
                }
            }
            else
            {
                LuaParseOnEventActions(L, entry, graph, state);
            }
        }
        lua_pop(L, 1);
    }
}

static void LuaParseStateEvents(lua_State* L, int idx, AnimationGraphRuntime& graph, GraphState& state)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return;

    int len = (int)lua_objlen(L, idx);
    for (int i = 1; i <= len; ++i)
    {
        lua_rawgeti(L, idx, i);
        if (lua_istable(L, -1))
        {
            int evtTable = LuaAbsIndex(L, -1);
            GraphStateEvent evt;

            int type = 0;
            if (LuaGetFieldIntLocal(L, evtTable, "Type", type))
                evt.type = type;

            lua_getfield(L, evtTable, "Time");
            if (lua_isnumber(L, -1) || lua_isboolean(L, -1))
            {
                float t = 0.0f;
                if (LuaValueToFloat(L, -1, t))
                    evt.time = t;
            }
            lua_pop(L, 1);

            lua_getfield(L, evtTable, "PostEvent");
            LuaParseEventTarget(L, -1, evt.postEvent, evt.postArgs);
            lua_pop(L, 1);

            lua_getfield(L, evtTable, "OnEvent");
            std::vector<GraphActionArg> dummyArgs;
            LuaParseEventTarget(L, -1, evt.onEvent, dummyArgs);
            lua_pop(L, 1);

            if (!evt.postEvent.empty())
                EnsureGraphParamLocal(graph, evt.postEvent, Scene3DRenderer::GRAPH_PARAM_TRIGGER, 0.0f);
            if (!evt.onEvent.empty())
                EnsureGraphParamLocal(graph, evt.onEvent, Scene3DRenderer::GRAPH_PARAM_TRIGGER, 0.0f);

            if (!evt.postEvent.empty() || !evt.onEvent.empty())
                state.events.push_back(evt);
        }
        lua_pop(L, 1);
    }
}

static void LuaParseStateActions(lua_State* L, int idx, AnimationGraphRuntime& graph, GraphState& state)
{
    idx = LuaAbsIndex(L, idx);
    if (!lua_istable(L, idx))
        return;

    if (LuaGetFieldTable(L, idx, "OnEnter"))
    {
        LuaParseActionList(L, -1, state.onEnterActions);
        lua_pop(L, 1);
    }
    if (LuaGetFieldTable(L, idx, "OnExit"))
    {
        LuaParseActionList(L, -1, state.onExitActions);
        lua_pop(L, 1);
    }
    if (LuaGetFieldTable(L, idx, "OnEvent"))
    {
        LuaParseOnEventActions(L, -1, graph, state);
        lua_pop(L, 1);
    }
}

static void AppendConditionSetsAnd(std::vector< std::vector<GraphCondition> >& base,
                                   const std::vector< std::vector<GraphCondition> >& add)
{
    if (add.empty())
        return;
    if (base.empty())
    {
        base = add;
        return;
    }
    std::vector< std::vector<GraphCondition> > out;
    for (size_t i = 0; i < base.size(); ++i)
    {
        for (size_t j = 0; j < add.size(); ++j)
        {
            std::vector<GraphCondition> merged = base[i];
            merged.insert(merged.end(), add[j].begin(), add[j].end());
            out.push_back(merged);
        }
    }
    base.swap(out);
}

static void AppendConditionSetsOr(std::vector< std::vector<GraphCondition> >& base,
                                  const std::vector< std::vector<GraphCondition> >& add)
{
    if (add.empty())
        return;
    base.insert(base.end(), add.begin(), add.end());
}

static void BuildSimpleConditionSets(lua_State* L,
                                     int op,
                                     int exprIndex,
                                     AnimationGraphRuntime& graph,
                                     std::vector< std::vector<GraphCondition> >& outSets)
{
    outSets.clear();
    exprIndex = LuaAbsIndex(L, exprIndex);

    lua_rawgeti(L, exprIndex, 2);
    std::string paramName = BuildGraphParamNameFromVar(L, -1);
    lua_pop(L, 1);
    if (paramName.empty())
        return;

    if (op == 0) // OP_TRUE
    {
        EnsureGraphParamLocal(graph, paramName, Scene3DRenderer::GRAPH_PARAM_FLOAT, 0.0f);
        int paramIndex = FindGraphParamIndexLocal(graph, paramName.c_str());
        if (paramIndex < 0)
            return;
        GraphCondition cond;
        cond.paramIndex = paramIndex;
        cond.op = GRAPH_COND_TRUE;
        outSets.push_back(std::vector<GraphCondition>(1, cond));
        return;
    }
    if (op == 1) // OP_FALSE
    {
        EnsureGraphParamLocal(graph, paramName, Scene3DRenderer::GRAPH_PARAM_FLOAT, 0.0f);
        int paramIndex = FindGraphParamIndexLocal(graph, paramName.c_str());
        if (paramIndex < 0)
            return;
        GraphCondition cond;
        cond.paramIndex = paramIndex;
        cond.op = GRAPH_COND_FALSE;
        outSets.push_back(std::vector<GraphCondition>(1, cond));
        return;
    }

    if (op == 2 || op == 3) // OP_IN_RANGE / OP_NOT_IN_RANGE
    {
        EnsureGraphParamLocal(graph, paramName, Scene3DRenderer::GRAPH_PARAM_FLOAT, 0.0f);
        int paramIndex = FindGraphParamIndexLocal(graph, paramName.c_str());
        if (paramIndex < 0)
            return;
        GraphCondition cond;
        cond.paramIndex = paramIndex;

        float minV = 0.0f, maxV = 0.0f;
        lua_rawgeti(L, exprIndex, 3);
        bool okMin = LuaValueToFloat(L, -1, minV);
        lua_pop(L, 1);
        lua_rawgeti(L, exprIndex, 4);
        bool okMax = LuaValueToFloat(L, -1, maxV);
        lua_pop(L, 1);
        if (!okMin || !okMax)
            return;

        if (op == 2)
        {
            GraphCondition c0 = cond;
            c0.op = GRAPH_COND_GREATER_EQUAL;
            c0.f = minV;
            GraphCondition c1 = cond;
            c1.op = GRAPH_COND_LESS_EQUAL;
            c1.f = maxV;
            std::vector<GraphCondition> both;
            both.push_back(c0);
            both.push_back(c1);
            outSets.push_back(both);
        }
        else
        {
            GraphCondition c0 = cond;
            c0.op = GRAPH_COND_LESS;
            c0.f = minV;
            std::vector<GraphCondition> left;
            left.push_back(c0);
            outSets.push_back(left);

            GraphCondition c1 = cond;
            c1.op = GRAPH_COND_GREATER;
            c1.f = maxV;
            std::vector<GraphCondition> right;
            right.push_back(c1);
            outSets.push_back(right);
        }
        return;
    }

    int paramType = Scene3DRenderer::GRAPH_PARAM_FLOAT;
    if (op == 10 || op == 11)
        paramType = Scene3DRenderer::GRAPH_PARAM_INT;

    bool valueIsString = false;
    unsigned int valueHash = 0;
    float val = 0.0f;
    lua_rawgeti(L, exprIndex, 3);
    if (lua_isstring(L, -1))
    {
        valueIsString = true;
        valueHash = HashStringLowerLocal(lua_tostring(L, -1));
        paramType = Scene3DRenderer::GRAPH_PARAM_INT;
    }
    else
    {
        bool okVal = LuaValueToFloat(L, -1, val);
        if (!okVal)
        {
            lua_pop(L, 1);
            return;
        }
    }
    lua_pop(L, 1);

    EnsureGraphParamLocal(graph, paramName, paramType, 0.0f);
    int paramIndex = FindGraphParamIndexLocal(graph, paramName.c_str());
    if (paramIndex < 0)
        return;

    GraphCondition cond;
    cond.paramIndex = paramIndex;

    switch (op)
    {
    case 4:  cond.op = GRAPH_COND_GREATER; break;
    case 5:  cond.op = GRAPH_COND_LESS; break;
    case 6:  cond.op = GRAPH_COND_LESS_EQUAL; break;
    case 7:  cond.op = GRAPH_COND_GREATER_EQUAL; break;
    case 8:  cond.op = GRAPH_COND_EQUAL; break;
    case 9:  cond.op = GRAPH_COND_NOT_EQUAL; break;
    case 10: cond.op = GRAPH_COND_TEST_BITS_TRUE; break;
    case 11: cond.op = GRAPH_COND_TEST_BITS_FALSE; break;
    default: cond.op = GRAPH_COND_EQUAL; break;
    }
    if (valueIsString)
    {
        cond.i = (int)valueHash;
        cond.f = (float)cond.i;
        cond.b = (cond.i != 0);
    }
    else
    {
        cond.f = val;
        cond.i = (int)val;
        cond.b = (val != 0.0f);
    }

    outSets.push_back(std::vector<GraphCondition>(1, cond));
}

static void ExpandLuaExprToConditionSets(lua_State* L,
                                         int exprIndex,
                                         AnimationGraphRuntime& graph,
                                         std::vector< std::vector<GraphCondition> >& outSets)
{
    outSets.clear();
    exprIndex = LuaAbsIndex(L, exprIndex);
    if (!lua_istable(L, exprIndex))
        return;

    lua_rawgeti(L, exprIndex, 1);
    if (!lua_isnumber(L, -1))
    {
        lua_pop(L, 1);
        return;
    }
    int op = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);

    if (op == 256 || op == 257 || op == 258) // OP_AND / OP_OR / OP_AND_OR_AND
    {
        if (op == 258)
        {
            std::vector< std::vector<GraphCondition> > leftA, leftB, rightA, rightB;
            lua_rawgeti(L, exprIndex, 2);
            ExpandLuaExprToConditionSets(L, -1, graph, leftA);
            lua_pop(L, 1);
            lua_rawgeti(L, exprIndex, 3);
            ExpandLuaExprToConditionSets(L, -1, graph, leftB);
            lua_pop(L, 1);
            std::vector< std::vector<GraphCondition> > left = leftA;
            AppendConditionSetsAnd(left, leftB);

            lua_rawgeti(L, exprIndex, 4);
            ExpandLuaExprToConditionSets(L, -1, graph, rightA);
            lua_pop(L, 1);
            lua_rawgeti(L, exprIndex, 5);
            ExpandLuaExprToConditionSets(L, -1, graph, rightB);
            lua_pop(L, 1);
            std::vector< std::vector<GraphCondition> > right = rightA;
            AppendConditionSetsAnd(right, rightB);

            AppendConditionSetsOr(outSets, left);
            AppendConditionSetsOr(outSets, right);
            return;
        }

        if (op == 256)
        {
            std::vector< std::vector<GraphCondition> > accum;
            for (int i = 2; i <= 5; ++i)
            {
                lua_rawgeti(L, exprIndex, i);
                if (lua_istable(L, -1))
                {
                    std::vector< std::vector<GraphCondition> > child;
                    ExpandLuaExprToConditionSets(L, -1, graph, child);
                    AppendConditionSetsAnd(accum, child);
                }
                lua_pop(L, 1);
            }
            outSets = accum;
            return;
        }

        if (op == 257)
        {
            std::vector< std::vector<GraphCondition> > accum;
            for (int i = 2; i <= 5; ++i)
            {
                lua_rawgeti(L, exprIndex, i);
                if (lua_istable(L, -1))
                {
                    std::vector< std::vector<GraphCondition> > child;
                    ExpandLuaExprToConditionSets(L, -1, graph, child);
                    AppendConditionSetsOr(accum, child);
                }
                lua_pop(L, 1);
            }
            outSets = accum;
            return;
        }
    }

    BuildSimpleConditionSets(L, op, exprIndex, graph, outSets);
}

static void ParseLuaConditionsList(lua_State* L,
                                   int listIndex,
                                   AnimationGraphRuntime& graph,
                                   std::vector< std::vector<GraphCondition> >& outSets)
{
    outSets.clear();
    listIndex = LuaAbsIndex(L, listIndex);
    if (!lua_istable(L, listIndex))
        return;

    int len = (int)lua_objlen(L, listIndex);
    if (len <= 0)
    {
        ExpandLuaExprToConditionSets(L, listIndex, graph, outSets);
        return;
    }

    std::vector< std::vector<GraphCondition> > accum;
    for (int i = 1; i <= len; ++i)
    {
        lua_rawgeti(L, listIndex, i);
        if (lua_istable(L, -1))
        {
            std::vector< std::vector<GraphCondition> > exprSets;
            ExpandLuaExprToConditionSets(L, -1, graph, exprSets);
            AppendConditionSetsAnd(accum, exprSets);
        }
        lua_pop(L, 1);
    }
    outSets = accum;
}

static std::string ResolveAnimNameToClip(const std::map<std::string, std::string>& animMap, const std::string& animName)
{
    std::map<std::string, std::string>::const_iterator it = animMap.find(animName);
    if (it != animMap.end())
        return it->second;
    return animName;
}

static JsonAnimClip* LoadLuaAnimClipChecked(const std::string& clipPath,
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

static bool LuaParsePickAnimEntry(lua_State* L,
                                  int animTable,
                                  const std::map<std::string, std::string>& animMap,
                                  const LuaScriptContext& ctx,
                                  const hkaSkeleton* skeleton,
                                  int jsonDecodeMode,
                                  int type2PackingMode,
                                  bool strictRigCoverage,
                                  GraphPickAnim& outAnim)
{
    animTable = LuaAbsIndex(L, animTable);
    std::string animName;
    if (!LuaGetFieldString(L, animTable, "Name", animName))
        return false;

    std::string clipKey = ResolveAnimNameToClip(animMap, animName);
    std::string clipPath;
    if (!ResolveLuaAnimClipPath(ctx, clipKey, clipPath))
        return false;

    JsonAnimClip* clip = LoadLuaAnimClipChecked(clipPath, skeleton, jsonDecodeMode, type2PackingMode, strictRigCoverage);
    if (!clip)
        return false;

    outAnim.clipPath = clipPath;
    outAnim.clip = clip;
    outAnim.duration = GetClipDurationLocal(clip);
    outAnim.frameTime = GetClipFrameTimeLocal(clip);

    bool loop = outAnim.loop;
    if (LuaGetFieldBool(L, animTable, "Looping", loop))
        outAnim.loop = loop;

    float rate = outAnim.speed;
    if (LuaGetFieldNumber(L, animTable, "Rate", rate))
        outAnim.speed = rate;

    std::string doneEvent;
    if (LuaGetFieldString(L, animTable, "DoneEvent", doneEvent))
        outAnim.doneEvent = doneEvent;

    return true;
}

static void AccumulateLuaBlendPartMask(lua_State* L,
                                       int partIndex,
                                       const hkaSkeleton* skeleton,
                                       std::vector<float>& outMask,
                                       float weight)
{
    if (!skeleton) return;
    partIndex = LuaAbsIndex(L, partIndex);
    if (!lua_istable(L, partIndex)) return;

    const int boneCount = skeleton->m_numBones;
    if (boneCount <= 0) return;
    if (outMask.empty())
        outMask.assign(boneCount, 0.0f);

    lua_pushnil(L);
    while (lua_next(L, partIndex) != 0)
    {
        int boneIndex = -1;
        if (lua_type(L, -2) == LUA_TSTRING)
        {
            const char* boneName = lua_tostring(L, -2);
            boneIndex = FindSkeletonBoneIndexCI_Local(skeleton, boneName);
        }
        else if (lua_type(L, -2) == LUA_TNUMBER)
        {
            int idx = (int)lua_tonumber(L, -2);
            if (idx > 0)
                boneIndex = idx - 1;
        }

        if (boneIndex >= 0 && boneIndex < boneCount && lua_isnumber(L, -1))
        {
            float v = (float)lua_tonumber(L, -1) * weight;
            float sum = outMask[boneIndex] + v;
            outMask[boneIndex] = (sum > 1.0f) ? 1.0f : sum;
        }
        lua_pop(L, 1);
    }
}

static void BuildMaskFromLuaBlendParts(lua_State* L,
                                       int blendPartsIndex,
                                       const hkaSkeleton* skeleton,
                                       std::vector<float>& outMask)
{
    blendPartsIndex = LuaAbsIndex(L, blendPartsIndex);
    if (!lua_istable(L, blendPartsIndex) || !skeleton) return;

    int len = (int)lua_objlen(L, blendPartsIndex);
    if (len <= 0)
    {
        AccumulateLuaBlendPartMask(L, blendPartsIndex, skeleton, outMask, 1.0f);
        return;
    }

    for (int i = 1; i <= len; ++i)
    {
        lua_rawgeti(L, blendPartsIndex, i);
        if (lua_istable(L, -1))
        {
            float partWeight = 1.0f;
            lua_rawgeti(L, -1, 1);
            if (lua_isnumber(L, -1))
                partWeight = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);

            lua_rawgeti(L, -1, 2);
            if (lua_istable(L, -1))
            {
                AccumulateLuaBlendPartMask(L, -1, skeleton, outMask, partWeight);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

struct LuaTransitionSpec
{
    std::string fromName;
    std::string toName;
    bool anyState;
    float duration;
    float exitTime;
    float maxTime;
    int priority;
    bool allowInterrupt;
    int syncHint;
    int blendMode;
    int rotMode;
    int layerRoot;
    std::vector<float> mask;
    std::vector<GraphCondition> conditions;

    LuaTransitionSpec()
        : anyState(false),
          duration(0.1f),
          exitTime(-1.0f),
          maxTime(-1.0f),
          priority(0),
          allowInterrupt(true),
          syncHint(0),
          blendMode(Scene3DRenderer::JSON_BLEND_CROSSFADE),
          rotMode(Scene3DRenderer::JSON_BLEND_ROT_SLERP),
          layerRoot(-1)
    {}
};

static bool LuaGetFieldStringOrValue(lua_State* L, int idx, const char* name, std::string& out)
{
    idx = LuaAbsIndex(L, idx);
    lua_getfield(L, idx, name);
    if (lua_isstring(L, -1))
    {
        out = lua_tostring(L, -1);
        lua_pop(L, 1);
        return true;
    }
    if (lua_istable(L, -1))
    {
        bool ok = LuaGetStringFromValue(L, -1, out);
        lua_pop(L, 1);
        return ok;
    }
    lua_pop(L, 1);
    return false;
}

static bool LuaHasField(lua_State* L, int idx, const char* name)
{
    idx = LuaAbsIndex(L, idx);
    lua_getfield(L, idx, name);
    bool ok = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return ok;
}

static int ParseTransitionBlendModeValue(float v)
{
    // GL_Globals: BLEND_NORMAL=0, BLEND_ADDITIVE=1, BLEND_ADDITIVE_INVERSE=2, BLEND_PRIORITY=3
    int iv = (int)v;
    if (iv == 1 || iv == 2) return Scene3DRenderer::JSON_BLEND_ADDITIVE;
    if (iv == 3) return Scene3DRenderer::JSON_BLEND_LAYERED;
    return Scene3DRenderer::JSON_BLEND_CROSSFADE;
}

static int ParseTransitionRotModeValue(float v)
{
    // GL_Globals doesn't define a rot mode; treat 1 as nlerp.
    int iv = (int)v;
    if (iv == 1) return Scene3DRenderer::JSON_BLEND_ROT_NLERP;
    return Scene3DRenderer::JSON_BLEND_ROT_SLERP;
}

static int ParseTransitionBlendModeString(const char* s)
{
    if (!s || !s[0]) return Scene3DRenderer::JSON_BLEND_CROSSFADE;
    if (_stricmp(s, "additive") == 0) return Scene3DRenderer::JSON_BLEND_ADDITIVE;
    if (_stricmp(s, "layered") == 0 || _stricmp(s, "mask") == 0 || _stricmp(s, "masked") == 0)
        return Scene3DRenderer::JSON_BLEND_LAYERED;
    return Scene3DRenderer::JSON_BLEND_CROSSFADE;
}

static int ParseTransitionRotModeString(const char* s)
{
    if (!s || !s[0]) return Scene3DRenderer::JSON_BLEND_ROT_SLERP;
    if (_stricmp(s, "nlerp") == 0) return Scene3DRenderer::JSON_BLEND_ROT_NLERP;
    return Scene3DRenderer::JSON_BLEND_ROT_SLERP;
}

static void LuaParseTransitionTable(lua_State* L,
                                    int trIndex,
                                    AnimationGraphRuntime& graph,
                                    const char* fromName,
                                    bool anyState,
                                    const hkaSkeleton* skeleton,
                                    std::vector<LuaTransitionSpec>& outSpecs)
{
    trIndex = LuaAbsIndex(L, trIndex);
    if (!lua_istable(L, trIndex))
        return;

    float typeVal = 0.0f;
    if (LuaGetFieldNumber(L, trIndex, "Type", typeVal))
    {
        if ((int)typeVal != 1)
            return;
    }

    std::string toName;
    if (!LuaGetFieldString(L, trIndex, "GotoState", toName))
        return;

    LuaTransitionSpec spec;
    if (fromName) spec.fromName = fromName;
    spec.toName = toName;
    spec.anyState = anyState;

    float blend = 0.1f;
    if (LuaGetFieldNumber(L, trIndex, "TransitionTime", blend))
        spec.duration = blend;
    else
        spec.duration = 0.1f;

    float minTime = -1.0f;
    if (LuaGetFieldNumber(L, trIndex, "MinTime", minTime) && minTime >= 0.0f)
    {
        spec.exitTime = minTime;
    }
    float maxTime = -1.0f;
    if (LuaGetFieldNumber(L, trIndex, "MaxTime", maxTime) && maxTime >= 0.0f)
    {
        spec.maxTime = maxTime;
    }

    float syncHint = 0.0f;
    if (LuaGetFieldNumber(L, trIndex, "SyncHint", syncHint))
    {
        spec.syncHint = (int)syncHint;
    }

    // Transition blend/rotation modes and optional layer root/mask.
    float blendModeNum = 0.0f;
    if (LuaGetFieldNumber(L, trIndex, "BlendMode", blendModeNum))
    {
        spec.blendMode = ParseTransitionBlendModeValue(blendModeNum);
    }
    else
    {
        std::string blendModeStr;
        if (LuaGetFieldString(L, trIndex, "BlendMode", blendModeStr))
            spec.blendMode = ParseTransitionBlendModeString(blendModeStr.c_str());
    }

    float rotModeNum = 0.0f;
    if (LuaGetFieldNumber(L, trIndex, "RotMode", rotModeNum))
    {
        spec.rotMode = ParseTransitionRotModeValue(rotModeNum);
    }
    else
    {
        std::string rotModeStr;
        if (LuaGetFieldString(L, trIndex, "RotMode", rotModeStr))
            spec.rotMode = ParseTransitionRotModeString(rotModeStr.c_str());
    }

    // Optional layer root (bone name or index)
    if (skeleton)
    {
        lua_getfield(L, trIndex, "LayerRoot");
        if (lua_isstring(L, -1))
        {
            const char* boneName = lua_tostring(L, -1);
            if (boneName && boneName[0])
                spec.layerRoot = FindSkeletonBoneIndexCI_Local(skeleton, boneName);
        }
        else if (lua_isnumber(L, -1))
        {
            int idx = (int)lua_tonumber(L, -1);
            if (idx > 0) spec.layerRoot = idx - 1;
        }
        lua_pop(L, 1);

        if (LuaGetFieldTable(L, trIndex, "BlendParts"))
        {
            BuildMaskFromLuaBlendParts(L, -1, skeleton, spec.mask);
            lua_pop(L, 1);
            if (!spec.mask.empty() && spec.blendMode == Scene3DRenderer::JSON_BLEND_CROSSFADE)
                spec.blendMode = Scene3DRenderer::JSON_BLEND_LAYERED;
        }
    }

    std::string onEvent;
    if (LuaGetFieldStringOrValue(L, trIndex, "OnEvent", onEvent))
    {
        if (_stricmp(onEvent.c_str(), "Update") == 0 || _stricmp(onEvent.c_str(), "Input") == 0)
        {
            // No trigger condition needed.
        }
        else if (onEvent.size() >= 5 && onEvent.substr(0, 5) == "DONE_")
        {
            spec.exitTime = -2.0f; // Mark as anim-done transition.
        }
        else
        {
            EnsureGraphParamLocal(graph, onEvent, Scene3DRenderer::GRAPH_PARAM_TRIGGER, 0.0f);
            int pidx = FindGraphParamIndexLocal(graph, onEvent.c_str());
            if (pidx >= 0)
            {
                GraphCondition cond;
                cond.paramIndex = pidx;
                cond.op = GRAPH_COND_TRUE;
                cond.b = true;
                spec.conditions.push_back(cond);
            }
        }
    }

    if (LuaGetFieldTable(L, trIndex, "Conditions"))
    {
        std::vector< std::vector<GraphCondition> > condSets;
        ParseLuaConditionsList(L, -1, graph, condSets);
        lua_pop(L, 1);

        if (!condSets.empty())
        {
            for (size_t i = 0; i < condSets.size(); ++i)
            {
                LuaTransitionSpec s = spec;
                s.conditions.insert(s.conditions.end(), condSets[i].begin(), condSets[i].end());
                outSpecs.push_back(s);
            }
            return;
        }
    }

    outSpecs.push_back(spec);
}

static void LuaParseTransitionsArray(lua_State* L,
                                     int tblIndex,
                                     AnimationGraphRuntime& graph,
                                     const char* fromName,
                                     bool anyState,
                                     const hkaSkeleton* skeleton,
                                     std::vector<LuaTransitionSpec>& outSpecs)
{
    tblIndex = LuaAbsIndex(L, tblIndex);
    if (!lua_istable(L, tblIndex))
        return;

    int len = (int)lua_objlen(L, tblIndex);
    for (int i = 1; i <= len; ++i)
    {
        lua_rawgeti(L, tblIndex, i);
        if (lua_istable(L, -1))
        {
            if (LuaHasField(L, -1, "GotoState") || LuaHasField(L, -1, "OnEvent"))
            {
                LuaParseTransitionTable(L, -1, graph, fromName, anyState, skeleton, outSpecs);
            }
            else
            {
                int innerLen = (int)lua_objlen(L, -1);
                for (int j = 1; j <= innerLen; ++j)
                {
                    lua_rawgeti(L, -1, j);
                    if (lua_istable(L, -1))
                    {
                        LuaParseTransitionTable(L, -1, graph, fromName, anyState, skeleton, outSpecs);
                    }
                    lua_pop(L, 1);
                }
            }
        }
        lua_pop(L, 1);
    }
}

static bool BuildBlendGraphFromLua(lua_State* L,
                                   AnimationGraphRuntime& graph,
                                   int tableIndex,
                                   const std::map<std::string, std::string>& animMap,
                                   const LuaScriptContext& ctx,
                                   const hkaSkeleton* skeleton,
                                   int jsonDecodeMode,
                                   int type2PackingMode,
                                   bool strictRigCoverage,
                                   const std::string& graphName,
                                   BlendGraphRuntime*& outGraph)
{
    tableIndex = LuaAbsIndex(L, tableIndex);
    if (!lua_istable(L, tableIndex))
        return false;

    BlendGraphRuntime* bg = new BlendGraphRuntime();
    bg->name = graphName;

    std::string resultName;
    LuaGetFieldString(L, tableIndex, "Result", resultName);
    bg->resultNodeName = resultName;

    std::map<std::string, int> nodeIndexMap;

    lua_pushnil(L);
    while (lua_next(L, tableIndex) != 0)
    {
        if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1))
        {
            const char* nodeNameC = lua_tostring(L, -2);
            std::string nodeName = nodeNameC ? nodeNameC : "";

            int nodeTable = LuaAbsIndex(L, -1);
            float typeVal = 0.0f;
            if (LuaGetFieldNumber(L, nodeTable, "Type", typeVal))
            {
                int typeInt = (int)typeVal;
                BlendGraphNode node;
                node.name = nodeName;

                if (typeInt == 0) node.type = BLEND_NODE_BLENDER;
                else if (typeInt == 1) node.type = BLEND_NODE_SAMPLER;
                else if (typeInt == 2) node.type = BLEND_NODE_STATEMACHINE;
                else if (typeInt == 5) node.type = BLEND_NODE_REFERENCE;
                else if (typeInt == 14) node.type = BLEND_NODE_SUBGRAPH;
                else node.type = BLEND_NODE_SAMPLER;

                if (node.type == BLEND_NODE_SAMPLER)
                {
                    if (LuaGetFieldTable(L, nodeTable, "Animation"))
                    {
                        int animTable = LuaAbsIndex(L, -1);
                        std::string animName;
                        if (LuaGetFieldString(L, animTable, "Name", animName))
                        {
                            std::string clipKey = ResolveAnimNameToClip(animMap, animName);
                            std::string clipPath;
                            if (ResolveLuaAnimClipPath(ctx, clipKey, clipPath))
                            {
                                node.clipPath = clipPath;
                                node.clip = LoadLuaAnimClipChecked(clipPath, skeleton, jsonDecodeMode, type2PackingMode, strictRigCoverage);
                                if (node.clip)
                                {
                                    float dur = GetClipDurationLocal(node.clip);
                                    if (dur > bg->duration)
                                        bg->duration = dur;
                                }
                            }
                            else
                            {
                                node.clipPath = clipKey;
                            }
                        }
                        bool loop = true;
                        if (LuaGetFieldBool(L, animTable, "Looping", loop))
                            node.loop = loop;
                        float rate = 1.0f;
                        if (LuaGetFieldNumber(L, animTable, "Rate", rate))
                            node.rate = rate;
                        bool syncVel = false;
                        if (LuaGetFieldBool(L, animTable, "SyncToVelocity", syncVel))
                            node.syncToVelocity = syncVel;
                        float rootSpeed = 0.0f;
                        if (LuaGetFieldNumber(L, animTable, "RootSpeed", rootSpeed))
                            node.rootSpeed = rootSpeed;

                        if (LuaGetFieldTable(L, animTable, "Progress"))
                        {
                            std::string paramName;
                            if (LuaGetStringFromValue(L, -1, paramName))
                            {
                                EnsureGraphParamLocal(graph, paramName, Scene3DRenderer::GRAPH_PARAM_FLOAT, 0.0f);
                                node.progressParamIndex = FindGraphParamIndexLocal(graph, paramName.c_str());
                                node.useProgressParam = (node.progressParamIndex >= 0);
                            }
                            lua_pop(L, 1);
                        }
                        lua_pop(L, 1);
                    }
                }
                else if (node.type == BLEND_NODE_BLENDER)
                {
                    bool normalize = false;
                    if (LuaGetFieldBool(L, nodeTable, "NormalizeWeights", normalize))
                        node.normalizeWeights = normalize;
                    bool syncTracks = false;
                    if (LuaGetFieldBool(L, nodeTable, "SyncTracks", syncTracks))
                        node.syncTracks = syncTracks;
                    bool syncToVel = false;
                    if (LuaGetFieldBool(L, nodeTable, "SyncToVelocity", syncToVel))
                        node.blenderSyncToVelocity = syncToVel;
                    float weightDamp = 0.0f;
                    if (LuaGetFieldNumber(L, nodeTable, "WeightDamp", weightDamp))
                        node.weightDamp = weightDamp;
                    float syncedRate = 1.0f;
                    if (LuaGetFieldNumber(L, nodeTable, "SyncedRate", syncedRate))
                        node.syncedRate = syncedRate;
                    float weightInterp = 0.0f;
                    if (LuaGetFieldNumber(L, nodeTable, "WeightInterpRate", weightInterp) && node.weightDamp <= 0.0f)
                        node.weightDamp = weightInterp;

                    if (LuaGetFieldTable(L, nodeTable, "Tracks"))
                    {
                        int tracksTable = LuaAbsIndex(L, -1);
                        int len = (int)lua_objlen(L, tracksTable);
                        for (int i = 1; i <= len; ++i)
                        {
                            lua_rawgeti(L, tracksTable, i);
                            if (lua_istable(L, -1))
                            {
                                BlendGraphTrack track;
                                std::string inputName;
                                if (LuaGetFieldString(L, -1, "Input", inputName))
                                {
                                    track.inputName = inputName;
                                }
                                float eventMode = 0.0f;
                                if (LuaGetFieldNumber(L, -1, "EventMode", eventMode))
                                    track.eventMode = (int)eventMode;
                                if (LuaGetFieldTable(L, -1, "Weight"))
                                {
                                    std::string weightName;
                                    float weightConst = 0.0f;
                                    if (LuaGetStringFromValue(L, -1, weightName))
                                    {
                                        track.weight.isConst = false;
                                        track.weight.name = weightName;
                                        EnsureGraphParamLocal(graph, weightName, Scene3DRenderer::GRAPH_PARAM_FLOAT, 0.0f);
                                    }
                                    else if (LuaGetNumberFromValue(L, -1, weightConst))
                                    {
                                        track.weight.isConst = true;
                                        track.weight.constant = weightConst;
                                    }
                                    lua_pop(L, 1);
                                }
                                else
                                {
                                    float w = 0.0f;
                                    if (LuaGetFieldNumber(L, -1, "Weight", w))
                                    {
                                        track.weight.isConst = true;
                                        track.weight.constant = w;
                                    }
                                }
                                if (LuaGetFieldTable(L, -1, "BlendParts"))
                                {
                                    BuildMaskFromLuaBlendParts(L, -1, skeleton, track.mask);
                                    lua_pop(L, 1);
                                }
                                node.tracks.push_back(track);
                            }
                            lua_pop(L, 1);
                        }
                        lua_pop(L, 1);
                    }
                }
                else if (node.type == BLEND_NODE_REFERENCE)
                {
                    std::string refName;
                    if (LuaGetFieldString(L, nodeTable, "Reference", refName) ||
                        LuaGetFieldString(L, nodeTable, "Input", refName))
                    {
                        node.refName = refName;
                    }
                }
                else if (node.type == BLEND_NODE_STATEMACHINE)
                {
                    std::string smName;
                    if (LuaGetFieldString(L, nodeTable, "StateMachine", smName))
                    {
                        node.stateMachineName = smName;
                    }
                }
                else if (node.type == BLEND_NODE_SUBGRAPH)
                {
                    std::string subName;
                    if (LuaGetFieldString(L, nodeTable, "Subgraph", subName))
                    {
                        node.subgraphName = subName;
                    }
                }

                int newIndex = (int)bg->nodes.size();
                bg->nodes.push_back(node);
                nodeIndexMap[nodeName] = newIndex;
            }
        }
        lua_pop(L, 1);
    }

    if (!bg->resultNodeName.empty())
    {
        std::map<std::string, int>::iterator it = nodeIndexMap.find(bg->resultNodeName);
        if (it != nodeIndexMap.end())
            bg->resultNode = it->second;
    }

    for (size_t i = 0; i < bg->nodes.size(); ++i)
    {
        BlendGraphNode& node = bg->nodes[i];
        for (size_t t = 0; t < node.tracks.size(); ++t)
        {
            if (!node.tracks[t].inputName.empty())
            {
                std::map<std::string, int>::iterator it = nodeIndexMap.find(node.tracks[t].inputName);
                if (it != nodeIndexMap.end())
                    node.tracks[t].inputNode = it->second;
            }
        }
        if (!node.refName.empty())
        {
            std::map<std::string, int>::iterator it = nodeIndexMap.find(node.refName);
            if (it != nodeIndexMap.end())
                node.referenceNode = it->second;
        }
    }

    outGraph = bg;
    return true;
}

static bool BuildGraphFromLuaStateMachine(lua_State* L,
                                          AnimationGraphRuntime& graph,
                                          int tableIndex,
                                          const std::map<std::string, std::string>& animMap,
                                          const LuaScriptContext& ctx,
                                          const hkaSkeleton* skeleton,
                                          int jsonDecodeMode,
                                          int type2PackingMode,
                                          bool strictRigCoverage)
{
    tableIndex = LuaAbsIndex(L, tableIndex);
    if (!lua_istable(L, tableIndex))
        return false;

    std::string smName;
    LuaGetFieldString(L, tableIndex, "Name", smName);
    if (!smName.empty())
        graph.name = smName;

    std::string defaultStateName;
    if (LuaGetFieldString(L, tableIndex, "DefaultState", defaultStateName))
    {
        // ok
    }
    else if (LuaGetFieldTable(L, tableIndex, "DefaultState"))
    {
        int defTable = LuaAbsIndex(L, -1);
        int len = (int)lua_objlen(L, defTable);
        for (int i = 1; i <= len; ++i)
        {
            lua_rawgeti(L, defTable, i);
            if (lua_isstring(L, -1))
            {
                defaultStateName = lua_tostring(L, -1);
            }
            else if (lua_istable(L, -1))
            {
                lua_rawgeti(L, -1, 2);
                if (lua_isstring(L, -1))
                {
                    defaultStateName = lua_tostring(L, -1);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    std::map<int, std::string> referenceMap;
    std::vector<LuaTransitionSpec> pendingTransitions;

    lua_pushnil(L);
    while (lua_next(L, tableIndex) != 0)
    {
        if ((lua_type(L, -2) == LUA_TSTRING || lua_type(L, -2) == LUA_TNUMBER) && lua_istable(L, -1))
        {
            std::string stateName;
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                const char* keyNameC = lua_tostring(L, -2);
                stateName = keyNameC ? keyNameC : "";
                if (stateName == "Name" || stateName == "DefaultState" || stateName == "Events" ||
                    stateName == "Actions" || stateName == "Transitions" || stateName == "Flags")
                {
                    lua_pop(L, 1);
                    continue;
                }
            }
            else
            {
                LuaGetFieldString(L, -1, "Name", stateName);
                if (stateName.empty())
                {
                    lua_pop(L, 1);
                    continue;
                }
            }

            int stateTable = LuaAbsIndex(L, -1);
            GraphState state;
            state.name = stateName;

            if (LuaGetFieldTable(L, stateTable, "Animation"))
            {
                int animTable = LuaAbsIndex(L, -1);
                std::string animName;
                if (LuaGetFieldString(L, animTable, "Name", animName))
                {
                    std::string clipKey = ResolveAnimNameToClip(animMap, animName);
                    std::string clipPath;
                    if (ResolveLuaAnimClipPath(ctx, clipKey, clipPath))
                    {
                        JsonAnimClip* clip = LoadLuaAnimClipChecked(clipPath, skeleton, jsonDecodeMode, type2PackingMode, strictRigCoverage);
                        if (clip)
                        {
                            state.clipPath = clipPath;
                            state.clip = clip;
                            state.duration = GetClipDurationLocal(clip);
                            state.frameTime = GetClipFrameTimeLocal(clip);
                        }
                    }
                }
                bool loop = true;
                if (LuaGetFieldBool(L, animTable, "Looping", loop))
                    state.loop = loop;
                float rate = 1.0f;
                if (LuaGetFieldNumber(L, animTable, "Rate", rate))
                    state.speed = rate;
                std::string doneEvent;
                if (LuaGetFieldString(L, animTable, "DoneEvent", doneEvent))
                {
                    state.doneEvent = doneEvent;
                    EnsureGraphParamLocal(graph, doneEvent, Scene3DRenderer::GRAPH_PARAM_TRIGGER, 0.0f);
                }
                lua_pop(L, 1);
            }
            else if (LuaGetFieldTable(L, stateTable, "PickRandom"))
            {
                int prTable = LuaAbsIndex(L, -1);
                if (LuaGetFieldTable(L, prTable, "Animations"))
                {
                    int animsTable = LuaAbsIndex(L, -1);
                    int len = (int)lua_objlen(L, animsTable);
                    if (len > 0)
                    {
                        lua_rawgeti(L, animsTable, 1);
                        if (lua_istable(L, -1))
                        {
                            std::string animName;
                            if (LuaGetFieldString(L, -1, "Name", animName))
                            {
                                std::string clipKey = ResolveAnimNameToClip(animMap, animName);
                                std::string clipPath;
                                if (ResolveLuaAnimClipPath(ctx, clipKey, clipPath))
                                {
                                    JsonAnimClip* clip = LoadLuaAnimClipChecked(clipPath, skeleton, jsonDecodeMode, type2PackingMode, strictRigCoverage);
                                    if (clip)
                                    {
                                        state.clipPath = clipPath;
                                        state.clip = clip;
                                        state.duration = GetClipDurationLocal(clip);
                                        state.frameTime = GetClipFrameTimeLocal(clip);
                                    }
                                }
                            }
                            bool loop = false;
                            if (LuaGetFieldBool(L, -1, "Looping", loop))
                                state.loop = loop;
                            float rate = 1.0f;
                            if (LuaGetFieldNumber(L, -1, "Rate", rate))
                                state.speed = rate;
                            std::string doneEvent;
                            if (LuaGetFieldString(L, -1, "DoneEvent", doneEvent))
                            {
                                state.doneEvent = doneEvent;
                                EnsureGraphParamLocal(graph, doneEvent, Scene3DRenderer::GRAPH_PARAM_TRIGGER, 0.0f);
                            }
                        }
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            else if (LuaGetFieldTable(L, stateTable, "PickConditional"))
            {
                int pcTable = LuaAbsIndex(L, -1);
                if (LuaGetFieldTable(L, pcTable, "Animations"))
                {
                    int animsTable = LuaAbsIndex(L, -1);
                    int len = (int)lua_objlen(L, animsTable);
                    for (int i = 1; i <= len; ++i)
                    {
                        lua_rawgeti(L, animsTable, i);
                        if (lua_istable(L, -1))
                        {
                            GraphPickAnim anim;
                            if (LuaParsePickAnimEntry(L, -1, animMap, ctx,
                                                      skeleton, jsonDecodeMode, type2PackingMode, strictRigCoverage,
                                                      anim))
                            {
                                if (!anim.doneEvent.empty())
                                    EnsureGraphParamLocal(graph, anim.doneEvent, Scene3DRenderer::GRAPH_PARAM_TRIGGER, 0.0f);
                                state.pickAnims.push_back(anim);
                            }
                        }
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }

                if (LuaGetFieldTable(L, pcTable, "Candidates"))
                {
                    int candTable = LuaAbsIndex(L, -1);
                    int defPick = 0;
                    if (LuaGetFieldIntLocal(L, candTable, "PickAnimDefault", defPick))
                        state.pickDefault = defPick;

                    int len = (int)lua_objlen(L, candTable);
                    for (int i = 1; i <= len; ++i)
                    {
                        lua_rawgeti(L, candTable, i);
                        if (lua_istable(L, -1))
                        {
                            GraphPickCandidate cand;
                            float pickNum = -1.0f;
                            if (LuaGetFieldNumber(L, -1, "PickAnim", pickNum))
                                cand.pickIndex = (int)pickNum;

                            if (LuaGetFieldTable(L, -1, "Conditions"))
                            {
                                std::vector< std::vector<GraphCondition> > condSets;
                                ParseLuaConditionsList(L, -1, graph, condSets);
                                cand.conditionSets = condSets;
                                lua_pop(L, 1);
                            }

                            if (cand.pickIndex >= 0)
                                state.pickCandidates.push_back(cand);
                        }
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }

                if (!state.pickAnims.empty())
                {
                    int pickIndex = state.pickDefault;
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

                lua_pop(L, 1);
            }

            if (LuaGetFieldTable(L, stateTable, "BlendGraph"))
            {
                int bgTable = LuaAbsIndex(L, -1);
                BlendGraphRuntime* bg = NULL;
                std::string bgName = state.name + "_BlendGraph";
                if (BuildBlendGraphFromLua(L, graph, bgTable, animMap, ctx,
                                           skeleton, jsonDecodeMode, type2PackingMode, strictRigCoverage,
                                           bgName, bg))
                {
                    state.blendGraph = bg;
                    state.blendGraphName = bgName;
                    graph.blendGraphs.push_back(bg);
                }
                lua_pop(L, 1);
            }

            if (LuaGetFieldTable(L, stateTable, "StateMachine"))
            {
                int smTable = LuaAbsIndex(L, -1);
                AnimationGraphRuntime* sub = new AnimationGraphRuntime();
                sub->owner = graph.owner ? graph.owner : &graph;
                sub->parent = &graph;
                if (BuildGraphFromLuaStateMachine(L, *sub, smTable, animMap, ctx,
                                                  skeleton, jsonDecodeMode, type2PackingMode, strictRigCoverage))
                {
                    state.subMachine = sub;
                    state.stateMachineName = sub->name;
                    graph.subMachines.push_back(sub);
                }
                else
                {
                    delete sub;
                }
                lua_pop(L, 1);
            }

            int flags = 0;
            if (LuaGetFieldIntLocal(L, stateTable, "Flags", flags))
                state.flags = flags;

            if (LuaGetFieldTable(L, stateTable, "Events"))
            {
                LuaParseStateEvents(L, -1, graph, state);
                lua_pop(L, 1);
            }

            if (LuaGetFieldTable(L, stateTable, "Actions"))
            {
                LuaParseStateActions(L, -1, graph, state);
                lua_pop(L, 1);
            }

            std::string refName;
            if (LuaGetFieldString(L, stateTable, "Reference", refName))
            {
                referenceMap[(int)graph.states.size()] = refName;
            }

            if (state.clip || state.blendGraph || state.subMachine || !refName.empty())
            {
                graph.states.push_back(state);

                if (LuaGetFieldTable(L, stateTable, "Transitions"))
                {
                    LuaParseTransitionsArray(L, -1, graph, state.name.c_str(), false, skeleton, pendingTransitions);
                    lua_pop(L, 1);
                }
            }
        }
        lua_pop(L, 1);
    }

    if (LuaGetFieldTable(L, tableIndex, "Transitions"))
    {
        LuaParseTransitionsArray(L, -1, graph, NULL, true, skeleton, pendingTransitions);
        lua_pop(L, 1);
    }

    for (std::map<int, std::string>::iterator it = referenceMap.begin(); it != referenceMap.end(); ++it)
    {
        int idx = it->first;
        if (idx < 0 || idx >= (int)graph.states.size()) continue;
        const std::string& ref = it->second;
        for (size_t i = 0; i < graph.states.size(); ++i)
        {
            if (graph.states[i].name == ref)
            {
                graph.states[idx].clip = graph.states[i].clip;
                graph.states[idx].clipPath = graph.states[i].clipPath;
                graph.states[idx].blendGraph = graph.states[i].blendGraph;
                graph.states[idx].blendGraphName = graph.states[i].blendGraphName;
                graph.states[idx].subMachine = graph.states[i].subMachine;
                graph.states[idx].stateMachineName = graph.states[i].stateMachineName;
                graph.states[idx].duration = graph.states[i].duration;
                graph.states[idx].frameTime = graph.states[i].frameTime;
                break;
            }
        }
    }

    graph.defaultState = -1;
    if (!defaultStateName.empty())
    {
        graph.defaultState = FindGraphStateIndexLocal(graph, defaultStateName.c_str());
    }
    if (graph.defaultState < 0 && !graph.states.empty())
        graph.defaultState = 0;

    for (size_t i = 0; i < pendingTransitions.size(); ++i)
    {
        const LuaTransitionSpec& spec = pendingTransitions[i];
        int fromIdx = spec.anyState ? -1 : FindGraphStateIndexLocal(graph, spec.fromName.c_str());
        int toIdx = FindGraphStateIndexLocal(graph, spec.toName.c_str());
        if (toIdx < 0) continue;

        GraphTransition tr;
        tr.fromState = fromIdx;
        tr.toState = toIdx;
        tr.anyState = spec.anyState;
        tr.duration = spec.duration;
        tr.exitTime = spec.exitTime;
        tr.maxTime = spec.maxTime;
        if (tr.exitTime == -2.0f)
        {
            tr.exitTime = 1.0f;
        }
        else if (tr.exitTime >= 0.0f)
        {
            float baseDur = 0.0f;
            if (fromIdx >= 0 && fromIdx < (int)graph.states.size())
                baseDur = graph.states[fromIdx].duration;
            if (baseDur > 0.0f)
                tr.exitTime = tr.exitTime / baseDur;
        }
        if (tr.maxTime >= 0.0f)
        {
            float baseDur = 0.0f;
            if (fromIdx >= 0 && fromIdx < (int)graph.states.size())
                baseDur = graph.states[fromIdx].duration;
            if (baseDur > 0.0f)
                tr.maxTime = tr.maxTime / baseDur;
        }
        tr.priority = spec.priority;
        tr.allowInterrupt = spec.allowInterrupt;
        tr.syncHint = spec.syncHint;
        tr.syncTime = (spec.syncHint != 0);
        tr.blendMode = spec.blendMode;
        tr.rotMode = spec.rotMode;
        tr.layerRoot = spec.layerRoot;
        tr.mask = spec.mask;
        tr.conditions = spec.conditions;
        graph.transitions.push_back(tr);
    }

    return !graph.states.empty();
}

bool BuildGraphFromLuaAnimationGraph(lua_State* L,
                                     AnimationGraphRuntime& graph,
                                     const std::map<std::string, std::string>& animMap,
                                     const LuaScriptContext& ctx,
                                     const hkaSkeleton* skeleton,
                                     int jsonDecodeMode,
                                     int type2PackingMode,
                                     bool strictRigCoverage)
{
    lua_getglobal(L, "AnimationGraph");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return false;
    }

    int graphTable = LuaAbsIndex(L, -1);
    if (LuaGetFieldTable(L, graphTable, "BaseStateMachine"))
    {
        int baseNode = LuaAbsIndex(L, -1);
        if (LuaGetFieldTable(L, baseNode, "StateMachine"))
        {
            int smTable = LuaAbsIndex(L, -1);
            bool ok = BuildGraphFromLuaStateMachine(L, graph, smTable, animMap, ctx,
                                                    skeleton, jsonDecodeMode, type2PackingMode, strictRigCoverage);
            lua_pop(L, 1);
            lua_pop(L, 1);
            lua_pop(L, 1);
            return ok;
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1);
    return false;
}
