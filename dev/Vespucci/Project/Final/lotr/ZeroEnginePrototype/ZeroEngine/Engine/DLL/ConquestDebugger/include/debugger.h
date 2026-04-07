#pragma once
// ============================================================================
// ConquestDebugger - Professional Game Debugging Framework
// For: The Lord of the Rings: Conquest
// ============================================================================

#ifndef CONQUEST_DEBUGGER_H
#define CONQUEST_DEBUGGER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

#ifdef CONQUEST_DEBUGGER_EXPORTS
    #define DEBUGGER_API __declspec(dllexport)
#else
    #define DEBUGGER_API __declspec(dllimport)
#endif

namespace ConquestDebugger {

// ============================================================================
// Log Levels
// ============================================================================
enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Critical = 5
};

// ============================================================================
// Memory Watch Types
// ============================================================================
enum class WatchType : int {
    ReadWrite,      // Any access
    Write,          // Write only
    Execute,        // Code execution
    Value           // Value change detection
};

// ============================================================================
// Thread State
// ============================================================================
struct ThreadState {
    DWORD threadId;
    HANDLE handle;
    DWORD eip;
    DWORD esp;
    DWORD ebp;
    DWORD eax, ebx, ecx, edx, esi, edi;
    bool suspended;
    DWORD lastActiveTime;
};

// ============================================================================
// Memory Watch Entry
// ============================================================================
struct MemoryWatch {
    const char* name;
    uintptr_t address;
    size_t size;
    WatchType type;
    DWORD lastValue;
    bool triggered;
};

// ============================================================================
// Core API Functions
// ============================================================================

// Initialization
DEBUGGER_API bool Initialize();
DEBUGGER_API void Shutdown();
DEBUGGER_API bool IsInitialized();

// Logging
DEBUGGER_API void Log(LogLevel level, const char* format, ...);
DEBUGGER_API void SetLogLevel(LogLevel minLevel);
DEBUGGER_API void SetLogFile(const char* path);

// Console
DEBUGGER_API bool CreateConsoleWindow();
DEBUGGER_API void DestroyConsoleWindow();

// Thread Monitoring
DEBUGGER_API void StartThreadMonitor();
DEBUGGER_API void StopThreadMonitor();
DEBUGGER_API bool CaptureThreadState(DWORD threadId, ThreadState* state);
DEBUGGER_API bool IsThreadFrozen(DWORD threadId, DWORD timeoutMs);

// Memory Watching
DEBUGGER_API int AddMemoryWatch(const char* name, uintptr_t addr, size_t size, WatchType type);
DEBUGGER_API void RemoveMemoryWatch(int watchId);
DEBUGGER_API DWORD ReadMemorySafe(uintptr_t addr, size_t size = 4);

// Freeze Detection
DEBUGGER_API void SetFreezeCallback(void (*callback)(const ThreadState* state));
DEBUGGER_API void SetFreezeTimeout(DWORD ms);

// Crash Handling
DEBUGGER_API void InstallCrashHandler();
DEBUGGER_API void GenerateMinidump(EXCEPTION_POINTERS* exceptionInfo);

// Utility
DEBUGGER_API const char* GetModuleName(uintptr_t address);
DEBUGGER_API void DumpStackTrace(DWORD threadId, char* buffer, size_t bufferSize);

} // namespace ConquestDebugger

#endif // CONQUEST_DEBUGGER_H

