// Sparkline.cpp
// =============================================================================
// Render a sparkline as a polyline. Auto-scales to the local min/max
// so a flat run of 0.99 doesn't blow up into a noisy curve.
// =============================================================================
// Written by: Eriumsss

#include "Sparkline.h"
#include "Theme.h"

namespace Vespucci {
namespace UI {
namespace Common {

void DrawSparkline(const f32* samples, i32 count, f32 width, f32 height, ImU32 col) {
    if (!samples || count <= 0 || width <= 0 || height <= 0) return;
    f32 mn = samples[0], mx = samples[0];
    for (i32 i = 1; i < count; ++i) {
        if (samples[i] < mn) mn = samples[i];
        if (samples[i] > mx) mx = samples[i];
    }
    if (mx - mn < 1e-6f) mx = mn + 1.0f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background trough.
    dl->AddRectFilled(origin,
        ImVec2(origin.x + width, origin.y + height),
        Theme::BgSubpanel, 0.0f);

    f32 stepX = width / (f32)(count - 1 > 0 ? count - 1 : 1);
    for (i32 i = 1; i < count; ++i) {
        f32 yA = origin.y + height - ((samples[i-1] - mn) / (mx - mn)) * height;
        f32 yB = origin.y + height - ((samples[i  ] - mn) / (mx - mn)) * height;
        f32 xA = origin.x + (f32)(i - 1) * stepX;
        f32 xB = origin.x + (f32) i      * stepX;
        dl->AddLine(ImVec2(xA, yA), ImVec2(xB, yB), col, 1.5f);
    }

    ImGui::Dummy(ImVec2(width, height));
}

} // namespace Common
} // namespace UI
} // namespace Vespucci
