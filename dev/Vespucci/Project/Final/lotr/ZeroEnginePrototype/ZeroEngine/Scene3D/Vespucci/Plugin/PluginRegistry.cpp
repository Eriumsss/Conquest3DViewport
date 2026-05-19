// PluginRegistry.cpp
// =============================================================================
// REGISTRY OF LOADED PLUGINS - LOOKUP BY NAME, ENUMERATE BY KIND
// =============================================================================
// Written by: Eriumsss

#include "PluginAbi.h"

#include "../Core/Logging.h"
#include "../Core/VespucciTypes.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace Plugin {

struct RegisteredRanker {
    std::string name;
    void*       userData;
    void (*scoreFn)(void*, const void*, void*);
};

struct RegisteredReasonGen {
    std::string name;
    void*       userData;
    const char* (*genFn)(void*, const void*);
};

namespace {
    static std::vector<RegisteredRanker>     s_rankers;
    static std::vector<RegisteredReasonGen>  s_reasonGens;
}

void RegisterRankerPlugin(const char* name, void* userData,
                           void (*fn)(void*, const void*, void*))
{
    if (!name || !fn) return;
    RegisteredRanker r; r.name = name; r.userData = userData; r.scoreFn = fn;
    s_rankers.push_back(r);
    Core::Logging::Info("Plugin registered ranker: %s", name);
}

void RegisterReasonGeneratorPlugin(const char* name, void* userData,
                                     const char* (*fn)(void*, const void*))
{
    if (!name || !fn) return;
    RegisteredReasonGen g; g.name = name; g.userData = userData; g.genFn = fn;
    s_reasonGens.push_back(g);
    Core::Logging::Info("Plugin registered reason-generator: %s", name);
}

i32 PluginRankerCount()    { return (i32)s_rankers.size(); }
i32 PluginReasonGenCount() { return (i32)s_reasonGens.size(); }

const char* PluginRankerNameAt(i32 i) {
    if (i < 0 || i >= (i32)s_rankers.size()) return 0;
    return s_rankers[(size_t)i].name.c_str();
}

const char* PluginReasonGenNameAt(i32 i) {
    if (i < 0 || i >= (i32)s_reasonGens.size()) return 0;
    return s_reasonGens[(size_t)i].name.c_str();
}

void ClearAllPlugins() {
    s_rankers.clear();
    s_reasonGens.clear();
}

} // namespace Plugin
} // namespace Vespucci
