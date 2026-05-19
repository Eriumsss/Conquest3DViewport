// SnapshotDiff.h
// =============================================================================
// FRAME-TO-FRAME DIFF — WHAT CHANGED BETWEEN TWO SNAPSHOT VERSIONS
// =============================================================================
// Written by: Eriumsss
//
// The SuggestionCache wants "did anything that affects MY query
// change this frame?" without rebuilding from scratch. SnapshotDiff
// answers that by comparing two consecutive snapshots and emitting
// a list of added / removed / mutated entity-and-wire indices.
//
// Cheap path: if version is unchanged, diff is empty. Otherwise we
// do a hash-based one-pass compare. We do NOT do anything fancier
// because the snapshot is rebuilt entirely on every mutation, so
// diff cost is O(N) and N is in the thousands; ~50us in practice.
// =============================================================================

#ifndef VESPUCCI_SCENE_SNAPSHOTDIFF_H_
#define VESPUCCI_SCENE_SNAPSHOTDIFF_H_

#include "../Core/VespucciTypes.h"
#include "SceneSnapshot.h"

#include <vector>

namespace Vespucci {
namespace Scene {

struct SnapshotDiff {
    SnapshotVersion        oldVersion;
    SnapshotVersion        newVersion;
    std::vector<Guid>      addedEntities;
    std::vector<Guid>      removedEntities;
    std::vector<Guid>      mutatedEntities;
    std::vector<Guid>      addedWireOutputs;
    std::vector<Guid>      removedWireOutputs;
    bool                   anyChange;
};

void ComputeDiff(const SceneSnapshot& oldSnap,
                 const SceneSnapshot& newSnap,
                 SnapshotDiff& outDiff);

} // namespace Scene
} // namespace Vespucci

#endif // VESPUCCI_SCENE_SNAPSHOTDIFF_H_
