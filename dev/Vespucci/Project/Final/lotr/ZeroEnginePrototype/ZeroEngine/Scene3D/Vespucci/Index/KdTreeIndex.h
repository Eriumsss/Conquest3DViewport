// KdTreeIndex.h
// =============================================================================
// K-D TREE — MEDIAN-SPLIT, ARENA-BACKED, NO REBALANCE BULLSHIT
// =============================================================================
// Written by: Eriumsss
//
// Build splits on the median of X then Z, alternating, until each
// leaf holds <= kLeafCap entities. We do NOT support insert / remove
// after build — the snapshot is immutable for its lifetime, the tree
// is rebuilt from scratch every snapshot version. That means we can
// skip the entire "incremental rebalance" body of pain that real
// k-d trees usually drag in, and the build code stays simple.
//
// Pathological data: clustered single-point levels make the splits
// degenerate (depth = N). The internal helper FuckOffPathologicalSplit
// detects this and drops into a flat-leaf brute-force fallback, same
// as queryRadius on a single cell. Clustered levels are how the dev
// maps load (200 rocks all at 0,0,0 because someone forgot to place
// them) — without this fallback we hit stack overflow in recursion.
// =============================================================================

#ifndef VESPUCCI_INDEX_KDTREEINDEX_H_
#define VESPUCCI_INDEX_KDTREEINDEX_H_

#include "ISpatialIndex.h"

#include <vector>

namespace Vespucci {
namespace Index {

class KdTreeIndex : public ISpatialIndex {
public:
    KdTreeIndex();

    Kind kind() const { return KIND_KdTree; }
    const char* name() const { return "KdTree"; }

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
    static const i32 kLeafCap = 16;
    static const i32 kMaxDepth = 32;     // hard ceiling — beyond this we choke

    struct Node {
        i32 leftChild;     // -1 if none
        i32 rightChild;    // -1 if none
        i32 entryStart;    // leaf: start in m_leaves; internal: -1
        i32 entryCount;    // leaf: count; internal: 0
        i32 axis;          // 0 = X, 2 = Z
        f32 splitValue;
    };

    // Voice-named failure helpers. The build path can deteriorate
    // hard on adversarial inputs (all same coords) and these names
    // give the stack trace some teeth when QA hits the breakage.
    i32  BuildSubtreeOrChokeAndDie(i32* idxArr, i32 n, i32 depth, i32 axis);
    void FuckOffPathologicalSplit(i32* idxArr, i32 n, Node& leaf);
    void RescueFromInfiniteRecursion(i32* idxArr, i32 n, Node& leaf);

    // Query helpers.
    void  RadiusRecurse(i32 nodeIdx, const Vec3& center, f32 r2,
                         Vespucci::EntityIndex* out, i32 cap, i32& wrote, i32& found) const;
    void  AabbRecurse(i32 nodeIdx, const Aabb& box,
                       Vespucci::EntityIndex* out, i32 cap, i32& wrote, i32& found) const;

    std::vector<Node>                  m_nodes;
    std::vector<Vespucci::EntityIndex> m_leaves;
    Span<Scene::EntityRow>             m_rows;
    BuildStats                         m_stats;
    bool                               m_pathological; // tripped FuckOffPathologicalSplit
};

} // namespace Index
} // namespace Vespucci

#endif // VESPUCCI_INDEX_KDTREEINDEX_H_
