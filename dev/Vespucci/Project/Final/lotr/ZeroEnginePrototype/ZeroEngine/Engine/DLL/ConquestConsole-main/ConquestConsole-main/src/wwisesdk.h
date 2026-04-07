#pragma once
#include <Windows.h>

// ============================================================================
// Wwise SDK Integration Layer
// Creates a separate Wwise sound engine instance (independent from the game's)
// using static libraries from the vendored Wwise SDK v2.1.2821 (2008).
//
// This module replaces miniaudio for custom audio playback, providing full
// Wwise feature support: banks, events, switches, states, 3D positioning, etc.
// ============================================================================
// Eriumsss

namespace WwiseSDK {

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /// Initialize the separate Wwise engine instance.
    /// Must be called after the game window is available (D3D device created).
    /// @param hWnd  Game window handle (needed for DirectSound initialization)
    /// @return true if all subsystems initialized successfully
    bool Initialize(HWND hWnd);

    /// Shut down the Wwise engine in reverse initialization order.
    /// Safe to call even if not initialized.
    void Shutdown();

    /// Process pending audio events and render audio.
    /// Must be called once per frame (typically from the D3D EndScene hook).
    void RenderAudio();

    /// Check if the Wwise engine is initialized and ready.
    bool IsInitialized();

    // ========================================================================
    // Bank Management
    // ========================================================================

    /// Load a sound bank by name from the banks folder.
    /// Banks are loaded from: ConquestMods/audio/banks/{bankName}.bnk
    /// @param bankName  Wide-string bank name (e.g., L"Init", L"MyCustomBank")
    /// @return true if the bank was loaded successfully
    bool LoadBank(const wchar_t* bankName);

    /// Unload a previously loaded bank by name.
    /// @param bankName  Wide-string bank name
    void UnloadBank(const wchar_t* bankName);

    /// Unload all loaded banks and clear internal tracking.
    void UnloadAllBanks();

    // ========================================================================
    // Event Playback
    // ========================================================================

    /// Post an event by name (wide string, as required by Wwise 2008 API).
    /// @param eventName     Wide-string event name (e.g., L"Play_MySound")
    /// @param gameObjectID  Game object to post the event on (0 = default)
    /// @return Playing ID, or 0 (AK_INVALID_PLAYING_ID) on failure
    unsigned long PostEvent(const wchar_t* eventName, unsigned long gameObjectID = 100);

    /// Post an event by numeric ID.
    /// @param eventID       Wwise event unique ID
    /// @param gameObjectID  Game object to post the event on
    /// @return Playing ID, or 0 on failure
    unsigned long PostEventByID(unsigned long eventID, unsigned long gameObjectID = 100);

    /// Stop all sounds on a specific game object, or all sounds if 0xFFFFFFFF.
    void StopAll(unsigned long gameObjectID = 0xFFFFFFFF);

    /// Stop a specific playing ID.
    void StopPlayingID(unsigned long playingID);

    // ========================================================================
    // Game Object Management
    // ========================================================================

    /// Register a game object with the Wwise engine.
    /// Must be done before posting events on that object.
    /// @param gameObjectID  Unique game object ID (cannot be 0 or 0xFFFFFFFF)
    /// @param name          Optional debug name (for profiling)
    /// @return true if successful
    bool RegisterGameObject(unsigned long gameObjectID, const char* name = nullptr);

    /// Unregister a game object.
    void UnregisterGameObject(unsigned long gameObjectID);

    // ========================================================================
    // Volume & State Control
    // ========================================================================

    /// Set a global RTPC value (e.g., master volume).
    /// @param rtpcName  Wide-string RTPC name
    /// @param value     RTPC value
    void SetRTPCValue(const wchar_t* rtpcName, float value);

    /// Set a switch value on a game object.
    void SetSwitch(const wchar_t* switchGroup, const wchar_t* switchState, unsigned long gameObjectID = 100);

    /// Set a global state.
    void SetState(const wchar_t* stateGroup, const wchar_t* stateValue);

    // ========================================================================
    // Configuration
    // ========================================================================

    /// Get the base path where banks are loaded from.
    const char* GetBankBasePath();
}

