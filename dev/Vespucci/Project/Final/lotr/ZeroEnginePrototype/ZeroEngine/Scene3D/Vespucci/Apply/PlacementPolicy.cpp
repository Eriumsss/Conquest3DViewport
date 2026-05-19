// PlacementPolicy.cpp
// =============================================================================
// SUGGEST-A-LAYER + SUGGEST-A-PARENT + SUGGEST-A-POSITION FOR NEWLY-
// CREATED ENTITIES. THE POLICY READS THE LOADED SNAPSHOT AND ANSWERS
// "WHERE WOULD A REASONABLE LEVEL DESIGNER PUT THIS THING?"
// =============================================================================
// Written by: Eriumsss
//
// When the designer creates a new entity, the editor's default behavior
// is to drop it at the world origin in the active layer. That's the
// least useful default — the new TriggerVolume lands in the Lighting
// layer at (0,0,0), and the designer has to drag it twice. PlacementPolicy
// is the brain that fixes that:
//
//   1. SuggestLayerForType — picks the layer where this type is most
//      commonly placed across the loaded level. AudioManagers go in the
//      Audio layer, CapturePoints in the Gameplay layer, etc.
//   2. SuggestParentForType — picks the most-common parent entity for
//      this type. PathNodes parent to PathNetworks; Outputs to their
//      owning entity.
//   3. SuggestPositionNearby — given the active selection (or viewport
//      camera focus), drop the new entity near it instead of at origin.
//   4. SanityCheckPlacement — refuses to place a singleton type if one
//      already exists, returns a soft warning the editor can show.
//
// All four are advisory. The editor's create flow can override any of
// them; this file just provides the smartest default available given
// what's in the snapshot.
// =============================================================================

#include "../Scene/SceneSnapshot.h"
#include "../Schema/ZETypeRegistry.h"
#include "../Core/Logging.h"
#include "imgui_glue.h"

#include <cstdio>
#include <unordered_map>

namespace Vespucci {
namespace Apply {

// Suggest a layer for a type — argmax of observed layer histogram for
// that type across the loaded level. Falls back to the active layer
// when the histogram is empty or the winner doesn't have enough
// support to beat noise.
LayerGuid SuggestLayerForTypeOrShrugAndUseActive(
    const Scene::SceneSnapshot& snap,
    TypeId t,
    LayerGuid activeLayer)
{
    if (!t.valid()) return activeLayer;

    std::unordered_map<LayerGuid, i32> layerCounts;
    Span<Scene::EntityRow> rows = snap.entities();
    for (usize i = 0; i < rows.size(); ++i) {
        const Scene::EntityRow& r = rows[i];
        if (r.typeId == t && r.layerGuid.valid()) {
            layerCounts[r.layerGuid]++;
        }
    }

    LayerGuid best = activeLayer;
    i32 bestCount = 0;
    for (std::unordered_map<LayerGuid, i32>::const_iterator it = layerCounts.begin();
         it != layerCounts.end(); ++it)
    {
        if (it->second > bestCount) {
            bestCount = it->second;
            best = it->first;
        }
    }

    if (bestCount < 2) {
        // Not enough confidence. Fall back to active.
        return activeLayer;
    }
    Core::Logging::Debug("PlacementPolicy: type 0x%08X best-fit layer 0x%08X (count=%d)",
        (unsigned)t.raw, (unsigned)best.raw, bestCount);
    return best;
}

// Suggest a parent GUID for a type — argmax of (typeId, parentGuid)
// histogram. PathNodes nest under PathNetworks; Outputs nest under
// their owning entity. Returns 0 if no strong winner.
Guid SuggestParentForTypeOrZero(const Scene::SceneSnapshot& snap, TypeId t) {
    if (!t.valid()) return Guid(0);

    std::unordered_map<u32, i32> parentCounts;
    Span<Scene::EntityRow> rows = snap.entities();
    for (usize i = 0; i < rows.size(); ++i) {
        const Scene::EntityRow& r = rows[i];
        if (r.typeId == t && r.parentGuid.raw != 0) {
            parentCounts[r.parentGuid.raw]++;
        }
    }

    u32 best = 0;
    i32 bestCount = 0;
    for (std::unordered_map<u32, i32>::const_iterator it = parentCounts.begin();
         it != parentCounts.end(); ++it)
    {
        if (it->second > bestCount) {
            bestCount = it->second;
            best = it->first;
        }
    }
    if (bestCount < 2) return Guid(0);
    return Guid(best);
}

// Suggest a spawn position for a new entity of a given type. Two
// strategies blend: if the designer has an active selection of the
// SAME type, snap near that selection. Otherwise drop at the camera
// focus point.
Vec3 SuggestSpawnPositionOrFallToOrigin(const Scene::SceneSnapshot& snap,
                                          TypeId t,
                                          Guid activeSelection,
                                          const Vec3& cameraFocus,
                                          f32 jitterRadius)
{
    Vec3 pos = cameraFocus;
    if (activeSelection.raw != 0) {
        EntityIndex idx = snap.indexForGuid(activeSelection);
        if (idx.valid()) {
            const Scene::EntityRow* row = snap.entityAt(idx);
            if (row && row->typeId == t) {
                // Same-type neighbor: drop slightly offset so the new
                // entity is visible.
                pos.x = row->position.x + jitterRadius;
                pos.y = row->position.y;
                pos.z = row->position.z + jitterRadius;
                return pos;
            } else if (row) {
                // Different-type neighbor: drop in front of it but at
                // a larger offset.
                pos.x = row->position.x + jitterRadius * 2.0f;
                pos.y = row->position.y;
                pos.z = row->position.z;
                return pos;
            }
        }
    }
    return pos;
}

// Pre-flight sanity check. Returns true if placement is OK, false if
// it should be blocked (and writes a designer-facing reason). Used by
// the create-entity wizard to refuse "place a second Director".
bool SanityCheckPlacementOrPunchOut(const Scene::SceneSnapshot& snap,
                                      TypeId t,
                                      char* outReason,
                                      i32 reasonCap)
{
    if (outReason && reasonCap > 0) outReason[0] = 0;
    if (!t.valid()) return true;

    Schema::ZETypeRegistry* reg = Schema::GlobalRegistry();
    if (!reg) return true;
    const Schema::TypeRecord* rec = reg->findById(t);
    if (!rec) return true;

    // Singleton check.
    if (rec->traits & (u32)Schema::TRAIT_Singleton) {
        Span<Scene::EntityRow> rows = snap.entities();
        i32 existing = 0;
        for (usize i = 0; i < rows.size(); ++i) {
            if (rows[i].typeId == t) existing++;
        }
        if (existing > 0) {
            if (outReason && reasonCap > 0) {
                const char* fmt = "%.*s is a singleton-type — already %d in this level";
                std::snprintf(outReason, (size_t)reasonCap, fmt,
                    (int)rec->name.size(), rec->name.data(), existing);
            }
            return false;
        }
    }

    // Deprecated check.
    if (rec->traits & (u32)Schema::TRAIT_DEPRECATED) {
        if (outReason && reasonCap > 0) {
            const char* fmt = "%.*s is a deprecated type — placing it is allowed but will be flagged in HealthFix";
            std::snprintf(outReason, (size_t)reasonCap, fmt,
                (int)rec->name.size(), rec->name.data());
        }
        // Soft pass — we let it through but flag.
        return true;
    }

    return true;
}

// Composite policy — runs all three suggestions and packs the result
// into one struct. Wizard calls this once per "Create entity" and
// reads what it needs.
struct PlacementSuggestion {
    LayerGuid suggestedLayer;
    Guid      suggestedParent;
    Vec3      suggestedPosition;
    bool      placementBlocked;
    char      blockReason[160];
};

PlacementSuggestion BuildFullPlacementSuggestion(
    const Scene::SceneSnapshot& snap,
    TypeId t,
    LayerGuid activeLayer,
    Guid activeSelection,
    const Vec3& cameraFocus)
{
    PlacementSuggestion p;
    p.suggestedLayer    = SuggestLayerForTypeOrShrugAndUseActive(snap, t, activeLayer);
    p.suggestedParent   = SuggestParentForTypeOrZero(snap, t);
    p.suggestedPosition = SuggestSpawnPositionOrFallToOrigin(
        snap, t, activeSelection, cameraFocus, 1.5f);
    p.placementBlocked  = !SanityCheckPlacementOrPunchOut(
        snap, t, p.blockReason, (i32)sizeof(p.blockReason));
    return p;
}

} // namespace Apply
} // namespace Vespucci

