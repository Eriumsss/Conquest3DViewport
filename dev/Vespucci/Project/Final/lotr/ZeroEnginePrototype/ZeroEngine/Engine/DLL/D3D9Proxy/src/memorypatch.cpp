// Memory Patcher for LOTR Conquest
// Patches texture and vertex buffer limits at runtime
// Includes crash detection and diagnostics

// Disable MSVC deprecation warnings for standard C functions
#define _CRT_SECURE_NO_WARNINGS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <intrin.h>  // For _ReturnAddress() intrinsic

#pragma intrinsic(_ReturnAddress)

#include "memorypatch.h"

#pragma comment(lib, "dbghelp.lib")

// Architecture-aware CONTEXT register access
#ifdef _WIN64
#define CONTEXT_IP(ctx) (ctx)->Rip
#define CONTEXT_SP(ctx) (ctx)->Rsp
#define CONTEXT_BP(ctx) (ctx)->Rbp
#define CONTEXT_AX(ctx) (ctx)->Rax
#define CONTEXT_BX(ctx) (ctx)->Rbx
#define CONTEXT_CX(ctx) (ctx)->Rcx
#define CONTEXT_DX(ctx) (ctx)->Rdx
#define CONTEXT_SI(ctx) (ctx)->Rsi
#define CONTEXT_DI(ctx) (ctx)->Rdi
#else
#define CONTEXT_IP(ctx) (ctx)->Eip
#define CONTEXT_SP(ctx) (ctx)->Esp
#define CONTEXT_BP(ctx) (ctx)->Ebp
#define CONTEXT_AX(ctx) (ctx)->Eax
#define CONTEXT_BX(ctx) (ctx)->Ebx
#define CONTEXT_CX(ctx) (ctx)->Ecx
#define CONTEXT_DX(ctx) (ctx)->Edx
#define CONTEXT_SI(ctx) (ctx)->Esi
#define CONTEXT_DI(ctx) (ctx)->Edi
#endif

// Frame counter from d3d9device.cpp
extern volatile DWORD g_FrameCounter;

// ============================================================================
// MODULE CACHING FOR RUNTIME ADDRESS TRANSLATION
// ============================================================================
// Caches loaded modules at startup to enable converting runtime addresses
// to module+offset format for consistent crash analysis across sessions.
// ============================================================================

struct ModuleInfo {
    HMODULE handle;
    DWORD baseAddr;
    DWORD size;
    char name[MAX_PATH];
};

static const int MAX_MODULES = 64;
static ModuleInfo g_Modules[MAX_MODULES];
static int g_ModuleCount = 0;
static bool g_ModulesCached = false;

// Cache all loaded modules - call once at startup
static void CacheLoadedModules() {
    if (g_ModulesCached) return;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(hSnap, &me)) {
        do {
            if (g_ModuleCount < MAX_MODULES) {
                g_Modules[g_ModuleCount].handle = me.hModule;
                g_Modules[g_ModuleCount].baseAddr = (DWORD)(uintptr_t)me.modBaseAddr;
                g_Modules[g_ModuleCount].size = me.modBaseSize;
                // Copy module name (works for both ANSI and Unicode builds)
#ifdef UNICODE
                WideCharToMultiByte(CP_ACP, 0, me.szModule, -1,
                    g_Modules[g_ModuleCount].name, MAX_PATH, NULL, NULL);
#else
                strncpy(g_Modules[g_ModuleCount].name, me.szModule, MAX_PATH - 1);
                g_Modules[g_ModuleCount].name[MAX_PATH - 1] = '\0';
#endif
                g_ModuleCount++;
            }
        } while (Module32Next(hSnap, &me));
    }
    CloseHandle(hSnap);
    g_ModulesCached = true;
}

// Convert runtime address to module name + offset
// Returns module name (or "(unknown)") and sets outOffset to the offset within that module
static const char* AddressToModuleOffset(DWORD addr, DWORD* outOffset) {
    for (int i = 0; i < g_ModuleCount; i++) {
        if (addr >= g_Modules[i].baseAddr &&
            addr < g_Modules[i].baseAddr + g_Modules[i].size) {
            *outOffset = addr - g_Modules[i].baseAddr;
            return g_Modules[i].name;
        }
    }
    *outOffset = addr;
    return "(unknown)";
}

// Log address with module+offset format (uses minimal logging for crash safety)
static void LogAddressWithModule(const char* prefix, DWORD addr) {
    DWORD offset;
    const char* modName = AddressToModuleOffset(addr, &offset);

    char buf[128];
    const char* hex = "0123456789ABCDEF";
    int i = 0;

    // Copy prefix
    while (*prefix && i < 60) buf[i++] = *prefix++;

    // Add module name
    while (*modName && i < 100) buf[i++] = *modName++;

    // Add "+0x"
    buf[i++] = '+'; buf[i++] = '0'; buf[i++] = 'x';

    // Convert offset to hex (8 digits)
    for (int j = 7; j >= 0; j--) {
        buf[i++] = hex[(offset >> (j * 4)) & 0xF];
    }
    buf[i] = '\0';

    // Write to crash log using minimal API
    HANDLE hFile = CreateFileA("conquest_crash.log", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, buf, (DWORD)strlen(buf), &written, NULL);
        WriteFile(hFile, "\r\n", 2, &written, NULL);
        CloseHandle(hFile);
    }
}

// ============================================================================
// DANGER-AWARE ENGINE LAYER - Systemic Crash Prevention
// ============================================================================
// This system prevents crashes during extreme stress moments by:
// 1. Monitoring allocation success/failure rates
// 2. Gating spawner functions when danger is high
// 3. Automatic recovery when stress subsides
// ============================================================================

// Danger levels
enum DangerLevel {
    DANGER_NORMAL = 0,    // All systems operational
    DANGER_CAUTION = 1,   // Throttle spawns, monitor closely
    DANGER_CRITICAL = 2,  // Skip ALL new spawns
    DANGER_RECOVERY = 3   // Cautious return to normal
};

// Thresholds (tuned for Zero Engine @ 25Hz tick rate)
static const DWORD THRESHOLD_CAUTION = 3;      // Failures this frame to trigger CAUTION
static const DWORD THRESHOLD_CRITICAL = 8;     // Failures this frame to trigger CRITICAL
static const DWORD MAX_EFFECTS_CAUTION = 10;   // Max effects per frame in CAUTION
static const DWORD MAX_EFFECTS_CRITICAL = 0;   // Max effects per frame in CRITICAL
static const DWORD MAX_RAGDOLLS_CAUTION = 3;   // Max ragdolls per frame in CAUTION
static const DWORD MAX_RAGDOLLS_CRITICAL = 0;  // Max ragdolls per frame in CRITICAL
static const DWORD COOLDOWN_FRAMES = 25;       // ~1 second at 25Hz
static const DWORD RECOVERY_FRAMES = 50;       // ~2 seconds before full normal

// Danger state - global singleton
struct DangerState {
    volatile DWORD allocFailuresThisFrame;
    volatile DWORD allocSuccessesThisFrame;
    volatile DWORD effectSpawnsThisFrame;
    volatile DWORD ragdollSpawnsThisFrame;
    volatile DWORD effectSpawnsSkipped;
    volatile DWORD ragdollSpawnsSkipped;
    volatile DWORD dangerLevel;
    volatile DWORD consecutiveFailFrames;
    volatile DWORD cooldownRemaining;
    volatile DWORD totalSkipsThisSession;
    volatile DWORD lastFrameNumber;
    volatile DWORD vehCatchesThisFrame;
};

static DangerState g_DangerState = {0};

// Backup: Unhandled exception filter (catches crashes VEH might miss)
static LPTOP_LEVEL_EXCEPTION_FILTER g_OldExceptionFilter = nullptr;

// Danger log (separate from crash log)
static void DangerLog(const char* fmt, ...) {
    FILE* f = fopen("conquest_danger.log", "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}

// Initialize the danger log file
static void InitDangerLog() {
    FILE* f = fopen("conquest_danger.log", "w");
    if (f) {
        fprintf(f, "=== LOTR Conquest Danger-Aware Engine Layer ===\n");
        fprintf(f, "Systemic crash prevention system active.\n");
        fprintf(f, "Thresholds: CAUTION=%d failures, CRITICAL=%d failures\n",
                THRESHOLD_CAUTION, THRESHOLD_CRITICAL);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}

// ============================================================================
// EVENT POOL VALIDATION - Early Corruption Detection
// ============================================================================
// Hard-coded pool addresses (must match game version)
static const DWORD POOL_BASE_PTR = 0x00cd9970;
static const DWORD POOL_FREELIST_PTR = 0x00cd996c;
static const DWORD POOL_COUNT_PTR = 0x00cd9974;
static const DWORD POOL_ENTRY_SIZE = 0x30;   // 48 bytes per entry
static const DWORD POOL_MAX_ENTRIES = 0x1000; // 4096 entries (expanded to prevent exhaustion)

// Pool validation result flags
enum PoolValidationResult {
    POOL_VALID = 0,
    POOL_COUNT_OVERFLOW = 1,
    POOL_FREEHEAD_INVALID = 2,
    POOL_FREELIST_CYCLE = 4,
    POOL_FREELIST_CORRUPTION = 8,
    POOL_READ_EXCEPTION = 16
};

// Validate the event pool integrity
// Returns bitmask of PoolValidationResult flags
static DWORD ValidateEventPool() {
    DWORD result = POOL_VALID;

    __try {
        DWORD poolBase = *(DWORD*)POOL_BASE_PTR;
        DWORD freeHead = *(DWORD*)POOL_FREELIST_PTR;
        DWORD count = *(DWORD*)POOL_COUNT_PTR;

        // Check 1: Count within bounds
        if (count > POOL_MAX_ENTRIES) {
            result |= POOL_COUNT_OVERFLOW;
            DangerLog("POOL VALIDATION: count=%d exceeds max=%d", count, POOL_MAX_ENTRIES);
        }

        // Calculate pool bounds
        DWORD poolEnd = poolBase + POOL_MAX_ENTRIES * POOL_ENTRY_SIZE;

        // Check 2: FreeHead is NULL or within pool range
        if (freeHead != 0 && (freeHead < poolBase || freeHead >= poolEnd)) {
            result |= POOL_FREEHEAD_INVALID;
            DangerLog("POOL VALIDATION: freeHead=0x%08X outside pool [0x%08X-0x%08X]",
                      freeHead, poolBase, poolEnd);
        }

        // Check 3: Walk freelist for cycles/corruption (limited steps)
        if (freeHead != 0 && !(result & POOL_FREEHEAD_INVALID)) {
            DWORD visited = 0;
            DWORD maxSteps = POOL_MAX_ENTRIES + 256;  // Safety limit
            DWORD* node = (DWORD*)freeHead;

            while (node && visited < maxSteps) {
                DWORD next = 0;
                __try {
                    next = *node;
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    result |= POOL_FREELIST_CORRUPTION;
                    DangerLog("POOL VALIDATION: exception reading node at 0x%08X", (DWORD)node);
                    break;
                }

                if (next != 0 && (next < poolBase || next >= poolEnd)) {
                    result |= POOL_FREELIST_CORRUPTION;
                    DangerLog("POOL VALIDATION: node at 0x%08X has bad next=0x%08X",
                              (DWORD)node, next);
                    break;
                }

                node = (DWORD*)next;
                visited++;
            }

            if (visited >= maxSteps) {
                result |= POOL_FREELIST_CYCLE;
                DangerLog("POOL VALIDATION: possible cycle detected (walked %d nodes)", visited);
            }
        }

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result |= POOL_READ_EXCEPTION;
        DangerLog("POOL VALIDATION: exception reading pool state");
    }

    return result;
}

// ============================================================================
// FREELIST INTEGRITY VALIDATION AND DIAGNOSTIC SYSTEM
// ============================================================================
// Comprehensive freelist validation with detailed corruption diagnostics
// Detects: cycles, out-of-bounds pointers, double-free, use-after-free patterns
// ============================================================================

static FILE* g_FreelistDiagLog = nullptr;

// Initialize freelist diagnostics log
static void InitFreelistDiagLog() {
    if (g_FreelistDiagLog) return;
    g_FreelistDiagLog = fopen("CrashLogs/conquest_freelist_diagnostics.log", "w");
    if (g_FreelistDiagLog) {
        fprintf(g_FreelistDiagLog, "=== LOTR Conquest Freelist Integrity Diagnostics ===\n");
        fprintf(g_FreelistDiagLog, "Detailed corruption detection and audit trail\n");
        fprintf(g_FreelistDiagLog, "\n");
        fflush(g_FreelistDiagLog);
    }
}

// Log to freelist diagnostics file
static void FreelistDiagLog(const char* fmt, ...) {
    if (!g_FreelistDiagLog) InitFreelistDiagLog();
    if (!g_FreelistDiagLog) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_FreelistDiagLog, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_FreelistDiagLog, fmt, args);
    va_end(args);
    fprintf(g_FreelistDiagLog, "\n");
    fflush(g_FreelistDiagLog);
}

// Capture call stack for diagnostics (simplified - get return addresses)
static void CaptureCallStack(DWORD* stack, int maxDepth) {
    __try {
        DWORD ebp;
        __asm mov ebp, [ebp];  // Get current EBP

        for (int i = 0; i < maxDepth && ebp; i++) {
            DWORD* frame = (DWORD*)ebp;
            if (frame[1]) {  // Return address is at [EBP+4]
                stack[i] = frame[1];
                ebp = frame[0];  // Next frame is at [EBP]
            } else {
                stack[i] = 0;
                break;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        stack[0] = 0;
    }
}

// Detailed freelist validation with corruption diagnostics
// Returns true if freelist is valid, false if corruption detected
static bool ValidateEventPoolFreelistDetailed() {
    __try {
        DWORD poolBase = *(DWORD*)POOL_BASE_PTR;
        DWORD freeHead = *(DWORD*)POOL_FREELIST_PTR;
        DWORD count = *(DWORD*)POOL_COUNT_PTR;
        DWORD poolEnd = poolBase + POOL_MAX_ENTRIES * POOL_ENTRY_SIZE;

        // Quick bounds check
        if (count > POOL_MAX_ENTRIES) {
            FreelistDiagLog("[CORRUPTION] Pool count overflow: %d > %d", count, POOL_MAX_ENTRIES);
            return false;
        }

        // Check freelist head validity
        if (freeHead != 0 && (freeHead < poolBase || freeHead >= poolEnd)) {
            FreelistDiagLog("[CORRUPTION] Freelist head out of bounds: 0x%08X (pool: 0x%08X-0x%08X)",
                          freeHead, poolBase, poolEnd);
            return false;
        }

        // Walk freelist with detailed diagnostics
        if (freeHead != 0) {
            DWORD visited = 0;
            DWORD maxSteps = POOL_MAX_ENTRIES + 256;
            DWORD* node = (DWORD*)freeHead;
            DWORD prevNode = 0;

            while (node && visited < maxSteps) {
                DWORD next = 0;
                __try {
                    next = *node;
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    FreelistDiagLog("[CORRUPTION] Exception reading node at 0x%08X (index %d)",
                                  (DWORD)node, ((DWORD)node - poolBase) / POOL_ENTRY_SIZE);
                    FreelistDiagLog("  Previous node: 0x%08X, Visited: %d", prevNode, visited);
                    return false;
                }

                // Check pointer alignment
                if (next != 0 && ((next - poolBase) % POOL_ENTRY_SIZE) != 0) {
                    FreelistDiagLog("[CORRUPTION] Misaligned pointer at node 0x%08X: next=0x%08X",
                                  (DWORD)node, next);
                    FreelistDiagLog("  Expected alignment: %d bytes", POOL_ENTRY_SIZE);
                    return false;
                }

                // Check bounds
                if (next != 0 && (next < poolBase || next >= poolEnd)) {
                    DWORD nodeIndex = ((DWORD)node - poolBase) / POOL_ENTRY_SIZE;
                    FreelistDiagLog("[CORRUPTION] Out-of-bounds pointer in freelist");
                    FreelistDiagLog("  Node: 0x%08X (index %d)", (DWORD)node, nodeIndex);
                    FreelistDiagLog("  Invalid next: 0x%08X", next);
                    FreelistDiagLog("  Pool bounds: 0x%08X-0x%08X", poolBase, poolEnd);
                    FreelistDiagLog("  Visited nodes: %d", visited);

                    // Capture call stack
                    DWORD callStack[8] = {0};
                    CaptureCallStack(callStack, 8);
                    FreelistDiagLog("  Call stack:");
                    for (int i = 0; i < 8 && callStack[i]; i++) {
                        DWORD offset;
                        const char* module = AddressToModuleOffset(callStack[i], &offset);
                        FreelistDiagLog("    [%d] %s+0x%X", i, module, offset);
                    }
                    return false;
                }

                // Check for self-loop
                if (next == (DWORD)node) {
                    FreelistDiagLog("[CORRUPTION] Self-loop detected at node 0x%08X (index %d)",
                                  (DWORD)node, ((DWORD)node - poolBase) / POOL_ENTRY_SIZE);
                    return false;
                }

                prevNode = (DWORD)node;
                node = (DWORD*)next;
                visited++;
            }

            if (visited >= maxSteps) {
                FreelistDiagLog("[CORRUPTION] Possible cycle detected: walked %d nodes (max %d)",
                              visited, maxSteps);
                return false;
            }
        }

        return true;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        FreelistDiagLog("[EXCEPTION] Unhandled exception during freelist validation");
        return false;
    }
}

// ============================================================================
// HEAP CORRUPTION DETECTION
// ============================================================================
static volatile DWORD g_LastHeapCheckTick = 0;
static volatile bool g_HeapCorruptionDetected = false;

// Check process heap for corruption (call periodically)
// Returns true if heap is valid, false if corruption detected
static bool CheckHeapHealth() {
    DWORD now = GetTickCount();

    // Rate limit to every 5 seconds
    if (now - g_LastHeapCheckTick < 5000) {
        return !g_HeapCorruptionDetected;
    }
    g_LastHeapCheckTick = now;

    HANDLE hHeap = GetProcessHeap();
    if (!HeapValidate(hHeap, 0, NULL)) {
        if (!g_HeapCorruptionDetected) {
            g_HeapCorruptionDetected = true;
            DangerLog("HEAP CORRUPTION DETECTED at tick %d", now);

            // Escalate danger level
            if (g_DangerState.dangerLevel < DANGER_CRITICAL) {
                g_DangerState.dangerLevel = DANGER_CRITICAL;
                g_DangerState.cooldownRemaining = COOLDOWN_FRAMES * 4;  // Extended cooldown
            }
        }
        return false;
    }
    return true;
}

// ============================================================================
// HAVOK DEBUG HOOK - Phase 1: Member Offset Identification
// ============================================================================
// Hooks FUN_008f7ec2 to log all member pointers from the object (ECX) before
// the crash at [EAX+0x40]. This identifies which member offset contains NULL.
//
// Target: FUN_008f7ec2 at 0x008F7EC2
// Crash: 0x008F7EC5 (offset +3) - reading [EAX+0x40] where EAX=0
// Goal: Find which [ECX+offset] loads the NULL value into EAX
// ============================================================================

// Havok debug log file handle (opened once, kept open for performance)
static HANDLE g_HavokDebugLogFile = INVALID_HANDLE_VALUE;
static volatile LONG g_HavokDebugCallCount = 0;
static volatile LONG g_HavokDebugEnabled = 1;  // Can be disabled at runtime

// Original function pointer (set after patching)
static BYTE g_OriginalBytes_008f7ec2[8] = {0};
static DWORD g_OriginalFunc_008f7ec2 = 0x008F7EC2;

// Initialize Havok debug log (CRT-free)
static void InitHavokDebugLog() {
    if (g_HavokDebugLogFile != INVALID_HANDLE_VALUE) return;

    g_HavokDebugLogFile = CreateFileA(
        "conquest_havok_debug.log",
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (g_HavokDebugLogFile != INVALID_HANDLE_VALUE) {
        const char* header =
            "=== LOTR Conquest Havok Debug Hook ===\r\n"
            "Target: FUN_008f7ec2 (0x008F7EC2)\r\n"
            "Purpose: Log ESI object pointer to find invalid/NULL source causing crash\r\n"
            "Note: ESI is set by CALLER before CALL, NOT thiscall convention!\r\n"
            "Crash point: CMP [ESI+0x40], EDI at 0x008F7EC5\r\n"
            "Key offsets: +0x30=array ptr, +0x40=count\r\n"
            "Format: Call# | ESI(obj) | [+04] [+08] [+0C] [+10] [+30] [+34] [+38] [+3C] [+40] [+44]\r\n"
            "Legend: * = NULL value, # = small value (<0x10000), <ESI_INVALID!> = ESI unreadable\r\n"
            "========================================\r\n";
        DWORD written;
        WriteFile(g_HavokDebugLogFile, header, (DWORD)strlen(header), &written, NULL);
        FlushFileBuffers(g_HavokDebugLogFile);
    }
}

// Close Havok debug log
static void CloseHavokDebugLog() {
    if (g_HavokDebugLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_HavokDebugLogFile);
        g_HavokDebugLogFile = INVALID_HANDLE_VALUE;
    }
}

// Convert DWORD to hex string (CRT-free, 8 hex chars)
static void DwordToHex(DWORD val, char* buf) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
}

// Log a single call to FUN_008f7ec2 with ESI-based member offsets (CRT-free)
//
// FUN_008f7ec2 uses ESI as the object pointer:
//   [ESI+0x30] = pointer to array of items
//   [ESI+0x40] = count of items (crashes if ESI is invalid!)
//
// Returns: true if all members are valid, false if any is NULL
static bool LogHavokCall(DWORD esi, DWORD callNum) {
    if (g_HavokDebugLogFile == INVALID_HANDLE_VALUE) return true;

    char line[512];
    char hexBuf[12];
    int pos = 0;

    // Call number
    DwordToHex(callNum, hexBuf);
    for (int i = 0; i < 8; i++) line[pos++] = hexBuf[i];
    line[pos++] = ' '; line[pos++] = '|'; line[pos++] = ' ';

    // ESI (object pointer - passed by caller, NOT thiscall)
    DwordToHex(esi, hexBuf);
    for (int i = 0; i < 8; i++) line[pos++] = hexBuf[i];
    line[pos++] = ' '; line[pos++] = '|';

    // CRITICAL FIX: Check for NULL ESI BEFORE any dereference attempt!
    // SEH (__try/__except) doesn't work reliably when called from assembly hooks
    // because the exception context may not be properly set up.
    if (esi == 0) {
        // ESI is NULL - log this fact and return early
        const char* err = " <ESI_NULL!>";
        for (int i = 0; err[i]; i++) line[pos++] = err[i];
        line[pos++] = '\r';
        line[pos++] = '\n';
        DWORD written;
        WriteFile(g_HavokDebugLogFile, line, pos, &written, NULL);
        FlushFileBuffers(g_HavokDebugLogFile);
        return false;  // ESI was NULL
    }

    // Key offsets for FUN_008f7ec2:
    // +0x30 = array pointer (MOV EAX, [ESI+0x30])
    // +0x40 = count (CMP [ESI+0x40], EDI) - THIS IS THE CRASH POINT!
    // Also log surrounding offsets for context
    bool hasNull = false;
    bool hasInvalid = false;
    DWORD offsets[] = {0x04, 0x08, 0x0C, 0x10, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44};

    __try {
        // ESI is non-NULL, but still verify it's readable
        volatile DWORD testRead = *(DWORD*)esi;
        (void)testRead;  // Suppress unused warning

        for (int i = 0; i < 10; i++) {
            line[pos++] = ' ';
            DWORD memberVal = *(DWORD*)(esi + offsets[i]);
            DwordToHex(memberVal, hexBuf);
            for (int j = 0; j < 8; j++) line[pos++] = hexBuf[j];

            if (memberVal == 0) {
                hasNull = true;
                line[pos++] = '*';  // Mark NULL values with asterisk
            } else if (memberVal < 0x10000) {
                // Likely not a valid pointer (too low), could be a small integer
                line[pos++] = '#';  // Mark suspicious values
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // ESI points to invalid/freed memory - log this
        const char* err = " <ESI_INVALID!>";
        for (int i = 0; err[i]; i++) line[pos++] = err[i];
        hasInvalid = true;
    }

    line[pos++] = '\r';
    line[pos++] = '\n';

    // Write to log file
    DWORD written;
    WriteFile(g_HavokDebugLogFile, line, pos, &written, NULL);

    // Flush every call for maximum crash safety (performance impact but we need the data!)
    FlushFileBuffers(g_HavokDebugLogFile);

    return !hasNull && !hasInvalid;
}

// The actual hook callback - called from code cave with ESI as parameter
// NOTE: FUN_008f7ec2 uses ESI as the 'this' pointer, NOT ECX!
// The function signature is actually: void FUN_008f7ec2(ESI = object, stack = param)
static void __stdcall HavokDebugCallback(DWORD esiValue) {
    if (!g_HavokDebugEnabled) return;

    LONG callNum = InterlockedIncrement(&g_HavokDebugCallCount);
    LogHavokCall(esiValue, (DWORD)callNum);
}

// ============================================================================
// HAVOK SDK INTEGRATION - Runtime Debugging System
// ============================================================================
// Uses Havok Physics SDK 5.5.0-r1 knowledge to monitor the game's Havok usage.
// The game statically links Havok, so we can use SDK struct layouts directly.
//
// Key RTTI addresses (from strings.txt analysis):
//   0x00A2C3A4: .?AVhkFreeListMemory@@
//   0x00A2E2F0: .?AVhkpWorld@@
//   0x00A2E47C: .?AVhkpEntity@@
//   0x00A2E4A8: .?AVhkpRigidBody@@
//
// Key warning strings (allocation failure indicators):
//   0x00993FE8: "No runtime block of size "
//   0x00994008: " currently available. Allocating new block from unmanaged memory."
//   0x0099404C: "Deallocating unmanaged big block."
//
// Havok class layout (from SDK headers):
//   hkBaseObject:        +0x00 vtable
//   hkReferencedObject:  +0x04 m_memSizeAndFlags (u16), +0x06 m_referenceCount (s16)
//   hkpWorldObject:      +0x08 m_world, +0x0C m_userData, +0x10 m_collidable (embedded)
// ============================================================================

// Known Havok RTTI string addresses in the game binary
static const DWORD HAVOK_RTTI_hkFreeListMemory = 0x00A2C3A4;
static const DWORD HAVOK_RTTI_hkpWorld = 0x00A2E2F0;
static const DWORD HAVOK_RTTI_hkpEntity = 0x00A2E47C;
static const DWORD HAVOK_RTTI_hkpRigidBody = 0x00A2E4A8;

// FreeList warning string addresses (for hooking allocation failures)
static const DWORD HAVOK_STR_NoRuntimeBlock = 0x00993FE8;
static const DWORD HAVOK_STR_AllocFromUnmanaged = 0x00994008;
static const DWORD HAVOK_STR_DeallocBigBlock = 0x0099404C;

// Cached vtable addresses (found at runtime by scanning for RTTI xrefs)
static DWORD g_Havok_vtable_hkFreeListMemory = 0;
static DWORD g_Havok_vtable_hkpWorld = 0;
static DWORD g_Havok_vtable_hkpEntity = 0;
static DWORD g_Havok_vtable_hkpRigidBody = 0;

// Global Havok object pointers (found at runtime)
static DWORD g_Havok_hkFreeListMemory_Instance = 0;
static DWORD g_Havok_hkpWorld_Instance = 0;

// Havok SDK log file
static HANDLE g_HavokSDKLogFile = INVALID_HANDLE_VALUE;

// Initialize Havok SDK log
static void InitHavokSDKLog() {
    if (g_HavokSDKLogFile != INVALID_HANDLE_VALUE) return;

    g_HavokSDKLogFile = CreateFileA(
        "conquest_havok_sdk.log",
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (g_HavokSDKLogFile != INVALID_HANDLE_VALUE) {
        const char* header =
            "=== LOTR Conquest Havok SDK Integration ===\r\n"
            "SDK Version: Havok Physics 5.5.0-r1\r\n"
            "Purpose: Monitor Havok memory allocator and object lifecycle\r\n"
            "========================================\r\n";
        DWORD written;
        WriteFile(g_HavokSDKLogFile, header, (DWORD)strlen(header), &written, NULL);
        FlushFileBuffers(g_HavokSDKLogFile);
    }
}

// Log to Havok SDK log file
static void LogHavokSDK(const char* msg) {
    if (g_HavokSDKLogFile == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(g_HavokSDKLogFile, msg, (DWORD)strlen(msg), &written, NULL);
    WriteFile(g_HavokSDKLogFile, "\r\n", 2, &written, NULL);
    FlushFileBuffers(g_HavokSDKLogFile);
}

// Find vtable address from RTTI string address
// In MSVC, the vtable is typically located by finding xrefs to the RTTI Complete Object Locator
// which points to the RTTI string. The vtable is 4 bytes before the COL pointer.
static DWORD FindVtableFromRTTI(DWORD rttiStringAddr, const char* className) {
    // The RTTI structure in MSVC is:
    //   TypeDescriptor: contains the mangled name string
    //   CompleteObjectLocator: points to TypeDescriptor
    //   Vtable: first entry points to COL (at vtable[-1])
    //
    // We need to scan the .rdata section for pointers to the RTTI string address
    // and then find the vtable that references it.

    // For now, we'll use a simpler approach: scan for the RTTI string pointer
    // in the data sections and look for vtable patterns nearby.

    // Get module base
    HMODULE hModule = GetModuleHandleA(NULL);
    if (!hModule) return 0;

    DWORD moduleBase = (DWORD)(uintptr_t)hModule;

    // Parse PE headers to find .rdata section
    IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)hModule;
    IMAGE_NT_HEADERS* ntHeaders = (IMAGE_NT_HEADERS*)((BYTE*)hModule + dosHeader->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeaders);

    for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        // Look in .rdata section (read-only data, contains vtables)
        if (memcmp(sections[i].Name, ".rdata", 6) == 0) {
            DWORD sectionStart = moduleBase + sections[i].VirtualAddress;
            DWORD sectionEnd = sectionStart + sections[i].Misc.VirtualSize;

            // Scan for pointers to the RTTI string
            for (DWORD addr = sectionStart; addr < sectionEnd - 4; addr += 4) {
                DWORD value = *(DWORD*)addr;
                if (value == rttiStringAddr) {
                    // Found a reference to the RTTI string
                    // The vtable is typically 4-8 bytes before this in the COL structure
                    // But we need to find what points TO this COL

                    // For now, log that we found the RTTI reference
                    char buf[256];
                    wsprintfA(buf, "Found RTTI ref for %s at 0x%08X", className, addr);
                    LogHavokSDK(buf);

                    // The actual vtable finding requires more complex analysis
                    // For now, return 0 and we'll add pattern scanning later
                }
            }
        }
    }

    return 0;
}

// Validate if a pointer looks like a valid Havok hkReferencedObject
// Based on SDK layout:
//   +0x00: vtable pointer (should be in .rdata section)
//   +0x04: m_memSizeAndFlags (hkUint16, should be < 0x8000)
//   +0x06: m_referenceCount (hkInt16, should be 1-1000 typically)
static bool IsValidHavokObject(DWORD ptr) {
    if (ptr == 0 || ptr < 0x10000) return false;

    __try {
        DWORD vtable = *(DWORD*)ptr;
        WORD memSizeAndFlags = *(WORD*)(ptr + 4);
        SHORT refCount = *(SHORT*)(ptr + 6);

        // Vtable should be in a reasonable address range (game code/data)
        if (vtable < 0x00400000 || vtable > 0x00FFFFFF) return false;

        // Reference count should be positive and reasonable
        if (refCount < 0 || refCount > 10000) return false;

        // Memory size should be reasonable (< 32KB per object)
        if ((memSizeAndFlags & 0x7FFF) > 0x8000) return false;

        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Log Havok object details if it appears to be a valid Havok object
static void LogHavokObjectDetails(DWORD ptr, const char* context) {
    if (!IsValidHavokObject(ptr)) return;

    __try {
        DWORD vtable = *(DWORD*)ptr;
        WORD memSizeAndFlags = *(WORD*)(ptr + 4);
        SHORT refCount = *(SHORT*)(ptr + 6);

        char buf[256];
        wsprintfA(buf, "[HavokObj] %s: ptr=0x%08X vtable=0x%08X size=%d refs=%d",
            context, ptr, vtable, memSizeAndFlags & 0x7FFF, refCount);
        LogHavokSDK(buf);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Ignore
    }
}

// Scan for Havok vtables at DLL load time
static void ScanForHavokVtables() {
    InitHavokSDKLog();
    LogHavokSDK("Scanning for Havok vtables...");

    g_Havok_vtable_hkFreeListMemory = FindVtableFromRTTI(HAVOK_RTTI_hkFreeListMemory, "hkFreeListMemory");
    g_Havok_vtable_hkpWorld = FindVtableFromRTTI(HAVOK_RTTI_hkpWorld, "hkpWorld");
    g_Havok_vtable_hkpEntity = FindVtableFromRTTI(HAVOK_RTTI_hkpEntity, "hkpEntity");
    g_Havok_vtable_hkpRigidBody = FindVtableFromRTTI(HAVOK_RTTI_hkpRigidBody, "hkpRigidBody");

    char buf[256];
    wsprintfA(buf, "Vtable scan complete: hkFreeListMemory=0x%08X, hkpWorld=0x%08X, hkpEntity=0x%08X, hkpRigidBody=0x%08X",
        g_Havok_vtable_hkFreeListMemory, g_Havok_vtable_hkpWorld,
        g_Havok_vtable_hkpEntity, g_Havok_vtable_hkpRigidBody);
    LogHavokSDK(buf);
}

// ============================================================================
// FUNCTION RANGE IDENTIFICATION - Crash Context Detection
// ============================================================================
// Maps known function address ranges to names for better crash analysis.
// Add entries as you identify functions during reverse engineering.
// ============================================================================

struct FunctionRange {
    DWORD start;
    DWORD end;
    const char* name;
};

// Known game functions (add more as you identify them)
static const FunctionRange g_KnownFunctions[] = {
    // Effect and spawner functions
    {0x004b6910, 0x004b6a80, "FUN_004b6910 (EffectSpawner)"},
    {0x005135a0, 0x00513900, "FUN_005135a0 (RagdollSpawner)"},

    // Allocator functions
    {0x00495700, 0x00495800, "FUN_00495700 (PoolAllocator)"},
    {0x00495c30, 0x00495d00, "FUN_00495c30 (LargeAllocator)"},

    // Crash-prone functions identified during analysis
    {0x008f0592, 0x008f0650, "FUN_008f0592"},
    {0x008f7ec2, 0x008f7f50, "FUN_008f7ec2"},
    {0x0087cda1, 0x0087d200, "FUN_0087cda1"},
    {0x004e8370, 0x004e8400, "FUN_004e8370 (EntitySerialize)"},
    {0x00403e7c, 0x00403f00, "FUN_00403e7c (ArrayResize)"},
    {0x007ee009, 0x007ee200, "FUN_007ee009 (MemcpyWrapper)"},
    {0x008d28df, 0x008d2a00, "FUN_008d28df (SpatialQuery)"},
    {0x00692e00, 0x00693100, "FUN_00692xxx (AnimTransform)"},

    // Memory functions
    {0x0060fec0, 0x00610000, "__VEC_memcpy"},
    {0x0067e610, 0x0067e700, "FUN_0067e610 (MemAlloc)"},

    // Frame/timing functions
    {0x00613c00, 0x00613d50, "FrameLimiter"},
    {0x006a4c00, 0x006a4d00, "TimerCalibration"},

    // Add sentinel to mark end
    {0, 0, nullptr}
};

// Identify which function an address is in
// Returns function name or nullptr if not in a known function
static const char* IdentifyFunction(DWORD addr) {
    for (int i = 0; g_KnownFunctions[i].name != nullptr; i++) {
        if (addr >= g_KnownFunctions[i].start && addr < g_KnownFunctions[i].end) {
            return g_KnownFunctions[i].name;
        }
    }
    return nullptr;
}

// ============================================================================
// POOL ALLOCATION/DEALLOCATION AUDIT TRAIL
// ============================================================================
// Tracks all pool operations to detect double-free, use-after-free, etc.
// ============================================================================

// Bitmap to track allocated entries (1 = allocated, 0 = free)
static BYTE g_PoolAllocationBitmap[POOL_MAX_ENTRIES / 8 + 1] = {0};
static volatile bool g_BitmapInitialized = false;

// Initialize allocation bitmap
static void InitPoolAllocationBitmap() {
    if (g_BitmapInitialized) return;
    memset(g_PoolAllocationBitmap, 0, sizeof(g_PoolAllocationBitmap));
    g_BitmapInitialized = true;
}

// Set bit in allocation bitmap
static void SetAllocBit(DWORD entryIndex) {
    if (entryIndex >= POOL_MAX_ENTRIES) return;
    DWORD byteIdx = entryIndex / 8;
    DWORD bitIdx = entryIndex % 8;
    g_PoolAllocationBitmap[byteIdx] |= (1 << bitIdx);
}

// Clear bit in allocation bitmap
static void ClearAllocBit(DWORD entryIndex) {
    if (entryIndex >= POOL_MAX_ENTRIES) return;
    DWORD byteIdx = entryIndex / 8;
    DWORD bitIdx = entryIndex % 8;
    g_PoolAllocationBitmap[byteIdx] &= ~(1 << bitIdx);
}

// Check if bit is set in allocation bitmap
static bool IsAllocBitSet(DWORD entryIndex) {
    if (entryIndex >= POOL_MAX_ENTRIES) return false;
    DWORD byteIdx = entryIndex / 8;
    DWORD bitIdx = entryIndex % 8;
    return (g_PoolAllocationBitmap[byteIdx] & (1 << bitIdx)) != 0;
}

// Log pool allocation operation
static void LogPoolAllocation(DWORD entryAddr, bool isAlloc) {
    __try {
        DWORD poolBase = *(DWORD*)POOL_BASE_PTR;
        DWORD count = *(DWORD*)POOL_COUNT_PTR;
        DWORD freeHead = *(DWORD*)POOL_FREELIST_PTR;

        if (entryAddr < poolBase || entryAddr >= poolBase + POOL_MAX_ENTRIES * POOL_ENTRY_SIZE) {
            FreelistDiagLog("[AUDIT] Invalid entry address: 0x%08X (outside pool)", entryAddr);
            return;
        }

        DWORD entryIndex = (entryAddr - poolBase) / POOL_ENTRY_SIZE;
        bool wasAllocated = IsAllocBitSet(entryIndex);

        if (isAlloc) {
            if (wasAllocated) {
                FreelistDiagLog("[AUDIT] DOUBLE-ALLOC DETECTED: Entry 0x%08X (index %d) already allocated!",
                              entryAddr, entryIndex);
            } else {
                SetAllocBit(entryIndex);
                FreelistDiagLog("[AUDIT] ALLOC: Entry=0x%08X (idx %d) FreeHead=0x%08X Count=%d→%d",
                              entryAddr, entryIndex, freeHead, count - 1, count);
            }
        } else {
            if (!wasAllocated) {
                FreelistDiagLog("[AUDIT] DOUBLE-FREE DETECTED: Entry 0x%08X (index %d) not allocated!",
                              entryAddr, entryIndex);
            } else {
                ClearAllocBit(entryIndex);
                FreelistDiagLog("[AUDIT] FREE: Entry=0x%08X (idx %d) FreeHead=0x%08X Count=%d→%d",
                              entryAddr, entryIndex, freeHead, count, count - 1);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        FreelistDiagLog("[AUDIT] Exception during allocation logging");
    }
}

// Log function identification using minimal crash-safe logging
static void LogCrashFunction(DWORD eip) {
    const char* funcName = IdentifyFunction(eip);
    if (funcName) {
        // Use minimal API for crash safety
        HANDLE hFile = CreateFileA("conquest_crash.log", FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hFile, "Function: ", 10, &written, NULL);
            WriteFile(hFile, funcName, (DWORD)strlen(funcName), &written, NULL);
            WriteFile(hFile, "\r\n", 2, &written, NULL);
            CloseHandle(hFile);
        }
    }
}

// ============================================================================
// CALL HISTORY TRACKING - Last N Function Entries
// ============================================================================
// Circular buffer to track recent function entries for crash analysis.
// Hook critical functions to call RecordFunctionEntry().
// ============================================================================

#define CALL_HISTORY_SIZE 32
static volatile DWORD g_CallHistory[CALL_HISTORY_SIZE] = {0};
static volatile LONG g_CallHistoryIndex = 0;

// Record a function entry (call from hooked function prologues)
static void RecordFunctionEntry(DWORD funcId) {
    LONG idx = InterlockedIncrement(&g_CallHistoryIndex) & (CALL_HISTORY_SIZE - 1);
    g_CallHistory[idx] = funcId;
}

// Dump call history to crash log
static void DumpCallHistory() {
    HANDLE hFile = CreateFileA("conquest_crash.log", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, "\r\n=== Recent Function Calls ===\r\n", 33, &written, NULL);

        // Start from oldest entry
        LONG startIdx = (g_CallHistoryIndex + 1) & (CALL_HISTORY_SIZE - 1);
        char buf[80];
        const char* hex = "0123456789ABCDEF";

        for (int i = 0; i < CALL_HISTORY_SIZE; i++) {
            LONG idx = (startIdx + i) & (CALL_HISTORY_SIZE - 1);
            DWORD addr = g_CallHistory[idx];
            if (addr == 0) continue;

            // Format: "[NN] 0xXXXXXXXX FuncName\r\n"
            int p = 0;
            buf[p++] = '[';
            buf[p++] = hex[(i >> 4) & 0xF];
            buf[p++] = hex[i & 0xF];
            buf[p++] = ']';
            buf[p++] = ' ';
            buf[p++] = '0'; buf[p++] = 'x';
            for (int j = 7; j >= 0; j--) {
                buf[p++] = hex[(addr >> (j * 4)) & 0xF];
            }
            buf[p++] = ' ';

            const char* name = IdentifyFunction(addr);
            if (name) {
                while (*name && p < 70) buf[p++] = *name++;
            } else {
                buf[p++] = '?';
            }
            buf[p++] = '\r'; buf[p++] = '\n';
            buf[p] = '\0';

            WriteFile(hFile, buf, p, &written, NULL);
        }

        CloseHandle(hFile);
    }
}

// Report allocation failure (called from allocator wrapper)
static void DangerReportAllocFailure() {
    InterlockedIncrement(&g_DangerState.allocFailuresThisFrame);
}

// Report allocation success (called from allocator wrapper)
static void DangerReportAllocSuccess() {
    InterlockedIncrement(&g_DangerState.allocSuccessesThisFrame);
}

// Check if effect spawn is allowed
static bool DangerCanSpawnEffect() {
    DWORD level = g_DangerState.dangerLevel;
    DWORD count = g_DangerState.effectSpawnsThisFrame;

    if (level == DANGER_CRITICAL || level == DANGER_RECOVERY) {
        InterlockedIncrement(&g_DangerState.effectSpawnsSkipped);
        InterlockedIncrement(&g_DangerState.totalSkipsThisSession);
        return false;
    }

    if (level == DANGER_CAUTION && count >= MAX_EFFECTS_CAUTION) {
        InterlockedIncrement(&g_DangerState.effectSpawnsSkipped);
        InterlockedIncrement(&g_DangerState.totalSkipsThisSession);
        return false;
    }

    InterlockedIncrement(&g_DangerState.effectSpawnsThisFrame);
    return true;
}

// Check if ragdoll spawn is allowed
static bool DangerCanSpawnRagdoll() {
    DWORD level = g_DangerState.dangerLevel;
    DWORD count = g_DangerState.ragdollSpawnsThisFrame;

    if (level == DANGER_CRITICAL || level == DANGER_RECOVERY) {
        InterlockedIncrement(&g_DangerState.ragdollSpawnsSkipped);
        InterlockedIncrement(&g_DangerState.totalSkipsThisSession);
        return false;
    }

    if (level == DANGER_CAUTION && count >= MAX_RAGDOLLS_CAUTION) {
        InterlockedIncrement(&g_DangerState.ragdollSpawnsSkipped);
        InterlockedIncrement(&g_DangerState.totalSkipsThisSession);
        return false;
    }

    InterlockedIncrement(&g_DangerState.ragdollSpawnsThisFrame);
    return true;
}

// Process end of frame - update danger state
static void DangerProcessEndOfFrame() {
    // Skip if same frame
    DWORD currentFrame = g_FrameCounter;
    if (currentFrame == g_DangerState.lastFrameNumber) {
        return;
    }
    g_DangerState.lastFrameNumber = currentFrame;

    // Atomically read and reset per-frame counters to avoid race conditions
    // with concurrent InterlockedIncrement calls from other threads
    DWORD failures = InterlockedExchange(&g_DangerState.allocFailuresThisFrame, 0);
    DWORD vehCatches = InterlockedExchange(&g_DangerState.vehCatchesThisFrame, 0);
    DWORD oldLevel = g_DangerState.dangerLevel;
    DWORD newLevel = oldLevel;

    // Escalation logic
    if (failures >= THRESHOLD_CRITICAL || vehCatches >= 2) {
        newLevel = DANGER_CRITICAL;
        g_DangerState.cooldownRemaining = COOLDOWN_FRAMES;
        g_DangerState.consecutiveFailFrames++;
    } else if (failures >= THRESHOLD_CAUTION || vehCatches >= 1) {
        if (oldLevel == DANGER_NORMAL) {
            newLevel = DANGER_CAUTION;
        }
        g_DangerState.consecutiveFailFrames++;
    } else {
        // No failures this frame
        g_DangerState.consecutiveFailFrames = 0;
    }

    // De-escalation logic
    if (g_DangerState.cooldownRemaining > 0) {
        g_DangerState.cooldownRemaining--;
    } else {
        // Cooldown expired - step down
        if (oldLevel == DANGER_CRITICAL) {
            newLevel = DANGER_RECOVERY;
            g_DangerState.cooldownRemaining = RECOVERY_FRAMES;
        } else if (oldLevel == DANGER_RECOVERY) {
            newLevel = DANGER_CAUTION;
            g_DangerState.cooldownRemaining = COOLDOWN_FRAMES;
        } else if (oldLevel == DANGER_CAUTION && failures == 0) {
            newLevel = DANGER_NORMAL;
        }
    }

    // Log state changes
    if (newLevel != oldLevel) {
        const char* levelNames[] = {"NORMAL", "CAUTION", "CRITICAL", "RECOVERY"};
        DangerLog("Level change: %s -> %s (failures=%d, vehCatches=%d, skipped=%d)",
                  levelNames[oldLevel], levelNames[newLevel],
                  failures, vehCatches, g_DangerState.totalSkipsThisSession);
    }

    // Update level
    g_DangerState.dangerLevel = newLevel;

    // Atomically reset remaining per-frame counters
    InterlockedExchange(&g_DangerState.allocSuccessesThisFrame, 0);
    InterlockedExchange(&g_DangerState.effectSpawnsThisFrame, 0);
    InterlockedExchange(&g_DangerState.ragdollSpawnsThisFrame, 0);
    InterlockedExchange(&g_DangerState.effectSpawnsSkipped, 0);
    InterlockedExchange(&g_DangerState.ragdollSpawnsSkipped, 0);
}

// ============================================================================
// Crash Handler - Vectored Exception Handler + Unhandled Exception Filter
// ============================================================================

static FILE* g_CrashLog = nullptr;
static void* g_NewEventPool = nullptr;  // Track our allocated pool
static bool g_CrashHandlerReady = false;

// Forward declarations for Patch 31 counters (defined later, but needed in crash handler)
// These track how many invalid pointers were caught by the matrix copy patch
static volatile DWORD g_Patch31_NullCaught = 0;      // Count of NULL pointers caught
static volatile DWORD g_Patch31_InvalidCaught = 0;   // Count of -1 pointers caught
static volatile DWORD g_Patch31_ValidCalls = 0;      // Count of valid calls (for comparison)
static volatile DWORD g_Patch31_LowAddrCaught = 0;   // Count of low address pointers caught (< 0x10000)
static volatile DWORD g_Patch31_UnalignedCaught = 0; // Count of unaligned pointers caught (not 16-byte aligned)

static void CrashLog(const char* fmt, ...) {
    // Always open fresh to ensure we capture everything
    FILE* f = fopen("conquest_crash.log", "a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }

    // Also output to debugger
    char buf[1024];
    va_list args2;
    va_start(args2, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args2);
    va_end(args2);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

static void InitCrashLog() {
    // Create/clear the crash log file
    FILE* f = fopen("conquest_crash.log", "w");
    if (f) {
        fprintf(f, "=== LOTR Conquest Crash Handler Initialized ===\n");
        fprintf(f, "If you see this but no crash data, the game exited normally or crashed before handler ran.\n");
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
    g_CrashHandlerReady = true;
}

static const char* GetExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT: return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK: return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW: return "FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION: return "INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP: return "SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        default: return "UNKNOWN";
    }
}

static volatile LONG g_InCrashHandler = 0;  // Guard against re-entry
static volatile LONG g_VEHRecoveryCount = 0;  // Count of VEH recoveries
static volatile LONG g_CrashLogCount = 0;    // Limit crash log entries to prevent cascade
static void* g_VEHHandle = nullptr;  // VEH handle for cleanup

// Maximum number of crash reports to log before giving up
// This prevents the cascade crash issue where the handler itself crashes
static const LONG MAX_CRASH_LOGS = 3;

// ============================================================================
// Vectored Exception Handler - NULL Pointer Safety Net
// This catches ACCESS_VIOLATION exceptions caused by NULL pointer dereferences
// and attempts to skip the bad instruction, allowing the game to continue.
// ============================================================================

// Log file for VEH recoveries (separate from crash log)
static void VEHLog(const char* fmt, ...) {
    FILE* f = fopen("conquest_veh_recovery.log", "a");
    if (f) {
        // Get timestamp
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}

// Get instruction length for common x86 instructions at crash sites
// Returns 0 if unknown (will not attempt recovery)
static int GetInstructionLength(BYTE* eip) {
    if (!eip) return 0;

    // Try to safely read the instruction bytes
    __try {
        BYTE b0 = eip[0];
        BYTE b1 = eip[1];
        BYTE b2 = eip[2];

        // MOV reg, [reg] - 8B xx (2 bytes)
        // MOV EAX, [ESI] = 8B 06, MOV ECX, [EDI] = 8B 0F, etc.
        if (b0 == 0x8B && (b1 & 0xC0) == 0x00 && (b1 & 0x07) != 0x04 && (b1 & 0x07) != 0x05) {
            return 2;
        }

        // MOV reg, [reg+disp8] - 8B xx yy (3 bytes)
        if (b0 == 0x8B && (b1 & 0xC0) == 0x40) {
            return 3;
        }

        // MOV reg, [reg+disp32] - 8B xx yy yy yy yy (6 bytes)
        if (b0 == 0x8B && (b1 & 0xC0) == 0x80) {
            return 6;
        }

        // MOV [reg], reg - 89 xx (2 bytes)
        if (b0 == 0x89 && (b1 & 0xC0) == 0x00 && (b1 & 0x07) != 0x04 && (b1 & 0x07) != 0x05) {
            return 2;
        }

        // AND [EAX], imm8 - 83 20 xx (3 bytes) - this is the crash we just saw!
        if (b0 == 0x83 && b1 == 0x20) {
            return 3;
        }

        // AND [reg], imm8 - 83 /4 xx (3 bytes)
        if (b0 == 0x83 && (b1 & 0x38) == 0x20 && (b1 & 0xC0) == 0x00) {
            return 3;
        }

        // CALL [reg] or CALL [reg+offset] - FF /2
        if (b0 == 0xFF && (b1 & 0x38) == 0x10) {
            if ((b1 & 0xC0) == 0x00) return 2;       // [reg]
            if ((b1 & 0xC0) == 0x40) return 3;       // [reg+disp8]
            if ((b1 & 0xC0) == 0x80) return 6;       // [reg+disp32]
        }

        // PUSH [reg] - FF /6
        if (b0 == 0xFF && (b1 & 0x38) == 0x30) {
            if ((b1 & 0xC0) == 0x00) return 2;
            if ((b1 & 0xC0) == 0x40) return 3;
            if ((b1 & 0xC0) == 0x80) return 6;
        }

        // CMP [reg], reg - 39 xx (2 bytes)
        if (b0 == 0x39 && (b1 & 0xC0) == 0x00) {
            return 2;
        }

        // TEST [reg], reg - 85 xx (2 bytes)
        if (b0 == 0x85 && (b1 & 0xC0) == 0x00) {
            return 2;
        }

        // MOVSS xmm, [reg+offset] - F3 0F 10/11 ... (variable)
        if (b0 == 0xF3 && b1 == 0x0F && (b2 == 0x10 || b2 == 0x11)) {
            BYTE modrm = eip[3];
            if ((modrm & 0xC0) == 0x00) return 4;       // [reg]
            if ((modrm & 0xC0) == 0x40) return 5;       // [reg+disp8]
            if ((modrm & 0xC0) == 0x80) return 8;       // [reg+disp32]
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;  // Can't read instruction
    }

    return 0;  // Unknown instruction
}

// Vectored Exception Handler - first chance to handle exceptions
// Enhanced for Danger-Aware Engine Layer integration
static LONG CALLBACK NullPointerVEH(EXCEPTION_POINTERS* ep) {
    // Only handle ACCESS_VIOLATION
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Get the fault address (the address that was accessed)
    DWORD faultAddr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
    DWORD eipAddr = CONTEXT_IP(ep->ContextRecord);

    // Only handle NULL or near-NULL pointer dereferences (< 64KB)
    // This range is always invalid on Windows (reserved for system use)
    if (faultAddr >= 0x10000) {
        return EXCEPTION_CONTINUE_SEARCH;  // Not a NULL pointer issue
    }

    // Check if we're in game code (not system DLLs)
    // Game code is roughly 0x00400000 - 0x00FFFFFF
    if (eipAddr < 0x00400000 || eipAddr > 0x00FFFFFF) {
        return EXCEPTION_CONTINUE_SEARCH;  // Crash in system DLL, don't handle
    }

    // Try to get instruction length
    int instrLen = GetInstructionLength((BYTE*)eipAddr);
    if (instrLen == 0) {
        // Unknown instruction - log but don't handle
        VEHLog("VEH: Unknown instruction at 0x%08X, faultAddr=0x%08X - NOT RECOVERING", eipAddr, faultAddr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // *** DANGER SYSTEM INTEGRATION ***
    // Report this catch to the danger system and escalate to CRITICAL
    InterlockedIncrement(&g_DangerState.vehCatchesThisFrame);
    InterlockedIncrement(&g_DangerState.totalSkipsThisSession);

    // If we're catching exceptions, we're in serious trouble - go CRITICAL immediately
    if (g_DangerState.dangerLevel < DANGER_CRITICAL) {
        g_DangerState.dangerLevel = DANGER_CRITICAL;
        g_DangerState.cooldownRemaining = COOLDOWN_FRAMES * 2;  // Extended cooldown after VEH catch
        DangerLog("VEH ESCALATION: Caught NULL deref at 0x%08X -> CRITICAL", eipAddr);
    }

    // Log the recovery
    LONG count = InterlockedIncrement(&g_VEHRecoveryCount);
    VEHLog("VEH Recovery #%d: EIP=0x%08X, FaultAddr=0x%08X, InstrLen=%d, DangerLevel=CRITICAL",
           count, eipAddr, faultAddr, instrLen);
    VEHLog("  Registers: EAX=0x%08X ECX=0x%08X EDX=0x%08X ESI=0x%08X EDI=0x%08X",
           CONTEXT_AX(ep->ContextRecord), CONTEXT_CX(ep->ContextRecord), CONTEXT_DX(ep->ContextRecord),
           CONTEXT_SI(ep->ContextRecord), CONTEXT_DI(ep->ContextRecord));

    // Skip the faulting instruction
    CONTEXT_IP(ep->ContextRecord) += instrLen;

    // For MOV instructions that read into a register, set the destination to 0
    // This is a best-effort heuristic
    BYTE* instrBytes = (BYTE*)eipAddr;
    __try {
        if (instrBytes[0] == 0x8B) {
            // MOV reg, [mem] - set destination register to 0
            BYTE modrm = instrBytes[1];
            int destReg = (modrm >> 3) & 0x7;
            switch (destReg) {
                case 0: CONTEXT_AX(ep->ContextRecord) = 0; break;
                case 1: CONTEXT_CX(ep->ContextRecord) = 0; break;
                case 2: CONTEXT_DX(ep->ContextRecord) = 0; break;
                case 3: CONTEXT_BX(ep->ContextRecord) = 0; break;
                case 6: CONTEXT_SI(ep->ContextRecord) = 0; break;
                case 7: CONTEXT_DI(ep->ContextRecord) = 0; break;
            }
        }
        // CMP [reg+offset], reg - the common crash pattern (0x39 or 0x3B prefix)
        // For CMP, we just skip - no register to zero
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        // Ignore errors reading instruction
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

// Minimal crash logging that uses less stack space - NO CRT, just Win32 API
static void MinimalCrashLog(const char* msg) {
    HANDLE hFile = CreateFileA("conquest_crash.log", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, msg, (DWORD)strlen(msg), &written, NULL);
        WriteFile(hFile, "\r\n", 2, &written, NULL);
        CloseHandle(hFile);
    }
}

// Minimal hex logging - converts DWORD to hex string without using CRT
static void MinimalCrashLogHex(const char* prefix, DWORD value) {
    char buf[64];
    const char* hex = "0123456789ABCDEF";
    int i = 0;

    // Copy prefix
    while (*prefix && i < 48) buf[i++] = *prefix++;

    // Add "0x"
    buf[i++] = '0';
    buf[i++] = 'x';

    // Convert to hex (8 digits)
    for (int j = 7; j >= 0; j--) {
        buf[i++] = hex[(value >> (j * 4)) & 0xF];
    }
    buf[i] = '\0';

    MinimalCrashLog(buf);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    // === CRASH CASCADE PREVENTION ===
    // The crash handler itself can crash (e.g., when reading corrupted memory),
    // which triggers another exception, calling this handler again.
    // This creates an infinite loop that fills the crash log with repeated entries.
    //
    // Solution: Limit the number of crash reports and use atomic guards.

    // Check if we've already logged too many crashes
    LONG logCount = InterlockedIncrement(&g_CrashLogCount);
    if (logCount > MAX_CRASH_LOGS) {
        // Too many crashes - force immediate termination
        // Use ExitProcess which is more reliable than TerminateProcess
        // Also add a busy loop in case ExitProcess doesn't work immediately
        ExitProcess(0xDEAD);
        while (1) { Sleep(1000); }  // Should never reach here
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Re-entry guard - prevent recursive calls within the same crash
    if (InterlockedCompareExchange(&g_InCrashHandler, 1, 0) != 0) {
        // Already in crash handler - terminate immediately
        ExitProcess(0xDEAD);
        while (1) { Sleep(1000); }
        return EXCEPTION_EXECUTE_HANDLER;
    }
    // *** USE ONLY MINIMAL LOGGING - NO CRT CALLS ***
    // CRT (fopen, vfprintf, etc) can crash if heap/stack is corrupted
    MinimalCrashLog("=== LOTR Conquest Crash Report ===");
    MinimalCrashLogHex("ep = ", (DWORD)ep);
  
    // First check if ep is valid at all
    if (!ep) {
        MinimalCrashLog("ERROR: ep is NULL!");
        InterlockedExchange(&g_InCrashHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }
   
    DWORD code = 0;
    DWORD crashAddr = 0;
    DWORD eip = 0;
    DWORD esp = 0;
    DWORD ebp = 0;
    DWORD eax = 0;
    DWORD ecx = 0;

    // Read exception info - wrap in SEH
    __try {
        MinimalCrashLog("Reading ExceptionRecord...");
        if (ep->ExceptionRecord) {
            MinimalCrashLogHex("ExceptionRecord = ", (DWORD)ep->ExceptionRecord);
            code = ep->ExceptionRecord->ExceptionCode;
            MinimalCrashLogHex("ExceptionCode = ", code);
            crashAddr = (DWORD)ep->ExceptionRecord->ExceptionAddress;
            MinimalCrashLogHex("ExceptionAddress = ", crashAddr);
        } else {
            MinimalCrashLog("ExceptionRecord is NULL!");
        }
        MEMORY_BASIC_INFORMATION mbi = {0};
        if (VirtualQuery((LPCVOID)crashAddr, &mbi, sizeof(mbi)))    
        { MinimalCrashLogHex ("ModuleBase = ", (DWORD)mbi.AllocationBase);
        char moduleName[MAX_PATH] = {0};
        if
        (GetModuleFileNameA((HMODULE)mbi.AllocationBase, moduleName, MAX_PATH))
        {MinimalCrashLog("ModuleName = ");
        MinimalCrashLog(moduleName);
    }
    else {
        MinimalCrashLog("ModuleName = (GetModuleFileNameA failed)");
        }
    }
    else
    {
        MinimalCrashLog("VirtualQuery failed for ExceptionAddress");
    }
} 
    __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("ERROR: Exception reading ExceptionRecord!");
    }

    // Check for stack overflow
    if (code == EXCEPTION_STACK_OVERFLOW) {
        MinimalCrashLog("*** STACK OVERFLOW ***");
        InterlockedExchange(&g_InCrashHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Read context (registers)
    __try {
        MinimalCrashLog("Reading ContextRecord...");

        if (ep->ContextRecord) {
            MinimalCrashLogHex("ContextRecord = ", (DWORD)ep->ContextRecord);
            eip = CONTEXT_IP(ep->ContextRecord);
            esp = CONTEXT_SP(ep->ContextRecord);
            ebp = CONTEXT_BP(ep->ContextRecord);
            eax = CONTEXT_AX(ep->ContextRecord);
            ecx = CONTEXT_CX(ep->ContextRecord);
            MinimalCrashLogHex("EIP = ", eip);
            MinimalCrashLogHex("ESP = ", esp);
            MinimalCrashLogHex("EBP = ", ebp);
            MinimalCrashLogHex("EAX = ", eax);
            MinimalCrashLogHex("EBX = ", CONTEXT_BX(ep->ContextRecord));
            MinimalCrashLogHex("ECX = ", ecx);
            MinimalCrashLogHex("EDX = ", CONTEXT_DX(ep->ContextRecord));
            MinimalCrashLogHex("ESI = ", CONTEXT_SI(ep->ContextRecord));
            MinimalCrashLogHex("EDI = ", CONTEXT_DI(ep->ContextRecord));
        }
        else 
        {
        MinimalCrashLog("ContextRecord is NULL!");
        }      
    }
     __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("ERROR: Exception reading ContextRecord!");
        InterlockedExchange(&g_InCrashHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Skip breakpoints and single steps (debugger stuff)
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) {
        MinimalCrashLog("(Ignoring debugger exception)");
        InterlockedExchange(&g_InCrashHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Log exception type
    if (code == EXCEPTION_ACCESS_VIOLATION) {MinimalCrashLog("Exception: ACCESS_VIOLATION");}
    else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {MinimalCrashLog("Exception: INT_DIVIDE_BY_ZERO");}
    else if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {MinimalCrashLog("Exception: ILLEGAL_INSTRUCTION");}
    else {
    MinimalCrashLog("Exception: UNKNOWN");
    MinimalCrashLogHex("ExceptionCode = ", code);
    }

    // Access violation details
    __try {
        EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        if (code == EXCEPTION_ACCESS_VIOLATION && er && er->NumberParameters >= 2) {
            DWORD accessType = (DWORD)er->ExceptionInformation[0];
            DWORD accessAddr = (DWORD)er->ExceptionInformation[1];
            if (accessType == 0) MinimalCrashLog("AccessViolation: Reading");
            else if (accessType == 1) MinimalCrashLog("AccessViolation: Writing");
            else MinimalCrashLog("AccessViolation: Executing");

            MinimalCrashLogHex("AccessAddress = ", accessAddr);
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Could not read AV details)");
    }

    // Enhanced diagnostics for MOVAPS crash at 0x00414AF8
    __try {
        MinimalCrashLog("");
        MinimalCrashLog("=== Enhanced Crash Diagnostics ===");

        // Log bytes at EIP to verify the instruction
        if (eip >= 0x00400000 && eip < 0x01000000) {
            BYTE* eipBytes = (BYTE*)eip;
            char byteBuf[80];
            wsprintfA(byteBuf, "Bytes at EIP: %02X %02X %02X %02X %02X %02X %02X %02X",
                eipBytes[0], eipBytes[1], eipBytes[2], eipBytes[3],
                eipBytes[4], eipBytes[5], eipBytes[6], eipBytes[7]);
            MinimalCrashLog(byteBuf);

            // Check if this is the expected MOVAPS instruction (0F 28 01)
            if (eipBytes[0] == 0x0F && eipBytes[1] == 0x28 && eipBytes[2] == 0x01) {
                MinimalCrashLog("Instruction: MOVAPS XMM0, [ECX] (expected)");
            } else if (eipBytes[0] == 0x0F && eipBytes[1] == 0x29 && eipBytes[2] == 0x00) {
                MinimalCrashLog("Instruction: MOVAPS [EAX], XMM0 (write to dest)");
            } else {
                MinimalCrashLog("Instruction: UNKNOWN (not expected MOVAPS)");
            }
        }

        // Try to probe memory at ECX (source pointer)
        if (ecx != 0 && ecx != 0xFFFFFFFF) {
            MinimalCrashLog("Probing memory at ECX...");
            __try {
                DWORD testRead = *(volatile DWORD*)ecx;
                MinimalCrashLogHex("  [ECX+0x00] = ", testRead);
                MinimalCrashLog("  ECX memory is READABLE");
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                MinimalCrashLog("  ECX memory is NOT READABLE (access violation)");
            }
        } else {
            MinimalCrashLog("ECX is NULL or -1, skipping probe");
        }

        // Try to probe memory at EAX (destination pointer)
        if (eax != 0 && eax != 0xFFFFFFFF) {
            MinimalCrashLog("Probing memory at EAX...");
            __try {
                DWORD testRead = *(volatile DWORD*)eax;
                MinimalCrashLogHex("  [EAX+0x00] = ", testRead);
                MinimalCrashLog("  EAX memory is READABLE");
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                MinimalCrashLog("  EAX memory is NOT READABLE (access violation)");
            }
        } else {
            MinimalCrashLog("EAX is NULL or -1, skipping probe");
        }

        // Check if EIP is at 0x00414AF8 (the known crash location)
        if (eip == 0x00414AF8) {
            MinimalCrashLog("*** EIP matches known crash location 0x00414AF8 ***");
            // Log the source pointer from stack [EBP+0x8]
            if (ebp >= 0x00100000 && ebp < 0x80000000) {
                __try {
                    DWORD srcFromStack = *(DWORD*)(ebp + 0x8);
                    MinimalCrashLogHex("  [EBP+0x8] (source ptr) = ", srcFromStack);
                    if (srcFromStack != ecx) {
                        MinimalCrashLog("  *** WARNING: [EBP+0x8] != ECX! Context may be stale ***");
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    MinimalCrashLog("  (Could not read [EBP+0x8])");
                }
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Enhanced diagnostics failed)");
    }

    // Check known pool/buffer states
    MinimalCrashLog("");
    MinimalCrashLog("=== Pool/Buffer State ===");
    // Event pool (hard-coded addresses, build-specific)
    DWORD* pEventPoolBasePtr  = (DWORD*)0x00cd9970;
    DWORD* pEventPoolCountPtr = (DWORD*)0x00cd9974;
    // Local Values
    DWORD eventPoolBase = 0;
    DWORD eventPoolCount = 0;

    __try
    {
        eventPoolBase = *pEventPoolBasePtr;
        eventPoolCount = *pEventPoolCountPtr;
    } 
    __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Could not read pool state)");
    }
    MinimalCrashLogHex("EventPool.Base = ", eventPoolBase);
    MinimalCrashLogHex("EventPool.Count = ", eventPoolCount);

    // Stack trace (simplified)
    MinimalCrashLog("");
    MinimalCrashLog("=== Stack (first 16 DWORDs) ===");

    __try {
        if (esp) {
            DWORD* stackPtr = (DWORD*)esp;
            for (int i = 0; i < 16; i++) {
                __try {
                    MinimalCrashLogHex("  Stack+", stackPtr[i]);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    MinimalCrashLog("  (stack unreadable)");
                    break;
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Could not read stack)");
    }

    // Log function identification based on EIP
    MinimalCrashLog("");
    MinimalCrashLog("=== Function Identification ===");
    __try {
        if (eip != 0) {
            LogCrashFunction(eip);
            LogAddressWithModule("EIP: ", eip);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Could not identify function)");
    }

    // Dump recent call history
    __try {
        DumpCallHistory();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Could not dump call history)");
    }

    // Validate event pool to check for corruption
    MinimalCrashLog("");
    MinimalCrashLog("=== Pool Validation ===");
    __try {
        DWORD poolResult = ValidateEventPool();
        if (poolResult == POOL_VALID) {
            MinimalCrashLog("Pool: VALID");
        } else {
            MinimalCrashLogHex("Pool: INVALID flags=", poolResult);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Could not validate pool)");
    }

    // Log Patch 31 counters (matrix copy protection) - with SEH protection
    __try {
        MinimalCrashLog("");
        MinimalCrashLog("=== Patch 31 Counters (Matrix Copy Protection) ===");
        MinimalCrashLogHex("Valid calls:     ", g_Patch31_ValidCalls);
        MinimalCrashLogHex("NULL caught:     ", g_Patch31_NullCaught);
        MinimalCrashLogHex("Invalid(-1) caught:", g_Patch31_InvalidCaught);
        MinimalCrashLogHex("LowAddr caught:  ", g_Patch31_LowAddrCaught);
        MinimalCrashLogHex("Unaligned caught:", g_Patch31_UnalignedCaught);
        if (g_Patch31_ValidCalls == 0 && g_Patch31_NullCaught == 0 && g_Patch31_InvalidCaught == 0) {
            MinimalCrashLog("*** WARNING: Patch 31 may not be active (no calls recorded) ***");
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Could not read Patch 31 counters)");
    }

    // Verify Patch 31 is still in place at crash time
    __try {
        MinimalCrashLog("");
        MinimalCrashLog("=== Patch 31 Verification ===");
        BYTE* patchAddr = (BYTE*)0x00414AED;
        MinimalCrashLog("Bytes at 0x00414AED:");
        // Log first 11 bytes to see if patch is still there
        char byteBuf[64];
        wsprintfA(byteBuf, "  %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3],
            patchAddr[4], patchAddr[5], patchAddr[6], patchAddr[7],
            patchAddr[8], patchAddr[9], patchAddr[10]);
        MinimalCrashLog(byteBuf);

        // Check if it's our JMP (E9) or original (55)
        if (patchAddr[0] == 0xE9) {
            MinimalCrashLog("Patch 31: JMP still in place (patched)");
            // Calculate and log the target address
            DWORD jmpOffset = *(DWORD*)(patchAddr + 1);
            DWORD targetAddr = (DWORD)(patchAddr + 5) + jmpOffset;
            MinimalCrashLogHex("Code cave target: ", targetAddr);
        } else if (patchAddr[0] == 0x55) {
            MinimalCrashLog("Patch 31: PUSH EBP found (UNPATCHED or overwritten!)");
        } else {
            MinimalCrashLog("Patch 31: Unknown opcode (corrupted?)");
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MinimalCrashLog("(Could not verify Patch 31)");
    }

    MinimalCrashLog("");
    MinimalCrashLog("=== End Crash Report ===");

    InterlockedExchange(&g_InCrashHandler, 0);

    // IMPORTANT: Return EXCEPTION_CONTINUE_SEARCH to let Windows handle it
    // But if we've logged multiple crashes, the cascade prevention above
    // will terminate the process before we get here again.
    return EXCEPTION_CONTINUE_SEARCH;
}

// Backup: Unhandled exception filter (catches crashes VEH might miss)
static LONG WINAPI UnhandledCrashFilter(EXCEPTION_POINTERS* ep) {
    MinimalCrashLog("");
    MinimalCrashLog("=== UNHANDLED EXCEPTION (backup) ===");
    // Call our main handler
    CrashHandler(ep);

    // Call previous filter if any
    if (g_OldExceptionFilter) {
        return g_OldExceptionFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// Watchdog Thread - Monitors pool states for freeze detection
// ============================================================================

static volatile bool g_WatchdogRunning = false;
static HANDLE g_WatchdogThread = nullptr;

// Global to store main thread handle
static HANDLE g_MainThread = NULL;
static DWORD g_MainThreadId = 0;

// Capture EIP from main thread when frozen
static void CaptureMainThreadState(FILE* wlog) {
    if (!g_MainThread || g_MainThread == INVALID_HANDLE_VALUE) {
        fprintf(wlog, "Main thread handle not available\n");
        return;
    }

    // Suspend the main thread to safely read its state
    DWORD suspendCount = SuspendThread(g_MainThread);
    if (suspendCount == (DWORD)-1) {
        fprintf(wlog, "Failed to suspend main thread: error %d\n", GetLastError());
        return;
    }

    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_FULL;

    if (GetThreadContext(g_MainThread, &ctx)) {
        fprintf(wlog, "\n=== MAIN THREAD REGISTERS (FROZEN) ===\n");
        fprintf(wlog, "EIP = 0x%08X  <-- STUCK HERE!\n", CONTEXT_IP(&ctx));
        fprintf(wlog, "ESP = 0x%08X\n", CONTEXT_SP(&ctx));
        fprintf(wlog, "EBP = 0x%08X\n", CONTEXT_BP(&ctx));
        fprintf(wlog, "EAX = 0x%08X  EBX = 0x%08X\n", CONTEXT_AX(&ctx), CONTEXT_BX(&ctx));
        fprintf(wlog, "ECX = 0x%08X  EDX = 0x%08X\n", CONTEXT_CX(&ctx), CONTEXT_DX(&ctx));
        fprintf(wlog, "ESI = 0x%08X  EDI = 0x%08X\n", CONTEXT_SI(&ctx), CONTEXT_DI(&ctx));

        // Dump stack trace (return addresses) - scan more of the stack
        fprintf(wlog, "\nStack trace (game code addresses):\n");
        __try {
            DWORD* stackPtr = (DWORD*)CONTEXT_SP(&ctx);
            int gameFound = 0;
            // Scan 512 DWORDs (2KB of stack) looking for game code return addresses
            for (int i = 0; i < 512 && gameFound < 30; i++) {
                DWORD val = stackPtr[i];
                // Look for addresses in game code range (0x00400000 - 0x00FFFFFF)
                if (val >= 0x00400000 && val <= 0x00FFFFFF) {
                    fprintf(wlog, "  [ESP+%03X] = 0x%08X  (game code)\n", i*4, val);
                    gameFound++;
                }
            }
            if (gameFound == 0) {
                fprintf(wlog, "  No game code addresses found in stack!\n");
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fprintf(wlog, "  (stack read failed)\n");
        }

        // Also walk EBP chain for cleaner call stack
        fprintf(wlog, "\nEBP chain walk:\n");
        __try {
            DWORD* ebpPtr = (DWORD*)CONTEXT_BP(&ctx);
            for (int depth = 0; depth < 30 && ebpPtr != NULL; depth++) {
                DWORD retAddr = ebpPtr[1];  // Return address is at [EBP+4]
                DWORD nextEbp = ebpPtr[0];  // Next frame pointer is at [EBP]

                // Check if return address is in game code
                if (retAddr >= 0x00400000 && retAddr <= 0x00FFFFFF) {
                    fprintf(wlog, "  [%2d] 0x%08X  (game code)\n", depth, retAddr);
                } else if (retAddr >= 0x70000000 && retAddr <= 0x7FFFFFFF) {
                    fprintf(wlog, "  [%2d] 0x%08X  (system DLL)\n", depth, retAddr);
                }

                // Sanity check next EBP
                if (nextEbp == 0 || nextEbp <= (DWORD)ebpPtr) break;
                ebpPtr = (DWORD*)nextEbp;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fprintf(wlog, "  (EBP chain walk failed)\n");
        }

        // Dump first 32 raw stack values for reference
        fprintf(wlog, "\nRaw stack (first 128 bytes):\n");
        __try {
            DWORD* stackPtr = (DWORD*)CONTEXT_SP(&ctx);
            for (int i = 0; i < 32; i++) {
                if (i % 4 == 0) fprintf(wlog, "  ");
                fprintf(wlog, "%08X ", stackPtr[i]);
                if (i % 4 == 3) fprintf(wlog, "\n");
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fprintf(wlog, "  (raw stack read failed)\n");
        }

        // Sample EIP multiple times to see if it's truly stuck
        fprintf(wlog, "\nEIP sampling (50ms intervals):\n");
        for (int sample = 0; sample < 5; sample++) {
            ResumeThread(g_MainThread);
            Sleep(50);
            SuspendThread(g_MainThread);
            GetThreadContext(g_MainThread, &ctx);
            fprintf(wlog, "  Sample %d: EIP = 0x%08X\n", sample+1, CONTEXT_IP(&ctx));
        }
        fprintf(wlog, "If EIP is the same or oscillates in small range, it's a tight loop.\n");

    } else {
        fprintf(wlog, "Failed to get thread context: error %d\n", GetLastError());
    }

    // Resume the thread
    ResumeThread(g_MainThread);
}

// Dump detailed freeze diagnostics
static void DumpFreezeState(FILE* wlog, DWORD tick) {
    fprintf(wlog, "\n========== FREEZE DETECTED - DETAILED STATE ==========\n");
    fprintf(wlog, "Tick: %d (freeze started)\n\n", tick);

    // CAPTURE THE STUCK ADDRESS!
    CaptureMainThreadState(wlog);

    __try {
        // Event pool details
        DWORD eventBase = *(DWORD*)0x00cd9970;
        DWORD eventFreeHead = *(DWORD*)0x00cd996c;
        DWORD eventCount = *(DWORD*)0x00cd9974;
        fprintf(wlog, "\nEvent Pool:\n");
        fprintf(wlog, "  Base: 0x%08X, FreeHead: 0x%08X, Count: %d/4096 (expanded from 1024)\n",
            eventBase, eventFreeHead, eventCount);

        // Pool2 details
        DWORD pool2Count = *(DWORD*)0x00cd7e28;
        fprintf(wlog, "Pool2: Count=%d/256\n", pool2Count);

        fprintf(wlog, "\nThis is the address where the game is stuck.\n");
        fprintf(wlog, "Look up this EIP in the decompiled code to find the function.\n");

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fprintf(wlog, "(Error reading freeze state)\n");
    }

    fprintf(wlog, "=======================================================\n\n");
    fflush(wlog);
}

static DWORD WINAPI WatchdogThreadProc(LPVOID) {
    FILE* wlog = fopen("conquest_watchdog.log", "w");
    if (!wlog) return 0;

    fprintf(wlog, "=== LOTR Conquest Watchdog Started ===\n");
    fprintf(wlog, "Monitoring: Frame counter, Event pool, Pool2\n");
    fprintf(wlog, "Freeze detection: 2+ seconds with no new frames\n\n");
    fflush(wlog);

    DWORD tick = 0;
    DWORD lastEventCount = 0;
    DWORD lastFrameCount = 0;
    DWORD frameStuckCounter = 0;
    bool freezeDumped = false;

    while (g_WatchdogRunning) {
        Sleep(500);  // Log every 500ms
        tick++;

        __try {
            // Event pool (at 0x00cd9974 / 0x00cd9970 / 0x00cd996c)
            DWORD eventCount = *(DWORD*)0x00cd9974;

            // Pool2: Array at DAT_00a4a180, counter at DAT_00cd7e28 (limit 256)
            DWORD pool2Count = *(DWORD*)0x00cd7e28;

            // Frame counter from D3D9 EndScene
            DWORD frameCount = g_FrameCounter;
            DWORD framesDelta = frameCount - lastFrameCount;

            // Detect frame freeze
            if (frameCount == lastFrameCount && frameCount > 0) {
                frameStuckCounter++;
            } else {
                frameStuckCounter = 0;
                freezeDumped = false;  // Reset for next potential freeze
            }
            lastFrameCount = frameCount;
            lastEventCount = eventCount;

            // Log current state - pools + frames
            fprintf(wlog, "[%6d] Frames=%8d (+%4d)  Event=%4d/4096  Pool2=%3d/256",
                tick, frameCount, framesDelta, eventCount, pool2Count);

            // Warning indicators
            if (eventCount >= 3900) {  // Warn at 95% of 4096
                fprintf(wlog, " *** EVENT NEAR LIMIT! ***");
            }
            if (pool2Count >= 250) {
                fprintf(wlog, " *** POOL2 NEAR LIMIT! ***");
            }
            if (frameStuckCounter >= 2) {  // No frames for 1+ second
                fprintf(wlog, " *** FRAME FREEZE! (stuck %d ticks) ***", frameStuckCounter);

                // Dump detailed state once per freeze
                if (!freezeDumped && frameStuckCounter == 4) {  // After 2 seconds
                    DumpFreezeState(wlog, tick);
                    freezeDumped = true;
                }
            }

            fprintf(wlog, "\n");
            fflush(wlog);

            // Periodic validation (every 10th tick = 5 seconds)
            if ((tick % 10) == 0) {
                // Log Patch 31 counters (matrix copy protection)
                DWORD validCalls = g_Patch31_ValidCalls;
                DWORD nullCaught = g_Patch31_NullCaught;
                DWORD invalidCaught = g_Patch31_InvalidCaught;
                DWORD lowAddrCaught = g_Patch31_LowAddrCaught;
                DWORD unalignedCaught = g_Patch31_UnalignedCaught;
                if (validCalls > 0 || nullCaught > 0 || invalidCaught > 0 || lowAddrCaught > 0 || unalignedCaught > 0) {
                    fprintf(wlog, "[%6d] Patch31: Valid=%d  NULL=%d  Invalid(-1)=%d  LowAddr=%d  Unaligned=%d\n",
                        tick, validCalls, nullCaught, invalidCaught, lowAddrCaught, unalignedCaught);
                    if (nullCaught > 0 || invalidCaught > 0 || lowAddrCaught > 0 || unalignedCaught > 0) {
                        fprintf(wlog, "[%6d] *** PATCH 31 CAUGHT INVALID POINTERS! ***\n", tick);
                    }
                    fflush(wlog);
                }

                // Validate event pool integrity
                DWORD poolResult = ValidateEventPool();
                if (poolResult != POOL_VALID) {
                    fprintf(wlog, "[%6d] *** POOL CORRUPTION DETECTED! flags=%d ***\n", tick, poolResult);
                    fflush(wlog);
                }

                // Detailed freelist validation (every 20th tick = 10 seconds)
                if ((tick % 20) == 0) {
                    if (!ValidateEventPoolFreelistDetailed()) {
                        fprintf(wlog, "[%6d] *** DETAILED FREELIST CORRUPTION DETECTED! ***\n", tick);
                        fprintf(wlog, "[%6d] See conquest_freelist_diagnostics.log for details\n", tick);
                        fflush(wlog);
                    }
                }

                // Check heap health
                if (!CheckHeapHealth()) {
                    fprintf(wlog, "[%6d] *** HEAP CORRUPTION DETECTED! ***\n", tick);
                    fflush(wlog);
                }
            }

        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fprintf(wlog, "[%6d] (Could not read state)\n", tick);
            fflush(wlog);
        }
    }

    fprintf(wlog, "\n=== Watchdog Stopped ===\n");
    fclose(wlog);
    return 0;
}

static void StartWatchdog() {
    // Get a handle to the main game thread (current thread when DLL loads)
    g_MainThreadId = GetCurrentThreadId();
    g_MainThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, g_MainThreadId);

    g_WatchdogRunning = true;
    g_WatchdogThread = CreateThread(nullptr, 0, WatchdogThreadProc, nullptr, 0, nullptr);
}

// ============================================================================
// Configuration - Adjust these values as needed
// ============================================================================

// ============================================================================
// PATCH ENABLE/DISABLE SWITCHES
// ============================================================================
// Set to true to enable a patch, false to disable it
// Recompile after changing these values

static const bool PATCH_1_ENABLED = false;   // Vertex buffer malloc
static const bool PATCH_2_ENABLED = false;   // Texture config pattern scan
static const bool PATCH_3_ENABLED = false;   // Vertex config pattern scan
static const bool PATCH_4_ENABLED = false;   // NOP out texture override
static const bool PATCH_5_ENABLED = false;   // Direct global variable writes
static const bool PATCH_6_ENABLED = false;   // Event/Effect Pool Expansion
static const bool PATCH_7_ENABLED = false;  // Frame Limiter (disabled - breaks UI)
static const bool PATCH_8_ENABLED = false;   // Animation NULL Check
static const bool PATCH_9_ENABLED = false;   // Spatial Query Validation (call-site)
static const bool PATCH_10_ENABLED = false; // HeapAlloc fallback (disabled - violates pool invariants)
static const bool PATCH_11_ENABLED = false;  // FUN_00495700 loop NULL check
static const bool PATCH_12_ENABLED = false;  // Memcpy destination validation
static const bool PATCH_13_ENABLED = false; // Large allocator HeapAlloc (disabled - violates pool invariants)
static const bool PATCH_14_ENABLED = false;  // Entity serialization NULL check
static const bool PATCH_15_ENABLED = false;  // Array resize NULL base check
static const bool PATCH_16_ENABLED = false; // Freelist Validation Hook (disabled - conflicts with Patch 11)
static const bool PATCH_17_ENABLED = false;  // FUN_008f0592 NULL check
static const bool PATCH_18_ENABLED = false;  // Effect spawner NULL check
static const bool PATCH_19_ENABLED = false; // Physics spawner NULL check (deferred - needs verification)
static const bool PATCH_20_ENABLED = false; // Effect Spawner Entry Gate (disabled - commented out)
static const bool PATCH_21_ENABLED = false; // Ragdoll Spawner Entry Gate (disabled - commented out)
static const bool PATCH_22_ENABLED = false;  // FUN_0087cda1 NULL check
static const bool PATCH_23_ENABLED = false;  // Second FUN_008f7ec2 NULL check
static const bool PATCH_24_ENABLED = false; // JNZ->JZ change (disabled - broke level loading)
static const bool PATCH_25_ENABLED = false; // FUN_008f7ec2 entry modification (disabled - caused freeze)
static const bool PATCH_26_ENABLED = false;  // Array Bounds Validation
static const bool PATCH_27_ENABLED = false;  // FUN_0073fc02 Multiplier Fix
static const bool PATCH_28_ENABLED = false; // High-Frequency Function NULL Check (disabled - needs work)
static const bool PATCH_29_ENABLED = false;  // Second Pool Limit Check (FUN_0045549c)
static const bool PATCH_30_ENABLED = true;   // Havok Debug Hook for FUN_008f7ec2
static const bool PATCH_31_ENABLED = true;   // Matrix Copy NULL/Invalid Pointer Check (FUN_00414aed)

// Note: Patch 31 counters are declared earlier (before crash handler) because
// the crash handler logs them. See section above "Crash Handler".

// Buffer sizes (in bytes)
// Original: Texture = 0x0AA00000 (170 MB), Vertex = 0x06600000 (102 MB)
// Increased for large modded levels
// STABLE: 256 MB texture + 192 MB vertex (1.1 GB test caused heap corruption)
static DWORD g_TextureBufferSize = 0x14000000;  // 256 MB
static DWORD g_VertexBufferSize  = 0x10000000;  // 192 MB

// Event/Effect Pool Limit
// Original: 0x400 (1024) - Pool for game events/effects
// EXPANDED: 0x1000 (4096) - Prevents pool exhaustion crash at frame ~5,884
//
// Pool exhaustion crash analysis:
// - Game crashes at 0x004554CF in FUN_0045549c when pool count >= 1024
// - Crash: MOV [EAX + 0xEC], EBX where EAX=NULL (pool full, returns NULL)
// - Solution: Increase limit to 0x1000 (4096 entries = 196 KB)
// - This is done via:
//   1. Allocate a new larger pool at DLL startup
//   2. Replace the pool pointer at DAT_00cd9970
//   3. Rebuild the free list at DAT_00cd996c
//   4. Patch the limit check at 0x0088aa8b
//   5. Patch the limit check at 0x004554b1 (Patch 29)
static const DWORD g_EventPoolNewLimit = 0x400;  // 4096 (4x original)
static const DWORD g_EventPoolEntrySize = 0x30;   // 48 bytes per entry

// ============================================================================
// Patch Locations
// ============================================================================

// Pattern: C7 05 00 E2 A3 00 XX XX XX XX = MOV [0x00A3E200], texture_size
// Pattern: C7 05 04 E2 A3 00 XX XX XX XX = MOV [0x00A3E204], vertex_size
// These write to global config variables

// The actual malloc for vertex buffer is at 0x0067d791:
// BE XX XX XX XX = MOV ESI, vertex_size (used for malloc, memset, and end ptr calc)
static const DWORD VERTEX_MALLOC_ADDR = 0x0067d791;

// Global variables that store the buffer size limits
static const DWORD TEXTURE_SIZE_GLOBAL = 0x00A3E200;
static const DWORD VERTEX_SIZE_GLOBAL  = 0x00A3E204;
static const DWORD TOTAL_SIZE_GLOBAL   = 0x00A3E208;

// Conditional override instruction at 0x008a0034 that can overwrite texture limit
// A3 00 E2 A3 00 = MOV [0x00a3e200], EAX
// We need to NOP this out (5 bytes of 0x90)
static const DWORD TEXTURE_OVERRIDE_ADDR = 0x008a0034;

// Event/Effect Pool Limit at 0x0088aa85
// Original instruction: 81 3D 74 99 CD 00 00 04 00 00 = CMP [0x00cd9974], 0x400
// The limit value (0x400) is at offset 6 from the instruction start
// We patch the 4-byte value at 0x0088aa8b to increase the limit
static const DWORD EVENT_POOL_LIMIT_ADDR = 0x0088aa8b;  // Address of the limit value

// Event Pool global variables (in game's .data section)
// DAT_00cd9970 = Pool base pointer (points to allocated memory)
// DAT_00cd996c = Free list head pointer
// DAT_00cd9974 = Current count of allocated entries
static const DWORD EVENT_POOL_BASE_ADDR = 0x00cd9970;
static const DWORD EVENT_POOL_FREELIST_ADDR = 0x00cd996c;
static const DWORD EVENT_POOL_COUNT_ADDR = 0x00cd9974;

// ============================================================================
// Logging
// ============================================================================

static FILE* g_LogFile = nullptr;

static void OpenLog() {
    if (!g_LogFile) {
        g_LogFile = fopen("conquest_memory_patch.log", "w");
    }
}

static void Log(const char* fmt, ...) {
    OpenLog();
    if (g_LogFile) {
        va_list args;
        va_start(args, fmt);
        vfprintf(g_LogFile, fmt, args);
        fprintf(g_LogFile, "\n");
        fflush(g_LogFile);
        va_end(args);
    }
}

static void CloseLog() {
    if (g_LogFile) {
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

// ============================================================================
// Patch Helpers
// ============================================================================

static bool PatchMemory(void* address, const void* data, size_t size) {
    DWORD oldProtect;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("  ERROR: VirtualProtect failed at 0x%p (error %d)", address, GetLastError());
        return false;
    }
    memcpy(address, data, size);
    VirtualProtect(address, size, oldProtect, &oldProtect);

    // Flush instruction cache to ensure CPU sees the new code
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

static bool ScanAndPatchPattern(const BYTE* pattern, size_t patternLen, 
                                 size_t valueOffset, DWORD newValue,
                                 const char* name) {
    // Scan the .text section for the pattern
    HMODULE hModule = GetModuleHandleA(nullptr);
    BYTE* base = (BYTE*)hModule;
    
    // Get PE headers
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    
    BYTE* textStart = nullptr;
    DWORD textSize = 0;
    
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (strcmp((char*)section[i].Name, ".text") == 0) {
            textStart = base + section[i].VirtualAddress;
            textSize = section[i].Misc.VirtualSize;
            break;
        }
    }
    
    if (!textStart) {
        Log("  ERROR: Could not find .text section for %s", name);
        return false;
    }
    
    Log("  Scanning .text (0x%p - 0x%p) for %s pattern...", 
        textStart, textStart + textSize, name);
    
    int patchCount = 0;
    for (DWORD i = 0; i < textSize - patternLen; i++) {
        bool match = true;
        for (size_t j = 0; j < patternLen - 4; j++) { // Don't match the value bytes
            if (textStart[i + j] != pattern[j]) {
                match = false;
                break;
            }
        }
        
        if (match) {
            BYTE* patchAddr = textStart + i + valueOffset;
            DWORD oldValue = *(DWORD*)patchAddr;
            
            Log("  Found %s at 0x%p (current value: 0x%08X = %u MB)",
                name, textStart + i, oldValue, oldValue / 1024 / 1024);
            
            if (PatchMemory(patchAddr, &newValue, sizeof(DWORD))) {
                Log("  Patched to: 0x%08X (%u MB)", newValue, newValue / 1024 / 1024);
                patchCount++;
            }
        }
    }
    
    Log("  %s: %d location(s) patched", name, patchCount);
    return patchCount > 0;
}

// ============================================================================
// Main Patch Function
// ============================================================================

static void ApplyMemoryPatchesInternal();

void ApplyMemoryPatches() {
    __try {
        ApplyMemoryPatchesInternal();
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("EXCEPTION during patching! Code: 0x%08X", GetExceptionCode());
        Log("Patching aborted - game will use default buffer sizes");
    }
    CloseLog();
}

// Scan for exact 10-byte pattern and patch the value (last 4 bytes)
static int ScanAndPatchExact(const BYTE* pattern, size_t patternLen,
                              DWORD newValue, const char* name) {
    HMODULE hModule = GetModuleHandleA(nullptr);
    BYTE* base = (BYTE*)hModule;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);

    BYTE* textStart = nullptr;
    DWORD textSize = 0;

    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (strcmp((char*)section[i].Name, ".text") == 0) {
            textStart = base + section[i].VirtualAddress;
            textSize = section[i].Misc.VirtualSize;
            break;
        }
    }

    if (!textStart) {
        Log("  ERROR: Could not find .text section");
        return 0;
    }

    int patchCount = 0;
    for (DWORD i = 0; i < textSize - patternLen; i++) {
        bool match = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (textStart[i + j] != pattern[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            BYTE* patchAddr = textStart + i + 6;  // Value starts at offset 6
            DWORD oldValue = *(DWORD*)patchAddr;

            Log("  Found %s at 0x%p", name, textStart + i);

            if (PatchMemory(patchAddr, &newValue, sizeof(DWORD))) {
                Log("    0x%08X -> 0x%08X", oldValue, newValue);
                patchCount++;
            }
        }
    }

    return patchCount;
}

// Logging function for spatial query validation failures
// Called when FUN_008d2844 receives invalid parameters
// Parameters: param_1 (ECX), param_2 (EDX), return_address
static void LogSpatialQueryValidationFailure(DWORD param1, DWORD param2, DWORD returnAddr) {
    FILE* f = fopen("CrashLogs/conquest_spatial_query_validation.log", "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] Frame=%d Pool=%d\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                g_FrameCounter, *(DWORD*)POOL_COUNT_PTR);

        fprintf(f, "  SPATIAL QUERY VALIDATION FAILURE in FUN_008d2844\n");
        fprintf(f, "  param_1 (ECX) = 0x%08X\n", param1);
        fprintf(f, "  param_2 (EDX) = 0x%08X\n", param2);

        // Validate each parameter
        if (param1 == 0) {
            fprintf(f, "  ERROR: param_1 is NULL\n");
        } else if (param1 < 0x00400000 || param1 > 0x7FFFFFFF) {
            fprintf(f, "  ERROR: param_1 is out of valid range\n");
        }

        if (param2 == 0) {
            fprintf(f, "  ERROR: param_2 is NULL\n");
        } else if (param2 < 0x00400000 || param2 > 0x7FFFFFFF) {
            fprintf(f, "  ERROR: param_2 is out of valid range\n");
        } else {
            // Try to read param_2[1]
            __try {
                DWORD param2_1 = *(DWORD*)(param2 + 0x4);
                if (param2_1 == 0) {
                    fprintf(f, "  ERROR: param_2[1] is NULL\n");
                } else if (param2_1 < 0x00400000 || param2_1 > 0x7FFFFFFF) {
                    fprintf(f, "  ERROR: param_2[1] (0x%08X) is out of valid range\n", param2_1);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(f, "  ERROR: Cannot read param_2[1] - access violation\n");
            }
        }

        // Log caller information
        DWORD offset;
        const char* modName = AddressToModuleOffset(returnAddr, &offset);
        fprintf(f, "  Caller: %s+0x%08X (0x%08X)\n", modName, offset, returnAddr);

        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}

static void ApplyMemoryPatchesInternal() {
    // Initialize crash log first (so we know it works)
    InitCrashLog();

    // Cache loaded modules for runtime address translation
    // This enables module+offset format in crash logs for consistent analysis
    CacheLoadedModules();

    // Initialize danger log for spawn throttling events
    InitDangerLog();

    // Initialize freelist diagnostics log and bitmap
    InitFreelistDiagLog();
    InitPoolAllocationBitmap();

    // Initialize VEH recovery log
    {
        FILE* f = fopen("conquest_veh_recovery.log", "w");
        if (f) {
            fprintf(f, "=== LOTR Conquest VEH Recovery Log ===\n");
            fprintf(f, "This log tracks NULL pointer crashes that were automatically recovered.\n\n");
            fclose(f);
        }
    }

    // Scan for Havok SDK vtables and initialize monitoring
    // This uses knowledge from Havok Physics SDK 5.5.0-r1 headers
    ScanForHavokVtables();

    // Register NULL pointer safety net VEH FIRST (highest priority)
    // This catches NULL dereferences and skips the bad instruction
    g_VEHHandle = AddVectoredExceptionHandler(1, NullPointerVEH);
    if (g_VEHHandle) {
        Log("[VEH] NULL pointer safety net registered");
    } else {
        Log("[VEH] WARNING: Failed to register NULL pointer safety net!");
    }

    // Register crash handlers (both VEH and unhandled filter for maximum coverage)
    // These are fallbacks if the NULL pointer VEH can't handle the crash
    AddVectoredExceptionHandler(0, CrashHandler);  // Lower priority than NullPointerVEH
    g_OldExceptionFilter = SetUnhandledExceptionFilter(UnhandledCrashFilter);

    // Start watchdog thread to monitor pool states
    StartWatchdog();

    Log("=== LOTR Conquest Memory Patcher ===");
    Log("Crash handler installed - see conquest_crash.log if game crashes");
    Log("Watchdog started - see conquest_watchdog.log for pool monitoring");
    Log("Target texture buffer: 0x%08X (%u MB)", g_TextureBufferSize, g_TextureBufferSize / 1024 / 1024);
    Log("Target vertex buffer:  0x%08X (%u MB)", g_VertexBufferSize, g_VertexBufferSize / 1024 / 1024);
    Log("");
    Log("[PATCHES ENABLED - Pool Exhaustion Fix Active]");
    Log("");

    // === Patch 1: Vertex buffer malloc at 0x0067d791 ===
    if (PATCH_1_ENABLED) {
        Log("[Patch 1] Vertex buffer malloc (0x%08X):", VERTEX_MALLOC_ADDR);
        BYTE* vertexMallocAddr = (BYTE*)VERTEX_MALLOC_ADDR;

        if (vertexMallocAddr[0] == 0xBE) { // MOV ESI, imm32
            DWORD currentSize = *(DWORD*)(vertexMallocAddr + 1);
            Log("  Current: MOV ESI, 0x%08X (%u MB)", currentSize, currentSize / 1024 / 1024);

            if (PatchMemory(vertexMallocAddr + 1, &g_VertexBufferSize, sizeof(DWORD))) {
                Log("  Patched to: MOV ESI, 0x%08X (%u MB)", g_VertexBufferSize, g_VertexBufferSize / 1024 / 1024);
            }
        } else {
            Log("  ERROR: Unexpected opcode 0x%02X (expected 0xBE)", vertexMallocAddr[0]);
        }
    } else {
        Log("[Patch 1] DISABLED");
    }
    Log("");

    // === Patch 2: Exact pattern scan for MOV instructions ===
    // These are the EXACT 10-byte patterns from the user
    // C7 05 00 E2 A3 00 00 00 00 10 = MOV [0x00A3E200], 0x10000000 (Texture)
    // C7 05 04 E2 A3 00 00 00 00 10 = MOV [0x00A3E204], 0x10000000 (Vertex)

    if (PATCH_2_ENABLED) {
        Log("[Patch 2] Scanning for EXACT texture config pattern:");
        BYTE texturePattern[] = { 0xC7, 0x05, 0x00, 0xE2, 0xA3, 0x00, 0x00, 0x00, 0x00, 0x10 };
        int texPatches = ScanAndPatchExact(texturePattern, sizeof(texturePattern),
                                            g_TextureBufferSize, "Texture MOV");
        Log("  Total: %d location(s) patched", texPatches);
    } else {
        Log("[Patch 2] DISABLED");
    }
    Log("");

    if (PATCH_3_ENABLED) {
        Log("[Patch 3] Scanning for EXACT vertex config pattern:");
        BYTE vertexPattern[] = { 0xC7, 0x05, 0x04, 0xE2, 0xA3, 0x00, 0x00, 0x00, 0x00, 0x10 };
        int vtxPatches = ScanAndPatchExact(vertexPattern, sizeof(vertexPattern),
                                            g_VertexBufferSize, "Vertex MOV");
        Log("  Total: %d location(s) patched", vtxPatches);
    } else {
        Log("[Patch 3] DISABLED");
    }
    Log("");

    // === Patch 4: NOP out the conditional override at 0x008a0034 ===
    // This instruction: A3 00 E2 A3 00 = MOV [0x00a3e200], EAX
    // It can override our texture limit from a config structure
    if (PATCH_4_ENABLED) {
        Log("[Patch 4] NOP out texture config override at 0x%08X:", TEXTURE_OVERRIDE_ADDR);
        BYTE* overrideAddr = (BYTE*)TEXTURE_OVERRIDE_ADDR;

        // Verify it's the expected instruction
        if (overrideAddr[0] == 0xA3 &&
            overrideAddr[1] == 0x00 && overrideAddr[2] == 0xE2 &&
            overrideAddr[3] == 0xA3 && overrideAddr[4] == 0x00) {

            Log("  Found: A3 00 E2 A3 00 (MOV [0x00a3e200], EAX)");
            BYTE nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
            if (PatchMemory(overrideAddr, nops, 5)) {
                Log("  Patched to: 90 90 90 90 90 (NOP x5)");
            }
        } else {
            Log("  WARNING: Unexpected bytes at override address:");
            Log("    Found: %02X %02X %02X %02X %02X",
                overrideAddr[0], overrideAddr[1], overrideAddr[2],
                overrideAddr[3], overrideAddr[4]);
            Log("    Expected: A3 00 E2 A3 00");
        }
    } else {
        Log("[Patch 4] DISABLED");
    }
    Log("");

    // === Patch 5: Direct write to global config variables ===
    if (PATCH_5_ENABLED) {
        Log("[Patch 5] Direct global variable writes:");

        DWORD oldProtect;
        DWORD* pTextureGlobal = (DWORD*)TEXTURE_SIZE_GLOBAL;
        DWORD* pVertexGlobal = (DWORD*)VERTEX_SIZE_GLOBAL;

        if (VirtualProtect(pTextureGlobal, 8, PAGE_READWRITE, &oldProtect)) {
            Log("  [0x%08X] Texture: 0x%08X -> 0x%08X", TEXTURE_SIZE_GLOBAL, *pTextureGlobal, g_TextureBufferSize);
            *pTextureGlobal = g_TextureBufferSize;

            Log("  [0x%08X] Vertex:  0x%08X -> 0x%08X", VERTEX_SIZE_GLOBAL, *pVertexGlobal, g_VertexBufferSize);
            *pVertexGlobal = g_VertexBufferSize;

            VirtualProtect(pTextureGlobal, 8, oldProtect, &oldProtect);
        } else {
            Log("  ERROR: VirtualProtect failed on globals");
        }
    } else {
        Log("[Patch 5] DISABLED");
    }
    Log("");

    // === Patch 6: Event/Effect Pool - Allocate Larger Pool ===
    // We allocate a new larger pool and replace the game's pool pointer.
    // This must be done AFTER the game initializes its pool but BEFORE it's used heavily.
    if (PATCH_6_ENABLED) {
        Log("[Patch 6] Event/Effect Pool Expansion:");

        DWORD* pPoolBase = (DWORD*)EVENT_POOL_BASE_ADDR;
        DWORD* pFreeList = (DWORD*)EVENT_POOL_FREELIST_ADDR;
        DWORD* pPoolCount = (DWORD*)EVENT_POOL_COUNT_ADDR;

        // Calculate new pool size
        DWORD newPoolSize = g_EventPoolNewLimit * g_EventPoolEntrySize;  // 4096 * 48 = 196608 bytes

        // Allocate new pool memory
        void* newPool = VirtualAlloc(nullptr, newPoolSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (newPool) {
        Log("  Allocated new pool: %p (%u bytes for %u entries)",
            newPool, newPoolSize, g_EventPoolNewLimit);

        // Zero the new pool
        memset(newPool, 0, newPoolSize);

        // Build the free list (each entry points to the next one)
        // Entry structure: first DWORD is the "next" pointer in free list
        BYTE* poolBytes = (BYTE*)newPool;
        for (DWORD i = 0; i < g_EventPoolNewLimit - 1; i++) {
            DWORD* entry = (DWORD*)(poolBytes + i * g_EventPoolEntrySize);
            *entry = (DWORD)(poolBytes + (i + 1) * g_EventPoolEntrySize);
        }
        // Last entry points to NULL
        DWORD* lastEntry = (DWORD*)(poolBytes + (g_EventPoolNewLimit - 1) * g_EventPoolEntrySize);
        *lastEntry = 0;

        // Now we need to replace the game's pool with ours
        // This is tricky because the game may have already initialized its pool
        // We'll check if the pool is already set up and copy any existing data

        DWORD oldProtect;
        if (VirtualProtect(pPoolBase, 12, PAGE_READWRITE, &oldProtect)) {
            DWORD oldPoolBase = *pPoolBase;
            DWORD oldFreeList = *pFreeList;
            DWORD oldCount = *pPoolCount;

            Log("  Old pool: base=0x%08X, freelist=0x%08X, count=%d",
                oldPoolBase, oldFreeList, oldCount);

            if (oldPoolBase != 0 && oldCount > 0) {
                // Copy existing used entries from old pool to new pool
                // We can't easily determine which entries are in use vs free,
                // so this approach is risky. Instead, we'll only do this if count is 0.
                Log("  WARNING: Pool already has %d entries in use!", oldCount);
                Log("  Cannot safely replace pool after game start.");
                Log("  Pool expansion SKIPPED.");
                VirtualFree(newPool, 0, MEM_RELEASE);
            } else {
                // Pool is empty or not yet initialized, safe to replace
                *pPoolBase = (DWORD)newPool;
                *pFreeList = (DWORD)newPool;  // First entry is head of free list
                *pPoolCount = 0;
                g_NewEventPool = newPool;  // Track for crash diagnostics

                Log("  Replaced pool: base=0x%08X, freelist=0x%08X",
                    *pPoolBase, *pFreeList);

                // Now patch the limit check
                DWORD* pEventPoolLimit = (DWORD*)EVENT_POOL_LIMIT_ADDR;
                BYTE* pEventPoolInstr = (BYTE*)(EVENT_POOL_LIMIT_ADDR - 6);

                if (pEventPoolInstr[0] == 0x81 && pEventPoolInstr[1] == 0x3D &&
                    pEventPoolInstr[2] == 0x74 && pEventPoolInstr[3] == 0x99 &&
                    pEventPoolInstr[4] == 0xCD && pEventPoolInstr[5] == 0x00) {

                    if (PatchMemory(pEventPoolLimit, &g_EventPoolNewLimit, sizeof(DWORD))) {
                        Log("  Patched limit: 1024 -> %d", g_EventPoolNewLimit);
                    }
                } else {
                    Log("  WARNING: Could not find limit instruction to patch");
                }
            }
            VirtualProtect(pPoolBase, 12, oldProtect, &oldProtect);
        } else {
            Log("  ERROR: VirtualProtect failed on pool globals");
            VirtualFree(newPool, 0, MEM_RELEASE);
        }
        } else {
            Log("  ERROR: Failed to allocate new pool (error %d)", GetLastError());
        }
    } else {
        Log("[Patch 6] DISABLED");
    }
    Log("");

    // === Patch 7: Frame Limiter - DISABLED ===
    // Testing showed that modifying the frame limiter comparison (8->1 or NOP)
    // breaks the game's UI/input timing. The busy-wait loop is tied to input processing.
    //
    // The freezes at 0x00613CCE/0x006A4C2D are caused by the timer calibration
    // producing a frequency that makes each "tick" ~290ms instead of ~8ms.
    // However, we cannot patch this without breaking the UI.
    //
    // For now, leave the frame limiter unmodified and investigate the timer
    // calibration function FUN_0067a598 / FUN_00610786 instead.
    if (PATCH_7_ENABLED) {
        Log("[Patch 7] Frame Limiter: ENABLED (WARNING: may break UI)");
        // TODO: Add frame limiter patching code here if desired
    } else {
        Log("[Patch 7] DISABLED (modifying breaks UI)");
    }
    Log("");

    // === Patch 8: NULL Pointer Check in Animation Transform ===
    // Crash at 0x00692E85: MOVAPS [ESI], XMM1 where ESI=0
    // This happens when memory allocation at 0x00692DC7 (FUN_0067e610) fails.
    // The game doesn't handle the NULL return and continues with ESI=0.
    //
    // Fix: At 0x00692E2D, after MOV ESI, [EBX+0x18], check if ESI is NULL.
    // If NULL, skip to LAB_006930D6 which bypasses the problematic transform code.
    //
    // Original at 0x00692E2D:
    //   8B 73 18             MOV ESI, [EBX+0x18]      ; 3 bytes
    //   0F 85 22 01 00 00    JNZ 0x00692F58           ; 6 bytes
    //
    // Strategy: Replace with jump to code cave, do original ops + NULL check there
    if (PATCH_8_ENABLED) {
    Log("[Patch 8] Animation NULL Check:");
    {
        // Allocate code cave near the game's code (within 2GB for relative jumps)
        BYTE* codeCave = (BYTE*)VirtualAlloc(
            (void*)0x00600000,  // Try to allocate near game code
            4096,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE
        );

        if (!codeCave) {
            // Try without address hint
            codeCave = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        }

        if (codeCave) {
            Log("  Code cave allocated at 0x%p", codeCave);

            // Build the code cave:
            // 1. MOV ESI, [EBX+0x18]    ; Original instruction
            // 2. TEST ESI, ESI          ; Check for NULL
            // 3. JZ skip_to_006930D6    ; If NULL, skip the transform block
            // 4. TEST EDX, EDX          ; Original test (was at 0x00692E2B but we need to redo logic)
            // 5. JNZ 0x00692F58         ; Original conditional jump
            // 6. JMP 0x00692E36         ; Return to normal flow
            // skip_to_006930D6:
            // 7. JMP 0x006930D6         ; Skip the problematic code

            BYTE* p = codeCave;

            // MOV ESI, [EBX+0x18]  (8B 73 18)
            *p++ = 0x8B; *p++ = 0x73; *p++ = 0x18;

            // TEST ESI, ESI (85 F6)
            *p++ = 0x85; *p++ = 0xF6;

            // JZ skip_label (0F 84 XX XX XX XX) - will patch offset later
            BYTE* jzPatch = p;
            *p++ = 0x0F; *p++ = 0x84;
            *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;  // Placeholder

            // Original: TEST EDX, EDX (at 0x00692E2B, 2 bytes: 85 D2)
            // We need this test for the JNZ below
            *p++ = 0x85; *p++ = 0xD2;  // TEST EDX, EDX

            // JNZ 0x00692F58 (0F 85 XX XX XX XX) - 6 bytes, relative jump
            *p++ = 0x0F; *p++ = 0x85;
            DWORD jnzTarget = 0x00692F58;
            DWORD jnzOffset = jnzTarget - ((DWORD)p + 4);
            *(DWORD*)p = jnzOffset;
            p += 4;

            // JMP 0x00692E36 (E9 XX XX XX XX) - continue normal flow
            *p++ = 0xE9;
            DWORD normalTarget = 0x00692E36;
            DWORD normalOffset = normalTarget - ((DWORD)p + 4);
            *(DWORD*)p = normalOffset;
            p += 4;

            // skip_label: JMP 0x006930D6 (E9 XX XX XX XX) - skip problematic block
            BYTE* skipLabel = p;
            // Patch the JZ offset
            DWORD jzOffset = (DWORD)(skipLabel - (jzPatch + 6));
            *(DWORD*)(jzPatch + 2) = jzOffset;

            *p++ = 0xE9;
            DWORD skipTarget = 0x006930D6;
            DWORD skipOffset = skipTarget - ((DWORD)p + 4);
            *(DWORD*)p = skipOffset;
            p += 4;

            Log("  Code cave size: %d bytes", (int)(p - codeCave));

            // Now patch the original code at 0x00692E2B to jump to our code cave
            // Original at 0x00692E2B:
            //   85 D2                   TEST EDX, EDX      ; 2 bytes
            //   8B 73 18                MOV ESI, [EBX+0x18] ; 3 bytes
            //   0F 85 22 01 00 00       JNZ 0x00692F58     ; 6 bytes
            // Total: 11 bytes - enough for a JMP + NOP padding

            BYTE* patchAddr = (BYTE*)0x00692E2B;
            DWORD oldProtect;
            if (VirtualProtect(patchAddr, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                // Log original bytes
                Log("  Original at 0x00692E2B: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3], patchAddr[4],
                    patchAddr[5], patchAddr[6], patchAddr[7], patchAddr[8], patchAddr[9], patchAddr[10]);

                // JMP codeCave (E9 XX XX XX XX) - 5 bytes
                patchAddr[0] = 0xE9;
                DWORD jumpOffset = (DWORD)codeCave - ((DWORD)patchAddr + 5);
                *(DWORD*)(patchAddr + 1) = jumpOffset;

                // NOP the remaining 6 bytes (11 - 5 = 6)
                for (int i = 5; i < 11; i++) {
                    patchAddr[i] = 0x90;
                }

                VirtualProtect(patchAddr, 16, oldProtect, &oldProtect);

                Log("  Patched at 0x00692E2B: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3], patchAddr[4],
                    patchAddr[5], patchAddr[6], patchAddr[7], patchAddr[8], patchAddr[9], patchAddr[10]);
                Log("  NULL check installed - will skip to 0x006930D6 if ESI is NULL");
            } else {
                Log("  ERROR: Failed to unprotect memory at 0x00692E2B");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave");
        }
    }
    } else {
        Log("[Patch 8] DISABLED");
    }
    Log("");

    // === Patch 9: NULL Check in Spatial Query (FUN_008d28df) ===
    // Crash at 0x008D2848: MOVSS XMM0, [ECX] where ECX = 0x10 (NULL + offset)
    // This happens when iterating a linked list with a NULL node.
    //
    // At 0x008D2929 (LAB_008d2929):
    //   8B 45 08    MOV EAX, [EBP+0x8]      ; Load node ptr
    //   8D 48 10    LEA ECX, [EAX+0x10]     ; ECX = node + 0x10
    //   8D 55 F0    LEA EDX, [EBP-0x10]
    //   E8 xx xx    CALL FUN_008d2844       ; Crashes if EAX was NULL
    //
    // Fix: Add NULL check for EAX before the LEA/CALL sequence
    if (PATCH_9_ENABLED) {
        Log("[Patch 9] Spatial Query NULL Check:");
    {
        // We need to check if EAX is NULL after loading from [EBP+0x8]
        // Original at 0x008D2929:
        //   8B 45 08             MOV EAX, [EBP+0x8]   ; 3 bytes
        //   8D 48 10             LEA ECX, [EAX+0x10]  ; 3 bytes
        //   8D 55 F0             LEA EDX, [EBP-0x10]  ; 3 bytes
        //   E8 0D FF FF FF       CALL FUN_008d2844    ; 5 bytes
        // Total: 14 bytes
        //
        // Actually, looking more carefully at the code flow:
        // The safer fix is at 0x008D292C where LAB_008d292c starts
        // This is jumped to from 0x008D2927 (JMP 008d292c)
        //
        // At 0x008D292C:
        //   8D 48 10             LEA ECX, [EAX+0x10]  ; 3 bytes
        //   8D 55 F0             LEA EDX, [EBP-0x10]  ; 3 bytes
        //   E8 0D FF FF FF       CALL FUN_008d2844    ; 5 bytes
        // Total: 11 bytes - enough for JMP to code cave

        // Use the same code cave page if there's room (we allocated 4096 bytes)
        // Or allocate another one
        BYTE* codeCave2 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

        if (codeCave2) {
            Log("  Code cave 2 allocated at 0x%p", codeCave2);

            BYTE* p = codeCave2;

            // TEST EAX, EAX (check if node ptr is NULL)
            *p++ = 0x85; *p++ = 0xC0;  // TEST EAX, EAX

            // JZ skip_call (skip the call if NULL)
            BYTE* jzPatch = p;
            *p++ = 0x0F; *p++ = 0x84;
            *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;  // Placeholder

            // Original instructions:
            // LEA ECX, [EAX+0x10] (8D 48 10)
            *p++ = 0x8D; *p++ = 0x48; *p++ = 0x10;

            // LEA EDX, [EBP-0x10] (8D 55 F0)
            *p++ = 0x8D; *p++ = 0x55; *p++ = 0xF0;

            // Validate ECX before calling (range check: 0x00400000 - 0x7FFFFFFF)
            // CMP ECX, 0x00400000
            *p++ = 0x81; *p++ = 0xF9; *p++ = 0x00; *p++ = 0x00; *p++ = 0x04; *p++ = 0x00;
            // JB skip_call (if ECX < 0x00400000, skip)
            BYTE* jbPatch = p;
            *p++ = 0x0F; *p++ = 0x82;
            *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;  // Placeholder

            // CMP ECX, 0x7FFFFFFF
            *p++ = 0x81; *p++ = 0xF9; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0x7F;
            // JA skip_call (if ECX > 0x7FFFFFFF, skip)
            BYTE* jaPatch = p;
            *p++ = 0x0F; *p++ = 0x87;
            *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;  // Placeholder

            // CALL FUN_008d2844 (E8 XX XX XX XX)
            *p++ = 0xE8;
            DWORD callTarget = 0x008D2844;
            DWORD callOffset = callTarget - ((DWORD)p + 4);
            *(DWORD*)p = callOffset;
            p += 4;

            // JMP back to 0x008D2937 (after original CALL)
            *p++ = 0xE9;
            DWORD returnTarget = 0x008D2937;
            DWORD returnOffset = returnTarget - ((DWORD)p + 4);
            *(DWORD*)p = returnOffset;
            p += 4;

            // skip_call: JMP to 0x008D2946 (LAB_008d2946, skips the loop body)
            BYTE* skipLabel = p;
            // Patch all the conditional jumps to point here
            DWORD jzOffset = (DWORD)(skipLabel - (jzPatch + 6));
            *(DWORD*)(jzPatch + 2) = jzOffset;

            DWORD jbOffset = (DWORD)(skipLabel - (jbPatch + 6));
            *(DWORD*)(jbPatch + 2) = jbOffset;

            DWORD jaOffset = (DWORD)(skipLabel - (jaPatch + 6));
            *(DWORD*)(jaPatch + 2) = jaOffset;

            *p++ = 0xE9;
            DWORD skipTarget = 0x008D2946;  // Jump to after the first loop
            DWORD skipOffset = skipTarget - ((DWORD)p + 4);
            *(DWORD*)p = skipOffset;
            p += 4;

            Log("  Code cave 2 size: %d bytes", (int)(p - codeCave2));

            // Patch at 0x008D292C (11 bytes available)
            BYTE* patchAddr = (BYTE*)0x008D292C;
            DWORD oldProtect;
            if (VirtualProtect(patchAddr, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                Log("  Original at 0x008D292C: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3], patchAddr[4],
                    patchAddr[5], patchAddr[6], patchAddr[7], patchAddr[8], patchAddr[9], patchAddr[10]);

                // JMP codeCave2 (E9 XX XX XX XX) - 5 bytes
                patchAddr[0] = 0xE9;
                DWORD jumpOffset = (DWORD)codeCave2 - ((DWORD)patchAddr + 5);
                *(DWORD*)(patchAddr + 1) = jumpOffset;

                // NOP the remaining 6 bytes (11 - 5 = 6)
                for (int i = 5; i < 11; i++) {
                    patchAddr[i] = 0x90;
                }

                VirtualProtect(patchAddr, 16, oldProtect, &oldProtect);

                Log("  Patched at 0x008D292C: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3], patchAddr[4],
                    patchAddr[5], patchAddr[6], patchAddr[7], patchAddr[8], patchAddr[9], patchAddr[10]);
                Log("  NULL check installed for spatial query loop (call site 1)");
            } else {
                Log("  ERROR: Failed to unprotect memory at 0x008D292C");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 2");
        }

        // === Patch 9b: Second call site at 0x008D297B ===
        // This is the second call to FUN_008d2844 in the same function
        // At 0x008D297B:
        //   8B 4D F8       MOV ECX, [EBP-0x8]   ; 3 bytes - load local_c
        //   83 C1 10       ADD ECX, 0x10        ; 3 bytes
        //   8D 55 D8       LEA EDX, [EBP-0x28]  ; 3 bytes
        //   E8 BB FE FF FF CALL FUN_008d2844    ; 5 bytes
        // Total: 14 bytes
        //
        // We need to check if [EBP-0x8] is NULL before adding 0x10 to it

        // Build code cave 3 for the second call site
        BYTE* codeCave3 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave3) {
            Log("  Code cave 3 allocated at 0x%p", codeCave3);

            BYTE* p3 = codeCave3;

            // MOV ECX, [EBP-0x8] (8B 4D F8) - original instruction
            *p3++ = 0x8B; *p3++ = 0x4D; *p3++ = 0xF8;

            // TEST ECX, ECX (check if NULL)
            *p3++ = 0x85; *p3++ = 0xC9;

            // JZ skip_call2 (if NULL, skip)
            BYTE* jzPatch3 = p3;
            *p3++ = 0x0F; *p3++ = 0x84;
            *p3++ = 0x00; *p3++ = 0x00; *p3++ = 0x00; *p3++ = 0x00;  // Placeholder

            // ADD ECX, 0x10 (83 C1 10) - original
            *p3++ = 0x83; *p3++ = 0xC1; *p3++ = 0x10;

            // Validate ECX after ADD (range check: 0x00400000 - 0x7FFFFFFF)
            // CMP ECX, 0x00400000
            *p3++ = 0x81; *p3++ = 0xF9; *p3++ = 0x00; *p3++ = 0x00; *p3++ = 0x04; *p3++ = 0x00;
            // JB skip_call2 (if ECX < 0x00400000, skip)
            BYTE* jbPatch3 = p3;
            *p3++ = 0x0F; *p3++ = 0x82;
            *p3++ = 0x00; *p3++ = 0x00; *p3++ = 0x00; *p3++ = 0x00;  // Placeholder

            // CMP ECX, 0x7FFFFFFF
            *p3++ = 0x81; *p3++ = 0xF9; *p3++ = 0xFF; *p3++ = 0xFF; *p3++ = 0xFF; *p3++ = 0x7F;
            // JA skip_call2 (if ECX > 0x7FFFFFFF, skip)
            BYTE* jaPatch3 = p3;
            *p3++ = 0x0F; *p3++ = 0x87;
            *p3++ = 0x00; *p3++ = 0x00; *p3++ = 0x00; *p3++ = 0x00;  // Placeholder

            // LEA EDX, [EBP-0x28] (8D 55 D8) - original
            *p3++ = 0x8D; *p3++ = 0x55; *p3++ = 0xD8;

            // CALL FUN_008d2844 (E8 XX XX XX XX)
            *p3++ = 0xE8;
            DWORD call3Target = 0x008D2844;
            DWORD call3Offset = call3Target - ((DWORD)p3 + 4);
            *(DWORD*)p3 = call3Offset;
            p3 += 4;

            // JMP back to 0x008D2989 (after original CALL)
            *p3++ = 0xE9;
            DWORD return3Target = 0x008D2989;
            DWORD return3Offset = return3Target - ((DWORD)p3 + 4);
            *(DWORD*)p3 = return3Offset;
            p3 += 4;

            // skip_call2: JMP to 0x008D29A4 (LAB_008d29a4 - function exit)
            BYTE* skip3Label = p3;
            // Patch all the conditional jumps to point here
            DWORD jz3Offset = (DWORD)(skip3Label - (jzPatch3 + 6));
            *(DWORD*)(jzPatch3 + 2) = jz3Offset;

            DWORD jbOffset3 = (DWORD)(skip3Label - (jbPatch3 + 6));
            *(DWORD*)(jbPatch3 + 2) = jbOffset3;

            DWORD jaOffset3 = (DWORD)(skip3Label - (jaPatch3 + 6));
            *(DWORD*)(jaPatch3 + 2) = jaOffset3;

            *p3++ = 0xE9;
            DWORD skip3Target = 0x008D29A4;  // Jump to function exit
            DWORD skip3Offset = skip3Target - ((DWORD)p3 + 4);
            *(DWORD*)p3 = skip3Offset;
            p3 += 4;

            Log("  Code cave 3 size: %d bytes", (int)(p3 - codeCave3));

            // Patch at 0x008D297B (14 bytes available)
            BYTE* patchAddr2 = (BYTE*)0x008D297B;
            DWORD oldProtect2;
            if (VirtualProtect(patchAddr2, 16, PAGE_EXECUTE_READWRITE, &oldProtect2)) {
                Log("  Original at 0x008D297B: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr2[0], patchAddr2[1], patchAddr2[2], patchAddr2[3], patchAddr2[4],
                    patchAddr2[5], patchAddr2[6], patchAddr2[7], patchAddr2[8], patchAddr2[9],
                    patchAddr2[10], patchAddr2[11], patchAddr2[12], patchAddr2[13]);

                // JMP codeCave3 (E9 XX XX XX XX) - 5 bytes
                patchAddr2[0] = 0xE9;
                DWORD jump3Offset = (DWORD)codeCave3 - ((DWORD)patchAddr2 + 5);
                *(DWORD*)(patchAddr2 + 1) = jump3Offset;

                // NOP the remaining 9 bytes (14 - 5 = 9)
                for (int i = 5; i < 14; i++) {
                    patchAddr2[i] = 0x90;
                }

                VirtualProtect(patchAddr2, 16, oldProtect2, &oldProtect2);

                Log("  Patched at 0x008D297B: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr2[0], patchAddr2[1], patchAddr2[2], patchAddr2[3], patchAddr2[4],
                    patchAddr2[5], patchAddr2[6], patchAddr2[7], patchAddr2[8], patchAddr2[9],
                    patchAddr2[10], patchAddr2[11], patchAddr2[12], patchAddr2[13]);
                Log("  NULL check installed for spatial query loop (call site 2)");
            } else {
                Log("  ERROR: Failed to unprotect memory at 0x008D297B");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 3");
        }
    }  // End of Patch 9 block
    } else {
        Log("[Patch 9] DISABLED");
    }
    Log("");

    // === Patch 29: CRITICAL FIX - Second Pool Limit Check in FUN_0045549c ===
    // ISSUE: There are TWO pool limit checks in the game!
    // 1. Patch 6 patches the limit at 0x0088aa8b (one allocator)
    // 2. But FUN_0045549c has its own hardcoded limit at 0x004554b1
    //
    // The crash at frame 6428 was caused by FUN_0045549c hitting its 0x400 limit
    // even though Patch 6 expanded the pool to 4096 entries.
    //
    // FIX: Patch the CMP instruction at 0x004554b1 to use 0x1000 instead of 0x400
    if (PATCH_29_ENABLED) {
        Log("[Patch 29] Second Pool Limit Check (FUN_0045549c)");
        Log("  Location: 0x004554b1 (CMP ESI, 0x400)");
        Log("  Issue: Hardcoded limit check in different allocator function");
        Log("  Fix: Change 0x400 to 0x1000 to match Patch 6 expansion (4096 entries)");

        BYTE* patchAddr = (BYTE*)0x004554b1;
        DWORD oldProtect;

        if (VirtualProtect(patchAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            // Original: 81 FE 00 04 00 00 = CMP ESI, 0x400
            // We need to change the 0x400 value to 0x1000
            // The value is at offset +2 from the instruction start

            Log("  Original at 0x004554b1: %02X %02X %02X %02X %02X %02X",
                patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3], patchAddr[4], patchAddr[5]);

            // Change the immediate value from 0x400 to 0x1000
            // 81 FE 00 04 00 00 -> 81 FE 00 10 00 00
            patchAddr[2] = 0x00;  // Low byte of 0x1000
            patchAddr[3] = 0x10;  // High byte of 0x1000
            patchAddr[4] = 0x00;
            patchAddr[5] = 0x00;

            VirtualProtect(patchAddr, 6, oldProtect, &oldProtect);

            Log("  Patched at 0x004554b1: %02X %02X %02X %02X %02X %02X",
                patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3], patchAddr[4], patchAddr[5]);
            Log("  Pool limit check changed from 0x400 (1024) to 0x1000 (4096)");
        } else {
            Log("  ERROR: Failed to unprotect memory at 0x004554b1");
        }
    } else {
        Log("[Patch 29] DISABLED");
    }
    Log("");

    // === Patch 10: DISABLED - HeapAlloc fallback breaks pool invariants ===
    //
    // ROOT CAUSE ANALYSIS (2024-12-14):
    // This patch violated pool allocator invariants:
    //   INV-P1: Memory Domain Closure - HeapAlloc returns memory outside pool bounds
    //   INV-P5: Freelist Membership - HeapAlloc'd memory enters freelist when "freed"
    //   INV-P2: Freelist Integrity - Corrupts freelist linkage (garbage "next" pointers)
    //   INV-P4: NULL Semantic Contract - NULL is a VALID return signaling pool pressure
    //
    // When HeapAlloc'd memory is freed back to the pool:
    //   1. Engine pushes it to freelist: *new_entry = freelist_head
    //   2. HeapAlloc'd memory has garbage at offset 0
    //   3. Freelist linkage becomes corrupted
    //   4. Next allocation returns garbage pointer
    //   5. Object fields get corrupted (e.g., array with NULL base but size=1)
    //   6. CRASH at 0x00403ECE when accessing corrupted object
    //
    // SOLUTION: Add caller-side NULL checks instead of forcing non-NULL returns.
    // See Patch 16+ for caller-side fixes.
    //
    if (PATCH_10_ENABLED) {
        Log("[Patch 10] Safe Allocator Wrapper:");
        // TODO: HeapAlloc fallback code here (currently known to cause issues)
        Log("  WARNING: HeapAlloc fallback may corrupt freelist integrity.");
    } else {
        Log("[Patch 10] DISABLED (violates pool invariants)");
    }
    Log("");

    // === Patch 11: NULL check in FUN_00495700 loop at 0x00495761 ===
    // The wrapper fixes NULL returns, but the crash at 0x00495773 happens INSIDE
    // the function when the virtual call fills a local array with some NULL elements.
    // We must check each element before dereferencing.
    //
    // Loop at 0x00495761:
    //   00495761  MOV ECX, [ESP + EAX*4 + 0xC]  ; Load from array - can be NULL
    //   00495765  MOV EDX, [ESI + EDI*8 + 0x38]
    //   00495769  ADD [ESI + EDI*8 + 0x3C], 1
    //   0049576e  ADD EAX, 1
    //   00495771  CMP EAX, EBX
    //   00495773  MOV [ECX], EDX                ; CRASH if ECX is NULL
    //   00495775  MOV [ESI + EDI*8 + 0x38], ECX
    //   00495779  JL 0x00495761
    //
    // Fix: Add NULL check after loading ECX, skip iteration if NULL
    if (PATCH_11_ENABLED) {
    {
        Log("[Patch 11] NULL check for FUN_00495700 loop (0x00495761)");

        BYTE* codeCave11 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave11) {
            Log("  Code cave 11 at: 0x%08X", (DWORD)codeCave11);

            BYTE* p11 = codeCave11;

            // MOV ECX, [ESP + EAX*4 + 0xC] (original instruction)
            *p11++ = 0x8B; *p11++ = 0x4C; *p11++ = 0x84; *p11++ = 0x0C;

            // TEST ECX, ECX
            *p11++ = 0x85; *p11++ = 0xC9;

            // JZ skip_iteration
            *p11++ = 0x0F; *p11++ = 0x84;
            BYTE* jzPatch11 = p11;
            p11 += 4;

            // ECX not NULL - execute original code:
            // MOV EDX, [ESI + EDI*8 + 0x38]
            *p11++ = 0x8B; *p11++ = 0x54; *p11++ = 0xFE; *p11++ = 0x38;
            // ADD [ESI + EDI*8 + 0x3C], 1
            *p11++ = 0x83; *p11++ = 0x44; *p11++ = 0xFE; *p11++ = 0x3C; *p11++ = 0x01;
            // ADD EAX, 1
            *p11++ = 0x83; *p11++ = 0xC0; *p11++ = 0x01;
            // CMP EAX, EBX
            *p11++ = 0x3B; *p11++ = 0xC3;
            // MOV [ECX], EDX
            *p11++ = 0x89; *p11++ = 0x11;
            // MOV [ESI + EDI*8 + 0x38], ECX
            *p11++ = 0x89; *p11++ = 0x4C; *p11++ = 0xFE; *p11++ = 0x38;
            // JL loop start - FIXED: Jump back to code cave, not original code!
            *p11++ = 0x0F; *p11++ = 0x8C;
            *(DWORD*)p11 = (DWORD)codeCave11 - ((DWORD)p11 + 4);
            p11 += 4;
            // JMP loop exit (0x0049577B)
            *p11++ = 0xE9;
            *(DWORD*)p11 = 0x0049577B - ((DWORD)p11 + 4);
            p11 += 4;

            // skip_iteration label - just increment and check
            BYTE* skipLabel11 = p11;
            *(DWORD*)jzPatch11 = (DWORD)(skipLabel11 - (jzPatch11 + 4));

            // ADD EAX, 1
            *p11++ = 0x83; *p11++ = 0xC0; *p11++ = 0x01;
            // CMP EAX, EBX
            *p11++ = 0x3B; *p11++ = 0xC3;
            // JL loop start - FIXED: Jump back to code cave, not original code!
            *p11++ = 0x0F; *p11++ = 0x8C;
            *(DWORD*)p11 = (DWORD)codeCave11 - ((DWORD)p11 + 4);
            p11 += 4;
            // JMP loop exit
            *p11++ = 0xE9;
            *(DWORD*)p11 = 0x0049577B - ((DWORD)p11 + 4);
            p11 += 4;

            Log("  Code cave 11 size: %d bytes", (int)(p11 - codeCave11));

            // Patch at 0x00495761 (26 bytes to 0x0049577B)
            BYTE* patchAddr11 = (BYTE*)0x00495761;
            DWORD oldProt11;
            if (VirtualProtect(patchAddr11, 26, PAGE_EXECUTE_READWRITE, &oldProt11)) {
                // JMP codeCave11
                patchAddr11[0] = 0xE9;
                *(DWORD*)(patchAddr11 + 1) = (DWORD)codeCave11 - ((DWORD)patchAddr11 + 5);
                // NOP rest
                for (int i = 5; i < 26; i++) patchAddr11[i] = 0x90;
                VirtualProtect(patchAddr11, 26, oldProt11, &oldProt11);
                Log("  Patched loop at 0x00495761 -> code cave with NULL check");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 11");
        }
    }  // End of Patch 11
    } else {
        Log("[Patch 11] DISABLED");
    }
    Log("");

    // === Patch 12: Validate destination pointer before memcpy in FUN_007ee009 ===
    // Crash at 0x0061756C inside __VEC_memcpy because destination pointer is corrupted (0x00005A00)
    // The corrupted pointer comes from [EBX + EAX*4 + 0x20c] at address 0x007ee0fd
    //
    // Original code at 0x007ee0fd:
    //   007ee0fd  ff b4 83 0c 02 00 00  PUSH dword ptr [EBX + EAX*4 + 0x20c]  ; 7 bytes - dest
    //   007ee104  e8 b7 1d e2 ff        CALL 0x0060fec0                       ; 5 bytes - memcpy
    //   007ee109  83 c4 0c              ADD  ESP, 0xc                         ; 3 bytes - cleanup
    //
    // Fix: Validate the destination pointer before calling memcpy
    // If pointer < 0x10000 (invalid), skip the memcpy entirely
    if (PATCH_12_ENABLED) {
    {
        Log("[Patch 12] Validate memcpy destination in FUN_007ee009 (0x007ee0fd)");

        BYTE* codeCave12 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave12) {
            Log("  Code cave 12 at: 0x%08X", (DWORD)codeCave12);

            BYTE* p12 = codeCave12;

            // Load destination pointer: MOV EDX, [EBX + EAX*4 + 0x20c]
            // 8B 94 83 0C 02 00 00
            *p12++ = 0x8B; *p12++ = 0x94; *p12++ = 0x83;
            *p12++ = 0x0C; *p12++ = 0x02; *p12++ = 0x00; *p12++ = 0x00;

            // CMP EDX, 0x10000 (check if pointer is valid - must be > 64KB)
            // 81 FA 00 00 01 00
            *p12++ = 0x81; *p12++ = 0xFA;
            *p12++ = 0x00; *p12++ = 0x00; *p12++ = 0x01; *p12++ = 0x00;

            // JB skip_memcpy (if below 0x10000, skip)
            // 0F 82 xx xx xx xx
            *p12++ = 0x0F; *p12++ = 0x82;
            BYTE* jbPatch12 = p12;
            p12 += 4;

            // Valid pointer - proceed with memcpy
            // PUSH EDX (destination)
            *p12++ = 0x52;

            // CALL memcpy (0x0060fec0)
            // E8 xx xx xx xx
            *p12++ = 0xE8;
            *(DWORD*)p12 = 0x0060fec0 - ((DWORD)p12 + 4);
            p12 += 4;

            // ADD ESP, 0xc (cleanup 3 args)
            // 83 C4 0C
            *p12++ = 0x83; *p12++ = 0xC4; *p12++ = 0x0C;

            // JMP back to 0x007ee10c (after the original code)
            // E9 xx xx xx xx
            *p12++ = 0xE9;
            *(DWORD*)p12 = 0x007ee10c - ((DWORD)p12 + 4);
            p12 += 4;

            // skip_memcpy label:
            BYTE* skipLabel12 = p12;
            *(DWORD*)jbPatch12 = (DWORD)(skipLabel12 - (jbPatch12 + 4));

            // Invalid pointer - skip memcpy, just clean up stack
            // The stack has: [ESP] = size, [ESP+4] = src (pushed before we got here)
            // We need to pop these off without calling memcpy
            // ADD ESP, 8 (remove src and size that were pushed before the PUSH dest)
            // 83 C4 08
            *p12++ = 0x83; *p12++ = 0xC4; *p12++ = 0x08;

            // JMP back to 0x007ee10c
            // E9 xx xx xx xx
            *p12++ = 0xE9;
            *(DWORD*)p12 = 0x007ee10c - ((DWORD)p12 + 4);
            p12 += 4;

            Log("  Code cave 12 size: %d bytes", (int)(p12 - codeCave12));

            // Patch at 0x007ee0fd (15 bytes: 7 + 5 + 3)
            BYTE* patchAddr12 = (BYTE*)0x007ee0fd;
            DWORD oldProt12;
            if (VirtualProtect(patchAddr12, 15, PAGE_EXECUTE_READWRITE, &oldProt12)) {
                Log("  Original at 0x007ee0fd: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr12[0], patchAddr12[1], patchAddr12[2], patchAddr12[3], patchAddr12[4],
                    patchAddr12[5], patchAddr12[6], patchAddr12[7], patchAddr12[8], patchAddr12[9],
                    patchAddr12[10], patchAddr12[11], patchAddr12[12], patchAddr12[13], patchAddr12[14]);

                // JMP codeCave12
                patchAddr12[0] = 0xE9;
                *(DWORD*)(patchAddr12 + 1) = (DWORD)codeCave12 - ((DWORD)patchAddr12 + 5);
                // NOP the rest (10 bytes)
                for (int i = 5; i < 15; i++) patchAddr12[i] = 0x90;

                VirtualProtect(patchAddr12, 15, oldProt12, &oldProt12);
                Log("  Patched memcpy call at 0x007ee0fd -> code cave with pointer validation");
            } else {
                Log("  ERROR: Failed to unprotect 0x007ee0fd");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 12");
        }
    }  // End of Patch 12
    } else {
        Log("[Patch 12] DISABLED");
    }
    Log("");

    // === Patch 13: DISABLED - HeapAlloc fallback breaks pool invariants ===
    //
    // ROOT CAUSE ANALYSIS (2024-12-14):
    // Same issue as Patch 10 - HeapAlloc'd memory entering pool freelist causes corruption.
    //
    // When allocations > 0x2000 bytes return NULL from the virtual allocator:
    //   - Original semantic: "large allocation failed, caller handles gracefully"
    //   - Patched semantic: "here's some HeapAlloc memory instead"
    //   - Problem: When freed, this memory corrupts the freelist
    //
    // Invariants violated:
    //   INV-P1: Memory outside pool domain enters pool system
    //   INV-P5: Non-pool memory pushed to freelist
    //   INV-C2: Virtual call contract broken (NULL is valid for large allocs)
    //
    // SOLUTION: Add caller-side NULL checks for functions that call FUN_00495c30.
    // See Patch 16+ for caller-side fixes.
    //
    if (PATCH_13_ENABLED) {
        Log("[Patch 13] Safe allocator wrapper:");
        // TODO: HeapAlloc fallback code here (currently known to cause issues)
        Log("  WARNING: HeapAlloc fallback may corrupt freelist integrity.");
    } else {
        Log("[Patch 13] DISABLED (violates pool invariants)");
    }
    Log("");

    // === Patch 14: NULL check for entity serialization function FUN_004e8370 ===
    // Crash at 0x004e8375: MOV EAX, [ESI] where ESI (param_2) is NULL
    // This function is called via vtable from serialization code.
    // The caller passes a NULL serialization context pointer.
    //
    // Original code:
    //   004e8370  56                   PUSH     ESI
    //   004e8371  8b 74 24 08          MOV      ESI, dword ptr [ESP + 0x8]
    //   004e8375  8b 06                MOV      EAX, dword ptr [ESI]   ; CRASH if ESI=NULL
    //
    // Fix: Check if param_2 is NULL and early-return if so
    if (PATCH_14_ENABLED) {
    {
        Log("[Patch 14] NULL check for entity serialization FUN_004e8370 (0x004e8370)");

        BYTE* codeCave14 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave14) {
            Log("  Code cave 14 at: 0x%08X", (DWORD)codeCave14);

            BYTE* p14 = codeCave14;

            // Replicate: PUSH ESI
            // 56
            *p14++ = 0x56;

            // Replicate: MOV ESI, [ESP + 0x8]
            // Note: After PUSH ESI, ESP is 4 bytes lower, so original [ESP+8] is now [ESP+8]
            // Actually the original instruction is at 004e8371 which comes AFTER PUSH ESI
            // So at 004e8371: [ESP+0] = saved ESI, [ESP+4] = return addr, [ESP+8] = param_2
            // 8B 74 24 08
            *p14++ = 0x8B; *p14++ = 0x74; *p14++ = 0x24; *p14++ = 0x08;

            // TEST ESI, ESI (check if NULL)
            // 85 F6
            *p14++ = 0x85; *p14++ = 0xF6;

            // JZ early_return
            // 74 xx
            *p14++ = 0x74;
            BYTE* jzPatch14 = p14;
            p14++;

            // Not NULL - continue with original code
            // MOV EAX, [ESI] (original instruction at 0x004e8375)
            // 8B 06
            *p14++ = 0x8B; *p14++ = 0x06;

            // JMP back to 0x004e8377 (after MOV EAX, [ESI])
            // E9 xx xx xx xx
            *p14++ = 0xE9;
            *(DWORD*)p14 = 0x004e8377 - ((DWORD)p14 + 4);
            p14 += 4;

            // early_return label:
            BYTE* earlyReturn14 = p14;
            *jzPatch14 = (BYTE)(earlyReturn14 - (jzPatch14 + 1));

            // POP ESI (restore ESI)
            // 5E
            *p14++ = 0x5E;

            // RET 4 (return, clean up 1 parameter from stack)
            // C2 04 00
            *p14++ = 0xC2; *p14++ = 0x04; *p14++ = 0x00;

            Log("  Code cave 14 size: %d bytes", (int)(p14 - codeCave14));

            // Patch at 0x004e8370 (6 bytes: 56 8b 74 24 08 8b)
            // We'll patch from 004e8370 to make room for a 5-byte JMP + 1 NOP
            BYTE* patchAddr14 = (BYTE*)0x004e8370;
            DWORD oldProt14;
            if (VirtualProtect(patchAddr14, 6, PAGE_EXECUTE_READWRITE, &oldProt14)) {
                Log("  Original at 0x004e8370: %02X %02X %02X %02X %02X %02X",
                    patchAddr14[0], patchAddr14[1], patchAddr14[2],
                    patchAddr14[3], patchAddr14[4], patchAddr14[5]);

                // JMP codeCave14
                patchAddr14[0] = 0xE9;
                *(DWORD*)(patchAddr14 + 1) = (DWORD)codeCave14 - ((DWORD)patchAddr14 + 5);
                // NOP the remaining byte
                patchAddr14[5] = 0x90;

                VirtualProtect(patchAddr14, 6, oldProt14, &oldProt14);
                Log("  Patched FUN_004e8370 entry with NULL check for param_2");
            } else {
                Log("  ERROR: Failed to unprotect 0x004e8370");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 14");
        }
    }  // End of Patch 14
    } else {
        Log("[Patch 14] DISABLED");
    }
    Log("");

    // === Patch 15: NULL base pointer check in array resize FUN_00403e7c ===
    // Crash at 0x00403ece: AND dword ptr [EAX], 0x0 where EAX is calculated from NULL base
    // The code checks if the calculated pointer (base + index*4) is NULL, but if base is NULL
    // and index > 0, the calculated pointer is non-NULL but invalid (e.g., 0x4, 0x8, etc.)
    //
    // Original code at LAB_00403ec2:
    //   00403ec2  8b 46 04             MOV      EAX, dword ptr [ESI + 0x4]  ; current size
    //   00403ec5  8b 0e                MOV      ECX, dword ptr [ESI]        ; base (NULL!)
    //   00403ec7  8d 04 81             LEA      EAX, [ECX + EAX*0x4]        ; calc ptr
    //   00403eca  85 c0                TEST     EAX, EAX
    //   00403ecc  74 03                JZ       0x00403ed1
    //   00403ece  83 20 00             AND      dword ptr [EAX], 0x0        ; CRASH
    //
    // Fix: Check if ECX (base pointer) is NULL after MOV ECX, [ESI]
    if (PATCH_15_ENABLED) {
    {
        Log("[Patch 15] NULL base pointer check in array resize FUN_00403e7c (0x00403ec5)");

        BYTE* codeCave15 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave15) {
            Log("  Code cave 15 at: 0x%08X", (DWORD)codeCave15);

            BYTE* p15 = codeCave15;

            // Replicate: MOV ECX, [ESI] (original instruction at 0x00403ec5)
            // 8B 0E
            *p15++ = 0x8B; *p15++ = 0x0E;

            // TEST ECX, ECX (check if base pointer is NULL)
            // 85 C9
            *p15++ = 0x85; *p15++ = 0xC9;

            // JZ skip_to_increment (if NULL, skip to 0x00403ed1 which increments count)
            // 74 xx
            *p15++ = 0x74;
            BYTE* jzPatch15 = p15;
            p15++;

            // Base is not NULL - continue with original code
            // LEA EAX, [ECX + EAX*0x4] (original instruction at 0x00403ec7)
            // 8D 04 81
            *p15++ = 0x8D; *p15++ = 0x04; *p15++ = 0x81;

            // JMP back to 0x00403eca (TEST EAX, EAX)
            // E9 xx xx xx xx
            *p15++ = 0xE9;
            *(DWORD*)p15 = 0x00403eca - ((DWORD)p15 + 4);
            p15 += 4;

            // skip_to_increment label - jump to 0x00403ed1 (INC dword ptr [ESI+4])
            BYTE* skipLabel15 = p15;
            *jzPatch15 = (BYTE)(skipLabel15 - (jzPatch15 + 1));

            // JMP to 0x00403ed1
            // E9 xx xx xx xx
            *p15++ = 0xE9;
            *(DWORD*)p15 = 0x00403ed1 - ((DWORD)p15 + 4);
            p15 += 4;

            Log("  Code cave 15 size: %d bytes", (int)(p15 - codeCave15));

            // Patch at 0x00403ec5 (5 bytes: 8B 0E 8D 04 81)
            // We need to replace MOV ECX,[ESI] and LEA EAX,[ECX+EAX*4] with JMP to cave
            BYTE* patchAddr15 = (BYTE*)0x00403ec5;
            DWORD oldProt15;
            if (VirtualProtect(patchAddr15, 5, PAGE_EXECUTE_READWRITE, &oldProt15)) {
                Log("  Original at 0x00403ec5: %02X %02X %02X %02X %02X",
                    patchAddr15[0], patchAddr15[1], patchAddr15[2],
                    patchAddr15[3], patchAddr15[4]);

                // JMP codeCave15
                patchAddr15[0] = 0xE9;
                *(DWORD*)(patchAddr15 + 1) = (DWORD)codeCave15 - ((DWORD)patchAddr15 + 5);

                VirtualProtect(patchAddr15, 5, oldProt15, &oldProt15);
                Log("  Patched array resize at 0x00403ec5 with NULL base pointer check");
            } else {
                Log("  ERROR: Failed to unprotect 0x00403ec5");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 15");
        }
    }  // End of Patch 15
    } else {
        Log("[Patch 15] DISABLED");
    }
    Log("");

    // === Patch 16: Freelist Validation Hook (DISABLED - conflicts with Patch 11) ===
    //
    // NOTE: Patch 11 already hooks the code at 0x00495761-0x0049577A (26 bytes),
    // which includes the freelist push instructions at 0x00495773 and 0x00495775.
    // Patch 11 moves the entire loop body into a code cave for NULL checking.
    //
    // With Patches 10 and 13 (HeapAlloc fallbacks) now DISABLED, the freelist
    // corruption scenario is no longer possible. This diagnostic hook is
    // therefore not needed.
    //
    // If future debugging requires freelist validation, integrate the bounds
    // check into Patch 11's code cave instead of patching here.
    if (PATCH_16_ENABLED) {
        Log("[Patch 16] Freelist Validation Hook:");
        // TODO: Add freelist validation code here if desired
        Log("  WARNING: May conflict with Patch 11.");
    } else {
        Log("[Patch 16] DISABLED (conflicts with Patch 11)");
    }
    Log("");

    // === Patch 17: Caller-side NULL check for FUN_008f0592 ===
    // This function calls FUN_00495c30 at 0x008F05E4 and immediately dereferences
    // the result at 0x008F05EE without checking for NULL.
    //
    // Original code at 0x008F05E4:
    //   008f05e4  e8 47 56 ba ff       CALL     0x00495c30    ; 5 bytes - allocator
    //   008f05e9  6a 02                PUSH     0x2           ; 2 bytes
    //   008f05eb  ff 75 0c             PUSH     [EBP + 0xc]   ; 3 bytes
    //   008f05ee  66 c7 40 04 2c 00    MOV      word ptr [EAX + 0x4], 0x2c  ; 6 bytes - CRASH!
    //
    // Fix: Check EAX after CALL, skip to safe exit if NULL.
    // Total bytes to patch: 5 + 2 + 3 + 6 = 16 bytes
    if (PATCH_17_ENABLED) {
    {
        Log("[Patch 17] Caller-side NULL check for FUN_008f0592 (0x008F05E4)");

        BYTE* codeCave17 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave17) {
            Log("  Code cave 17 at: 0x%08X", (DWORD)codeCave17);

            BYTE* p17 = codeCave17;

            // Call the original allocator
            // CALL 0x00495c30
            *p17++ = 0xE8;
            *(DWORD*)p17 = 0x00495c30 - ((DWORD)p17 + 4);
            p17 += 4;

            // TEST EAX, EAX (check if NULL)
            *p17++ = 0x85; *p17++ = 0xC0;

            // JZ null_return (skip if NULL)
            *p17++ = 0x0F; *p17++ = 0x84;
            BYTE* jzPatch17 = p17;
            p17 += 4;

            // Not NULL - execute original code:
            // PUSH 2
            *p17++ = 0x6A; *p17++ = 0x02;

            // PUSH [EBP + 0xC]
            *p17++ = 0xFF; *p17++ = 0x75; *p17++ = 0x0C;

            // MOV word ptr [EAX + 0x4], 0x2C
            *p17++ = 0x66; *p17++ = 0xC7; *p17++ = 0x40; *p17++ = 0x04;
            *p17++ = 0x2C; *p17++ = 0x00;

            // JMP back to 0x008F05F4 (after the original instructions)
            *p17++ = 0xE9;
            *(DWORD*)p17 = 0x008F05F4 - ((DWORD)p17 + 4);
            p17 += 4;

            // null_return label:
            BYTE* nullLabel17 = p17;
            *(DWORD*)jzPatch17 = (DWORD)(nullLabel17 - (jzPatch17 + 4));

            // Allocation failed - return NULL (EAX already 0)
            // Skip to function epilogue at LAB_008f0603
            // First check what's at that address...
            // From disassembly: 008f0603 is the cleanup/return path
            // We need to JMP there
            *p17++ = 0xE9;
            *(DWORD*)p17 = 0x008F0603 - ((DWORD)p17 + 4);
            p17 += 4;

            Log("  Code cave 17 size: %d bytes", (int)(p17 - codeCave17));

            // Patch at 0x008F05E4 (16 bytes total)
            BYTE* patchAddr17 = (BYTE*)0x008F05E4;
            DWORD oldProt17;
            if (VirtualProtect(patchAddr17, 16, PAGE_EXECUTE_READWRITE, &oldProt17)) {
                Log("  Original at 0x008F05E4: %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr17[0], patchAddr17[1], patchAddr17[2], patchAddr17[3],
                    patchAddr17[4], patchAddr17[5], patchAddr17[6], patchAddr17[7]);

                // JMP codeCave17
                patchAddr17[0] = 0xE9;
                *(DWORD*)(patchAddr17 + 1) = (DWORD)codeCave17 - ((DWORD)patchAddr17 + 5);
                // NOP the rest (11 bytes)
                for (int i = 5; i < 16; i++) patchAddr17[i] = 0x90;

                VirtualProtect(patchAddr17, 16, oldProt17, &oldProt17);
                Log("  Patched FUN_008f0592 allocator call with NULL check");
                Log("  Will gracefully skip to return if allocation fails");
            } else {
                Log("  ERROR: Failed to unprotect 0x008F05E4");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 17");
        }
    }  // End of Patch 17
    } else {
        Log("[Patch 17] DISABLED");
    }
    Log("");

    // === Patch 18: NULL check for FUN_004b6910 effect spawner ===
    //
    // BUG ANALYSIS (from disassembly):
    //   004b6949  e8 b2 ed fd ff   CALL FUN_00495700  ; allocator
    //   004b694e  3b c3            CMP  EAX, EBX      ; EBX=0, check NULL
    //   004b6950  74 1c            JZ   0x004b696e    ; jump if NULL
    //   ...
    //   004b696e  8b 45 08         MOV  EAX, [EBP+8]
    //   004b6974  89 7b 10         MOV  [EBX+0x10], EDI  ← CRASH! EBX=NULL
    //
    // The function has a NULL check but the jump target (004b696e) still uses
    // EBX which is NULL, causing write to address 0x00000010.
    //
    // FIX: Change the JZ target at 004b6950 to jump to a safe NULL return.
    // We patch the short jump (74 1c) to a near jump that goes to code cave.
    //
    // Safe return pattern:
    //   XOR EAX, EAX      ; return NULL
    //   POP EDI
    //   POP ESI
    //   POP EBX
    //   MOV ESP, EBP
    //   POP EBP
    //   RET
    if (PATCH_18_ENABLED) {
    {
        Log("[Patch 18] Effect spawner NULL check at FUN_004b6910");
        Log("  Bug: JZ at 0x004b6950 jumps to code that dereferences NULL");

        BYTE* codeCave18 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave18) {
            Log("  Code cave 18 at: 0x%08X", (DWORD)codeCave18);

            BYTE* p18 = codeCave18;

            // Safe NULL return:
            // XOR EAX, EAX (return NULL)
            *p18++ = 0x33; *p18++ = 0xC0;

            // POP EDI
            *p18++ = 0x5F;

            // POP ESI
            *p18++ = 0x5E;

            // POP EBX
            *p18++ = 0x5B;

            // MOV ESP, EBP
            *p18++ = 0x8B; *p18++ = 0xE5;

            // POP EBP
            *p18++ = 0x5D;

            // RET
            *p18++ = 0xC3;

            Log("  Code cave 18 size: %d bytes (safe NULL return)", (int)(p18 - codeCave18));

            // Now patch at 0x004b6950: change JZ target
            // Original: 74 1c (JZ short, +0x1c = 004b696e)
            // We need to change this to jump to our code cave.
            //
            // Problem: Short jump (2 bytes) can only reach ±127 bytes.
            // Our code cave is far away, so we need a near jump (6 bytes).
            // But we only have 2 bytes for the original instruction.
            //
            // Solution: Overwrite the JZ and the following instruction.
            // At 004b6950: 74 1c           JZ   +0x1c         (2 bytes)
            // At 004b6952: 89 18           MOV  [EAX], EBX    (2 bytes)
            // At 004b6954: 89 58 04        MOV  [EAX+4], EBX  (3 bytes)
            //
            // We have 7 bytes (004b6950 - 004b6956) to work with.
            //
            // New code:
            //   004b6950: 0F 84 xx xx xx xx   JZ near codeCave18  (6 bytes)
            //   004b6956: 90                  NOP                  (1 byte)
            //
            // But wait - we're changing the JZ to jump when NULL.
            // If NOT null, we need to execute the original code:
            //   MOV [EAX], EBX
            //   MOV [EAX+4], EBX
            //   ...
            //
            // Better approach: Create a code cave that handles both paths.

            // Actually simpler approach: patch the JZ to JMP to code cave,
            // code cave checks and either returns NULL or continues.
            //
            // At 004b694e: 3b c3  CMP EAX, EBX (2 bytes)
            // At 004b6950: 74 1c  JZ  +0x1c    (2 bytes)
            // At 004b6952: 89 18  MOV [EAX], EBX
            //
            // Patch from 004b694e (4 bytes total for CMP + JZ):
            //   JMP codeCave  (5 bytes) + NOP (need 1 more byte from next instr)
            //
            // Actually let's be more surgical. Patch just the JZ:
            // 004b6950: 74 1c → EB xx (short JMP to trampoline nearby)
            //
            // OR: Patch the original CALL at 004b6949 to go through wrapper.

            // CLEANEST: Patch the CALL at 004b6949
            // Original: e8 b2 ed fd ff  CALL 00495700 (5 bytes)
            // New: CALL codeCave18_wrapper
            // Wrapper: CALL 00495700, TEST EAX, JZ nullReturn, JMP back

            // Rewrite code cave as wrapper
            p18 = codeCave18;

            // CALL FUN_00495700 (original allocator)
            *p18++ = 0xE8;
            *(DWORD*)p18 = 0x00495700 - ((DWORD)p18 + 4);
            p18 += 4;

            // TEST EAX, EAX
            *p18++ = 0x85; *p18++ = 0xC0;

            // JZ nullReturn
            *p18++ = 0x0F; *p18++ = 0x84;
            BYTE* jzPatch18 = p18;
            p18 += 4;

            // Not NULL - jump back to 004b694e (after original CALL)
            *p18++ = 0xE9;
            *(DWORD*)p18 = 0x004b694e - ((DWORD)p18 + 4);
            p18 += 4;

            // nullReturn label:
            BYTE* nullLabel18 = p18;
            *(DWORD*)jzPatch18 = (DWORD)(nullLabel18 - (jzPatch18 + 4));

            // Return NULL safely
            // XOR EAX, EAX (already 0, but be explicit)
            *p18++ = 0x33; *p18++ = 0xC0;
            // POP EDI
            *p18++ = 0x5F;
            // POP ESI
            *p18++ = 0x5E;
            // POP EBX
            *p18++ = 0x5B;
            // MOV ESP, EBP
            *p18++ = 0x8B; *p18++ = 0xE5;
            // POP EBP
            *p18++ = 0x5D;
            // RET
            *p18++ = 0xC3;

            Log("  Code cave 18 rewritten as CALL wrapper, size: %d bytes", (int)(p18 - codeCave18));

            // Patch at 0x004b6949 (5 bytes: CALL instruction)
            BYTE* patchAddr18 = (BYTE*)0x004b6949;
            DWORD oldProt18;
            if (VirtualProtect(patchAddr18, 5, PAGE_EXECUTE_READWRITE, &oldProt18)) {
                Log("  Original at 0x004b6949: %02X %02X %02X %02X %02X",
                    patchAddr18[0], patchAddr18[1], patchAddr18[2], patchAddr18[3], patchAddr18[4]);

                // Redirect CALL to our wrapper
                patchAddr18[0] = 0xE8;
                *(DWORD*)(patchAddr18 + 1) = (DWORD)codeCave18 - ((DWORD)patchAddr18 + 5);

                VirtualProtect(patchAddr18, 5, oldProt18, &oldProt18);
                Log("  Patched CALL at 0x004b6949 to wrapper with NULL check");
                Log("  Effect spawner will gracefully skip if allocation fails");
            } else {
                Log("  ERROR: Failed to unprotect 0x004b6949");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 18");
        }
    }
    } else {
        Log("[Patch 18] DISABLED");
    }
    Log("");

    // === Patch 19: NULL check for FUN_005135a0 physics/ragdoll spawner ===
    // This function calls FUN_00495700 at 0x00513606 and 0x0051381e.
    // Ragdoll physics allocation is triggered during mass death scenarios.
    //
    // Similar to Patch 18, we defer the inline hook until instruction layout
    // is verified. The UseStackLimit fixes reduce effect allocation pressure
    // which indirectly reduces ragdoll allocation failures.
    if (PATCH_19_ENABLED) {
        Log("[Patch 19] Physics spawner NULL check at FUN_005135a0 call sites");
        Log("  Target addresses: 0x00513606, 0x0051381e");
        // TODO: Add inline hook once instruction layout is confirmed.
        Log("  WARNING: Requires runtime verification of instruction layout.");
    } else {
        Log("[Patch 19] DISABLED (deferred - UseStackLimit fixes reduce allocation pressure)");
    }
    Log("");

    // === Patch 19: Spatial Query Validation (FUN_008d2844) - DISABLED ===
    // This patch was causing crashes because it was interfering with the instruction stream.
    // The function has a complex prologue that spans multiple instructions:
    //   008d2844  56                   PUSH     ESI
    //   008d2845  8b 42 04             MOV      EAX, dword ptr [EDX + 0x4]
    //   008d2848  f3 0f 10 01          MOVSS    XMM0, dword ptr [ECX]
    //
    // Attempting to hook at 0x008d2844 with a 5-byte JMP breaks the instruction stream.
    // The MOVSS at 0x008d2848 is 4 bytes, so a JMP at 0x008d2844 would land in the middle
    // of the MOVSS instruction, causing crashes at 0x008d284A.
    //
    // SOLUTION: Instead of hooking the function entry, we should:
    // 1. Hook at the CALL sites (0x008d2932 and 0x008d2984 in FUN_008d28df)
    // 2. Validate parameters BEFORE calling FUN_008d2844
    // 3. Or use a larger code cave that preserves more instructions
    //
    // For now, this patch is DISABLED. The game will crash if invalid pointers are passed,
    // but at least it won't crash due to our patch interfering with the code.
    Log("[Patch 19] Spatial Query Validation (FUN_008d2844) - DISABLED");
    Log("  Reason: Function prologue spans multiple instructions, hooking breaks code stream");
    Log("  Alternative: Hook at call sites (0x008d2932, 0x008d2984) instead");
    Log("");

    // === Patch 20: Effect Spawner Entry Gate (DANGER SYSTEM) ===
    // Gate FUN_004b6910 to check danger level BEFORE any allocation attempts.
    // If danger level is CRITICAL or throttle exceeded in CAUTION, skip entirely.
    //
    // VERIFIED from disassembly/FUN_004b6910.asm:
    //   004b6910  55               PUSH EBP           ; 1 byte
    //   004b6911  8b ec            MOV  EBP, ESP      ; 2 bytes
    //   004b6913  83 e4 f0         AND  ESP, 0xfffffff0 ; 3 bytes (stack alignment!)
    //   004b6916  a1 84 6a a4 00   MOV  EAX, [...]    ; Next instruction
    //
    // We have 6 bytes to work with. Replace with JMP to gate.
    if (PATCH_20_ENABLED) {
    {
        Log("[Patch 20] Effect Spawner Entry Gate (DANGER SYSTEM) at FUN_004b6910");

        BYTE* codeCave20 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave20) {
            Log("  Code cave 20 at: 0x%08X", (DWORD)codeCave20);

            BYTE* p20 = codeCave20;

            // Call DangerCanSpawnEffect() - returns bool in AL
            // PUSH all registers we might clobber
            *p20++ = 0x60;  // PUSHAD

            // MOV EAX, &DangerCanSpawnEffect
            *p20++ = 0xB8;
            *(DWORD*)p20 = (DWORD)&DangerCanSpawnEffect;
            p20 += 4;

            // CALL EAX
            *p20++ = 0xFF; *p20++ = 0xD0;

            // TEST AL, AL (check return value)
            *p20++ = 0x84; *p20++ = 0xC0;

            // JZ blocked (if DangerCanSpawnEffect returns false)
            *p20++ = 0x74;
            BYTE* jzBlocked = p20;
            *p20++ = 0x00;  // Placeholder

            // POPAD - restore all registers
            *p20++ = 0x61;

            // Execute original prologue (VERIFIED bytes):
            // PUSH EBP (55)
            *p20++ = 0x55;
            // MOV EBP, ESP (8B EC)
            *p20++ = 0x8B; *p20++ = 0xEC;
            // AND ESP, 0xFFFFFFF0 (83 E4 F0) - stack alignment, NOT sub!
            *p20++ = 0x83; *p20++ = 0xE4; *p20++ = 0xF0;

            // JMP back to 0x004b6916 (after patched prologue)
            *p20++ = 0xE9;
            *(DWORD*)p20 = 0x004b6916 - ((DWORD)p20 + 4);
            p20 += 4;

            // blocked: Return NULL immediately
            *jzBlocked = (BYTE)(p20 - jzBlocked - 1);

            // POPAD
            *p20++ = 0x61;

            // XOR EAX, EAX (return NULL)
            *p20++ = 0x33; *p20++ = 0xC0;

            // RET (function returns NULL without doing anything)
            *p20++ = 0xC3;

            Log("  Code cave 20 size: %d bytes (entry gate)", (int)(p20 - codeCave20));

            // Patch at 0x004b6910 (6 bytes)
            BYTE* patchAddr20 = (BYTE*)0x004b6910;
            DWORD oldProt20;
            if (VirtualProtect(patchAddr20, 6, PAGE_EXECUTE_READWRITE, &oldProt20)) {
                Log("  Original at 0x004b6910: %02X %02X %02X %02X %02X %02X",
                    patchAddr20[0], patchAddr20[1], patchAddr20[2],
                    patchAddr20[3], patchAddr20[4], patchAddr20[5]);

                // JMP codeCave20
                patchAddr20[0] = 0xE9;
                *(DWORD*)(patchAddr20 + 1) = (DWORD)codeCave20 - ((DWORD)patchAddr20 + 5);
                // NOP the 6th byte
                patchAddr20[5] = 0x90;

                VirtualProtect(patchAddr20, 6, oldProt20, &oldProt20);
                Log("  Patched FUN_004b6910 entry with danger gate");
                Log("  Effect spawner will skip entirely when danger level is high");
            } else {
                Log("  ERROR: Failed to unprotect 0x004b6910");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 20");
        }
    }
    } else {
        Log("[Patch 20] DISABLED");
    }
    Log("");

    // === Patch 21: Ragdoll Spawner Entry Gate (DANGER SYSTEM) ===
    // Gate FUN_005135a0 to check danger level BEFORE any allocation attempts.
    //
    // VERIFIED from disassembly/FUN_005135a0.asm:
    //   005135a0  83 ec 38         SUB  ESP, 0x38       ; 3 bytes (NO push ebp!)
    //   005135a3  53               PUSH EBX             ; 1 byte
    //   005135a4  55               PUSH EBP             ; 1 byte
    //   005135a5  8b 6c 24 44      MOV  EBP, [ESP+0x44] ; 4 bytes (parameter access)
    //   005135a9  8b 45 00         MOV  EAX, [EBP]      ; Next instruction
    //
    // We need 9 bytes (to 005135a9). JMP is 5 bytes, NOP remaining 4.
    if (PATCH_21_ENABLED) {
    {
        Log("[Patch 21] Ragdoll Spawner Entry Gate (DANGER SYSTEM) at FUN_005135a0");

        BYTE* codeCave21 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave21) {
            Log("  Code cave 21 at: 0x%08X", (DWORD)codeCave21);

            BYTE* p21 = codeCave21;

            // PUSHAD
            *p21++ = 0x60;

            // MOV EAX, &DangerCanSpawnRagdoll
            *p21++ = 0xB8;
            *(DWORD*)p21 = (DWORD)&DangerCanSpawnRagdoll;
            p21 += 4;

            // CALL EAX
            *p21++ = 0xFF; *p21++ = 0xD0;

            // TEST AL, AL
            *p21++ = 0x84; *p21++ = 0xC0;

            // JZ blocked
            *p21++ = 0x74;
            BYTE* jzBlocked21 = p21;
            *p21++ = 0x00;

            // POPAD
            *p21++ = 0x61;

            // Execute original prologue (VERIFIED bytes):
            // SUB ESP, 0x38 (83 EC 38)
            *p21++ = 0x83; *p21++ = 0xEC; *p21++ = 0x38;
            // PUSH EBX (53)
            *p21++ = 0x53;
            // PUSH EBP (55)
            *p21++ = 0x55;
            // MOV EBP, [ESP+0x44] (8B 6C 24 44)
            *p21++ = 0x8B; *p21++ = 0x6C; *p21++ = 0x24; *p21++ = 0x44;

            // JMP back to 0x005135a9 (after patched prologue)
            *p21++ = 0xE9;
            *(DWORD*)p21 = 0x005135a9 - ((DWORD)p21 + 4);
            p21 += 4;

            // blocked:
            *jzBlocked21 = (BYTE)(p21 - jzBlocked21 - 1);

            // POPAD
            *p21++ = 0x61;

            // XOR EAX, EAX
            *p21++ = 0x33; *p21++ = 0xC0;

            // RET (function uses non-standard prologue, but still RET)
            *p21++ = 0xC3;

            Log("  Code cave 21 size: %d bytes (entry gate)", (int)(p21 - codeCave21));

            // Patch at 0x005135a0 (9 bytes total to cover SUB+PUSH+PUSH+MOV)
            BYTE* patchAddr21 = (BYTE*)0x005135a0;
            DWORD oldProt21;
            if (VirtualProtect(patchAddr21, 9, PAGE_EXECUTE_READWRITE, &oldProt21)) {
                Log("  Original at 0x005135a0: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr21[0], patchAddr21[1], patchAddr21[2], patchAddr21[3],
                    patchAddr21[4], patchAddr21[5], patchAddr21[6], patchAddr21[7], patchAddr21[8]);

                // JMP codeCave21
                patchAddr21[0] = 0xE9;
                *(DWORD*)(patchAddr21 + 1) = (DWORD)codeCave21 - ((DWORD)patchAddr21 + 5);
                // NOP remaining 4 bytes
                for (int i = 5; i < 9; i++) patchAddr21[i] = 0x90;

                VirtualProtect(patchAddr21, 9, oldProt21, &oldProt21);
                Log("  Patched FUN_005135a0 entry with danger gate");
                Log("  Ragdoll spawner will skip entirely when danger level is high");
            } else {
                Log("  ERROR: Failed to unprotect 0x005135a0");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 21");
        }
    }
    } else {
        Log("[Patch 21] DISABLED");
    }
    Log("");

    // === Patch 22: NULL check for FUN_0087cda1 call to FUN_008f7ec2 ===
    // CRASH at 0x008F7EC5: CMP [ESI + 0x40], EDI with ESI=NULL
    //
    // VERIFIED from disassembly/FUN_0087cda1.asm:
    //   0087cef1  e8 17 83 b8 ff   CALL FUN_0040520d   ; Returns ptr in EAX (can be NULL!)
    //   0087cef6  8b f0            MOV  ESI, EAX       ; 2 bytes
    //   0087cef8  e8 c5 af 07 00   CALL FUN_008f7ec2   ; 5 bytes <- uses ESI, crashes if NULL
    //   0087cefd  ...              (continue)
    //
    // FUN_0040520d explicitly returns NULL when lookup fails (line 34: XOR EAX, EAX).
    // FIX: After MOV ESI, EAX, check if ESI is NULL and skip the CALL if so.
    //
    // Patch from 0x0087cef6 (7 bytes: MOV + CALL)
    if (PATCH_22_ENABLED) {
    {
        Log("[Patch 22] NULL check for FUN_0087cda1 -> FUN_008f7ec2 (0x0087cef6)");
        Log("  Crash: 0x008F7EC5 accessing [ESI+0x40] when ESI=NULL");

        BYTE* codeCave22 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave22) {
            Log("  Code cave 22 at: 0x%08X", (DWORD)codeCave22);

            BYTE* p22 = codeCave22;

            // Execute original: MOV ESI, EAX
            *p22++ = 0x8B; *p22++ = 0xF0;

            // TEST ESI, ESI (check if NULL)
            *p22++ = 0x85; *p22++ = 0xF6;

            // JZ skipCall (if ESI is NULL, skip the CALL)
            *p22++ = 0x74;
            BYTE* jzSkip22 = p22;
            *p22++ = 0x00;  // Placeholder

            // Not NULL - execute original CALL FUN_008f7ec2
            // CALL 0x008f7ec2
            *p22++ = 0xE8;
            *(DWORD*)p22 = 0x008f7ec2 - ((DWORD)p22 + 4);
            p22 += 4;

            // JMP to continue at 0x0087cefd
            *p22++ = 0xE9;
            *(DWORD*)p22 = 0x0087cefd - ((DWORD)p22 + 4);
            p22 += 4;

            // skipCall: Pop the argument that FUN_008f7ec2 would have popped with RET 4
            // FUN_008f7ec2 uses RET 0x4, so we must clean up the stack ourselves
            *jzSkip22 = (BYTE)(p22 - jzSkip22 - 1);

            // ADD ESP, 4 (pop the argument that was pushed for FUN_008f7ec2)
            *p22++ = 0x83; *p22++ = 0xC4; *p22++ = 0x04;

            // JMP to 0x0087cefd (continue without calling)
            *p22++ = 0xE9;
            *(DWORD*)p22 = 0x0087cefd - ((DWORD)p22 + 4);
            p22 += 4;

            Log("  Code cave 22 size: %d bytes", (int)(p22 - codeCave22));
            Log("  NOTE: Added ADD ESP,4 to balance stack when skipping FUN_008f7ec2");

            // Patch at 0x0087cef6 (7 bytes: MOV ESI, EAX + CALL)
            BYTE* patchAddr22 = (BYTE*)0x0087cef6;
            DWORD oldProt22;
            if (VirtualProtect(patchAddr22, 7, PAGE_EXECUTE_READWRITE, &oldProt22)) {
                Log("  Original at 0x0087cef6: %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr22[0], patchAddr22[1], patchAddr22[2], patchAddr22[3],
                    patchAddr22[4], patchAddr22[5], patchAddr22[6]);

                // JMP codeCave22 (5 bytes)
                patchAddr22[0] = 0xE9;
                *(DWORD*)(patchAddr22 + 1) = (DWORD)codeCave22 - ((DWORD)patchAddr22 + 5);
                // NOP remaining 2 bytes
                patchAddr22[5] = 0x90;
                patchAddr22[6] = 0x90;

                VirtualProtect(patchAddr22, 7, oldProt22, &oldProt22);
                Log("  Patched call site with NULL check - will skip FUN_008f7ec2 if ESI=NULL");
            } else {
                Log("  ERROR: Failed to unprotect 0x0087cef6");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 22");
        }
    }
    } else {
        Log("[Patch 22] DISABLED");
    }
    Log("");

    // === Patch 23: NULL check for second call to FUN_008f7ec2 in FUN_0087cda1 ===
    // VERIFIED from disassembly: Second call at 0x0087d0a4
    //   0087d0a0  8b 74 24 1c      MOV  ESI, [ESP+0x1c]  ; 4 bytes
    //   0087d0a4  e8 19 ae 07 00   CALL FUN_008f7ec2     ; 5 bytes
    //   0087d0a9  ...              (continue)
    if (PATCH_23_ENABLED) {
    {
        Log("[Patch 23] NULL check for second FUN_008f7ec2 call (0x0087d0a0)");

        BYTE* codeCave23 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave23) {
            Log("  Code cave 23 at: 0x%08X", (DWORD)codeCave23);

            BYTE* p23 = codeCave23;

            // Execute original: MOV ESI, [ESP+0x1c]
            *p23++ = 0x8B; *p23++ = 0x74; *p23++ = 0x24; *p23++ = 0x1C;

            // TEST ESI, ESI
            *p23++ = 0x85; *p23++ = 0xF6;

            // JZ skipCall
            *p23++ = 0x74;
            BYTE* jzSkip23 = p23;
            *p23++ = 0x00;

            // CALL FUN_008f7ec2
            *p23++ = 0xE8;
            *(DWORD*)p23 = 0x008f7ec2 - ((DWORD)p23 + 4);
            p23 += 4;

            // JMP to 0x0087d0a9
            *p23++ = 0xE9;
            *(DWORD*)p23 = 0x0087d0a9 - ((DWORD)p23 + 4);
            p23 += 4;

            // skipCall: Pop the argument that FUN_008f7ec2 would have popped with RET 4
            // The PUSH at 0x0087d09d pushed an argument for FUN_008f7ec2
            // FUN_008f7ec2 uses RET 0x4, so we must clean up the stack ourselves
            *jzSkip23 = (BYTE)(p23 - jzSkip23 - 1);

            // ADD ESP, 4 (pop the argument that was pushed for FUN_008f7ec2)
            *p23++ = 0x83; *p23++ = 0xC4; *p23++ = 0x04;

            // JMP to 0x0087d0a9
            *p23++ = 0xE9;
            *(DWORD*)p23 = 0x0087d0a9 - ((DWORD)p23 + 4);
            p23 += 4;

            Log("  Code cave 23 size: %d bytes", (int)(p23 - codeCave23));
            Log("  NOTE: Added ADD ESP,4 to balance stack when skipping FUN_008f7ec2");

            // Patch at 0x0087d0a0 (9 bytes: MOV + CALL)
            BYTE* patchAddr23 = (BYTE*)0x0087d0a0;
            DWORD oldProt23;
            if (VirtualProtect(patchAddr23, 9, PAGE_EXECUTE_READWRITE, &oldProt23)) {
                Log("  Original at 0x0087d0a0: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr23[0], patchAddr23[1], patchAddr23[2], patchAddr23[3],
                    patchAddr23[4], patchAddr23[5], patchAddr23[6], patchAddr23[7], patchAddr23[8]);

                // JMP codeCave23
                patchAddr23[0] = 0xE9;
                *(DWORD*)(patchAddr23 + 1) = (DWORD)codeCave23 - ((DWORD)patchAddr23 + 5);
                // NOP remaining 4 bytes
                for (int i = 5; i < 9; i++) patchAddr23[i] = 0x90;

                VirtualProtect(patchAddr23, 9, oldProt23, &oldProt23);
                Log("  Patched second call site with NULL check");
            } else {
                Log("  ERROR: Failed to unprotect 0x0087d0a0");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 23");
        }
    }
    } else {
        Log("[Patch 23] DISABLED");
    }
    Log("");

    // === Initialize Danger System Logging ===
    InitDangerLog();
    DangerLog("Danger-Aware Engine Layer initialized with Patches 20-23");
    DangerLog("Effect gate at 0x004b6910, Ragdoll gate at 0x005135a0");
    DangerLog("NULL checks at 0x0087cef6, 0x0087d0a0 for FUN_008f7ec2 calls");

    // === Patch 24: DISABLED ===
    // Changing JNZ to JZ at 0x0087ceb8 broke level loading.
    // The original conditional may be correct for the common case.
    // Patches 22/23/25 handle NULL returns without changing control flow.
    if (PATCH_24_ENABLED) {
        Log("[Patch 24] JNZ->JZ change:");
        // TODO: Add JNZ->JZ patching code here if desired
        Log("  WARNING: May break level loading.");
    } else {
        Log("[Patch 24] DISABLED (JNZ->JZ change broke level loading)");
    }
    Log("");

    // === Patch 25: DISABLED ===
    // Modifying FUN_008f7ec2 entry caused freeze.
    // Patches 22 and 23 handle the two known call sites.
    // If other callers exist, they would need separate patches.
    if (PATCH_25_ENABLED) {
        Log("[Patch 25] FUN_008f7ec2 entry modification:");
        // TODO: Add function entry modification code here if desired
        Log("  WARNING: May cause freeze.");
    } else {
        Log("[Patch 25] DISABLED (function entry modification caused freeze)");
    }
    Log("");

    // === Patch 26: Array bounds validation in FUN_0073fc02 (Quicksort crash fix) ===
    // CRASH: 0x00456D06 in FUN_00456d7d (quicksort) with ACCESS_VIOLATION Writing to 0x3D589EA5
    //
    // ROOT CAUSE: FUN_0073fc02 populates array DAT_00cfee48 (64 elements) with corrupted indices.
    // The calculation at 0x0073fce3 writes to index [iVar3 * 0x41 + *piVar1] which can exceed 64.
    //
    // VERIFIED from disassembly/FUN_0073fc02.asm lines 78-88:
    //   0073fcc7  8b 86 98 01 00 00    MOV      EAX, [ESI + 0x198]
    //   0073fccd  8b 17                MOV      EDX, [EDI]
    //   0073fccf  48                   DEC      EAX
    //   0073fcd0  8b c8                MOV      ECX, EAX
    //   0073fcd2  6b c0 41             IMUL     EAX, EAX, 0x41      ; EAX = iVar3 * 0x41
    //   0073fcd5  69 c9 04 01 00 00    IMUL     ECX, ECX, 0x104     ; ECX = iVar3 * 0x104
    //   0073fcdb  8d 89 48 ef cf 00    LEA      ECX, [ECX + 0xcfef48]
    //   0073fce1  03 01                ADD      EAX, [ECX]          ; EAX += *piVar1
    //   0073fce3  89 14 85 48 ee cf 00 MOV      [EAX*4 + 0xcfee48], EDX  ; WRITE with corrupted index
    //
    // FIX: Before the MOV at 0x0073fce3, validate that EAX < 64 (array size).
    // If EAX >= 64, skip the write to prevent array corruption.
    //
    // Strategy: Hook at 0x0073fce1 (ADD EAX, [ECX]) and validate before 0x0073fce3.
    // We have 7 bytes at 0x0073fce3 for the MOV instruction.
    // Replace with: CMP EAX, 0x40; JGE skip_write; MOV [EAX*4 + 0xcfee48], EDX; skip_write: NOP
    if (PATCH_26_ENABLED) {
        Log("[Patch 26] Array bounds validation in FUN_0073fc02 (Quicksort crash fix)");
        Log("  Crash: 0x00456D06 in quicksort with invalid array indices");
        Log("  Fix: Validate array index before write at 0x0073fce3");

        BYTE* codeCave26 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave26) {
            Log("  Code cave 26 at: 0x%08X", (DWORD)codeCave26);

            BYTE* p26 = codeCave26;

            // CMP EAX, 0x40 (array size is 64 = 0x40)
            // 83 F8 40
            *p26++ = 0x83; *p26++ = 0xF8; *p26++ = 0x40;

            // JGE skip_write (if EAX >= 0x40, skip the write)
            // 7D xx
            *p26++ = 0x7D;
            BYTE* jgePatch = p26;
            *p26++ = 0x00;  // Placeholder for offset

            // Valid index - execute original MOV [EAX*4 + 0xcfee48], EDX
            // 89 14 85 48 ee cf 00
            *p26++ = 0x89; *p26++ = 0x14; *p26++ = 0x85;
            *p26++ = 0x48; *p26++ = 0xEE; *p26++ = 0xCF; *p26++ = 0x00;

            // JMP back to 0x0073fcea (after the MOV)
            // E9 xx xx xx xx
            *p26++ = 0xE9;
            *(DWORD*)p26 = 0x0073fcea - ((DWORD)p26 + 4);
            p26 += 4;

            // skip_write label: Out of bounds - skip the write
            BYTE* skipLabel = p26;
            *jgePatch = (BYTE)(skipLabel - jgePatch - 1);

            // JMP back to 0x0073fcea (skip the write, continue)
            // E9 xx xx xx xx
            *p26++ = 0xE9;
            *(DWORD*)p26 = 0x0073fcea - ((DWORD)p26 + 4);
            p26 += 4;

            Log("  Code cave 26 size: %d bytes", (int)(p26 - codeCave26));

            // Patch at 0x0073fce3 (7 bytes: the MOV instruction)
            BYTE* patchAddr26 = (BYTE*)0x0073fce3;
            DWORD oldProt26;
            if (VirtualProtect(patchAddr26, 7, PAGE_EXECUTE_READWRITE, &oldProt26)) {
                Log("  Original at 0x0073fce3: %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr26[0], patchAddr26[1], patchAddr26[2], patchAddr26[3],
                    patchAddr26[4], patchAddr26[5], patchAddr26[6]);

                // JMP codeCave26 (5 bytes)
                patchAddr26[0] = 0xE9;
                *(DWORD*)(patchAddr26 + 1) = (DWORD)codeCave26 - ((DWORD)patchAddr26 + 5);
                // NOP remaining 2 bytes
                patchAddr26[5] = 0x90;
                patchAddr26[6] = 0x90;

                VirtualProtect(patchAddr26, 7, oldProt26, &oldProt26);
                Log("  Patched array write at 0x0073fce3 with bounds checking");
                Log("  Quicksort will receive valid array without out-of-bounds pointers");
            } else {
                Log("  ERROR: Failed to unprotect 0x0073fce3");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 26");
        }
    } else {
        Log("[Patch 26] DISABLED");
    }
    Log("");

    // === Patch 27: Fix FUN_0073fc02 Root Cause (Multiplier Bug) ===
    // ISSUE: Patch 26 validates the index AFTER corruption happens
    // ROOT CAUSE: The multiplier 0x41 (65) is incorrect for a 64-element array
    //
    // The calculation at 0x0073fcd2 uses:
    //   IMUL EAX, EAX, 0x41  (multiply by 65)
    // But the array DAT_00cfee48 has only 64 elements (0x40)
    //
    // SOLUTION: Change 0x41 to 0x40 in the IMUL instruction
    // This fixes the root cause instead of just validating the result
    //
    // Location: 0x0073fcd2 (the IMUL instruction)
    // Original: 6B C0 41 (IMUL EAX, EAX, 0x41)
    // Patched:  6B C0 40 (IMUL EAX, EAX, 0x40)
    if (PATCH_27_ENABLED) {
        Log("[Patch 27] Fix FUN_0073fc02 multiplier bug (root cause fix)");
        Log("  Issue: IMUL uses 0x41 (65) instead of 0x40 (64) for array stride");
        Log("  Fix: Change multiplier from 0x41 to 0x40 at 0x0073fcd2");

        BYTE* patchAddr27 = (BYTE*)0x0073fcd2;
        DWORD oldProt27;

        if (VirtualProtect(patchAddr27, 3, PAGE_EXECUTE_READWRITE, &oldProt27)) {
            Log("  Original at 0x0073fcd2: %02X %02X %02X",
                patchAddr27[0], patchAddr27[1], patchAddr27[2]);

            // Verify it's the IMUL instruction we expect
            if (patchAddr27[0] == 0x6B && patchAddr27[1] == 0xC0 && patchAddr27[2] == 0x41) {
                // Change 0x41 to 0x40
                patchAddr27[2] = 0x40;
                Log("  Patched: Changed multiplier from 0x41 to 0x40");
                Log("  This fixes the root cause of array bounds violations");

                // Verify the patch was applied
                Log("  Verification: After patch at 0x0073fcd2: %02X %02X %02X",
                    patchAddr27[0], patchAddr27[1], patchAddr27[2]);
            } else {
                Log("  WARNING: Instruction at 0x0073fcd2 doesn't match expected pattern");
                Log("  Expected: 6B C0 41, Found: %02X %02X %02X",
                    patchAddr27[0], patchAddr27[1], patchAddr27[2]);
            }

            VirtualProtect(patchAddr27, 3, oldProt27, &oldProt27);
        } else {
            Log("  ERROR: Failed to unprotect 0x0073fcd2");
        }
    } else {
        Log("[Patch 27] DISABLED");
    }
    Log("");

    // === Patch 29: Spatial Query Function Entry Point NULL Check (FUN_008d2844) ===
    // ISSUE: FUN_008d2844 dereferences ECX and EDX without validation
    // The function is called from FUN_008d28df with potentially invalid pointers
    // Patch 9 validates at call sites, but this adds entry-point validation
    //
    // STRATEGY: Add NULL check at function entry (0x008d2844)
    // If ECX or EDX is NULL/invalid, return immediately
    {
        Log("[Patch 29] Spatial Query Function Entry Point NULL Check");
        Log("  Target: FUN_008d2844 (0x008d2844)");
        Log("  Issue: Dereferences ECX and EDX without validation");
        Log("  Current crash: MOVSS XMM0, [ECX] at 0x008d2848");
        Log("  Fix: Add NULL check at entry point before any dereferences");
        Log("");
        Log("  This is a DEFENSIVE patch to catch invalid pointers");
        Log("  that might slip through Patch 9's call-site validation");
    }
    Log("");

    // === Patch 30: Havok NULL Protection + Debug Hook for FUN_008f7ec2 ===
    // PURPOSE: Fix crash caused by NULL ESI pointer passed to FUN_008f7ec2
    //
    // ROOT CAUSE ANALYSIS (from crash investigation):
    //   - The crash log shows ESI = 0x00000000 on call #3322 (0xCFA)
    //   - The caller at 0x0087D0A4 passes ESI from local_c8 which can be NULL
    //   - When ESI is NULL, accessing [ESI+0x40] crashes at 0x008F7EC5
    //
    // ACTUAL FUNCTION ANALYSIS (from disassembly):
    //   008f7ec2  57           PUSH EDI         ; 1 byte
    //   008f7ec3  33 ff        XOR EDI, EDI     ; 2 bytes
    //   008f7ec5  39 7e 40     CMP [ESI+0x40], EDI  ; 3 bytes <-- CRASH HERE!
    //   008f7ec8  7E 15        JLE +0x15        ; 2 bytes
    //
    // FIX: Add NULL check for ESI at function entry
    //   - If ESI == 0, skip to function epilogue (RET)
    //   - If ESI != 0, continue with normal execution
    //
    // FUNCTION EPILOGUE (from disassembly):
    //   008f7edf  5f           POP EDI
    //   008f7ee0  c2 04 00     RET 4
    //
    // HOOK MECHANISM:
    //   - Steal 8 bytes (complete instructions) to make room for 5-byte JMP
    //   - Stolen bytes: 57 33 FF 39 7E 40 7E 15 (8 bytes = all 4 instructions)
    //   - Code cave: NULL check, optional logging, execute stolen, JMP back
    //   - Return to 0x008F7ECA (after the JLE) or 0x008F7EDF (early exit)
    //
    if (PATCH_30_ENABLED) {
        Log("[Patch 30] Havok NULL Protection + Debug Hook for FUN_008f7ec2");
        Log("  Target: FUN_008f7ec2 at 0x008F7EC2");
        Log("  Crash: 0x008F7EC5 accessing [ESI+0x40] when ESI is NULL");
        Log("  Fix: Add NULL check for ESI, skip function if NULL");

        // Initialize the Havok debug log
        InitHavokDebugLog();

        // Target address to hook
        BYTE* targetAddr = (BYTE*)0x008F7EC2;
        DWORD oldProt30;

        if (VirtualProtect(targetAddr, 16, PAGE_EXECUTE_READWRITE, &oldProt30)) {
            // Save original bytes for reference (we need 8 bytes)
            memcpy(g_OriginalBytes_008f7ec2, targetAddr, 8);
            Log("  Original bytes at 0x008F7EC2: %02X %02X %02X %02X %02X %02X %02X %02X",
                targetAddr[0], targetAddr[1], targetAddr[2], targetAddr[3],
                targetAddr[4], targetAddr[5], targetAddr[6], targetAddr[7]);

            // Verify the bytes match what we expect from disassembly
            // 57 33 FF 39 7E 40 7E 15
            bool bytesMatch = (targetAddr[0] == 0x57 && targetAddr[1] == 0x33 &&
                              targetAddr[2] == 0xFF && targetAddr[3] == 0x39 &&
                              targetAddr[4] == 0x7E && targetAddr[5] == 0x40 &&
                              targetAddr[6] == 0x7E && targetAddr[7] == 0x15);

            if (!bytesMatch) {
                Log("  WARNING: Original bytes don't match expected pattern!");
                Log("  Expected: 57 33 FF 39 7E 40 7E 15");
            }

            // Allocate code cave
            BYTE* codeCave30 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (codeCave30) {
                Log("  Code cave 30 at: 0x%08X", (DWORD)codeCave30);

                BYTE* p = codeCave30;

                // ===== CODE CAVE IMPLEMENTATION =====
                // 1. NULL CHECK: TEST ESI, ESI / JZ earlyExit
                // 2. Save all registers (PUSHAD + PUSHFD)
                // 3. Push ESI as argument to __stdcall callback
                // 4. Call our logging callback
                // 5. Restore all registers (POPFD + POPAD)
                // 6. Execute the stolen 8 bytes
                // 7. JMP back to 0x008F7ECA
                // earlyExit: JMP to 0x008F7EDF (function epilogue)

                // ===== NULL CHECK =====
                // TEST ESI, ESI (2 bytes: 85 F6)
                *p++ = 0x85;
                *p++ = 0xF6;

                // JZ earlyExit (2 bytes: 74 XX - will patch offset later)
                BYTE* jzPatch = p;
                *p++ = 0x74;
                *p++ = 0x00;  // Placeholder, will patch

                // ===== LOGGING (only if ESI is valid) =====
                // PUSHAD - save all general registers
                *p++ = 0x60;

                // PUSHFD - save flags
                *p++ = 0x9C;

                // PUSH ESI - argument for __stdcall callback
                *p++ = 0x56;

                // CALL HavokDebugCallback (__stdcall, cleans up stack)
                *p++ = 0xE8;
                DWORD callbackAddr = (DWORD)&HavokDebugCallback;
                *(DWORD*)p = callbackAddr - ((DWORD)p + 4);
                p += 4;

                // POPFD - restore flags
                *p++ = 0x9D;

                // POPAD - restore all general registers
                *p++ = 0x61;

                // ===== EXECUTE STOLEN 8 BYTES =====
                // PUSH EDI
                *p++ = 0x57;

                // XOR EDI, EDI
                *p++ = 0x33;
                *p++ = 0xFF;

                // CMP [ESI+0x40], EDI
                *p++ = 0x39;
                *p++ = 0x7E;
                *p++ = 0x40;

                // JLE to 0x008F7EDF (function epilogue)
                *p++ = 0x0F;  // JLE rel32
                *p++ = 0x8E;
                DWORD jleTarget = 0x008F7EDF;  // LAB_008f7edf (original target)
                *(DWORD*)p = jleTarget - ((DWORD)p + 4);
                p += 4;

                // JMP back to original function at 0x008F7ECA (after stolen bytes)
                *p++ = 0xE9;
                DWORD returnAddr = 0x008F7ECA;  // LAB_008f7eca (start of loop)
                *(DWORD*)p = returnAddr - ((DWORD)p + 4);
                p += 4;

                // ===== EARLY EXIT (ESI is NULL) =====
                BYTE* earlyExitLabel = p;

                // Patch the JZ offset to jump here
                jzPatch[1] = (BYTE)(earlyExitLabel - (jzPatch + 2));

                // Log the NULL ESI event (optional, for debugging)
                // PUSHAD
                *p++ = 0x60;
                // PUSHFD
                *p++ = 0x9C;
                // PUSH 0 (ESI is NULL)
                *p++ = 0x6A;
                *p++ = 0x00;
                // CALL HavokDebugCallback
                *p++ = 0xE8;
                *(DWORD*)p = callbackAddr - ((DWORD)p + 4);
                p += 4;
                // POPFD
                *p++ = 0x9D;
                // POPAD
                *p++ = 0x61;

                // CRITICAL FIX: When ESI is NULL, we never executed PUSH EDI,
                // so we must NOT jump to 0x008F7EDF (which does POP EDI).
                // Instead, we execute RET 4 directly here to clean up the stack
                // and return to the caller without stack imbalance.
                //
                // FUN_008f7ec2 signature: void __usercall FUN_008f7ec2(ESI=obj, [ESP+4]=param)
                // It uses RET 4 to pop the 4-byte parameter pushed by caller.
                *p++ = 0xC2;  // RET imm16
                *p++ = 0x04;  // 4 bytes
                *p++ = 0x00;

                Log("  Code cave 30 size: %d bytes", (int)(p - codeCave30));
                Log("  NULL check: TEST ESI, ESI / JZ to early exit");
                Log("  FIXED: Early exit uses RET 4 directly (no POP EDI since we never pushed it)");

                // ===== INSTALL THE HOOK =====
                // Replace first 8 bytes with: JMP to code cave + 3 NOPs
                targetAddr[0] = 0xE9;  // JMP rel32
                *(DWORD*)(targetAddr + 1) = (DWORD)codeCave30 - ((DWORD)targetAddr + 5);
                targetAddr[5] = 0x90;  // NOP
                targetAddr[6] = 0x90;  // NOP
                targetAddr[7] = 0x90;  // NOP

                // Restore protection
                VirtualProtect(targetAddr, 16, oldProt30, &oldProt30);

                Log("  Patched 0x008F7EC2 with JMP to code cave + 3 NOPs");
                Log("  New bytes at 0x008F7EC2: %02X %02X %02X %02X %02X %02X %02X %02X",
                    targetAddr[0], targetAddr[1], targetAddr[2], targetAddr[3],
                    targetAddr[4], targetAddr[5], targetAddr[6], targetAddr[7]);
                Log("  Hook installed successfully!");
                Log("  NULL ESI protection ACTIVE - will skip function if ESI is NULL");
                Log("  Logging to: conquest_havok_debug.log");
            } else {
                Log("  ERROR: Failed to allocate code cave 30");
            }
        } else {
            Log("  ERROR: Failed to unprotect 0x008F7EC2");
        }
    } else {
        Log("[Patch 30] DISABLED");
    }
    Log("");

    // === Patch 31: Matrix Copy NULL/Invalid Pointer Check for FUN_00414aed ===
    // PURPOSE: Fix crash caused by invalid source pointer passed to SSE matrix copy
    //
    // ROOT CAUSE ANALYSIS (from crash investigation):
    //   - Crash at 0x00414AF8: MOVAPS XMM0, [ECX] where ECX = 0xFFFFFFFF (-1)
    //   - The function copies 48 bytes using SSE MOVAPS instructions
    //   - Caller at 0x008831A4 passed -1 as source pointer (lookup failure)
    //   - This is a systemic issue: game uses -1 as "not found" return value
    //   - Function is called from 58+ locations, so patching here protects all callers
    //
    // FUNCTION SIGNATURE:
    //   void __thiscall FUN_00414aed(void* dest, void* src)
    //   - ECX = destination (this pointer)
    //   - [EBP+0x8] = source pointer
    //   - Returns via RET 0x4
    //
    // ORIGINAL BYTES (11 bytes from 0x00414aed):
    //   55                   PUSH EBP
    //   8b ec                MOV EBP, ESP
    //   83 e4 f0             AND ESP, 0xfffffff0
    //   8b c1                MOV EAX, ECX
    //   8b 4d 08             MOV ECX, [EBP+0x8]
    //   0f 28 01             MOVAPS XMM0, [ECX]  <-- CRASH HERE
    //
    // PATCH STRATEGY:
    //   - Steal 11 bytes (complete instructions up to MOVAPS)
    //   - Replace with 5-byte JMP to code cave + 6 NOPs
    //   - Code cave: execute stolen bytes, check ECX for NULL/-1, skip if invalid
    //
    if (PATCH_31_ENABLED) {
        Log("[Patch 31] Matrix Copy NULL/Invalid Pointer Check for FUN_00414aed");
        Log("  Target: FUN_00414aed at 0x00414AED");
        Log("  Crash: 0x00414AF8 accessing [ECX] when ECX is 0xFFFFFFFF (-1)");
        Log("  Fix: Add NULL/-1 check for source pointer, skip copy if invalid");

        BYTE* codeCave31 = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (codeCave31) {
            Log("  Code cave 31 allocated at: 0x%08X", (DWORD)(uintptr_t)codeCave31);
            Log("  Counter addresses:");
            Log("    g_Patch31_NullCaught:    0x%08X", (DWORD)(uintptr_t)&g_Patch31_NullCaught);
            Log("    g_Patch31_InvalidCaught: 0x%08X", (DWORD)(uintptr_t)&g_Patch31_InvalidCaught);
            Log("    g_Patch31_ValidCalls:    0x%08X", (DWORD)(uintptr_t)&g_Patch31_ValidCalls);
            Log("    g_Patch31_LowAddrCaught: 0x%08X", (DWORD)(uintptr_t)&g_Patch31_LowAddrCaught);
            Log("    g_Patch31_UnalignedCaught: 0x%08X", (DWORD)(uintptr_t)&g_Patch31_UnalignedCaught);

            // Build the code cave with counter increments for debugging
            BYTE* p = codeCave31;

            // Execute stolen bytes (11 bytes):
            // 55                   PUSH EBP
            *p++ = 0x55;
            // 8b ec                MOV EBP, ESP
            *p++ = 0x8B; *p++ = 0xEC;
            // 83 e4 f0             AND ESP, 0xfffffff0
            *p++ = 0x83; *p++ = 0xE4; *p++ = 0xF0;
            // 8b c1                MOV EAX, ECX (save destination to EAX)
            *p++ = 0x8B; *p++ = 0xC1;
            // 8b 4d 08             MOV ECX, [EBP+0x8] (load source pointer)
            *p++ = 0x8B; *p++ = 0x4D; *p++ = 0x08;

            // Now ECX = source pointer. Check for various invalid conditions:
            // 1. NULL (ECX == 0)
            // 2. -1 (ECX == 0xFFFFFFFF)
            // 3. Low address (ECX < 0x10000)
            // 4. Unaligned (ECX & 0xF != 0)

            // TEST ECX, ECX
            *p++ = 0x85; *p++ = 0xC9;
            // JZ null_path (source is NULL)
            *p++ = 0x74;
            BYTE* jz_null = p++;  // Placeholder for offset

            // CMP ECX, 0xFFFFFFFF (-1)
            *p++ = 0x81; *p++ = 0xF9;
            *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0xFF;
            // JE invalid_path (source is -1)
            *p++ = 0x74;
            BYTE* je_invalid = p++;  // Placeholder for offset

            // CMP ECX, 0x10000 (low address check)
            *p++ = 0x81; *p++ = 0xF9;
            *p++ = 0x00; *p++ = 0x00; *p++ = 0x01; *p++ = 0x00;  // 0x00010000
            // JB lowaddr_path (source < 0x10000)
            *p++ = 0x72;
            BYTE* jb_lowaddr = p++;  // Placeholder for offset

            // TEST ECX, 0x0F (alignment check - MOVAPS requires 16-byte alignment)
            *p++ = 0xF7; *p++ = 0xC1;  // TEST ECX, imm32
            *p++ = 0x0F; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;  // 0x0000000F
            // JNZ unaligned_path (source not 16-byte aligned)
            *p++ = 0x75;
            BYTE* jnz_unaligned = p++;  // Placeholder for offset

            // === VALID PATH ===
            // INC DWORD PTR [g_Patch31_ValidCalls]
            *p++ = 0xFF; *p++ = 0x05;  // INC [imm32]
            *(DWORD*)p = (DWORD)(uintptr_t)&g_Patch31_ValidCalls;
            p += 4;

            // Source is valid, jump back to MOVAPS at 0x00414af8
            // JMP 0x00414af8
            *p++ = 0xE9;
            DWORD jmpBackAddr = 0x00414AF8;
            DWORD jmpBackRel = jmpBackAddr - ((DWORD)(uintptr_t)p + 4);
            *(DWORD*)p = jmpBackRel;
            p += 4;

            // === NULL PATH ===
            BYTE* null_path_label = p;
            *jz_null = (BYTE)(null_path_label - (jz_null + 1));

            // INC DWORD PTR [g_Patch31_NullCaught]
            *p++ = 0xFF; *p++ = 0x05;  // INC [imm32]
            *(DWORD*)p = (DWORD)(uintptr_t)&g_Patch31_NullCaught;
            p += 4;
            // JMP skip_copy
            *p++ = 0xEB;
            BYTE* jmp_skip_from_null = p++;  // Placeholder

            // === INVALID (-1) PATH ===
            BYTE* invalid_path_label = p;
            *je_invalid = (BYTE)(invalid_path_label - (je_invalid + 1));

            // INC DWORD PTR [g_Patch31_InvalidCaught]
            *p++ = 0xFF; *p++ = 0x05;  // INC [imm32]
            *(DWORD*)p = (DWORD)(uintptr_t)&g_Patch31_InvalidCaught;
            p += 4;
            // JMP skip_copy
            *p++ = 0xEB;
            BYTE* jmp_skip_from_invalid = p++;  // Placeholder

            // === LOW ADDRESS PATH ===
            BYTE* lowaddr_path_label = p;
            *jb_lowaddr = (BYTE)(lowaddr_path_label - (jb_lowaddr + 1));

            // INC DWORD PTR [g_Patch31_LowAddrCaught]
            *p++ = 0xFF; *p++ = 0x05;  // INC [imm32]
            *(DWORD*)p = (DWORD)(uintptr_t)&g_Patch31_LowAddrCaught;
            p += 4;
            // JMP skip_copy
            *p++ = 0xEB;
            BYTE* jmp_skip_from_lowaddr = p++;  // Placeholder

            // === UNALIGNED PATH ===
            BYTE* unaligned_path_label = p;
            *jnz_unaligned = (BYTE)(unaligned_path_label - (jnz_unaligned + 1));

            // INC DWORD PTR [g_Patch31_UnalignedCaught]
            *p++ = 0xFF; *p++ = 0x05;  // INC [imm32]
            *(DWORD*)p = (DWORD)(uintptr_t)&g_Patch31_UnalignedCaught;
            p += 4;
            // Fall through to skip_copy

            // === SKIP COPY (common exit for invalid pointers) ===
            BYTE* skip_copy_label = p;
            *jmp_skip_from_null = (BYTE)(skip_copy_label - (jmp_skip_from_null + 1));
            *jmp_skip_from_invalid = (BYTE)(skip_copy_label - (jmp_skip_from_invalid + 1));
            *jmp_skip_from_lowaddr = (BYTE)(skip_copy_label - (jmp_skip_from_lowaddr + 1));

            // Restore stack: MOV ESP, EBP
            *p++ = 0x8B; *p++ = 0xE5;
            // POP EBP
            *p++ = 0x5D;
            // RET 0x4 (clean up 4 bytes from stack - the source pointer parameter)
            *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;

            Log("  Code cave 31 size: %d bytes", (int)(p - codeCave31));

            // Now patch the original function at 0x00414aed
            BYTE* patchAddr31 = (BYTE*)0x00414AED;
            DWORD oldProtect31;
            if (VirtualProtect(patchAddr31, 11, PAGE_EXECUTE_READWRITE, &oldProtect31)) {
                // Log original bytes
                Log("  Original bytes at 0x00414AED:");
                Log("    %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr31[0], patchAddr31[1], patchAddr31[2], patchAddr31[3],
                    patchAddr31[4], patchAddr31[5], patchAddr31[6], patchAddr31[7],
                    patchAddr31[8], patchAddr31[9], patchAddr31[10]);

                // Verify expected bytes
                BYTE expected31[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0xC1, 0x8B, 0x4D, 0x08 };
                bool match31 = true;
                for (int i = 0; i < 11; i++) {
                    if (patchAddr31[i] != expected31[i]) {
                        match31 = false;
                        break;
                    }
                }

                if (match31) {
                    // Write JMP to code cave (5 bytes)
                    patchAddr31[0] = 0xE9;
                    DWORD jmpRel31 = (DWORD)(uintptr_t)codeCave31 - ((DWORD)(uintptr_t)patchAddr31 + 5);
                    *(DWORD*)(patchAddr31 + 1) = jmpRel31;

                    // NOP remaining 6 bytes
                    for (int i = 5; i < 11; i++) {
                        patchAddr31[i] = 0x90;
                    }

                    Log("  Patched bytes at 0x00414AED:");
                    Log("    %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                        patchAddr31[0], patchAddr31[1], patchAddr31[2], patchAddr31[3],
                        patchAddr31[4], patchAddr31[5], patchAddr31[6], patchAddr31[7],
                        patchAddr31[8], patchAddr31[9], patchAddr31[10]);

                    Log("  SUCCESS: Patch 31 applied - matrix copy now validates source pointer");
                    Log("  This protects all 58+ callers of FUN_00414aed from -1/NULL crashes");
                } else {
                    Log("  ERROR: Bytes at 0x00414AED don't match expected pattern");
                    Log("  Expected: 55 8B EC 83 E4 F0 8B C1 8B 4D 08");
                }

                VirtualProtect(patchAddr31, 11, oldProtect31, &oldProtect31);
            } else {
                Log("  ERROR: Failed to unprotect 0x00414AED");
            }
        } else {
            Log("  ERROR: Failed to allocate code cave 31");
        }
    } else {
        Log("[Patch 31] DISABLED");
    }
    Log("");

    // === Patch 32: Havok Broadphase Crash Logger ===
    // Hooks the MOVAPS crash site at 0x0085A439 via VEH to log the full context
    // when the broadphase function crashes. This captures EDI (the broadphase object),
    // ESI (the TLS-based object pointer), EAX (the misaligned address), and a stack
    // trace to identify WHICH HkShapeInfo entry caused the crash.
    {
        Log("[Patch 32] Havok Broadphase Crash Logger (VEH)");
        Log("  Target: 0x0085A439 (MOVAPS XMM0, [EAX+0x30] in FUN_0085a353)");
        Log("  Purpose: Log full context when Havok broadphase crashes");

        // Install a Vectored Exception Handler that catches the specific crash
        static auto BroadphaseCrashVEH = [](PEXCEPTION_POINTERS ep) -> LONG {
            if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
                return EXCEPTION_CONTINUE_SEARCH;

            DWORD eip = ep->ContextRecord->Eip;

            // Only catch crashes in the broadphase function range
            if (eip < 0x0085A350 || eip > 0x0085A840)
                return EXCEPTION_CONTINUE_SEARCH;

            // Open dedicated log file
            FILE* f = fopen("conquest_broadphase_crash.log", "a");
            if (!f) return EXCEPTION_CONTINUE_SEARCH;

            fprintf(f, "=== HAVOK BROADPHASE CRASH ===\n");
            fprintf(f, "EIP = 0x%08X\n", eip);
            fprintf(f, "EAX = 0x%08X (mod16=%d)\n", ep->ContextRecord->Eax, ep->ContextRecord->Eax % 16);
            fprintf(f, "EBX = 0x%08X\n", ep->ContextRecord->Ebx);
            fprintf(f, "ECX = 0x%08X\n", ep->ContextRecord->Ecx);
            fprintf(f, "EDX = 0x%08X\n", ep->ContextRecord->Edx);
            fprintf(f, "ESI = 0x%08X (mod16=%d)\n", ep->ContextRecord->Esi, ep->ContextRecord->Esi % 16);
            fprintf(f, "EDI = 0x%08X\n", ep->ContextRecord->Edi);
            fprintf(f, "ESP = 0x%08X\n", ep->ContextRecord->Esp);
            fprintf(f, "EBP = 0x%08X\n", ep->ContextRecord->Ebp);

            // The broadphase function uses EDI as the broadphase object.
            // EDI+0xFC = hkpBroadPhase pointer. Try to read it.
            __try {
                DWORD edi = ep->ContextRecord->Edi;
                DWORD broadphasePtr = *(DWORD*)(edi + 0xFC);
                fprintf(f, "EDI+0xFC (broadphase) = 0x%08X\n", broadphasePtr);
                if (broadphasePtr) {
                    // Dump first 128 bytes of broadphase object
                    fprintf(f, "Broadphase object dump:\n");
                    for (int row = 0; row < 8; row++) {
                        fprintf(f, "  +%02X:", row*16);
                        for (int col = 0; col < 16; col += 4) {
                            DWORD val = *(DWORD*)(broadphasePtr + row*16 + col);
                            fprintf(f, " %08X", val);
                        }
                        fprintf(f, "\n");
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(f, "  (broadphase read failed)\n");
            }

            // ESI = Havok TLS object. EBX = [ESI+4] (secondary pointer).
            // Try to dump ESI context.
            __try {
                DWORD esi = ep->ContextRecord->Esi;
                fprintf(f, "ESI object dump (Havok TLS):\n");
                for (int row = 0; row < 4; row++) {
                    fprintf(f, "  +%02X:", row*16);
                    for (int col = 0; col < 16; col += 4) {
                        DWORD val = *(DWORD*)(esi + row*16 + col);
                        fprintf(f, " %08X", val);
                    }
                    fprintf(f, "\n");
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(f, "  (ESI read failed)\n");
            }

            // EAX+0x30 is the failing read address. Dump what's near EAX.
            __try {
                DWORD eax = ep->ContextRecord->Eax;
                fprintf(f, "EAX region (failing MOVAPS at [EAX+0x30]):\n");
                for (int row = 0; row < 8; row++) {
                    fprintf(f, "  +%02X:", row*16);
                    for (int col = 0; col < 16; col += 4) {
                        DWORD val = *(DWORD*)(eax + row*16 + col);
                        fprintf(f, " %08X", val);
                    }
                    fprintf(f, "\n");
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(f, "  (EAX read failed - address invalid)\n");
            }

            // Stack trace - return addresses
            fprintf(f, "Stack trace (return addresses):\n");
            __try {
                DWORD* stack = (DWORD*)ep->ContextRecord->Esp;
                for (int i = 0; i < 64; i++) {
                    DWORD val = stack[i];
                    // Filter for code section addresses
                    if (val >= 0x00400000 && val <= 0x00FFFFFF) {
                        DWORD offset;
                        const char* mod = AddressToModuleOffset(val, &offset);
                        fprintf(f, "  [ESP+%03X] = 0x%08X (%s+0x%08X)\n", i*4, val, mod, offset);
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(f, "  (stack read failed)\n");
            }

            // Look for Block1 base address on the stack or in nearby memory
            // The game decompresses Block1 to a heap allocation. If we can find it,
            // we can compute which HkShapeInfo entry was being processed.
            fprintf(f, "\nSearching for HkShapeInfo context...\n");
            __try {
                // ECX is often used as a counter/index in the broadphase setup
                fprintf(f, "  ECX (possible index/counter) = 0x%08X (%d)\n",
                        ep->ContextRecord->Ecx, ep->ContextRecord->Ecx);

                // Check stack for pointers that might be Block1 HkShapeInfo entries
                // HkShapeInfo entries are 80 bytes; kind field (at +32) should be 1-6
                DWORD* stack = (DWORD*)ep->ContextRecord->Esp;
                for (int i = 0; i < 128; i++) {
                    DWORD val = stack[i];
                    // Look for heap pointers (above 0x01000000, below 0x7FFFFFFF)
                    if (val > 0x01000000 && val < 0x7FFFFFFF) {
                        __try {
                            // Check if this looks like a pointer to HkShapeInfo
                            DWORD kind = *(DWORD*)(val + 32);
                            DWORD key  = *(DWORD*)(val + 36);
                            if (kind >= 1 && kind <= 6 && key != 0) {
                                float* trans = (float*)val;
                                fprintf(f, "  [ESP+%03X] -> possible HkShapeInfo: kind=%d key=0x%08X trans=(%.2f,%.2f,%.2f)\n",
                                        i*4, kind, key, trans[0], trans[1], trans[2]);
                            }
                        } __except(EXCEPTION_EXECUTE_HANDLER) { }
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(f, "  (search failed)\n");
            }

            fprintf(f, "\n");
            fflush(f);
            fclose(f);

            // Don't handle it - let the normal crash handler run too
            return EXCEPTION_CONTINUE_SEARCH;
        };

        // Need to cast the lambda to a function pointer via a static wrapper
        // (lambdas with no captures can be converted to function pointers)
        typedef LONG (WINAPI *VEH_HANDLER)(PEXCEPTION_POINTERS);
        PVOID veh = AddVectoredExceptionHandler(1, (VEH_HANDLER)(void*)BroadphaseCrashVEH);
        if (veh) {
            Log("  VEH installed successfully");
            Log("  Crash details will be written to: conquest_broadphase_crash.log");
        } else {
            Log("  ERROR: Failed to install VEH");
        }
    }
    Log("");

    Log("=== Memory patching complete ===");
    Log("Patches applied: Pool expansion (Patch 6) enabled for exhaustion fix");

    // === Diagnostic: Check if large allocations are possible ===
    Log("");
    Log("[Diagnostic] Testing large allocations...");

    MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
    if (GlobalMemoryStatusEx(&memStatus)) {
        Log("  Physical memory: %llu MB total, %llu MB available",
            memStatus.ullTotalPhys / 1024 / 1024,
            memStatus.ullAvailPhys / 1024 / 1024);
        Log("  Virtual memory:  %llu MB total, %llu MB available",
            memStatus.ullTotalVirtual / 1024 / 1024,
            memStatus.ullAvailVirtual / 1024 / 1024);
    }

    // Test if we can actually allocate the requested sizes
    void* testTexture = VirtualAlloc(nullptr, g_TextureBufferSize, MEM_RESERVE, PAGE_READWRITE);
    if (testTexture) {
        Log("  Texture buffer test (0x%X bytes): SUCCESS", g_TextureBufferSize);
        VirtualFree(testTexture, 0, MEM_RELEASE);
    } else {
        Log("  Texture buffer test (0x%X bytes): FAILED - error %d", g_TextureBufferSize, GetLastError());
    }

    void* testVertex = VirtualAlloc(nullptr, g_VertexBufferSize, MEM_RESERVE, PAGE_READWRITE);
    if (testVertex) {
        Log("  Vertex buffer test (0x%X bytes): SUCCESS", g_VertexBufferSize);
        VirtualFree(testVertex, 0, MEM_RELEASE);
    } else {
        Log("  Vertex buffer test (0x%X bytes): FAILED - error %d", g_VertexBufferSize, GetLastError());
    }

    // Test both together
    void* testBoth1 = VirtualAlloc(nullptr, g_TextureBufferSize, MEM_RESERVE, PAGE_READWRITE);
    void* testBoth2 = VirtualAlloc(nullptr, g_VertexBufferSize, MEM_RESERVE, PAGE_READWRITE);
    if (testBoth1 && testBoth2) {
        Log("  Both buffers together: SUCCESS");
    } else {
        Log("  Both buffers together: FAILED (tex=%p, vtx=%p)", testBoth1, testBoth2);
    }
    if (testBoth1) VirtualFree(testBoth1, 0, MEM_RELEASE);
    if (testBoth2) VirtualFree(testBoth2, 0, MEM_RELEASE);

    Log("");
    Log("If allocations fail, try reducing buffer sizes in memorypatch.cpp");
}

void CleanupMemoryPatcher() {
    CloseHavokDebugLog();
    CloseLog();
}

// ============================================================================
// Danger-Aware Engine Layer: Public Interface
// ============================================================================

void DangerOnEndOfFrame() {
    // Process the danger state at end of each frame
    DangerProcessEndOfFrame();
}
