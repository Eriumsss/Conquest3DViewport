inherit("npc_common")
inherit("npc_archer_actions")
inherit("npc_archer_behaviours")
inherit("npc_archer_reflex")
inherit("npc_archer_set")
inherit("npc_archer_sequences")
inherit("npc_archer_states_attack")
inherit("npc_archer_states_movement")
inherit("npc_archer_states_idle")
inherit("npc_archer_accuracy")
Creature = setmetatable({
  ClassType = Constant.Creature.ClassName.ARCHER,
  EngageStyle = Constant.Creature.EngageStyle.RANGED,
  SensesSystem = Derived(SensesSystemBaseBipedArcher, {}),
  UseFullLocomotion = true
}, {__index = CreatureBase})
CreatureDetection = setmetatable({
  LayersDefinition = {
    Close = {
      Name = "Layer.Close.Range",
      Range = {MinRange = 0, MaxRange = 20},
      OpponentSelectionClassName = "OpponentSelection.ClosestUnit"
    },
    MidRange = {
      Name = "Layer.Mid.Range",
      Range = {MinRange = 20, MaxRange = 40},
      OpponentSelectionClassName = "OpponentSelection.FromOpponentFirst"
    },
    FarRange = {
      Name = "Layer.Far.Range",
      Range = {MinRange = 40, MaxRange = 800},
      OpponentSelectionClassName = "OpponentSelection.Classes"
    }
  },
  EnableTrackingLayers = true
}, {__index = DetectionSystemDefaultSoldierBase})
Beautifier = setmetatable({EnableAimingUsage = true}, {__index = BeautifierBase})
CreatureTargeting = setmetatable({
  IgnoreVerticalFiltering = true,
  UseSubNetComparison = false,
  AccuracyDefinition = {
    Player_LevelAccuracy = Accuracy_Player_Definition,
    AI_LevelAccuracy = Accuracy_AI_Definition,
    ProjectileRangeLimits = {MinRange = 0, MaxRange = 100}
  }
}, {__index = TargetingSystemBase})
PathFollower = {
  PFieldPathFollower = {
    SizeFieldCosts = {
      Wall = -1,
      Soldier = 1,
      Giant = 1.1,
      Mount = 1,
      Siege = 1.2,
      Oliphant = 1.2,
      OliphantSmallMargin = 1.2,
      OliphantLargeMargin = 1.2
    },
    DynamicAvoidance = PField_DynamicAvoidance_Base,
    Tracking = PField_Tracking_Biped_Base
  }
}
Reflex = setmetatable({}, {__index = Reflex_Definition})
SurroundingEnvironment = {Enabled = true, LocalOffsetLength = 1.6}
VirtualInput = {
  BossUnitSpeedScale = 1,
  RegularUnitSpeedScale = 1,
  SpeedLimit = {MinRange = 0, MaxRange = 1},
  SpeedScaleLimit = {MinRange = 0, MaxRange = 1.25},
  OverrideYAxisConstraint = {
    Item1 = {
      Stance = StanceValues.Idle,
      Action = ActionValues.Unknown
    },
    Item2 = {
      Stance = StanceValues.Idle,
      Action = ActionValues.AIBehavior
    }
  }
}
Weapons = setmetatable({}, {__index = WeaponsArcherBase})
Brain = {
  ActionsList = Actions_Definition,
  ActionSequencesList = Sequences_Definition,
  MainGoalsList = {
    SetList = {
      View_GoalMoveTo = Derived(BehaviourGoal_GoalMoveToBase, {}),
      View_Fight = Derived(View_Fight_Default, {}),
      View_Death = Derived(Set_DeathBase, {}),
      View_Action_Intention = Derived(View_Goal_Action_Intention_Base, {}),
      View_Goal_Assault_Object = Derived(View_Goal_Assault_Object_Base, {}),
      View_Action_Wait_At = Derived(View_Goal_Action_WaitAt_Biped_Base, {}),
      View_Action_Gateway = Derived(BehaviourGoal_Action_Gateway_Archer_Base, {}),
      View_Interaction_AcquireObject = Derived(BehaviourGoal_Interaction_AcquireObject_Base, {})
    },
    BehavioursList = {
      Attack_Ranged = Derived(Behaviour_Archer_Range, {}),
      AttackMoveTo = Derived(Behaviour_Fighting_MoveTo, {}),
      AttackIdle = Derived(Behaviour_Offensive_Transition_Base, {}),
      Goal_GoTo = Derived(Behaviour_Goal_GoTo, {}),
      Goal_Intention = Derived(Behaviour_Intention, {}),
      Goal_Wait = Derived(Behaviour_Goal_Wait_Fidget, {}),
      Goal_Gateway = Derived(Behaviour_Goal_Gateway_Base, {}),
      Goal_Interaction_AcquireObject = Derived(Behaviour_Goal_AcquireObject_Base, {}),
      Hurt_Death = Derived(Behaviour_Hurt_Death_Base, {})
    },
    BehaviourStatesList = {
      Attack_Projectile = Derived(State_Attack_Projectile, {}),
      Attack_Intention = Derived(State_Attack_Intention, {}),
      Move_Melee = Derived(State_Move_Melee, {}),
      Move_Retreat = Derived(State_Move_Retreat, {}),
      Move_OutskirtReposition = Derived(State_RepositionTowardOutskirt, {}),
      Idle = Derived(State_Idle, {}),
      Idle_Tawnt = Derived(State_Idle_Offense_Tawnt, {}),
      Idle_Offense = Derived(State_Idle_Offense, {}),
      Idle_Intention = Derived(State_Idle_Intention_Base, {})
    }
  },
  GoalsSystem = Derived(GoalsSystemBase, {})
}
