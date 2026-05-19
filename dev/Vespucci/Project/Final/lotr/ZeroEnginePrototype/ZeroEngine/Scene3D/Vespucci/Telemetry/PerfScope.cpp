// PerfScope.cpp
// =============================================================================
// Per-label rolling-window stats. Window of 256 samples per label.
// =============================================================================
// Written by: Eriumsss

#include "PerfScope.h"

#include "../Core/HeatTimer.h"
#include "../Core/Hash.h"

#include <algorithm>
#include <unordered_map>

namespace Vespucci {
namespace Telemetry {

namespace {
    static const i32 kWindow = 256;
    struct LabelData {
        const char* label;
        f32         samples[kWindow];
        i32         cursor;
        u64         total;
    };
    static std::unordered_map<u32, LabelData> s_byHash;
} // namespace

PerfScope::PerfScope(const char* label)
    : m_label(label), m_startTicks(Core::NowTicks()) {}

PerfScope::~PerfScope() {
    u64 ns = Core::TicksToNs(Core::NowTicks() - m_startTicks);
    RecordPerfSampleManually(m_label, ns);
}

void RecordPerfSampleManually(const char* label, u64 durationNs) {
    if (!label) return;
    u32 h = Core::XxHash32CStr(label, 0xCAFEBABEu);
    LabelData& d = s_byHash[h];
    if (!d.label) { d.label = label; d.cursor = 0; d.total = 0; for (i32 i = 0; i < kWindow; ++i) d.samples[i] = 0.0f; }
    d.samples[d.cursor] = (f32)durationNs / 1000000.0f;
    d.cursor = (d.cursor + 1) % kWindow;
    d.total++;
}

PerfStats GetPerfStatsFor(const char* label) {
    PerfStats s = { 0,0,0,0,0 };
    if (!label) return s;
    u32 h = Core::XxHash32CStr(label, 0xCAFEBABEu);
    std::unordered_map<u32, LabelData>::const_iterator it = s_byHash.find(h);
    if (it == s_byHash.end()) return s;
    const LabelData& d = it->second;
    s.sampleCount = d.total;
    i32 valid = (d.total < kWindow) ? (i32)d.total : kWindow;
    if (valid == 0) return s;
    f32 sum = 0.0f;
    f32 sorted[kWindow];
    for (i32 i = 0; i < valid; ++i) { sorted[i] = d.samples[i]; sum += d.samples[i]; }
    s.avgMs = sum / (f32)valid;
    s.lastMs = d.samples[(d.cursor + kWindow - 1) % kWindow];
    std::sort(sorted, sorted + valid);
    s.p50Ms = sorted[valid / 2];
    i32 p95i = (i32)((f32)valid * 0.95f);
    if (p95i >= valid) p95i = valid - 1;
    s.p95Ms = sorted[p95i];
    return s;
}

i32 PerfLabelCount() { return (i32)s_byHash.size(); }

const char* PerfLabelAt(i32 i) {
    if (i < 0 || i >= (i32)s_byHash.size()) return 0;
    std::unordered_map<u32, LabelData>::const_iterator it = s_byHash.begin();
    std::advance(it, i);
    return it->second.label;
}

PerfStats PerfStatsAt(i32 i) {
    PerfStats s = { 0,0,0,0,0 };
    if (i < 0 || i >= (i32)s_byHash.size()) return s;
    std::unordered_map<u32, LabelData>::const_iterator it = s_byHash.begin();
    std::advance(it, i);
    return GetPerfStatsFor(it->second.label);
}

void ResetAllPerfStats() { s_byHash.clear(); }

} // namespace Telemetry
} // namespace Vespucci
