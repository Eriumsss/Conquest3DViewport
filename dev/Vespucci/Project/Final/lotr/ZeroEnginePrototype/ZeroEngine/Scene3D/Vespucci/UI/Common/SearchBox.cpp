// SearchBox.cpp
// =============================================================================
// Themed search/filter input row. ImGui InputText + a small X button
// that clears the buffer + a TextDisabled "(N matches)" suffix.
// =============================================================================
// Written by: Eriumsss

#include "SearchBox.h"
#include "Theme.h"

#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace Common {

bool DrawSearchBox(const char* label, const char* hint,
                    char* buf, i32 bufCap, i32 matchCount)
{
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-180.0f);
    if (ImGui::InputTextWithHint("##sb_input", hint ? hint : "filter...",
                                  buf, (size_t)bufCap)) changed = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("X##sb_clear")) {
        if (bufCap > 0) buf[0] = 0;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d match%s)", matchCount, matchCount == 1 ? "" : "es");
    ImGui::PopID();
    return changed;
}

} // namespace Common
} // namespace UI
} // namespace Vespucci
