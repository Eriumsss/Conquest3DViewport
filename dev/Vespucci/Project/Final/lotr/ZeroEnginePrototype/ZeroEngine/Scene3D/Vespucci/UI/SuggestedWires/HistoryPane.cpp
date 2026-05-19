// HistoryPane.cpp
// =============================================================================
// HISTORY PANE - PER-SESSION LIST OF SUGGESTIONS THE USER ACTED ON
// (ACCEPTED / IGNORED / UNDONE) FOR THE BRAIN OVERLAY AND GOLDEN-TEST
// FIXTURES. THE GHOST IN THE MACHINE'S DIARY.
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"

#include <ctime>
#include <vector>

namespace Vespucci {
namespace UI {
namespace SuggestedWires {

enum HistoryAction {
    HIST_Accepted = 0,
    HIST_Ignored  = 1,
    HIST_Undone   = 2
};

struct HistoryEntry {
    Guid           sourceGuid;
    Guid           targetGuid;
    score_t        score;
    HistoryAction  action;
    u64            unixSec;
};

namespace {
    static std::vector<HistoryEntry> s_history;
    static const i32 kHistoryCap = 256;

    void TrimHistoryToCap() {
        if ((i32)s_history.size() > kHistoryCap) {
            s_history.erase(s_history.begin(),
                s_history.begin() + ((i32)s_history.size() - kHistoryCap));
        }
    }
} // namespace

void LogSuggestionEvent(Guid src, Guid tgt, score_t score, HistoryAction action) {
    HistoryEntry e;
    e.sourceGuid = src; e.targetGuid = tgt; e.score = score;
    e.action = action; e.unixSec = (u64)std::time(0);
    s_history.push_back(e);
    TrimHistoryToCap();
}

void RenderHistoryPane(PanelState& s) {
    if (s_history.empty()) {
        ImGui::TextDisabled("(no suggestion history this session)");
        return;
    }
    // Render newest-first, capped at 30 visible.
    i32 shown = 0;
    for (i32 i = (i32)s_history.size() - 1; i >= 0 && shown < 30; --i, ++shown) {
        const HistoryEntry& e = s_history[(size_t)i];
        const char* actLabel =
            (e.action == HIST_Accepted) ? "accept"   :
            (e.action == HIST_Ignored)  ? "ignore"   : "undo";
        ImGui::Text("%s 0x%08X -> 0x%08X (%.2f)", actLabel,
            (unsigned)e.sourceGuid.raw, (unsigned)e.targetGuid.raw, e.score);
    }
    (void)s;
}

void ClearHistoryBetweenLevels() {
    s_history.clear();
}

} // namespace SuggestedWires
} // namespace UI
} // namespace Vespucci
