// HealPlanner.cpp
// =============================================================================
// Topo-sort-ish ordering. Self-targeting wires repair first; ghost-
// target wires repair second; deprecated-target third; everything
// else trails. Within a tier we keep the user's input order.
// =============================================================================
// Written by: Eriumsss

#include "HealPlanner.h"

#include "../Core/Logging.h"

#include <algorithm>

namespace Vespucci {
namespace Repair {

void PlanHealOrderOrChokeOnConflicts(std::vector<HealStep>& steps) {
    // Stable sort by tier so equal-tier ties preserve insertion order.
    std::stable_sort(steps.begin(), steps.end(),
        [](const HealStep& a, const HealStep& b) {
            auto Tier = [](BrokenRefKind k) -> i32 {
                switch (k) {
                    case BROKEN_Self:             return 0;
                    case BROKEN_TargetGhost:      return 1;
                    case BROKEN_OwnerGhost:       return 2;
                    case BROKEN_DeprecatedTarget: return 3;
                    case BROKEN_EmptyEvent:       return 4;
                    case BROKEN_EmptyAction:      return 5;
                }
                return 9;
            };
            return Tier(a.broken.kind) < Tier(b.broken.kind);
        });
    Core::Logging::Debug("HealPlanner: ordered %zu steps", steps.size());
}

} // namespace Repair
} // namespace Vespucci
