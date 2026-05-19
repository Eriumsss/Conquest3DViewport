// ScoreBar.h
// =============================================================================
// HORIZONTAL SCORE BAR - GRADIENT FILL FOR THE SUGGESTION CONFIDENCE
// =============================================================================
// Written by: Eriumsss
//
// Tiny 4-pixel-tall gradient bar under each suggestion. Red→Yellow→
// Green from low confidence to high. Low cost; the sidebar runs five
// of these per frame.
// =============================================================================

#ifndef VESPUCCI_UI_COMMON_SCOREBAR_H_
#define VESPUCCI_UI_COMMON_SCOREBAR_H_

#include "../../Core/VespucciTypes.h"
#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace Common {

void DrawScoreBar(f32 score01, f32 width, f32 height = 4.0f);

} // namespace Common
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_COMMON_SCOREBAR_H_
