// BrokenRefScanner.h
// =============================================================================
// SCAN THE WIRE GRAPH FOR BROKEN TARGET REFS - GHOSTS, ORPHANS, CYCLES
// =============================================================================
// Written by: Eriumsss
//
// Designers delete entities. Wires that pointed at those entities are
// now broken refs. The HealthFix panel surfaces them; this scanner is
// what finds them. Walks every wire in the snapshot, validates the
// target lookup, classifies the failure mode, returns a flat list.
// =============================================================================

#ifndef VESPUCCI_REPAIR_BROKENREFSCANNER_H_
#define VESPUCCI_REPAIR_BROKENREFSCANNER_H_

#include "../Core/VespucciTypes.h"
#include "../Scene/SceneSnapshot.h"

#include <vector>

namespace Vespucci {
namespace Repair {

enum BrokenRefKind {
    BROKEN_TargetGhost      = 0,   // target_guid does not resolve to any entity
    BROKEN_OwnerGhost       = 1,   // owner does not resolve
    BROKEN_EmptyEvent       = 2,   // event_name is empty - wire fires nothing
    BROKEN_EmptyAction      = 3,   // action_name is empty - target sees nothing
    BROKEN_Self             = 4,   // wire targets itself (cycle of length 1)
    BROKEN_DeprecatedTarget = 5    // target type is flagged deprecated
};

struct BrokenRef {
    WireIndex      wire;
    BrokenRefKind  kind;
    Guid           ownerGuid;
    Guid           targetGuid;
    Core::StringRef detail;
};

void ScanForBrokenRefs(const Scene::SceneSnapshot& snap,
                        std::vector<BrokenRef>& outList);

} // namespace Repair
} // namespace Vespucci

#endif // VESPUCCI_REPAIR_BROKENREFSCANNER_H_
