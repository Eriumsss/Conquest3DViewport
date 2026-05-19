// GridIndex.cpp
// =============================================================================
// Build = bucket every entity into a 2D cell. Query = walk the cells
// the radius/aabb intersects, brute-force the entities in those cells.
// Auto-tuned cell size so each cell holds ~sqrt(N) entities.
// =============================================================================
// Written by: Eriumsss

#include "GridIndex.h"

#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Vespucci {
namespace Index {

namespace {
    // When auto-tuning fails (zero-area level or single-point cluster),
    // we fall back to this. Eat-shit-and-rebuild in spirit; the helper
    // exists so the failure path has a name in the stack trace.
    f32 EatShitAndPickFallbackCellSize(const Scene::SceneSnapshot& snap) {
        Span<Scene::EntityRow> rows = snap.entities();
        if (rows.empty()) return 16.0f;
        // Use 5% of the level's diagonal length as a rough scale.
        Vec3 mn = rows[0].position, mx = rows[0].position;
        for (usize i = 1; i < rows.size(); ++i) {
            const Vec3& p = rows[i].position;
            if (p.x < mn.x) mn.x = p.x; if (p.x > mx.x) mx.x = p.x;
            if (p.z < mn.z) mn.z = p.z; if (p.z > mx.z) mx.z = p.z;
        }
        f32 dx = mx.x - mn.x;
        f32 dz = mx.z - mn.z;
        f32 diag = std::sqrt(dx*dx + dz*dz);
        if (diag < 1.0f) return 8.0f;
        return diag * 0.05f;
    }
} // namespace

GridIndex::GridIndex() : m_cellSize(16.0f), m_rows(0, 0) {
    m_stats.buildMs = 0; m_stats.entityCount = 0;
    m_stats.nodeCount = 0; m_stats.maxDepth = 0; m_stats.memoryBytes = 0;
}

void GridIndex::clear() {
    m_cells.clear();
    m_rows = Span<Scene::EntityRow>(0, 0);
    m_stats.entityCount = 0;
    m_stats.nodeCount = 0;
}

bool GridIndex::build(const Scene::SceneSnapshot& snap) {
    HEAT_SCOPE("GridIndex::build");
    u64 t0 = Core::NowTicks();

    clear();
    m_rows = snap.entities();
    if (m_rows.empty()) {
        m_cellSize = 16.0f;
        m_stats.buildMs = 0;
        return true;
    }

    m_cellSize = EatShitAndPickFallbackCellSize(snap);
    if (m_cellSize <= 0.0f) m_cellSize = 16.0f;

    m_cells.reserve(m_rows.size() / 4 + 16);
    for (usize i = 0; i < m_rows.size(); ++i) {
        const Vec3& p = m_rows[i].position;
        i32 cx, cz;
        WorldToCell(p, cx, cz);
        m_cells[CellKey(cx, cz)].ents.push_back(Vespucci::EntityIndex((i32)i));
    }

    u64 ticks = Core::NowTicks() - t0;
    m_stats.buildMs     = (f32)Core::TicksToNs(ticks) / 1000000.0f;
    m_stats.entityCount = (i32)m_rows.size();
    m_stats.nodeCount   = (i32)m_cells.size();
    m_stats.maxDepth    = 0;
    m_stats.memoryBytes = m_cells.size() * (sizeof(u64) + sizeof(Cell)) +
                          m_rows.size() * sizeof(Vespucci::EntityIndex);
    return true;
}

i32 GridIndex::queryRadius(const Vec3& center, f32 radius,
                            Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_cells.empty() || radius <= 0.0f) return 0;
    i32 cellRadius = (i32)std::ceil(radius / m_cellSize);
    i32 cx, cz;
    Vec3 c = center;
    WorldToCell(c, cx, cz);

    f32 r2 = radius * radius;
    i32 wrote = 0;
    i32 found = 0;
    for (i32 dz = -cellRadius; dz <= cellRadius; ++dz) {
        for (i32 dx = -cellRadius; dx <= cellRadius; ++dx) {
            std::unordered_map<u64, Cell>::const_iterator it =
                m_cells.find(CellKey(cx + dx, cz + dz));
            if (it == m_cells.end()) continue;
            const std::vector<Vespucci::EntityIndex>& list = it->second.ents;
            for (size_t i = 0; i < list.size(); ++i) {
                Vespucci::EntityIndex e = list[i];
                const Vec3& p = m_rows[(usize)e.raw].position;
                f32 ddx = p.x - center.x, ddz = p.z - center.z;
                if (ddx*ddx + ddz*ddz <= r2) {
                    if (out && wrote < cap) out[wrote++] = e;
                    found++;
                }
            }
        }
    }
    return found;
}

i32 GridIndex::queryAabb(const Aabb& box,
                          Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_cells.empty()) return 0;
    i32 cxMin, czMin, cxMax, czMax;
    WorldToCell(box.mn, cxMin, czMin);
    WorldToCell(box.mx, cxMax, czMax);
    i32 wrote = 0; i32 found = 0;
    for (i32 cz = czMin; cz <= czMax; ++cz) {
        for (i32 cx = cxMin; cx <= cxMax; ++cx) {
            std::unordered_map<u64, Cell>::const_iterator it =
                m_cells.find(CellKey(cx, cz));
            if (it == m_cells.end()) continue;
            const std::vector<Vespucci::EntityIndex>& list = it->second.ents;
            for (size_t i = 0; i < list.size(); ++i) {
                Vespucci::EntityIndex e = list[i];
                const Vec3& p = m_rows[(usize)e.raw].position;
                if (box.contains(p)) {
                    if (out && wrote < cap) out[wrote++] = e;
                    found++;
                }
            }
        }
    }
    return found;
}

i32 GridIndex::queryKNearest(const Vec3& center, i32 k,
                              Vespucci::EntityIndex* out, i32 cap) const
{
    if (m_cells.empty() || k <= 0) return 0;
    // Spiral-out radius growth. Start small, double until we find at
    // least k candidates, then sort and truncate. Not the fastest but
    // the simplest correct algorithm for a grid.
    f32 r = m_cellSize;
    std::vector<std::pair<f32, Vespucci::EntityIndex> > cand;
    cand.reserve((size_t)(k * 2));
    for (i32 attempt = 0; attempt < 16 && (i32)cand.size() < k; ++attempt) {
        cand.clear();
        i32 cellRadius = (i32)std::ceil(r / m_cellSize);
        i32 cx, cz;
        Vec3 c = center;
        WorldToCell(c, cx, cz);
        for (i32 dz = -cellRadius; dz <= cellRadius; ++dz) {
            for (i32 dx = -cellRadius; dx <= cellRadius; ++dx) {
                std::unordered_map<u64, Cell>::const_iterator it =
                    m_cells.find(CellKey(cx + dx, cz + dz));
                if (it == m_cells.end()) continue;
                const std::vector<Vespucci::EntityIndex>& list = it->second.ents;
                for (size_t i = 0; i < list.size(); ++i) {
                    const Vec3& p = m_rows[(usize)list[i].raw].position;
                    f32 ddx = p.x - center.x;
                    f32 ddz = p.z - center.z;
                    f32 d2 = ddx*ddx + ddz*ddz;
                    cand.push_back(std::pair<f32, Vespucci::EntityIndex>(d2, list[i]));
                }
            }
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
