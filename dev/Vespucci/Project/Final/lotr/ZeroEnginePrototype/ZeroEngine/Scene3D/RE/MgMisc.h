// MgMisc.h
// Reconstructed miscellaneous engine functions that don't fit a larger subsystem.
//
// Source functions:
//   FUN_005c7e40  UTC time delta calculator (EC_time)
//   FUN_006bb641  Scaleform/GFx text renderer state reset (EC_time, "Times New Roman")
//   FUN_007cc118  InstanceClassName entity lookup (EC_math)
//   FUN_0079c210  StancesFilter/ActionsFilter validator (EC_math)
//   FUN_008238b3  Spawn event dispatcher (EC_math, 832 addr-count)

#pragma once
#include "MgTypes.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// FUN_005c7e40 — UTC time delta calculator
// ============================================================================
// Calls: __time64, __gmtime64, __mktime64, __difftime64
// This is the standard UTC timezone offset calculation, storing the
// result into a 64-bit value at [EBX+0x94] (some session/network object).
// The offset is adjusted by 0xe10 (3600 seconds = 1 hour) if a timezone
// flag at [ESP+0x34] is set.
//
// Object layout:
//   +0x94: int64_t utc_offset_seconds
//   +0xa0: int64_t reference_time_low   (64-bit, split as [+0xa0]+[+0xa4])
//   +0xa4: int64_t reference_time_high

struct MgTimeObject {
    uint8_t _pad[0x94];
    int64_t utc_offset_seconds; // +0x94 (from 0x005ab350 which stores int64 here)
    uint8_t _pad2[0x08];
    int64_t reference_time;     // +0xa0 (split dword [+0xa0] and [+0xa4])
};

// FUN_005c7e40 — compute and store UTC offset into time_obj->utc_offset_seconds.
// use_dst: if non-zero, adds 3600 seconds (DST adjustment, [ESP+0x34]).
void MgTime_ComputeUTCOffset(MgTimeObject* time_obj, int use_dst);

// ============================================================================
// FUN_006bb641 — Scaleform/GFx text format object reset
// ============================================================================
// "Times New Roman" font reset for a GFx TextFormat object.
// Clears format flags, alpha channel, sets specific style bits.
//
// Object layout (GFx::TextFormat partial):
//   +0x1c: uint32_t flags (RGBA-like, masked with 0xFF000000)
//   +0x20: uint16_t style_flags2
//   +0x24: uint8_t  misc_flags (AND 0xf3)
//   +0x58: uint16_t format_flags
//
// After reset:
//   format_flags |= 0x1 (dirty)
//   flags &= 0xFF000000, then |= 0xFF000000 (set full alpha)
//   style_flags2 &= 0x0 (cleared)
//   format_flags &= 0xFBFD, then |= 0xC0
//   misc_flags &= 0xF3
struct MgGfxTextFormat {
    uint8_t  _pad_00[0x1c];
    uint32_t color_flags;   // +0x1c
    uint8_t  _pad_20[0x04];
    uint16_t style_flags2;  // +0x24 (actually at +0x20 per ASM AND word[ESI+0x20])
    uint8_t  misc_flags;    // +0x24
    uint8_t  _pad_25[0x33];
    uint16_t format_flags;  // +0x58
};

void MgGfx_ResetTextFormat(MgGfxTextFormat* fmt);

// ============================================================================
// FUN_007cc118 — InstanceClassName entity lookup
// ============================================================================
// Looks up a game entity by its class name string.
// Returns the entity pointer or NULL if not found / validation fails.
//
// Pattern:
//   PUSH 0xe560e0 / PUSH 0x9db598  → construct string key "InstanceClassName"
//   CALL 0x008a69ac                 → get class name string from type system
//   PUSH EDI (constructed string)
//   CALL 0x007c6a3b                 → find_entity_by_class_name(&name_str)
//   TEST ESI,ESI / JZ return_null
//   PUSH [EBP+0x8] (search_param)
//   CALL vtable[0x20]               → entity->validate(param) → bool
//   if failed: CALL vtable[0x4](1)  → entity->release()
//   return entity or NULL
MgEntity* MgEntity_FindByClassName(const char* class_name, void* search_param);

// ============================================================================
// FUN_0079c210 — StancesFilter / ActionsFilter validator
// ============================================================================
// Validates whether a blend entry has non-empty stance and action filters.
// Object layout (blend filter entry):
//   +0x04: MgFilterArray stances_filter  (+0x04 ptr, +0x0C count)
//   +0x10: MgFilterArray actions_filter  (+0x10 ptr, +0x18 count)
//
// Logic:
//   PUSH 0xe560e0 / PUSH 0x9c7eac → type check using 0x008a6948 (IsTypeOf)
//   if not the right type → return false
//   read stances at [EDI+0x4] / [EDI+0xC] (ptr + count)
//   read actions at [EDI+0x10] / [EDI+0x18]
//   return true if either count != 0
struct MgAnimFilterEntry {
    uint32_t _unk_00;
    void*    stances_ptr;    // +0x04
    uint8_t  _pad_08[0x04];
    int      stances_count;  // +0x0C
    void*    actions_ptr;    // +0x10
    uint8_t  _pad_14[0x04];
    int      actions_count;  // +0x18
};

int MgAnimFilter_HasStancesOrActions(const MgAnimFilterEntry* filter,
                                      const void* type_check_arg);

// ============================================================================
// FUN_008238b3 — Spawn event dispatcher (partial, 832 addr-count)
// ============================================================================
// Dispatches OnSpawn and unit-class spawn events (OnWarriorSpawn, OnArcherSpawn,
// OnDruidSpawn, OnEngineerSpawn, OnAssassinSpawn) based on spawn data.
//
// Object layout (spawner entity):
//   +0x150: void* spawn_data_list   (list of spawn entries)
//   +0x1a8: spawn_event_list        (+0x14 count, +0x4 ptr, +0xC tail)
//   +0x1f0..+0x208: spawn event IDs resolved from names:
//     [+0x1f0] OnSpawn id
//     [+0x1f4] OnWarriorSpawn id
//     [+0x1f8] OnArcherSpawn id
//     [+0x1fc] OnDruidSpawn id
//     [+0x200] OnEngineerSpawn id
//     [+0x204] OnAssassinSpawn id
//     [+0x208] OnReinforcementsZero id
//     [+0x20c] OnDepleted id
//
// The event IDs are resolved by calling 0x0084ddc3 (resolve_event_by_name)
// with string keys pushed via 0x0067e6d8. Names hardcoded in .rdata:
//   0x9cd4b4 = "OnSpawn"
//   0x9cd4bc = "OnReinforcementsZero"
//   0x9cd4d4 = "OnDepleted"
//   0x9cd4e0 = "OnWarriorSpawn"
//   0x9cd4f0 = "OnArcherSpawn"
//   0x9cd500 = "OnDruidSpawn"
//   0x9cd510 = "OnEngineerSpawn"
//   0x9cd520 = "OnAssassinSpawn"

struct MgSpawnerEntity {
    uint8_t  _pad_000[0x150];
    void*    spawn_data_list;   // +0x150
    uint8_t  _pad_154[0x054];  // +0x154..+0x1a7
    // spawn event linked list head at +0x1a8
    uint8_t  _pad_1a8[0x048];
    uint32_t event_id_OnSpawn;               // +0x1f0
    uint32_t event_id_OnWarriorSpawn;        // +0x1f4
    uint32_t event_id_OnArcherSpawn;         // +0x1f8
    uint32_t event_id_OnDruidSpawn;          // +0x1fc
    uint32_t event_id_OnEngineerSpawn;       // +0x200
    uint32_t event_id_OnAssassinSpawn;       // +0x204
    uint32_t event_id_OnReinforcementsZero;  // +0x208
    uint32_t event_id_OnDepleted;            // +0x20c
};

// Dependency: resolve_event_by_name — game: 0x0084ddc3
typedef uint32_t (__thiscall* PFN_MgEvent_ResolveByName)(void* spawner,
                                                           const void* event_name_str);
extern PFN_MgEvent_ResolveByName g_pfn_MgEvent_ResolveByName;

// FUN_008238b3 — resolve all spawn event IDs and build the spawn dispatch list.
// Returns 1 on success (game: MOV AL,1 at end), 0 on early-out.
int MgSpawn_InitEventIds(MgSpawnerEntity* spawner, const void* spawn_def, int flags);

#ifdef __cplusplus
} // extern "C"
#endif
