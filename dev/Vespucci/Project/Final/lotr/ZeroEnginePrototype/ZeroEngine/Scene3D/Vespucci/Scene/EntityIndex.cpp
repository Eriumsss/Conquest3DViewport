// EntityIndex.cpp
// =============================================================================
// Build the bucket maps. Single pass over the snapshot's entity rows.
// =============================================================================
// Written by: Eriumsss

#include "EntityIndex.h"

#include "../Core/Logging.h"

namespace Vespucci {
namespace Scene {

namespace {
    static EntityIndexer* s_globalIndexer = 0;
}
EntityIndexer* GlobalIndexer()                       { return s_globalIndexer; }
void           SetGlobalIndexer(EntityIndexer* idx)   { s_globalIndexer = idx; }

EntityIndexer::EntityIndexer() {}

void EntityIndexer::build(const SceneSnapshot& snap) {
    m_byType.clear();
    m_byLayer.clear();
    m_byParent.clear();
    m_distinctTypes.clear();
    m_distinctLayers.clear();
    m_outputs.clear();

    Span<EntityRow> rows = snap.entities();
    m_byType.reserve((size_t)snap.entityCount());

    for (usize i = 0; i < rows.size(); ++i) {
        Vespucci::EntityIndex eIdx((i32)i);
        const EntityRow& r = rows[i];

        std::vector<Vespucci::EntityIndex>& tBucket = m_byType[r.typeId];
        if (tBucket.empty()) m_distinctTypes.push_back(r.typeId);
        tBucket.push_back(eIdx);

        std::vector<Vespucci::EntityIndex>& lBucket = m_byLayer[r.layerGuid];
        if (lBucket.empty()) m_distinctLayers.push_back(r.layerGuid);
        lBucket.push_back(eIdx);

        if (r.parentGuid.valid()) {
            m_byParent[r.parentGuid].push_back(eIdx);
        }

        if (r.isOutputEnvelope) m_outputs.push_back(eIdx);
    }
}

i32 EntityIndexer::entitiesOfType(TypeId t, Vespucci::EntityIndex* out, i32 cap) const {
    std::unordered_map<TypeId, std::vector<Vespucci::EntityIndex> >::const_iterator it
        = m_byType.find(t);
    if (it == m_byType.end()) return 0;
    i32 n = (i32)it->second.size();
    if (out && cap > 0) {
        i32 c = (n < cap) ? n : cap;
        for (i32 i = 0; i < c; ++i) out[i] = it->second[(size_t)i];
    }
    return n;
}

i32 EntityIndexer::entitiesInLayer(LayerGuid l, Vespucci::EntityIndex* out, i32 cap) const {
    std::unordered_map<LayerGuid, std::vector<Vespucci::EntityIndex> >::const_iterator it
        = m_byLayer.find(l);
    if (it == m_byLayer.end()) return 0;
    i32 n = (i32)it->second.size();
    if (out && cap > 0) {
        i32 c = (n < cap) ? n : cap;
        for (i32 i = 0; i < c; ++i) out[i] = it->second[(size_t)i];
    }
    return n;
}

i32 EntityIndexer::entitiesUnderParent(Guid parentGuid, Vespucci::EntityIndex* out, i32 cap) const {
    std::unordered_map<Guid, std::vector<Vespucci::EntityIndex> >::const_iterator it
        = m_byParent.find(parentGuid);
    if (it == m_byParent.end()) return 0;
    i32 n = (i32)it->second.size();
    if (out && cap > 0) {
        i32 c = (n < cap) ? n : cap;
        for (i32 i = 0; i < c; ++i) out[i] = it->second[(size_t)i];
    }
    return n;
}

i32      EntityIndexer::distinctTypeCount()  const { return (i32)m_distinctTypes.size(); }
TypeId   EntityIndexer::distinctTypeAt(i32 i) const {
    if (i < 0 || i >= (i32)m_distinctTypes.size()) return TypeId(0);
    return m_distinctTypes[(size_t)i];
}
i32       EntityIndexer::distinctLayerCount() const { return (i32)m_distinctLayers.size(); }
LayerGuid EntityIndexer::distinctLayerAt(i32 i) const {
    if (i < 0 || i >= (i32)m_distinctLayers.size()) return LayerGuid(0);
    return m_distinctLayers[(size_t)i];
}

i32 EntityIndexer::outputEntityCount() const { return (i32)m_outputs.size(); }
Vespucci::EntityIndex EntityIndexer::outputEntityAt(i32 i) const {
    if (i < 0 || i >= (i32)m_outputs.size()) return Vespucci::EntityIndex();
    return m_outputs[(size_t)i];
}

} // namespace Scene
} // namespace Vespucci
