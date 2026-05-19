// AnimationGraphRuntimeTypes.h — The DNA of Pandemic's State Machine
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Runtime type definitions for the animation graph state machine — states,
// transitions, conditions, parameters, blend trees, sub-machines, actions.
// This is the data model behind Pandemic's Lua-driven animation system.
//
// Every struct here mirrors something we found in the .exe's RTTI data
// or inferred from Lua script string references: "GraphCondition",
// "GraphTransition", "GraphState", "BlendNode", "SubMachine"...
// Pandemic built a full visual state machine editor that exported to
// Lua. The Lua scripts drove these C++ structs at runtime. We rebuilt
// the C++ side from the binary. The Lua side is partially reconstructed.
//
// "In the middle of difficulty lies opportunity." — Einstein
// The difficulty: reverse-engineering a state machine with 12 condition
// operators, nested blend graphs, sub-machine recursion, and trigger
// parameters. The opportunity: understanding how a real AAA studio
// structured their animation pipeline. Worth every sleepless night.
// -----------------------------------------------------------------------
#ifndef ANIMATION_GRAPH_RUNTIME_TYPES_H
#define ANIMATION_GRAPH_RUNTIME_TYPES_H

#include <string>
#include <vector>
#include <Common/Base/Container/Array/hkArray.h>
#include <Common/Base/Math/QsTransform/hkQsTransform.h>
#include "AnimationCurve.h"

struct JsonAnimClip;

enum GraphConditionOp
{
    GRAPH_COND_EQUAL = 0,
    GRAPH_COND_NOT_EQUAL,
    GRAPH_COND_GREATER,
    GRAPH_COND_LESS,
    GRAPH_COND_GREATER_EQUAL,
    GRAPH_COND_LESS_EQUAL,
    GRAPH_COND_TRUE,
    GRAPH_COND_FALSE,
    GRAPH_COND_IN_RANGE,
    GRAPH_COND_NOT_IN_RANGE,
    GRAPH_COND_TEST_BITS_TRUE,
    GRAPH_COND_TEST_BITS_FALSE
};

struct GraphParam
{
    std::string name;
    int type; // Scene3DRenderer::GRAPH_PARAM_* (0 = float)
    float f;
    int i;
    bool b;
    bool triggered;
    bool hasRange;
    float minValue;
    float maxValue;

    GraphParam()
        : type(0),
          f(0.0f),
          i(0),
          b(false),
          triggered(false),
          hasRange(false),
          minValue(0.0f),
          maxValue(1.0f)
    {}
};

struct GraphCondition
{
    int paramIndex;
    GraphConditionOp op;
    float f;
    int i;
    bool b;

    GraphCondition()
        : paramIndex(-1),
          op(GRAPH_COND_EQUAL),
          f(0.0f),
          i(0),
          b(false)
    {}
};

enum GraphActionArgType
{
    GRAPH_ACT_ARG_FLOAT = 0,
    GRAPH_ACT_ARG_INT,
    GRAPH_ACT_ARG_BOOL,
    GRAPH_ACT_ARG_STRING
};

struct GraphActionArg
{
    int type;
    float f;
    int i;
    bool b;
    std::string s;

    GraphActionArg()
        : type(GRAPH_ACT_ARG_FLOAT),
          f(0.0f),
          i(0),
          b(false)
    {}
};

struct GraphAction
{
    std::string name;
    std::vector<GraphActionArg> args;
};

struct GraphStateEvent
{
    int type; // ON_ENTER/ON_EXIT/ON_TIME/ON_EVENT (GL_Globals.lua)
    std::string onEvent;
    std::string postEvent;
    float time;
    std::vector<GraphActionArg> postArgs;

    GraphStateEvent()
        : type(0),
          time(0.0f)
    {}
};

struct GraphStateEventAction
{
    std::string eventName;
    std::vector<GraphAction> actions;
};

struct GraphPickAnim
{
    std::string clipPath;
    JsonAnimClip* clip;
    bool loop;
    float speed;
    float duration;
    float frameTime;
    std::string doneEvent;

    GraphPickAnim()
        : clip(NULL),
          loop(false),
          speed(1.0f),
          duration(0.0f),
          frameTime(1.0f / 30.0f)
    {}
};

struct GraphPickCandidate
{
    int pickIndex;
    std::vector< std::vector<GraphCondition> > conditionSets;

    GraphPickCandidate()
        : pickIndex(-1)
    {}
};

struct GraphState
{
    std::string name;
    std::string clipPath;
    JsonAnimClip* clip;
    std::string blendGraphName;
    struct BlendGraphRuntime* blendGraph;
    std::string stateMachineName;
    struct AnimationGraphRuntime* subMachine;
    bool loop;
    float speed;
    float duration;
    float frameTime;
    int flags;
    std::string doneEvent;
    std::vector<GraphPickAnim> pickAnims;
    std::vector<GraphPickCandidate> pickCandidates;
    int pickDefault;
    int pickSelected;
    std::vector<GraphStateEvent> events;
    std::vector<GraphAction> onEnterActions;
    std::vector<GraphAction> onExitActions;
    std::vector<GraphStateEventAction> onEventActions;
    hkArray<hkQsTransform> additiveRefLocal;
    bool additiveRefValid;

    GraphState()
        : clip(NULL),
          blendGraph(NULL),
          subMachine(NULL),
          loop(true),
          speed(1.0f),
          duration(0.0f),
          frameTime(1.0f / 30.0f),
          flags(0),
          pickDefault(0),
          pickSelected(-1),
          additiveRefValid(false)
    {}

    GraphState(const GraphState& other)
        : name(other.name),
          clipPath(other.clipPath),
          clip(other.clip),
          blendGraphName(other.blendGraphName),
          blendGraph(other.blendGraph),
          stateMachineName(other.stateMachineName),
          subMachine(other.subMachine),
          loop(other.loop),
          speed(other.speed),
          duration(other.duration),
          frameTime(other.frameTime),
          flags(other.flags),
          doneEvent(other.doneEvent),
          pickAnims(other.pickAnims),
          pickCandidates(other.pickCandidates),
          pickDefault(other.pickDefault),
          pickSelected(other.pickSelected),
          events(other.events),
          onEnterActions(other.onEnterActions),
          onExitActions(other.onExitActions),
          onEventActions(other.onEventActions),
          additiveRefValid(other.additiveRefValid)
    {
        const int count = other.additiveRefLocal.getSize();
        additiveRefLocal.setSize(count);
        for (int i = 0; i < count; ++i)
        {
            additiveRefLocal[i] = other.additiveRefLocal[i];
        }
    }

    GraphState& operator=(const GraphState& other)
    {
        if (this != &other)
        {
            name = other.name;
            clipPath = other.clipPath;
            clip = other.clip;
            blendGraphName = other.blendGraphName;
            blendGraph = other.blendGraph;
            stateMachineName = other.stateMachineName;
            subMachine = other.subMachine;
            loop = other.loop;
            speed = other.speed;
            duration = other.duration;
            frameTime = other.frameTime;
            flags = other.flags;
            doneEvent = other.doneEvent;
            pickAnims = other.pickAnims;
            pickCandidates = other.pickCandidates;
            pickDefault = other.pickDefault;
            pickSelected = other.pickSelected;
            events = other.events;
            onEnterActions = other.onEnterActions;
            onExitActions = other.onExitActions;
            onEventActions = other.onEventActions;
            additiveRefValid = other.additiveRefValid;

            const int count = other.additiveRefLocal.getSize();
            additiveRefLocal.setSize(count);
            for (int i = 0; i < count; ++i)
            {
                additiveRefLocal[i] = other.additiveRefLocal[i];
            }
        }
        return *this;
    }
};

struct GraphTransition
{
    int fromState;
    int toState;
    bool anyState;
    float duration;
    float exitTime;
    int easingType;
    float cp1x, cp1y, cp2x, cp2y;
    int priority;
    int blendMode; // Scene3DRenderer::JSON_BLEND_*
    int rotMode;   // Scene3DRenderer::JSON_BLEND_ROT_*
    int layerRoot;
    bool allowInterrupt;
    bool syncTime;
    int syncHint;   // 0=none, 1=progress, 2=one-minus-progress (GL_Globals SYNC_HINT_*)
    float maxTime;  // normalized max-time window (0..1), <0 disables
    std::vector<float> mask;
    std::vector<GraphCondition> conditions;

    GraphTransition()
        : fromState(-1),
          toState(-1),
          anyState(false),
          duration(0.2f),
          exitTime(-1.0f),
          easingType(EASING_LINEAR),
          cp1x(0.0f), cp1y(0.0f), cp2x(1.0f), cp2y(1.0f),
          priority(0),
          blendMode(0),
          rotMode(0),
          layerRoot(-1),
          allowInterrupt(true),
          syncTime(false),
          syncHint(0),
          maxTime(-1.0f)
    {}
};

struct GraphMask
{
    std::string name;
    std::vector<float> weights;
};

struct GraphWeight
{
    bool isConst;
    float constant;
    std::string name;
    float args[4];
    int argCount;

    GraphWeight()
        : isConst(true),
          constant(0.0f),
          argCount(0)
    {
        args[0] = args[1] = args[2] = args[3] = 0.0f;
    }
};

struct BlendGraphTrack
{
    int inputNode;
    std::string inputName;
    int eventMode;
    GraphWeight weight;
    std::vector<float> mask;

    BlendGraphTrack()
        : inputNode(-1),
          eventMode(0)
    {}
};

enum BlendGraphNodeType
{
    BLEND_NODE_SAMPLER = 0,
    BLEND_NODE_BLENDER,
    BLEND_NODE_STATEMACHINE,
    BLEND_NODE_SUBGRAPH,
    BLEND_NODE_REFERENCE
};

struct BlendGraphNode
{
    std::string name;
    int type;

    std::string clipPath;
    std::string refName;
    std::string subgraphName;
    std::string stateMachineName;
    JsonAnimClip* clip;
    bool loop;
    float rate;
    bool syncToVelocity;
    float rootSpeed;
    int progressParamIndex;
    bool useProgressParam;

    bool normalizeWeights;
    bool syncTracks;
    bool blenderSyncToVelocity;
    float weightDamp;
    float syncedRate;
    float blenderTime;
    std::vector<float> smoothedWeights;
    std::vector<float> tmpWeights;
    std::vector<BlendGraphTrack> tracks;

    struct AnimationGraphRuntime* stateMachine;
    struct BlendGraphRuntime* subgraph;
    int referenceNode;

    BlendGraphNode()
        : type(BLEND_NODE_SAMPLER),
          clip(NULL),
          loop(true),
          rate(1.0f),
          syncToVelocity(false),
          rootSpeed(0.0f),
          progressParamIndex(-1),
          useProgressParam(false),
          normalizeWeights(false),
          syncTracks(false),
          blenderSyncToVelocity(false),
          weightDamp(0.0f),
          syncedRate(1.0f),
          blenderTime(0.0f),
          stateMachine(NULL),
          subgraph(NULL),
          referenceNode(-1)
    {}
};

struct BlendGraphRuntime
{
    std::string name;
    std::string resultNodeName;
    int resultNode;
    float duration;
    std::vector<BlendGraphNode> nodes;

    BlendGraphRuntime()
        : resultNode(-1),
          duration(0.0f)
    {}
};

struct AnimationGraphRuntime
{
    bool valid;
    bool enabled;
    std::string name;
    std::vector<GraphParam> params;
    std::vector<GraphMask> masks;
    std::vector<BlendGraphRuntime*> blendGraphs;
    std::vector<AnimationGraphRuntime*> subMachines;
    AnimationGraphRuntime* owner;
    AnimationGraphRuntime* parent;
    std::vector<GraphState> states;
    std::vector<GraphTransition> transitions;

    int defaultState;
    int currentState;
    int nextState;
    int activeTransition;
    bool inTransition;

    float stateTime;
    float nextTime;
    float transitionTime;
    float transitionDuration;
    float lastEventTime;
    float lastNextEventTime;
    float lastDeltaTime;

    AnimationGraphRuntime()
        : valid(false),
          enabled(true),
          owner(NULL),
          parent(NULL),
          defaultState(-1),
          currentState(-1),
          nextState(-1),
          activeTransition(-1),
          inTransition(false),
          stateTime(0.0f),
          nextTime(0.0f),
          transitionTime(0.0f),
          transitionDuration(0.0f),
          lastEventTime(0.0f),
          lastNextEventTime(0.0f),
          lastDeltaTime(0.0f)
    {}
};

#endif
