// SceneSnapshot.h
// =============================================================================
// SCENE SNAPSHOT — IMMUTABLE FRAME-STABLE VIEW OF THE LOADED LEVEL
// =============================================================================
// Written by: Eriumsss
//
// The doc-locked architecture: ranker reads a frozen snapshot, builder
// rebuilds in the background, atomic swap when the new one is ready.
// This is the snapshot. Once Build() returns, NOTHING in the snapshot
// changes — every ranker query in the same frame reads consistent
// data, no torn reads, no half-updated parent chains.
//
// Source of all this data is `ImGuiGlueFrameArgs` (host EXE fills it
// before each DrawFrame). We copy the parts we need into snapshot-
// owned arrays so the snapshot survives across frames if the args
// pointers churn. Memory cost is one u32 per entity field, times the
// number of fields we mirror — for 5,000-entity levels it adds up to
// roughly 1 MB. Acceptable for the working-set size editing demands.
//
// Snapshot version is the monotonic mutation counter the ranker cache
// keys on. Builder bumps version every Build(); the cache invalidates
// memoized results when it sees a version mismatch.
// =============================================================================

#ifndef VESPUCCI_SCENE_SCENESNAPSHOT_H_
#define VESPUCCI_SCENE_SCENESNAPSHOT_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"

#include <vector>

struct ImGuiGlueFrameArgs; // forward — defined in Scene3D/imgui_glue.h

namespace Vespucci {
namespace Scene {

// One mirrored entity row. Width-tight on purpose — a million of
// these would still fit in 100 MB.
struct EntityRow {
    Guid               guid;
    Guid               parentGuid;
    LayerGuid          layerGuid;
    TypeId             typeId;            // resolved via ZETypeRegistry, 0 if unknown
    Core::StringRef    name;              // points into snapshot's string pool
    Core::StringRef    typeName;
    Vec3               position;
    u32                gamemodeMask;
    u32                traits;            // mirrored from ZETypeRegistry::TypeRecord::traits
    bool               isOutputEnvelope;  // true if the entity IS an Output (wire envelope)
    bool               deprecated;
};

// One wire connection (resolved Triple).
struct WireRow {
    EntityIndex     ownerIdx;        // who owns this Output
    EntityIndex     outputIdx;       // the Output entity
    EntityIndex     targetIdx;       // -1 if broken
    Guid            outputGuid;
    Guid            targetGuid;
    Core::StringRef eventName;       // canonical lowercase
    Core::StringRef actionName;      // canonical lowercase
    f32             delay;
    bool            sticky;
};

class SceneSnapshot {
public:
    SceneSnapshot();
    ~SceneSnapshot();

    // Build from args. Replaces all snapshot state. Bumps version.
    // Returns true on success; false if args is malformed.
    bool build(const ImGuiGlueFrameArgs& args);

    // Read access. Stable across frames until next build().
    SnapshotVersion version() const { return m_version; }
    i32             entityCount() const;
    i32             wireCount() const;
    const EntityRow* entityAt(EntityIndex i) const;
    const WireRow*   wireAt(WireIndex i) const;

    // Lookup: GUID -> EntityIndex. Returns invalid index if not found.
    EntityIndex indexForGuid(Guid g) const;

    // Iteration helpers.
    Span<EntityRow> entities() const;
    Span<WireRow>   wires() const;

    // Frame-time stat for the debug overlay.
    f32 lastBuildMs() const { return m_lastBuildMs; }

    // Drop everything. Use when level unloads.
    void clear();

private:
    void buildEntityRows(const ImGuiGlueFrameArgs& args);
    void buildWireRows(const ImGuiGlueFrameArgs& args);
    void rebuildGuidIndex();

    SnapshotVersion         m_version;
    std::vector<EntityRow>  m_entities;
    std::vector<WireRow>    m_wires;

    // Open-addressing GUID -> index table. Sized to next-power-of-two
    // larger than 2x entity count.
    std::vector<i32>        m_guidBuckets;   // -1 = empty
    u32                     m_guidMask;

    // Owns interned name strings so EntityRow::name pointers stay
    // valid for the snapshot's lifetime.
    void*                   m_pool;          // Core::StringPool*

    f32                     m_lastBuildMs;
};

// Process singleton.
SceneSnapshot* GlobalSnapshot();
void           SetGlobalSnapshot(SceneSnapshot* s);

} // namespace Scene
} // namespace Vespucci

#endif // VESPUCCI_SCENE_SCENESNAPSHOT_H_
