// HealPlanner.h
// =============================================================================
// HEAL PLANNER - SEQUENCE A SET OF REPAIR ACTIONS SO THEY DON'T FIGHT
// =============================================================================
// Written by: Eriumsss
//
// User selects N broken wires + their chosen replacement targets and
// hits "Apply All". The planner orders the repairs to avoid stomping
// on each other (e.g. delete-then-recreate must run delete first; a
// repair that points at another broken target must run AFTER that
// target's own repair).
// =============================================================================

#ifndef VESPUCCI_REPAIR_HEALPLANNER_H_
#define VESPUCCI_REPAIR_HEALPLANNER_H_

#include "../Core/VespucciTypes.h"
#include "BrokenRefScanner.h"
#include "DidYouMean.h"

#include <vector>

namespace Vespucci {
namespace Repair {

struct HealStep {
    BrokenRef       broken;
    RepairCandidate replacement;
};

// Sort the steps into a safe execution order. Stable when no
// dependencies exist between steps.
void PlanHealOrderOrChokeOnConflicts(std::vector<HealStep>& steps);

} // namespace Repair
} // namespace Vespucci

#endif // VESPUCCI_REPAIR_HEALPLANNER_H_
