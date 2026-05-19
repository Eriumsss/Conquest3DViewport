// BvhIndex.h
// =============================================================================
// BVH — SAH BUILD, BINARY TREE, FAST RAY/AABB QUERIES
// =============================================================================
// Written by: Eriumsss
//
// Surface-Area-Heuristic BVH built top-down. The Vespucci spatial
// queries don't strictly need ray-tracing depth, but BVH is the index
// the doc locked as the fourth peer for benchmarking. It tends to win
// for skewed-density levels (Helm's Deep has dense walls + sparse
// outer field) where Grid wastes memory and KdTree's median-split
// gives middle-of-the-road query times.
// =============================================================================

#ifndef VESPUCCI_INDEX_BVHINDEX_H_
#define VESPUCCI_INDEX_BVHINDEX_H_

#include "ISpatialIndex.h"

#include <vector>

namespace Vespucci {
namespace Index {

class BvhIndex : public ISpatialIndex {
public:
    BvhIndex();

    Kind kind() const { return KIND_BVH; }
    const char* name() const { return "BVH"; }

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
    static const i32 kLeafCap = 8;
    static const i32 kSAHBuckets = 12;

    struct Node {
        Aabb bounds;
        i32  leftChild;     // -1 if leaf
        i32  rightChild;    // -1 if leaf
        i32  entryStart;    // valid for leaf
        i32  entryCount;
    };

    // Voice-named failure / pathological-input helpers.
    i32  BuildOrChokeOnDegenerateAabb(i32* idxArr, i32 n, i32 depth);
    f32  EvaluateSAHOrShrugAndPickMedian(i32* idxArr, i32 n, i32 axis, f32& outSplit);
    void StuffLeafBecauseSplitFailed(i32* idxArr, i32 n, Node& leaf);

    void RadiusRecurseOrCullAtBoundary(i32 nodeIdx, const Vec3& center, f32 r2,
                                        Vespucci::EntityIndex* out, i32 cap,
                                        i32& wrote, i32& found) const;
    void AabbRecurseOrCullAtBoundary(i32 nodeIdx, const Aabb& box,
                                      Vespucci::EntityIndex* out, i32 cap,
                                      i32& wrote, i32& found) const;

    std::vector<Node>                  m_nodes;
    std::vector<Vespucci::EntityIndex> m_leaves;
    Span<Scene::EntityRow>             m_rows;
    BuildStats                         m_stats;
};

} // namespace Index
} // namespace Vespucci

#endif // VESPUCCI_INDEX_BVHINDEX_H_
