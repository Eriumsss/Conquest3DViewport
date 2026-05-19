// Sparkline.h
// =============================================================================
// MINI INLINE GRAPH FOR THE DEBUG OVERLAY - LATENCY HISTOGRAM ETC
// =============================================================================
// Written by: Eriumsss
//
// 16-32 sample inline line graph. Used by the Brain debug tab to
// show ranker-latency history, suggestion-show rate, accept rate.
// Cheap: one line strip per sparkline, no axes, no labels.
// =============================================================================

#ifndef VESPUCCI_UI_COMMON_SPARKLINE_H_
#define VESPUCCI_UI_COMMON_SPARKLINE_H_

#include "../../Core/VespucciTypes.h"
#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace Common {

void DrawSparkline(const f32* samples, i32 count,
                   f32 width, f32 height,
                   ImU32 lineColor);

} // namespace Common
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_COMMON_SPARKLINE_H_
