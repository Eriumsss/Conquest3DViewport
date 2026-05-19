// FuzzyMatch.cpp
// =============================================================================
// Damerau-Levenshtein with cap + bigram Jaccard. Both ASCII-only
// because the LOTR:C corpus is ASCII and we are NOT shipping a
// half-cocked Unicode implementation that would lie about what it
// understands.
// =============================================================================
// Written by: Eriumsss

#include "FuzzyMatch.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace Vespucci {
namespace Autocomplete {

namespace {
    inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
} // namespace

i32 DamerauLevDistanceCappedOrChokeOnHugeStrings(const Core::StringRef& a,
                                                  const Core::StringRef& b,
                                                  i32 maxDist)
{
    if (a.size() == 0) return (i32)b.size();
    if (b.size() == 0) return (i32)a.size();
    static const i32 kMaxLen = 64;
    if (a.size() > kMaxLen || b.size() > kMaxLen) return maxDist + 1;

    // Two-row rolling array + adjacent-row for transposition.
    i32 prevPrev[kMaxLen + 2];
    i32 prev[kMaxLen + 2];
    i32 cur[kMaxLen + 2];
    for (i32 j = 0; j <= (i32)b.size(); ++j) prev[j] = j;
    for (i32 j = 0; j <= (i32)b.size() + 1; ++j) prevPrev[j] = (i32)b.size() + 2;

    for (i32 i = 1; i <= (i32)a.size(); ++i) {
        cur[0] = i;
        i32 minRow = cur[0];
        for (i32 j = 1; j <= (i32)b.size(); ++j) {
            char ca = Lower(a.data()[i-1]);
            char cb = Lower(b.data()[j-1]);
            i32 cost = (ca == cb) ? 0 : 1;
            i32 ins = cur[j-1] + 1;
            i32 del = prev[j]   + 1;
            i32 sub = prev[j-1] + cost;
            i32 best = ins; if (del < best) best = del; if (sub < best) best = sub;
            // Transposition (Damerau).
            if (i > 1 && j > 1) {
                char paa = Lower(a.data()[i-2]);
                char pbb = Lower(b.data()[j-2]);
                if (ca == pbb && cb == paa) {
                    i32 trans = prevPrev[j-2] + cost;
                    if (trans < best) best = trans;
                }
            }
            cur[j] = best;
            if (best < minRow) minRow = best;
        }
        if (minRow > maxDist) return maxDist + 1; // early bail
        std::memcpy(prevPrev, prev, sizeof(prev));
        std::memcpy(prev, cur, sizeof(cur));
    }
    return prev[(i32)b.size()];
}

f32 BigramJaccardSimilarity(const Core::StringRef& a, const Core::StringRef& b) {
    if (a.size() < 2 || b.size() < 2) return 0.0f;
    std::unordered_set<u32> sa, sb;
    for (usize i = 0; i + 1 < a.size(); ++i) {
        u32 g = ((u32)Lower(a.data()[i]) << 8) | (u32)Lower(a.data()[i+1]);
        sa.insert(g);
    }
    for (usize i = 0; i + 1 < b.size(); ++i) {
        u32 g = ((u32)Lower(b.data()[i]) << 8) | (u32)Lower(b.data()[i+1]);
        sb.insert(g);
    }
    i32 inter = 0;
    for (std::unordered_set<u32>::const_iterator it = sa.begin(); it != sa.end(); ++it) {
        if (sb.find(*it) != sb.end()) inter++;
    }
    i32 unionSize = (i32)(sa.size() + sb.size() - inter);
    if (unionSize == 0) return 0.0f;
    return (f32)inter / (f32)unionSize;
}

f32 FuzzyScoreForReranking(const Core::StringRef& query, const Core::StringRef& cand) {
    if (query.size() == 0 || cand.size() == 0) return 0.0f;
    i32 d = DamerauLevDistanceCappedOrChokeOnHugeStrings(query, cand, 4);
    f32 longest = (f32)(query.size() > cand.size() ? query.size() : cand.size());
    f32 levSim = 1.0f - ((f32)d / longest);
    if (levSim < 0.0f) levSim = 0.0f;
    f32 jacc = BigramJaccardSimilarity(query, cand);
    return 0.6f * levSim + 0.4f * jacc;
}

} // namespace Autocomplete
} // namespace Vespucci
