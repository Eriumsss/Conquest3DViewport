// GoldenSuggestionTests.h
// =============================================================================
// PUBLIC SHAPE OF THE GOLDEN-TEST SUITE - GoldenCase, GoldenResult,
// AND THE TWO ENTRY POINTS INVOKED FROM RegressionRunner.cpp / CLI
// =============================================================================
// Written by: Eriumsss
//
// The implementations live in GoldenSuggestionTests.cpp. This header
// only exposes what the runner and the offline GoldenRunnerMain.cpp
// need to construct a std::vector<GoldenCase> with a complete dtor.
// =============================================================================

#ifndef VESPUCCI_QA_GOLDENSUGGESTIONTESTS_H_
#define VESPUCCI_QA_GOLDENSUGGESTIONTESTS_H_

#include "../Core/VespucciTypes.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace QA {

struct GoldenCase {
    std::string         scenarioId;
    Guid                sourceGuid;
    std::string         event;
    std::vector<Guid>   expectedTopK;
};

struct GoldenResult {
    i32 totalCases;
    i32 passed;
    i32 failed;
};

bool         LoadGoldenCasesFromFile(const char* path, std::vector<GoldenCase>& out);
GoldenResult RunGoldenAndReport(const std::vector<GoldenCase>& cases);

} // namespace QA
} // namespace Vespucci

#endif // VESPUCCI_QA_GOLDENSUGGESTIONTESTS_H_
