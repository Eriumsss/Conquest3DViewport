// RepairList.cpp
// =============================================================================
// LIST RENDERER FOR THE HEALTH-FIX PANEL'S LEFT-SIDE COLUMN
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../../imgui/imgui.h"

#include <cstdio>

namespace Vespucci {
namespace UI {
namespace HealthFix {

namespace {
    const char* KindLabel(Repair::BrokenRefKind k) {
        switch (k) {
            case Repair::BROKEN_TargetGhost:      return "GHOST_TGT";
            case Repair::BROKEN_OwnerGhost:       return "GHOST_OWN";
            case Repair::BROKEN_EmptyEvent:       return "EMPTY_EVT";
            case Repair::BROKEN_EmptyAction:      return "EMPTY_ACT";
            case Repair::BROKEN_Self:             return "SELF_LOOP";
            case Repair::BROKEN_DeprecatedTarget: return "DEPRECATED";
        }
        return "?";
    }
    bool IsCritical(Repair::BrokenRefKind k) {
        return k == Repair::BROKEN_TargetGhost || k == Repair::BROKEN_OwnerGhost ||
               k == Repair::BROKEN_Self;
    }
} // namespace

void RenderRepairList(PanelState& s) {
    if (s.scanResults.empty()) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
            "No broken refs detected. The level is clean.");
        return;
    }
    for (i32 i = 0; i < (i32)s.scanResults.size(); ++i) {
        const Repair::BrokenRef& b = s.scanResults[(size_t)i];
        if (s.showOnlyCriticalKinds && !IsCritical(b.kind)) continue;
        char rowLabel[200];
        std::snprintf(rowLabel, sizeof(rowLabel),
            "[%s] 0x%08X -> 0x%08X##hf_row_%d",
            KindLabel(b.kind),
            (unsigned)b.ownerGuid.raw, (unsigned)b.targetGuid.raw,
            i);
        bool selected = (i == s.selectedRowIdx);
        if (ImGui::Selectable(rowLabel, selected)) {
            s.selectedRowIdx = i;
        }
        if (selected && b.detail.size() > 0) {
            ImGui::TextDisabled("%.*s", (int)b.detail.size(), b.detail.data());
        }
    }
}

} // namespace HealthFix
} // namespace UI
} // namespace Vespucci
