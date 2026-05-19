// EventAutocompletePopup.cpp
// =============================================================================
// EVENT AUTOCOMPLETE - DROPDOWN OPS, ARROW-KEY NAV, COMMIT-ON-ENTER
// =============================================================================
// Written by: Eriumsss
//
// The popup is single-instance-per-input-field. Caller threads its
// PopupState through every frame the field is focused; we run the
// query, render the dropdown, listen for keybinds, set committedText
// when the user picks. Failure modes (null index, empty filter,
// nothing to suggest) are silent fallbacks because a noisy
// autocomplete UI is worse than no autocomplete UI.
// =============================================================================

#include "EventAutocompletePopup.h"

#include "../../Autocomplete/IPrefixIndex.h"
#include "../../Autocomplete/FuzzyMatch.h"
#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Vespucci {
namespace UI {
namespace Autocomplete {

namespace {
    static const i32 kMaxHits = 16;

    // Acquire ranked candidates for the current filter. Falls through
    // from prefix-match to fuzzy-rerank if the prefix yields nothing.
    // Returns the number of hits written.
    i32 LockOnHitsForFilter(Vespucci::Autocomplete::IPrefixIndex* idx,
                             const char* filter,
                             Vespucci::Autocomplete::PrefixHit* outHits,
                             i32 cap)
    {
        if (!idx || !filter) return 0;
        Core::StringRef pref(filter);
        i32 n = idx->queryPrefix(pref, outHits, cap);
        if (n > 0) return n;

        // Fallback: scan all entries, fuzzy-rank. Bounded by entryCount;
        // for the LOTR:C corpus (~1k) this is a few hundred microseconds.
        // We do NOT pull a giant scan helper out of the index; we
        // reuse the empty-prefix sweep by querying with "" (returns
        // every entry).
        std::vector<Vespucci::Autocomplete::PrefixHit> all((size_t)idx->entryCount());
        i32 total = idx->queryPrefix(Core::StringRef("", 0), all.data(), (i32)all.size());

        struct Scored { Vespucci::Autocomplete::PrefixHit h; f32 s; };
        std::vector<Scored> ranked;
        ranked.reserve((size_t)total);
        for (i32 i = 0; i < total; ++i) {
            f32 s = Vespucci::Autocomplete::FuzzyScoreForReranking(pref, all[(size_t)i].name);
            if (s >= 0.25f) {
                Scored sc; sc.h = all[(size_t)i]; sc.s = s;
                ranked.push_back(sc);
            }
        }
        std::sort(ranked.begin(), ranked.end(),
            [](const Scored& a, const Scored& b) { return a.s > b.s; });
        i32 take = (i32)ranked.size();
        if (take > cap) take = cap;
        for (i32 i = 0; i < take; ++i) outHits[i] = ranked[(size_t)i].h;
        return take;
    }
} // namespace

void InitPopupOrChokeOnNullIndex(PopupState& s, Vespucci::Autocomplete::IPrefixIndex* idx) {
    s.visible = false;
    s.filter[0] = 0;
    s.selectedIdx = 0;
    s.index = idx;
    s.committedText = Core::StringRef();
    s.committedThisFrame = false;
    s.cancelledThisFrame = false;
}

bool DrawAutocompletePopup(PopupState& s, const char* labelId,
                            f32 maxHeight, Core::StringRef& outChosen)
{
    s.committedThisFrame = false;
    s.cancelledThisFrame = false;
    if (!s.visible || !s.index) return false;

    Vespucci::Autocomplete::PrefixHit hits[kMaxHits];
    i32 nHits = LockOnHitsForFilter(s.index, s.filter, hits, kMaxHits);
    if (nHits == 0) return false;

    if (s.selectedIdx >= nHits) s.selectedIdx = nHits - 1;
    if (s.selectedIdx < 0)      s.selectedIdx = 0;

    // Keybind capture - arrow up/down, Enter, Escape. Done BEFORE
    // rendering so a click on a row in the same frame still commits.
    bool committed = false;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        s.selectedIdx = (s.selectedIdx + 1) % nHits;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        s.selectedIdx = (s.selectedIdx - 1 + nHits) % nHits;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        committed = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s.visible = false;
        s.cancelledThisFrame = true;
        return false;
    }

    ImGui::PushID(labelId);
    ImGui::SetNextWindowSize(ImVec2(280.0f, maxHeight));
    if (ImGui::BeginChild("##autocomplete_popup", ImVec2(280.0f, maxHeight),
                          true, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
    {
        for (i32 i = 0; i < nHits; ++i) {
            char rowLabel[160];
            std::snprintf(rowLabel, sizeof(rowLabel),
                "%.*s   (%u uses)##acrow_%d",
                (int)hits[i].name.size(), hits[i].name.data(),
                (unsigned)hits[i].usageCount,
                (int)i);
            bool selected = (i == s.selectedIdx);
            if (ImGui::Selectable(rowLabel, selected)) {
                s.selectedIdx = i;
                committed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndChild();
    ImGui::PopID();

    if (committed && nHits > 0) {
        outChosen = hits[s.selectedIdx].name;
        s.committedText = outChosen;
        s.committedThisFrame = true;
        s.visible = false;
        return true;
    }
    return false;
}

void ResetPopupBetweenSessions(PopupState& s) {
    s.visible = false;
    s.filter[0] = 0;
    s.selectedIdx = 0;
    s.committedText = Core::StringRef();
    s.committedThisFrame = false;
    s.cancelledThisFrame = false;
}

} // namespace Autocomplete
} // namespace UI
} // namespace Vespucci
