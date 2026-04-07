// MgMount.h
// Reconstructed mount/dismount distance parameter loader.
//
// Source function:
//   FUN_007c8c66 — reads CloseDismountDistance and FarDismountDistance
//                  from named param table, squares them for distance² comparisons.
//
// ASM pattern:
//   PUSH ESI (param)
//   PUSH EDI = ECX (this/mount_obj)
//   CALL 0x007c8c08 (validate_mount_obj — returns bool)
//   if !valid → return 0
//   FLD float[0x9c33f0] ("CloseDismountDistance" default = some float)
//   PUSH ECX / FSTP float[ESP]
//   PUSH 0x9dc580          ("CloseDismountDistance" string)
//   CALL 0x008a66ba         (get_float_param_fld — reads named float param, returns in XMM0)
//   FLD float[0x9c362c]    (another constant)
//   CVTSS2SD XMM0,XMM0
//   MOVAPD XMM1,XMM0 / MULSD XMM1,XMM0  ← square
//   CVTSD2SS XMM0,XMM1
//   MOVSS [EDI+0xb4], XMM0  ← store CloseDismountDistSq at +0xb4
//   ... same for FarDismountDistance → stored at [EDI+0xb8]
//   MOV AL, 1 → return true
//
// Object layout (MgMountable):
//   +0xb4: float close_dismount_dist_sq
//   +0xb8: float far_dismount_dist_sq

#pragma once
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Named float param getter (game: 0x008a66ba)
// Reads a named float param from a global param table.
// The FLD / PUSH / PUSH pattern passes the default value on the FPU stack.
// Returns the param value in XMM0 (here as return float).
// ---------------------------------------------------------------------------
typedef float (__cdecl* PFN_MgParam_GetFloat)(float default_val,
                                               const char* param_name);
extern PFN_MgParam_GetFloat g_pfn_MgParam_GetFloat;

// Mount object validator (game: 0x007c8c08)
typedef int (__thiscall* PFN_MgMount_Validate)(void* mount, void* param);
extern PFN_MgMount_Validate g_pfn_MgMount_Validate;

// Default dismount distances (game .rdata values):
extern float g_MgMount_DefaultCloseDismountDist; // *(float*)0x9c33f0
extern float g_MgMount_DefaultFarDismountDist;   // *(float*)0x9c362c

// ---------------------------------------------------------------------------
// MgMountable — partial layout of a mountable entity object.
// Only the fields touched by FUN_007c8c66 are defined.
// ---------------------------------------------------------------------------
struct MgMountable {
    uint8_t _pad[0xb4];
    float   close_dismount_dist_sq; // +0xb4
    float   far_dismount_dist_sq;   // +0xb8
};

// ---------------------------------------------------------------------------
// FUN_007c8c66 — load dismount distances from named params, store squared.
// Returns 1 on success, 0 if mount object fails validation.
// ---------------------------------------------------------------------------
int MgMount_LoadDismountDistances(MgMountable* mount, void* source_param);

#ifdef __cplusplus
} // extern "C"
#endif
