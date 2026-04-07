// ============================================================================
// Crash Handler - Exception handling and minidump generation
// ============================================================================

#include "../include/debugger.h"
#include "../include/memory_watcher.h"
#include <cstdio>
#include <DbgHelp.h>

#pragma comment(lib, "dbghelp.lib")

namespace ConquestDebugger {

static LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

static const char* GetExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        default:                                 return "UNKNOWN";
    }
}

DEBUGGER_API void GenerateMinidump(EXCEPTION_POINTERS* exceptionInfo) {
    // Generate timestamped filename
    SYSTEMTIME st;
    GetLocalTime(&st);
    char filename[256];
    snprintf(filename, sizeof(filename), "conquest_crash_%04d%02d%02d_%02d%02d%02d.dmp",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE file = CreateFileA(filename, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = exceptionInfo;
        mdei.ClientPointers = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
            MiniDumpWithFullMemory, &mdei, nullptr, nullptr);

        CloseHandle(file);
        Log(LogLevel::Info, "Minidump saved: %s", filename);
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exceptionInfo) {
    PEXCEPTION_RECORD er = exceptionInfo->ExceptionRecord;
    PCONTEXT ctx = exceptionInfo->ContextRecord;

    Log(LogLevel::Critical, "");
    Log(LogLevel::Critical, "╔══════════════════════════════════════════════════════════╗");
    Log(LogLevel::Critical, "║                    CRASH DETECTED                        ║");
    Log(LogLevel::Critical, "╚══════════════════════════════════════════════════════════╝");
    Log(LogLevel::Critical, "");
    Log(LogLevel::Critical, "Exception: %s (0x%08X)", GetExceptionName(er->ExceptionCode), er->ExceptionCode);
    Log(LogLevel::Critical, "Address:   0x%08X", er->ExceptionAddress);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = (er->ExceptionInformation[0] == 0) ? "reading" : "writing";
        Log(LogLevel::Critical, "Fault:     %s address 0x%08X", op, er->ExceptionInformation[1]);
    }

    Log(LogLevel::Critical, "");
    Log(LogLevel::Critical, "=== REGISTERS ===");
    Log(LogLevel::Critical, "EIP = 0x%08X  EFLAGS = 0x%08X", ctx->Eip, ctx->EFlags);
    Log(LogLevel::Critical, "EAX = 0x%08X  EBX = 0x%08X", ctx->Eax, ctx->Ebx);
    Log(LogLevel::Critical, "ECX = 0x%08X  EDX = 0x%08X", ctx->Ecx, ctx->Edx);
    Log(LogLevel::Critical, "ESI = 0x%08X  EDI = 0x%08X", ctx->Esi, ctx->Edi);
    Log(LogLevel::Critical, "ESP = 0x%08X  EBP = 0x%08X", ctx->Esp, ctx->Ebp);

    // Dump pool states
    auto pools = MemoryWatcher::Instance().GetPoolState();
    Log(LogLevel::Critical, "");
    Log(LogLevel::Critical, "=== MEMORY POOLS ===");
    Log(LogLevel::Critical, "Event Pool: %d/%d", pools.eventCount, pools.eventLimit);
    Log(LogLevel::Critical, "Pool2:      %d/%d", pools.pool2Count, pools.pool2Limit);

    // Stack dump
    Log(LogLevel::Critical, "");
    Log(LogLevel::Critical, "=== STACK (potential return addresses) ===");
    __try {
        DWORD* stack = (DWORD*)ctx->Esp;
        for (int i = 0; i < 32; i++) {
            DWORD val = stack[i];
            if (val >= 0x00400000 && val <= 0x00FFFFFF) {
                Log(LogLevel::Critical, "  [ESP+%02X] = 0x%08X", i * 4, val);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(LogLevel::Critical, "  (stack read failed)");
    }

    // === Havok Broadphase Crash Context ===
    // If crash is in the broadphase function (0x0085A350..0x0085A840), log extra context
    if (ctx->Eip >= 0x0085A350 && ctx->Eip <= 0x0085A840) {
        Log(LogLevel::Critical, "");
        Log(LogLevel::Critical, "=== HAVOK BROADPHASE CONTEXT ===");
        Log(LogLevel::Critical, "EAX mod16=%d  ESI mod16=%d", ctx->Eax % 16, ctx->Esi % 16);

        // EDI+0xFC = broadphase pointer
        __try {
            DWORD bp = *(DWORD*)(ctx->Edi + 0xFC);
            Log(LogLevel::Critical, "EDI+0xFC (broadphase) = 0x%08X", bp);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log(LogLevel::Critical, "EDI+0xFC read failed");
        }

        // Dump ESI object (Havok TLS)
        __try {
            DWORD esi = ctx->Esi;
            Log(LogLevel::Critical, "ESI (Havok TLS) dump:");
            for (int r = 0; r < 4; r++) {
                DWORD a = *(DWORD*)(esi+r*16+0), b = *(DWORD*)(esi+r*16+4);
                DWORD c = *(DWORD*)(esi+r*16+8), d = *(DWORD*)(esi+r*16+12);
                Log(LogLevel::Critical, "  +%02X: %08X %08X %08X %08X", r*16, a, b, c, d);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log(LogLevel::Critical, "  (ESI read failed)");
        }

        // Dump EAX region (failing MOVAPS at [EAX+0x30])
        __try {
            DWORD eax = ctx->Eax;
            Log(LogLevel::Critical, "EAX region ([EAX+0x30] = MOVAPS target):");
            for (int r = 0; r < 4; r++) {
                DWORD a = *(DWORD*)(eax+r*16+0), b = *(DWORD*)(eax+r*16+4);
                DWORD c = *(DWORD*)(eax+r*16+8), d = *(DWORD*)(eax+r*16+12);
                Log(LogLevel::Critical, "  +%02X: %08X %08X %08X %08X", r*16, a, b, c, d);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log(LogLevel::Critical, "  (EAX read failed - address invalid)");
        }

        // Search stack for HkShapeInfo-like structures
        Log(LogLevel::Critical, "Searching stack for HkShapeInfo pointers...");
        __try {
            DWORD* stack = (DWORD*)ctx->Esp;
            int found = 0;
            for (int i = 0; i < 128 && found < 5; i++) {
                DWORD val = stack[i];
                if (val > 0x01000000 && val < 0x7FFFFFFF) {
                    __try {
                        DWORD kind = *(DWORD*)(val + 32);
                        DWORD key  = *(DWORD*)(val + 36);
                        if (kind >= 1 && kind <= 6 && key != 0) {
                            float* t = (float*)val;
                            Log(LogLevel::Critical, "  [ESP+%03X] -> kind=%d key=0x%08X pos=(%.1f,%.1f,%.1f)",
                                i*4, kind, key, t[0], t[1], t[2]);
                            found++;
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) { }
                }
            }
            if (!found) Log(LogLevel::Critical, "  (none found)");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log(LogLevel::Critical, "  (search failed)");
        }
    }

    // Generate minidump
    GenerateMinidump(exceptionInfo);

    Log(LogLevel::Critical, "");
    Log(LogLevel::Critical, "Press any key to exit...");

    // Keep console open
    Sleep(60000);

    return EXCEPTION_CONTINUE_SEARCH;
}

DEBUGGER_API void InstallCrashHandler() {
    g_previousFilter = SetUnhandledExceptionFilter(CrashFilter);
    Log(LogLevel::Debug, "Crash handler installed");
}

} // namespace ConquestDebugger

