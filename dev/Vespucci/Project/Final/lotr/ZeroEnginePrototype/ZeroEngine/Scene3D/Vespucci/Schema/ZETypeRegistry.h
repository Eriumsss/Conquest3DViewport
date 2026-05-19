// ZETypeRegistry.h
// =============================================================================
// THE GODDAMN TYPE GAZETTEER FOR EVERY ZE LEVEL ENTITY EVER COMPILED
// =============================================================================
// Written by: Eriumsss
//
// Every motherfucking thing in a Zero Engine level is a typed entity:
// CapturePoint, TriggerVolume, ScoreManager, SpawnPoint, DoorActivator,
// VOTrigger, ObjectiveMarker, AnimationController, GateController,
// GhostKnight, PathNetwork, MissileLauncher, ScriptRelay, Director.
// One hundred seventy-two distinct top-level type names across the
// LOTR:C corpus, with one thousand five hundred and three field
// definitions stretched across them. EA shut Pandemic down before
// they could publish the schema, so we have ZERO official spec - we
// reverse-engineered the field layouts from the raw .lvl bytes one
// painful goddamn entry at a time. That work lives in:
//   - project_level_json_deep.md (the source-of-truth memory)
//   - LevelReader.h (the runtime parser)
//   - EntityFieldDefs.h (the field-by-field disassembly)
//
// THIS HEADER DOES NOT REPARSE THE BINARY. We trust LevelReader for
// that fight. ZETypeRegistry is a NORMALIZED VIEW the ranker can
// query without giving a shit about block1_obj_offset / type_def_index
// / field_crc detective work. It exposes:
//   - TypeId by name (xxhash of the canonical lowercase name)
//   - Display name for UI ("CapturePoint", not "capture_point" or
//     CRC 0xDA8E2F77)
//   - Parent-chain walk (CapturePoint extends Volume extends Entity)
//   - Trait flags (is-spatial, has-events, has-outputs, is-singleton)
//   - The TypeId set valid as wire ENDPOINTS (eligible source for
//     drag-to-wire) and INPUTS (eligible target). The compat matrix
//     starts from this set; if a type cannot be wired from at all
//     (ScoreManager OUTPUT? no such thing), we drop it before the
//     ranker even thinks about it.
//
// Storage: open-addressing hash from TypeId to TypeRecord. ~200 entries
// in the corpus, hash table sized to next-power-of-two * 2. Lookup is
// one hash + a handful of probes, well under 50 ns. Init is ~2 ms once
// at editor startup; rebuild on level load only if the level brings a
// type the registry has not seen before (rare, usually means modders).
//
// Lifetime: registry is a process singleton. Created in Vespucci::Init,
// destroyed in Vespucci::Shutdown. Never null after Init returns.
// =============================================================================

#ifndef VESPUCCI_SCHEMA_ZETYPEREGISTRY_H_
#define VESPUCCI_SCHEMA_ZETYPEREGISTRY_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"

namespace Vespucci {
namespace Schema {

// Trait flags. Bitmask. Pulled from level.json analysis - if we add
// a trait we add a bit at the END (struct-layout-end commandment
// applies to flag enums too, otherwise older serialized golden tests
// drift in their numerical values).
enum TypeTrait {
    TRAIT_None         = 0,
    TRAIT_Spatial      = 1 <<  0,    // has WorldTransform / position
    TRAIT_HasEvents    = 1 <<  1,    // emits events through Outputs[]
    TRAIT_HasInputs    = 1 <<  2,    // accepts events through input_event field
    TRAIT_HasOutputs   = 1 <<  3,    // owns Outputs[] block (envelope type)
    TRAIT_Singleton    = 1 <<  4,    // exactly one expected per level (Director, ScoreManager, ...)
    TRAIT_Volume       = 1 <<  5,    // has a bounding extent (TriggerVolume, CaptureArea)
    TRAIT_PathNode     = 1 <<  6,    // member of a PathNetwork
    TRAIT_Spawner      = 1 <<  7,    // creates other entities at runtime
    TRAIT_Animated     = 1 <<  8,    // drives an AnimationController
    TRAIT_Audio        = 1 <<  9,    // VOTrigger, AmbientSound, AudioManager
    TRAIT_Cinematic    = 1 << 10,    // CameraTrack, CinematicEvent, Director
    TRAIT_Gameplay     = 1 << 11,    // CapturePoint, ObjectiveMarker, ScoreManager
    TRAIT_Render       = 1 << 12,    // StaticMesh, ParticleEmitter, LightSource
    TRAIT_Physics      = 1 << 13,    // RigidBody, ConstraintJoint
    TRAIT_AI           = 1 << 14,    // GhostKnight, AIDirector, BehaviorTree
    TRAIT_Layer        = 1 << 15,    // is itself a layer (templateLayer types)
    TRAIT_DEPRECATED   = 1 << 16,    // exists in old maps, do not suggest as a target for new wires
};

// Per-type record. One entry per known entity type. Stored in the
// registry's hash table; the ranker holds POINTERS to these records
// and they MUST stay stable for the registry's lifetime - which is
// the editor's whole life, so do not realloc the records array
// after init without telling everyone first.
struct TypeRecord {
    TypeId           id;            // hash of canonical lowercase name
    Core::StringRef  name;          // display name (CapturePoint)
    Core::StringRef  canonicalName; // canonical lowercase (capturepoint)
    TypeId           parentId;      // 0 if no parent (Entity is the root)
    u32              traits;        // bitmask of TypeTrait
    i32              crcMpeg;       // Pandemic CRC of name (for level.lvl matching)
    Core::StringRef  description;   // one-line UI tooltip text
};

// Registry. Lookup-only after Init; not thread-safe for concurrent
// writes. Reads from any thread are fine (no internal mutation post-Init).
class ZETypeRegistry {
public:
    ZETypeRegistry();
    ~ZETypeRegistry();

    // Initialize from the built-in table (ZETypeRegistry_Builtins.cpp).
    // After this returns true, all built-in types are registered. Idempotent.
    bool init();

    // Register a single type. Returns the assigned TypeId. Used by
    // Init for the built-in table and by Schema/SignatureLoader_Lua.cpp
    // when a level file declares a type the built-ins don't cover.
    TypeId registerType(const char* displayName,
                        const char* canonicalName,
                        TypeId parentId,
                        u32 traits,
                        i32 crcMpeg,
                        const char* description);

    // Look up by various keys.
    const TypeRecord* findById(TypeId id) const;
    const TypeRecord* findByName(const char* displayName) const;
    const TypeRecord* findByCanonical(const char* canonical) const;
    const TypeRecord* findByMpegCrc(i32 crc) const;

    // Iteration. Used by the corpus builder to enumerate all known
    // types and by the debug overlay to show "registered types: N".
    i32              count() const;
    const TypeRecord* at(i32 idx) const;

    // Trait test convenience.
    bool hasTrait(TypeId id, TypeTrait t) const;

    // Walk parent chain. Returns true if `descendant` is `ancestor`
    // or any of its ancestors. Walks max 16 levels to avoid runaway
    // on a malformed registry; the deepest known LOTR:C chain is 4.
    bool isSubtypeOf(TypeId descendant, TypeId ancestor) const;

    // Reset for tests. Drops all registered types. NEVER call from
    // editor code after Init() - corrupts ranker pointers.
    void resetForTests();

    // Stats for telemetry.
    struct Stats {
        i32 typeCount;
        i32 hashBuckets;
        i32 hashCollisions;
    };
    Stats stats() const;

private:
    struct Bucket {
        TypeId id;
        i32    recordIdx;   // -1 = empty
    };

    void grow();
    i32  probe(TypeId id) const;

    Bucket*     m_buckets;
    i32         m_bucketCount;
    i32         m_bucketMask;

    TypeRecord* m_records;
    i32         m_recordCap;
    i32         m_recordCount;

    void*       m_arena;     // Core::Arena*
    void*       m_pool;      // Core::StringPool*

    mutable i32 m_collisions;
};

// Global accessor. Returns the singleton registry, or NULL if Init
// has not been called yet (caller must handle that during editor boot).
ZETypeRegistry* GlobalRegistry();
void            SetGlobalRegistry(ZETypeRegistry* reg);

} // namespace Schema
} // namespace Vespucci

#endif // VESPUCCI_SCHEMA_ZETYPEREGISTRY_H_
