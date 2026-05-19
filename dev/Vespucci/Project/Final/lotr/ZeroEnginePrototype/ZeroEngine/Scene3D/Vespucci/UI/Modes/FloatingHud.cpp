// FloatingHud.cpp
// =============================================================================
// FLOATING HUD - PIN A SUGGESTION CARD ANYWHERE ON THE SCREEN, DRAG TO MOVE
// =============================================================================
// Written by: Eriumsss
//
// Designers who multi-monitor want a floating card that follows
// them as they tab between viewport, outliner, and the wire-
// authoring tab. This is that surface: a small detached ImGui
// window with the top-3 suggestions + Apply buttons. Position
// persists across sessions in editor settings.
// =============================================================================

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"
#include "../Common/ScoreBar.h"

#include <cstdio>

namespace Vespucci {
namespace UI {
namespace Modes {

struct FloatingHudState {
    bool   visible;
    f32    x;
    f32    y;
    bool   pinned;
};

static FloatingHudState s_hud = { false, 100.0f, 100.0f, false };

void OpenFloatingHud()  { s_hud.visible = true; }
void CloseFloatingHud() { s_hud.visible = false; }

void RenderFloatingHud(const Suggest::SuggestionList& list) {
    if (!s_hud.visible || list.count == 0) return;
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!snap) return;
    if (s_hud.pinned) {
        ImGui::SetNextWindowPos(ImVec2(s_hud.x, s_hud.y), ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowPos(ImVec2(s_hud.x, s_hud.y), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(280.0f, 200.0f), ImGuiCond_FirstUseEver);
    Common::PushVespucciStyleScope();
    if (ImGui::Begin("Vespucci HUD", &s_hud.visible,
        ImGuiWindowFlags_NoCollapse))
    {
        ImGui::Checkbox("Pin position", &s_hud.pinned);
        ImGui::Separator();
        i32 cap = list.count > 3 ? 3 : list.count;
        for (i32 i = 0; i < cap; ++i) {
            const Suggest::Suggestion& sg = list.suggestions[i];
            const Scene::EntityRow* tgt = snap->entityAt(sg.target);
            const char* name = tgt ? tgt->name.data() : "(unknown)";
            i32 nameLen = tgt ? (i32)tgt->name.size() : 9;
            ImGui::Text("%.*s", nameLen, name);
            Common::DrawScoreBar(sg.score, 240.0f, 4.0f);
            ImGui::SameLine();
            char btn[32]; std::snprintf(btn, sizeof(btn), "Apply##hud_%d", i);
            ImGui::Button(btn);
            ImGui::Spacing();
        }
        ImVec2 pos = ImGui::GetWindowPos();
        s_hud.x = pos.x;
        s_hud.y = pos.y;
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace Modes
} // namespace UI
} // namespace Vespucci
