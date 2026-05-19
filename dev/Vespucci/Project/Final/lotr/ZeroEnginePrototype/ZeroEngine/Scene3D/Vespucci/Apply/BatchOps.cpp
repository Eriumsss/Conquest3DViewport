// BatchOps.cpp
// =============================================================================
// MULTI-SUGGESTION BATCH APPLY - "ACCEPT TOP 3 ALL AT ONCE", "FIX EVERY
// BROKEN WIRE WITH ITS BEST CANDIDATE", "BULK-PURGE EVERY SELF-LOOP"
// =============================================================================
// Written by: Eriumsss
//
// Apply commits one wire request per args buffer per editor frame —
// the EXE-side connector reads the args struct's chainAddConnectionRequested
// at the end of DrawFrame and writes the wire to the level. So a batch
// of N wires drains over N frames. BatchOps owns the deferred queue,
// the per-frame drain, the rollback path on failure, and the per-batch
// undo grouping (one Undo click reverts the whole batch instead of
// peeling them off one wire at a time).
//
// Three batch flavors:
//   1. EnqueueSuggestionBatch — generic accept-N-suggestions.
//   2. EnqueueHealAllBatch    — Health-Fix's "fix everything" path.
//   3. EnqueueDeleteBatch     — pure deletion sweep (no replacement).
//
// Each is just a different way to populate the same internal queue;
// the drain logic doesn't know or care which flavor put the entry in.
// =============================================================================

#include "Apply.h"
#include "Undo.h"

#include "../Core/Logging.h"

#include <cstring>
#include <vector>

namespace Vespucci {
namespace Apply {

enum BatchOpKind {
    BATCH_OP_AddWire     = 0,    // commit a Suggestion as a new wire
    BATCH_OP_DeleteWire  = 1,    // delete an existing wire (source -> output)
    BATCH_OP_HealStep    = 2     // delete-then-add (broken -> replacement)
};

struct PendingCommit {
    BatchOpKind         kind;
    Suggest::Suggestion s;          // valid when kind == AddWire or HealStep
    Guid                sourceGuid;
    Guid                deleteSrc;  // valid when kind == DeleteWire or HealStep
    Guid                deleteOut;  // (output guid for the wire being deleted)
    u32                 batchTag;   // identifier shared across all entries of one user-action
};

namespace {
    static std::vector<PendingCommit> s_queue;
    static u32                        s_nextBatchTag = 1;

    // Stats for the debug overlay so designers can see batch progress.
    static i32 s_lastBatchSize     = 0;
    static i32 s_lastBatchSucceeded = 0;
    static i32 s_lastBatchFailed    = 0;
} // namespace

// Push one suggestion onto the queue. Caller passes the active batch
// tag; if zero, this entry is a one-off and gets its own tag.
void EnqueueBatchOrSpitOnTheList(const Suggest::Suggestion& s, Guid sourceGuid) {
    PendingCommit p;
    p.kind        = BATCH_OP_AddWire;
    p.s           = s;
    p.sourceGuid  = sourceGuid;
    p.deleteSrc   = Guid(0);
    p.deleteOut   = Guid(0);
    p.batchTag    = s_nextBatchTag++;
    s_queue.push_back(p);
}

// Push a multi-suggestion batch; returns the tag they share so the UI
// can render "applying 2 of 5..." and the undo system can group-revert.
u32 EnqueueSuggestionBatchOrSpitOnTheList(const Suggest::Suggestion* items,
                                            i32 count,
                                            Guid sourceGuid)
{
    if (count <= 0 || !items) return 0;
    u32 tag = s_nextBatchTag++;
    s_lastBatchSize = count;
    s_lastBatchSucceeded = 0;
    s_lastBatchFailed = 0;
    for (i32 i = 0; i < count; ++i) {
        PendingCommit p;
        p.kind        = BATCH_OP_AddWire;
        p.s           = items[i];
        p.sourceGuid  = sourceGuid;
        p.deleteSrc   = Guid(0);
        p.deleteOut   = Guid(0);
        p.batchTag    = tag;
        s_queue.push_back(p);
    }
    Core::Logging::Info("BatchOps: enqueued %d-suggestion batch (tag=%u, source=0x%08X)",
        count, tag, (unsigned)sourceGuid.raw);
    return tag;
}

// Push a delete sweep — every entry deletes one wire, no replacement.
u32 EnqueueDeleteBatchOrPissOff(const Guid* sources, const Guid* outputs,
                                  i32 count)
{
    if (count <= 0 || !sources || !outputs) return 0;
    u32 tag = s_nextBatchTag++;
    s_lastBatchSize = count;
    s_lastBatchSucceeded = 0;
    s_lastBatchFailed = 0;
    for (i32 i = 0; i < count; ++i) {
        PendingCommit p;
        std::memset(&p, 0, sizeof(p));
        p.kind       = BATCH_OP_DeleteWire;
        p.deleteSrc  = sources[i];
        p.deleteOut  = outputs[i];
        p.batchTag   = tag;
        s_queue.push_back(p);
    }
    Core::Logging::Info("BatchOps: enqueued %d-wire delete sweep (tag=%u)",
        count, tag);
    return tag;
}

// Push a heal sweep — each entry deletes one broken wire and adds a
// replacement based on the matched suggestion. Used by Health-Fix's
// Patrick-mode "Fix everything" button path.
u32 EnqueueHealBatchOrPissOff(const Guid* brokenSources,
                                const Guid* brokenTargets,
                                const Suggest::Suggestion* replacements,
                                i32 count)
{
    if (count <= 0 || !brokenSources || !brokenTargets || !replacements) return 0;
    u32 tag = s_nextBatchTag++;
    s_lastBatchSize = count;
    s_lastBatchSucceeded = 0;
    s_lastBatchFailed = 0;
    for (i32 i = 0; i < count; ++i) {
        PendingCommit p;
        p.kind       = BATCH_OP_HealStep;
        p.s          = replacements[i];
        p.sourceGuid = brokenSources[i];
        p.deleteSrc  = brokenSources[i];
        p.deleteOut  = brokenTargets[i];
        p.batchTag   = tag;
        s_queue.push_back(p);
    }
    Core::Logging::Info("BatchOps: enqueued %d-step heal batch (tag=%u)",
        count, tag);
    return tag;
}

// Drain ONE entry per call. Caller drives this once per editor frame
// from Vespucci::Update so wires commit at one-per-frame pace.
bool DrainOneFromTheBatchOrLeaveTheRestStanding(ImGuiGlueFrameArgs& args) {
    if (s_queue.empty()) return false;
    PendingCommit p = s_queue.front();
    s_queue.erase(s_queue.begin());
    bool ok = false;
    switch (p.kind) {
        case BATCH_OP_AddWire:
            ok = ApplySuggestionOrChokeOnGarbage(p.s, p.sourceGuid, args);
            if (ok) {
                UndoOp op;
                std::memset(&op, 0, sizeof(op));
                op.kind        = UNDO_DeleteAddedWire;
                op.sourceGuid  = p.sourceGuid;
                op.targetGuid  = p.s.targetGuid;
                op.eventName   = p.s.eventName;
                op.actionName  = p.s.actionName;
                op.delay       = p.s.suggestedDelay;
                op.sticky      = p.s.suggestedSticky;
                PushUndoOpOrChokeOnTheStackOverflow(op);
            }
            break;

        case BATCH_OP_DeleteWire:
            ok = DeleteWireOrPissOff(p.deleteSrc, p.deleteOut, args);
            if (ok) {
                UndoOp op;
                std::memset(&op, 0, sizeof(op));
                op.kind        = UNDO_RecreateDeletedWire;
                op.sourceGuid  = p.deleteSrc;
                op.targetGuid  = p.deleteOut;
                op.outputGuid  = p.deleteOut;
                PushUndoOpOrChokeOnTheStackOverflow(op);
            }
            break;

        case BATCH_OP_HealStep:
            // Heal step is a 2-action: delete the broken wire, then
            // re-push the add-suggestion onto the FRONT of the queue
            // so the next drain commits the replacement on the next
            // frame. This keeps the one-request-per-frame contract.
            ok = DeleteWireOrPissOff(p.deleteSrc, p.deleteOut, args);
            if (ok) {
                PendingCommit add;
                add.kind        = BATCH_OP_AddWire;
                add.s           = p.s;
                add.sourceGuid  = p.sourceGuid;
                add.deleteSrc   = Guid(0);
                add.deleteOut   = Guid(0);
                add.batchTag    = p.batchTag;
                s_queue.insert(s_queue.begin(), add);
            }
            break;
    }

    if (ok) s_lastBatchSucceeded++;
    else    s_lastBatchFailed++;

    if (!ok) {
        Core::Logging::Warn("BatchOps: drained step failed (kind=%d tag=%u)",
            (int)p.kind, p.batchTag);
    }
    return ok;
}

// Drain UP TO `cap` entries in one call. Used when the editor wants
// to power through a batch on a single frame at the cost of more
// frame-time pressure on the args buffer (note: the EXE side may not
// honor multiple chainAdd requests in one frame — current API is
// one-per-frame, so cap=1 is the safe default).
i32 DrainBatchUntilEmptyOrCap(ImGuiGlueFrameArgs& args, i32 cap) {
    i32 done = 0;
    while (done < cap && !s_queue.empty()) {
        if (!DrainOneFromTheBatchOrLeaveTheRestStanding(args)) break;
        done++;
    }
    return done;
}

i32  BatchPendingCount() { return (i32)s_queue.size(); }
void NukeTheBatchBecauseLevelChanged() {
    s_queue.clear();
    s_lastBatchSize = 0;
    s_lastBatchSucceeded = 0;
    s_lastBatchFailed = 0;
}

// Stats for the debug overlay.
i32  LastBatchSize()      { return s_lastBatchSize; }
i32  LastBatchSucceeded() { return s_lastBatchSucceeded; }
i32  LastBatchFailed()    { return s_lastBatchFailed; }

// Inspect the queue without draining — Brain panel uses this to render
// "5 wires queued, draining at 1/frame" telemetry.
i32 PeekPendingBatchTag(i32 frontOffset) {
    if (frontOffset < 0 || frontOffset >= (i32)s_queue.size()) return 0;
    return (i32)s_queue[(size_t)frontOffset].batchTag;
}

} // namespace Apply
} // namespace Vespucci
