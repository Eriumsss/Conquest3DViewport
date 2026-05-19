// Panel.cpp
// =============================================================================
// GOLDEN RUNNER ORCHESTRATOR - LOAD JSON, RUN, SUMMARY + DRILL-DOWN
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Core/Logging.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../Suggest/CandidateGenerator.h"
#include "../../Suggest/IRanker.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace GoldenRunner {

void RenderResultsDiffTable(PanelState& s);

namespace {
    // Helper — produce a compact human-readable description of a top-K
    // GUID list ("0x4A2B 0x9F1C 0x3E80"). Caps at 4 entries to keep
    // table cells narrow.
    std::string DescribeTopK(const std::vector<Guid>& guids) {
        std::string out;
        out.reserve(64);
        i32 cap = (i32)guids.size();
        if (cap > 4) cap = 4;
        for (i32 i = 0; i < cap; ++i) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%08X", (unsigned)guids[i].raw);
            if (i > 0) out += " ";
            out += hex;
        }
        if ((i32)guids.size() > 4) out += " ...";
        if (out.empty()) out = "(empty)";
        return out;
    }
} // namespace

void InitPanelState(PanelState& s) {
    s.visible = true;       // surface flag controls actual show/hide
    s.loadedPath[0] = 0;
    s.cases.clear();
    s.results.clear();
    s.summary.totalCases = 0;
    s.summary.passed = 0;
    s.summary.failed = 0;
    s.hasRunOnce = false;
    s.lastLoadFailed = false;
    s.hideFailing = false;
    s.scenarioFilter[0] = 0;
    s.showAdvanced = false;
}

bool LoadGoldenCasesIntoPanel(PanelState& s, const char* path) {
    s.cases.clear();
    s.results.clear();
    s.lastLoadFailed = false;
    if (!path || !path[0]) return false;
    if (!QA::LoadGoldenCasesFromFile(path, s.cases)) {
        s.lastLoadFailed = true;
        Core::Logging::Error("GoldenRunner: cannot load '%s'", path);
        return false;
    }
    std::strncpy(s.loadedPath, path, sizeof(s.loadedPath) - 1);
    s.loadedPath[sizeof(s.loadedPath) - 1] = 0;
    Core::Logging::Info("GoldenRunner: loaded %zu cases from %s",
        s.cases.size(), path);
    return true;
}

void RunGoldensCapturingPerCase(PanelState& s) {
    s.results.clear();
    s.results.reserve(s.cases.size());
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    Suggest::CandidateGenerator* gen = Suggest::GlobalGenerator();
    Suggest::IRanker* ranker = Suggest::ActiveRanker();

    for (size_t i = 0; i < s.cases.size(); ++i) {
        const QA::GoldenCase& gc = s.cases[i];
        CaseResult cr;
        cr.scenarioId = gc.scenarioId;
        cr.expectedTopKDescription = DescribeTopK(gc.expectedTopK);

        // Without a live snapshot we can only validate the case shape.
        if (!snap || !gen || !ranker) {
            cr.passed = false;
            cr.actualTopKDescription =
                "(ranker pipeline not bound — load a level first)";
            cr.topScore = 0.0f;
            s.results.push_back(cr);
            continue;
        }

        Vespucci::EntityIndex srcIdx = snap->indexForGuid(gc.sourceGuid);
        if (!srcIdx.valid()) {
            cr.passed = false;
            cr.actualTopKDescription = "(source GUID not in current snapshot)";
            cr.topScore = 0.0f;
            s.results.push_back(cr);
            continue;
        }

        // Run the full pipeline for this case.
        Core::StringRef evt = gc.event.empty()
                              ? Core::StringRef("ontrigger", 9)
                              : Core::StringRef(gc.event.c_str(), (i32)gc.event.size());

        Core::SmallVec<Vespucci::EntityIndex, 256> candidates;
        i32 nCand = gen->generate(srcIdx, evt, candidates,
                                  Limits::kMaxCandidatesPerQuery);

        // Score and capture top-K GUIDs.
        struct Scored {
            Vespucci::EntityIndex idx;
            score_t               score;
            Guid                  guid;
        };
        std::vector<Scored> scored;
        scored.reserve((size_t)nCand);
        for (i32 c = 0; c < nCand; ++c) {
            Vespucci::EntityIndex cIdx = candidates[c];
            const Scene::EntityRow* tgt = snap->entityAt(cIdx);
            if (!tgt) continue;
            Suggest::ReasonChip dummy[4];
            i32 dummyCount = 0;
            score_t sc = ranker->scoreCandidate(*snap, srcIdx, evt, cIdx,
                                                  dummy, 4, dummyCount);
            Scored row;
            row.idx   = cIdx;
            row.score = sc;
            row.guid  = tgt->guid;
            scored.push_back(row);
        }
        std::sort(scored.begin(), scored.end(),
            [](const Scored& a, const Scored& b) { return a.score > b.score; });

        // Compare against expected top-K (order-sensitive, matches the
        // golden file's contract).
        std::vector<Guid> actualGuids;
        actualGuids.reserve(scored.size());
        i32 cap = (i32)scored.size();
        if (cap > (i32)gc.expectedTopK.size()) cap = (i32)gc.expectedTopK.size();
        for (i32 c = 0; c < cap; ++c) actualGuids.push_back(scored[(size_t)c].guid);

        cr.actualTopKDescription = DescribeTopK(actualGuids);
        cr.topScore = scored.empty() ? 0.0f : scored[0].score;
        cr.passed = (actualGuids.size() == gc.expectedTopK.size());
        for (size_t k = 0; k < actualGuids.size() && cr.passed; ++k) {
            if (actualGuids[k].raw != gc.expectedTopK[k].raw) cr.passed = false;
        }
        s.results.push_back(cr);
    }

    // Aggregate.
    s.summary = QA::RunGoldenAndReport(s.cases);   // also logs via Core::Logging
    // Recompute pass/fail from per-case results so the UI matches what
    // we showed (RunGoldenAndReport's V1 passes if expectedTopK is non-empty).
    s.summary.totalCases = (i32)s.results.size();
    s.summary.passed = 0;
    s.summary.failed = 0;
    for (size_t i = 0; i < s.results.size(); ++i) {
        if (s.results[i].passed) s.summary.passed++; else s.summary.failed++;
    }
    s.hasRunOnce = true;
}

void RenderGoldenRunnerPanel(PanelState& s, ImGuiGlueFrameArgs& /*args*/) {
    if (!s.visible) return;
    Common::PushVespucciStyleScope();

    if (ImGui::Begin("Vespucci Golden Runner", &s.visible)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Common::Theme::TextPrimary);
        ImGui::Text("Golden Runner");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 96.0f);
        ImGui::Checkbox("Advanced##gr_adv", &s.showAdvanced);
        ImGui::Separator();

        // Patrick mode — single big "Run goldens" with a tiny path field.
        if (ImGui::Button("Open golden cases...##gr_open")) {
            ImGui::OpenPopup("##gr_open_path");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", s.loadedPath[0] ? s.loadedPath
                                                  : "(no golden file loaded)");

        if (ImGui::BeginPopup("##gr_open_path")) {
            ImGui::Text("Paste a `golden_suggestions.json` path:");
            static char s_pathField[280];
            ImGui::SetNextItemWidth(420.0f);
            bool fired = ImGui::InputText("##gr_path_in", s_pathField, sizeof(s_pathField),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Load##gr_load") || fired) {
                if (LoadGoldenCasesIntoPanel(s, s_pathField)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##gr_cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (s.lastLoadFailed) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f),
                "Last load failed — check the path and the file's JSON shape.");
        }

        if (s.cases.empty()) {
            ImGui::TextDisabled("Load a golden file to begin. Format: array of {id, source_guid, event, expected[]}.");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.32f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.72f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.85f, 0.46f, 1.0f));
            if (ImGui::Button("Run goldens##gr_run", ImVec2(-1.0f, 40.0f))) {
                RunGoldensCapturingPerCase(s);
            }
            ImGui::PopStyleColor(3);

            ImGui::TextDisabled("Loaded %zu cases. Last run: %s",
                s.cases.size(),
                s.hasRunOnce ? "see results below" : "(not run yet)");

            if (s.hasRunOnce) {
                f32 passRatio = (s.summary.totalCases > 0)
                    ? (f32)s.summary.passed / (f32)s.summary.totalCases : 0.0f;
                ImVec4 col = (s.summary.failed == 0)
                    ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                    : ImVec4(1.00f, 0.65f, 0.30f, 1.0f);
                ImGui::TextColored(col,
                    "Result: %d / %d passed (%.0f%%)",
                    s.summary.passed, s.summary.totalCases, passRatio * 100.0f);

                if (s.showAdvanced) {
                    ImGui::Separator();
                    ImGui::Checkbox("Hide passing##gr_hidepass", &s.hideFailing);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(220.0f);
                    ImGui::InputTextWithHint("##gr_filter", "scenario id substring",
                        s.scenarioFilter, sizeof(s.scenarioFilter));
                    RenderResultsDiffTable(s);
                } else if (s.summary.failed > 0) {
                    if (ImGui::CollapsingHeader("Show failing cases##gr_failing")) {
                        // Quick failure list outside Advanced.
                        if (ImGui::BeginTable("##gr_fail_table", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                        {
                            ImGui::TableSetupColumn("Scenario");
                            ImGui::TableSetupColumn("Expected");
                            ImGui::TableSetupColumn("Got");
                            ImGui::TableHeadersRow();
                            for (size_t i = 0; i < s.results.size(); ++i) {
                                const CaseResult& r = s.results[i];
                                if (r.passed) continue;
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(r.scenarioId.c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(r.expectedTopKDescription.c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(r.actualTopKDescription.c_str());
                            }
                            ImGui::EndTable();
                        }
                    }
                }
            }
        }
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace GoldenRunner
} // namespace UI
} // namespace Vespucci
