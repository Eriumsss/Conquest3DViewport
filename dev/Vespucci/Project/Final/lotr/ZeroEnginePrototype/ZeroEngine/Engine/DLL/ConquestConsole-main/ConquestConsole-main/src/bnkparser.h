#pragma once
#include <Windows.h>
#include <vector>
#include <string>
//Eriumsss
// ============================================================================
// BNK Parser - Parse Wwise v34 soundbank data in memory
// 
// BNK Structure (little endian):
//   BKHD - Bank Header: version, bankId
//   DIDX - Data Index: list of embedded WEM files
//   DATA - Raw WEM audio data
//   HIRC - Hierarchy: events, sounds, actions, containers
//   STID - String Mappings: bank ID -> name
// ============================================================================

namespace BnkParser {

// Wwise HIRC object types (v34 - Wwise 2008.1)
enum HircType : BYTE {
    HIRC_STATE          = 0x01,
    HIRC_SOUND          = 0x02,
    HIRC_ACTION         = 0x03,
    HIRC_EVENT          = 0x04,
    HIRC_RANSEQ_CNTR    = 0x05,  // Random/Sequence Container
    HIRC_SWITCH_CNTR    = 0x06,
    HIRC_ACTOR_MIXER    = 0x07,
    HIRC_BUS            = 0x08,
    HIRC_LAYER_CNTR     = 0x09,
    HIRC_MUSIC_SEGMENT  = 0x0A,
    HIRC_MUSIC_TRACK    = 0x0B,
    HIRC_MUSIC_SWITCH   = 0x0C,
    HIRC_MUSIC_RANSEQ   = 0x0D,
    HIRC_ATTENUATION    = 0x0E,
    HIRC_DIALOGUE_EVENT = 0x0F,
};

// Parsed embedded WEM info (from DIDX)
struct WemInfo {
    DWORD id;       // WEM file ID (FNV hash)
    DWORD offset;   // Offset in DATA section
    DWORD size;     // Size in bytes
};

// Parsed HIRC event info
struct EventInfo {
    DWORD id;           // Event ID
    BYTE actionCount;   // Number of actions
    std::vector<DWORD> actionIds;  // Action IDs
};

// Parsed HIRC sound info
struct SoundInfo {
    DWORD id;       // Sound object ID
    DWORD sourceId; // Source WEM ID (embedded or streamed)
    bool isStreamed;
    BYTE codec;     // 0x02=ADPCM, 0x04=VORBIS
};

// Parsed bank info
struct BankInfo {
    DWORD bankId;           // Bank ID (FNV hash)
    DWORD version;          // Wwise version (34 for Conquest)
    std::string bankName;   // From STID section
    
    std::vector<WemInfo> wems;      // Embedded WEM files
    std::vector<EventInfo> events;  // CAkEvent objects
    std::vector<SoundInfo> sounds;  // CAkSound objects
    
    DWORD hircObjectCount;  // Total HIRC objects
    bool isValid;           // Successfully parsed
};

// Parse a BNK file from memory
// Returns true if parsing succeeded (at least BKHD was valid)
bool ParseBankFromMemory(const BYTE* data, DWORD size, BankInfo& outInfo);

// Parse just the bank ID from BKHD (minimal parsing)
DWORD GetBankIdFromMemory(const BYTE* data, DWORD size);

// Parse just the bank name from STID section
bool GetBankNameFromMemory(const BYTE* data, DWORD size, char* outName, int maxLen);

} // namespace BnkParser

