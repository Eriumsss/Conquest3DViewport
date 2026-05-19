// Panel.h
// =============================================================================
// COMPAT EDITOR - DESIGNER-FACING UI FOR AUTHORING .compat RULES
// =============================================================================
// Written by: Eriumsss

#ifndef VESPUCCI_UI_COMPATEDITOR_PANEL_H_
#define VESPUCCI_UI_COMPATEDITOR_PANEL_H_

#include "../../Core/VespucciTypes.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace UI {
namespace CompatEditor {

// One entry in the probe-pair drop-down — sourced from the live
// snapshot's wires so designers see real combinations from their
// actual level rather than a hardcoded `OnTrigger -> Activate`.
struct MatrixProbePair {
    std::string eventName;
    std::string actionName;
    i32         observedCount;     // how many wires in the level use this pair
};

struct PanelState {
    bool        visible;
    std::string sourceText;     // designer-edited rule text
    std::string lastError;       // last parse/sema error
    bool        liveValidate;
    char        currentFilePath[256];

    // Matrix-tab tuning state — appended at struct end.
    std::vector<MatrixProbePair> matrixProbePairs;   // cached, rebuilt when snapshot moves
    SnapshotVersion              matrixProbeBuiltAt; // last snapshot version we scanned
    i32                          matrixProbeIdx;     // which pair the user is inspecting
    i32                          matrixTypeCap;      // user-controlled row/col cap (default 32)
    char                         matrixSrcFilter[64]; // substring filter for source rows
    char                         matrixTgtFilter[64]; // substring filter for target columns

    // Cross-page selection mirror — when the designer clicks an entity
    // anywhere, the matrix tab can pre-filter to that entity's source
    // type so the relevant row is visible without scrolling.
    Guid                         focusedEntityGuid;
};

void InitPanelState(PanelState& s);
void RenderCompatEditor(PanelState& s);

} // namespace CompatEditor
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_COMPATEDITOR_PANEL_H_
