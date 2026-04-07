// MgFX.cpp
// Reconstructed from:
//   FX/Particle/  (4 files)    — SortParticles, BindControllerStates, PhysicsController
//   FX/Emitter/   (13 files)   — Team1/2PlayerEmitter, Effectors, EmitterEffectors,
//                                EmittersMax/Min, Enabled/Defs, HeroEmitters, atexit x4
//   FX/Effect/    (8 files)    — PblCRC/Effect/VisualEffect, LoadFromTemplate,
//                                AmmoEffect_Update, atexit x3 + accessor
//   FX/Lightning/ (4 files)    — LoadProps, GetLocation, FXTest, atexit
//   FX/CameraFX/  (8 files)    — 8 atexit registrations
//   FX/GameEffects/(4 files)   — 4 atexit registrations

#include "MgFX.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Game function pointers (set before calling any function that uses them)
// ---------------------------------------------------------------------------
typedef void  (__cdecl*  PFN_StringCopyConstruct)(const void* src, void* out);
typedef int   (__cdecl*  PFN_FindTypedParam)(void* key1, void* data, void* key2);
typedef void  (__cdecl*  PFN_Atexit)(void (*fn)(void));
typedef float (__cdecl*  PFN_FloatReader)(const void* data, const char* key);
typedef int   (__cdecl*  PFN_BoolReader)(const void* data, int def, const char* key);
typedef void* (__cdecl*  PFN_TypeGetClassName)(uint32_t type_handle, const void* key,
                                               void* out_name);
typedef void* (__cdecl*  PFN_Alloc)(size_t bytes);

static PFN_StringCopyConstruct g_pfn_CopyConstruct  = NULL; // 0x0067e6d8
static PFN_FindTypedParam      g_pfn_FindTypedParam  = NULL; // 0x007e866b
static PFN_Atexit              g_pfn_atexit          = NULL; // 0x0060d709
static PFN_FloatReader         g_pfn_FloatReader     = NULL; // 0x008a66ba
static PFN_BoolReader          g_pfn_BoolReader      = NULL; // 0x008a65f1
static PFN_TypeGetClassName    g_pfn_GetClassName    = NULL; // 0x008a69ac
static PFN_Alloc               g_pfn_Alloc           = NULL; // 0x0067e610

// Global registration counter (game binary at 0x00a355b0)
static uint32_t s_sampler_counter = 0;

// ---------------------------------------------------------------------------
// Helper: standard bool property lookup  (EDI = entry, returns byte at offset+0x10)
// ---------------------------------------------------------------------------
static int bool_prop_lookup(const MgAnimBlendEntry* entry,
                             const void* type_key_rdata,
                             const void* field_key_rdata)
{
    if (!entry) return 0;
    const void* blend_data = entry->blend_data;
    if (!blend_data) return 0;
    if (!g_pfn_FindTypedParam) return 0;

    int offset = g_pfn_FindTypedParam((void*)type_key_rdata,
                                       (void*)blend_data,
                                       (void*)field_key_rdata);
    if (offset == -1) return 0;

    return (int)(*(const uint8_t*)((const uint8_t*)entry + offset + 0x10));
}

// ---------------------------------------------------------------------------
// Helper: uint32_t property lookup  (returns value at offset+0x10)
// ---------------------------------------------------------------------------
static uint32_t uint32_prop_lookup(const MgAnimBlendEntry* entry,
                                    const void* type_key_rdata,
                                    const void* field_key_rdata,
                                    uint32_t default_val)
{
    if (!entry || !entry->blend_data || !g_pfn_FindTypedParam)
        return default_val;

    int offset = g_pfn_FindTypedParam((void*)type_key_rdata,
                                       entry->blend_data,
                                       (void*)field_key_rdata);
    if (offset == -1) return default_val;

    return *(const uint32_t*)((const uint8_t*)entry + offset + 0x10);
}

// ---------------------------------------------------------------------------
// Helper: array pointer lookup  (returns ptr to array struct at offset+0x10)
// ---------------------------------------------------------------------------
static void* array_ptr_lookup(const MgAnimBlendEntry* entry,
                               const void* type_key_rdata,
                               const void* field_key_rdata,
                               void* empty_sentinel)
{
    if (!entry || !entry->blend_data || !g_pfn_FindTypedParam)
        return empty_sentinel;

    int offset = g_pfn_FindTypedParam((void*)type_key_rdata,
                                       entry->blend_data,
                                       (void*)field_key_rdata);
    if (offset == -1) return empty_sentinel;

    return (void*)((const uint8_t*)entry + offset + 0x10);
}

// ===========================================================================
// PARTICLE
// ===========================================================================

// FUN_00406800
// ASM: .rdata 0x9cead0 = type key ("PblCRC"), 0x9e74dc = field key ("SortParticles")
int MgParticle_GetSortParticles(const MgAnimBlendEntry* entry)
{
    // Type key: 0x9cead0 ("PblCRC"), Field key: 0x9e74dc ("SortParticles")
    static const void* k_type  = (const void*)0x9cead0;
    static const void* k_field = (const void*)0x9e74dc;
    return bool_prop_lookup(entry, k_type, k_field);
}

// FUN_0044959e
// .rdata: 0x9cead0 = "PblCRC", 0x9e7568 = "BindControllerStatesToLifespanOfParticle"
int MgParticle_GetBindControllerStatesToLifespan(const MgAnimBlendEntry* entry)
{
    static const void* k_type  = (const void*)0x9cead0;
    static const void* k_field = (const void*)0x9e7568;
    return bool_prop_lookup(entry, k_type, k_field);
}

// FUN_009560fb — MgPhysicsControllerParticle_Register  (atexit, 57 addr)
// .rdata 0x9c9dac = "MgPhysicsControllerParticle"
// Struct at [0xcdfc8c]: vtable=0xcf78e8, name_str=[EBP-4 copy], active=0
// Cleanup fn at 0x96e495.
static MgFXSamplerEntry s_PhysicsControllerParticle;  // mirrors [0xcdfc8c]

void MgPhysicsControllerParticle_Register(void)
{
    static const void* k_name = (const void*)0x9c9dac;
    if (!g_pfn_CopyConstruct || !g_pfn_atexit) return;

    // In the game: CopyConstruct(0x9c9dac, &tmp), store tmp in struct
    // s_PhysicsControllerParticle mirrors [0xcdfc8c]
    s_PhysicsControllerParticle.type_ptr  = (void*)0xcf78e8;
    s_PhysicsControllerParticle.name_str  = NULL; // filled by CopyConstruct in game
    s_PhysicsControllerParticle.active    = 0;
    // g_pfn_atexit(cleanup_0x96e495);  // registered in game
}

// FUN_0095615b — 43 addr — calls 0x009409c1 (type system lookup by class)
void* MgPhysicsControllerParticle_GetInstance(void)
{
    // In the game: pushes name ptr, ESI=0xce0edc, calls 0x009409c1
    // Standalone: return NULL
    return NULL;
}

// ===========================================================================
// EMITTER
// ===========================================================================

// FUN_0042e595 — GetTeam2PlayerEmitter
// EAX = obj ptr; [EAX+0x150] = player_team_data
// .rdata: 0x9cb648 = "MgHandle", 0x9cd268 = "Team2PlayerEmitter"
// Default if no team data: [0xcd82f8] (null handle)
uint32_t MgEmitter_GetTeam2PlayerEmitter(const MgEmitterObject* obj, uint32_t* out)
{
    if (!out) return 0;

    static const void* k_type  = (const void*)0x9cb648; // "MgHandle"
    static const void* k_field = (const void*)0x9cd268; // "Team2PlayerEmitter"

    const void* team_data = obj ? obj->player_team_data : NULL;
    if (!team_data || !g_pfn_FindTypedParam) {
        *out = 0; // game returns [0xcd82f8] — null handle sentinel
        return (uint32_t)(uintptr_t)out;
    }

    const MgAnimBlendEntry fake_entry = { NULL, (void*)team_data, NULL };
    int offset = g_pfn_FindTypedParam((void*)k_type, (void*)team_data, (void*)k_field);
    if (offset == -1) {
        *out = 0;
        return (uint32_t)(uintptr_t)out;
    }
    *out = *(const uint32_t*)((const uint8_t*)team_data + offset + 0x10);
    return (uint32_t)(uintptr_t)out;
}

// FUN_0042f3a9 — GetTeam1PlayerEmitter
// .rdata: 0x9cb648 = "MgHandle", 0x9cd254 = "Team1PlayerEmitter"
uint32_t MgEmitter_GetTeam1PlayerEmitter(const MgEmitterObject* obj, uint32_t* out)
{
    if (!out) return 0;

    static const void* k_type  = (const void*)0x9cb648;
    static const void* k_field = (const void*)0x9cd254; // "Team1PlayerEmitter"

    const void* team_data = obj ? obj->player_team_data : NULL;
    if (!team_data || !g_pfn_FindTypedParam) {
        *out = 0;
        return (uint32_t)(uintptr_t)out;
    }

    int offset = g_pfn_FindTypedParam((void*)k_type, (void*)team_data, (void*)k_field);
    if (offset == -1) {
        *out = 0;
        return (uint32_t)(uintptr_t)out;
    }
    *out = *(const uint32_t*)((const uint8_t*)team_data + offset + 0x10);
    return (uint32_t)(uintptr_t)out;
}

// FUN_00448b00 — GetEffectors
// .rdata: 0x9cbddc = "MgTemplateArrayT<MgHandle>", 0x9e7990 = "Effectors"
// Default: 0xe94a8c (empty array sentinel)
void* MgEmitter_GetEffectors(const MgAnimBlendEntry* entry)
{
    static const void* k_type  = (const void*)0x9cbddc;
    static const void* k_field = (const void*)0x9e7990;
    static void* k_empty = (void*)0xe94a8c;
    return array_ptr_lookup(entry, k_type, k_field, k_empty);
}

// FUN_00455505 — GetEmitterEffectors
// .rdata: 0x9cbddc = "MgTemplateArrayT<MgHandle>", 0x9e799c = "EmitterEffectors"
void* MgEmitter_GetEmitterEffectors(const MgAnimBlendEntry* entry)
{
    static const void* k_type  = (const void*)0x9cbddc;
    static const void* k_field = (const void*)0x9e799c;
    static void* k_empty = (void*)0xe94a8c;
    return array_ptr_lookup(entry, k_type, k_field, k_empty);
}

// FUN_004781b9 — GetEmittersMax
// .rdata: 0x9ca6e4 = type key, 0x9e786c = "EmittersMax". Default: 1
uint32_t MgEmitter_GetEmittersMax(const MgAnimBlendEntry* entry)
{
    static const void* k_type  = (const void*)0x9ca6e4;
    static const void* k_field = (const void*)0x9e786c;
    return uint32_prop_lookup(entry, k_type, k_field, 1u);
}

// FUN_00ea4480 — GetEmittersMin
// .rdata: 0x9ca6e4, 0x9e7860 = "EmittersMin". Default: 1
uint32_t MgEmitter_GetEmittersMin(const MgAnimBlendEntry* entry)
{
    static const void* k_type  = (const void*)0x9ca6e4;
    static const void* k_field = (const void*)0x9e7860;
    return uint32_prop_lookup(entry, k_type, k_field, 1u);
}

// FUN_0077eaac — LoadEmitterDefinitions  (228 addr)
//
// Reads three MgHandle-type fields from template_block:
//   "Emitter_Self_Definition"    → stored at emitter_state+0x0c
//   "Emitter_Object_Definition"  → stored at emitter_state+0x10
//   "Receiver_Self_Definition"   → stored at emitter_state+0x14
// Also reads bool "Enabled" (with default=1) from template_block.
// Returns 1 if any field was set or Enabled is true; 0 otherwise.
//
// Sub-calls:
//   0x0042844c — check if template has field by name
//   0x007a0f5d — get field value
//   0x0042c081 — validate handle
//   0x0044332a — copy handle
//   0x008a65f1 — bool reader (Enabled)
//
// In standalone: no-op, returns 0.
int MgEmitter_LoadDefinitions(void* emitter_state, const void* template_block)
{
    (void)emitter_state;
    (void)template_block;
    // Full implementation requires sub-function 0x0042844c (template field check)
    // and 0x007a0f5d (get field value) — deferred pending those sub-functions.
    return 0;
}

// FUN_009400dd — InitHeroEmitters  (196 addr)
//
// Takes team_data (arg0), output emitter object (EDI).
// Sets EDI+0x1a8 = vtable ptr 0x9d13a8, EDI+0x1ac = 0x9d1440 (name strings).
// Sets EDI+0x150 = team_data ptr.
// Then reads "MgHandle"/"Team1HeroEmitter" → EDI+0x160
//      and  "MgHandle"/"Team2HeroEmitter"  → EDI+0x164
// .rdata: 0x9cb648="MgHandle", 0x9d12f0="Team1HeroEmitter", 0x9d1304="Team2HeroEmitter"
// Default for each: [0xcd82f8] (null handle).
MgEmitterObject* MgEmitter_InitHeroEmitters(const void* team_data, MgEmitterObject* out)
{
    if (!out) return NULL;

    out->player_team_data = (void*)team_data;

    static const void* k_type   = (const void*)0x9cb648;
    static const void* k_hero1  = (const void*)0x9d12f0;
    static const void* k_hero2  = (const void*)0x9d1304;

    out->team1_hero_emitter = 0; // default: null handle [0xcd82f8]
    out->team2_hero_emitter = 0;

    if (!team_data || !g_pfn_FindTypedParam) return out;

    const void* blend_data = *(const void**)((const uint8_t*)team_data + 0x4);
    if (!blend_data) return out;

    int off1 = g_pfn_FindTypedParam((void*)k_type, (void*)blend_data, (void*)k_hero1);
    if (off1 != -1)
        out->team1_hero_emitter = *(const uint32_t*)((const uint8_t*)team_data + off1 + 0x10);

    int off2 = g_pfn_FindTypedParam((void*)k_type, (void*)blend_data, (void*)k_hero2);
    if (off2 != -1)
        out->team2_hero_emitter = *(const uint32_t*)((const uint8_t*)team_data + off2 + 0x10);

    return out;
}

// ---------------------------------------------------------------------------
// Emitter atexit registrations  (57–66 addr each)
//
// All follow the pattern:
//   CopyConstruct(name_rdata, &tmp)
//   [struct+0x00] = chain_head_ptr or unique vtable
//   [struct+0x04] = name string copy
//   [struct+0x08] = counter_byte (from s_sampler_counter++)  OR  0
//   atexit(cleanup_fn)
//
// MgResourceEmitter: struct at [0xce1d20], chain [0xce12c0]
// MgSpawnEmitter:    struct at [0xce1d60], chain [0xce12c0]
// MgSoundEmitter:    struct at [0xcf7828], chain [0xce12c0]
// MgBC_SetHeroEmitter: struct at [0xcff984?] (confirmed below from FUN_00965f58 chain)
//                     Actually 0x9f0398 name, chain = [0xcff984] based on CamEffect chain
// ---------------------------------------------------------------------------

static MgFXSamplerEntry s_ResourceEmitter;
static MgFXSamplerEntry s_SpawnEmitter;
static MgFXSamplerEntry s_SoundEmitter;
static MgFXSamplerEntry s_BC_SetHeroEmitter;

void MgResourceEmitter_Register(void)
{
    // FUN_0095778e: name=0x9cd2ac, [0xce1d20] chain→[0xce12c0], counter byte
    s_ResourceEmitter.type_ptr = (void*)0xce12c0;
    s_ResourceEmitter.name_str = NULL;
    s_ResourceEmitter.active   = (uint8_t)(s_sampler_counter++);
    // In game: also calls atexit(cleanup_0x96e612)
}

void MgSpawnEmitter_Register(void)
{
    // FUN_0095789b: name=0x9cd494, [0xce1d60] chain→[0xce12c0]
    s_SpawnEmitter.type_ptr = (void*)0xce12c0;
    s_SpawnEmitter.name_str = NULL;
    s_SpawnEmitter.active   = 0;
    // In game: atexit cleanup registered
}

void MgSoundEmitter_Register(void)
{
    // FUN_00959ffd: name=0x9d1a00, [0xcf7828] chain→[0xce12c0]
    s_SoundEmitter.type_ptr = (void*)0xce12c0;
    s_SoundEmitter.name_str = NULL;
    s_SoundEmitter.active   = 0;
}

void MgBC_SetHeroEmitter_Register(void)
{
    // FUN_00965ca0: name=0x9f0398
    s_BC_SetHeroEmitter.type_ptr = NULL;
    s_BC_SetHeroEmitter.name_str = NULL;
    s_BC_SetHeroEmitter.active   = 0;
}

void* MgSoundEmitter_GetInstance(void)
{
    // FUN_0095a05f: calls 0x009409c1(0xcf7838) — type system accessor
    return NULL; // requires game binary
}

// ===========================================================================
// EFFECT
// ===========================================================================

// FUN_0049cfec — GetEffect  (85 addr)
// .rdata: 0x9cb660 = type key 0 (first in pbl type list), 0x9d57a8 = "Effect"
// Default: [0xe560e0] (type system null CRC)
uint32_t MgEffect_GetEffect(const MgAnimBlendEntry* entry, uint32_t* out)
{
    if (!out) return 0;

    static const void* k_type  = (const void*)0x9cb660;
    static const void* k_field = (const void*)0x9d57a8;

    if (!entry || !entry->blend_data || !g_pfn_FindTypedParam) {
        *out = 0; // game returns [0xe560e0]
        return (uint32_t)(uintptr_t)out;
    }

    int offset = g_pfn_FindTypedParam((void*)k_type, entry->blend_data, (void*)k_field);
    if (offset == -1) {
        *out = 0;
        return (uint32_t)(uintptr_t)out;
    }
    *out = *(const uint32_t*)((const uint8_t*)entry + offset + 0x10);
    return (uint32_t)(uintptr_t)out;
}

// FUN_00ea54b0 — GetVisualEffect  (85 addr)
// .rdata: 0x9cb660, 0x9f18ac = "VisualEffect"
uint32_t MgEffect_GetVisualEffect(const MgAnimBlendEntry* entry, uint32_t* out)
{
    if (!out) return 0;

    static const void* k_type  = (const void*)0x9cb660;
    static const void* k_field = (const void*)0x9f18ac;

    if (!entry || !entry->blend_data || !g_pfn_FindTypedParam) {
        *out = 0;
        return (uint32_t)(uintptr_t)out;
    }

    int offset = g_pfn_FindTypedParam((void*)k_type, entry->blend_data, (void*)k_field);
    if (offset == -1) {
        *out = 0;
        return (uint32_t)(uintptr_t)out;
    }
    *out = *(const uint32_t*)((const uint8_t*)entry + offset + 0x10);
    return (uint32_t)(uintptr_t)out;
}

// FUN_008891c8 — MgEffect_LoadFromTemplate  (675 addr)
//
// Reads from template_src:
//   1. Float "Duration"  via 0x008a66ba  → stored at out_entry+0x0c
//   2. Bool  "EndStage"  via 0x008a65f1  → bit0 of out_entry+0x18
//   3. Type enum         via 0x008a69ac  → stored at out_entry+0x10
//   4. Iterates "Effects" array:
//        counts matching entries, stores count at out_entry+0x14 (uint16_t),
//        allocates count*0x10 bytes via 0x0067e610
//        stores alloc ptr at out_entry+0x08
//   5. Same pass for "Components":
//        count at out_entry+0x?? (separate uint16_t)
//
// String keys (from .rdata in game):
//   0x9c8510 = "Duration"
//   0x9c851c = "EndStage" (bool, default false)
//   0x9c8524 = type key for type enum
//   0x9c8530 = "Effects"
//
// Sub-calls:
//   0x008a66ba — float reader  (template_data, key) → XMM0
//   0x008a65f1 — bool reader   (template_data, default, key) → AL
//   0x008a69ac — MgTypeSystem_GetClassName
//   0x008a6413 — iterator_start (data, default, key) → AL (has_more)
//   0x008a63d3 — iterator_advance → AL (valid)
//   0x008a63c5 — iterator_next
//   0x0067e610 — allocator
//
// In standalone: no-op (requires game binary sub-functions).
void MgEffect_LoadFromTemplate(const void* template_src, MgEffectEntry* out_entry)
{
    if (!out_entry) return;

    // Zero output entry
    memset(out_entry, 0, sizeof(MgEffectEntry));

    if (!template_src) return;
    if (!g_pfn_FloatReader || !g_pfn_BoolReader || !g_pfn_GetClassName || !g_pfn_Alloc)
        return;

    // In the game binary this function calls 0x008a66ba for Duration,
    // 0x008a65f1 for EndStage, then iterates the Effects and Components arrays.
    // Deferred: requires iterator sub-functions 0x008a6413/63d3/63c5 to be analyzed.
    //
    // Known field string addresses:
    //   Duration:  [0x9c8510]
    //   EndStage:  [0x9c851c]
    //   Effects:   [0x9c8530]
    //   Components: (adjacent to Effects in .rdata)
    (void)template_src;
}

// FUN_00831b19 — MgAmmoEffect_Update  (521 addr)
//
// Thiscall (ECX = ammo object).
// Early out if bit1 of [ECX+0x244] is set (flag: no ammo effect active).
// Reads vtable[0x64] to check ammo type, calls 0x0083134d (spawn sub).
// Complex weapon/ammo state machine — requires vtable + world ptrs.
// Stub pending full struct analysis.
void MgAmmoEffect_Update(void* ammo_obj, const void* world_state)
{
    (void)ammo_obj;
    (void)world_state;
    // TODO: implement after MgAmmoObject vtable layout is recovered
}

// ---------------------------------------------------------------------------
// Effect atexit registrations
// ---------------------------------------------------------------------------

static MgFXSamplerEntry s_EffectController;
static MgFXSamplerEntry s_GameEffectArea;
static MgFXSamplerEntry s_EffectInstanceGameObject;

void MgEffectController_Register(void)
{
    // FUN_009580bd: name=0x9cddfc, [0xcf0fc4] chain→[0xcf78e8], active=0
    s_EffectController.type_ptr = (void*)0xcf78e8;
    s_EffectController.name_str = NULL;
    s_EffectController.active   = 0;
}

void MgGameEffectArea_Register(void)
{
    // FUN_00958154: name=0x9cde54, [0xcf7024] chain→[0xce12c0]
    s_GameEffectArea.type_ptr = (void*)0xce12c0;
    s_GameEffectArea.name_str = NULL;
    s_GameEffectArea.active   = 0;
}

void MgEffectInstanceGameObject_Register(void)
{
    // FUN_00964570: name=0x9e7258, [0xcfd760] chain→[0xce12c0]
    s_EffectInstanceGameObject.type_ptr = (void*)0xce12c0;
    s_EffectInstanceGameObject.name_str = NULL;
    s_EffectInstanceGameObject.active   = 0;
}

void* MgEffectInstanceGameObject_GetInstance(void)
{
    // FUN_009645d2: ESI=0xcfd778, calls 0x009409c1
    return NULL;
}

// ===========================================================================
// LIGHTNING
// ===========================================================================

// FUN_0078637a — MgLightning_LoadProps  (76 addr)
//
// Reads from template_block via game sub-functions:
//   "MaxTimeToLive" (float, default=[0x9c07cc]) → multiply by [0x9ffd58] (ticks/sec)
//                   → store as uint32_t at out+0x18
//   "AcceptLightning" (bool, default=1) via 0x008a65f1
//                   → bit0 of out+0x1c
//
// Sub-calls:
//   0x008a66ba — float reader (data, key) → XMM0
//   0x00610786 — float-to-ticks converter (FLD float, FMUL [0x9ffd58], CALL) → EAX
//   0x008a65f1 — bool reader (data, 1, key) → AL
//
// .rdata: 0x9e5588="MaxTimeToLive", 0x9e5598="AcceptLightning", 0x9c07cc=default_float
void MgLightning_LoadProps(const void* template_block, MgLightningObject* out)
{
    if (!out) return;

    // Defaults
    out->max_time_to_live = 0;
    out->accept_lightning = 1;

    if (!template_block) return;
    if (!g_pfn_FloatReader || !g_pfn_BoolReader) return;

    // In game: FLD [0x9c07cc] default, call 0x008a66ba → XMM0
    // FMUL [0x9ffd58] → ticks, call 0x00610786 → EAX → [EDI+0x18]
    // Then bool read for AcceptLightning with default=1 → bit0 of [EDI+0x1c]
    // Deferred: requires 0x00610786 (float-to-ticks) sub.
    (void)template_block;
}

// FUN_008293a6 — MgLightning_GetLocation  (195 addr)
//
// Once-init static at 0xe67ef0: copies string "LightningLocation" (0x9cd08c)
// into the static on first call (flag at 0xe67ef4).
//
// Resolves lightning source position:
//   - Calls 0x0084c3ad(entity_ptr) to get entity world object → EAX
//   - If NULL: reads position from entity via 0x0084c3f3, offset +0x30 → 4 floats
//   - If not NULL: calls 0x008fca65(EAX, &list) to get node list,
//       uses round-robin index [lightning_obj_ptr+0] to pick node,
//       calls 0x0084c9cf to get node world position → 4 floats at out_pos
void MgLightning_GetLocation(const void* entity_ptr,
                              const void* lightning_obj_ptr,
                              float       out_pos[4])
{
    if (!out_pos) return;
    out_pos[0] = out_pos[1] = out_pos[2] = out_pos[3] = 0.0f;

    // Requires game entity accessor 0x0084c3ad and 0x008fca65 — deferred.
    (void)entity_ptr;
    (void)lightning_obj_ptr;
}

// FUN_008298cd — MgLightning_FXTest  (353 addr)
//
// Main per-frame lightning update:
//   1. Once-init: if [obj+0x2988] == NULL, looks up "fx_lightning_test" string
//      via [0xcd7ea0] resolver and calls 0x0087603a to allocate chain.
//   2. Iterates 64 sub-objects at [obj + n*0xa0]:
//        checks [obj+n*0xa0+0x8c] (active flag)
//        if active: calls 0x00827c8f → accumulates count
//   3. If count > 0: allocates count*0x24 bytes, calls 0x0068c124
//      (zero-init vb), stores at [obj+0x2800], sets [obj+0x104] and [obj+0x108].
//   4. Calls sub 0x0082ea92 etc. to build lightning strip geometry.
//
// .rdata: 0x9cd078 = "fx_lightning_test"
void MgLightning_FXTest(MgLightningObject* lightning_obj)
{
    if (!lightning_obj) return;
    // Full implementation requires geometry sub-functions 0x00827c8f, 0x0082ea92.
    // Deferred pending those sub-functions' analysis.
}

// FUN_0095764e — atexit registration  (57 addr)
// name=0x9cd064 ("MgLightningFXObject"), struct at [0xce1ca0], chain→[0xce12c0]
static MgFXSamplerEntry s_LightningFXObject;

void MgLightningFXObject_Register(void)
{
    s_LightningFXObject.type_ptr = (void*)0xce12c0;
    s_LightningFXObject.name_str = NULL;
    s_LightningFXObject.active   = 0;
}

// ===========================================================================
// CAMERA FX — 8 atexit registrations
//
// Two chains:
//   Chain A: MgCamEffect [0xcff824] (vtable 0xe568f4) → MgCamShakeEffect [0xcff830]
//            → MgCamMassSpringEffect [0xcff83c] → MgCamRollShakeEffect [0xcff848]
//   Chain B: MgCamFOVEffect [0xcff854] (vtable 0xe568f4) → MgFOVBlendToEffect [0xcff860]
//            → MgFOVResetEffect [0xcff86c] → MgFOVClickEffect [0xcff878]
//
// Type name .rdata addresses (from MOV ECX, 0x9fXXXX in each function):
//   MgCamEffect:           0x9f06b8
//   MgCamShakeEffect:      0x9f06c4
//   MgCamMassSpringEffect: 0x9f06d8
//   MgCamRollShakeEffect:  0x9f06f0
//   MgCamFOVEffect:        0x9f0708
//   MgFOVBlendToEffect:    0x9f0718
//   MgFOVResetEffect:      0x9f072c
//   MgFOVClickEffect:      0x9f0740
// ===========================================================================

static MgFXSamplerEntry s_CamEffect;
static MgFXSamplerEntry s_CamShakeEffect;
static MgFXSamplerEntry s_CamMassSpringEffect;
static MgFXSamplerEntry s_CamRollShakeEffect;
static MgFXSamplerEntry s_CamFOVEffect;
static MgFXSamplerEntry s_FOVBlendToEffect;
static MgFXSamplerEntry s_FOVResetEffect;
static MgFXSamplerEntry s_FOVClickEffect;

void MgCamEffect_Register(void)
{
    // FUN_00965f58: name=0x9f06b8, [0xcff824] vtable=0xe568f4
    s_CamEffect.type_ptr = (void*)0xe568f4;
    s_CamEffect.name_str = NULL;
    s_CamEffect.active   = 0;
}

void MgCamShakeEffect_Register(void)
{
    // FUN_00965f91: name=0x9f06c4, chain→[0xcff824]
    s_CamShakeEffect.type_ptr = &s_CamEffect; // mirrors [0xcff824]
    s_CamShakeEffect.name_str = NULL;
    s_CamShakeEffect.active   = 0;
}

void MgCamMassSpringEffect_Register(void)
{
    // FUN_00965fca: name=0x9f06d8, chain→[0xcff824]
    s_CamMassSpringEffect.type_ptr = &s_CamEffect;
    s_CamMassSpringEffect.name_str = NULL;
    s_CamMassSpringEffect.active   = 0;
}

void MgCamRollShakeEffect_Register(void)
{
    // FUN_00966003: name=0x9f06f0, chain→[0xcff824]
    s_CamRollShakeEffect.type_ptr = &s_CamEffect;
    s_CamRollShakeEffect.name_str = NULL;
    s_CamRollShakeEffect.active   = 0;
}

void MgCamFOVEffect_Register(void)
{
    // FUN_0096603c: name=0x9f0708, [0xcff854] vtable=0xe568f4 (new chain root)
    s_CamFOVEffect.type_ptr = (void*)0xe568f4;
    s_CamFOVEffect.name_str = NULL;
    s_CamFOVEffect.active   = 0;
}

void MgFOVBlendToEffect_Register(void)
{
    // FUN_00966075: name=0x9f0718, chain→[0xcff854]
    s_FOVBlendToEffect.type_ptr = &s_CamFOVEffect;
    s_FOVBlendToEffect.name_str = NULL;
    s_FOVBlendToEffect.active   = 0;
}

void MgFOVResetEffect_Register(void)
{
    // FUN_009660ae: name=0x9f072c, chain→[0xcff854]
    s_FOVResetEffect.type_ptr = &s_CamFOVEffect;
    s_FOVResetEffect.name_str = NULL;
    s_FOVResetEffect.active   = 0;
}

void MgFOVClickEffect_Register(void)
{
    // FUN_009660e7: name=0x9f0740, chain→[0xcff854]
    s_FOVClickEffect.type_ptr = &s_CamFOVEffect;
    s_FOVClickEffect.name_str = NULL;
    s_FOVClickEffect.active   = 0;
}

// ===========================================================================
// GAME EFFECTS — 4 atexit registrations
//
// Chain: MgGM_EffectStatusUpdate [0xcffa38] → MgGM_ApplyGameEffectGroup [0xcffa44]
//        → MgGM_DeactivateGameEffectGroup [0xcffa50]
// MgGlobalGameEffects [0xd017e4]: uses global counter byte at [0xa355b0]
//
// Type name .rdata addresses:
//   MgGM_EffectStatusUpdate:          0x9f1078
//   MgGM_ApplyGameEffectGroup:        0x9f1090
//   MgGM_DeactivateGameEffectGroup:   0x9f10ac
//   MgGlobalGameEffects:              0x9f14f0
// ===========================================================================

static MgFXSamplerEntry s_GM_EffectStatusUpdate;
static MgFXSamplerEntry s_GM_ApplyGameEffectGroup;
static MgFXSamplerEntry s_GM_DeactivateGameEffectGroup;
static MgFXSamplerEntry s_GlobalGameEffects;

void MgGM_EffectStatusUpdate_Register(void)
{
    // FUN_0096687d: name=0x9f1078, [0xcffa38] chain→[0xcff984] (SetHeroEmitter chain)
    s_GM_EffectStatusUpdate.type_ptr = (void*)0xcff984; // chain root
    s_GM_EffectStatusUpdate.name_str = NULL;
    s_GM_EffectStatusUpdate.active   = 0;
}

void MgGM_ApplyGameEffectGroup_Register(void)
{
    // FUN_009668b6: name=0x9f1090, chain→[0xcff984]
    s_GM_ApplyGameEffectGroup.type_ptr = (void*)0xcff984;
    s_GM_ApplyGameEffectGroup.name_str = NULL;
    s_GM_ApplyGameEffectGroup.active   = 0;
}

void MgGM_DeactivateGameEffectGroup_Register(void)
{
    // FUN_009668ef: name=0x9f10ac, chain→[0xcff984]
    s_GM_DeactivateGameEffectGroup.type_ptr = (void*)0xcff984;
    s_GM_DeactivateGameEffectGroup.name_str = NULL;
    s_GM_DeactivateGameEffectGroup.active   = 0;
}

void MgGlobalGameEffects_Register(void)
{
    // FUN_0096718a: name=0x9f14f0, [0xd017e4] chain→[0xce12c0]
    // Uses s_sampler_counter (mirrors [0xa355b0]) as the active byte field.
    s_GlobalGameEffects.type_ptr = (void*)0xce12c0;
    s_GlobalGameEffects.name_str = NULL;
    s_GlobalGameEffects.active   = (uint8_t)(s_sampler_counter++);
}
