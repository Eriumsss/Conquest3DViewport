// ================================================================
// MgCRC.h - Dual CRC Hash Functions (from ConquestLLC.exe disassembly)
//
// The game uses TWO distinct CRC functions:
//   1) Runtime string hash (0x0067e699): CRC-32/MPEG-2, non-reflected
//      - Polynomial: 0x04C11DB7 (left-shift)
//      - Case-insensitive via INDEX_ARRAY at 0x00A35A08
//      - Used for: Lua type dispatch, game state names, runtime string lookups
//      - HASHING_ARRAY at 0x00A35608, INDEX_ARRAY at 0x00A35A08
//
//   2) Data integrity CRC (0x006d23e9): Standard reflected CRC32
//      - Polynomial: 0xEDB88320 (right-shift)
//      - No case folding
//      - Used for: binary data checksums (mesh, texture integrity)
//      - CRC table at 0x009BE378
//
// IMPORTANT: PAK binary files store PRE-COMPUTED CRCs as raw uint32.
// The game reads these directly without recomputing (confirmed at 0x007e905c).
// The lotrc Rust parser's hash function is correct for PAK data because it
// matches the BUILD TOOLS (Pandemic's editor) that originally computed the CRCs.
// Neither runtime hash function (1 or 2 above) matches lotrc's hash.
//
// The third hash function (LotrHashString in LevelReader.cpp at 0x007E3F40)
// is Pandemic's custom string hash used by the build tools and lotrc.
// These three are NOT interchangeable.
// ================================================================

#pragma once
#include <stdint.h>
#include <stddef.h>

namespace MgCRC {

// ------------------------------------------------------------------
// Runtime CRC-32/MPEG-2 (address 0x0067e699)
// Used for runtime string dispatch (Lua, game states, event names)
// NOT for PAK binary data
// ------------------------------------------------------------------
uint32_t HashString_Runtime(const char* str);

// With seed for chained hashing
uint32_t HashString_Runtime(const char* str, uint32_t seed);

// Wrapper matching game's FUN_0067e6d8 behavior:
// Returns 0 if str is null, otherwise hashes
inline uint32_t HashString_RuntimeSafe(const char* str) {
    if (!str) return 0;
    return HashString_Runtime(str);
}

// ------------------------------------------------------------------
// Data integrity CRC32 (address 0x006d23e9)
// Standard reflected CRC32 (poly 0xEDB88320)
// Used for binary data checksums, NOT for name hashing
// ------------------------------------------------------------------
uint32_t CRC32_Data(const void* data, size_t length, uint32_t initialCRC = 0xFFFFFFFF);

// ------------------------------------------------------------------
// Key Constants
// ------------------------------------------------------------------

// Hash function lookup tables (game addresses)
static const uint32_t ADDR_HASHING_ARRAY = 0x00A35608;  // 256-entry CRC table (runtime hash)
static const uint32_t ADDR_INDEX_ARRAY   = 0x00A35A08;  // 256-byte case folding table
static const uint32_t ADDR_DATA_CRC_TABLE = 0x009BE378;  // 256-entry reflected CRC table

// Function addresses
static const uint32_t ADDR_HashString    = 0x0067E699;  // Runtime string hash
static const uint32_t ADDR_HashWrapper   = 0x0067E6D8;  // Safe wrapper (null check + store)
static const uint32_t ADDR_CRC32_Data    = 0x006D23E9;  // Data integrity CRC

// GameObjs magic number
static const uint32_t GAMEOBJS_MAGIC = 0x4D414704;  // "MAG\x04" at offset 0 of GameObjs block

} // namespace MgCRC