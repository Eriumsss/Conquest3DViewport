// KdTreeIndex.cpp
// =============================================================================
// Median-split k-d tree. Arena-allocated nodes. Failure paths get the
// motherfucking voice — when the level data goes pathological (every
// entity at the origin because some artist forgot to place their
// stuff), the recursive split degenerates and we slam into either a
// stack overflow or O(N^2) build time. The named failure helpers
// catch that BEFORE we eat a crash.
// =============================================================================
// Written by: Eriumsss

#include "KdTreeIndex.h"

#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"
#include "../Core/VespucciAssert.h"

#include <algorithm>
#include <cmath>

namespace Vespucci {
namespace Index {

namespace {
    // Partition by median on `axis` (0 = X, 2 = Z). Returns the median
    // index. Standard nth_element trick — O(N) average.
    i32 PartitionByMedian(i32* idxArr, i32 n, i32 axis,
                           Span<Scene::EntityRow> rows, f32& outSplit)
    {
        if (n <= 0) { outSplit = 0; return 0; }
        i32 mid = n / 2;
        std::nth_element(idxArr, idxArr + mid, idxArr + n,
            [&](i32 a, i32 b) {
                f32 av = (axis == 0) ? rows[(usize)a].position.x : rows[(usize)a].position.z;
                f32 bv = (axis == 0) ? rows[(usize)b].position.x : rows[(usize)b].position.z;
                return av < bv;
            });
        i32 medianIdx = idxArr[mid];
        outSplit = (axis == 0) ? rows[(usize)medianIdx].position.x
                                 : rows[(usize)medianIdx].position.z;
        return mid;
    }
} // namespace

KdTreeIndex::KdTreeIndex() : m_rows(0, 0), m_pathological(false) {
    m_stats.buildMs = 0; m_stats.entityCount = 0;
    m_stats.nodeCount = 0; m_stats.maxDepth = 0; m_stats.memoryBytes = 0;
}

void KdTreeIndex::clear() {
    m_nodes.clear();
    m_leaves.clear();
    m_rows = Span<Scene::EntityRow>(0, 0);
    m_pathological = false;
}

bool KdTreeIndex::build(const Scene::SceneSnapshot& snap) {
    HEAT_SCOPE("KdTreeIndex::build");
    u64 t0 = Core::NowTicks();

    clear();
    m_rows = snap.entities();
    if (m_rows.empty()) {
        m_stats.buildMs = 0; m_stats.entityCount = 0;
        return true;
    }

    std::vector<i32> idx;
    idx.resize(m_rows.size());
    for (i32 i = 0; i < (i32)m_rows.size(); ++i) idx[(size_t)i] = i;

    m_nodes.reserve(m_rows.size() * 2);
    BuildSubtreeOrChokeAndDie(idx.data(), (i32)idx.size(), 0, 0);

    u64 ticks = Core::NowTicks() - t0;
    m_stats.buildMs     = (f32)Core::TicksToNs(ticks) / 1000000.0f;
    m_stats.entityCount = (i32)m_rows.size();
    m_stats.nodeCount   = (i32)m_nodes.size();
    m_stats.maxDepth    = m_pathological ? 0 : (i32)std::ceil(std::log2((f64)(m_rows.size() + 1)));
    m_stats.memoryBytes = m_nodes.size() * sizeof(Node) +
                          m_leaves.size() * sizeof(Vespucci::EntityIndex);

    if (m_pathological) {
        Core::Logging::Warn("KdTreeIndex: pathological clustering detected — fell back to flat leaf");
    }
    return true;
}

i32 KdTreeIndex::BuildSubtreeOrChokeAndDie(i32* idxArr, i32 n, i32 depth, i32 axis) {
    Node nd;
    nd.leftChild = -1;
    nd.rightChild = -1;
    nd.entryStart = -1;
    nd.entryCount = 0;
    nd.axis = axis;
    nd.splitValue = 0.0f;

    if (n <= kLeafCap || depth >= kMaxDepth) {
        FuckOffPathologicalSplit(idxArr, n, nd);
        m_nodes.push_back(nd);
        return (i32)m_nodes.size() - 1;
    }

    f32 split;
    i32 mid = PartitionByMedian(idxArr, n, axis, m_rows, split);

    // Detect pathological case: median didn't actually split (all
    // entities have the same coord on this axis). Kick into the
    // fallback so we don't burn the whole stack trying.
    bool stuck = false;
    if (n > 1) {
        f32 first = (axis == 0) ? m_rows[(usize)idxArr[0]].position.x
                                  : m_rows[(usize)idxArr[0]].position.z;
        bool allSame = true;
        for (i32 i = 1; i < n; ++i) {
            f32 v = (axis == 0) ? m_rows[(usize)idxArr[i]].position.x
                                   : m_rows[(usize)idxArr[i]].position.z;
            if (v != first) { allSame = false; break; }
        }
        if (allSame) stuck = true;
    }

    if (stuck) {
        RescueFromInfiniteRecursion(idxArr, n, nd);
        m_nodes.push_back(nd);
        return (i32)m_nodes.size() - 1;
    }

    nd.splitValue = split;
    i32 selfIdx = (i32)m_nodes.size();
    m_nodes.push_back(nd);

    i32 nextAxis = (axis == 0) ? 2 : 0;
    i32 leftIdx  = BuildSubtreeOrChokeAndDie(idxArr,           mid,        depth + 1, nextAxis);
    i32 rightIdx = BuildSubtreeOrChokeAndDie(idxArr + mid + 1, n - mid - 1, depth + 1, nextAxis);
    m_nodes[(size_t)selfIdx].leftChild  = leftIdx;
    m_nodes[(size_t)selfIdx].rightChild = rightIdx;
    return selfIdx;
}

void KdTreeIndex::FuckOffPathologicalSplit(i32* idxArr, i32 n, Node& leaf) {
    leaf.entryStart = (i32)m_leaves.size();
    leaf.entryCount = n;
    for (i32 i = 0; i < n; ++i) {
        m_leaves.push_back(Vespucci::EntityIndex(idxArr[i]));
    }
}

void KdTreeIndex::RescueFromInfiniteRecursion(i32* idxArr, i32 n, Node& leaf) {
    m_pathological = true;
    Core::Logging::Debug("KdTreeIndex: rescue triggered, %d entries collapsed into one leaf", n);
    leaf.entryStart = (i32)m_leaves.size();
    leaf.entryCount = n;
    for (i32 i = 0; i < n; ++i) {
        m_leaves.push_back(Vespucci::EntityIndex(idxArr[i]));
    }
}

void KdTreeIndex::RadiusRecurse(i32 nodeIdx, const Vec3& center, f32 r2,
                                 Vespucci::EntityIndex* out, i32 cap,
                                 i32& wrote, i32& found) const
{
    if (nodeIdx < 0 || nodeIdx >= (i32)m_nodes.size()) return;
    const Node& nd = m_nodes[(size_t)nodeIdx];
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
    f32 v = (nd.axis == 0) ? center.x : center.z;
    if (v - std::sqrt(r2) <= nd.splitValue) RadiusRecurse(nd.leftChild,  center, r2, out, cap, wrote, found);
    if (v + std::sqrt(r2) >= nd.splitValue) RadiusRecurse(nd.rightChild, center, r2, out, cap, wrote, found);
}

void KdTreeIndex::AabbRecurse(i32 nodeIdx, const Aabb& box,
                               Vespucci::EntityIndex* out, i32 cap,
                               i32& wrote, i32& found) const
{
    if (nodeIdx < 0 || nodeIdx >= (i32)m_nodes.size()) return;
    const Node& nd = m_nodes[(size_t)nodeIdx];
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
    f32 lo = (nd.axis == 0) ? box.mn.x : box.mn.z;
    f32 hi = (nd.axis == 0) ? box.mx.x : box.mx.z;
    if (lo <= nd.splitValue) AabbRecurse(nd.leftChild,  box, out, cap, wrote, found);
    if (hi >= nd.splitValue) AabbRecurse(nd.rightChild, box, out, cap, wrote, found);
}

i32 KdTreeIndex::queryRadius(const Vec3& center, f32 radius,
                              Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_nodes.empty() || radius <= 0.0f) return 0;
    f32 r2 = radius * radius;
    i32 wrote = 0; i32 found = 0;
    RadiusRecurse(0, center, r2, out, cap, wrote, found);
    return found;
}

i32 KdTreeIndex::queryAabb(const Aabb& box,
                            Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_nodes.empty()) return 0;
    i32 wrote = 0; i32 found = 0;
    AabbRecurse(0, box, out, cap, wrote, found);
    return found;
}

i32 KdTreeIndex::queryKNearest(const Vec3& center, i32 k,
                                Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_nodes.empty() || k <= 0) return 0;
    // Spiral-out radius. Same shitty-but-correct strategy as Grid.
    Vec3 c = center;
    f32 r = 16.0f;
    std::vector<std::pair<f32, Vespucci::EntityIndex> > cand;
    cand.reserve((size_t)(k * 2));
    for (i32 attempt = 0; attempt < 16 && (i32)cand.size() < k; ++attempt) {
        cand.clear();
        std::vector<Vespucci::EntityIndex> tmp((size_t)(m_rows.size()));
        i32 n = queryRadius(c, r, tmp.data(), (i32)tmp.size());
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
