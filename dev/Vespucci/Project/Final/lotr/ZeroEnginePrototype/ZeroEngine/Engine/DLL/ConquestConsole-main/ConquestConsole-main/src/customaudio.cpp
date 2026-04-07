// ============================================================================
// Custom Audio Implementation
// Uses the real Wwise SDK (separate engine instance) for mod audio playback.
// Replaces the previous miniaudio backend while keeping the same public API.
// ============================================================================
// Eriumsss

#include "customaudio.h"
#include "wwisesdk.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace CustomAudio {

static bool g_Initialized = false;
static bool g_LoggingEnabled = true;
static DWORD g_CustomPlayCount = 0;
static float g_Volume = 1.0f;

static const char* MOD_FOLDER = "ConquestMods\\audio\\";
static const char* BANK_FOLDER = "ConquestMods\\audio\\banks\\";

// ============================================================================
// Sound Tracking - Track playing IDs so they can be stopped by event name
// ============================================================================
struct TrackedSound {
    unsigned long playingID;
    bool inUse;
    char eventName[64];
};

static const int MAX_TRACKED_SOUNDS = 32;
static TrackedSound g_TrackedSounds[MAX_TRACKED_SOUNDS] = {};

// Stop event mappings: stop_* -> play_*
struct StopEventMapping {
    const char* stopEvent;
    const char* playEvent;
};

static const StopEventMapping g_StopMappings[] = {
    {"stop_front_end", "play_front_end"},
    {"stop_music", nullptr},
    {"stop_music_now", nullptr},
    {"stop_all_but_music", nullptr},
};
static const int NUM_STOP_MAPPINGS = sizeof(g_StopMappings) / sizeof(g_StopMappings[0]);

// ============================================================================
// Internal: Auto-load all .bnk files from the bank folder
// ============================================================================
static void AutoLoadBanks() {
    char searchPath[MAX_PATH];
    sprintf_s(searchPath, "%s*.bnk", BANK_FOLDER);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    int loaded = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t wideName[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, fd.cFileName, -1, wideName, MAX_PATH);
            if (WwiseSDK::LoadBank(wideName))
                loaded++;
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

    if (loaded > 0) {
        char buf[128];
        sprintf_s(buf, "[CustomAudio] Auto-loaded %d bank(s) from %s\n", loaded, BANK_FOLDER);
        OutputDebugStringA(buf);
    }
}

// ============================================================================
// Internal: Find a free tracking slot (reuse oldest if full)
// ============================================================================
static int FindFreeSlot() {
    for (int i = 0; i < MAX_TRACKED_SOUNDS; i++) {
        if (!g_TrackedSounds[i].inUse)
            return i;
    }
    // All slots full - reuse slot 0
    g_TrackedSounds[0].inUse = false;
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

bool Initialize() {
    if (g_Initialized)
        return true;

    memset(g_TrackedSounds, 0, sizeof(g_TrackedSounds));

    // Directories created by WwiseSDK::Initialize()
    // Auto-load any .bnk files already in the bank folder
    if (WwiseSDK::IsInitialized())
        AutoLoadBanks();

    g_Initialized = true;
    OutputDebugStringA("[CustomAudio] Initialized (Wwise SDK backend)\n");
    return true;
}

void Shutdown() {
    if (!g_Initialized)
        return;

    // Unload all custom banks
    if (WwiseSDK::IsInitialized())
        WwiseSDK::UnloadAllBanks();

    memset(g_TrackedSounds, 0, sizeof(g_TrackedSounds));
    g_Initialized = false;
    OutputDebugStringA("[CustomAudio] Shutdown complete\n");
}

bool HasCustomAudio(const char* eventName) {
    // With Wwise backend, we can't cheaply check if an event exists in loaded banks.
    // Return true if WwiseSDK is initialized and has banks loaded - the PostEvent
    // call will simply return 0 (AK_INVALID_PLAYING_ID) if the event isn't found.
    (void)eventName;
    return WwiseSDK::IsInitialized();
}

bool TryPlayCustomAudio(const char* eventName) {
    if (!g_Initialized || !WwiseSDK::IsInitialized() || !eventName)
        return false;

    // Convert narrow event name to wide string for Wwise API
    wchar_t wideEvent[128];
    MultiByteToWideChar(CP_ACP, 0, eventName, -1, wideEvent, 128);

    // Post event through our separate Wwise engine instance
    unsigned long playingID = WwiseSDK::PostEvent(wideEvent);

    if (playingID == 0)
        return false; // Event not found in any loaded bank

    // Track for stop support
    int slot = FindFreeSlot();
    TrackedSound* ts = &g_TrackedSounds[slot];
    ts->playingID = playingID;
    strncpy_s(ts->eventName, eventName, sizeof(ts->eventName) - 1);
    ts->inUse = true;

    g_CustomPlayCount++;

    if (g_LoggingEnabled) {
        char buf[256];
        sprintf_s(buf, "[CustomAudio] PostEvent '%s' -> playingID=%lu\n", eventName, playingID);
        OutputDebugStringA(buf);
    }

    return true;
}

void StopSound(const char* eventName) {
    if (!g_Initialized || !eventName)
        return;

    int stoppedCount = 0;
    for (int i = 0; i < MAX_TRACKED_SOUNDS; i++) {
        if (g_TrackedSounds[i].inUse &&
            strcmp(g_TrackedSounds[i].eventName, eventName) == 0) {
            WwiseSDK::StopPlayingID(g_TrackedSounds[i].playingID);
            g_TrackedSounds[i].inUse = false;
            stoppedCount++;
        }
    }

    if (g_LoggingEnabled && stoppedCount > 0) {
        char buf[256];
        sprintf_s(buf, "[CustomAudio] Stopped %d sound(s) for event: %s\n", stoppedCount, eventName);
        OutputDebugStringA(buf);
    }
}

bool HandleStopEvent(const char* eventName) {
    if (!g_Initialized || !eventName)
        return false;

    // General stop events
    if (strcmp(eventName, "stop_music") == 0 ||
        strcmp(eventName, "stop_music_now") == 0) {
        StopAll();
        return true;
    }

    // Check mapped stop events
    for (int i = 0; i < NUM_STOP_MAPPINGS; i++) {
        if (strcmp(eventName, g_StopMappings[i].stopEvent) == 0) {
            if (g_StopMappings[i].playEvent)
                StopSound(g_StopMappings[i].playEvent);
            else
                StopAll();
            return true;
        }
    }

    // Automatic stop_* -> play_* mapping
    if (strncmp(eventName, "stop_", 5) == 0) {
        char playEvent[64];
        snprintf(playEvent, sizeof(playEvent), "play_%s", eventName + 5);

        for (int i = 0; i < MAX_TRACKED_SOUNDS; i++) {
            if (g_TrackedSounds[i].inUse &&
                strcmp(g_TrackedSounds[i].eventName, playEvent) == 0) {
                StopSound(playEvent);
                return true;
            }
        }
    }

    return false;
}

void StopAll() {
    if (!g_Initialized)
        return;

    // Stop via Wwise SDK (kills all sounds on the default game object)
    if (WwiseSDK::IsInitialized())
        WwiseSDK::StopAll();

    // Clear tracking
    int stoppedCount = 0;
    for (int i = 0; i < MAX_TRACKED_SOUNDS; i++) {
        if (g_TrackedSounds[i].inUse) {
            g_TrackedSounds[i].inUse = false;
            stoppedCount++;
        }
    }

    if (g_LoggingEnabled) {
        char buf[128];
        sprintf_s(buf, "[CustomAudio] StopAll: cleared %d tracked sound(s)\n", stoppedCount);
        OutputDebugStringA(buf);
    }
}

bool IsInitialized() {
    return g_Initialized;
}

const char* GetModFolderPath() {
    return MOD_FOLDER;
}

DWORD GetCustomPlayCount() {
    return g_CustomPlayCount;
}

void SetVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    g_Volume = volume;

    // Set volume via Wwise RTPC (user should define "CustomVolume" RTPC in their Wwise project)
    if (WwiseSDK::IsInitialized()) {
        wchar_t rtpcName[] = L"CustomVolume";
        WwiseSDK::SetRTPCValue(rtpcName, g_Volume * 100.0f); // Wwise RTPC typically 0-100
    }

    if (g_LoggingEnabled) {
        char buf[64];
        sprintf_s(buf, "[CustomAudio] Volume set to %.2f\n", g_Volume);
        OutputDebugStringA(buf);
    }
}

float GetVolume() {
    return g_Volume;
}

void SetLogging(bool enabled) {
    g_LoggingEnabled = enabled;
}

bool IsLogging() {
    return g_LoggingEnabled;
}

} // namespace CustomAudio
