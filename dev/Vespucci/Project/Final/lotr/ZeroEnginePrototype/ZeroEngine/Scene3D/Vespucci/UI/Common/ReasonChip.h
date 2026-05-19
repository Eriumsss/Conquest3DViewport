// ReasonChip.h
// =============================================================================
// REASON CHIP - SMALL COLORED BADGE NEXT TO EACH SUGGESTION
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked: every suggestion ships with up to 3 reason chips so
// the designer never sees a black-box score. This file renders ONE
// chip - a rounded pill with reason text + a confidence-tinted
// background. The Suggested Wires sidebar lays them out horizontally;
// the HealthFix panel uses them in a stack.
// =============================================================================

#ifndef VESPUCCI_UI_COMMON_REASONCHIP_H_
#define VESPUCCI_UI_COMMON_REASONCHIP_H_

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"
#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace Common {

// Render a single reason chip starting at the current ImGui cursor.
// Returns the size occupied so the caller can lay them out
// horizontally without a full ItemRect query.
ImVec2 DrawReasonChip(const Suggest::ReasonChip& chip);

// Render a horizontal row of up to N chips, wrapping at maxWidth.
void DrawChipRowWithWrap(const Suggest::ReasonChip* chips, i32 count,
                          f32 maxWidth);

} // namespace Common
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_COMMON_REASONCHIP_H_
