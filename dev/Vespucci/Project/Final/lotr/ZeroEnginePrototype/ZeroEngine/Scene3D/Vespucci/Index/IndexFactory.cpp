// IndexFactory.cpp
// =============================================================================
// Pick a spatial index by entity count + level density. The chosen
// peer is allocated, returned to the caller, who owns its lifetime.
// Default policy: Grid for < 500, KdTree for 500-5000, BVH above.
// RTree is reserved for explicit pin via the debug overlay.
// =============================================================================
// Written by: Eriumsss

#include "ISpatialIndex.h"
#include "GridIndex.h"
#include "KdTreeIndex.h"
#include "RTreeIndex.h"
#include "BvhIndex.h"

#include "../Core/Logging.h"

namespace Vespucci {
namespace Index {

namespace {
    static ISpatialIndex* s_globalSpatial = 0;
}
ISpatialIndex* GlobalSpatial()                     { return s_globalSpatial; }
void           SetGlobalSpatial(ISpatialIndex* idx) { s_globalSpatial = idx; }

ISpatialIndex* MakeIndexForLevel(i32 entityCount, ISpatialIndex::Kind preferredOrNone) {
    ISpatialIndex::Kind pick;
    if (preferredOrNone != ISpatialIndex::KIND_None) {
        pick = preferredOrNone;
    } else if (entityCount < 500) {
        pick = ISpatialIndex::KIND_Grid;
    } else if (entityCount < 5000) {
        pick = ISpatialIndex::KIND_KdTree;
    } else {
        pick = ISpatialIndex::KIND_BVH;
    }

    ISpatialIndex* result = 0;
    switch (pick) {
        case ISpatialIndex::KIND_Grid:   result = new GridIndex();   break;
        case ISpatialIndex::KIND_KdTree: result = new KdTreeIndex(); break;
        case ISpatialIndex::KIND_RTree:  result = new RTreeIndex();  break;
        case ISpatialIndex::KIND_BVH:    result = new BvhIndex();    break;
        default:                         result = new GridIndex();   break;
    }
    Core::Logging::Info("IndexFactory: picked %s for level (entities=%d, preferred=%d)",
        result->name(), entityCount, (int)preferredOrNone);
    return result;
}

void DestroyIndex(ISpatialIndex* idx) {
    delete idx;
}

} // namespace Index
} // namespace Vespucci
