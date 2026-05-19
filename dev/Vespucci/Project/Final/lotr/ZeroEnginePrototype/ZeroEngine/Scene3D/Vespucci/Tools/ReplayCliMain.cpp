// ReplayCliMain.cpp
// =============================================================================
// PLAY A REPLAY FILE FROM CLI, REPORT TIMING + CHECKSUM
// =============================================================================
// Written by: Eriumsss

#include "../QA/ReplayRecorder.h"

#include <cstdio>
#include <vector>

namespace Vespucci {
namespace QA {
    bool LoadReplayFromFile(const char* path, std::vector<ReplayEvent>& out);
    i32  PlayReplayCalculatingChecksum(const std::vector<ReplayEvent>& events, u64& outChecksum);
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: replay_cli.exe <replay.bin>\n");
        return 1;
    }
    const char* path = argv[1];
    std::vector<Vespucci::QA::ReplayEvent> events;
    if (!Vespucci::QA::LoadReplayFromFile(path, events)) {
        std::fprintf(stderr, "fatal: cannot load '%s'\n", path);
        return 2;
    }
    Vespucci::u64 checksum = 0;
    Vespucci::i32 n = Vespucci::QA::PlayReplayCalculatingChecksum(events, checksum);
    std::printf("replayed %d events, checksum 0x%016llX\n",
        n, (unsigned long long)checksum);
    return 0;
}
