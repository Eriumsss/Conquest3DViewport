// ============================================================================
// ConquestDebugger - DLL Entry Point
// ============================================================================

#include "../include/debugger.h"
#include "../include/console.h"
#include "../include/thread_monitor.h"
#include "../include/memory_watcher.h"

#include <cstdio>

namespace ConquestDebugger {
    static bool g_initialized = false;
    static DWORD g_mainThreadId = 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            ConquestDebugger::g_mainThreadId = GetCurrentThreadId();
            break;

        case DLL_PROCESS_DETACH:
            if (ConquestDebugger::g_initialized) {
                ConquestDebugger::Shutdown();
            }
            break;
    }
    return TRUE;
}

namespace ConquestDebugger {

// ============================================================================
// Core API Implementation
// ============================================================================

DEBUGGER_API bool Initialize() {
    if (g_initialized) return true;

    // Create console window for real-time output
    Console::Instance().Create("Conquest Debugger v1.0");
    Console::Instance().SetSize(120, 40);

    Log(LogLevel::Info, "========================================");
    Log(LogLevel::Info, "  ConquestDebugger v1.0 Initialized");
    Log(LogLevel::Info, "  LOTR: Conquest Diagnostic Framework");
    Log(LogLevel::Info, "========================================");
    Log(LogLevel::Info, "");

    // Set up thread monitor with main thread
    ThreadMonitor::Instance().SetMainThread(g_mainThreadId);
    ThreadMonitor::Instance().SetFreezeTimeout(2000);  // 2 second freeze detection
    ThreadMonitor::Instance().Start();

    // Set up memory watcher with default game addresses
    MemoryWatcher::Instance().AddWatch("EventPool.Count", Addresses::EVENT_POOL_COUNT, 4, WatchType::Value);
    MemoryWatcher::Instance().AddWatch("EventPool.Base", Addresses::EVENT_POOL_BASE, 4, WatchType::Value);
    MemoryWatcher::Instance().AddWatch("Pool2.Count", Addresses::POOL2_COUNT, 4, WatchType::Value);
    MemoryWatcher::Instance().Start();

    // Install crash handler
    InstallCrashHandler();

    Log(LogLevel::Info, "Main thread ID: %d", g_mainThreadId);
    Log(LogLevel::Info, "Thread monitor: ACTIVE");
    Log(LogLevel::Info, "Memory watcher: ACTIVE");
    Log(LogLevel::Info, "Crash handler:  ACTIVE");
    Log(LogLevel::Info, "");
    Log(LogLevel::Info, "Monitoring for freezes and crashes...");
    Log(LogLevel::Info, "");

    g_initialized = true;
    return true;
}

DEBUGGER_API void Shutdown() {
    if (!g_initialized) return;

    Log(LogLevel::Info, "Shutting down ConquestDebugger...");

    ThreadMonitor::Instance().Stop();
    MemoryWatcher::Instance().Stop();
    Console::Instance().Destroy();

    g_initialized = false;
}

DEBUGGER_API bool IsInitialized() {
    return g_initialized;
}

} // namespace ConquestDebugger

// ============================================================================
// C-style export for version.dll proxy to call
// ============================================================================
extern "C" __declspec(dllexport) bool Initialize() {
    return ConquestDebugger::Initialize();
}
