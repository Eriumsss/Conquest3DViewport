// Panel.h
// =============================================================================
// SUGGESTED WIRES SIDEBAR - THE HEADLINE FEATURE OF VESPUCCI SMART WIRING
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked priority #2 surface and the demo target for declaring
// the brain "real": select a CapturePoint, the sidebar instantly
// shows the top-5 likely wires, each with reason chips, each with
// a Preview / Apply / Ignore / Why? button. Click Apply, the wire
// commits through chainAddConnectionRequested. Click Undo, it
// reverts. That whole loop is the milestone the doc marks as
// "oh shit, this is a real editor."
//
// This file is the orchestrator. The actual list rendering lives in
// RankedList.cpp; chip layout in ReasonStripe.cpp; filter row in
// Filters.cpp; the buttons in AcceptRejectButtons.cpp; pinning state
// in Pinning.cpp; per-session history in HistoryPane.cpp.
// =============================================================================

#ifndef VESPUCCI_UI_SUGGESTEDWIRES_PANEL_H_
#define VESPUCCI_UI_SUGGESTEDWIRES_PANEL_H_

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"

struct ImGuiGlueFrameArgs;

namespace Vespucci {
namespace UI {
namespace SuggestedWires {

// Per-session state. One per Forge tab instance.
struct PanelState {
    bool         visible;
    Guid         focusedSourceGuid;     // entity the sidebar is showing for
    char         eventFilter[64];        // overrides the implicit event picker
    i32          selectedSuggestionIdx;
    bool         showPinnedOnly;
    f32          minScoreOverride;       // -1 = use weights default
    bool         autoPreviewOnHover;

    // Patrick-mode UX gate: by default the sidebar shows ONE big primary
    // button + plain-English entity names. Filters / pinning / history /
    // event-filter / score breakdown all live behind this advanced toggle
    // so designers don't drown in cockpit knobs they don't need.
    bool         showAdvanced;
};

void InitPanelState(PanelState& s);

// Draw the sidebar. Caller binds this once per editor frame, inside
// a docked child window. Reads the active SuggestionList from the
// global rankerSelector + cache. Writes Apply requests into args
// when the user commits.
void RenderSidebarPanel(PanelState& s, ImGuiGlueFrameArgs& args);

// External hook: the Forge calls this when the user clicks an entity
// to update what the sidebar focuses on.
void SetFocusedEntity(PanelState& s, Guid sourceGuid);

} // namespace SuggestedWires
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_SUGGESTEDWIRES_PANEL_H_
