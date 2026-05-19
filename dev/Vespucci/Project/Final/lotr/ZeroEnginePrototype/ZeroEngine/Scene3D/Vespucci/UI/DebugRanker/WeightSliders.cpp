// WeightSliders.cpp
// =============================================================================
// HOT-RELOADABLE RANKER WEIGHT TUNING SLIDERS
// =============================================================================
// Written by: Eriumsss

#include "../../../imgui/imgui.h"

#include "../../Core/Logging.h"

namespace Vespucci {
namespace UI {
namespace DebugRanker {

// File-static weights mirror; the HybridRanker re-reads these between
// queries via a hook the Weight loader registers in Phase N init.
struct LiveWeights {
    f32 typePrior;
    f32 eventPrior;
    f32 nameSimilarity;
    f32 spatialProximity;
    f32 sameLayer;
    f32 sameParent;
    f32 recency;
    f32 overloadPenalty;
};

static LiveWeights s_w = { 0.30f, 0.25f, 0.10f, 0.08f, 0.10f, 0.05f, 0.12f, 0.10f };

void RenderWeightSliders() {
    bool changed = false;
    changed |= ImGui::SliderFloat("Type prior",        &s_w.typePrior,        0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Event prior",       &s_w.eventPrior,       0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Name similarity",   &s_w.nameSimilarity,   0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Spatial proximity", &s_w.spatialProximity, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Same layer",        &s_w.sameLayer,        0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Same parent",       &s_w.sameParent,       0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Recency",           &s_w.recency,          0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Overload penalty",  &s_w.overloadPenalty,  0.0f, 1.0f);
    if (changed) {
        Core::Logging::Debug("DebugRanker: weights changed live (next query uses new values)");
    }
    if (ImGui::Button("Save to weights.json")) {
        Core::Logging::Info("DebugRanker: save weights.json - hook lands in Phase N");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        LiveWeights def = { 0.30f, 0.25f, 0.10f, 0.08f, 0.10f, 0.05f, 0.12f, 0.10f };
        s_w = def;
    }
}

} // namespace DebugRanker
} // namespace UI
} // namespace Vespucci
