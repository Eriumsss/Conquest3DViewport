// Vespucci <-> running-game bridge client.
//
// Connects to the named pipe served by d3d9.dll inside the running
// LOTR:Conquest process. Reads live camera + entity state each frame and
// exposes it as a simple snapshot. The Scene3DRenderer calls Tick() once
// per frame to drain inbound messages, and Render() to draw ghost entities
// using the editor's live D3D9 device.

#pragma once

#include <d3d9.h>
#include <stdint.h>
#include <vector>

namespace GameBridge {

struct EntitySnap {
    uint32_t addr;
    float    pos[3];
    float    velocity[3];
    float    health;
    uint32_t team;
    uint32_t flags;  // bit0=player, bit1=hero
};

struct Snapshot {
    bool     connected;
    uint32_t gamePid;
    char     gameExe[64];

    // Live camera from the game (c239..c246).
    float    camPos[3];
    float    camFwd[3];
    float    camFov;
    uint32_t viewportW, viewportH;

    // Live player.
    bool     playerValid;
    float    playerPos[3];
    float    playerHp;
    uint32_t playerTeam;
    uint32_t playerCreaturePtr;

    // Live entity pool snapshot.
    std::vector<EntitySnap> entities;

    // Level/frame metadata.
    char     levelName[64];
    uint64_t lastFrameIdx;
};

// Call once at editor startup (spawns I/O thread — non-blocking).
void Init();

// Call once at editor shutdown.
void Shutdown();

// Call each frame BEFORE Render(). Copies the latest inbound snapshot
// from the I/O thread into a main-thread-owned buffer.
void Tick();

// Draw ghost entities + camera frustum using the editor's D3D9 device.
// Safe to call regardless of connection state (no-op when disconnected).
void Render(IDirect3DDevice9* device);

// Read-only accessor for the current snapshot (from main thread).
const Snapshot& CurrentSnapshot();

// Send a raw command to the running game (thread-safe).
void SendCommand(uint32_t msgType, const void* payload, uint32_t payloadSize);

}  // namespace GameBridge
