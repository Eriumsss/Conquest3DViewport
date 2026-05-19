// CandidateGenerator.h
// =============================================================================
// CANDIDATE GEN - PULL THE FEW HUNDRED VIABLE TARGETS OUT OF A LEVEL
// HOLDING TENS OF THOUSANDS OF ENTITIES, AND DO IT FAST AS HELL
// =============================================================================
// Written by: Eriumsss
//
// Hot path. The ranker ASSUMES this gives it a small candidate set
// already pre-filtered for compatibility. We do NOT score every entity
// in the level and trim later — that would be the kind of cocksucking
// brute-force the doc explicitly bans.
//
// Recall sources, in priority order:
//   1. Compat-allowed type buckets (from CompatibilityMatrix + EntityIndex)
//   2. Same-layer entities
//   3. Same-parent entities
//   4. Spatially-near entities (radius from source position)
//   5. Recency-boosted entities (last N session interactions)
//
// We dedupe across sources and cap at Limits::kMaxCandidatesPerQuery.
// =============================================================================

#ifndef VESPUCCI_SUGGEST_CANDIDATEGENERATOR_H_
#define VESPUCCI_SUGGEST_CANDIDATEGENERATOR_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"
#include "../Core/SmallVec.h"
#include "../Scene/SceneSnapshot.h"

namespace Vespucci {
namespace Scene { class EntityIndexer; }
namespace Index { class ISpatialIndex; }
namespace Compat { class CompatibilityMatrix; }
namespace Suggest {

class CandidateGenerator {
public:
    CandidateGenerator();

    void bind(const Scene::SceneSnapshot* snap,
              const Scene::EntityIndexer* idx,
              const Index::ISpatialIndex* spatial,
              const Compat::CompatibilityMatrix* compat);

    // Produce candidates for a (source, event) query. Returns count;
    // outCandidates is populated up to maxOut. Pre-deduped; each
    // candidate appears at most once.
    i32 generate(Vespucci::EntityIndex sourceIdx,
                  const Core::StringRef& eventCanonical,
                  Core::SmallVec<Vespucci::EntityIndex, 256>& outCandidates,
                  i32 maxOut) const;

    // Stats for the debug overlay.
    struct Stats {
        i32 typeBucketHits;
        i32 layerBucketHits;
        i32 parentBucketHits;
        i32 spatialHits;
        i32 dupesDropped;
        i32 hardFilterRejects;
        i32 finalCount;
    };
    Stats lastStats() const { return m_stats; }

private:
    // Voice-named pipeline stages — each pulls from one source and
    // contributes to the working set. Failure modes inside each are
    // logged and skipped, never propagated up as exceptions.
    void TrawlCompatibleTypeBucketsOrChokeOnEmpty(
        Vespucci::EntityIndex sourceIdx,
        const Core::StringRef& eventCanonical,
        Core::SmallVec<Vespucci::EntityIndex, 256>& work) const;
    void DragSameLayerCorpsesIn(Vespucci::EntityIndex sourceIdx,
                                Core::SmallVec<Vespucci::EntityIndex, 256>& work) const;
    void DragSameParentBastardsIn(Vespucci::EntityIndex sourceIdx,
                                   Core::SmallVec<Vespucci::EntityIndex, 256>& work) const;
    void GrabSpatiallyNearMotherfuckers(Vespucci::EntityIndex sourceIdx,
                                          Core::SmallVec<Vespucci::EntityIndex, 256>& work) const;
    void DedupeAndCullThePoolDownToSize(Core::SmallVec<Vespucci::EntityIndex, 256>& work,
                                          i32 maxOut) const;

    const Scene::SceneSnapshot*          m_snap;
    const Scene::EntityIndexer*          m_idx;
    const Index::ISpatialIndex*          m_spatial;
    const Compat::CompatibilityMatrix*   m_compat;
    mutable Stats                        m_stats;
};

// Process singleton — orchestrator allocates one in Vespucci::Init,
// publishes here, every consumer (sidebar / health-fix / autocomplete /
// repair) pulls it without threading the pointer through every API.
CandidateGenerator* GlobalGenerator();
void                SetGlobalGenerator(CandidateGenerator* g);

} // namespace Suggest
} // namespace Vespucci

#endif // VESPUCCI_SUGGEST_CANDIDATEGENERATOR_H_
