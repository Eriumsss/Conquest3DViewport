// LocalCorpusBuilder.cpp
// =============================================================================
// Walk wires, bump priors. Reject ghosts - if a wire's owner or target
// has no resolved TypeId we cannot ladder-update for it, and counting
// ghosts as observations would poison the prior the same way every
// rumor poisons a small village.
// =============================================================================
// Written by: Eriumsss

#include "LocalCorpusBuilder.h"

#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"
#include "../Scene/SceneSnapshot.h"

namespace Vespucci {
namespace Corpus {

LocalCorpusBuilder::LocalCorpusBuilder() : m_priors(0) {
    m_stats.wiresVisited       = 0;
    m_stats.observationsBumped = 0;
    m_stats.ghostsRejected     = 0;
    m_stats.lastBuildMs        = 0.0f;
}

void LocalCorpusBuilder::bind(CorpusPriors* priors) { m_priors = priors; }

bool LocalCorpusBuilder::RejectGhostObservationOrFeedItIn(const Scene::SceneSnapshot& snap,
                                                           i32 wireIdx)
{
    const Scene::WireRow* w = snap.wireAt(WireIndex(wireIdx));
    if (!w || !w->ownerIdx.valid() || !w->targetIdx.valid()) {
        m_stats.ghostsRejected++;
        return false; // ghost - skip
    }
    const Scene::EntityRow* owner = snap.entityAt(w->ownerIdx);
    const Scene::EntityRow* tgt   = snap.entityAt(w->targetIdx);
    if (!owner || !tgt || !owner->typeId.valid() || !tgt->typeId.valid()) {
        m_stats.ghostsRejected++;
        return false; // unresolved type - skip
    }
    if (m_priors) m_priors->bumpObservation(owner->typeId, w->eventName, tgt->typeId);
    m_stats.observationsBumped++;
    return true;
}

i32 LocalCorpusBuilder::buildFromSnapshot(const Scene::SceneSnapshot& snap) {
    HEAT_SCOPE("LocalCorpusBuilder::buildFromSnapshot");
    if (m_priors) m_priors->clear();
    return appendFromSnapshot(snap);
}

i32 LocalCorpusBuilder::appendFromSnapshot(const Scene::SceneSnapshot& snap) {
    HEAT_SCOPE("LocalCorpusBuilder::appendFromSnapshot");
    u64 t0 = Core::NowTicks();

    i32 added = 0;
    Span<Scene::WireRow> wires = snap.wires();
    m_stats.wiresVisited       = (i32)wires.size();
    m_stats.observationsBumped = 0;
    m_stats.ghostsRejected     = 0;
    for (i32 i = 0; i < (i32)wires.size(); ++i) {
        if (RejectGhostObservationOrFeedItIn(snap, i)) added++;
    }
    u64 ticks = Core::NowTicks() - t0;
    m_stats.lastBuildMs = (f32)Core::TicksToNs(ticks) / 1000000.0f;

    if (m_stats.ghostsRejected > m_stats.observationsBumped) {
        Core::Logging::Warn("LocalCorpus: %d ghosts rejected vs %d good (more than half are dead refs - level may be corrupt)",
            m_stats.ghostsRejected, m_stats.observationsBumped);
    } else {
        Core::Logging::Info("LocalCorpus: bumped %d observations (%d ghosts skipped) in %.2f ms",
            m_stats.observationsBumped, m_stats.ghostsRejected, m_stats.lastBuildMs);
    }
    return added;
}

} // namespace Corpus
} // namespace Vespucci
