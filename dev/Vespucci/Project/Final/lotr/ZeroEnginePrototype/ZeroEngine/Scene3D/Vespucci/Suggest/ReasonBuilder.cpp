// ReasonBuilder.cpp
// =============================================================================
// REASON CHIP MANUFACTURE — TURNS FEATURE SCORES INTO HUMAN STRINGS
// =============================================================================
// Written by: Eriumsss
//
// Take the seven FeatureScores, pick the top maxChips contributors by
// score, write one chip per contributor with text the designer can
// actually read. The localization plug-in hook lives in
// Localization/StringTable so swapping en→tr doesn't touch this file —
// we look up tokens by name and let the string table return whatever
// language is active.
//
// Doc-locked invariants:
//   1. We never emit a chip for a zero or negative score. Empty chips
//      are misleading — designer reads "spatially nearby: 0.00" and
//      thinks the system is broken.
//   2. The chip order matches the descending-by-score sort. Top chip
//      is the dominant signal. UI surfaces the first chip prominently.
//   3. Plain English, no engine jargon. "matches this event" not
//      "eventPrior P(t|src,evt) = 0.83".
//
// Three flavors:
//   - BuildReasonsOrLeaveTheFuckersBlank — primary path used by Hybrid
//     and TypeDirected. Top-N chips by score.
//   - BuildReasonsForRepair — Health-Fix's "did you mean" UX wants a
//     slightly different chip set (no event chip, no recency).
//   - BuildReasonsForAutocomplete — popup wants prefix-vs-fuzzy chips.
// =============================================================================

#include "ReasonBuilder.h"

#include "../Core/Logging.h"

#include <algorithm>
#include <cstring>

namespace Vespucci {
namespace Suggest {

namespace {
    struct Slot { f32 score; const char* text; };

    // Push into a fixed-cap min-tracking pool so we end up with the
    // top maxChips by score. Simple linear since maxChips is tiny.
    void OfferToTheReasonPoolOrSendItPacking(Slot* pool, i32 cap, i32& size,
                                                f32 score, const char* text)
    {
        if (score <= 0.0f) return;
        if (size < cap) {
            pool[size].score = score;
            pool[size].text  = text;
            size++;
            return;
        }
        // Find the weakest entry; replace if our score beats it.
        i32 weak = 0;
        for (i32 i = 1; i < size; ++i) if (pool[i].score < pool[weak].score) weak = i;
        if (score > pool[weak].score) {
            pool[weak].score = score;
            pool[weak].text  = text;
        }
    }
} // namespace

void BuildReasonsOrLeaveTheFuckersBlank(const FeatureScores& fs,
                                          ReasonChip* outChips,
                                          i32 maxChips,
                                          i32& outCount)
{
    outCount = 0;
    if (!outChips || maxChips <= 0) return;

    static const i32 kPoolCap = 8;
    Slot pool[kPoolCap];
    i32  poolSize = 0;

    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.typePrior,        "common type pattern");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.eventPrior,       "matches this event");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.nameSimilarity,   "name match");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.spatialProximity, "spatially nearby");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.sameLayer,        "same layer");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.sameParent,       "same parent");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.recency,          "recent in your session");

    // Sort the pool by score descending and take top maxChips.
    for (i32 i = 0; i < poolSize - 1; ++i) {
        for (i32 j = i + 1; j < poolSize; ++j) {
            if (pool[j].score > pool[i].score) {
                Slot t = pool[i]; pool[i] = pool[j]; pool[j] = t;
            }
        }
    }
    i32 take = poolSize < maxChips ? poolSize : maxChips;
    for (i32 i = 0; i < take; ++i) {
        outChips[i].text         = Core::StringRef(pool[i].text);
        outChips[i].contribution = pool[i].score;
    }
    outCount = take;
}

// Repair-flavored reason set — Health-Fix uses these chips for the
// "did you mean" candidate list. We omit eventPrior and recency
// because repair candidates aren't being suggested as new wires;
// they're being suggested as REPLACEMENTS for an existing dead one.
// Chip language reflects that — "type matches" instead of "common
// type pattern", "stayed in the same layer" instead of "same layer".
void BuildReasonsForRepair(const FeatureScores& fs,
                            ReasonChip* outChips,
                            i32 maxChips,
                            i32& outCount)
{
    outCount = 0;
    if (!outChips || maxChips <= 0) return;

    static const i32 kPoolCap = 6;
    Slot pool[kPoolCap];
    i32  poolSize = 0;

    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.typePrior,        "type matches the original");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.nameSimilarity,   "name is similar");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.spatialProximity, "physically near the original");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.sameLayer,        "stayed in the same layer");
    OfferToTheReasonPoolOrSendItPacking(pool, kPoolCap, poolSize,
        fs.sameParent,       "shares the same parent");

    for (i32 i = 0; i < poolSize - 1; ++i) {
        for (i32 j = i + 1; j < poolSize; ++j) {
            if (pool[j].score > pool[i].score) {
                Slot t = pool[i]; pool[i] = pool[j]; pool[j] = t;
            }
        }
    }
    i32 take = poolSize < maxChips ? poolSize : maxChips;
    for (i32 i = 0; i < take; ++i) {
        outChips[i].text         = Core::StringRef(pool[i].text);
        outChips[i].contribution = pool[i].score;
    }
    outCount = take;
}

// Autocomplete-flavored reason set — popup wants a single chip that
// describes the match path (exact / prefix / acronym / fuzzy). The
// AutocompleteRanker provides this in its Provenance enum; this
// helper turns the enum into a one-line chip text.
const char* AutocompleteProvenanceChipText(i32 provenance) {
    switch (provenance) {
        case 0: return "starts with what you typed";
        case 1: return "exact match";
        case 2: return "matches the capital letters";
        case 3: return "close match (typo-tolerant)";
    }
    return "matches your input";
}

// Aggregate score → severity descriptor used by chips on the side of
// a confidence bar. Designer wants one word, not a number.
const char* ScoreSeverityText(score_t s) {
    if (s >= 0.85f) return "very confident";
    if (s >= 0.65f) return "confident";
    if (s >= 0.45f) return "plausible";
    if (s >= 0.25f) return "speculative";
    return                  "weak";
}

// Compose a single one-line "why" string for the SuggestedWires
// Patrick-mode default view. Combines the top reason chip with a
// confidence descriptor.
i32 ComposeSinglePatrickReason(const FeatureScores& fs,
                                 score_t total,
                                 char* outBuf, i32 cap)
{
    if (!outBuf || cap <= 0) return 0;
    ReasonChip chips[3];
    i32 chipCount = 0;
    BuildReasonsOrLeaveTheFuckersBlank(fs, chips, 3, chipCount);
    const char* topChip = (chipCount > 0)
        ? chips[0].text.data()
        : "available wire pattern";
    const char* sev = ScoreSeverityText(total);
    // Compose: "<sev>: <topChip>"
    i32 written = 0;
    for (const char* p = sev; *p && written < cap - 1; ++p) outBuf[written++] = *p;
    if (written < cap - 2) outBuf[written++] = ':';
    if (written < cap - 2) outBuf[written++] = ' ';
    for (const char* p = topChip; *p && written < cap - 1; ++p) outBuf[written++] = *p;
    outBuf[written] = 0;
    return written;
}

} // namespace Suggest
} // namespace Vespucci
