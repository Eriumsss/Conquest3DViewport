// FrequencyRanker.cpp
// =============================================================================
// PURE-CORPUS-PRIOR RANKING. NO BELLS, NO WHISTLES, JUST COUNTS.
// =============================================================================
// Written by: Eriumsss
//
// Score = blend of (a) exp(logProb from CorpusPriors) for the matched
// (source-type, event, target-type) triple, with backoff cascade across
// the doc-locked five priors levels, and (b) a small novelty bump for
// rarely-suggested types so the corpus doesn't lock the designer into
// the most-frequent answer forever.
//
// This ranker is the BASELINE every other strategy is benchmarked
// against. If Hybrid does not beat Frequency on a held-out set,
// Hybrid's weights are wrong and the cookie crumbles.
//
// Failure modes:
//   - No priors bound      → returns 0 (no opinion). Caller falls back
//                            to TypeDirected or whatever the selector
//                            picks next.
//   - Empty source/target  → returns 0 with one debug reason.
//   - Cold-start corpus    → backoff cascade reaches the global floor
//                            and returns ~0.05; the gate suppresses.
// =============================================================================

#include "../IRanker.h"
#include "../../Corpus/CorpusPriors.h"
#include "../../Core/Logging.h"
#include "../../Scene/SceneSnapshot.h"

#include <cmath>
#include <cstring>

namespace Vespucci {
namespace Suggest {

namespace {
    // Soft caps so a runaway log-prob (e.g. observation count blowup
    // from a corrupt corpus blob) cannot land outside [0,1] and break
    // the gate's margin math.
    static const f32 kFreqScoreFloor = 0.0f;
    static const f32 kFreqScoreCeil  = 1.0f;

    // Per-backoff-level human label. Index matches CorpusPriors's
    // lastLevelFired() return value.
    const char* BackoffLevelLabel(i32 level) {
        switch (level) {
            case 1: return "exact (source, event, target) corpus hit";
            case 2: return "source-type + event corpus match";
            case 3: return "event-name corpus match";
            case 4: return "type-only corpus prior";
            case 5: return "global cross-map fallback";
            default:return "no corpus signal — using uniform prior";
        }
    }

    // Tiny novelty bump — when the candidate's type is observed in
    // fewer than N% of all (source-type, event) wires, give it a
    // small uplift so the ranker doesn't keep promoting the same
    // top-1 entity to oblivion. Helps the long tail.
    f32 NoveltyBumpForRareTypes(const Corpus::CorpusPriors* priors,
                                  TypeId src, const Core::StringRef& evt,
                                  TypeId cand)
    {
        if (!priors) return 0.0f;
        // Stats give us aggregate observations; use the lastLevelFired
        // to gauge specificity. Levels 4-5 are vague prior — the type
        // is rare in the corpus → small bump.
        i32 lvl = priors->lastLevelFired();
        if (lvl >= 4) return 0.04f;
        return 0.0f;
        (void)src; (void)evt; (void)cand;
    }
} // namespace

class FrequencyRanker : public IRanker {
public:
    explicit FrequencyRanker(const Corpus::CorpusPriors* priors)
        : m_priors(priors), m_queriesEvaluated(0), m_lastFloorHits(0) {}

    Kind kind() const { return KIND_Frequency; }
    const char* name() const { return "Frequency"; }

    // Internal stats accessor for the debug overlay. Always live, no
    // gating — designer can read when stuck on "why is the ranker
    // returning all 0.05?".
    i32 queriesEvaluated() const { return m_queriesEvaluated; }
    i32 lastFloorHits()    const { return m_lastFloorHits; }

    score_t scoreCandidate(const Scene::SceneSnapshot& snap,
                            Vespucci::EntityIndex sourceIdx,
                            const Core::StringRef& eventCanonical,
                            Vespucci::EntityIndex candidateIdx,
                            ReasonChip* outReasons,
                            i32 maxReasons,
                            i32& outReasonCount) const
    {
        outReasonCount = 0;
        m_queriesEvaluated++;

        const Scene::EntityRow* src = snap.entityAt(sourceIdx);
        const Scene::EntityRow* tgt = snap.entityAt(candidateIdx);
        if (!src || !tgt) return 0.0f;
        if (!m_priors) {
            if (outReasons && maxReasons > 0) {
                outReasons[0].text         = Core::StringRef("no corpus priors bound");
                outReasons[0].contribution = 0.0f;
                outReasonCount = 1;
            }
            return 0.0f;
        }

        // Primary signal — log-prob through the backoff cascade.
        logprob_t lp = m_priors->logProb(src->typeId, eventCanonical, tgt->typeId);
        score_t base = std::exp(lp);

        // Novelty bump for long-tail types; small enough that exact-
        // hits still win, big enough that a rare-but-legitimate target
        // isn't cucked by the corpus's bias toward what's already
        // popular.
        f32 novelty = NoveltyBumpForRareTypes(m_priors, src->typeId,
                                                eventCanonical, tgt->typeId);

        score_t s = base + novelty;
        if (s < kFreqScoreFloor) s = kFreqScoreFloor;
        if (s > kFreqScoreCeil)  s = kFreqScoreCeil;

        // If we landed near the floor it usually means cold-start —
        // tally for the dashboard so the designer knows the corpus
        // is empty rather than the ranker is broken.
        if (s < 0.06f) m_lastFloorHits++;

        if (outReasons && maxReasons > 0) {
            i32 lvl = m_priors->lastLevelFired();
            outReasons[0].text         = Core::StringRef(BackoffLevelLabel(lvl));
            outReasons[0].contribution = base;
            outReasonCount = 1;

            if (novelty > 0.0f && maxReasons > 1) {
                outReasons[1].text         = Core::StringRef("rare-type novelty bump");
                outReasons[1].contribution = novelty;
                outReasonCount = 2;
            }
        }
        return s;
    }

    // Optional rebind — used by the selector when Vespucci hot-reloads
    // a corpus from disk.
    void rebindPriors(const Corpus::CorpusPriors* p) { m_priors = p; }

private:
    const Corpus::CorpusPriors* m_priors;
    mutable i32 m_queriesEvaluated;
    mutable i32 m_lastFloorHits;
};

// Factory hook — the RankerSelector instantiates this strategy.
IRanker* MakeFrequencyRanker(const Corpus::CorpusPriors* priors) {
    return new FrequencyRanker(priors);
}

// Convenience accessor — debug overlay reads here to render the
// "queries evaluated" / "floor-hit ratio" telemetry.
i32 FrequencyRankerQueriesEvaluated(const IRanker* r) {
    if (!r || r->kind() != IRanker::KIND_Frequency) return 0;
    return static_cast<const FrequencyRanker*>(r)->queriesEvaluated();
}

i32 FrequencyRankerFloorHits(const IRanker* r) {
    if (!r || r->kind() != IRanker::KIND_Frequency) return 0;
    return static_cast<const FrequencyRanker*>(r)->lastFloorHits();
}

} // namespace Suggest
} // namespace Vespucci

