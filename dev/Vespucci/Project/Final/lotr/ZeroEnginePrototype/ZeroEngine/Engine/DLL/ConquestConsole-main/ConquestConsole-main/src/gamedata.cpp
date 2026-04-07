#include "gamedata.h"
#include <cstdio>
//Eriumsss
// ============================================================================
// Game Data Reader Implementation
// Based on reverse engineering of ConquestLLC.exe
// ============================================================================

namespace GameData {

    // Game base address (usually 0x00400000 for non-ASLR executables)
    static DWORD g_BaseAddress = 0;

    // Known global pointers - these are ABSOLUTE addresses from Ghidra
    // The game doesn't use ASLR, so base is 0x00400000
    static const DWORD ADDR_GAME_STATE_ABS = 0x00cd7fdc;    // Primary game state pointer

    // Creature array at DAT_00cd7f20 + 0x64 (4 creature pointers for 4 factions)
    // Found in FUN_0045849c: piVar4 = (int *)(DAT_00cd7f20 + 100); loop 4 times
    static const DWORD ADDR_CREATURE_ARRAY_ABS = 0x00cd7f20;  // Base of creature data
    static const DWORD OFF_CREATURE_ARRAY      = 0x64;        // Offset to 4-creature array (100 decimal)

    // DAT_00cd8038 with +0xc -> +0x20 chain (from FUN_0045849c line 21)
    static const DWORD ADDR_CONTEXT_ABS = 0x00cd8038;       // Context object pointer

    // OLD: Direct player pointer from DAT_00cd8048 + 0x162c (static profile, not live)
    static const DWORD ADDR_PLAYER_MANAGER_ABS = 0x00cd8048;  // Player manager pointer
    static const DWORD OFF_CURRENT_PLAYER      = 0x162c;      // Offset to current player pointer

    // Creature structure offsets (from MgCreature base)
    // Based on FUN_007cd0c1: position is at [creature + 0x124] + 0x40/0x48
    static const DWORD OFF_CREATURE_TRANSFORM = 0x124;  // Pointer to transform structure
    static const DWORD OFF_TRANSFORM_POS_X    = 0x40;   // Position X in transform
    static const DWORD OFF_TRANSFORM_POS_Y    = 0x44;   // Position Y (height) in transform
    static const DWORD OFF_TRANSFORM_POS_Z    = 0x48;   // Position Z in transform

    // Alternative direct offsets (original assumption)
    static const DWORD OFF_CREATURE_POS_X    = 0x50;    // Position X (direct)
    static const DWORD OFF_CREATURE_POS_Y    = 0x54;    // Position Y (height)
    static const DWORD OFF_CREATURE_POS_Z    = 0x58;    // Position Z
    static const DWORD OFF_CREATURE_HEALTH   = 0x134;   // Health ratio (float)
    static const DWORD OFF_CREATURE_TEAM     = 0x1ca0;  // Team ID

    // Default AABB size for creatures (approximate humanoid size)
    static const float DEFAULT_AABB_HALF_WIDTH  = 0.5f;   // X and Z half-size
    static const float DEFAULT_AABB_HEIGHT      = 2.0f;   // Y full height

    // OLD: Player controller offsets - from FUN_0079ce88 (requires context object, not raw GameState)
    // GameState + 0x18c = PlayerController
    // PlayerController + 0xc = Creature
    static const DWORD OFF_PLAYER_CTRL       = 0x18c;   // Pointer at +18c in game state
    static const DWORD OFF_CREATURE_PTR      = 0xc;     // Creature pointer at +c in player controller

    // Safe memory read helpers
    template<typename T>
    bool SafeRead(DWORD address, T* out) {
        __try {
            *out = *(T*)address;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeReadPtr(DWORD address, DWORD* out) {
        return SafeRead<DWORD>(address, out);
    }

    bool SafeReadFloat(DWORD address, float* out) {
        return SafeRead<float>(address, out);
    }

    bool SafeReadInt(DWORD address, int* out) {
        return SafeRead<int>(address, out);
    }

    DWORD GetBaseAddress() {
        if (g_BaseAddress == 0) {
            // Get the base address of the main module
            g_BaseAddress = (DWORD)GetModuleHandleA(NULL);
        }
        return g_BaseAddress;
    }

    bool Initialize() {
        g_BaseAddress = GetBaseAddress();
        return g_BaseAddress != 0;
    }

    // Get debug info about the pointer chain
    DebugInfo GetDebugInfo() {
        DebugInfo info = {};
        info.baseAddress = GetBaseAddress();
        info.failStep = 0;

        // Initialize scan arrays
        for (int i = 0; i < DebugInfo::SCAN_COUNT; i++) {
            info.scanOffsets[i] = i * 4;
            info.scanValues[i] = 0;
            info.scan2Offsets[i] = i * 4;
            info.scan2Values[i] = 0;
        }
        info.scan2Base = 0;

        // Read DAT_00cd7f20 (creature array base)
        DWORD creatureArrayRelOffset = ADDR_CREATURE_ARRAY_ABS - 0x00400000;
        DWORD creatureArrayAddr = info.baseAddress + creatureArrayRelOffset;

        DWORD creatureArrayBase = 0;
        if (!SafeReadPtr(creatureArrayAddr, &creatureArrayBase)) {
            info.failStep = 1;
            return info;
        }

        // Store for display
        info.gameStateAddr = creatureArrayAddr;
        info.gameStateValue = creatureArrayBase;

        if (creatureArrayBase == 0) {
            info.failStep = 1;
            return info;
        }

        // Scan the creature array at offset 0x64 (4 creatures for 4 factions)
        info.playerCtrlAddr = creatureArrayBase + OFF_CREATURE_ARRAY;
        info.playerCtrlValue = 0;  // Will show the array

        // Read the 4 creature pointers from the array
        DWORD creatures[4] = {0};
        for (int i = 0; i < 4; i++) {
            SafeReadPtr(creatureArrayBase + OFF_CREATURE_ARRAY + (i * 4), &creatures[i]);
        }

        // Scan area around creature array (0x50 to 0x7C)
        for (int i = 0; i < DebugInfo::SCAN_COUNT; i++) {
            DWORD offset = 0x50 + (i * 4);
            info.scanOffsets[i] = offset;
            SafeReadPtr(creatureArrayBase + offset, &info.scanValues[i]);
        }

        // Find first non-zero creature (player's creature)
        DWORD creature = 0;
        int playerTeam = -1;
        for (int i = 0; i < 4; i++) {
            if (creatures[i] != 0) {
                creature = creatures[i];
                playerTeam = i;
                break;
            }
        }

        if (creature == 0) {
            info.failStep = 2;
            return info;
        }

        // Store creature info
        info.creatureAddr = creatureArrayBase + OFF_CREATURE_ARRAY + (playerTeam * 4);
        info.creatureValue = creature;

        // Scan the creature structure (first 64 bytes)
        info.scan2Base = creature;
        for (int i = 0; i < DebugInfo::SCAN_COUNT; i++) {
            DWORD offset = i * 4;
            info.scan2Offsets[i] = offset;
            SafeReadPtr(creature + offset, &info.scan2Values[i]);
        }

        return info;
    }

    // Get pointer to current player creature
    // Uses DAT_00cd7f20 + 0x64 creature array (4 creatures for 4 factions)
    DWORD GetPlayerCreaturePtr() {
        DWORD baseAddr = GetBaseAddress();
        if (baseAddr == 0) return 0;

        // Read DAT_00cd7f20
        DWORD creatureArrayRelOffset = ADDR_CREATURE_ARRAY_ABS - 0x00400000;
        DWORD creatureArrayAddr = baseAddr + creatureArrayRelOffset;

        DWORD creatureArrayBase = 0;
        if (!SafeReadPtr(creatureArrayAddr, &creatureArrayBase) || creatureArrayBase == 0) {
            return 0;
        }

        // Find first non-zero creature in the array
        for (int i = 0; i < 4; i++) {
            DWORD creature = 0;
            if (SafeReadPtr(creatureArrayBase + OFF_CREATURE_ARRAY + (i * 4), &creature) && creature != 0) {
                return creature;
            }
        }

        return 0;
    }

    bool IsInGame() {
        return GetPlayerCreaturePtr() != 0;
    }

    PlayerData GetPlayerData() {
        PlayerData data = {};
        data.valid = false;

        DWORD creature = GetPlayerCreaturePtr();
        if (creature == 0) {
            return data;
        }

        // Try transform-based position first (from FUN_007cd0c1)
        // Position is at [creature + 0x124] + 0x40/0x44/0x48
        DWORD transform = 0;
        if (SafeReadPtr(creature + OFF_CREATURE_TRANSFORM, &transform) && transform != 0) {
            // Read from transform structure
            if (!SafeReadFloat(transform + OFF_TRANSFORM_POS_X, &data.position.x)) {
                // Fall back to direct offsets
                if (!SafeReadFloat(creature + OFF_CREATURE_POS_X, &data.position.x)) return data;
                if (!SafeReadFloat(creature + OFF_CREATURE_POS_Z, &data.position.z)) return data;
                if (!SafeReadFloat(creature + OFF_CREATURE_POS_Y, &data.position.y)) return data;
            } else {
                // Continue with transform-based reading
                if (!SafeReadFloat(transform + OFF_TRANSFORM_POS_Z, &data.position.z)) return data;
                if (!SafeReadFloat(transform + OFF_TRANSFORM_POS_Y, &data.position.y)) return data;
            }
        } else {
            // Fall back to direct offsets
            if (!SafeReadFloat(creature + OFF_CREATURE_POS_X, &data.position.x)) return data;
            if (!SafeReadFloat(creature + OFF_CREATURE_POS_Z, &data.position.z)) return data;
            if (!SafeReadFloat(creature + OFF_CREATURE_POS_Y, &data.position.y)) return data;
        }

        // Read health
        if (!SafeReadFloat(creature + OFF_CREATURE_HEALTH, &data.health)) return data;
        data.maxHealth = 100.0f;  // Placeholder - need to find max health offset

        // Read team
        if (!SafeReadInt(creature + OFF_CREATURE_TEAM, &data.team)) return data;

        data.valid = true;
        return data;
    }

    // Get all entities in the game world for collision box rendering
    // Currently uses the creature array (4 creatures for 4 factions)
    // TODO: Expand to include all NPCs from hkpWorld physics entities
    std::vector<EntityInfo> GetAllEntities() {
        std::vector<EntityInfo> entities;

        DWORD baseAddr = GetBaseAddress();
        if (baseAddr == 0) return entities;

        // Read DAT_00cd7f20 (creature array base)
        DWORD creatureArrayRelOffset = ADDR_CREATURE_ARRAY_ABS - 0x00400000;
        DWORD creatureArrayAddr = baseAddr + creatureArrayRelOffset;

        DWORD creatureArrayBase = 0;
        if (!SafeReadPtr(creatureArrayAddr, &creatureArrayBase) || creatureArrayBase == 0) {
            return entities;
        }

        // Get player creature for comparison
        DWORD playerCreature = GetPlayerCreaturePtr();

        // Read the 4 creature pointers from the array at offset 0x64
        for (int i = 0; i < 4; i++) {
            DWORD creature = 0;
            if (!SafeReadPtr(creatureArrayBase + OFF_CREATURE_ARRAY + (i * 4), &creature) || creature == 0) {
                continue;
            }

            EntityInfo ent = {};
            ent.address = creature;
            ent.isPlayer = (creature == playerCreature);

            // Read team ID
            BYTE teamByte = 0;
            SafeRead<BYTE>(creature + OFF_CREATURE_TEAM, &teamByte);
            ent.teamId = teamByte;

            // Try to read position from transform structure
            DWORD transform = 0;
            bool gotPosition = false;

            if (SafeReadPtr(creature + OFF_CREATURE_TRANSFORM, &transform) && transform != 0) {
                // Read position from transform (XYZ order, Y is height)
                if (SafeReadFloat(transform + OFF_TRANSFORM_POS_X, &ent.position.x) &&
                    SafeReadFloat(transform + OFF_TRANSFORM_POS_Y, &ent.position.y) &&
                    SafeReadFloat(transform + OFF_TRANSFORM_POS_Z, &ent.position.z)) {
                    gotPosition = true;
                }
            }

            // Fallback to direct offsets if transform failed
            if (!gotPosition) {
                if (!SafeReadFloat(creature + OFF_CREATURE_POS_X, &ent.position.x) ||
                    !SafeReadFloat(creature + OFF_CREATURE_POS_Y, &ent.position.y) ||
                    !SafeReadFloat(creature + OFF_CREATURE_POS_Z, &ent.position.z)) {
                    continue;  // Skip if we can't read position
                }
            }

            // Create default AABB around position (humanoid-sized box)
            // AABB is centered on X/Z, but Y goes from feet to head
            ent.aabbMinX = ent.position.x - DEFAULT_AABB_HALF_WIDTH;
            ent.aabbMaxX = ent.position.x + DEFAULT_AABB_HALF_WIDTH;
            ent.aabbMinY = ent.position.y;  // Feet at position Y
            ent.aabbMaxY = ent.position.y + DEFAULT_AABB_HEIGHT;  // Head above
            ent.aabbMinZ = ent.position.z - DEFAULT_AABB_HALF_WIDTH;
            ent.aabbMaxZ = ent.position.z + DEFAULT_AABB_HALF_WIDTH;

            entities.push_back(ent);
        }

        return entities;
    }
}

