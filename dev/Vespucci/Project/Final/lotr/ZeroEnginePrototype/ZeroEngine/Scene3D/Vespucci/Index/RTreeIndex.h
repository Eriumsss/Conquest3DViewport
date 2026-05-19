// RTreeIndex.h
// =============================================================================
// R-TREE — BULK-LOAD STR ALGORITHM. NO INSERT/DELETE — REBUILD-OR-DIE.
// =============================================================================
// Written by: Eriumsss
//
// We do NOT support incremental insert because the snapshot is
// immutable and we rebuild on every version bump. STR (Sort-Tile-
// Recursive) bulk-load is the cheapest correct way to construct a
// balanced R-tree from scratch — sort entities along axes, partition
// into vertical strips, partition strips into nodes, pack upward.
//
// One thing R-trees do BETTER than k-d trees: bounded leaves. Each
// leaf carries an AABB so radius / box queries can prune entire
// subtrees on overlap test, no per-leaf brute force. For LOTR:C
// scale (~1500 entities) this difference is small but the doc
// locked all four peers and we deliver all four.
// =============================================================================

#ifndef VESPUCCI_INDEX_RTREEINDEX_H_
#define VESPUCCI_INDEX_RTREEINDEX_H_

#include "ISpatialIndex.h"

#include <vector>

namespace Vespucci {
namespace Index {

class RTreeIndex : public ISpatialIndex {
public:
    RTreeIndex();

    Kind kind() const { return KIND_RTree; }
    const char* name() const { return "RTree"; }

    bool build(const Scene::SceneSnapshot& snap);
    void clear();

    i32 queryRadius(const Vec3& center, f32 radius,
                    Vespucci::EntityIndex* out, i32 cap) const;
    i32 queryAabb(const Aabb& box,
                  Vespucci::EntityIndex* out, i32 cap) const;
    i32 queryKNearest(const Vec3& center, i32 k,
                      Vespucci::EntityIndex* out, i32 cap) const;

    BuildStats lastBuildStats() const { return m_stats; }

private:
    static const i32 kFanOut = 8;        // entries per non-leaf node
    static const i32 kLeafCap = 16;      // entities per leaf

    struct Node {
        Aabb     bounds;
        i32      firstChild;     // index in m_nodes for internal, -1 for leaf
        i32      childCount;     // 0..kFanOut for internal
        i32      entryStart;     // -1 for internal, leaf points into m_leaves
        i32      entryCount;     // 0 for internal, count for leaves
    };

    // Voice helpers — STR build can blow up if N exceeds 24-bit
    // (a level with 16M entities, lol nope, but the assert tells
    // future-us why we believed it would never happen).
    void   FuckThisLevelAndBailEmpty();
    i32    BuildLeavesOrChokeOnTheFanOut(std::vector<Vespucci::EntityIndex>& sorted,
                                          std::vector<Aabb>& outBounds);
    void   PackTowardRootUntilOneLeftStanding(std::vector<Aabb>& bounds,
                                               std::vector<i32>& nodeIdx);
    void   RadiusRecurseOrCutOffAtBoundary(i32 nodeIdx, const Vec3& center, f32 r2,
                                            Vespucci::EntityIndex* out, i32 cap,
                                            i32& wrote, i32& found) const;
    void   AabbRecurseOrCutOffAtBoundary(i32 nodeIdx, const Aabb& box,
                                          Vespucci::EntityIndex* out, i32 cap,
                                          i32& wrote, i32& found) const;

    std::vector<Node>                  m_nodes;
    std::vector<Vespucci::EntityIndex> m_leaves;
    Span<Scene::EntityRow>             m_rows;
    i32                                m_rootIdx;
    BuildStats                         m_stats;
};

} // namespace Index
} // namespace Vespucci

#endif // VESPUCCI_INDEX_RTREEINDEX_H_
