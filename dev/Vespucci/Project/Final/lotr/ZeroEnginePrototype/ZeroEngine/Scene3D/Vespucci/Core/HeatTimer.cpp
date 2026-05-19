// HeatTimer.cpp
// =============================================================================
// QPC-backed perf scope, ring-buffered per frame. Single-threaded by
// design - editor side is single-threaded, so the ring is just a flat
// static array with no locking. If we ever spawn worker threads for
// corpus rebuild they get their OWN HeatRing and we merge in the
// dashboard, never share this one.
// =============================================================================
// Written by: Eriumsss

#include "HeatTimer.h"
#include "Hash.h"
#include "VespucciAssert.h"

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace Vespucci {
namespace Core {

namespace {
    static const i32 kHeatRingCap = 4096;

    HeatRingEntry s_ring[kHeatRingCap];
    i32           s_ringSize = 0;
    u64           s_totalNs  = 0;

    // QPC frequency, queried once. If we are on a system without
    // QPC (we are not, this is Windows-only) the timer returns 0
    // and the dashboard shows "no profiler" instead of crashing.
#if defined(_WIN32)
    LARGE_INTEGER s_freq      = {0};
    bool          s_freqValid = false;
#endif
} // namespace

u64 NowTicks() {
#if defined(_WIN32)
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
#else
    return 0;
#endif
}

u64 TicksToNs(u64 ticks) {
#if defined(_WIN32)
    if (!s_freqValid) {
        QueryPerformanceFrequency(&s_freq);
        s_freqValid = (s_freq.QuadPart != 0);
    }
    if (!s_freqValid) return 0;
    // Multiply-then-divide to keep precision. If ticks overflow u64
    // when multiplied by 1e9 we are running for ~292 years and have
    // bigger problems than this calculation.
    return (ticks * 1000000000ull) / (u64)s_freq.QuadPart;
#else
    return 0;
#endif
}

HeatScope::HeatScope(const char* name)
    : m_name(name), m_startTicks(NowTicks()) {}

HeatScope::~HeatScope() {
    u64 endTicks = NowTicks();
    u64 ns = TicksToNs(endTicks - m_startTicks);
    HeatRecord(m_name, ns);
}

void HeatRecord(const char* name, u64 durationNs) {
    if (s_ringSize >= kHeatRingCap) {
        // Ring full this frame. Drop new samples - this is a
        // "you put 4000+ scopes in one frame" event, profile
        // the profiler at that point.
        return;
    }
    HeatRingEntry& e = s_ring[s_ringSize];
    e.name       = name;
    e.durationNs = durationNs;
    e.hash       = XxHash32CStr(name, 0xDEADBEEFu);
    s_ringSize++;
    s_totalNs   += durationNs;
}

i32 ReadHeatFrame(HeatRingEntry* out, i32 cap) {
    i32 n = (s_ringSize < cap) ? s_ringSize : cap;
    for (i32 i = 0; i < n; ++i) out[i] = s_ring[i];
    return n;
}

void FlushHeatFrame() {
    s_ringSize = 0;
    s_totalNs  = 0;
}

u64 HeatTotalNsThisFrame()    { return s_totalNs; }
u32 HeatScopeCountThisFrame() { return (u32)s_ringSize; }

} // namespace Core
} // namespace Vespucci
