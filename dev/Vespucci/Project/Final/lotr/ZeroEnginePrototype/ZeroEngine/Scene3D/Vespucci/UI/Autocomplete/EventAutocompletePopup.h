// EventAutocompletePopup.h
// =============================================================================
// THE GODDAMN AUTOCOMPLETE DROPDOWN - FIRST UI SURFACE TO LIGHT UP
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked priority #1: this is the first UI surface Vespucci ships.
// It hooks into the existing Phase 10c Event Creator popup that
// already lives in EventGraphEditor.cpp - we DO NOT replace that
// popup, we replace its plain InputText with one that opens THIS
// dropdown when the user starts typing an event or action name.
//
// The dropdown lists ranked completions (frequency-weighted, prefix-
// filtered, fuzzy-rerankd if the prefix yields nothing). Arrow keys
// navigate; Enter commits; Escape cancels; clicking with the mouse
// also commits.
// =============================================================================

#ifndef VESPUCCI_UI_AUTOCOMPLETE_EVENTAUTOCOMPLETEPOPUP_H_
#define VESPUCCI_UI_AUTOCOMPLETE_EVENTAUTOCOMPLETEPOPUP_H_

#include "../../Core/VespucciTypes.h"
#include "../../Core/StringRef.h"

namespace Vespucci {
namespace Autocomplete { class IPrefixIndex; }
namespace UI {
namespace Autocomplete {

// Popup state. The host caller owns one of these and threads it
// through DrawAutocompletePopup() each frame the popup should be
// visible. Multiple instances let the Event Creator track the
// event-name field and the action-name field independently.
struct PopupState {
    bool                              visible;
    char                              filter[64];
    i32                               selectedIdx;
    Vespucci::Autocomplete::IPrefixIndex* index;
    Core::StringRef                   committedText; // set when user picks
    bool                              committedThisFrame;
    bool                              cancelledThisFrame;
};

void InitPopupOrChokeOnNullIndex(PopupState& s, Vespucci::Autocomplete::IPrefixIndex* idx);

// Draw the popup. Returns true if the user committed a selection
// this frame; outChosen is filled with the chosen text. Caller is
// expected to copy outChosen into their target buffer.
bool DrawAutocompletePopup(PopupState& s, const char* labelId,
                            f32 maxHeight,
                            Core::StringRef& outChosen);

void ResetPopupBetweenSessions(PopupState& s);

} // namespace Autocomplete
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_AUTOCOMPLETE_EVENTAUTOCOMPLETEPOPUP_H_
