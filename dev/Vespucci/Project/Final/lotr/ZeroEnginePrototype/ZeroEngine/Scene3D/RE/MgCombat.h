// MgCombat.h
// Reconstructed combat subsystem.
// Sources: analyzed/Combat/
//
// Functions:
//   FUN_007626a4  — combat effect lookup (13 fields)
//   FUN_00848a2c  — impact / material lookup table

#pragma once
#include <stdint.h>
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// MgCombatFilter — 13-field filter structure for effect lookups.
// Recovered from FUN_007626a4 string refs and 0x008a69ac call pattern.
//
// Field names (in order of lookup, via MgTypeSystem_GetClassName 0x008a69ac):
//   0  self_type
//   1  self_stance
//   2  self_race
//   3  target_type
//   4  target_stance
//   5  target_race
//   6  outgoing_attack_type
//   7  incoming_attack_type
//   8  (EffectLookup / Events — header fields)
//   9  …13 (remaining sub-fields)
//
// Layout: each field is a resolved type handle (void*), derived from string
// keys via MgTypeSystem_GetClassName and the "EffectLookup"/"Events" containers.
// Global effect table: [0xcd7e24]
// ---------------------------------------------------------------------------
struct MgCombatFilter {
    void*    self_type;              // +0x00
    void*    self_stance;            // +0x04
    void*    self_race;              // +0x08
    void*    target_type;            // +0x0c
    void*    target_stance;          // +0x10
    void*    target_race;            // +0x14
    void*    outgoing_attack_type;   // +0x18
    void*    incoming_attack_type;   // +0x1c
    uint8_t  _reserved[0x10];       // +0x20 remaining fields
};

// ---------------------------------------------------------------------------
// MgCombatEffectEntry — one entry in the effect lookup table.
// Minimal layout recovered from FUN_007626a4.
// Full layout is pending deeper analysis of sub-function 0x00862xxx.
// ---------------------------------------------------------------------------
struct MgCombatEffectEntry {
    MgCombatFilter filter;  // +0x00 : 13-field filter block
    void*          effect;  // effect payload pointer
    // … (further fields not yet recovered)
};

// ---------------------------------------------------------------------------
// FUN_007626a4 — MgCombat_LookupEffect (large, ~300 addr-count)
//
// Searches the global combat effect table ([0xcd7e24]) for an entry
// matching the provided filter.
//
// For each of the 13 fields, it reads the type name via:
//   MgTypeSystem_GetClassName(0xe560e0, field_key_ptr) → name string
//
// Then iterates the effect table and returns the first matching entry.
//
// String refs in the function:
//   "EffectLookup", "Events", "self_type", "self_stance", "self_race",
//   "target_type", "target_stance", "target_race",
//   "outgoing_attack_type", "incoming_attack_type", …
//
// Returns: MgCombatEffectEntry* (or NULL if not found).
// ---------------------------------------------------------------------------
typedef void (__cdecl* PFN_MgTypeSystem_GetClassName)(void* type_handle,
                                                       const void* class_key,
                                                       MgString* out_name);
extern PFN_MgTypeSystem_GetClassName g_pfn_MgTypeSystem_GetClassName; // 0x008a69ac

const struct MgCombatEffectEntry* MgCombat_LookupEffect(
    const MgCombatFilter* filter);

// ---------------------------------------------------------------------------
// MgImpactEntry — one entry in the impact/material table.
// Recovered from FUN_00848a2c string refs.
//
// Field names:
//   "impact", "weapon", "material", "impact_kill", "impact_size"
// ---------------------------------------------------------------------------
struct MgImpactEntry {
    MgString impact;       // +0x00
    MgString weapon;       // +0x08
    MgString material;     // +0x10
    MgString impact_kill;  // +0x18
    int32_t  impact_size;  // +0x20
};

// ---------------------------------------------------------------------------
// FUN_00848a2c — MgCombat_LookupImpact (1407 addr-count)
//
// Searches the impact/material lookup table using the provided field keys.
// Fields looked up: "impact", "weapon", "material", "impact_kill", "impact_size".
//
// This is the largest function in Combat/ (1407 addr-count).
// Full reconstruction is deferred pending sub-function analysis.
// Returns: MgImpactEntry* (or NULL if not found).
// ---------------------------------------------------------------------------
const struct MgImpactEntry* MgCombat_LookupImpact(
    const char* impact_name,
    const char* weapon_name,
    const char* material_name);

#ifdef __cplusplus
} // extern "C"
#endif
