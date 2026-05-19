// GhostTargetOverlay.cpp
// =============================================================================
// Stage the candidates each frame; the renderer-side hook reads them
// to paint world-space ghost markers + arcs. Snap test runs on
// release-to-place. Conservative gates prevent the user from
// accidentally locking onto a low-confidence target they did not
// intend - because the cost of one wrong wire is way worse than the
// cost of having to drop the mouse one extra time on purpose.
// =============================================================================
// Written by: Eriumsss

#include "GhostTargetOverlay.h"

#include "../../Core/Logging.h"
#include "../../Core/VespucciAssert.h"
#include "../../Scene/SceneSnapshot.h"

#include <cstring>

namespace Vespucci {
namespace UI {
namespace GhostOverlay {

namespace {
    static const i32 kMaxStaged = 8; // hard cap, doc allows up to 3 visible
    static Suggest::Suggestion s_staged[kMaxStaged];
    static i32                 s_count = 0;
    static OverlayConfig       s_cfg;
    static bool                s_cfgInited = false;
} // namespace

OverlayConfig DefaultGhostConfig() {
    OverlayConfig c;
    c.snapRadiusPx          = 32.0f;
    c.minConfidenceForSnap  = 0.55f;
    c.ghostAlpha            = 0.45f;
    c.pulseAnimation        = true;
    c.drawConnectingArc     = true;
    c.maxGhostsShown        = 3;
    return c;
}

void StageCandidatesForOverlay(const Suggest::Suggestion* candidates,
                                 i32 count, const OverlayConfig& cfg)
{
    if (!s_cfgInited) { s_cfg = DefaultGhostConfig(); s_cfgInited = true; }
    s_cfg = cfg;

    if (!candidates || count <= 0) {
        s_count = 0;
        return;
    }
    i32 take = count;
    if (take > cfg.maxGhostsShown) take = cfg.maxGhostsShown;
    if (take > kMaxStaged)         take = kMaxStaged;
    s_count = take;
    for (i32 i = 0; i < take; ++i) s_staged[i] = candidates[i];
}

void ClearStagedCandidates() {
    s_count = 0;
}

i32  GetStagedCandidateCount() { return s_count; }

const Suggest::Suggestion* GetStagedCandidate(i32 i) {
    if (i < 0 || i >= s_count) return 0;
    return &s_staged[i];
}

OverlayConfig GetActiveConfig() {
    if (!s_cfgInited) { s_cfg = DefaultGhostConfig(); s_cfgInited = true; }
    return s_cfg;
}

i32 ResolveSnapOrReleaseControl(const Vec3& cursorWorld, f32 cursorScreenRadiusPx) {
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!snap || s_count == 0) return -1;
    OverlayConfig cfg = GetActiveConfig();

    // Find the closest staged candidate that ALSO clears the
    // confidence gate. Worst-of-both-worlds prevention: a near-
    // cursor candidate the ranker is unsure about does NOT win.
    i32 best = -1;
    f32 bestDist2 = 1e30f;
    for (i32 i = 0; i < s_count; ++i) {
        if (s_staged[i].confidence < cfg.minConfidenceForSnap) continue;
        const Scene::EntityRow* tgt = snap->entityAt(s_staged[i].target);
        if (!tgt) continue;
        f32 dx = tgt->position.x - cursorWorld.x;
        f32 dy = tgt->position.y - cursorWorld.y;
        f32 dz = tgt->position.z - cursorWorld.z;
        f32 d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < bestDist2) {
            bestDist2 = d2;
            best = i;
        }
    }
    if (best < 0) return -1;

    // Crude screen-radius approximation - the renderer side has the
    // proper world-to-screen transform; here we use a flat distance
    // gate as the conservative outer ring. The renderer's own snap
    // test refines this.
    f32 screenLikeRadius = cursorScreenRadiusPx * 0.2f; // world units
    if (bestDist2 > screenLikeRadius * screenLikeRadius) return -1;
    return best;
}

} // namespace GhostOverlay
} // namespace UI
} // namespace Vespucci
