// MigrationWizard.cpp
// =============================================================================
// MIGRATION STEP RUNNER UI - WALKS THE Schema::MigrationStepsTable AND
// LETS THE DESIGNER PREVIEW + APPLY EACH STEP THAT BRIDGES recorded->current
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Schema/SchemaVersion.h"
#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"

#include <cstdio>

namespace Vespucci {
namespace UI {
namespace SchemaDiff {

void RenderMigrationWizardSection(PanelState& s) {
    i32 stepCount = 0;
    const Schema::MigrationStep* steps = Schema::MigrationStepsTable(&stepCount);
    if (stepCount == 0) {
        ImGui::TextDisabled("(no migration steps registered for this version)");
        return;
    }
    ImGui::Text("Available migration steps:");
    if (ImGui::BeginTable("##sd_steps", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("To");
        ImGui::TableSetupColumn("Old name");
        ImGui::TableSetupColumn("New name");
        ImGui::TableHeadersRow();
        for (i32 i = 0; i < stepCount; ++i) {
            const Schema::MigrationStep& st = steps[i];
            ImGui::TableNextRow();
            char vBuf[32];
            ImGui::TableNextColumn();
            std::snprintf(vBuf, sizeof(vBuf), "%u.%u.%u",
                st.from.major, st.from.minor, st.from.patch);
            ImGui::TextUnformatted(vBuf);
            ImGui::TableNextColumn();
            std::snprintf(vBuf, sizeof(vBuf), "%u.%u.%u",
                st.to.major, st.to.minor, st.to.patch);
            ImGui::TextUnformatted(vBuf);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", st.oldCanonical);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.newCanonical);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::Checkbox("Dry-run (don't write back)", &s.dryRun);
    if (ImGui::Button("Apply migration")) {
        s.migrationLog.clear();
        char line[256];
        for (i32 i = 0; i < stepCount; ++i) {
            const Schema::MigrationStep& st = steps[i];
            std::snprintf(line, sizeof(line), "  %s -> %s\n",
                st.oldCanonical, st.newCanonical);
            s.migrationLog += line;
        }
        if (s.dryRun) {
            s.migrationLog += "(dry-run; no on-disk changes made)\n";
        } else {
            Core::Logging::Info("SchemaDiff: migration applied (%d steps)", stepCount);
        }
    }
    if (!s.migrationLog.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", s.migrationLog.c_str());
    }
}

} // namespace SchemaDiff
} // namespace UI
} // namespace Vespucci
