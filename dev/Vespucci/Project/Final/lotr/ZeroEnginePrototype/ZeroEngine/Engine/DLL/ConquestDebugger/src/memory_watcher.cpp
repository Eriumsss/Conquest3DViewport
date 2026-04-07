// ============================================================================
// Memory Watcher - Monitor memory addresses
// ============================================================================

#include "../include/memory_watcher.h"
#include "../include/debugger.h"
#include <cstdio>

namespace ConquestDebugger {

MemoryWatcher& MemoryWatcher::Instance() {
    static MemoryWatcher instance;
    return instance;
}

MemoryWatcher::MemoryWatcher()
    : m_running(false)
    , m_watcherThread(nullptr)
    , m_nextWatchId(1)
    , m_watchCallback(nullptr)
{
    InitializeCriticalSection(&m_lock);
}

MemoryWatcher::~MemoryWatcher() {
    Stop();
    DeleteCriticalSection(&m_lock);
}

DWORD MemoryWatcher::ReadSafe(uintptr_t address, size_t size) {
    DWORD value = 0;
    __try {
        switch (size) {
            case 1: value = *(BYTE*)address; break;
            case 2: value = *(WORD*)address; break;
            case 4: value = *(DWORD*)address; break;
            default: value = *(DWORD*)address; break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0xDEADBEEF;
    }
    return value;
}

bool MemoryWatcher::WriteSafe(uintptr_t address, DWORD value, size_t size) {
    __try {
        DWORD oldProtect;
        if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        switch (size) {
            case 1: *(BYTE*)address = (BYTE)value; break;
            case 2: *(WORD*)address = (WORD)value; break;
            case 4: *(DWORD*)address = value; break;
            default: *(DWORD*)address = value; break;
        }
        VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int MemoryWatcher::AddWatch(const char* name, uintptr_t address, size_t size, WatchType type) {
    EnterCriticalSection(&m_lock);

    MemoryWatch watch;
    watch.name = name;
    watch.address = address;
    watch.size = size;
    watch.type = type;
    watch.lastValue = ReadSafe(address, size);
    watch.triggered = false;

    m_watches.push_back(watch);
    int id = m_nextWatchId++;

    LeaveCriticalSection(&m_lock);

    Log(LogLevel::Debug, "MemoryWatcher: Added watch '%s' at 0x%08X", name, address);
    return id;
}

void MemoryWatcher::RemoveWatch(int watchId) {
    EnterCriticalSection(&m_lock);
    if (watchId > 0 && watchId <= (int)m_watches.size()) {
        m_watches.erase(m_watches.begin() + (watchId - 1));
    }
    LeaveCriticalSection(&m_lock);
}

void MemoryWatcher::ClearAllWatches() {
    EnterCriticalSection(&m_lock);
    m_watches.clear();
    LeaveCriticalSection(&m_lock);
}

MemoryWatcher::PoolState MemoryWatcher::GetPoolState() {
    PoolState state;
    state.eventCount = ReadSafe(Addresses::EVENT_POOL_COUNT);
    state.eventLimit = Addresses::EVENT_POOL_LIMIT;
    state.eventBase = ReadSafe(Addresses::EVENT_POOL_BASE);
    state.eventFreeHead = ReadSafe(Addresses::EVENT_POOL_FREELIST);
    state.pool2Count = ReadSafe(Addresses::POOL2_COUNT);
    state.pool2Limit = Addresses::POOL2_LIMIT;
    return state;
}

bool MemoryWatcher::Start() {
    if (m_running) return true;
    m_running = true;
    m_watcherThread = CreateThread(nullptr, 0, WatcherThreadProc, this, 0, nullptr);
    return m_watcherThread != nullptr;
}

void MemoryWatcher::Stop() {
    if (!m_running) return;
    m_running = false;
    if (m_watcherThread) {
        WaitForSingleObject(m_watcherThread, 2000);
        CloseHandle(m_watcherThread);
        m_watcherThread = nullptr;
    }
}

DWORD WINAPI MemoryWatcher::WatcherThreadProc(LPVOID param) {
    MemoryWatcher* self = static_cast<MemoryWatcher*>(param);
    self->WatcherLoop();
    return 0;
}

void MemoryWatcher::WatcherLoop() {
    while (m_running) {
        Poll();
        Sleep(100);  // Check every 100ms
    }
}

void MemoryWatcher::Poll() {
    EnterCriticalSection(&m_lock);
    for (auto& watch : m_watches) {
        DWORD newValue = ReadSafe(watch.address, watch.size);
        if (watch.type == WatchType::Value && newValue != watch.lastValue) {
            if (m_watchCallback) {
                m_watchCallback(&watch, watch.lastValue, newValue);
            }
            watch.lastValue = newValue;
        }
    }
    LeaveCriticalSection(&m_lock);
}

void MemoryWatcher::SetWatchCallback(void (*callback)(const MemoryWatch*, DWORD, DWORD)) {
    m_watchCallback = callback;
}

} // namespace ConquestDebugger

