// DiffView.cpp
// =============================================================================
// DIFF VIEW - SHOW THE BROKEN WIRE'S BEFORE/AFTER NEXT TO CANDIDATE LIST
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Apply/Apply.h"
#include "../../Apply/Undo.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"
#include "imgui_glue.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace HealthFix {

void RenderDiffViewForRow(PanelState& s, ImGuiGlueFrameArgs& args) {
    if (s.scanResults.empty() || s.selectedRowIdx < 0 ||
        s.selectedRowIdx >= (i32)s.scanResults.size())
    {
        ImGui::TextDisabled("(select a row on the left to see repair candidates)");
        return;
    }
    const Repair::BrokenRef& b = s.scanResults[(size_t)s.selectedRowIdx];
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!snap) return;

    ImGui::Text("Broken wire");
    ImGui::Separator();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "owner: 0x%08X", (unsigned)b.ownerGuid.raw);
    ImGui::TextDisabled("%s", buf);
    std::snprintf(buf, sizeof(buf), "target: 0x%08X (broken)", (unsigned)b.targetGuid.raw);
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", buf);
    if (b.detail.size() > 0) {
        ImGui::TextWrapped("%.*s", (int)b.detail.size(), b.detail.data());
    }

    // Some broken-ref classes have no fuzzy-replacement story. Self-loops
    // have no intended target to reverse-engineer. Ghost targets are
    // 0x00000000 — no name or type for the ranker to chew on. Empty
    // event/action wires fire nothing meaningful. The only sane move
    // is to nuke the wire and move on, so we surface a Delete button
    // here for those classes.
    bool kindIsUnrepairable =
        (b.kind == Repair::BROKEN_Self ||
         b.kind == Repair::BROKEN_TargetGhost ||
         b.kind == Repair::BROKEN_OwnerGhost ||
         b.kind == Repair::BROKEN_EmptyEvent ||
         b.kind == Repair::BROKEN_EmptyAction);

    if (kindIsUnrepairable) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.22f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.90f, 0.26f, 0.26f, 1.0f));
        if (ImGui::Button("Delete this wire (no replacement)##hf_delete_solo")) {
            Apply::DeleteWireOrPissOff(b.ownerGuid, b.targetGuid, args);
            // Push undo so a panicked designer can take it back.
            Apply::UndoOp op;
            std::memset(&op, 0, sizeof(op));
            op.kind        = Apply::UNDO_RecreateDeletedWire;
            op.sourceGuid  = b.ownerGuid;
            op.targetGuid  = b.targetGuid;
            op.outputGuid  = b.targetGuid;
            Apply::PushUndoOpOrChokeOnTheStackOverflow(op);
            Core::Logging::Info("HealthFix: deleted unrepairable wire %u -> %u (kind=%d)",
                (unsigned)b.ownerGuid.raw, (unsigned)b.targetGuid.raw, (int)b.kind);
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextDisabled("(no fuzzy candidate makes sense for this class)");
    }

    ImGui::Spacing();
    ImGui::Text("Did you mean...");
    ImGui::Separator();

    if (s.currentCandidates.empty()) {
        ImGui::TextDisabled("(no candidate replacements found - "
                              "try widening the layer / spatial scope)");
        return;
    }

    for (i32 i = 0; i < (i32)s.currentCandidates.size(); ++i) {
        const Repair::RepairCandidate& c = s.currentCandidates[(size_t)i];
        const Scene::EntityRow* tgt = snap->entityAt(c.idx);
        const char* tgtName = tgt ? tgt->name.data() : "(unknown)";
        i32 tgtNameLen = tgt ? (i32)tgt->name.size() : 9;
        char rowLabel[200];
        std::snprintf(rowLabel, sizeof(rowLabel),
            "%.*s   (score %.2f)##hf_cand_%d",
            tgtNameLen, tgtName, c.score, i);
        if (ImGui::Selectable(rowLabel, false)) {
            // Fire repair: delete broken wire + chain-add new wire.
            Apply::DeleteWireOrPissOff(b.ownerGuid, b.targetGuid, args);
            // Synthesize a Suggestion to apply.
            Suggest::Suggestion sg;
            std::memset(&sg, 0, sizeof(sg));
            sg.target           = c.idx;
            sg.targetGuid       = c.guid;
            sg.score            = c.score;
            sg.confidence       = c.score;
            sg.eventName        = Core::StringRef("ontrigger", 9);
            sg.actionName       = Core::StringRef("activate", 8);
            sg.suggestedDelay   = 0.0f;
            sg.suggestedSticky  = false;
            sg.reasonCount      = 1;
            sg.reasons[0].text  = c.reason;
            sg.reasons[0].contribution = c.score;
            Apply::ApplySuggestionOrChokeOnGarbage(sg, b.ownerGuid, args);

            // Push undo op.
            Apply::UndoOp op;
            op.kind        = Apply::UNDO_DeleteAddedWire;
            op.sourceGuid  = b.ownerGuid;
            op.targetGuid  = c.guid;
            op.outputGuid  = Guid(0);
            op.eventName   = sg.eventName;
            op.actionName  = sg.actionName;
            op.delay       = 0.0f;
            op.sticky      = false;
            Apply::PushUndoOpOrChokeOnTheStackOverflow(op);
        }
        if (c.reason.size() > 0) {
            ImGui::TextDisabled("  %.*s", (int)c.reason.size(), c.reason.data());
        }
    }
}

} // namespace HealthFix
} // namespace UI
} // namespace Vespucci
