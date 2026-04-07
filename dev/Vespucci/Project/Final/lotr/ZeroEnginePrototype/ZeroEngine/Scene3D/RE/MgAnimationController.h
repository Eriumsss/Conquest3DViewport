// MgAnimationController.h
// Reconstructed from disassembly: FUN_0088a3eb (main init), FUN_00955ba7/be0 (type reg)
//
// MgAnimationController is Pandemic's wrapper around Havok's hkaAnimatedSkeleton.
// It does NOT use Havok's playback directly. Instead it drives a MgAnimationNode
// tree (BlendGraph) that produces the final pose.
//
// Initialization flow (from 0x0088A3EB, 945 bytes):
//   Phase 1: Open SYSTEM_MODULES / "Animations" data block
//   Phase 2: Allocate 0x128 (296) byte state object -> this+0x4
//   Phase 3: Load AnimTable by name ("AnimTableName" config key)
//   Phase 4: Load AnimationGraph by hash -> stateObj+0x98
//   Phase 5: Load HitTypePriority (up to 32 entries)
//   Phase 6: Load NetworkedLocals (hash,int pairs) -> stateObj+0xA0/0xA4
//   Phase 7: Wire bone slots (this+0x818, stride 8) -> finalize
//
// Class hierarchy (from type registration at 0x00955ba7, 0x00955d64, 0x00955e3a):
//   EntityController (vtable 0xcf78e8)
//     +-- MgAnimationController        (type record 0xcd9ab8)
//           +-- MgCreatureAnimController (type record 0xcdcb5c, parent hash 0xffd8bfd8)
//           +-- MgPropAnimController     (type record 0xcdcb90)
//
// Key game binary helper functions:
//   0x0088AC9B  Animation state object constructor (allocates 0x128 bytes)
//   0x0088AEDA  Animation state setup (arg = 0x400)
//   0x00884BAD  AnimTable lookup by name in global manager [0x00cd7e74]
//   0x0040573D  AnimationGraph resource load by hash
//   0x00889CD0  Attach AnimTable to controller
//   0x00889E69  Node factory / get animation evaluation result
//   0x00882355  SetChildNode / apply animation result to bone
//   0x00888BD0  Bone slot setup
//   0x00888E3B  Final animation wiring
//   0x0088AE68  Animation activate/finalize
//   0x00884626  SPU sample buffer init (3x 0x11C + 1x 0x11C)

#pragma once
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// MgAnimStateObject — the 296-byte (0x128) animation state
// Allocated in Phase 2 of init, stored at MgAnimationController::this+0x4
// ---------------------------------------------------------------------------
typedef struct MgAnimStateObject {
    char _pad0[0x98];
    void* animationGraph;       // +0x98: AnimationGraph pointer (loaded by hash)
    void* animTable;            // +0x9C: AnimTable pointer (animation clip lookup)
    unsigned char networkedLocalsCount; // +0xA0: count of networked local params
    char _padA1[3];
    void* networkedLocals;      // +0xA4: array of MgAnimNetworkedLocal (8 bytes each)
    char _padA8[4];
    unsigned int parentConfigHash; // +0xAC: copied from controller's config hash
    char _padB0[0x128 - 0xB0];
} MgAnimStateObject;

// ---------------------------------------------------------------------------
// MgAnimBoneBinding — 8-byte entry in the bone binding array at this+0x818
// ---------------------------------------------------------------------------
typedef struct MgAnimBoneBinding {
    unsigned int boneNameHash;  // +0x00: hash of bone name
    void* data;                 // +0x04: binding data pointer
} MgAnimBoneBinding;

// ---------------------------------------------------------------------------
// MgAnimNetworkedLocal — 8-byte entry for networked animation parameters
// Used for network replication of animation state (default value = -1)
// ---------------------------------------------------------------------------
typedef struct MgAnimNetworkedLocal {
    unsigned int nameHash;      // +0x00: parameter name hash
    int value;                  // +0x04: parameter value
} MgAnimNetworkedLocal;

// ---------------------------------------------------------------------------
// MgAnimationController — main animation controller class
//
// Layout reconstructed from disassembly offsets in FUN_0088a3eb:
//   +0x00  vtable pointer
//   +0x04  MgAnimStateObject* (296 bytes, allocated in Phase 2)
//   +0x08  data source / system module pointer
//   +0x0C  config hash/ID
//   ...gap (likely cached pointers, active state refs, flags)...
//   +0x818 MgAnimBoneBinding[N] (bone binding array, stride 8)
//   +0xA14 bone binding count
// ---------------------------------------------------------------------------
typedef struct MgAnimationController {
    void* vtable;               // +0x00
    MgAnimStateObject* state;   // +0x04: animation state object (0x128 bytes)
    void* dataSource;           // +0x08: system module data pointer
    unsigned int configHash;    // +0x0C: entity config hash

    char _pad10[0x808];         // +0x10..0x818: unknown internals

    MgAnimBoneBinding boneBindings[256]; // +0x818: max 256 bindings (stride 8)

    char _padBoneEnd[0xA14 - 0x818 - (256 * 8)];

    int boneBindingCount;       // +0xA14: number of active bone bindings
} MgAnimationController;

// ---------------------------------------------------------------------------
// Type info blocks (extern, defined in MgAnimationController.cpp)
// ---------------------------------------------------------------------------
extern MgTypeInfoBlock g_MgAnimationController_Type;      // record at 0xcd9ab8
extern MgTypeInfoBlock g_MgCreatureAnimController_Type;    // record at 0xcdcb5c
extern MgTypeInfoBlock g_MgPropAnimController_Type;        // record at 0xcdcb90

// ---------------------------------------------------------------------------
// Static type registration (Phase 1 + Phase 2 combined)
// ---------------------------------------------------------------------------
void MgAnimationController_StaticInit(void);      // FUN_00955ba7
void MgCreatureAnimController_StaticInit(void);    // FUN_00955d64
void MgPropAnimController_StaticInit(void);        // FUN_00955e3a

// ---------------------------------------------------------------------------
// FUN_0088a3eb: Main 7-phase initialization
// Returns MgAnimStateObject* on success, NULL on failure
// ---------------------------------------------------------------------------
MgAnimStateObject* MgAnimationController_Init(
    MgAnimationController* self,
    void* entityConfig
);

// ---------------------------------------------------------------------------
// FUN_00884626: SPU sample buffer init
// Allocates 3+1 pose buffers (0x11C bytes each) at self+0x4094/0x4098
// ---------------------------------------------------------------------------
void MgAnimSample_Init(void* self);

// ---------------------------------------------------------------------------
// FUN_008A4CDA: AnimTable loader from SYSTEM_MODULES
// ---------------------------------------------------------------------------
void MgAnimTable_LoadFromSystemModules(void);

// ---------------------------------------------------------------------------
// FUN_00877A00: Build sorted lookup (qsort+bsearch)
// ---------------------------------------------------------------------------
void MgAnimTable_BuildSortedLookup(void* self);

// ---------------------------------------------------------------------------
// FUN_00905838: Animation event system init
// ---------------------------------------------------------------------------
void MgAnimEvents_Init(void);

// ---------------------------------------------------------------------------
// FUN_007A06A5: Read Animation.System config (TriggerDelay, etc.)
// ---------------------------------------------------------------------------
void MgAnimSystem_ReadConfig(void* self, void* dataReader);

// ---------------------------------------------------------------------------
// FUN_007900B5: Read animation-driven movement flags
// ---------------------------------------------------------------------------
void MgAnimDriven_ReadConfigFlags(void* self, void* dataReader);

// ---------------------------------------------------------------------------
// FUN_007BDF06: Load animation-driven behavior config
// ---------------------------------------------------------------------------
void MgAnimDriven_LoadBehaviorConfig(void* self, void* dataReader);

#ifdef __cplusplus
} // extern "C"
#endif
