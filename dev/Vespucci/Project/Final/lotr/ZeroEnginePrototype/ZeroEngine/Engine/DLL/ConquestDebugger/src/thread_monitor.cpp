// ============================================================================
// Thread Monitor - Freeze detection and EIP capture
// ============================================================================

#include "../include/thread_monitor.h"
#include "../include/debugger.h"
#include "../include/memory_watcher.h"
#include <cstdio>

namespace ConquestDebugger {

ThreadMonitor& ThreadMonitor::Instance() {
    static ThreadMonitor instance;
    return instance;
}

ThreadMonitor::ThreadMonitor()
    : m_running(false)
    , m_monitorThread(nullptr)
    , m_mainThreadId(0)
    , m_mainThreadHandle(nullptr)
    , m_freezeTimeout(2000)
    , m_lastEIP(0)
    , m_eipStuckCount(0)
    , m_freezeCallback(nullptr)
    , m_frameCounterPtr(nullptr)
    , m_lastFrameCount(0)
    , m_frameStuckTicks(0)
{
}

ThreadMonitor::~ThreadMonitor() {
    Stop();
}

void ThreadMonitor::SetMainThread(DWORD threadId) {
    m_mainThreadId = threadId;
    if (m_mainThreadHandle) {
        CloseHandle(m_mainThreadHandle);
    }
    m_mainThreadHandle = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE, threadId);
}

bool ThreadMonitor::Start() {
    if (m_running) return true;
    if (!m_mainThreadHandle) {
        Log(LogLevel::Error, "ThreadMonitor: No main thread handle set");
        return false;
    }

    m_running = true;
    m_monitorThread = CreateThread(nullptr, 0, MonitorThreadProc, this, 0, nullptr);
    return m_monitorThread != nullptr;
}

void ThreadMonitor::Stop() {
    if (!m_running) return;
    m_running = false;

    if (m_monitorThread) {
        WaitForSingleObject(m_monitorThread, 2000);
        CloseHandle(m_monitorThread);
        m_monitorThread = nullptr;
    }
    if (m_mainThreadHandle) {
        CloseHandle(m_mainThreadHandle);
        m_mainThreadHandle = nullptr;
    }
}

DWORD WINAPI ThreadMonitor::MonitorThreadProc(LPVOID param) {
    ThreadMonitor* self = static_cast<ThreadMonitor*>(param);
    self->MonitorLoop();
    return 0;
}

bool ThreadMonitor::CaptureState(DWORD threadId, ThreadState* state) {
    HANDLE thread = (threadId == m_mainThreadId) ? m_mainThreadHandle :
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, threadId);

    if (!thread) return false;

    DWORD suspendCount = SuspendThread(thread);
    if (suspendCount == (DWORD)-1) {
        if (thread != m_mainThreadHandle) CloseHandle(thread);
        return false;
    }

    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_FULL;
    bool success = GetThreadContext(thread, &ctx) != 0;

    if (success) {
        state->threadId = threadId;
        state->handle = thread;
        state->eip = ctx.Eip;
        state->esp = ctx.Esp;
        state->ebp = ctx.Ebp;
        state->eax = ctx.Eax;
        state->ebx = ctx.Ebx;
        state->ecx = ctx.Ecx;
        state->edx = ctx.Edx;
        state->esi = ctx.Esi;
        state->edi = ctx.Edi;
        state->suspended = true;
        state->lastActiveTime = GetTickCount();
    }

    ResumeThread(thread);
    if (thread != m_mainThreadHandle) CloseHandle(thread);
    return success;
}

void ThreadMonitor::SampleEIP(DWORD threadId, DWORD* samples, int count, DWORD intervalMs) {
    HANDLE thread = (threadId == m_mainThreadId) ? m_mainThreadHandle :
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, threadId);
    if (!thread) return;

    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL;

    for (int i = 0; i < count; i++) {
        SuspendThread(thread);
        if (GetThreadContext(thread, &ctx)) {
            samples[i] = ctx.Eip;
        } else {
            samples[i] = 0;
        }
        ResumeThread(thread);
        if (i < count - 1) Sleep(intervalMs);
    }

    if (thread != m_mainThreadHandle) CloseHandle(thread);
}

void ThreadMonitor::GetStackTrace(DWORD threadId, char* buffer, size_t bufferSize) {
    ThreadState state;
    if (!CaptureState(threadId, &state)) {
        snprintf(buffer, bufferSize, "(failed to capture thread state)");
        return;
    }

    int offset = snprintf(buffer, bufferSize,
        "EIP=0x%08X ESP=0x%08X EBP=0x%08X\nStack (return addresses):\n",
        state.eip, state.esp, state.ebp);

    // Walk stack for return addresses
    __try {
        DWORD* stackPtr = (DWORD*)state.esp;
        for (int i = 0; i < 32 && offset < (int)bufferSize - 50; i++) {
            DWORD val = stackPtr[i];
            if (val >= 0x00400000 && val <= 0x00FFFFFF) {
                offset += snprintf(buffer + offset, bufferSize - offset,
                    "  [ESP+%02X] = 0x%08X\n", i * 4, val);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        offset += snprintf(buffer + offset, bufferSize - offset, "  (stack read failed)\n");
    }
}

void ThreadMonitor::MonitorLoop() {
    Log(LogLevel::Debug, "ThreadMonitor: Starting monitor loop");

    DWORD tick = 0;
    bool freezeDetected = false;
    DWORD freezeStartTick = 0;

    while (m_running) {
        Sleep(500);  // Check every 500ms
        tick++;

        // Sample current EIP
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;

        SuspendThread(m_mainThreadHandle);
        bool gotContext = GetThreadContext(m_mainThreadHandle, &ctx) != 0;
        ResumeThread(m_mainThreadHandle);

        if (!gotContext) continue;

        DWORD currentEIP = ctx.Eip;

        // Check pool states
        auto pools = MemoryWatcher::Instance().GetPoolState();

        // Detect if EIP is stuck in same region (within 0x1000 bytes = likely same function)
        bool eipStuck = false;
        if (m_lastEIP != 0) {
            DWORD diff = (currentEIP > m_lastEIP) ? (currentEIP - m_lastEIP) : (m_lastEIP - currentEIP);
            if (diff < 0x1000) {
                m_eipStuckCount++;
                eipStuck = (m_eipStuckCount >= 4);  // 2+ seconds in same function
            } else {
                m_eipStuckCount = 0;
            }
        }
        m_lastEIP = currentEIP;

        // Log status every tick
        if (tick % 2 == 0) {  // Every second
            Log(LogLevel::Debug, "[%4d] EIP=0x%08X  Event=%d/%d  Pool2=%d/%d%s",
                tick, currentEIP, pools.eventCount, pools.eventLimit,
                pools.pool2Count, pools.pool2Limit,
                eipStuck ? "  ** STUCK **" : "");
        }

        // Handle freeze detection
        if (eipStuck && !freezeDetected) {
            freezeDetected = true;
            freezeStartTick = tick;

            Log(LogLevel::Critical, "");
            Log(LogLevel::Critical, "╔══════════════════════════════════════════════════════════╗");
            Log(LogLevel::Critical, "║           FREEZE DETECTED - CAPTURING STATE              ║");
            Log(LogLevel::Critical, "╚══════════════════════════════════════════════════════════╝");

            // Capture detailed state
            ThreadState state;
            if (CaptureState(m_mainThreadId, &state)) {
                Log(LogLevel::Critical, "");
                Log(LogLevel::Critical, "=== MAIN THREAD STATE ===");
                Log(LogLevel::Critical, "EIP = 0x%08X  <-- STUCK HERE!", state.eip);
                Log(LogLevel::Critical, "ESP = 0x%08X", state.esp);
                Log(LogLevel::Critical, "EBP = 0x%08X", state.ebp);
                Log(LogLevel::Critical, "EAX = 0x%08X  EBX = 0x%08X", state.eax, state.ebx);
                Log(LogLevel::Critical, "ECX = 0x%08X  EDX = 0x%08X", state.ecx, state.edx);
                Log(LogLevel::Critical, "ESI = 0x%08X  EDI = 0x%08X", state.esi, state.edi);

                // Sample EIP multiple times
                Log(LogLevel::Critical, "");
                Log(LogLevel::Critical, "=== EIP SAMPLING (50ms intervals) ===");
                DWORD samples[10];
                SampleEIP(m_mainThreadId, samples, 10, 50);
                for (int i = 0; i < 10; i++) {
                    Log(LogLevel::Critical, "  Sample %2d: EIP = 0x%08X", i + 1, samples[i]);
                }

                // Stack trace
                Log(LogLevel::Critical, "");
                Log(LogLevel::Critical, "=== STACK TRACE ===");
                char stackBuf[4096];
                GetStackTrace(m_mainThreadId, stackBuf, sizeof(stackBuf));
                Log(LogLevel::Critical, "%s", stackBuf);

                // Pool states
                Log(LogLevel::Critical, "");
                Log(LogLevel::Critical, "=== MEMORY POOLS ===");
                Log(LogLevel::Critical, "Event Pool: %d/%d (Base=0x%08X, FreeHead=0x%08X)",
                    pools.eventCount, pools.eventLimit, pools.eventBase, pools.eventFreeHead);
                Log(LogLevel::Critical, "Pool2:      %d/%d", pools.pool2Count, pools.pool2Limit);

                // Frame limiter timer state (the busy-wait loop uses these)
                // DAT_00e571f0 = low DWORD of last frame time
                // DAT_00e571f4 = high DWORD of last frame time
                Log(LogLevel::Critical, "");
                Log(LogLevel::Critical, "=== FRAME LIMITER TIMER STATE ===");
                DWORD timerLo = MemoryWatcher::ReadSafe(0x00e571f0, 4);
                DWORD timerHi = MemoryWatcher::ReadSafe(0x00e571f4, 4);
                Log(LogLevel::Critical, "Last Frame Time: 0x%08X%08X", timerHi, timerLo);

                // Also read the frequency that was calculated
                DWORD freqLo = MemoryWatcher::ReadSafe(0x00e57248, 4);
                DWORD freqHi = MemoryWatcher::ReadSafe(0x00e5724c, 4);
                Log(LogLevel::Critical, "Timer Frequency: 0x%08X%08X", freqHi, freqLo);

                // Callback
                if (m_freezeCallback) {
                    m_freezeCallback(&state);
                }
            }

            Log(LogLevel::Critical, "");
            Log(LogLevel::Critical, "Look up EIP 0x%08X in decompiled code to find the stuck function.", currentEIP);
            Log(LogLevel::Critical, "");
        }

        // Reset freeze detection if EIP moves significantly
        if (!eipStuck && freezeDetected) {
            Log(LogLevel::Info, "Freeze cleared - thread is progressing again");
            freezeDetected = false;
            m_eipStuckCount = 0;
        }
    }

    Log(LogLevel::Debug, "ThreadMonitor: Monitor loop ended");
}

} // namespace ConquestDebugger

