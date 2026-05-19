// ConfidenceGate.cpp
// =============================================================================
// THE THREE-GATE SUPPRESSION FILTER. WRONG SUGGESTION > NO SUGGESTION.
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked invariants from both design docs:
//   1. ABSOLUTE FLOOR — any score below this is too weak to surface.
//      "Maybe?" is worse than "I don't know."
//   2. MARGIN RATIO — if top1 and top2 are within marginRatio of each
//      other, the ranker is admitting it can't decide. Suppressing
//      both is safer than picking one and being wrong.
//   3. LIST SIZE CAP — even when many candidates pass the floor, we
//      never overflow the sidebar's eyeball-budget. Top-N only.
//
// Tunable from `Suggest/weights.json` and hot-reloadable from the
// DebugRanker overlay (Advanced toggle reveals the sliders).
//
// Gate ordering matters:
//   - Margin gate runs FIRST. A tied top-1/top-2 means the whole list
//     is suspicious; we don't want the floor gate to keep one item
//     from a flat distribution and present it as confident.
//   - Floor gate runs SECOND on the survivors.
//   - List cap runs LAST.
//
// Three preset modes the editor lets the designer pick:
//   AGGRESSIVE   — high floor, tight margin, small list. Designer
//                  only sees rock-solid suggestions.
//   BALANCED     — default. Ships out-of-the-box.
//   PERMISSIVE   — low floor, loose margin, longer list. Designer
//                  is exploring; happy to see weak speculative wires.
// =============================================================================

#include "ConfidenceGate.h"

#include "../Core/Logging.h"
#include "../Core/VespucciAssert.h"

namespace Vespucci {
namespace Suggest {

GateConfig DefaultGateConfig() {
    GateConfig c;
    c.absoluteFloor = 0.10f;
    c.marginRatio   = 0.05f;
    c.maxResults    = Limits::kMaxSidebarSuggestions;
    c.enabled       = true;
    return c;
}

GateConfig AggressiveGateConfig() {
    GateConfig c;
    c.absoluteFloor = 0.35f;
    c.marginRatio   = 0.15f;
    c.maxResults    = 3;
    c.enabled       = true;
    return c;
}

GateConfig PermissiveGateConfig() {
    GateConfig c;
    c.absoluteFloor = 0.04f;
    c.marginRatio   = 0.01f;
    c.maxResults    = 10;
    c.enabled       = true;
    return c;
}

GateConfig DisabledGateConfig() {
    GateConfig c;
    c.absoluteFloor = 0.0f;
    c.marginRatio   = 0.0f;
    c.maxResults    = 256;
    c.enabled       = false;
    return c;
}

namespace {
    // Voice-named gate primitives.
    bool ChokeBelowTheFloorYouSpinelessFucker(score_t s, f32 floor) {
        return s < floor;
    }
    bool TopTwoAreATiedDickridingMess(score_t top1, score_t top2, f32 marginRatio) {
        if (top1 <= 0.0f) return true;
        f32 marg = (top1 - top2) / top1;
        return marg < marginRatio;
    }
} // namespace

i32 ApplyGatesOrSuppressTheBitchassWeakOnes(Suggestion* sorted, i32 count,
                                              const GateConfig& cfg,
                                              i32* outSuppressed)
{
    if (outSuppressed) *outSuppressed = 0;
    if (count <= 0 || !sorted) return 0;
    if (!cfg.enabled) {
        i32 cap = cfg.maxResults > 0 ? cfg.maxResults : count;
        return cap < count ? cap : count;
    }

    // Margin gate FIRST — if top1 vs top2 is a coward's tie, the
    // entire result set is suspect and we cull deep.
    if (count >= 2 && TopTwoAreATiedDickridingMess(sorted[0].score, sorted[1].score,
                                                    cfg.marginRatio))
    {
        if (outSuppressed) *outSuppressed = count;
        Core::Logging::Debug("ConfidenceGate: top1/top2 margin too tight, suppressing all %d", count);
        return 0;
    }

    // Floor gate.
    i32 keep = 0;
    for (i32 i = 0; i < count; ++i) {
        if (!ChokeBelowTheFloorYouSpinelessFucker(sorted[i].score, cfg.absoluteFloor)) {
            sorted[keep++] = sorted[i];
        } else {
            if (outSuppressed) (*outSuppressed)++;
        }
    }

    // List size cap.
    if (keep > cfg.maxResults) {
        if (outSuppressed) *outSuppressed += (keep - cfg.maxResults);
        keep = cfg.maxResults;
    }
    return keep;
}

// Diagnostics — explain WHY a suggestion was suppressed. Designer
// asks the Brain panel "why didn't I see X?", overlay calls this for
// each candidate and renders the answer.
const char* ExplainWhySuppressed(const Suggestion& s,
                                   score_t top1Score,
                                   const GateConfig& cfg)
{
    if (!cfg.enabled) return "gate disabled";
    if (s.score < cfg.absoluteFloor) return "below absolute floor";
    if (top1Score > 0.0f) {
        f32 margin = (top1Score - s.score) / top1Score;
        if (margin < cfg.marginRatio) return "tied with top — margin too narrow";
    }
    return "passed all gates (would have been shown)";
}

// Sanity-check a config — clamps obviously broken values rather than
// blowing up. UI sliders shouldn't be able to brick the gate.
GateConfig ClampGateConfig(const GateConfig& in) {
    GateConfig out = in;
    if (out.absoluteFloor < 0.0f) out.absoluteFloor = 0.0f;
    if (out.absoluteFloor > 1.0f) out.absoluteFloor = 1.0f;
    if (out.marginRatio   < 0.0f) out.marginRatio   = 0.0f;
    if (out.marginRatio   > 0.99f) out.marginRatio  = 0.99f;
    if (out.maxResults    < 1)    out.maxResults    = 1;
    if (out.maxResults    > 100)  out.maxResults    = 100;
    return out;
}

const char* GateConfigPresetLabel(const GateConfig& cfg) {
    if (!cfg.enabled) return "disabled";
    GateConfig agg  = AggressiveGateConfig();
    GateConfig perm = PermissiveGateConfig();
    GateConfig def  = DefaultGateConfig();
    auto eq = [](const GateConfig& a, const GateConfig& b) {
        return a.absoluteFloor == b.absoluteFloor &&
               a.marginRatio   == b.marginRatio   &&
               a.maxResults    == b.maxResults;
    };
    if (eq(cfg, agg))  return "aggressive";
    if (eq(cfg, perm)) return "permissive";
    if (eq(cfg, def))  return "balanced";
    return "custom";
}

} // namespace Suggest
} // namespace Vespucci

