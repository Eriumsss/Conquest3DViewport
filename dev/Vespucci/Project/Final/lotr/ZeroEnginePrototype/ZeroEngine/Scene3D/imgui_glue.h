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
    int                  propEditKind;              /* 0=int, 1=float, 2=guid, 3=vec3, 4=matrix */
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
