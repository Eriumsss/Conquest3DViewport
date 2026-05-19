// AcceptRejectButtons.cpp
// =============================================================================
// THE FOUR BUTTONS UNDER A SELECTED SUGGESTION: PREVIEW / APPLY / IGNORE / WHY
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Apply/Apply.h"
#include "../../Apply/Preview.h"
#include "../../Apply/Undo.h"
#include "../../Core/Logging.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Tooltip.h"
#include "../Common/Theme.h"
#include "imgui_glue.h"

namespace Vespucci {
namespace UI {
namespace SuggestedWires {

namespace {
    // Tactical-named action helpers. These are the verbs the panel
    // dispatches to when a button fires.
    bool DeployPreviewStaging(PanelState& s, const Suggest::Suggestion& sg) {
        Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
        if (!snap) return false;
        Apply::StageWireOrPushItOffTheCliff(sg, s.focusedSourceGuid, snap->version());
        Core::Logging::Info("SuggestedWires: previewed %d staged edits",
            Apply::StagedEditCount());
        return true;
    }
    bool ExecuteCommitOnSuggestion(PanelState& s, const Suggest::Suggestion& sg,
                                    ImGuiGlueFrameArgs& args)
    {
        if (!Apply::ApplySuggestionOrChokeOnGarbage(sg, s.focusedSourceGuid, args)) return false;
        // Push undo op.
        Apply::UndoOp op;
        op.kind        = Apply::UNDO_DeleteAddedWire;
        op.sourceGuid  = s.focusedSourceGuid;
        op.targetGuid  = sg.targetGuid;
        op.outputGuid  = Guid(0); // host fills chainAddLastNewGuid next frame
        op.entityGuid  = Guid(0);
        op.eventName   = sg.eventName;
        op.actionName  = sg.actionName;
        op.delay       = sg.suggestedDelay;
        op.sticky      = sg.suggestedSticky;
        Apply::PushUndoOpOrChokeOnTheStackOverflow(op);
        return true;
    }
    void RenderWhyTooltipOnHover(const Suggest::Suggestion& sg) {
        if (!ImGui::IsItemHovered()) return;
        char hdrBuf[128]; std::snprintf(hdrBuf, sizeof(hdrBuf),
            "Why this target? (score %.3f, conf %.3f)", sg.score, sg.confidence);
        Common::TooltipBuilder tb;
        tb.SetHeader(hdrBuf);
        tb.AddBodyLine("The ranker picked this target because of the chips below.");
        tb.AddReasonChips(sg.reasons, sg.reasonCount);
        tb.AddKeyValue("event",  sg.eventName.data());
        tb.AddKeyValue("action", sg.actionName.data());
        tb.Render();
    }
} // namespace

void RenderButtonsForSuggestion(PanelState& s, const Suggest::Suggestion& sg,
                                 ImGuiGlueFrameArgs& args)
{
    if (ImGui::Button("Preview", ImVec2(70, 0))) {
        DeployPreviewStaging(s, sg);
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply", ImVec2(70, 0))) {
        ExecuteCommitOnSuggestion(s, sg, args);
    }
    ImGui::SameLine();
    if (ImGui::Button("Ignore", ImVec2(70, 0))) {
        Core::Logging::Debug("SuggestedWires: user ignored target 0x%08X",
            (unsigned)sg.targetGuid.raw);
    }
    ImGui::SameLine();
    ImGui::Button("Why?", ImVec2(60, 0));
    RenderWhyTooltipOnHover(sg);
}

} // namespace SuggestedWires
} // namespace UI
} // namespace Vespucci
