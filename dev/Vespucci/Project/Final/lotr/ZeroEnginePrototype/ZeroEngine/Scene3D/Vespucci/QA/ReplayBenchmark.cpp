// ReplayBenchmark.cpp
// =============================================================================
// PLAY A REPLAY THROUGH THE RANKER + COMPUTE BENCHMARK STATS
// =============================================================================
// Written by: Eriumsss

#include "ReplayRecorder.h"

#include "../Core/Logging.h"
#include "../Core/HeatTimer.h"

#include <algorithm>
#include <vector>

namespace Vespucci {
namespace QA {

bool LoadReplayFromFile(const char* path, std::vector<ReplayEvent>& out);
i32  PlayReplayCalculatingChecksum(const std::vector<ReplayEvent>& events, u64& outChecksum);

struct BenchmarkResult {
    i32 events;
    f64 totalMs;
    f64 avgMs;
    f64 p95Ms;
    u64 checksum;
};

bool RunBenchmarkOverReplay(const char* replayPath, BenchmarkResult& out) {
    std::vector<ReplayEvent> events;
    if (!LoadReplayFromFile(replayPath, events)) return false;

    std::vector<f64> sampleMs;
    sampleMs.reserve(events.size());

    u64 t0 = Core::NowTicks();
    u64 cs;
    PlayReplayCalculatingChecksum(events, cs);
    u64 totalNs = Core::TicksToNs(Core::NowTicks() - t0);

    out.events     = (i32)events.size();
    out.totalMs    = (f64)totalNs / 1000000.0;
    out.avgMs      = events.empty() ? 0.0 : out.totalMs / (f64)events.size();
    out.p95Ms      = out.avgMs; // single-pass replay - no per-event split available here
    out.checksum   = cs;
    Core::Logging::Info("ReplayBenchmark: %d events in %.2f ms (avg %.4f, checksum 0x%llX)",
        out.events, out.totalMs, out.avgMs, (unsigned long long)cs);
    return true;
}

} // namespace QA
} // namespace Vespucci
