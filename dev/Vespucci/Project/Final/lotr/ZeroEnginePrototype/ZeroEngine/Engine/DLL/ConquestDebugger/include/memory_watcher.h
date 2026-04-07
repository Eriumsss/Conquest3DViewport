#pragma once
// ============================================================================
// Memory Watcher - Monitor memory addresses for changes/access
// ============================================================================

#ifndef MEMORY_WATCHER_H
#define MEMORY_WATCHER_H

#include "debugger.h"
#include <vector>
#include <string>

namespace ConquestDebugger {

// Predefined watches for LOTR Conquest
struct GameAddress {
    const char* name;
    uintptr_t address;
    const char* description;
};

// Known game addresses (from reverse engineering)
namespace Addresses {
    // Event Pool
    constexpr uintptr_t EVENT_POOL_BASE     = 0x00cd9970;
    constexpr uintptr_t EVENT_POOL_FREELIST = 0x00cd996c;
    constexpr uintptr_t EVENT_POOL_COUNT    = 0x00cd9974;
    constexpr uintptr_t EVENT_POOL_LIMIT    = 4096;  // After our patch

    // Pool2
    constexpr uintptr_t POOL2_COUNT         = 0x00cd7e28;
    constexpr uintptr_t POOL2_LIMIT         = 256;

    // Other known addresses
    constexpr uintptr_t GAME_STATE          = 0x00cdb344;
    constexpr uintptr_t LINKED_LIST_HEAD1   = 0x00cde3fc;
    constexpr uintptr_t LINKED_LIST_HEAD2   = 0x00cdcbdc;
}

class MemoryWatcher {
public:
    static MemoryWatcher& Instance();

    bool Start();
    void Stop();
    bool IsRunning() const { return m_running; }

    // Add a watch on a memory address
    int AddWatch(const char* name, uintptr_t address, size_t size, WatchType type);
    void RemoveWatch(int watchId);
    void ClearAllWatches();

    // Read memory safely (with SEH protection)
    static DWORD ReadSafe(uintptr_t address, size_t size = 4);
    static bool WriteSafe(uintptr_t address, DWORD value, size_t size = 4);

    // Check all watches for changes
    void Poll();

    // Get current pool states
    struct PoolState {
        DWORD eventCount;
        DWORD eventLimit;
        DWORD eventBase;
        DWORD eventFreeHead;
        DWORD pool2Count;
        DWORD pool2Limit;
    };
    PoolState GetPoolState();

    // Callbacks
    void SetWatchCallback(void (*callback)(const MemoryWatch* watch, DWORD oldVal, DWORD newVal));

private:
    MemoryWatcher();
    ~MemoryWatcher();
    MemoryWatcher(const MemoryWatcher&) = delete;
    MemoryWatcher& operator=(const MemoryWatcher&) = delete;

    static DWORD WINAPI WatcherThreadProc(LPVOID param);
    void WatcherLoop();

    volatile bool m_running;
    HANDLE m_watcherThread;
    std::vector<MemoryWatch> m_watches;
    int m_nextWatchId;
    void (*m_watchCallback)(const MemoryWatch*, DWORD, DWORD);

    CRITICAL_SECTION m_lock;
};

} // namespace ConquestDebugger

#endif // MEMORY_WATCHER_H

