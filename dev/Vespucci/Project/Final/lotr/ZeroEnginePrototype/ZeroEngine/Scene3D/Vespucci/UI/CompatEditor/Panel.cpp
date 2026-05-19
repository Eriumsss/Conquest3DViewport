// Panel.cpp
// =============================================================================
// Compat editor - text editor + live parse + apply-on-save.
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Compat/DSL/Parser.h"
#include "../../Compat/DSL/Sema.h"
#include "../../Compat/DSL/Diagnostics.h"
#include "../../Compat/DSL/SourceMap.h"
#include "../../Core/Arena.h"
#include "../../Core/PathUtils.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

namespace Vespucci {
namespace UI {
namespace CompatEditor {

void RenderRuleEditorBuffer(PanelState& s);
void RenderValidatorOutput(PanelState& s);
void RenderMatrixGridSnapshot(PanelState& s);

void InitPanelState(PanelState& s) {
    s.visible = true;
    s.sourceText.clear();
    s.lastError.clear();
    s.liveValidate = true;
    s.currentFilePath[0] = 0;

    // Matrix-tab tuning state defaults.
    s.matrixProbePairs.clear();
    s.matrixProbeBuiltAt = SnapshotVersion(0);
    s.matrixProbeIdx     = 0;
    s.matrixTypeCap      = 32;
    s.matrixSrcFilter[0] = 0;
    s.matrixTgtFilter[0] = 0;
    s.focusedEntityGuid  = Guid(0);
}

void RenderCompatEditor(PanelState& s) {
    if (!s.visible) return;
    Common::PushVespucciStyleScope();
    if (ImGui::Begin("Vespucci Compat Editor", &s.visible)) {
        ImGui::Text("File: %s", s.currentFilePath[0] ? s.currentFilePath : "(unsaved)");
        ImGui::SameLine();
        ImGui::Checkbox("Live validate", &s.liveValidate);

        // Cross-page focused entity — when one is selected, auto-fill
        // the matrix source filter with its type name so the relevant
        // row floats into view without scrolling.
        if (s.focusedEntityGuid.valid()) {
            Scene::SceneSnapshot* snapC = Scene::GlobalSnapshot();
            const Scene::EntityRow* row = 0;
            if (snapC) {
                Vespucci::EntityIndex idx = snapC->indexForGuid(s.focusedEntityGuid);
                if (idx.valid()) row = snapC->entityAt(idx);
            }
            if (row) {
                ImGui::TextColored(ImVec4(0.93f, 0.95f, 0.98f, 1.0f),
                    "Selected: %.*s (%.*s)",
                    (int)row->name.size(), row->name.data(),
                    (int)row->typeName.size(), row->typeName.data());
                // Pre-load the matrix's source-row filter to the
                // selected entity's type — designer hits the Matrix
                // tab and the relevant rows are already filtered.
                if (row->typeName.size() > 0 && s.matrixSrcFilter[0] == 0) {
                    usize n = row->typeName.size();
                    if (n + 1 > sizeof(s.matrixSrcFilter)) n = sizeof(s.matrixSrcFilter) - 1;
                    for (usize k = 0; k < n; ++k) s.matrixSrcFilter[k] = row->typeName.data()[k];
                    s.matrixSrcFilter[n] = 0;
                }
            }
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("##cmp_tabs")) {
            if (ImGui::BeginTabItem("Rules")) {
                RenderRuleEditorBuffer(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Validator")) {
                RenderValidatorOutput(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Matrix")) {
                RenderMatrixGridSnapshot(s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace CompatEditor
} // namespace UI
} // namespace Vespucci
