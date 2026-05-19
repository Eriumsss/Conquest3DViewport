// HealthScorer.cpp
// =============================================================================
// PER-ENTITY + LEVEL-WIDE HEALTH SCORING. CONVERTS THE BROKEN-REF
// SCANNER'S RAW OUTPUT INTO PRIORITIZED, COLOR-CODED SCORES THE
// OUTLINER + HEALTH-FIX PANEL CAN RANK BY.
// =============================================================================
// Written by: Eriumsss
//
// The scanner gives us a flat list — "here are 22 broken wires." That
// list is useless to a designer who's looking at Helm's Deep with 5,000
// entities and wants to know:
//
//   - Which TEN entities are the most broken?
//   - Is the LEVEL overall healthy or hosed?
//   - Which broken-wire CLASSES dominate (self-loops vs ghost targets
//     vs deprecated targets)? Tells the team what to triage first.
//
// All three answers come out of HealthScorer. Per-entity score is on a
// 0..100 scale (100 = clean). Level-wide health is a weighted average
// where critical classes (self-loops, owner ghosts) drag harder than
// soft issues (empty action, deprecated target). Per-class severity
// buckets count how many of each class were found and how bad each
// class is for the level as a whole.
//
// The weights below are tuned against observed LOTR:C corpus damage —
// self-loops are catastrophic (they fire forever), ghost targets are
// usually one-shot dead wires (annoying but recoverable), deprecated
// targets are often correctable to a non-deprecated equivalent
// without designer review.
// =============================================================================

#include "BrokenRefScanner.h"

#include "../Scene/SceneSnapshot.h"
#include "../Schema/ZETypeRegistry.h"
#include "../Core/Logging.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Vespucci {
namespace Repair {

// Severity score per broken-wire class. Higher = worse. The level-
// health calculator multiplies the count of each class by its
// severity to produce the aggregate damage value.
struct ClassSeverity {
    BrokenRefKind kind;
    f32           severity;   // 0..1, applied as wire-damage weight
    const char*   label;
    const char*   designerHint;
};

namespace {
    // Tuned by inspecting Helm's Deep, Black Gate, Pelennor, and Mordor.
    // Self-loops and owner ghosts hit hardest because they prevent
    // gameplay logic from advancing. Empty events / actions are silent
    // failures — the wire exists but fires nothing, so the level keeps
    // running but the designer's intent is mute. Deprecated targets are
    // soft — the wire fires, just into something we tagged old.
    const ClassSeverity kSeverityTable[] = {
        { BROKEN_TargetGhost,      0.85f, "Ghost target",
          "Wire points at a deleted entity — it fires into the void." },
        { BROKEN_OwnerGhost,       0.95f, "Ghost owner",
          "Wire's owning entity was deleted but the wire row remains — "
          "this is a level-file inconsistency that belongs in the corpus janitor." },
        { BROKEN_EmptyEvent,       0.45f, "Empty event name",
          "Wire fires no event — usually a copy-paste artifact." },
        { BROKEN_EmptyAction,      0.40f, "Empty action name",
          "Wire's target receives no action — silent failure." },
        { BROKEN_Self,             1.00f, "Self-loop",
          "Wire targets its own owner — may cause an infinite event chain." },
        { BROKEN_DeprecatedTarget, 0.20f, "Deprecated target type",
          "Wire points at an entity whose type was deprecated. Re-route "
          "to a non-deprecated equivalent if one exists." }
    };
    const i32 kSeverityCount = (i32)(sizeof(kSeverityTable) /
                                     sizeof(kSeverityTable[0]));

    f32 SeverityFor(BrokenRefKind k) {
        for (i32 i = 0; i < kSeverityCount; ++i) {
            if (kSeverityTable[i].kind == k) return kSeverityTable[i].severity;
        }
        return 0.5f; // unknown class — treat as middle severity
    }

    const char* LabelFor(BrokenRefKind k) {
        for (i32 i = 0; i < kSeverityCount; ++i) {
            if (kSeverityTable[i].kind == k) return kSeverityTable[i].label;
        }
        return "Unknown";
    }
} // namespace

// Per-entity health score (0..100). Higher is healthier. Aggregates
// every broken wire that touches `entityGuid` (as owner OR target),
// weighted by class severity. Designer wants high numbers.
i32 ComputePerEntityHealthScore(Guid entityGuid,
                                  const std::vector<BrokenRef>& brokenList,
                                  i32 wireDegree)
{
    if (wireDegree <= 0) return 100;       // no wires, trivially clean
    f32 damage = 0.0f;
    for (size_t i = 0; i < brokenList.size(); ++i) {
        bool touches = (brokenList[i].ownerGuid  == entityGuid ||
                        brokenList[i].targetGuid == entityGuid);
        if (!touches) continue;
        damage += SeverityFor(brokenList[i].kind);
    }
    f32 fraction = damage / (f32)wireDegree;
    if (fraction > 1.0f) fraction = 1.0f;
    f32 score = (1.0f - fraction) * 100.0f;
    if (score < 0.0f) score = 0.0f;
    return (i32)(score + 0.5f);
}

// Build a `guid -> score` map for every entity in the snapshot. The
// Outliner renders this as a colored health badge next to entity rows.
// Caller decides what color thresholds to use; this function only
// populates the raw 0..100 score.
void BuildHealthMap(const Scene::SceneSnapshot& snap,
                    const std::vector<BrokenRef>& brokenList,
                    std::unordered_map<u32, i32>& outScores)
{
    outScores.clear();
    outScores.reserve((size_t)snap.entityCount());

    // First pass: count wire degree per entity. Snapshot's WireRow
    // already gives us owner / target indices we can de-resolve to
    // GUIDs via the entity table.
    std::unordered_map<u32, i32> wireDegree;
    Span<Scene::WireRow> wires = snap.wires();
    for (usize w = 0; w < wires.size(); ++w) {
        const Scene::EntityRow* owner = snap.entityAt(wires[w].ownerIdx);
        if (owner) wireDegree[owner->guid.raw]++;
        if (wires[w].targetIdx.valid()) {
            const Scene::EntityRow* tgt = snap.entityAt(wires[w].targetIdx);
            if (tgt) wireDegree[tgt->guid.raw]++;
        }
    }

    // Second pass: score each entity.
    Span<Scene::EntityRow> rows = snap.entities();
    for (usize i = 0; i < rows.size(); ++i) {
        i32 deg = 1;
        std::unordered_map<u32, i32>::const_iterator it =
            wireDegree.find(rows[i].guid.raw);
        if (it != wireDegree.end()) deg = it->second;
        i32 score = ComputePerEntityHealthScore(rows[i].guid, brokenList, deg);
        outScores[rows[i].guid.raw] = score;
    }
}

// Aggregate level-wide health number (0..100). Weighted average over
// every entity's score, with critical-class entities pulled toward
// the floor harder than soft-issue ones.
i32 ComputeLevelHealthScore(const Scene::SceneSnapshot& snap,
                             const std::vector<BrokenRef>& brokenList)
{
    i32 entityN = snap.entityCount();
    if (entityN == 0) return 100;
    if (brokenList.empty()) return 100;

    f32 totalDamage = 0.0f;
    for (size_t i = 0; i < brokenList.size(); ++i) {
        totalDamage += SeverityFor(brokenList[i].kind);
    }
    // Damage relative to entity count — a level with 5000 entities and
    // 22 broken wires is far healthier than a 50-entity level with 22.
    f32 normalized = totalDamage / (f32)entityN;
    if (normalized > 1.0f) normalized = 1.0f;
    f32 score = (1.0f - normalized) * 100.0f;
    if (score < 0.0f) score = 0.0f;
    return (i32)(score + 0.5f);
}

// Per-class breakdown — count of each broken-wire class plus its
// aggregated damage contribution. Used by the HealthFix panel's
// summary header and by the QA dashboard.
struct ClassReport {
    BrokenRefKind kind;
    i32           count;
    f32           totalSeverity;
    const char*   label;
    const char*   designerHint;
};

void BuildClassBreakdown(const std::vector<BrokenRef>& brokenList,
                          std::vector<ClassReport>& outReport)
{
    outReport.clear();
    outReport.reserve((size_t)kSeverityCount);
    for (i32 i = 0; i < kSeverityCount; ++i) {
        ClassReport r;
        r.kind          = kSeverityTable[i].kind;
        r.count         = 0;
        r.totalSeverity = 0.0f;
        r.label         = kSeverityTable[i].label;
        r.designerHint  = kSeverityTable[i].designerHint;
        outReport.push_back(r);
    }
    for (size_t i = 0; i < brokenList.size(); ++i) {
        for (size_t r = 0; r < outReport.size(); ++r) {
            if (outReport[r].kind == brokenList[i].kind) {
                outReport[r].count++;
                outReport[r].totalSeverity += kSeverityTable[r].severity;
                break;
            }
        }
    }
    // Sort descending by total severity so the most damaging classes
    // surface first in the UI.
    std::sort(outReport.begin(), outReport.end(),
        [](const ClassReport& a, const ClassReport& b) {
            return a.totalSeverity > b.totalSeverity;
        });
}

// Top-N most damaged entities — the Outliner's "show me what's on
// fire" view. Returns indices into snap's entity table sorted ascending
// by score (lowest score = most damaged).
i32 FindTopDamagedEntities(const Scene::SceneSnapshot& snap,
                             const std::vector<BrokenRef>& brokenList,
                             EntityIndex* outIndices,
                             i32* outScores,
                             i32 cap)
{
    if (cap <= 0 || !outIndices) return 0;
    std::unordered_map<u32, i32> healthMap;
    BuildHealthMap(snap, brokenList, healthMap);

    struct Slot { EntityIndex idx; i32 score; };
    std::vector<Slot> slots;
    slots.reserve(healthMap.size());
    Span<Scene::EntityRow> rows = snap.entities();
    for (usize i = 0; i < rows.size(); ++i) {
        std::unordered_map<u32, i32>::const_iterator it =
            healthMap.find(rows[i].guid.raw);
        if (it == healthMap.end()) continue;
        if (it->second >= 100) continue;     // healthy, skip
        Slot s;
        s.idx   = EntityIndex((i32)i);
        s.score = it->second;
        slots.push_back(s);
    }
    std::sort(slots.begin(), slots.end(),
        [](const Slot& a, const Slot& b) { return a.score < b.score; });

    i32 emit = (i32)slots.size();
    if (emit > cap) emit = cap;
    for (i32 i = 0; i < emit; ++i) {
        outIndices[i] = slots[(size_t)i].idx;
        if (outScores) outScores[i] = slots[(size_t)i].score;
    }
    return emit;
}

// Per-layer health aggregation — useful for "this layer is broken,
// fix it before merging." Returns layer guid -> average score.
void BuildPerLayerHealth(const Scene::SceneSnapshot& snap,
                          const std::vector<BrokenRef>& brokenList,
                          std::unordered_map<u32, i32>& outLayerHealth)
{
    outLayerHealth.clear();
    std::unordered_map<u32, i32> entityHealth;
    BuildHealthMap(snap, brokenList, entityHealth);

    std::unordered_map<u32, i32> layerSum;
    std::unordered_map<u32, i32> layerCount;
    Span<Scene::EntityRow> rows = snap.entities();
    for (usize i = 0; i < rows.size(); ++i) {
        u32 layer = rows[i].layerGuid.raw;
        std::unordered_map<u32, i32>::const_iterator it =
            entityHealth.find(rows[i].guid.raw);
        i32 score = (it != entityHealth.end()) ? it->second : 100;
        layerSum[layer]   += score;
        layerCount[layer] += 1;
    }
    for (std::unordered_map<u32, i32>::const_iterator it = layerCount.begin();
         it != layerCount.end(); ++it)
    {
        if (it->second > 0) {
            outLayerHealth[it->first] = layerSum[it->first] / it->second;
        } else {
            outLayerHealth[it->first] = 100;
        }
    }
}

// Per-type health aggregation — answers "which type of entity tends
// to break most often in this level?" Useful for triage and for
// generating compat rules that filter that type out.
void BuildPerTypeHealth(const Scene::SceneSnapshot& snap,
                         const std::vector<BrokenRef>& brokenList,
                         std::unordered_map<u32, i32>& outTypeHealth)
{
    outTypeHealth.clear();
    std::unordered_map<u32, i32> entityHealth;
    BuildHealthMap(snap, brokenList, entityHealth);

    std::unordered_map<u32, i32> typeSum;
    std::unordered_map<u32, i32> typeCount;
    Span<Scene::EntityRow> rows = snap.entities();
    for (usize i = 0; i < rows.size(); ++i) {
        u32 t = rows[i].typeId.raw;
        if (t == 0) continue;
        std::unordered_map<u32, i32>::const_iterator it =
            entityHealth.find(rows[i].guid.raw);
        i32 score = (it != entityHealth.end()) ? it->second : 100;
        typeSum[t]   += score;
        typeCount[t] += 1;
    }
    for (std::unordered_map<u32, i32>::const_iterator it = typeCount.begin();
         it != typeCount.end(); ++it)
    {
        if (it->second > 0) {
            outTypeHealth[it->first] = typeSum[it->first] / it->second;
        } else {
            outTypeHealth[it->first] = 100;
        }
    }
}

// Color helper — maps a 0..100 health score to a packed RGBA u32.
// Designer staring at the Outliner can scan-color-code at a glance
// without reading numbers. Green = healthy, yellow = warning, orange
// = serious, red = critical.
u32 ColorForHealthScore(i32 score) {
    if (score >= 95) return 0xFF60D080u; // green
    if (score >= 80) return 0xFFA0D060u; // yellow-green
    if (score >= 60) return 0xFFD0C040u; // yellow
    if (score >= 40) return 0xFFE08040u; // orange
    if (score >= 20) return 0xFFE05040u; // red-orange
    return                  0xFFE03030u; // red
}

const char* TextLabelForHealthScore(i32 score) {
    if (score >= 95) return "Pristine";
    if (score >= 80) return "Healthy";
    if (score >= 60) return "Showing wear";
    if (score >= 40) return "Damaged";
    if (score >= 20) return "Critical";
    return                  "Hosed";
}

} // namespace Repair
} // namespace Vespucci
