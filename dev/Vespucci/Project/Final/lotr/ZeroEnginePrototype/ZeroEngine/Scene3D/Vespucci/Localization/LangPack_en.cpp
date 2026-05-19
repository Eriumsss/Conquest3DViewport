// LangPack_en.cpp
// =============================================================================
// ENGLISH STRINGS FOR EVERY VESPUCCI UI SURFACE
// =============================================================================
// Written by: Eriumsss

#include "StringTable.h"

namespace Vespucci {
namespace Loc {

void RegisterAllEnglishStrings() {
    // ── Suggested Wires sidebar ────────────────────────────────────
    RegisterLocalizedString(LANG_en, "ui.sidebar.title",            "Suggested Wires");
    RegisterLocalizedString(LANG_en, "ui.sidebar.subtitle",         "(Vespucci Smart Wiring)");
    RegisterLocalizedString(LANG_en, "ui.sidebar.no_focus",
        "No focused entity. Click any entity in The Forge to load suggestions.");
    RegisterLocalizedString(LANG_en, "ui.sidebar.no_results",
        "(no suggestions - ranker is silent or this entity has nothing wireable)");
    RegisterLocalizedString(LANG_en, "ui.sidebar.btn.preview",      "Preview");
    RegisterLocalizedString(LANG_en, "ui.sidebar.btn.apply",        "Apply");
    RegisterLocalizedString(LANG_en, "ui.sidebar.btn.ignore",       "Ignore");
    RegisterLocalizedString(LANG_en, "ui.sidebar.btn.why",          "Why?");
    RegisterLocalizedString(LANG_en, "ui.sidebar.btn.undo",         "Undo last apply");
    RegisterLocalizedString(LANG_en, "ui.sidebar.section.pinned",   "Pinned");
    RegisterLocalizedString(LANG_en, "ui.sidebar.section.history",  "History");

    // ── Reason chip text ───────────────────────────────────────────
    RegisterLocalizedString(LANG_en, "reason.same_layer",        "same layer");
    RegisterLocalizedString(LANG_en, "reason.same_parent",       "same parent");
    RegisterLocalizedString(LANG_en, "reason.spatial_near",      "spatially nearby");
    RegisterLocalizedString(LANG_en, "reason.type_pattern",      "common type pattern");
    RegisterLocalizedString(LANG_en, "reason.event_match",       "matches this event");
    RegisterLocalizedString(LANG_en, "reason.name_match",        "name match");
    RegisterLocalizedString(LANG_en, "reason.recent_session",    "recent in your session");
    RegisterLocalizedString(LANG_en, "reason.global_corpus",     "global cross-map prior");

    // ── HealthFix panel ────────────────────────────────────────────
    RegisterLocalizedString(LANG_en, "ui.healthfix.title",          "Vespucci Health-fix");
    RegisterLocalizedString(LANG_en, "ui.healthfix.broken_count",   "Broken refs: %d");
    RegisterLocalizedString(LANG_en, "ui.healthfix.no_broken",
        "No broken refs detected. The level is clean.");
    RegisterLocalizedString(LANG_en, "ui.healthfix.btn.autoheal",   "Auto-heal top candidates");
    RegisterLocalizedString(LANG_en, "ui.healthfix.btn.rescan",     "Force rescan");
    RegisterLocalizedString(LANG_en, "ui.healthfix.btn.clear",      "Clear selection");
    RegisterLocalizedString(LANG_en, "ui.healthfix.did_you_mean",   "Did you mean...");

    // ── DebugRanker / Brain overlay ────────────────────────────────
    RegisterLocalizedString(LANG_en, "ui.brain.title",              "Vespucci Brain");
    RegisterLocalizedString(LANG_en, "ui.brain.subtitle",
        "Vespucci ranker telemetry (live)");
    RegisterLocalizedString(LANG_en, "ui.brain.snapshot_version",   "Snapshot version: %u");

    // ── CompatEditor ───────────────────────────────────────────────
    RegisterLocalizedString(LANG_en, "ui.compat.title",             "Vespucci Compat Editor");
    RegisterLocalizedString(LANG_en, "ui.compat.tab.rules",         "Rules");
    RegisterLocalizedString(LANG_en, "ui.compat.tab.validator",     "Validator");
    RegisterLocalizedString(LANG_en, "ui.compat.tab.matrix",        "Matrix");

    // ── CorpusBrowser ──────────────────────────────────────────────
    RegisterLocalizedString(LANG_en, "ui.corpus.title",             "Vespucci Corpus Browser");
    RegisterLocalizedString(LANG_en, "ui.corpus.tab.per_type",      "Per-type");
    RegisterLocalizedString(LANG_en, "ui.corpus.tab.per_event",     "Per-event");

    // ── SchemaDiff ─────────────────────────────────────────────────
    RegisterLocalizedString(LANG_en, "ui.schemadiff.title",         "Vespucci Schema Diff");
    RegisterLocalizedString(LANG_en, "ui.schemadiff.compatible",
        "Versions are compatible. No migration required.");
    RegisterLocalizedString(LANG_en, "ui.schemadiff.incompatible",
        "Major-version mismatch. Cannot load this corpus without rebuild.");

    // ── Plural forms (English: one + other) ────────────────────────
    RegisterLocalizedString(LANG_en, "count.suggestions.one",        "%d suggestion");
    RegisterLocalizedString(LANG_en, "count.suggestions.other",      "%d suggestions");
    RegisterLocalizedString(LANG_en, "count.matches.one",            "%d match");
    RegisterLocalizedString(LANG_en, "count.matches.other",          "%d matches");
}

} // namespace Loc
} // namespace Vespucci
