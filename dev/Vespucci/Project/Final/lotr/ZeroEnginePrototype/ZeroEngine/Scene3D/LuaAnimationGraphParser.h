// LuaAnimationGraphParser.h — Reading the Dead's Scripts
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// "God is dead. God remains dead. And we have killed him." — Nietzsche
// Pandemic is dead. Pandemic remains dead. And EA killed them. But their
// Lua scripts survive on the retail disc — animation state machines,
// blend graphs, transition rules, all written in Lua 5.0 and embedded
// in the PAK files as compiled bytecode. This parser loads those scripts,
// executes them in a sandboxed lua_State, and extracts the state machine
// definition into our AnimationGraphRuntime C++ structs.
//
// Pandemic's Lua animation system was SOPHISTICATED. States with blend
// trees. Transitions with multi-condition guards. Sub-machine recursion.
// Parametric blend nodes driven by float variables. All defined in Lua
// tables that mirror their C++ runtime. We found the Lua→C++ binding
// strings in the .exe: "AnimStateMachine.new", "State.setClip",
// "Transition.addCondition". They had a full scripting API. We rebuilt
// the C++ side that those scripts were MEANT to talk to.
//
// "If you're going through hell, keep going." — Churchill
// Parsing undocumented Lua bytecode from a dead studio IS hell.
// We kept going. The state machine works now. Most of the time.
// -----------------------------------------------------------------------
#ifndef LUA_ANIMATION_GRAPH_PARSER_H
#define LUA_ANIMATION_GRAPH_PARSER_H

#include <map>
#include <string>

struct lua_State;
struct AnimationGraphRuntime;
struct LuaScriptContext;
class hkaSkeleton;

bool BuildGraphFromLuaAnimationGraph(lua_State* L,
                                     AnimationGraphRuntime& graph,
                                     const std::map<std::string, std::string>& animMap,
                                     const LuaScriptContext& ctx,
                                     const hkaSkeleton* skeleton,
                                     int jsonDecodeMode,
                                     int type2PackingMode,
                                     bool strictRigCoverage);

#endif
