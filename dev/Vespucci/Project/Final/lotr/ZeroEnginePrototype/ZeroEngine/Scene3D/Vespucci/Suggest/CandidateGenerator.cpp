// CandidateGenerator.cpp
// =============================================================================
// Pull candidates from four recall sources, dedupe, cap. Each helper
// is a named pipeline stage so the stack trace tells you exactly
// which fucker was running when shit went sideways.
// =============================================================================
// Written by: Eriumsss

#include "CandidateGenerator.h"

#include "../Compat/CompatibilityMatrix.h"
#include "../Compat/DSL/Vm.h"
#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"
#include "../Index/ISpatialIndex.h"
#include "../Scene/EntityIndex.h"
#include "../Schema/EventActionSignatureDB.h"
#include "../Schema/ZETypeRegistry.h"
#include "Features.h"

#include <cstring>
#include <unordered_set>

namespace Vespucci {
namespace Suggest {

namespace {
    static CandidateGenerator* s_globalGen = 0;
}
CandidateGenerator* GlobalGenerator()                       { return s_globalGen; }
void                SetGlobalGenerator(CandidateGenerator* g) { s_globalGen = g; }

CandidateGenerator::CandidateGenerator()
    : m_snap(0), m_idx(0), m_spatial(0), m_compat(0)
{
    std::memset(&m_stats, 0, sizeof(m_stats));
}

void CandidateGenerator::bind(const Scene::SceneSnapshot* snap,
                               const Scene::EntityIndexer* idx,
                               const Index::ISpatialIndex* spatial,
                               const Compat::CompatibilityMatrix* compat)
{
    m_snap    = snap;
    m_idx     = idx;
    m_spatial = spatial;
    m_compat  = compat;
}

void CandidateGenerator::TrawlCompatibleTypeBucketsOrChokeOnEmpty(
    Vespucci::EntityIndex sourceIdx,
    const Core::StringRef& /*eventCanonical*/,
    Core::SmallVec<Vespucci::EntityIndex, 256>& work) const
{
    if (!m_snap || !m_idx) return;
    const Scene::EntityRow* src = m_snap->entityAt(sourceIdx);
    if (!src) return;
    // V1 strategy: walk all distinct types and pull one bucket each.
    // The compat matrix prunes incompatibles AFTER the bucket walk.
    // A finer-grained strategy (only types that match the event's
    // emitter signature) is a future optimization once
    // EventActionSignatureDB exposes inverse lookups.
    i32 typeCount = m_idx->distinctTypeCount();
    for (i32 i = 0; i < typeCount && (i32)work.size() < 256; ++i) {
        TypeId t = m_idx->distinctTypeAt(i);
        Vespucci::EntityIndex buf[64];
        i32 n = m_idx->entitiesOfType(t, buf, 64);
        for (i32 j = 0; j < n && (i32)work.size() < 256; ++j) {
            if (buf[j].raw == sourceIdx.raw) continue;
            work.push_back(buf[j]);
            m_stats.typeBucketHits++;
        }
    }
}

void CandidateGenerator::DragSameLayerCorpsesIn(Vespucci::EntityIndex sourceIdx,
                                                  Core::SmallVec<Vespucci::EntityIndex, 256>& work) const
{
    if (!m_snap || !m_idx) return;
    const Scene::EntityRow* src = m_snap->entityAt(sourceIdx);
    if (!src || !src->layerGuid.valid()) return;
    Vespucci::EntityIndex buf[64];
    i32 n = m_idx->entitiesInLayer(src->layerGuid, buf, 64);
    for (i32 i = 0; i < n && (i32)work.size() < 256; ++i) {
        if (buf[i].raw == sourceIdx.raw) continue;
        work.push_back(buf[i]);
        m_stats.layerBucketHits++;
    }
}

void CandidateGenerator::DragSameParentBastardsIn(Vespucci::EntityIndex sourceIdx,
                                                    Core::SmallVec<Vespucci::EntityIndex, 256>& work) const
{
    if (!m_snap || !m_idx) return;
    const Scene::EntityRow* src = m_snap->entityAt(sourceIdx);
    if (!src || !src->parentGuid.valid()) return;
    Vespucci::EntityIndex buf[64];
    i32 n = m_idx->entitiesUnderParent(src->parentGuid, buf, 64);
    for (i32 i = 0; i < n && (i32)work.size() < 256; ++i) {
        if (buf[i].raw == sourceIdx.raw) continue;
        work.push_back(buf[i]);
        m_stats.parentBucketHits++;
    }
}

void CandidateGenerator::GrabSpatiallyNearMotherfuckers(Vespucci::EntityIndex sourceIdx,
                                                          Core::SmallVec<Vespucci::EntityIndex, 256>& work) const
{
    if (!m_snap || !m_spatial) return;
    const Scene::EntityRow* src = m_snap->entityAt(sourceIdx);
    if (!src) return;
    Vespucci::EntityIndex buf[64];
    i32 n = m_spatial->queryRadius(src->position, 64.0f, buf, 64);
    for (i32 i = 0; i < n && (i32)work.size() < 256; ++i) {
        if (buf[i].raw == sourceIdx.raw) continue;
        work.push_back(buf[i]);
        m_stats.spatialHits++;
    }
}

void CandidateGenerator::DedupeAndCullThePoolDownToSize(
    Core::SmallVec<Vespucci::EntityIndex, 256>& work, i32 maxOut) const
{
    std::unordered_set<i32> seen;
    seen.reserve((size_t)work.size());
    Core::SmallVec<Vespucci::EntityIndex, 256> deduped;
    for (i32 i = 0; i < work.size() && deduped.size() < maxOut; ++i) {
        if (seen.find(work[i].raw) != seen.end()) {
            m_stats.dupesDropped++;
            continue;
        }
        seen.insert(work[i].raw);
        deduped.push_back(work[i]);
    }
    work = deduped;
    m_stats.finalCount = work.size();
}

i32 CandidateGenerator::generate(Vespucci::EntityIndex sourceIdx,
                                  const Core::StringRef& eventCanonical,
                                  Core::SmallVec<Vespucci::EntityIndex, 256>& out,
                                  i32 maxOut) const
{
    HEAT_SCOPE("CandidateGenerator::generate");
    std::memset(&m_stats, 0, sizeof(m_stats));
    out.clear();

    Core::SmallVec<Vespucci::EntityIndex, 256> work;
    TrawlCompatibleTypeBucketsOrChokeOnEmpty(sourceIdx, eventCanonical, work);
    DragSameLayerCorpsesIn(sourceIdx, work);
    DragSameParentBastardsIn(sourceIdx, work);
    GrabSpatiallyNearMotherfuckers(sourceIdx, work);
    DedupeAndCullThePoolDownToSize(work, maxOut);

    // Hard compat filter pass. Anything denied by signature DB or
    // DSL rules gets evicted before the ranker ever sees it.
    if (m_compat && m_snap) {
        const Scene::EntityRow* src = m_snap->entityAt(sourceIdx);
        for (i32 i = 0; i < work.size(); ++i) {
            const Scene::EntityRow* tgt = m_snap->entityAt(work[i]);
            if (!src || !tgt) continue;
            Compat::DSL::QueryContext q;
            std::memset(&q, 0, sizeof(q));
            q.sourceType  = src->typeId;
            q.targetType  = tgt->typeId;
            q.eventName   = eventCanonical;
            q.actionName  = Core::StringRef("activate", 8); // default; ranker may override
            q.sourceGuid  = src->guid;
            q.targetGuid  = tgt->guid;
            q.sourceLayer = src->layerGuid;
            q.targetLayer = tgt->layerGuid;
            q.sourceParent = src->parentGuid;
            q.targetParent = tgt->parentGuid;
            q.distance    = Vec3DistOrZeroIfDeadGuids(src->position, tgt->position);
            q.sourceSpatial = (src->traits & (u32)Schema::TRAIT_Spatial) != 0;
            q.targetSpatial = (tgt->traits & (u32)Schema::TRAIT_Spatial) != 0;
            q.sourceDeprecated = src->deprecated;
            q.targetDeprecated = tgt->deprecated;
            q.sourceName = src->name;
            q.targetName = tgt->name;
            Compat::CompatResult r;
            m_compat->check(q, r);
            if (r.verdict == Compat::COMPAT_Deny) {
                m_stats.hardFilterRejects++;
                // Mark as invalid by overwriting with last; pop later.
                work[i] = Vespucci::EntityIndex(-1);
            }
        }
        // Compact the work vector.
        Core::SmallVec<Vespucci::EntityIndex, 256> compacted;
        for (i32 i = 0; i < work.size(); ++i) {
            if (work[i].valid()) compacted.push_back(work[i]);
        }
        work = compacted;
    }

    for (i32 i = 0; i < work.size() && i < maxOut; ++i) {
        out.push_back(work[i]);
    }
    m_stats.finalCount = out.size();
    return out.size();
}

} // namespace Suggest
} // namespace Vespucci
