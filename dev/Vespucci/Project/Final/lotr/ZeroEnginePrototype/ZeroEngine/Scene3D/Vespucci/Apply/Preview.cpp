// Preview.cpp
// =============================================================================
// Staged-edit list. Single-threaded. Wiped on snapshot version change.
// =============================================================================
// Written by: Eriumsss

#include "Preview.h"

#include "../Core/Logging.h"

#include <vector>

namespace Vespucci {
namespace Apply {

namespace {
    static std::vector<StagedEdit> s_staged;
    static SnapshotVersion         s_anchorVersion(0);

    void EvictIfTheSnapshotMovedUnderUs(SnapshotVersion v) {
        if (s_anchorVersion != v) {
            if (!s_staged.empty()) {
                Core::Logging::Debug("Preview: snapshot moved (%u -> %u), wiping %zu staged",
                    s_anchorVersion.raw, v.raw, s_staged.size());
            }
            s_staged.clear();
            s_anchorVersion = v;
        }
    }
} // namespace

void StageWireOrPushItOffTheCliff(const Suggest::Suggestion& s,
                                    Guid sourceGuid,
                                    SnapshotVersion v)
{
    EvictIfTheSnapshotMovedUnderUs(v);
    StagedEdit e;
    e.kind = STAGED_AddWire;
    e.sourceGuid     = sourceGuid;
    e.targetGuid     = s.targetGuid;
    e.outputGuid     = Guid(0);
    e.eventName      = s.eventName;
    e.actionName     = s.actionName;
    e.delay          = s.suggestedDelay;
    e.sticky         = s.suggestedSticky;
    e.stagedAtVersion = v;
    s_staged.push_back(e);
}

void StageWireDeleteOrCryAboutIt(Guid sourceGuid, Guid outputGuid, SnapshotVersion v) {
    EvictIfTheSnapshotMovedUnderUs(v);
    StagedEdit e;
    e.kind = STAGED_DeleteWire;
    e.sourceGuid = sourceGuid;
    e.outputGuid = outputGuid;
    e.targetGuid = Guid(0);
    e.delay = 0; e.sticky = false;
    e.stagedAtVersion = v;
    s_staged.push_back(e);
}

i32 StagedEditCount() { return (i32)s_staged.size(); }

const StagedEdit* StagedEditAt(i32 i) {
    if (i < 0 || i >= (i32)s_staged.size()) return 0;
    return &s_staged[(size_t)i];
}

void ClearAllStagedEditsBecauseTheSnapshotMoved() {
    s_staged.clear();
}

} // namespace Apply
} // namespace Vespucci
