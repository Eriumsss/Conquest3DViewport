#include "audiohook.h"
#include "event_mapping.h"
#include "hash_dictionary.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>

//
// Windows multimedia includes
//
// We use the Win32 Multimedia API to play simple sound files
// from disk when a matching event name is found.  PlaySoundA
// supports .wav files out of the box, while the MCI interface
// can handle other formats such as .mp3 if the appropriate
// codecs are installed on the target system.  Linking against
// winmm.lib provides the necessary implementations for
// PlaySoundA and mciSendStringA.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
//Eriumsss
// ============================================================================
// Audio Hook Implementation
// Hooks Wwise PostEvent to log all audio events
// ============================================================================

namespace AudioHook {

// Game addresses (from 243-task RE analysis)
static const DWORD ADDR_POST_EVENT_BY_ID = 0x00561360;
static const DWORD ADDR_GET_ID_FROM_STRING = 0x00560190;
static const DWORD ADDR_SET_SWITCH_BY_ID = 0x0055eef0;   // SetSwitch(groupId, valueId, gameObjId)
static const DWORD ADDR_SET_STATE_BY_ID = 0x0055ffd0;    // SetState(groupId, valueId)
static const DWORD ADDR_STOP_ALL = 0x0055fca0;           // StopAll(gameObjId)
static const DWORD ADDR_AUDIO_ENABLED = 0x00a3e851;

// Original function pointers
typedef DWORD(__cdecl* PostEventByID_t)(DWORD eventId, DWORD gameObjId, DWORD flags, void* callback, void* cookie);
typedef DWORD(__cdecl* GetIDFromString_t)(const wchar_t* eventName);
typedef DWORD(__cdecl* SetSwitchByID_t)(DWORD groupId, DWORD valueId, DWORD gameObjId);
typedef DWORD(__cdecl* SetStateByID_t)(DWORD groupId, DWORD valueId);
typedef void(__cdecl* StopAll_t)(DWORD gameObjId);

// Hook trampoline bytes storage
static BYTE g_OriginalBytesPostEvent[5] = {0};
static BYTE g_OriginalBytesGetID[5] = {0};
static BYTE g_OriginalBytesSetSwitch[5] = {0};
static BYTE g_OriginalBytesSetState[5] = {0};
static bool g_HookPostEventInstalled = false;
static bool g_HookGetIDInstalled = false;
static bool g_HookSetSwitchInstalled = false;
static bool g_HookSetStateInstalled = false;

// Hash dictionary for resolving unknown IDs
static std::unordered_map<DWORD, std::string> g_HashDictionary;
static int g_DictionaryResolvedCount = 0;

// Switch/State change tracking
static AudioHook::SwitchStateChange g_SwitchChanges[AudioHook::MAX_SWITCH_CHANGES] = {0};
static int g_SwitchChangeIndex = 0;
static int g_TotalSwitchChanges = 0;

// Event logging
static bool g_LoggingEnabled = true;
static bool g_FilterNoisy = true;  // Filter out footsteps and other noisy events
static DWORD g_TotalEventCount = 0;
static DWORD g_FilteredEventCount = 0;  // Count of filtered events
static DWORD g_UnknownEventCount = 0;   // Count of uncracked/unknown events played
static AudioEvent g_Events[MAX_EVENTS] = {0};
static int g_EventIndex = 0;
static DWORD g_SessionStartTime = 0;

// Track last valid game object ID for manual playback
// Wwise uses AK_INVALID_GAME_OBJECT = (DWORD)-1 as invalid
// Game object 0 is often the "global" or "transport" object in some versions
static DWORD g_LastValidGameObjectId = 0;

// Full event log for export (stores all events, not just circular buffer)
struct EventLogEntry {
    DWORD timestamp;      // Relative to session start (ms)
    DWORD eventId;
    char eventName[64];
    char bankName[32];
};
static std::vector<EventLogEntry> g_EventLog;
// No limit on event log - grows as needed

// Noisy event patterns to filter (when g_FilterNoisy is true)
// Uses case-insensitive substring matching
static const char* g_NoisyPatterns[] = {
    "footstep",       // JSON event name
    "Effects-0717",   // TXTP name for footsteps
    "Creatures-0451", // Creature ambient sounds (very frequent)
    "Creatures-0453", // Creature ambient sounds (very frequent)
    nullptr
};

// Case-insensitive substring search
static bool ContainsPattern(const char* str, const char* pattern) {
    if (!str || !pattern || !*str || !*pattern) return false;

    size_t strLen = strlen(str);
    size_t patLen = strlen(pattern);
    if (patLen > strLen) return false;

    for (size_t i = 0; i <= strLen - patLen; i++) {
        bool match = true;
        for (size_t j = 0; j < patLen; j++) {
            char c1 = str[i + j];
            char c2 = pattern[j];
            // Convert to lowercase for comparison
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 != c2) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// Check if an event should be filtered
static bool ShouldFilterEvent(const char* primaryName, const char* secondaryName) {
    if (!g_FilterNoisy) return false;

    // Check both primary (TXTP name) and secondary (JSON name) for noisy patterns
    for (int i = 0; g_NoisyPatterns[i] != nullptr; i++) {
        if (ContainsPattern(primaryName, g_NoisyPatterns[i])) {
            return true;
        }
        if (ContainsPattern(secondaryName, g_NoisyPatterns[i])) {
            return true;
        }
    }
    return false;
}

// Event name lookup tables
static std::unordered_map<DWORD, std::string> g_EventNames;        // From JSON (val -> readable key)
static std::unordered_map<DWORD, std::string> g_CapturedNames;     // Runtime captured (hash -> string)
static std::unordered_map<DWORD, std::string> g_EventBankMapping;  // Event ID -> Display label (from TXTP)
static std::unordered_map<DWORD, std::string> g_EventSourceBank;   // Event ID -> Source bank name (for filtering)
static int g_CapturedCount = 0;
static int g_EventBankMappingCount = 0;
static bool g_FilterByLoadedBanks = true;  // Only show events from loaded banks

// Active bank tracking - permanent list of all banks loaded this session
static std::vector<std::string> g_LoadedBanks;  // Permanent list of all loaded banks

// Forward declaration
static bool IsEventBankLoaded(DWORD eventId);
static const int MAX_RECENT_BANKS = 8;  // For recent display
struct RecentBank {
    char name[48];
    DWORD hash;
    DWORD timestamp;
};
static RecentBank g_RecentBanks[MAX_RECENT_BANKS] = {0};
static int g_RecentBankIndex = 0;
static char g_LastActiveBank[48] = {0};  // Most recently used bank for display

// FNV-1 hash (32-bit) - matches Wwise's hashing
static DWORD FNV1Hash(const wchar_t* str) {
    if (!str) return 0;
    DWORD hash = 2166136261u;  // FNV offset basis
    while (*str) {
        // Convert wchar to lowercase for case-insensitive matching
        wchar_t c = *str++;
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
        hash *= 16777619u;     // FNV prime
        hash ^= (DWORD)(c & 0xFF);
    }
    return hash;
}

// Bank name inference table (from 91 STID names)
static const char* InferBankName(const char* eventName) {
    if (!eventName) return nullptr;

    // Character-specific events
    if (strstr(eventName, "gandalf")) return "HeroGandalf";
    if (strstr(eventName, "aragorn")) return "HeroAragorn";
    if (strstr(eventName, "legolas")) return "HeroLegolas";
    if (strstr(eventName, "gimli")) return "HeroGimli";
    if (strstr(eventName, "frodo")) return "HeroFrodo";
    if (strstr(eventName, "eowyn")) return "HeroEowyn";
    if (strstr(eventName, "theoden")) return "HeroTheoden";
    if (strstr(eventName, "faramir")) return "HeroFaramir";
    if (strstr(eventName, "elrond")) return "HeroElrond";
    if (strstr(eventName, "isildur")) return "HeroIsildur";
    if (strstr(eventName, "sauron")) return "HeroSauron";
    if (strstr(eventName, "saruman")) return "HeroSaruman";
    if (strstr(eventName, "nazgul")) return "HeroNazgul";
    if (strstr(eventName, "gothmog")) return "HeroGothmog";
    if (strstr(eventName, "lurtz")) return "HeroLurtz";
    if (strstr(eventName, "witchking")) return "HeroWitchKing";
    if (strstr(eventName, "wormtongue")) return "HeroWormtongue";
    if (strstr(eventName, "treebeard")) return "HeroTreebeard";

    // Creatures/Vehicles
    if (strstr(eventName, "Balrog") || strstr(eventName, "balrog")) return "SFXBalrog";
    if (strstr(eventName, "Troll") || strstr(eventName, "troll")) return "SFXTroll";
    if (strstr(eventName, "Warg") || strstr(eventName, "warg")) return "SFXWarg";
    if (strstr(eventName, "Horse") || strstr(eventName, "horse")) return "SFXHorse";
    if (strstr(eventName, "Eagle") || strstr(eventName, "eagle")) return "SFXEagle";
    if (strstr(eventName, "Oliphaunt") || strstr(eventName, "oliphaunt")) return "SFXOliphant";
    if (strstr(eventName, "Catapult") || strstr(eventName, "catapult")) return "SFXCatapult";
    if (strstr(eventName, "Ballista") || strstr(eventName, "ballista")) return "SFXBallista";

    // UI events
    if (strncmp(eventName, "ui_", 3) == 0) return "UI";
    if (strcmp(eventName, "UI") == 0) return "UI";

    // Music
    if (strstr(eventName, "Music") || strstr(eventName, "music") ||
        strstr(eventName, "mus_")) return "Music";

    // Combat
    if (strstr(eventName, "swing") || strstr(eventName, "impact") ||
        strstr(eventName, "Block") || strstr(eventName, "punch") ||
        strstr(eventName, "kick") || strstr(eventName, "attack")) return "BaseCombat";

    // Voice/Chatter
    if (strstr(eventName, "VO") || strstr(eventName, "taunt") ||
        strstr(eventName, "cheer") || strstr(eventName, "vocal")) return "VoiceOver";

    // Game states
    if (strstr(eventName, "pause") || strstr(eventName, "cp_")) return "Effects";

    // Default categories
    if (strstr(eventName, "footstep")) return "Effects";
    if (strstr(eventName, "amb") || strstr(eventName, "shell")) return "Ambience";

    return nullptr;
}

// Forward declarations
static DWORD __cdecl HookedPostEvent(DWORD eventId, DWORD gameObjId, DWORD flags, void* callback, void* cookie);
static DWORD __cdecl HookedGetIDFromString(const wchar_t* eventName);
static DWORD __cdecl HookedSetSwitch(DWORD groupId, DWORD valueId, DWORD gameObjId);
static DWORD __cdecl HookedSetState(DWORD groupId, DWORD valueId);
static void LoadEventNames();
static void LoadHashDictionary();
static bool InstallHooks();
static void RemoveHooks();

// Custom audio helpers (forward declarations)
static bool FileExists(const std::string& filepath);
static void PlayCustomAudio(const std::string& baseName);
static void LoadAudioOverrides();
static void InitCustomAudio();

// Helper to resolve hash using dictionary
static const char* ResolveHash(DWORD hash) {
    auto it = g_HashDictionary.find(hash);
    if (it != g_HashDictionary.end()) {
        return it->second.c_str();
    }
    return nullptr;
}

// ============================================================================
// Hooked GetIDFromString - captures string names before hashing
// ============================================================================
static DWORD __cdecl HookedGetIDFromString(const wchar_t* eventName) {
    DWORD hash = 0;

    // Temporarily remove hook to call original
    if (g_HookGetIDInstalled) {
        DWORD oldProtect;
        VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)ADDR_GET_ID_FROM_STRING, g_OriginalBytesGetID, 5);
        VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, oldProtect, &oldProtect);

        // Call original to get the real hash
        hash = ((GetIDFromString_t)ADDR_GET_ID_FROM_STRING)(eventName);

        // Reinstall hook
        VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        BYTE jmp[5];
        jmp[0] = 0xE9;
        DWORD relAddr = (DWORD)&HookedGetIDFromString - ADDR_GET_ID_FROM_STRING - 5;
        memcpy(&jmp[1], &relAddr, 4);
        memcpy((void*)ADDR_GET_ID_FROM_STRING, jmp, 5);
        VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, oldProtect, &oldProtect);
    }

    // Capture the string name and store mapping
    if (eventName && g_LoggingEnabled) {
        // Convert wchar_t to char for storage
        char narrowName[64] = {0};
        int len = 0;
        for (int i = 0; i < 63 && eventName[i]; i++) {
            narrowName[i] = (char)(eventName[i] & 0xFF);
            len++;
        }
        narrowName[len] = '\0';

        // Store if we don't already have this hash
        if (g_CapturedNames.find(hash) == g_CapturedNames.end()) {
            g_CapturedNames[hash] = narrowName;
            g_CapturedCount++;

            // Debug output
            char buf[128];
            sprintf_s(buf, "[AudioHook] Captured: %s -> 0x%08X\n", narrowName, hash);
            OutputDebugStringA(buf);
        }

        // Track as active bank if it looks like a bank/category name
        // (Hero*, Chatter*, Level_*, SFX*, etc.)
        bool isBank = (strstr(narrowName, "Hero") == narrowName) ||
                      (strstr(narrowName, "Chatter") == narrowName) ||
                      (strstr(narrowName, "Level_") == narrowName) ||
                      (strstr(narrowName, "SFX") == narrowName) ||
                      (strstr(narrowName, "Music") == narrowName) ||
                      (strstr(narrowName, "VO") == narrowName) ||
                      (strstr(narrowName, "Ambience") == narrowName);

        if (isBank) {
            // Add to recent banks (circular buffer for display)
            RecentBank& bank = g_RecentBanks[g_RecentBankIndex];
            strncpy_s(bank.name, narrowName, 47);
            bank.hash = hash;
            bank.timestamp = GetTickCount();
            g_RecentBankIndex = (g_RecentBankIndex + 1) % MAX_RECENT_BANKS;

            // Update last active bank for display
            strncpy_s(g_LastActiveBank, narrowName, 47);

            // Add to permanent loaded banks list (if not already present)
            std::string bankName(narrowName);
            bool found = false;
            for (const auto& b : g_LoadedBanks) {
                if (b == bankName) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                g_LoadedBanks.push_back(bankName);
            }
        }
    }

    return hash;
}

// ============================================================================
// Hooked SetSwitch - captures switch group/value changes
// ============================================================================
static DWORD __cdecl HookedSetSwitch(DWORD groupId, DWORD valueId, DWORD gameObjId) {
    DWORD result = 0;

    // Temporarily remove hook to call original
    if (g_HookSetSwitchInstalled) {
        DWORD oldProtect;
        VirtualProtect((void*)ADDR_SET_SWITCH_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)ADDR_SET_SWITCH_BY_ID, g_OriginalBytesSetSwitch, 5);
        VirtualProtect((void*)ADDR_SET_SWITCH_BY_ID, 5, oldProtect, &oldProtect);

        result = ((SetSwitchByID_t)ADDR_SET_SWITCH_BY_ID)(groupId, valueId, gameObjId);

        // Reinstall hook
        VirtualProtect((void*)ADDR_SET_SWITCH_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        BYTE jmp[5];
        jmp[0] = 0xE9;
        DWORD relAddr = (DWORD)&HookedSetSwitch - ADDR_SET_SWITCH_BY_ID - 5;
        memcpy(&jmp[1], &relAddr, 4);
        memcpy((void*)ADDR_SET_SWITCH_BY_ID, jmp, 5);
        VirtualProtect((void*)ADDR_SET_SWITCH_BY_ID, 5, oldProtect, &oldProtect);
    }

    // Log the switch change
    if (g_LoggingEnabled) {
        AudioHook::SwitchStateChange& change = g_SwitchChanges[g_SwitchChangeIndex];
        change.timestamp = GetTickCount();
        change.groupId = groupId;
        change.valueId = valueId;
        change.gameObjId = gameObjId;
        change.isSwitch = true;

        // Try to resolve names from dictionary
        const char* groupName = ResolveHash(groupId);
        const char* valueName = ResolveHash(valueId);

        if (groupName) {
            strncpy_s(change.groupName, groupName, 31);
        } else {
            sprintf_s(change.groupName, "0x%08X", groupId);
        }

        if (valueName) {
            strncpy_s(change.valueName, valueName, 31);
        } else {
            sprintf_s(change.valueName, "0x%08X", valueId);
        }

        g_SwitchChangeIndex = (g_SwitchChangeIndex + 1) % AudioHook::MAX_SWITCH_CHANGES;
        g_TotalSwitchChanges++;

        // Debug output
        char buf[128];
        sprintf_s(buf, "[AudioHook] SetSwitch: %s = %s (obj: 0x%X)\n",
                  change.groupName, change.valueName, gameObjId);
        OutputDebugStringA(buf);
    }

    return result;
}

// ============================================================================
// Hooked SetState - captures state group/value changes
// ============================================================================
static DWORD __cdecl HookedSetState(DWORD groupId, DWORD valueId) {
    DWORD result = 0;

    // Temporarily remove hook to call original
    if (g_HookSetStateInstalled) {
        DWORD oldProtect;
        VirtualProtect((void*)ADDR_SET_STATE_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)ADDR_SET_STATE_BY_ID, g_OriginalBytesSetState, 5);
        VirtualProtect((void*)ADDR_SET_STATE_BY_ID, 5, oldProtect, &oldProtect);

        result = ((SetStateByID_t)ADDR_SET_STATE_BY_ID)(groupId, valueId);

        // Reinstall hook
        VirtualProtect((void*)ADDR_SET_STATE_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        BYTE jmp[5];
        jmp[0] = 0xE9;
        DWORD relAddr = (DWORD)&HookedSetState - ADDR_SET_STATE_BY_ID - 5;
        memcpy(&jmp[1], &relAddr, 4);
        memcpy((void*)ADDR_SET_STATE_BY_ID, jmp, 5);
        VirtualProtect((void*)ADDR_SET_STATE_BY_ID, 5, oldProtect, &oldProtect);
    }

    // Log the state change
    if (g_LoggingEnabled) {
        AudioHook::SwitchStateChange& change = g_SwitchChanges[g_SwitchChangeIndex];
        change.timestamp = GetTickCount();
        change.groupId = groupId;
        change.valueId = valueId;
        change.gameObjId = 0;  // SetState has no game object
        change.isSwitch = false;

        // Try to resolve names from dictionary
        const char* groupName = ResolveHash(groupId);
        const char* valueName = ResolveHash(valueId);

        if (groupName) {
            strncpy_s(change.groupName, groupName, 31);
        } else {
            sprintf_s(change.groupName, "0x%08X", groupId);
        }

        if (valueName) {
            strncpy_s(change.valueName, valueName, 31);
        } else {
            sprintf_s(change.valueName, "0x%08X", valueId);
        }

        g_SwitchChangeIndex = (g_SwitchChangeIndex + 1) % AudioHook::MAX_SWITCH_CHANGES;
        g_TotalSwitchChanges++;

        // Debug output
        char buf[128];
        sprintf_s(buf, "[AudioHook] SetState: %s = %s\n",
                  change.groupName, change.valueName);
        OutputDebugStringA(buf);
    }

    return result;
}

// ============================================================================
// Hooked PostEvent function
// ============================================================================
static DWORD __cdecl HookedPostEvent(DWORD eventId, DWORD gameObjId, DWORD flags, void* callback, void* cookie) {
    DWORD playingId = 0;

    // Track valid game object IDs for manual playback
    // Skip invalid IDs (0xFFFFFFFF is AK_INVALID_GAME_OBJECT)
    if (gameObjId != 0xFFFFFFFF && gameObjId != 0) {
        g_LastValidGameObjectId = gameObjId;
    }

    // ---------------------------------------------------------------------
    // Custom audio override
    // ---------------------------------------------------------------------
    // Before calling the original Wwise PostEvent, check if a corresponding
    // custom audio file exists under the ConquestMods/audio directory.  If
    // found, play the custom sound instead and skip the original event.
    const char* customEventName = LookupEventName(eventId);
    if (customEventName && customEventName[0] != '\0') {
        std::string basePath = "Audio/mods";
        basePath += customEventName;
        // Check for any supported extension.  We call FileExists here to
        // avoid unnecessary playback attempts.  If a file is found,
        // PlayCustomAudio will search again and play the appropriate file.
        if (FileExists(basePath + ".wav") || FileExists(basePath + ".mp3") || FileExists(basePath + ".ogg")) {
            PlayCustomAudio(basePath);
            return 0;  // Skip original Wwise playback
        }
    }

    // Temporarily remove hook to call original
    if (g_HookPostEventInstalled) {
        DWORD oldProtect;
        VirtualProtect((void*)ADDR_POST_EVENT_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)ADDR_POST_EVENT_BY_ID, g_OriginalBytesPostEvent, 5);
        VirtualProtect((void*)ADDR_POST_EVENT_BY_ID, 5, oldProtect, &oldProtect);

        // Call original
        playingId = ((PostEventByID_t)ADDR_POST_EVENT_BY_ID)(eventId, gameObjId, flags, callback, cookie);

        // Reinstall hook
        VirtualProtect((void*)ADDR_POST_EVENT_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        BYTE jmp[5];
        jmp[0] = 0xE9;
        DWORD relAddr = (DWORD)&HookedPostEvent - ADDR_POST_EVENT_BY_ID - 5;
        memcpy(&jmp[1], &relAddr, 4);
        memcpy((void*)ADDR_POST_EVENT_BY_ID, jmp, 5);
        VirtualProtect((void*)ADDR_POST_EVENT_BY_ID, 5, oldProtect, &oldProtect);
    }

    // Log the event if logging is enabled
    if (g_LoggingEnabled) {
        // Look up display label from compiled mapping
        // This now contains either:
        //   - Resolved name: "swing", "creature_attack", "music_01"
        //   - Bank prefix if unknown: "Effects::effect_01"
        //   - Unknown: not in mapping (fall back to raw hex)
        //
        // ALWAYS show the mapped name - all 577 events now have semantic names.
        const char* displayLabel = nullptr;
        auto bankIt = g_EventBankMapping.find(eventId);
        if (bankIt != g_EventBankMapping.end()) {
            displayLabel = bankIt->second.c_str();
        }

        // Also check runtime captured names and JSON for additional info
        const char* capturedName = nullptr;
        auto capturedIt = g_CapturedNames.find(eventId);
        if (capturedIt != g_CapturedNames.end()) {
            capturedName = capturedIt->second.c_str();
        }
        if (!capturedName) {
            capturedName = LookupEventName(eventId);
        }

        // Use display label as primary, captured name as additional info
        const char* primaryName = displayLabel ? displayLabel : nullptr;
        const char* secondaryName = capturedName;

        // Check if this event should be filtered (for display, not for export)
        bool shouldFilter = ShouldFilterEvent(primaryName ? primaryName : "", secondaryName ? secondaryName : "");

        // Check if this is an uncracked/unknown event
        // Unknown events have patterns like "BankName-NNNN" or "BankName::0xHASH" or just hex ID
        bool isUnknown = false;
        if (primaryName) {
            const char* dash = strstr(primaryName, "-0");
            const char* hex = strstr(primaryName, "0x");
            if ((dash && dash[1] == '0' && dash[2] >= '0' && dash[2] <= '9') || hex) {
                isUnknown = true;
            }
        } else {
            isUnknown = true;  // No name at all = unknown
        }

        // Always count the event
        g_TotalEventCount++;
        if (shouldFilter) {
            g_FilteredEventCount++;
        }
        if (isUnknown) {
            g_UnknownEventCount++;
        }

        // Only add to circular display buffer if not filtered
        if (!shouldFilter) {
            AudioEvent& evt = g_Events[g_EventIndex];
            evt.timestamp = GetTickCount();
            evt.eventId = eventId;
            evt.gameObjId = gameObjId;
            evt.playingId = playingId;

            // bankName now stores the primary display label
            if (primaryName) {
                strncpy_s(evt.bankName, primaryName, 31);
            } else {
                sprintf_s(evt.bankName, "0x%08X", eventId);
            }

            // eventName stores additional captured name if available
            if (secondaryName) {
                strncpy_s(evt.eventName, secondaryName, 63);
            } else {
                evt.eventName[0] = '\0';
            }

            g_EventIndex = (g_EventIndex + 1) % MAX_EVENTS;
        }

        // Always log to full event log for export (including filtered events)
        // No limit - log grows as needed
        EventLogEntry entry;
        entry.timestamp = GetTickCount() - g_SessionStartTime;
        entry.eventId = eventId;

        if (primaryName) {
            strncpy_s(entry.bankName, primaryName, 31);
        } else {
            sprintf_s(entry.bankName, "0x%08X", eventId);
        }

        if (secondaryName) {
            strncpy_s(entry.eventName, secondaryName, 63);
        } else {
            entry.eventName[0] = '\0';
        }

        g_EventLog.push_back(entry);
    }

    return playingId;
}

// ============================================================================
// Load event bank mappings from compiled-in event_mapping.h
// ============================================================================
static void LoadEventBankMapping() {
    // Use the extended helper that also builds bank association map
    BuildEventMappingTableWithBanks(g_EventBankMapping, g_EventSourceBank);
    g_EventBankMappingCount = (int)g_EventMappingCount;

    char buf[256];
    sprintf_s(buf, "[AudioHook] Loaded %d event->bank mappings with source bank tracking\n", g_EventBankMappingCount);
    OutputDebugStringA(buf);
}

// Check if event's source bank is currently loaded
static bool IsEventBankLoaded(DWORD eventId) {
    if (!g_FilterByLoadedBanks) return true;  // Filtering disabled

    auto it = g_EventSourceBank.find(eventId);
    if (it == g_EventSourceBank.end()) return true;  // Unknown bank, allow it

    const std::string& sourceBank = it->second;

    // Check if source bank is in the loaded banks list
    for (const auto& loadedBank : g_LoadedBanks) {
        if (loadedBank == sourceBank) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Load hash dictionary for switch/state/event name resolution
// ============================================================================
static void LoadHashDictionary() {
    // Build dictionary from compiled-in data
    BuildHashDictionary(g_HashDictionary);
    g_DictionaryResolvedCount = (int)g_HashDictionary.size();

    char buf[256];
    sprintf_s(buf, "[AudioHook] Built hash dictionary with %d entries (switches, abilities, banks)\n",
              g_DictionaryResolvedCount);
    OutputDebugStringA(buf);
}

// ============================================================================
// Load event names from WWiseIDTable.audio.json
// ============================================================================
static void LoadEventNames() {
    // Try to load from JSON file - multiple fallback paths
    const char* paths[] = {
        "WWiseIDTable.audio.json",
        "..\\WWiseIDTable.audio.json",
        "..\\..\\WWiseIDTable.audio.json",
        "C:\\Users\\Yusuf\\Desktop\\Code\\ConquestConsole\\WWiseIDTable.audio.json",
        "D:\\Games\\LOTR Conquest\\WWiseIDTable.audio.json",
        "C:\\Program Files (x86)\\Electronic Arts\\The Lord of the Rings - Conquest\\WWiseIDTable.audio.json"
    };

    std::ifstream file;
    const char* loadedPath = nullptr;
    for (const char* path : paths) {
        file.open(path);
        if (file.is_open()) {
            loadedPath = path;
            break;
        }
    }

    if (!file.is_open()) {
        OutputDebugStringA("[AudioHook] ERROR: Could not open WWiseIDTable.audio.json from any path!\n");
        return;
    }

    // Simple JSON parsing - look for "key": "..." and "val": number pairs
    std::string line;
    std::string currentKey;
    int readableCount = 0;
    int totalCount = 0;

    while (std::getline(file, line)) {
        // Look for "key": "value"
        size_t keyPos = line.find("\"key\":");
        if (keyPos != std::string::npos) {
            size_t firstQuote = line.find('"', keyPos + 6);
            size_t lastQuote = line.rfind('"');
            if (firstQuote != std::string::npos && lastQuote > firstQuote) {
                currentKey = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
            }
        }

        // Look for "val": number
        size_t valPos = line.find("\"val\":");
        if (valPos != std::string::npos && !currentKey.empty()) {
            size_t numStart = line.find_first_of("0123456789", valPos);
            if (numStart != std::string::npos) {
                DWORD eventId = (DWORD)strtoul(line.c_str() + numStart, nullptr, 10);

                // Only store readable names (not starting with 0x)
                if (currentKey.length() > 0 && currentKey[0] != '0') {
                    g_EventNames[eventId] = currentKey;
                    readableCount++;
                }
                totalCount++;
                currentKey.clear();
            }
        }
    }

    file.close();

    char buf[256];
    sprintf_s(buf, "[AudioHook] Loaded %d readable names from %d entries (%s)\n",
              readableCount, totalCount, loadedPath ? loadedPath : "unknown");
    OutputDebugStringA(buf);
}

// ============================================================================
// Install JMP hooks
// ============================================================================
static bool InstallHooks() {
    bool success = true;
    DWORD oldProtect;
    BYTE jmp[5];

    // Hook PostEvent by ID
    if (!g_HookPostEventInstalled) {
        if (VirtualProtect((void*)ADDR_POST_EVENT_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_OriginalBytesPostEvent, (void*)ADDR_POST_EVENT_BY_ID, 5);
            jmp[0] = 0xE9;
            DWORD relAddr = (DWORD)&HookedPostEvent - ADDR_POST_EVENT_BY_ID - 5;
            memcpy(&jmp[1], &relAddr, 4);
            memcpy((void*)ADDR_POST_EVENT_BY_ID, jmp, 5);
            VirtualProtect((void*)ADDR_POST_EVENT_BY_ID, 5, oldProtect, &oldProtect);
            g_HookPostEventInstalled = true;
            OutputDebugStringA("[AudioHook] PostEvent hook installed\n");
        } else {
            success = false;
        }
    }

    // Hook GetIDFromString
    if (!g_HookGetIDInstalled) {
        if (VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_OriginalBytesGetID, (void*)ADDR_GET_ID_FROM_STRING, 5);
            jmp[0] = 0xE9;
            DWORD relAddr = (DWORD)&HookedGetIDFromString - ADDR_GET_ID_FROM_STRING - 5;
            memcpy(&jmp[1], &relAddr, 4);
            memcpy((void*)ADDR_GET_ID_FROM_STRING, jmp, 5);
            VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, oldProtect, &oldProtect);
            g_HookGetIDInstalled = true;
            OutputDebugStringA("[AudioHook] GetIDFromString hook installed\n");
        } else {
            success = false;
        }
    }

    // Hook SetSwitch by ID
    if (!g_HookSetSwitchInstalled) {
        if (VirtualProtect((void*)ADDR_SET_SWITCH_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_OriginalBytesSetSwitch, (void*)ADDR_SET_SWITCH_BY_ID, 5);
            jmp[0] = 0xE9;
            DWORD relAddr = (DWORD)&HookedSetSwitch - ADDR_SET_SWITCH_BY_ID - 5;
            memcpy(&jmp[1], &relAddr, 4);
            memcpy((void*)ADDR_SET_SWITCH_BY_ID, jmp, 5);
            VirtualProtect((void*)ADDR_SET_SWITCH_BY_ID, 5, oldProtect, &oldProtect);
            g_HookSetSwitchInstalled = true;
            OutputDebugStringA("[AudioHook] SetSwitch hook installed\n");
        } else {
            success = false;
        }
    }

    // Hook SetState by ID
    if (!g_HookSetStateInstalled) {
        if (VirtualProtect((void*)ADDR_SET_STATE_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_OriginalBytesSetState, (void*)ADDR_SET_STATE_BY_ID, 5);
            jmp[0] = 0xE9;
            DWORD relAddr = (DWORD)&HookedSetState - ADDR_SET_STATE_BY_ID - 5;
            memcpy(&jmp[1], &relAddr, 4);
            memcpy((void*)ADDR_SET_STATE_BY_ID, jmp, 5);
            VirtualProtect((void*)ADDR_SET_STATE_BY_ID, 5, oldProtect, &oldProtect);
            g_HookSetStateInstalled = true;
            OutputDebugStringA("[AudioHook] SetState hook installed\n");
        } else {
            success = false;
        }
    }

    return success;
}

// ============================================================================
// Remove hooks and restore original bytes
// ============================================================================
static void RemoveHooks() {
    DWORD oldProtect;

    if (g_HookPostEventInstalled) {
        if (VirtualProtect((void*)ADDR_POST_EVENT_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy((void*)ADDR_POST_EVENT_BY_ID, g_OriginalBytesPostEvent, 5);
            VirtualProtect((void*)ADDR_POST_EVENT_BY_ID, 5, oldProtect, &oldProtect);
        }
        g_HookPostEventInstalled = false;
    }

    if (g_HookGetIDInstalled) {
        if (VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy((void*)ADDR_GET_ID_FROM_STRING, g_OriginalBytesGetID, 5);
            VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, oldProtect, &oldProtect);
        }
        g_HookGetIDInstalled = false;
    }

    if (g_HookSetSwitchInstalled) {
        if (VirtualProtect((void*)ADDR_SET_SWITCH_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy((void*)ADDR_SET_SWITCH_BY_ID, g_OriginalBytesSetSwitch, 5);
            VirtualProtect((void*)ADDR_SET_SWITCH_BY_ID, 5, oldProtect, &oldProtect);
        }
        g_HookSetSwitchInstalled = false;
    }

    if (g_HookSetStateInstalled) {
        if (VirtualProtect((void*)ADDR_SET_STATE_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy((void*)ADDR_SET_STATE_BY_ID, g_OriginalBytesSetState, 5);
            VirtualProtect((void*)ADDR_SET_STATE_BY_ID, 5, oldProtect, &oldProtect);
        }
        g_HookSetStateInstalled = false;
    }
}

// ============================================================================
// Public API
// ============================================================================
bool Initialize() {
    g_SessionStartTime = GetTickCount();
    g_EventLog.clear();
    g_EventLog.reserve(1000);  // Pre-allocate for performance
    g_LoadedBanks.clear();
    LoadEventNames();
    LoadEventBankMapping();
    LoadHashDictionary();  // Load FNV-1a hash dictionary
    return InstallHooks();
}

void Shutdown() {
    RemoveHooks();
    g_EventNames.clear();
    g_CapturedNames.clear();
    g_EventBankMapping.clear();
    g_EventSourceBank.clear();
    g_EventLog.clear();
    g_LoadedBanks.clear();
    g_HashDictionary.clear();
}

void SetLogging(bool enabled) {
    g_LoggingEnabled = enabled;
}

bool IsLogging() {
    return g_LoggingEnabled;
}

void SetBankFiltering(bool enabled) {
    g_FilterByLoadedBanks = enabled;
}

bool IsBankFilteringEnabled() {
    return g_FilterByLoadedBanks;
}

const AudioEvent* GetRecentEvents(int* count) {
    if (count) {
        *count = (g_TotalEventCount < MAX_EVENTS) ? g_TotalEventCount : MAX_EVENTS;
    }
    return g_Events;
}

DWORD GetTotalEventCount() {
    return g_TotalEventCount;
}

void ClearLog() {
    memset(g_Events, 0, sizeof(g_Events));
    g_EventIndex = 0;
    g_TotalEventCount = 0;
}

const char* LookupEventName(DWORD eventId) {
    auto it = g_EventNames.find(eventId);
    if (it != g_EventNames.end()) {
        return it->second.c_str();
    }
    return nullptr;
}

bool IsAudioEnabled() {
    __try {
        return *(BYTE*)ADDR_AUDIO_ENABLED != 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int GetCapturedCount() {
    return g_CapturedCount;
}

int GetBankMappingCount() {
    return g_EventBankMappingCount;
}

void SetFilterNoisy(bool enabled) {
    g_FilterNoisy = enabled;
}

bool IsFilterNoisy() {
    return g_FilterNoisy;
}

DWORD GetFilteredEventCount() {
    return g_FilteredEventCount;
}

DWORD GetUnknownEventCount() {
    return g_UnknownEventCount;
}

int ExportCapturedNames(const char* filepath) {
    if (!filepath) return 0;

    std::ofstream file(filepath);
    if (!file.is_open()) return 0;

    int capturedCount = 0;
    file << "# Audio Debug Export\n";
    file << "# Generated by DebugOverlay\n\n";

    // Section 1: Captured bank names (from GetIDFromString)
    file << "================================================================================\n";
    file << "CAPTURED BANK NAMES (from GetIDFromString hook)\n";
    file << "================================================================================\n";
    file << "# Format: hash (hex) -> name\n\n";

    for (const auto& pair : g_CapturedNames) {
        file << "0x" << std::hex << pair.first << " -> " << pair.second << "\n";
        capturedCount++;
    }

    // Section 2: Event log (all events played this session)
    file << "\n================================================================================\n";
    file << "EVENT LOG (" << std::dec << g_EventLog.size() << " events played this session)\n";
    file << "================================================================================\n";
    file << "# Format: timestamp_ms | TXTP_name | event_name (if known)\n\n";

    for (const auto& entry : g_EventLog) {
        file << std::dec << entry.timestamp << "ms | ";
        if (entry.bankName[0] != '\0') {
            // bankName now contains full TXTP name like "BaseCombat-0705"
            file << entry.bankName;
            // Only add eventName if it's a real name (not just a hex ID)
            if (entry.eventName[0] != '0' || entry.eventName[1] != 'x') {
                file << " | " << entry.eventName;
            }
        } else {
            file << entry.eventName;
        }
        file << "\n";
    }

    file.close();

    char buf[256];
    sprintf_s(buf, "[AudioHook] Exported %d banks + %zu events to %s\n",
              capturedCount, g_EventLog.size(), filepath);
    OutputDebugStringA(buf);

    return capturedCount + (int)g_EventLog.size();
}

const char* GetLastActiveBank() {
    if (g_LastActiveBank[0] == '\0') return nullptr;
    return g_LastActiveBank;
}

int GetActiveBanks(const char** names, int maxCount) {
    if (!names || maxCount <= 0) return 0;

    int count = 0;
    DWORD now = GetTickCount();

    // Return banks active in last 5 seconds, most recent first
    for (int i = 0; i < MAX_RECENT_BANKS && count < maxCount; i++) {
        int idx = (g_RecentBankIndex - 1 - i + MAX_RECENT_BANKS) % MAX_RECENT_BANKS;
        if (g_RecentBanks[idx].name[0] != '\0' && (now - g_RecentBanks[idx].timestamp) < 5000) {
            names[count++] = g_RecentBanks[idx].name;
        }
    }
    return count;
}

int GetLoadedBankCount() {
    return (int)g_LoadedBanks.size();
}

int GetLoadedBanks(const char** names, int maxCount) {
    if (!names || maxCount <= 0) return 0;

    int count = 0;
    for (size_t i = 0; i < g_LoadedBanks.size() && count < maxCount; i++) {
        names[count++] = g_LoadedBanks[i].c_str();
    }
    return count;
}

int GetDictionaryResolvedCount() {
    return g_DictionaryResolvedCount;
}

const char* LookupHashInDictionary(DWORD hash) {
    return ResolveHash(hash);
}

const SwitchStateChange* GetRecentSwitchChanges(int* count) {
    if (count) {
        *count = (g_TotalSwitchChanges < MAX_SWITCH_CHANGES) ? g_TotalSwitchChanges : MAX_SWITCH_CHANGES;
    }
    return g_SwitchChanges;
}

// ============================================================================
// Asset Browser API Implementation
// ============================================================================

// Static storage for tracked bank info
static BankAssetInfo g_TrackedBanks[MAX_TRACKED_BANKS] = {0};
static int g_TrackedBankCount = 0;

// Helper to find or add a bank to tracking
static BankAssetInfo* FindOrAddTrackedBank(const char* bankName, DWORD bankHash) {
    // Search existing
    for (int i = 0; i < g_TrackedBankCount; i++) {
        if (g_TrackedBanks[i].bankHash == bankHash) {
            return &g_TrackedBanks[i];
        }
    }
    // Add new if space available
    if (g_TrackedBankCount < MAX_TRACKED_BANKS) {
        BankAssetInfo* info = &g_TrackedBanks[g_TrackedBankCount++];
        info->bankHash = bankHash;
        strncpy_s(info->bankName, bankName ? bankName : "Unknown", sizeof(info->bankName) - 1);
        info->eventCount = 0;
        info->wemCount = 0;
        info->soundCount = 0;
        info->isLoaded = true;
        return info;
    }
    return nullptr;
}

// Called when we detect a bank being used (from GetIDFromString hook)
void UpdateTrackedBankFromName(const char* bankName, DWORD bankHash) {
    BankAssetInfo* info = FindOrAddTrackedBank(bankName, bankHash);
    if (info) {
        info->isLoaded = true;
        // Event/WEM counts will be populated when we hook LoadBank
    }
}

int GetTrackedBanks(BankAssetInfo* outBanks, int maxCount) {
    if (!outBanks || maxCount <= 0) return 0;

    // First, sync with g_LoadedBanks to ensure we have all banks
    for (size_t i = 0; i < g_LoadedBanks.size(); i++) {
        const std::string& name = g_LoadedBanks[i];
        // Compute hash for this bank name
        DWORD hash = 0;
        // Simple hash for tracking (not FNV-1a, just for dedup)
        for (size_t j = 0; j < name.size(); j++) {
            hash = hash * 31 + (BYTE)name[j];
        }
        FindOrAddTrackedBank(name.c_str(), hash);
    }

    int count = 0;
    for (int i = 0; i < g_TrackedBankCount && count < maxCount; i++) {
        outBanks[count++] = g_TrackedBanks[i];
    }
    return count;
}

int GetTotalTrackedBankCount() {
    return g_TrackedBankCount > 0 ? g_TrackedBankCount : (int)g_LoadedBanks.size();
}

int GetTotalTrackedEventCount() {
    int total = 0;
    for (int i = 0; i < g_TrackedBankCount; i++) {
        total += g_TrackedBanks[i].eventCount;
    }
    // If no detailed tracking yet, estimate from event mapping
    if (total == 0) {
        total = g_EventBankMappingCount;
    }
    return total;
}

int GetTotalTrackedWemCount() {
    int total = 0;
    for (int i = 0; i < g_TrackedBankCount; i++) {
        total += g_TrackedBanks[i].wemCount;
    }
    return total;
}

// ============================================================================
// Playback API - Call game's Wwise functions directly
// ============================================================================

DWORD PlayEvent(DWORD eventId, DWORD gameObjectId) {
    if (!g_HookPostEventInstalled) {
        OutputDebugStringA("[AudioHook] PlayEvent: Hooks not installed\n");
        return 0;
    }

    // If no game object specified, use the last valid one we captured
    // This is important because Wwise requires registered game objects
    DWORD actualGameObj = gameObjectId;
    if (actualGameObj == 0 && g_LastValidGameObjectId != 0) {
        actualGameObj = g_LastValidGameObjectId;
    }

    char buf[128];
    sprintf_s(buf, "[AudioHook] PlayEvent: eventId=0x%08X gameObj=%u (last=%u)\n",
        eventId, actualGameObj, g_LastValidGameObjectId);
    OutputDebugStringA(buf);

    // Temporarily restore original bytes to call PostEvent
    DWORD oldProtect;
    VirtualProtect((LPVOID)ADDR_POST_EVENT_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)ADDR_POST_EVENT_BY_ID, g_OriginalBytesPostEvent, 5);
    VirtualProtect((LPVOID)ADDR_POST_EVENT_BY_ID, 5, oldProtect, &oldProtect);

    // Call the original PostEvent
    PostEventByID_t postEvent = (PostEventByID_t)ADDR_POST_EVENT_BY_ID;
    DWORD playingId = postEvent(eventId, actualGameObj, 0, nullptr, nullptr);

    // Reinstall our hook
    VirtualProtect((LPVOID)ADDR_POST_EVENT_BY_ID, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    BYTE jmpPatch[5];
    jmpPatch[0] = 0xE9; // JMP rel32
    DWORD relAddr = (DWORD)&HookedPostEvent - ADDR_POST_EVENT_BY_ID - 5;
    memcpy(&jmpPatch[1], &relAddr, 4);
    memcpy((void*)ADDR_POST_EVENT_BY_ID, jmpPatch, 5);
    VirtualProtect((LPVOID)ADDR_POST_EVENT_BY_ID, 5, oldProtect, &oldProtect);

    sprintf_s(buf, "[AudioHook] PlayEvent result: PlayingID=%u\n", playingId);
    OutputDebugStringA(buf);

    return playingId;
}

void StopAllSounds(DWORD gameObjectId) {
    // StopAll is not hooked, so we can call it directly
    StopAll_t stopAll = (StopAll_t)ADDR_STOP_ALL;
    stopAll(gameObjectId);

    char buf[64];
    sprintf_s(buf, "[AudioHook] StopAll: gameObj=%u\n", gameObjectId);
    OutputDebugStringA(buf);
}

DWORD GetLastValidGameObjectId() {
    return g_LastValidGameObjectId;
}

DWORD PlayEventByName(const char* eventName, DWORD gameObjectId) {
    if (!g_HookGetIDInstalled || !eventName) {
        OutputDebugStringA("[AudioHook] PlayEventByName: Hooks not installed or null name\n");
        return 0;
    }

    // Convert char* to wchar_t* for GetIDFromString
    wchar_t wideName[64] = {0};
    MultiByteToWideChar(CP_ACP, 0, eventName, -1, wideName, 64);

    // Temporarily remove GetIDFromString hook to call original
    DWORD oldProtect;
    VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)ADDR_GET_ID_FROM_STRING, g_OriginalBytesGetID, 5);
    VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, oldProtect, &oldProtect);

    // Get the hash for this event name
    GetIDFromString_t getIdFromString = (GetIDFromString_t)ADDR_GET_ID_FROM_STRING;
    DWORD eventId = getIdFromString(wideName);

    // Reinstall GetIDFromString hook
    VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    BYTE jmp[5];
    jmp[0] = 0xE9;
    DWORD relAddr = (DWORD)&HookedGetIDFromString - ADDR_GET_ID_FROM_STRING - 5;
    memcpy(&jmp[1], &relAddr, 4);
    memcpy((void*)ADDR_GET_ID_FROM_STRING, jmp, 5);
    VirtualProtect((void*)ADDR_GET_ID_FROM_STRING, 5, oldProtect, &oldProtect);

    char buf[128];
    sprintf_s(buf, "[AudioHook] PlayEventByName: '%s' -> 0x%08X\n", eventName, eventId);
    OutputDebugStringA(buf);

    // Now play the event using the hash
    if (eventId != 0) {
        return PlayEvent(eventId, gameObjectId);
    }
    return 0;
}

// ============================================================================
// Custom audio helper implementations
//
// These provide simple stubs for checking file existence, initializing the
// custom audio engine, loading overrides, and playing custom audio.
// You can replace these with your own implementations using an audio
// library such as miniaudio.  The current stubs compile but do not
// actually play audio.

static bool FileExists(const std::string& filepath) {
    std::ifstream f(filepath.c_str(), std::ios::binary);
    return f.good();
}

static void PlayCustomAudio(const std::string& baseName) {
    // Determine which file extension exists.  We search common formats
    // in order of preference.  If none exists, bail out early.
    std::string path;
    // Prefer .wav because PlaySound can handle it natively
    if (FileExists(baseName + ".wav")) {
        path = baseName + ".wav";
    } else if (FileExists(baseName + ".mp3")) {
        path = baseName + ".mp3";
    } else if (FileExists(baseName + ".ogg")) {
        // The Windows multimedia APIs do not natively support Ogg Vorbis.
        // If an Ogg file exists and the system has a codec installed it may
        // still play via MCI, so we fall back to it last.  Otherwise
        // nothing will be heard.
        path = baseName + ".ogg";
    } else {
        return;
    }

    // If the file ends with .wav, use PlaySound for simple asynchronous
    // playback.  This API returns immediately and does not block the game.
    if (path.size() >= 4) {
        std::string ext = path.substr(path.size() - 4);
        // Convert extension to lowercase for comparison
        for (char& c : ext) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }
        if (ext == ".wav") {
            // PlaySoundA expects a null‑terminated ANSI string.  We request
            // asynchronous playback and disable the default system sounds on
            // failure.  SND_NODEFAULT ensures that if the file cannot be
            // played no fallback beep is emitted.
            PlaySoundA(path.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
            return;
        }
    }

    // For other formats (e.g. .mp3, .ogg), attempt to use the
    // Multimedia Control Interface (MCI).  We close any previous
    // alias before opening a new one to ensure a clean slate.
    // Use a fixed alias name to avoid leaking handles across plays.
    mciSendStringA("close mp3alias", NULL, 0, NULL);
    std::string openCmd = "open \"" + path + "\" type mpegvideo alias mp3alias";
    mciSendStringA(openCmd.c_str(), NULL, 0, NULL);
    std::string playCmd = "play mp3alias from 0";
    mciSendStringA(playCmd.c_str(), NULL, 0, NULL);
}

static void LoadAudioOverrides() {
    // TODO: implement parsing of overrides if needed.  This stub does
    // nothing and can be expanded to load mappings from a file.
}

static void InitCustomAudio() {
    // TODO: initialize your custom audio engine here, if necessary.
    // This stub intentionally does nothing.
}

} // namespace AudioHook
