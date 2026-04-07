// MgEngine.cpp
// Reconstructed from:
//   FUN_00421c62  (MaxShadowDistance)
//   FUN_0047da02  (CastShadow)
//   FUN_004afa9a  (ReceiveShadow)
//   FUN_00876dd5  (MgRenderer_Init)
//   FUN_0089eefc  (MgEngine_InitThreadDescriptors)
//   FUN_0095659f  (MgPhysicsDriverSampler_Register)

#include "MgEngine.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Game function pointer: find_typed_param — 0x007e866b
// __cdecl (key1, blend_data, key2) → int offset or -1
// ---------------------------------------------------------------------------
typedef int (__cdecl* PFN_FindTypedParam)(void* key1, void* data, void* key2);
static PFN_FindTypedParam g_pfn_FindTypedParam = NULL;

// Game function pointer: Mg::String::CopyConstruct — 0x0067e6d8
// __thiscall (ECX=src_const, ESI=out_MgString)
typedef void (__cdecl* PFN_StringCopyConstruct)(const void* src, void* out);
static PFN_StringCopyConstruct g_pfn_StringCopyConstruct = NULL;

// ---------------------------------------------------------------------------
// Shadow property accessors common pattern:
//   1. If entry == NULL or entry->blend_data == NULL → return default
//   2. Construct two MgString key objects on stack
//   3. call find_typed_param(key1, blend_data, key2) → offset (or -1)
//   4. If -1 → return default
//   5. return *(T*)((uint8_t*)entry + offset + 0x10)
//
// In standalone reconstruction (g_pfn_FindTypedParam == NULL) we always
// return the documented defaults.
// ---------------------------------------------------------------------------

// FUN_00421c62 — MaxShadowDistance
// Default: [0x009c2b98] = some game-binary float constant (documented as ~512.0f)
float MgShadow_GetMaxDistance(const MgAnimBlendEntry* entry)
{
    static const float k_default = 512.0f; // mirrors MOV XMM0,[0x009c2b98]

    if (!entry || !entry->blend_data) return k_default;
    if (!g_pfn_FindTypedParam)       return k_default;

    // Construct type key "float" and field key "MaxShadowDistance"
    // (game uses 0x0067e6d8 to copy from .rdata ptr 0x9ca6dc / 0x9d174c)
    // In reconstruction we pass string pointers directly.
    static const char* k_type  = "float";
    static const char* k_field = "MaxShadowDistance";

    int offset = g_pfn_FindTypedParam((void*)k_type, entry->blend_data, (void*)k_field);
    if (offset == -1) return k_default;

    // Value lives at entry_base + offset + 0x10
    return *(const float*)((const uint8_t*)entry + offset + 0x10);
}

// FUN_0047da02 — CastShadow
// Default: 1 (true) — game: MOV AL,1 / JMP
int MgShadow_GetCastShadow(const MgAnimBlendEntry* entry)
{
    if (!entry || !entry->blend_data) return 1;
    if (!g_pfn_FindTypedParam)       return 1;

    static const char* k_type  = "bool";    // 0x9cead0 in game .rdata
    static const char* k_field = "CastShadow"; // 0x9e74a8

    int offset = g_pfn_FindTypedParam((void*)k_type, entry->blend_data, (void*)k_field);
    if (offset == -1) return 1;

    return (int)(*(const uint8_t*)((const uint8_t*)entry + offset + 0x10));
}

// FUN_004afa9a — ReceiveShadow
// Default: 1 (true)
int MgShadow_GetReceiveShadow(const MgAnimBlendEntry* entry)
{
    if (!entry || !entry->blend_data) return 1;
    if (!g_pfn_FindTypedParam)       return 1;

    static const char* k_type  = "bool";     // 0x9cead0
    static const char* k_field = "ReceiveShadow"; // 0x9e74b4

    int offset = g_pfn_FindTypedParam((void*)k_type, entry->blend_data, (void*)k_field);
    if (offset == -1) return 1;

    return (int)(*(const uint8_t*)((const uint8_t*)entry + offset + 0x10));
}

// ---------------------------------------------------------------------------
// FUN_0095659f — MgPhysicsDriverSampler_Register
//
// Registers the physics driver sampler via _atexit.
// ASM:
//   MOV  ECX, 0x9ca948       ; "MgPhysicsDriverSampler" const ptr
//   LEA  ESI, [EBP-4]        ; local MgString
//   CALL 0x0067e6d8           ; Mg::String::CopyConstruct → ESI = filled
//   MOV  [0xce0ffc], 0xce0fb0 ; sampler struct at static global
//   MOV  [0xce1000], EAX      ; name string ptr
//   MOV  byte [0xce1004], 0   ; flag = 0
//   PUSH 0x96e4c5 / CALL _atexit
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
#endif

// Sampler static block at 0xce0fb0..0xce1004
static void MgPhysicsDriverSampler_Cleanup(void);

void MgPhysicsDriverSampler_Register(void)
{
    // In game binary:
    //   *(void**)0x00ce0ffc = (void*)0x00ce0fb0;  // sampler struct ptr
    //   *(void**)0x00ce1000 = name_string_ptr;     // "MgPhysicsDriverSampler"
    //   *(uint8_t*)0x00ce1004 = 0;
    //   atexit(0x0096e4c5);                        // cleanup fn
    //
    // In standalone reconstruction: no-op (game binary not loaded).
    (void)MgPhysicsDriverSampler_Cleanup;
}

static void MgPhysicsDriverSampler_Cleanup(void)
{
    // mirrors FUN_0096e4c5 — cleanup on exit (not yet analyzed)
}

// ---------------------------------------------------------------------------
// FUN_00876dd5 — MgRenderer_Init  (__thiscall, ECX = MgRenderer*)
//
// ASM summary:
//   CALL 0x0089f25c    ; check system readiness → AL
//   JZ   return_false
//   LEA  ESI,[EBX+0x24] / MOV EDI,0x800 / CALL 0x004067f8   ; alloc mesh_list
//   LEA  ESI,[EBX+0x2c]                 / CALL 0x004067f8   ; alloc renderable_list
//   LEA  ESI,[EBX+0x8c] / MOV EDI,0x400 / MOV byte[EBX+0x88],0 / CALL 0x004067f8
//   LEA  ESI,[EBX+0x94]                 / CALL 0x004067f8
//   LEA  ESI,[EBX+0x9c]                 / CALL 0x004067f8
//   … init several linked-list fields (0x004067f8 / 0x0040b321 / 0x0040b38a / 0x0040b430 / 0x0040b4cb)
//   AND  dword[EBX+0xa44], 0            ; clear init flag
//   MOV  ECX,0x9c94f0                   ; "$DefaultTexture" string const
//   CALL 0x0067e6d8                      ; construct name
//   … alloc and store DefaultTexture at [EBX+0x84]
//   MOV  AL, 1
// ---------------------------------------------------------------------------
typedef void* (__cdecl* PFN_AllocArray)(void** out_ptr, uint32_t count);
static PFN_AllocArray g_pfn_AllocArray = NULL; // 0x004067f8

int MgRenderer_Init(MgRenderer* renderer)
{
    if (!renderer) return 0;

    // Clear flag
    // (game: AND dword[EBX+0xa44], 0)
    // *(uint32_t*)((uint8_t*)renderer + 0xa44) = 0;

    if (!g_pfn_AllocArray) return 0; // need game binary

    g_pfn_AllocArray(&renderer->mesh_list,       0x800);
    g_pfn_AllocArray(&renderer->renderable_list, 0x800);
    renderer->_flag_88 = 0;
    g_pfn_AllocArray(&renderer->material_list_a, 0x400);
    g_pfn_AllocArray(&renderer->material_list_b, 0x400);
    g_pfn_AllocArray(&renderer->material_list_c, 0x400);

    // Note: several more linked-list inits follow (0x0040b321/b38a/b430/b4cb)
    // and DefaultTexture construction — omitted pending further analysis.

    return 1;
}

// ---------------------------------------------------------------------------
// FUN_0089eefc — MgEngine_InitThreadDescriptors
//
// Zeroes 0x1280 bytes at 0xa4a6f8 then writes each thread descriptor.
// Thread name string ptrs are stored in game .rdata — documented here.
//
// Thread block layout (offset from block base, each block = 0x20 bytes):
//   +0x00  name ptr  (const char* in .rdata)
//   +0x04  0
//   +0x08  0
//   +0x0c  stack_size (0x10000 or 0x18000)
//   +0x10  runtime value (set from a stack arg at call time)
//   +0x14  affinity mask (-1 = all)
//   +0x18  flag (1)
//
// Additional task descriptors written after each thread block describe
// the per-thread task entry points (init/tick/shutdown function pointers).
//
// This function should only be called with the game binary loaded;
// in standalone mode it is a no-op.
// ---------------------------------------------------------------------------
void MgEngine_InitThreadDescriptors(void)
{
    // Thread base address (game virtual address).
    // Writing to absolute VAs only works with the game binary mapped.
    // Documented thread names and addresses for reference:
    //
    // 0xa4a6f8  "Main thread (rendering)"   stack=0x10000  vtable=0x9c7434
    // 0xa4a718  "Render task"               fn=0x89eb9a/0x89e693/0x89e5e3
    // 0xa4a940  "Physics manager thread"     stack=0x18000  vtable=0x9c7458
    //              priority=5  fn=0x85b3ee/0x859b7b/0x85b1a5
    // 0xa4ab90  "Loading thread"            stack=0x10000  vtable=0x9c7488
    //              priority=4  fn=0x8a0804/0x8a07ff/0x8a1124(video!)
    // 0xa4ade0  "Input thread"              stack=0x10000  vtable=0x9c74a8
    //              priority=5  fn=0x7da315/0x7d9cfc/0x7d9ecb
    // 0xa4ae08  "Game thread"               stack=0x10000  vtable=0x9c74b8
    //              fn=0x89e829/0x89e7d1/0x89ec51
    //              data=0xa4b978
    // 0xa4b038  Task="Game task"  vtable=0x9c74cc
    //              fn2=0x89e829/0x89e7d1/0x89ec51
    //
    // Video thread FUN_008a1124 is referenced at 0xa4abc4.
    // __controlfp_s is called to set FPU precision after all descriptors.
    //
    // In standalone reconstruction: no-op.
    (void)0;
}
