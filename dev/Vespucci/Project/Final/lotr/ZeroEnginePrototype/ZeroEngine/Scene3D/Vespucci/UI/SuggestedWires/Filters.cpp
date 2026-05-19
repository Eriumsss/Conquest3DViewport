// Filters.cpp
// =============================================================================
// FILTER LOGIC - APPLY THE SIDEBAR'S USER-FACING FILTERS TO A LIST
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Suggest/SuggestionTypes.h"

namespace Vespucci {
namespace UI {
namespace SuggestedWires {

bool MatchesEventFilter(const Suggest::Suggestion& sg, const char* filter) {
    if (!filter || !filter[0]) return true;
    if (sg.eventName.size() == 0) return false;
    // Case-insensitive substring containment.
    auto Lower = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
    usize fLen = 0; while (filter[fLen]) ++fLen;
    if (fLen == 0) return true;
    for (usize i = 0; i + fLen <= sg.eventName.size(); ++i) {
        bool ok = true;
        for (usize k = 0; k < fLen; ++k) {
            char a = Lower(sg.eventName.data()[i + k]);
            char b = Lower(filter[k]);
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

i32 FilterListInPlace(Suggest::SuggestionList& list,
                      const char* eventFilter,
                      f32 minScore)
{
    i32 keep = 0;
    for (i32 i = 0; i < list.count; ++i) {
        const Suggest::Suggestion& sg = list.suggestions[i];
        if (!MatchesEventFilter(sg, eventFilter)) continue;
        if (sg.score < minScore) continue;
        list.suggestions[keep++] = sg;
    }
    list.count = keep;
    return keep;
}

} // namespace SuggestedWires
} // namespace UI
} // namespace Vespucci
