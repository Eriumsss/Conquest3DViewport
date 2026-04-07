// MgEngine.h
// Reconstructed engine sub-systems:
//   FUN_00421c62  — shadow MaxShadowDistance property reader
//   FUN_0047da02  — shadow CastShadow property reader
//   FUN_004afa9a  — shadow ReceiveShadow property reader
//   FUN_00876dd5  — renderer object initialiser
//   FUN_0089eefc  — engine thread descriptor table init
//   FUN_0095659f  — physics driver sampler registration
//
// Sources: analyzed/Engine/
// All functions use EDI as implicit this/entry where noted.

#pragma once
#include <stdint.h>
#include <stdlib.h>
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Shadow property readers  (FUN_00421c62, FUN_0047da02, FUN_004afa9a)
//
// All three use EDI as an implicit pointer to the template/blend entry.
// They call 0x007e866b to look up a typed field by (type_key, data, field_key).
// On failure (EDI==NULL, data==NULL, or field not found) a default is returned.
//
// The game stores field values at: *(T*)((uint8_t*)entry + found_offset + 0x10)
//
// String constants (game .rdata):
//   0x9ca6dc = "float"            — type key for MaxShadowDistance
//   0x9cead0 = "bool"             — type key for CastShadow / ReceiveShadow
//   0x9d174c = "MaxShadowDistance"
//   0x9e74a8 = "CastShadow"
//   0x9e74b4 = "ReceiveShadow"
//   0x9c2b98 = default float (MaxShadowDistance default, from game .rdata)
// ---------------------------------------------------------------------------

// FUN_00421c62 — reads "float"/"MaxShadowDistance" from template data.
// EDI = const MgAnimBlendEntry* entry
float MgShadow_GetMaxDistance(const MgAnimBlendEntry* entry);

// FUN_0047da02 — reads "bool"/"CastShadow".  Default = 1 (true).
int   MgShadow_GetCastShadow(const MgAnimBlendEntry* entry);

// FUN_004afa9a — reads "bool"/"ReceiveShadow".  Default = 1 (true).
int   MgShadow_GetReceiveShadow(const MgAnimBlendEntry* entry);

// ---------------------------------------------------------------------------
// Physics driver sampler registration  (FUN_0095659f)
//
// ASM:
//   MOV  ECX, 0x9ca948              ; ptr to "MgPhysicsDriverSampler" string const
//   LEA  ESI, [EBP-4]               ; local MgString buffer
//   CALL 0x0067e6d8                  ; Mg::String::CopyConstruct
//   MOV  [0x00ce0ffc], 0xce0fb0      ; sampler struct ptr at static
//   MOV  [0x00ce1000], EAX           ; name string ptr
//   MOV  byte [0x00ce1004], 0        ; init flag = 0
//   PUSH 0x96e4c5 / CALL _atexit     ; register cleanup at exit
// ---------------------------------------------------------------------------
void MgPhysicsDriverSampler_Register(void);

// ---------------------------------------------------------------------------
// MgRenderer — partial layout recovered from FUN_00876dd5.
//
// Five array fields, allocated via 0x004067f8 (array-alloc helper):
//   +0x24 / +0x2c  : 0x800 (2048) entries each  (e.g. mesh list, renderable list)
//   +0x8c / +0x94 / +0x9c : 0x400 (1024) entries each
//   +0x88          : byte flag, cleared to 0 before first alloc
//   +0x84          : ptr to MgTexture object for "$DefaultTexture"
//   +0xa44         : 0 (init flag)
// ---------------------------------------------------------------------------
struct MgRenderer {
    uint8_t  _pad_00[0x24];
    void*    mesh_list;          // +0x24 : 0x800-entry array
    uint8_t  _pad_28[0x4];
    void*    renderable_list;    // +0x2c : 0x800-entry array
    uint8_t  _pad_30[0x58];
    uint32_t _flag_88;           // +0x88 : cleared to 0 before alloc
    void*    material_list_a;    // +0x8c : 0x400-entry array
    void*    material_list_b;    // +0x94 : 0x400-entry array
    void*    material_list_c;    // +0x9c : 0x400-entry array
    uint8_t  _pad_a0[0x44];
    void*    default_texture;    // +0x84 (stored at +0x84)
    // NOTE: +0xa44 = 0 (cleared with AND dword[EBX+0xa44], 0)
};

// FUN_00876dd5 — thiscall, ECX = MgRenderer*.
// Returns 1 on success (AL=1), 0 if readiness check (0x0089f25c) fails.
int MgRenderer_Init(MgRenderer* renderer);

// ---------------------------------------------------------------------------
// MgThreadDescriptor — one entry in the engine thread table at 0xa4a6f8.
// Recovered from FUN_0089eefc field assignments.
// ---------------------------------------------------------------------------
struct MgThreadDescriptor {
    const char* name;        // +0x00 : pointer to name string in .rdata
    uint32_t    _unk_04;     // +0x04 = 0
    uint32_t    _unk_08;     // +0x08 = 0
    uint32_t    stack_size;  // +0x0c : 0x10000 or 0x18000
    uint32_t    runtime_val; // +0x10 : set from stack at init time
    int32_t     affinity;    // +0x14 : -1 = all cores
    uint32_t    flag;        // +0x18 : 1
};

// Six thread table entries are written starting at game address 0xa4a6f8.
// Thread order (derived from string refs):
//   [0] Main thread (rendering)
//   [1] Physics manager thread
//   [2] Loading thread
//   [3] Input thread
//   [4] Game thread
//   [5] (Video thread — points to FUN_008a1124)
//
// FUN_0089eefc — __cdecl, no standard parameters; uses ESP+0x10 runtime value.
// Also calls __controlfp_s to configure FPU precision.
void MgEngine_InitThreadDescriptors(void);

#ifdef __cplusplus
} // extern "C"
#endif
