// Preview.h
// =============================================================================
// PREVIEW LAYER - SHOW THE WIRE AS A GHOST BEFORE WE COMMIT
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked rule: clicks Apply MUST show a preview first. This file
// holds the staged-edit list. The UI sidebar / health-fix panel writes
// into it on Preview button; the F4 wire renderer reads it and draws
// ghost-styled wires for staged edits; Apply.cpp actually commits.
//
// Staged edits are TRANSIENT - they live until the user commits or
// cancels. They do NOT survive a snapshot rebuild. If a snapshot
// version changes mid-preview, we wipe the staged list and tell the
// UI to redraw - the underlying graph moved, the preview is stale.
// =============================================================================

#ifndef VESPUCCI_APPLY_PREVIEW_H_
#define VESPUCCI_APPLY_PREVIEW_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"
#include "../Suggest/SuggestionTypes.h"

namespace Vespucci {
namespace Apply {

enum StagedEditKind {
    STAGED_AddWire    = 0,
    STAGED_DeleteWire = 1,
    STAGED_Reparent   = 2
};

struct StagedEdit {
    StagedEditKind   kind;
    Guid             sourceGuid;
    Guid             targetGuid;
    Guid             outputGuid;     // for delete
    Core::StringRef  eventName;
    Core::StringRef  actionName;
    f32              delay;
    bool             sticky;
    SnapshotVersion  stagedAtVersion;
};

void StageWireOrPushItOffTheCliff(const Suggest::Suggestion& s,
                                    Guid sourceGuid,
                                    SnapshotVersion currentVersion);
void StageWireDeleteOrCryAboutIt(Guid sourceGuid, Guid outputGuid,
                                   SnapshotVersion currentVersion);

i32  StagedEditCount();
const StagedEdit* StagedEditAt(i32 i);

void ClearAllStagedEditsBecauseTheSnapshotMoved();

} // namespace Apply
} // namespace Vespucci

#endif // VESPUCCI_APPLY_PREVIEW_H_
