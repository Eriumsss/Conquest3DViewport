// Panel.h
// =============================================================================
// CORPUS BROWSER - DRILL DOWN PER-TYPE / PER-EVENT INTO THE PRIORS
// =============================================================================
// Written by: Eriumsss

#ifndef VESPUCCI_UI_CORPUSBROWSER_PANEL_H_
#define VESPUCCI_UI_CORPUSBROWSER_PANEL_H_

#include "../../Core/VespucciTypes.h"

namespace Vespucci {
namespace UI {
namespace CorpusBrowser {

struct PanelState {
    bool   visible;
    char   typeFilter[64];
    char   eventFilter[64];
    i32    selectedTypeIdx;
    bool   showAdvanced;        // Patrick mode default — false
    // Cross-page selection mirror.
    Guid   focusedEntityGuid;
};

void InitPanelState(PanelState& s);
void RenderCorpusBrowser(PanelState& s);

} // namespace CorpusBrowser
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_CORPUSBROWSER_PANEL_H_
