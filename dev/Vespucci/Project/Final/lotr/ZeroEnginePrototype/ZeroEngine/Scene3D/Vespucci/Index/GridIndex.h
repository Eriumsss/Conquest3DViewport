// GridIndex.h
// =============================================================================
// GRID — UNIFORM SPATIAL HASH. THE WORKHORSE. MEMORY-CHEAP, QUERY-FAST.
// =============================================================================
// Written by: Eriumsss
//
// Hash entity positions into a flat 2D grid (we ignore Y because the
// LOTR:C maps are mostly flat on a battlefield scale; for cinematic
// camera-tracking we add a Y bucket later if perf demands). Cell size
// is auto-tuned to roughly sqrt(entityCount) entities per cell so
// query cost is balanced.
// =============================================================================

#ifndef VESPUCCI_INDEX_GRIDINDEX_H_
#define VESPUCCI_INDEX_GRIDINDEX_H_

#include "ISpatialIndex.h"

#include <vector>
#include <unordered_map>

namespace Vespucci {
namespace Index {

class GridIndex : public ISpatialIndex {
public:
    GridIndex();

    Kind kind() const { return KIND_Grid; }
    const char* name() const { return "Grid"; }

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
    struct Cell { std::vector<Vespucci::EntityIndex> ents; };

    inline u64 CellKey(i32 cx, i32 cz) const {
        // Fold two i32 into one u64 hash key; bias by 2^31 to handle
        // negative coords without modulo weirdness.
        u32 ux = (u32)(cx + 1073741824);
        u32 uz = (u32)(cz + 1073741824);
        return ((u64)ux << 32) | (u64)uz;
    }
    inline void WorldToCell(const Vec3& p, i32& cx, i32& cz) const {
        cx = (i32)std::floor(p.x / m_cellSize);
        cz = (i32)std::floor(p.z / m_cellSize);
    }

    f32                                m_cellSize;
    std::unordered_map<u64, Cell>      m_cells;
    Span<Scene::EntityRow>             m_rows;
    BuildStats                         m_stats;
};

} // namespace Index
} // namespace Vespucci

#endif // VESPUCCI_INDEX_GRIDINDEX_H_
