// Panel.cpp
// =============================================================================
// Corpus browser orchestrator. Patrick mode shows a one-sentence
// summary; the per-type / per-event tables hide behind Advanced.
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Corpus/CorpusPriors.h"
#include "../../Schema/ZETypeRegistry.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace CorpusBrowser {

void RenderPerTypeView(PanelState& s);
void RenderPerEventView(PanelState& s);

void InitPanelState(PanelState& s) {
    s.visible = true;
    s.typeFilter[0] = 0;
    s.eventFilter[0] = 0;
    s.selectedTypeIdx = 0;
    s.showAdvanced = false;
    s.focusedEntityGuid = Guid(0);
}

namespace {
    // Quick aggregate read of the global priors. Returns counts the
    // headline summary uses to decide whether the corpus is actually
    // populated or just an empty husk.
    void TallyCorpusOrAdmitDefeat(i32& outTriples, u64& outObservations,
                                    i32& outSrcPairs,
                                    i32& outEventPairs)
    {
        outTriples = 0;
        outObservations = 0;
        outSrcPairs = 0;
        outEventPairs = 0;
        Corpus::CorpusPriors* p = Corpus::GlobalPriors();
        if (!p) return;
        Corpus::CorpusPriors::Stats st = p->stats();
        outTriples       = st.tripleEntries;
        outObservations  = st.totalObservations;
        outSrcPairs      = st.srcPairEntries;
        outEventPairs    = st.eventPairEntries;
    }
} // namespace

void RenderCorpusBrowser(PanelState& s) {
    if (!s.visible) return;
    Common::PushVespucciStyleScope();
    if (ImGui::Begin("Vespucci Corpus Browser", &s.visible)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Common::Theme::TextPrimary);
        ImGui::Text("Corpus Browser");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        f32 toggleWidth = 96.0f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - toggleWidth);
        ImGui::Checkbox("Advanced##cb_adv", &s.showAdvanced);
        ImGui::Separator();

        i32 triples = 0;
        u64 observations = 0;
        i32 srcPairs = 0, eventPairs = 0;
        TallyCorpusOrAdmitDefeat(triples, observations, srcPairs, eventPairs);

        if (triples == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.45f, 1.0f),
                "Corpus is empty. Load a level — Vespucci ingests its wires "
                "into the corpus automatically. The ranker uses these "
                "frequencies to pick the most-likely target for new wires.");
        } else {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                "%d (source, event, target) triples observed (%llu total "
                "wire observations across %d source-event pairs and %d "
                "event-action pairs).",
                triples, (unsigned long long)observations,
                srcPairs, eventPairs);
        }

        // Cross-page focused entity — pre-fill the type filter so the
        // designer's selected entity's type floats to the top of the
        // per-type table.
        if (s.focusedEntityGuid.valid()) {
            Scene::SceneSnapshot* snapB = Scene::GlobalSnapshot();
            const Scene::EntityRow* row = 0;
            if (snapB) {
                Vespucci::EntityIndex idx = snapB->indexForGuid(s.focusedEntityGuid);
                if (idx.valid()) row = snapB->entityAt(idx);
            }
            if (row) {
                ImGui::TextColored(ImVec4(0.93f, 0.95f, 0.98f, 1.0f),
                    "Selected: %.*s (%.*s)",
                    (int)row->name.size(), row->name.data(),
                    (int)row->typeName.size(), row->typeName.data());
                if (row->typeName.size() > 0 && s.typeFilter[0] == 0) {
                    usize n = row->typeName.size();
                    if (n + 1 > sizeof(s.typeFilter)) n = sizeof(s.typeFilter) - 1;
                    std::memcpy(s.typeFilter, row->typeName.data(), n);
                    s.typeFilter[n] = 0;
                }
            }
        }

        if (!s.showAdvanced) {
            ImGui::End();
            Common::PopVespucciStyleScope();
            return;
        }

        ImGui::Separator();
        if (ImGui::BeginTabBar("##cb_tabs")) {
            if (ImGui::BeginTabItem("Per-type")) {
                RenderPerTypeView(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Per-event")) {
                RenderPerEventView(s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace CorpusBrowser
} // namespace UI
} // namespace Vespucci
