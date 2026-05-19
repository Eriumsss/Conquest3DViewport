// ReasonBuilder.h
// =============================================================================
// REASON BUILDER - WHY THE FUCK SHOULD THE DESIGNER TRUST THIS SUGGESTION
// =============================================================================
// Written by: Eriumsss
//
// Every suggestion ships with up to 3 reason chips. The doc-locked
// rule: "no black box". A designer reading "ScoreManager (0.87)"
// learns nothing; "ScoreManager - same Mission layer, common after
// OnCapture, used in your last wire" sells the suggestion.
//
// We build reasons FROM the FeatureScores struct. Each non-zero
// feature contributes one chip text + its sub-score. The top 3 by
// contribution survive into the Suggestion.
// =============================================================================

#ifndef VESPUCCI_SUGGEST_REASONBUILDER_H_
#define VESPUCCI_SUGGEST_REASONBUILDER_H_

#include "SuggestionTypes.h"
#include "Features.h"

namespace Vespucci {
namespace Suggest {

void BuildReasonsOrLeaveTheFuckersBlank(const FeatureScores& fs,
                                         ReasonChip* outChips,
                                         i32 maxChips,
                                         i32& outCount);

} // namespace Suggest
} // namespace Vespucci

#endif // VESPUCCI_SUGGEST_REASONBUILDER_H_
