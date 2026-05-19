// PluginAbi.h
// =============================================================================
// VESPUCCI PLUGIN ABI - C-CALLABLE INTERFACE FOR THIRD-PARTY EXTENSIONS
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked decision: keep the plugin SDK at full scope. Plugins
// can register their own ranker strategies, their own UI panels,
// their own reason-chip text generators. They load via standard
// Win32 LoadLibrary; the registry tracks them in PluginRegistry.cpp.
//
// ABI is C-callable so plugin authors can compile against the same
// header without dragging in Vespucci's full C++ implementation
// surface. They get: Suggest contract, Schema query, Apply hooks.
// =============================================================================

#ifndef VESPUCCI_PLUGIN_PLUGINABI_H_
#define VESPUCCI_PLUGIN_PLUGINABI_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define VESPUCCI_PLUGIN_ABI_VERSION 1

typedef struct VespucciPluginInfo {
    int    abiVersion;
    const char* pluginName;
    const char* pluginVersion;
    const char* author;
} VespucciPluginInfo;

typedef enum VespucciPluginKind {
    VESPUCCI_PLUGIN_KIND_Ranker     = 0,
    VESPUCCI_PLUGIN_KIND_UIPanel    = 1,
    VESPUCCI_PLUGIN_KIND_ReasonText = 2,
    VESPUCCI_PLUGIN_KIND_FeatureSrc = 3
} VespucciPluginKind;

// Each plugin DLL exports a single C function with this signature.
typedef int (*VespucciPluginEntry)(VespucciPluginInfo* outInfo, VespucciPluginKind* outKind);

// Plugin-side registration helpers (the implementations live inside
// the Vespucci DLL; plugins call them via dlsym/GetProcAddress on
// the host).
typedef void (*VespucciPlugin_RegisterRanker_t)(const char* name,
    void* userData,
    void (*scoreFn)(void* userData, const void* query, void* outResult));

typedef void (*VespucciPlugin_RegisterReasonGenerator_t)(const char* name,
    void* userData,
    const char* (*genFn)(void* userData, const void* features));

#ifdef __cplusplus
} // extern C
#endif

#endif // VESPUCCI_PLUGIN_PLUGINABI_H_
