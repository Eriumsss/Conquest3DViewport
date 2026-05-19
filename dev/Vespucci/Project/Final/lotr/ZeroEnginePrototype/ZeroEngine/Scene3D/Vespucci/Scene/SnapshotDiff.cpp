// SnapshotDiff.cpp
// =============================================================================
// One pass over each snapshot, hash-set-based add/remove/mutate split.
// =============================================================================
// Written by: Eriumsss

#include "SnapshotDiff.h"

#include <unordered_set>
#include <unordered_map>

namespace Vespucci {
namespace Scene {

void ComputeDiff(const SceneSnapshot& oldSnap,
                 const SceneSnapshot& newSnap,
                 SnapshotDiff& out)
{
    out.oldVersion = oldSnap.version();
    out.newVersion = newSnap.version();
    out.addedEntities.clear();
    out.removedEntities.clear();
    out.mutatedEntities.clear();
    out.addedWireOutputs.clear();
    out.removedWireOutputs.clear();
    out.anyChange = false;

    if (oldSnap.version() == newSnap.version()) return;

    // Entity diff
    std::unordered_map<Guid, EntityRow> oldByGuid;
    Span<EntityRow> oldRows = oldSnap.entities();
    oldByGuid.reserve(oldRows.size());
    for (usize i = 0; i < oldRows.size(); ++i) oldByGuid[oldRows[i].guid] = oldRows[i];

    std::unordered_set<Guid> seen;
    seen.reserve(oldRows.size());

    Span<EntityRow> newRows = newSnap.entities();
    for (usize i = 0; i < newRows.size(); ++i) {
        const EntityRow& r = newRows[i];
        seen.insert(r.guid);
        std::unordered_map<Guid, EntityRow>::iterator it = oldByGuid.find(r.guid);
        if (it == oldByGuid.end()) {
            out.addedEntities.push_back(r.guid);
            out.anyChange = true;
        } else {
            const EntityRow& o = it->second;
            // Mutation: any structural field change.
            if (o.parentGuid != r.parentGuid || o.layerGuid != r.layerGuid ||
                o.typeId   != r.typeId)
            {
                out.mutatedEntities.push_back(r.guid);
                out.anyChange = true;
            }
        }
    }
    for (usize i = 0; i < oldRows.size(); ++i) {
        if (seen.find(oldRows[i].guid) == seen.end()) {
            out.removedEntities.push_back(oldRows[i].guid);
            out.anyChange = true;
        }
    }

    // Wire diff: simple add / remove on outputGuid identity.
    std::unordered_set<Guid> oldOuts;
    Span<WireRow> oldW = oldSnap.wires();
    oldOuts.reserve(oldW.size());
    for (usize i = 0; i < oldW.size(); ++i) oldOuts.insert(oldW[i].outputGuid);

    std::unordered_set<Guid> newOuts;
    Span<WireRow> newW = newSnap.wires();
    newOuts.reserve(newW.size());
    for (usize i = 0; i < newW.size(); ++i) newOuts.insert(newW[i].outputGuid);

    for (std::unordered_set<Guid>::const_iterator it = newOuts.begin(); it != newOuts.end(); ++it) {
        if (oldOuts.find(*it) == oldOuts.end()) {
            out.addedWireOutputs.push_back(*it);
            out.anyChange = true;
        }
    }
    for (std::unordered_set<Guid>::const_iterator it = oldOuts.begin(); it != oldOuts.end(); ++it) {
        if (newOuts.find(*it) == newOuts.end()) {
            out.removedWireOutputs.push_back(*it);
            out.anyChange = true;
        }
    }
}

} // namespace Scene
} // namespace Vespucci
