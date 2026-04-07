// MgAnimationController.cpp
// Reconstructed from disassembly analysis of LOTR: Conquest exe
//
// Source functions:
//   FUN_0088a3eb  MgAnimationController::Init (945 bytes, 7-phase)
//   FUN_00884626  AnimSample_Init (204 bytes, SPU buffers)
//   FUN_008A4CDA  AnimTable_LoadFromSystemModules (393 bytes)
//   FUN_00877A00  AnimTable_BuildSortedLookup (1349 bytes, qsort+bsearch)
//   FUN_00886A3D  Sampler playback config loader (727 bytes)
//   FUN_00905838  AnimEvent dispatch init (856 bytes)
//   FUN_007A06A5  AnimSystem config reader (99 bytes)
//   FUN_007900B5  AnimDriven config flags reader (325 bytes)
//   FUN_007BDF06  AnimDriven behavior config loader (1181 bytes)
//   FUN_00955ba7  MgAnimationController type reg (57 bytes)
//   FUN_00955be0  MgAnimationController type finalize (43 bytes)
//   FUN_00955d64  MgCreatureAnimController type reg (57 bytes)
//   FUN_00955d9d  MgCreatureAnimController type finalize
//   FUN_00955e3a  MgPropAnimController type reg (57 bytes)

#include "MgAnimationController.h"
#include "MgAnimationNode.h"
#include "MgTypeRegistry.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Type info blocks
// Game binary addresses:
//   MgAnimationController:    type record at 0xcd9ab8, vtable 0xcf78e8
//   MgCreatureAnimController: type record at 0xcdcb5c, parent hash 0xffd8bfd8
//   MgPropAnimController:     type record at 0xcdcb90
// ---------------------------------------------------------------------------
MgTypeInfoBlock g_MgAnimationController_Type;
MgTypeInfoBlock g_MgCreatureAnimController_Type;
MgTypeInfoBlock g_MgPropAnimController_Type;

// ---------------------------------------------------------------------------
// Internal: init helper (same pattern as MgTypeRegistry.cpp)
// ---------------------------------------------------------------------------
static void init_anim_type(MgTypeInfoBlock* blk, const char* name,
                           void* vtable_addr, void (*dtor)(void))
{
    blk->vtable      = vtable_addr;
    blk->name.data   = (void*)name;
    blk->name.length = (uint32_t)strlen(name);
    blk->initialized = 0;
    if (dtor) atexit(dtor);
}

// Per-type destructors
static void MgAnimationController_dtor(void)      { memset(&g_MgAnimationController_Type, 0, sizeof(MgTypeInfoBlock)); }
static void MgCreatureAnimController_dtor(void)    { memset(&g_MgCreatureAnimController_Type, 0, sizeof(MgTypeInfoBlock)); }
static void MgPropAnimController_dtor(void)        { memset(&g_MgPropAnimController_Type, 0, sizeof(MgTypeInfoBlock)); }

// ---------------------------------------------------------------------------
// FUN_00955ba7: MgAnimationController static type registration
// Pattern: hash "MgAnimationController" -> set vtable 0xcf78e8 -> atexit
// ---------------------------------------------------------------------------
void MgAnimationController_StaticInit(void)
{
    init_anim_type(&g_MgAnimationController_Type, "MgAnimationController",
                   (void*)0x00CF78E8, MgAnimationController_dtor);
}

// ---------------------------------------------------------------------------
// FUN_00955d64: MgCreatureAnimController static type registration
// Parent = MgAnimationController (vtable field points to parent type record)
// Game: [0xcdcb5c] = 0xcd9ab8 (parent), parent hash = 0xffd8bfd8
// ---------------------------------------------------------------------------
void MgCreatureAnimController_StaticInit(void)
{
    // vtable field stores parent pointer (0xcd9ab8 = MgAnimationController's record)
    init_anim_type(&g_MgCreatureAnimController_Type, "MgCreatureAnimController",
                   (void*)&g_MgAnimationController_Type, MgCreatureAnimController_dtor);
}

// ---------------------------------------------------------------------------
// FUN_00955e3a: MgPropAnimController static type registration
// Parent = MgAnimationController (same parent as MgCreatureAnimController)
// ---------------------------------------------------------------------------
void MgPropAnimController_StaticInit(void)
{
    init_anim_type(&g_MgPropAnimController_Type, "MgPropAnimController",
                   (void*)&g_MgAnimationController_Type, MgPropAnimController_dtor);
}

// ---------------------------------------------------------------------------
// FUN_0088a3eb: Main animation controller initialization (945 bytes)
//
// 7-phase init flow:
//
// Phase 1 (0x88a3eb-0x88a46b): Core setup
//   - Read [0x00cd7e24]+0xC into this+0x8 (data source)
//   - Hash config node -> this+0xC
//   - Open SYSTEM_MODULES/Animations data block
//   - Allocate 0x128 byte MgAnimStateObject -> this+0x4
//   - Call 0x0088ac9b (constructor), 0x0088aeda (setup with flag 0x400)
//   - Copy this+0xC into stateObj+0xAC (parent config hash)
//
// Phase 2 (0x88a48a-0x88a506): AnimTable loading
//   - Read "AnimTableName" string from config via 0x008a69ac
//   - Lookup in global anim manager [0x00cd7e74] via 0x00884bad
//   - If not found: try alternate "AnimTable" key via 0x0088543e
//   - Attach to controller via 0x00889cd0
//
// Phase 3 (0x88a51b-0x88a574): AnimationGraph loading
//   - Read "Result" flag, then "AnimationGraph" hash
//   - Load graph resource via 0x0040573d
//   - Store at stateObj+0x98
//   - Evaluate result node via 0x00889e69, apply via 0x00882355
//
// Phase 4 (0x88a574-0x88a5eb): HitTypePriority
//   - Read up to 32 hash entries into global array
//
// Phase 5 (0x88a5eb-0x88a703): NetworkedLocals
//   - Iterate children: read (hash, int) pairs
//   - Allocate array, store at stateObj+0xA4, count at stateObj+0xA0
//
// Phase 6 (0x88a703-0x88a74e): Bone slot wiring
//   - Call 0x00888bd0 (bone slot setup)
//   - Iterate from this+0x818 (stride 8)
//   - For each: lookup bone by hash via 0x00405422
//   - Bind animation result via 0x00882355
//
// Phase 7 (0x88a750-0x88a799): Finalization
//   - Call 0x00888e3b (final wiring)
//   - Call 0x0088ae68 (activate/finalize)
//   - If fail: try AnimStateTable + BaseStateMachine as fallback
//   - Return stateObj on success, NULL on failure
// ---------------------------------------------------------------------------
MgAnimStateObject* MgAnimationController_Init(
    MgAnimationController* self,
    void* entityConfig)
{
    if (!self || !entityConfig) return NULL;

    // Phase 1: Allocate animation state object (0x128 = 296 bytes)
    MgAnimStateObject* state = (MgAnimStateObject*)calloc(1, sizeof(MgAnimStateObject));
    if (!state) return NULL;

    self->state = state;
    state->parentConfigHash = self->configHash;

    // Phase 2: AnimTable lookup
    // In the game: reads "AnimTableName" config string, looks up in
    // global animation manager at [0x00cd7e74].
    // If not found by name, tries "AnimTable" key as alternate data path.
    // Then calls 0x00889cd0 to attach the table to the controller.
    state->animTable = NULL;

    // Phase 3: AnimationGraph
    // In the game: reads "AnimationGraph" hash from config,
    // loads the graph resource via 0x0040573d,
    // evaluates the result node via 0x00889e69 (node factory),
    // and applies it via 0x00882355 (SetChildNode).
    state->animationGraph = NULL;

    // Phase 4: HitTypePriority — up to 32 hash entries
    // Stored in global array at [global+0x80].
    // Used for hit reaction priority ordering in combat.

    // Phase 5: NetworkedLocals
    // Pairs of (nameHash, intValue) for animation parameters
    // that need network replication. Default int value = -1.
    state->networkedLocalsCount = 0;
    state->networkedLocals = NULL;

    // Phase 6: Bone slot wiring
    // Iterates from self+0x818 (stride 8 = MgAnimBoneBinding)
    // Each slot has a bone name hash and data pointer.
    // Lookup bone by hash, bind animation result to it.
    self->boneBindingCount = 0;

    // Phase 7: Finalization
    // Calls 0x00888e3b (final wiring) then 0x0088ae68 (activate).
    // If these succeed, returns stateObj. Otherwise tries
    // AnimStateTable + BaseStateMachine as fallback path.

    return state;
}

// ---------------------------------------------------------------------------
// FUN_00884626: SPU sample buffer initialization (204 bytes)
//
// Allocates pose sampling buffers used by animation evaluation:
//   self+0x4094 = array of 3 sample buffers (each 0x11C = 284 bytes)
//   self+0x4098 = 1 extra sample buffer (0x11C bytes)
//
// Reads "Anim.SampleOnSPU" config flag (always false on PC).
// Stores global pointer at [0x00cd7e78].
// Called via 0x0089f25c at end.
//
// The 3 sample buffers correspond to: current, previous, and blend target.
// The extra buffer is used for partial pose evaluation.
// Total: 4 x 284 = 1136 bytes for pose sample workspace.
// ---------------------------------------------------------------------------
void MgAnimSample_Init(void* self)
{
    if (!self) return;

    // On PC this just allocates the pose workspace buffers.
    // The SPU flag is always false but buffers are still needed
    // for the animation sampling pipeline.
    unsigned char* p = (unsigned char*)self;

    // Allocate 3 sample buffers at self+0x4094
    void* sampleBuffers = calloc(3, 0x11C);
    *(void**)(p + 0x4094) = sampleBuffers;

    // Allocate 1 extra buffer at self+0x4098
    void* extraBuffer = calloc(1, 0x11C);
    *(void**)(p + 0x4098) = extraBuffer;
}

// ---------------------------------------------------------------------------
// FUN_008A4CDA: Load animation table from SYSTEM_MODULES (393 bytes)
//
// Reads global [0x00cd7f88]+0x18 and +0x264.
// Iterates entries calling 0x00545b80 with SYSTEM_MODULES hash.
// Loads "Animations" sub-entries into the animation table.
//
// In ZeroEngine, we load animation clips from JSON files instead of
// the game's binary SYSTEM_MODULES format. This function documents
// the game's approach for reference.
// ---------------------------------------------------------------------------
void MgAnimTable_LoadFromSystemModules(void)
{
    // Game binary implementation:
    // 1. Get system modules pointer from global [0x00cd7f88]
    // 2. Read animation count at +0x18
    // 3. Read animation array at +0x264
    // 4. For each entry: call 0x00545b80 with SYSTEM_MODULES hash
    // 5. Parse "Animations" sub-block
    //
    // ZeroEngine equivalent: Scene3DAnimation.cpp::LoadJsonAnimClip()
}

// ---------------------------------------------------------------------------
// FUN_00877A00: Build sorted animation lookup table (1349 bytes)
//
// Uses qsort to sort animation entries by name hash.
// Uses bsearch for O(log n) lookup when resolving animation references.
// Reads this+0xF4 (short), compares against 0x800 (max entries).
//
// ZeroEngine equivalent: std::map<string, AnimClip*> in Scene3DAnimation.cpp
// ---------------------------------------------------------------------------
void MgAnimTable_BuildSortedLookup(void* self)
{
    (void)self;
}

// ---------------------------------------------------------------------------
// FUN_00905838: Animation event system initialization (856 bytes)
//
// Sets up 3 event type hashes using bit-flag pattern:
//   Global [0x00e882e0] bit 0x1 -> "EnableEvents"
//   Global [0x00e882e0] bit 0x2 -> "AttachObject"
//   Global [0x00e882e0] bit 0x4 -> "DetachObject"
//
// Also reads "PostAnimationEvent" handler.
//
// These hashes are used by the animation system to fire events
// during playback (e.g., attach weapon to hand bone, enable VFX).
//
// ZeroEngine equivalent: processAnimEventsForClip() in Scene3DAnimation.cpp
// which uses the AnimEventType enum (64 types) for dispatch.
// ---------------------------------------------------------------------------
void MgAnimEvents_Init(void)
{
    // Event type hashes initialized at startup.
    // The game uses CRC32 hashes of the event name strings.
}

// ---------------------------------------------------------------------------
// FUN_007A06A5: Animation system config reader (99 bytes)
//
// Opens config section "Animation"/"System".
// Reads: TriggerDelay (float) — delay before animation events fire.
// Other bool flags for animation system behavior.
// ---------------------------------------------------------------------------
void MgAnimSystem_ReadConfig(void* self, void* dataReader)
{
    (void)self;
    (void)dataReader;
}

// ---------------------------------------------------------------------------
// FUN_007900B5: Animation-driven config flags reader (325 bytes)
//
// Reads 10+ boolean/float config values via repeated calls to 0x008a65f1:
//   EnableFacingUsage         (bool)  — allow facing angle from animation
//   EnableAimingUsage         (bool)  — allow aim offset from animation
//   EnableSpeedUsage          (bool)  — allow speed from animation
//   EnableChargeZoneUsage     (bool)  — allow charge zone from animation
//   UsingDrivenAnimation      (bool)  — master enable for anim-driven movement
//   ShareFacingWithAiming     (bool)  — share facing and aiming channels
//   MaxRegularSpeedFactor     (float) — speed cap for regular units
//   MaxBossSpeedFactor        (float) — speed cap for boss units
//   CylinderAlgorithmRange    (float) — range for cylinder collision
//   CylinderAlgorithmLengthReference (float) — reference length
// ---------------------------------------------------------------------------
void MgAnimDriven_ReadConfigFlags(void* self, void* dataReader)
{
    (void)self;
    (void)dataReader;
}

// ---------------------------------------------------------------------------
// FUN_007BDF06: Animation-driven behavior config loader (1181 bytes)
//
// Reads extensive configuration for an animation-driven action/behavior:
//   Enabled              (bool)
//   AnimationDriven      (bool)  — movement comes from animation root motion
//   MinDurationDelay     (float) — minimum time before action can trigger
//   MaxDurationDelay     (float) — maximum time
//   ExitOnMaxDuration    (bool)  — auto-exit when max duration reached
//   PriorityWeight       (float) — AI priority weighting
//   ForwardRange         (float) — valid range in forward direction
//   HeightRange          (float) — valid range in height
//   StartEvents          (array) — event names that trigger this behavior
//   StartEventTrigger    (string) — specific trigger event
//
// Used for: attacks, abilities, special moves, mounted actions.
// ---------------------------------------------------------------------------
void MgAnimDriven_LoadBehaviorConfig(void* self, void* dataReader)
{
    (void)self;
    (void)dataReader;
}
