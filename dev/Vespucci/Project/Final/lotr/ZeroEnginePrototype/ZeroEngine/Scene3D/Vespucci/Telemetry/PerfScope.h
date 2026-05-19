// PerfScope.h
// =============================================================================
// HIGHER-LEVEL PERF SCOPE - SCOPED RAII THAT FEEDS THE TELEMETRY DASHBOARD
// =============================================================================
// Written by: Eriumsss
//
// Core/HeatTimer is the per-scope ring buffer for the BRAIN tab. This
// is the cross-cutting telemetry version that aggregates labeled
// scopes across frames into rolling averages + p50/p95 percentiles
// for the editor's perf dashboard. They cooperate: PerfScope is the
// outer-loop tracker, HeatTimer is the inner-loop microbenchmark.
// =============================================================================

#ifndef VESPUCCI_TELEMETRY_PERFSCOPE_H_
#define VESPUCCI_TELEMETRY_PERFSCOPE_H_

#include "../Core/VespucciTypes.h"

namespace Vespucci {
namespace Telemetry {

class PerfScope {
public:
    PerfScope(const char* label);
    ~PerfScope();
private:
    const char* m_label;
    u64         m_startTicks;
};

void RecordPerfSampleManually(const char* label, u64 durationNs);

struct PerfStats {
    f32 lastMs;
    f32 avgMs;
    f32 p50Ms;
    f32 p95Ms;
    u64 sampleCount;
};

PerfStats GetPerfStatsFor(const char* label);
i32       PerfLabelCount();
const char* PerfLabelAt(i32 i);
PerfStats   PerfStatsAt(i32 i);

void ResetAllPerfStats();

} // namespace Telemetry
} // namespace Vespucci

#define VESPUCCI_PERF_SCOPE(label) \
    ::Vespucci::Telemetry::PerfScope _vesp_perf_##__LINE__(label)

#endif // VESPUCCI_TELEMETRY_PERFSCOPE_H_
