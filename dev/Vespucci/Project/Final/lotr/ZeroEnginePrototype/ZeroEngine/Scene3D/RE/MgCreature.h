// ================================================================
// MgCreature.h - Creature Entity Lifecycle
// Reconstructed from ConquestLLC.exe disassembly
//
// Creature is the most complex entity type in the game.
// Each creature has 7 subsystems allocated at init (~4.6KB total).
//
// Sources:
//   FUN_008423bb — Creature::OnInit (1692 bytes, subsystem allocation)
//   FUN_00840848 — Creature::HandleAction (4767 bytes, biggest gameplay fn)
//   FUN_008471d7 — Creature::OnDeath handler
//   FUN_0083979d — SetStealth
//   FUN_0083a14b — UpdateStealthTimer
//   FUN_0085960b — Ragdoll::Init (819 bytes)
//
// Type registration:
//   FUN_009572ea — MgCreature type registration
//   FUN_00957509 — MgCreature field table registration
//   Type descriptor at 0x00CE1BB0, vtable base at 0xCE1B64
// ================================================================

#pragma once
#include <stdint.h>
#include "MgPblTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// Creature Subsystem Sizes (from FUN_008423bb allocation calls)
// Total: ~4.6KB per creature
// ------------------------------------------------------------------
#define MG_CREATURE_BRAIN_SIZE       0x14   // MgCreatureBrain (via 0x00818b7f)
#define MG_CREATURE_COMBAT_SIZE      0xD0   // CombatController (via 0x00913235)
#define MG_CREATURE_PHYSICS_SIZE     0xB4   // PhysCreatureState (via 0x008194e3)
#define MG_CREATURE_INVENTORY_SIZE   0x60   // InventoryController (via 0x0081a8dc)
#define MG_CREATURE_NETSYNC_SIZE     0x34   // NetworkSyncComponent (via 0x007055e7)
#define MG_CREATURE_AI_SIZE          0xF90  // AIBehaviorTree (via 0x0087b773) — LARGEST
#define MG_CREATURE_HEALTH_SIZE      0xBC   // HealthTracker (via 0x00705f94, only if HP > 0)

// ------------------------------------------------------------------
// Creature Memory Layout (key offsets from disassembly)
// ------------------------------------------------------------------
#define MG_CREATURE_OFF_MEMBER_LIST    0x150  // member object list
#define MG_CREATURE_OFF_DEATH_FLAG     0x600  // death flag byte
#define MG_CREATURE_OFF_DIRTY_FLAGS    0x624  // dirty flags (0x10=damage, 0x2=death)
#define MG_CREATURE_OFF_CHAR_TYPE_CRC  0x754  // character type CRC
#define MG_CREATURE_OFF_AI_BRAIN       0x758  // AIBehaviorTree pointer
#define MG_CREATURE_OFF_COMBAT_CTRL    0x764  // CombatController pointer
#define MG_CREATURE_OFF_PHYS_STATE     0x948  // PhysCreatureState pointer
#define MG_CREATURE_OFF_INVENTORY      0x94C  // InventoryController pointer
#define MG_CREATURE_OFF_HEALTH         0x950  // HealthTracker pointer
#define MG_CREATURE_OFF_EVT_SABOTAGE   0x98C  // OnSabotage event CRC storage
#define MG_CREATURE_OFF_EVT_HEALTH13   0x990  // OnHealth(1/3) event CRC
#define MG_CREATURE_OFF_EVT_HEALTH23   0x99C  // OnHealth(2/3) event CRC
#define MG_CREATURE_OFF_FLAGS          0xA5C  // stealth/status flags byte
#define MG_CREATURE_OFF_DEATH_PHYSICS  0x1CE8 // DeathDeleteWaitForPhysics flag
#define MG_CREATURE_OFF_PHYS_FLAG2     0x1CF0 // additional physics flag

// Creature flags bits (at offset 0xA5C)
#define MG_CREATURE_FLAG_DEAD          0x08   // bit 3: dead
#define MG_CREATURE_FLAG_PLAYER_CTRL   0x10   // bit 4: player controlled
#define MG_CREATURE_FLAG_STEALTHED     0x40   // bit 6: currently stealthed

// Dirty flags bits (at offset 0x624)
#define MG_CREATURE_DIRTY_DEATH        0x02   // bit 1: death pending
#define MG_CREATURE_DIRTY_DAMAGE       0x10   // bit 4: damage taken

// ------------------------------------------------------------------
// Creature Action Types (vtable-based dispatch in FUN_00840848)
// Each action type is identified by comparing against type descriptor addresses
// ------------------------------------------------------------------
#define MG_ACTION_TAKE_DAMAGE    0xCFFA80  // TakeDamage action descriptor
#define MG_ACTION_EQUIP_WEAPON   0xCFF990  // EquipWeapon action descriptor
#define MG_ACTION_USE_ABILITY    0xCFFA2C  // UseAbility action descriptor
#define MG_ACTION_SET_STATS      0xCFFA38  // SetStats action descriptor
#define MG_ACTION_KILL_DEATH     0xCFFA74  // Kill/Death action descriptor
#define MG_ACTION_CREATURE_DEATH 0xCFFA8C  // creature_death action descriptor
#define MG_ACTION_DAMAGE_DOT     0xCFFABC  // DamageOverTime action descriptor

// ------------------------------------------------------------------
// Spawn Emitter (from FUN_008238b3 and FUN_00822ea6)
// ------------------------------------------------------------------

// Spawn queue node (0x24 = 36 bytes per request)
typedef struct MgSpawnNode {
    MgHandle  objectHandle;    // +0x00 entity to spawn
    MgHandle  worldHandle;     // +0x04 world handle
    float     position[3];     // +0x08 spawn position
    uint32_t  _pad;            // +0x14
    uint32_t  classIndex;      // +0x18 character class index (0-4)
    uint8_t   flags;           // +0x1C flag byte
    uint8_t   _pad2[3];        // +0x1D
    struct MgSpawnNode* next;  // +0x20 doubly-linked next
    struct MgSpawnNode* prev;  // +0x24 doubly-linked prev (overlaps with struct end)
} MgSpawnNode;

// Spawn emitter event CRC offsets (from FUN_008238b3)
#define MG_SPAWN_EVT_OnSpawn          0x1F0  // "OnSpawn" CRC at 0x9cd4b4
#define MG_SPAWN_EVT_OnWarriorSpawn   0x1F4  // "OnWarriorSpawn" at 0x9cd4e0
#define MG_SPAWN_EVT_OnArcherSpawn    0x1F8  // "OnArcherSpawn" at 0x9cd4f0
#define MG_SPAWN_EVT_OnDruidSpawn     0x1FC  // "OnDruidSpawn" at 0x9cd500
#define MG_SPAWN_EVT_OnEngineerSpawn  0x200  // "OnEngineerSpawn" at 0x9cd510
#define MG_SPAWN_EVT_OnAssassinSpawn  0x204  // "OnAssassinSpawn" at 0x9cd520
#define MG_SPAWN_EVT_OnReinfZero      0x208  // "OnReinforcementsZero" at 0x9cd4bc
#define MG_SPAWN_EVT_OnDepleted       0x20C  // "OnDepleted" at 0x9cd4d4

// ------------------------------------------------------------------
// GroupObject Event Offsets (from FUN_009446b4)
// ------------------------------------------------------------------
#define MG_GROUP_EVT_OnMemberSpawn            0x16C
#define MG_GROUP_EVT_OnMemberDeath            0x170
#define MG_GROUP_EVT_OnMemberSuicide          0x174
#define MG_GROUP_EVT_OnAllDead                0x178
#define MG_GROUP_EVT_OnAllAlive               0x17C
#define MG_GROUP_EVT_OnMemberAdded            0x180
#define MG_GROUP_EVT_OnMemberRemoved          0x184
#define MG_GROUP_EVT_OnMemberKilledByHostile  0x188  // by hostile PLAYER
#define MG_GROUP_EVT_OnMemberKilledByFriendly 0x18C  // by friendly PLAYER
#define MG_GROUP_EVT_OnMemberKilledByTeam     0x190  // by hostile TEAM (used for scoring)

// ------------------------------------------------------------------
// Function Addresses
// ------------------------------------------------------------------
#define MG_ADDR_CreatureOnInit      0x008423BB  // Creature subsystem allocation (1692 bytes)
#define MG_ADDR_CreatureHandleAction 0x00840848  // Action dispatch (4767 bytes)
#define MG_ADDR_CreatureOnDeath     0x008471D7  // Death handler
#define MG_ADDR_SetStealth          0x0083979D  // Stealth toggle
#define MG_ADDR_UpdateStealthTimer  0x0083A14B  // Stealth timer tick
#define MG_ADDR_RagdollInit         0x0085960B  // Ragdoll setup (819 bytes)
#define MG_ADDR_SpawnEmitterHandler 0x008238B3  // SpawnEmitter::HandleAction
#define MG_ADDR_SpawnEnqueue        0x00822EA6  // EnqueueSpawnRequest
#define MG_ADDR_GroupObjectHandler  0x009446B4  // GroupObject::HandleAction
#define MG_ADDR_GroupAddMember      0x009441F7  // GroupObject::AddMember
#define MG_ADDR_MaxCreaturesReader  0x0042E279  // MaxCreatures field reader
#define MG_ADDR_ScorePostDeath      0x007137CB  // Post death to score system

// Type registration addresses
#define MG_ADDR_CreatureRegister       0x009572EA  // MgCreature type registration
#define MG_ADDR_CreatureFieldsRegister 0x00957509  // MgCreature field table
#define MG_ADDR_SpawnEmitterRegister   0x0095789B  // MgSpawnEmitter type
#define MG_ADDR_ResourceSpawnRegister  0x009577F9  // MgResourceSpawnObject type
#define MG_ADDR_PlayerRespawnerReg     0x00959C20  // MgPlayerRespawner type
#define MG_ADDR_GroupObjectRegister    0x00957035  // MgGroupObject type
#define MG_ADDR_BCSpawnPlayer          0x00965AD8  // MgBC_SpawnPlayer behavior
#define MG_ADDR_BCEnableSpawning       0x00965DBD  // MgBC_EnablePlayerSpawning
#define MG_ADDR_ExSetSpawnTimer        0x0093A461  // Lua-callable spawn timer setter

#ifdef __cplusplus
} // extern "C"
#endif