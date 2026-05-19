// AutocompleteRanker.cpp
// =============================================================================
// RANKER VARIANT FOR EVENT / ACTION / NAME PICKER POPUPS — PREFIX-AWARE
// =============================================================================
// Written by: Eriumsss
//
// Different beast from target ranking. Input is a partial string the
// designer is typing into the EventGraphEditor's event-name combo or
// the create-entity wizard's name field. Output is a ranked list of
// completion candidates pulled from the autocomplete prefix index
// (Trie / MARISA / Cedar / SortedVec — IndexFactory picks one).
//
// Score model:
//   base       = log(1 + usageCount)         — corpus popularity
//   prefixBonus = 0.5 if candidate starts with user input
//                  (case-insensitive), 0 otherwise
//   exactBonus  = 0.8 if candidate equals user input
//                  (designer fully typed it; promote it hard)
//   acronymBonus = 0.3 if user input is an initialism for the
//                  candidate ("OnC" -> "OnCapture")
//   fuzzPenalty = -0.1 * Levenshtein distance for non-prefix entries
//                  so misspellings can still surface in the long tail
//
// Three knobs:
//   - maxResults: hard cap on returned list size
//   - includeFuzzy: if false, only prefix matches survive
//   - corpusBoostScale: multiply base by this; 0 = ignore corpus
//
// Real-world failure modes:
//   - corpus wasn't ingested → all usageCount=0 → log() = 0 → ranking
//     falls back to prefix-only. Still useful but loses the popularity
//     signal. The Brain panel surfaces this as "corpus empty" warning.
// =============================================================================

#include "SuggestionTypes.h"
#include "../Core/StringRef.h"
#include "../Core/Logging.h"
#include "../Corpus/CorpusPriors.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace Vespucci {
namespace Suggest {

struct AutocompleteCandidate {
    Core::StringRef name;
    u32             usageCount;
    score_t         score;
    // Provenance — which scoring path landed this candidate. Useful
    // for the debug overlay's "why is this on top?" question.
    enum Provenance {
        PROV_PrefixHit  = 0,
        PROV_ExactHit   = 1,
        PROV_AcronymHit = 2,
        PROV_FuzzyHit   = 3
    };
    Provenance      provenance;
};

namespace {
    // Acronym match — does `shortS` walk the capital letters of `longS`?
    // "OnC" matches "OnCapture", "OnE" matches "OnEnter".
    bool AcronymMatchesIgnoreCase(const Core::StringRef& shortS,
                                    const Core::StringRef& longS)
    {
        if (shortS.size() == 0 || shortS.size() > 8) return false;
        if (shortS.size() >= longS.size())             return false;
        usize si = 0;
        bool atBoundary = true;
        for (usize i = 0; i < longS.size() && si < shortS.size(); ++i) {
            char c = longS.data()[i];
            char want = shortS.data()[si];
            // Lower both sides for the compare.
            char wL = (want >= 'A' && want <= 'Z') ? (char)(want + 32) : want;
            char cL = (c    >= 'A' && c    <= 'Z') ? (char)(c    + 32) : c;
            if ((atBoundary || (c >= 'A' && c <= 'Z')) && wL == cL) {
                si++;
                atBoundary = false;
                continue;
            }
            atBoundary = (c == '_' || c == '.' || c == ' ' || c == '-');
        }
        return si == shortS.size();
    }

    // Tiny Levenshtein with a hard cap so we never blow stack on
    // pathological inputs. We only call it for non-prefix candidates
    // where the prefix-bonus path didn't fire.
    static const i32 kAcMaxLev = 24;
    i32 ShortLevenshtein(const Core::StringRef& a, const Core::StringRef& b) {
        if (a.size() == 0) return (i32)b.size();
        if (b.size() == 0) return (i32)a.size();
        if (a.size() > kAcMaxLev || b.size() > kAcMaxLev) return kAcMaxLev;
        i32 prev[kAcMaxLev + 1];
        i32 curr[kAcMaxLev + 1];
        for (i32 j = 0; j <= (i32)b.size(); ++j) prev[j] = j;
        for (i32 i = 1; i <= (i32)a.size(); ++i) {
            curr[0] = i;
            for (i32 j = 1; j <= (i32)b.size(); ++j) {
                char ca = a.data()[i-1];
                char cb = b.data()[j-1];
                if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                i32 cost = (ca == cb) ? 0 : 1;
                i32 ins = curr[j-1] + 1;
                i32 del = prev[j]   + 1;
                i32 sub = prev[j-1] + cost;
                i32 best = ins; if (del < best) best = del; if (sub < best) best = sub;
                curr[j] = best;
            }
            std::memcpy(prev, curr, sizeof(curr));
        }
        return prev[(i32)b.size()];
    }
} // namespace

// Take a list of candidates (already prefix-filtered by the prefix
// index for the prefix path; the fuzz path includes everything the
// caller fed in) and rerank them.
i32 RerankAutocompleteOrEatTheBitchassFiller(
    const Core::StringRef& userPrefix,
    const AutocompleteCandidate* candidates,
    i32 candCount,
    AutocompleteCandidate* outRanked,
    i32 maxOut)
{
    if (!candidates || candCount <= 0 || maxOut <= 0) return 0;

    std::vector<AutocompleteCandidate> work;
    work.reserve((size_t)candCount);
    for (i32 i = 0; i < candCount; ++i) {
        AutocompleteCandidate c = candidates[i];
        f32 base = std::log(1.0f + (f32)c.usageCount);

        bool exactPrefix = c.name.startsWithIgnoreCase(userPrefix);
        bool exactWhole  = c.name.equalsIgnoreCase(userPrefix);
        bool acronym     = AcronymMatchesIgnoreCase(userPrefix, c.name);

        f32 score = base;
        if (exactWhole) {
            score += 0.8f;
            c.provenance = AutocompleteCandidate::PROV_ExactHit;
        } else if (exactPrefix) {
            score += 0.5f;
            c.provenance = AutocompleteCandidate::PROV_PrefixHit;
        } else if (acronym) {
            score += 0.3f;
            c.provenance = AutocompleteCandidate::PROV_AcronymHit;
        } else {
            // Long-tail fuzz path. Compute Levenshtein distance and
            // ding the score per character. We only want this to
            // surface candidates the prefix index missed because the
            // user typo'd a letter.
            i32 lev = ShortLevenshtein(userPrefix, c.name);
            score -= 0.1f * (f32)lev;
            c.provenance = AutocompleteCandidate::PROV_FuzzyHit;
        }
        c.score = score;
        work.push_back(c);
    }

    std::sort(work.begin(), work.end(),
        [](const AutocompleteCandidate& a, const AutocompleteCandidate& b) {
            return a.score > b.score;
        });

    i32 take = (i32)work.size();
    if (take > maxOut) take = maxOut;
    for (i32 i = 0; i < take; ++i) outRanked[i] = work[(size_t)i];
    return take;
}

// Convenience overload — rerank ONLY prefix matches; anything that
// doesn't pass startsWithIgnoreCase gets dropped silently. Used by
// callers that want strict prefix completion (event-name combo box).
i32 RerankAutocompleteStrictPrefix(
    const Core::StringRef& userPrefix,
    const AutocompleteCandidate* candidates,
    i32 candCount,
    AutocompleteCandidate* outRanked,
    i32 maxOut)
{
    if (!candidates || candCount <= 0 || maxOut <= 0) return 0;
    std::vector<AutocompleteCandidate> work;
    work.reserve((size_t)candCount);
    for (i32 i = 0; i < candCount; ++i) {
        if (!candidates[i].name.startsWithIgnoreCase(userPrefix)) continue;
        AutocompleteCandidate c = candidates[i];
        f32 base = std::log(1.0f + (f32)c.usageCount);
        c.score = base + (c.name.equalsIgnoreCase(userPrefix) ? 0.8f : 0.5f);
        c.provenance = c.name.equalsIgnoreCase(userPrefix)
            ? AutocompleteCandidate::PROV_ExactHit
            : AutocompleteCandidate::PROV_PrefixHit;
        work.push_back(c);
    }
    std::sort(work.begin(), work.end(),
        [](const AutocompleteCandidate& a, const AutocompleteCandidate& b) {
            return a.score > b.score;
        });
    i32 take = (i32)work.size();
    if (take > maxOut) take = maxOut;
    for (i32 i = 0; i < take; ++i) outRanked[i] = work[(size_t)i];
    return take;
}

const char* ProvenanceLabel(AutocompleteCandidate::Provenance p) {
    switch (p) {
        case AutocompleteCandidate::PROV_ExactHit:   return "exact";
        case AutocompleteCandidate::PROV_PrefixHit:  return "prefix";
        case AutocompleteCandidate::PROV_AcronymHit: return "acronym";
        case AutocompleteCandidate::PROV_FuzzyHit:   return "fuzzy";
    }
    return "?";
}

} // namespace Suggest
} // namespace Vespucci

