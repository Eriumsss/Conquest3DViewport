// ================================================================
// MgLevelLoader.cpp - Level Loading Pipeline
// Reconstructed from ConquestLLC.exe disassembly
// See MgLevelLoader.h for full documentation
// ================================================================

#include "MgLevelLoader.h"
#include "MgCRC.h"
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// ValidateGameObjsHeader
//
// Checks the 32-byte GameObjs header (from FUN_007e8ecb):
//   [+0x00] magic must be 0x4D414704
//   [+0x04] numTypes < 10000 (sanity)
//   [+0x0C] numEntities < 100000 (sanity)
//   [+0x08] typeOffset must be within data bounds
//   [+0x10] entityOffset must be within data bounds
// ------------------------------------------------------------------
int MgLevel_ValidateGameObjsHeader(const MgGameObjsHeader* header, uint32_t dataSize) {
    if (!header) return 0;
    if (header->magic != MG_GAMEOBJS_MAGIC) return 0;
    if (header->numTypes > 10000) return 0;
    if (header->numEntities > 100000) return 0;
    if (header->typeOffset >= dataSize) return 0;
    if (header->entityOffset >= dataSize) return 0;
    return 1;
}

// ------------------------------------------------------------------
// ValidateLevelMasterHeader (from FUN_007437a1)
//
// The first 0x38 bytes of the level file:
//   [+0x00] version must be 4
// If version != 4, the game refuses to load.
// ------------------------------------------------------------------
int MgLevel_ValidateMasterHeader(const MgLevelMasterHeader* header) {
    if (!header) return 0;
    return (header->version == 4) ? 1 : 0;
}

// ------------------------------------------------------------------
// GamemodeResolution (FUN_00916d9f, 493 bytes)
//
// Three-pass matching to find the active gamemode entity:
//
//   Pass 1: Match by GameModeID hash
//     For each entity of type "gamemode":
//       hash = CRC of entity's GameModeID string
//       if hash == [levelManager+0x30] -> match, store at [+0x260]
//
//   Pass 2: Match by GameModeType CRC
//     For each entity of type "gamemode":
//       if entity.typeCRC == [levelManager+0x2C] -> match
//
//   Pass 3: Match by entity name string
//     For each entity of type "gamemode":
//       hash = CRC of entity name
//       if hash == [levelManager+0x34] -> match
//
// If no match after 3 passes, the level has no active gamemode.
// Result stored at [levelManager + MG_GAMEMODE_MATCH_OFFSET].
// ------------------------------------------------------------------
typedef struct {
    uint32_t gameModeIDHash;  // [+0x30] from lobby selection
    uint32_t gameModeTypeCRC; // [+0x2C] from lobby selection
    uint32_t gameModeNameHash;// [+0x34] from lobby selection
} MgGamemodeQuery;

int MgLevel_ResolveGamemode(
    const MgGamemodeQuery* query,
    const uint32_t* entityTypeCRCs,
    const uint32_t* entityNameHashes,
    const uint32_t* entityGameModeIDHashes,
    uint32_t numEntities,
    uint32_t* outMatchIndex
) {
    if (!query || !outMatchIndex) return 0;

    static uint32_t s_gamemodeCRC = 0;
    if (s_gamemodeCRC == 0) {
        s_gamemodeCRC = MgCRC_HashString_Runtime("gamemode");
    }

    // Pass 1: Match by GameModeID hash
    if (entityGameModeIDHashes) {
        for (uint32_t i = 0; i < numEntities; i++) {
            if (entityTypeCRCs[i] != s_gamemodeCRC) continue;
            if (entityGameModeIDHashes[i] == query->gameModeIDHash) {
                *outMatchIndex = i;
                return 1;
            }
        }
    }

    // Pass 2: Match by GameModeType CRC
    for (uint32_t i = 0; i < numEntities; i++) {
        if (entityTypeCRCs[i] != s_gamemodeCRC) continue;
        if (entityTypeCRCs[i] == query->gameModeTypeCRC) {
            *outMatchIndex = i;
            return 2;
        }
    }

    // Pass 3: Match by entity name hash
    if (entityNameHashes) {
        for (uint32_t i = 0; i < numEntities; i++) {
            if (entityTypeCRCs[i] != s_gamemodeCRC) continue;
            if (entityNameHashes[i] == query->gameModeNameHash) {
                *outMatchIndex = i;
                return 3;
            }
        }
    }

    return 0;  // No match
}

// ------------------------------------------------------------------
// FindBlock (FUN_0067a957)
//
// Iterates registered PAK readers, searching each for a block
// matching the given name hash. The game hashes the block name
// string (e.g., "level", "gamemode") and compares against
// registered block entries.
//
// Returns pointer to block data, or NULL if not found.
// ------------------------------------------------------------------
const void* MgLevel_FindBlock(uint32_t nameHash) {
    // Stub — requires live PAK reader registry at 0x00A415D4.
    // In viewer mode, LevelReader handles block lookup directly.
    (void)nameHash;
    return NULL;
}

// ------------------------------------------------------------------
// State Machine helpers
//
// The game's main loop is driven by a state machine:
//   MgState (root) -> MgSystemState -> MgShellState
//     -> MgGameLoadState -> MgBaseLevelRunState
//     -> MgClientLevelRunState / MgServerLevelRunState
//     -> MgLevelCompletionState -> MgLevelFlushState
//
// MgGameLoadState tick (FUN_0091728c) runs the 12-step loading:
//   State 3: Format PAK/BIN paths
//   State 4: Open files (CreateFileA, 5 retries)
//   State 5: Read 0x38-byte master header
//   State 6: Parse PAK data header (subsystem offsets)
//   State 7: zlib decompress (64KB double-buffered)
//   State 8: Register PAK reader
//   State 9: Hash "level", FindBlock, parse GameObjs
//   State 10: Entity spawn loop (CreateOnLoad)
//   State 11: Gamemode resolution (3-pass)
//   State 12: Transition to MgBaseLevelRunState
// ------------------------------------------------------------------

#ifdef __cplusplus
} // extern "C"
#endif
