// GoldenRunnerMain.cpp
// =============================================================================
// STANDALONE GOLDEN RUNNER - LOAD FIXTURE, RUN GOLDEN TESTS, EXIT 0/N
// =============================================================================
// Written by: Eriumsss

#include "../QA/ReplayRecorder.h"

#include "../Core/Logging.h"

#include <cstdio>

namespace Vespucci {
namespace QA {
    i32 RunAllRegressionsOrSlamTheDoor(const char* goldenJsonPath,
                                         const char* replayBinPath);
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: golden_runner.exe <golden.json> [replay.bin]\n");
        return 1;
    }
    const char* gold   = argv[1];
    const char* replay = argc >= 3 ? argv[2] : 0;
    int rc = (int)Vespucci::QA::RunAllRegressionsOrSlamTheDoor(gold, replay);
    if (rc == 0) std::printf("ALL GREEN\n");
    else         std::fprintf(stderr, "FAILED rc=%d\n", rc);
    return rc;
}
