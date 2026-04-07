// MgTypeRegistry.cpp
// Reconstructed from FUN_0095e584, FUN_0095e815, FUN_0095e84e, FUN_0095e92e,
//                    FUN_0095e9d5, FUN_0095f03e, FUN_00958296, FUN_0095937a
//
// Each function follows the identical MSVC static-initializer template:
//   PUSH ECX / PUSH ESI
//   MOV ECX, [string_const_addr]    ; load string descriptor
//   LEA ESI, [EBP-4]                ; &local_string
//   CALL 0x0067e6d8                  ; construct Mg::String
//   MOV EAX, [EBP-4]                ; EAX = string data ptr
//   PUSH [dtor_addr]                 ; atexit arg
//   MOV dword [vtable_global], vtable_value
//   MOV [name_global],  EAX
//   MOV byte [init_global], 0x0
//   CALL 0x0060d709                  ; register atexit destructor
//
// In our reconstruction we store the type name as a plain C string and
// call atexit with a per-type dtor. The vtable pointers are stored as
// numeric constants matching the game binary for cross-reference.

#include "MgTypeRegistry.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Type info blocks — initialised in StaticInit functions below.
// vtable field stores the game binary vtable address as an integer for
// reference/comparison. In a live hook scenario this would point to the
// actual vtable in the loaded game binary.
// ---------------------------------------------------------------------------
MgTypeInfoBlock g_MgCrosshairController_Type;
MgTypeInfoBlock g_MgMathCounter_Type;
MgTypeInfoBlock g_MgAIActionInstance_Type;
MgTypeInfoBlock g_MgAIActionSequenceInstanceI_Type;
MgTypeInfoBlock g_MgAIActionSequenceInstance_Type;
MgTypeInfoBlock g_MgAIActionSequenceStageInstance_Type;
MgTypeInfoBlock g_MgAIActionSequenceRandomizedInstance_Type;
MgTypeInfoBlock g_NodeInstance_Type;

// ---------------------------------------------------------------------------
// Internal helper: initialise one type block.
// Mirrors the ASM: set vtable, copy string ptr, clear init byte, call atexit.
// ---------------------------------------------------------------------------
static void init_type(MgTypeInfoBlock* blk, const char* name,
                       void* vtable_addr, void (*dtor)(void))
{
    blk->vtable      = vtable_addr;
    blk->name.data   = (void*)name; // plain C string pointer
    blk->name.length = (uint32_t)strlen(name);
    blk->initialized = 0;
    if (dtor) atexit(dtor);
}

// ---------------------------------------------------------------------------
// Per-type destructors (registered via atexit — mirrors the pushed dtor addr)
// Game addresses: 0x96e6b6 (crosshair), 0x96e850 (mathcounter), etc.
// ---------------------------------------------------------------------------
static void MgCrosshairController_dtor(void)              { memset(&g_MgCrosshairController_Type, 0, sizeof(g_MgCrosshairController_Type)); }
static void MgMathCounter_dtor(void)                      { memset(&g_MgMathCounter_Type, 0, sizeof(g_MgMathCounter_Type)); }
static void MgAIActionInstance_dtor(void)                 { memset(&g_MgAIActionInstance_Type, 0, sizeof(g_MgAIActionInstance_Type)); }
static void MgAIActionSequenceInstanceI_dtor(void)        { memset(&g_MgAIActionSequenceInstanceI_Type, 0, sizeof(g_MgAIActionSequenceInstanceI_Type)); }
static void MgAIActionSequenceInstance_dtor(void)         { memset(&g_MgAIActionSequenceInstance_Type, 0, sizeof(g_MgAIActionSequenceInstance_Type)); }
static void MgAIActionSequenceStageInstance_dtor(void)    { memset(&g_MgAIActionSequenceStageInstance_Type, 0, sizeof(g_MgAIActionSequenceStageInstance_Type)); }
static void MgAIActionSequenceRandomizedInstance_dtor(void){ memset(&g_MgAIActionSequenceRandomizedInstance_Type, 0, sizeof(g_MgAIActionSequenceRandomizedInstance_Type)); }
static void NodeInstance_dtor(void)                       { memset(&g_NodeInstance_Type, 0, sizeof(g_NodeInstance_Type)); }

// ---------------------------------------------------------------------------
// FUN_00958296 — MgCrosshairController static init
// vtable_global=0xcf7078, vtable_value=0xcf7060, name_global=0xcf707c,
// init_global=0xcf7080, string_const=0x9cdf70, dtor=0x96e6b6
// ---------------------------------------------------------------------------
void MgCrosshairController_StaticInit(void)
{
    init_type(&g_MgCrosshairController_Type, "MgCrosshairController",
              (void*)0x00CF7060, MgCrosshairController_dtor);
}

// ---------------------------------------------------------------------------
// FUN_0095937a — MgMathCounter static init
// vtable_global=0xcf74ec, vtable_value=0xce12c0, name_global=0xcf74f0,
// init_global=0xcf74f4, string_const=0x9cff24, dtor=0x96e850
// ---------------------------------------------------------------------------
void MgMathCounter_StaticInit(void)
{
    init_type(&g_MgMathCounter_Type, "MgMathCounter",
              (void*)0x00CE12C0, MgMathCounter_dtor);
}

// ---------------------------------------------------------------------------
// FUN_0095e584 — MgAIActionInstance static init
// vtable_global=0xcfc0f0, vtable_value=0xe94bc0, name_global=0xcfc0f4,
// init_global=0xcfc0f8, string_const=0x9de0a0, dtor=0x96ed9b
// ---------------------------------------------------------------------------
void MgAIActionInstance_StaticInit(void)
{
    init_type(&g_MgAIActionInstance_Type, "MgAIActionInstance",
              (void*)0x00E94BC0, MgAIActionInstance_dtor);
}

// ---------------------------------------------------------------------------
// FUN_0095e815 — MgAIActionSequenceInstanceI static init
// vtable_global=0xcfc114, vtable_value=0xe94bc0, name_global=0xcfc118,
// init_global=0xcfc11c, string_const=0x9de218, dtor=0x96ed9e
// Note: same vtable value as MgAIActionInstance (interface type)
// ---------------------------------------------------------------------------
void MgAIActionSequenceInstanceI_StaticInit(void)
{
    init_type(&g_MgAIActionSequenceInstanceI_Type, "MgAIActionSequenceInstanceI",
              (void*)0x00E94BC0, MgAIActionSequenceInstanceI_dtor);
}

// ---------------------------------------------------------------------------
// FUN_0095e84e — MgAIActionSequenceInstance static init
// vtable_global=0xcfc120, vtable_value=0xcfc114 (points to SequenceInstanceI block),
// name_global=0xcfc124, init_global=0xcfc128, string_const=0x9de234, dtor=0x96ed9f
// ---------------------------------------------------------------------------
void MgAIActionSequenceInstance_StaticInit(void)
{
    init_type(&g_MgAIActionSequenceInstance_Type, "MgAIActionSequenceInstance",
              (void*)0x00CFC114, MgAIActionSequenceInstance_dtor);
}

// ---------------------------------------------------------------------------
// FUN_0095e92e — MgAIActionSequenceStageInstance static init
// vtable_global=0xcfc144, vtable_value=0xcfc114, name_global=0xcfc148,
// init_global=0xcfc14c, string_const=0x9de49c, dtor=0x96eda7
// ---------------------------------------------------------------------------
void MgAIActionSequenceStageInstance_StaticInit(void)
{
    init_type(&g_MgAIActionSequenceStageInstance_Type, "MgAIActionSequenceStageInstance",
              (void*)0x00CFC114, MgAIActionSequenceStageInstance_dtor);
}

// ---------------------------------------------------------------------------
// FUN_0095e9d5 — MgAIActionSequenceRandomizedInstance static init
// vtable_global=0xcfc15c, vtable_value=0xcfc114, name_global=0xcfc160,
// init_global=0xcfc164, string_const=0x9de5a0, dtor=0x96edae
// ---------------------------------------------------------------------------
void MgAIActionSequenceRandomizedInstance_StaticInit(void)
{
    init_type(&g_MgAIActionSequenceRandomizedInstance_Type,
              "MgAIActionSequenceRandomizedInstance",
              (void*)0x00CFC114, MgAIActionSequenceRandomizedInstance_dtor);
}

// ---------------------------------------------------------------------------
// FUN_0095f03e — NodeInstance static init
// vtable_global=0xcfc290, vtable_value=0xe568f4, name_global=0xcfc294,
// init_global=0xcfc298, string_const=0x9df07c, dtor=0x96ede0
// ---------------------------------------------------------------------------
void NodeInstance_StaticInit(void)
{
    init_type(&g_NodeInstance_Type, "NodeInstance",
              (void*)0x00E568F4, NodeInstance_dtor);
}

// ---------------------------------------------------------------------------
// Registry table for MgTypeRegistry_Find
// ---------------------------------------------------------------------------
static MgTypeInfoBlock* s_all_types[] = {
    &g_MgCrosshairController_Type,
    &g_MgMathCounter_Type,
    &g_MgAIActionInstance_Type,
    &g_MgAIActionSequenceInstanceI_Type,
    &g_MgAIActionSequenceInstance_Type,
    &g_MgAIActionSequenceStageInstance_Type,
    &g_MgAIActionSequenceRandomizedInstance_Type,
    &g_NodeInstance_Type,
};
static const int s_type_count = (int)(sizeof(s_all_types)/sizeof(s_all_types[0]));

void MgTypeRegistry_InitAll(void)
{
    MgCrosshairController_StaticInit();
    MgMathCounter_StaticInit();
    MgAIActionInstance_StaticInit();
    MgAIActionSequenceInstanceI_StaticInit();
    MgAIActionSequenceInstance_StaticInit();
    MgAIActionSequenceStageInstance_StaticInit();
    MgAIActionSequenceRandomizedInstance_StaticInit();
    NodeInstance_StaticInit();
}

const MgTypeInfoBlock* MgTypeRegistry_Find(const char* name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_type_count; ++i) {
        MgTypeInfoBlock* blk = s_all_types[i];
        if (blk->name.data && strcmp((const char*)blk->name.data, name) == 0)
            return blk;
    }
    return NULL;
}
