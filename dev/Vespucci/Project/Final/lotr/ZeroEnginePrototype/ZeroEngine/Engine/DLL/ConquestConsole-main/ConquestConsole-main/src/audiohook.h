#pragma once
#include <Windows.h>

// ============================================================================
// Audio Hook - Intercept Wwise audio calls for debugging
// ============================================================================
//Eriumsss
namespace AudioHook {
    // Maximum events to keep in circular buffer
    static const int MAX_EVENTS = 32;

    // Audio event structure
    struct AudioEvent {
        DWORD timestamp;        // GetTickCount when event fired
        DWORD eventId;          // Wwise event ID
        DWORD gameObjId;        // Game object that triggered it
        DWORD playingId;        // Returned playing ID (0 if failed)
        char eventName[64];     // Event name (if found in lookup table)
        char bankName[32];      // Inferred STID bank name (from 91 known banks)
    };

    // Initialize audio hooks and load event name table
    bool Initialize();

    // Shutdown hooks
    void Shutdown();

    // Toggle audio logging
    void SetLogging(bool enabled);
    bool IsLogging();

    // Get recent audio events (circular buffer)
    const AudioEvent* GetRecentEvents(int* count);

    // Get total event count since init
    DWORD GetTotalEventCount();

    // Clear event log
    void ClearLog();

    // Lookup event name by ID (returns nullptr if not found)
    const char* LookupEventName(DWORD eventId);

    // Get audio system status
    bool IsAudioEnabled();  // DAT_00a3e851

    // Get count of captured event names from GetIDFromString hook
    int GetCapturedCount();

    // Get count of event->bank mappings loaded from TXTP data
    int GetBankMappingCount();

    // Toggle noisy event filter (footsteps etc.)
    void SetFilterNoisy(bool enabled);
    bool IsFilterNoisy();
    DWORD GetFilteredEventCount();

    // Get count of uncracked/unknown events played this session
    // Unknown events are those with TXTP names like "BankName-NNNN" but no semantic name
    DWORD GetUnknownEventCount();

    // Toggle bank-based filtering (only show events from loaded banks)
    // This prevents showing "HeroLurtz-0026" when HeroLurtz.bnk isn't loaded
    void SetBankFiltering(bool enabled);
    bool IsBankFilteringEnabled();

    // Export captured names to file (returns count exported)
    int ExportCapturedNames(const char* filepath);

    // Get last active bank name (for display context)
    const char* GetLastActiveBank();

    // Get list of recently active banks (last 5 seconds)
    int GetActiveBanks(const char** names, int maxCount);

    // Get count of all banks loaded this session
    int GetLoadedBankCount();

    // Get list of all banks loaded this session (permanent)
    int GetLoadedBanks(const char** names, int maxCount);

    // Get count of resolved hashes from dictionary
    int GetDictionaryResolvedCount();

    // Lookup hash in dictionary (returns nullptr if not found)
    const char* LookupHashInDictionary(DWORD hash);

    // Get recent switch/state changes for display
    struct SwitchStateChange {
        DWORD timestamp;
        DWORD groupId;
        DWORD valueId;
        DWORD gameObjId;
        char groupName[32];
        char valueName[32];
        bool isSwitch;  // true=SetSwitch, false=SetState
    };
    static const int MAX_SWITCH_CHANGES = 16;
    const SwitchStateChange* GetRecentSwitchChanges(int* count);

    // Asset Browser API - for displaying loaded bank contents
    struct BankAssetInfo {
        DWORD bankHash;         // FNV-1a hash of bank name
        char bankName[48];      // Semantic name (e.g., "HeroSauron")
        int eventCount;         // Number of events in this bank
        int wemCount;           // Number of embedded WEM files
        int soundCount;         // Number of CAkSound objects
        bool isLoaded;          // Currently loaded in memory
    };
    static const int MAX_TRACKED_BANKS = 64;

    // Get all tracked banks (loaded this session)
    int GetTrackedBanks(BankAssetInfo* outBanks, int maxCount);

    // Get total counts for display
    int GetTotalTrackedBankCount();
    int GetTotalTrackedEventCount();
    int GetTotalTrackedWemCount();

    // Playback API - trigger Wwise events on demand
    // Returns playing ID (>0) on success, 0 on failure
    DWORD PlayEvent(DWORD eventId, DWORD gameObjectId = 0);

    // Play event by name string (uses GetIDFromString to hash, then calls PostEvent)
    // Useful for testing stop_all, stop_music, etc.
    DWORD PlayEventByName(const char* eventName, DWORD gameObjectId = 0);

    // Stop all sounds on a game object (0 = all objects)
    void StopAllSounds(DWORD gameObjectId = 0);

    // Get last valid game object ID (for display/debugging)
    DWORD GetLastValidGameObjectId();
}

