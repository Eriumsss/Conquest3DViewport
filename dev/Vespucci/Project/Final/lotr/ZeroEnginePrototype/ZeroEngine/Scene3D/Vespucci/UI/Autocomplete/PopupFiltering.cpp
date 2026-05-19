// PopupFiltering.cpp
// =============================================================================
// FILTER STRING NORMALIZATION + DEBOUNCE - SAVES THE INDEX FROM
// THIRTY QUERIES PER KEYSTROKE WHEN THE DESIGNER IS SPAMMING THE KEYBOARD
// =============================================================================
// Written by: Eriumsss
//
// The popup gets one ImGui frame per keystroke. Without debouncing,
// a designer typing fast triggers a fresh prefix query at every
// frame, even when the filter string didn't actually change.
// Debounce: only run the index query when the canonicalized filter
// changes OR when at least kMinFrames have elapsed since last query.
// =============================================================================

#include "EventAutocompletePopup.h"

#include <cstring>

namespace Vespucci {
namespace UI {
namespace Autocomplete {

struct DebounceState {
    char     lastFilter[64];
    i32      framesSinceQuery;
};

static DebounceState s_debounce = { {0}, 0 };

static const i32 kMinFrames = 1;

void NormalizeFilterForCompare(const char* in, char* out, i32 cap) {
    if (!in || !out || cap <= 0) return;
    i32 j = 0;
    for (i32 i = 0; in[i] && j + 1 < cap; ++i) {
        char c = in[i];
        if (c == ' ' || c == '\t') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[j++] = c;
    }
    out[j] = 0;
}

bool ShouldRequeryIndex(const char* currentFilter) {
    char canon[64];
    NormalizeFilterForCompare(currentFilter, canon, sizeof(canon));
    bool changed = (std::strcmp(canon, s_debounce.lastFilter) != 0);
    s_debounce.framesSinceQuery++;
    if (changed) {
        std::strncpy(s_debounce.lastFilter, canon, sizeof(s_debounce.lastFilter) - 1);
        s_debounce.lastFilter[sizeof(s_debounce.lastFilter) - 1] = 0;
        s_debounce.framesSinceQuery = 0;
        return true;
    }
    if (s_debounce.framesSinceQuery >= kMinFrames) {
        s_debounce.framesSinceQuery = 0;
        return true;
    }
    return false;
}

void ResetFilterDebounceState() {
    s_debounce.lastFilter[0] = 0;
    s_debounce.framesSinceQuery = 0;
}

} // namespace Autocomplete
} // namespace UI
} // namespace Vespucci
