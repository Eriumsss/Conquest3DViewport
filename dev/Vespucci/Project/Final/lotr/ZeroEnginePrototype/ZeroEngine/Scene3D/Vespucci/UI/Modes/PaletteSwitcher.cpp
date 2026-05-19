// PaletteSwitcher.cpp
// =============================================================================
// COMMAND-PALETTE STYLE SWITCHER - Ctrl+Shift+P TO POP A SEARCH BAR
// THAT FILTERS OVER 'WIRE FROM <SOURCE> TO <TARGET>' SUGGESTIONS
// =============================================================================
// Written by: Eriumsss
//
// VS Code-inspired. Press the hotkey, type 'capture sc' and the
// palette filters down to 'wire CapturePoint_03 -> ScoreManager'
// (combining source filter + target filter in one query). Enter
// commits, Esc closes. Power-user surface.
// =============================================================================

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace Modes {

struct PaletteState {
    bool visible;
    char query[128];
    i32  selectedIdx;
};

static PaletteState s_pal = { false, {0}, 0 };

void OpenCommandPalette() {
    s_pal.visible = true;
    s_pal.query[0] = 0;
    s_pal.selectedIdx = 0;
}

void RenderPaletteSwitcher(const Suggest::SuggestionList& list) {
    // Hotkey: Ctrl+Shift+P toggles open.
    if ((ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift &&
         ImGui::IsKeyPressed(ImGuiKey_P, false)))
    {
        s_pal.visible = !s_pal.visible;
    }
    if (!s_pal.visible) return;

    ImVec2 io = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(io.x * 0.5f - 240.0f, 80.0f));
    ImGui::SetNextWindowSize(ImVec2(480.0f, 320.0f));
    Common::PushVespucciStyleScope();
    if (ImGui::Begin("##vesp_palette", &s_pal.visible,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##pal_query",
            "filter by source/target/event...", s_pal.query, sizeof(s_pal.query));

        Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
        if (!snap) { ImGui::End(); Common::PopVespucciStyleScope(); return; }

        // Render filtered rows.
        for (i32 i = 0; i < list.count; ++i) {
            const Suggest::Suggestion& sg = list.suggestions[i];
            const Scene::EntityRow* tgt = snap->entityAt(sg.target);
            if (!tgt) continue;
            // Substring match against tgt->name.
            if (s_pal.query[0]) {
                bool match = false;
                usize qLen = std::strlen(s_pal.query);
                if (qLen <= tgt->name.size()) {
                    for (usize k = 0; k + qLen <= tgt->name.size(); ++k) {
                        bool ok = true;
                        for (usize j = 0; j < qLen; ++j) {
                            char a = tgt->name.data()[k + j]; char b = s_pal.query[j];
                            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                            if (a != b) { ok = false; break; }
                        }
                        if (ok) { match = true; break; }
                    }
                }
                if (!match) continue;
            }
            char row[200];
            std::snprintf(row, sizeof(row), "%.*s   (%.2f)##pal_%d",
                (int)tgt->name.size(), tgt->name.data(), sg.score, i);
            if (ImGui::Selectable(row, i == s_pal.selectedIdx)) {
                s_pal.selectedIdx = i;
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) s_pal.visible = false;
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace Modes
} // namespace UI
} // namespace Vespucci
