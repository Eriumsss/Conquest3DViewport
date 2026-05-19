// ================================================================
// MgLevelLoader.h - Level Loading Pipeline
// Reconstructed from ConquestLLC.exe disassembly
//
// Complete chain from user selecting a level to gameplay beginning:
//
//   1. UI: MgGameLoadState formats "levels\mapname.pak" / ".bin"
//   2. FUN_008a10cc: Store paths, push LOAD_LEVEL event (type 0x0A)
//   3. FUN_00679f0e: CreateFileA with 5 retries on failure
//   4. FUN_007437a1: Read 0x38-byte master header (version=4)
//   5. FUN_008787e3: Parse PAK data header (subsystem offsets)
//   6. FUN_00876c99: zlib decompress (64KB double-buffered streaming)
//   7. FUN_0067a89e: Register PAK reader in global registry
//   8. FUN_00917ae6: Hash "level", FindBlock, get GameObjs data
//   9. FUN_007e8ecb: Parse GameObjs (magic 0x4D414704)
//      a. Register 14 PBL primitive types (one-time via 0x007e86d1)
//      b. Parse type table: iterate types, hash into lookup map
//      c. Parse entity table: iterate entities, resolve type CRCs
//      d. Sort entities by type CRC (insertion sort at 0x007e91e0)
//      e. GUID resolution (0x007e8bf8): MgHandle/MgHandleArray/MgOutputArray
//  10. FUN_007e78b7: Entity spawn loop
//      a. Check CreateOnLoad field (CRC at 0x9D84EC)
//      b. Check QualityCategory against global setting
//      c. Call EntityFactory (0x007e76d4) for each passing entity
//  11. FUN_00916d9f: Gamemode resolution (3-pass matching)
//  12. Gameplay: MgClientLevelRunState + MgServerLevelRunState
//
// State Machine:
//   MgState (root at 0xcd9808)
//   -> MgSystemState (engine init)
//   -> MgShellState -> MgShellRunState (menu)
//   -> MgGameLoadState (loading screen at 0xcd98d0)
//   -> MgBaseLevelRunState (level active at 0xcd9850)
//   -> MgClientLevelRunState (client tick at 0xcd9898)
//   -> MgServerLevelRunState (server tick at 0xcd9910)
//   -> MgLevelCompletionState (end at 0xcd98dc)
//   -> MgLevelFlushState (cleanup at 0xcd985c)
//
// Sources:
//   FUN_0091728c — MgGameLoadState tick (state machine with states 3-12)
//   FUN_008a10cc — StartLevelLoad (store paths, push event)
//   FUN_00679f0e — FileOpen (Win32 CreateFileA, 5 retries)
//   FUN_007437a1 — ReadLevelData (0x38-byte header, version=4)
//   FUN_008787e3 — InitLevelFromPakData (subsystem offsets)
//   FUN_00876c99 — DecompressBlock (zlib 64KB streaming)
//   FUN_0067a89e — RegisterPakReader
//   FUN_0067a957 — FindBlock (iterate readers, search by name hash)
//   FUN_00917ae6 — InitLevel (hash "level", find block, parse GameObjs)
//   FUN_007e8ecb — ParseGameObjsBlock (magic check, types, entities)
//   FUN_007e8bf8 — GUIDResolution (MgHandle, MgHandleArray, MgOutputArray)
//   FUN_007e78b7 — EntitySpawnLoop (CreateOnLoad check)
//   FUN_00916d9f — GamemodeResolution (3-pass matching)
// ================================================================

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// GameObjs Binary Layout (confirmed from 0x007e8ecb)
// ------------------------------------------------------------------
// Header (32 bytes):
//   [+0x00] magic       uint32 = 0x4D414704
//   [+0x04] numTypes    uint32
//   [+0x08] typeOffset  uint32 (byte offset to type table)
//   [+0x0C] numEntities uint32
//   [+0x10] entityOffset uint32 (byte offset to entity data)
//   [+0x14] reserved[3] uint32
//
// Type Entry:
//   [+0x00] typeCRC      uint32 (pre-computed CRC of type name)
//   [+0x04] fieldCount   uint16 (number of fields)
//   [+0x06] padding      uint16
//   Followed by fieldCount * FieldEntry:
//     [+0x00] fieldNameCRC  uint32
//     [+0x04] fieldTypeCRC  uint32
//     [+0x08] dataOffset    uint32
//
// Entity Entry:
//   [+0x00] layerGUID  uint32
//   [+0x04] typeCRC    uint32
//   [+0x08] dataSize   uint16 (size of following data blob)
//   [+0x0A] flags      uint16
//   Followed by dataSize bytes of field data

#define MG_GAMEOBJS_MAGIC 0x4D414704

typedef struct {
    uint32_t magic;         // 0x4D414704
    uint32_t numTypes;
    uint32_t typeOffset;
    uint32_t numEntities;
    uint32_t entityOffset;
    uint32_t reserved[3];
} MgGameObjsHeader;

// ------------------------------------------------------------------
// PAK Data Header Subsystem Offsets (from FUN_008787e3)
// At [EBX+0x114] through [EBX+0x168] in the level data struct
// ------------------------------------------------------------------
typedef struct {
    // Counts (at EBX+0xB0 through EBX+0x104)
    uint32_t meshCountA;      // +0xB0 (stride 0x100)
    uint32_t materialCount;   // +0xB4 (stride 0x164)
    uint32_t meshCountB;      // +0xB8 (stride 0x168)
    uint32_t objectCountA;    // +0xBC (stride 0x1E8)
    uint32_t objectCountB;    // +0xC0 (stride 0x1D0)
    uint32_t zoneCount;       // +0xC4 (stride 0x248)
    uint32_t _pad1[3];
    uint32_t collisionCount;  // +0xD0 (stride 0x7C)
    uint32_t _pad2;
    uint32_t physicsCount;    // +0xD8 (stride 0x74)

    // Offsets into loaded data (at EBX+0x114 through EBX+0x168)
    // Set during FUN_008787e3 after data is loaded
} MgPakDataHeader;

// ------------------------------------------------------------------
// Level Master Header (0x38 bytes, from FUN_007437a1)
// Read at the very start of level file loading
// ------------------------------------------------------------------
typedef struct {
    uint32_t version;         // +0x00 must be 4
    // Remaining fields vary, key ones:
    // +0x28: data block size
    // +0x30: string table size
    // +0x38: entity group count
} MgLevelMasterHeader;

// ------------------------------------------------------------------
// File Open Parameters (from FUN_00679f0e)
// ------------------------------------------------------------------
#define MG_FILE_MODE_READ    0
#define MG_FILE_MODE_WRITE   1
#define MG_FILE_MODE_READWRITE 2
#define MG_FILE_RETRY_COUNT  5  // CreateFileA retries on failure

// ------------------------------------------------------------------
// Zlib Decompression (from FUN_00876c99)
// ------------------------------------------------------------------
#define MG_ZLIB_CHUNK_SIZE     0x10000  // 64KB per chunk
#define MG_ZLIB_BUFFER_A       0xE68050 // First decompression buffer
#define MG_ZLIB_BUFFER_B       0xE78050 // Second buffer (double-buffered)

// ------------------------------------------------------------------
// PAK Reader Registry (from FUN_0067a89e)
// ------------------------------------------------------------------
#define MG_ADDR_PakReaderCount  0x00A415D2  // uint16: registered PAK reader count
#define MG_ADDR_PakReaderArray  0x00A415D4  // pointer to array of PAK reader objects

// ------------------------------------------------------------------
// Level Loading Globals
// ------------------------------------------------------------------
#define MG_ADDR_PakPath      0x00CD8310  // char[260]: current PAK file path
#define MG_ADDR_BinPath      0x00CD8414  // char[260]: current BIN file path
#define MG_ADDR_LoadState    0x00CD830C  // uint32: 0x1C=load, 0x1D=unload
#define MG_ADDR_LevelManager 0x00CD7F88  // pointer to level manager instance

// ------------------------------------------------------------------
// State Machine Addresses (from static registration)
// ------------------------------------------------------------------
#define MG_ADDR_StateRoot           0x00CD9808  // MgState (root)
#define MG_ADDR_StateBaseLevelRun   0x00CD9850  // MgBaseLevelRunState
#define MG_ADDR_StateLevelFlush     0x00CD985C  // MgLevelFlushState
#define MG_ADDR_StateClientLevelRun 0x00CD9898  // MgClientLevelRunState
#define MG_ADDR_StateGameLoad       0x00CD98D0  // MgGameLoadState
#define MG_ADDR_StateLevelComplete  0x00CD98DC  // MgLevelCompletionState
#define MG_ADDR_StateServerLevelRun 0x00CD9910  // MgServerLevelRunState

// ------------------------------------------------------------------
// Gamemode Resolution (from FUN_00916d9f, 493 bytes)
//
// Three-pass matching to find the active gamemode entity:
//   Pass 1: Match by GameModeID hash (entity hash vs [EBX+0x30])
//   Pass 2: Match by GameModeType CRC (entity CRC vs [EBX+0x2c])
//   Pass 3: Match by entity name string (hash name, compare to [EBX+0x34])
//
// Stores result at [level_manager + 0x260]
// ------------------------------------------------------------------
#define MG_GAMEMODE_MATCH_OFFSET 0x260  // offset in level manager for active gamemode

// String addresses used by level loading
#define MG_STR_Level            0x009C79C0  // "level"
#define MG_STR_Gamemode         0x009C6F48  // "gamemode"
#define MG_STR_LevelsFormat     0x009C79A0  // "levels\%s"
#define MG_STR_PakFormat        0x009C7990  // "%s.pak"
#define MG_STR_BinFormat        0x009C7998  // "%s.bin"
#define MG_STR_ShellPak         0x009C70A0  // "levels\shell.pak"
#define MG_STR_ShellBin         0x009C70B4  // "levels\shell.bin"
#define MG_STR_LevelInfoDat     0x009C7DE8  // "levels/level_info.dat"

// ------------------------------------------------------------------
// Function Addresses
// ------------------------------------------------------------------
#define MG_ADDR_GameLoadStateTick    0x0091728C  // MgGameLoadState tick
#define MG_ADDR_StartLevelLoad       0x008A10CC  // Store paths, push event
#define MG_ADDR_FileOpen             0x00679F0E  // Win32 CreateFileA wrapper
#define MG_ADDR_ReadLevelData        0x007437A1  // 0x38-byte header read
#define MG_ADDR_InitFromPakData      0x008787E3  // Parse PAK subsystem offsets
#define MG_ADDR_DecompressBlock      0x00876C99  // zlib decompression
#define MG_ADDR_RegisterPakReader    0x0067A89E  // Register PAK reader
#define MG_ADDR_FindBlock            0x0067A957  // Find block by name hash
#define MG_ADDR_InitLevel            0x00917AE6  // Main level init (1348 bytes)
#define MG_ADDR_ParseGameObjs        0x007E8ECB  // GameObjs parser (918 bytes)
#define MG_ADDR_GUIDResolution       0x007E8BF8  // GUID -> Handle resolution (561 bytes)
#define MG_ADDR_EntitySpawnLoop      0x007E78B7  // CreateOnLoad entity loop (228 bytes)
#define MG_ADDR_GamemodeResolution   0x00916D9F  // 3-pass gamemode finder (493 bytes)

#ifdef __cplusplus
} // extern "C"
#endif