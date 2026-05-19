// ================================================================
// MgCreature.cpp - Creature Entity Lifecycle
// Reconstructed from ConquestLLC.exe disassembly
// See MgCreature.h for full documentation
// ================================================================

#include "MgCreature.h"
#include "MgEventSystem.h"
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// Creature::OnInit (FUN_008423bb, 1692 bytes)
//
// Allocates 7 subsystems in order:
//   1. MgCreatureBrain     (0x14 bytes via 0x00818b7f)
//   2. CombatController    (0xD0 bytes via 0x00913235)
//   3. PhysCreatureState   (0xB4 bytes via 0x008194e3)
//   4. InventoryController (0x60 bytes via 0x0081a8dc)
//   5. NetworkSyncComponent(0x34 bytes via 0x007055e7)
//   6. AIBehaviorTree      (0xF90 bytes via 0x0087b773) — largest
//   7. HealthTracker       (0xBC bytes via 0x00705f94) — only if HP > 0
//
// Stores pointers at creature memory offsets:
//   [+0x758] = AIBehaviorTree
//   [+0x764] = CombatController
//   [+0x948] = PhysCreatureState
//   [+0x94C] = InventoryController
//   [+0x950] = HealthTracker
//
// Total subsystem allocation: ~4.6KB per creature.
// ------------------------------------------------------------------
int MgCreature_Init(void* creatureEntity) {
    if (!creatureEntity) return 0;
    uint8_t* ent = (uint8_t*)creatureEntity;

    // Allocate subsystems
    void* brain = malloc(MG_CREATURE_BRAIN_SIZE);
    void* combat = malloc(MG_CREATURE_COMBAT_SIZE);
    void* physics = malloc(MG_CREATURE_PHYSICS_SIZE);
    void* inventory = malloc(MG_CREATURE_INVENTORY_SIZE);
    void* netsync = malloc(MG_CREATURE_NETSYNC_SIZE);
    void* ai = malloc(MG_CREATURE_AI_SIZE);

    if (!brain || !combat || !physics || !inventory || !netsync || !ai) {
        free(brain); free(combat); free(physics);
        free(inventory); free(netsync); free(ai);
        return 0;
    }

    memset(brain, 0, MG_CREATURE_BRAIN_SIZE);
    memset(combat, 0, MG_CREATURE_COMBAT_SIZE);
    memset(physics, 0, MG_CREATURE_PHYSICS_SIZE);
    memset(inventory, 0, MG_CREATURE_INVENTORY_SIZE);
    memset(netsync, 0, MG_CREATURE_NETSYNC_SIZE);
    memset(ai, 0, MG_CREATURE_AI_SIZE);

    // Store pointers at confirmed offsets
    *(void**)(ent + MG_CREATURE_OFF_AI_BRAIN)   = ai;
    *(void**)(ent + MG_CREATURE_OFF_COMBAT_CTRL) = combat;
    *(void**)(ent + MG_CREATURE_OFF_PHYS_STATE)  = physics;
    *(void**)(ent + MG_CREATURE_OFF_INVENTORY)   = inventory;
    *(void**)(ent + MG_CREATURE_OFF_HEALTH)      = NULL;  // Allocated later if HP > 0

    return 1;
}

// ------------------------------------------------------------------
// Creature::OnDeath (FUN_008471d7)
//
// Called when creature HP reaches zero:
//   1. Set death flag at [creature+0x600]
//   2. Set dirty flag bit 0x02 at [creature+0x624]
//   3. Fire OnDeath event (triggers score system via MgPointManager)
//   4. If DeathDeleteWaitForPhysics at [+0x1CE8]:
//      wait for ragdoll to settle before removing entity
//   5. Post death to score system via FUN_007137cb
//      (decrements defender count, increments kill count)
// ------------------------------------------------------------------
void MgCreature_OnDeath(void* creatureEntity) {
    if (!creatureEntity) return;
    uint8_t* ent = (uint8_t*)creatureEntity;

    // Set death flag
    ent[MG_CREATURE_OFF_DEATH_FLAG] = 1;

    // Set dirty death bit
    uint32_t* dirtyFlags = (uint32_t*)(ent + MG_CREATURE_OFF_DIRTY_FLAGS);
    *dirtyFlags |= MG_CREATURE_DIRTY_DEATH;

    // Mark creature flags
    uint8_t* flagsByte = (uint8_t*)(ent + MG_CREATURE_OFF_FLAGS);
    *flagsByte |= MG_CREATURE_FLAG_DEAD;

    // In the real game: fires OnDeath output event, posts to score system
    // via FUN_007137cb, and starts ragdoll if physics enabled.
}

// ------------------------------------------------------------------
// SetStealth (FUN_0083979d)
//
// Toggles stealth state on creature:
//   1. Set/clear bit 6 at [creature+0xA5C]
//   2. If entering stealth: start stealth timer
//   3. If leaving stealth: clear timer, restore visibility
// ------------------------------------------------------------------
void MgCreature_SetStealth(void* creatureEntity, int stealthActive) {
    if (!creatureEntity) return;
    uint8_t* ent = (uint8_t*)creatureEntity;

    uint8_t* flagsByte = (uint8_t*)(ent + MG_CREATURE_OFF_FLAGS);
    if (stealthActive) {
        *flagsByte |= MG_CREATURE_FLAG_STEALTHED;
    } else {
        *flagsByte &= ~MG_CREATURE_FLAG_STEALTHED;
    }
}

// ------------------------------------------------------------------
// HandleAction (FUN_00840848, 4767 bytes)
//
// Biggest gameplay function — dispatches actions by comparing
// action type descriptor address against known constants:
//
//   if (actionType == 0xCFFA80) -> TakeDamage
//     read damage, apply HP reduction, check death threshold
//   if (actionType == 0xCFF990) -> EquipWeapon
//     set weapon slot, update combat controller
//   if (actionType == 0xCFFA2C) -> UseAbility
//     trigger class-specific ability (stealth, heal, bomb, etc.)
//   if (actionType == 0xCFFA38) -> SetStats
//     override DamageAdj, SpeedAdj, HealthAdj (used by VictoryMode)
//   if (actionType == 0xCFFA74) -> Kill/Death
//     force-kill creature (script-triggered)
//   if (actionType == 0xCFFA8C) -> creature_death
//     secondary death handler (cleanup, score posting)
//   if (actionType == 0xCFFABC) -> DamageOverTime
//     apply periodic damage (fire, poison)
//
// Stub: requires live vtable infrastructure to dispatch.
// ------------------------------------------------------------------
void MgCreature_HandleAction(void* creatureEntity, uint32_t actionType, const void* actionData) {
    if (!creatureEntity || !actionData) return;

    // Action dispatch — documentation stub for the viewer.
    // The real function is 4767 bytes of comparisons and subsystem calls.
    (void)actionType;
}

// ------------------------------------------------------------------
// SpawnEmitter helpers
// ------------------------------------------------------------------

// EnqueueSpawnRequest (FUN_00822ea6)
// Allocates a 0x24-byte MgSpawnNode and links it into the spawn queue.
MgSpawnNode* MgSpawn_Enqueue(
    MgHandle objectHandle,
    MgHandle worldHandle,
    float posX, float posY, float posZ,
    uint32_t classIndex
) {
    MgSpawnNode* node = (MgSpawnNode*)malloc(sizeof(MgSpawnNode));
    if (!node) return NULL;

    memset(node, 0, sizeof(MgSpawnNode));
    node->objectHandle = objectHandle;
    node->worldHandle  = worldHandle;
    node->position[0]  = posX;
    node->position[1]  = posY;
    node->position[2]  = posZ;
    node->classIndex   = classIndex;
    node->flags        = 0;
    node->next         = NULL;
    node->prev         = NULL;

    return node;
}

void MgSpawn_FreeNode(MgSpawnNode* node) {
    if (node) free(node);
}

#ifdef __cplusplus
} // extern "C"
#endif
