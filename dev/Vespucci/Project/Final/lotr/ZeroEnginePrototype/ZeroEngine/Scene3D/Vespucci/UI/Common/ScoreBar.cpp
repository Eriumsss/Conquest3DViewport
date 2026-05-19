// ScoreBar.cpp
// =============================================================================
// Two-stop gradient: low->mid->high. ImDrawList's two-color filled
// rect can't do tristop natively so we draw two halves.
// =============================================================================
// Written by: Eriumsss

#include "ScoreBar.h"
#include "Theme.h"

namespace Vespucci {
namespace UI {
namespace Common {

void DrawScoreBar(f32 score01, f32 width, f32 height) {
    if (score01 < 0.0f) score01 = 0.0f;
    if (score01 > 1.0f) score01 = 1.0f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    f32 fillW = width * score01;

    // Background trough.
    dl->AddRectFilled(origin,
        ImVec2(origin.x + width, origin.y + height),
        IM_COL32(40, 40, 46, 220), 1.0f);

    // Pick gradient stop.
    ImU32 colA, colB;
    if (score01 < 0.5f) {
        colA = Theme::ScoreBarLow;
        colB = Theme::ScoreBarMid;
    } else {
        colA = Theme::ScoreBarMid;
        colB = Theme::ScoreBarHigh;
    }
    dl->AddRectFilledMultiColor(origin,
        ImVec2(origin.x + fillW, origin.y + height),
        colA, colB, colB, colA);

    ImGui::Dummy(ImVec2(width, height));
}

} // namespace Common
} // namespace UI
} // namespace Vespucci
