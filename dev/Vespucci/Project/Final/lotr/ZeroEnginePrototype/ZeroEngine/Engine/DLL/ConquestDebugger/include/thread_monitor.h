#pragma once
// ============================================================================
// Thread Monitor - Detects freezes and captures thread state
// ============================================================================

#ifndef THREAD_MONITOR_H
#define THREAD_MONITOR_H

#include "debugger.h"
#include <vector>

namespace ConquestDebugger {

class ThreadMonitor {
public:
    static ThreadMonitor& Instance();

    bool Start();
    void Stop();
    bool IsRunning() const { return m_running; }

    // Register the main game thread to monitor
    void SetMainThread(DWORD threadId);
    DWORD GetMainThreadId() const { return m_mainThreadId; }

    // Capture current state of a thread
    bool CaptureState(DWORD threadId, ThreadState* state);

    // Check if thread appears frozen (no progress for timeout ms)
    bool IsFrozen(DWORD threadId, DWORD timeoutMs = 2000);

    // Freeze detection settings
    void SetFreezeTimeout(DWORD ms) { m_freezeTimeout = ms; }
    void SetFreezeCallback(void (*callback)(const ThreadState*)) { m_freezeCallback = callback; }

    // EIP sampling - sample instruction pointer multiple times
    void SampleEIP(DWORD threadId, DWORD* samples, int count, DWORD intervalMs);

    // Get human-readable stack trace
    void GetStackTrace(DWORD threadId, char* buffer, size_t bufferSize);

private:
    ThreadMonitor();
    ~ThreadMonitor();
    ThreadMonitor(const ThreadMonitor&) = delete;
    ThreadMonitor& operator=(const ThreadMonitor&) = delete;

    static DWORD WINAPI MonitorThreadProc(LPVOID param);
    void MonitorLoop();

    volatile bool m_running;
    HANDLE m_monitorThread;
    DWORD m_mainThreadId;
    HANDLE m_mainThreadHandle;
    DWORD m_freezeTimeout;
    DWORD m_lastEIP;
    DWORD m_eipStuckCount;
    void (*m_freezeCallback)(const ThreadState*);

    // Track frame counter for freeze detection
    volatile DWORD* m_frameCounterPtr;
    DWORD m_lastFrameCount;
    DWORD m_frameStuckTicks;
};

} // namespace ConquestDebugger

#endif // THREAD_MONITOR_H

