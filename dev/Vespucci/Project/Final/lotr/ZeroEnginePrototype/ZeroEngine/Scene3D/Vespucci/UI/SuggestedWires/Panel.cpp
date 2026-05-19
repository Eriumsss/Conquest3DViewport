// Panel.cpp
// =============================================================================
// THE HEADLINE PANEL - ORCHESTRATES EVERY SUB-COMPONENT FOR ONE FRAME
// =============================================================================
// Written by: Eriumsss
//
// Frame flow:
//   1. Resolve source entity from focus state.
//   2. Run ranker query (cached per snapshot version).
//   3. Render filter row (RankedList.cpp's filter helper).
//   4. Render ranked list of suggestions with reason chips.
//   5. Render per-suggestion buttons (Preview / Apply / Ignore / Why?).
//   6. On Apply, write through Apply::ApplySuggestionOrChokeOnGarbage.
//   7. On Preview, stage through Preview::StageWireOrPushItOffTheCliff.
//
// We do NOT block on snapshot rebuild. If the ranker says "no result"
// because the snapshot is mid-flux we render the previous frame's
// list with a "stale" badge and let the cache catch up.
// =============================================================================

#include "Panel.h"

#include "../../Apply/Apply.h"
#include "../../Apply/Preview.h"
#include "../../Apply/Undo.h"
#include "../../Core/Logging.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"
#include "../Common/ReasonChip.h"
#include "../Common/ScoreBar.h"
#include "../Common/Tooltip.h"
#include "imgui_glue.h"

#include <cstdio>
#include <cstring>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace SuggestedWires {

// External forward decls — sub-components defined in sibling .cpps.
i32 RunRankerForFocusOrAbort(Guid sourceGuid, const char* eventFilter,
                              Suggest::SuggestionList& outList);
void RenderFilterRow(PanelState& s);
void RenderRankedList(PanelState& s, const Suggest::SuggestionList& list);
void RenderButtonsForSuggestion(PanelState& s, const Suggest::Suggestion& sg,
                                 ImGuiGlueFrameArgs& args);
void RenderHistoryPane(PanelState& s);
void RenderPinnedSection(PanelState& s);

void InitPanelState(PanelState& s) {
    s.visible = true;
    s.focusedSourceGuid = Guid(0);
    s.eventFilter[0] = 0;
    s.selectedSuggestionIdx = 0;
    s.showPinnedOnly = false;
    s.minScoreOverride = -1.0f;
    s.autoPreviewOnHover = false;
    s.showAdvanced = false; // Patrick mode default — knobs hidden until asked
}

void SetFocusedEntity(PanelState& s, Guid sourceGuid) {
    if (s.focusedSourceGuid != sourceGuid) {
        s.focusedSourceGuid = sourceGuid;
        s.selectedSuggestionIdx = 0;
    }
}

void RenderSidebarPanel(PanelState& s, ImGuiGlueFrameArgs& args) {
    if (!s.visible) return;

    Common::PushVespucciStyleScope();

    // Bridge the EXE's selection state into our focused-source. The
    // host writes `selectedEditorObjGuid` whenever the designer clicks
    // an entity in the viewport / Outliner / Level Inspector. We read
    // it every frame and pivot the ranker output to that entity. Zero
    // means "nothing selected" — let SetFocusedEntity take it as the
    // null Guid and the panel's own guard renders the "click an entity"
    // hint until the user picks one.
    SetFocusedEntity(s, Guid((u32)args.selectedEditorObjGuid));

    // Wrap in a top-level Begin so the panel is its own dockable window
    // instead of an orphan child. BeginChild without a parent Begin is
    // illegal in ImGui and silently fails to render — that's what
    // hid the panel for the user.
    if (!ImGui::Begin("Vespucci Suggested Wires", &s.visible)) {
        ImGui::End();
        Common::PopVespucciStyleScope();
        return;
    }

    if (ImGui::BeginChild("##VespucciSuggestedWires", ImVec2(Common::Theme::SidebarWidth, 0),
                          true, ImGuiWindowFlags_AlwaysVerticalScrollbar))
    {
        // ── Header ─────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, Common::Theme::TextPrimary);
        ImGui::Text("Suggested Wires");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        // Tiny "Advanced" toggle in the header, right-aligned. Designer
        // never has to find this until they want to start tuning.
        f32 toggleWidth = 96.0f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - toggleWidth);
        ImGui::Checkbox("Advanced##sw_adv", &s.showAdvanced);
        ImGui::Separator();

        if (!s.focusedSourceGuid.valid()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.78f, 0.45f, 1.0f));
            ImGui::TextWrapped("Click any entity in the viewport, the Outliner, or the Level Inspector. "
                                "Suggestions show up here automatically.");
            ImGui::PopStyleColor();
        } else {
            // ── Focused entity header — plain English name, hex hidden ──
            Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
            const Scene::EntityRow* focusedRow = 0;
            if (snap) {
                Vespucci::EntityIndex fIdx = snap->indexForGuid(s.focusedSourceGuid);
                if (fIdx.valid()) focusedRow = snap->entityAt(fIdx);
            }
            if (focusedRow) {
                ImGui::TextColored(ImVec4(0.93f, 0.95f, 0.98f, 1.0f),
                    "Selected: %.*s",
                    (int)focusedRow->name.size(), focusedRow->name.data());
                if (focusedRow->typeName.size() > 0) {
                    ImGui::TextDisabled("Type: %.*s",
                        (int)focusedRow->typeName.size(), focusedRow->typeName.data());
                }
            } else {
                ImGui::TextDisabled("Selected: (entity not found in snapshot yet)");
            }

            if (s.showAdvanced) {
                ImGui::TextDisabled("GUID: 0x%08X", (unsigned)s.focusedSourceGuid.raw);
                RenderFilterRow(s);
            }
            ImGui::Spacing();

            // ── Run ranker ─────────────────────────────────────────
            Suggest::SuggestionList list;
            i32 nResults = RunRankerForFocusOrAbort(s.focusedSourceGuid,
                                                     s.eventFilter, list);
            if (nResults == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.78f, 0.82f, 1.0f));
                ImGui::TextWrapped("Nothing to suggest yet. Either this entity doesn't fire events "
                                    "(decorations, lights, props) or the corpus hasn't seen this type wired before.");
                ImGui::PopStyleColor();
            } else {
                // ── PATRICK MODE: one giant button at the top to apply top-1 ──
                const Suggest::Suggestion& topPick = list.suggestions[0];
                const Scene::EntityRow* topTgt = snap ? snap->entityAt(topPick.target) : 0;
                const char* topTgtName = topTgt ? topTgt->name.data() : "(unknown)";
                i32 topTgtNameLen = topTgt ? (i32)topTgt->name.size() : 9;

                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.32f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.72f, 0.40f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.85f, 0.46f, 1.0f));
                char primaryLabel[200];
                std::snprintf(primaryLabel, sizeof(primaryLabel),
                    "Wire it up  →  %.*s", topTgtNameLen, topTgtName);
                if (ImGui::Button(primaryLabel, ImVec2(-1.0f, 44.0f))) {
                    s.selectedSuggestionIdx = 0;
                    // Reuse the same Apply path the Apply button hits.
                    Apply::ApplySuggestionOrChokeOnGarbage(topPick,
                        s.focusedSourceGuid, args);
                    Apply::UndoOp op;
                    std::memset(&op, 0, sizeof(op));
                    op.kind        = Apply::UNDO_DeleteAddedWire;
                    op.sourceGuid  = s.focusedSourceGuid;
                    op.targetGuid  = topPick.targetGuid;
                    op.eventName   = topPick.eventName;
                    op.actionName  = topPick.actionName;
                    op.delay       = topPick.suggestedDelay;
                    op.sticky      = topPick.suggestedSticky;
                    Apply::PushUndoOpOrChokeOnTheStackOverflow(op);
                    Core::Logging::Info("SuggestedWires: Patrick-mode wire-up — %.*s -> %.*s (score %.2f)",
                        focusedRow ? (int)focusedRow->name.size() : 0,
                        focusedRow ? focusedRow->name.data() : "",
                        topTgtNameLen, topTgtName, topPick.score);
                }
                ImGui::PopStyleColor(3);

                // Score / confidence one-liner under the big button.
                ImGui::TextDisabled("Score %.2f  •  %d more suggestion%s available",
                    topPick.score, list.count - 1, list.count == 2 ? "" : "s");

                // Top reason chip under the button so the designer sees
                // *why* in one glance.
                if (topPick.reasonCount > 0 && topPick.reasons[0].text.size() > 0) {
                    ImGui::TextWrapped("Why: %.*s",
                        (int)topPick.reasons[0].text.size(),
                        topPick.reasons[0].text.data());
                }

                ImGui::Spacing();

                // ── Show alternatives expander (closed by default) ──
                if (list.count > 1) {
                    if (ImGui::CollapsingHeader("Show alternatives##sw_alts")) {
                        RenderRankedList(s, list);
                        if (s.selectedSuggestionIdx >= 0 &&
                            s.selectedSuggestionIdx < list.count)
                        {
                            ImGui::Separator();
                            const Suggest::Suggestion& chosen = list.suggestions[s.selectedSuggestionIdx];
                            RenderButtonsForSuggestion(s, chosen, args);
                        }
                    }
                }
            }

            // ── Advanced expander gates pinning, history, undo ──
            if (s.showAdvanced) {
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Pinned##sw_pinned")) RenderPinnedSection(s);
                if (ImGui::CollapsingHeader("History##sw_hist"))   RenderHistoryPane(s);
                ImGui::Separator();
                if (ImGui::Button("Undo last apply", ImVec2(-1.0f, 0.0f))) {
                    if (!Apply::PopUndoOpAndFireItOrPissOff(args)) {
                        Core::Logging::Info("SuggestedWires: undo stack empty");
                    }
                }
                ImGui::TextDisabled("(stack depth: %d)", Apply::UndoStackDepth());
            } else {
                // Tiny undo even in Patrick mode — designers panic, they
                // need a one-click escape hatch.
                ImGui::Spacing();
                if (Apply::UndoStackDepth() > 0) {
                    if (ImGui::SmallButton("Undo last##sw_undo_small")) {
                        Apply::PopUndoOpAndFireItOrPissOff(args);
                    }
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();

    Common::PopVespucciStyleScope();
}

} // namespace SuggestedWires
} // namespace UI
} // namespace Vespucci
