// Features.h
// =============================================================================
// FEATURE EXTRACTORS - SEVEN AXES OF JUDGMENT FOR EVERY CANDIDATE
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked seven features: type prior, event prior, name similarity,
// spatial proximity, layer co-location, parent co-location, recency,
// minus an overload penalty. Every ranker calls these to compute the
// per-axis sub-scores; the rankers themselves differ only in HOW
// they weight + combine them.
//
// We do NOT bake weights into this file. Weights live in
// Suggest/weights.json and are read at startup, hot-reloadable from
// the DebugRanker overlay. This file is the math, period.
// =============================================================================

#ifndef VESPUCCI_SUGGEST_FEATURES_H_
#define VESPUCCI_SUGGEST_FEATURES_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"
#include "../Scene/SceneSnapshot.h"

namespace Vespucci {
namespace Corpus { class CorpusPriors; }
namespace Suggest {

// Per-feature score, normalized to [0, 1] (or with a documented
// out-of-range case for the overload penalty).
struct FeatureScores {
    f32 typePrior;
    f32 eventPrior;
    f32 nameSimilarity;
    f32 spatialProximity;
    f32 sameLayer;
    f32 sameParent;
    f32 recency;
    f32 overloadPenalty;     // subtracted from the final score
};

// Compute all seven features for a single candidate. Hot path; no
// allocations. Caller-supplied corpus may be NULL (cold-start path).
void ComputeAllFeaturesOrChokeOnEmpty(
    const Scene::SceneSnapshot& snap,
    const Corpus::CorpusPriors* priors,
    Vespucci::EntityIndex sourceIdx,
    const Core::StringRef& eventCanonical,
    Vespucci::EntityIndex candidateIdx,
    FeatureScores& out);

// Multi-signal name similarity (Levenshtein + token overlap + prefix
// + suffix + acronym). Returns [0, 1].
f32 NameSimilarityHammerOutOfTwoStrings(const Core::StringRef& a,
                                          const Core::StringRef& b);

// Levenshtein distance on lowercase ASCII. Capped at 32 for cost.
i32 LevenshteinNoMercy(const Core::StringRef& a, const Core::StringRef& b);

// Token overlap (split on '_' and case-change boundaries). Returns
// the Jaccard index of token sets.
f32 TokenJaccardSplitTheBitchassNamesUp(const Core::StringRef& a, const Core::StringRef& b);

// Prefix bonus - longest shared prefix as a fraction of the shorter
// name's length. Used for naming-convention matches like
// "HelmsGate_Trigger" vs "HelmsGate_Controller".
f32 LongestSharedPrefixFraction(const Core::StringRef& a, const Core::StringRef& b);

// Acronym bonus - true if the shorter string is a CamelCase acronym
// of the longer. e.g. "VOT" matches "VOTrigger".
bool AcronymMatchesYouLazyMotherfucker(const Core::StringRef& shortS,
                                        const Core::StringRef& longS);

// Distance helper - L2 in 3D. Ranker uses this in spatialProximity.
f32 Vec3DistOrZeroIfDeadGuids(const Vec3& a, const Vec3& b);

} // namespace Suggest
} // namespace Vespucci

#endif // VESPUCCI_SUGGEST_FEATURES_H_
