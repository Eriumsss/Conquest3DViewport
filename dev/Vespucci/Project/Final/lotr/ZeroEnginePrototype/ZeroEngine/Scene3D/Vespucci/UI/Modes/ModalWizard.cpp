// ModalWizard.cpp
// =============================================================================
// FULL-SCREEN MODAL WIZARD MODE - GUIDED FLOW FOR NEW DESIGNERS
// =============================================================================
// Written by: Eriumsss
//
// Some designers want the editor to HOLD THEIR HAND through wiring
// their first mission. Modal wizard mode pops a centered window
// that walks them through: 1) pick the source entity, 2) pick the
// event, 3) pick the target from suggestions, 4) confirm. Each
// step shows the relevant suggestions inline + has Back/Next
// navigation. When the user enables this mode the sidebar / ribbon
// hide so they get one focused surface at a time.
// =============================================================================

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <cstdio>

namespace Vespucci {
namespace UI {
namespace Modes {

enum WizardStep {
    WIZ_PickSource = 0,
    WIZ_PickEvent  = 1,
    WIZ_PickTarget = 2,
    WIZ_Confirm    = 3,
    WIZ_Done       = 4
};

struct WizardState {
    bool         visible;
    WizardStep   step;
    Guid         sourceGuid;
    char         eventBuf[64];
    Guid         targetGuid;
};

static WizardState s_wiz = { false, WIZ_PickSource, Guid(0), {0}, Guid(0) };

void OpenWizard()  { s_wiz.visible = true;  s_wiz.step = WIZ_PickSource; }
void CloseWizard() { s_wiz.visible = false; }

void RenderModalWizard(const Suggest::SuggestionList& list) {
    if (!s_wiz.visible) return;
    ImVec2 io = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(io.x * 0.5f - 240.0f, io.y * 0.5f - 200.0f));
    ImGui::SetNextWindowSize(ImVec2(480.0f, 400.0f));
    Common::PushVespucciStyleScope();
    if (ImGui::Begin("Vespucci Wizard", &s_wiz.visible,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
    {
        switch (s_wiz.step) {
            case WIZ_PickSource:
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.55f, 1.0f),
                    "Step 1 of 4: Pick the entity that will fire the event");
                ImGui::Text("Click an entity in the 3D viewport, then click Next.");
                if (ImGui::Button("Next") && s_wiz.sourceGuid.valid()) s_wiz.step = WIZ_PickEvent;
                break;
            case WIZ_PickEvent:
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.55f, 1.0f),
                    "Step 2 of 4: What event should fire?");
                ImGui::InputTextWithHint("##wiz_evt", "OnCapture, OnDeath, OnTrigger, ...",
                    s_wiz.eventBuf, sizeof(s_wiz.eventBuf));
                if (ImGui::Button("Back")) s_wiz.step = WIZ_PickSource;
                ImGui::SameLine();
                if (ImGui::Button("Next") && s_wiz.eventBuf[0]) s_wiz.step = WIZ_PickTarget;
                break;
            case WIZ_PickTarget:
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.55f, 1.0f),
                    "Step 3 of 4: Pick the target");
                ImGui::TextDisabled("Suggestions for %s:", s_wiz.eventBuf);
                for (i32 i = 0; i < list.count; ++i) {
                    const Suggest::Suggestion& sg = list.suggestions[i];
                    char rowLabel[200];
                    std::snprintf(rowLabel, sizeof(rowLabel),
                        "0x%08X  (%.2f)##wiz_t_%d",
                        (unsigned)sg.targetGuid.raw, sg.score, i);
                    if (ImGui::Selectable(rowLabel, sg.targetGuid == s_wiz.targetGuid)) {
                        s_wiz.targetGuid = sg.targetGuid;
                    }
                }
                if (ImGui::Button("Back")) s_wiz.step = WIZ_PickEvent;
                ImGui::SameLine();
                if (ImGui::Button("Next") && s_wiz.targetGuid.valid()) s_wiz.step = WIZ_Confirm;
                break;
            case WIZ_Confirm:
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.55f, 1.0f),
                    "Step 4 of 4: Confirm");
                ImGui::Text("Source: 0x%08X", (unsigned)s_wiz.sourceGuid.raw);
                ImGui::Text("Event:  %s",     s_wiz.eventBuf);
                ImGui::Text("Target: 0x%08X", (unsigned)s_wiz.targetGuid.raw);
                if (ImGui::Button("Back")) s_wiz.step = WIZ_PickTarget;
                ImGui::SameLine();
                if (ImGui::Button("Apply")) {
                    s_wiz.step = WIZ_Done;
                    s_wiz.visible = false;
                }
                break;
            case WIZ_Done:
                CloseWizard();
                break;
        }
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
}

} // namespace Modes
} // namespace UI
} // namespace Vespucci
