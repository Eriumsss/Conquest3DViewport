// Hash Dictionary for FNV-1a Reverse Lookup
// Generated from 243-task Wwise RE analysis
// Provides pre-computed hash -> string mappings for unknown Wwise IDs
//
// Sources:
// - captured_audio_names.txt (runtime captures)
// - TXTP switch group analysis
// - English.json ability names
// - Wwise naming conventions (Play_, Stop_, Set_, etc.)
//Eriumsss
#pragma once

#include <Windows.h>
#include <unordered_map>
#include <string>

// ============================================================================
// FNV-1a Hash Implementation (matches Wwise GetIDFromString @ 0x00560190)
// ============================================================================
// Wwise uses FNV-1a with:
//   - Offset basis: 0x811c9dc5
//   - Prime: 0x1000193
//   - Case-insensitive (lowercase conversion before hashing)

inline DWORD ComputeFNV1a(const char* str) {
    if (!str) return 0;
    DWORD hash = 0x811c9dc5u;  // FNV-1a offset basis
    while (*str) {
        char c = *str++;
        // Convert to lowercase for case-insensitive matching
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        hash ^= (DWORD)(unsigned char)c;
        hash *= 0x1000193u;  // FNV-1a prime
    }
    return hash;
}

inline DWORD ComputeFNV1aW(const wchar_t* str) {
    if (!str) return 0;
    DWORD hash = 0x811c9dc5u;
    while (*str) {
        wchar_t c = *str++;
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
        hash ^= (DWORD)(c & 0xFF);
        hash *= 0x1000193u;
    }
    return hash;
}

// ============================================================================
// Switch Group IDs (decoded from TXTP analysis)
// ============================================================================
namespace WwiseSwitch {
    // Switch Group IDs
    constexpr DWORD GROUP_ATTACK_TYPE   = 2661483290u;  // AttackType
    constexpr DWORD GROUP_PLAYER_TYPE   = 1728396083u;  // PlayerType
    constexpr DWORD GROUP_WEAPON_TYPE   = 3893417221u;  // WeaponType
    constexpr DWORD GROUP_ABILITY_TYPE  = 1214237073u;  // AbilityType
    constexpr DWORD GROUP_DAMAGE_TYPE   = 3865314626u;  // DamageType
    
    // AttackType values
    constexpr DWORD ATTACK_MELEE        = 595159781u;
    constexpr DWORD ATTACK_RANGED       = 3474813125u;
    constexpr DWORD ATTACK_ABILITY      = 276002327u;
    constexpr DWORD ATTACK_SPECIAL      = 229480994u;
    constexpr DWORD ATTACK_COMBO        = 3887404748u;
    
    // PlayerType values
    constexpr DWORD PLAYER_PLAYER       = 1668749452u;
    constexpr DWORD PLAYER_AI           = 979470758u;
    
    // WeaponType values
    constexpr DWORD WEAPON_SWORD        = 1080479481u;
    constexpr DWORD WEAPON_MACE         = 2454616260u;
    constexpr DWORD WEAPON_AXE          = 2181839183u;
    constexpr DWORD WEAPON_BOW          = 645565787u;
    constexpr DWORD WEAPON_STAFF        = 1007615010u;
    constexpr DWORD WEAPON_SPEAR        = 546945295u;
    constexpr DWORD WEAPON_HAMMER       = 3529103590u;
    
    // DamageType values
    constexpr DWORD DAMAGE_PHYSICAL     = 2451844658u;
    constexpr DWORD DAMAGE_FIRE         = 2058049674u;
    constexpr DWORD DAMAGE_LIGHTNING    = 1216965916u;
    constexpr DWORD DAMAGE_POISON       = 0u;  // TODO: capture this
    
    // Common state: None
    constexpr DWORD STATE_NONE          = 0x2ca33bdbu;
}

// ============================================================================
// Pre-computed Hash Entries
// ============================================================================
struct HashDictEntry {
    DWORD hash;
    const char* name;
    const char* category;  // "switch_group", "switch_value", "event", "bank", "rtpc"
};

// Switch groups and values (from Phase 4 analysis)
static const HashDictEntry g_SwitchDictionary[] = {
    // Switch Groups
    {2661483290u, "AttackType", "switch_group"},
    {1728396083u, "PlayerType", "switch_group"},
    {3893417221u, "WeaponType", "switch_group"},
    {1214237073u, "AbilityType", "switch_group"},
    {3865314626u, "DamageType", "switch_group"},
    
    // AttackType values
    {595159781u, "melee", "switch_value"},
    {3474813125u, "ranged", "switch_value"},
    {276002327u, "ability", "switch_value"},
    {229480994u, "special", "switch_value"},
    {3887404748u, "combo", "switch_value"},
    
    // PlayerType values
    {1668749452u, "player", "switch_value"},
    {979470758u, "AI", "switch_value"},
    
    // WeaponType values
    {1080479481u, "sword", "switch_value"},
    {2454616260u, "mace", "switch_value"},
    {2181839183u, "axe", "switch_value"},
    {645565787u, "bow", "switch_value"},
    {1007615010u, "staff", "switch_value"},
    {546945295u, "spear", "switch_value"},
    {3529103590u, "hammer", "switch_value"},
    
    // DamageType values
    {2451844658u, "physical", "switch_value"},
    {2058049674u, "fire", "switch_value"},
    {1216965916u, "lightning", "switch_value"},
    
    // Common states
    {0x2ca33bdbu, "None", "state_value"},
    
    {0, nullptr, nullptr}  // Terminator
};

// RTPC parameters (from Phase 2 analysis)
static const HashDictEntry g_RTPCDictionary[] = {
    {0xdc4c65b0u, "volume_master", "rtpc"},
    {0xe7f119bbu, "volume_music", "rtpc"},
    {0xdafafc77u, "volume_sfx", "rtpc"},
    {0xf80e9e43u, "volume_vo", "rtpc"},
    {0x0f940ccdu, "battle_size", "rtpc"},
    {0, nullptr, nullptr}
};

// Forward declaration for dictionary builder
void BuildHashDictionary(std::unordered_map<DWORD, std::string>& outDict);

