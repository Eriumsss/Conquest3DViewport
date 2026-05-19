// BatchActions.cpp
// =============================================================================
// BATCH OPERATIONS FOR THE HEALTH-FIX PANEL - AUTO-HEAL, RESCAN, CLEAR
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Apply/Apply.h"
#include "../../Apply/Undo.h"
#include "../../Repair/HealPlanner.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"
#include "imgui_glue.h"

#include <cstring>
#include <vector>

namespace Vespucci {
namespace UI {
namespace HealthFix {

void RenderBatchActions(PanelState& s, ImGuiGlueFrameArgs& args) {
    if (s.scanResults.empty()) return;

    if (ImGui::Button("Auto-heal top candidates")) {
        Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
        if (snap) {
            std::vector<Repair::HealStep> steps;
            steps.reserve(s.scanResults.size());
            for (size_t i = 0; i < s.scanResults.size(); ++i) {
                Repair::RepairCandidate buf[1];
                i32 n = Repair::GenerateDidYouMean(*snap, s.scanResults[i], buf, 1);
                if (n == 0) continue;
                Repair::HealStep step;
                step.broken      = s.scanResults[i];
                step.replacement = buf[0];
                steps.push_back(step);
            }
            Repair::PlanHealOrderOrChokeOnConflicts(steps);
            if (!steps.empty()) {
                Apply::DeleteWireOrPissOff(steps[0].broken.ownerGuid,
                    steps[0].broken.targetGuid, args);
                Suggest::Suggestion sg;
                std::memset(&sg, 0, sizeof(sg));
                sg.targetGuid = steps[0].replacement.guid;
                sg.eventName  = Core::StringRef("ontrigger", 9);
                sg.actionName = Core::StringRef("activate", 8);
                Apply::ApplySuggestionOrChokeOnGarbage(sg,
                    steps[0].broken.ownerGuid, args);
            }
            Core::Logging::Info("HealthFix: queued %zu auto-heal steps", steps.size());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto-purge unrepairable")) {
        // Sweep every broken-ref class that DidYouMean cannot meaningfully
        // suggest a replacement for — self-loops, null-target ghosts, empty
        // event/action wires — and delete them in one shot. Each delete
        // pushes its own undo op so the designer can roll back individually.
        i32 purged = 0;
        for (size_t i = 0; i < s.scanResults.size(); ++i) {
            const Repair::BrokenRef& b = s.scanResults[i];
            bool unrepairable =
                (b.kind == Repair::BROKEN_Self ||
                 b.kind == Repair::BROKEN_TargetGhost ||
                 b.kind == Repair::BROKEN_OwnerGhost ||
                 b.kind == Repair::BROKEN_EmptyEvent ||
                 b.kind == Repair::BROKEN_EmptyAction);
            if (!unrepairable) continue;
            Apply::DeleteWireOrPissOff(b.ownerGuid, b.targetGuid, args);
            Apply::UndoOp op;
            std::memset(&op, 0, sizeof(op));
            op.kind        = Apply::UNDO_RecreateDeletedWire;
            op.sourceGuid  = b.ownerGuid;
            op.targetGuid  = b.targetGuid;
            op.outputGuid  = b.targetGuid;
            Apply::PushUndoOpOrChokeOnTheStackOverflow(op);
            purged++;
        }
        if (purged > 0) {
            s.lastScanVersion = SnapshotVersion(0);
            s.scanResults.clear();
            s.selectedRowIdx = -1;
            s.currentCandidates.clear();
        }
        Core::Logging::Info("HealthFix: auto-purged %d unrepairable wires", purged);
    }
    ImGui::SameLine();
    if (ImGui::Button("Force rescan")) {
        s.lastScanVersion = SnapshotVersion(0);
        s.scanResults.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear selection")) {
        s.selectedRowIdx = -1;
        s.currentCandidates.clear();
    }
}

} // namespace HealthFix
} // namespace UI
} // namespace Vespucci
