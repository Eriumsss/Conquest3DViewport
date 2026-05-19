// FuzzyMatch.h
// =============================================================================
// FUZZY MATCHING - FOR WHEN THE DESIGNER TYPES "OnTrgger" AND MEANS "OnTrigger"
// =============================================================================
// Written by: Eriumsss
//
// The prefix index handles exact prefix matches. Fuzzy match handles
// the typo case: drop one character, swap two adjacent characters,
// substitute one for another. Damerau-Levenshtein distance with a
// hard cap at 2 edits because anything past that is the user typing
// a different goddamn word, not a typo.
//
// We also expose an n-gram based "similarity score in [0,1]" for the
// Suggest layer's name-similarity feature in cases where the prefix
// path returns nothing usable.
// =============================================================================

#ifndef VESPUCCI_AUTOCOMPLETE_FUZZYMATCH_H_
#define VESPUCCI_AUTOCOMPLETE_FUZZYMATCH_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"

namespace Vespucci {
namespace Autocomplete {

// Damerau-Levenshtein with adjacent transposition support, capped
// at maxDist for early bail.
i32 DamerauLevDistanceCappedOrChokeOnHugeStrings(const Core::StringRef& a,
                                                  const Core::StringRef& b,
                                                  i32 maxDist);

// N-gram (bigram) Jaccard similarity in [0, 1]. Cheaper than
// Levenshtein and good enough for "are these two strings vibes-aligned".
f32 BigramJaccardSimilarity(const Core::StringRef& a, const Core::StringRef& b);

// Combined fuzzy score in [0, 1] - the one the Suggest layer calls.
// Internally weights Damerau-Lev and bigram Jaccard.
f32 FuzzyScoreForReranking(const Core::StringRef& query, const Core::StringRef& candidate);

} // namespace Autocomplete
} // namespace Vespucci

#endif // VESPUCCI_AUTOCOMPLETE_FUZZYMATCH_H_
