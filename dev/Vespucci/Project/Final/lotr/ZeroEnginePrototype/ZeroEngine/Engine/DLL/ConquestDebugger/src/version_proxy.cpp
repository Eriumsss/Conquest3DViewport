// ============================================================================
// version.dll Proxy - Loads real version.dll and ConquestDebugger
// This file should be compiled as a SEPARATE DLL named "version.dll"
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Real version.dll handle and function pointers
static HMODULE g_realVersionDll = nullptr;

// Generic function pointer for all exports
typedef FARPROC FnPtr;
static FnPtr g_origFuncs[17] = { nullptr };

// Function name table
static const char* g_funcNames[] = {
    "GetFileVersionInfoA",
    "GetFileVersionInfoByHandle",
    "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA",
    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW",
    "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW",
    "VerFindFileA",
    "VerFindFileW",
    "VerInstallFileA",
    "VerInstallFileW",
    "VerLanguageNameA",
    "VerLanguageNameW",
    "VerQueryValueA",
    "VerQueryValueW"
};

// Our debugger DLL
static HMODULE g_debuggerDll = nullptr;
typedef bool(*DebuggerInit_t)();
static DebuggerInit_t pDebuggerInit = nullptr;

static void LoadRealVersionDll() {
    if (g_realVersionDll) return;

    // Get system directory and load real version.dll from there
    char systemPath[MAX_PATH];
    GetSystemDirectoryA(systemPath, MAX_PATH);
    lstrcatA(systemPath, "\\version.dll");

    g_realVersionDll = LoadLibraryA(systemPath);
    if (!g_realVersionDll) {
        MessageBoxA(nullptr, "Failed to load real version.dll", "Error", MB_ICONERROR);
        return;
    }

    // Get all function pointers
    for (int i = 0; i < 17; i++) {
        g_origFuncs[i] = GetProcAddress(g_realVersionDll, g_funcNames[i]);
    }
}

static void LoadDebugger() {
    if (g_debuggerDll) return;

    // Debug: show that version.dll proxy loaded
    MessageBoxA(nullptr, "version.dll proxy loaded!", "Debug", MB_OK);

    // Load ConquestDebugger.dll from same directory
    g_debuggerDll = LoadLibraryA("ConquestDebugger.dll");
    if (g_debuggerDll) {
        pDebuggerInit = (DebuggerInit_t)GetProcAddress(g_debuggerDll, "Initialize");
        if (pDebuggerInit) {
            pDebuggerInit();
        } else {
            MessageBoxA(nullptr, "ConquestDebugger.dll loaded but Initialize() not found", "Error", MB_ICONERROR);
        }
    } else {
        DWORD err = GetLastError();
        char msg[256];
        wsprintfA(msg, "Failed to load ConquestDebugger.dll\nError code: %d", err);
        MessageBoxA(nullptr, msg, "Error", MB_ICONERROR);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            LoadRealVersionDll();
            LoadDebugger();
            break;

        case DLL_PROCESS_DETACH:
            if (g_debuggerDll) {
                FreeLibrary(g_debuggerDll);
            }
            if (g_realVersionDll) {
                FreeLibrary(g_realVersionDll);
            }
            break;
    }
    return TRUE;
}

// ============================================================================
// Exported functions - use naked functions with jmp for perfect forwarding
// The DEF file maps these internal names to the actual export names
// ============================================================================

// Index constants for g_origFuncs array
#define IDX_GetFileVersionInfoA         0
#define IDX_GetFileVersionInfoByHandle  1
#define IDX_GetFileVersionInfoExA       2
#define IDX_GetFileVersionInfoExW       3
#define IDX_GetFileVersionInfoSizeA     4
#define IDX_GetFileVersionInfoSizeExA   5
#define IDX_GetFileVersionInfoSizeExW   6
#define IDX_GetFileVersionInfoSizeW     7
#define IDX_GetFileVersionInfoW         8
#define IDX_VerFindFileA                9
#define IDX_VerFindFileW                10
#define IDX_VerInstallFileA             11
#define IDX_VerInstallFileW             12
#define IDX_VerLanguageNameA            13
#define IDX_VerLanguageNameW            14
#define IDX_VerQueryValueA              15
#define IDX_VerQueryValueW              16

// Generate forwarding functions using inline assembly
#define FORWARD_FUNC(name, idx) \
    extern "C" __declspec(naked) void __stdcall Proxy_##name() { \
        __asm { jmp [g_origFuncs + idx * 4] } \
    }

FORWARD_FUNC(GetFileVersionInfoA, IDX_GetFileVersionInfoA)
FORWARD_FUNC(GetFileVersionInfoByHandle, IDX_GetFileVersionInfoByHandle)
FORWARD_FUNC(GetFileVersionInfoExA, IDX_GetFileVersionInfoExA)
FORWARD_FUNC(GetFileVersionInfoExW, IDX_GetFileVersionInfoExW)
FORWARD_FUNC(GetFileVersionInfoSizeA, IDX_GetFileVersionInfoSizeA)
FORWARD_FUNC(GetFileVersionInfoSizeExA, IDX_GetFileVersionInfoSizeExA)
FORWARD_FUNC(GetFileVersionInfoSizeExW, IDX_GetFileVersionInfoSizeExW)
FORWARD_FUNC(GetFileVersionInfoSizeW, IDX_GetFileVersionInfoSizeW)
FORWARD_FUNC(GetFileVersionInfoW, IDX_GetFileVersionInfoW)
FORWARD_FUNC(VerFindFileA, IDX_VerFindFileA)
FORWARD_FUNC(VerFindFileW, IDX_VerFindFileW)
FORWARD_FUNC(VerInstallFileA, IDX_VerInstallFileA)
FORWARD_FUNC(VerInstallFileW, IDX_VerInstallFileW)
FORWARD_FUNC(VerLanguageNameA, IDX_VerLanguageNameA)
FORWARD_FUNC(VerLanguageNameW, IDX_VerLanguageNameW)
FORWARD_FUNC(VerQueryValueA, IDX_VerQueryValueA)
FORWARD_FUNC(VerQueryValueW, IDX_VerQueryValueW)

