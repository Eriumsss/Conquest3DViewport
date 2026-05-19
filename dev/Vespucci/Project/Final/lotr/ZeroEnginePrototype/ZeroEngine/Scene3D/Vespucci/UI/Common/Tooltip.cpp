// Tooltip.cpp
// =============================================================================
// Render the structured tooltip when called. Builder pattern so the
// caller can compose conditional content without nesting if-blocks.
// =============================================================================
// Written by: Eriumsss

#include "Tooltip.h"
#include "Theme.h"
#include "ReasonChip.h"

#include "../../../imgui/imgui.h"

#include <cstring>

namespace Vespucci {
namespace UI {
namespace Common {

TooltipBuilder::TooltipBuilder()
    : m_header(0), m_lineCount(0), m_kvCount(0), m_chips(0), m_chipCount(0)
{
    std::memset(m_lines, 0, sizeof(m_lines));
    std::memset(m_kvKeys, 0, sizeof(m_kvKeys));
    std::memset(m_kvVals, 0, sizeof(m_kvVals));
}

void TooltipBuilder::SetHeader(const char* h) { m_header = h; }

void TooltipBuilder::AddBodyLine(const char* l) {
    if (m_lineCount < 8 && l) m_lines[m_lineCount++] = l;
}

void TooltipBuilder::AddKeyValue(const char* k, const char* v) {
    if (m_kvCount < 8 && k && v) {
        m_kvKeys[m_kvCount] = k;
        m_kvVals[m_kvCount] = v;
        m_kvCount++;
    }
}

void TooltipBuilder::AddReasonChips(const Suggest::ReasonChip* chips, i32 count) {
    m_chips = chips;
    m_chipCount = count;
}

void TooltipBuilder::Render() {
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextPrimary);
    if (m_header) {
        ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.55f, 1.0f), "%s", m_header);
        ImGui::Separator();
    }
    for (i32 i = 0; i < m_lineCount; ++i) {
        ImGui::TextUnformatted(m_lines[i]);
    }
    if (m_kvCount > 0) {
        if (m_lineCount > 0) ImGui::Spacing();
        if (ImGui::BeginTable("##tooltip_kv", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody))
        {
            for (i32 i = 0; i < m_kvCount; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", m_kvKeys[i]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m_kvVals[i]);
            }
            ImGui::EndTable();
        }
    }
    if (m_chips && m_chipCount > 0) {
        ImGui::Spacing();
        DrawChipRowWithWrap(m_chips, m_chipCount, 320.0f);
    }
    ImGui::PopStyleColor();
    ImGui::EndTooltip();
}

} // namespace Common
} // namespace UI
} // namespace Vespucci
