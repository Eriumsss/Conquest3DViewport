// SearchBox.h
// =============================================================================
// COMPACT SEARCH/FILTER INPUT WITH X-CLEAR + LIVE COUNT
// =============================================================================
// Written by: Eriumsss
//
// Drop-in for any Vespucci panel that needs a filter row. Wraps
// ImGui::InputTextWithHint with the Vespucci theme + a clear-button
// + a "(N matches)" counter the panel computes externally.
// =============================================================================

#ifndef VESPUCCI_UI_COMMON_SEARCHBOX_H_
#define VESPUCCI_UI_COMMON_SEARCHBOX_H_

#include "../../Core/VespucciTypes.h"

namespace Vespucci {
namespace UI {
namespace Common {

// Returns true if the buffer changed this frame.
bool DrawSearchBox(const char* label, const char* hint,
                    char* buf, i32 bufCap, i32 matchCount);

} // namespace Common
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_COMMON_SEARCHBOX_H_
