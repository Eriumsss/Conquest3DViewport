// SuggestionTypes.h
// =============================================================================
// THE PUBLIC SHAPES THE RANKER HANDS BACK TO THE UI
// =============================================================================
// Written by: Eriumsss
//
// Every layer of the Suggest pipeline produces or consumes one of
// these shapes. Keep this header LIGHT — pulled in from a dozen
// places, slow includes here propagate everywhere.
// =============================================================================

#ifndef VESPUCCI_SUGGEST_SUGGESTIONTYPES_H_
#define VESPUCCI_SUGGEST_SUGGESTIONTYPES_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"

namespace Vespucci {
namespace Suggest {

// One reason chip attached to a suggestion. The UI renders these as
// small badges next to the suggested target.
struct ReasonChip {
    Core::StringRef text;       // localized, points into a string pool
    f32             contribution; // how much this reason added to the score
};

// One suggestion = one ranked target with score, reasons, and the
// payload needed to commit it through Apply.
struct Suggestion {
    Vespucci::EntityIndex target;
    Guid                  targetGuid;
    score_t               score;
    conf_t                confidence;
    Core::StringRef       eventName;        // pre-resolved canonical event
    Core::StringRef       actionName;       // pre-resolved canonical action
    f32                   suggestedDelay;
    bool                  suggestedSticky;
    ReasonChip            reasons[4];       // up to 4 reasons (cap kMaxReasonsPerSuggestion+1)
    i32                   reasonCount;
    bool                  faulted;          // ranker hit a runtime issue
};

// The bundle returned from a ranker query. Holds top-K suggestions
// plus diagnostic info the debug overlay reads.
struct SuggestionList {
    Suggestion suggestions[Limits::kMaxCandidatesPerQuery];
    i32        count;
    i32        candidatePoolSize;       // how many candidates entered the ranker
    i32        candidatesAfterHardFilter; // how many survived compat
    i32        suppressedByConfidence;
    i32        rulesEvaluated;
    f32        rankerMs;                // wallclock for this query
    SnapshotVersion snapshotVersionUsed;
};

// Query the ranker accepts. Filled by the UI before the call.
struct SuggestionQuery {
    Guid               sourceGuid;
    Core::StringRef    eventName;       // canonical lowercase
    i32                topK;            // requested top-K (capped at limits)
    bool               wantReasons;
    bool               applyConfidenceGate;
    bool               useSpatialFeatures; // false to skip the spatial index
};

} // namespace Suggest
} // namespace Vespucci

#endif // VESPUCCI_SUGGEST_SUGGESTIONTYPES_H_
