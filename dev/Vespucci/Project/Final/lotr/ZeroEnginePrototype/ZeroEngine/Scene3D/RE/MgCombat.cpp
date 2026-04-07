// MgCombat.cpp
// Reconstructed from:
//   FUN_007626a4  (MgCombat_LookupEffect, ~300 addr)
//   FUN_00848a2c  (MgCombat_LookupImpact, 1407 addr)

#include "MgCombat.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Game function pointers
// ---------------------------------------------------------------------------
PFN_MgTypeSystem_GetClassName g_pfn_MgTypeSystem_GetClassName = NULL; // 0x008a69ac

// Global effect table pointer  (game: [0xcd7e24])
static void* g_effect_table = NULL;

// ---------------------------------------------------------------------------
// FUN_007626a4 — MgCombat_LookupEffect
//
// ASM flow (simplified):
//   For each field in the filter (13 fields via string key array):
//     PUSH type_handle (0xe560e0) / PUSH field_key_ptr
//     CALL 0x008a69ac   ; MgTypeSystem_GetClassName → name string
//     ; store resolved name into local filter copy
//   ; Iterate effect table (ptr at [0xcd7e24])
//   ; Compare each entry's filter fields against the input filter
//   ; Return first match or NULL
//
// String constants for field keys (game .rdata):
//   "EffectLookup", "Events", "self_type", "self_stance", "self_race",
//   "target_type", "target_stance", "target_race",
//   "outgoing_attack_type", "incoming_attack_type",
//   (3 additional field names not yet read)
//
// In standalone reconstruction:
//   No effect table → always returns NULL.
// ---------------------------------------------------------------------------
static const char* k_combat_field_names[] = {
    "self_type",
    "self_stance",
    "self_race",
    "target_type",
    "target_stance",
    "target_race",
    "outgoing_attack_type",
    "incoming_attack_type",
    NULL  // sentinel
};

const struct MgCombatEffectEntry* MgCombat_LookupEffect(
    const MgCombatFilter* filter)
{
    if (!filter || !g_effect_table) return NULL;
    if (!g_pfn_MgTypeSystem_GetClassName) return NULL;

    // In the game binary, for each field, call MgTypeSystem_GetClassName
    // with the engine type handle (0xe560e0) to resolve a type name string,
    // then compare against each entry in the effect table.
    //
    // Type handle constant: 0xe560e0 (engine-wide type system handle)
    void* type_handle = (void*)0xe560e0; // only valid with game binary loaded
    (void)type_handle;
    (void)k_combat_field_names;

    // Table iteration and field comparison require game binary vtable access.
    // Deferred pending sub-function 0x00862xxx analysis.
    return NULL;
}

// ---------------------------------------------------------------------------
// FUN_00848a2c — MgCombat_LookupImpact (1407 addr-count)
//
// The largest function in Combat/.
// String refs: "impact", "weapon", "material", "impact_kill", "impact_size"
// These match the MgImpactEntry field names.
//
// This function builds a multi-key lookup over the impact/material table.
// The full table structure and search algorithm require deeper analysis of
// the 1407-addr body — deferred.
// ---------------------------------------------------------------------------
const struct MgImpactEntry* MgCombat_LookupImpact(
    const char* impact_name,
    const char* weapon_name,
    const char* material_name)
{
    (void)impact_name;
    (void)weapon_name;
    (void)material_name;
    // TODO: implement after sub-function analysis of FUN_00848a2c body
    return NULL;
}
