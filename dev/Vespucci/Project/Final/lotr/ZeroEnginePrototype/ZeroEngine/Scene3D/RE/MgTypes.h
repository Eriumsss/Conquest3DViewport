// MgTypes.h
// Reconstructed common types and primitives for the Magellan/Pandemic engine.
// Inferred from field access patterns across all analyzed ASM dumps.
//
// Calling convention notes (from disassembly):
//   Many functions use EDI as implicit 'self' pointer (non-standard).
//   Standard thiscall uses ECX. Where EDI is used, functions are __cdecl
//   with the object pointer passed in EDI by the caller.
//
// Script event dispatch (0x00725c3b):
//   __cdecl MgEntity_DispatchScriptEvent(uint32_t entity_id,
//                                         const char* event_name,
//                                         MgScriptEventArg* args,
//                                         int num_args)
//
// String constructor (0x0067e6d8):
//   __thiscall Mg::String::String(const MgStringConst* src, MgString* out)
//
// Param lookup (0x007e866b):
//   __cdecl int find_anim_param(MgString* key1, void* blend_data, MgString* key2)
//   Returns byte offset into blend entry where the value lives, or -1 if not found.

#pragma once
#include <stdint.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// MgString — engine string type (opaque, managed by 0x0067e6d8)
// Size inferred: at least 8 bytes (two DWORDs seen in stack locals)
// ---------------------------------------------------------------------------
struct MgString {
    void*    data;      // +0x00 internal char buffer / ref-counted blob
    uint32_t length;    // +0x04
};

// ---------------------------------------------------------------------------
// MgEntity — base game entity. Only +0x64 (entity_id) confirmed by multiple
// ex* script dispatch functions.
// ---------------------------------------------------------------------------
struct MgEntity {
    uint8_t  _pad[0x64];
    uint32_t entity_id; // +0x64: used by all exSet* script dispatch calls
};

// ---------------------------------------------------------------------------
// MgScriptEventArg — argument block passed to MgEntity_DispatchScriptEvent.
// type codes seen:
//   3 = double (2× 8-byte doubles in the block)
//   4 = unknown (used by exSetTimer, single value)
//   5 = two-value block (used by exSetClassInfo)
// Layout: first DWORD = type tag, pad to 16-byte alignment, then value(s)
// ---------------------------------------------------------------------------
struct MgScriptEventArg {
    uint32_t type;      // +0x00 type tag
    uint32_t _pad0;     // +0x04
    uint32_t _pad1;     // +0x08
    uint32_t _pad2;     // +0x0C
    // second slot (if 2-arg dispatch)
    uint32_t type2;     // +0x10
    uint32_t _pad3;     // +0x14
    uint32_t _pad4;     // +0x18
    uint32_t _pad5;     // +0x1C
};

// Double-valued event arg (type==3 or type==4)
struct MgScriptEventArgDouble {
    uint32_t type;   // +0x00
    uint8_t  _pad[4];
    double   value;  // +0x08
};

// Two-double event arg block (exSetCrosshairPos, exSetCrosshairColor)
struct MgScriptEventArgDouble2 {
    uint32_t type0;  // +0x00
    uint8_t  _pad0[4];
    double   val0;   // +0x08
    uint32_t type1;  // +0x10
    uint8_t  _pad1[4];
    double   val1;   // +0x18
};

// ---------------------------------------------------------------------------
// MgAnimBlendEntry — object holding blend parameter data.
// +0x04 = pointer to blend_data source (NULL → use defaults for all params).
// Value layout when found: *(T*)((uint8_t*)blend_entry + found_offset + 0x10)
// ---------------------------------------------------------------------------
struct MgAnimBlendEntry {
    uint32_t _unk_00;   // +0x00
    void*    blend_data;// +0x04: if NULL, all param accessors return defaults
    // ... unknown further fields ...
};

// ---------------------------------------------------------------------------
// MgTypeInfoBlock — static type registration block written by FUN_0095xxxx.
// Three consecutive globals per type: vtable ptr, name string, init byte.
// ---------------------------------------------------------------------------
struct MgTypeInfoBlock {
    void*    vtable;        // +0x00
    MgString name;          // +0x04
    uint8_t  initialized;   // +0x0C (byte flag, set to 0 at registration)
};
