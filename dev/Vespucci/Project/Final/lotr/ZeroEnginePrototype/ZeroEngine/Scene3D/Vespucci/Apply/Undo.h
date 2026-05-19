// Undo.h
// =============================================================================
// UNDO STACK FOR APPLIED SUGGESTIONS - INVERSE OPS, BOUNDED HISTORY
// =============================================================================
// Written by: Eriumsss
//
// User clicks Apply on a suggested wire, the wire commits, then the
// user clicks Undo. We push an INVERSE op onto the undo stack at
// commit time, pop on undo, fire the reverse host request. History
// caps at kUndoCap because keeping ten thousand entries does not
// help anyone and bloats the editor's working set.
// =============================================================================

#ifndef VESPUCCI_APPLY_UNDO_H_
#define VESPUCCI_APPLY_UNDO_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"

struct ImGuiGlueFrameArgs;

namespace Vespucci {
namespace Apply {

enum UndoOpKind {
    UNDO_DeleteAddedWire   = 0,   // inverse of an Apply that added a wire
    UNDO_RecreateDeletedWire = 1, // inverse of a wire-delete (best-effort)
    UNDO_DeleteCreatedEntity = 2, // inverse of a Create-from-template
    UNDO_RecreateDeletedEntity = 3 // inverse of a Delete (NOT yet wired)
};

struct UndoOp {
    UndoOpKind      kind;
    Guid            sourceGuid;
    Guid            targetGuid;
    Guid            outputGuid;     // valid for delete/recreate-wire
    Guid            entityGuid;     // for entity create/delete
    Core::StringRef eventName;
    Core::StringRef actionName;
    f32             delay;
    bool            sticky;
};

void PushUndoOpOrChokeOnTheStackOverflow(const UndoOp& op);
bool PopUndoOpAndFireItOrPissOff(ImGuiGlueFrameArgs& args);
void ClearUndoBecauseLevelChanged();
i32  UndoStackDepth();

} // namespace Apply
} // namespace Vespucci

#endif // VESPUCCI_APPLY_UNDO_H_
