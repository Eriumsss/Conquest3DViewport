// PluginLoader.cpp
// =============================================================================
// LOAD VESPUCCI PLUGIN DLLs FROM A DIRECTORY - LoadLibraryA + GetProcAddress
// =============================================================================
// Written by: Eriumsss

#include "PluginAbi.h"

#include "../Core/Logging.h"
#include "../Core/PathUtils.h"
#include "../Core/VespucciTypes.h"

#include <string>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace Vespucci {
namespace Plugin {

void RegisterRankerPlugin(const char* name, void* userData,
                           void (*fn)(void*, const void*, void*));
void RegisterReasonGeneratorPlugin(const char* name, void* userData,
                                     const char* (*fn)(void*, const void*));

namespace {
    static std::vector<void*> s_loadedHandles;
} // namespace

bool LoadOnePluginOrChokeOnAbiMismatch(const char* dllPath) {
#if defined(_WIN32)
    if (!dllPath || !*dllPath) return false;
    HMODULE h = LoadLibraryA(dllPath);
    if (!h) {
        Core::Logging::Warn("PluginLoader: LoadLibrary failed for '%s' (err=%lu)",
            dllPath, (unsigned long)GetLastError());
        return false;
    }
    VespucciPluginEntry entry = (VespucciPluginEntry)GetProcAddress(h, "VespucciPluginMain");
    if (!entry) {
        Core::Logging::Warn("PluginLoader: '%s' does not export VespucciPluginMain", dllPath);
        FreeLibrary(h);
        return false;
    }
    VespucciPluginInfo info = {0};
    VespucciPluginKind kind;
    int rc = entry(&info, &kind);
    if (rc != 0) {
        Core::Logging::Warn("PluginLoader: '%s' refused to init (rc=%d)", dllPath, rc);
        FreeLibrary(h);
        return false;
    }
    if (info.abiVersion != VESPUCCI_PLUGIN_ABI_VERSION) {
        Core::Logging::Error("PluginLoader: '%s' is ABI v%d, host is v%d - "
            "rebuild the plugin against the matching SDK or eat shit",
            dllPath, info.abiVersion, VESPUCCI_PLUGIN_ABI_VERSION);
        FreeLibrary(h);
        return false;
    }
    Core::Logging::Info("PluginLoader: loaded '%s' v%s by %s (kind=%d)",
        info.pluginName ? info.pluginName : "?",
        info.pluginVersion ? info.pluginVersion : "?",
        info.author ? info.author : "?",
        (int)kind);
    s_loadedHandles.push_back((void*)h);
    return true;
#else
    (void)dllPath;
    return false;
#endif
}

i32 ScanAndLoadPluginsInDir(const char* dirPath) {
#if defined(_WIN32)
    if (!dirPath || !Core::Path::IsDir(dirPath)) return 0;
    std::string pattern = std::string(dirPath) + "\\*.dll";
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pattern.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return 0;
    i32 loaded = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string full = std::string(dirPath) + "\\" + fd.cFileName;
        if (LoadOnePluginOrChokeOnAbiMismatch(full.c_str())) loaded++;
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    Core::Logging::Info("PluginLoader: %d plugins loaded from '%s'", loaded, dirPath);
    return loaded;
#else
    (void)dirPath;
    return 0;
#endif
}

void UnloadAllPlugins() {
#if defined(_WIN32)
    for (size_t i = 0; i < s_loadedHandles.size(); ++i) {
        FreeLibrary((HMODULE)s_loadedHandles[i]);
    }
#endif
    s_loadedHandles.clear();
}

} // namespace Plugin
} // namespace Vespucci
