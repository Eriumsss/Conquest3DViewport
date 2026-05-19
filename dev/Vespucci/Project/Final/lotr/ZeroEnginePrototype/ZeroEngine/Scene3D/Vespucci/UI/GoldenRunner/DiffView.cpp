// DiffView.cpp
// =============================================================================
// PER-CASE PASS / FAIL TABLE WITH EXPECTED-VS-ACTUAL DIFF
// =============================================================================
// Written by: Eriumsss
//
// Color-coded scrollable table. Green row = pass, red row = fail. Each
// row shows scenarioId, the expected top-K (first 4 GUIDs), the actual
// top-K, and the top score the ranker assigned. Designer can spot
// regressions at a glance — "this used to suggest the south gate, now
// it suggests the north tower, that's wrong."
// =============================================================================

#include "Panel.h"

#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <cstring>

namespace Vespucci {
namespace UI {
namespace GoldenRunner {

namespace {
    bool ScenarioPassesFilter(const CaseResult& r, const PanelState& s) {
        if (s.hideFailing && !r.passed) {
            // Hide-passing checkbox label is "Hide passing" — meaning
            // hide the rows that PASS so failures pop. That's what
            // designers actually want during regression hunts.
        }
        if (s.hideFailing && r.passed) return false;
        if (s.scenarioFilter[0]) {
            usize fn = std::strlen(s.scenarioFilter);
            if (fn > 0) {
                bool found = false;
                const char* hay = r.scenarioId.c_str();
                usize hlen = r.scenarioId.size();
                for (usize i = 0; i + fn <= hlen; ++i) {
                    bool ok = true;
                    for (usize k = 0; k < fn; ++k) {
                        char ca = hay[i + k];
                        char cb = s.scenarioFilter[k];
                        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                        if (ca != cb) { ok = false; break; }
                    }
                    if (ok) { found = true; break; }
                }
                if (!found) return false;
            }
        }
        return true;
    }
} // namespace

void RenderResultsDiffTable(PanelState& s) {
    if (s.results.empty()) {
        ImGui::TextDisabled("(no results — hit Run goldens above)");
        return;
    }

    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Sortable;

    ImVec2 outerSize = ImVec2(0.0f, 360.0f);
    if (ImGui::BeginTable("##gr_results_table", 5, flags, outerSize)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Result",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Scenario", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Expected", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Got",      ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Top score", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < s.results.size(); ++i) {
            const CaseResult& r = s.results[i];
            if (!ScenarioPassesFilter(r, s)) continue;

            ImGui::TableNextRow();
            ImU32 bgCol = r.passed
                ? IM_COL32(40, 70, 50, 90)
                : IM_COL32(90, 40, 40, 90);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgCol);

            ImGui::TableNextColumn();
            ImGui::TextColored(
                r.passed ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                         : ImVec4(1.00f, 0.55f, 0.55f, 1.0f),
                "%s", r.passed ? "PASS" : "FAIL");

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.scenarioId.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.expectedTopKDescription.c_str());

            ImGui::TableNextColumn();
            // If the actual diverges, color-flag in the cell text too.
            if (r.passed) {
                ImGui::TextUnformatted(r.actualTopKDescription.c_str());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.55f, 1.0f),
                    "%s", r.actualTopKDescription.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.2f", r.topScore);
        }
        ImGui::EndTable();
    }
}

} // namespace GoldenRunner
} // namespace UI
} // namespace Vespucci
