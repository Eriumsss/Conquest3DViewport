// MLStubRanker.cpp
// =============================================================================
// PLACEHOLDER FOR A FUTURE LEARNED MODEL. WEIGHT-VECTOR DOT PRODUCT.
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked open question #3 was "MLStub keep or drop." User said
// keep. So here it is — a learned linear model in stub form. Loads
// a weight vector from a binary file (mlstub_weights.bin), dots it
// against the eight feature scalars, runs through a sigmoid, returns
// the score.
//
// We do NOT ship a TRAINED model in this codebase. A future training
// pipeline (label sessions from ReplayRecorder, gradient descent on
// Hybrid-as-baseline as ground truth) can write that file, and this
// ranker picks it up. Until then it uses zero-weights and behaves
// like "always returns 0" — exactly the kind of sandbag that lets
// the rest of the build system include this file without crashing
// while keeping the option to swap in a real model open.
//
// File format (mlstub_weights.bin):
//   bytes  0..31   8 × float32 weights (typePrior, eventPrior,
//                   nameSimilarity, spatialProximity, sameLayer,
//                   sameParent, recency, overloadPenaltyNeg)
//   bytes 32..35   1 × float32 bias
//   bytes 36..39   1 × uint32 magic '0xMLST' (=0x4D4C5354) — sanity check
//   bytes 40..47   1 × uint64 trained-on-replay-checksum (audit)
//
// The training tool will write a richer format eventually; this
// ranker reads the simplest possible shape so it can never crash on
// a half-cooked file.
// =============================================================================

#include "../IRanker.h"
#include "../Features.h"
#include "../../Corpus/CorpusPriors.h"
#include "../../Core/FileIO.h"
#include "../../Core/Logging.h"

#include <cmath>
#include <cstring>

namespace Vespucci {
namespace Suggest {

namespace {
    // Magic bytes for the weight file. Designer trying to point this
    // ranker at the wrong blob (e.g. a corpus.bin) will get a clean
    // refusal instead of garbage scores.
    static const u32 kMlstubMagic = 0x4D4C5354u; // 'MLST'

    // Per-feature human label — used in reason chips when MLStub
    // happens to assign large weight to one feature in particular.
    const char* FeatureNameAt(i32 i) {
        switch (i) {
            case 0: return "type prior (model)";
            case 1: return "event prior (model)";
            case 2: return "name similarity (model)";
            case 3: return "spatial proximity (model)";
            case 4: return "same-layer (model)";
            case 5: return "same-parent (model)";
            case 6: return "recency (model)";
            case 7: return "overload penalty (model)";
            default: return "unknown feature";
        }
    }
} // namespace

class MLStubRanker : public IRanker {
public:
    explicit MLStubRanker(const Corpus::CorpusPriors* priors)
        : m_priors(priors), m_loaded(false), m_trainingChecksum(0),
          m_queriesEvaluated(0)
    {
        std::memset(m_w, 0, sizeof(m_w));
        m_bias = 0.0f;
    }

    Kind kind() const { return KIND_MLStub; }
    const char* name() const { return "MLStub"; }

    // Load the weight blob. Returns true if the file is valid AND
    // the magic matches; logs a warning and falls back to zero
    // weights otherwise.
    bool LoadWeightsOrPretendItsZero(const char* path) {
        m_loaded = false;
        if (!path || !*path) return false;
        Core::Blob b = Core::ReadBlob(path);
        if (!b.data) {
            Core::Logging::Warn("MLStub: file '%s' not present, behaving like a sandbag",
                path);
            return false;
        }
        // Minimum header is 8 weights + 1 bias + 1 magic = 36 bytes.
        // Optional 8-byte training checksum trails. Anything shorter
        // is junk and we drop it on the floor.
        if (b.size < (i64)(sizeof(m_w) + sizeof(m_bias) + sizeof(u32))) {
            Core::Logging::Warn("MLStub: '%s' too short (%lld bytes) — refusing",
                path, (long long)b.size);
            Core::FreeBlob(b);
            return false;
        }

        std::memcpy(m_w, b.data, sizeof(m_w));
        std::memcpy(&m_bias, b.data + sizeof(m_w), sizeof(m_bias));
        u32 magic = 0;
        std::memcpy(&magic, b.data + sizeof(m_w) + sizeof(m_bias), sizeof(magic));
        if (magic != kMlstubMagic) {
            Core::Logging::Warn("MLStub: '%s' magic 0x%08X != expected 0x%08X — "
                "refusing to load. Is this actually an mlstub_weights.bin?",
                path, magic, kMlstubMagic);
            std::memset(m_w, 0, sizeof(m_w));
            m_bias = 0.0f;
            Core::FreeBlob(b);
            return false;
        }

        // Optional training-checksum field for audit.
        if (b.size >= (i64)(sizeof(m_w) + sizeof(m_bias) + sizeof(u32) + sizeof(u64))) {
            std::memcpy(&m_trainingChecksum,
                b.data + sizeof(m_w) + sizeof(m_bias) + sizeof(u32),
                sizeof(m_trainingChecksum));
        }

        Core::FreeBlob(b);
        m_loaded = true;
        Core::Logging::Info("MLStub: weights loaded from %s (bias=%.3f, "
            "training checksum=0x%016llX)",
            path, m_bias, (unsigned long long)m_trainingChecksum);
        return true;
    }

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

        if (!m_loaded) {
            // Honest no-op — caller is using us before training has
            // produced a model. Return 0; the selector should fall
            // back to Hybrid in this case.
            return 0.0f;
        }

        FeatureScores fs;
        ComputeAllFeaturesOrChokeOnEmpty(snap, m_priors, sourceIdx, eventCanonical,
                                          candidateIdx, fs);

        // Feature vector — same order as the weight file.
        f32 x[8] = {
            fs.typePrior, fs.eventPrior, fs.nameSimilarity, fs.spatialProximity,
            fs.sameLayer, fs.sameParent, fs.recency, -fs.overloadPenalty
        };

        f32 dot = m_bias;
        f32 perFeatureContribution[8];
        for (i32 i = 0; i < 8; ++i) {
            perFeatureContribution[i] = m_w[i] * x[i];
            dot += perFeatureContribution[i];
        }

        // Sigmoid to [0,1].
        score_t s = 1.0f / (1.0f + std::exp(-dot));

        // Reasons — pick the top contributing features by absolute
        // contribution magnitude, label with the model-specific
        // suffix so designers know this is the learned ranker
        // talking, not Hybrid.
        if (outReasons && maxReasons > 0) {
            i32 cap = (maxReasons < 8) ? maxReasons : 8;
            i32 picked[8] = {0,1,2,3,4,5,6,7};
            // Selection sort top-k by abs contribution.
            for (i32 i = 0; i < cap; ++i) {
                i32 bestJ = i;
                for (i32 j = i + 1; j < 8; ++j) {
                    if (std::fabs(perFeatureContribution[picked[j]]) >
                        std::fabs(perFeatureContribution[picked[bestJ]]))
                    {
                        bestJ = j;
                    }
                }
                i32 tmp = picked[i]; picked[i] = picked[bestJ]; picked[bestJ] = tmp;
            }
            i32 r = 0;
            for (i32 i = 0; i < cap; ++i) {
                f32 c = perFeatureContribution[picked[i]];
                if (std::fabs(c) < 0.01f) break;
                outReasons[r].text         = Core::StringRef(FeatureNameAt(picked[i]));
                outReasons[r].contribution = c;
                r++;
            }
            outReasonCount = r;
        }
        return s;
    }

    void rebindPriors(const Corpus::CorpusPriors* p) { m_priors = p; }
    bool isModelLoaded() const { return m_loaded; }
    i32  queriesEvaluated() const { return m_queriesEvaluated; }
    u64  trainingChecksum() const { return m_trainingChecksum; }

private:
    f32  m_w[8];
    f32  m_bias;
    bool m_loaded;
    u64  m_trainingChecksum;
    mutable i32 m_queriesEvaluated;
    const Corpus::CorpusPriors* m_priors;
};

IRanker* MakeMLStubRanker(const Corpus::CorpusPriors* priors) {
    MLStubRanker* r = new MLStubRanker(priors);
    // Try the canonical path so a designer that drops a trained
    // model into the editor's cwd doesn't have to wire it up.
    r->LoadWeightsOrPretendItsZero("mlstub_weights.bin");
    return r;
}

} // namespace Suggest
} // namespace Vespucci

