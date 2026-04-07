#pragma once

#include <Windows.h>
#include <vector>
//Eriumsss
// ============================================================================
// Game Data Reader
// Reads player position, health, team from game memory
// ============================================================================

namespace GameData {

    // Player position (X, Y, Z - standard XYZ, Y is height)
    struct Vector3 {
        float x;
        float y;  // Height
        float z;
    };

    // Entity info for collision box rendering
    struct EntityInfo {
        DWORD address;      // Memory address of entity
        Vector3 position;   // World position
        float aabbMinX, aabbMinY, aabbMinZ;  // AABB min bounds
        float aabbMaxX, aabbMaxY, aabbMaxZ;  // AABB max bounds
        int teamId;         // Team ID (0=neutral, 1=good, 2=evil)
        bool isPlayer;      // True if this is the local player
    };

    // Debug info for pointer chain
    struct DebugInfo {
        DWORD baseAddress;      // Module base address
        DWORD gameStateAddr;    // Address of game state pointer
        DWORD gameStateValue;   // Value at game state pointer
        DWORD playerCtrlAddr;   // Address we read for player controller
        DWORD playerCtrlValue;  // Value of player controller pointer
        DWORD creatureAddr;     // Address we read for creature
        DWORD creatureValue;    // Value of creature pointer
        int failStep;           // Which step failed (0=none, 1=gamestate, 2=playerctrl, 3=creature)

        // Scan results - scan game state structure for valid pointers
        static const int SCAN_COUNT = 32;
        DWORD scanOffsets[SCAN_COUNT];   // Offsets tried
        DWORD scanValues[SCAN_COUNT];    // Values at those offsets

        // Second level scan - scan the object at +54
        DWORD scan2Base;                 // Base pointer for second scan
        DWORD scan2Offsets[SCAN_COUNT];  // Offsets
        DWORD scan2Values[SCAN_COUNT];   // Values
    };

    // Player data structure
    struct PlayerData {
        bool valid;           // True if player data was successfully read
        Vector3 position;     // World position
        float health;         // Current health (0.0 - 1.0 ratio or absolute)
        float maxHealth;      // Max health
        int team;             // Team ID
        int classType;        // Player class (Warrior, Archer, etc.)
    };

    // Initialize the game data reader
    bool Initialize();

    // Get current player data
    PlayerData GetPlayerData();

    // Get debug info about pointer chain
    DebugInfo GetDebugInfo();

    // Check if player is in-game (not in menu)
    bool IsInGame();

    // Get the base module address of the game
    DWORD GetBaseAddress();

    // Get all entities in the game world for collision box rendering
    std::vector<EntityInfo> GetAllEntities();

    // Get player creature pointer
    DWORD GetPlayerCreaturePtr();
}

