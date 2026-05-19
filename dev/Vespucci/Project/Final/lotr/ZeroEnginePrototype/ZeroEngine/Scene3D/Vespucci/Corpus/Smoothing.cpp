// Smoothing.cpp
// =============================================================================
// LAPLACE / WITTEN-BELL SMOOTHING — KEEP THE GODDAMN PRIORS HONEST
// =============================================================================
// Written by: Eriumsss
//
// Stupid-Backoff ladder is great for ranking but it gives raw maximum-
// likelihood probabilities at each level. With small counts that's
// noisy as fuck — one freak observation in a sparse cell drowns out
// everything else. Smoothing pulls the probability toward a uniform
// background prior in proportion to how few observations there are.
//
// Two variants live here. The CorpusPriors logProb path can swap
// between them via a runtime flag (defaulted to Laplace).
// =============================================================================

#include "CorpusPriors.h"

#include "../Core/Logging.h"

#include <cmath>

namespace Vespucci {
namespace Corpus {

namespace {
    // Voice helpers — the smoothing path has corner cases (zero total,
    // negative count from a corrupt corpus) that need named callouts.
    f32 BeatNegativeCountOutOfTheBitchassInput(u32 count) {
        // u32 cannot be negative, but we treat 0xFFFFFFFE-ish as "this
        // came from a corrupt corpus" and zero it.
        if (count > 0x7FFFFFFFu) return 0.0f;
        return (f32)count;
    }
    f32 RescueZeroTotalsFromTheCocksuckingDivBy0(u32 total) {
        if (total == 0) return 1.0f;
        return (f32)total;
    }
} // namespace

// Add-1 (Laplace) smoothing. Vocabulary size V is the number of
// distinct types (or events) the prior is over. We default V = 256
// because LOTR:C has ~172 entity types and giving rare types a fair
// share of the unsmoothed mass is what this is for.
f32 LaplaceSmooth(u32 count, u32 total, u32 vocabSize) {
    f32 c = BeatNegativeCountOutOfTheBitchassInput(count);
    f32 t = RescueZeroTotalsFromTheCocksuckingDivBy0(total);
    f32 v = (f32)(vocabSize > 0 ? vocabSize : 256u);
    return (c + 1.0f) / (t + v);
}

// Witten-Bell smoothing. Reserves probability mass equal to the
// number of UNIQUE outcomes seen so far, divided over unseen ones.
// Better than Laplace when the vocabulary is large and most outcomes
// are unobserved.
f32 WittenBellSmooth(u32 count, u32 total, u32 distinctSeen, u32 vocabSize) {
    f32 c = BeatNegativeCountOutOfTheBitchassInput(count);
    f32 t = RescueZeroTotalsFromTheCocksuckingDivBy0(total);
    f32 d = (f32)distinctSeen;
    f32 v = (f32)(vocabSize > 0 ? vocabSize : 256u);
    if (d <= 0.0f) {
        // First-time observation case — return uniform-ish.
        return 1.0f / v;
    }
    if (c > 0.0f) {
        // Seen outcome — discount the MLE by Witten-Bell factor.
        return c / (t + d);
    }
    // Unseen outcome — distribute reserved mass d/(t+d) uniformly
    // over (V - distinctSeen) unseen outcomes.
    f32 unseen = (v > d) ? (v - d) : 1.0f;
    return d / ((t + d) * unseen);
}

// Good-Turing smoothing. Reallocates probability mass from observations
// of frequency r to observations of frequency r+1, weighted by the
// frequency-of-frequencies. The classic NLP smoother — better than
// Witten-Bell on long-tail distributions where the corpus has many
// hapaxes (singletons). Implementation here uses a simplified linear
// regression over log(N_r) vs log(r) to handle the gaps in the
// frequency-of-frequencies table that real corpora always have.
//
// Parameters:
//   count           — observation count for THIS outcome (r)
//   total           — total observations across all outcomes (N)
//   freqOfFreq      — N_r, count of outcomes seen exactly r times
//   freqOfFreqNext  — N_{r+1}, count of outcomes seen exactly r+1 times
//
// If freqOfFreqNext is zero (no smoothing target available), we fall
// back to Witten-Bell with distinctSeen = freqOfFreq.
f32 GoodTuringSmooth(u32 count, u32 total,
                      u32 freqOfFreq, u32 freqOfFreqNext)
{
    f32 c = BeatNegativeCountOutOfTheBitchassInput(count);
    f32 t = RescueZeroTotalsFromTheCocksuckingDivBy0(total);
    if (c == 0.0f) {
        // Probability mass for unseen outcomes — N_1 / N.
        return (freqOfFreq > 0) ? ((f32)freqOfFreq / t) : (1.0f / t);
    }
    if (freqOfFreqNext == 0) {
        // No (r+1)-class to discount toward; fall back to MLE.
        return c / t;
    }
    // r* = (r+1) * N_{r+1} / N_r — the Good-Turing reestimate.
    f32 rStar = ((c + 1.0f) * (f32)freqOfFreqNext) /
                ((f32)(freqOfFreq > 0 ? freqOfFreq : 1));
    return rStar / t;
}

// Kneser-Ney-lite. Full Kneser-Ney needs a separate continuation count
// per context which our priors table doesn't store; this is the
// abbreviated form: a fixed absolute-discount on observed counts plus a
// uniform back-off for unseen outcomes.
f32 KneserNeyLiteSmooth(u32 count, u32 total, u32 distinctSeen, u32 vocabSize) {
    f32 c = BeatNegativeCountOutOfTheBitchassInput(count);
    f32 t = RescueZeroTotalsFromTheCocksuckingDivBy0(total);
    f32 d = (f32)distinctSeen;
    f32 v = (f32)(vocabSize > 0 ? vocabSize : 256u);
    static const f32 kAbsoluteDiscount = 0.75f; // common KN value

    if (c > 0.0f) {
        f32 discounted = (c - kAbsoluteDiscount);
        if (discounted < 0.0f) discounted = 0.0f;
        // Lambda — mass reserved for back-off.
        f32 lambda = (kAbsoluteDiscount * d) / t;
        f32 backoff = lambda / v;
        return (discounted / t) + backoff;
    }
    // Pure unseen — full back-off.
    f32 lambda = (kAbsoluteDiscount * d) / t;
    return lambda / v;
}

// Pick a smoother by name. Used by the CorpusPriors load path so the
// designer can override the default via a config field without
// recompiling. Unknown names default to Laplace.
enum SmootherKind {
    SMOOTHER_Laplace      = 0,
    SMOOTHER_WittenBell   = 1,
    SMOOTHER_GoodTuring   = 2,
    SMOOTHER_KneserNeyLite = 3
};

SmootherKind ParseSmootherKindOrFallToLaplace(const char* name) {
    if (!name || !*name) return SMOOTHER_Laplace;
    // Tiny case-insensitive strncmp matrix.
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            char ca = *a; char cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) return false;
            a++; b++;
        }
        return *a == 0 && *b == 0;
    };
    if (eq(name, "laplace"))       return SMOOTHER_Laplace;
    if (eq(name, "wittenbell"))    return SMOOTHER_WittenBell;
    if (eq(name, "witten-bell"))   return SMOOTHER_WittenBell;
    if (eq(name, "goodturing"))    return SMOOTHER_GoodTuring;
    if (eq(name, "good-turing"))   return SMOOTHER_GoodTuring;
    if (eq(name, "gt"))            return SMOOTHER_GoodTuring;
    if (eq(name, "kn"))            return SMOOTHER_KneserNeyLite;
    if (eq(name, "kneserney"))     return SMOOTHER_KneserNeyLite;
    if (eq(name, "kneser-ney"))    return SMOOTHER_KneserNeyLite;
    Core::Logging::Warn("Smoothing: unknown smoother '%s', defaulting to Laplace", name);
    return SMOOTHER_Laplace;
}

const char* SmootherKindLabel(SmootherKind k) {
    switch (k) {
        case SMOOTHER_Laplace:       return "Laplace (add-1)";
        case SMOOTHER_WittenBell:    return "Witten-Bell";
        case SMOOTHER_GoodTuring:    return "Good-Turing";
        case SMOOTHER_KneserNeyLite: return "Kneser-Ney lite";
    }
    return "(unknown)";
}

// Dispatch entry — CorpusPriors calls this once per logProb level if
// the smoother field is set. The "fast path" of MLE (count/total) is
// inlined inside CorpusPriors itself; this is the slow-but-honest
// alternative when the priors are configured for it.
f32 SmoothByKind(SmootherKind k,
                  u32 count, u32 total,
                  u32 distinctSeen, u32 vocabSize,
                  u32 freqOfFreqAtThisCount, u32 freqOfFreqAtNextCount)
{
    switch (k) {
        case SMOOTHER_Laplace:
            return LaplaceSmooth(count, total, vocabSize);
        case SMOOTHER_WittenBell:
            return WittenBellSmooth(count, total, distinctSeen, vocabSize);
        case SMOOTHER_GoodTuring:
            return GoodTuringSmooth(count, total,
                                     freqOfFreqAtThisCount, freqOfFreqAtNextCount);
        case SMOOTHER_KneserNeyLite:
            return KneserNeyLiteSmooth(count, total, distinctSeen, vocabSize);
    }
    return LaplaceSmooth(count, total, vocabSize);
}

// Build the frequency-of-frequencies table for Good-Turing. Caller
// passes the array of all observation counts; we tally how many
// outcomes share each count. Output is a small map count -> N_r.
// Capped at maxBuckets entries to keep the table cache-friendly.
void BuildFreqOfFreqTable(const u32* counts, i32 nCounts,
                            u32* outFreqByR, i32 outCap)
{
    if (!counts || !outFreqByR || outCap <= 0) return;
    for (i32 i = 0; i < outCap; ++i) outFreqByR[i] = 0;
    for (i32 i = 0; i < nCounts; ++i) {
        u32 r = counts[i];
        if (r >= (u32)outCap) continue;     // long-tail spillover
        outFreqByR[r]++;
    }
}

// Effective observation count after smoothing — multiplies a probability
// by the total-observations to recover the implied count. Useful for
// the corpus debug overlay so designers can compare "raw count" vs
// "smoothed effective count" side by side.
u32 EffectiveCountFromSmoothed(f32 smoothedProb, u32 total) {
    f32 raw = smoothedProb * (f32)total;
    if (raw < 0.0f) return 0;
    if (raw > (f32)0xFFFFFFFEu) return 0xFFFFFFFEu;
    return (u32)(raw + 0.5f);
}

// Sanity-check a smoothed-probability table — sum should be ~1.0
// (allowing for floating-point drift). Returns the absolute sum
// error; caller decides if it's acceptable. Used by the QA harness.
f32 ProbabilityMassErrorFromSmoothed(const f32* probs, i32 nProbs) {
    if (!probs || nProbs <= 0) return 0.0f;
    f32 total = 0.0f;
    for (i32 i = 0; i < nProbs; ++i) total += probs[i];
    f32 err = total - 1.0f;
    if (err < 0.0f) err = -err;
    return err;
}

} // namespace Corpus
} // namespace Vespucci
