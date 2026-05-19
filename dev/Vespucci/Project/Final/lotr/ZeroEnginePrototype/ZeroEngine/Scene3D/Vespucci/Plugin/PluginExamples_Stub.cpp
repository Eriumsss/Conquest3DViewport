// PluginExamples_Stub.cpp
// =============================================================================
// EXAMPLE PLUGIN STUBS - DOCUMENTATION-BY-CODE FOR PLUGIN AUTHORS
// =============================================================================
// Written by: Eriumsss
//
// This file is NOT linked into the production DLL. It compiles
// against the plugin ABI and shows authors what the entry point
// looks like + how to register a custom ranker / reason generator.
// Use these as a copy-paste starting point for real plugins.
//
// NOTE: a real plugin lives in its own DLL. This stub is here for
// reference only. Build it via Tools/build_plugin_examples.bat
// (added to the build batch list separately).
// =============================================================================

#include "PluginAbi.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
    #define VESPUCCI_PLUGIN_EXPORT __declspec(dllexport)
#else
    #define VESPUCCI_PLUGIN_EXPORT
#endif

extern "C" {

static void ExampleScoreFn(void* /*userData*/, const void* /*query*/, void* /*outResult*/) {
    // A real plugin would inspect the query, compute a score, and
    // write it into outResult. The host does the rest.
}

static const char* ExampleReasonGenFn(void* /*userData*/, const void* /*features*/) {
    return "example reason from plugin stub";
}

VESPUCCI_PLUGIN_EXPORT int VespucciPluginMain(VespucciPluginInfo* outInfo,
                                                VespucciPluginKind* outKind)
{
    if (!outInfo || !outKind) return 1;
    outInfo->abiVersion    = VESPUCCI_PLUGIN_ABI_VERSION;
    outInfo->pluginName    = "VespucciExampleStub";
    outInfo->pluginVersion = "1.0.0";
    outInfo->author        = "Eriumsss";
    *outKind               = VESPUCCI_PLUGIN_KIND_Ranker;

    // To actually register the example ranker, the plugin would
    // call back into the host via a dlsym/GetProcAddress lookup of
    // the host's RegisterRankerPlugin function. The example skips
    // that to keep the stub linkable in isolation.
    (void)ExampleScoreFn;
    (void)ExampleReasonGenFn;
    return 0;
}

} // extern C
