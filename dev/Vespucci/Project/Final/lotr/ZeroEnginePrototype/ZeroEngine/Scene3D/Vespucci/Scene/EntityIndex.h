// EntityIndex.h
// =============================================================================
// ENTITY INDEX — TYPE/LAYER/PARENT BUCKET LOOKUPS OFF THE SNAPSHOT
// =============================================================================
// Written by: Eriumsss
//
// SceneSnapshot owns the rows; EntityIndexer owns the slicing-by-
// attribute reverse maps. The candidate generator hits these indexes
// thousands of times per ranker query, so they are pre-built when
// the snapshot rolls and read-only afterward.
//
// Class is named EntityIndexer (not EntityIndex) because the strong-
// typed wrapper from VespucciTypes.h already owns the EntityIndex
// name and the compiler will not negotiate.
// =============================================================================

#ifndef VESPUCCI_SCENE_ENTITYINDEX_H_
#define VESPUCCI_SCENE_ENTITYINDEX_H_

#include "../Core/VespucciTypes.h"
#include "SceneSnapshot.h"

#include <unordered_map>
#include <vector>

namespace Vespucci {
namespace Scene {

class EntityIndexer {
public:
    EntityIndexer();

    // Build from a snapshot. Drops prior state.
    void build(const SceneSnapshot& snap);

    // Slice-by-type. Returns count; outIndices fills up to `cap`.
    // Pass cap=0 for count-only query.
    i32 entitiesOfType(TypeId t, Vespucci::EntityIndex* outIndices, i32 cap) const;
    i32 entitiesInLayer(LayerGuid l, Vespucci::EntityIndex* outIndices, i32 cap) const;
    i32 entitiesUnderParent(Guid parentGuid, Vespucci::EntityIndex* outIndices, i32 cap) const;

    // Iteration over distinct types/layers actually present.
    i32       distinctTypeCount()  const;
    TypeId    distinctTypeAt(i32 i) const;

    i32       distinctLayerCount() const;
    LayerGuid distinctLayerAt(i32 i) const;

    // Output entities (TRAIT_HasOutputs). Useful for the wire pipeline.
    i32                   outputEntityCount() const;
    Vespucci::EntityIndex outputEntityAt(i32 i) const;

private:
    std::unordered_map<TypeId,    std::vector<Vespucci::EntityIndex> > m_byType;
    std::unordered_map<LayerGuid, std::vector<Vespucci::EntityIndex> > m_byLayer;
    std::unordered_map<Guid,      std::vector<Vespucci::EntityIndex> > m_byParent;

    std::vector<TypeId>                  m_distinctTypes;
    std::vector<LayerGuid>               m_distinctLayers;
    std::vector<Vespucci::EntityIndex>   m_outputs;
};

// Process singleton parallel to GlobalSnapshot/GlobalRegistry. Orchestrator
// allocates in Vespucci::Init, rebuilds when the snapshot version moves.
EntityIndexer* GlobalIndexer();
void           SetGlobalIndexer(EntityIndexer* idx);

} // namespace Scene
} // namespace Vespucci

#endif // VESPUCCI_SCENE_ENTITYINDEX_H_
