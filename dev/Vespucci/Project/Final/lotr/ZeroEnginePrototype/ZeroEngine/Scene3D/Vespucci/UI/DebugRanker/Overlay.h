// Overlay.h
// =============================================================================
// DEBUG RANKER OVERLAY - 'BRAIN' TAB OF THE FORGE
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked mandatory surface. Shows the ranker's last query in
// gory detail: candidate pool size, hard-filter pass count, per-
// feature score breakdown for top-K, suppress reasons, snapshot
// version, local/global mix ratio. Tuning weights without this
// overlay is fortune-telling, period.
// =============================================================================

#ifndef VESPUCCI_UI_DEBUGRANKER_OVERLAY_H_
#define VESPUCCI_UI_DEBUGRANKER_OVERLAY_H_

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"

namespace Vespucci {
namespace UI {
namespace DebugRanker {

struct OverlayState {
    bool                    visible;
    Suggest::SuggestionList lastList;
    bool                    autoCaptureLast;
    f32                     latencySamples[64];
    i32                     latencyCursor;
    // Patrick-mode UX gate. Default false → designers see ONE summary
    // line + a "no panic" status. The full cockpit (latency graph,
    // weight sliders, score breakdown, feature histogram, ranker
    // strategy switcher) only renders when the user opts in.
    bool                    showAdvanced;
    // Cross-page selection mirror — every Vespucci panel reads the
    // host's `selectedEditorObjGuid` each frame and pivots its view
    // to that entity. Brain shows the entity name + the score
    // breakdown for IT specifically.
    Guid                    focusedEntityGuid;
};

void InitOverlayState(OverlayState& s);
void RenderBrainOverlay(OverlayState& s);
void CaptureLastSuggestion(OverlayState& s, const Suggest::SuggestionList& list);

// Publish a ranker query result into whatever brain state is currently
// registered. Sidebar / Health-Fix / Autocomplete call this after every
// real ranker pass so the Brain panel sees live numbers without each
// consumer needing to know which OverlayState instance is the one.
void PublishLastQuery(const Suggest::SuggestionList& list);
OverlayState* GlobalBrainState();
void          SetGlobalBrainState(OverlayState* s);

} // namespace DebugRanker
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_DEBUGRANKER_OVERLAY_H_
