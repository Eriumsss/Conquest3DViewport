// StringTable.cpp
// =============================================================================
// Two side-by-side hash maps (one per language). T() looks up active
// language first, falls back to en, falls back to the key.
// =============================================================================
// Written by: Eriumsss

#include "StringTable.h"

#include "../Core/Hash.h"

#include <unordered_map>

namespace Vespucci {
namespace Loc {

namespace {
    static std::unordered_map<u32, const char*> s_en;
    static std::unordered_map<u32, const char*> s_tr;
    static LangCode s_active = LANG_en;

    std::unordered_map<u32, const char*>* TableForLang(LangCode lc) {
        switch (lc) {
            case LANG_en: return &s_en;
            case LANG_tr: return &s_tr;
        }
        return &s_en;
    }
} // namespace

void SetActiveLanguage(LangCode lc) { s_active = lc; }
LangCode GetActiveLanguage()         { return s_active; }

void RegisterLocalizedString(LangCode lc, const char* key, const char* text) {
    if (!key || !text) return;
    u32 h = Core::XxHash32CStr(key, 0xACED1010u);
    (*TableForLang(lc))[h] = text;
}

const char* T(const char* key) {
    if (!key) return "";
    u32 h = Core::XxHash32CStr(key, 0xACED1010u);
    std::unordered_map<u32, const char*>* primary = TableForLang(s_active);
    std::unordered_map<u32, const char*>::const_iterator it = primary->find(h);
    if (it != primary->end()) return it->second;
    if (s_active != LANG_en) {
        it = s_en.find(h);
        if (it != s_en.end()) return it->second;
    }
    return key;
}

i32 RegisteredStringCount(LangCode lc) {
    return (i32)TableForLang(lc)->size();
}

} // namespace Loc
} // namespace Vespucci
