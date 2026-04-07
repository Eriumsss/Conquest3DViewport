#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d9.h>
#include <cstdio>
#include <vector>
#include "d3d9hook.h"
#include "debugrenderer.h"
#include "input.h"
#include "config.h"
#include "gamedata.h"
#include "audiohook.h"
#include "event_mapping.h"
//Eriumsss
// ============================================================================
// LOTR: Conquest Debug Overlay DLL
// Inject this DLL into the game to enable debug visualization
// ============================================================================

// Global state
static Config::DebugOptions g_Options;
static DWORD g_FrameCount = 0;
static DWORD g_LastFPSTime = 0;
static int g_FPS = 0;
static int g_FrameCounter = 0;
static IDirect3DDevice9* g_CurrentDevice = nullptr;

// Asset Browser state
static int g_SelectedEventIndex = 0;

// Forward declarations
void RenderOverlay(IDirect3DDevice9* device);
void UpdateFPS();
void HandleInput(IDirect3DDevice9* device);
void DrawDebugMenu(int x, int y);

// Safe memory read helpers (SEH-compatible, no C++ objects)
static DWORD SafeReadDWORD(DWORD addr) {
    DWORD result = 0;
    __try { result = *(DWORD*)addr; } __except(1) {}
    return result;
}

static BYTE SafeReadBYTE(DWORD addr) {
    BYTE result = 0;
    __try { result = *(BYTE*)addr; } __except(1) {}
    return result;
}

static float SafeReadFLOAT(DWORD addr) {
    float result = 0.0f;
    __try { result = *(float*)addr; } __except(1) {}
    return result;
}

// ============================================================================
// Main render callback - called every frame from EndScene hook
// ============================================================================
void RenderOverlay(IDirect3DDevice9* device) {
    if (!device) return;
    g_CurrentDevice = device;

    // Update input and FPS
    Input::Update();
    HandleInput(device);
    UpdateFPS();
    g_FrameCount++;

    // Initialize renderer if needed and draw
    DebugRenderer::BeginFrame(device);

    // Get player data and debug info
    GameData::PlayerData player = GameData::GetPlayerData();
    GameData::DebugInfo dbgInfo = GameData::GetDebugInfo();

    // Use config line height for spacing
    const int lineH = Config::LINE_HEIGHT;
    const int startX = Config::OVERLAY_MARGIN_X;
    const int startY = Config::OVERLAY_MARGIN_Y;
    int y = startY;

    // Calculate box height - compact console
    int numLines = 14;  // Reduced lines for compact view
    int boxHeight = numLines * lineH + 16;
    int boxWidth = 520;  // Narrower for compact view

    // Draw background box using DebugRenderer (semi-transparent, rounded corners effect via alpha)
    DebugRenderer::DrawFilledRect((float)startX, (float)startY, (float)boxWidth, (float)boxHeight, 0xB0000000);
    DebugRenderer::DrawRect((float)startX, (float)startY, (float)boxWidth, (float)boxHeight, 0xFF00FF00);

    // Draw text with proper spacing
    y += 10;  // Padding inside box
    DebugRenderer::DrawText(startX + 10, y, 0xFFFFFFFF, "Debug Overlay Active");
    y += lineH;
    DebugRenderer::DrawTextF(startX + 10, y, 0xFFFFFF00, "FPS: %d  Frame: %d", g_FPS, g_FrameCount);
    y += lineH;
    DebugRenderer::DrawTextF(startX + 10, y, 0xFF00FFFF, "Wire: %s  Menu: %s",
        g_Options.wireframeMode ? "ON" : "OFF",
        g_Options.showDebugMenu ? "ON" : "OFF");
    y += lineH;
    DebugRenderer::DrawText(startX + 10, y, 0xFFAAAAAA, "F1:Hide F4:Wire F5:Menu F6:Collision");
    y += lineH;

    // Show creature array: DAT_00cd7f20 + 0x64 has 4 creature pointers
    DebugRenderer::DrawTextF(startX + 10, y, 0xFF88FF88, "DAT_00cd7f20@%08X = %08X",
        dbgInfo.gameStateAddr, dbgInfo.gameStateValue);
    y += lineH;

    // Show the 4 creature array entries (using safe read helpers)
    DWORD creatures[4] = {0};
    if (dbgInfo.gameStateValue != 0) {
        for (int i = 0; i < 4; i++) {
            creatures[i] = SafeReadDWORD(dbgInfo.gameStateValue + 0x64 + (i * 4));
        }
    }
    DebugRenderer::DrawTextF(startX + 10, y, 0xFFFFAA00,
        "Creatures[0-3]: %08X %08X %08X %08X", creatures[0], creatures[1], creatures[2], creatures[3]);
    y += lineH;

    if (dbgInfo.creatureValue != 0) {
        // Track if creature pointer changes
        static DWORD lastCreature = 0;
        bool creatureChanged = (dbgInfo.creatureValue != lastCreature);
        lastCreature = dbgInfo.creatureValue;

        DebugRenderer::DrawTextF(startX + 10, y, creatureChanged ? 0xFFFF0000 : 0xFF00FF00,
            "Creature: %08X %s", dbgInfo.creatureValue, creatureChanged ? "(CHANGED!)" : "");
        y += lineH;

        // TEAM ID - confirmed at +0x1CA0 (byte)
        BYTE teamId = SafeReadBYTE(dbgInfo.creatureValue + 0x1ca0);

        const char* teamNames[] = {"Neutral", "Good", "Evil", "Team3"};
        const char* teamName = (teamId < 4) ? teamNames[teamId] : "Unknown";
        DWORD teamColor = (teamId == 1) ? 0xFF00FF00 : (teamId == 2) ? 0xFFFF0000 : 0xFFFFFF00;
        DebugRenderer::DrawTextF(startX + 10, y, teamColor,
            "TEAM: %d (%s)", teamId, teamName);
        y += lineH;

        // === CONFIRMED WORKING DATA ===

        // Input direction vectors (confirmed working)
        float inputX = SafeReadFLOAT(dbgInfo.creatureValue + 0xA0);
        float inputY = SafeReadFLOAT(dbgInfo.creatureValue + 0xA4);
        float inputZ = SafeReadFLOAT(dbgInfo.creatureValue + 0xA8);
        DebugRenderer::DrawTextF(startX + 10, y, 0xFF88FFFF,
            "Input (+A0/A4/A8): %.1f  %.1f  %.1f", inputX, inputY, inputZ);
        y += lineH;

        // Velocity/movement (changes on walk + mouse)
        float velX = SafeReadFLOAT(dbgInfo.creatureValue + 0xC0);
        float velY = SafeReadFLOAT(dbgInfo.creatureValue + 0xC4);
        float velZ = SafeReadFLOAT(dbgInfo.creatureValue + 0xC8);
        DebugRenderer::DrawTextF(startX + 10, y, 0xFFAAFFAA,
            "Vel (+C0/C4/C8): %.1f  %.1f  %.1f", velX, velY, velZ);
        y += lineH;

        // Camera/look direction
        float lookX = SafeReadFLOAT(dbgInfo.creatureValue + 0xD0);
        float lookY = SafeReadFLOAT(dbgInfo.creatureValue + 0xD4);
        float lookZ = SafeReadFLOAT(dbgInfo.creatureValue + 0xD8);
        DebugRenderer::DrawTextF(startX + 10, y, 0xFFFFAAFF,
            "Look (+D0/D4/D8): %.2f  %.2f  %.2f", lookX, lookY, lookZ);
        y += lineH;

        // === 3D COLLISION BOX RENDERING (F6 toggle) ===
        if (g_Options.displayCollision) {
            // Get all entities and draw their collision boxes
            std::vector<GameData::EntityInfo> entities = GameData::GetAllEntities();

            int entityCount = 0;
            for (const auto& ent : entities) {
                // Choose color based on team and player status
                DWORD color;
                if (ent.isPlayer) {
                    color = 0xFF00FFFF;  // Cyan for local player
                } else if (ent.teamId == 1) {
                    color = 0xFF00FF00;  // Green for good team
                } else if (ent.teamId == 2) {
                    color = 0xFFFF0000;  // Red for evil team
                } else {
                    color = 0xFFFFFF00;  // Yellow for neutral/unknown
                }

                // Draw AABB box
                float centerX = (ent.aabbMinX + ent.aabbMaxX) * 0.5f;
                float centerY = (ent.aabbMinY + ent.aabbMaxY) * 0.5f;
                float centerZ = (ent.aabbMinZ + ent.aabbMaxZ) * 0.5f;
                float sizeX = ent.aabbMaxX - ent.aabbMinX;
                float sizeY = ent.aabbMaxY - ent.aabbMinY;
                float sizeZ = ent.aabbMaxZ - ent.aabbMinZ;

                DebugRenderer::DrawBox3D(centerX, centerY, centerZ, sizeX, sizeY, sizeZ, color);
                entityCount++;
            }

            // Show entity count and W2S debug info
            DebugRenderer::DrawTextF(startX + 10, y, 0xFF00FF00,
                "Entities: %d (F6 to toggle)", entityCount);
            y += lineH;

            // Show W2S debug info for first entity
            if (!entities.empty()) {
                const auto& ent = entities[0];
                float sx, sy;
                bool w2sOk = DebugRenderer::WorldToScreen(ent.position.x, ent.position.y, ent.position.z, &sx, &sy);
                int failReason = DebugRenderer::GetW2SFailReason();
                const D3DVIEWPORT9* vp = DebugRenderer::GetLastViewport();
                const D3DMATRIX* view = DebugRenderer::GetLastViewMatrix();
                const D3DMATRIX* proj = DebugRenderer::GetLastProjMatrix();

                const char* failReasons[] = {"OK", "NoDevice", "ViewFail", "ProjFail", "VPFail", "BehindCam"};
                DebugRenderer::DrawTextF(startX + 10, y, w2sOk ? 0xFF00FF00 : 0xFFFF0000,
                    "W2S: %s -> (%.0f,%.0f) Pos:(%.1f,%.1f,%.1f)",
                    failReasons[failReason], sx, sy, ent.position.x, ent.position.y, ent.position.z);
                y += lineH;

                // Show view matrix diagonal
                DebugRenderer::DrawTextF(startX + 10, y, 0xFF88FFFF,
                    "View: %.2f %.2f %.2f %.2f", view->_11, view->_22, view->_33, view->_44);
                y += lineH;

                // Show projection matrix diagonal
                DebugRenderer::DrawTextF(startX + 10, y, 0xFFFF88FF,
                    "Proj: %.2f %.2f %.2f %.2f", proj->_11, proj->_22, proj->_33, proj->_44);
                y += lineH;
            }
        }

    } else if (!player.valid) {
        // Show debug info about pointer chain
        DebugRenderer::DrawTextF(startX + 10, y, 0xFFFF4444, "Player: Not found (step %d failed)", dbgInfo.failStep);
        y += lineH;
        DebugRenderer::DrawTextF(startX + 10, y, 0xFFAA88FF, "Base: %08X  GS=%08X",
            dbgInfo.baseAddress, dbgInfo.gameStateValue);
        y += lineH;
        // Show player manager info (DAT_00cd8048)
        DebugRenderer::DrawTextF(startX + 10, y, 0xFFAA88FF, "PlayerMgr@%08X=%08X  Player@+162C=%08X",
            dbgInfo.playerCtrlAddr, dbgInfo.playerCtrlValue, dbgInfo.creatureValue);
        y += lineH;

        // Show scan of player manager structure around 0x162c
        if (dbgInfo.playerCtrlValue != 0) {
            DebugRenderer::DrawText(startX + 10, y, 0xFFFFAA00, "PlayerMgr scan (0x1620-0x165C):");
            y += lineH;
            // Show 4 values per line, 4 rows = 16 values
            for (int row = 0; row < 4; row++) {
                int i = row * 4;
                DebugRenderer::DrawTextF(startX + 10, y, 0xFF88FF88,
                    "+%04X:%08X +%04X:%08X +%04X:%08X +%04X:%08X",
                    dbgInfo.scanOffsets[i], dbgInfo.scanValues[i],
                    dbgInfo.scanOffsets[i+1], dbgInfo.scanValues[i+1],
                    dbgInfo.scanOffsets[i+2], dbgInfo.scanValues[i+2],
                    dbgInfo.scanOffsets[i+3], dbgInfo.scanValues[i+3]);
                y += lineH;
            }
        }

        // Show player/creature structure scan
        if (dbgInfo.scan2Base != 0) {
            DebugRenderer::DrawTextF(startX + 10, y, 0xFFFFAA00, "Player@%08X scan (0x00-0x3C):", dbgInfo.scan2Base);
            y += lineH;
            // Show 4 values per line, 4 rows = 16 values
            for (int row = 0; row < 4; row++) {
                int i = row * 4;
                DebugRenderer::DrawTextF(startX + 10, y, 0xFFAAFF88,
                    "+%02X:%08X +%02X:%08X +%02X:%08X +%02X:%08X",
                    dbgInfo.scan2Offsets[i], dbgInfo.scan2Values[i],
                    dbgInfo.scan2Offsets[i+1], dbgInfo.scan2Values[i+1],
                    dbgInfo.scan2Offsets[i+2], dbgInfo.scan2Values[i+2],
                    dbgInfo.scan2Offsets[i+3], dbgInfo.scan2Values[i+3]);
                y += lineH;
            }
        }
    }

    // === AUDIO DEBUG PANEL (F7 toggle) - Right side of screen ===
    if (g_Options.displayAudio) {
        // Get screen dimensions for right-side positioning
        D3DVIEWPORT9 viewport;
        device->GetViewport(&viewport);
        int screenW = viewport.Width;
        int screenH = viewport.Height;

        // Audio panel dimensions - compact and on the right edge
        const int audioLineH = Config::AUDIO_LINE_HEIGHT;
        int audioPanelW = Config::AUDIO_PANEL_WIDTH;
        int audioPanelH = (Config::AUDIO_EVENTS_SHOWN + 5) * audioLineH + 20;  // 5 header lines + events
        int audioX = screenW - audioPanelW - Config::AUDIO_PANEL_MARGIN;
        int audioY = Config::AUDIO_PANEL_MARGIN;

        // Semi-transparent dark background
        DebugRenderer::DrawFilledRect((float)audioX, (float)audioY, (float)audioPanelW, (float)audioPanelH, 0xB0101020);
        DebugRenderer::DrawRect((float)audioX, (float)audioY, (float)audioPanelW, (float)audioPanelH, 0xFF4488AA);

        int ay = audioY + 6;
        int padX = 8;

        // Compact header
        DebugRenderer::DrawText(audioX + padX, ay, 0xFF4488FF, "Audio [F7]");
        ay += audioLineH;

        // Show audio status - compact single line
        bool audioEnabled = AudioHook::IsAudioEnabled();
        bool filterOn = AudioHook::IsFilterNoisy();
        DWORD filteredCount = AudioHook::GetFilteredEventCount();
        DWORD unknownCount = AudioHook::GetUnknownEventCount();
        DebugRenderer::DrawTextF(audioX + padX, ay, audioEnabled ? 0xFF88FF88 : 0xFFFF6666,
            "%s | %d evts | Filter[F9]:%s(%d)",
            audioEnabled ? "ON" : "OFF",
            AudioHook::GetTotalEventCount(),
            filterOn ? "Y" : "N",
            filteredCount);
        ay += audioLineH;

        // Show unknown event count (priority for identification)
        if (unknownCount > 0) {
            DebugRenderer::DrawTextF(audioX + padX, ay, 0xFFFF44FF,
                "? Unknown: %d (need identification)", unknownCount);
            ay += audioLineH;
        }

        // Show loaded banks in compact form
        int loadedBankCount = AudioHook::GetLoadedBankCount();
        if (loadedBankCount > 0) {
            const char* loadedBanks[32] = {nullptr};
            int gotCount = AudioHook::GetLoadedBanks(loadedBanks, 32);

            // Show first 4 banks + count
            char bankStr[200] = "";
            int shown = 0;
            for (int i = 0; i < gotCount && shown < 4; i++) {
                if (shown > 0) strcat_s(bankStr, ",");
                strcat_s(bankStr, loadedBanks[i]);
                shown++;
            }
            if (gotCount > 4) {
                char more[16];
                sprintf_s(more, "+%d", gotCount - 4);
                strcat_s(bankStr, more);
            }
            DebugRenderer::DrawTextF(audioX + padX, ay, 0xFFBB9944, "Banks: %s", bankStr);
            ay += audioLineH;
        }

        // Separator line
        DebugRenderer::DrawFilledRect((float)(audioX + padX), (float)ay, (float)(audioPanelW - padX * 2), 1.0f, 0xFF444466);
        ay += 4;

        DebugRenderer::DrawText(audioX + padX, ay, 0xFF888899, "Recent [F8 export]");
        ay += audioLineH;

        // Get recent events
        int eventCount = 0;
        const AudioHook::AudioEvent* events = AudioHook::GetRecentEvents(&eventCount);

        // Show more events in compact format
        int showCount = Config::AUDIO_EVENTS_SHOWN;
        int startIdx = (eventCount >= showCount) ? eventCount - showCount : 0;
        for (int i = eventCount - 1; i >= startIdx && i >= 0; i--) {
            const AudioHook::AudioEvent& evt = events[i % AudioHook::MAX_EVENTS];
            if (evt.timestamp == 0) continue;

            DWORD age = GetTickCount() - evt.timestamp;

            // Check if this is an uncracked event (TXTP name with no semantic name)
            // Uncracked events show as "BankName-NNNN" or "BankName::0xHASH"
            bool isUncracked = false;
            const char* displayName = evt.bankName;
            if (displayName && displayName[0]) {
                // Check for patterns: contains "-0" followed by digits OR contains "0x"
                const char* dash = strstr(displayName, "-0");
                const char* hex = strstr(displayName, "0x");
                if ((dash && dash[1] == '0' && dash[2] >= '0' && dash[2] <= '9') || hex) {
                    isUncracked = true;
                }
            }

            // Color coding:
            // MAGENTA = uncracked event (priority for identification!)
            // GREEN = fresh cracked event
            // YELLOW = recent cracked event
            // GRAY = old cracked event
            DWORD ageColor;
            if (isUncracked) {
                ageColor = 0xFFFF44FF;  // Bright magenta for unknown events
            } else {
                ageColor = (age < 300) ? 0xFF00FF00 : (age < 1500) ? 0xFFCCCC00 : 0xFF666666;
            }

            // Compact format: age + name (prefix with ? for uncracked)
            if (evt.eventName[0] != '\0') {
                DebugRenderer::DrawTextF(audioX + padX, ay, ageColor,
                    "%s%3dms %s|%s", isUncracked ? "?" : "", age, evt.bankName, evt.eventName);
            } else {
                DebugRenderer::DrawTextF(audioX + padX, ay, ageColor,
                    "%s%3dms %s", isUncracked ? "?" : "", age, evt.bankName);
            }
            ay += audioLineH;
        }
    }

    // === ASSET BROWSER PANEL (F10 toggle) - Left side below main overlay ===
    if (g_Options.displayAssetBrowser) {
        // Get screen dimensions
        D3DVIEWPORT9 viewport;
        device->GetViewport(&viewport);
        int screenH = viewport.Height;

        // Asset browser panel dimensions
        const int abLineH = Config::AUDIO_LINE_HEIGHT;
        int abPanelW = 420;
        int abPanelH = 400;
        int abX = Config::OVERLAY_MARGIN_X;
        int abY = screenH - abPanelH - Config::OVERLAY_MARGIN_Y;

        // Semi-transparent dark background
        DebugRenderer::DrawFilledRect((float)abX, (float)abY, (float)abPanelW, (float)abPanelH, 0xB0102010);
        DebugRenderer::DrawRect((float)abX, (float)abY, (float)abPanelW, (float)abPanelH, 0xFF44AA44);

        int aby = abY + 6;
        int padX = 8;

        // Header
        DebugRenderer::DrawText(abX + padX, aby, 0xFF44FF44, "Asset Browser [F10]");
        aby += abLineH;

        // Controls help
        DebugRenderer::DrawText(abX + padX, aby, 0xFF888888,
            "UP/DOWN=Select  ENTER=Play  END=Stop All");
        aby += abLineH;

        // Summary stats
        int bankCount = AudioHook::GetTotalTrackedBankCount();
        int eventCount = AudioHook::GetTotalTrackedEventCount();
        DWORD lastGameObj = AudioHook::GetLastValidGameObjectId();
        DebugRenderer::DrawTextF(abX + padX, aby, 0xFFAAFFAA,
            "Banks: %d | GameObj: %u", bankCount, lastGameObj);
        aby += abLineH;

        // Separator
        DebugRenderer::DrawFilledRect((float)(abX + padX), (float)aby, (float)(abPanelW - padX * 2), 1.0f, 0xFF446644);
        aby += 4;

        DebugRenderer::DrawText(abX + padX, aby, 0xFF888899, "Events (from event_mapping):");
        aby += abLineH;

        // Show events from event_mapping.h with selection
        int mappingCount = (int)g_EventMappingCount;

        int startIdx = (g_SelectedEventIndex / 12) * 12; // Page of 12 events
        int showCount = 12;

        for (int i = 0; i < showCount && (startIdx + i) < mappingCount; i++) {
            int idx = startIdx + i;
            const EventMappingEntry& evt = g_EventMappingData[idx];

            bool isSelected = (idx == g_SelectedEventIndex);
            DWORD color = isSelected ? 0xFFFFFF00 : 0xFF88CC88;
            const char* prefix = isSelected ? "> " : "  ";

            DebugRenderer::DrawTextF(abX + padX, aby, color,
                "%s%s (%s)", prefix, evt.name, evt.bank);
            aby += abLineH;
        }

        // Show page info
        int totalPages = (mappingCount + 11) / 12;
        int currentPage = (g_SelectedEventIndex / 12) + 1;
        DebugRenderer::DrawTextF(abX + padX, aby, 0xFF666666,
            "Page %d/%d (Total: %d events)", currentPage, totalPages, mappingCount);
        aby += abLineH;

        // Separator
        DebugRenderer::DrawFilledRect((float)(abX + padX), (float)aby, (float)(abPanelW - padX * 2), 1.0f, 0xFF446644);
        aby += 4;

        // Loaded Banks section
        DebugRenderer::DrawText(abX + padX, aby, 0xFF888899, "Loaded Banks:");
        aby += abLineH;

        // Get tracked banks
        AudioHook::BankAssetInfo banks[32];
        int gotBanks = AudioHook::GetTrackedBanks(banks, 32);

        // Show banks (max 8 to fit in panel)
        int showMax = 8;
        for (int i = 0; i < gotBanks && i < showMax; i++) {
            const AudioHook::BankAssetInfo& bank = banks[i];
            DWORD color = bank.isLoaded ? 0xFF88FF88 : 0xFF666666;
            DebugRenderer::DrawTextF(abX + padX, aby, color,
                "  %s", bank.bankName);
            aby += abLineH;
        }

        if (gotBanks > showMax) {
            DebugRenderer::DrawTextF(abX + padX, aby, 0xFF666666,
                "  ... +%d more", gotBanks - showMax);
        } else if (gotBanks == 0) {
            DebugRenderer::DrawText(abX + padX, aby, 0xFF666666, "  (No banks loaded yet)");
        }
    }

    DebugRenderer::EndFrame();

    // Debug menu
    if (g_Options.showDebugMenu) {
        DrawDebugMenu(startX, startY + boxHeight + 10);
    }
}

// ============================================================================
// Handle keyboard input for toggling options
// ============================================================================
void HandleInput(IDirect3DDevice9* device) {
    // F1 - Toggle overlay visibility
    if (Input::IsKeyPressed(Config::KEY_TOGGLE_OVERLAY)) {
        g_Options.overlayEnabled = !g_Options.overlayEnabled;
    }

    // F4 - Toggle wireframe mode (uses BeginScene hook for proper timing)
    if (Input::IsKeyPressed(Config::KEY_TOGGLE_WIREFRAME)) {
        g_Options.wireframeMode = !g_Options.wireframeMode;
        D3D9Hook::SetWireframe(g_Options.wireframeMode);
    }

    // F5 - Toggle debug menu
    if (Input::IsKeyPressed(Config::KEY_TOGGLE_MENU)) {
        g_Options.showDebugMenu = !g_Options.showDebugMenu;
    }

    // F6 - Toggle collision boxes
    if (Input::IsKeyPressed(Config::KEY_TOGGLE_COLLISION)) {
        g_Options.displayCollision = !g_Options.displayCollision;
    }

    // F7 - Toggle audio debug
    if (Input::IsKeyPressed(Config::KEY_TOGGLE_AUDIO)) {
        g_Options.displayAudio = !g_Options.displayAudio;
    }

    // F8 - Export captured audio names
    if (Input::IsKeyPressed(VK_F8)) {
        int count = AudioHook::ExportCapturedNames("captured_audio_names.txt");
        char buf[128];
        sprintf_s(buf, "[DebugOverlay] Exported %d captured audio names\n", count);
        OutputDebugStringA(buf);
    }

    // F9 - Toggle noisy event filter
    if (Input::IsKeyPressed(VK_F9)) {
        bool newState = !AudioHook::IsFilterNoisy();
        AudioHook::SetFilterNoisy(newState);
        char buf[128];
        sprintf_s(buf, "[DebugOverlay] Noisy event filter %s\n", newState ? "ON" : "OFF");
        OutputDebugStringA(buf);
    }

    // NUMPAD 0 - Stop all sounds via Wwise StopAll API
    if (Input::IsKeyPressed(VK_NUMPAD0)) {
        AudioHook::StopAllSounds(0);
        OutputDebugStringA("[DebugOverlay] NUMPAD0: StopAllSounds(0)\n");
    }

    // NUMPAD 1 - Play "stop_all" event
    if (Input::IsKeyPressed(VK_NUMPAD1)) {
        AudioHook::PlayEventByName("stop_all", 0);
        OutputDebugStringA("[DebugOverlay] NUMPAD1: stop_all\n");
    }

    // NUMPAD 2 - Play "stop_music" event
    if (Input::IsKeyPressed(VK_NUMPAD2)) {
        AudioHook::PlayEventByName("stop_music", 0);
        OutputDebugStringA("[DebugOverlay] NUMPAD2: stop_music\n");
    }

    // NUMPAD 3 - Play "stop_ambience" event
    if (Input::IsKeyPressed(VK_NUMPAD3)) {
        AudioHook::PlayEventByName("stop_ambience", 0);
        OutputDebugStringA("[DebugOverlay] NUMPAD3: stop_ambience\n");
    }

    // F10 - Toggle asset browser
    if (Input::IsKeyPressed(Config::KEY_TOGGLE_ASSET_BROWSER)) {
        g_Options.displayAssetBrowser = !g_Options.displayAssetBrowser;
    }

    // Asset Browser controls (only when panel is visible)
    if (g_Options.displayAssetBrowser) {
        int eventCount = (int)g_EventMappingCount;

        // UP - Select previous event
        if (Input::IsKeyPressed(VK_UP)) {
            g_SelectedEventIndex--;
            if (g_SelectedEventIndex < 0) {
                g_SelectedEventIndex = eventCount - 1;
            }
        }

        // DOWN - Select next event
        if (Input::IsKeyPressed(VK_DOWN)) {
            g_SelectedEventIndex++;
            if (g_SelectedEventIndex >= eventCount) {
                g_SelectedEventIndex = 0;
            }
        }

        // PAGE UP - Jump 12 events back
        if (Input::IsKeyPressed(VK_PRIOR)) {
            g_SelectedEventIndex -= 12;
            if (g_SelectedEventIndex < 0) {
                g_SelectedEventIndex = 0;
            }
        }

        // PAGE DOWN - Jump 12 events forward
        if (Input::IsKeyPressed(VK_NEXT)) {
            g_SelectedEventIndex += 12;
            if (g_SelectedEventIndex >= eventCount) {
                g_SelectedEventIndex = eventCount - 1;
            }
        }

        // ENTER - Play selected event
        if (Input::IsKeyPressed(VK_RETURN)) {
            if (g_SelectedEventIndex >= 0 && g_SelectedEventIndex < eventCount) {
                const EventMappingEntry& evt = g_EventMappingData[g_SelectedEventIndex];
                DWORD playingId = AudioHook::PlayEvent(evt.id);
                char buf[128];
                sprintf_s(buf, "[DebugOverlay] Playing: %s (0x%08X) -> %u\n",
                    evt.name, evt.id, playingId);
                OutputDebugStringA(buf);
            }
        }

        // DELETE - Stop all sounds
        if (Input::IsKeyPressed(VK_DELETE)) {
            AudioHook::StopAllSounds(0);
            OutputDebugStringA("[DebugOverlay] Stopped all sounds\n");
        }
    }
}

// ============================================================================
// Calculate FPS
// ============================================================================
void UpdateFPS() {
    g_FrameCounter++;
    DWORD currentTime = GetTickCount();
    
    if (currentTime - g_LastFPSTime >= 1000) {
        g_FPS = g_FrameCounter;
        g_FrameCounter = 0;
        g_LastFPSTime = currentTime;
    }
}

// ============================================================================
// Draw the debug options menu
// ============================================================================
void DrawDebugMenu(int x, int y) {
    // Background
    DebugRenderer::DrawFilledRect((float)x - 5, (float)y - 5, 280, 150,
        Config::CLR_BACKGROUND);

    DebugRenderer::DrawTextShadowed(x, y, Config::CLR_HIGHLIGHT, "=== Debug Menu ===");
    y += Config::LINE_HEIGHT;

    // Display current toggle states
    auto drawOption = [&](const char* name, bool enabled) {
        DWORD color = enabled ? Config::CLR_HIGHLIGHT : Config::CLR_TEXT;
        DebugRenderer::DrawTextF(x, y, color, "[%c] %s", enabled ? 'X' : ' ', name);
        y += Config::LINE_HEIGHT;
    };

    drawOption("Wireframe Mode (F4)", g_Options.wireframeMode);
    drawOption("Collision Boxes (F6)", g_Options.displayCollision);

    y += Config::LINE_HEIGHT;
    DebugRenderer::DrawTextShadowed(x, y, 0xFFAAAAAA, "Future features:");
    y += Config::LINE_HEIGHT;
    DebugRenderer::DrawText(x, y, 0xFF888888, "- NPC Info overlay");
    y += Config::LINE_HEIGHT;
    DebugRenderer::DrawText(x, y, 0xFF888888, "- Pathfinding viz");
}

// ============================================================================
// DLL Initialization Thread
// ============================================================================
DWORD WINAPI InitThread(LPVOID lpParam) {
    // Wait for game to initialize D3D
    Sleep(3000);

    // Initialize our systems
    Input::Initialize();

    // Initialize D3D9 hook
    if (!D3D9Hook::Initialize()) {
        MessageBoxA(NULL, "Failed to initialize D3D9 hook!", "Debug Overlay", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize audio hook
    if (!AudioHook::Initialize()) {
        // Non-fatal, just log and continue
        OutputDebugStringA("[DebugOverlay] Warning: Failed to initialize audio hook\n");
    }

    // Set our render callback
    D3D9Hook::SetRenderCallback(RenderOverlay);

    return 0;
}

// ============================================================================
// DLL Entry Point
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CreateThread(NULL, 0, InitThread, hModule, 0, NULL);
            break;

        case DLL_PROCESS_DETACH:
            AudioHook::Shutdown();
            D3D9Hook::Shutdown();
            DebugRenderer::Release();
            break;
    }
    return TRUE;
}

