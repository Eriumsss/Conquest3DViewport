// Undo.cpp
// =============================================================================
// Bounded undo stack. Pop fires the inverse host request through the
// existing chainAdd/chainDelete/host-request fields.
// =============================================================================
// Written by: Eriumsss

#include "Undo.h"
#include "Apply.h"

#include "../Core/Logging.h"
#include "imgui_glue.h"

#include <vector>

namespace Vespucci {
namespace Apply {

namespace {
    static const i32 kUndoCap = 64;
    static std::vector<UndoOp> s_stack;

    void DropOldestIfStackIsBitchassFull() {
        if ((i32)s_stack.size() >= kUndoCap) {
            s_stack.erase(s_stack.begin());
        }
    }
} // namespace

void PushUndoOpOrChokeOnTheStackOverflow(const UndoOp& op) {
    DropOldestIfStackIsBitchassFull();
    s_stack.push_back(op);
}

bool PopUndoOpAndFireItOrPissOff(ImGuiGlueFrameArgs& args) {
    if (s_stack.empty()) {
        Core::Logging::Info("Undo: nothing to undo, you spineless coward");
        return false;
    }
    UndoOp op = s_stack.back();
    s_stack.pop_back();

    switch (op.kind) {
        case UNDO_DeleteAddedWire:
            return DeleteWireOrPissOff(op.sourceGuid, op.outputGuid, args);
        case UNDO_RecreateDeletedWire: {
            // Best-effort recreate via chain-add. The server-side
            // recreates with a new outputGuid; the user will not get
            // BYTE-IDENTICAL, but functionally the wire returns.
            args.chainAddConnectionRequested = 1;
            args.chainAddSourceGuid = op.sourceGuid.raw;
            args.chainAddTargetGuid = op.targetGuid.raw;
            // event/action stored as StringRef into snapshot pool; copy.
            usize n = op.eventName.size();
            if (n + 1 > sizeof(args.chainAddOutputEventName)) n = sizeof(args.chainAddOutputEventName) - 1;
            std::memcpy(args.chainAddOutputEventName, op.eventName.data(), n);
            args.chainAddOutputEventName[n] = 0;
            n = op.actionName.size();
            if (n + 1 > sizeof(args.chainAddInputActionName)) n = sizeof(args.chainAddInputActionName) - 1;
            std::memcpy(args.chainAddInputActionName, op.actionName.data(), n);
            args.chainAddInputActionName[n] = 0;
            args.chainAddDelay  = op.delay;
            args.chainAddSticky = op.sticky ? 1 : 0;
            args.chainAddParameter[0] = 0;
            return true;
        }
        case UNDO_DeleteCreatedEntity:
            return DeleteEntityOrChokeOnGhost(op.entityGuid, args);
        case UNDO_RecreateDeletedEntity:
            // Recreate from-template path — host needs the original
            // template guid which the undo record does NOT carry in
            // V1. Logged as a known limitation.
            Core::Logging::Warn("Undo: recreate-deleted-entity is V1 unimplemented (host needs template ref)");
            return false;
    }
    return false;
}

void ClearUndoBecauseLevelChanged() {
    s_stack.clear();
}

i32 UndoStackDepth() { return (i32)s_stack.size(); }

} // namespace Apply
} // namespace Vespucci
