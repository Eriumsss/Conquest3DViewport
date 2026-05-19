// Validator.cpp
// =============================================================================
// SHOW PARSE/SEMA DIAGNOSTICS WITH CARET POINTERS - REUSES THE SAME
// RENDERED FORMAT THE COMMAND-LINE TOOL EMITS
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Compat/DSL/Diagnostics.h"
#include "../../Compat/DSL/Parser.h"
#include "../../Compat/DSL/Sema.h"
#include "../../Compat/DSL/SourceMap.h"
#include "../../Core/Arena.h"
#include "../../Schema/ZETypeRegistry.h"
#include "../../Schema/EventActionSignatureDB.h"
#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace CompatEditor {

void RenderValidatorOutput(PanelState& s) {
    if (s.sourceText.empty()) {
        ImGui::TextDisabled("(no rule text loaded)");
        return;
    }
    Vespucci::Compat::DSL::Parser p;
    Core::Arena arena(64 * 1024);
    Vespucci::Compat::DSL::DiagnosticBag bag;
    Vespucci::Compat::DSL::AstNode* root =
        p.parse(s.sourceText.c_str(), s.sourceText.size(), &arena, &bag);

    Vespucci::Compat::DSL::SourceMap smap;
    smap.build(s.sourceText.c_str(), s.sourceText.size());

    if (root) {
        Vespucci::Compat::DSL::Sema sema;
        sema.run(root, Schema::GlobalRegistry(),
                 Schema::GlobalSignatureDB(), &bag);
    }

    ImGui::Text("Errors: %d   Warnings: %d", bag.errorCount(), bag.warningCount());
    ImGui::Separator();
    std::string rendered = bag.render(s.currentFilePath[0] ? s.currentFilePath : "<editor>",
        s.sourceText.c_str(), s.sourceText.size(), smap);
    ImGui::TextWrapped("%s", rendered.c_str());
}

} // namespace CompatEditor
} // namespace UI
} // namespace Vespucci
