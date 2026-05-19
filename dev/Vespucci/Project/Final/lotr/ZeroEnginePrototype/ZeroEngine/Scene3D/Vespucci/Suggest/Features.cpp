// Features.cpp
// =============================================================================
// SEVEN-FEATURE EXTRACTOR. NORMALIZED TO [0,1]. NO ALLOCS ON HOT PATH.
// =============================================================================
// Written by: Eriumsss

#include "Features.h"

#include "../Corpus/CorpusPriors.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Vespucci {
namespace Suggest {

namespace {
    static const i32 kMaxLev = 32;

    char LowerOne(char c) {
        if (c >= 'A' && c <= 'Z') return (char)(c + 32);
        return c;
    }

    bool IsBoundaryChar(char c) {
        return c == '_' || c == ' ' || c == '-' || c == '.';
    }
} // namespace

f32 Vec3DistOrZeroIfDeadGuids(const Vec3& a, const Vec3& b) {
    f32 dx = a.x - b.x;
    f32 dy = a.y - b.y;
    f32 dz = a.z - b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

i32 LevenshteinNoMercy(const Core::StringRef& a, const Core::StringRef& b) {
    if (a.size() == 0) return (i32)b.size();
    if (b.size() == 0) return (i32)a.size();
    if (a.size() > kMaxLev || b.size() > kMaxLev) {
        // Bail above cap - return worst-case to disqualify as a name match.
        return kMaxLev;
    }
    i32 prev[kMaxLev + 1];
    i32 curr[kMaxLev + 1];
    for (i32 j = 0; j <= (i32)b.size(); ++j) prev[j] = j;
    for (i32 i = 1; i <= (i32)a.size(); ++i) {
        curr[0] = i;
        for (i32 j = 1; j <= (i32)b.size(); ++j) {
            char ca = LowerOne(a.data()[i-1]);
            char cb = LowerOne(b.data()[j-1]);
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

f32 TokenJaccardSplitTheBitchassNamesUp(const Core::StringRef& a, const Core::StringRef& b) {
    static const i32 kMaxToks = 16;
    Core::StringRef toksA[kMaxToks];
    Core::StringRef toksB[kMaxToks];
    i32 nA = 0, nB = 0;

    auto Split = [&](const Core::StringRef& s, Core::StringRef* toks, i32& n) {
        n = 0;
        usize start = 0;
        for (usize i = 0; i < s.size() && n < kMaxToks; ++i) {
            char c = s.data()[i];
            bool boundary = IsBoundaryChar(c);
            // CamelCase boundary
            if (i > 0 && c >= 'A' && c <= 'Z') {
                char prev = s.data()[i-1];
                if (prev >= 'a' && prev <= 'z') boundary = true;
            }
            if (boundary && i > start) {
                toks[n++] = Core::StringRef(s.data() + start, i - start);
                start = i + (IsBoundaryChar(c) ? 1 : 0);
            }
        }
        if (n < kMaxToks && start < s.size()) {
            toks[n++] = Core::StringRef(s.data() + start, s.size() - start);
        }
    };
    Split(a, toksA, nA);
    Split(b, toksB, nB);
    if (nA == 0 || nB == 0) return 0.0f;

    i32 inter = 0;
    for (i32 i = 0; i < nA; ++i) {
        for (i32 j = 0; j < nB; ++j) {
            if (toksA[i].equalsIgnoreCase(toksB[j])) { inter++; break; }
        }
    }
    i32 unionSize = nA + nB - inter;
    if (unionSize == 0) return 0.0f;
    return (f32)inter / (f32)unionSize;
}

f32 LongestSharedPrefixFraction(const Core::StringRef& a, const Core::StringRef& b) {
    usize minLen = a.size() < b.size() ? a.size() : b.size();
    if (minLen == 0) return 0.0f;
    usize match = 0;
    for (; match < minLen; ++match) {
        if (LowerOne(a.data()[match]) != LowerOne(b.data()[match])) break;
    }
    return (f32)match / (f32)minLen;
}

bool AcronymMatchesYouLazyMotherfucker(const Core::StringRef& shortS, const Core::StringRef& longS) {
    if (shortS.size() == 0 || shortS.size() > 8) return false;
    if (shortS.size() >= longS.size()) return false;
    usize si = 0;
    bool atBoundary = true;
    for (usize i = 0; i < longS.size() && si < shortS.size(); ++i) {
        char c = longS.data()[i];
        if (atBoundary || (c >= 'A' && c <= 'Z')) {
            char want = LowerOne(shortS.data()[si]);
            char got  = LowerOne(c);
            if (want == got) {
                si++;
                atBoundary = false;
                continue;
            }
        }
        atBoundary = IsBoundaryChar(c);
    }
    return si == shortS.size();
}

f32 NameSimilarityHammerOutOfTwoStrings(const Core::StringRef& a, const Core::StringRef& b) {
    if (a.size() == 0 || b.size() == 0) return 0.0f;
    // Levenshtein-derived similarity in [0,1]
    i32 lev = LevenshteinNoMercy(a, b);
    f32 longLen = (f32)(a.size() > b.size() ? a.size() : b.size());
    f32 levSim = 1.0f - ((f32)lev / longLen);
    if (levSim < 0.0f) levSim = 0.0f;

    f32 jacc   = TokenJaccardSplitTheBitchassNamesUp(a, b);
    f32 prefix = LongestSharedPrefixFraction(a, b);
    f32 acroBonus = 0.0f;
    if (AcronymMatchesYouLazyMotherfucker(a, b) ||
        AcronymMatchesYouLazyMotherfucker(b, a)) acroBonus = 0.3f;

    // Weighted combo. Tuned by feel; corpus benchmark can override.
    f32 score = 0.45f * levSim + 0.30f * jacc + 0.15f * prefix + acroBonus;
    if (score > 1.0f) score = 1.0f;
    return score;
}

void ComputeAllFeaturesOrChokeOnEmpty(
    const Scene::SceneSnapshot& snap,
    const Corpus::CorpusPriors* priors,
    Vespucci::EntityIndex sourceIdx,
    const Core::StringRef& eventCanonical,
    Vespucci::EntityIndex candidateIdx,
    FeatureScores& out)
{
    std::memset(&out, 0, sizeof(out));
    const Scene::EntityRow* src = snap.entityAt(sourceIdx);
    const Scene::EntityRow* tgt = snap.entityAt(candidateIdx);
    if (!src || !tgt) return;

    // Type prior - log-prob from corpus, exp() to [0,1]-ish range.
    if (priors) {
        logprob_t lp = priors->logProb(src->typeId, eventCanonical, tgt->typeId);
        // Map log-prob to [0,1]: exp clamps small probabilities to a
        // floor so a never-seen pair still contributes a sliver.
        out.typePrior = std::exp(lp);
        if (out.typePrior > 1.0f) out.typePrior = 1.0f;
    } else {
        out.typePrior = 0.05f; // cold-start floor
    }
    out.eventPrior = out.typePrior; // approximation; doc-locked
                                     // backoff lives inside CorpusPriors

    out.nameSimilarity = NameSimilarityHammerOutOfTwoStrings(src->name, tgt->name);

    f32 dist = Vec3DistOrZeroIfDeadGuids(src->position, tgt->position);
    static const f32 kSpatialScale = 50.0f; // units; ranker tunes via weights
    out.spatialProximity = std::exp(-dist / kSpatialScale);

    out.sameLayer  = (src->layerGuid.valid() && src->layerGuid == tgt->layerGuid) ? 1.0f : 0.0f;
    out.sameParent = (src->parentGuid.valid() && src->parentGuid == tgt->parentGuid) ? 1.0f : 0.0f;
    out.recency    = 0.0f; // populated by RankerSelector from session-state

    // Overload penalty - target with too many existing inbound wires
    // gets dinged. The ranker does NOT have direct access to
    // WireGraphIndex here; it can be wired via a separate path. For
    // V1 we expose the slot at zero and let HybridRanker fill it.
    out.overloadPenalty = 0.0f;
}

} // namespace Suggest
} // namespace Vespucci
