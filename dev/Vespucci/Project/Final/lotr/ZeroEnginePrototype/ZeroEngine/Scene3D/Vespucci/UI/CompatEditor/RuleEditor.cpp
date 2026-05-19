// RuleEditor.cpp
// =============================================================================
// MULTI-LINE RULE TEXT EDITOR WITH LIVE PARSE FEEDBACK
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace CompatEditor {

void RenderRuleEditorBuffer(PanelState& s) {
    static char buf[16 * 1024] = "";
    if (s.sourceText.size() > 0 && buf[0] == 0) {
        std::strncpy(buf, s.sourceText.c_str(), sizeof(buf) - 1);
    }
    ImGui::InputTextMultiline("##cmp_text", buf, sizeof(buf),
        ImVec2(-1.0f, ImGui::GetContentRegionAvail().y - 60.0f));
    s.sourceText.assign(buf);
    if (ImGui::Button("Parse + apply")) {
        // Hook into DSL Parser/Sema/IrLower/RuleSet via Phase N init.
        s.lastError.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save .compat")) {
        // WriteAtomic into currentFilePath.
    }
    ImGui::SameLine();
    if (!s.lastError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", s.lastError.c_str());
    }
}

} // namespace CompatEditor
} // namespace UI
} // namespace Vespucci
