// Panel.cpp
// =============================================================================
// Health-fix panel orchestrator. Lists broken-ref scan results, lets
// the user pick a 'Did you mean' replacement, fires through Apply.
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Apply/Apply.h"
#include "../../Apply/Undo.h"
#include "../../Suggest/SuggestionTypes.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"
#include "../Common/SearchBox.h"
#include "imgui_glue.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace HealthFix {

void RenderRepairList(PanelState& s);
void RenderDiffViewForRow(PanelState& s, ImGuiGlueFrameArgs& args);
void RenderBatchActions(PanelState& s, ImGuiGlueFrameArgs& args);
void RegenerateCandidatesForSelection(PanelState& s);

void InitHealthFixPanel(PanelState& s) {
    s.visible = true;
    s.scanResults.clear();
    s.selectedRowIdx = 0;
    s.currentCandidates.clear();
    s.filterText[0] = 0;
    s.showOnlyCriticalKinds = false;
    s.lastScanVersion = SnapshotVersion(0);
    s.showAdvanced = false; // Patrick mode default
    s.focusedEntityGuid = Guid(0);
}

void RescanIfSnapshotMoved(PanelState& s) {
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!snap) return;
    if (s.lastScanVersion == snap->version() && !s.scanResults.empty()) return;

    // The snapshot bumps version every successful build, even when no
    // entity / wire actually changed. If we wipe selectedRowIdx on
    // every bump, the user's click is overwritten before they can act
    // on it — that's the snapback bug. So we rescan, but ONLY reset
    // selection when the result set actually changed shape OR the
    // previous selection now points past the end of a shorter list.
    size_t prevSize = s.scanResults.size();
    Repair::ScanForBrokenRefs(*snap, s.scanResults);
    s.lastScanVersion = snap->version();

    bool sizeChanged = s.scanResults.size() != prevSize;
    bool idxOutOfRange = s.selectedRowIdx < 0 ||
                         s.selectedRowIdx >= (i32)s.scanResults.size();
    if (sizeChanged || idxOutOfRange) {
        s.selectedRowIdx = s.scanResults.empty() ? -1 : 0;
        s.currentCandidates.clear();
        // Auto-prime candidates for the new top row so the user sees
        // something on first open instead of an empty right pane.
        if (s.selectedRowIdx >= 0) {
            RegenerateCandidatesForSelection(s);
        }
    }
}

void RegenerateCandidatesForSelection(PanelState& s) {
    s.currentCandidates.clear();
    if (s.selectedRowIdx < 0 || s.selectedRowIdx >= (i32)s.scanResults.size()) return;
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!snap) return;
    Repair::RepairCandidate buf[5];
    i32 n = Repair::GenerateDidYouMean(*snap, s.scanResults[(size_t)s.selectedRowIdx], buf, 5);
    for (i32 i = 0; i < n; ++i) s.currentCandidates.push_back(buf[i]);
}

void RenderHealthFixPanel(PanelState& s, ImGuiGlueFrameArgs& args) {
    if (!s.visible) return;
    Common::PushVespucciStyleScope();
    if (ImGui::Begin("Vespucci Health-fix", &s.visible)) {
        RescanIfSnapshotMoved(s);

        // Header row — left side shows the count, right side has the
        // Advanced toggle so designers can drop the cockpit when needed.
        ImGui::Text("Broken wires: %d", (i32)s.scanResults.size());
        ImGui::SameLine();
        f32 toggleWidth = 96.0f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - toggleWidth);
        ImGui::Checkbox("Advanced##hf_adv", &s.showAdvanced);

        // Cross-page focused entity — name + count of broken wires
        // touching it. Designer sees "this entity has 3 broken wires"
        // at a glance no matter which page they came from.
        if (s.focusedEntityGuid.valid()) {
            Scene::SceneSnapshot* snapH = Scene::GlobalSnapshot();
            const Scene::EntityRow* focusedRow = 0;
            if (snapH) {
                Vespucci::EntityIndex fIdx = snapH->indexForGuid(s.focusedEntityGuid);
                if (fIdx.valid()) focusedRow = snapH->entityAt(fIdx);
            }
            i32 touching = 0;
            for (size_t i = 0; i < s.scanResults.size(); ++i) {
                if (s.scanResults[i].ownerGuid  == s.focusedEntityGuid ||
                    s.scanResults[i].targetGuid == s.focusedEntityGuid) touching++;
            }
            if (focusedRow) {
                if (touching > 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.45f, 1.0f),
                        "Selected: %.*s — %d broken wire%s touch this entity",
                        (int)focusedRow->name.size(), focusedRow->name.data(),
                        touching, touching == 1 ? "" : "s");
                } else {
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                        "Selected: %.*s — clean, no broken wires touch this",
                        (int)focusedRow->name.size(), focusedRow->name.data());
                }
            }
        }
        ImGui::Separator();

        // ── Patrick-mode default: the headline summary + one giant ──
        // ── "Fix everything I can" button. Designers click once,    ──
        // ── self-loops + null targets get purged, healable refs get ──
        // ── auto-rerouted to the top-1 DidYouMean candidate.        ──
        if (s.scanResults.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                "Level is clean — no broken wires found.");
        } else {
            // Tally classes for the headline.
            i32 nUnrep = 0, nHealable = 0;
            for (size_t i = 0; i < s.scanResults.size(); ++i) {
                Repair::BrokenRefKind k = s.scanResults[i].kind;
                bool unrep =
                    (k == Repair::BROKEN_Self ||
                     k == Repair::BROKEN_TargetGhost ||
                     k == Repair::BROKEN_OwnerGhost ||
                     k == Repair::BROKEN_EmptyEvent ||
                     k == Repair::BROKEN_EmptyAction);
                if (unrep) nUnrep++; else nHealable++;
            }
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.45f, 1.0f),
                "Found %d unrepairable wires (will be deleted) and %d healable wires "
                "(will be re-routed to the best candidate).",
                nUnrep, nHealable);

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.32f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.72f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.85f, 0.46f, 1.0f));
            if (ImGui::Button("Fix everything I can##hf_fix_all", ImVec2(-1.0f, 44.0f))) {
                Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
                i32 purged = 0, healed = 0;
                for (size_t i = 0; i < s.scanResults.size(); ++i) {
                    const Repair::BrokenRef& b = s.scanResults[i];
                    bool unrep =
                        (b.kind == Repair::BROKEN_Self ||
                         b.kind == Repair::BROKEN_TargetGhost ||
                         b.kind == Repair::BROKEN_OwnerGhost ||
                         b.kind == Repair::BROKEN_EmptyEvent ||
                         b.kind == Repair::BROKEN_EmptyAction);
                    if (unrep) {
                        Apply::DeleteWireOrPissOff(b.ownerGuid, b.targetGuid, args);
                        Apply::UndoOp op;
                        std::memset(&op, 0, sizeof(op));
                        op.kind        = Apply::UNDO_RecreateDeletedWire;
                        op.sourceGuid  = b.ownerGuid;
                        op.targetGuid  = b.targetGuid;
                        op.outputGuid  = b.targetGuid;
                        Apply::PushUndoOpOrChokeOnTheStackOverflow(op);
                        purged++;
                    } else if (snap) {
                        Repair::RepairCandidate cand[1];
                        i32 nc = Repair::GenerateDidYouMean(*snap, b, cand, 1);
                        if (nc > 0) {
                            Apply::DeleteWireOrPissOff(b.ownerGuid, b.targetGuid, args);
                            Suggest::Suggestion sg;
                            std::memset(&sg, 0, sizeof(sg));
                            sg.target           = cand[0].idx;
                            sg.targetGuid       = cand[0].guid;
                            sg.score            = cand[0].score;
                            sg.confidence       = cand[0].score;
                            sg.eventName        = Core::StringRef("ontrigger", 9);
                            sg.actionName       = Core::StringRef("activate", 8);
                            sg.reasonCount      = 1;
                            sg.reasons[0].text  = cand[0].reason;
                            sg.reasons[0].contribution = cand[0].score;
                            Apply::ApplySuggestionOrChokeOnGarbage(sg, b.ownerGuid, args);
                            Apply::UndoOp op;
                            std::memset(&op, 0, sizeof(op));
                            op.kind        = Apply::UNDO_DeleteAddedWire;
                            op.sourceGuid  = b.ownerGuid;
                            op.targetGuid  = cand[0].guid;
                            op.eventName   = sg.eventName;
                            op.actionName  = sg.actionName;
                            Apply::PushUndoOpOrChokeOnTheStackOverflow(op);
                            healed++;
                        }
                    }
                }
                Core::Logging::Info("HealthFix: Patrick-mode fix-all — %d purged, %d healed",
                    purged, healed);
                // Force a rescan next frame so the list updates.
                s.lastScanVersion = SnapshotVersion(0);
                s.scanResults.clear();
                s.selectedRowIdx = -1;
                s.currentCandidates.clear();
            }
            ImGui::PopStyleColor(3);

            // Tiny escape hatch even in Patrick mode.
            ImGui::Spacing();
            if (Apply::UndoStackDepth() > 0 && ImGui::SmallButton("Undo last##hf_undo_small")) {
                Apply::PopUndoOpAndFireItOrPissOff(args);
            }
        }

        // The full cockpit lives behind Advanced.
        if (!s.showAdvanced) {
            ImGui::End();
            Common::PopVespucciStyleScope();
            return;
        }

        ImGui::Separator();
        ImGui::Checkbox("Critical only", &s.showOnlyCriticalKinds);
        Common::DrawSearchBox("##hf_search", "filter (name or guid)",
            s.filterText, sizeof(s.filterText), (i32)s.scanResults.size());
        ImGui::Separator();

        // Two-column: list on left (where clicks change selectedRowIdx),
        // diff/repair on right (which CONSUMES the selection). We capture
        // prevIdx around the LEFT pane because that's where selection
        // actually mutates; if it changed we regenerate candidates BEFORE
        // the right pane renders so DiffView sees the fresh list. The old
        // version captured prev around the right pane and never fired.
        i32 prevIdx = s.selectedRowIdx;
        if (ImGui::BeginChild("##hf_left", ImVec2(380, 0), true)) {
            RenderRepairList(s);
        }
        ImGui::EndChild();
        if (prevIdx != s.selectedRowIdx) {
            RegenerateCandidatesForSelection(s);
        }
        ImGui::SameLine();
        if (ImGui::BeginChild("##hf_right", ImVec2(0, 0), true)) {
            RenderDiffViewForRow(s, args);
        }
        ImGui::EndChild();

        ImGui::Separator();
        RenderBatchActions(s, args);
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace HealthFix
} // namespace UI
} // namespace Vespucci
