// ReplayPlayer.cpp
// =============================================================================
// REPLAY A RECORDED SESSION FILE - FEED EVENTS BACK INTO THE EDITOR
// FOR REGRESSION TESTING. ALSO USED BY THE OFFLINE BENCHMARK CLI.
// =============================================================================
// Written by: Eriumsss

#include "ReplayRecorder.h"

#include "../Core/FileIO.h"
#include "../Core/Logging.h"

#include <vector>

namespace Vespucci {
namespace QA {

bool LoadReplayFromFile(const char* path, std::vector<ReplayEvent>& out) {
    out.clear();
    Core::Mapped m;
    if (!Core::MapReadOnly(path, m)) return false;
    bool ok = false;
    if (m.size >= (i64)(sizeof(u32) * 2)) {
        const u32* hdr = (const u32*)m.data;
        if (hdr[0] == 0x52455052u && hdr[1] == 1) {
            const ReplayEvent* events = (const ReplayEvent*)(m.data + sizeof(u32) * 2);
            i64 nEvents = (m.size - (i64)sizeof(u32) * 2) / (i64)sizeof(ReplayEvent);
            out.reserve((size_t)nEvents);
            for (i64 i = 0; i < nEvents; ++i) out.push_back(events[i]);
            ok = true;
        } else {
            Core::Logging::Warn("ReplayPlayer: '%s' header mismatch", path);
        }
    }
    Core::Unmap(m);
    return ok;
}

i32 PlayReplayCalculatingChecksum(const std::vector<ReplayEvent>& events,
                                    u64& outChecksum)
{
    outChecksum = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        outChecksum ^= ((u64)events[i].kind << 56) |
                        ((u64)events[i].snapshotVersion << 24) |
                        ((u64)events[i].guidA.raw);
    }
    return (i32)events.size();
}

} // namespace QA
} // namespace Vespucci
