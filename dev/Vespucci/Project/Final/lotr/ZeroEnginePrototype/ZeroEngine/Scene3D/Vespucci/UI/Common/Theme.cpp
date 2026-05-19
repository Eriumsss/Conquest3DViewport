// Theme.cpp
// =============================================================================
// Push/Pop a Vespucci ImGui style scope. Hot path tolerant — caller
// can wrap their panel without the rest of the editor's UI noticing.
// =============================================================================
// Written by: Eriumsss

#include "Theme.h"

namespace Vespucci {
namespace UI {
namespace Common {

void PushVespucciStyleScope() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg,       Theme::BgPanel);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,        Theme::BgSubpanel);
    ImGui::PushStyleColor(ImGuiCol_Text,           Theme::TextPrimary);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,   Theme::TextDim);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  Theme::BgHoveredRow);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   Theme::BgSelectedRow);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(Theme::PadX, Theme::PadY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
        ImVec2(Theme::PadX * 0.5f, Theme::RowGap));
}

void PopVespucciStyleScope() {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(6);
}

} // namespace Common
} // namespace UI
} // namespace Vespucci
