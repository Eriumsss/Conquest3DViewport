// Dashboard.cpp
// =============================================================================
// TELEMETRY DASHBOARD - RENDERED INSIDE THE BRAIN OVERLAY'S 'METRICS' TAB
// =============================================================================
// Written by: Eriumsss

#include "PerfScope.h"
#include "Counters.h"

#include "../imgui/imgui.h"

#include <cstdio>

namespace Vespucci {
namespace Telemetry {

void RenderTelemetryDashboard() {
    if (ImGui::CollapsingHeader("Counters")) {
        i32 n = CounterCount();
        if (ImGui::BeginTable("##tel_counters", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();
            for (i32 i = 0; i < n; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(CounterNameAt(i));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)CounterValueAt(i));
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Perf scopes")) {
        i32 n = PerfLabelCount();
        if (ImGui::BeginTable("##tel_perf", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Label");
            ImGui::TableSetupColumn("Last (ms)");
            ImGui::TableSetupColumn("Avg (ms)");
            ImGui::TableSetupColumn("p50");
            ImGui::TableSetupColumn("p95");
            ImGui::TableHeadersRow();
            for (i32 i = 0; i < n; ++i) {
                PerfStats s = PerfStatsAt(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(PerfLabelAt(i));
                ImGui::TableNextColumn(); ImGui::Text("%.3f", s.lastMs);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", s.avgMs);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", s.p50Ms);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", s.p95Ms);
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::Button("Reset all counters")) ResetAllCounters();
    ImGui::SameLine();
    if (ImGui::Button("Reset all perf stats")) ResetAllPerfStats();
}

} // namespace Telemetry
} // namespace Vespucci
