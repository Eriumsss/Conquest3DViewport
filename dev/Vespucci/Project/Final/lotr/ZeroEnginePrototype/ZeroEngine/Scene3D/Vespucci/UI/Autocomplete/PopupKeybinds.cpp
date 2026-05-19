// PopupKeybinds.cpp
// =============================================================================
// KEYBIND PRIMITIVES - CENTRALIZE THE ARROW/ENTER/ESC HANDLING SO THE
// HEALTHFIX, SUGGESTEDWIRES, AND COMPATEDITOR PANELS CAN REUSE THE EXACT
// SAME COMMIT-CANCEL UX WITHOUT REIMPLEMENTING THE LISTENERS PER FILE
// =============================================================================
// Written by: Eriumsss
//
// ImGui keybind state is global per-frame, so any panel that needs
// "arrow keys move the selection, Enter commits, Esc cancels" can
// call DispatchListNavKeys() and get a normalized command back. We
// do NOT bake gameplay-side modifier-key behavior in here - that
// lives in the panel that owns the binding.
// =============================================================================

#include "../../Core/VespucciTypes.h"
#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace Autocomplete {

enum NavCommand {
    NAV_None       = 0,
    NAV_MoveUp     = 1,
    NAV_MoveDown   = 2,
    NAV_PageUp     = 3,
    NAV_PageDown   = 4,
    NAV_JumpHome   = 5,
    NAV_JumpEnd    = 6,
    NAV_Commit     = 7,
    NAV_Cancel     = 8
};

NavCommand DispatchListNavKeys() {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return NAV_Cancel;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter,  false)) return NAV_Commit;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))     return NAV_MoveDown;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))       return NAV_MoveUp;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown))      return NAV_PageDown;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp))        return NAV_PageUp;
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false))   return NAV_JumpHome;
    if (ImGui::IsKeyPressed(ImGuiKey_End,  false))   return NAV_JumpEnd;
    return NAV_None;
}

// Apply a NavCommand to a [0, count) selection index and return the
// new index. PageUp/PageDown jump 10 items at a time.
i32 ApplyNavCommandToSelection(NavCommand c, i32 selectedIdx, i32 count) {
    if (count <= 0) return 0;
    static const i32 kPageStep = 10;
    switch (c) {
        case NAV_MoveDown:  return (selectedIdx + 1) % count;
        case NAV_MoveUp:    return (selectedIdx - 1 + count) % count;
        case NAV_PageDown:  selectedIdx += kPageStep; if (selectedIdx >= count) selectedIdx = count - 1; return selectedIdx;
        case NAV_PageUp:    selectedIdx -= kPageStep; if (selectedIdx < 0) selectedIdx = 0;              return selectedIdx;
        case NAV_JumpHome:  return 0;
        case NAV_JumpEnd:   return count - 1;
        default:            return selectedIdx;
    }
}

} // namespace Autocomplete
} // namespace UI
} // namespace Vespucci
