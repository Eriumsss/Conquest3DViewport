// Panel.h
// =============================================================================
// GOLDEN RUNNER UI - LOAD A GOLDEN-CASES JSON, RUN IT, SEE PASS / FAIL
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked QA gate. The golden-cases file (`QA/golden_suggestions.json`)
// pins the expected top-K ranker output for a curated list of scenarios.
// CI runs `Tools/GoldenRunnerMain.exe` against it; this panel is the
// designer-facing equivalent that lets level designers verify the
// ranker still suggests the right thing for "the canonical CapturePoint
// in Helm's Deep" or "the gate trigger in Black Gate" without leaving
// the editor.
//
// Patrick mode default: a single "Run goldens" button. Pass / fail
// summary line. Click "Show details" to expand into a per-case table.
// =============================================================================

#ifndef VESPUCCI_UI_GOLDENRUNNER_PANEL_H_
#define VESPUCCI_UI_GOLDENRUNNER_PANEL_H_

#include "../../Core/VespucciTypes.h"
#include "../../QA/GoldenSuggestionTests.h"

#include <string>
#include <vector>

struct ImGuiGlueFrameArgs;

namespace Vespucci {
namespace UI {
namespace GoldenRunner {

// One per-case result captured from the most recent run. The QA layer
// returns aggregate pass/fail counts; this panel wants per-case detail
// for the table view.
struct CaseResult {
    std::string scenarioId;
    bool        passed;
    std::string actualTopKDescription;   // human-readable summary
    std::string expectedTopKDescription;
    f32         topScore;
};

struct PanelState {
    bool                       visible;
    char                       loadedPath[260];
    std::vector<QA::GoldenCase> cases;
    std::vector<CaseResult>    results;
    QA::GoldenResult           summary;
    bool                       hasRunOnce;
    bool                       lastLoadFailed;

    // Filter / view tuning.
    bool                       hideFailing;
    char                       scenarioFilter[64];

    // UX gate.
    bool                       showAdvanced;
};

void InitPanelState(PanelState& s);
void RenderGoldenRunnerPanel(PanelState& s, ImGuiGlueFrameArgs& args);

bool LoadGoldenCasesIntoPanel(PanelState& s, const char* path);
void RunGoldensCapturingPerCase(PanelState& s);

} // namespace GoldenRunner
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_GOLDENRUNNER_PANEL_H_
