// BvhIndex.cpp
// =============================================================================
// SAH BVH bulk build. Bucket each centroid along the longest axis,
// pick the split with the lowest surface-area-heuristic cost. Falls
// back to median split if SAH gives nothing useful (single-point
// cluster). Failure paths get harsh names per the rules.
// =============================================================================
// Written by: Eriumsss

#include "BvhIndex.h"

#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"

#include <algorithm>
#include <cmath>

namespace Vespucci {
namespace Index {

namespace {
    f32 SurfaceArea(const Aabb& b) {
        f32 dx = b.mx.x - b.mn.x;
        f32 dy = b.mx.y - b.mn.y;
        f32 dz = b.mx.z - b.mn.z;
        return 2.0f * (dx*dy + dy*dz + dz*dx);
    }
    Aabb Centroid(const Vec3& p) { return Aabb(p, p); }
    Aabb Union(const Aabb& a, const Aabb& b) {
        Vec3 mn(a.mn.x < b.mn.x ? a.mn.x : b.mn.x,
                a.mn.y < b.mn.y ? a.mn.y : b.mn.y,
                a.mn.z < b.mn.z ? a.mn.z : b.mn.z);
        Vec3 mx(a.mx.x > b.mx.x ? a.mx.x : b.mx.x,
                a.mx.y > b.mx.y ? a.mx.y : b.mx.y,
                a.mx.z > b.mx.z ? a.mx.z : b.mx.z);
        return Aabb(mn, mx);
    }
    bool AabbOverlapsSphere(const Aabb& b, const Vec3& c, f32 r) {
        f32 dx = std::max(0.0f, std::max(b.mn.x - c.x, c.x - b.mx.x));
        f32 dz = std::max(0.0f, std::max(b.mn.z - c.z, c.z - b.mx.z));
        return (dx*dx + dz*dz) <= r*r;
    }
} // namespace

BvhIndex::BvhIndex() : m_rows(0, 0) {
    m_stats.buildMs = 0; m_stats.entityCount = 0;
    m_stats.nodeCount = 0; m_stats.maxDepth = 0; m_stats.memoryBytes = 0;
}

void BvhIndex::clear() {
    m_nodes.clear();
    m_leaves.clear();
    m_rows = Span<Scene::EntityRow>(0, 0);
}

bool BvhIndex::build(const Scene::SceneSnapshot& snap) {
    HEAT_SCOPE("BvhIndex::build");
    u64 t0 = Core::NowTicks();

    clear();
    m_rows = snap.entities();
    if (m_rows.empty()) {
        m_stats.buildMs = 0;
        return true;
    }

    std::vector<i32> idx;
    idx.resize(m_rows.size());
    for (i32 i = 0; i < (i32)m_rows.size(); ++i) idx[(size_t)i] = i;

    BuildOrChokeOnDegenerateAabb(idx.data(), (i32)idx.size(), 0);

    u64 ticks = Core::NowTicks() - t0;
    m_stats.buildMs     = (f32)Core::TicksToNs(ticks) / 1000000.0f;
    m_stats.entityCount = (i32)m_rows.size();
    m_stats.nodeCount   = (i32)m_nodes.size();
    m_stats.memoryBytes = m_nodes.size() * sizeof(Node) +
                          m_leaves.size() * sizeof(Vespucci::EntityIndex);
    return true;
}

void BvhIndex::StuffLeafBecauseSplitFailed(i32* idxArr, i32 n, Node& leaf) {
    leaf.leftChild   = -1;
    leaf.rightChild  = -1;
    leaf.entryStart  = (i32)m_leaves.size();
    leaf.entryCount  = n;
    Vec3 mn(0,0,0), mx(0,0,0);
    for (i32 i = 0; i < n; ++i) {
        Vespucci::EntityIndex e(idxArr[i]);
        m_leaves.push_back(e);
        const Vec3& p = m_rows[(usize)e.raw].position;
        if (i == 0) { mn = p; mx = p; }
        else {
            if (p.x < mn.x) mn.x = p.x; if (p.x > mx.x) mx.x = p.x;
            if (p.y < mn.y) mn.y = p.y; if (p.y > mx.y) mx.y = p.y;
            if (p.z < mn.z) mn.z = p.z; if (p.z > mx.z) mx.z = p.z;
        }
    }
    leaf.bounds = Aabb(mn, mx);
}

f32 BvhIndex::EvaluateSAHOrShrugAndPickMedian(i32* idxArr, i32 n, i32 axis, f32& outSplit) {
    if (n <= 1) return 1e30f;

    Aabb centroids = Centroid(m_rows[(usize)idxArr[0]].position);
    for (i32 i = 1; i < n; ++i) centroids = Union(centroids, Centroid(m_rows[(usize)idxArr[i]].position));

    f32 lo = (axis == 0) ? centroids.mn.x : (axis == 1 ? centroids.mn.y : centroids.mn.z);
    f32 hi = (axis == 0) ? centroids.mx.x : (axis == 1 ? centroids.mx.y : centroids.mx.z);
    if (hi - lo < 1e-4f) {
        // Degenerate: all centroids on this axis are equal. Drop to
        // median-split fallback. The shrug is real.
        outSplit = (lo + hi) * 0.5f;
        return 1e30f;
    }

    // Bucket centroids.
    struct Bucket { i32 count; Aabb bounds; };
    Bucket buckets[12];
    for (i32 i = 0; i < kSAHBuckets; ++i) {
        buckets[i].count = 0;
        buckets[i].bounds = Aabb(Vec3(0,0,0), Vec3(0,0,0));
    }
    for (i32 i = 0; i < n; ++i) {
        const Vec3& p = m_rows[(usize)idxArr[i]].position;
        f32 v = (axis == 0) ? p.x : (axis == 1 ? p.y : p.z);
        f32 t = (v - lo) / (hi - lo);
        i32 b = (i32)(t * kSAHBuckets);
        if (b < 0) b = 0;
        if (b >= kSAHBuckets) b = kSAHBuckets - 1;
        if (buckets[b].count == 0) buckets[b].bounds = Aabb(p, p);
        else buckets[b].bounds = Union(buckets[b].bounds, Aabb(p, p));
        buckets[b].count++;
    }

    // Sweep cost.
    f32 bestCost = 1e30f;
    i32 bestSplit = -1;
    for (i32 split = 1; split < kSAHBuckets; ++split) {
        i32 leftCount = 0; Aabb leftB = buckets[0].bounds;
        for (i32 i = 0; i < split; ++i) {
            leftCount += buckets[i].count;
            if (i == 0) leftB = buckets[i].bounds;
            else leftB = Union(leftB, buckets[i].bounds);
        }
        i32 rightCount = 0; Aabb rightB = buckets[kSAHBuckets - 1].bounds;
        for (i32 i = split; i < kSAHBuckets; ++i) {
            rightCount += buckets[i].count;
            if (i == split) rightB = buckets[i].bounds;
            else rightB = Union(rightB, buckets[i].bounds);
        }
        if (leftCount == 0 || rightCount == 0) continue;
        f32 cost = leftCount * SurfaceArea(leftB) + rightCount * SurfaceArea(rightB);
        if (cost < bestCost) {
            bestCost = cost;
            bestSplit = split;
        }
    }
    if (bestSplit < 0) { outSplit = (lo + hi) * 0.5f; return 1e30f; }
    outSplit = lo + (hi - lo) * (f32)bestSplit / (f32)kSAHBuckets;
    return bestCost;
}

i32 BvhIndex::BuildOrChokeOnDegenerateAabb(i32* idxArr, i32 n, i32 depth) {
    Node nd;
    nd.leftChild = -1; nd.rightChild = -1;
    nd.entryStart = -1; nd.entryCount = 0;
    nd.bounds = Aabb(Vec3(0,0,0), Vec3(0,0,0));

    if (n <= kLeafCap || depth >= 32) {
        StuffLeafBecauseSplitFailed(idxArr, n, nd);
        m_nodes.push_back(nd);
        return (i32)m_nodes.size() - 1;
    }

    f32 splitX, splitZ;
    f32 costX = EvaluateSAHOrShrugAndPickMedian(idxArr, n, 0, splitX);
    f32 costZ = EvaluateSAHOrShrugAndPickMedian(idxArr, n, 2, splitZ);

    i32 axis;
    f32 split;
    if (costX <= costZ) { axis = 0; split = splitX; }
    else                { axis = 2; split = splitZ; }

    if (costX > 1e29f && costZ > 1e29f) {
        StuffLeafBecauseSplitFailed(idxArr, n, nd);
        m_nodes.push_back(nd);
        return (i32)m_nodes.size() - 1;
    }

    // Partition.
    auto axisVal = [&](i32 e) {
        const Vec3& p = m_rows[(usize)e].position;
        return (axis == 0) ? p.x : p.z;
    };
    i32* mid = std::partition(idxArr, idxArr + n,
        [&](i32 e) { return axisVal(e) < split; });
    if (mid == idxArr || mid == idxArr + n) {
        // Bad partition. Drop into leaf.
        StuffLeafBecauseSplitFailed(idxArr, n, nd);
        m_nodes.push_back(nd);
        return (i32)m_nodes.size() - 1;
    }

    i32 selfIdx = (i32)m_nodes.size();
    m_nodes.push_back(nd);

    i32 leftCount = (i32)(mid - idxArr);
    i32 leftIdx   = BuildOrChokeOnDegenerateAabb(idxArr,           leftCount,         depth + 1);
    i32 rightIdx  = BuildOrChokeOnDegenerateAabb(idxArr + leftCount, n - leftCount,   depth + 1);

    // Compute bounds = union of children's bounds.
    Aabb bb = Union(m_nodes[(size_t)leftIdx].bounds,
                    m_nodes[(size_t)rightIdx].bounds);
    m_nodes[(size_t)selfIdx].bounds     = bb;
    m_nodes[(size_t)selfIdx].leftChild  = leftIdx;
    m_nodes[(size_t)selfIdx].rightChild = rightIdx;
    m_nodes[(size_t)selfIdx].entryStart = -1;
    m_nodes[(size_t)selfIdx].entryCount = 0;
    return selfIdx;
}

void BvhIndex::RadiusRecurseOrCullAtBoundary(i32 nodeIdx, const Vec3& center, f32 r2,
                                               Vespucci::EntityIndex* out, i32 cap,
                                               i32& wrote, i32& found) const
{
    if (nodeIdx < 0 || nodeIdx >= (i32)m_nodes.size()) return;
    const Node& nd = m_nodes[(size_t)nodeIdx];
    if (!AabbOverlapsSphere(nd.bounds, center, std::sqrt(r2))) return;
    if (nd.entryCount > 0) {
        for (i32 i = 0; i < nd.entryCount; ++i) {
            Vespucci::EntityIndex e = m_leaves[(size_t)(nd.entryStart + i)];
            const Vec3& p = m_rows[(usize)e.raw].position;
            f32 dx = p.x - center.x, dz = p.z - center.z;
            if (dx*dx + dz*dz <= r2) {
                if (out && wrote < cap) out[wrote++] = e;
                found++;
            }
        }
        return;
    }
    RadiusRecurseOrCullAtBoundary(nd.leftChild,  center, r2, out, cap, wrote, found);
    RadiusRecurseOrCullAtBoundary(nd.rightChild, center, r2, out, cap, wrote, found);
}

void BvhIndex::AabbRecurseOrCullAtBoundary(i32 nodeIdx, const Aabb& box,
                                             Vespucci::EntityIndex* out, i32 cap,
                                             i32& wrote, i32& found) const
{
    if (nodeIdx < 0 || nodeIdx >= (i32)m_nodes.size()) return;
    const Node& nd = m_nodes[(size_t)nodeIdx];
    if (!nd.bounds.overlaps(box)) return;
    if (nd.entryCount > 0) {
        for (i32 i = 0; i < nd.entryCount; ++i) {
            Vespucci::EntityIndex e = m_leaves[(size_t)(nd.entryStart + i)];
            const Vec3& p = m_rows[(usize)e.raw].position;
            if (box.contains(p)) {
                if (out && wrote < cap) out[wrote++] = e;
                found++;
            }
        }
        return;
    }
    AabbRecurseOrCullAtBoundary(nd.leftChild,  box, out, cap, wrote, found);
    AabbRecurseOrCullAtBoundary(nd.rightChild, box, out, cap, wrote, found);
}

i32 BvhIndex::queryRadius(const Vec3& center, f32 radius,
                           Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_nodes.empty() || radius <= 0.0f) return 0;
    i32 wrote = 0; i32 found = 0;
    RadiusRecurseOrCullAtBoundary(0, center, radius * radius, out, cap, wrote, found);
    return found;
}

i32 BvhIndex::queryAabb(const Aabb& box,
                         Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_nodes.empty()) return 0;
    i32 wrote = 0; i32 found = 0;
    AabbRecurseOrCullAtBoundary(0, box, out, cap, wrote, found);
    return found;
}

i32 BvhIndex::queryKNearest(const Vec3& center, i32 k,
                             Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_nodes.empty() || k <= 0) return 0;
    f32 r = 16.0f;
    std::vector<std::pair<f32, Vespucci::EntityIndex> > cand;
    cand.reserve((size_t)(k * 2));
    for (i32 attempt = 0; attempt < 16 && (i32)cand.size() < k; ++attempt) {
        cand.clear();
        std::vector<Vespucci::EntityIndex> tmp((size_t)(m_rows.size()));
        i32 n = queryRadius(center, r, tmp.data(), (i32)tmp.size());
        for (i32 i = 0; i < n; ++i) {
            const Vec3& p = m_rows[(usize)tmp[(size_t)i].raw].position;
            f32 dx = p.x - center.x, dz = p.z - center.z;
            cand.push_back(std::pair<f32, Vespucci::EntityIndex>(dx*dx + dz*dz, tmp[(size_t)i]));
        }
        if ((i32)cand.size() >= k) break;
        r *= 2.0f;
    }
    std::sort(cand.begin(), cand.end());
    i32 take = (i32)cand.size();
    if (take > k) take = k;
    if (out && cap < take) take = cap;
    for (i32 i = 0; i < take; ++i) out[i] = cand[(size_t)i].second;
    return take;
}

} // namespace Index
} // namespace Vespucci
