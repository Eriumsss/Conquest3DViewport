# Scene3D/RE — Reconstructed Engine Subsystems

Reverse-engineered C++ implementations derived from analyzed ASM dumps.
Sources: `RE/analysis/disassembly_organized_v13c/analyzed/` (17 subsystem folders)
+ `disassembly_organized_v13c/FX/` (8 subsystem subfolders, 44 files).

---

## Files in this directory

| File | Source ASM | Subsystem | Functions |
|------|-----------|-----------|-----------|
| MgTypes.h | — | Shared types | MgString, MgEntity, MgScriptEventArg, MgAnimBlendEntry, MgTypeInfoBlock |
| MgAnimBlend.h/.cpp | FUN_0041feef–0043fd91 | Animation blend params | FadeInTime, FadeOutTime, HoldTime, OneTimeOnly, MinFrame |
| MgScriptEvents.h/.cpp | FUN_0093a461–b0a6, adf9 | Script event dispatch | exSetSpawnTimer/Timer/ShowTimer, exSetCrosshairStyle/Color/Pos, exShowHideCrosshair, exSetClassInfo |
| MgTypeRegistry.h/.cpp | FUN_00958296–f03e | Static type registration | MgCrosshairController, MgMathCounter, MgAIActionInstance*, NodeInstance |
| MgTimeline.h/.cpp | FUN_0073619b, 006e2030–006f37b6 | Timeline/animation | exSetTime tick, GotoLabeledFrame (×2), CallFrame dispatcher |
| MgMount.h/.cpp | FUN_007c8c66 | Mount system | LoadDismountDistances (CloseDismountDist², FarDismountDist²) |
| MgNetwork.h/.cpp | FUN_005bbc40 | Network auth | BuildAuthTicket (ticket/macAddr/consoleId/tosVersion) |
| MgMisc.h/.cpp | FUN_005c7e40, 006bb641, 007cc118, 0079c210, 008238b3 | Misc | UTC offset, GFx text reset, entity by class name, stance/action filter, spawn event IDs |
| MgCRT.h/.cpp | _asctime_006167D5 | CRT wrapper | Mg_asctime (thread-safe, 26-byte per-thread buffer) |
| **MgEngine.h/.cpp** | FUN_00421c62, 0047da02, 004afa9a, 00876dd5, 0089eefc, 0095659f | Engine init | MaxShadowDistance, CastShadow, ReceiveShadow, MgRenderer_Init, thread table init, physics sampler |
| **MgVideo.h/.cpp** | FUN_0070d950–61–b7, 00749f38, 0074a588, ac75, acb3, af89, 008a1124 | Bink video | Open, Goto, Pause, RegisterFrameBuffers, PlayerInit, FrameUpdate, VideoThread |
| **MgD3D.h/.cpp** | FUN_006a5620, 006a63e6, 0045008d, 0045013f, 0068a645 | Direct3D 9 | CreateDevice, SelectShaderBinary, CompilePixelShader, InitGfxShaders, RegisterDepthSamplers |
| **MgTexture.h/.cpp** | FUN_00747153, 00748455, 0045ce80, 004bf690, 00ea4550, 0070f674, 0089f289 | Textures | LoadFromFile, UploadMips, GetTexture/Normal/Refraction/CloudNoise, IsNull |
| **MgCombat.h/.cpp** | FUN_007626a4, 00848a2c | Combat effects | LookupEffect (13-field filter), LookupImpact (stub) |
| **MgPblTypes.h/.cpp** | FUN_007e86d1, 007e76d4, 00428789, 004a6b46, 004fa16a | Pbl math types | pbl::Vector3/4/Matrix4x4 registration, GetWorldTransform, RoadMatrices, GetTransform |
| **MgFX.h/.cpp** | FUN_00406800–0096718a (44 files in FX/) | Particles, Emitters, Effects, Lightning, CameraFX, GameEffects | SortParticles, BindControllerStates, Team1/2PlayerEmitter, Effectors, EmittersMax/Min, HeroEmitters, Effect/VisualEffect CRC, LoadFromTemplate, AmmoEffect_Update, LightningProps/Location/FXTest, 8 CameraFX atexit, 4 GameEffects atexit |
| Lua/ | 56 ASM files (0x0054xxxx–006df49a + 2 from FX/) | Lua VM | Deferred — see Lua/MgLua_pending.txt |
| **MgAnimationController.h/.cpp** | FUN_0088a3eb, 00884626, 008a4cda, 00877a00, 00886a3d, 00905838, 007a06a5, 007900b5, 007bdf06, 00955ba7–e3a | Animation controller | MgAnimationController init (7-phase), type registration (Controller/Creature/Prop), SPU sample init, AnimTable loader, event init, config readers |
| **MgAnimationNode.h/.cpp** | FUN_00889e69, 00887c90, 00882355, 00889994, 008885b8, 00887a9f, 008877b7, 00889597, 00885be3, 008866ec, 00886478, 00886d14, 00889d69, 008898cc, 009556ba–966bd4 | Animation nodes | 16-type node factory, BlendGraph loader, SetChildNode, per-type data loaders (Sampler/Blend/StateMachine/Selector/PoseMatcher/FootPlacement/LookAt/TwoJointsIk/GlobalSRTFixup/DrivenRagdoll/Effect), 29 type registrations |

---

## Calling conventions

- **`g_pfn_*` function pointers**: set these to the game binary addresses when running
  with the game DLL loaded. When NULL, functions use default/stub behavior.
- **EDI implicit self**: the animation blend functions use EDI (not ECX) as the
  object pointer. In C++ reconstruction these take an explicit `const MgAnimBlendEntry*`.
- **`0x0067e6d8`**: Mg::String::CopyConstruct — thiscall, used by param lookup and type registration.
- **`0x00725c3b`**: MgEntity::DispatchScriptEvent — used by all ex* functions.
- **`0x0084ddc3`**: MgEvent::ResolveByName — used by spawn event ID init.

---

## ASM source folder layout (post-reorganization)

| Folder | Files | Contents |
|--------|-------|----------|
| `Animation/` | 9 | AnimBlend params, Timeline tick/goto, CallFrame dispatcher |
| `Script/` | 16 | exSet*/exShow* script event dispatchers, crosshair/class events |
| `TypeSystem/` | 16 | Static type registration (MgCrosshairController, MgMathCounter, NodeInstance…) |
| `Mount/` | 1 | LoadDismountDistances |
| `Network/` | 1 | BuildAuthTicket |
| `Misc/` | 4 | UTC offset, entity-by-class-name, stance/action filter, spawn event IDs |
| `CRT/` | 4 | Mg_asctime, __initmbctable, __setenvp, __wincmdln |
| `Lua/` | 56 | Lua VM internals (lapi.c, ldo.c, lgc.c, lvm.c) + data loader XML parser + math lib + 2 lparser.c functions from FX/ |
| `D3D/` | 12 | Direct3D device init, IAT thunks, PS/VS shader compilation, depth samplers, GPU vendor path |
| `Texture/` | 7 | D3DXCreateTextureFromFileExA wrapper, mip upload, CRC-keyed texture accessors |
| `Video/` | 9 | Bink: Open/Goto/Pause/FrameUpdate/RegisterBuffers/SetMemory/VideoThread |
| `GFx/` | 18 | Scaleform: display object properties, gradient/matrix types, glyph cache, GMatrix2D + 7 sprite/button functions from FX/ |
| `Combat/` | 2 | Combat effect lookup (self/target type/stance/race), impact/material table |
| `PblTypes/` | 5 | pbl::Matrix4x4/Vector3/Vector4 type registration, entity world transform |
| `Engine/` | 8 | Thread init (6 threads), renderer init, shadow params, physics sampler, anim blend tree |
| `Havok/` | 1 | Havok profiler timer grid (TlsGetValue, "Thread %d", "Spu %d") |
| `SIMD/` | 451 | EC_cluster SIMD internals (movaps/shufps/mulps) — not yet analyzed |

## Calling conventions (additional)

- **`0x007e866b`**: find_typed_param(key1, data, key2) — __cdecl, used by shadow/texture/transform prop readers.
- **`0x008a69ac`**: MgTypeSystem_GetClassName(type_handle, class_key, out_name) — used by combat effect lookup.
- **Bink IAT**: 0x00970440–0x00970470 — Bink SDK function pointer table (filled by Windows loader).
- **D3D device**: 0x00cd808c — global IDirect3DDevice9* set by MgD3D_CreateDevice.
- **GPU vendor**: 0x00d17b4c — DWORD set by D3D adapter query; read by MgD3D_SelectShaderBinary.

---

## Not yet implemented

- `SIMD/` (451 files) — SIMD rendering/animation/physics internals (EC_cluster)
- `GFx/` (18 files) — Scaleform display object, gradients, glyph cache, 7 sprite/button fns from FX/;
  FUN_006e39e9 + FUN_0093940f too large to read
- `Lua/` (56 ASM files) — 5 originally deferred + 49 VM functions + 2 lparser.c from FX/
  (see Lua/MgLua_pending.txt)
- `MgFX` partial stubs — MgEmitter_LoadDefinitions (needs 0x0042844c sub), MgEffect_LoadFromTemplate
  (needs iterator subs 0x008a6413/63d3/63c5), MgAmmoEffect_Update (needs vtable layout),
  MgLightning_LoadProps (needs 0x00610786 float-to-ticks), MgLightning_FXTest (needs geometry subs)
- `Engine/AnimBlendTree` — FUN_00888a1f (BlendTime) + FUN_00889994 (SyncTracks/MinRate/MaxRate) — depend on Lua VM
- `Combat/Impact` — FUN_00848a2c (1407 addr) — full implementation pending sub-function analysis
- `Havok/` — FUN_0049d9e0 (481 addr) — Havok profiler timer grid pending struct analysis
- `FX/Atlas/` — FUN_00762d45 (2807 addr, atlas/particle/sprite) + FUN_008d15e8 (atlas_1/atlas_2)
  not yet implemented
- `FX/LoadingScreen/` — FUN_00748a8a (556 addr, flash/loadscreen_%s.dds) not yet implemented
