// LangPack_tr.cpp
// =============================================================================
// TÜRKÇE STRINGS - VESPUCCI'NIN TURKISH UI PACK
// =============================================================================
// Written by: Eriumsss
//
// The product ships with a real Turkish pack, not
// machine-translation-grade slop. Every string here is hand-written
// to match the same tone the English UI sets.
// =============================================================================

#include "StringTable.h"

namespace Vespucci {
namespace Loc {

void RegisterAllTurkishStrings() {
    //  Suggested Wires sidebar 
    RegisterLocalizedString(LANG_tr, "ui.sidebar.title",            "Önerilen Bağlantılar");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.subtitle",         "(Vespucci Smart Wiring)");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.no_focus",
        "Odaklanmış bir entity yok. The Forge'da bir entity'ye tıklayarak öneri yükle.");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.no_results",
        "(öneri yok - ranker sessiz ya da bu entity'nin bağlanacak hiçbir şeyi yok)");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.btn.preview",      "Önizle");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.btn.apply",        "Uygula");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.btn.ignore",       "Yoksay");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.btn.why",          "Neden?");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.btn.undo",         "Son uygulamayı geri al");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.section.pinned",   "Sabitlenmiş");
    RegisterLocalizedString(LANG_tr, "ui.sidebar.section.history",  "Geçmiş");

    //  Reason chip text 
    RegisterLocalizedString(LANG_tr, "reason.same_layer",        "aynı layer");
    RegisterLocalizedString(LANG_tr, "reason.same_parent",       "aynı parent");
    RegisterLocalizedString(LANG_tr, "reason.spatial_near",      "yakın konum");
    RegisterLocalizedString(LANG_tr, "reason.type_pattern",      "type paterni");
    RegisterLocalizedString(LANG_tr, "reason.event_match",       "bu eventle eşleşiyor");
    RegisterLocalizedString(LANG_tr, "reason.name_match",        "isim eşleşmesi");
    RegisterLocalizedString(LANG_tr, "reason.recent_session",    "oturumda yakın zamanda kullanıldı");
    RegisterLocalizedString(LANG_tr, "reason.global_corpus",     "haritalar-arası global prior");

    //  HealthFix panel 
    RegisterLocalizedString(LANG_tr, "ui.healthfix.title",          "Level Onarımı");
    RegisterLocalizedString(LANG_tr, "ui.healthfix.broken_count",   "Bozuk referans: %d");
    RegisterLocalizedString(LANG_tr, "ui.healthfix.no_broken",
        "Bozuk referans yok. Level temiz.");
    RegisterLocalizedString(LANG_tr, "ui.healthfix.btn.autoheal",   "En iyi adayları otomatik onar");
    RegisterLocalizedString(LANG_tr, "ui.healthfix.btn.rescan",     "Yeniden tara");
    RegisterLocalizedString(LANG_tr, "ui.healthfix.btn.clear",      "Seçimi temizle");
    RegisterLocalizedString(LANG_tr, "ui.healthfix.did_you_mean",   "Bunu mu demek istedin...");

    //  DebugRanker / Brain overlay 
    RegisterLocalizedString(LANG_tr, "ui.brain.title",              "Beyin Haritası");
    RegisterLocalizedString(LANG_tr, "ui.brain.subtitle",
        "Vespucci ranker telemetrisi (canlı)");
    RegisterLocalizedString(LANG_tr, "ui.brain.snapshot_version",   "Snapshot sürümü: %u");

    //  CompatEditor 
    RegisterLocalizedString(LANG_tr, "ui.compat.title",             "Uyumluluk Editörü");
    RegisterLocalizedString(LANG_tr, "ui.compat.tab.rules",         "Kurallar");
    RegisterLocalizedString(LANG_tr, "ui.compat.tab.validator",     "Doğrulayıcı");
    RegisterLocalizedString(LANG_tr, "ui.compat.tab.matrix",        "Matris");

    //  CorpusBrowser 
    RegisterLocalizedString(LANG_tr, "ui.corpus.title",             "Corpus Tarayıcı");
    RegisterLocalizedString(LANG_tr, "ui.corpus.tab.per_type",      "Type bazında");
    RegisterLocalizedString(LANG_tr, "ui.corpus.tab.per_event",     "Event bazında");

    //  SchemaDiff 
    RegisterLocalizedString(LANG_tr, "ui.schemadiff.title",         "Şema Farkı");
    RegisterLocalizedString(LANG_tr, "ui.schemadiff.compatible",
        "Sürümler uyumlu. Migrasyona gerek yok.");
    RegisterLocalizedString(LANG_tr, "ui.schemadiff.incompatible",
        "Major sürüm uyumsuzluğu. Bu corpus rebuild olmadan yüklenemez.");

    //  Plural forms (Turkish has only 'other' canonically) 
    RegisterLocalizedString(LANG_tr, "count.suggestions.one",        "%d öneri");
    RegisterLocalizedString(LANG_tr, "count.suggestions.other",      "%d öneri");
    RegisterLocalizedString(LANG_tr, "count.matches.one",            "%d eşleşme");
    RegisterLocalizedString(LANG_tr, "count.matches.other",          "%d eşleşme");
}

} // namespace Loc
} // namespace Vespucci
