// StatusBarHints.cpp
// =============================================================================
// STATUS-BAR HINT - SINGLE-LINE 'TOP-1 SUGGESTION' DOCKED AT EDITOR BOTTOM
// =============================================================================
// Written by: Eriumsss
//
// The most minimal display strategy. Shows ONE line at the bottom
// of the editor: '[Hint] CapturePoint_03 -> ScoreManager (0.87) -
// click to apply'. Click commits, F1 cycles the next candidate,
// Esc dismisses. Used by power-users who already know what the
// ranker tends to suggest and just want one-keystroke accept.
// =============================================================================

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <cstdio>

namespace Vespucci {
namespace UI {
namespace Modes {

struct StatusBarState {
    i32  rotateIdx;
    bool dismissed;
};

static StatusBarState s_sb = { 0, false };

void RenderStatusBarHint(const Suggest::SuggestionList& list) {
    if (list.count == 0 || s_sb.dismissed) return;
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!snap) return;
    if (s_sb.rotateIdx >= list.count) s_sb.rotateIdx = 0;
    const Suggest::Suggestion& sg = list.suggestions[s_sb.rotateIdx];
    const Scene::EntityRow* tgt = snap->entityAt(sg.target);
    const char* name = tgt ? tgt->name.data() : "(unknown)";
    i32 nameLen = tgt ? (i32)tgt->name.size() : 9;
    ImVec2 io = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, io.y - 24.0f));
    ImGui::SetNextWindowSize(ImVec2(io.x, 24.0f));
    if (ImGui::Begin("##vesp_statusbar", 0,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus))
    {
        ImGui::Text("Hint: ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
            "%.*s  (%.2f)", nameLen, name, sg.score);
        ImGui::SameLine();
        ImGui::TextDisabled("(F1 next, Esc dismiss, Enter apply)");
    }
    ImGui::End();

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) s_sb.rotateIdx = (s_sb.rotateIdx + 1) % list.count;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) s_sb.dismissed = true;
}

void ResetStatusBarOnLevelChange() {
    s_sb.rotateIdx = 0;
    s_sb.dismissed = false;
}

} // namespace Modes
} // namespace UI
} // namespace Vespucci
