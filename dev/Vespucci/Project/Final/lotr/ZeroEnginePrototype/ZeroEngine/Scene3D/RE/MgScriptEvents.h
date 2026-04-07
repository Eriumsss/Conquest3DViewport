// MgScriptEvents.h
// Reconstructed script-callable entity event dispatch functions.
//
// Source functions (EC_math / EC_time — mislabeled):
//   FUN_0093a461   exSetSpawnTimer(entity, int frame)
//   FUN_0093ae75   exSetTimer(entity, float duration) [arg block only; no time param in regs]
//   FUN_0093ae9f   exShowTimer(entity, bool show)
//   FUN_0093afaf   exSetCrosshairStyle(entity, bool style)
//   FUN_0093afea   exSetCrosshairColor(entity, R,G,B,A bytes)
//   FUN_0093b054   exSetCrosshairPos(entity, float x, float y)
//   FUN_0093b0a6   exShowCrosshair / exHideCrosshair (entity, bool show)
//   FUN_0093adf9   exSetClassInfo(entity, string, int)
//
// All share the same dispatch pattern:
//   1. Build an arg block on stack (type tag + value(s))
//   2. CALL 0x00725c3b (MgEntity_DispatchScriptEvent) with:
//        entity->entity_id, event_name_ptr, &arg_block, num_args
//
// Event arg type codes observed:
//   3 = double (8-byte IEEE 754 double)
//   4 = double (same layout, different semantic — used by exSetTimer)
//   5 = two-value block (used by exSetClassInfo with string+int)
//
// Crosshair color encoding (from FUN_0093afea):
//   arg0 = (double)((R<<8 | G<<16 | B | A) / 255.0)   [packed RGBA normalized]
//   arg1 = (double)(alpha_int * 100 / 255)              [alpha as 0-100 percent]

#pragma once
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Dependency: MgEntity_DispatchScriptEvent
// Game address: 0x00725c3b
// __cdecl void dispatch(uint32_t entity_id, const char* event_name,
//                       void* arg_block, int num_args)
// ---------------------------------------------------------------------------
typedef void (__cdecl* PFN_MgEntity_DispatchScriptEvent)(
        uint32_t entity_id,
        const char* event_name,
        void* arg_block,
        int   num_args);

extern PFN_MgEntity_DispatchScriptEvent g_pfn_MgEntity_DispatchScriptEvent;

// ---------------------------------------------------------------------------
// FUN_0093a461 — exSetSpawnTimer
// Sets spawn timer to `frame` (converted to double).
// Event name ptr: 0x9ebe34 ("exSetSpawnTimer")
// arg type: 3 (double), value = (double)frame
// ---------------------------------------------------------------------------
void exSetSpawnTimer(MgEntity* entity, int frame);

// ---------------------------------------------------------------------------
// FUN_0093ae75 — exSetTimer
// Sets countdown timer duration.
// Event name ptr: 0x9ebbe0 ("exSetTimer")
// arg type: 4, value packed in arg block from EAX reg
// ---------------------------------------------------------------------------
void exSetTimer(MgEntity* entity);

// ---------------------------------------------------------------------------
// FUN_0093ae9f — exShowTimer
// Shows or hides the HUD timer display.
// Event name ptr: 0x9eb3c8 ("exShowTimer")
// arg type: 3, value = (double)(uint8_t)show
// ---------------------------------------------------------------------------
void exShowTimer(MgEntity* entity, int show);

// ---------------------------------------------------------------------------
// FUN_0093afaf — exSetCrosshairStyle
// Sets crosshair visual style (bool flag).
// Event name ptr: 0x9ebb90 ("exSetCrosshairStyle")
// arg type: 3, value = (double)(uint8_t)style
// ---------------------------------------------------------------------------
void exSetCrosshairStyle(MgEntity* entity, int style);

// ---------------------------------------------------------------------------
// FUN_0093afea — exSetCrosshairColor
// Sets crosshair RGBA color.
// Event name ptr: 0x9ebb7c ("exSetCrosshairColor")
// 2 args (type 3 each):
//   arg0.value = (double)packed_rgb_int    where packed = (R<<8|G)<<8|B | A packed
//   arg1.value = (double)(alpha * 100 / 255)
// ---------------------------------------------------------------------------
void exSetCrosshairColor(MgEntity* entity,
                          uint8_t R, uint8_t G, uint8_t B, uint8_t A);

// ---------------------------------------------------------------------------
// FUN_0093b054 — exSetCrosshairPos
// Sets crosshair screen position (x, y in screen space).
// Event name ptr: 0x9ebb68 ("exSetCrosshairPos")
// 2 args (type 3 each): arg0.value=(double)x, arg1.value=(double)y
// ---------------------------------------------------------------------------
void exSetCrosshairPos(MgEntity* entity, float x, float y);

// ---------------------------------------------------------------------------
// FUN_0093b0a6 — exShowCrosshair / exHideCrosshair
// Dispatches either "exShowCrosshair" (show==true) or "exHideCrosshair" (show==false).
// Event name ptrs: 0x9ebb48 (show), 0x9ebb58 (hide)
// 0 args (num_args=0, but passes entity->entity_id at [entity+0x64])
// ---------------------------------------------------------------------------
void exShowHideCrosshair(MgEntity* entity, int show);

// ---------------------------------------------------------------------------
// FUN_0093adf9 — exSetClassInfo
// Sets class/team information on an entity.
// Event name ptr: 0x9ebbfc ("exSetClassInfo")
// 2 args (both type 5):
//   arg0.value0 = class_id (string/int, from ECX at call)
//   arg1.value  = team_id  (int)
// ---------------------------------------------------------------------------
void exSetClassInfo(MgEntity* entity, uint32_t class_id, int team_id);

#ifdef __cplusplus
} // extern "C"
#endif
