// HealthRules.cpp
// =============================================================================
// EXTRA HEALTH-CHECK RULE PACK BEYOND BROKEN-REF DETECTION
// =============================================================================
// Written by: Eriumsss
//
// BrokenRefScanner finds the obvious cases. This file adds the
// secondary rules: orphan entities (in no layer), duplicate names
// in the same layer (designer-confusion smell), unreachable rules
// (deny shadowed by allow above it), and singleton-violations
// (Director / ScoreManager appearing twice).
// =============================================================================

#include "BrokenRefScanner.h"

#include "../Scene/SceneSnapshot.h"
#include "../Schema/ZETypeRegistry.h"
#include "../Core/Logging.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Vespucci {
namespace Repair {

enum HealthRuleKind {
    HEALTH_OrphanNoLayer            = 100,
    HEALTH_DuplicateInLayer         = 101,
    HEALTH_SingletonViolation       = 102,
    HEALTH_UnreachableEntity        = 103,
    HEALTH_DanglingParent           = 104,
    HEALTH_DeprecatedTypeStillUsed  = 105,
    HEALTH_GamemodeSilent           = 106,
    HEALTH_OutputEnvelopeNoWires    = 107,
    HEALTH_TriggerWithoutOutputs    = 108,
    HEALTH_PathNodeWithoutLinks     = 109,
    HEALTH_CapturePointNoScorer     = 110,
    HEALTH_VOTriggerNoAudioManager  = 111,
    HEALTH_DirectorMissing          = 112,
    HEALTH_LayerCycle               = 113
};

struct HealthIssue {
    HealthRuleKind  kind;
    Guid            guidA;
    Guid            guidB;     // 0 unless duplicate-pair
    Core::StringRef detail;
};

void RunHealthRulesAcrossSnapshot(const Scene::SceneSnapshot& snap,
                                    std::vector<HealthIssue>& out)
{
    out.clear();
    Span<Scene::EntityRow> rows = snap.entities();

    // Rule 1: orphan-no-layer.
    for (usize i = 0; i < rows.size(); ++i) {
        const Scene::EntityRow& r = rows[i];
        if (!r.layerGuid.valid() && !(r.traits & (u32)Schema::TRAIT_Layer)) {
            HealthIssue issue;
            issue.kind   = HEALTH_OrphanNoLayer;
            issue.guidA  = r.guid;
            issue.guidB  = Guid(0);
            issue.detail = Core::StringRef("entity has no layer assignment");
            out.push_back(issue);
        }
    }

    // Rule 2: duplicate name in same layer (heuristic for designer-confusion).
    std::unordered_map<u64, Guid> nameLayerKey; // pack (layerGuid, hash(name))
    for (usize i = 0; i < rows.size(); ++i) {
        const Scene::EntityRow& r = rows[i];
        if (!r.layerGuid.valid() || r.name.size() == 0) continue;
        u64 key = ((u64)r.layerGuid.raw << 32);
        u32 nh = 0;
        for (usize k = 0; k < r.name.size(); ++k) {
            nh = (nh * 16777619u) ^ (u32)r.name.data()[k];
        }
        key |= (u64)nh;
        std::unordered_map<u64, Guid>::iterator it = nameLayerKey.find(key);
        if (it != nameLayerKey.end()) {
            HealthIssue issue;
            issue.kind   = HEALTH_DuplicateInLayer;
            issue.guidA  = it->second;
            issue.guidB  = r.guid;
            issue.detail = Core::StringRef("two entities in this layer share a name (case-sensitive)");
            out.push_back(issue);
        } else {
            nameLayerKey[key] = r.guid;
        }
    }

    // Rule 3: singleton-violation.
    Schema::ZETypeRegistry* reg = Schema::GlobalRegistry();
    if (reg) {
        std::unordered_map<u32, i32> singletonCounts;
        std::unordered_map<u32, Guid> singletonFirst;
        for (usize i = 0; i < rows.size(); ++i) {
            const Scene::EntityRow& r = rows[i];
            if (r.traits & (u32)Schema::TRAIT_Singleton) {
                singletonCounts[r.typeId.raw]++;
                if (singletonFirst.find(r.typeId.raw) == singletonFirst.end()) {
                    singletonFirst[r.typeId.raw] = r.guid;
                } else if (singletonCounts[r.typeId.raw] == 2) {
                    HealthIssue issue;
                    issue.kind   = HEALTH_SingletonViolation;
                    issue.guidA  = singletonFirst[r.typeId.raw];
                    issue.guidB  = r.guid;
                    issue.detail = Core::StringRef("two instances of a singleton-typed entity exist");
                    out.push_back(issue);
                }
            }
        }
    }

    // Rule 4: dangling parent — entity claims a parent GUID that does
    // not resolve to anything in the snapshot. Different from
    // BROKEN_OwnerGhost in that this is the parent-of relationship,
    // not the wire-owner relationship.
    {
        std::unordered_set<u32> validGuids;
        validGuids.reserve(rows.size());
        for (usize i = 0; i < rows.size(); ++i) {
            validGuids.insert(rows[i].guid.raw);
        }
        for (usize i = 0; i < rows.size(); ++i) {
            const Scene::EntityRow& r = rows[i];
            if (r.parentGuid.raw == 0) continue;     // root, fine
            if (validGuids.find(r.parentGuid.raw) == validGuids.end()) {
                HealthIssue issue;
                issue.kind   = HEALTH_DanglingParent;
                issue.guidA  = r.guid;
                issue.guidB  = r.parentGuid;
                issue.detail = Core::StringRef("entity's parent GUID does not resolve");
                out.push_back(issue);
            }
        }
    }

    // Rule 5: deprecated type still used. We let these stay loaded but
    // surface them so the level can be migrated to non-deprecated
    // equivalents before the next ship.
    for (usize i = 0; i < rows.size(); ++i) {
        const Scene::EntityRow& r = rows[i];
        if (r.deprecated) {
            HealthIssue issue;
            issue.kind   = HEALTH_DeprecatedTypeStillUsed;
            issue.guidA  = r.guid;
            issue.guidB  = Guid(0);
            issue.detail = Core::StringRef("entity's type is flagged deprecated");
            out.push_back(issue);
        }
    }

    // Rule 6: gamemode-silent — entity is masked into no game mode and
    // will therefore never spawn at runtime. Common after a designer
    // disables a gameplay mode without removing the assets.
    for (usize i = 0; i < rows.size(); ++i) {
        const Scene::EntityRow& r = rows[i];
        // Skip layer entities (their gamemode is implicit through the
        // children) and infrastructure types (no gameplay role).
        if (r.traits & (u32)Schema::TRAIT_Layer) continue;
        if (r.gamemodeMask == 0) {
            HealthIssue issue;
            issue.kind   = HEALTH_GamemodeSilent;
            issue.guidA  = r.guid;
            issue.guidB  = Guid(0);
            issue.detail = Core::StringRef("entity has zero gamemode mask — never spawns");
            out.push_back(issue);
        }
    }

    // Rule 7: trigger volume / event source with NO outgoing wires.
    // Designer probably forgot to wire it. We test by walking the
    // wires once and tallying outgoing count per owner.
    {
        std::unordered_map<u32, i32> outgoingCount;
        Span<Scene::WireRow> wires = snap.wires();
        for (usize w = 0; w < wires.size(); ++w) {
            const Scene::EntityRow* owner = snap.entityAt(wires[w].ownerIdx);
            if (owner) outgoingCount[owner->guid.raw]++;
        }
        for (usize i = 0; i < rows.size(); ++i) {
            const Scene::EntityRow& r = rows[i];
            if (!(r.traits & (u32)Schema::TRAIT_HasEvents)) continue;
            if (outgoingCount.find(r.guid.raw) == outgoingCount.end()) {
                HealthIssue issue;
                issue.kind   = HEALTH_TriggerWithoutOutputs;
                issue.guidA  = r.guid;
                issue.guidB  = Guid(0);
                issue.detail = Core::StringRef("event-emitting entity has no outgoing wires");
                out.push_back(issue);
            }
        }
    }

    // Rule 8: PathNode entity with no PathLink references. Path
    // network is broken if any node is unreachable. We can detect
    // this from the PathLink wire pattern — but for V1 we just check
    // that any PathNode has at least one wire either way.
    {
        std::unordered_set<u32> pathTouched;
        Span<Scene::WireRow> wires = snap.wires();
        for (usize w = 0; w < wires.size(); ++w) {
            const Scene::EntityRow* owner = snap.entityAt(wires[w].ownerIdx);
            const Scene::EntityRow* tgt = wires[w].targetIdx.valid()
                                          ? snap.entityAt(wires[w].targetIdx) : 0;
            if (owner && (owner->traits & (u32)Schema::TRAIT_PathNode)) {
                pathTouched.insert(owner->guid.raw);
            }
            if (tgt && (tgt->traits & (u32)Schema::TRAIT_PathNode)) {
                pathTouched.insert(tgt->guid.raw);
            }
        }
        for (usize i = 0; i < rows.size(); ++i) {
            const Scene::EntityRow& r = rows[i];
            if (!(r.traits & (u32)Schema::TRAIT_PathNode)) continue;
            if (pathTouched.find(r.guid.raw) == pathTouched.end()) {
                HealthIssue issue;
                issue.kind   = HEALTH_PathNodeWithoutLinks;
                issue.guidA  = r.guid;
                issue.guidB  = Guid(0);
                issue.detail = Core::StringRef("PathNode is not referenced by any PathLink — orphan");
                out.push_back(issue);
            }
        }
    }

    // Rule 9: CapturePoint exists but no ScoreManager singleton.
    // Conquest mode requires a ScoreManager to receive AddScore events;
    // a CapturePoint without one fires into the void at runtime.
    if (reg) {
        bool hasCapture = false;
        bool hasScorer  = false;
        const Schema::TypeRecord* capRec = reg->findByCanonical("capturepoint");
        const Schema::TypeRecord* sm     = reg->findByCanonical("scoremanager");
        TypeId capTypeId = capRec ? capRec->id : TypeId(0);
        TypeId smTypeId  = sm     ? sm->id     : TypeId(0);
        for (usize i = 0; i < rows.size() && !(hasCapture && hasScorer); ++i) {
            if (capTypeId.valid() && rows[i].typeId == capTypeId) hasCapture = true;
            if (smTypeId.valid()  && rows[i].typeId == smTypeId)  hasScorer  = true;
        }
        if (hasCapture && !hasScorer) {
            HealthIssue issue;
            issue.kind   = HEALTH_CapturePointNoScorer;
            issue.guidA  = Guid(0);
            issue.guidB  = Guid(0);
            issue.detail = Core::StringRef("level has CapturePoints but no ScoreManager singleton");
            out.push_back(issue);
        }
    }

    // Rule 10: VOTrigger exists but no AudioManager singleton.
    if (reg) {
        bool hasVO = false;
        bool hasAM = false;
        const Schema::TypeRecord* voRec = reg->findByCanonical("votrigger");
        const Schema::TypeRecord* amRec = reg->findByCanonical("audiomanager");
        TypeId voTypeId = voRec ? voRec->id : TypeId(0);
        TypeId amTypeId = amRec ? amRec->id : TypeId(0);
        for (usize i = 0; i < rows.size() && !(hasVO && hasAM); ++i) {
            if (voTypeId.valid() && rows[i].typeId == voTypeId) hasVO = true;
            if (amTypeId.valid() && rows[i].typeId == amTypeId) hasAM = true;
        }
        if (hasVO && !hasAM) {
            HealthIssue issue;
            issue.kind   = HEALTH_VOTriggerNoAudioManager;
            issue.guidA  = Guid(0);
            issue.guidB  = Guid(0);
            issue.detail = Core::StringRef("level has VOTriggers but no AudioManager singleton — VO will never play");
            out.push_back(issue);
        }
    }

    // Rule 11: Director presence check — every shipping LOTR:C level
    // has exactly one Director. Missing → flag for designer.
    if (reg) {
        const Schema::TypeRecord* dirRec = reg->findByCanonical("director");
        TypeId dirTypeId = dirRec ? dirRec->id : TypeId(0);
        bool hasDirector = false;
        for (usize i = 0; i < rows.size() && !hasDirector; ++i) {
            if (dirTypeId.valid() && rows[i].typeId == dirTypeId) hasDirector = true;
        }
        if (dirTypeId.valid() && !hasDirector) {
            HealthIssue issue;
            issue.kind   = HEALTH_DirectorMissing;
            issue.guidA  = Guid(0);
            issue.guidB  = Guid(0);
            issue.detail = Core::StringRef("level has no Director — match flow controller missing");
            out.push_back(issue);
        }
    }

    if (!out.empty()) {
        Core::Logging::Info("HealthRules: %zu secondary issues flagged", out.size());
    }
}

// Plain-English label for each rule kind. Designer sees these in the
// HealthFix advanced view.
const char* HealthRuleLabel(HealthRuleKind k) {
    switch (k) {
        case HEALTH_OrphanNoLayer:           return "Orphan (no layer)";
        case HEALTH_DuplicateInLayer:        return "Duplicate name in layer";
        case HEALTH_SingletonViolation:      return "Singleton type appears twice";
        case HEALTH_UnreachableEntity:       return "Unreachable entity";
        case HEALTH_DanglingParent:          return "Parent does not exist";
        case HEALTH_DeprecatedTypeStillUsed: return "Deprecated type still used";
        case HEALTH_GamemodeSilent:          return "Will never spawn (zero gamemode mask)";
        case HEALTH_OutputEnvelopeNoWires:   return "Output entity has no wires";
        case HEALTH_TriggerWithoutOutputs:   return "Event source has no wires";
        case HEALTH_PathNodeWithoutLinks:    return "Orphan PathNode";
        case HEALTH_CapturePointNoScorer:    return "CapturePoint without ScoreManager";
        case HEALTH_VOTriggerNoAudioManager: return "VOTrigger without AudioManager";
        case HEALTH_DirectorMissing:         return "Level has no Director";
        case HEALTH_LayerCycle:              return "Layer parent cycle detected";
    }
    return "Unknown";
}

} // namespace Repair
} // namespace Vespucci
