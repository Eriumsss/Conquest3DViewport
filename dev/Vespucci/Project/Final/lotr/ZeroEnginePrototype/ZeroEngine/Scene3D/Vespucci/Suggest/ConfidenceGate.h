// ConfidenceGate.h
// =============================================================================
// THREE-GATE CONFIDENCE FILTER - WRONG SUGGESTION > NO SUGGESTION
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked three gates from both design docs:
//   1. Absolute score floor - below this, suggestion is too weak to show.
//   2. Top-1 / top-2 margin - if the top two are nearly tied, suggest
//      neither (the ranker is admitting uncertainty).
//   3. List size cap - never show more than kMaxSidebarSuggestions
//      (5 in sidebar, 3 inline).
//
// Configurable via Suggest/weights.json. Hot-reloadable from the
// DebugRanker overlay.
// =============================================================================

#ifndef VESPUCCI_SUGGEST_CONFIDENCEGATE_H_
#define VESPUCCI_SUGGEST_CONFIDENCEGATE_H_

#include "../Core/VespucciTypes.h"
#include "SuggestionTypes.h"

namespace Vespucci {
namespace Suggest {

struct GateConfig {
    f32 absoluteFloor;     // discard below this score
    f32 marginRatio;       // require (top1 - top2) / top1 >= this
    i32 maxResults;        // hard cap
    bool enabled;
};

GateConfig DefaultGateConfig();

// Apply gates to a sorted (descending by score) list of candidates.
// Returns the number of survivors. Optionally fills outSuppressedCount
// with how many were dropped by the gates.
i32 ApplyGatesOrSuppressTheBitchassWeakOnes(
    Suggestion* sorted, i32 count,
    const GateConfig& cfg,
    i32* outSuppressedCount);

} // namespace Suggest
} // namespace Vespucci

#endif // VESPUCCI_SUGGEST_CONFIDENCEGATE_H_
