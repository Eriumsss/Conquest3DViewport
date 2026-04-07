// MgAnimBlend.h
// Reconstructed animation blend parameter accessor functions.
//
// Source functions (EC_time / EC_math categories — mislabeled by heuristic):
//   FUN_0041feef  FadeInTime  (float, default 0.0f)
//   FUN_0041ff4b  FadeOutTime (float, default 0.0f)
//   FUN_0041ffa7  HoldTime    (float, default 0.0f)
//   FUN_00429020  OneTimeOnly (bool,  default true)
//   FUN_0043fd91  MinFrame    (int,   default 0)
//
// All share the same lookup pattern:
//   1. If blend_entry == NULL  → return default
//   2. If blend_entry->blend_data == NULL → return default
//   3. Construct string keys for the param name, call find_anim_param()
//   4. If found → return value at (blend_entry + found_byte_offset + 0x10)
//   5. Else      → return default
//
// The object pointer is passed in the EDI register (non-standard calling
// convention used throughout the animation subsystem).

#pragma once
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Dependency stubs — implemented by the engine's param table subsystem.
// Declare here so callers can hook/override them.
// ---------------------------------------------------------------------------

// Constructs an engine string from a compile-time string constant descriptor.
// ECX = const MgStringConst*, ESI = MgString* output destination.
// Game address: 0x0067e6d8
typedef void (__thiscall* PFN_MgString_CopyConstruct)(const void* src_const, MgString* out);
extern PFN_MgString_CopyConstruct g_pfn_MgString_CopyConstruct;

// Looks up a named parameter in the blend data table.
// Returns byte offset from blend_entry_base where the value resides (+0x10),
// or -1 if not found.
// Game address: 0x007e866b
typedef int (__cdecl* PFN_find_anim_param)(MgString* key1, void* blend_data, MgString* key2);
extern PFN_find_anim_param g_pfn_find_anim_param;

// ---------------------------------------------------------------------------
// Default constants (game binary addresses for reference):
//   FadeInTime  default: *(float*)0x009c1f9c
//   FadeOutTime default: *(float*)0x009c5284
//   HoldTime    default: *(float*)0x00a41b8c
// We store them here so the viewer can patch them.
// ---------------------------------------------------------------------------
extern float g_MgAnimBlend_DefaultFadeInTime;    // = 0.0f
extern float g_MgAnimBlend_DefaultFadeOutTime;   // = 0.0f
extern float g_MgAnimBlend_DefaultHoldTime;      // = 0.0f

// ---------------------------------------------------------------------------
// Public API
// All functions take the blend entry pointer (EDI in original).
// NULL entry is handled gracefully — returns the default value.
// ---------------------------------------------------------------------------

// FUN_0041feef: reads "FadeInTime" float parameter.
float MgAnimBlend_GetFadeInTime(const MgAnimBlendEntry* entry);

// FUN_0041ff4b: reads "FadeOutTime" float parameter.
float MgAnimBlend_GetFadeOutTime(const MgAnimBlendEntry* entry);

// FUN_0041ffa7: reads "HoldTime" float parameter.
float MgAnimBlend_GetHoldTime(const MgAnimBlendEntry* entry);

// FUN_00429020: reads "OneTimeOnly" bool parameter.
// Returns true (default) if entry is NULL or param is absent.
int   MgAnimBlend_GetOneTimeOnly(const MgAnimBlendEntry* entry);

// FUN_0043fd91: reads "MinFrame" integer parameter.
// Returns 0 (default) if entry is NULL or param is absent.
int   MgAnimBlend_GetMinFrame(const MgAnimBlendEntry* entry);

#ifdef __cplusplus
} // extern "C"
#endif
