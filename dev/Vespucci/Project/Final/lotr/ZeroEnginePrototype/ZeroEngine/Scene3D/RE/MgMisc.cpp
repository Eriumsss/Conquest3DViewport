// MgMisc.cpp
// Reconstructed from:
//   FUN_005c7e40 (UTC time delta)
//   FUN_006bb641 (GFx text format reset)
//   FUN_007cc118 (entity by class name)
//   FUN_0079c210 (stance/action filter validator)
//   FUN_008238b3 (spawn event ID resolver)

#include "MgMisc.h"
#include <string.h>
#include <time.h>

PFN_MgEvent_ResolveByName g_pfn_MgEvent_ResolveByName = NULL;

// ---------------------------------------------------------------------------
// FUN_005c7e40 — MgTime_ComputeUTCOffset
//
// ASM uses __time64/__gmtime64/__mktime64 to compute the local-UTC offset.
// Stores result into obj->utc_offset_seconds, adjusting by reference_time.
// If use_dst: add 0xe10 (3600 seconds) before storing.
//
// C equivalent of the computation:
//   t = time(NULL)
//   gmt = gmtime(&t)   → local copy
//   utc_t = mktime(gmt)
//   offset = (int64_t)difftime(utc_t, local_ref) + obj->reference_time
//   if use_dst: offset += 3600
//   store via 0x005ab350 (int64 setter at [EBX+0x94])
// ---------------------------------------------------------------------------
void MgTime_ComputeUTCOffset(MgTimeObject* time_obj, int use_dst)
{
    if (!time_obj) return;

    __time64_t now = _time64(NULL);
    struct tm* gmt_tm = _gmtime64(&now);
    if (!gmt_tm) return;

    // Make a local copy (gmtime returns static buffer)
    struct tm gmt_copy = *gmt_tm;

    // Convert UTC struct to time_t to compute delta
    __time64_t utc_t = _mktime64(&gmt_copy);
    double diff = _difftime64(utc_t, now);

    // Subtract reference_time (from obj->reference_time stored as 64-bit int)
    int64_t ref = time_obj->reference_time;
    int64_t offset = (int64_t)diff - ref;

    // DST adjustment: if use_dst flag, add 3600 seconds (0xe10)
    if (use_dst)
        offset += 0xe10;

    // Store via int64 setter (game: 0x005ab350 writes to [EBX+0x94])
    time_obj->utc_offset_seconds = offset;
}

// ---------------------------------------------------------------------------
// FUN_006bb641 — MgGfx_ResetTextFormat
//
// Resets a GFx TextFormat object to defaults (with "Times New Roman" font).
//
// ASM:
//   OR  word[ESI+0x58], 0x1        ; format_flags |= dirty
//   MOV EDI, 0xff000000
//   AND dword[ESI+0x1c], EDI       ; color_flags &= 0xFF000000 (keep alpha channel only)
//   PUSH 0x9f8d44 / OR EAX,-1 / CALL 0x006bb570  ; set font name = "Times New Roman"
//   FLD [0x9c1cc0] / PUSH ECX / FSTP [ESP]
//   CALL 0x0047c9d3                ; set font size to some default float
//   PUSH 0 / CALL 0x006bb49b       ; set some flag0 to false
//   PUSH 0 / CALL 0x006bb460       ; set some flag1 to false
//   MOV AX,[ESI+0x58]
//   OR dword[ESI+0x1c], EDI        ; restore alpha = 0xFF000000
//   AND byte[ESI+0x24], 0xf3       ; clear bits 2,3 of misc_flags
//   AND word[ESI+0x20], 0x0        ; clear style_flags2
//   AND AX, 0xfbfd                 ; clear format bits
//   OR  AX, 0xc0                   ; set format bits
//   PUSH 0 / LEA EDI,[ESI+0x10]
//   MOV word[ESI+0x58], AX
//   CALL 0x006fc4af                ; font update
//   AND word[ESI+0x58], 0xfeff     ; clear dirty flag bit 8
// ---------------------------------------------------------------------------
void MgGfx_ResetTextFormat(MgGfxTextFormat* fmt)
{
    if (!fmt) return;

    // Mark dirty
    fmt->format_flags |= 0x1;

    // Clear color to opaque black (keep alpha channel mask)
    fmt->color_flags &= 0xFF000000u;

    // (font name / size / flags calls omitted — require vtable dispatch)
    // Restore full alpha
    fmt->color_flags |= 0xFF000000u;

    // Clear style bits
    fmt->misc_flags    &= 0xF3;
    fmt->style_flags2   = 0;

    // Update format flags: clear bits, set 0xC0
    fmt->format_flags = (uint16_t)((fmt->format_flags & 0xFBFD) | 0xC0);

    // Clear dirty bit 8
    fmt->format_flags &= 0xFEFF;
}

// ---------------------------------------------------------------------------
// FUN_007cc118 — MgEntity_FindByClassName
//
// Dependency function pointers (game addresses):
//   0x008a69ac = MgTypeSystem_GetClassName(type_handle, class_key) → name string
//   0x007c6a3b = MgEntityManager_FindByClassNameStr(&name_str) → entity ptr
// ---------------------------------------------------------------------------
typedef void (__cdecl* PFN_GetClassName)(void* type_handle,
                                          const void* class_key,
                                          MgString* out_name);
typedef MgEntity* (__cdecl* PFN_FindByName)(const MgString* name);
typedef int  (__thiscall* PFN_Entity_Validate)(MgEntity* e, void* param);
typedef void (__thiscall* PFN_Entity_Release)(MgEntity* e, int flag);

// Hardcoded type handle constants from ASM:
//   0xe560e0 = engine type system handle
//   0x9db598 = "InstanceClassName" string descriptor

MgEntity* MgEntity_FindByClassName(const char* class_name, void* search_param)
{
    // In a reconstruction without game binary, do a stub implementation.
    // In a hooked environment, call the original functions via stored ptrs.
    (void)class_name;
    (void)search_param;
    // TODO: hook g_pfn_GetClassName / g_pfn_FindByName when game binary loaded
    return NULL;
}

// ---------------------------------------------------------------------------
// FUN_0079c210 — MgAnimFilter_HasStancesOrActions
//
// ASM:
//   PUSH 0xe560e0 / PUSH 0x9c7eac / CALL 0x008a6948 (IsTypeOf check)
//   TEST AL,AL / JZ return_false
//   PUSH 0x9e45c8 ("StancesFilter") / PUSH &[EDI+0x4] / PUSH [ESP+0x14]
//   CALL 0x00428cf7                 (read array field → stores ptr+count at [EDI+0x4..0xC])
//   repeat for ActionsFilter at [EDI+0x10..0x18]
//   CMP [EDI+0xC],0  (stances_count)
//   JNZ return_true
//   CMP [EDI+0x18],0 (actions_count)
//   JZ  return_false
//   return_true: MOV AL,1
// ---------------------------------------------------------------------------
int MgAnimFilter_HasStancesOrActions(const MgAnimFilterEntry* filter,
                                      const void* type_check_arg)
{
    if (!filter) return 0;
    (void)type_check_arg; // type check stub — assume passes in reconstruction

    // The read_array_field calls (0x00428cf7) populate the ptr+count fields.
    // In our reconstruction, we trust that the fields are already populated
    // by the caller (as they would be after loading from data).
    return (filter->stances_count != 0 || filter->actions_count != 0) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// FUN_008238b3 — MgSpawn_InitEventIds
//
// Resolves spawn event names to IDs and stores them in the spawner entity.
// Also iterates the spawn_data_list to dispatch initial spawn events.
//
// Name constants (game .rdata):
//   0x9cd4b4 = "OnSpawn", 0x9cd4bc = "OnReinforcementsZero", 0x9cd4d4 = "OnDepleted"
//   0x9cd4e0 = "OnWarriorSpawn",  0x9cd4f0 = "OnArcherSpawn"
//   0x9cd500 = "OnDruidSpawn",    0x9cd510 = "OnEngineerSpawn"
//   0x9cd520 = "OnAssassinSpawn"
// ---------------------------------------------------------------------------
static const char* k_OnSpawn               = "OnSpawn";
static const char* k_OnReinforcementsZero  = "OnReinforcementsZero";
static const char* k_OnDepleted            = "OnDepleted";
static const char* k_OnWarriorSpawn        = "OnWarriorSpawn";
static const char* k_OnArcherSpawn         = "OnArcherSpawn";
static const char* k_OnDruidSpawn          = "OnDruidSpawn";
static const char* k_OnEngineerSpawn       = "OnEngineerSpawn";
static const char* k_OnAssassinSpawn       = "OnAssassinSpawn";

// Validate-spawn-def: game 0x00854224
typedef int (__cdecl* PFN_ValidateSpawnDef)(const void* spawn_def, int flags);
// Get spawn def list size: game 0x0041b818
typedef void* (__thiscall* PFN_GetSpawnList)(void* spawn_data);
// Dispatch one spawn entry: game 0x00822e59
typedef void (__thiscall* PFN_DispatchSpawnEntry)(MgSpawnerEntity* spawner, void* entry_ptr);

int MgSpawn_InitEventIds(MgSpawnerEntity* spawner, const void* spawn_def, int flags)
{
    if (!spawner) return 0;

    // Resolve event names to IDs via engine event system
    // (game: constructs MgString per name, calls 0x0084ddc3)
    if (g_pfn_MgEvent_ResolveByName) {
        spawner->event_id_OnSpawn              = g_pfn_MgEvent_ResolveByName(spawner, k_OnSpawn);
        spawner->event_id_OnReinforcementsZero = g_pfn_MgEvent_ResolveByName(spawner, k_OnReinforcementsZero);
        spawner->event_id_OnDepleted           = g_pfn_MgEvent_ResolveByName(spawner, k_OnDepleted);
        spawner->event_id_OnWarriorSpawn       = g_pfn_MgEvent_ResolveByName(spawner, k_OnWarriorSpawn);
        spawner->event_id_OnArcherSpawn        = g_pfn_MgEvent_ResolveByName(spawner, k_OnArcherSpawn);
        spawner->event_id_OnDruidSpawn         = g_pfn_MgEvent_ResolveByName(spawner, k_OnDruidSpawn);
        spawner->event_id_OnEngineerSpawn      = g_pfn_MgEvent_ResolveByName(spawner, k_OnEngineerSpawn);
        spawner->event_id_OnAssassinSpawn      = g_pfn_MgEvent_ResolveByName(spawner, k_OnAssassinSpawn);
    }

    // Note: the first half of FUN_008238b3 (before the event ID resolution)
    // iterates spawn_data_list and dispatches initial spawn entries.
    // That path calls 0x0041b818 (get_spawn_list_info) and 0x00822e59 (dispatch_entry).
    // Those are stubs here pending further analysis of their sub-functions.
    (void)spawn_def;
    (void)flags;

    return 1;
}
