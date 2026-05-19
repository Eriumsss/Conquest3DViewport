// ReasonStripe.cpp
// =============================================================================
// ALTERNATE COMPACT LAYOUT - SHOW REASON CHIPS AS A HORIZONTAL STRIPE
// UNDER THE SUGGESTION ROW. USED BY THE COMPACT-DENSITY VARIANT OF THE
// SIDEBAR (SETTINGS TOGGLE).
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Suggest/SuggestionTypes.h"
#include "../Common/ReasonChip.h"
#include "../Common/Theme.h"
#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace SuggestedWires {

void RenderReasonStripeForSuggestion(const Suggest::Suggestion& sg, f32 maxWidth) {
    if (sg.reasonCount <= 0) return;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 1.0f));
    Common::DrawChipRowWithWrap(sg.reasons, sg.reasonCount, maxWidth);
    ImGui::PopStyleVar();
}

} // namespace SuggestedWires
} // namespace UI
} // namespace Vespucci
