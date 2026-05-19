// ISpatialIndex.h
// =============================================================================
// SPATIAL INDEX INTERFACE — FOUR PEERS, ONE QUERY SHAPE, MAY THE BEST WIN
// =============================================================================
// Written by: Eriumsss
//
// The plan locked four spatial-index peers: Grid, KdTree, RTree, BVH.
// They all answer the same questions:
//   - radius query around a world position
//   - AABB query
//   - k-nearest neighbors
// They differ in build cost, query cost, memory, and how they degrade
// with pathological data. The benchmark runs all four against the
// loaded level and picks the fastest; the factory returns whichever
// won. This interface is the contract every peer obeys.
// =============================================================================

#ifndef VESPUCCI_INDEX_ISPATIALINDEX_H_
#define VESPUCCI_INDEX_ISPATIALINDEX_H_

#include "../Core/VespucciTypes.h"
#include "../Scene/SceneSnapshot.h"

namespace Vespucci {
namespace Index {

// Per-peer build statistics for the benchmark/debug overlay.
struct BuildStats {
    f32 buildMs;
    i32 entityCount;
    i32 nodeCount;        // peer-specific (cells / tree nodes / etc.)
    i32 maxDepth;         // 0 if not applicable
    u64 memoryBytes;      // approximate, peer-reported
};

class ISpatialIndex {
public:
    virtual ~ISpatialIndex() {}

    // Tag — used by IndexFactory and the debug overlay.
    enum Kind {
        KIND_Grid    = 0,
        KIND_KdTree  = 1,
        KIND_RTree   = 2,
        KIND_BVH     = 3,
        KIND_None    = 4
    };
    virtual Kind kind() const = 0;
    virtual const char* name() const = 0;

    // Build. Resets prior state. Returns true on success.
    virtual bool build(const Scene::SceneSnapshot& snap) = 0;

    // Drop everything.
    virtual void clear() = 0;

    // Query: entities whose position is within `radius` of `center`.
    // Returns count; outIndices holds up to `cap`.
    virtual i32 queryRadius(const Vec3& center, f32 radius,
                            Vespucci::EntityIndex* outIndices, i32 cap) const = 0;

    // Query: entities whose position is within an AABB.
    virtual i32 queryAabb(const Aabb& box,
                          Vespucci::EntityIndex* outIndices, i32 cap) const = 0;

    // Query: k nearest entities to `center`. The output is in
    // ascending distance order. Returns count (may be < k if the
    // level has fewer entities). Distances NOT returned in this v1.
    virtual i32 queryKNearest(const Vec3& center, i32 k,
                              Vespucci::EntityIndex* outIndices, i32 cap) const = 0;

    // Stats from the most recent build().
    virtual BuildStats lastBuildStats() const = 0;
};

// Process singleton — orchestrator publishes whichever peer the
// IndexFactory picked for the current level. Consumers grab it
// without needing to know which peer flavor they got.
ISpatialIndex* GlobalSpatial();
void           SetGlobalSpatial(ISpatialIndex* idx);

} // namespace Index
} // namespace Vespucci

#endif // VESPUCCI_INDEX_ISPATIALINDEX_H_
