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

#ifdef __cplusplus
} // extern "C"
#endif
