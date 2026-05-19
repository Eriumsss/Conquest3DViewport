// FeatureHistogram.cpp
// =============================================================================
// HISTOGRAM OF SCORE DISTRIBUTION ACROSS THE LAST QUERY
// =============================================================================
// Written by: Eriumsss

#include "Overlay.h"

#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace DebugRanker {

void RenderFeatureHistogram(const Suggest::SuggestionList& list) {
    if (list.count == 0) {
        ImGui::TextDisabled("(no data)");
        return;
    }
    f32 buckets[10] = {0,0,0,0,0,0,0,0,0,0};
    for (i32 i = 0; i < list.count; ++i) {
        f32 s = list.suggestions[i].score;
        if (s < 0.0f) s = 0.0f; if (s > 1.0f) s = 1.0f;
        i32 b = (i32)(s * 9.999f);
        if (b > 9) b = 9;
        buckets[b] += 1.0f;
    }
    f32 maxV = 1.0f;
    for (i32 i = 0; i < 10; ++i) if (buckets[i] > maxV) maxV = buckets[i];

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    f32 barW = 24.0f;
    f32 maxH = 60.0f;
    for (i32 i = 0; i < 10; ++i) {
        f32 h = (buckets[i] / maxV) * maxH;
        ImVec2 a(origin.x + i * (barW + 2), origin.y + (maxH - h));
        ImVec2 b(a.x + barW, origin.y + maxH);
        dl->AddRectFilled(a, b, IM_COL32(120, 180, 220, 220));
    }
    ImGui::Dummy(ImVec2(10 * (barW + 2), maxH + 4));
}

} // namespace DebugRanker
} // namespace UI
} // namespace Vespucci
