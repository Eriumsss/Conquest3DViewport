// HybridRanker.cpp
// =============================================================================
// THE GODDAMN DEFAULT - WEIGHTED LINEAR COMBO OF ALL SEVEN FEATURES
// =============================================================================
// Written by: Eriumsss
//
// This is the ranker the doc-locked architecture wants riding shotgun
// on the editor by default. Pulls FeatureScores from Features.cpp,
// multiplies each by its tunable weight, sums, subtracts the overload
// penalty. Weights load from Suggest/weights.json at startup and are
// hot-reloadable from the DebugRanker overlay.
// =============================================================================

#include "../IRanker.h"
#include "../Features.h"

#include "../../Core/JsonLite.h"
#include "../../Core/Logging.h"
#include "../../Core/PathUtils.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../Corpus/CorpusPriors.h"

#include <cstring>

namespace Vespucci {
namespace Suggest {

struct HybridWeights {
    f32 typePrior;
    f32 eventPrior;
    f32 nameSimilarity;
    f32 spatialProximity;
    f32 sameLayer;
    f32 sameParent;
    f32 recency;
    f32 overloadPenalty;
};

static HybridWeights DefaultHybridWeights() {
    HybridWeights w;
    w.typePrior        = 0.30f;
    w.eventPrior       = 0.25f;
    w.nameSimilarity   = 0.10f;
    w.spatialProximity = 0.08f;
    w.sameLayer        = 0.10f;
    w.sameParent       = 0.05f;
    w.recency          = 0.12f;
    w.overloadPenalty  = 0.10f;
    return w;
}

bool LoadHybridWeightsOrFallBackToBlessedDefaults(const char* jsonPath, HybridWeights& out) {
    out = DefaultHybridWeights();
    if (!jsonPath || !*jsonPath) return false;
    Core::JsonLite::Parser p;
    if (!p.parseFile(jsonPath)) {
        Core::Logging::Warn("HybridRanker: weights.json parse failed (%s); using defaults",
            p.lastError());
        return false;
    }
    const Core::JsonLite::Value* root = p.root();
    if (!root) return false;
    out.typePrior        = (f32)Core::JsonLite::OptDouble(root, "typePrior",        out.typePrior);
    out.eventPrior       = (f32)Core::JsonLite::OptDouble(root, "eventPrior",       out.eventPrior);
    out.nameSimilarity   = (f32)Core::JsonLite::OptDouble(root, "nameSimilarity",   out.nameSimilarity);
    out.spatialProximity = (f32)Core::JsonLite::OptDouble(root, "spatialProximity", out.spatialProximity);
    out.sameLayer        = (f32)Core::JsonLite::OptDouble(root, "sameLayer",        out.sameLayer);
    out.sameParent       = (f32)Core::JsonLite::OptDouble(root, "sameParent",       out.sameParent);
    out.recency          = (f32)Core::JsonLite::OptDouble(root, "recency",          out.recency);
    out.overloadPenalty  = (f32)Core::JsonLite::OptDouble(root, "overloadPenalty",  out.overloadPenalty);
    Core::Logging::Info("HybridRanker: loaded weights from %s", jsonPath);
    return true;
}

class HybridRanker : public IRanker {
public:
    explicit HybridRanker(const Corpus::CorpusPriors* priors)
        : m_priors(priors), m_w(DefaultHybridWeights()) {}

    void setWeights(const HybridWeights& w) { m_w = w; }
    const HybridWeights& weights() const { return m_w; }

    Kind kind() const { return KIND_Hybrid; }
    const char* name() const { return "Hybrid"; }

    score_t scoreCandidate(const Scene::SceneSnapshot& snap,
                            Vespucci::EntityIndex sourceIdx,
                            const Core::StringRef& eventCanonical,
                            Vespucci::EntityIndex candidateIdx,
                            ReasonChip* outReasons,
                            i32 maxReasons,
                            i32& outReasonCount) const
    {
        outReasonCount = 0;
        FeatureScores fs;
        ComputeAllFeaturesOrChokeOnEmpty(snap, m_priors, sourceIdx, eventCanonical,
                                          candidateIdx, fs);

        score_t s = 0.0f;
        s += m_w.typePrior        * fs.typePrior;
        s += m_w.eventPrior       * fs.eventPrior;
        s += m_w.nameSimilarity   * fs.nameSimilarity;
        s += m_w.spatialProximity * fs.spatialProximity;
        s += m_w.sameLayer        * fs.sameLayer;
        s += m_w.sameParent       * fs.sameParent;
        s += m_w.recency          * fs.recency;
        s -= m_w.overloadPenalty  * fs.overloadPenalty;
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;

        // Reason building delegates to ReasonBuilder via the scoreboard.
        // We build chips here directly because Hybrid wants to weight
        // them by the SAME w.* weights for consistency in the UI.
        struct Slot { f32 score; const char* text; };
        Slot pool[] = {
            { m_w.typePrior        * fs.typePrior,        "common type pattern" },
            { m_w.eventPrior       * fs.eventPrior,       "matches this event" },
            { m_w.nameSimilarity   * fs.nameSimilarity,   "name match" },
            { m_w.spatialProximity * fs.spatialProximity, "spatially nearby" },
            { m_w.sameLayer        * fs.sameLayer,        "same layer" },
            { m_w.sameParent       * fs.sameParent,       "same parent" },
            { m_w.recency          * fs.recency,          "recent in your session" }
        };
        i32 take = (maxReasons < 7) ? maxReasons : 7;
        // Top-K by contribution, simple selection sort.
        for (i32 i = 0; i < take; ++i) {
            i32 bestJ = i;
            for (i32 j = i + 1; j < 7; ++j) {
                if (pool[j].score > pool[bestJ].score) bestJ = j;
            }
            Slot t = pool[i]; pool[i] = pool[bestJ]; pool[bestJ] = t;
            if (pool[i].score <= 0.0f) break;
            outReasons[outReasonCount].text         = Core::StringRef(pool[i].text);
            outReasons[outReasonCount].contribution = pool[i].score;
            outReasonCount++;
        }
        return s;
    }

private:
    const Corpus::CorpusPriors* m_priors;
    HybridWeights               m_w;
};

IRanker* MakeHybridRanker(const Corpus::CorpusPriors* priors) {
    return new HybridRanker(priors);
}

} // namespace Suggest
} // namespace Vespucci
