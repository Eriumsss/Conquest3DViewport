// MgAnimationNode.cpp
// Reconstructed from disassembly analysis of LOTR: Conquest exe
//
// Source functions:
//   FUN_00889e69  Node factory / tree builder (1280 bytes)
//   FUN_00887c90  BlendGraph top-level loader (2344 bytes)
//   FUN_00882355  SetChildNode (wires child at node+0x40)
//   FUN_00886a3d  NodeSampler data loader (727 bytes)
//   FUN_00889994  NodeBlend data loader
//   FUN_008885b8  NodeStateMachine data loader
//   FUN_00887a9f  NodeSelector (PickRandom) loader (497 bytes)
//   FUN_008877b7  NodeSelector (PickConditional) loader (744 bytes)
//   FUN_00889597  NodePoseMatcher data loader (821 bytes)
//   FUN_00885be3  NodeFootPlacement data loader
//   FUN_008866ec  NodeLookAt data loader
//   FUN_00886478  NodeTwoJointsIk data loader
//   FUN_00886d14  NodeGlobalSRTFixup data loader
//   FUN_00889d69  NodeDrivenRagdoll data loader
//   FUN_008898cc  NodeEffect data loader
//   FUN_009556ba..00955e3a  Type registrations (29 functions)

#include "MgAnimationNode.h"
#include "MgTypeRegistry.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Type info blocks for all MgAnimationNode subtypes
// Each has a game binary address and vtable for cross-reference.
// ---------------------------------------------------------------------------

// Node definition types (base + 14 subtypes)
MgTypeInfoBlock g_MgAnimationNode_Type;
MgTypeInfoBlock g_MgAnimationNodeBlend_Type;
MgTypeInfoBlock g_MgAnimationNodeDrivenRagdoll_Type;
MgTypeInfoBlock g_MgAnimationNodeEffect_Type;
MgTypeInfoBlock g_MgAnimationNodeFootPlacement_Type;
MgTypeInfoBlock g_MgAnimationNodeGlobalSRTFixup_Type;
MgTypeInfoBlock g_MgAnimationNodeLookAt_Type;
MgTypeInfoBlock g_MgAnimationNodePoseMatcher_Type;
MgTypeInfoBlock g_MgAnimationNodeResult_Type;
MgTypeInfoBlock g_MgAnimationNodeSampler_Type;
MgTypeInfoBlock g_MgAnimationNodeSelector_Type;
MgTypeInfoBlock g_MgAnimationNodeStateMachine_Type;
MgTypeInfoBlock g_MgAnimationNodeTwoJointsIk_Type;
MgTypeInfoBlock g_MgAnimationNodeProp_Type;

// Instance types (runtime instances of nodes)
MgTypeInfoBlock g_MgAnimationNodeInstance_Type;
MgTypeInfoBlock g_MgAnimationNodeSamplerInstance_Type;
MgTypeInfoBlock g_MgAnimationNodeSelectorInstance_Type;
MgTypeInfoBlock g_MgAnimationNodePoseMatcherInstance_Type;
MgTypeInfoBlock g_MgAnimationNodePropInstance_Type;

// IK Solver types (separate hierarchy at game address 0xe568f4)
MgTypeInfoBlock g_MgAnimationFootIkSolver_Type;
MgTypeInfoBlock g_MgAnimationLookAtSolver_Type;
MgTypeInfoBlock g_MgAnimationTwoJointsIkSolver_Type;

// Other animation-related types
MgTypeInfoBlock g_MgSplineController_Type;
MgTypeInfoBlock g_MgAnimProp_Type;
MgTypeInfoBlock g_MgAnimatedPointLight_Type;

// ---------------------------------------------------------------------------
// Internal: init helper (same pattern as MgTypeRegistry.cpp::init_type)
// ---------------------------------------------------------------------------
static void init_node_type(MgTypeInfoBlock* blk, const char* name,
                           void* parent_or_vtable, void (*dtor)(void))
{
    blk->vtable      = parent_or_vtable;
    blk->name.data   = (void*)name;
    blk->name.length = (uint32_t)strlen(name);
    blk->initialized = 0;
    if (dtor) atexit(dtor);
}

// Per-type destructors (memset to zero, registered via atexit)
static void dtor_MgAnimationNode(void)                   { memset(&g_MgAnimationNode_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeBlend(void)              { memset(&g_MgAnimationNodeBlend_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeDrivenRagdoll(void)      { memset(&g_MgAnimationNodeDrivenRagdoll_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeEffect(void)             { memset(&g_MgAnimationNodeEffect_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeFootPlacement(void)      { memset(&g_MgAnimationNodeFootPlacement_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeGlobalSRTFixup(void)     { memset(&g_MgAnimationNodeGlobalSRTFixup_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeLookAt(void)             { memset(&g_MgAnimationNodeLookAt_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodePoseMatcher(void)        { memset(&g_MgAnimationNodePoseMatcher_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeResult(void)             { memset(&g_MgAnimationNodeResult_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeSampler(void)            { memset(&g_MgAnimationNodeSampler_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeSelector(void)           { memset(&g_MgAnimationNodeSelector_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeStateMachine(void)       { memset(&g_MgAnimationNodeStateMachine_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeTwoJointsIk(void)        { memset(&g_MgAnimationNodeTwoJointsIk_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeProp(void)               { memset(&g_MgAnimationNodeProp_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeInstance(void)            { memset(&g_MgAnimationNodeInstance_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeSamplerInstance(void)     { memset(&g_MgAnimationNodeSamplerInstance_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodeSelectorInstance(void)    { memset(&g_MgAnimationNodeSelectorInstance_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodePoseMatcherInstance(void) { memset(&g_MgAnimationNodePoseMatcherInstance_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationNodePropInstance(void)        { memset(&g_MgAnimationNodePropInstance_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationFootIkSolver(void)           { memset(&g_MgAnimationFootIkSolver_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationLookAtSolver(void)           { memset(&g_MgAnimationLookAtSolver_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimationTwoJointsIkSolver(void)      { memset(&g_MgAnimationTwoJointsIkSolver_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgSplineController(void)                { memset(&g_MgSplineController_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimProp(void)                        { memset(&g_MgAnimProp_Type, 0, sizeof(MgTypeInfoBlock)); }
static void dtor_MgAnimatedPointLight(void)              { memset(&g_MgAnimatedPointLight_Type, 0, sizeof(MgTypeInfoBlock)); }

// ---------------------------------------------------------------------------
// Static initializers — each mirrors a FUN_009556xx through FUN_00966xxx
// Pattern: construct name string, set parent/vtable, register atexit
//
// Game binary vtable addresses (for cross-reference):
//   MgAnimationNode base:  0xcd99a4
//   MgAnimationNodeInstance base: 0xcd9978
//   IK Solvers base: 0xe568f4
//   MgAnimationController: 0xcd9ab8 (parent for MgSplineController)
//   MgSplineController: 0xcf78e8
// ---------------------------------------------------------------------------

void MgAnimationNode_StaticInit(void) {
    init_node_type(&g_MgAnimationNode_Type, "MgAnimationNode",
                   (void*)0x00CD99A4, dtor_MgAnimationNode);
}

// All subtypes have MgAnimationNode as parent (vtable field -> parent record)
void MgAnimationNodeBlend_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeBlend_Type, "MgAnimationNodeBlend",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeBlend);
}
void MgAnimationNodeDrivenRagdoll_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeDrivenRagdoll_Type, "MgAnimationNodeDrivenRagdoll",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeDrivenRagdoll);
}
void MgAnimationNodeEffect_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeEffect_Type, "MgAnimationNodeEffect",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeEffect);
}
void MgAnimationNodeFootPlacement_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeFootPlacement_Type, "MgAnimationNodeFootPlacement",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeFootPlacement);
}
void MgAnimationNodeGlobalSRTFixup_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeGlobalSRTFixup_Type, "MgAnimationNodeGlobalSRTFixup",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeGlobalSRTFixup);
}
void MgAnimationNodeLookAt_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeLookAt_Type, "MgAnimationNodeLookAt",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeLookAt);
}
void MgAnimationNodePoseMatcher_StaticInit(void) {
    init_node_type(&g_MgAnimationNodePoseMatcher_Type, "MgAnimationNodePoseMatcher",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodePoseMatcher);
}
void MgAnimationNodeResult_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeResult_Type, "MgAnimationNodeResult",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeResult);
}
void MgAnimationNodeSampler_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeSampler_Type, "MgAnimationNodeSampler",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeSampler);
}
void MgAnimationNodeSelector_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeSelector_Type, "MgAnimationNodeSelector",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeSelector);
}
void MgAnimationNodeStateMachine_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeStateMachine_Type, "MgAnimationNodeStateMachine",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeStateMachine);
}
void MgAnimationNodeTwoJointsIk_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeTwoJointsIk_Type, "MgAnimationNodeTwoJointsIk",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeTwoJointsIk);
}
void MgAnimationNodeProp_StaticInit(void) {
    init_node_type(&g_MgAnimationNodeProp_Type, "MgAnimationNodeProp",
                   &g_MgAnimationNode_Type, dtor_MgAnimationNodeProp);
}

void MgAnimationNodeTypes_InitAll(void)
{
    // Base node type
    MgAnimationNode_StaticInit();

    // 14 node subtypes (all parent = MgAnimationNode)
    MgAnimationNodeBlend_StaticInit();
    MgAnimationNodeDrivenRagdoll_StaticInit();
    MgAnimationNodeEffect_StaticInit();
    MgAnimationNodeFootPlacement_StaticInit();
    MgAnimationNodeGlobalSRTFixup_StaticInit();
    MgAnimationNodeLookAt_StaticInit();
    MgAnimationNodePoseMatcher_StaticInit();
    MgAnimationNodeResult_StaticInit();
    MgAnimationNodeSampler_StaticInit();
    MgAnimationNodeSelector_StaticInit();
    MgAnimationNodeStateMachine_StaticInit();
    MgAnimationNodeTwoJointsIk_StaticInit();
    MgAnimationNodeProp_StaticInit();

    // Instance types (parent = MgAnimationNodeInstance)
    init_node_type(&g_MgAnimationNodeInstance_Type, "MgAnimationNodeInstance",
                   (void*)0x00CD9978, dtor_MgAnimationNodeInstance);
    init_node_type(&g_MgAnimationNodeSamplerInstance_Type, "MgAnimationNodeSamplerInstance",
                   &g_MgAnimationNodeInstance_Type, dtor_MgAnimationNodeSamplerInstance);
    init_node_type(&g_MgAnimationNodeSelectorInstance_Type, "MgAnimationNodeSelectorInstance",
                   &g_MgAnimationNodeInstance_Type, dtor_MgAnimationNodeSelectorInstance);
    init_node_type(&g_MgAnimationNodePoseMatcherInstance_Type, "MgAnimationNodePoseMatcherInstance",
                   &g_MgAnimationNodeInstance_Type, dtor_MgAnimationNodePoseMatcherInstance);
    init_node_type(&g_MgAnimationNodePropInstance_Type, "MgAnimationNodePropInstance",
                   &g_MgAnimationNodeInstance_Type, dtor_MgAnimationNodePropInstance);

    // IK Solvers (separate hierarchy, game vtable base 0xe568f4)
    init_node_type(&g_MgAnimationFootIkSolver_Type, "MgAnimationFootIkSolver",
                   (void*)0x00E568F4, dtor_MgAnimationFootIkSolver);
    init_node_type(&g_MgAnimationLookAtSolver_Type, "MgAnimationLookAtSolver",
                   (void*)0x00E568F4, dtor_MgAnimationLookAtSolver);
    init_node_type(&g_MgAnimationTwoJointsIkSolver_Type, "MgAnimationTwoJointsIkSolver",
                   (void*)0x00E568F4, dtor_MgAnimationTwoJointsIkSolver);

    // Other animation types
    init_node_type(&g_MgSplineController_Type, "MgSplineController",
                   (void*)0x00CF78E8, dtor_MgSplineController);
    init_node_type(&g_MgAnimProp_Type, "MgAnimProp",
                   (void*)0x00CF7238, dtor_MgAnimProp);
    init_node_type(&g_MgAnimatedPointLight_Type, "MgAnimatedPointLight",
                   (void*)0x00CE12C0, dtor_MgAnimatedPointLight);
}

// ---------------------------------------------------------------------------
// FUN_00882355: SetChildNode
// Wires a child node into parent's input slot at node+0x40
// Original: reads [ESI+0x40], detaches old child via 0x008822a2,
// attaches new child via 0x008822d3, stores at [ESI+0x40]
// ---------------------------------------------------------------------------
void MgAnimNode_SetChild(void* parentNode, void* childNode)
{
    if (!parentNode) return;
    // In the game binary: *(void**)((char*)parentNode + 0x40) = childNode;
    unsigned char* p = (unsigned char*)parentNode;
    *(void**)(p + 0x40) = childNode;
}

// ---------------------------------------------------------------------------
// FUN_00889E69: Node factory / tree builder
// Pass 1: Read node type string, dispatch through 16-entry jump table
//          to create typed nodes
// Pass 2: Fill data via per-type loader functions
//
// Type dispatch (from switch at 0x0088A36B):
//   "Result"          -> 0 -> 0x4057BA
//   "Animation"       -> 1 -> 0x405643
//   "StateMachine"    -> 2 -> 0x4056C0
//   "PickRandom"      -> 6 -> 0x4055CB
//   "PickConditional" -> 6 -> 0x4055CB (same creator, different data loader)
//   "Effect"          -> 7 -> 0x405837
//   "DrivenRagdoll"   -> 8 -> 0x4059A9
//   "LookAt"          -> 9 -> 0x4058AF
//   "FootPlacement"   -> 10 -> 0x40592C
//   "TwoJointsIk"     -> 11 -> 0x405A26
//   "PoseMatcher"     -> 12 -> 0x405AA3
//   "GlobalSRTFixup"  -> 13 -> 0x405B20
//   "Subgraph"        -> 14 -> RECURSIVE call to 0x00889E69
// ---------------------------------------------------------------------------
void* MgAnimNodeFactory_BuildTree(void* self, void* dataReader)
{
    // Full implementation requires the game's data serialization system
    // (0x008a6413 for scope enter, 0x008a63c5 for scope exit, etc.)
    // This is a structural placeholder showing the dispatch logic.
    (void)self;
    (void)dataReader;
    return NULL;
}

// ---------------------------------------------------------------------------
// FUN_00887C90: BlendGraph top-level loader
// Tries node types in priority order:
//   1. BlendGraph reference -> recursive subgraph
//   2. PickRandom -> weighted random selector
//   3. PickConditional -> condition-based selector
//   4. Animation -> single clip sampler
//   5. Proxy -> named reference
//   6. StateMachine -> state machine
// Then loads: track weights -> transitions (0x44 bytes each) -> events
// ---------------------------------------------------------------------------
void MgBlendGraph_Load(void* self, void* nodeFactory, void* input, void* output)
{
    (void)self;
    (void)nodeFactory;
    (void)input;
    (void)output;
}

// ---------------------------------------------------------------------------
// Per-type data loaders (structural placeholders)
// Each reads typed fields from the game's serialized data format
// ---------------------------------------------------------------------------

// FUN_00886A3D: NodeSampler playback config
// Reads: DONE_%s, RefID, DoneEvent, Looping, SendAnimEvents, Reverse,
//        MinLoopCount, MaxLoopCount, SyncID, MinPhase, MaxPhase, Speed, BlendTime
void MgAnimNodeSampler_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_00889994: NodeBlend config
// Reads: SyncTracks, NormalizeWeights, SyncToVelocity, MinRate, MaxRate,
//        SyncedRate, WeightDamp, Tracks, EventMode, Input
void MgAnimNodeBlend_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_008885B8: NodeStateMachine config
// Reads: Unnamed, %s_%s, Proxy, DefaultState, SuppressStateActivation
// Three passes: enumerate states, wire state blend graphs, load transitions
void MgAnimNodeStateMachine_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_00887A9F: NodeSelector (PickRandom) config
// Reads: Animations (count), Weights (float array)
// Creates child sampler nodes for each animation
void MgAnimNodeSelector_LoadRandomData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_008877B7: NodeSelector (PickConditional) config
// Reads: Animations, Candidates (0xC bytes each: conditions ptr, count, default),
//        PickAnimDefault, PickAnim, Conditions
void MgAnimNodeSelector_LoadConditionalData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_00889597: NodePoseMatcher config
// Reads: RootBone, OtherBone, AnotherBone, Poses (linked list of 0x18-byte entries),
//        Animation ref, SampleTime, InputIndex, BlendTime, Inputs
void MgAnimNodePoseMatcher_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_00885BE3: NodeFootPlacement config
// Per-foot (up to 8): HipBoneName, KneeBoneName, AnkleBoneName, FootEndBoneName
// Setup: CosMinKneeAngle, CosMaxKneeAngle, height params, foot IK gain
void MgAnimNodeFootPlacement_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_008866EC: NodeLookAt config
// Reads: Solvers (per-solver: FwdAxis vec4, LimitAngle, XYZGain vec4, OverallGain)
// Angles converted from degrees to radians: value * M_PI / 180.0
void MgAnimNodeLookAt_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_00886478: NodeTwoJointsIk config
// Per-IK: BoneRoot, BoneHinge, BoneEnd hashes
// HingeAxisLS vec4, DefaultTargetPosMS vec4
// MinHingeAngle, MaxHingeAngle (deg->rad), Active, TargetHP, ENABLE_IK_%s
void MgAnimNodeTwoJointsIk_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_00886D14: NodeGlobalSRTFixup config
// Reads: FilteredBones (up to 12 hashes + weights), BoneTilt, BoneDisp,
//        TiltAxisLS vec4, DispAxisLS vec4, SecondAxisLS vec4,
//        TiltDamping, DispDamping, MinTilt, MaxTilt, MinDisp, MaxDisp
void MgAnimNodeGlobalSRTFixup_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_00889D69: NodeDrivenRagdoll config
// Reads: DoneEvent, constraint entries (0x14 bytes: weight float, flags)
// Builds doubly-linked list at node+0x90
void MgAnimNodeDrivenRagdoll_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}

// FUN_008898CC: NodeEffect (AI/Event StateMachine) config
// Reads: States (0x10 bytes each: reference + OnEvent data), DefaultState
void MgAnimNodeEffect_LoadData(void* node, void* dataReader)
{
    (void)node;
    (void)dataReader;
}
