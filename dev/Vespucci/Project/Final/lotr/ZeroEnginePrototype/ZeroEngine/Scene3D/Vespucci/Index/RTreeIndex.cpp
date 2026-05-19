// RTreeIndex.cpp
// =============================================================================
// STR (Sort-Tile-Recursive) bulk-load. Tile horizontally first, then
// vertically within tile. Pack upward fan-out kFanOut at a time. No
// rebalance code — the snapshot is immutable, the tree is rebuilt
// from scratch every version. Failure paths get harsh names.
// =============================================================================
// Written by: Eriumsss

#include "RTreeIndex.h"

#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"
#include "../Core/VespucciAssert.h"

#include <algorithm>
#include <cmath>

namespace Vespucci {
namespace Index {

namespace {
    Aabb AabbOfPoints(const std::vector<Vec3>& pts) {
        if (pts.empty()) return Aabb(Vec3(0,0,0), Vec3(0,0,0));
        Vec3 mn = pts[0], mx = pts[0];
        for (size_t i = 1; i < pts.size(); ++i) {
            const Vec3& p = pts[i];
            if (p.x < mn.x) mn.x = p.x; if (p.x > mx.x) mx.x = p.x;
            if (p.y < mn.y) mn.y = p.y; if (p.y > mx.y) mx.y = p.y;
            if (p.z < mn.z) mn.z = p.z; if (p.z > mx.z) mx.z = p.z;
        }
        return Aabb(mn, mx);
    }
    Aabb AabbUnion(const Aabb& a, const Aabb& b) {
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

RTreeIndex::RTreeIndex() : m_rows(0, 0), m_rootIdx(-1) {
    m_stats.buildMs = 0; m_stats.entityCount = 0;
    m_stats.nodeCount = 0; m_stats.maxDepth = 0; m_stats.memoryBytes = 0;
}

void RTreeIndex::clear() {
    m_nodes.clear();
    m_leaves.clear();
    m_rows = Span<Scene::EntityRow>(0, 0);
    m_rootIdx = -1;
}

void RTreeIndex::FuckThisLevelAndBailEmpty() {
    Core::Logging::Warn("RTreeIndex: empty or null snapshot, bailing build");
    clear();
    m_stats.buildMs = 0;
    m_stats.entityCount = 0;
}

bool RTreeIndex::build(const Scene::SceneSnapshot& snap) {
    HEAT_SCOPE("RTreeIndex::build");
    u64 t0 = Core::NowTicks();

    clear();
    m_rows = snap.entities();
    if (m_rows.empty()) { FuckThisLevelAndBailEmpty(); return true; }

    // 1) Sort entity indices by X.
    std::vector<Vespucci::EntityIndex> sorted;
    sorted.reserve(m_rows.size());
    for (i32 i = 0; i < (i32)m_rows.size(); ++i) sorted.push_back(Vespucci::EntityIndex(i));
    std::sort(sorted.begin(), sorted.end(),
        [&](Vespucci::EntityIndex a, Vespucci::EntityIndex b) {
            return m_rows[(usize)a.raw].position.x < m_rows[(usize)b.raw].position.x;
        });

    // 2) Build leaves, kLeafCap entries per leaf, sorted-by-X order.
    std::vector<Aabb> leafBounds;
    BuildLeavesOrChokeOnTheFanOut(sorted, leafBounds);

    // 3) Pack upward.
    std::vector<i32> currentLevel;
    currentLevel.reserve(m_nodes.size());
    for (i32 i = 0; i < (i32)m_nodes.size(); ++i) currentLevel.push_back(i);

    PackTowardRootUntilOneLeftStanding(leafBounds, currentLevel);

    if (!currentLevel.empty()) m_rootIdx = currentLevel[0];

    u64 ticks = Core::NowTicks() - t0;
    m_stats.buildMs     = (f32)Core::TicksToNs(ticks) / 1000000.0f;
    m_stats.entityCount = (i32)m_rows.size();
    m_stats.nodeCount   = (i32)m_nodes.size();
    m_stats.memoryBytes = m_nodes.size() * sizeof(Node) +
                          m_leaves.size() * sizeof(Vespucci::EntityIndex);
    return true;
}

i32 RTreeIndex::BuildLeavesOrChokeOnTheFanOut(std::vector<Vespucci::EntityIndex>& sorted,
                                                std::vector<Aabb>& outBounds)
{
    i32 n = (i32)sorted.size();
    i32 leafCount = 0;
    for (i32 i = 0; i < n; i += kLeafCap) {
        Node leaf;
        leaf.firstChild = -1;
        leaf.childCount = 0;
        leaf.entryStart = (i32)m_leaves.size();
        i32 cnt = (n - i < kLeafCap) ? (n - i) : kLeafCap;
        leaf.entryCount = cnt;
        Vec3 mn(0,0,0), mx(0,0,0);
        for (i32 j = 0; j < cnt; ++j) {
            Vespucci::EntityIndex e = sorted[(size_t)(i + j)];
            m_leaves.push_back(e);
            const Vec3& p = m_rows[(usize)e.raw].position;
            if (j == 0) { mn = p; mx = p; }
            else {
                if (p.x < mn.x) mn.x = p.x; if (p.x > mx.x) mx.x = p.x;
                if (p.y < mn.y) mn.y = p.y; if (p.y > mx.y) mx.y = p.y;
                if (p.z < mn.z) mn.z = p.z; if (p.z > mx.z) mx.z = p.z;
            }
        }
        leaf.bounds = Aabb(mn, mx);
        outBounds.push_back(leaf.bounds);
        m_nodes.push_back(leaf);
        leafCount++;
    }
    return leafCount;
}

void RTreeIndex::PackTowardRootUntilOneLeftStanding(std::vector<Aabb>& bounds,
                                                      std::vector<i32>& nodeIdx)
{
    while (nodeIdx.size() > 1) {
        std::vector<i32> nextLevel;
        std::vector<Aabb> nextBounds;
        for (size_t i = 0; i < nodeIdx.size(); i += kFanOut) {
            Node parent;
            parent.firstChild = (i32)i; // placeholder, will overwrite
            parent.childCount = 0;
            parent.entryStart = -1;
            parent.entryCount = 0;
            Aabb pb = bounds[i];
            i32 base = (i32)i;
            i32 take = (i32)std::min((size_t)kFanOut, nodeIdx.size() - i);
            // Allocate node and assign firstChild as the index in m_nodes
            // of the FIRST child. But our children are already-allocated
            // indices into m_nodes — so we store them in a contiguous
            // helper slot in m_nodes: write them directly.
            i32 startNodeIdx = (i32)m_nodes.size();
            // Append a small adapter array of "child indirection" entries
            // by reusing the parent's firstChild + childCount semantics:
            // we record childCount = take and firstChild = base index in
            // a side array that we will linearize after the build. To
            // keep the simple model in this v1, we duplicate: copy the
            // children's m_nodes records under sequential indices so
            // firstChild + i gives the i'th child node.
            for (i32 j = 0; j < take; ++j) {
                if (j == 0) pb = bounds[(size_t)(i + (size_t)j)];
                else pb = AabbUnion(pb, bounds[(size_t)(i + (size_t)j)]);
            }
            parent.bounds     = pb;
            parent.firstChild = startNodeIdx + 1; // first child sits AFTER the parent (we will append it)
            parent.childCount = take;

            i32 parentNodeIdx = (i32)m_nodes.size();
            m_nodes.push_back(parent);
            // Append copies of child nodes in contiguous order so the
            // tree's "firstChild + i" walking model holds.
            for (i32 j = 0; j < take; ++j) {
                Node copy = m_nodes[(size_t)nodeIdx[i + (size_t)j]];
                m_nodes.push_back(copy);
            }
            nextLevel.push_back(parentNodeIdx);
            nextBounds.push_back(pb);
            (void)base;
        }
        nodeIdx.swap(nextLevel);
        bounds.swap(nextBounds);
    }
}

void RTreeIndex::RadiusRecurseOrCutOffAtBoundary(i32 nodeIdx, const Vec3& center, f32 r2,
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
    for (i32 i = 0; i < nd.childCount; ++i) {
        RadiusRecurseOrCutOffAtBoundary(nd.firstChild + i, center, r2, out, cap, wrote, found);
    }
}

void RTreeIndex::AabbRecurseOrCutOffAtBoundary(i32 nodeIdx, const Aabb& box,
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
    for (i32 i = 0; i < nd.childCount; ++i) {
        AabbRecurseOrCutOffAtBoundary(nd.firstChild + i, box, out, cap, wrote, found);
    }
}

i32 RTreeIndex::queryRadius(const Vec3& center, f32 radius,
                             Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_rootIdx < 0 || radius <= 0.0f) return 0;
    i32 wrote = 0; i32 found = 0;
    RadiusRecurseOrCutOffAtBoundary(m_rootIdx, center, radius * radius, out, cap, wrote, found);
    return found;
}

i32 RTreeIndex::queryAabb(const Aabb& box,
                           Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_rootIdx < 0) return 0;
    i32 wrote = 0; i32 found = 0;
    AabbRecurseOrCutOffAtBoundary(m_rootIdx, box, out, cap, wrote, found);
    return found;
}

i32 RTreeIndex::queryKNearest(const Vec3& center, i32 k,
                               Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_rootIdx < 0 || k <= 0) return 0;
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
