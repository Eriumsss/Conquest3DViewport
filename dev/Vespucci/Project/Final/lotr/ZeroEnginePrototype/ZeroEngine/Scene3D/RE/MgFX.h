// MgFX.h
// Reconstructed FX subsystem: Particles, Emitters, Effects, Lightning,
// Camera FX, and Game Effects.
//
// Sources (FX/ subsystem ASM dumps):
//   Particle/     FUN_00406800, 0044959e, 009560fb, 0095615b
//   Emitter/      FUN_0042e595, 0042f3a9, 00448b00, 00455505, 004781b9,
//                 0077eaac, 009400dd, 0095778e, 0095789b, 00959ffd, 0095a05f,
//                 00965ca0, 00ea4480
//   Effect/       FUN_0049cfec, 00831b19, 008891c8, 009580bd, 00958154,
//                 00964570, 009645d2, 00ea54b0
//   Lightning/    FUN_0078637a, 008293a6, 008298cd, 0095764e
//   CameraFX/     FUN_00965f58–009660e7  (8 atexit registrations)
//   GameEffects/  FUN_0096687d–0096718a  (4 atexit registrations)
//
// Calling convention notes:
//   Many accessors use EDI as implicit self (non-standard), ECX receives
//   the .rdata string address for CopyConstruct.
//   Atexit registrations use 0x0060d709 (_atexit wrapper).
//   Property lookup: find_typed_param (0x007e866b): int __cdecl(key1, data, key2)
//
// Global addresses referenced:
//   0x0067e6d8  Mg::String::CopyConstruct
//   0x007e866b  find_typed_param
//   0x0060d709  _atexit wrapper
//   0x00cd82f8  NULL MgHandle sentinel (default return for handle lookups)
//   0x00e560e0  type system default CRC / NULL CRC sentinel
//   0x00a355b0  global sampler registration counter

#pragma once
#include <stdint.h>
#include "MgTypes.h"
#include "MgPblTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
struct MgLightningObject;

// ---------------------------------------------------------------------------
// MgFXSamplerEntry — common atexit-registered sampler node (57-addr pattern)
//
// Layout recovered from FUN_009560fb (MgPhysicsControllerParticle):
//   +0x00  void*       vtable_or_type_ptr  (e.g. 0xcf78e8, 0xce12c0, 0xe568f4)
//   +0x04  void*       name_str_copy       (set from CopyConstruct result)
//   +0x08  uint8_t     active_flag         (0 initially)
//
// The "next" pointer (chain) is set by the *previous* entry's struct_ptr field,
// forming a singly-linked list. 0xce12c0 appears to be a shared chain root.
// ---------------------------------------------------------------------------
typedef struct MgFXSamplerEntry {
    void*   type_ptr;     // +0x00
    void*   name_str;     // +0x04
    uint8_t active;       // +0x08
} MgFXSamplerEntry;

// ---------------------------------------------------------------------------
// MgEffectEntry — layout from FUN_008891c8 (Duration/EndStage/Effects/Components)
//
// +0x08  void*     template_data  (source template block passed to prop lookups)
// +0x0c  float     Duration       (read via 0x008a66ba float reader)
// +0x10  uint32_t  type_enum      (set from MgTypeSystem_GetClassName result)
// +0x14  uint16_t  component_count
// +0x18  uint8_t   flags          (bit0 = EndStage bool, bit1..7 preserved)
// +0x08  void*     components_ptr (allocated when component_count > 0)
// ---------------------------------------------------------------------------
typedef struct MgEffectEntry {
    void*    _unk_00;
    void*    _unk_04;
    void*    template_data;   // +0x08
    float    duration;        // +0x0c
    uint32_t type_enum;       // +0x10
    void*    components_ptr;  // +0x08 (overlaps? — separate alloc tracked at +0x08)
    uint16_t component_count; // +0x14
    uint8_t  _pad_16;
    uint8_t  flags;           // +0x18  bit0=end_stage
} MgEffectEntry;

// ---------------------------------------------------------------------------
// MgLightningObject — layout from FUN_0078637a / FUN_008298cd
//
// +0x18  uint32_t  max_time_to_live  (in ticks; converted from float seconds
//                                     by multiplying 0x9ffd58 = 1/fps constant)
// +0x1c  uint8_t   accept_lightning  (bool, bit0)
// +0x8c  uint8_t   active            (checked in lightning loop, FUN_008298cd)
// +0x2988 void*    lightning_chain   (lazy-allocated; first call to fx_lightning_test)
// ---------------------------------------------------------------------------
typedef struct MgLightningObject {
    uint8_t  _pad_00[0x18];
    uint32_t max_time_to_live; // +0x18
    uint8_t  accept_lightning; // +0x1c
    uint8_t  _pad_1d[0x6f];
    uint8_t  active;           // +0x8c
    uint8_t  _pad_8d[0x24fb];
    void*    lightning_chain;  // +0x2988
} MgLightningObject;

// ---------------------------------------------------------------------------
// MgEmitterObject — layout from FUN_009400dd and FUN_0042e595/f3a9
//
// +0x150  void*    player_team_data  (set at init; used by Team1/2PlayerEmitter)
// +0x160  uint32_t team1_hero_emitter_handle
// +0x164  uint32_t team2_hero_emitter_handle
// ---------------------------------------------------------------------------
typedef struct MgEmitterObject {
    uint8_t  _pad_00[0x150];
    void*    player_team_data;        // +0x150
    uint8_t  _pad_154[0xc];
    uint32_t team1_hero_emitter;      // +0x160
    uint32_t team2_hero_emitter;      // +0x164
} MgEmitterObject;

// ===========================================================================
// PARTICLE — FUN_00406800, FUN_0044959e
// ===========================================================================

// FUN_00406800 — SortParticles
// EDI = MgAnimBlendEntry*; [EDI+0x4] = blend_data
// Looks up "PblCRC" / "SortParticles" via find_typed_param.
// Returns byte value at offset+0x10 from entry (bool property).
int  MgParticle_GetSortParticles(const MgAnimBlendEntry* entry);

// FUN_0044959e — BindControllerStatesToLifespanOfParticle
// Same pattern as SortParticles; different field name.
int  MgParticle_GetBindControllerStatesToLifespan(const MgAnimBlendEntry* entry);

// FUN_009560fb — MgPhysicsControllerParticle_Register  (atexit, 57 addr)
void MgPhysicsControllerParticle_Register(void);

// FUN_0095615b — MgPhysicsControllerParticle_GetInstance  (43 addr)
// Calls sub_009409c1 (type system accessor).
void* MgPhysicsControllerParticle_GetInstance(void);

// ===========================================================================
// EMITTER — property accessors and atexit registrations
// ===========================================================================

// FUN_0042e595 — GetTeam2PlayerEmitter
// [EAX+0x150] = player_team_data; returns MgHandle from "MgHandle"/"Team2PlayerEmitter"
// Default: [0xcd82f8] (null handle sentinel).
uint32_t MgEmitter_GetTeam2PlayerEmitter(const MgEmitterObject* obj, uint32_t* out);

// FUN_0042f3a9 — GetTeam1PlayerEmitter
uint32_t MgEmitter_GetTeam1PlayerEmitter(const MgEmitterObject* obj, uint32_t* out);

// FUN_00448b00 — GetEffectors
// EDI = MgAnimBlendEntry*; looks up "MgTemplateArrayT<MgHandle>"/"Effectors".
// Returns pointer to array-field struct (ptr+count layout) at offset+0x10.
// Default: 0xe94a8c (empty array sentinel).
void* MgEmitter_GetEffectors(const MgAnimBlendEntry* entry);

// FUN_00455505 — GetEmitterEffectors
// Same array-lookup pattern; field = "EmitterEffectors".
void* MgEmitter_GetEmitterEffectors(const MgAnimBlendEntry* entry);

// FUN_004781b9 — GetEmittersMax
// EDI = MgAnimBlendEntry*; "uint32_t"/"EmittersMax".
// Default: 1 (if no template data).
uint32_t MgEmitter_GetEmittersMax(const MgAnimBlendEntry* entry);

// FUN_00ea4480 — GetEmittersMin
// Same pattern; field = "EmittersMin". Default: 1.
uint32_t MgEmitter_GetEmittersMin(const MgAnimBlendEntry* entry);

// FUN_0077eaac — LoadEmitterDefinitions (228 addr)
// Loads "Emitter_Self_Definition", "Emitter_Object_Definition",
// "Receiver_Self_Definition" from a template block plus "Enabled" bool.
// Returns 1 if at least one definition or Enabled was set.
int  MgEmitter_LoadDefinitions(void* emitter_state, const void* template_block);

// FUN_009400dd — InitHeroEmitters (196 addr)
// Reads "MgHandle"/"Team1HeroEmitter" and "MgHandle"/"Team2HeroEmitter"
// from team data, stores results at EDI+0x160 and EDI+0x164.
MgEmitterObject* MgEmitter_InitHeroEmitters(const void* team_data, MgEmitterObject* out);

// Atexit registrations (57–66 addr each):
void MgResourceEmitter_Register(void);    // FUN_0095778e  → [0xce1d20]
void MgSpawnEmitter_Register(void);       // FUN_0095789b  → [0xce1d60]
void MgSoundEmitter_Register(void);       // FUN_00959ffd  → [0xcf7828]
void MgBC_SetHeroEmitter_Register(void);  // FUN_00965ca0  → [0xcff984? check]

// FUN_0095a05f — MgSoundEmitter_GetInstance  (accessor, calls 0x009409c1)
void* MgSoundEmitter_GetInstance(void);

// ===========================================================================
// EFFECT — property accessors, large initializers, atexit registrations
// ===========================================================================

// FUN_0049cfec — GetEffect  (85 addr)
// EDI = out uint32_t*; arg = MgAnimBlendEntry*
// Looks up "PblCRC"/"Effect" → returns uint32_t CRC value.
// Default: [0xe560e0].
uint32_t MgEffect_GetEffect(const MgAnimBlendEntry* entry, uint32_t* out);

// FUN_00ea54b0 — GetVisualEffect  (85 addr)
// Same pattern; field = "VisualEffect".
uint32_t MgEffect_GetVisualEffect(const MgAnimBlendEntry* entry, uint32_t* out);

// FUN_008891c8 — MgEffect_LoadFromTemplate  (675 addr)
// Reads Duration, EndStage, Effects array, Components array from a template.
// Two-pass: first counts matching entries, allocates, then fills.
// Calls 0x008a66ba (float reader), 0x008a65f1 (bool reader),
//       0x008a69ac (MgTypeSystem_GetClassName), 0x008a6413/63d3/63c5 (iterator ops),
//       0x0067e610 (allocator).
// arg0 = template source ptr, arg1 = MgEffectEntry* out
void MgEffect_LoadFromTemplate(const void* template_src, MgEffectEntry* out_entry);

// FUN_00831b19 — MgAmmoEffect_Update  (521 addr)
// Thiscall (ECX = ammo object). Updates effect state based on ammo type.
// Reads "AmmoEffect" from template, performs collision/spawn dispatch.
// Refs: vtable[0x64], 0x0083134d (effect spawn sub), 0x00897ee3 (attach effect).
// Stub: requires full vtable + world state to implement.
void MgAmmoEffect_Update(void* ammo_obj, const void* world_state);

// Atexit registrations:
void MgEffectController_Register(void);       // FUN_009580bd  → [0xcf0fc4]
void MgGameEffectArea_Register(void);         // FUN_00958154  → [0xcf7024]
void MgEffectInstanceGameObject_Register(void); // FUN_00964570 → [0xcfd760]

// FUN_009645d2 — MgEffectInstanceGameObject_GetInstance  (43 addr)
void* MgEffectInstanceGameObject_GetInstance(void);

// ===========================================================================
// LIGHTNING — FUN_0078637a, 008293a6, 008298cd, 0095764e
// ===========================================================================

// FUN_0078637a — MgLightning_LoadProps  (76 addr)
// Reads "MaxTimeToLive" (float, converted to ticks) and "AcceptLightning" (bool)
// from template block into a MgLightningObject.
// arg = template block ptr, EDI = MgLightningObject*, ECX passed implicitly.
void MgLightning_LoadProps(const void* template_block, MgLightningObject* out);

// FUN_008293a6 — MgLightning_GetLocation  (195 addr)
// Resolves the lightning origin position for a lightning source object.
// Uses a static once-init string (0xe67ef0 flag at 0xe67ef4) for "LightningLocation".
// Reads 4 floats (pbl::Vector4) into EDI.
// arg0 = entity_ptr, arg1 = lightning_object_ptr, EDI = float[4] out.
void MgLightning_GetLocation(const void* entity_ptr,
                              const void* lightning_obj_ptr,
                              float       out_pos[4]);

// FUN_008298cd — MgLightning_FXTest  (353 addr)
// fx_lightning_test: main per-frame lightning update.
// Iterates 64 sub-objects (stride 0xa0) checking [obj+0x8c] active flag,
// accumulates active count, allocates a vertex buffer (0x24 bytes/segment),
// then builds lightning geometry.
// arg = MgLightningObject*
void MgLightning_FXTest(MgLightningObject* lightning_obj);

// FUN_0095764e — MgLightningFXObject_Register  (57 addr, atexit)
void MgLightningFXObject_Register(void);  // → [0xce1ca0]

// ===========================================================================
// CAMERA FX — 8 atexit registrations  (FUN_00965f58–009660e7)
// ===========================================================================

// Sampler globals (struct heads at these .bss addresses):
//   MgCamEffect             [0xcff824]  ← root entry (vtable 0xe568f4)
//   MgCamShakeEffect        [0xcff830]  ← chain from [0xcff824]
//   MgCamMassSpringEffect   [0xcff83c]  ← chain from [0xcff824]
//   MgCamRollShakeEffect    [0xcff848]  ← chain from [0xcff824]
//   MgCamFOVEffect          [0xcff854]  ← root entry (vtable 0xe568f4)
//   MgFOVBlendToEffect      [0xcff860]  ← chain from [0xcff854]
//   MgFOVResetEffect        [0xcff86c]  ← chain from [0xcff854]
//   MgFOVClickEffect        [0xcff878]  ← chain from [0xcff854]

void MgCamEffect_Register(void);          // FUN_00965f58  → [0xcff824]
void MgCamShakeEffect_Register(void);     // FUN_00965f91  → [0xcff830]
void MgCamMassSpringEffect_Register(void);// FUN_00965fca  → [0xcff83c]
void MgCamRollShakeEffect_Register(void); // FUN_00966003  → [0xcff848]
void MgCamFOVEffect_Register(void);       // FUN_0096603c  → [0xcff854]
void MgFOVBlendToEffect_Register(void);   // FUN_00966075  → [0xcff860]
void MgFOVResetEffect_Register(void);     // FUN_009660ae  → [0xcff86c]
void MgFOVClickEffect_Register(void);     // FUN_009660e7  → [0xcff878]

// ===========================================================================
// GAME EFFECTS — 4 atexit registrations  (FUN_0096687d–0096718a)
// ===========================================================================

// Sampler globals:
//   MgGM_EffectStatusUpdate      [0xcffa38]
//   MgGM_ApplyGameEffectGroup    [0xcffa44]
//   MgGM_DeactivateGameEffectGroup [0xcffa50]
//   MgGlobalGameEffects          [0xd017e4]  (uses global counter at 0xa355b0)

void MgGM_EffectStatusUpdate_Register(void);        // FUN_0096687d
void MgGM_ApplyGameEffectGroup_Register(void);      // FUN_009668b6
void MgGM_DeactivateGameEffectGroup_Register(void); // FUN_009668ef
void MgGlobalGameEffects_Register(void);            // FUN_0096718a

#ifdef __cplusplus
} // extern "C"
#endif
