// LocalCorpusBuilder.h
// =============================================================================
// LOCAL CORPUS BUILDER - HARVESTS WIRE PATTERNS OFF THE LOADED LEVEL
// =============================================================================
// Written by: Eriumsss
//
// Walks every WireRow in the snapshot and shoves the (sourceType,
// eventName, targetType) triple into the priors table. Run on every
// level load. Cheap (O(W) over wires, ~5k wires on Helm's Deep) so we
// can rebuild on every snapshot version without flinching.
//
// The local corpus does not OUTLIVE the level. When you load Black
// Gate the Helm's Deep corpus is dropped (or cached for fast switch
// back, depending on user setting). The persistent cross-map corpus
// lives elsewhere - see GlobalCorpusBuilder for that fight.
// =============================================================================

#ifndef VESPUCCI_CORPUS_LOCALCORPUSBUILDER_H_
#define VESPUCCI_CORPUS_LOCALCORPUSBUILDER_H_

#include "../Core/VespucciTypes.h"
#include "CorpusPriors.h"

namespace Vespucci {
namespace Scene { class SceneSnapshot; }
namespace Corpus {

class LocalCorpusBuilder {
public:
    LocalCorpusBuilder();

    // Bind to the priors that will receive observations.
    void bind(CorpusPriors* priors);

    // Walk the snapshot, bump observations. Clears the bound priors
    // first so re-running on a stale level doesn't double-count.
    // Returns the number of observations bumped.
    i32 buildFromSnapshot(const Scene::SceneSnapshot& snap);

    // Same shape but APPENDS without clearing - used by cross-session
    // recall stuff where we want to keep accumulating.
    i32 appendFromSnapshot(const Scene::SceneSnapshot& snap);

    // Stats for the debug overlay.
    struct Stats {
        i32 wiresVisited;
        i32 observationsBumped;
        i32 ghostsRejected;     // wires with broken target/owner
        f32 lastBuildMs;
    };
    Stats stats() const { return m_stats; }

private:
    // Voice-named failure helper - when a wire's owner or target type
    // does not resolve to a TypeId, we cannot bump the priors for it.
    // The skip is real but logged because if half the level is
    // ghosting, the whole goddamn corpus is corrupt.
    bool RejectGhostObservationOrFeedItIn(const Scene::SceneSnapshot& snap,
                                           i32 wireIdx);

    CorpusPriors* m_priors;
    Stats         m_stats;
};

} // namespace Corpus
} // namespace Vespucci

#endif // VESPUCCI_CORPUS_LOCALCORPUSBUILDER_H_
