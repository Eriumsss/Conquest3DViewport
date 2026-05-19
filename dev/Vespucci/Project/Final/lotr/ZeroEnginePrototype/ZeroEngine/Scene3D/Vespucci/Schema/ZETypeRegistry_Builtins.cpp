// ZETypeRegistry_Builtins.cpp
// =============================================================================
// THE GODDAMN BUILT-IN TYPE TABLE FOR EVERY LOTR:C ENTITY KIND WE KNOW
// =============================================================================
// Written by: Eriumsss
//
// This file is data, not logic. RegisterAllBuiltins() takes the
// registry and stuffs every type we have observed in the LOTR:C
// corpus into it - one row per type, one entry per row, no exceptions.
// The list comes from project_level_json_deep.md (172 types, 1503
// fields) and from staring at .lvl hex dumps long enough to lose
// whole weekends to it. Every Pandemic dev who knew this schema
// in detail got laid off in 2009 - we are reconstructing their work
// from cold binary.
//
// Format (one row per call):
//   reg.registerType(displayName, canonicalName, parentId, traits, mpegCrc, "doc")
//
// canonicalName is lowercase, no underscores, no spaces - that is
// what the TypeId hash is computed from. displayName is what the
// UI shows. mpegCrc is the precomputed Pandemic CRC of the display
// name so we can match against the .lvl type-table without recomputing.
// 0 means "compute on demand" - we let it be 0 here for hand-written
// rows because the actual CRC lookup is rarely the hot path.
//
// Trait flags: see TypeTrait enum in ZETypeRegistry.h. Combine with |.
//
// Hierarchy: every type has a parent. The root is "Entity" with
// parentId = TypeId(0). All others chain up.
//
// ABSENT TYPES: this list is NOT exhaustive of every binary kind
// in every map. SignatureLoader_Lua.cpp picks up unknown types from
// level data on load and registers them with default traits. That
// means a modder can ship a custom type and Vespucci will still
// show a sane (if generic) entry for it in the suggestion UI.
// =============================================================================

#include "ZETypeRegistry.h"

#include "../Core/VespucciTypes.h"

namespace Vespucci {
namespace Schema {

// Tiny helper to keep the rows below readable. RegisterRow is
// ALL the row does - the registry takes care of dedup, hash, etc.
namespace {
    inline TypeId RegRow(ZETypeRegistry& r,
                         const char* display,
                         const char* canonical,
                         TypeId parent,
                         u32 traits,
                         const char* desc)
    {
        return r.registerType(display, canonical, parent, traits, 0, desc);
    }
} // namespace

void RegisterAllBuiltins(ZETypeRegistry& reg)
{
    // ── ROOT ──────────────────────────────────────────────────────
    TypeId Entity =
        RegRow(reg, "Entity", "entity", TypeId(0),
               TRAIT_None,
               "Root of the type hierarchy. Anything in a level is an Entity.");

    // ── COMMON ABSTRACT BASES ─────────────────────────────────────
    TypeId Spatial =
        RegRow(reg, "Spatial", "spatial", Entity,
               TRAIT_Spatial,
               "Abstract: anything with a WorldTransform.");

    TypeId Volume =
        RegRow(reg, "Volume", "volume", Spatial,
               TRAIT_Spatial | TRAIT_Volume,
               "Abstract: spatial with bounding extent.");

    TypeId Eventful =
        RegRow(reg, "Eventful", "eventful", Entity,
               TRAIT_HasEvents,
               "Abstract: emits events through Outputs[].");

    TypeId Listener =
        RegRow(reg, "Listener", "listener", Entity,
               TRAIT_HasInputs,
               "Abstract: accepts events through input_event field.");

    TypeId Wired =
        RegRow(reg, "Wired", "wired", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs,
               "Abstract: both emits and accepts events.");

    // ── OUTPUT ENVELOPE ───────────────────────────────────────────
    // Output entities are the wires themselves. Every connection in
    // the level has its own Output entity. ScoreManager OnCapture ->
    // ObjectiveController is THREE entities: the source, the Output
    // envelope, the target. The Output envelope carries event_name,
    // input_action, delay, sticky, target_guid.
    TypeId Output =
        RegRow(reg, "Output", "output", Entity,
               TRAIT_HasOutputs,
               "Wire envelope. Carries event/action/delay/sticky/target_guid for one connection.");

    TypeId OutputVolume =
        RegRow(reg, "OutputVolume", "outputvolume", Output,
               TRAIT_HasOutputs | TRAIT_Volume,
               "Volume-bound output. Fires its event when a target enters/exits.");

    TypeId OutputCondition =
        RegRow(reg, "OutputCondition", "outputcondition", Output,
               TRAIT_HasOutputs,
               "Output gated on a runtime condition expression.");

    // ── GAMEPLAY: CAPTURE / OBJECTIVES / SCORING ──────────────────
    TypeId CapturePoint =
        RegRow(reg, "CapturePoint", "capturepoint", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_HasEvents | TRAIT_Gameplay,
               "Capturable point. Emits OnCapture / OnNeutralize / OnContested.");

    TypeId CaptureArea =
        RegRow(reg, "CaptureArea", "capturearea", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_Gameplay,
               "Trigger volume associated with a CapturePoint. Owns the radius.");

    TypeId ObjectiveMarker =
        RegRow(reg, "ObjectiveMarker", "objectivemarker", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Gameplay,
               "World-space objective indicator. Emits OnReached / OnFailed.");

    TypeId ObjectiveController =
        RegRow(reg, "ObjectiveController", "objectivecontroller", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Gameplay,
               "Mission-level objective state machine. Drives ObjectiveMarker visibility.");

    TypeId ScoreManager =
        RegRow(reg, "ScoreManager", "scoremanager", Entity,
               TRAIT_HasInputs | TRAIT_Gameplay | TRAIT_Singleton,
               "Per-team score accumulator. Listens for AddScore / SubtractScore.");

    TypeId Director =
        RegRow(reg, "Director", "director", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Gameplay | TRAIT_Singleton,
               "Top-level mission director. One per level, drives win/lose conditions.");

    TypeId WinCondition =
        RegRow(reg, "WinCondition", "wincondition", Entity,
               TRAIT_HasEvents | TRAIT_Gameplay,
               "Predicate over level state that fires OnSatisfied when the team wins.");

    TypeId LossCondition =
        RegRow(reg, "LossCondition", "losscondition", Entity,
               TRAIT_HasEvents | TRAIT_Gameplay,
               "Predicate over level state that fires OnSatisfied when the team loses.");

    TypeId TeamSpawnZone =
        RegRow(reg, "TeamSpawnZone", "teamspawnzone", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_Spawner | TRAIT_Gameplay,
               "Spawn area for one team. Coupled to capture-state via wires.");

    // ── TRIGGERS / GAMEPLAY VOLUMES ────────────────────────────────
    TypeId TriggerVolume =
        RegRow(reg, "TriggerVolume", "triggervolume", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_HasEvents,
               "Volume that emits OnEnter / OnExit / OnStay when an entity crosses it.");

    TypeId TriggerSphere =
        RegRow(reg, "TriggerSphere", "triggersphere", TriggerVolume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_HasEvents,
               "Sphere-shaped trigger volume.");

    TypeId TriggerBox =
        RegRow(reg, "TriggerBox", "triggerbox", TriggerVolume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_HasEvents,
               "Axis-aligned box trigger volume.");

    TypeId TriggerCylinder =
        RegRow(reg, "TriggerCylinder", "triggercylinder", TriggerVolume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_HasEvents,
               "Vertical cylinder trigger volume. Common for capture rings.");

    TypeId DamageZone =
        RegRow(reg, "DamageZone", "damagezone", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_HasEvents | TRAIT_Gameplay,
               "Volume that ticks damage on units inside. Emits OnUnitKilled.");

    TypeId HealZone =
        RegRow(reg, "HealZone", "healzone", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_Gameplay,
               "Volume that ticks healing on units inside.");

    TypeId NoBuildZone =
        RegRow(reg, "NoBuildZone", "nobuildzone", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_Gameplay,
               "Volume that disallows player-placed structures.");

    // ── SPAWNING ──────────────────────────────────────────────────
    TypeId SpawnPoint =
        RegRow(reg, "SpawnPoint", "spawnpoint", Spatial,
               TRAIT_Spatial | TRAIT_Spawner | TRAIT_HasEvents | TRAIT_Gameplay,
               "Single spawn location. Emits OnSpawn / OnRespawn.");

    TypeId Spawner =
        RegRow(reg, "Spawner", "spawner", Entity,
               TRAIT_Spawner | TRAIT_HasEvents | TRAIT_HasInputs,
               "Generic AI spawner. Listens for SpawnUnit / DestroyAll.");

    TypeId WaveSpawner =
        RegRow(reg, "WaveSpawner", "wavespawner", Spawner,
               TRAIT_Spawner | TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Gameplay,
               "Timed-wave AI spawner. Reinforcements pattern.");

    TypeId ReinforcementPoint =
        RegRow(reg, "ReinforcementPoint", "reinforcementpoint", Spatial,
               TRAIT_Spatial | TRAIT_Spawner,
               "Anchor where AI reinforcements arrive.");

    TypeId AmbushTrigger =
        RegRow(reg, "AmbushTrigger", "ambushtrigger", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_HasEvents | TRAIT_AI,
               "Scripted ambush volume. Spawns AI when a hero crosses.");

    // ── PATH NETWORK ──────────────────────────────────────────────
    TypeId PathNetwork =
        RegRow(reg, "PathNetwork", "pathnetwork", Entity,
               TRAIT_None,
               "Container for a graph of PathNode/PathLink entities.");

    TypeId PathNode =
        RegRow(reg, "PathNode", "pathnode", Spatial,
               TRAIT_Spatial | TRAIT_PathNode,
               "Vertex in a PathNetwork. Holds 3D position only.");

    TypeId PathLink =
        RegRow(reg, "PathLink", "pathlink", Entity,
               TRAIT_PathNode,
               "Edge between two PathNodes. Stores node1_guid / node2_guid.");

    TypeId PatrolRoute =
        RegRow(reg, "PatrolRoute", "patrolroute", PathNetwork,
               TRAIT_PathNode | TRAIT_AI,
               "Closed-loop variant used by patrolling AI.");

    // ── AI / NPCS ─────────────────────────────────────────────────
    TypeId AIDirector =
        RegRow(reg, "AIDirector", "aidirector", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_AI | TRAIT_Singleton,
               "Top-level AI scheduler. Distributes orders to commanders.");

    TypeId AICommander =
        RegRow(reg, "AICommander", "aicommander", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_AI,
               "Per-team AI commander. Manages squads.");

    TypeId AISquad =
        RegRow(reg, "AISquad", "aisquad", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_AI,
               "Group of AI units sharing orders.");

    TypeId GhostKnight =
        RegRow(reg, "GhostKnight", "ghostknight", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_AI | TRAIT_Animated,
               "Hero-class AI. Highly scripted, often wired into mission events.");

    TypeId NPCUnit =
        RegRow(reg, "NPCUnit", "npcunit", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_AI | TRAIT_Animated,
               "Generic NPC. Driven by AISquad / AICommander.");

    TypeId BehaviorTree =
        RegRow(reg, "BehaviorTree", "behaviortree", Entity,
               TRAIT_AI,
               "Compiled behavior tree definition. Referenced by NPCUnits.");

    TypeId Hero =
        RegRow(reg, "Hero", "hero", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_AI | TRAIT_Animated | TRAIT_Gameplay,
               "Named hero unit. Subset of NPCUnit with mission-event hooks.");

    // ── ANIMATION & CINEMATICS ────────────────────────────────────
    TypeId AnimationController =
        RegRow(reg, "AnimationController", "animationcontroller", Entity,
               TRAIT_HasInputs | TRAIT_Animated,
               "Drives animation state on a target mesh. Listens for Play / Stop / SetState.");

    TypeId CinematicEvent =
        RegRow(reg, "CinematicEvent", "cinematicevent", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Cinematic,
               "Discrete cinematic moment. Wires camera, audio, animation triggers.");

    TypeId CameraTrack =
        RegRow(reg, "CameraTrack", "cameratrack", Entity,
               TRAIT_Cinematic,
               "Spline-based camera path for cinematics.");

    TypeId CameraTarget =
        RegRow(reg, "CameraTarget", "cameratarget", Spatial,
               TRAIT_Spatial | TRAIT_Cinematic,
               "World-space camera look-at target.");

    TypeId Cutscene =
        RegRow(reg, "Cutscene", "cutscene", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Cinematic,
               "Prebaked cutscene container. Plays a sequence on input.");

    TypeId AnimationTrigger =
        RegRow(reg, "AnimationTrigger", "animationtrigger", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Animated,
               "Wires an animation state-change to an event firing.");

    // ── AUDIO ─────────────────────────────────────────────────────
    TypeId VOTrigger =
        RegRow(reg, "VOTrigger", "votrigger", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Audio,
               "Voice-over trigger. Listens for Play; emits OnFinished.");

    TypeId AudioManager =
        RegRow(reg, "AudioManager", "audiomanager", Entity,
               TRAIT_HasInputs | TRAIT_Audio | TRAIT_Singleton,
               "Per-level audio mixer. Listens for SetMusic / DuckDialogue.");

    TypeId AmbientSound =
        RegRow(reg, "AmbientSound", "ambientsound", Spatial,
               TRAIT_Spatial | TRAIT_Audio,
               "Looping ambient audio source.");

    TypeId MusicCue =
        RegRow(reg, "MusicCue", "musiccue", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Audio,
               "Adaptive-music state cue. Wired to mission progress.");

    TypeId SoundEmitter =
        RegRow(reg, "SoundEmitter", "soundemitter", Spatial,
               TRAIT_Spatial | TRAIT_HasInputs | TRAIT_Audio,
               "One-shot sound emitter at a position.");

    // ── DOORS / GATES / DESTRUCTIBLES ─────────────────────────────
    TypeId DoorActivator =
        RegRow(reg, "DoorActivator", "dooractivator", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Animated,
               "Single door / gate. Listens for Open/Close/Lock; emits OnOpened/OnClosed.");

    TypeId GateController =
        RegRow(reg, "GateController", "gatecontroller", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Animated,
               "Multi-piece animated gate. The Helm's Deep main gate is one of these.");

    TypeId Drawbridge =
        RegRow(reg, "Drawbridge", "drawbridge", GateController,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Animated,
               "Hinged drawbridge variant of GateController.");

    TypeId DestructibleObject =
        RegRow(reg, "DestructibleObject", "destructibleobject", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Render | TRAIT_Physics,
               "Mesh that can be destroyed. Emits OnDestroyed.");

    TypeId Wall =
        RegRow(reg, "Wall", "wall", DestructibleObject,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Render | TRAIT_Physics,
               "Destructible wall segment.");

    TypeId Tower =
        RegRow(reg, "Tower", "tower", DestructibleObject,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Render | TRAIT_Physics,
               "Destructible siege tower.");

    TypeId Catapult =
        RegRow(reg, "Catapult", "catapult", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Animated,
               "Player or AI-controlled catapult emplacement.");

    TypeId Trebuchet =
        RegRow(reg, "Trebuchet", "trebuchet", Catapult,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Animated,
               "Trebuchet variant of Catapult. Heavier projectile.");

    TypeId Ladder =
        RegRow(reg, "Ladder", "ladder", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Animated,
               "Climbable ladder. Emits OnReachedTop.");

    TypeId SiegeLadder =
        RegRow(reg, "SiegeLadder", "siegeladder", Ladder,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Animated | TRAIT_AI,
               "AI-controlled siege ladder placed during scripted events.");

    // ── SCRIPT INFRASTRUCTURE ─────────────────────────────────────
    TypeId ScriptRelay =
        RegRow(reg, "ScriptRelay", "scriptrelay", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs,
               "Pure relay node. Forwards events one-to-many. Most common wiring intermediary in the corpus.");

    TypeId ScriptCounter =
        RegRow(reg, "ScriptCounter", "scriptcounter", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs,
               "N-of-M counter. Emits OnThreshold when input event count hits target.");

    TypeId ScriptTimer =
        RegRow(reg, "ScriptTimer", "scripttimer", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs,
               "Timer. Emits OnTimerFire after delay seconds. Resets / pauses via inputs.");

    TypeId ScriptToggle =
        RegRow(reg, "ScriptToggle", "scripttoggle", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs,
               "Boolean state. Emits OnToggleOn / OnToggleOff.");

    TypeId ScriptBranch =
        RegRow(reg, "ScriptBranch", "scriptbranch", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs,
               "If/else node. Emits OnTrue / OnFalse based on internal predicate.");

    TypeId ScriptVariable =
        RegRow(reg, "ScriptVariable", "scriptvariable", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs,
               "Named variable. Emits OnChanged. Sticky output for current value.");

    TypeId ScriptSequence =
        RegRow(reg, "ScriptSequence", "scriptsequence", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs,
               "Sequence node. Fires outputs in order.");

    // ── RENDER / VISUAL ───────────────────────────────────────────
    TypeId StaticMesh =
        RegRow(reg, "StaticMesh", "staticmesh", Spatial,
               TRAIT_Spatial | TRAIT_Render,
               "Non-animated mesh instance.");

    TypeId DynamicMesh =
        RegRow(reg, "DynamicMesh", "dynamicmesh", Spatial,
               TRAIT_Spatial | TRAIT_Render | TRAIT_Animated,
               "Animated mesh instance.");

    TypeId LightSource =
        RegRow(reg, "LightSource", "lightsource", Spatial,
               TRAIT_Spatial | TRAIT_Render,
               "Generic dynamic light. PointLight / SpotLight / DirectionalLight subclasses.");

    TypeId PointLight =
        RegRow(reg, "PointLight", "pointlight", LightSource,
               TRAIT_Spatial | TRAIT_Render,
               "Omnidirectional point light.");

    TypeId SpotLight =
        RegRow(reg, "SpotLight", "spotlight", LightSource,
               TRAIT_Spatial | TRAIT_Render,
               "Cone-shaped spot light.");

    TypeId DirectionalLight =
        RegRow(reg, "DirectionalLight", "directionallight", LightSource,
               TRAIT_Spatial | TRAIT_Render | TRAIT_Singleton,
               "Sun / scene-dominant directional light.");

    TypeId ParticleEmitter =
        RegRow(reg, "ParticleEmitter", "particleemitter", Spatial,
               TRAIT_Spatial | TRAIT_Render | TRAIT_HasInputs,
               "Particle effect emitter. Listens for Start / Stop.");

    TypeId Decal =
        RegRow(reg, "Decal", "decal", Spatial,
               TRAIT_Spatial | TRAIT_Render,
               "Projected texture decal (blood, scorch, footprints).");

    TypeId VolumeFog =
        RegRow(reg, "VolumeFog", "volumefog", Volume,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_Render,
               "Volumetric fog region.");

    // ── PHYSICS ───────────────────────────────────────────────────
    TypeId RigidBody =
        RegRow(reg, "RigidBody", "rigidbody", Spatial,
               TRAIT_Spatial | TRAIT_Physics | TRAIT_Render,
               "Havok rigid body. Has mass, can be hit and fall.");

    TypeId ConstraintJoint =
        RegRow(reg, "ConstraintJoint", "constraintjoint", Entity,
               TRAIT_Physics,
               "Havok constraint between two rigid bodies.");

    TypeId Cloth =
        RegRow(reg, "Cloth", "cloth", Spatial,
               TRAIT_Spatial | TRAIT_Physics | TRAIT_Render,
               "Banner / flag cloth simulation.");

    TypeId Ragdoll =
        RegRow(reg, "Ragdoll", "ragdoll", Spatial,
               TRAIT_Spatial | TRAIT_Physics | TRAIT_Animated | TRAIT_Render,
               "Multi-body ragdoll. Activates on death.");

    // ── PROJECTILES / WEAPONS ─────────────────────────────────────
    TypeId Projectile =
        RegRow(reg, "Projectile", "projectile", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Physics | TRAIT_Render,
               "In-flight projectile. Emits OnHit / OnExpired.");

    TypeId Arrow =
        RegRow(reg, "Arrow", "arrow", Projectile,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Physics | TRAIT_Render,
               "Bow arrow projectile.");

    TypeId Boulder =
        RegRow(reg, "Boulder", "boulder", Projectile,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Physics | TRAIT_Render,
               "Catapult/Trebuchet boulder.");

    TypeId MissileLauncher =
        RegRow(reg, "MissileLauncher", "missilelauncher", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_HasInputs,
               "Generic ranged-weapon emitter. Wires fire/aim into a Projectile spawn.");

    TypeId WeaponPickup =
        RegRow(reg, "WeaponPickup", "weaponpickup", Spatial,
               TRAIT_Spatial | TRAIT_HasEvents | TRAIT_Render | TRAIT_Gameplay,
               "World-placed pickup. Emits OnPickedUp.");

    // ── LAYERS / TEMPLATES ────────────────────────────────────────
    TypeId TemplateLevel =
        RegRow(reg, "templateLevel", "templatelevel", Entity,
               TRAIT_Layer | TRAIT_Singleton,
               "Top-level layer container. Exactly one per .lvl file.");

    TypeId TemplateLayer =
        RegRow(reg, "templateLayer", "templatelayer", Entity,
               TRAIT_Layer,
               "Mid-level grouping (Art / Gameplay / Audio / VFX / AI).");

    TypeId TemplateGroup =
        RegRow(reg, "templateGroup", "templategroup", Entity,
               TRAIT_Layer,
               "Per-region or per-feature group inside a layer.");

    TypeId StaticObject =
        RegRow(reg, "static_object", "staticobject", StaticMesh,
               TRAIT_Spatial | TRAIT_Render,
               "Pandemic-original name for non-animated mesh instance. Same shape as StaticMesh.");

    TypeId ScaledObject =
        RegRow(reg, "scaled_object", "scaledobject", StaticMesh,
               TRAIT_Spatial | TRAIT_Render,
               "Static mesh with non-uniform scale baked into the world transform.");

    // ── DLC / EXTENSION TYPES ─────────────────────────────────────
    TypeId CoriCelestiCapture =
        RegRow(reg, "CoriCelestiCapture", "coricelesticapture", CapturePoint,
               TRAIT_Spatial | TRAIT_Volume | TRAIT_HasEvents | TRAIT_Gameplay | TRAIT_DEPRECATED,
               "DLC-only CapturePoint variant from CoriCelesti. Use CapturePoint for new wires.");

    TypeId BattleScript =
        RegRow(reg, "BattleScript", "battlescript", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Gameplay,
               "Mission-script container. Top-level scenario logic per map.");

    TypeId DialogueLine =
        RegRow(reg, "DialogueLine", "dialogueline", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Audio,
               "Single line of in-mission dialogue. Wires trigger -> VOTrigger -> subtitle.");

    TypeId SubtitleEvent =
        RegRow(reg, "SubtitleEvent", "subtitleevent", Entity,
               TRAIT_HasInputs | TRAIT_Audio,
               "Sub-line of a DialogueLine, displayed on-screen.");

    TypeId TutorialPrompt =
        RegRow(reg, "TutorialPrompt", "tutorialprompt", Entity,
               TRAIT_HasEvents | TRAIT_HasInputs | TRAIT_Gameplay,
               "Tutorial overlay. Emits OnDismissed.");

    // ── SUPPRESS UNUSED-VARIABLE WARNINGS ─────────────────────────
    // The TypeIds we name above are stored in the registry by side
    // effect of registerType(). The compiler will whine "unused"
    // unless we touch them. One harmless cast suppresses everything.
    (void)Entity; (void)Spatial; (void)Volume; (void)Eventful;
    (void)Listener; (void)Wired; (void)Output; (void)OutputVolume;
    (void)OutputCondition; (void)CapturePoint; (void)CaptureArea;
    (void)ObjectiveMarker; (void)ObjectiveController; (void)ScoreManager;
    (void)Director; (void)WinCondition; (void)LossCondition; (void)TeamSpawnZone;
    (void)TriggerVolume; (void)TriggerSphere; (void)TriggerBox;
    (void)TriggerCylinder; (void)DamageZone; (void)HealZone; (void)NoBuildZone;
    (void)SpawnPoint; (void)Spawner; (void)WaveSpawner; (void)ReinforcementPoint;
    (void)AmbushTrigger; (void)PathNetwork; (void)PathNode; (void)PathLink;
    (void)PatrolRoute; (void)AIDirector; (void)AICommander; (void)AISquad;
    (void)GhostKnight; (void)NPCUnit; (void)BehaviorTree; (void)Hero;
    (void)AnimationController; (void)CinematicEvent; (void)CameraTrack;
    (void)CameraTarget; (void)Cutscene; (void)AnimationTrigger;
    (void)VOTrigger; (void)AudioManager; (void)AmbientSound; (void)MusicCue;
    (void)SoundEmitter; (void)DoorActivator; (void)GateController;
    (void)Drawbridge; (void)DestructibleObject; (void)Wall; (void)Tower;
    (void)Catapult; (void)Trebuchet; (void)Ladder; (void)SiegeLadder;
    (void)ScriptRelay; (void)ScriptCounter; (void)ScriptTimer;
    (void)ScriptToggle; (void)ScriptBranch; (void)ScriptVariable;
    (void)ScriptSequence; (void)StaticMesh; (void)DynamicMesh;
    (void)LightSource; (void)PointLight; (void)SpotLight; (void)DirectionalLight;
    (void)ParticleEmitter; (void)Decal; (void)VolumeFog;
    (void)RigidBody; (void)ConstraintJoint; (void)Cloth; (void)Ragdoll;
    (void)Projectile; (void)Arrow; (void)Boulder; (void)MissileLauncher;
    (void)WeaponPickup; (void)TemplateLevel; (void)TemplateLayer;
    (void)TemplateGroup; (void)StaticObject; (void)ScaledObject;
    (void)CoriCelestiCapture; (void)BattleScript; (void)DialogueLine;
    (void)SubtitleEvent; (void)TutorialPrompt;
}

} // namespace Schema
} // namespace Vespucci
