// MgMount.cpp
// Reconstructed from FUN_007c8c66 (117 addr-count, EC_math category)
//
// The function reads CloseDismountDistance and FarDismountDistance from the
// engine's named parameter system, squares each value, and stores the result
// in the mount object. The squaring avoids sqrt in distance checks at runtime.

#include "MgMount.h"

PFN_MgParam_GetFloat  g_pfn_MgParam_GetFloat  = NULL;
PFN_MgMount_Validate  g_pfn_MgMount_Validate  = NULL;

// Defaults from game .rdata (exact values unknown; set reasonable defaults)
float g_MgMount_DefaultCloseDismountDist = 3.0f;  // *(float*)0x9c33f0
float g_MgMount_DefaultFarDismountDist   = 6.0f;  // *(float*)0x9c362c

// Named param string constants (game .rdata addresses):
//   0x9dc580 = "CloseDismountDistance"
//   0x9dc598 = "FarDismountDistance"
static const char* k_param_close = "CloseDismountDistance"; // 0x9dc580
static const char* k_param_far   = "FarDismountDistance";   // 0x9dc598

// ---------------------------------------------------------------------------
// FUN_007c8c66 — MgMount_LoadDismountDistances
//
// Exact ASM reconstruction:
//   PUSH ESI / MOV ESI,[ESP+0x8] (source_param)
//   PUSH EDI / MOV EDI,ECX       (this = mount)
//   PUSH ESI
//   CALL 0x007c8c08               validate_mount_obj(mount, source_param)
//   POP ECX
//   TEST AL,AL / JZ return_false
//   FLD float[0x9c33f0]          ; default close dist
//   PUSH ECX / FSTP [ESP]        ; push as float arg
//   PUSH 0x9dc580                ; "CloseDismountDistance"
//   CALL 0x008a66ba              ; get_float_param(default, name) → XMM0
//   FLD float[0x9c362c]          ; (another constant — actually same pattern for far)
//   CVTSS2SD XMM0,XMM0
//   MOVAPD XMM1,XMM0 / MULSD XMM1,XMM0  ; square
//   CVTSD2SS XMM0,XMM1
//   MOVSS [EDI+0xb4], XMM0       ; store close_sq
//   ; repeat with FarDismountDistance
//   PUSH 0x9dc598 / CALL get_float_param → XMM0
//   square → MOVSS [EDI+0xb8], XMM0
//   MOV AL,1
// ---------------------------------------------------------------------------
int MgMount_LoadDismountDistances(MgMountable* mount, void* source_param)
{
    if (!mount) return 0;

    // Validate mount object (game: CALL 0x007c8c08)
    if (g_pfn_MgMount_Validate) {
        if (!g_pfn_MgMount_Validate(mount, source_param))
            return 0;
    }

    // Read CloseDismountDistance
    float close_dist;
    if (g_pfn_MgParam_GetFloat) {
        close_dist = g_pfn_MgParam_GetFloat(g_MgMount_DefaultCloseDismountDist,
                                             k_param_close);
    } else {
        close_dist = g_MgMount_DefaultCloseDismountDist;
    }

    // Square it (game: CVTSS2SD, MULSD, CVTSD2SS — double-precision squaring)
    double close_d = (double)close_dist;
    mount->close_dismount_dist_sq = (float)(close_d * close_d);

    // Read FarDismountDistance
    float far_dist;
    if (g_pfn_MgParam_GetFloat) {
        far_dist = g_pfn_MgParam_GetFloat(g_MgMount_DefaultFarDismountDist,
                                           k_param_far);
    } else {
        far_dist = g_MgMount_DefaultFarDismountDist;
    }

    double far_d = (double)far_dist;
    mount->far_dismount_dist_sq = (float)(far_d * far_d);

    return 1;
}
