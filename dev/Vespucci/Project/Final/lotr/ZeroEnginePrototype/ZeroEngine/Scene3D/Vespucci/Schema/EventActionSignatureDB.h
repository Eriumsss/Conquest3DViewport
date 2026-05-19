// EventActionSignatureDB.h
// =============================================================================
// EVENT/ACTION SIGNATURES — WHAT EACH GODDAMN TYPE CAN EMIT AND ACCEPT
// =============================================================================
// Written by: Eriumsss
//
// ZETypeRegistry knows the types. This database knows what each type
// CAN do. CapturePoint emits OnCapture / OnNeutralize / OnContested.
// ScoreManager accepts AddScore / SubtractScore / ResetScore. Wiring
// CapturePoint.OnCapture -> ScoreManager.AddScore is legal; wiring
// CapturePoint.OnCapture -> StaticMesh.Move is fucking nonsense and
// must never enter the candidate set.
//
// This is the "hard compatibility filter" backbone. The compat matrix
// in Compat/ asks two questions:
//   1. Does the source type EMIT this event?
//   2. Does the target type ACCEPT this action?
// Both checks live here. Compat/RuleSet adds higher-level constraints
// (this layer cannot wire to that one, this game-mode mask blocks it,
// etc.) on top of the signature check.
//
// Storage: per-TypeId, two lists: emitted events, accepted actions.
// Each entry is a normalized lowercase name, a display name, and an
// optional doc string. Normalized name is what the corpus stores;
// display is what the UI shows.
//
// Population: built-in defaults registered in EventActionSignatureDB.cpp
// for the major types (CapturePoint, TriggerVolume, ScoreManager,
// Director, ScriptRelay, GateController, VOTrigger, AnimationController).
// Lua-loader fills in modder-supplied types from level scripts. Unknown
// types get a permissive default that accepts any common name and
// emits OnTrigger / OnFinished as fallbacks.
// =============================================================================

#ifndef VESPUCCI_SCHEMA_EVENTACTIONSIGNATUREDB_H_
#define VESPUCCI_SCHEMA_EVENTACTIONSIGNATUREDB_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"

namespace Vespucci {
namespace Schema {

class ZETypeRegistry;

// One entry per known event or action.
struct SignatureEntry {
    Core::StringRef name;          // canonical lowercase name ("oncapture")
    Core::StringRef displayName;   // UI display ("OnCapture")
    Core::StringRef doc;           // tooltip text
    u32             frequency;     // observed corpus count (0 if hand-authored)
};

// Per-type signature record. Both lists may be empty if the type does
// not emit or accept events.
struct TypeSignature {
    TypeId           typeId;
    SignatureEntry*  emittedEvents;
    i32              emittedCount;
    SignatureEntry*  acceptedActions;
    i32              acceptedCount;
};

class EventActionSignatureDB {
public:
    EventActionSignatureDB();
    ~EventActionSignatureDB();

    // Initialize from built-in tables. Idempotent.
    bool init(const ZETypeRegistry* reg);

    // Register an emitted event for `typeId`. Adds if not already
    // present; bumps frequency by `freq` if present.
    void registerEvent(TypeId typeId,
                       const char* canonicalName,
                       const char* displayName,
                       const char* doc,
                       u32 freq);

    // Register an accepted action for `typeId`.
    void registerAction(TypeId typeId,
                        const char* canonicalName,
                        const char* displayName,
                        const char* doc,
                        u32 freq);

    // Lookup. Returns NULL if no signature is recorded for the type.
    const TypeSignature* findSignature(TypeId typeId) const;

    // Quick predicates - the hot path of the compat filter.
    bool typeEmitsEvent(TypeId typeId, const char* eventCanonicalName) const;
    bool typeAcceptsAction(TypeId typeId, const char* actionCanonicalName) const;

    // Walk all events emitted by `typeId` (used by the autocomplete
    // popup to suggest event names ranked by frequency).
    const SignatureEntry* eventsFor(TypeId typeId, i32* outCount) const;
    const SignatureEntry* actionsFor(TypeId typeId, i32* outCount) const;

    // Bulk update: bump `freq` of an existing entry. The corpus
    // builder calls this thousands of times per level load.
    void bumpEventFrequency(TypeId typeId, const char* canonicalName, u32 inc);
    void bumpActionFrequency(TypeId typeId, const char* canonicalName, u32 inc);

    // Stats for telemetry.
    struct Stats {
        i32 typeCount;
        i32 totalEvents;
        i32 totalActions;
    };
    Stats stats() const;

    // Reset for tests. Drops all entries.
    void resetForTests();

private:
    // Open-addressing hash from TypeId to TypeSignature index.
    struct Bucket {
        TypeId id;
        i32    sigIdx;   // -1 = empty
    };

    void grow();
    i32  probe(TypeId id) const;
    TypeSignature& ensureSignature(TypeId id);
    SignatureEntry* findEntry(SignatureEntry* arr, i32 count, const char* canonicalName) const;
    void appendEntry(SignatureEntry*& arr, i32& count, i32& cap,
                     const char* canonical, const char* display,
                     const char* doc, u32 freq);

    Bucket*         m_buckets;
    i32             m_bucketCount;
    i32             m_bucketMask;

    TypeSignature*  m_sigs;
    i32             m_sigCount;
    i32             m_sigCap;

    void*           m_arena;     // Core::Arena*
    void*           m_pool;      // Core::StringPool*
};

// Process singleton.
EventActionSignatureDB* GlobalSignatureDB();
void                    SetGlobalSignatureDB(EventActionSignatureDB* db);

} // namespace Schema
} // namespace Vespucci

#endif // VESPUCCI_SCHEMA_EVENTACTIONSIGNATUREDB_H_
