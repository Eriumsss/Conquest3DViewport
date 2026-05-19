// MgPblTypes.h
// Reconstructed pbl:: (Pandemic Base Library) math type system.
// Sources: analyzed/PblTypes/
//
// Functions:
//   FUN_007e86d1  — Pbl type name registration (14 type/CRC pairs, one-shot init)
//   FUN_007e76d4  — Entity world transform accessor (4×4 float matrix copy)
//   FUN_00428789  — MgTemplateArrayT<pbl::Matrix4x4> / RoadMatrices accessor
//   FUN_004a6b46  — pbl::Matrix4x4 transform property reader
//   FUN_004fa16a  — pbl::Matrix4x4 Transform property reader (variant)
//
// Type registration globals:
//   Init flag:     0xcf793c  (written to 1 after first call)
//   Linked list:   0xcf792c  (head of registered type pairs)
//
// All 14 type registrations use Mg::String::CopyConstruct (0x0067e6d8).
// String address pairs from game .rdata (see MgPblTypes_Register body).

#pragma once
#include <stdint.h>
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// pbl::Vector3 — 3-component float vector
// ---------------------------------------------------------------------------
typedef struct {
    float x, y, z;
} pbl_Vector3;

// ---------------------------------------------------------------------------
// pbl::Vector4 — 4-component float vector
// ---------------------------------------------------------------------------
typedef struct {
    float x, y, z, w;
} pbl_Vector4;

// ---------------------------------------------------------------------------
// pbl::Matrix4x4 — row-major 4×4 float matrix
// Confirmed by 4 MOVSD groups in FUN_007e76d4 (copies 4 rows of 4 floats).
// ---------------------------------------------------------------------------
typedef struct {
    float m[4][4];
} pbl_Matrix4x4;

// ---------------------------------------------------------------------------
// PblColor — 4-component color (pbl::Color)
// ---------------------------------------------------------------------------
typedef struct {
    float r, g, b, a;
} PblColor;

// ---------------------------------------------------------------------------
// MgHandle — engine object handle type
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t id;
    uint32_t _pad;
} MgHandle;

// ---------------------------------------------------------------------------
// PblCRC — 32-bit CRC handle used as type/asset key
// ---------------------------------------------------------------------------
typedef uint32_t PblCRC;

// ---------------------------------------------------------------------------
// MgPblTypeNamePair — one entry in the type registration linked list at 0xcf792c.
// Recovered from FUN_007e86d1 structure writes.
// ---------------------------------------------------------------------------
struct MgPblTypeNamePair {
    MgString             short_name;  // +0x00 short type name (e.g. "vec3")
    MgString             full_name;   // +0x08 full C++ name (e.g. "pbl::Vector3")
    struct MgPblTypeNamePair* next;   // +0x10 linked list next ptr
};

// ---------------------------------------------------------------------------
// FUN_007e86d1 — MgPblTypes_Register
//
// One-shot initialiser: registers 14 type name pairs.
// Guard at [0xcf793c]: if already 1, returns immediately (one-shot).
// After registration, sets [0xcf793c] = 1.
//
// 14 pairs (short_name → full_name), order from ASM string addr pairs:
//   0x9cb660 → 0x9d8570  (unknown short)    → unknown full
//   0x9cb648 → 0x9d8574  (unknown)           → unknown
//   0x9d857c → 0x9d8588  (unknown)           → unknown
//   0x9ca734 → 0x9d8590  "MgHandle"          → "MgHandle"
//   0x9d8598 → 0x9d85a8  (unknown)           → unknown
//   0x9cd5e8 → 0x9d85b0  (ref: pbl::Vector3) → "pbl::Vector3"
//   0x9d0648 → 0x9d85bc  (unknown)           → unknown
//   0x9cbddc → 0x9d85c4  (unknown)           → unknown
//   0x9ceae8 → 0x9d85d0  (unknown)           → unknown
//   0x9ce024 → 0x9d85dc  (unknown)           → unknown
//   0x9d85e8 → 0x9d8600  (unknown)           → unknown
//   0x9d09a4 → 0x9d8608  (unknown)           → unknown
//   0x9d8610 → 0x9d8630  (unknown)           → unknown
//   0x9d1498 → 0x9d863c  "PblCRC"            → "PblCRC"
//
// Linked list head: [0xcf792c]
// ---------------------------------------------------------------------------
void MgPblTypes_Register(void);

// ---------------------------------------------------------------------------
// FUN_007e76d4 — MgEntity_GetWorldTransform
//
// Reads the world transform matrix of an entity/factory object.
//
// ASM:
//   PUSH "MgTemplateString" / "Factory" / "pbl::Matrix4x4" / "WorldTransform"
//   ; These string key lookups find the template data block
//   ; Then copies 4×4 float matrix via 4 groups of MOVSD instructions:
//   MOVSD group 1: 4× MOVSD  (row 0: 16 bytes)
//   MOVSD group 2: 4× MOVSD  (row 1)
//   MOVSD group 3: 4× MOVSD  (row 2)
//   MOVSD group 4: 4× MOVSD  (row 3)
//
// Params (recovered from stack layout):
//   entity_or_factory — pointer to entity/factory object
//   out_matrix        — pointer to 4×4 float matrix output
//
// String keys used (game .rdata):
//   "MgTemplateString", "Factory", "pbl::Matrix4x4", "WorldTransform", "MgHandle"
// ---------------------------------------------------------------------------
void MgEntity_GetWorldTransform(const void*     entity_or_factory,
                                pbl_Matrix4x4*  out_matrix);

// ---------------------------------------------------------------------------
// FUN_00428789 — MgTemplate_GetRoadMatrices (~84 addr-count)
//
// Reads "MgTemplateArrayT<pbl::Matrix4x4>" / "RoadMatrices" template field.
// Pattern: uses 0x007e866b (find_typed_param) with those string keys.
// Default: empty array (NULL ptr / count 0).
// ---------------------------------------------------------------------------
void MgTemplate_GetRoadMatrices(const MgAnimBlendEntry* entry,
                                 pbl_Matrix4x4**         out_ptr,
                                 uint32_t*               out_count);

// ---------------------------------------------------------------------------
// FUN_004a6b46 — MgTemplate_GetTransform (~84 addr-count)
//
// Reads "pbl::Matrix4x4" / "transform" template field.
// Default: identity matrix.
// ---------------------------------------------------------------------------
void MgTemplate_GetTransform(const MgAnimBlendEntry* entry,
                              pbl_Matrix4x4*          out_matrix);

// ---------------------------------------------------------------------------
// FUN_004fa16a — MgTemplate_GetTransformV2 (~84 addr-count)
//
// Same pattern; field key = "Transform" (capital T).
// Default: identity matrix.
// ---------------------------------------------------------------------------
void MgTemplate_GetTransformV2(const MgAnimBlendEntry* entry,
                                pbl_Matrix4x4*          out_matrix);

// ===========================================================================
// MgHandle Encoding (from disassembly at 0x007e4347, 0x007e60f7, 0x007e8bf8)
//
// MgHandle.id is a 32-bit packed value:
//   Bits 0-17  (18 bits): generation counter / type tag
//     - Value 1 (0x00001) marks a valid resolved handle
//     - Value 0 marks an unresolved/null handle
//   Bits 18-31 (14 bits): index into entity array
//     - Dynamic entities: array at [world + 0x274]
//     - Static entities:  array at [world + 0x270]
//     - Each array entry is 12 bytes: {generation, entity_ptr, flags}
//
// Handle allocation (0x007e4347):
//   handle = (slot_index << 18) | (generation & 0x3FFFF)
//
// Handle resolution (0x007e60f7):
//   generation = handle & 0x3FFFF
//   index      = handle >> 18
//   entity     = entity_array[index * 12]
//   valid      = (stored_generation == handle_generation)
//
// GUID Resolution (0x007e8bf8):
//   During level load, raw GUIDs from binary data are resolved to handles.
//   The resolver iterates all entities' fields and converts:
//     MgHandle fields (type hash 0x56a6fb5c) -> single GUID -> handle
//     MgHandleArray fields (type hash 0x3bcce54b) -> array of GUIDs -> handles
//     MgOutputArray fields (type hash 0x7f97829f) -> [targetGUID:4][eventHash:4] per output
//   Null/self sentinel GUID 0xCF98052F is skipped (not resolved).
// ===========================================================================

// Field type hashes used by GUID resolution (from 0x007e8bf8)
#define MG_HANDLE_TYPE_HASH      0x56a6fb5c  // CRC of "MgHandle" field type
#define MG_HANDLEARRAY_TYPE_HASH 0x3bcce54b  // CRC of "MgHandleArray" / MgTemplateArrayT<MgHandle>
#define MG_OUTPUTARRAY_TYPE_HASH 0x7f97829f  // CRC of "MgOutputArray" / output connection array
#define MG_HANDLE_NULL_SENTINEL  0xCF98052F  // Null/self GUID sentinel (same as KIND_GUID_VAL)

// Handle bit layout
#define MG_HANDLE_GEN_MASK   0x0003FFFF  // Bits 0-17: generation/tag (18 bits)
#define MG_HANDLE_IDX_SHIFT  18          // Bits 18-31: entity array index

// Handle helpers
static inline uint32_t MgHandle_GetIndex(MgHandle h) {
    return h.id >> MG_HANDLE_IDX_SHIFT;
}

static inline uint32_t MgHandle_GetGeneration(MgHandle h) {
    return h.id & MG_HANDLE_GEN_MASK;
}

static inline int MgHandle_IsValid(MgHandle h) {
    return (h.id & MG_HANDLE_GEN_MASK) != 0;
}

static inline int MgHandle_IsNull(MgHandle h) {
    return h.id == 0 || h.id == MG_HANDLE_NULL_SENTINEL;
}

static inline MgHandle MgHandle_FromIndex(uint32_t index) {
    MgHandle h;
    h.id = (index << MG_HANDLE_IDX_SHIFT) | 1;
    h._pad = 0;
    return h;
}

// Entity array entry (12 bytes per slot, from disassembly)
typedef struct {
    uint32_t generation;    // +0x00: generation counter for handle validation
    void*    entity_ptr;    // +0x04: pointer to entity object (or NULL if slot free)
    uint32_t flags;         // +0x08: entity flags
} MgEntitySlot;

// Global addresses (from disassembly)
// Entity table base: [0x00cd7e2c] -> entity manager
// Dynamic entity array: [entity_manager + 0x274]
// Static entity array:  [entity_manager + 0x270]
// Active gamemode: [0x00cd7f88 + 0x260]

#ifdef __cplusplus
} // extern "C"
#endif
