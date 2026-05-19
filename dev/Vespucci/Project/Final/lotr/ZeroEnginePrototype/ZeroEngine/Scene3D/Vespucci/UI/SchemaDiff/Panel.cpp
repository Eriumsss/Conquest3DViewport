// Panel.cpp
// =============================================================================
// Schema-diff orchestrator. Two columns: 'recorded' on the left,
// 'current' on the right; below them, the migration step list and a
// 'run migration' button.
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <cstdio>

namespace Vespucci {
namespace UI {
namespace SchemaDiff {

void RenderMigrationWizardSection(PanelState& s);

void InitPanelState(PanelState& s) {
    s.visible = true;       // surface flag controls actual show/hide
    s.recordedVersion = Schema::SchemaVersionTriple{0, 0, 0};
    s.currentVersion  = Schema::CurrentSchemaVersion();
    s.corpusPath.clear();
    s.dryRun = true;
    s.migrationLog.clear();
}

namespace {
    void RenderVersionPill(const char* label, Schema::SchemaVersionTriple v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s: %u.%u.%u",
            label, v.major, v.minor, v.patch);
        ImGui::TextDisabled("%s", buf);
    }
} // namespace

void RenderSchemaDiffPanel(PanelState& s) {
    if (!s.visible) return;
    Common::PushVespucciStyleScope();
    if (ImGui::Begin("Vespucci Schema Diff", &s.visible)) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.55f, 1.0f),
            "Corpus version mismatch handler");
        ImGui::Separator();
        RenderVersionPill("Recorded", s.recordedVersion);
        RenderVersionPill("Current",  s.currentVersion);

        bool loadable = Schema::IsLoadable(s.recordedVersion, s.currentVersion);
        if (loadable) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                "Versions are compatible. No migration required.");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                "Major-version mismatch. Cannot load this corpus without rebuild.");
        }
        ImGui::Separator();
        RenderMigrationWizardSection(s);
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace SchemaDiff
} // namespace UI
} // namespace Vespucci
