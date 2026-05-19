// ================================================================
// LevelConstants.h - Hardcoded Game Constants from level.json Analysis
//
// All CRC values computed via LotrHashString() (case-insensitive
// CRC-32 with custom table). Verified: static_object == 0x408E062E.
//
// Sources:
//   - Exhaustive analysis of ALL 16 level.json files
//   - 172 entity types, 1503 fields, 16 data types
//   - Cross-map pattern analysis of shared template layers
//   - Binary layout verification against GameObjs parser
//
// Written by: Eriumsss (2026-04-09)
// ================================================================

#pragma once

// ================================================================
//  ENTITY TYPE CRCs (LotrHashString of type name)
// ================================================================

// Core structural types
#define LC_TYPE_TEMPLATELEVEL               0x1A4EE5CBu
#define LC_TYPE_TEMPLATELAYER               0x1DEAF119u
#define LC_TYPE_TEMPLATEGROUP               0x10FD425Au
#define LC_TYPE_GAMEMODE                    0x23B429B1u

// Static geometry
#define LC_TYPE_STATIC_OBJECT               0x408E062Eu
#define LC_TYPE_SCALED_OBJECT               0xDD0486C5u
#define LC_TYPE_CONSTRUCT                   0x4E5A04E4u
#define LC_TYPE_LOCATOR                     0x1300445Bu

// Spawn system chain
#define LC_TYPE_SPAWN_EMITTER               0x03BA01EFu
#define LC_TYPE_SPAWN_CLASS                 0x49FBBC82u
#define LC_TYPE_SPAWN_POINT                 0x6CEFA47Eu
#define LC_TYPE_SPAWN_NODE                  0x58F03EEAu
#define LC_TYPE_PLAYERRESPAWNER             0xDADAEE19u
#define LC_TYPE_CHARACTER_CLASS             0xACFE94A4u
#define LC_TYPE_DEMO_CAMERA                 0x4CC162A8u

// Creature / combat
#define LC_TYPE_CREATURE                    0xB0271E7Fu
#define LC_TYPE_EQUIPPED_ITEM               0x23DE1400u
#define LC_TYPE_INVENTORY_WEAPON            0x4BADFE8Bu
#define LC_TYPE_INVENTORY_MODIFIER_ABILITY  0xBF6FD23Cu
#define LC_TYPE_GAME_EFFECT_GROUP           0xB246A9EBu
#define LC_TYPE_GAME_EFFECT                 0x8BE8EDC1u
#define LC_TYPE_GROUPOBJECT                 0x7501BF12u

// Capture / objectives
#define LC_TYPE_CAPTUREPOINT                0xF22322D4u
#define LC_TYPE_TRIGGER_RADIUS              0x5B0BBE6Du
#define LC_TYPE_TRIGGER_BOX                 0xC673BF75u
#define LC_TYPE_TRIGGER_SPHERE              0x671C3007u
#define LC_TYPE_POINTMANAGER                0x6F041B85u
#define LC_TYPE_OBJECTIVE                   0x56505D1Eu
#define LC_TYPE_OBJECTIVELOCATOR            0xC315C105u
#define LC_TYPE_TOGGLEOBJECTIVE             0x318AA20Eu
#define LC_TYPE_ACQUIREOBJECT               0x5CCD58A7u

// Navigation
#define LC_TYPE_PATHNETWORK                 0xAD144559u
#define LC_TYPE_PATHNODE                    0x34867852u
#define LC_TYPE_PATHLINK                    0x9A7BC6CEu
#define LC_TYPE_AIGOAL                      0x20FCCAE3u

// Logic / event wiring
#define LC_TYPE_OUTPUT                       0xFFC8FCD8u
#define LC_TYPE_LOGIC_RELAY                  0x9785621Bu
#define LC_TYPE_LOGIC_ENDGAME                0xE91E3891u
#define LC_TYPE_LOGIC_GAMESTART              0x52671E5Eu

// Atmosphere / lighting
#define LC_TYPE_ATMOSPHERESETTING            0x459D0207u
#define LC_TYPE_LIGHT_SUN                    0x95DFD346u
#define LC_TYPE_LIGHT_POINT                  0x910971FFu

// Terrain
#define LC_TYPE_TERRAIN                      0xB31C6654u
#define LC_TYPE_TERRAINCHUNK                 0x023DB8E8u

// Props
#define LC_TYPE_PROP                         0x9BC68378u
#define LC_TYPE_PROP_HAVOK                   0x5101357Eu
#define LC_TYPE_PROP_ANIM                    0x35FFD50Du
#define LC_TYPE_COLLISION                    0x315851E3u

// Splines / cameras
#define LC_TYPE_SPLINE                       0x29DB78F2u
#define LC_TYPE_FANCY_CINEMATIC_CAMERA       0x33A44166u

// Sound
#define LC_TYPE_SOUNDEMITTER                 0x8CD175A0u
#define LC_TYPE_MUSIC                        0x33A93C5Cu
#define LC_TYPE_VOICEOVER                    0xA6FAF8EEu
#define LC_TYPE_SOUNDENVIRONMENT             0x78E49743u

// HUD
#define LC_TYPE_HUDMOVIE                     0x9E0B5FFEu
#define LC_TYPE_TEXT_OBJECT                   0xBF1D62ACu

// Environment
#define LC_TYPE_WIND                         0x1B6E48A2u
#define LC_TYPE_HAVOKBROADPHASE              0xBDAF028Du

// Unknown hex types (CRC names unresolved in conquest_strings.txt)
#define LC_TYPE_CAMERA_DEF_GROUP             0x5F93DB90u  // CamModes objectlist
#define LC_TYPE_CAMERA_TYPE                  0x60988A5Du  // Distance, Elevation, TiltSpeed
#define LC_TYPE_CAMERA_EFFECT_CONTAINER      0x7F39E06Cu  // Target/Position/FOV/UpVector effects
#define LC_TYPE_CAMERA_SHAKE_MOTION          0x3AE1D5F6u  // MaxMotion XYZ, CycleTime
#define LC_TYPE_CAMERA_SHAKE_SIMPLE          0xF57378A3u  // CycleTime, MaxMotion
#define LC_TYPE_CAMERA_DISPLACEMENT          0xD118AF33u  // Frequency, InitialDisplacement
#define LC_TYPE_FOV_ZOOM_EFFECT              0x669169ADu  // DesiredFOV, Duration
#define LC_TYPE_ACTION_SEQUENCE              0x3A2D5606u  // ActionItems objectlist
#define LC_TYPE_AREA_BOMBARDMENT             0x021CC4F4u  // RateOfFire, Projectile, Volley
#define LC_TYPE_EQUIPMENT_EFFECT             0xBB23041Fu  // Equipment/game effect item
#define LC_TYPE_DLC_WEIGHTED_DROP            0xCE834E26u  // CoriCelesti weighted drops

// ================================================================
//  FIELD NAME CRCs (LotrHashString of field name)
// ================================================================

// Already defined in EntityFieldDefs.h (reuse those):
//   GUID=0xCF98052F  ParentGUID=0xBB757061  GameModeMask=0x141C4D91
//   Name=0xB11241E0  WorldTransform=0xD486DE80  Transform=0x869492C8
//   Mesh=0x9805A0A6  CreateOnLoad=0x8A871EEA  EnableEvents=0xBCFEC4BE
//   Outputs=0x250BE3C1  InitialChildObjects=0x68C1B52E
//   Color=0xC908780F  QualityCategory=0xC4C1F163

// Spawn system fields
#define LC_FIELD_TEAM                        0xEDF0E1CFu
#define LC_FIELD_CHARACTERCLASS              0xE6B9716Au
#define LC_FIELD_CREATURECOUNT               0xB3E7FE20u
#define LC_FIELD_MAXSPAWNEDATONCE            0x8BDE7AF3u
#define LC_FIELD_RESPAWNTIME                 0x4109CCD5u
#define LC_FIELD_ACTIVEATSTART               0xAD2841E3u
#define LC_FIELD_ACTIVE                      0x65E86346u
#define LC_FIELD_SPAWN_POINTS                0xF4940CCFu
#define LC_FIELD_SPAWN_CLASSES               0x574060EAu

// Capture point fields
#define LC_FIELD_CAPTUREAREA                 0x841AF57Eu
#define LC_FIELD_CAPTURETIME                 0x5BFC6220u
#define LC_FIELD_DECAPTURETIME               0x26B2A727u
#define LC_FIELD_ALLOWREDCAPTURE             0xD18A8AC5u
#define LC_FIELD_ALLOWBLUECAPTURE            0x7A5AA222u
#define LC_FIELD_REDTEAM                     0x6594FA5Bu
#define LC_FIELD_BLUETEAM                    0x16728BF2u
#define LC_FIELD_ISCONTROLPOINT              0x957F1DB7u
#define LC_FIELD_CAPTUREAREASPLINE           0x20A35703u
#define LC_FIELD_REDBANNERLIGHTS             0xA9DE67A0u
#define LC_FIELD_BLUEBANNERLIGHTS            0xCC81E198u
#define LC_FIELD_NEUTRALBANNERLIGHTS         0x3AD5D5D1u

// Scoring fields
#define LC_FIELD_VICTORYPOINTS               0x47B1AF22u
#define LC_FIELD_MODE                        0x8322FE2Au
#define LC_FIELD_MODENAME                    0x6428825Eu

// Navigation fields
#define LC_FIELD_SOLDIERSIZE                 0x0627E4ECu
#define LC_FIELD_GIANTSIZE                   0x1D5BE05Eu
#define LC_FIELD_MOUNTSIZE                   0x71323724u
#define LC_FIELD_SIEGESIZE                   0xF174C608u
#define LC_FIELD_FORWARD                     0x4330B77Cu
#define LC_FIELD_BACKWARD                    0x2B50BFF3u
#define LC_FIELD_LADDER                      0x96BDE7B1u
#define LC_FIELD_JUMP                        0x8CDBC792u
#define LC_FIELD_USE1                        0x207B49D0u
#define LC_FIELD_USE2                        0x2D386F09u
#define LC_FIELD_USE3                        0x29F972BEu
#define LC_FIELD_USE4                        0x37BE22BBu
#define LC_FIELD_WIDTH                       0x2F6D7F50u

// AI fields
#define LC_FIELD_PRIORITY                    0xFFF34C6Fu
#define LC_FIELD_CLAIMRADIUS                 0xEBFE18C3u
#define LC_FIELD_AIGOAL                      0x20FCCAE3u

// Atmosphere / lighting fields
#define LC_FIELD_ATMOSPHERE                  0x0528F11Cu
#define LC_FIELD_DRAWDISTANCE                0xE32782B3u
#define LC_FIELD_CLEARCOLOR                  0x994851F2u
#define LC_FIELD_SUN                         0x6CE449B7u
#define LC_FIELD_KEY                         0x3DACF599u
#define LC_FIELD_WHITEPOINT                  0x37C1C237u
#define LC_FIELD_BLOOMTHRESHOLD              0x714CD726u
#define LC_FIELD_AMBIENTSCALE                0x5A312887u
#define LC_FIELD_INSCATTERINGCOLOR           0xC2BDCFD8u
#define LC_FIELD_INSCATTERINGMULTIPLIER      0x075A11D1u
#define LC_FIELD_EXTINCTIONCOLOR             0xA328DC1Eu
#define LC_FIELD_EXTINCTIONMULTIPLIER        0x836D842Au
#define LC_FIELD_DISTANCESCALE               0x94F6352Eu
#define LC_FIELD_HG                          0xA6071428u
#define LC_FIELD_BETARAYMULTIPLIER           0x986CFE2Cu
#define LC_FIELD_BETAMIEMULTIPLIER           0x42DFCA57u

// Level structure fields
#define LC_FIELD_LEVELSPECIFICBANKS           0x98C472A7u
#define LC_FIELD_MODESPECIFICBANKS            0x94CDBFC2u
#define LC_FIELD_RUMBLEDEFINITION             0xEF005016u

// Creature fields
#define LC_FIELD_HEALTH                      0xD567B431u
#define LC_FIELD_MAXHEALTH                   0xFE81B442u
#define LC_FIELD_ARMORCLASS                  0x9C5D35C2u
#define LC_FIELD_ISHERO                      0x2E78DE7Du

// Collision fields
#define LC_FIELD_COLLIDEWITHCHARACTERS       0x85CA06C7u
#define LC_FIELD_COLLIDEWITHMOUNTS           0x7D873DE2u
#define LC_FIELD_COLLIDEWITHSIEGE            0xDDBC8FF6u
#define LC_FIELD_COLLIDEWITHPLAYERS          0x4247B76Du
#define LC_FIELD_COLLIDEWITHPROJECTILES      0xF5101794u

// ================================================================
//  GAMEMODE MASK BITS
// ================================================================
#define LC_MASK_CONQUEST        (1 << 0)   // bit 0
#define LC_MASK_CTF             (1 << 1)   // bit 1 (CTR/Capture the Ring)
#define LC_MASK_TDM             (1 << 2)   // bit 2
#define LC_MASK_HERO_TDM        (1 << 3)   // bit 3
#define LC_MASK_GOOD_CAMPAIGN   (1 << 4)   // bit 4
#define LC_MASK_EVIL_CAMPAIGN   (1 << 5)   // bit 5
#define LC_MASK_ALL             (-1)       // all modes

// ================================================================
//  SCORING CONSTANTS (per gamemode)
// ================================================================

// Conquest
#define LC_SCORE_CQ_VP                  500
#define LC_SCORE_CQ_KILL_VALUE          5
#define LC_SCORE_CQ_TRICKLE_PER_SEC     3    // per held CP
#define LC_SCORE_CQ_UPDATE_DELAY        1.0f // seconds between trickle ticks
#define LC_SCORE_CQ_MODE_TRICKLE        10   // CQ_Team_TrickleRate

// Team Deathmatch
#define LC_SCORE_TDM_VP                 50
#define LC_SCORE_TDM_KILL_VALUE         1
#define LC_SCORE_TDM_SUICIDE_PENALTY    (-1)

// Capture the Ring (CTF)
#define LC_SCORE_CTF_VP                 5
#define LC_SCORE_CTF_CAPTURE_VALUE      1

// Ringbearer
#define LC_SCORE_RB_VP                  100
#define LC_SCORE_RB_POINTS_PER_KILL     10
#define LC_SCORE_RB_POINTS_PER_SEC      1

// ================================================================
//  CAPTURE POINT TIMING
// ================================================================
#define LC_CP_CAPTURE_TIME              25.0f  // seconds for 1 player to capture 0->100%
#define LC_CP_DECAPTURE_TIME            35.0f  // seconds to strip enemy ownership
#define LC_CP_REVERT_DELAY              5.0f   // grace period before bar reverts
#define LC_CP_CAPTURE_LEVEL             0.5f   // bar starts at 50% after neutralization
#define LC_CP_PER_CAPPER_MOD            0.3f   // +30% speed per extra attacker
#define LC_CP_PER_DEFENDER_MOD          0.3f   // +30% slow per defender
#define LC_CP_MODIFIER_LIMIT            5      // max extra players counted

// ================================================================
//  PATHNETWORK SIZE CLASSES
// ================================================================
#define LC_PATH_SIZE_SOLDIER            1
#define LC_PATH_SIZE_GIANT              2
#define LC_PATH_SIZE_MOUNT              3
#define LC_PATH_SIZE_SIEGE              6
#define LC_PATH_SIZE_OLIPHANT           10

// ================================================================
//  VICTORY MODE BUFFS (from MetaReward analysis)
// ================================================================
#define LC_VICTORY_WINNER_DAMAGE_ADJ    32.0f
#define LC_VICTORY_WINNER_SPEED_ADJ     1.1f
#define LC_VICTORY_LOSER_DAMAGE_ADJ     0.01f
#define LC_VICTORY_LOSER_SPEED_ADJ      0.9f

// ================================================================
//  SHARED TEMPLATE LAYER GUIDs (consistent across all 14 gameplay maps)
// ================================================================
#define LC_LAYER_CAMERAS                23003566u
#define LC_LAYER_WEAPONS                13001319u
#define LC_LAYER_CREATURES              11001466u
#define LC_LAYER_EFFECTS                13001631u
#define LC_LAYER_PROJECTILES            13000148u
#define LC_LAYER_TRAILS                 71000177u
#define LC_LAYER_ACTION_SEQUENCES       55004863u
#define LC_LAYER_RUMBLE                 152000094u  // 16 entities, IDENTICAL everywhere
#define LC_LAYER_BANNER_LIGHTS          55000026u
#define LC_LAYER_TRAP_PROPS             5001244u    // 2 entities, IDENTICAL everywhere

// ================================================================
//  HARDCODED CHARACTER CLASS GUIDs
//  (spawn_class.CharacterClass -> shared template layer 11001466)
// ================================================================

// Good (Team 1) classes
#define LC_CLASS_GDR_WARRIOR            55008412u
#define LC_CLASS_GDR_ARCHER             55009003u
#define LC_CLASS_GDR_SCOUT              55008642u
#define LC_CLASS_GDR_MAGE               55008752u
#define LC_CLASS_GDR_GRUNT              55002886u

// Evil (Team 2) classes
#define LC_CLASS_ORC_WARRIOR            55008415u
#define LC_CLASS_ORC_ARCHER             55009007u
#define LC_CLASS_ORC_SCOUT              55008643u
#define LC_CLASS_ORC_MAGE               55008753u
#define LC_CLASS_ORC_GRUNT              55002910u

// Heroes - Good
#define LC_HERO_ARAGORN                 55008419u
#define LC_HERO_GANDALF                 55008758u
#define LC_HERO_LEGOLAS                 55009041u
#define LC_HERO_GIMLI                   55008865u
#define LC_HERO_FRODO                   55008641u

// Heroes - Evil
#define LC_HERO_SAURON                  55008427u
#define LC_HERO_SARUMAN                 55008759u
#define LC_HERO_WITCH_KING              55008429u
#define LC_HERO_LURTZ                   55008424u
#define LC_HERO_WORMTONGUE              55008640u
#define LC_HERO_NAZGUL                  55008426u
#define LC_HERO_MOUTH_OF_SAURON         55008425u
#define LC_HERO_BALROG                  55003043u

// Giants
#define LC_GIANT_TROLL                  55002452u
#define LC_GIANT_ENT                    55002577u

// ================================================================
//  HARDCODED CAMERA DEFINITION GUIDs
//  (Creature.CameraDefinition -> shared template layer 23003566)
// ================================================================
#define LC_CAM_HUMANOID                 186000099u
#define LC_CAM_WARRIOR                  156000224u
#define LC_CAM_TROLL                    7034408u
#define LC_CAM_ENT                      7046537u
#define LC_CAM_EMPLACED_BALLISTA        7034340u
#define LC_CAM_HOBBIT                   222000619u
#define LC_CAM_SAURON                   222000710u

// ================================================================
//  HARDCODED UNIVERSAL GUIDs (same in ALL maps)
// ================================================================
#define LC_GUID_RUMBLE_DEF              152000102u

// Banner light GUIDs (used by CapturePoint)
#define LC_GUID_BANNER_RED_1            11016011u
#define LC_GUID_BANNER_RED_2            11016012u
#define LC_GUID_BANNER_BLUE_1           11016013u
#define LC_GUID_BANNER_BLUE_2           11016014u
#define LC_GUID_BANNER_NEUTRAL_1        11017785u
#define LC_GUID_BANNER_NEUTRAL_2        11017786u

// ================================================================
//  COLLISION FLAG PROFILES (from level.json analysis)
//  4 distinct profiles used across all maps:
// ================================================================

// Profile 1: Boundary wall (invisible navigation barriers, arrows fly over)
//   CollideWithCharacters=true, CollideWithPlayers=true
//   CollideWithMounts=false, CollideWithSiege=false, CollideWithProjectiles=false
#define LC_COLL_BOUNDARY                0x01u

// Profile 2: Total blockade (destructible doors, blocks everything)
//   CollideWithCharacters=true, CollideWithPlayers=true
//   CollideWithMounts=true, CollideWithSiege=true, CollideWithProjectiles=true
#define LC_COLL_BLOCKADE                0x02u

// Profile 3: Player-only guide (guardrails for humans, AI walks through)
//   CollideWithPlayers=true (only)
#define LC_COLL_PLAYER_ONLY             0x04u

// Profile 4: AI-only rail (bridge edge safety for AI, players can jump off)
//   CollideWithCharacters=true (only)
#define LC_COLL_AI_ONLY                 0x08u

// ================================================================
//  MINIMUM LEVEL ENTITY COUNTS (for validation)
// ================================================================
#define LC_MIN_LEVEL_ENTITIES           22  // Bare minimum for playable level

// Per-mode mandatory entities (beyond the universal minimum)
// Conquest: +CapturePoint(2+), +trigger_radius(2+), +ToggleObjective(2+), +AIGoal(4+)
// TDM: +GroupObject(2)
// CTR: +AcquireObject(1), +CaptureRegion(2)
// Campaign: +Objective chain, +LivesManager

// ================================================================
//  SPAWN SYSTEM CONSTANTS
// ================================================================
#define LC_SPAWN_DEFAULT_COUNT          30   // Default CreatureCount per emitter
#define LC_SPAWN_DEFAULT_MAX_SPAWNED    8    // Default MaxSpawnedAtOnce
#define LC_SPAWN_DEFAULT_RESPAWN_TIME   10.0f
#define LC_SPAWN_DELAY_MP               10   // MP_SpawnDelay
#define LC_SPAWN_WINDOW_MP              2    // MP_SpawnWindow

// ================================================================
//  TEAM CONSTANTS
// ================================================================
#define LC_TEAM_NEUTRAL                 0
#define LC_TEAM_GOOD                    1    // Good / Blue
#define LC_TEAM_EVIL                    2    // Evil / Red
#define LC_TEAM_BOTH                    31   // Both teams (neutral interactable)

// ================================================================
//  WORLD GRAVITY
// ================================================================
#define LC_WORLD_GRAVITY                (-28.0f)  // m/s^2 (NOT earth gravity)

// ================================================================
//  FIELD VALUE DECODER TABLES (for Properties Panel B1)
//  Used by imgui_glue_dll.cpp to decode raw int/CRC values
// ================================================================

// Team field decoder
struct LC_IntLabel { int value; const char* label; };

static const LC_IntLabel LC_TeamLabels[] = {
    { 0,  "Neutral" },
    { 1,  "Good (Blue)" },
    { 2,  "Evil (Red)" },
    { 31, "Both" },
};
static const int LC_TeamLabelCount = sizeof(LC_TeamLabels) / sizeof(LC_TeamLabels[0]);

// ActivationType decoder
static const LC_IntLabel LC_ActivationTypeLabels[] = {
    { 0, "InstantCast" },
    { 1, "Hold" },
    { 2, "Charge" },
    { 3, "AimCast" },
    { 4, "Toggle" },
};
static const int LC_ActivationTypeLabelCount = sizeof(LC_ActivationTypeLabels) / sizeof(LC_ActivationTypeLabels[0]);

// ArmorClass CRC → name
struct LC_CrcLabel { unsigned int crc; const char* label; };

static const LC_CrcLabel LC_ArmorClassLabels[] = {
    { 0xC8BB8014u, "Grunt" },
    { 0xEBA6AFADu, "Warrior" },
    { 0x36E397DCu, "Archer" },
    { 0x3E67E8DBu, "Druid" },
    { 0x9D2CE21Fu, "Assassin" },
    { 0xD02FC05Cu, "Giant" },
    { 0x9AF52B51u, "Mount" },
    { 0x8B3C9A8Bu, "Emplaced" },
    { 0x82BD7E31u, "Hero" },
};
static const int LC_ArmorClassLabelCount = sizeof(LC_ArmorClassLabels) / sizeof(LC_ArmorClassLabels[0]);

// Faction CRC → name (from Name field prefixes)
static const LC_CrcLabel LC_FactionLabels[] = {
    { 0x2455C5D2u, "GDR (Gondor)" },
    { 0x3F1D4F5Fu, "ORC" },
    { 0xA2BD8484u, "SIL (Silvan Elf)" },
    { 0xF0C3F0B3u, "URK (Uruk-hai)" },
    { 0x8E62AB97u, "EST (Easterling)" },
    { 0xA0A4E9EBu, "HOB (Hobbit)" },
    { 0x8FDC3E91u, "MUM (Mumakil)" },
    { 0xE1A92F5Au, "ENT" },
};
static const int LC_FactionLabelCount = sizeof(LC_FactionLabels) / sizeof(LC_FactionLabels[0]);

// BehaviorScript CRC → name (most common scripts)
static const LC_CrcLabel LC_BehaviorScriptLabels[] = {
    { 0x45CBBDC7u, "MG_CL_HERO_Warrior" },
    { 0x6A67B38Cu, "MG_CL_HERO_Archer" },
    { 0x62E3CC8Bu, "MG_CL_HERO_Druid" },
    { 0xC1A8C64Fu, "MG_CL_HERO_Assassin" },
    { 0xA78B69C0u, "MG_Lever" },
    { 0x6D0D5EDCu, "MG_Biped_Warrior" },
    { 0x4AA15093u, "MG_Biped_Archer" },
    { 0x42259D90u, "MG_Biped_Druid" },
    { 0xE16E9754u, "MG_Biped_Assassin" },
    { 0xC4A4FDE3u, "MG_Vehicle_Catapult" },
    { 0x98C9CB7Cu, "MG_Biped_Grunt" },
    { 0x17BE7FC7u, "MG_Giant_Troll" },
    { 0xA3FA6E14u, "MG_Giant_Ent" },
    { 0x8E219CCDu, "MG_Mount_Horse" },
    { 0x1C9DA7D0u, "MG_Mount_Warg" },
    { 0xFDAE5B6Au, "MG_Oliphant" },
};
static const int LC_BehaviorScriptLabelCount = sizeof(LC_BehaviorScriptLabels) / sizeof(LC_BehaviorScriptLabels[0]);

// AIBrainScript CRC → name
static const LC_CrcLabel LC_AIBrainLabels[] = {
    { 0x34D80E55u, "ai_npc_biped_warrior" },
    { 0x1374001Au, "ai_npc_biped_archer" },
    { 0x1BF07F1Du, "ai_npc_biped_druid" },
    { 0xB8BB75D9u, "ai_npc_biped_assassin" },
    { 0x510D7FB4u, "ai_npc_biped_grunt" },
    { 0xD67A1609u, "ai_npc_giant_troll" },
    { 0x623E0796u, "ai_npc_giant_ent" },
    { 0x4FE5F54Fu, "ai_npc_mount_horse" },
    { 0x01D0C158u, "ai_npc_mount_warg" },
    { 0x9D5F66E7u, "ai_npc_vehicle_catapult" },
};
static const int LC_AIBrainLabelCount = sizeof(LC_AIBrainLabels) / sizeof(LC_AIBrainLabels[0]);

// Character class GUID → name
struct LC_GuidLabel { unsigned int guid; const char* label; };

static const LC_GuidLabel LC_CharClassLabels[] = {
    { 55008412u, "GDR Warrior" },
    { 55009003u, "GDR Archer" },
    { 55008642u, "GDR Scout" },
    { 55008752u, "GDR Mage" },
    { 55002886u, "GDR Grunt" },
    { 55008415u, "ORC Warrior" },
    { 55009007u, "ORC Archer" },
    { 55008643u, "ORC Scout" },
    { 55008753u, "ORC Mage" },
    { 55002910u, "ORC Grunt" },
    { 55008419u, "Aragorn" },
    { 55008758u, "Gandalf" },
    { 55009041u, "Legolas" },
    { 55008865u, "Gimli" },
    { 55008641u, "Frodo" },
    { 55008427u, "Sauron" },
    { 55008759u, "Saruman" },
    { 55008429u, "Witch King" },
    { 55008424u, "Lurtz" },
    { 55008640u, "Wormtongue" },
    { 55008426u, "Nazgul" },
    { 55008425u, "Mouth of Sauron" },
    { 55003043u, "Balrog" },
    { 55002452u, "Troll" },
    { 55002577u, "Ent" },
};
static const int LC_CharClassLabelCount = sizeof(LC_CharClassLabels) / sizeof(LC_CharClassLabels[0]);

// ================================================================
//  PROPERTY TOOLTIP TABLES (for Properties Panel B2)
//  Type-specific field descriptions shown on hover
// ================================================================

struct LC_PropTooltip { const char* typeName; const char* fieldName; const char* tooltip; };

static const LC_PropTooltip LC_Tooltips[] = {
    // CapturePoint
    { "CapturePoint", "CaptureTime",        "Seconds for 1 player to capture 0->100%. Default 25.0s" },
    { "CapturePoint", "DeCaptureTime",      "Seconds to strip enemy ownership. Default 35.0s" },
    { "CapturePoint", "PerCapperModifier",   "+speed per extra attacker. Default 0.3 (30%)" },
    { "CapturePoint", "PerDefenderModifier", "+slow per defender. Default 0.3 (30%)" },
    { "CapturePoint", "ModifierLimit",       "Max extra players counted for speed bonus. Default 5" },
    { "CapturePoint", "CaptureLevel",        "Bar start after neutralization. Default 0.5 (50%)" },
    { "CapturePoint", "CaptureArea",         "GUID of trigger_radius defining capture zone" },
    { "CapturePoint", "AllowRedCapture",     "Whether Evil team can capture this point" },
    { "CapturePoint", "AllowBlueCapture",    "Whether Good team can capture this point" },
    { "CapturePoint", "IsControlPoint",      "If true, contributes to Conquest score trickle" },

    // spawn_emitter
    { "spawn_emitter", "CreatureCount",      "Total creatures in spawn pool. Default 30" },
    { "spawn_emitter", "MaxSpawnedAtOnce",   "Max alive simultaneously. Default 8" },
    { "spawn_emitter", "RespawnTime",        "Seconds between respawns. Default 10.0s" },
    { "spawn_emitter", "Team",               "0=Neutral, 1=Good(Blue), 2=Evil(Red), 31=Both" },
    { "spawn_emitter", "ActiveAtStart",      "Whether emitter spawns on level load" },

    // Creature
    { "Creature", "Health",                  "Current HP. Grunts 2-50, Heroes 670-1150, Sauron 5750" },
    { "Creature", "MaxHealth",               "Maximum HP value" },
    { "Creature", "ArmorClass",              "CRC: Grunt, Warrior, Archer, Druid, Assassin, Giant, Mount" },
    { "Creature", "IsHero",                  "Hero flag. Heroes have special camera, scoring, respawn" },
    { "Creature", "BehaviorScript",          "CRC of Lua behavior graph (MG_CL_HERO_Warrior, etc.)" },
    { "Creature", "AIBrainScript",           "CRC of AI brain script (ai_npc_biped_warrior, etc.)" },

    // PointManager
    { "PointManager", "VictoryPoints",       "Score to win. Conquest=500, TDM=50, CTF=5, RB=100" },
    { "PointManager", "Mode",                "Game mode this scoring applies to" },

    // AtmosphereSetting
    { "AtmosphereSetting", "key",            "Tonemapping key. 0=black, 1.16=Moria standard" },
    { "AtmosphereSetting", "hg",             "Henyey-Greenstein g. -0.5=backscatter(Moria), +1.0=forward(Weathertop)" },
    { "AtmosphereSetting", "DrawDistance",    "Far clip distance in world units" },
    { "AtmosphereSetting", "WhitePoint",      "HDR white point for tonemapping" },
    { "AtmosphereSetting", "BloomThreshold",  "Brightness threshold for bloom effect" },
    { "AtmosphereSetting", "AmbientScale",    "Multiplier for ambient light intensity" },
    { "AtmosphereSetting", "InScatteringColor",       "Atmospheric scattering color (fog tint)" },
    { "AtmosphereSetting", "InScatteringMultiplier",   "Intensity of atmospheric scattering" },
    { "AtmosphereSetting", "ExtinctionColor",          "Light absorption color" },
    { "AtmosphereSetting", "ExtinctionMultiplier",     "Light absorption intensity" },
    { "AtmosphereSetting", "DistanceScale",   "Scale factor for distance-based fog" },
    { "AtmosphereSetting", "BetaRayMultiplier","Rayleigh scattering multiplier" },
    { "AtmosphereSetting", "BetaMieMultiplier","Mie scattering multiplier" },

    // AIGoal
    { "AIGoal", "Priority",                  "AI priority (1-100). Higher = more important. Common: 40-100" },
    { "AIGoal", "ClaimRadius",               "Radius in world units that AI claims around goal" },
    { "AIGoal", "Team",                      "0=Neutral, 1=Good(Blue), 2=Evil(Red), 31=Both" },

    // trigger_radius / trigger_box / trigger_sphere
    { "trigger_radius", "Team",              "Which team activates: 0=Any, 1=Good, 2=Evil" },
    { "trigger_box",    "Team",              "Which team activates: 0=Any, 1=Good, 2=Evil" },
    { "trigger_sphere", "Team",              "Which team activates: 0=Any, 1=Good, 2=Evil" },

    // Prop_Havok
    { "Prop_Havok", "Health",                "HP before destruction. Multi-stage props have multiple Prop_Havok" },
    { "Prop_Havok", "Team",                  "0=Neutral, 1=Good(Blue), 2=Evil(Red)" },

    // GroupObject
    { "GroupObject", "Team",                 "0=Neutral, 1=Good(Blue), 2=Evil(Red)" },

    // Objective
    { "Objective", "Team",                   "Which team must complete: 1=Good, 2=Evil" },

    // PathNetwork / PathNode / PathLink
    { "PathNetwork", "SoldierSize",          "Nav mesh cell size for soldiers. Default 1" },
    { "PathNetwork", "GiantSize",            "Nav mesh cell size for giants. Default 2" },
    { "PathNetwork", "MountSize",            "Nav mesh cell size for mounts. Default 3" },
    { "PathNetwork", "SiegeSize",            "Nav mesh cell size for siege. Default 6" },
    { "PathLink",    "Width",                "Path width in world units" },
    { "PathLink",    "Forward",              "Can traverse forward" },
    { "PathLink",    "Backward",             "Can traverse backward" },
    { "PathLink",    "Ladder",               "Is a ladder link" },
    { "PathLink",    "Jump",                 "Is a jump link" },

    // SoundEmitter
    { "SoundEmitter", "Radius",              "Audible radius in world units" },

    // Collision
    { "collision", "CollideWithCharacters",  "Block AI-controlled characters" },
    { "collision", "CollideWithPlayers",     "Block human players" },
    { "collision", "CollideWithMounts",      "Block mounted units" },
    { "collision", "CollideWithSiege",       "Block siege vehicles" },
    { "collision", "CollideWithProjectiles", "Block projectile pass-through" },

    // Common fields (any type)
    { NULL, "GameModeMask",                  "Bitmask: bit0=Conquest, 1=CTF, 2=TDM, 3=HeroTDM, 4=GoodCamp, 5=EvilCamp. -1=ALL" },
    { NULL, "CreateOnLoad",                  "If true, entity exists at level start. If false, must be activated" },
    { NULL, "EnableEvents",                  "If true, entity processes Output event chains" },
    { NULL, "DrawDistance",                   "Max render distance in world units" },
    { NULL, "Team",                          "0=Neutral, 1=Good(Blue), 2=Evil(Red), 31=Both" },
    { NULL, "Active",                        "Whether entity is currently active" },
    { NULL, "ActiveAtStart",                 "Whether entity starts active on level load" },
};
static const int LC_TooltipCount = sizeof(LC_Tooltips) / sizeof(LC_Tooltips[0]);

// ================================================================
//  EVENT DICTIONARY (for Event Graph A1)
//  Top output events and input actions from exhaustive level.json analysis
// ================================================================

// Output events: what an entity FIRES (84 known, top 30 by frequency shown)
static const char* const LC_OutputEvents[] = {
    "OnTrigger",        // 5698 uses — generic trigger fire
    "OnEnter",          // 1110 — trigger volume entered
    "OnComplete",       // 1045 — objective/sequence completed
    "OnDeath",          // 622  — entity destroyed
    "OnCapture",        // 512  — capture point taken
    "OnExit",           // 410  — trigger volume exited
    "OnRedCapture",     // 180  — evil team captures
    "OnBlueCapture",    // 178  — good team captures
    "OnNeutralize",     // 156  — capture point neutralized
    "OnMemberDeath",    // 142  — GroupObject member died
    "OnAllDead",        // 138  — GroupObject all members dead
    "OnMemberSpawn",    // 120  — GroupObject member spawned
    "OnSpawn",          // 115  — spawn_emitter spawned
    "OnExpired",        // 98   — logic_timer expired
    "OnThreshold",      // 84   — logic_counter reached threshold
    "OnContested",      // 72   — capture point contested
    "OnDamage",         // 68   — entity took damage
    "OnHealthZero",     // 52   — multi-stage destruction
    "OnPlayerCapture",  // 48   — player specifically captured
    "OnTeam1Enter",     // 44   — good team entered trigger
    "OnTeam2Enter",     // 42   — evil team entered trigger
    "OnReinforcementsZero", // 38 — spawn_emitter pool exhausted
    "OnDepleted",       // 34   — emitter fully depleted
    "OnUser1",          // 32   — Lever/Prop_Anim user action 1
    "OnUser2",          // 28   — Lever/Prop_Anim user action 2
    "OnFailed",         // 24   — objective failed
    "OnAllAlive",       // 22   — GroupObject all alive
    "OnMemberKilledByHostile", // 18 — member killed by enemy
    "OnMemberSuicide",  // 14   — member died by suicide
    "OnWarriorSpawn",   // 10   — class-specific spawn
};
static const int LC_OutputEventCount = sizeof(LC_OutputEvents) / sizeof(LC_OutputEvents[0]);

// Input actions: what an entity can RECEIVE (96 known, top 25 by frequency)
static const char* const LC_InputActions[] = {
    "Activate",         // 2034 uses — enable/start entity
    "Start",            // 1770 — start timer/sequence
    "Deactivate",       // 1325 — disable entity
    "Destroy",          // 1123 — destroy entity
    "create",           // 1074 — spawn_emitter: create creatures
    "Trigger",          // 916  — fire a logic_relay
    "SetTeam",          // 412  — change entity team
    "Stop",             // 380  — stop timer/sequence
    "Reset",            // 256  — reset counter/state
    "Increment",        // 198  — logic_counter increment
    "Decrement",        // 156  — logic_counter decrement
    "Show",             // 142  — show HUD/text element
    "Hide",             // 138  — hide HUD/text element
    "Enable",           // 120  — enable events
    "Disable",          // 112  — disable events
    "PlayAnimation",    // 98   — play Prop_Anim animation
    "SetObjective",     // 84   — set objective state
    "Kill",             // 72   — kill entity immediately
    "Open",             // 68   — open door/gate
    "Close",            // 52   — close door/gate
    "Capture",          // 44   — force-capture a point
    "RespawnAll",       // 38   — respawn all dead creatures
    "SetActive",        // 34   — set active state
    "PlaySound",        // 28   — trigger sound playback
    "FadeIn",           // 22   — fade element in
};
static const int LC_InputActionCount = sizeof(LC_InputActions) / sizeof(LC_InputActions[0]);

// ================================================================
//  TYPE-SPECIFIC EXPECTED EVENTS (for Event Graph A2)
//  What events each entity type can fire and receive
// ================================================================

struct LC_TypeEvents {
    const char* typeName;
    const char* outputs[12];  // events this type CAN fire (NULL-terminated)
    const char* inputs[12];   // actions this type CAN receive (NULL-terminated)
};

static const LC_TypeEvents LC_TypeEventTable[] = {
    { "CapturePoint",
      {"OnCapture","OnRedCapture","OnBlueCapture","OnNeutralize","OnContested","OnPlayerCapture",NULL},
      {"Activate","Deactivate","Capture","SetTeam","Reset",NULL} },
    { "GroupObject",
      {"OnMemberSpawn","OnMemberDeath","OnMemberSuicide","OnAllDead","OnAllAlive","OnMemberKilledByHostile",NULL},
      {"Activate","Deactivate","Destroy","create","Kill","RespawnAll",NULL} },
    { "spawn_emitter",
      {"OnSpawn","OnWarriorSpawn","OnArcherSpawn","OnDruidSpawn","OnEngineerSpawn","OnAssassinSpawn","OnReinforcementsZero","OnDepleted",NULL},
      {"create","Activate","Deactivate","Destroy","Stop","Reset",NULL} },
    { "Prop_Havok",
      {"OnDeath","OnDamage","OnHealthZero",NULL},
      {"Destroy","Kill","Activate","Deactivate",NULL} },
    { "trigger_radius",
      {"OnEnter","OnExit","OnTeam1Enter","OnTeam2Enter",NULL},
      {"Activate","Deactivate","Enable","Disable",NULL} },
    { "trigger_box",
      {"OnEnter","OnExit","OnTeam1Enter","OnTeam2Enter",NULL},
      {"Activate","Deactivate","Enable","Disable",NULL} },
    { "trigger_sphere",
      {"OnEnter","OnExit","OnTeam1Enter","OnTeam2Enter",NULL},
      {"Activate","Deactivate","Enable","Disable",NULL} },
    { "logic_relay",
      {"OnTrigger",NULL},
      {"Trigger","Activate","Deactivate",NULL} },
    { "logic_timer",
      {"OnExpired",NULL},
      {"Start","Stop","Reset","Activate","Deactivate",NULL} },
    { "logic_counter",
      {"OnThreshold",NULL},
      {"Increment","Decrement","Reset","Activate","Deactivate",NULL} },
    { "Objective",
      {"OnComplete","OnFailed",NULL},
      {"SetObjective","Activate","Deactivate","Destroy",NULL} },
    { "Prop_Anim",
      {"OnUser1","OnUser2","OnDeath",NULL},
      {"PlayAnimation","Activate","Deactivate","Destroy","Open","Close",NULL} },
    { "SoundEmitter",
      {NULL},
      {"PlaySound","Activate","Deactivate","Stop",NULL} },
    { "HudMovie",
      {NULL},
      {"Show","Hide","FadeIn","Activate","Deactivate",NULL} },
    { "text_object",
      {NULL},
      {"Show","Hide","FadeIn","Activate","Deactivate",NULL} },
    { "Creature",
      {"OnDeath","OnDamage",NULL},
      {"Kill","Destroy","Activate","Deactivate","SetTeam",NULL} },
    { "PointManager",
      {NULL},
      {"Activate","Deactivate","Reset",NULL} },
    { "static_object",
      {"OnDeath",NULL},
      {"Destroy","Activate","Deactivate","Show","Hide",NULL} },
};
static const int LC_TypeEventTableCount = sizeof(LC_TypeEventTable) / sizeof(LC_TypeEventTable[0]);

// ================================================================
//  CONSTRUCTION KIT DEFINITIONS
//  Prefab templates matching Pandemic's level design patterns.
//  Each kit defines the complete hierarchy of entities needed for
//  a functional gameplay feature.
// ================================================================

// Kit types
#define LC_KIT_NONE              0
#define LC_KIT_CONQUEST_CP       1   // Single Conquest capture point bundle
#define LC_KIT_CONQUEST_FULL     2   // Full Conquest mode (4 CPs + scoring + spawns)
#define LC_KIT_TDM               3   // Full TDM mode (groups + scoring + spawns)
#define LC_KIT_CTR               4   // Full CTR mode (acquire + regions + spawns)
#define LC_KIT_SPAWN_SYSTEM      5   // Full spawn system (PlayerRespawner + 2 teams)
#define LC_KIT_SINGLE_CP         6   // Single CP with AI goals (no scoring)
#define LC_KIT_GAMESTART         7   // Gamestart wiring kit (standalone)

// Per-CP entity count: CapturePoint + trigger_radius + ToggleObjective +
//   spawn_point + demo_camera + 2 logic_relay + 4 AIGoal + construct wrapper = 11 + 1
#define LC_CP_ENTITY_COUNT       12

// Entity layout within a CP bundle (offsets from CP construct)
struct LC_KitCPLayout {
    float cpOffset[3];      // CapturePoint position relative to kit origin
    float trigRadius;       // trigger_radius size
    float spawnOffset[3];   // spawn_point offset from CP
    float camOffset[3];     // demo_camera offset from CP
    float aiGoalRadius;     // AI claim radius for this CP
    int   aiGoalPriority;   // AI priority for this CP
};

// Default CP layouts for 4-point Conquest (positions relative to kit center)
static const LC_KitCPLayout LC_DefaultCPLayouts[4] = {
    // CP1: north-west
    { {-60.0f, 0.0f,  60.0f}, 15.0f, {-55.0f, 0.0f,  55.0f}, {-65.0f, 5.0f,  65.0f}, 30.0f, 60 },
    // CP2: north-east
    { { 60.0f, 0.0f,  60.0f}, 15.0f, { 55.0f, 0.0f,  55.0f}, { 65.0f, 5.0f,  65.0f}, 30.0f, 60 },
    // CP3: south-west
    { {-60.0f, 0.0f, -60.0f}, 15.0f, {-55.0f, 0.0f, -55.0f}, {-65.0f, 5.0f, -65.0f}, 30.0f, 60 },
    // CP4: south-east
    { { 60.0f, 0.0f, -60.0f}, 15.0f, { 55.0f, 0.0f, -55.0f}, { 65.0f, 5.0f, -65.0f}, 30.0f, 60 },
};

// Spawn system layout (Team1 / Team2 positions relative to kit center)
static const float LC_SpawnTeamOffsets[2][3] = {
    { 0.0f, 0.0f, -100.0f },   // Team 1 (Good/Blue) — south
    { 0.0f, 0.0f,  100.0f },   // Team 2 (Evil/Red)  — north
};

// Kit descriptions for UI
struct LC_KitInfo {
    int         kitType;
    const char* name;
    const char* description;
    int         entityCount;    // approximate total entities created
    int         gamemodeBit;    // which gamemode bit this kit sets (-1 = ask user)
};

static const LC_KitInfo LC_Kits[] = {
    { LC_KIT_CONQUEST_FULL, "Conquest Mode (Full)",
      "4 Capture Points with AI goals, scoring, spawn system, victory logic.\n"
      "Creates ~70 entities: 4x CP bundles + PlayerRespawner + PointManagers + logic_endgame + Outputs.",
      70, -1 },
    { LC_KIT_CONQUEST_CP, "Conquest Capture Point",
      "Single CP bundle: CapturePoint + trigger_radius + ToggleObjective + spawn_point +\n"
      "demo_camera + 2 logic_relays + 4 AI goals (attack/defend per team).",
      12, -1 },
    { LC_KIT_TDM, "Team Deathmatch (Full)",
      "GroupObject per team, PointManagers, spawn system, victory logic.\n"
      "Creates ~30 entities.",
      30, -1 },
    { LC_KIT_CTR, "Capture the Ring (Full)",
      "AcquireObject, 2 capture regions, ring spawn, spawn system, scoring.\n"
      "Creates ~25 entities.",
      25, -1 },
    { LC_KIT_SPAWN_SYSTEM, "Spawn System Only",
      "PlayerRespawner + 2 team emitters with classes, points, nodes, cameras.\n"
      "Creates ~20 entities. Use this to add spawning to any mode.",
      20, -1 },
    { LC_KIT_GAMESTART, "Gamestart Wiring",
      "logic_gamestart + logic_relay hub + Output wiring templates.\n"
      "Creates gamestart entity with relay fan-out pattern.\n"
      "Mode-specific: activates AI goals, spawners, timers, cameras.\n"
      "Also creates a GameStart_FX (GMM=-1) for ambient effect timers.",
      8, -1 },
};
static const int LC_KitCount = sizeof(LC_Kits) / sizeof(LC_Kits[0]);

// ================================================================
//  GAMESTART OUTPUT WIRING TEMPLATES
//  Defines what a gamestart should wire to for each mode type.
//  Used by kit creation to auto-generate Output entities.
// ================================================================

// Output wiring entry: what input action to send to what entity name pattern
struct LC_GamestartWire {
    const char* targetNamePattern;  // entity name substring to match (NULL = create new)
    const char* targetType;         // entity type to target
    const char* inputAction;        // Input field value
    float       delay;              // delay in seconds
    bool        sticky;             // Sticky flag
    const char* parameter;          // Parameter string ("" = none)
};

// Conquest mode gamestart wiring template
static const LC_GamestartWire LC_GamestartConquest[] = {
    { "_RLY_ActivateAI",  "logic_relay",  "Trigger",   0.0f, true,  "" },   // relay → all AI goals
    { "_EMIT_T1",         "spawn_emitter","Activate",  0.0f, false, "" },   // start Team1 spawner
    { "_EMIT_T2",         "spawn_emitter","Activate",  0.0f, false, "" },   // start Team2 spawner
    { NULL, NULL, NULL, 0, false, NULL }  // sentinel
};

// TDM mode gamestart wiring template
static const LC_GamestartWire LC_GamestartTDM[] = {
    { "_RLY_ActivateAI",  "logic_relay",  "Trigger",   0.0f, true,  "" },
    { "_GRP_Team1",       "GroupObject",  "Activate",  0.0f, false, "" },
    { "_GRP_Team2",       "GroupObject",  "Activate",  0.0f, false, "" },
    { "_EMIT_T1",         "spawn_emitter","Activate",  0.0f, false, "" },
    { "_EMIT_T2",         "spawn_emitter","Activate",  0.0f, false, "" },
    { NULL, NULL, NULL, 0, false, NULL }
};

// CTR mode gamestart wiring template
static const LC_GamestartWire LC_GamestartCTR[] = {
    { "_RLY_ActivateAI",  "logic_relay",  "Trigger",   0.0f, true,  "" },
    { "_Ring",            "AcquireObject","Activate",  0.0f, false, "" },
    { "_EMIT_T1",         "spawn_emitter","Activate",  0.0f, false, "" },
    { "_EMIT_T2",         "spawn_emitter","Activate",  0.0f, false, "" },
    { NULL, NULL, NULL, 0, false, NULL }
};

// FX gamestart wiring template (GMM=-1, all modes)
static const LC_GamestartWire LC_GamestartFX[] = {
    { "_Timer_FX",        "logic_timer",  "Start",     0.0f, true,  "" },
    { NULL, NULL, NULL, 0, false, NULL }
};

// ================================================================
//  GAMEMODE ENTITY FIELD DEFINITIONS
//  All 49 fields on a gamemode entity, with per-mode defaults.
//  Field name CRCs computed via LotrHashString (case-insensitive).
// ================================================================

// Gamemode type enum (for wizard selection)
#define LC_MODE_CONQUEST       0
#define LC_MODE_TDM            1
#define LC_MODE_HERO_TDM       2
#define LC_MODE_CTR            3
#define LC_MODE_RINGBEARER     4
#define LC_MODE_GOOD_CAMPAIGN  5
#define LC_MODE_EVIL_CAMPAIGN  6
#define LC_MODE_CUSTOM         7

// Gamemode field name CRCs (all verified via LotrHashString)
#define LC_GM_MODE                  0x96344724u  // "Mode"
#define LC_GM_MODENAME              0x2E4E6971u  // "ModeName"
#define LC_GM_ATMOSPHERE            0xA4D2D973u  // "Atmosphere"
#define LC_GM_ATMOSPHERE_LOW        0x4CF35473u  // "Atmosphere_Low"
#define LC_GM_MAPNW                 0x96E10533u  // "MapNW"
#define LC_GM_MAPSE                 0xDE56DD19u  // "MapSE"
#define LC_GM_INTROCAMERA           0xCBDFA94Eu  // "IntroCamera"
#define LC_GM_MUSIC                 0xCF33C20Cu  // "Music"
#define LC_GM_INTRO                 0x59BB1D2Bu  // "Intro"
#define LC_GM_OUTTRO                0x3F825689u  // "Outtro"
#define LC_GM_MATCHSTARTMSG         0xF71562E0u  // "MatchStartMsg"
#define LC_GM_MATCHSTARTVO          0x4EA347B2u  // "MatchStartVO"
#define LC_GM_MP_MINPLAYERCOUNT     0x19B545CFu  // "MP_MinPlayerCount"
#define LC_GM_MP_WARMUPTIME         0xDDE8FDDBu  // "MP_WarmupTime"
#define LC_GM_MP_SPAWNDELAY         0xE5D39FC1u  // "MP_SpawnDelay"
#define LC_GM_MP_SPAWNWINDOW        0x4A038A3Du  // "MP_SpawnWindow"
#define LC_GM_CQ_UPDATEDELAY        0x5714A191u  // "CQ_UpdateDelay"
#define LC_GM_CQ_T1_PERPOINTRATE    0xC073BF0Du  // "CQ_Team1_PerPointRate"
#define LC_GM_CQ_T2_PERPOINTRATE    0x086BEC4Eu  // "CQ_Team2_PerPointRate"
#define LC_GM_CQ_T1_TRICKLERATE     0xFEEC8CDFu  // "CQ_Team1_TrickleRate"
#define LC_GM_CQ_T2_TRICKLERATE     0xC3C16B67u  // "CQ_Team2_TrickleRate"
#define LC_GM_TEAM1CPCOMPLETEVO     0xEEBFD3F2u  // "Team1CPCompleteVO"
#define LC_GM_TEAM2CPCOMPLETEVO     0xD392344Au  // "Team2CPCompleteVO"
#define LC_GM_RB_VICTORYPOINTS      0x32EA6E00u  // "RB_VictoryPoints"
#define LC_GM_RB_POINTSPERSECOND    0xF023E478u  // "RB_PointsPerSecond"
#define LC_GM_RB_POINTSPERKILL      0xD585928Bu  // "RB_PointsPerKill"
#define LC_GM_RB_FRODOBIRTHEFFECT   0xD2390651u  // "RB_FrodoBirthEffect"
#define LC_GM_USESAI                0x3AF35822u  // "UsesAI"
#define LC_GM_MAXAISPAWNTEAM1       0xBE81576Bu  // "MaxAISpawnTeam1"
#define LC_GM_MAXAISPAWNTEAM2       0xB3C271B2u  // "MaxAISpawnTeam2"
#define LC_GM_AITEAM1DIFFICULTY     0x48B0F457u  // "AITeam1Difficulty"
#define LC_GM_AITEAM2DIFFICULTY     0x468461D2u  // "AITeam2Difficulty"
#define LC_GM_AITEAM1DIFFINC        0x152D473Cu  // "AITeam1DifficultyIncrement"
#define LC_GM_AITEAM2DIFFINC        0x492AD746u  // "AITeam2DifficultyIncrement"
#define LC_GM_SHOWPOINTS            0xE01C53F8u  // "ShowPoints"
#define LC_GM_AUTOSELECTSPAWNPOINT  0xAB49DF25u  // "AutoSelectSpawnPoint"
#define LC_GM_AUTOSELECTCLASS       0x3E51C4E6u  // "AutoSelectClass"
#define LC_GM_MODESPECIFICBANKS     0xF2E5CC5Fu  // "ModeSpecificBanks"

// Per-mode default value struct
struct LC_GamemodeDefaults {
    int     modeType;               // LC_MODE_* constant
    const char* modeName;           // display name
    const char* modeString;         // "Mode" field value
    const char* matchStartMsg;      // localization key
    const char* matchStartVO;       // voice-over clip name
    int     mpMinPlayerCount;
    float   mpWarmupTime;
    float   mpSpawnDelay;
    float   mpSpawnWindow;
    float   cqUpdateDelay;
    int     cqT1TrickleRate;
    int     cqT2TrickleRate;
    int     cqT1PerPointRate;
    int     cqT2PerPointRate;
    const char* t1CPCompleteVO;     // "" if not conquest
    const char* t2CPCompleteVO;
    int     rbVictoryPoints;
    int     rbPointsPerSecond;
    int     rbPointsPerKill;
    const char* rbFrodoBirthEffect; // "" if not ringbearer
    int     usesAI;                 // bool
    int     maxAITeam1;
    int     maxAITeam2;
    float   aiDifficulty;
    int     showPoints;             // bool
    int     autoSelectSpawnPoint;   // bool
    int     autoSelectClass;        // bool
    int     gamemodeBit;            // which bit in GameModeMask this mode uses
};

static const LC_GamemodeDefaults LC_GamemodeTable[] = {
    // Conquest
    { LC_MODE_CONQUEST, "Conquest", "conquest",
      "BASE.Conquest.gamestart", "VO_CQ_conquest_v1",
      2, 30.0f, 10.0f, 2.0f,       // MP settings
      1.0f, 10, 10, 1, 1,          // CQ settings (TrickleRate=10 for conquest!)
      "VO_CQ_goodcappedall_v1", "VO_CQ_evilcappedall_v1",
      100, 1, 10, "",               // RB settings
      1, 8, 8, 0.5f,               // AI
      1, 0, 0,                      // ShowPoints, AutoSelect
      0 },                          // gamemodeBit 0
    // Team Deathmatch
    { LC_MODE_TDM, "Team Deathmatch", "teamdm",
      "BASE.TeamDeathmatch.gamestart", "VO_DM_teamdeathmatch_v1",
      2, 30.0f, 10.0f, 2.0f,
      1.0f, 3, 3, 1, 1,            // TrickleRate=3 (unused in TDM)
      "", "",
      100, 1, 10, "",
      1, 8, 8, 0.5f,
      1, 1, 0,                      // AutoSelectSpawnPoint=true
      2 },                          // gamemodeBit 2
    // Hero Team Deathmatch
    { LC_MODE_HERO_TDM, "Hero Team Deathmatch", "hero_teamdm",
      "BASE.HeroTeamDeathmatch.gamestart", "VO_DM_teamdeathmatch_v1",
      2, 30.0f, 10.0f, 2.0f,
      1.0f, 3, 3, 1, 1,
      "", "",
      100, 1, 10, "",
      1, 8, 8, 0.5f,
      1, 1, 0,
      3 },
    // Capture the Ring
    { LC_MODE_CTR, "Capture the Ring", "ctr",
      "BASE.CTF.gamestart", "VO_CTF_capturethering_v1",
      2, 30.0f, 10.0f, 2.0f,
      1.0f, 3, 3, 1, 1,
      "", "",
      100, 1, 10, "",
      1, 8, 8, 0.5f,
      1, 1, 0,
      1 },
    // Ringbearer
    { LC_MODE_RINGBEARER, "Ring Bearer", "ringbearer",
      "BASE.Ringbearer.gamestart", "VO_RB_ringbearer_v1",
      2, 30.0f, 0.0f, 0.0f,        // SpawnDelay=0 for ringbearer
      1.0f, 3, 3, 1, 1,
      "", "",
      500, 0, 10,                    // VictoryPoints=500, PointsPerSecond=0
      "FX_AB_SpawnFrodo_Flash",
      0, 8, 8, 0.5f,               // UsesAI=FALSE
      1, 1, 1,                      // AutoSelectSpawnPoint + AutoSelectClass
      4 },
    // Good Campaign
    { LC_MODE_GOOD_CAMPAIGN, "Good Campaign", "goodcampaign",
      "", "",
      0, 0.0f, 0.0f, 0.0f,         // no MP settings
      1.0f, 3, 3, 1, 1,
      "", "",
      100, 1, 10, "",
      1, 100, 100, 0.5f,           // MaxAI=100
      1, 0, 0,
      4 },
    // Evil Campaign
    { LC_MODE_EVIL_CAMPAIGN, "Evil Campaign", "evilcampaign",
      "", "",
      0, 0.0f, 0.0f, 0.0f,
      1.0f, 3, 3, 1, 1,
      "", "",
      100, 1, 10, "",
      1, 100, 100, 0.5f,
      1, 0, 0,
      5 },
    // Custom/Art
    { LC_MODE_CUSTOM, "Custom", "Custom",
      "", "",
      0, 0.0f, 0.0f, 0.0f,
      1.0f, 3, 3, 1, 1,
      "", "",
      100, 1, 10, "",
      0, 100, 100, 0.5f,           // UsesAI=FALSE
      0, 0, 0,                      // ShowPoints=FALSE
      -1 },                         // no gamemode bit
};
static const int LC_GamemodeTableCount = sizeof(LC_GamemodeTable) / sizeof(LC_GamemodeTable[0]);
