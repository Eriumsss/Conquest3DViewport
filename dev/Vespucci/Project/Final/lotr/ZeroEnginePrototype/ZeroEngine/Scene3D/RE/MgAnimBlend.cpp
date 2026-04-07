// MgAnimBlend.cpp
// Reconstructed from:
//   FUN_0041feef (0041feef–0041ff4a) — FadeInTime
//   FUN_0041ff4b (0041ff4b–0041ffa6) — FadeOutTime
//   FUN_0041ffa7 (0041ffa7–00420002) — HoldTime
//   FUN_00429020 (00429020–0042906d) — OneTimeOnly
//   FUN_0043fd91 (0043fd91–0043fdde) — MinFrame
//
// All five follow the exact same template, differing only in:
//   - String constant addresses for the param name
//   - Default return value
//   - Return type (float / bool / int)
//
// String constants referenced (game binary .rdata addresses):
//   0x9ca6dc  base string pool address used as key1 for all five
//   0x9cf5c8  "FadeInTime"   key2
//   0x9cf5d4  "FadeOutTime"  key2
//   0x9cf5e0  "HoldTime"     key2
//   0x9cead0  "OneTimeOnly"  key1 (different base!)
//   0x9d09d0  "OneTimeOnly"  key2
//   0x9ca6e4  "MinFrame"     key1
//   0x9e7968  "MinFrame"     key2
//
// Helper: reads a value from the param table.
// Template <T> covers float, int, bool.

#include "MgAnimBlend.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Dependency function pointers — set to game binary addresses at startup
// if running with game DLL loaded, or to stub implementations otherwise.
// ---------------------------------------------------------------------------
PFN_MgString_CopyConstruct g_pfn_MgString_CopyConstruct = NULL;
PFN_find_anim_param        g_pfn_find_anim_param        = NULL;

// ---------------------------------------------------------------------------
// Default values (match game binary constants at those .rdata addresses)
// ---------------------------------------------------------------------------
float g_MgAnimBlend_DefaultFadeInTime  = 0.0f;
float g_MgAnimBlend_DefaultFadeOutTime = 0.0f;
float g_MgAnimBlend_DefaultHoldTime    = 0.0f;

// ---------------------------------------------------------------------------
// Internal: generic param read helper.
// Mirrors the exact ASM pattern in all five functions:
//
//   if (!entry || !entry->blend_data) return default_value_raw;
//   construct key1 from src1_const, key2 from src2_const
//   offset = find_anim_param(&key1, blend_data, &key2)
//   if (offset == -1) return default_value_raw
//   return *(T*)((uint8_t*)entry + offset + 0x10)
//
// raw_default: bit-cast of the default float/int/bool, stored as uint32_t.
// Returns the raw bytes; caller casts to the desired type.
// ---------------------------------------------------------------------------
static uint32_t
MgAnimBlend_ReadParamRaw(const MgAnimBlendEntry* entry,
                          const void* src1_const,
                          const void* src2_const,
                          uint32_t    raw_default)
{
    // Step 1: NULL guard (game: TEST EDI,EDI / JZ return_default)
    if (!entry) return raw_default;

    // Step 2: blend_data NULL guard (game: MOV EBX,[EDI+4] / TEST EBX,EBX)
    void* blend_data = entry->blend_data;
    if (!blend_data) return raw_default;

    // Step 3: construct string keys via engine string ctor
    // (game: MOV ECX,src1_const / LEA ESI,[EBP-4] / CALL 0x0067e6d8)
    if (!g_pfn_MgString_CopyConstruct || !g_pfn_find_anim_param)
        return raw_default; // hooks not installed — return default

    MgString key1, key2;
    memset(&key1, 0, sizeof(key1));
    memset(&key2, 0, sizeof(key2));
    g_pfn_MgString_CopyConstruct(src1_const, &key1);
    g_pfn_MgString_CopyConstruct(src2_const, &key2);

    // Step 4: look up param (game: CALL 0x007e866b)
    // ESI = blend_data, EAX = &key2, [ESP] = &key1
    int byte_offset = g_pfn_find_anim_param(&key1, blend_data, &key2);

    // Step 5: if not found return default
    // (game: CMP EAX,-1 / JNZ found / MOVSS XMM0,[default_addr] / RET)
    if (byte_offset == -1) return raw_default;

    // Step 6: read value (game: MOVSS XMM0,[EAX + EDI*1 + 0x10])
    // EAX = byte_offset, EDI = entry base, +0x10 = value offset within entry
    const uint8_t* value_ptr = (const uint8_t*)entry + byte_offset + 0x10;
    uint32_t result;
    memcpy(&result, value_ptr, sizeof(result));
    return result;
}

// ---------------------------------------------------------------------------
// FUN_0041feef — GetFadeInTime
// key1: 0x9ca6dc, key2: 0x9cf5c8 ("FadeInTime")
// Default: *(float*)0x009c1f9c  → stored as g_MgAnimBlend_DefaultFadeInTime
// ---------------------------------------------------------------------------
float MgAnimBlend_GetFadeInTime(const MgAnimBlendEntry* entry)
{
    static const void* k_src1 = (const void*)0x9ca6dc;
    static const void* k_src2 = (const void*)0x9cf5c8;
    uint32_t raw;
    memcpy(&raw, &g_MgAnimBlend_DefaultFadeInTime, 4);
    raw = MgAnimBlend_ReadParamRaw(entry, k_src1, k_src2, raw);
    float result;
    memcpy(&result, &raw, 4);
    return result;
}

// ---------------------------------------------------------------------------
// FUN_0041ff4b — GetFadeOutTime
// key1: 0x9ca6dc, key2: 0x9cf5d4 ("FadeOutTime")
// Default: *(float*)0x009c5284  → g_MgAnimBlend_DefaultFadeOutTime
// ---------------------------------------------------------------------------
float MgAnimBlend_GetFadeOutTime(const MgAnimBlendEntry* entry)
{
    static const void* k_src1 = (const void*)0x9ca6dc;
    static const void* k_src2 = (const void*)0x9cf5d4;
    uint32_t raw;
    memcpy(&raw, &g_MgAnimBlend_DefaultFadeOutTime, 4);
    raw = MgAnimBlend_ReadParamRaw(entry, k_src1, k_src2, raw);
    float result;
    memcpy(&result, &raw, 4);
    return result;
}

// ---------------------------------------------------------------------------
// FUN_0041ffa7 — GetHoldTime
// key1: 0x9ca6dc, key2: 0x9cf5e0 ("HoldTime")
// Default: *(float*)0x00a41b8c  → g_MgAnimBlend_DefaultHoldTime
// ---------------------------------------------------------------------------
float MgAnimBlend_GetHoldTime(const MgAnimBlendEntry* entry)
{
    static const void* k_src1 = (const void*)0x9ca6dc;
    static const void* k_src2 = (const void*)0x9cf5e0;
    uint32_t raw;
    memcpy(&raw, &g_MgAnimBlend_DefaultHoldTime, 4);
    raw = MgAnimBlend_ReadParamRaw(entry, k_src1, k_src2, raw);
    float result;
    memcpy(&result, &raw, 4);
    return result;
}

// ---------------------------------------------------------------------------
// FUN_00429020 — GetOneTimeOnly
// key1: 0x9cead0, key2: 0x9d09d0 ("OneTimeOnly")
// Default: returns 1 (true) — game uses MOV AL,1 when not found
// Value read as byte: MOV AL, byte ptr [EAX + EDI + 0x10]
// ---------------------------------------------------------------------------
int MgAnimBlend_GetOneTimeOnly(const MgAnimBlendEntry* entry)
{
    static const void* k_src1 = (const void*)0x9cead0;
    static const void* k_src2 = (const void*)0x9d09d0;
    const uint32_t default_true = 1u;
    uint32_t raw = MgAnimBlend_ReadParamRaw(entry, k_src1, k_src2, default_true);
    // Only the low byte is meaningful (game uses MOVZX from byte)
    return (int)(raw & 0xFF);
}

// ---------------------------------------------------------------------------
// FUN_0043fd91 — GetMinFrame
// key1: 0x9ca6e4, key2: 0x9e7968 ("MinFrame")
// Default: returns 0 — game uses XOR EAX,EAX when not found
// Value read as DWORD: MOV EAX, dword ptr [EAX + EDI + 0x10]
// ---------------------------------------------------------------------------
int MgAnimBlend_GetMinFrame(const MgAnimBlendEntry* entry)
{
    static const void* k_src1 = (const void*)0x9ca6e4;
    static const void* k_src2 = (const void*)0x9e7968;
    uint32_t raw = MgAnimBlend_ReadParamRaw(entry, k_src1, k_src2, 0u);
    return (int)raw;
}
