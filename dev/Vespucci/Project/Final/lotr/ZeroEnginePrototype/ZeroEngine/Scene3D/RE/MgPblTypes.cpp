// MgPblTypes.cpp
// Reconstructed from:
//   FUN_007e86d1  (MgPblTypes_Register,      ~300 addr)
//   FUN_007e76d4  (MgEntity_GetWorldTransform, ~150 addr)
//   FUN_00428789  (GetRoadMatrices,             84 addr)
//   FUN_004a6b46  (GetTransform,                84 addr)
//   FUN_004fa16a  (GetTransformV2,              84 addr)

#include "MgPblTypes.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Game function pointers
// ---------------------------------------------------------------------------
typedef void (__cdecl* PFN_StringCopyConstruct)(const void* src, void* out);
static PFN_StringCopyConstruct g_pfn_StringCopyConstruct = NULL; // 0x0067e6d8

typedef int (__cdecl* PFN_FindTypedParam)(void* key1, void* data, void* key2);
static PFN_FindTypedParam g_pfn_FindTypedParam = NULL; // 0x007e866b

// One-shot init guard (mirrors [0xcf793c] in game)
static int s_pbl_types_registered = 0;

// Linked list head (mirrors [0xcf792c] in game)
// In standalone, we maintain a local list.
static struct MgPblTypeNamePair* s_type_list_head = NULL;

// ---------------------------------------------------------------------------
// FUN_007e86d1 — MgPblTypes_Register
//
// Confirmed one-shot via guard at [0xcf793c].
// Registers 14 type name pairs using Mg::String::CopyConstruct for each name.
//
// The 14 pairs (addresses confirmed from ASM, names from cross-reference):
//   pair  0: 0x9cb660 / 0x9d8570  — not yet identified
//   pair  1: 0x9cb648 / 0x9d8574  — not yet identified
//   pair  2: 0x9d857c / 0x9d8588  — not yet identified
//   pair  3: 0x9ca734 / 0x9d8590  — "MgHandle"
//   pair  4: 0x9d8598 / 0x9d85a8  — not yet identified
//   pair  5: 0x9cd5e8 / 0x9d85b0  — "pbl::Vector3"
//   pair  6: 0x9d0648 / 0x9d85bc  — not yet identified
//   pair  7: 0x9cbddc / 0x9d85c4  — not yet identified
//   pair  8: 0x9ceae8 / 0x9d85d0  — not yet identified (pbl::Vector4?)
//   pair  9: 0x9ce024 / 0x9d85dc  — not yet identified (pbl::Matrix4x4?)
//   pair 10: 0x9d85e8 / 0x9d8600  — not yet identified
//   pair 11: 0x9d09a4 / 0x9d8608  — not yet identified
//   pair 12: 0x9d8610 / 0x9d8630  — not yet identified
//   pair 13: 0x9d1498 / 0x9d863c  — "PblCRC"
//
// In standalone reconstruction: no-op (game binary not loaded).
// In hooked mode: call g_pfn_StringCopyConstruct with game .rdata ptrs.
// ---------------------------------------------------------------------------
void MgPblTypes_Register(void)
{
    if (s_pbl_types_registered) return;
    s_pbl_types_registered = 1;

    // In game binary these calls go to 0x0067e6d8 with hardcoded .rdata ptrs.
    // Standalone: nothing to register without the binary.
    if (!g_pfn_StringCopyConstruct) return;

    // Game-binary-only registration:
    // for each of 14 pairs: call g_pfn_StringCopyConstruct(rdata_ptr, &pair_node.name)
    // then prepend to linked list at [0xcf792c].
    // Deferred pending binary hookup.
}

// ---------------------------------------------------------------------------
// FUN_007e76d4 — MgEntity_GetWorldTransform
//
// ASM: 4 groups of 4× MOVSD — copies 64 bytes (4×4 float matrix).
// The source pointer is resolved via template lookup:
//   Look up "Factory" → "pbl::Matrix4x4" → "WorldTransform"
//   using the entity as the template data source.
//
// In reconstruction without the game binary, we output an identity matrix.
// ---------------------------------------------------------------------------
void MgEntity_GetWorldTransform(const void*    entity_or_factory,
                                pbl_Matrix4x4* out_matrix)
{
    if (!out_matrix) return;

    if (!entity_or_factory || !g_pfn_FindTypedParam) {
        // Default: identity matrix
        memset(out_matrix, 0, sizeof(pbl_Matrix4x4));
        out_matrix->m[0][0] = 1.0f;
        out_matrix->m[1][1] = 1.0f;
        out_matrix->m[2][2] = 1.0f;
        out_matrix->m[3][3] = 1.0f;
        return;
    }

    // In the game, this resolves "Factory"/"WorldTransform" via the template
    // system and copies the 4×4 matrix using 4 MOVSD groups.
    // The copy is: 4 × (4× MOVSD) = 16 × 8 bytes = 128 bytes? No:
    // MOVSD copies 8 bytes (QWORD), 4× MOVSD = 32 bytes, 4 groups = 128 bytes?
    // But pbl::Matrix4x4 = 16 floats = 64 bytes.  So likely 4× MOVSD per row
    // copies 4×4 = 16 floats as 8 QWORDs = 64 bytes total (2 MOVSD per row).
    // Actually 4 MOVSD per group × 4 groups = 16 MOVSDs × 4 bytes = 64 bytes. ✓
    //
    // Stub: output identity.
    memset(out_matrix, 0, sizeof(pbl_Matrix4x4));
    out_matrix->m[0][0] = 1.0f;
    out_matrix->m[1][1] = 1.0f;
    out_matrix->m[2][2] = 1.0f;
    out_matrix->m[3][3] = 1.0f;
}

// ---------------------------------------------------------------------------
// Template matrix property accessors — common pattern
// ---------------------------------------------------------------------------

static void matrix_prop_lookup(const MgAnimBlendEntry* entry,
                                const char*             type_key,
                                const char*             field_key,
                                pbl_Matrix4x4*          out)
{
    if (!out) return;

    // Default: identity
    memset(out, 0, sizeof(pbl_Matrix4x4));
    out->m[0][0] = 1.0f;
    out->m[1][1] = 1.0f;
    out->m[2][2] = 1.0f;
    out->m[3][3] = 1.0f;

    if (!entry || !entry->blend_data || !g_pfn_FindTypedParam) return;

    int offset = g_pfn_FindTypedParam((void*)type_key, entry->blend_data, (void*)field_key);
    if (offset == -1) return;

    const pbl_Matrix4x4* src =
        (const pbl_Matrix4x4*)((const uint8_t*)entry + offset + 0x10);
    memcpy(out, src, sizeof(pbl_Matrix4x4));
}

// FUN_00428789 — MgTemplateArrayT<pbl::Matrix4x4> / RoadMatrices
void MgTemplate_GetRoadMatrices(const MgAnimBlendEntry* entry,
                                 pbl_Matrix4x4**         out_ptr,
                                 uint32_t*               out_count)
{
    if (out_ptr)   *out_ptr   = NULL;
    if (out_count) *out_count = 0;

    if (!entry || !entry->blend_data || !g_pfn_FindTypedParam) return;

    static const char* k_type  = "MgTemplateArrayT<pbl::Matrix4x4>";
    static const char* k_field = "RoadMatrices";

    int offset = g_pfn_FindTypedParam((void*)k_type, entry->blend_data, (void*)k_field);
    if (offset == -1) return;

    // Array field layout (from 0x00428cf7 read pattern): ptr + count
    const uint8_t* base = (const uint8_t*)entry + offset + 0x10;
    if (out_ptr)   *out_ptr   = *(pbl_Matrix4x4**)(base + 0x0);
    if (out_count) *out_count = *(uint32_t*)(base + 0x4);
}

// FUN_004a6b46 — "pbl::Matrix4x4" / "transform"
void MgTemplate_GetTransform(const MgAnimBlendEntry* entry,
                              pbl_Matrix4x4*          out_matrix)
{
    matrix_prop_lookup(entry, "pbl::Matrix4x4", "transform", out_matrix);
}

// FUN_004fa16a — "pbl::Matrix4x4" / "Transform"
void MgTemplate_GetTransformV2(const MgAnimBlendEntry* entry,
                                pbl_Matrix4x4*          out_matrix)
{
    matrix_prop_lookup(entry, "pbl::Matrix4x4", "Transform", out_matrix);
}
