#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
//Eriumsss
// ============================================================================
// Debug Overlay Configuration
// LOTR: Conquest Debug Renderer
// ============================================================================

namespace Config {
    // Toggle keys (virtual key codes)
    constexpr int KEY_TOGGLE_OVERLAY    = VK_F1;    // Toggle entire overlay
    constexpr int KEY_TOGGLE_FPS        = VK_F2;    // Toggle FPS counter
    constexpr int KEY_TOGGLE_POSITION   = VK_F3;    // Toggle player position
    constexpr int KEY_TOGGLE_WIREFRAME  = VK_F4;    // Toggle wireframe mode
    constexpr int KEY_TOGGLE_MENU       = VK_F5;    // Toggle debug menu
    constexpr int KEY_TOGGLE_COLLISION  = VK_F6;    // Toggle collision boxes
    constexpr int KEY_TOGGLE_AUDIO      = VK_F7;    // Toggle audio debug
    constexpr int KEY_TOGGLE_ASSET_BROWSER = VK_F10; // Toggle asset browser
    constexpr int KEY_RELOAD_CONFIG     = VK_F12;   // Reload configuration

    // Colors (ARGB format) - prefixed with CLR_ to avoid Windows macro conflicts
    constexpr DWORD CLR_TEXT          = 0xFFFFFFFF;  // White
    constexpr DWORD CLR_TEXT_SHADOW   = 0xFF000000;  // Black
    constexpr DWORD CLR_HIGHLIGHT     = 0xFF00FF00;  // Green
    constexpr DWORD CLR_WARNING       = 0xFFFFFF00;  // Yellow
    constexpr DWORD CLR_ERROR         = 0xFFFF0000;  // Red
    constexpr DWORD CLR_BACKGROUND    = 0x80000000;  // Semi-transparent black

    // Overlay positioning
    constexpr int OVERLAY_MARGIN_X      = 10;
    constexpr int OVERLAY_MARGIN_Y      = 10;
    constexpr int LINE_HEIGHT           = 16;

    // Font settings
    constexpr int FONT_SIZE             = 14;
    constexpr const char* FONT_NAME     = "Consolas";

    // Audio panel settings (right side of screen)
    constexpr int AUDIO_PANEL_WIDTH     = 380;
    constexpr int AUDIO_PANEL_MARGIN    = 10;
    constexpr int AUDIO_LINE_HEIGHT     = 14;
    constexpr int AUDIO_EVENTS_SHOWN    = 12;

    // Debug options (match Lua DebugSystem table)
    struct DebugOptions {
        bool overlayEnabled         = true;
        bool showFPS                = true;
        bool showPosition           = true;
        bool showMemoryUsage        = false;
        bool wireframeMode          = false;
        bool showDebugMenu          = false;
        
        // NPC Debug
        bool displayNPCInfo         = false;
        bool displayNPCBehaviour    = false;
        
        // Pathfinding Debug  
        bool displayPathfinding     = false;
        bool displayNavMesh         = false;
        
        // Collision Debug
        bool displayCollision       = false;
        bool displayRedBoxes        = false;

        // Audio Debug
        bool displayAudio           = false;
        bool displayAssetBrowser    = false;
    };
}

