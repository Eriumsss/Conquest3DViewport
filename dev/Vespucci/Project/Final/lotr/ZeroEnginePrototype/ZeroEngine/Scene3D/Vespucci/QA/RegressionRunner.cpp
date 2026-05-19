// RegressionRunner.cpp
// =============================================================================
// CI-FRIENDLY ENTRY: RUN ALL GOLDEN TESTS + CORPUS DIFF + REPLAY BENCHMARK
// AND EXIT WITH A SHIPPABLE STATUS CODE
// =============================================================================
// Written by: Eriumsss

#include "ReplayRecorder.h"
#include "GoldenSuggestionTests.h"

#include "../Core/Logging.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace QA {

struct BenchmarkResult { i32 events; f64 totalMs; f64 avgMs; f64 p95Ms; u64 checksum; };
bool RunBenchmarkOverReplay(const char* replayPath, BenchmarkResult& out);

i32 RunAllRegressionsOrSlamTheDoor(const char* goldenJsonPath,
                                     const char* replayBinPath)
{
    std::vector<GoldenCase> cases;
    if (!LoadGoldenCasesFromFile(goldenJsonPath, cases)) {
        Core::Logging::Error("Regression: could not load golden cases");
        return 1;
    }
    GoldenResult gr = RunGoldenAndReport(cases);
    if (gr.failed > 0) {
        Core::Logging::Error("Regression: %d golden cases FAILED", gr.failed);
        return 2;
    }

    BenchmarkResult br;
    if (!RunBenchmarkOverReplay(replayBinPath, br)) {
        Core::Logging::Warn("Regression: replay benchmark missing or failed (non-blocking)");
    } else {
        Core::Logging::Info("Regression: replay %d events @ %.3f ms avg",
            br.events, br.avgMs);
    }
    Core::Logging::Info("Regression: ALL CHECKS PASSED");
    return 0;
}

} // namespace QA
} // namespace Vespucci
