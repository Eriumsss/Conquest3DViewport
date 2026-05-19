// ScoreBreakdown.cpp
// =============================================================================
// PER-CANDIDATE SCORE BREAKDOWN - SHOW WHERE THE NUMBER CAME FROM
// =============================================================================
// Written by: Eriumsss

#include "Overlay.h"

#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"
#include "../Common/ScoreBar.h"
#include "../Common/ReasonChip.h"

#include <cstdio>

namespace Vespucci {
namespace UI {
namespace DebugRanker {

void RenderScoreBreakdown(const Suggest::SuggestionList& list) {
    if (list.count == 0) {
        ImGui::TextDisabled("(no last query)");
        return;
    }
    for (i32 i = 0; i < list.count; ++i) {
        const Suggest::Suggestion& sg = list.suggestions[i];
        char hdr[200];
        std::snprintf(hdr, sizeof(hdr),
            "[%d] target 0x%08X  score %.3f  conf %.3f%s",
            i, (unsigned)sg.targetGuid.raw, sg.score, sg.confidence,
            sg.faulted ? "  FAULTED" : "");
        ImGui::Text("%s", hdr);
        Common::DrawScoreBar(sg.score, 280.0f, 4.0f);
        if (sg.reasonCount > 0) {
            Common::DrawChipRowWithWrap(sg.reasons, sg.reasonCount, 280.0f);
        }
        ImGui::Spacing();
    }
}

} // namespace DebugRanker
} // namespace UI
} // namespace Vespucci
