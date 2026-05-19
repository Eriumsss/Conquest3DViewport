// GhostTargetOverlay.h
// =============================================================================
// GHOST TARGET OVERLAY - WORLD-SPACE PREVIEW WIRE WHILE THE USER DRAGS
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked LAST UI surface to ship. Highest-risk because a wrong
// snap-to-target wires the user into a target they did not intend
// and they only notice 30 seconds later when the wire fires the
// wrong cutscene. The shipping rule: ghost preview ONLY appears
// once the lower-tier UI surfaces (sidebar, autocomplete, health-
// fix) have proved the ranker's quality on real designer use.
//
// Behavior: while the user is mid-drag in The Forge's wire-creation
// flow, this overlay highlights the top-3 ranked target candidates
// in 3D world space. Snap-to-cursor only when distance + confidence
// both exceed the gates. Cursor moves outside the snap radius and
// the ghost releases — designer always retains full control.
// =============================================================================

#ifndef VESPUCCI_UI_GHOSTOVERLAY_GHOSTTARGETOVERLAY_H_
#define VESPUCCI_UI_GHOSTOVERLAY_GHOSTTARGETOVERLAY_H_

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"

namespace Vespucci {
namespace UI {
namespace GhostOverlay {

struct OverlayConfig {
    f32  snapRadiusPx;          // pixels
    f32  minConfidenceForSnap;  // [0,1]
    f32  ghostAlpha;            // base alpha
    bool pulseAnimation;
    bool drawConnectingArc;
    i32  maxGhostsShown;        // capped at 3 per doc
};

OverlayConfig DefaultGhostConfig();

// Push the current frame's ranked candidates into the overlay so
// the renderer can paint them on the next viewport pass. Called by
// the Forge's wire-drag handler each frame the drag is live.
void StageCandidatesForOverlay(const Suggest::Suggestion* candidates,
                                 i32 count,
                                 const OverlayConfig& cfg);

void ClearStagedCandidates();

// Read access for the renderer (LevelScene-side hook will call this).
i32  GetStagedCandidateCount();
const Suggest::Suggestion* GetStagedCandidate(i32 i);
OverlayConfig GetActiveConfig();

// Snap test - given the cursor's world position and the staged
// candidates, returns the index of the candidate within snap range
// (or -1). The Forge uses this to lock the wire's target endpoint
// when the user releases the mouse.
i32 ResolveSnapOrReleaseControl(const Vec3& cursorWorld,
                                  f32 cursorScreenRadiusPx);

} // namespace GhostOverlay
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_GHOSTOVERLAY_GHOSTTARGETOVERLAY_H_
