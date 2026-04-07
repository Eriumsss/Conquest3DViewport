// MgScriptEvents.cpp
// Reconstructed from ASM dumps (EC_math / EC_time categories, mislabeled).
//
// All ex* functions follow this exact pattern:
//   1. SUB ESP, 0x24              (allocate 36-byte arg block, cleared to 0)
//   2. Zero the block with AND/ADD loop
//   3. Pack argument(s) into the block
//   4. PUSH num_args
//   5. PUSH &arg_block
//   6. MOV EAX, [entity]
//   7. PUSH event_name_str_ptr
//   8. PUSH [EAX + 0x64]          (entity->entity_id)
//   9. CALL 0x00725c3b
//
// The arg block zero-clearing loop:
//   XOR ECX,ECX / INC ECX  -- count = 2 iterations (so 2 × 16 = 32 bytes cleared)
//   LAB: AND [EAX],0 / ADD EAX,16 / DEC ECX / JNS LAB
//   This clears 2 × 16-byte chunks = 32 bytes starting from [EBP-0x24].
//
// Arg block layout for type==3 (double):
//   +0x00: uint32_t type = 3
//   +0x04..0x07: pad
//   +0x08: double value   (8 bytes)
//
// For two-arg dispatch (exSetCrosshairPos, exSetCrosshairColor, exSetClassInfo):
//   [EBP-0x24]: first arg  (type, pad, value)
//   [EBP-0x14]: second arg (type, pad, value)
//   num_args = 2

#include "MgScriptEvents.h"
#include <string.h>

PFN_MgEntity_DispatchScriptEvent g_pfn_MgEntity_DispatchScriptEvent = NULL;

// Game binary string pointer constants for event names:
static const char* k_evname_exSetSpawnTimer   = "exSetSpawnTimer";   // 0x9ebe34
static const char* k_evname_exSetTimer        = "exSetTimer";        // 0x9ebbe0
static const char* k_evname_exShowTimer       = "exShowTimer";       // 0x9eb3c8
static const char* k_evname_exSetCrosshairStyle = "exSetCrosshairStyle"; // 0x9ebb90
static const char* k_evname_exSetCrosshairColor = "exSetCrosshairColor"; // 0x9ebb7c
static const char* k_evname_exSetCrosshairPos   = "exSetCrosshairPos";   // 0x9ebb68
static const char* k_evname_exShowCrosshair     = "exShowCrosshair";     // 0x9ebb48
static const char* k_evname_exHideCrosshair     = "exHideCrosshair";     // 0x9ebb58
static const char* k_evname_exSetClassInfo      = "exSetClassInfo";      // 0x9ebbfc

// Helper: dispatch a single-double arg event
static void dispatch_double(MgEntity* entity, const char* evname,
                             int type_tag, double value)
{
    if (!g_pfn_MgEntity_DispatchScriptEvent || !entity) return;

    // Mirrors the 36-byte zeroed stack frame, then first 16-byte slot filled
    struct { uint32_t type; uint32_t p0; double val; } arg;
    memset(&arg, 0, sizeof(arg));
    arg.type = (uint32_t)type_tag;
    arg.val  = value;

    g_pfn_MgEntity_DispatchScriptEvent(entity->entity_id, evname, &arg, 1);
}

// Helper: dispatch two-double arg event
static void dispatch_double2(MgEntity* entity, const char* evname,
                               int type0, double val0,
                               int type1, double val1)
{
    if (!g_pfn_MgEntity_DispatchScriptEvent || !entity) return;

    // Two consecutive 16-byte slots: [type0,pad,val0][type1,pad,val1]
    struct {
        uint32_t type0; uint32_t p0; double val0;
        uint32_t type1; uint32_t p1; double val1;
    } args;
    memset(&args, 0, sizeof(args));
    args.type0 = (uint32_t)type0;
    args.val0  = val0;
    args.type1 = (uint32_t)type1;
    args.val1  = val1;

    g_pfn_MgEntity_DispatchScriptEvent(entity->entity_id, evname, &args, 2);
}

// ---------------------------------------------------------------------------
// FUN_0093a461 — exSetSpawnTimer
// CVTSI2SD XMM0, [EBP+0xC]  (int frame → double)
// MOV [EBP-0x10], 0x3         type = 3
// MOVSD [EBP-0x8], XMM0
// PUSH 1 / PUSH &arg / PUSH 0x9ebe34 / PUSH [entity+0x64]
// ---------------------------------------------------------------------------
void exSetSpawnTimer(MgEntity* entity, int frame)
{
    dispatch_double(entity, k_evname_exSetSpawnTimer, 3, (double)frame);
}

// ---------------------------------------------------------------------------
// FUN_0093ae75 — exSetTimer
// MOV [EBP-0x14], 0x4   type = 4
// No value written to val slot (EAX passed in but not used as double here).
// num_args = 1
// Note: type 4 with no explicit value — the arg block tail is zeroed,
//       so effectively dispatches with type=4, value=0.0.
// ---------------------------------------------------------------------------
void exSetTimer(MgEntity* entity)
{
    dispatch_double(entity, k_evname_exSetTimer, 4, 0.0);
}

// ---------------------------------------------------------------------------
// FUN_0093ae9f — exShowTimer
// MOVZX EAX, AL          (bool → uint8_t)
// CVTSI2SD XMM0, EAX     (→ double)
// MOV [EBP-0x14], 0x3    type = 3
// MOVSD [EBP-0xC], XMM0
// num_args = 1
// ---------------------------------------------------------------------------
void exShowTimer(MgEntity* entity, int show)
{
    uint8_t v = (uint8_t)(show ? 1 : 0);
    dispatch_double(entity, k_evname_exShowTimer, 3, (double)v);
}

// ---------------------------------------------------------------------------
// FUN_0093afaf — exSetCrosshairStyle
// MOVZX EAX, AL
// CVTSI2SD XMM0, EAX
// MOV [ESP+0x10], 0x3    type = 3
// MOVSD [ESP+0x18], XMM0
// num_args = 1
// ---------------------------------------------------------------------------
void exSetCrosshairStyle(MgEntity* entity, int style)
{
    uint8_t v = (uint8_t)(style ? 1 : 0);
    dispatch_double(entity, k_evname_exSetCrosshairStyle, 3, (double)v);
}

// ---------------------------------------------------------------------------
// FUN_0093afea — exSetCrosshairColor
// Two double args, both type 3:
//
// Reconstruction of the packing (from ASM):
//   MOVZX EDX, [EBP+0x14]       → A (4th byte param)
//   MOV   CH,  [EBP+0xC]        → G into CH
//   MOV   CL,  [EBP+0x10]       → B into CL
//   SHL   ECX, 0x8              → ECX = G<<16 | B<<8
//   OR    ECX, EDX              → ECX = G<<16 | B<<8 | A
//   CVTSI2SD XMM0, ECX          → arg0.val = (double)(G<<16|B<<8|A)
//   IDIV  ECX (255)             → EAX = quotient = packed / 255
//   MOVSD [EBP-0x1C], XMM0      → arg0.val stored
//   IMUL  EAX,EAX,0x64          → EAX = (packed/255)*100
//   CVTSI2SD XMM0, EAX          → arg1.val
//   MOVSD [EBP-0x0C], XMM0      → arg1.val stored
//
// Note: R byte is loaded to CH via [EBP+0xC] but R appears to be at 0xC.
// Parameter order from prototype: (R=0xC, G=0x10, B=0x14, A=0x18) - 4 byte params
// Re-reading ASM: MOV CH,[EBP+0xC], MOV CL,[EBP+0x10], MOVZX EDX,[EBP+0x14],
//   MOVZX EAX,[EBP+0x18] → A at 0x18.
// So: CH=R, CL=G, EDX=B, EAX=A
//   ECX = R<<8 | G, SHL ECX,8 → R<<16 | G<<8, OR ECX,EDX → R<<16|G<<8|B
//   then OR to get A... wait: OR ECX,EDX where EDX=B.
//   Hmm let me re-read more carefully:
//     MOV CH, [EBP+0xC]      → bits 15:8 = param1
//     MOV CL, [EBP+0x10]     → bits 7:0  = param2
//     SHL ECX, 0x8           → now bits 23:16=param1, 15:8=param2
//     OR  ECX, EDX           → bits 7:0 = EDX = MOVZX from [EBP+0x14]
//   So packed_rgb = param1<<16 | param2<<8 | param3(B)
//   Then: IMUL EAX, quotient, 0x64 where EAX = packed/255, so arg1 = (packed/255)*100
// ---------------------------------------------------------------------------
void exSetCrosshairColor(MgEntity* entity,
                          uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    if (!g_pfn_MgEntity_DispatchScriptEvent || !entity) return;

    // Reconstruct the exact packing from FUN_0093afea:
    // CH=[EBP+0xC]=R, CL=[EBP+0x10]=G, EDX=[EBP+0x14]=B, EAX=[EBP+0x18]=A
    // packed = R<<16 | G<<8 | B
    int packed = ((int)R << 16) | ((int)G << 8) | (int)B;

    // arg0: (double)packed packed color normalized by division
    double arg0_val = (double)packed;

    // arg1: IMUL EAX,(packed/255),100 — integer quotient times 100
    int quotient = packed / 255;
    double arg1_val = (double)(quotient * 100);

    dispatch_double2(entity, k_evname_exSetCrosshairColor,
                     3, arg0_val,
                     3, arg1_val);
}

// ---------------------------------------------------------------------------
// FUN_0093b054 — exSetCrosshairPos
// MOVSS XMM0,[EBP+0xC]  (float x)
// MOV [EBP-0x24],3 and [EBP-0x14],3  (type=3 for both args)
// CVTPS2PD XMM0,XMM0 (float → double)
// MOVSD [EBP-0x1C], XMM0   → arg0.val = (double)x
// MOVSS XMM0,[EBP+0x10]  (float y)
// CVTPS2PD XMM0,XMM0
// MOVSD [EBP-0xC], XMM0    → arg1.val = (double)y
// num_args = 2
// ---------------------------------------------------------------------------
void exSetCrosshairPos(MgEntity* entity, float x, float y)
{
    dispatch_double2(entity, k_evname_exSetCrosshairPos,
                     3, (double)x,
                     3, (double)y);
}

// ---------------------------------------------------------------------------
// FUN_0093b0a6 — exShowCrosshair / exHideCrosshair
// Selects event name based on `show` bool (bool param in [ESP+0xC]):
//   show != 0 → EAX = 0x9ebb48 ("exShowCrosshair")
//   show == 0 → EAX = 0x9ebb58 ("exHideCrosshair")
// PUSH 0 / PUSH 0 / PUSH EAX(event_name) / PUSH [entity+0x64]
// CALL 0x00725c3b
// num_args = 0, arg_block = NULL (pushed as two 0s)
// ---------------------------------------------------------------------------
void exShowHideCrosshair(MgEntity* entity, int show)
{
    if (!g_pfn_MgEntity_DispatchScriptEvent || !entity) return;
    const char* evname = show ? k_evname_exShowCrosshair : k_evname_exHideCrosshair;
    // num_args = 0, arg_block = NULL (as in original: PUSH 0 / PUSH 0)
    g_pfn_MgEntity_DispatchScriptEvent(entity->entity_id, evname, NULL, 0);
}

// ---------------------------------------------------------------------------
// FUN_0093adf9 — exSetClassInfo
// 5-slot arg block, slots 0 and 1 used (type=5 each):
//   arg0: type=5 at [EBP-0x24], class_id (uint32_t) at [EBP-0x1C] (ECX param)
//   arg1: type=5 at [EBP-0x14], team_id  (int)      at [EBP-0x0C] (EAX=[EBP+0x10])
// num_args = 2
// ---------------------------------------------------------------------------
void exSetClassInfo(MgEntity* entity, uint32_t class_id, int team_id)
{
    if (!g_pfn_MgEntity_DispatchScriptEvent || !entity) return;

    // Two 16-byte slots, type=5
    struct {
        uint32_t type0; uint32_t p0; uint32_t class_id; uint32_t pad0;
        uint32_t type1; uint32_t p1; int      team_id;  uint32_t pad1;
    } args;
    memset(&args, 0, sizeof(args));
    args.type0    = 5;
    args.class_id = class_id;
    args.type1    = 5;
    args.team_id  = team_id;

    g_pfn_MgEntity_DispatchScriptEvent(entity->entity_id,
                                        k_evname_exSetClassInfo, &args, 2);
}
