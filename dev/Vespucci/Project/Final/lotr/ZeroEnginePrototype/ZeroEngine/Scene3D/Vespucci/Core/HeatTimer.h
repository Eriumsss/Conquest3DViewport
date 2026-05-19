// HeatTimer.h
// =============================================================================
// HEATTIMER — RAII PERF SCOPE. NANOSECOND-CHEAP, ALWAYS LIVE.
// =============================================================================
// Written by: Eriumsss
//
// Every hot path in Vespucci is wrapped in HEAT_SCOPE("name"). The
// macro creates a stack RAII object that records start/end via QPC,
// hashes the name once, and submits the duration to a thread-local
// flat ring buffer. The HotPathProfiler in Telemetry/ reads the ring
// each frame to render the "what is eating my budget" overlay.
//
// Cost: ~50 ns per scope on a modern x86 (one QPC at start, one at
// end, one ring write). That is cheaper than every alternative I
// looked at - including Tracy, which is fucking great but adds a
// dependency the size of a small kingdom and broadcasts over TCP
// to a viewer we don't run. We are not Tracy. We are an editor for
// a dead studio's binary format. Keep it small.
//
// All scopes are ALWAYS LIVE. No #ifdef PROFILE_BUILD garbage. The
// data is only useful when it's there, and 50 ns × 1000 scopes per
// frame is 50 us, which is 0.3% of a 16ms frame. Fine.
// =============================================================================

#ifndef VESPUCCI_CORE_HEATTIMER_H_
#define VESPUCCI_CORE_HEATTIMER_H_

#include "VespucciTypes.h"

namespace Vespucci {
namespace Core {

// Fixed-name scope. The label pointer must outlive the timer's
// destruction. Use string literals at the call site.
class HeatScope {
public:
    HeatScope(const char* name);
    ~HeatScope();
private:
    const char* m_name;
    u64         m_startTicks;
};

// Submit a one-shot duration without an RAII scope. Use this when
// you have already measured the duration externally (e.g. nested
// in a callback).
void HeatRecord(const char* name, u64 durationNs);

// Read the last frame's heat scopes. Returns count; pointers stable
// until next FlushHeatFrame().
struct HeatRingEntry {
    const char* name;
    u64         durationNs;
    u32         hash;     // pre-hashed name for the dashboard
};

i32  ReadHeatFrame(HeatRingEntry* out, i32 cap);
void FlushHeatFrame();   // call once per editor frame, wipes the ring

// Stats helpers - the dashboard uses these.
u64  HeatTotalNsThisFrame();
u32  HeatScopeCountThisFrame();

// Tick-to-nanoseconds. Exposed so non-Vespucci code wrapping this
// (the editor's own perf overlay) can convert without re-querying
// QueryPerformanceFrequency.
u64 TicksToNs(u64 ticks);
u64 NowTicks();

} // namespace Core
} // namespace Vespucci

// ── Macro ─────────────────────────────────────────────────────────────
// Creates a uniquely-named local. The name has to dodge collisions
// with multiple HEAT_SCOPE calls in the same block (rare, but happens).
#define VESPUCCI_HEAT_PASTE2(a, b) a##b
#define VESPUCCI_HEAT_PASTE(a, b)  VESPUCCI_HEAT_PASTE2(a, b)
#define HEAT_SCOPE(name) ::Vespucci::Core::HeatScope VESPUCCI_HEAT_PASTE(_heatScope_, __LINE__)(name)

#endif // VESPUCCI_CORE_HEATTIMER_H_
