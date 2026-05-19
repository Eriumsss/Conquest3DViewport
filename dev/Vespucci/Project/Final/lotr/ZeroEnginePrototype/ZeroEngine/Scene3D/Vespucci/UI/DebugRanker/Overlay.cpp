// Overlay.cpp
// =============================================================================
// Render the Brain overlay - candidate pool stats, top-K breakdown,
// latency sparkline, weight tuning sliders.
// =============================================================================
// Written by: Eriumsss

#include "Overlay.h"

#include "../../Core/Logging.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../Suggest/IRanker.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"
#include "../Common/Sparkline.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace DebugRanker {

void RenderWeightSliders();
void RenderScoreBreakdown(const Suggest::SuggestionList& list);
void RenderFeatureHistogram(const Suggest::SuggestionList& list);

void InitOverlayState(OverlayState& s) {
    s.visible = true;
    s.autoCaptureLast = true;
    std::memset(s.latencySamples, 0, sizeof(s.latencySamples));
    s.latencyCursor = 0;
    std::memset(&s.lastList, 0, sizeof(s.lastList));
    s.showAdvanced = false; // Patrick mode default
    s.focusedEntityGuid = Guid(0);
}

void CaptureLastSuggestion(OverlayState& s, const Suggest::SuggestionList& list) {
    if (!s.autoCaptureLast) return;
    s.lastList = list;
    s.latencySamples[s.latencyCursor] = list.rankerMs;
    s.latencyCursor = (s.latencyCursor + 1) % 64;
}

namespace {
    static OverlayState* s_globalBrain = 0;
}
OverlayState* GlobalBrainState()                     { return s_globalBrain; }
void          SetGlobalBrainState(OverlayState* s)    { s_globalBrain = s; }

void PublishLastQuery(const Suggest::SuggestionList& list) {
    if (!s_globalBrain) return;
    CaptureLastSuggestion(*s_globalBrain, list);
}

void RenderBrainOverlay(OverlayState& s) {
    if (!s.visible) return;
    Common::PushVespucciStyleScope();
    if (ImGui::Begin("Vespucci Brain", &s.visible)) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.55f, 1.0f),
            "Vespucci ranker telemetry");
        ImGui::SameLine();
        f32 toggleWidth = 96.0f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - toggleWidth);
        ImGui::Checkbox("Advanced##br_adv", &s.showAdvanced);
        ImGui::Separator();

        // ── Cross-page focused-entity readout ──
        // Mirrors the host's selection so every panel agrees on what
        // entity the designer is asking about. Vespucci::Update has
        // already piped args.selectedEditorObjGuid into focusedEntityGuid
        // on this frame — we just render it here.
        Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
        if (s.focusedEntityGuid.valid() && snap) {
            EntityIndex fIdx = snap->indexForGuid(s.focusedEntityGuid);
            if (fIdx.valid()) {
                const Scene::EntityRow* row = snap->entityAt(fIdx);
                if (row) {
                    ImGui::TextColored(ImVec4(0.93f, 0.95f, 0.98f, 1.0f),
                        "Focused: %.*s",
                        (int)row->name.size(), row->name.data());
                    if (row->typeName.size() > 0) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%.*s)",
                            (int)row->typeName.size(), row->typeName.data());
                    }
                }
            }
        } else {
            ImGui::TextDisabled("Focused: (nothing selected — click an entity anywhere)");
        }

        // ── Patrick mode default — single sentence health summary ──
        bool snapAlive   = (snap && snap->entityCount() > 0);
        bool rankerAlive = (s.lastList.count > 0 || s.lastList.candidatePoolSize > 0);
        f32  buildMs     = snap ? snap->lastBuildMs() : 0.0f;
        f32  rankerMs    = s.lastList.rankerMs;
        bool perfHealthy = (buildMs < 5.0f && rankerMs < 5.0f);

        ImVec4 statusCol = (snapAlive && rankerAlive && perfHealthy)
            ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
            : ImVec4(1.00f, 0.78f, 0.45f, 1.0f);
        if (!snapAlive) {
            ImGui::TextColored(statusCol,
                "No level loaded yet. Brain will light up once a snapshot exists.");
        } else if (!rankerAlive) {
            ImGui::TextColored(statusCol,
                "Snapshot live (%d entities). Ranker hasn't been asked anything yet — "
                "open Suggested Wires and click an entity to wake it up.",
                snap->entityCount());
        } else {
            ImGui::TextColored(statusCol,
                "Healthy. Snapshot %d entities @ %.2f ms, last ranker query %d "
                "candidate%s in %.2f ms.",
                snap->entityCount(), buildMs,
                s.lastList.candidatePoolSize,
                s.lastList.candidatePoolSize == 1 ? "" : "s",
                rankerMs);
        }

        // Plain-English status of the active ranker.
        Suggest::IRanker::Kind activeKind = Suggest::ActiveRankerKind();
        const char* rankerName =
            (activeKind == Suggest::IRanker::KIND_Frequency)    ? "Frequency"      :
            (activeKind == Suggest::IRanker::KIND_TypeDirected) ? "Type-directed" :
            (activeKind == Suggest::IRanker::KIND_Hybrid)       ? "Hybrid"         :
            (activeKind == Suggest::IRanker::KIND_MLStub)       ? "ML-stub"        :
                                                                   "(none)";
        ImGui::TextDisabled("Active ranker: %s", rankerName);

        // ── Cockpit gate — everything below only when Advanced is on ──
        if (!s.showAdvanced) {
            ImGui::End();
            Common::PopVespucciStyleScope();
            return;
        }

        ImGui::Separator();
        ImGui::Checkbox("Auto-capture last query", &s.autoCaptureLast);

        if (snap) {
            ImGui::Text("Snapshot version: %u", snap->version().raw);
            ImGui::Text("Snapshot rebuild: %.2f ms", snap->lastBuildMs());
        }

        ImGui::Separator();
        ImGui::Text("Last query");
        char hdr[256];
        std::snprintf(hdr, sizeof(hdr),
            "  pool=%d  passed-hard=%d  shown=%d  suppressed=%d  rules=%d  ranker=%.2fms",
            s.lastList.candidatePoolSize,
            s.lastList.candidatesAfterHardFilter,
            s.lastList.count,
            s.lastList.suppressedByConfidence,
            s.lastList.rulesEvaluated,
            s.lastList.rankerMs);
        ImGui::TextDisabled("%s", hdr);

        // Ranker strategy switcher — designers can hot-swap and see the
        // effect on the next query. Hybrid is the default.
        ImGui::Separator();
        ImGui::Text("Ranker strategy:");
        const char* strategyLabels[4] = {
            "Frequency", "Type-directed", "Hybrid", "ML-stub"
        };
        Suggest::IRanker::Kind strategyKinds[4] = {
            Suggest::IRanker::KIND_Frequency,
            Suggest::IRanker::KIND_TypeDirected,
            Suggest::IRanker::KIND_Hybrid,
            Suggest::IRanker::KIND_MLStub
        };
        for (i32 i = 0; i < 4; ++i) {
            if (i > 0) ImGui::SameLine();
            bool sel = (activeKind == strategyKinds[i]);
            if (ImGui::RadioButton(strategyLabels[i], sel) && !sel) {
                Suggest::SetActiveRankerKind(strategyKinds[i]);
            }
        }

        ImGui::Separator();
        ImGui::Text("Latency history");
        Common::DrawSparkline(s.latencySamples, 64, 280.0f, 40.0f,
            IM_COL32(180, 220, 200, 255));

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Score breakdown")) {
            RenderScoreBreakdown(s.lastList);
        }
        if (ImGui::CollapsingHeader("Feature histogram")) {
            RenderFeatureHistogram(s.lastList);
        }
        if (ImGui::CollapsingHeader("Weights")) {
            RenderWeightSliders();
        }
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace DebugRanker
} // namespace UI
} // namespace Vespucci
