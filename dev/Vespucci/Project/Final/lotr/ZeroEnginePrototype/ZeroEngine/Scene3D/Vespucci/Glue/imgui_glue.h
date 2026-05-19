// imgui_glue.h — Duct-Taping ImGui to a Stolen Havok D3D9 Context
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// The bridge between Dear ImGui and our stolen Havok 5.5 rendering
// pipeline. ImGui wants a D3D9 device. Havok's hkgWindow wraps one.
// This glue layer extracts the IDirect3DDevice9* from Havok's grubby
// hands and feeds it to ImGui's D3D9 backend. Also handles Win32
// message processing (WM_MOUSEMOVE, WM_KEYDOWN, etc.) so ImGui's
// input system works inside our custom window proc.
//
// Pandemic didn't have ImGui. They had their own debug UI system
// (we found "DebugMenuManager" in the .exe strings). We have something
// better: a UI framework maintained by actual living humans in 2026.
//
// "Hell is other people." — Sartre. Hell is also other people's
// rendering abstractions. Havok wraps D3D9. ImGui wraps D3D9. We
// wrap both. Three layers of abstraction around a 2004 graphics API.
// -----------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d9.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IMGUI_GLUE_CALL
#define IMGUI_GLUE_CALL __cdecl
#endif

#ifdef IMGUI_GLUE_EXPORTS
#define IMGUI_GLUE_API __declspec(dllexport)
#else
#define IMGUI_GLUE_API __declspec(dllimport)
#endif

typedef enum ImGuiGraphParamType
{
    IMGUI_GRAPH_PARAM_FLOAT = 0,
    IMGUI_GRAPH_PARAM_INT = 1,
    IMGUI_GRAPH_PARAM_BOOL = 2,
    IMGUI_GRAPH_PARAM_TRIGGER = 3
} ImGuiGraphParamType;

typedef struct ImGuiGlueFrameArgs
{
    HWND        mainHwnd;
    HWND        viewportHwnd;
    int         viewportW; /* output: viewport content width (pixels) */
    int         viewportH; /* output: viewport content height (pixels) */

    /* Display-only stats */
    float       fps;
    const char* modelName;
    const char* animName;

    /* Browser data (input) */
    const char** modelNames;
    int         modelCount;
    int         selectedModel;

    const char** animNames;
    int         animCount;
    int         selectedAnim;

    /* Playback state (input) */
    float       animTime;
    float       animDuration;
    float       playbackSpeed;
    int         loopEnabled;     /* bool */
    int         playing;         /* bool: 1=playing, 0=paused */
    int         rotInterpMode;   /* Scene3DRenderer::RotInterpMode */
    int         animFixedEnabled;    /* bool */
    float       animFixedStep;       /* seconds */
    int         animFixedMaxSteps;   /* clamp substeps per frame */
    float       animFixedAccumulator;/* seconds */
    int         animFixedStepsLast;  /* substeps used last frame */
    int         timeWarpEnabled;     /* bool */
    int         timeWarpType;        /* EasingType from AnimationCurve.h */
    float       timeWarpCp1x;
    float       timeWarpCp1y;
    float       timeWarpCp2x;
    float       timeWarpCp2y;
    int         rootMotionMode;      /* Scene3DRenderer::RootMotionMode */
    int         rootMotionLockX;     /* bool */
    int         rootMotionLockY;     /* bool */
    int         rootMotionLockZ;     /* bool */
    float       rootMotionOffsetX;
    float       rootMotionOffsetY;
    float       rootMotionOffsetZ;
    int         rootMotionWarpMode;    /* Scene3DRenderer::RootMotionWarpMode */
    float       rootMotionWarpTargetX;
    float       rootMotionWarpTargetY;
    float       rootMotionWarpTargetZ;
    int         compressAuto;       /* bool */
    int         compressQuantizeRot;/* bool */
    int         compressStripRot;   /* bool */
    float       compressPosTol;
    float       compressRotTolDeg;
    float       compressScaleTol;
    float       compressRootTol;
    int         compressStatsValid; /* bool */
    int         compressRotBefore;
    int         compressRotAfter;
    int         compressTransBefore;
    int         compressTransAfter;
    int         compressScaleBefore;
    int         compressScaleAfter;
    int         compressRootBefore;
    int         compressRootAfter;

    /* IK state (input) */
    int         ikEnabled;       /* bool */
    int         footIkEnabled;   /* bool */
    int         lookAtEnabled;   /* bool */
    int         aimEnabled;      /* bool */
    int         lookAtBoneIndex;
    int         aimBoneIndex;
    float       lookAtTargetX;
    float       lookAtTargetY;
    float       lookAtTargetZ;
    float       aimTargetX;
    float       aimTargetY;
    float       aimTargetZ;
    float       lookAtWeight;
    float       aimWeight;
    int         ikChainCount;
    const char** ikChainNames;
    const int*  ikChainEnabledFlags; /* bool array */
    int         ikSelectedChain;
    float       ikSelectedTargetX;
    float       ikSelectedTargetY;
    float       ikSelectedTargetZ;

    /* Physics / ragdoll (input) */
    int         physEnabled;     /* bool */
    int         ragdollEnabled;  /* bool */
    float       ragdollBlend;    /* 0..1 */
    float       physPosStiffness;
    float       physPosDamping;
    float       physRotStiffness;
    float       physRotDamping;
    float       ragdollGravity;

    /* Blend state (input) */
    int         blendEnabled;    /* bool */
    int         blendMode;       /* Scene3DRenderer::JsonBlendMode */
    int         blendRotMode;    /* Scene3DRenderer::JsonBlendRotMode */
    float       blendAlpha;      /* 0..1 */
    int         blendAnimIndex;  /* index into animNames, or -1 */
    int         blendLayerRootBone; /* bone index for layered mask root, or -1 */
    int         loopRegionEnabled;    /* bool */
    float       loopRegionIn;         /* seconds */
    float       loopRegionOut;        /* seconds */
    /* Multi-clip blend layers */
    const int*  blendLayerActive;       /* array[16] of bool */
    const float* blendLayerWeights;    /* array[16] of weight */
    const int*  blendLayerClipIndices; /* array[16] of clip index, -1=none */
    const int*  blendLayerLoops;       /* array[16] of bool */
    const unsigned char* blendLayerBoneMasks; /* 16*62 byte array: boneMasks[layer*62 + bone] */
    int         blendAutoNormalize;   /* bool */

    /* Pose snapshot */
    int         poseSnapshotValid;       /* bool */
    int         poseSnapshotBlendActive; /* bool */
    float       poseSnapshotBlendAlpha;  /* 0..1 */

    /* Root motion path */
    int         rootPathEnabled;         /* bool */
    int         rootPathCount;           /* number of points */

    int         blendIndependentTime; /* bool - blend clip runs on its own clock */
    int         blendLoopBlendClip;   /* bool - loop the blend clip independently */
    float       blendFadeDuration;    /* auto-fade duration in seconds */

    /* Animation graph (input) */
    int         graphEnabled;        /* bool */
    const char* graphName;
    const char* graphPath;
    int         graphListCount;
    const char** graphListNames;
    const char** graphListPaths;
    int         graphCurrentState;   /* index */
    int         graphNextState;      /* index or -1 */
    int         graphInTransition;   /* bool */
    float       graphStateTime;
    float       graphStateDuration;
    float       graphTransitionTime;
    float       graphTransitionDuration;
    int         graphParamCount;
    const char** graphParamNames;
    const int*  graphParamTypes;     /* ImGuiGraphParamType */
    const float* graphParamFloats;
    const int*  graphParamInts;
    const int*  graphParamBools;
    const float* graphParamMins;
    const float* graphParamMaxs;
    const int*  graphParamHasRange;  /* bool */

    /* Lua AnimTable (input) */
    int         luaTableListCount;
    const char** luaTableListNames;
    const char** luaTableListPaths;
    const char* luaTablePath;
    const char* luaTableName;
    int         luaTableNameCount;
    const char** luaTableNames;
    int         luaTableUsedCount;
    const char** luaTableUsedNames;
    int         luaTableEntryCount;
    const char** luaTableEntryKeys;
    const char** luaTableEntryValues;
    const char** luaTableEntryPrimaryClips;
    const char** luaTableEntrySources;
    int         luaTableMissingCount;
    const char** luaTableMissingNames;
    int         luaTableMissingTableCount;
    const char** luaTableMissingTableNames;
    const char* luaTableError;

    /* Lua AnimationStatesTranslator (input) */
    int         luaStatesListCount;
    const char** luaStatesListNames;
    const char** luaStatesListPaths;
    const char* luaStatesPath;
    int         luaStatesEntryCount;
    const char** luaStatesEntryKeys;
    const char** luaStatesEntryNames;
    const char** luaStatesEntryStances;
    const char** luaStatesEntryActions;
    const char* luaStatesError;
    int         selectedAnimStateIndex;   /* -1 = none */
    const char* selectedAnimStateKey;
    const char* selectedAnimStateName;
    
    /* ASM Parity Toggles (Phase 2.5) (input) */
    int         animationDrivenEnabled;   /* bool */
    int         rootMotionWarpEnabled;    /* bool */
    int         animDrivenMode;           /* 0=Default, 1=ClipDriven, 2=MotionDriven */

    /* Resolved/Filtered Clips (Phase 3-4) (input) */
    int         resolvedClipsCount;       /* Phase 3: unfiltered clip count */
    const char** resolvedClips;            /* Phase 3: clip paths */
    int         filteredClipsCount;       /* Phase 4: filtered clip count */
    const char** filteredClips;            /* Phase 4: filtered clip paths */
    int         selectedClipIndex;        /* Phase 4: currently selected clip index */
    
    /* Graph State Machine (input - Phase 5) */
    const char* graphStateForActiveAnimState;  /* Matched graph state name */
    int         graphStateIndexForAnimState;   /* Matched graph state index */
    int         currentGraphStateIndex;        /* Current active graph state index */
    int         graphStateCount;               /* Total graph states available */
    const char** graphStateNames;              /* Array of graph state names */

    /* Motion Matching Bridge (input - Phase 6) */
    int         motionMatchUseFilteredClips;   /* bool: enable motion matching on filtered clips */
    int         motionMatchFilteredClipCount;  /* Number of clips in filtered motion match space */
    int         motionMatchFilteredDatabaseValid;  /* bool: filtered DB is valid */

    /* State Machine Visualization (input) */
    const char** graphStateClips;              /* clip path per graph state */
    const int*  graphStateLoopFlags;           /* loop flag per graph state (bool array) */
    const float* graphStateSpeeds;             /* playback speed per graph state */
    const float* graphStateDurations;          /* duration per graph state */
    const int*  graphStateTransitionCounts;    /* number of outgoing transitions per state */
    const int*  graphStateTransitionTargetsFlat; /* flattened array of transition target indices */
    int         graphStateTransitionTotalCount;  /* total size of flattened targets array */
    const int*  graphStateOnEnterActionCounts; /* number of onEnter actions per state */
    const int*  graphStateOnExitActionCounts;  /* number of onExit actions per state */
    const char** graphStateOnEnterActionNames; /* flattened onEnter action names */
    const char** graphStateOnExitActionNames;  /* flattened onExit action names */
    int         graphStateOnEnterTotalCount;   /* total onEnter action names */
    int         graphStateOnExitTotalCount;    /* total onExit action names */
    const int*  graphStateHasSubMachine;       /* bool: state has a sub-state-machine */
    const int*  graphStateHasBlendGraph;       /* bool: state has a blend graph */
    const char** graphStateSubMachineNames;    /* sub-machine name per state (or NULL) */
    const char** graphStateBlendGraphNames;    /* blend graph name per state (or NULL) */

    /* Active Transition Detail (input) */
    int         activeTransitionIndex;         /* index of active transition, or -1 */
    int         activeTransitionFromState;     /* source state index */
    int         activeTransitionToState;       /* target state index */
    float       activeTransitionProgress;      /* 0..1 progress */
    int         activeTransitionConditionCount;/* # conditions for active transition */
    const char** activeTransitionConditionTexts; /* human-readable conditions */

    /* Creature Variables (input) */
    float       creatureVars[64];              /* live CV_0..CV_63 values */
    int         creatureVarNamedCount;         /* number of named CVs */
    const int*  creatureVarNamedIndices;       /* CV index for each named entry */
    const char** creatureVarNamedLabels;       /* label for each named entry (e.g. "CV_43 Stance") */

    /* Lua srclua file browser (input) */
    int         luaSrcFileCount;
    const char** luaSrcFileNames;     /* display names e.g. "[ANM] BlackGates/ANM_HERO_Gandalf" */
    const char** luaSrcFilePaths;     /* full paths */
    const char** luaSrcFileCategories;/* category tags: ANM, AT, ATK, SM, BC, GL, Other */

    /* Lua editor (input) */
    char*       luaEditorBuffer;
    int         luaEditorBufferSize;
    int         luaEditorBufferLen;
    const char* luaEditorPath;       /* target save path */
    const char* luaEditorSourcePath; /* actual loaded source */
    const char* luaEditorStatus;
    int         luaEditorIsBinary;   /* original file was bytecode */
    const char* luaGuessedModelName; /* last guessed model name (or NULL) */

    /* Lua dependency tree (input) — flat array, parent indices form tree */
    int          luaDepNodeCount;
    const char** luaDepNodeNames;     /* display name (e.g. "ANM_HERO_Gandalf") */
    const char** luaDepNodePaths;     /* full resolved path (or "" if unresolved) */
    const char** luaDepNodeTypes;     /* "root", "import", "inherit" */
    const char** luaDepNodeCategories;/* "ANM","AT","ATK","SM","BC","GL","Other" */
    const int*   luaDepNodeParents;   /* parent index (-1 for root) */
    const int*   luaDepNodeDepths;    /* depth level */
    const int*   luaDepNodeResolved;  /* 1 if file was found, 0 if not */

    /* Motion matching (input) */
    int         motionMatchEnabled;      /* bool */
    int         motionMatchDbValid;      /* bool */
    int         motionMatchClipCount;
    int         motionMatchFrameCount;
    float       motionMatchSearchInterval;
    float       motionMatchBlendDuration;
    float       motionMatchTargetVelX;
    float       motionMatchTargetVelZ;
    float       motionMatchTargetFacingX;
    float       motionMatchTargetFacingZ;
    float       motionMatchLastScore;
    const char* motionMatchCurrentClipKey;

    int         showOverlay;     /* bool */

    /* Materials (input) */
    const char** materialLines;
    int          materialCount;
    const char*  warningText;

    /* Particle effects (input) */
    const char** effectNames;
    int          effectCount;
    int          selectedEffect;

    /* Outputs from UI to host (set by DLL) */
    int         requestRescan;       /* bool */
    int         requestLoadModel;    /* bool */
    int         requestLoadAnim;     /* bool */
    int         requestedModelIndex; /* int */
    int         requestedAnimIndex;  /* int */
    int         requestPlayToggle;   /* bool */
    int         requestStop;         /* bool */
    int         requestPrev;         /* bool */
    int         requestNext;         /* bool */
    int         requestLoopToggle;   /* bool */
    int         requestSetTime;      /* bool */
    float       requestedTime;       /* seconds */
    int         requestSetSpeed;     /* bool */
    float       requestedSpeed;      /* multiplier */
    int         requestSetRotInterpMode; /* bool */
    int         requestedRotInterpMode;  /* RotInterpMode */
    int         requestSetIKEnabled;     /* bool */
    int         requestedIKEnabled;      /* 0/1 */
    int         requestSetFootIKEnabled; /* bool */
    int         requestedFootIKEnabled;  /* 0/1 */
    int         requestSetLookAtEnabled; /* bool */
    int         requestedLookAtEnabled;  /* 0/1 */
    int         requestSetAimEnabled;    /* bool */
    int         requestedAimEnabled;     /* 0/1 */
    int         requestSetLookAtBone;    /* bool */
    int         requestedLookAtBone;     /* bone index */
    int         requestSetAimBone;       /* bool */
    int         requestedAimBone;        /* bone index */
    int         requestSetLookAtTarget;  /* bool */
    float       requestedLookAtTargetX;
    float       requestedLookAtTargetY;
    float       requestedLookAtTargetZ;
    int         requestSetAimTarget;     /* bool */
    float       requestedAimTargetX;
    float       requestedAimTargetY;
    float       requestedAimTargetZ;
    int         requestSetLookAtWeight;  /* bool */
    float       requestedLookAtWeight;
    int         requestSetAimWeight;     /* bool */
    float       requestedAimWeight;
    int         requestSetIKChainEnabled;/* bool */
    int         requestedIKChainIndex;
    int         requestedIKChainEnabled; /* 0/1 */
    int         requestSetIKSelectedChain; /* bool */
    int         requestedIKSelectedChain;
    int         requestSetIKChainTarget; /* bool */
    int         requestedIKChainTargetIndex;
    float       requestedIKChainTargetX;
    float       requestedIKChainTargetY;
    float       requestedIKChainTargetZ;
    int         requestSetIKChainTargetBone; /* bool */
    int         requestedIKChainTargetBoneIndex;
    int         requestedIKChainTargetBone;
    int         requestRebuildIKChains;  /* bool */
    int         requestSetPhysEnabled;   /* bool */
    int         requestedPhysEnabled;    /* 0/1 */
    int         requestSetRagdollEnabled;/* bool */
    int         requestedRagdollEnabled; /* 0/1 */
    int         requestSetRagdollBlend;  /* bool */
    float       requestedRagdollBlend;
    int         requestSetPhysPosStiffness; /* bool */
    float       requestedPhysPosStiffness;
    int         requestSetPhysPosDamping;   /* bool */
    float       requestedPhysPosDamping;
    int         requestSetPhysRotStiffness; /* bool */
    float       requestedPhysRotStiffness;
    int         requestSetPhysRotDamping;   /* bool */
    float       requestedPhysRotDamping;
    int         requestSetRagdollGravity;   /* bool */
    float       requestedRagdollGravity;
    int         requestResetPhysicsState;   /* bool */
    int         requestApplyHitImpulse;     /* bool */
    int         requestedHitBoneIndex;
    float       requestedHitLinearX;
    float       requestedHitLinearY;
    float       requestedHitLinearZ;
    float       requestedHitAngularX;
    float       requestedHitAngularY;
    float       requestedHitAngularZ;
    int         requestSetAnimFixedEnabled; /* bool */
    int         requestedAnimFixedEnabled;  /* 0/1 */
    int         requestSetAnimFixedStep;    /* bool */
    float       requestedAnimFixedStep;     /* seconds */
    int         requestSetAnimFixedMaxSteps;/* bool */
    int         requestedAnimFixedMaxSteps; /* int */
    int         requestResetAnimFixedAccumulator; /* bool */
    int         requestSetTimeWarpEnabled; /* bool */
    int         requestedTimeWarpEnabled;  /* 0/1 */
    int         requestSetTimeWarpType;    /* bool */
    int         requestedTimeWarpType;     /* EasingType */
    int         requestSetTimeWarpCP;      /* bool */
    float       requestedTimeWarpCp1x;
    float       requestedTimeWarpCp1y;
    float       requestedTimeWarpCp2x;
    float       requestedTimeWarpCp2y;
    int         requestSetRootMotionMode;  /* bool */
    int         requestedRootMotionMode;   /* RootMotionMode */
    int         requestSetRootMotionLocks; /* bool */
    int         requestedRootMotionLockX;  /* 0/1 */
    int         requestedRootMotionLockY;  /* 0/1 */
    int         requestedRootMotionLockZ;  /* 0/1 */
    int         requestResetRootMotion;    /* bool */
    int         requestSetRootMotionWarpEnabled; /* bool */
    int         requestedRootMotionWarpEnabled;  /* 0/1 */
    int         requestSetRootMotionWarpMode;    /* bool */
    int         requestedRootMotionWarpMode;     /* RootMotionWarpMode */
    int         requestSetRootMotionWarpTarget;  /* bool */
    float       requestedRootMotionWarpTargetX;
    float       requestedRootMotionWarpTargetY;
    float       requestedRootMotionWarpTargetZ;
    int         requestSetCompressAuto;       /* bool */
    int         requestedCompressAuto;        /* 0/1 */
    int         requestSetCompressTolerances; /* bool */
    float       requestedCompressPosTol;
    float       requestedCompressRotTolDeg;
    float       requestedCompressScaleTol;
    float       requestedCompressRootTol;
    int         requestSetCompressQuantizeRot;/* bool */
    int         requestedCompressQuantizeRot; /* 0/1 */
    int         requestSetCompressStripRot;   /* bool */
    int         requestedCompressStripRot;    /* 0/1 */
    int         requestOptimizeActiveClip;    /* bool */
    int         requestSpawnEffect;  /* bool */
    int         requestedEffectIndex;/* int */

    /* Blend outputs from UI to host (set by DLL) */
    int         requestSetBlendEnabled;     /* bool */
    int         requestedBlendEnabled;      /* 0/1 */
    int         requestSetBlendMode;        /* bool */
    int         requestedBlendMode;         /* JsonBlendMode */
    int         requestSetBlendRotMode;     /* bool */
    int         requestedBlendRotMode;      /* JsonBlendRotMode */
    int         requestSetBlendAlpha;       /* bool */
    float       requestedBlendAlpha;        /* 0..1 */
    int         requestSetBlendAnimIndex;   /* bool */
    int         requestedBlendAnimIndex;    /* index into animNames, or -1 */
    int         requestSetBlendLayerRootBone; /* bool */
    int         requestedBlendLayerRootBone;  /* bone index, or -1 */
    int         requestSetBlendIndependentTime; /* bool */
    int         requestedBlendIndependentTime;  /* bool */
    int         requestSetBlendLoopBlendClip;   /* bool */
    int         requestedBlendLoopBlendClip;    /* bool */
    int         requestSetBlendFadeDuration;    /* bool */
    float       requestedBlendFadeDuration;     /* seconds */
    int         requestBlendFadeIn;             /* bool - trigger fade-in */
    int         requestBlendFadeOut;            /* bool - trigger fade-out */

    /* Multi-clip blend layers */
    int         requestSetBlendLayerState;     /* bool */
    int         requestSetBlendLayerWeight;    /* bool */
    int         requestSetBlendLayerClip;      /* bool */
    int         requestedBlendLayerIdx;        /* which layer 0-3 */
    int         requestedBlendLayerActive;     /* bool */
    float       requestedBlendLayerWeight;     /* weight */
    int         requestedBlendLayerClipIdx;    /* clip index */
    int         requestSetBlendLayerLoop;      /* bool */
    int         requestedBlendLayerLoop;       /* bool */
    int         requestSetBlendLayerBoneMask;     /* bool - set single bone */
    int         requestedBlendLayerBoneMaskBone;  /* bone index */
    int         requestedBlendLayerBoneMaskValue; /* 0 or 1 */
    int         requestSetBlendLayerBoneMaskAll;  /* bool - set all bones */
    int         requestSetBlendLayerBoneMaskPreset; /* bool - apply preset */
    int         requestedBlendLayerBoneMaskPreset;  /* 0=upper, 1=lower */
    int         requestSetBlendAutoNormalize;  /* bool */
    int         requestedBlendAutoNormalize;   /* bool */

    /* Pose snapshot */
    int         requestCapturePoseSnapshot;        /* bool */
    int         requestSetPoseSnapshotBlendActive; /* bool */
    int         requestedPoseSnapshotBlendActive;  /* bool */
    int         requestSetPoseSnapshotBlendAlpha;  /* bool */
    float       requestedPoseSnapshotBlendAlpha;   /* 0..1 */

    /* Root motion path */
    int         requestSetRootPathEnabled;        /* bool */
    int         requestedRootPathEnabled;         /* bool */
    int         requestClearRootPath;             /* bool */

    /* A/B loop region */
    int         requestSetLoopRegion;          /* bool */
    float       requestedLoopRegionIn;         /* seconds */
    float       requestedLoopRegionOut;        /* seconds */
    int         requestClearLoopRegion;        /* bool */

    /* Animation graph outputs from UI to host (set by DLL) */
    int         requestLoadGraph;          /* bool */
    char        requestedGraphPath[512];
    int         requestClearGraph;         /* bool */
    int         requestSetGraphEnabled;    /* bool */
    int         requestedGraphEnabled;     /* 0/1 */
    int         requestSetGraphParam;      /* bool */
    int         requestedGraphParamIndex;  /* param index */
    int         requestedGraphParamType;   /* ImGuiGraphParamType */
    float       requestedGraphParamFloat;
    int         requestedGraphParamInt;
    int         requestedGraphParamBool;
    int         requestFireGraphTrigger;   /* bool */
    int         requestedGraphTriggerIndex;/* param index */
    int         requestSetGraphState;      /* bool */
    int         requestResetGraph;         /* bool */
    int         requestBuildGraphFromCurrentAnim; /* bool */

    /* Lua AnimTable outputs from UI to host (set by DLL) */
    int         requestLoadLuaTable;   /* bool */
    char        requestedLuaTablePath[512];
    int         requestLoadLuaStates;  /* bool */
    char        requestedLuaStatesPath[512];
    int         requestSetAnimStatesSelectedIndex; /* bool (Phase 2) */
    int         requestedAnimStatesSelectedIndex;  /* int (Phase 2) */
    int         requestSetAnimationDriven;    /* bool (Phase 2.5) */
    int         requestedAnimationDriven;     /* 0/1 (Phase 2.5) */
    int         requestSetRootMotionWarp;     /* bool (Phase 2.5) */
    int         requestedRootMotionWarp;      /* 0/1 (Phase 2.5) */
    int         requestSetAnimDrivenMode;     /* bool (Phase 2.5) */
    int         requestedAnimDrivenMode;      /* 0/1/2 (Phase 2.5) */
    
    /* Phase 4: Filtered Clip Selection outputs from UI to host (set by DLL) */
    int         requestSetSelectedClipIndex;  /* bool (Phase 4) */
    int         requestedSelectedClipIndex;   /* clip index (Phase 4) */
    int         requestPlayFilteredClip;      /* bool (Phase 4) */
    int         requestedPlayClipIndex;       /* clip index to play (Phase 4) */
    
    /* Phase 5: Graph State Transition outputs from UI to host (set by DLL) */
    int         requestTransitionToGraphState;   /* bool (Phase 5) */
    int         requestedGraphStateIndex;       /* graph state index (Phase 5) */
    
    /* Phase 6: Motion Matching Bridge outputs from UI to host (set by DLL) */
    int         requestSetMotionMatchFiltered;  /* bool */
    int         requestedMotionMatchFiltered;   /* 0/1 (enable/disable filtered motion matching) */
    
    int         requestPlayLuaClip;    /* bool */
    char        requestedLuaClipName[256];
    int         requestGuessLuaModel;  /* bool */

    /* Lua editor outputs from UI to host (set by DLL) */
    int         requestOpenLuaEditor;    /* bool */
    char        requestedLuaEditorPath[512];
    int         requestReloadLuaEditor;  /* bool */
    int         requestSaveLuaEditor;    /* bool */

    /* Motion matching outputs from UI to host (set by DLL) */
    int         requestSetMotionMatchEnabled; /* bool */
    int         requestedMotionMatchEnabled;  /* 0/1 */
    int         requestRebuildMotionMatchDb;  /* bool */
    int         requestClearMotionMatchDb;    /* bool */
    int         requestSetMotionMatchTargetVelocity; /* bool */
    float       requestedMotionMatchTargetVelX;
    float       requestedMotionMatchTargetVelZ;
    int         requestSetMotionMatchTargetFacing;   /* bool */
    float       requestedMotionMatchTargetFacingX;
    float       requestedMotionMatchTargetFacingZ;
    int         requestSetMotionMatchSearchInterval; /* bool */
    float       requestedMotionMatchSearchInterval;
    int         requestSetMotionMatchBlendDuration;  /* bool */
    float       requestedMotionMatchBlendDuration;

    /* Host should use these to gate camera/input when hovering UI (set by DLL). */
    int         wantCaptureMouse;    /* bool */
    int         wantCaptureKeyboard; /* bool */

    /* Bones/editor data (input) */
    const char** boneNames;
    const int*   boneParents;
    int          boneCount;
    int          selectedBone;

    int          editMode;    /* bool: persistent edit mode (host-owned) */
    int          pendingEdit; /* bool: selected bone has uncommitted edit */
    int          recording;   /* bool: AutoKey */
    int          interpMode;  /* Scene3DRenderer::InterpMode: 0=hold (stepped), 1=interpolate (uses easingType per key) */

    /* Editor easing defaults (input) */
    int          editorEasingType; /* EasingType from AnimationCurve.h (used for new keys + UI default) */
    float        editorEasingCp1x;
    float        editorEasingCp1y;
    float        editorEasingCp2x;
    float        editorEasingCp2y;

    /* Active keyframe under playhead for selected bone (input; -1 if none) */
    int          activeRotKeyIndex;
    float        activeRotKeyTimeMs;
    int          activeRotKeyEasingType;
    float        activeRotKeyCp1x;
    float        activeRotKeyCp1y;
    float        activeRotKeyCp2x;
    float        activeRotKeyCp2y;

    int          activeTransKeyIndex;
    float        activeTransKeyTimeMs;
    int          activeTransKeyEasingType;
    float        activeTransKeyCp1x;
    float        activeTransKeyCp1y;
    float        activeTransKeyCp2x;
    float        activeTransKeyCp2y;
    int          activeTransKeyInterpMode; /* 0=Constant, 1=Linear, 2=Cubic(Hermite) */
    float        activeTransKeyInTanX;
    float        activeTransKeyOutTanX;
    float        activeTransKeyInTanY;
    float        activeTransKeyOutTanY;
    float        activeTransKeyInTanZ;
    float        activeTransKeyOutTanZ;

    int          activeScaleKeyIndex;
    float        activeScaleKeyTimeMs;
    int          activeScaleKeyEasingType;
    float        activeScaleKeyCp1x;
    float        activeScaleKeyCp1y;
    float        activeScaleKeyCp2x;
    float        activeScaleKeyCp2y;
    int          activeScaleKeyInterpMode; /* 0=Constant, 1=Linear, 2=Cubic(Hermite) */
    float        activeScaleKeyInTanX;
    float        activeScaleKeyOutTanX;
    float        activeScaleKeyInTanY;
    float        activeScaleKeyOutTanY;
    float        activeScaleKeyInTanZ;
    float        activeScaleKeyOutTanZ;

    int          gizmoMode;   /* see Scene3DRenderer::GizmoMode */
    int          gizmoSpace;  /* see Scene3DRenderer::GizmoSpace */

    int          rotSnapEnabled;  /* bool */
    float        rotSnapDegrees;
    int          moveSnapEnabled; /* bool */
    float        moveSnapUnits;

    float        editorTimelineDuration;

    int          hasBoneTRS; /* bool */
    float        boneTx, boneTy, boneTz;
    float        boneRxDeg, boneRyDeg, boneRzDeg;
    float        boneSx, boneSy, boneSz;

    const char*  exportName; /* optional initial value for export UI */

    /* Bones/editor outputs from UI to host (set by DLL) */
    int         requestSelectBone;     /* bool */
    int         requestedBoneIndex;    /* int */

    int         requestSetEditMode;    /* bool */
    int         requestedEditMode;     /* 0/1 */

    int         requestSetGizmoMode;   /* bool */
    int         requestedGizmoMode;    /* int */
    int         requestSetGizmoSpace;  /* bool */
    int         requestedGizmoSpace;   /* int */

    int         requestSetRecording;   /* bool */
    int         requestedRecording;    /* 0/1 */
    int         requestSetInterpMode;  /* bool */
    int         requestedInterpMode;   /* 0/1 */

    /* Easing outputs from UI to host (set by DLL) */
    int         requestSetEditorEasing;   /* bool */
    int         requestedEditorEasingType;/* EasingType */
    float       requestedEditorEasingCp1x;
    float       requestedEditorEasingCp1y;
    float       requestedEditorEasingCp2x;
    float       requestedEditorEasingCp2y;

    int         requestSetActiveKeyEasing;  /* bool */
    int         requestedActiveKeyChannel;  /* 0=rotation, 1=translation, 2=scale */
    int         requestedActiveKeyIndex;    /* key index into editor key arrays */
    int         requestedActiveKeyEasingType;/* EasingType */
    float       requestedActiveKeyCp1x;
    float       requestedActiveKeyCp1y;
    float       requestedActiveKeyCp2x;
    float       requestedActiveKeyCp2y;

    int         requestSetActiveKeyInterpolation; /* bool */
    int         requestedActiveKeyInterpMode;     /* 0=Constant, 1=Linear, 2=Cubic(Hermite) */
    float       requestedActiveKeyInTanX;
    float       requestedActiveKeyOutTanX;
    float       requestedActiveKeyInTanY;
    float       requestedActiveKeyOutTanY;
    float       requestedActiveKeyInTanZ;
    float       requestedActiveKeyOutTanZ;

    int         requestSetRotSnapEnabled; /* bool */
    int         requestedRotSnapEnabled; /* 0/1 */
    int         requestSetRotSnapDegrees; /* bool */
    float       requestedRotSnapDegrees;
    int         requestSetMoveSnapEnabled; /* bool */
    int         requestedMoveSnapEnabled; /* 0/1 */
    int         requestSetMoveSnapUnits;   /* bool */
    float       requestedMoveSnapUnits;

    int         requestCommitEdit;     /* bool */
    int         requestCancelEdit;     /* bool */

    int         requestResetSelectedBone; /* bool */
    int         requestResetAllBones;     /* bool */

    int         requestUndoPose;       /* bool */
    int         requestRedoPose;       /* bool */

    int         requestKeyBoneTRS;     /* bool */
    float       requestedBoneTx, requestedBoneTy, requestedBoneTz;
    float       requestedBoneRxDeg, requestedBoneRyDeg, requestedBoneRzDeg;
    float       requestedBoneSx, requestedBoneSy, requestedBoneSz;
    int         requestedKeyRot;       /* 0/1 */
    int         requestedKeyTrans;     /* 0/1 */
    int         requestedKeyScale;     /* 0/1 */

    int         requestSetTimelineDuration; /* bool */
    float       requestedTimelineDuration;

    int         requestExportClip;     /* bool */
    char        requestedExportName[64];

    /* Timeline events (input) */
    int          eventCount;
    const float* eventTimes;
    const int*   eventCategories;
    const char** eventSummaries;

    /* Timeline event editor outputs (set by DLL) */
    int         requestAddEvent;                /* bool */
    int         requestedAddEventType;          /* AnimEventType enum value */
    float       requestedAddEventTime;          /* seconds */

    /* Event parameters (up to 9 values, each can be Int, Float, or CRC) */
    int         requestedAddEventParamCount;    /* number of parameters (0-9) */
    int         requestedAddEventParamTypes[9]; /* 0=Int, 1=Float, 2=CRC */
    int         requestedAddEventParamInts[9];  /* Int values */
    float       requestedAddEventParamFloats[9];/* Float values */
    char        requestedAddEventParamCrcs[9][128]; /* CRC string values */

    /* Legacy fields for backward compatibility */
    int         requestedAddEventIsSoundCue;    /* 0=SoundEvent, 1=SoundCue (deprecated) */
    char        requestedAddEventCrc0[128];
    char        requestedAddEventCrc1[128];
    char        requestedAddEventCrc2[128];

    int         requestDeleteEvent;             /* bool */
    int         requestedDeleteEventIndex;      /* int */

    /* ImGui viewport render-to-texture (input) */
    void*       viewportTexture;       /* IDirect3DTexture9* (ImTextureID) or NULL */
    int         viewportTextureWidth;  /* pixels */
    int         viewportTextureHeight; /* pixels */

    /* Viewport content rect in main window client coordinates (output, set by DLL). */
    int         viewportX;
    int         viewportY;

    /* Environment / skybox (input) */
    const char** skyboxNames;
    int          skyboxCount;
    int          activeSkyboxIndex;
    int          skyboxEnabled; /* bool */
    int          skyRenderMode; /* 0=Backdrop,1=Mesh,2=Hybrid (Scene3DRenderer::SkyRenderMode) */

    /* Environment / skybox outputs (set by DLL) */
    int         requestSetSkyboxEnabled; /* bool */
    int         requestedSkyboxEnabled; /* 0/1 */
    int         requestSelectSkybox;    /* bool */
    int         requestedSkyboxIndex;   /* int */
    int         requestSetSkyRenderMode;/* bool */
    int         requestedSkyRenderMode; /* int */

    /* Environment mash presets (set by DLL) */
    int         requestApplyEnvMash;    /* bool */
    int         requestedEnvMashId;     /* int */

    /* Environment cloud layer (input) */
    int         cloudEnabled;           /* bool */
    int         cloudSkyboxIndex;       /* int (index into skyboxNames) */

    /* Environment cloud layer outputs (set by DLL) */
    int         requestSetCloudEnabled;     /* bool */
    int         requestedCloudEnabled;      /* 0/1 */
    int         requestSelectCloudSkybox;   /* bool */
    int         requestedCloudSkyboxIndex;  /* int */

    /* Level (PAK/BIN) browser — input from host */
    int          levelLoaded;               /* bool: a level is currently loaded */
    const char*  levelName;                 /* active level name, e.g. "Training" */
    const char*  levelLoadError;            /* NULL if OK, else error message */
    int          pakModelCount;
    const char** pakModelNames;
    int          pakTextureCount;
    const char** pakTextureNames;
    const int*   pakTextureWidths;          /* width per texture (0 if unknown) */
    const int*   pakTextureHeights;         /* height per texture */
    int          pakAnimCount;
    const char** pakAnimNames;
    int          pakPakEffectCount;
    const char** pakPakEffectNames;
    int          pakGfxBlockCount;
    const char** pakGfxBlockNames;
    /* Per-model detail (parallel to pakModelNames) */
    const unsigned int* pakModelCrcs;
    const int*          pakModelBoneCounts;
    const int*          pakModelMatCounts;
    const int*          pakModelGamemodeMasks;
    /* Per-texture detail (parallel to pakTextureNames) */
    const int*          pakTextureDepths;
    const int*          pakTextureLevels;
    const unsigned int* pakTextureFormats;
    const int*          pakTextureGamemodeMasks;
    /* Per-animation detail (parallel to pakAnimNames) */
    const unsigned int* pakAnimCrcs;
    const int*          pakAnimSizes;
    const int*          pakAnimSizesComp;
    const int*          pakAnimGamemodeMasks;
    /* Per-effect/GFX sizes */
    const int*          pakEffectSizes;
    const int*          pakEffectGamemodeMasks;
    const int*          pakGfxSizes;
    /* All raw BIN assets (includes JSON, Lua, scripts, audio, etc.) */
    int                 binRawCount;
    const char**        binRawNames;
    const unsigned int* binRawKinds;
    const int*          binRawSizes;
    const int*          binRawSizesComp;
    /* Name resolver status (input from host) */
    int          stringsLoaded;             /* 1 if conquest_strings.txt was loaded */
    int          stringsCount;              /* number of strings loaded */
    /* Sub-block entries from PAK block1 and block2 (Lua, GameObjs, LangStrings, Data, etc.) */
    int          pakSubBlock1Count;
    const char** pakSubBlock1Names;         /* resolved name or hex CRC */
    const char** pakSubBlock1Types;         /* "Lua","Data","GameObjs","LangStrings","Spray","Crowd","PFields","AtlasUV","SSA","Unknown" */
    const int*   pakSubBlock1Sizes;
    int          pakSubBlock2Count;
    const char** pakSubBlock2Names;
    const char** pakSubBlock2Types;
    const int*   pakSubBlock2Sizes;
    /* ObjAs — compressed model/object data blocks */
    int                  pakObjACount;
    const char**         pakObjANames;
    const int*           pakObjASizes;
    const int*           pakObjASizesComp;
    /* Obj0s — asset type references */
    int                  pakObj0Count;
    const char**         pakObj0Names;
    const unsigned int*  pakObj0TypeCodes;
    /* Animation blocks — per-game-mode compressed animation data */
    int                  pakAnimBlockCount;
    const char**         pakAnimBlockNames;    /* e.g. "evilcampaign" */
    const char**         pakAnimBlockKeyNames; /* e.g. "Evil Campaign" */
    const int*           pakAnimBlockSizes;
    const int*           pakAnimBlockSizesComp;
    /* Pak vals (block_a) — per-asset PAK values */
    int                  pakValCount;
    const char**         pakValNames;
    const int*           pakValGamemodeMasks;
    /* Buffer infos — raw vertex/index buffer descriptors */
    int                  pakBufferInfoCount;
    const char**         pakBufferInfoNames;
    const unsigned int*  pakBufferInfoExtra1; /* offset */
    const unsigned int*  pakBufferInfoExtra2; /* size */
    /* Mat1 — basic material (diffuse) */
    int                  pakMat1Count;
    const char**         pakMat1Names;
    const unsigned int*  pakMat1Tex0;         /* tex0 crc */
    const unsigned int*  pakMat1Tex1;         /* tex1 crc */
    /* Mat2 — material type 2 */
    int                  pakMat2Count;
    const char**         pakMat2Names;
    const unsigned int*  pakMat2Tex0;
    const unsigned int*  pakMat2Tex1;
    /* Mat3 — material type 3 */
    int                  pakMat3Count;
    const char**         pakMat3Names;
    const unsigned int*  pakMat3Tex0;
    const unsigned int*  pakMat3Tex1;
    /* Mat4 — material type 4 */
    int                  pakMat4Count;
    const char**         pakMat4Names;
    const unsigned int*  pakMat4Tex0;
    const unsigned int*  pakMat4Tex1;
    /* MatExtra — extended material data */
    int                  pakMatExtraCount;
    const char**         pakMatExtraNames;
    /* Shape infos — collision shape descriptors */
    int                  pakShapeInfoCount;
    const char**         pakShapeInfoNames;
    const unsigned int*  pakShapeInfoOffsets; /* offset into block1 */
    const unsigned int*  pakShapeInfoKinds;   /* shape kind enum */
    /* Havok shape infos */
    int                  pakHkShapeCount;
    const char**         pakHkShapeNames;
    const unsigned int*  pakHkShapeKinds;     /* kind */
    const unsigned int*  pakHkShapeANums;     /* a_num */
    /* Havok constraint datas */
    int                  pakHkConstraintDataCount;
    const char**         pakHkConstraintDataNames;
    const unsigned int*  pakHkConstraintDataKinds; /* kind */
    /* Vertex buffer infos */
    int                  pakVBuffInfoCount;
    const char**         pakVBuffInfoNames;
    const unsigned int*  pakVBuffInfoSizes;   /* buffer byte size */
    const unsigned int*  pakVBuffInfoOffsets; /* offset */
    /* Index buffer infos */
    int                  pakIBuffInfoCount;
    const char**         pakIBuffInfoNames;
    const unsigned int*  pakIBuffInfoSizes;   /* buffer byte size */
    const unsigned int*  pakIBuffInfoFormats; /* index format */
    /* Havok constraint infos */
    int                  pakHkConstraintInfoCount;
    const char**         pakHkConstraintInfoNames;
    const unsigned int*  pakHkConstraintInfoKinds;      /* kind */
    const unsigned int*  pakHkConstraintInfoBoneParents;/* bone_parents_num */
    /* Particle field infos */
    int                  pakPFieldCount;
    const char**         pakPFieldNames;
    const unsigned int*  pakPFieldGamemodeMasks; /* gamemode_guid */
    const unsigned int*  pakPFieldWidths;        /* width */
    /* Radiosity values infos */
    int                  pakRadiosityCount;
    const char**         pakRadiosityNames;
    const unsigned int*  pakRadiosityNums;    /* num values */
    const unsigned int*  pakRadiosityOffsets; /* offset into block1 */
    /* Foliage infos */
    int                  pakFoliageCount;
    const char**         pakFoliageNames;
    const unsigned int*  pakFoliageKinds;     /* kind */
    /* GameObjs — level entity placement data (scene graph) */
    int                  gameObjCount;
    const char**         gameObjNames;        /* resolved Name field */
    const char**         gameObjTypeNames;    /* resolved type (e.g. "static_object") */
    const unsigned int*  gameObjGuids;
    const unsigned int*  gameObjParentGuids;
    const int*           gameObjGamemodeMasks;
    const unsigned int*  gameObjLayerGuids;
    const char**         gameObjMeshNames;    /* resolved Mesh field (may be empty) */
    const float*         gameObjPosX;         /* position X (from WorldTransform) */
    const float*         gameObjPosY;         /* position Y */
    const float*         gameObjPosZ;         /* position Z */
    int                  gameObjTypeDefCount;
    const char**         gameObjTypeDefNames; /* type definition names */
    const int*           gameObjTypeDefFieldCounts;
    const int*           gameObjTypeDefInstanceCounts;

    /* GameObjs — event system & relationship fields (parallel to gameObjNames) */
    const unsigned int*  gameObjTargetGuids;     /* Output blocks: target GUID */
    const char**         gameObjOutputEvents;    /* Output blocks: "Output" event name */
    const char**         gameObjInputEvents;     /* Output blocks: "Input" command name */
    const float*         gameObjDelays;          /* Output blocks: delay in seconds */
    const int*           gameObjSticky;          /* Output blocks: sticky flag (0/1) */
    const char**         gameObjModeNames;       /* gamemode: "Mode" field (e.g. "goodcampaign") */

    /* GameObjs — GUID array fields (variable-length per entity) */
    /* Packed format: outputsOffsets[i] = index into outputsData where entity i's outputs start */
    /* outputsCounts[i] = how many GUIDs entity i has in its Outputs array */
    int                  gameObjOutputsTotal;     /* total GUIDs in outputsData */
    const unsigned int*  gameObjOutputsData;      /* packed GUID array */
    const int*           gameObjOutputsOffsets;   /* per-entity offset into outputsData */
    const int*           gameObjOutputsCounts;    /* per-entity count */

    int                  gameObjLayersArrTotal;   /* Layers arrays (templateLevel/gamemode) */
    const unsigned int*  gameObjLayersArrData;
    const int*           gameObjLayersArrOffsets;
    const int*           gameObjLayersArrCounts;

    int                  gameObjNodesTotal;       /* Nodes arrays (PathNetwork) */
    const unsigned int*  gameObjNodesData;
    const int*           gameObjNodesOffsets;
    const int*           gameObjNodesCounts;

    /* PathLink: node1/node2 (parallel to gameObjNames) */
    const unsigned int*  gameObjNode1Guids;      /* PathLink: node1 GUID */
    const unsigned int*  gameObjNode2Guids;      /* PathLink: node2 GUID */

    /* Generic GUID references — ALL single-GUID and objectlist fields packed flat.
       Per entity i: refs are at allGuidRefData[allGuidRefOffsets[i]*2 .. +(allGuidRefCounts[i]*2)]
       Each ref = 2 uint32: { fieldNameCRC, targetGUID } */
    int                  allGuidRefTotalPairs;
    const unsigned int*  allGuidRefData;          /* packed pairs: [crc0,guid0, crc1,guid1, ...] */
    const int*           allGuidRefCounts;        /* per entity: number of ref pairs */
    const int*           allGuidRefOffsets;       /* per entity: start pair index in allGuidRefData */

    /* Viewport toggle — host writes viewportEnabled; DLL writes requestToggleViewport */
    int          viewportEnabled;           /* 1 = 3D scene active, 0 = blank */
    int          requestToggleViewport;     /* DLL sets 1 when user clicks toggle button */
    /* Level browser — outputs from DLL to host */
    int          requestLoadLevel;          /* bool: user pressed Load */
    char         requestedLevelPakPath[512];
    char         requestedLevelBinPath[512];
    int          requestLoadStrings;        /* bool: user pressed Load Strings */
    char         requestedStringsPath[512];

    /* Level Blocks → Viewport sync */
    int          requestLoadEntityMesh;     /* DLL sets 1 when user double-clicks entity with mesh */
    char         requestedEntityMeshName[256]; /* mesh name to load in viewport */
    float        requestedEntityPosX;       /* entity world position for camera focus */
    float        requestedEntityPosY;
    float        requestedEntityPosZ;

    /* Editor visualization fields — MUST BE AT END OF STRUCT for backward compatibility
       These are not populated until EXE is rebuilt with VS2005+Havok.
       DLL infers shapes from type names until then. */
    const char**         gameObjEditorShapes;    /* "Type": "Billboard"/"Sphere"/"Box"/"Box_Trigger" */
    const unsigned int*  gameObjEditorColors;    /* "Color": 0xAARRGGBB */
    const char**         gameObjEditorTextures;  /* "Texture": "fed_timer.tga" etc. */
    const float*         gameObjEditorOuters;    /* "Outer": sphere radius */
    const float*         gameObjEditorSizeX;     /* "size" X component */
    const float*         gameObjEditorSizeY;     /* "size" Y component */
    const float*         gameObjEditorSizeZ;     /* "size" Z component */

    /* Level Scene rendering state — set by host before DrawFrame */
    int          sceneLoaded;         /* 1 if level scene geometry is currently rendered */
    int          sceneInstanceCount;  /* number of placed mesh instances in scene */
    int          sceneModelCount;     /* number of unique models loaded */
    /* Level Scene request — set by DLL after DrawFrame */
    int          requestRenderScene;  /* user clicked "Render Full Map" button */

    /* Level Inspector — object picking + details panel */
    int          inspectorHoveredIdx;        /* instance index under cursor (-1=none) */
    int          inspectorSelectedIdx;       /* clicked/selected instance (-1=none) */
    /* Selected instance details (valid when inspectorSelectedIdx >= 0) */
    const char*  inspectorSelName;           /* object name */
    const char*  inspectorSelType;           /* type name */
    const char*  inspectorSelMesh;           /* mesh name */
    unsigned int inspectorSelGuid;
    unsigned int inspectorSelParentGuid;
    int          inspectorSelGameModeMask;
    const float* inspectorSelMatrix;         /* 16 floats, WorldTransform */
    int          inspectorSelPartCount;      /* number of mesh parts */

    /* GameMode filter — input/output */
    int          gameModeFilter;             /* current filter: -1=all, 0=Campaign, 1=TDM, 3=Conquest */

    /* Inspector position editing — output from DLL to host */
    int          inspectorPosChanged;        /* bool: user edited position this frame */
    float        inspectorNewPos[3];         /* new X,Y,Z position if changed */
    int          inspectorResetRequested;    /* bool: user wants to reset to original */
    int          inspectorMatrixChanged;     /* bool: user edited rotation/scale this frame */
    float        inspectorNewMatrix[16];     /* full 4x4 matrix if changed */
    int          requestSavePak;            /* bool: user clicked Save PAK */

    /* ================================================================
     * Mocap Studio (added at END to preserve struct layout)
     * ================================================================ */

    /* Mocap state (input — set by host before DrawFrame) */
    int          mocapState;              /* MocapState enum: 0=idle,1=loading,2=processing,3=streaming,4=done,5=error */
    float        mocapProgress;           /* 0..1 */
    const char*  mocapStatusMsg;          /* current status text */
    const char*  mocapErrorMsg;           /* error text (when state==5) */
    int          mocapTotalFrames;        /* total frames expected */
    int          mocapReceivedFrames;     /* frames received so far */
    float        mocapFps;                /* video FPS */
    int          mocapSubjectCount;       /* number of detected subjects */
    float        mocapPlaybackTime;       /* current playback time (seconds) */
    float        mocapDuration;           /* total animation duration (seconds) */
    int          mocapPlaying;            /* bool: playback active */
    float        mocapFingerCurlL;        /* left hand grip 0..1 */
    float        mocapFingerCurlR;        /* right hand grip 0..1 */

    /* Mocap skeleton preview data (input — set by host) */
    const float* mocapBonePositions;      /* 24*3 floats: SMPL joint world positions for wireframe */
    int          mocapBonePositionCount;  /* number of joints (24) */

    /* Mocap outputs (set by DLL after DrawFrame) */
    int          mocapRequestLoadVideo;   /* bool: user clicked Load Video */
    int          mocapRequestCancel;      /* bool: user clicked Cancel */
    int          mocapRequestTogglePlay;  /* bool: user toggled play/pause */
    int          mocapRequestSetTime;     /* bool: user scrubbed timeline */
    float        mocapRequestedTime;      /* seconds */
    int          mocapRequestConvert;     /* bool: user clicked Convert to Conquest */
    int          mocapRequestApplyToModel;/* bool: user clicked Apply to Model */
    int          mocapRequestSetFingerCurl;/* bool */
    float        mocapRequestedFingerCurlL;
    float        mocapRequestedFingerCurlR;

    /* 3D editor object selection — links viewport pick to Event Graph */
    int          selectedEditorObjIdx;       /* editor obj index in LevelScene (-1=none) */
    unsigned int selectedEditorObjGuid;      /* GUID of selected editor obj (0=none) */
    const char*  selectedEditorObjName;      /* name of selected editor obj */
    const char*  selectedEditorObjType;      /* type_name of selected editor obj */
    float        selectedEditorObjPos[3];    /* world position */

    /* Editor object relation filter — output from DLL to host */
    int                  editorObjFilterActive;   /* bool: filter is toggled on */
    int                  editorObjFilterGuidCount; /* number of GUIDs in filter set */
    const unsigned int*  editorObjFilterGuids;     /* pointer to static array of GUIDs */

    /* Property Editor — field data for selected entity (input to DLL) */
    int                  propEntityGuid;          /* GUID of entity whose props are shown (0=none) */
    const char*          propEntityType;           /* type name */
    int                  propFieldCount;           /* number of fields */
    const char**         propFieldNames;            /* field names [propFieldCount] */
    const char**         propFieldKindNames;        /* kind names [propFieldCount] */
    const unsigned int*  propFieldKindCrcs;         /* kind CRCs [propFieldCount] */
    const unsigned int*  propFieldOffsets;           /* byte offsets [propFieldCount] */
    const int*           propFieldIntVals;          /* int/bool/color values [propFieldCount] */
    const float*         propFieldFloatVals;        /* float values [propFieldCount] */
    const char**         propFieldStringVals;       /* resolved string values [propFieldCount] */
    const unsigned int*  propFieldGuidVals;         /* GUID values [propFieldCount] */
    const float*         propFieldMatrixVals;       /* 16 floats per matrix field [propFieldCount*16] */
    const float*         propFieldVec3Vals;         /* 3 floats per vec3 field [propFieldCount*3] */
    int                  propListTotalGuids;        /* total packed GUIDs for all list fields */
    const unsigned int*  propListGuidData;          /* packed GUID array */
    const int*           propListOffsets;            /* per-field offset into propListGuidData */
    const int*           propListCounts;             /* per-field count */

    /* Property Editor — edit requests (output from DLL) */
    int                  propEditRequested;         /* bool: user edited a field */
    int                  propEditFieldIndex;        /* which field index */
    int                  propEditKind;              /* 0=int, 1=float, 2=guid, 3=vec3, 4=matrix, 5=crc/string */
    int                  propEditIntVal;
    float                propEditFloatVal;
    unsigned int         propEditGuidVal;
    float                propEditVec3Val[3];
    float                propEditMatrixVal[16];

    /* ================================================================
     * Entity Creation (added at END to preserve struct layout)
     * ================================================================ */

    /* Entity Creation — input to DLL from host */
    int          createMenuRequested;        /* bool: Ctrl+RMB hit detected this frame */
    float        createWorldPos[3];          /* world position of right-click */

    /* Available type defs for creation (parallel arrays, input) */
    int          createTypeDefCount;
    const char** createTypeDefNames;         /* type names e.g. "static_object" */
    const int*   createTypeDefFieldCounts;   /* field count per type */
    const int*   createTypeDefInstanceCounts;/* instances in level per type (0 = no donor) */

    /* Hierarchy data for parent picker (input) — reuses gameObj arrays above */
    /* The DLL can use gameObjGuids/gameObjParentGuids/gameObjTypeNames/gameObjLayerGuids */
    /* to build the hierarchy tree for parent selection. */

    /* Entity Creation — output from DLL to host */
    int          createEntityRequested;      /* bool: user confirmed creation */
    int          createEntityTypeDefIdx;     /* which type def to use */
    float        createEntityPos[3];         /* final world position (editable in wizard) */
    unsigned int createEntityParentGuid;     /* parent entity GUID */
    int          createEntityGameModeMask;   /* gamemode bits */
    unsigned int createEntityLayerGuid;      /* layer to assign to */
    char         createEntityName[128];      /* user-entered name */

    /* ================================================================
     * Asset Tree → Map Highlight (added at END to preserve struct layout)
     * ================================================================ */

    /* Asset highlight — output from DLL to host */
    int          assetHighlightType;        /* 0=none, 1=model CRC, 2=texture CRC, 3=gameobj idx, 4=mat tex0 CRC */
    unsigned int assetHighlightCrc;         /* CRC value for types 1,2,4 */
    int          assetHighlightGameObjIdx;  /* gameobj index for type 3 (-1=none) */

    /* ================================================================
     * Asset Data Inspector — F6 (added at END to preserve struct layout)
     * ================================================================ */

    /* F6 toggle — input from host */
    int          assetInspectorToggle;       /* bool: host wants to toggle panel visibility */

    /* Collision visualization state */
    int          collisionVisEnabled;        /* input: 1 if collisions are currently shown */
    int          collisionShapeCount;        /* input: number of collision shapes loaded */
    int          collisionVisToggle;         /* output from DLL: user toggled collision vis */

    /* Inspector request — output from DLL: what asset to inspect */
    int          assetInspectorRequestCat;   /* category from level loader tree (-1=none) */
    int          assetInspectorRequestIdx;   /* index within category (-1=none) */

    /* Inspector data — input to DLL: raw Block1 bytes for selected asset */
    int          assetInspectorValid;        /* bool: data below is valid */
    int          assetInspectorCat;          /* category of inspected asset */
    int          assetInspectorIdx;          /* index within category */
    unsigned int assetInspectorBlock1Off;    /* absolute Block1 offset of this entry */
    int          assetInspectorStride;       /* byte size of this entry */
    const unsigned char* assetInspectorData; /* pointer to raw bytes (stride bytes, read-only) */

    /* Inspector edit — output from DLL: user changed a field value */
    int          assetInspectorEditRequested;/* bool: user edited a field */
    unsigned int assetInspectorEditOffset;   /* absolute Block1 offset of edited field */
    int          assetInspectorEditSize;     /* byte size of edited value (1,2,4,8) */
    int          assetInspectorEditInt;      /* int/u32 value */
    float        assetInspectorEditFloat;    /* float value */

    /* ================================================================
     * Model Picker for Entity Creation (added at END to preserve struct layout)
     * ================================================================ */

    /* Available models for placement — input to DLL from host */
    int                  createModelCount;       /* number of models in level */
    const char**         createModelNames;       /* model name strings [createModelCount] */
    const unsigned int*  createModelCrcs;        /* model CRCs [createModelCount] */

    /* Selected model — output from DLL to host */
    unsigned int         createEntityMeshCrc;    /* selected model CRC (0 = no mesh / editor-only) */

    /* ================================================================
     * Model Viewer (added at END to preserve struct layout)
     * ================================================================ */

    /* Save PAK status — input from host */
    int                  savePakStatus;          /* 0=idle, 1=saving, 2=compressing, 3=writing, 4=reloading, 5=done, -1=error */
    const char*          savePakMessage;         /* status message string */

    /* Model viewer — input from host */
    void*                modelViewerTexture;      /* IDirect3DTexture9* rendered by host, displayed by DLL */
    int                  modelViewerTexW;         /* render target width */
    int                  modelViewerTexH;         /* render target height */
    int                  modelViewerHasCollision; /* bool: collision data exists for viewed model */
    int                  modelViewerCollisionTris;/* triangle count in generated collision */
    int                  modelViewerBuildProgress;/* 0-100 progress, -1 = not building */
    const char*          modelViewerModelName;    /* name of currently viewed model */

    /* Model viewer — output from DLL to host */
    int                  modelViewerOpen;         /* bool: window is open */
    int                  modelViewerRequestOpen;  /* bool: user clicked "Model Viewer" button */
    unsigned int         modelViewerRequestCrc;   /* CRC of model to view (from asset tree click etc.) */
    int                  modelViewerWireframe;    /* bool: wireframe mode */
    int                  modelViewerShowCollision;/* bool: show collision wireframe overlay */
    int                  modelViewerBuildRequested;/* bool: user clicked "Build Collision" */
    int                  modelViewerSaveRequested; /* bool: user clicked "Save to PAK" */
    float                modelViewerOrbitYaw;     /* camera orbit yaw (radians) */
    float                modelViewerOrbitPitch;   /* camera orbit pitch (radians) */
    float                modelViewerOrbitDist;    /* camera orbit distance */

    /* ================================================================
     * Editor Object Filtering — Phase 1+ (MUST BE AT END)
     * ================================================================ */
    float                editorObjMaxDist;        /* outer cull radius in meters, 0 = disabled */
    float                editorObjFadeStart;      /* inner fade start (auto: maxDist * 0.7) */
    float                editorObjMinDist;        /* min exclusion zone radius, 0 = disabled */
    int                  editorObjVisibleCount;   /* output: how many passed filter this frame */
    int                  editorObjTotalCount;     /* output: total editor objects in level */
    unsigned int         editorObjCategoryMask;   /* Phase 2: bitmask of visible categories, 0x1FFF=all */
    /* Phase 3: search filter — supports prefixes: "type:spawn", "layer:Defense", or plain substring */
    int                  editorObjSearchMode;     /* 0=name, 1=type, 2=layer */
    char                 editorObjSearchTerm[128]; /* parsed search term (prefix stripped) */
    int                  editorObjLabelMaxCount;  /* Phase 4: max labels to render, 0=unlimited */
    unsigned int         requestSelectEditorObjGuid; /* DLL→host: select this editor obj by GUID, 0=no request */
    unsigned int         requestFocusEditorObjGuid;  /* DLL→host: fly camera to this editor obj by GUID, 0=no request */
    /* Team filter (extracted per entity, -1=unknown) */
    const int*           gameObjTeams;               /* per-entity Team field value (-1=unknown, 0=neutral, 1=blue, 2=red, 31=CP-controlled) */

    /* Phase 3-5 visualization toggles (level.json integration) */
    int          showPathLinks;              /* input: current state */
    int          showCaptureRadii;           /* input: current state */
    int          showSpawnChains;            /* input: current state */
    int          togglePathLinks;            /* output from DLL: user toggled */
    int          toggleCaptureRadii;         /* output from DLL: user toggled */
    int          toggleSpawnChains;          /* output from DLL: user toggled */

    /* Level validation */
    int          requestValidateLevel;       /* output from DLL: user clicked Validate */
    int          validationErrorCount;       /* input: last validation error count */
    int          validationWarningCount;     /* input: last validation warning count */

    /* Level templates */
    int          requestNewLevelTemplate;    /* output from DLL: 0=none, 1=Conquest, 2=TDM, 3=CTR */

    /* Phase 6-7 visualization toggles (AI goals, sound radii) */
    int          showAIGoals;                /* input: current state */
    int          showSoundRadii;             /* input: current state */
    int          toggleAIGoals;              /* output from DLL: user toggled */
    int          toggleSoundRadii;           /* output from DLL: user toggled */

    /* E2: Output entity event autocomplete — output from DLL */
    char         createOutputEvent[64];      /* selected output event name (e.g. "OnCapture") */
    char         createInputAction[64];      /* selected input action name (e.g. "Activate") */

    /* Construction Kit creation — output from DLL */
    int          createKitRequested;         /* bool: user confirmed kit creation */
    int          createKitType;              /* LC_KIT_* constant */
    float        createKitPos[3];            /* world position for kit center */
    int          createKitGameModeMask;      /* gamemode bits */
    unsigned int createKitParentGuid;        /* parent entity (typically a templateLayer) */
    unsigned int createKitLayerGuid;         /* layer to assign entities to */
    char         createKitPrefix[32];        /* naming prefix (e.g. "CQ", "TDM") */
    int          createKitCPCount;           /* number of capture points (1-6, for Conquest) */

    /* Gamemode creation — output from DLL */
    int          createGamemodeRequested;   /* bool: user confirmed gamemode creation */
    int          createGamemodeType;        /* LC_MODE_* constant */
    unsigned int createGmAtmosphereGuid;    /* Atmosphere GUID reference */
    unsigned int createGmMapNWGuid;         /* MapNW Locator GUID */
    unsigned int createGmMapSEGuid;         /* MapSE Locator GUID */
    unsigned int createGmLayerGuids[8];     /* Layers[] GUID array (up to 8) */
    int          createGmLayerCount;        /* number of valid entries in createGmLayerGuids */
    unsigned int createGmLayerGuid;         /* which layer this gamemode entity itself sits on */
    char         createGmName[128];         /* entity name */

    /* Property Editor — string/CRC edit value (kind=5) */
    char         propEditStringVal[128];    /* output from DLL: new string value for CRC/String fields */

    /* Gamemode ModeSpecificBanks (output from DLL) */
    char         createGmBanks[3][128];    /* up to 3 audio bank filenames */
    int          createGmBankCount;        /* number of valid banks */

    /* ── New fields added at END to avoid VS2005/VS2022 layout mismatch ── */
    int          gmfShowGlobals;            /* show GMM=-1 entities when filtering */
    int          gmfShowScripts;            /* show GMM=0 entities when filtering */
    unsigned int gmfBitMask;               /* bitmask of which gamemode bits to show */
    unsigned int levelBlockSelGuid;         /* GUID of entity selected in Level Blocks panel (0=none) */

    /* After Save PAK: host writes the saved _modified path here so the DLL
       can update its file-path text field. Next reload will use _modified. */
    char         lastSavedPakPath[512];     /* set by host after successful save, '\0' if none */

    /* Cinematic playback — DLL output, host reads after DrawFrame */
    int          cinePlayRequested;         /* 1=play, 2=stop, 0=no change */
    int          cineCameraIndex;           /* index into cineCameras array */
    /* Cinematic camera data — host input, populated from LevelScene */
    int          cineCameraCount;           /* number of cinematic cameras */
    const char** cineCameraNames;           /* array of name pointers */
    const unsigned int* cineCameraGuids;    /* array of GUIDs */
    const float* cineCameraFovs;            /* FOV per camera */
    const float* cineCameraDurations;       /* TotalDuration per camera */
    const float* cineCameraPosTimes;        /* PositionTravelTime per camera */
    const float* cineCameraTgtTimes;        /* TargetTravelTime per camera */
    /* Cinematic playback state — host input for DLL display */
    int          cineIsPlaying;             /* bool: currently playing */
    float        cinePlaybackTime;          /* current playback time (seconds) */
    float        cinePlaybackDuration;      /* total duration of current cinematic */
    /* Selected camera's spline data — host populates for DLL editing */
    int          cineSelPosNodeCount;       /* position track node count */
    const float* cineSelPosNodes;           /* x,y,z,arc × count */
    int          cineSelTgtNodeCount;       /* target track node count */
    const float* cineSelTgtNodes;           /* x,y,z,arc × count */
    unsigned int cineSelPosTrackGuid;       /* position track GUID */
    unsigned int cineSelTgtTrackGuid;       /* target track GUID */
    const char*  cineSelPosTrackName;       /* position track name */
    const char*  cineSelTgtTrackName;       /* target track name */
    /* Cinematic panel state — DLL output */
    int          cinePanelOpen;             /* bool: cinematic panel is open */
    int          cinePanelSelCamera;        /* selected camera index (-1=none) */
    /* Cinematic property edits — DLL output, host creates FieldEdits */
    int          cineEditRequested;         /* bool: DLL wants to edit a camera property */
    unsigned int cineEditGuid;              /* GUID of entity to edit */
    char         cineEditField[64];         /* field name */
    int          cineEditKind;              /* 0=int, 1=float, 2=guid, 4=matrix, 5=string */
    float        cineEditFloatVal;          /* value for kind=1 */
    int          cineEditIntVal;            /* value for kind=0 */
    unsigned int cineEditGuidVal;           /* value for kind=2 */
    float        cineEditMatrixVal[16];     /* value for kind=4 */
    char         cineEditStringVal[128];    /* value for kind=5 */
    /* Spline node edits — DLL output */
    int          cineNodeEditRequested;     /* bool: DLL wants to edit spline nodes */
    unsigned int cineNodeEditGuid;          /* GUID of spline entity */
    int          cineNodeEditCount;         /* number of nodes */
    const float* cineNodeEditData;          /* pointer to float4 × count (DLL static buffer) */
    /* Spline list for track swap dropdowns — host input */
    int          cineSplineCount;           /* number of splines in level */
    const char** cineSplineNames;           /* name pointers */
    const unsigned int* cineSplineGuids;    /* GUID array */
    /* Pending field-edit queue summary (Chain editor safety UX) */
    int          pendingFieldEditCount;             /* host input: size of m_fieldEdits */
    int          pendingFieldEditsCancelRequested;  /* DLL output: bool, clear all pending edits */
    /* Chain-editor: delete one connection (zero the source's Outputs[] slot holding outputGuid).
       Safe because game iterators skip zero GUIDs — soft delete, no list-header rebuild needed. */
    int          chainDeleteConnectionRequested;    /* DLL output: bool */
    unsigned int chainDeleteSourceGuid;             /* DLL output: source entity whose Outputs[] to patch */
    unsigned int chainDeleteOutputGuid;             /* DLL output: the Output entity GUID to remove from that list */
    /* Chain-editor: add one connection. Creates a new Output entity and writes its GUID into
       a zero slot in source's Outputs[] list. If no zero slot exists, the add fails (until
       RebuildOutputsList lands in a later chunk). */
    int          chainAddConnectionRequested;       /* DLL output: bool */
    unsigned int chainAddSourceGuid;                /* DLL output: source entity */
    unsigned int chainAddTargetGuid;                /* DLL output: target entity */
    char         chainAddOutputEventName[64];       /* DLL output: event name on source (e.g. "OnDeath") */
    char         chainAddInputActionName[64];       /* DLL output: action on target (e.g. "Activate") */
    float        chainAddDelay;                     /* DLL output: seconds */
    int          chainAddSticky;                    /* DLL output: 0/1 */
    char         chainAddParameter[64];             /* DLL output: optional string param */
    int          chainAddResult;                    /* HOST input: 0=none, 1=ok, 2=no-zero-slot, 3=type-missing, 4=bad-args */
    unsigned int chainAddLastNewGuid;               /* HOST input: the Output entity GUID that was created (if ok) */
    /* Outputs-list diagnostics for currently-selected source entity.
       Populated by host each frame when s_lbSelIdx points to an entity with Outputs. */
    int          selEntOutputsTotal;                /* host input: total Outputs[] slot count */
    int          selEntOutputsUsed;                 /* host input: non-zero GUIDs */
    int          selEntOutputsZeroSlots;            /* host input: zero GUIDs (= available for Add) */
    /* Pending field-edit list for per-edit revert UI. Parallel arrays sized pendingFieldEditCount. */
    const unsigned int* pendingFieldEditEntityGuids;  /* host input */
    const char**        pendingFieldEditFieldNames;   /* host input */
    const int*          pendingFieldEditKinds;        /* host input: 0..7 */
    int                 pendingFieldEditRevertIndex;  /* DLL output: index to erase, -1 = none */
    /* Live-collaboration session (TCP on LAN). Host + clients model. */
    int                 collabRole;                   /* host input: 0=solo,1=host,2=client */
    int                 collabStatus;                 /* host input: 0=idle,1=hosting,2=connecting,3=handshake,4=connected,5=error */
    const char*         collabStatusText;             /* host input: short message for toolbar */
    int                 collabPeerCount;              /* host input: # connected peers */
    unsigned short      collabPort;                   /* host input: host's bound port (if hosting) */
    const char*         collabLocalIp;                /* host input: host's local IP display string */
    unsigned char       collabMyGuidPrefix;           /* host input: 0=host native, else client's assigned byte */
    /* DLL-driven session operations (write-1-trigger). Host zeros each after read. */
    int                 collabHostRequested;          /* DLL output: start hosting */
    int                 collabJoinRequested;          /* DLL output: join host */
    int                 collabStopRequested;          /* DLL output: end session */
    char                collabJoinIp[64];             /* DLL output: ip to join */
    unsigned short      collabJoinPort;               /* DLL output: port to join */
    unsigned short      collabHostPort;               /* DLL output: port to host on (default 1818) */
    int                 collabJoinNoLocalPak;         /* DLL output: 1 = client has no PAK locally, ask host to stream */
    float               collabXferProgress;           /* host input: 0..1 during PAK streaming to client */
    unsigned int        collabXferDoneKB;             /* host input: bytes done / 1024 (DLL fits u32) */
    unsigned int        collabXferTotalKB;            /* host input: bytes total / 1024 */
    /* ── SplineEditor (Cinematic page rewrite) ─────────────────────────
       New fields ALWAYS go at the END of this struct. New members
       slipped into the middle break the .obj layout for every file
       that pre-compiled against the old struct, producing ACCESS_VIOLATION
       crashes deep in nthdll free() that look completely unrelated.
       See feedback_struct_layout.md memory. END. ALWAYS. END. */
    int                 cineSplineEditMode;           /* DLL output: 0=off, 1=editing pos track, 2=editing tgt track */
    float               cineCurvatureAmount;          /* DLL output: -1..+1, sideways bend strength */
    int                 cineCurvatureFreq;            /* DLL output: 1..5, 1=crescent, 2=S, 3+=snake */
    /* Zone B QoL additions. Appended at END to preserve struct layout. */
    int                 pendingEntityCount;           /* host input: pending entities not yet baked */
    int                 pendingDeletedGuidCount;      /* host input: deleted GUIDs pending save */
    int                 pendingCollisionCount;        /* host input: pending collision meshes */
    int                 multiEditTeamRequested;       /* DLL output: apply Team to many entities */
    int                 multiEditTeamValue;           /* DLL output: 0=neutral,1=blue,2=red,31=both/CP */
    int                 multiEditGuidCount;           /* DLL output: number of GUIDs in multiEditGuids */
    const unsigned int* multiEditGuids;               /* DLL output: selected entity GUIDs */
    /* Phase 1 XSI authoring: F4 event-wiring overlay control.
       Appended at END. Same struct-layout rule, do NOT slip these into
       the middle of the struct or you will be hunting heap corruption
       at midnight wishing you had read the comment. */
    int                 eventWiresShow;               /* DLL output: 0=hide owner->Output->target wires, 1=show */
    unsigned int        eventWireFocusGuid;           /* DLL output: 0=draw all wires, non-zero=isolate this Output's chain */
    /* Phase 2 XSI authoring: scene hierarchy outliner drag-reparent.
       DLL fills these when the user drags an entity onto another entity
       in the outliner panel. Host consumes them ONCE per frame, applies
       a FieldEdit on ParentGUID, updates the in-memory parent_guid so
       the next frame's tree shows the move. Both zero = no request. */
    unsigned int        requestReparentChildGuid;     /* DLL output: entity that got dragged */
    unsigned int        requestReparentNewParentGuid; /* DLL output: new parent (0 = make top-level) */
    /* Phase 3 XSI authoring: layer manager.
       DLL is the source of truth for layer hide/lock/solo/active state.
       Every frame DLL fills these output arrays from its INI-persisted
       sets. Host pushes them straight into LevelScene each frame, so
       the renderer's layer filter and the panel checkbox state can
       never desync. Pointers stay valid until next DrawFrame call. */
    int                 hiddenLayerCount;             /* DLL output: count of GUIDs in hiddenLayerGuids */
    const unsigned int* hiddenLayerGuids;             /* DLL output: layers whose entities should be hidden */
    int                 lockedLayerCount;             /* DLL output: count of GUIDs in lockedLayerGuids */
    const unsigned int* lockedLayerGuids;             /* DLL output: layers whose entities should be unpickable */
    unsigned int        isolatedLayerGuid;            /* DLL output: 0=no isolation, non-zero=show only this layer */
    unsigned int        activeLayerGuid;              /* DLL output: where new entities go (0=no active layer) */
    /* Phase 4 XSI authoring: LEVEL viewport gizmo (translate / rotate /
       scale). Renamed `levelGizmo*` because earlier in this struct
       there is already a `gizmoMode`/`gizmoSpace` pair owned by the
       Scene3DRenderer bone gizmo (see line 665). The two manipulators
       live in different worlds, do not share state, and would collide
       if we reused the names.
       levelGizmoMode: 0=off, 1=translate, 2=rotate, 3=scale (W/E/R in XSI).
       levelGizmoSpace: 0=world, 1=local.
       levelGizmoSnap: world-units for translate/scale, degrees for rotate.
       levelGizmoDragging: host-input, 1 if a gizmo drag is currently
                           live so the DLL can paint the toolbar
                           buttons differently. */
    int                 levelGizmoMode;               /* DLL output */
    int                 levelGizmoSpace;              /* DLL output */
    float               levelGizmoSnap;               /* DLL output */
    int                 levelGizmoDragging;           /* host input */
    /* Phase 6 XSI authoring: general spline/path control handles.
       The original cinematic-track editor (cineSplineEditMode) was
       hard-wired to camera position/target tracks because that is the
       fucking ONE thing the OG author cared about. Any other spline-
       bearing entity in the level (PathNetworks, cutscene tracks,
       Pandemic's mystery spline blobs we still have not fully decoded)
       got the FULL Microsoft-Excel experience: open the inspector,
       type x/y/z numbers into DragFloat fields, alt-tab between two
       windows like it was 1997.
       This block grants the SAME goddamn drag/insert/delete UX to
       any entity that owns spline_nodes. Picks via the entity the
       user already has selected, runs the existing FABRIK chain
       solver, and writes back through the existing cineNodeEditData
       persistence path. Zero new save plumbing.
       APPENDED AT END per the struct-layout-end commandment. Read it
       again. END. ALWAYS. END. Slip a field into the middle of this
       struct and you will be hunting heap corruption inside ntdll
       free() at an ungodly hour wondering why your save pipeline
       miscompares by exactly four bytes. Do not be that guy. */
    int                 splineHandlesEnabled;         /* DLL output: 1 = show + drive handles */
    unsigned int        splineHandlesFocusGuid;       /* host input: GUID of currently selected spline (0 = none) */
    int                 splineHandleShiftHeld;        /* DLL output: 1 = Shift is held this frame (insert-node modifier) */
    int                 splineHandleDeletePressed;    /* DLL output: 1 = Delete was just pressed (remove selected node) */
    /* Phase 9c (final consolidation): hotkey requests parked from
       Phase 7. H = toggle hidden state of the focused entity's layer.
       Ctrl+D = duplicate the focused entity through the existing
       PendingGameObj clone pipeline. Both writes from the host
       hotkey block, both consumed downstream:
       - hostRequestToggleLayerHiddenGuid: DLL consumes in always-write
         block, toggles g_layerHidden membership, EditorFilterSave().
       - hostRequestDuplicateEntityGuid: host consumes inline (the
         duplicate fan-out lives in LevelReader, not in the DLL).
       APPENDED AT END per the struct-layout-end commandment. */
    unsigned int        hostRequestToggleLayerHiddenGuid;  /* host output: layer GUID to toggle hidden, 0 = none */
    unsigned int        hostRequestDuplicateEntityGuid;    /* host output: entity GUID to clone, 0 = none */
    /* Phase 10e: entity authoring requests from The Forge.
       APPENDED AT END per the struct-layout-end commandment. Fields:
       - hostRequestDeleteEntityGuid: DLL writes when the user picks
         "Delete Entity" or hits Delete on a focused selection. EXE
         consumer mirrors the existing wipe-layer path: AddDeletedGuid
         on the LevelReader, removeByGuid on the LevelScene, erase from
         the in-memory gos list, and rebuild the UI pointer arrays.
       - hostRequestCreateEntityFromTemplateGuid: DLL writes the GUID
         of an existing level entity to be cloned as a "fresh" instance
         (renamed, moved off-origin, added to the active layer). Same
         pipeline as Ctrl+D duplicate but invoked from the Create
         Entity wizard instead of the hotkey.
       Both consumed in DrawFrame's host-action half. Host zeros each
       after read so the DLL only fires once per click. */
    unsigned int        hostRequestDeleteEntityGuid;             /* DLL output: entity GUID to delete, 0 = none */
    unsigned int        hostRequestCreateEntityFromTemplateGuid; /* DLL output: template entity GUID to clone-as-new, 0 = none */

    /* ─────────────────────────────────────────────────────────────────────
       3dCrowd inspector — DLL↔host channel for the crowd properties panel.
       APPENDED AT THE FUCKING END. The struct-layout-end commandment is
       not a suggestion. Insert above this block and the .obj cache goes
       out of sync, your imgui_d3d9.dll renders against a phantom layout,
       the inspector reads from random heap, the editor crashes and you
       look at me like *I* did it. Don't.

       HOST → DLL (read by panel):
         selInstIsCrowd       1 if the currently selected LevelInstance is
                              a crowd member (back-refs set), 0 otherwise.
                              Drives whether the crowd panel appears.
         selCrowdItemIdx      Source-of-truth back-ref into m_crowdItems.
         selCrowdValIdx       Source-of-truth back-ref into instances[].
         selCrowdArchetype    NUL-terminated key_main string for display
                              ("CRD_CH_urk_spr_all_01" etc.). 96 chars
                              covers every observed archetype + margin.
         selCrowdMeshName     NUL-terminated key (pose-snapshot mesh).
         selCrowdRightHand    NUL-terminated right-hand weapon name.
         selCrowdLeftHand     NUL-terminated left-hand weapon name.
         selCrowdAnimCount    How many clips in the archetype's anim list.
         selCrowdInstCount    Total placements in the same CrowdItem.
         selCrowdPlaybackRate unk_4 — animation time scale.
         selCrowdLod          Current LOD squared-distance for this placement.
         selCrowdPosX/Y/Z     Current world position for this placement.
         selCrowdRotation     Current Y-yaw radians for this placement.

       DLL → HOST (requests fired on button click, host zeros after read):
         crowdDeleteRequested      1 = delete the selected crowd member.
                                   Reads selCrowdItemIdx/ValIdx for the target.
         crowdCloneRequested       1 = clone the selected crowd member at
                                   the same position + 1m on world +X.
         crowdLodEditRequested     1 = host should apply pendingCrowdLod
                                   to the selected member. The DLL's slider
                                   sets pendingCrowdLod every frame; the
                                   request flag fires on slider commit so
                                   the host doesn't dirty-flag every drag-
                                   frame uselessly.
         pendingCrowdLod           New LOD squared-distance to write.
         crowdPlaybackEditRequested  1 = host should apply pendingCrowdPlayback
                                   to the entire CrowdItem (playbackRate
                                   is a per-archetype field, not per-instance).
         pendingCrowdPlayback      New playbackRate to write.

       ───────────────────────────────────────────────────────────────────── */
    int                  selInstIsCrowd;
    int                  selCrowdItemIdx;
    int                  selCrowdValIdx;
    char                 selCrowdArchetype[96];
    char                 selCrowdMeshName[96];
    char                 selCrowdRightHand[96];
    char                 selCrowdLeftHand[96];
    int                  selCrowdAnimCount;
    int                  selCrowdInstCount;
    float                selCrowdPlaybackRate;
    float                selCrowdLod;
    float                selCrowdPosX;
    float                selCrowdPosY;
    float                selCrowdPosZ;
    float                selCrowdRotation;
    int                  crowdDeleteRequested;
    int                  crowdCloneRequested;
    int                  crowdLodEditRequested;
    float                pendingCrowdLod;
    int                  crowdPlaybackEditRequested;
    float                pendingCrowdPlayback;

    /* Crowd archetype enumeration — host populates these before DrawFrame
       so the right-click "Place Crowd Character Here" submenu can list
       every CrowdItem currently in the level. crowdArchetypeNames is a
       pointer to an array of NUL-terminated strings (host owns the storage).
       crowdArchetypeCount is the array length. The DLL only reads — never
       writes the pointer. Host re-points it each frame (no aliasing
       lifetime worry as long as the host vector survives till DrawFrame
       returns, which it does because both run on the main thread). */
    int                  crowdArchetypeCount;
    const char* const*   crowdArchetypeNames;

    /* DLL→host: place a new crowd character at the given world pos using
       the archetype index from the crowdArchetypeNames list above. The
       host calls addCrowdInstanceToItem(pendingCrowdPlaceArchetypeItem,
       ...) and rebuilds the LevelInstances. Fires once per menu click. */
    int                  crowdPlaceRequested;
    int                  pendingCrowdPlaceArchetypeItem;
    float                pendingCrowdPlacePos[3];

    /* ─────────────────────────────────────────────────────────────────────
       3dCrowd Generator — bulk spawn N crowd characters of the same
       archetype scattered in a circle around a world point. DLL renders
       the Crowd Generator window with the knobs; user hits Spawn; the
       request flag fires; host runs the random-placement loop and
       appends every result via addCrowdInstanceToItem, then triggers
       one rebuildCrowdInstances at the end so the LevelInstance vector
       only gets churned once instead of N times.

       Default reasonable values the host sets when the user FIRST opens
       the generator panel via the right-click "Generate Crowd Cluster
       Here" menu item:
         pendingCrowdGenArchetype  = 0 (first available archetype)
         pendingCrowdGenCount      = 10
         pendingCrowdGenRadius     = 5.0 (meters)
         pendingCrowdGenRandomYaw  = 1 (yes, vary per instance)
         pendingCrowdGenJitterY    = 0.0 (no vertical jitter)
         pendingCrowdGenPos[]      = right-click world position
       ───────────────────────────────────────────────────────────────────── */
    int                  crowdGeneratorOpen;            /* DLL window-visibility state (host doesn't touch) */
    int                  crowdGenerateRequested;        /* DLL→host: fire the random-placement loop */
    int                  pendingCrowdGenArchetype;      /* CrowdItem index (-1 invalid) */
    int                  pendingCrowdGenCount;          /* how many instances to spawn */
    float                pendingCrowdGenRadius;         /* circle radius around pendingCrowdGenPos[] (meters) */
    int                  pendingCrowdGenRandomYaw;      /* 1 = random per instance, 0 = all face -Z */
    float                pendingCrowdGenJitterY;        /* +/- Y jitter range (0 = flat ring) */
    float                pendingCrowdGenPos[3];         /* circle center (usually right-click world pos) */

    /* ─────────────────────────────────────────────────────────────────────
       3dCrowd Editor — author the 3dCrowd block itself. Sibling to the
       Inspector (per-instance editing) and Generator (bulk spawn). Lets
       the user list every CrowdItem header, focus on one (hiding the
       other 6 in the viewport), rename its `key`, swap mesh / weapon
       archetypes via autocomplete, edit playback rate, manage the anim
       CRC list, add/remove instances per item, and add/duplicate/delete
       whole CrowdItems.

       Pattern (matches Inspector/Generator):
         host populates display fields pre-DrawFrame
         DLL draws the panel, fires request flags on user input
         host consumes request flags post-DrawFrame, calls LevelScene
         host triggers ONE rebuildCrowdInstances() at the end

       Custom strings the user invents in `key` go into a queue on
       LevelReader; they only hit pak_strings.json at level save time
       (matches the ExportPendingToJson append pattern).

       Focus mode is editor-only — doesn't touch the in-game DLL's
       runtime hide path. It just gates rebuildCrowdInstances() so
       non-focused items don't emit LevelInstances. Toggle re-rebuilds.
       ───────────────────────────────────────────────────────────────────── */
    int                  crowdEditorOpen;                  /* menubar toggle: View → 3dCrowd Editor */

    int                  crowdEditorItemCount;             /* host writes: number of CrowdItems */
    char                 crowdEditorItemKeys[64][96];      /* host writes: item.meshKeyName (the 'key' user invents) */
    char                 crowdEditorItemMeshNames[64][96]; /* host writes: item.modelKeyName (key_main) */
    int                  crowdEditorItemInstCounts[64];    /* host writes: item.instances.size() */

    int                  crowdEditorSelectedItem;          /* DLL writes on click; -1 = no selection */
    int                  crowdEditorFocusModeOn;           /* DLL toggles; host reads + applies in rebuild */
    int                  crowdEditorFocusedItem;           /* host mirrors the selected item when focus toggles on */

    /* Selected item header — host populates from m_crowdItems[selectedItem] */
    char                 crowdEditorSelKey[96];
    char                 crowdEditorSelKeyMain[96];
    char                 crowdEditorSelKeyRight[96];
    char                 crowdEditorSelKeyLeft[96];
    float                crowdEditorSelPlaybackRate;
    int                  crowdEditorSelAnimCount;
    char                 crowdEditorSelAnimNames[64][96];
    int                  crowdEditorSelInstCount;

    /* Autocomplete sources — host enumerates from LevelScene::m_modelCache
       and the union of all CrowdItem anim lists each frame */
    int                  crowdEditorMeshOptionCount;       /* CRD_/HMD_/PR_ etc. */
    char                 crowdEditorMeshOptions[512][96];
    int                  crowdEditorWeaponOptionCount;     /* WP_ prefix */
    char                 crowdEditorWeaponOptions[256][96];
    int                  crowdEditorAnimOptionCount;       /* full level anim list */
    char                 crowdEditorAnimOptions[256][96];

    /* Request flags + pending values — DLL fires once, host consumes + clears */
    int                  crowdEditorRenameKeyRequested;
    char                 pendingCrowdEditorKey[96];
    int                  crowdEditorSetKeyMainRequested;
    char                 pendingCrowdEditorKeyMain[96];
    int                  crowdEditorSetKeyRightRequested;
    char                 pendingCrowdEditorKeyRight[96];
    int                  crowdEditorSetKeyLeftRequested;
    char                 pendingCrowdEditorKeyLeft[96];
    int                  crowdEditorSetPlaybackRequested;
    float                pendingCrowdEditorPlayback;
    int                  crowdEditorAddAnimRequested;
    char                 pendingCrowdEditorAddAnim[96];
    int                  crowdEditorRemoveAnimRequested;
    int                  pendingCrowdEditorRemoveAnimIdx;
    int                  crowdEditorAddInstanceRequested;  /* host computes position at camera target */
    int                  crowdEditorAddNewItemRequested;
    char                 pendingCrowdEditorNewKey[96];
    char                 pendingCrowdEditorNewKeyMain[96];
    int                  crowdEditorDuplicateItemRequested;
    int                  crowdEditorDeleteItemRequested;
    int                  crowdEditorFocusToggleRequested;

    /* 3dCrowd Editor v1.1 — skeleton-compat anim ordering. Host sorts
       crowdEditorAnimOptions so the first N entries are name-token-
       compatible with the currently-selected CrowdItem's mesh. DLL
       renders compat anims highlighted; remainder kept available for
       manual override. -1 = no compat sort applied (e.g. no item selected
       or mesh has no distinctive tokens). Appended at end per the
       struct-layout-end commandment. */
    int                  crowdEditorAnimCompatCount;

    /* 3dCrowd Inspector v1.3 — manual pos/rot edit. DLL drags or types,
       writes the user's intended pos+yaw into pendingCrowdPosRot[3]+
       pendingCrowdRotation, fires crowdPosRotEditRequested; host calls
       setCrowdInstancePosRot(selCrowdItemIdx, selCrowdValIdx, ...). */
    int                  crowdPosRotEditRequested;
    float                pendingCrowdPosX;
    float                pendingCrowdPosY;
    float                pendingCrowdPosZ;
    float                pendingCrowdRotation;

    /* 3dCrowd Editor v1.4 — in-panel per-instance editing. The Editor
       lists every instance of the currently-selected CrowdItem; clicking
       a row sets crowdEditorSelInst (DLL state) and the host populates
       crowdEditorSelInstPos/Rot/Lod for display. User types new values,
       fires crowdEditorInstEditRequested; host applies via
       setCrowdInstancePosRot + LOD direct mutation. Delete via
       crowdEditorDeleteInstRequested. Picking-broken workaround: this
       lets the user edit instances WITHOUT a viewport click. */
    int                  crowdEditorSelInst;           /* -1 = no inst focused */
    float                crowdEditorSelInstPos[3];
    float                crowdEditorSelInstRot;
    float                crowdEditorSelInstLod;
    int                  crowdEditorInstEditRequested;
    float                pendingCrowdEditorInstPos[3];
    float                pendingCrowdEditorInstRot;
    float                pendingCrowdEditorInstLod;
    int                  crowdEditorDeleteInstRequested;

    /* ─────────────────────────────────────────────────────────────────────
       Crowd Mesh Builder — author merged character+weapon meshes so a
       hero rig (CH_elf_Haldir_01) can be used as a crowd archetype with
       weapons VISIBLY in hand. The shipping engine doesn't runtime-attach
       key_right/key_left weapons (verified via asm — see analyzed/Crowd
       README). Weapons only appear on CRD meshes because they're baked
       as mesh parts of the model itself. This panel lets the user pick
       a base mesh + weapon(s) + attach bone, preview the placement, and
       bake a new merged model that's properly registered in the level.

       Mesh-merge math runs in level_patcher.py at save time (where the
       lotrc_rs JSON format is well-understood). C++ side just emits the
       spec sidecar ze_merged_models.json.

       Appended at the END per the struct-layout-end commandment.
       ───────────────────────────────────────────────────────────────────── */
    int                  crowdBuilderOpen;                  /* menubar toggle */

    /* Mesh lists — host enumerates from m_modelCache each frame */
    int                  crowdBuilderBaseMeshOptionCount;
    char                 crowdBuilderBaseMeshOptions[512][96]; /* CH_*, CRD_* */
    int                  crowdBuilderWeaponMeshOptionCount;
    char                 crowdBuilderWeaponMeshOptions[256][96]; /* WP_*, PR_* */

    /* Selection state — DLL writes on click; host keeps via static */
    int                  crowdBuilderSelBaseIdx;
    int                  crowdBuilderSelWeaponIdx;
    char                 crowdBuilderSelBaseName[96];
    char                 crowdBuilderSelWeaponName[96];

    /* Bone list — host populates from selected base mesh's bones */
    int                  crowdBuilderBoneOptionCount;
    char                 crowdBuilderBoneOptions[128][64];
    int                  crowdBuilderSelBoneIdx;
    char                 crowdBuilderSelBoneName[64];

    /* Offset tweaks — drag in panel, live preview updates */
    float                crowdBuilderOffsetXYZ[3];
    float                crowdBuilderOffsetYawPitchRoll[3]; /* radians */

    /* New model name — typed by user; goes into ze_merged_models.json
       and pak_strings on bake */
    char                 crowdBuilderNewMeshName[96];

    /* Request flags */
    int                  crowdBuilderPreviewActive;         /* host spawns preview when set */
    int                  crowdBuilderBakeRequested;         /* one-shot, host consumes */

    /* Strip Baked Weapon — second sub-tool inside Crowd Mesh Builder, lets
     * the user take an existing CRD whose weapon is baked into the body
     * mesh (e.g. CRD_CH_elf_ancn_swd_all_01 has its sword as part of slot 0)
     * and write out a NEW CRD with weapon-region vertices stripped. Host
     * collects the source/bones/new-name fields on Strip request and emits
     * a ze_strip_specs.json entry; Python applies it before merge step.
     * NEW MEMBERS AT END per the struct-layout-end rule. */
    int                  stripPanelOpen;                    /* sub-panel toggle */
    int                  stripSourceCrdOptionCount;
    char                 stripSourceCrdOptions[256][96];    /* CRD_* model names */
    int                  stripSourceSelIdx;
    char                 stripSourceSelName[96];
    int                  stripBoneOptionCount;
    char                 stripBoneOptions[128][64];         /* bones of selected source */
    int                  stripBoneSelected[128];            /* 1 = checked, 0 = unchecked */
    char                 stripNewName[96];
    int                  stripRequested;                    /* one-shot, host consumes */
    /* UV-region stripping. When stripUvBoxEnabled is set, vertices are
     * stripped ONLY if their TextureCoord(0) UV falls inside the bbox
     * [u_min,v_min]..[u_max,v_max]. Combined with the bone filter as AND,
     * giving surgical "sword only, not hand" selection on shared-bone CRDs. */
    int                  stripUvBoxEnabled;
    float                stripUvBoxMin[2];                  /* [u_min, v_min] */
    float                stripUvBoxMax[2];                  /* [u_max, v_max] */

    /* Mesh Surgery panel. Pick any CRD/CH model, hit auto-find (spatial
     * outlier heuristic: hand-height verts extending past body Z), or
     * draw a UV box manually on the atlas. Strip and queue. Save bakes it.
     * NEW MEMBERS AT END per the struct-layout-end rule. */
    int                  meshSurgeryPanelOpen;
    int                  meshSurgerySourceOptionCount;
    char                 meshSurgerySourceOptions[512][96]; /* CH_/CRD_ models */
    int                  meshSurgerySelSourceIdx;
    char                 meshSurgerySelSourceName[96];
    float                meshSurgerySelectionUVRect[4];     /* u_min, v_min, u_max, v_max */
    int                  meshSurgerySelectedVertCount;      /* filled by host after auto-find */
    char                 meshSurgeryNewName[96];
    int                  meshSurgeryAutoFindRequested;      /* one-shot, host consumes */
    int                  meshSurgeryStripRequested;         /* one-shot, host consumes */
    char                 meshSurgeryStatusText[256];        /* feedback line for the UI */
    /* Atlas texture handle for the picked source. Host stashes the
     * IDirect3DTexture9* (or void* equivalent) here so the DLL can pass it
     * to ImGui::Image as the texture ID. NULL means no atlas available
     * (model has no diffuse texture or texture lookup failed). */
    void*                meshSurgeryAtlasTexture;
    int                  meshSurgeryAtlasW;
    int                  meshSurgeryAtlasH;
    char                 meshSurgeryAtlasTextureName[96];
    /* Preview UV bbox from auto-find. Drawn green on the atlas so the
     * user sees what the heuristic detected vs what they're about to
     * strip (red). Full triangle wireframe overlay is deferred,
     * requires pointer-based data marshalling we haven't built yet. */
    float                meshSurgeryAutoFindPreview[4];   /* u_min, v_min, u_max, v_max */
    int                  meshSurgeryAutoFindPreviewValid; /* 0 = no preview */

    /* Crowd Mesh Builder embed-vs-newslot toggle.
     * 0 = legacy. Weapon becomes a new mesh slot with its own mat and
     *     texture. The crowd render path silently drops the slot when
     *     flags don't line up the way the dead EA pipeline tools set
     *     them. Fucked us for hours.
     * 1 = embed. Weapon verts get pre-transformed and shoved straight
     *     into base.slot[0]. UVs map into a stolen corner of the base
     *     atlas. One slot, one mat, one texture, no surface for the
     *     crowd path to drop it on. */
    int                  crowdBuilderEmbedMode;
    /* Atlas region the embed targets. User pastes this from Mesh
     * Surgery's auto-find (typically the patch the stripped weapon
     * used to occupy). */
    float                crowdBuilderEmbedUVRect[4];

    /* ============================================================
     * EMBED UV PICKER, OR: STOP MAKING THE USER TYPE FLOATS BLIND
     * ============================================================
     * Host passes the base mesh's atlas DDS through as a raw D3D9
     * texture pointer so the panel can draw it inline and let the
     * user click-drag a rect on actual pixels instead of guessing
     * four floats with the wrong sign convention three times in a
     * row. Same mechanism Mesh Surgery already uses. NULL means
     * "no atlas yet, fall back to the slider widget" - usually
     * because the user hasn't picked a base mesh or the picked
     * mesh has no parts[0].diffuseTex (broken material on the
     * source asset, which happens on a depressing number of these
     * old CRD models EA never bothered to QA before the studio got
     * fed to the woodchipper).
     */
    void*                crowdBuilderBaseAtlasTexture;
    int                  crowdBuilderBaseAtlasW;
    int                  crowdBuilderBaseAtlasH;
    char                 crowdBuilderBaseAtlasTextureName[96];

    /* ============================================================
     * AUTO-SPAWN, OR: THE GODDAMN ANSWER TO "WHERE DID MY MESH GO"
     * ============================================================
     * Ticked, the bake also queues a 3dCrowd item placement so the
     * freshly merged mesh actually lands in the world. Untickedd,
     * the bake writes models/<name>.json and walks off whistling,
     * and the user reloads to an empty viewport ten minutes later
     * wondering if they fucked something up. They didn't. The
     * panel did. We fix that here.
     *
     * Host seeds crowdBuilderSpawnPos one-shot from the camera
     * focus point so the default places the mesh right where the
     * user was looking when they hit BAKE, not at world origin in
     * the middle of nowhere where they'll never find it.
     */
    int                  crowdBuilderAutoSpawn;
    float                crowdBuilderSpawnPos[3];
    int                  crowdBuilderSpawnCount;   /* default 3, host clamps */

    /* ============================================================
     * ADJUST BAKED WEAPON — Bow Fixer sub-panel
     * ============================================================
     * After Crowd Mesh Builder bakes a merged CRD, the weapon (bow,
     * sword, etc) is frozen in place inside slot[0]'s vertex stream.
     * Without this panel the only way to nudge it is re-bake from
     * stripped + slide the offset + re-pack + reload, a 5-minute
     * loop per attempt that turns "the bow is 8cm too low" into a
     * day of work.
     *
     * This panel reads ze_embedded_weapons.json (written by the bake
     * pipeline at the dump root), lists merged CRDs and their
     * weapons, and lets the user drag XYZ + YPR sliders. The engine
     * applies the delta in BONE-LOCAL space every frame by
     * transforming the weapon's verts on slot[0]'s VB. Apply
     * persists the delta to ze_embedded_weapon_xforms.json so the
     * next Save Level commits it to the on-disk model JSON.
     *
     * APPENDED AT END per the struct-layout-end commandment.
     */
    int                  bowAdjustOpen;                       /* sub-panel toggle */
    int                  bowAdjustModelOptionCount;
    char                 bowAdjustModelOptions[256][96];      /* models with embedded weapons */
    int                  bowAdjustSelModelIdx;
    char                 bowAdjustSelModelName[96];
    int                  bowAdjustWeaponOptionCount;
    /* Semantically holds bone names since M5 (the "Weapon" label is
     * back-compat baggage). Sized to fit a full character skeleton —
     * a typical CRD has 50-60 bones, occasional hero rig pushes 80.
     * Matches crowdBuilderBoneOptions which is the canonical bone-list
     * capacity in this file. */
    char                 bowAdjustWeaponOptions[128][64];     /* bone names on selected model */
    int                  bowAdjustSelWeaponIdx;
    float                bowAdjustDeltaXYZ[3];                /* metres, bone-local */
    float                bowAdjustDeltaYPR[3];                /* degrees, bone-local Yaw/Pitch/Roll */
    int                  bowAdjustResetRequested;             /* one-shot, host zeros sliders */
    int                  bowAdjustApplyRequested;             /* one-shot, host persists to sidecar */
    char                 bowAdjustStatusText[160];            /* feedback line for the UI */

    /* ============================================================
     * TRIANGLE PICKER — embedded model viewer + click-to-pick. Lives
     * INSIDE the Crowd Mesh Builder window as a collapsing section so
     * the user gets selection + move in the same tool. The host
     * renders the picked model to a dedicated RT each frame and stashes
     * the D3D9 texture pointer below; the panel draws it with
     * ImGui::Image and forwards click coordinates back.
     *
     * APPENDED AT END per the struct-layout-end commandment.
     */
    int                  triPickerOpen;                       /* section toggle in Crowd Mesh Builder */
    void*                triPickerViewerTex;                  /* IDirect3DTexture9* the panel renders */
    int                  triPickerViewerW;                    /* viewer image width in pixels */
    int                  triPickerViewerH;                    /* viewer image height in pixels */
    /* Camera orbit for the embedded viewer. Panel reads + writes; host
     * uses the current values to compute view+proj for the RT render. */
    float                triPickerOrbitYaw;                   /* radians */
    float                triPickerOrbitPitch;                 /* radians */
    float                triPickerOrbitDist;                  /* meters */
    /* One-shot click coordinates. Panel sets when the user clicks on
     * the viewer image; host ray-picks on consume and clears. xy are
     * in image-pixel coords with (0,0) = top-left. */
    int                  triPickerClickX;
    int                  triPickerClickY;
    int                  triPickerClickPending;               /* 1 when there's an unconsumed click */
    int                  triPickerShiftHeld;                  /* 1 = extend selection, 0 = replace */
    int                  triPickerClearSelection;             /* one-shot, host empties selection */
    int                  triPickerGrowConnected;              /* one-shot, host floods to connected tris */
    /* Slider deltas (world space — the embedded viewer is a bind-pose
     * render so what you see is what you commit). Reset zeros them. */
    float                triPickerDeltaXYZ[3];
    int                  triPickerResetSliders;
    int                  triPickerApplyRequested;             /* one-shot, persists xforms sidecar */
    int                  triPickerSelectedTriCount;           /* host reports back for status */
    int                  triPickerSelectedVertCount;          /* host reports back for status */
    char                 triPickerStatusText[160];

    /* ─── PATH PLACE (Crowd Generator sub-tool) ────────────────────────
     * DLL→host. User draws a polyline in the viewport (FABRIK handles
     * via SplineEditor), tweaks spacing/rows/facing/jitter, clicks
     * Stamp. Host samples the polyline at arc-length and spawns N
     * CrowdVals on the selected archetype. The radial-scatter pipeline
     * (crowdGenerateRequested + pendingCrowdGen*) stays untouched;
     * this is a separate request. Appended at the end of the struct
     * per feedback_struct_layout.md (no inserts mid-struct or .obj
     * layout breaks and the engine crashes on load). */
    int                  pathPlaceRequested;       /* one-shot trigger */
    int                  pathPlaceArchetype;       /* index into crowdArchetypeNames */
    int                  pathPlaceNodeCount;       /* number of XYZ triples below */
    float                pathPlaceNodes[64*3];     /* flat XYZ array, max 64 nodes */
    float                pathPlaceSpacing;         /* meters between instances along arc */
    int                  pathPlaceRows;            /* formation depth (1 = single file) */
    float                pathPlaceRowGap;          /* meters between rows */
    int                  pathPlaceFacingMode;      /* 0=tangent 1=perp 2=look-at 3=fixed 4=random */
    float                pathPlaceFixedYaw;        /* radians, facing=fixed */
    float                pathPlaceLookAt[3];       /* world point, facing=look-at */
    float                pathPlacePosJitter;       /* meters, +/- per-instance */
    float                pathPlaceYawJitter;       /* radians, +/- per-instance */
    int                  pathPlaceSnapGround;      /* 1 = raycast Y onto terrain */
    unsigned int         pathPlaceRngSeed;         /* deterministic jitter */

    /* PATH PLACE LIVE PREVIEW: DLL→host every frame the path-place
     * tool is active. Host renders ghost markers at each sampled
     * position without spawning real crowd instances. The preview
     * uses the SAME sampling math as the Stamp commit, so what the
     * user sees is what they get. */
    int                  pathPlacePreviewActive;   /* 1 = render markers */
    int                  pathPlacePreviewCount;    /* host reports back: how many would spawn */

    /* ================================================================
     * Model viewer LOD + render-mode debug (MUST BE AT END).
     *
     * DLL writes these every frame; EXE reads them after DrawFrame
     * returns and feeds them into Scene3DRenderer::setLodVisible /
     * setModelRenderMode / setModelShaderMode. Slots 0..4 are visible
     * LODs (LOD0 = highest detail), slot 5 is the collision-proxy
     * bucket. Default: LOD0 on, everything else off — the single thing
     * that kills the "extra gray low-poly layer over the real mesh"
     * symptom on freshly loaded game models.
     * ================================================================ */
    int                  modelViewerLodVisible[6]; /* bool per slot: 0..4=LOD, 5=collision proxy */
    int                  modelViewerRenderMode;    /* 0=Textured 1=Untextured 2=Wireframe 3=FlatColor */
    int                  modelViewerShaderMode;    /* 0=Lit 1=Unlit 2=NormalsAsColor 3=UVAsColor */

    /* ================================================================
     * Render pass kill switches (MUST BE AT END).
     *
     * Each flag gates one pass in Scene3DRenderer::stepAndRender. The
     * primary use case is hunting "extra layer over the character
     * mesh" bugs: toggle each pass off one at a time until the ghost
     * geometry disappears, then you've found the offender. The host
     * applies these via the matching Scene3DRenderer::setPassEnable*
     * setters every frame.
     * ================================================================ */
    int                  passEnableSkeleton;     /* renderSkeletonFromPose */
    int                  passEnableMesh;         /* renderMesh (GameModel) */
    int                  passEnablePlayerChar;   /* renderPlayerCharacter (physics) */
    int                  passEnableDisplayWorld; /* m_displayWorld->render (Havok auto) */
    int                  passEnableFx;           /* m_effectManager->render */

    /* GameModel shader path. Host's Scene3DRenderer reads this to
     * decide which render pipeline draws the loaded character.
     *   0 = Havok Fixed-Function (renderMesh, current behavior)
     *   1 = Havok DisplayWorld   (hkgDisplayObject auto-render)
     *   2 = Game Shaders         (GameShaderCache compiled D3D9 PS)
     *   3 = Unlit Debug          (flat white, no lighting/textures)
     * gameModelShaderPSName names the specific PS for path 2; empty
     * string means "use the cache's default for this material". The
     * full list of available names is published back to the DLL via
     * gameShaderNames below so the dropdown can show real options. */
    int                  gameModelShaderPath;
    char                 gameModelShaderPSName[96];

    /* Host -> DLL: list of PS shader names available in the loaded
     * GameShaderCache. Filled every frame the cache is loaded; empty
     * when no level/cache is active. The DLL renders a dropdown from
     * these strings — pick one and the host binds it. */
    const char* const*   gameShaderNames;        /* array of cstrs */
    int                  gameShaderNameCount;    /* length of gameShaderNames */

    /* ================================================================
     * Diagnostic instrumentation (MUST BE AT END).
     *
     * The user can't see what's drawing the silhouette overlay, so we
     * give them ground-truth: per-pass tint, per-frame stats, per-part
     * inspector, and force-single-pass debug.
     * ================================================================ */

    /* DLL -> host */
    int  renderDebugTintPasses;   /* 1 = each pass paints its own color */
    int  renderDebugForcePass;    /* 0=both, 1=opaque only, 2=transparent only */
    int  renderDebugDumpRequested;/* 1 = write render_debug_dump.log this frame */
    int  renderDebugPartVisible[64]; /* per-meshPart hide toggle (1=visible) */

    /* host -> DLL (read-only — stats and meshPart info to display) */
    int  passStatsDrawCalls[6];   /* indexed by STAT_MESH..STAT_FX */
    int  passStatsTriangles[6];
    int  passStatsMeshParts[6];
    int  displayWorldObjectCount; /* getNumDisplayObjects() snapshot */

    /* host -> DLL: loaded mesh-part summary table. Filled every frame
     * a GameModel is loaded; gameModelPartCount==0 when nothing's
     * loaded. The DLL inspector lists each part with material/texture
     * info so the user can hide them one at a time. */
    int  gameModelPartCount;
    const char* const* gameModelPartMatNames;    /* array, length=partCount */
    const char* const* gameModelPartDiffNames;
    const int*         gameModelPartVertexCounts;
    const int*         gameModelPartIndexCounts;
    const int*         gameModelPartLodLevels;
    const int*         gameModelPartHasTexture;  /* 1 if diffuseTexture != NULL */
} ImGuiGlueFrameArgs;

typedef bool (IMGUI_GLUE_CALL *ImGuiGlue_Init_t)(HWND mainHwnd, IDirect3DDevice9* device, const char* iniFilename);
typedef void (IMGUI_GLUE_CALL *ImGuiGlue_Shutdown_t)();
typedef bool (IMGUI_GLUE_CALL *ImGuiGlue_WndProc_t)(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
typedef void (IMGUI_GLUE_CALL *ImGuiGlue_NewFrame_t)();
typedef void (IMGUI_GLUE_CALL *ImGuiGlue_DrawFrame_t)(const ImGuiGlueFrameArgs* args);
typedef void (IMGUI_GLUE_CALL *ImGuiGlue_Render_t)();
typedef void (IMGUI_GLUE_CALL *ImGuiGlue_InvalidateDeviceObjects_t)();
typedef void (IMGUI_GLUE_CALL *ImGuiGlue_CreateDeviceObjects_t)();

IMGUI_GLUE_API bool IMGUI_GLUE_CALL ImGuiGlue_Init(HWND mainHwnd, IDirect3DDevice9* device, const char* iniFilename);
IMGUI_GLUE_API void IMGUI_GLUE_CALL ImGuiGlue_Shutdown();
IMGUI_GLUE_API bool IMGUI_GLUE_CALL ImGuiGlue_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
IMGUI_GLUE_API void IMGUI_GLUE_CALL ImGuiGlue_NewFrame();
IMGUI_GLUE_API void IMGUI_GLUE_CALL ImGuiGlue_DrawFrame(const ImGuiGlueFrameArgs* args);
IMGUI_GLUE_API void IMGUI_GLUE_CALL ImGuiGlue_Render();
IMGUI_GLUE_API void IMGUI_GLUE_CALL ImGuiGlue_InvalidateDeviceObjects();
IMGUI_GLUE_API void IMGUI_GLUE_CALL ImGuiGlue_CreateDeviceObjects();

#ifdef __cplusplus
}
#endif
