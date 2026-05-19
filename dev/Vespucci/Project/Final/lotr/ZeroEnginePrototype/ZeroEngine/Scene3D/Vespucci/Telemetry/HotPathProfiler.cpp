// HotPathProfiler.cpp
// =============================================================================
// SAMPLE-BASED HOT-PATH IDENTIFIER - 'WHICH SCOPE OWNS MOST OF MY FRAME?'
// =============================================================================
// Written by: Eriumsss

#include "PerfScope.h"

#include "../imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace Vespucci {
namespace Telemetry {

void RenderHotPathProfiler() {
    i32 n = PerfLabelCount();
    if (n == 0) {
        ImGui::TextDisabled("(no perf scopes recorded yet)");
        return;
    }
    struct Row { const char* label; f32 avgMs; f32 p95Ms; u64 count; };
    std::vector<Row> rows;
    rows.reserve((size_t)n);
    for (i32 i = 0; i < n; ++i) {
        PerfStats s = PerfStatsAt(i);
        Row r; r.label = PerfLabelAt(i); r.avgMs = s.avgMs; r.p95Ms = s.p95Ms; r.count = s.sampleCount;
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(),
        [](const Row& a, const Row& b) { return a.avgMs > b.avgMs; });

    ImGui::Text("Top hot paths (sorted by avg ms desc):");
    if (ImGui::BeginTable("##hot", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Label");
        ImGui::TableSetupColumn("Avg ms");
        ImGui::TableSetupColumn("p95 ms");
        ImGui::TableSetupColumn("Samples");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(rows[i].label);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", rows[i].avgMs);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", rows[i].p95Ms);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)rows[i].count);
        }
        ImGui::EndTable();
    }
}

} // namespace Telemetry
} // namespace Vespucci
