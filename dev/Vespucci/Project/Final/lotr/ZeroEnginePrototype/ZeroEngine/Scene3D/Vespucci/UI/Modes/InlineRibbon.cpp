// InlineRibbon.cpp
// =============================================================================
// COMPACT INLINE RIBBON - SHOWS TOP-3 SUGGESTIONS AS A HORIZONTAL STRIP
// UNDER THE CURRENT FORGE TAB BAR. ALTERNATIVE TO THE FULL SIDEBAR.
// =============================================================================
// Written by: Eriumsss
//
// Some designers prefer their workspace uncluttered. The Modes/
// alternative-display-strategies set lets them swap the sidebar for
// a thin ribbon, or a status-bar hint, or a floating HUD - whatever
// matches their flow. All three modes pull from the same
// SuggestionList; only the rendering shape differs.
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

void RenderInlineRibbon(const Suggest::SuggestionList& list) {
    if (list.count == 0) return;
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!snap) return;

    Common::PushVespucciStyleScope();
    if (ImGui::BeginChild("##vesp_ribbon", ImVec2(0, 60.0f), true,
                           ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::Text("Suggested:");
        ImGui::SameLine();
        i32 cap = list.count > 3 ? 3 : list.count;
        for (i32 i = 0; i < cap; ++i) {
            const Suggest::Suggestion& sg = list.suggestions[i];
            const Scene::EntityRow* tgt = snap->entityAt(sg.target);
            const char* name = tgt ? tgt->name.data() : "(unknown)";
            i32 nameLen = tgt ? (i32)tgt->name.size() : 9;
            char btn[200];
            std::snprintf(btn, sizeof(btn), "%.*s##rib_%d", nameLen, name, i);
            if (ImGui::Button(btn)) {
                // Click commits via the panel's apply path. For V1
                // ribbon, we just log; the Forge toggle wires this
                // through Apply::ApplySuggestionOrChokeOnGarbage in
                // Phase N init.
            }
            ImGui::SameLine();
            Common::DrawScoreBar(sg.score, 60.0f, 3.0f);
            ImGui::SameLine();
        }
    }
    ImGui::EndChild();
    Common::PopVespucciStyleScope();
}

} // namespace Modes
} // namespace UI
} // namespace Vespucci
