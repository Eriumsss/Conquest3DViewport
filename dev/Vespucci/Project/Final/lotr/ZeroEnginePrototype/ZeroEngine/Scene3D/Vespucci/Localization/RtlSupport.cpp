// RtlSupport.cpp
// =============================================================================
// RTL-LANGUAGE SUPPORT STUB - HOOKS FOR FUTURE ARABIC/HEBREW PACKS
// =============================================================================
// Written by: Eriumsss
//
// Vespucci does not currently ship a RTL language pack but the
// editor's UI layout has to know whether the active language reads
// right-to-left so panels can mirror their column order. This file
// is the policy gate - flip a returned bool if/when we add an Arabic
// or Hebrew lang pack.
// =============================================================================

#include "StringTable.h"

namespace Vespucci {
namespace Loc {

bool ActiveLanguageIsRtl() {
    LangCode lc = GetActiveLanguage();
    switch (lc) {
        case LANG_en: return false;
        case LANG_tr: return false;
    }
    return false;
}

f32 RtlMirrorAdjustForX(f32 originalX, f32 panelWidth, f32 elementWidth) {
    if (!ActiveLanguageIsRtl()) return originalX;
    return panelWidth - originalX - elementWidth;
}

} // namespace Loc
} // namespace Vespucci
