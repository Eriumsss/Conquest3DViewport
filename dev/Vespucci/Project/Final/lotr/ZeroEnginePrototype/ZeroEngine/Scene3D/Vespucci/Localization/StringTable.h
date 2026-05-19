// StringTable.h
// =============================================================================
// LOCALIZATION STRING TABLE - KEY -> LANGUAGE-LOCAL TEXT
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked: en + tr lang packs, more added by appending another
// LangPack_*.cpp. Keys are short identifiers ("ui.suggested.title",
// "reason.same_layer", etc). Lookups are O(1) via hash map.
// =============================================================================

#ifndef VESPUCCI_LOCALIZATION_STRINGTABLE_H_
#define VESPUCCI_LOCALIZATION_STRINGTABLE_H_

#include "../Core/VespucciTypes.h"

namespace Vespucci {
namespace Loc {

enum LangCode {
    LANG_en = 0,
    LANG_tr = 1
};

void  SetActiveLanguage(LangCode lc);
LangCode GetActiveLanguage();

// Register one (key, text) pair for a given language. Lang packs
// call this from their static init.
void  RegisterLocalizedString(LangCode lc, const char* key, const char* text);

// Lookup the localized string for the active language, falling back
// to en if missing, falling back to the key itself if both miss.
const char* T(const char* key);

i32  RegisteredStringCount(LangCode lc);

} // namespace Loc
} // namespace Vespucci

#endif // VESPUCCI_LOCALIZATION_STRINGTABLE_H_
