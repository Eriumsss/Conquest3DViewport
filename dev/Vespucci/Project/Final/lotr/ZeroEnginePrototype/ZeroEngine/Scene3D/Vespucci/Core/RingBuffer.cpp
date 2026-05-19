// RingBuffer.cpp
// =============================================================================
// All RingBuffer methods are templates so they live in the header.
// This .cpp exists ONLY because the build batch lists Core/*.cpp by
// hand and we want the file present on disk so the inventory matches
// the plan. It also pulls in a couple of integration test stubs that
// run when VESPUCCI_RING_INTEGRATION_TEST is defined at compile time -
// useful if you want to smoke-test the ring without booting the editor.
// =============================================================================
// Written by: Eriumsss

#include "RingBuffer.h"

#include "Logging.h"

namespace Vespucci {
namespace Core {

#if defined(VESPUCCI_RING_INTEGRATION_TEST)

// Smoke test entry point. Compile with -DVESPUCCI_RING_INTEGRATION_TEST,
// link the resulting object into a tiny test harness, call this once.
// Asserts on any unexpected behavior. Not built by default.
void RingBufferSelfTest() {
    {
        RingBuffer<i32, 4> r(RingBuffer<i32, 4>::DROP_OLDEST);
        r.push(1); r.push(2); r.push(3);
        VESPUCCI_ASSERT(r.size() == 3, "ring size after 3 pushes");
        VESPUCCI_ASSERT(r.oldest() == 1, "ring oldest mismatch");
        VESPUCCI_ASSERT(r.newest() == 3, "ring newest mismatch");
        r.push(4); r.push(5);
        VESPUCCI_ASSERT(r.size() == 4, "ring size capped");
        VESPUCCI_ASSERT(r.oldest() == 2, "ring drop-oldest evict 1");
        VESPUCCI_ASSERT(r.newest() == 5, "ring newest is 5");

        i32 buf[8]; i32 n = r.snapshot(buf, 8);
        VESPUCCI_ASSERT(n == 4, "snapshot count");
        VESPUCCI_ASSERT(buf[0] == 2 && buf[1] == 3 && buf[2] == 4 && buf[3] == 5,
            "snapshot order [%d,%d,%d,%d]", buf[0], buf[1], buf[2], buf[3]);
    }

    {
        RingBuffer<i32, 3> r(RingBuffer<i32, 3>::DROP_NEWEST);
        VESPUCCI_ASSERT(r.push(10), "drop-newest push 1");
        VESPUCCI_ASSERT(r.push(20), "drop-newest push 2");
        VESPUCCI_ASSERT(r.push(30), "drop-newest push 3");
        VESPUCCI_ASSERT(!r.push(40), "drop-newest 4 must be rejected");
        VESPUCCI_ASSERT(r.size() == 3, "drop-newest size still 3");
        VESPUCCI_ASSERT(r.oldest() == 10 && r.newest() == 30, "drop-newest preserves first three");
    }

    Logging::Info("RingBuffer self-test passed");
}

#endif // VESPUCCI_RING_INTEGRATION_TEST

} // namespace Core
} // namespace Vespucci
