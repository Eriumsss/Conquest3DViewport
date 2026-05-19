// TypeDirectedRanker.cpp
// =============================================================================
// TYPE-SIGNATURE-FIRST RANKING. CORPUS WEIGHTING SECONDARY.
// =============================================================================
// Written by: Eriumsss
//
// This strategy gives heavy weight to "the source's type signature
// CAN emit this event" and "the target's type signature CAN accept
// the implied action" — i.e. the hard-compat layer's hits dominate
// the score. Corpus prior contributes a tiebreaker. Designed for
// cold-start levels where the corpus is thin and we should default
// to what the type rules permit, not what we have happened to see.
//
// Score breakdown (max ~1.0):
//   0.50  source emits the requested event (signature DB hit)
//   0.30  target accepts at least one canonical action (sig DB hit)
//   0.10  parent/child or layer relationship hint
//   0.10  corpus tiebreaker (capped low so signatures dominate)
//
// Reason chips are emitted per contributing factor so the designer
// can see which signal carried the score, not just the final number.
// =============================================================================

#include "../IRanker.h"
#include "../../Corpus/CorpusPriors.h"
#include "../../Schema/EventActionSignatureDB.h"
#include "../../Schema/ZETypeRegistry.h"
#include "../../Scene/SceneSnapshot.h"

#include <cmath>
#include <cstring>

namespace Vespucci {
namespace Suggest {

namespace {
    // Soft cap on the corpus-tiebreak contribution — we never let
    // corpus prior overwhelm a signature-compat hit, even if the
    // corpus is wildly biased toward one (src, tgt) pair.
    static const f32 kCorpusTiebreakCap = 0.10f;

    // Bonus when the target's type is a child / parent of the source's
    // type in the registry's parent-chain. Catches "ScriptRelay -> any
    // gameplay-trait child" patterns where the corpus would be silent.
    f32 ParentChildBonusOrZero(Schema::ZETypeRegistry* reg,
                                 TypeId srcType, TypeId tgtType)
    {
        if (!reg || !srcType.valid() || !tgtType.valid()) return 0.0f;
        if (reg->isSubtypeOf(tgtType, srcType)) return 0.05f;
        if (reg->isSubtypeOf(srcType, tgtType)) return 0.05f;
        return 0.0f;
    }

    // Layer-adjacency bonus — entities in the same layer are more
    // likely to wire to each other in any signature-compatible pair.
    f32 LayerAdjacencyBonusOrZero(const Scene::EntityRow& a,
                                    const Scene::EntityRow& b)
    {
        if (a.layerGuid.valid() && a.layerGuid == b.layerGuid) return 0.05f;
        return 0.0f;
    }
} // namespace

class TypeDirectedRanker : public IRanker {
public:
    TypeDirectedRanker(const Corpus::CorpusPriors* priors,
                       const Schema::EventActionSignatureDB* sigDB)
        : m_priors(priors), m_sigDB(sigDB) {}

    Kind kind() const { return KIND_TypeDirected; }
    const char* name() const { return "TypeDirected"; }

    score_t scoreCandidate(const Scene::SceneSnapshot& snap,
                            Vespucci::EntityIndex sourceIdx,
                            const Core::StringRef& eventCanonical,
                            Vespucci::EntityIndex candidateIdx,
                            ReasonChip* outReasons,
                            i32 maxReasons,
                            i32& outReasonCount) const
    {
        outReasonCount = 0;
        const Scene::EntityRow* src = snap.entityAt(sourceIdx);
        const Scene::EntityRow* tgt = snap.entityAt(candidateIdx);
        if (!src || !tgt) return 0.0f;

        // ── Signature DB checks ──────────────────────────────────────
        bool emits      = false;
        bool acceptsAny = false;
        i32  emittedFreq = 0;
        if (m_sigDB) {
            char eventBuf[64];
            usize n = eventCanonical.size();
            if (n + 1 > sizeof(eventBuf)) n = sizeof(eventBuf) - 1;
            std::memcpy(eventBuf, eventCanonical.data(), n);
            eventBuf[n] = 0;
            emits = m_sigDB->typeEmitsEvent(src->typeId, eventBuf);
            // Look up the freq of this specific event for the source
            // type — heavily-emitted events score higher than one-off
            // emissions, because frequent emitters are usually wired
            // intentionally.
            i32 evtCount = 0;
            const Schema::SignatureEntry* events =
                m_sigDB->eventsFor(src->typeId, &evtCount);
            for (i32 i = 0; i < evtCount && events; ++i) {
                if (events[i].name.equalsIgnoreCase(eventCanonical)) {
                    emittedFreq = (i32)events[i].frequency;
                    break;
                }
            }
            const Schema::TypeSignature* tgtSig = m_sigDB->findSignature(tgt->typeId);
            acceptsAny = (tgtSig && tgtSig->acceptedCount > 0);
        }

        // ── Score components ─────────────────────────────────────────
        score_t emitScore   = emits ? 0.50f : 0.0f;
        score_t acceptScore = acceptsAny ? 0.30f : 0.0f;
        // Frequency bump — log-scaled so 100 emissions don't pin to 1.
        f32 freqBump = 0.0f;
        if (emittedFreq > 0) {
            freqBump = std::log10((f32)emittedFreq + 1.0f) * 0.05f;
            if (freqBump > 0.10f) freqBump = 0.10f;
        }

        Schema::ZETypeRegistry* reg = Schema::GlobalRegistry();
        f32 parentBonus = ParentChildBonusOrZero(reg, src->typeId, tgt->typeId);
        f32 layerBonus  = LayerAdjacencyBonusOrZero(*src, *tgt);

        // Corpus tiebreaker.
        score_t corpusScore = 0.0f;
        if (m_priors) {
            logprob_t lp = m_priors->logProb(src->typeId, eventCanonical, tgt->typeId);
            corpusScore = std::exp(lp);
            if (corpusScore > kCorpusTiebreakCap) corpusScore = kCorpusTiebreakCap;
        }

        score_t total = emitScore + acceptScore + freqBump + parentBonus +
                        layerBonus + corpusScore;
        if (total > 1.0f) total = 1.0f;
        if (total < 0.0f) total = 0.0f;

        // ── Reason chips ─────────────────────────────────────────────
        if (outReasons && maxReasons > 0 && total > 0.0f) {
            i32 r = 0;
            if (emits && r < maxReasons) {
                outReasons[r].text         = Core::StringRef("source type emits this event");
                outReasons[r].contribution = emitScore + freqBump;
                r++;
            }
            if (acceptsAny && r < maxReasons) {
                outReasons[r].text         = Core::StringRef("target type accepts actions");
                outReasons[r].contribution = acceptScore;
                r++;
            }
            if (parentBonus > 0.0f && r < maxReasons) {
                outReasons[r].text         = Core::StringRef("type hierarchy match");
                outReasons[r].contribution = parentBonus;
                r++;
            }
            if (layerBonus > 0.0f && r < maxReasons) {
                outReasons[r].text         = Core::StringRef("same layer");
                outReasons[r].contribution = layerBonus;
                r++;
            }
            if (corpusScore > 0.04f && r < maxReasons) {
                outReasons[r].text         = Core::StringRef("corpus prior tiebreak");
                outReasons[r].contribution = corpusScore;
                r++;
            }
            outReasonCount = r;
        }

        return total;
    }

    void rebindPriors(const Corpus::CorpusPriors* p) { m_priors = p; }
    void rebindSigDB(const Schema::EventActionSignatureDB* db) { m_sigDB = db; }

private:
    const Corpus::CorpusPriors*           m_priors;
    const Schema::EventActionSignatureDB* m_sigDB;
};

// Factory hook.
IRanker* MakeTypeDirectedRanker(const Corpus::CorpusPriors* priors,
                                 const Schema::EventActionSignatureDB* sigDB)
{
    return new TypeDirectedRanker(priors, sigDB);
}

} // namespace Suggest
} // namespace Vespucci

