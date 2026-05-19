// DidYouMean.h
// =============================================================================
// 'DID YOU MEAN' SUGGESTIONS FOR BROKEN-REF REPAIR
// =============================================================================
// Written by: Eriumsss
//
// Given a broken wire, propose up to 3 candidate replacement targets
// ranked by the doc-locked rule: type signature + layer dominate
// string similarity. Wraps the RepairRanker (Suggest layer) and the
// spatial-index radius query.
// =============================================================================

#ifndef VESPUCCI_REPAIR_DIDYOUMEAN_H_
#define VESPUCCI_REPAIR_DIDYOUMEAN_H_

#include "../Core/VespucciTypes.h"
#include "BrokenRefScanner.h"

namespace Vespucci {
namespace Suggest { struct Suggestion; }
namespace Repair {

struct RepairCandidate {
    Vespucci::EntityIndex idx;
    Guid                  guid;
    score_t               score;
    Core::StringRef       reason;     // short text for the chip
};

// Generate up to maxOut candidates for the given broken wire.
i32 GenerateDidYouMean(const Scene::SceneSnapshot& snap,
                        const BrokenRef& broken,
                        RepairCandidate* outCandidates,
                        i32 maxOut);

} // namespace Repair
} // namespace Vespucci

#endif // VESPUCCI_REPAIR_DIDYOUMEAN_H_
