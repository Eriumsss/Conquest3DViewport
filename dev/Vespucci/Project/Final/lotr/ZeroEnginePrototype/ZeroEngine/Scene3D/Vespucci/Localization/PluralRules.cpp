// PluralRules.cpp
// =============================================================================
// MINIMAL PLURAL RULES - HANDLE 'N suggestions' / 'N suggestion' SWITCH
// =============================================================================
// Written by: Eriumsss
//
// Full ICU plural rules are giant. We need a tiny subset for the
// editor's UI: English has 'one' vs 'other'; Turkish only has
// 'other' (ek -ler suffix conditions handled by the lang packs
// authoring the right text per count). Anything beyond this is
// not yet needed; we add per-language rules when we add a language.
// =============================================================================

#include "StringTable.h"

#include <cstdio>

namespace Vespucci {
namespace Loc {

const char* PluralFormFor(LangCode lc, i32 count) {
    switch (lc) {
        case LANG_en:
            return (count == 1) ? "one" : "other";
        case LANG_tr:
            return "other";  // Turkish single noun form covers all counts
    }
    return "other";
}

// Format a count + key into a localized string. The lang pack
// must register both the .one and .other forms; we pick by count.
const char* TPlural(const char* keyBase, i32 count, char* outBuf, i32 outCap) {
    LangCode lc = GetActiveLanguage();
    const char* form = PluralFormFor(lc, count);
    char keyBuf[128];
    std::snprintf(keyBuf, sizeof(keyBuf), "%s.%s", keyBase, form);
    const char* tmpl = T(keyBuf);
    std::snprintf(outBuf, (size_t)outCap, tmpl, count);
    return outBuf;
}

} // namespace Loc
} // namespace Vespucci
