#pragma once
#include <Windows.h>

// ============================================================================
// Custom Audio - Play custom audio via a separate Wwise SDK engine instance
// Banks are auto-loaded from ConquestMods/audio/banks/*.bnk
// ============================================================================
// Eriumsss

namespace CustomAudio {
    // Initialize the custom audio tracking layer
    // WwiseSDK must be initialized first (with HWND). Auto-loads banks.
    bool Initialize();

    // Shutdown and unload all custom banks
    void Shutdown();

    // Check if custom audio backend is available
    // Returns true if WwiseSDK is initialized (events may or may not exist in banks)
    bool HasCustomAudio(const char* eventName);

    // Post a Wwise event by name through the separate engine instance
    // Returns true if the event was found in a loaded bank and is playing
    // Returns false if no bank contains this event (caller falls through to game audio)
    bool TryPlayCustomAudio(const char* eventName);

    // Stop a specific sound by event name (e.g., "play_front_end")
    // Also handles stop_* events by mapping to corresponding play_* events
    void StopSound(const char* eventName);

    // Handle stop events - checks if this is a stop event and stops the corresponding sound
    // Returns true if a stop was handled, false otherwise
    bool HandleStopEvent(const char* eventName);

    // Stop all custom audio playback
    void StopAll();

    // Check if custom audio system is initialized
    bool IsInitialized();

    // Get the mod folder path (for display/logging)
    const char* GetModFolderPath();

    // Get count of custom audio files played this session
    DWORD GetCustomPlayCount();

    // Volume control (0.0 to 1.0, default 1.0)
    void SetVolume(float volume);
    float GetVolume();

    // Logging toggle
    void SetLogging(bool enabled);
    bool IsLogging();
}

