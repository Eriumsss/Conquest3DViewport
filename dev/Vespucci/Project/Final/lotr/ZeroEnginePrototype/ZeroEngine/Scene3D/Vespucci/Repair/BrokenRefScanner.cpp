// BrokenRefScanner.cpp
// =============================================================================
// One pass over every wire in the snapshot. Classifies the failure
// mode, records owner + target guids for the HealthFix UI to render.
// =============================================================================
// Written by: Eriumsss

#include "BrokenRefScanner.h"

#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"

namespace Vespucci {
namespace Repair {

void ScanForBrokenRefs(const Scene::SceneSnapshot& snap,
                        std::vector<BrokenRef>& out)
{
    HEAT_SCOPE("Repair::ScanForBrokenRefs");
    out.clear();
    Span<Scene::WireRow> wires = snap.wires();
    out.reserve(wires.size() / 8);

    for (i32 i = 0; i < (i32)wires.size(); ++i) {
        const Scene::WireRow& w = wires[(usize)i];
        BrokenRef r;
        r.wire        = WireIndex(i);
        r.ownerGuid   = Guid(0);
        r.targetGuid  = w.targetGuid;
        r.detail      = Core::StringRef();

        const Scene::EntityRow* owner = w.ownerIdx.valid() ? snap.entityAt(w.ownerIdx) : 0;
        const Scene::EntityRow* tgt   = w.targetIdx.valid() ? snap.entityAt(w.targetIdx) : 0;
        if (owner) r.ownerGuid = owner->guid;

        if (!owner) {
            r.kind = BROKEN_OwnerGhost;
            r.detail = Core::StringRef("owner not in snapshot - this wire is hanging from nothing");
            out.push_back(r);
            continue;
        }
        if (!tgt) {
            r.kind = BROKEN_TargetGhost;
            r.detail = Core::StringRef("target_guid does not resolve to any entity");
            out.push_back(r);
            continue;
        }
        if (w.ownerIdx == w.targetIdx) {
            r.kind = BROKEN_Self;
            r.detail = Core::StringRef("wire targets its own owner - infinite-loop risk");
            out.push_back(r);
            continue;
        }
        if (w.eventName.size() == 0) {
            r.kind = BROKEN_EmptyEvent;
            r.detail = Core::StringRef("event name is empty - this wire fires nothing");
            out.push_back(r);
            continue;
        }
        if (w.actionName.size() == 0) {
            r.kind = BROKEN_EmptyAction;
            r.detail = Core::StringRef("action name is empty - target sees no event");
            out.push_back(r);
            continue;
        }
        if (tgt->deprecated) {
            r.kind = BROKEN_DeprecatedTarget;
            r.detail = Core::StringRef("target entity type is flagged deprecated");
            out.push_back(r);
            continue;
        }
    }

    if (!out.empty()) {
        Core::Logging::Info("Repair: %d broken-ref candidates flagged", (int)out.size());
    }
}

} // namespace Repair
} // namespace Vespucci
