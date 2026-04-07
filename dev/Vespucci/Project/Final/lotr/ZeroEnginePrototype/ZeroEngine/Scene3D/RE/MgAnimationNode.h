// MgAnimationNode.h
// Reconstructed from disassembly: FUN_00889e69 (node factory), FUN_00887c90 (BlendGraph loader)
//
// The game's animation system is built around a tree of MgAnimationNode objects.
// The BlendGraph parser (0x00887C90) recursively builds the tree, and the
// node factory (0x00889E69) creates typed nodes from serialized data.
//
// Node type enum (from switch table at 0x0088A36B):
//   0  = NodeResult          output/root node
//   1  = NodeSampler         plays a single animation clip
//   2  = NodeStateMachine    state machine with transitions
//   3  = (reserved)
//   4  = (reserved)
//   5  = NodeBlend           blends multiple inputs with weights
//   6  = NodeSelector        picks animation (random or conditional)
//   7  = NodeEffect          VFX trigger node
//   8  = NodeDrivenRagdoll   ragdoll/animation blend
//   9  = NodeLookAt          IK look-at target
//   10 = NodeFootPlacement   foot IK grounding
//   11 = NodeTwoJointsIk     2-bone IK solver
//   12 = NodePoseMatcher     pose matching/selection
//   13 = NodeGlobalSRTFixup  root transform correction
//   14 = Subgraph            recursive (calls node factory again)
//   15 = (Unknown)
//
// Class hierarchy (from type registration at 0x009556xx-0x00955exx):
//   MgAnimationNode (base, 0xcd99a4)
//     +-- MgAnimationNodeBlend
//     +-- MgAnimationNodeDrivenRagdoll
//     +-- MgAnimationNodeEffect
//     +-- MgAnimationNodeFootPlacement
//     +-- MgAnimationNodeGlobalSRTFixup
//     +-- MgAnimationNodeLookAt
//     +-- MgAnimationNodePoseMatcher
//     +-- MgAnimationNodeResult
//     +-- MgAnimationNodeSampler
//     +-- MgAnimationNodeSelector
//     +-- MgAnimationNodeStateMachine
//     +-- MgAnimationNodeTwoJointsIk
//     +-- MgAnimationNodeProp
//
//   MgAnimationNodeInstance (base, 0xcd9978)
//     +-- MgAnimationNodeSamplerInstance
//     +-- MgAnimationNodeSelectorInstance
//     +-- MgAnimationNodePoseMatcherInstance
//     +-- MgAnimationNodePropInstance
//
//   IK Solvers (separate hierarchy, 0xe568f4)
//     +-- MgAnimationFootIkSolver
//     +-- MgAnimationLookAtSolver
//     +-- MgAnimationTwoJointsIkSolver

#pragma once
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Node type enum — matches switch table at 0x0088A36B
// ---------------------------------------------------------------------------
typedef enum MgAnimNodeType {
    MG_ANIM_NODE_RESULT           = 0,
    MG_ANIM_NODE_SAMPLER          = 1,
    MG_ANIM_NODE_STATEMACHINE     = 2,
    MG_ANIM_NODE_RESERVED_3       = 3,
    MG_ANIM_NODE_RESERVED_4       = 4,
    MG_ANIM_NODE_BLEND            = 5,
    MG_ANIM_NODE_SELECTOR         = 6,
    MG_ANIM_NODE_EFFECT           = 7,
    MG_ANIM_NODE_DRIVEN_RAGDOLL   = 8,
    MG_ANIM_NODE_LOOKAT           = 9,
    MG_ANIM_NODE_FOOT_PLACEMENT   = 10,
    MG_ANIM_NODE_TWO_JOINTS_IK    = 11,
    MG_ANIM_NODE_POSE_MATCHER     = 12,
    MG_ANIM_NODE_GLOBAL_SRT_FIXUP = 13,
    MG_ANIM_NODE_SUBGRAPH         = 14,
    MG_ANIM_NODE_UNKNOWN_15       = 15,
    MG_ANIM_NODE_COUNT            = 16
} MgAnimNodeType;

// ---------------------------------------------------------------------------
// Node creation function pointers (from Pass 1 switch table)
// ---------------------------------------------------------------------------
// 0x004057BA  NodeResult creator
// 0x00405643  NodeSampler creator
// 0x004056C0  NodeStateMachine creator
// 0x004055CB  NodeSelector creator
// 0x00405837  NodeEffect creator
// 0x004059A9  NodeDrivenRagdoll creator
// 0x004058AF  NodeLookAt creator
// 0x0040592C  NodeFootPlacement creator
// 0x00405A26  NodeTwoJointsIk creator
// 0x00405AA3  NodePoseMatcher creator
// 0x00405B20  NodeGlobalSRTFixup creator
// 0x00405B9D  Unknown15 creator

// ---------------------------------------------------------------------------
// MgAnimationNodeSampler — plays a single animation clip
// Data loaded by FUN_00886A3D
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeSamplerData {
    // Common node header (offsets 0x00..0x93 shared by all nodes)
    char _header[0x3C];
    char _weightData[0x58];     // +0x3C..0x93: weight track data

    unsigned int animRefHash;   // +0x94: animation reference hash
    float minPhase;             // +0x98: minimum phase
    float maxPhase;             // +0x9C: maximum phase
    float minRate;              // +0xA0: minimum playback rate
    float maxRate;              // +0xA4: maximum playback rate
    unsigned short minLoopCount;// +0xA8: minimum loop count
    unsigned short maxLoopCount;// +0xAA: maximum loop count
    unsigned int flags;         // +0xAC: bit0=Looping
    unsigned int refIdHash;     // +0xB0: RefID hash (sprintf DONE_%s)
    unsigned int doneEventHash; // +0xB4: DoneEvent hash
    char _padB8[4];
    float syncIdWeight;         // +0xBC: SyncID weight
    float speed;                // +0xC0: playback speed
    float blendTime;            // +0xC4: blend time
    unsigned char flagsByte;    // +0xC8: bit0=Reverse, bit1=SendAnimEvents,
                                //        bit2=SyncToVelocity, bit3=StartMaxPhase
} MgAnimNodeSamplerData;

// ---------------------------------------------------------------------------
// MgAnimationNodeBlend — blends multiple animation inputs
// Data loaded by FUN_00889994
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeBlendData {
    char _header[0x3C];
    char _weightData1[0x2C];    // +0x3C..0x67: primary weight track
    char _weightData2[0x2C];    // +0x68..0x93: secondary weight track
    int syncedRate;             // +0x80: synced rate mode (overwritten in gap)
    char _pad84[8];
    int eventMode;              // +0x8C: event dispatch mode
    char _pad90[8];
    void* perTrackData;         // +0x98: per-track array (0xA0 bytes per track)
    float minRate;              // +0x9C: minimum rate
    float maxRate;              // +0xA0: maximum rate
    unsigned char syncTracks;   // +0xA4: sync tracks flag
    unsigned char normalizeWeights; // +0xA5: normalize weights flag
    char _padA6;
    unsigned char syncToVelocity; // +0xA7: sync to velocity flag
} MgAnimNodeBlendData;

// ---------------------------------------------------------------------------
// MgAnimationNodeStateMachine — state machine with transitions
// Data loaded by FUN_008885B8
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeStateMachineData {
    char _header[0x18];
    unsigned int stateNameHash; // +0x18: state name hash (per state entry)
    char _pad1C[0x20];
    char transitions[8 * 0x14]; // +0x3C..0xDB: transition array (8 max, 0x14 each)
    int stateCount;             // +0xDC: number of states
    unsigned int rootSmHash;    // +0xE0: root state machine name hash
    unsigned char suppressActivation; // +0xE4: SuppressStateActivation flag
} MgAnimNodeStateMachineData;

// ---------------------------------------------------------------------------
// MgAnimationNodeFootPlacement — foot IK grounding
// Data loaded by FUN_00885BE3
// Strings: PelvisBoneName, Hip/Knee/Ankle/FootEndBoneName, CosMin/MaxKneeAngle
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeFootPlacementData {
    char _header[0xE0];
    unsigned int enableEventHash;   // +0xE0: EnableEvent hash
    unsigned int disableEventHash;  // +0xE4: DisableEvent hash
    char _padE8[0x408];
    unsigned int pelvisBoneHash;    // +0x4F0: PelvisBoneName hash
} MgAnimNodeFootPlacementData;

// ---------------------------------------------------------------------------
// MgAnimationNodeLookAt — IK look-at target
// Data loaded by FUN_008866EC
// Strings: Solvers, FwdAxis, LimitAngle, XYZGain, OverallGain
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeLookAtData {
    char _header[0x90];
    float easeOutAngle;         // +0x90: ease out angle (radians)
    float limitAngle;           // +0x94: limit angle (radians)
    unsigned int boneARef;      // +0x98: bone A reference
    unsigned int boneBRef;      // +0x9C: bone B reference
    float overallGain;          // +0xA0: overall gain
    char _padA4[0x394];
    unsigned char flags;        // +0x438: bit0=Active
} MgAnimNodeLookAtData;

// ---------------------------------------------------------------------------
// MgAnimationNodeTwoJointsIk — 2-bone IK solver
// Data loaded by FUN_00886478
// Strings: BoneRoot, BoneHinge, BoneEnd, HingeAxisLS, MinHingeAngle, MaxHingeAngle
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeTwoJointsIkData {
    char _header[0x90];
    unsigned int boneRootHash;  // BoneRoot name hash
    unsigned int boneHingeHash; // BoneHinge name hash
    unsigned int boneEndHash;   // BoneEnd name hash
    float hingeAxisLS[4];       // HingeAxisLS (vec4, local space)
    float defaultTargetPosMS[4];// DefaultTargetPosMS (vec4, model space)
    float minHingeAngle;        // MinHingeAngle (radians, converted from deg)
    float maxHingeAngle;        // MaxHingeAngle (radians, converted from deg)
    unsigned char active;       // Active flag
    unsigned int targetHPRef;   // TargetHP reference
} MgAnimNodeTwoJointsIkData;

// ---------------------------------------------------------------------------
// MgAnimationNodeGlobalSRTFixup — root transform correction
// Data loaded by FUN_00886D14
// Strings: FilteredBones, BoneTilt, BoneDisp, TiltAxisLS, etc.
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeGlobalSRTFixupData {
    char _header[0x90];
    unsigned int boneTiltRef;   // +0x90: BoneTilt reference
    unsigned int boneDispRef;   // +0x94: BoneDisp reference
    char _pad98[8];
    float tiltAxisLS[4];        // +0xA0: TiltAxisLS (vec4)
    float dispAxisLS[4];        // +0xB0: DispAxisLS (vec4)
    float secondAxisLS[4];      // +0xC0: SecondAxisLS (vec4)
    float tiltDamping;          // +0xD0: TiltDamping
    float dispDamping;          // +0xD4: DispDamping
    float minTilt;              // +0xD8: MinTilt (radians)
    float maxTilt;              // +0xDC: MaxTilt (radians)
    float minDisp;              // +0xE0: MinDisp
    float maxDisp;              // +0xE4: MaxDisp
    char _padE8[0x0C];
    unsigned int filteredBones[12]; // +0xF4: up to 12 filtered bone hashes
    float filteredWeights[12];  // +0x124: corresponding weights
    unsigned short filteredCount; // +0x154: count of filtered bones
} MgAnimNodeGlobalSRTFixupData;

// ---------------------------------------------------------------------------
// MgAnimationNodeDrivenRagdoll — ragdoll/animation blend
// Data loaded by FUN_00889D69
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeDrivenRagdollData {
    char _header[0x90];
    void* constraintListHead;   // +0x90: linked list head (0x14-byte entries)
    void* constraintListTail;   // +0x94: linked list tail
    char _pad98[4];
    unsigned int doneEventHash; // +0x9C: DoneEvent name hash
    unsigned char active;       // +0xA0: active flag
} MgAnimNodeDrivenRagdollData;

// ---------------------------------------------------------------------------
// MgAnimationNodePoseMatcher — pose matching/selection
// Data loaded by FUN_00889597
// Strings: RootBone, OtherBone, AnotherBone, Poses, Animation, SampleTime
// ---------------------------------------------------------------------------
typedef struct MgAnimPoseEntry {
    unsigned int animRefHash;   // +0x00: Animation reference hash
    float sampleTime;           // +0x04: SampleTime
    int inputIndex;             // +0x08: InputIndex
    float blendTime;            // +0x0C: BlendTime
    char _pad10[8];             // linked list pointers
} MgAnimPoseEntry;

typedef struct MgAnimNodePoseMatcherData {
    char _header[0x3C];
    int inputCount;             // +0x3C: number of inputs
    char _pad40[4];
    int poseCount;              // +0x44: number of poses
    void* poseTailPtr;          // +0x48: pose list tail
    void* poseHeadPtr;          // +0x4C: pose list head
    unsigned int rootBoneHash;  // +0x50: RootBone hash
    unsigned int otherBoneHash; // +0x54: OtherBone hash
    unsigned int anotherBoneHash; // +0x58: AnotherBone hash
} MgAnimNodePoseMatcherData;

// ---------------------------------------------------------------------------
// MgAnimationNodeSelector — conditional or random picker
// PickRandom loaded by FUN_00887A9F
// PickConditional loaded by FUN_008877B7
// ---------------------------------------------------------------------------
typedef struct MgAnimNodeSelectorCandidate {
    void* conditions;           // +0x00: condition array pointer
    int conditionCount;         // +0x04: condition count
    int pickAnimDefault;        // +0x08: default pick index
} MgAnimNodeSelectorCandidate;

// ---------------------------------------------------------------------------
// BlendGraph transition entry — 0x44 bytes each
// From FUN_00887C90 Phase 8
// ---------------------------------------------------------------------------
typedef struct MgBlendGraphTransition {
    char _preData[0x2C];
    int type;                   // type (0,1,2,3)
    float blendTime;            // blend time (float)
    unsigned int reference;     // reference hash
    char _midData[0x10];
    void* conditionBlock;       // condition data
} MgBlendGraphTransition;

// ---------------------------------------------------------------------------
// Type info blocks for all animation node types
// ---------------------------------------------------------------------------
extern MgTypeInfoBlock g_MgAnimationNode_Type;
extern MgTypeInfoBlock g_MgAnimationNodeBlend_Type;
extern MgTypeInfoBlock g_MgAnimationNodeDrivenRagdoll_Type;
extern MgTypeInfoBlock g_MgAnimationNodeEffect_Type;
extern MgTypeInfoBlock g_MgAnimationNodeFootPlacement_Type;
extern MgTypeInfoBlock g_MgAnimationNodeGlobalSRTFixup_Type;
extern MgTypeInfoBlock g_MgAnimationNodeLookAt_Type;
extern MgTypeInfoBlock g_MgAnimationNodePoseMatcher_Type;
extern MgTypeInfoBlock g_MgAnimationNodeResult_Type;
extern MgTypeInfoBlock g_MgAnimationNodeSampler_Type;
extern MgTypeInfoBlock g_MgAnimationNodeSelector_Type;
extern MgTypeInfoBlock g_MgAnimationNodeStateMachine_Type;
extern MgTypeInfoBlock g_MgAnimationNodeTwoJointsIk_Type;
extern MgTypeInfoBlock g_MgAnimationNodeProp_Type;

extern MgTypeInfoBlock g_MgAnimationNodeInstance_Type;
extern MgTypeInfoBlock g_MgAnimationNodeSamplerInstance_Type;
extern MgTypeInfoBlock g_MgAnimationNodeSelectorInstance_Type;
extern MgTypeInfoBlock g_MgAnimationNodePoseMatcherInstance_Type;
extern MgTypeInfoBlock g_MgAnimationNodePropInstance_Type;

extern MgTypeInfoBlock g_MgAnimationFootIkSolver_Type;
extern MgTypeInfoBlock g_MgAnimationLookAtSolver_Type;
extern MgTypeInfoBlock g_MgAnimationTwoJointsIkSolver_Type;

extern MgTypeInfoBlock g_MgSplineController_Type;
extern MgTypeInfoBlock g_MgAnimProp_Type;
extern MgTypeInfoBlock g_MgAnimatedPointLight_Type;

// ---------------------------------------------------------------------------
// Static initializers (one per type, called at C++ startup via _atexit)
// ---------------------------------------------------------------------------
void MgAnimationNode_StaticInit(void);
void MgAnimationNodeBlend_StaticInit(void);
void MgAnimationNodeDrivenRagdoll_StaticInit(void);
void MgAnimationNodeEffect_StaticInit(void);
void MgAnimationNodeFootPlacement_StaticInit(void);
void MgAnimationNodeGlobalSRTFixup_StaticInit(void);
void MgAnimationNodeLookAt_StaticInit(void);
void MgAnimationNodePoseMatcher_StaticInit(void);
void MgAnimationNodeResult_StaticInit(void);
void MgAnimationNodeSampler_StaticInit(void);
void MgAnimationNodeSelector_StaticInit(void);
void MgAnimationNodeStateMachine_StaticInit(void);
void MgAnimationNodeTwoJointsIk_StaticInit(void);
void MgAnimationNodeProp_StaticInit(void);
void MgAnimationNodeTypes_InitAll(void);

// ---------------------------------------------------------------------------
// Node factory — FUN_00889E69
// Creates typed nodes from serialized data. Called recursively for Subgraph.
// Pass 1: creates nodes by type string
// Pass 2: fills data via per-type loader functions
// ---------------------------------------------------------------------------
void* MgAnimNodeFactory_BuildTree(void* self, void* dataReader);

// ---------------------------------------------------------------------------
// BlendGraph loader — FUN_00887C90
// Top-level loader. Tries types in order:
//   BlendGraph ref -> PickRandom -> PickConditional -> Animation ->
//   Proxy -> StateMachine
// Then loads: track weights -> transitions -> events
// ---------------------------------------------------------------------------
void MgBlendGraph_Load(void* self, void* nodeFactory, void* input, void* output);

// ---------------------------------------------------------------------------
// SetChildNode — FUN_00882355
// Wires a child node into a parent node's input slot at node+0x40
// ---------------------------------------------------------------------------
void MgAnimNode_SetChild(void* parentNode, void* childNode);

// ---------------------------------------------------------------------------
// Per-type data loader functions
// ---------------------------------------------------------------------------
void MgAnimNodeSampler_LoadData(void* node, void* dataReader);     // 0x00886A3D
void MgAnimNodeBlend_LoadData(void* node, void* dataReader);       // 0x00889994
void MgAnimNodeStateMachine_LoadData(void* node, void* dataReader);// 0x008885B8
void MgAnimNodeSelector_LoadRandomData(void* node, void* dataReader);     // 0x00887A9F
void MgAnimNodeSelector_LoadConditionalData(void* node, void* dataReader);// 0x008877B7
void MgAnimNodePoseMatcher_LoadData(void* node, void* dataReader); // 0x00889597
void MgAnimNodeFootPlacement_LoadData(void* node, void* dataReader);// 0x00885BE3
void MgAnimNodeLookAt_LoadData(void* node, void* dataReader);      // 0x008866EC
void MgAnimNodeTwoJointsIk_LoadData(void* node, void* dataReader); // 0x00886478
void MgAnimNodeGlobalSRTFixup_LoadData(void* node, void* dataReader);// 0x00886D14
void MgAnimNodeDrivenRagdoll_LoadData(void* node, void* dataReader);// 0x00889D69
void MgAnimNodeEffect_LoadData(void* node, void* dataReader);      // 0x008898CC

#ifdef __cplusplus
} // extern "C"
#endif
