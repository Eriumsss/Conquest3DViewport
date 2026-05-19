// Panel.h
// =============================================================================
// HEALTH-FIX PANEL - LISTS BROKEN REFS WITH 'DID YOU MEAN' OPTIONS
// =============================================================================
// Written by: Eriumsss

#ifndef VESPUCCI_UI_HEALTHFIX_PANEL_H_
#define VESPUCCI_UI_HEALTHFIX_PANEL_H_

#include "../../Core/VespucciTypes.h"
#include "../../Repair/BrokenRefScanner.h"
#include "../../Repair/DidYouMean.h"

#include <vector>

struct ImGuiGlueFrameArgs;

namespace Vespucci {
namespace UI {
namespace HealthFix {

struct PanelState {
    bool                            visible;
    std::vector<Repair::BrokenRef>  scanResults;
    i32                             selectedRowIdx;
    std::vector<Repair::RepairCandidate> currentCandidates;
    char                            filterText[64];
    bool                            showOnlyCriticalKinds;
    SnapshotVersion                 lastScanVersion;

    // Patrick-mode UX gate. Default false → designers see one big
    // "Fix everything" button + a one-line summary. Click "Advanced"
    // to surface the per-row list, the Did-You-Mean drill-down, and
    // the existing batch-action toolbar.
    bool                            showAdvanced;
    // Cross-page selection mirror — when the designer clicks an entity
    // anywhere (Outliner / 3D viewport / Inspector), this gets the
    // entity's GUID and we filter / highlight broken-ref rows that
    // touch it. Vespucci::Update writes this each frame from the
    // host's selectedEditorObjGuid.
    Guid                            focusedEntityGuid;
};

void InitHealthFixPanel(PanelState& s);
void RenderHealthFixPanel(PanelState& s, ImGuiGlueFrameArgs& args);

} // namespace HealthFix
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_HEALTHFIX_PANEL_H_
