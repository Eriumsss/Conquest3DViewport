// StableSort.cpp
// =============================================================================
// All template, but this .cpp exists for the Core/*.cpp inventory and
// optionally hosts the integration self-test (-DVESPUCCI_SORT_INTEGRATION_TEST).
// Same pattern as RingBuffer.cpp.
// =============================================================================
// Written by: Eriumsss

#include "StableSort.h"

#include "VespucciAssert.h"
#include "Logging.h"

namespace Vespucci {
namespace Core {

#if defined(VESPUCCI_SORT_INTEGRATION_TEST)

namespace {
    struct ScoredItem {
        i32 id;
        f32 score;
    };
    struct ScoreLess {
        bool operator()(const ScoredItem& a, const ScoredItem& b) const {
            return a.score < b.score;
        }
    };
} // namespace

void StableSortSelfTest() {
    {
        i32 a[] = {5, 2, 8, 1, 9, 3, 7, 4, 6, 0};
        StableInsertionSort(a, 10, DefaultLess<i32>());
        for (i32 i = 0; i < 10; ++i) {
            VESPUCCI_ASSERT(a[i] == i, "insertion sort idx %d got %d", i, a[i]);
        }
    }

    {
        ScoredItem a[] = {
            {0, 0.10f}, {1, 0.40f}, {2, 0.05f}, {3, 0.80f}, {4, 0.30f},
            {5, 0.95f}, {6, 0.70f}, {7, 0.20f}, {8, 0.60f}, {9, 0.50f}
        };
        ScoredItem out[3];
        i32 n = TopK(a, 10, 3, out, ScoreLess());
        VESPUCCI_ASSERT(n == 3, "top-k count");
        // Top-3 by score = id 5 (0.95), id 3 (0.80), id 6 (0.70)
        VESPUCCI_ASSERT(out[0].id == 5, "top-k[0] = id 5 got %d", out[0].id);
        VESPUCCI_ASSERT(out[1].id == 3, "top-k[1] = id 3 got %d", out[1].id);
        VESPUCCI_ASSERT(out[2].id == 6, "top-k[2] = id 6 got %d", out[2].id);
    }

    {
        i32 a[] = {7, 2, 9, 1, 5, 3, 8, 4, 6, 0};
        i32 n = PartialSort(a, 10, 4, DefaultLess<i32>());
        VESPUCCI_ASSERT(n == 4, "partial sort count");
        // Top-4 ascending = 9, 8, 7, 6 (descending order in output)
        VESPUCCI_ASSERT(a[0] == 9 && a[1] == 8 && a[2] == 7 && a[3] == 6,
            "partial sort head [%d,%d,%d,%d]", a[0], a[1], a[2], a[3]);
    }

    Logging::Info("StableSort self-test passed");
}

#endif // VESPUCCI_SORT_INTEGRATION_TEST

} // namespace Core
} // namespace Vespucci
