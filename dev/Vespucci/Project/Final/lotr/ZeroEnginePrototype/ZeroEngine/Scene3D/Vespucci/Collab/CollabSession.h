// CollabSession.h -Live collaborative editing over TCP on LAN.
//
// Two editor instances peer up: one clicks Host Session, the other clicks Join.
// After handshake, edits on either side broadcast to peers in real-time.
//
// This Chunk 1 file covers only the TCP transport + handshake + PAK-match gate.
// Broadcast/replay of actual edits (FieldEdit, Add/Delete) lands in Chunk 2-3.
//
// Architectural notes:
//   - Host is authoritative. Host assigns each client a GUID prefix byte so
//     concurrent AddPendingEntity on different peers produce non-colliding GUIDs.
//   - Same-LAN only; manual IP:port entry; no NAT traversal.
//   - Message framing reuses the pattern from vespucci_bridge.h:
//       [u32 msgType][u32 payloadSize][payload bytes]

#ifndef COLLAB_SESSION_H
#define COLLAB_SESSION_H

#include <stdint.h>
#include <string>
#include <vector>

namespace ZeroEngine {

// Forward-declared so we don't drag the full 900-line LevelReader.h into
// every file that includes CollabSession.h. The actual definitions live in
// LevelReader.h and are brought in by CollabSession.cpp only.
class  LevelReader;
struct FieldEdit;
struct PendingGameObj;

// Session role.
enum CollabRole {
    COLLAB_ROLE_NONE   = 0,  // solo (not in a session)
    COLLAB_ROLE_HOST   = 1,
    COLLAB_ROLE_CLIENT = 2
};

// High-level status for toolbar display.
enum CollabStatus {
    COLLAB_STATUS_IDLE        = 0,  // solo / disconnected
    COLLAB_STATUS_HOSTING     = 1,  // hosting, waiting for/serving peers
    COLLAB_STATUS_CONNECTING  = 2,  // client: dialling host
    COLLAB_STATUS_HANDSHAKE   = 3,  // transport up, negotiating
    COLLAB_STATUS_RECEIVING   = 4,  // client: streaming PAK from host
    COLLAB_STATUS_CONNECTED   = 5,  // handshake done, in real-time sync
    COLLAB_STATUS_ERROR       = 6   // last error in GetStatusText()
};

// Message types on the wire. 0xCE.. prefix so they can't collide with the
// vespucci_bridge game IPC channel if anyone ever muxes them.
enum CollabMsg {
    CM_HELLO              = 0xCE01,  // client → host: peer name, PAK hash, needPakStream flag
    CM_WELCOME            = 0xCE02,  // host   → client: assigned guid prefix, willStreamPak
    CM_ERR_PAK_MISMATCH   = 0xCE03,  // host   → client: refuse, hashes don't match
    CM_ERR_FULL           = 0xCE04,  // host   → client: too many clients
    CM_PING               = 0xCE05,
    CM_PONG               = 0xCE06,
    // PAK file streaming -for clients joining without a local copy of the level.
    // Order: BEGIN(PAK) → CHUNK*N → END(0) → BEGIN(BIN) → CHUNK*N → END(1) → END(2).
    CM_PAK_BEGIN          = 0xCE10,
    CM_PAK_CHUNK          = 0xCE11,
    CM_PAK_END            = 0xCE12,
    CM_ERR_NOTHING_LOADED = 0xCE13,  // host had no level loaded but client asked for stream
    // ── Chunk 3: live edit broadcast ────────────────────────────────────
    // Every goddamn DragFloat, every connection wire, every Revert, every
    // Wipe. These are the messages that make the session actually COLLAB
    // instead of a glorified file-sharing drop. Payloads are all in the
    // same host-native little-endian format (x86-only, we are not porting
    // this to the fucking Cell processor). See CollabSession.cpp
    // Serialize/Deserialize pairs for wire layout.
    CM_FIELD_EDIT         = 0xCE20,  // (either direction) FieldEdit record
    CM_ADD_ENTITY         = 0xCE21,  // (either direction) full PendingGameObj
    CM_DELETE_GUID        = 0xCE22,  // (either direction) uint32 guid
    CM_CLEAR_FIELD_EDITS  = 0xCE23   // (either direction) wipes all field edits
};

// Public API -called from the viewport main thread each frame.
class CollabSession {
public:
    // Lifetime is process-wide; there's one instance, accessed via the getter
    // below. The ZeroEngine3DViewport main loop calls Tick() each frame.
    static CollabSession& Instance();

    // ── Lifecycle ──────────────────────────────────────────────────────────
    // Call once at editor startup; succeeds or logs and stays idle.
    bool Init();
    // Call at editor shutdown.
    void Shutdown();

    // ── Session operations ────────────────────────────────────────────────
    // Start hosting on `port` (default 1818). Returns false if a socket is
    // already bound or port is in use.
    bool HostStart(uint16_t port);
    // Connect as a client to `ip` : `port`. Returns immediately; use
    // GetStatus() to see progress.
    //
    // needPakStream:
    //   false -client has the same PAK loaded locally; PAK hash gate enforced
    //   true  -client has nothing loaded; host streams its PAK + BIN files
    //           before the session goes to CONNECTED. While streaming, status
    //           reads RECEIVING and GetXferProgress() returns 0.0..1.0.
    bool JoinStart(const char* ip, uint16_t port, bool needPakStream);

    // End whatever session is active and return to IDLE.
    void Stop();

    // ── Configuration ────────────────────────────────────────────────────
    // Tell the session the currently-loaded PAK's identity. Used by the
    // handshake to confirm both peers are editing the same file. Call any
    // time a level is loaded or reloaded.
    //   pakPath        -absolute path (for the UI only)
    //   pakHash        -32-bit content hash (e.g. LotrHashString of first 4KB)
    //   pakSizeBytes   -file size, used as a coarse tiebreaker
    void SetLoadedPak(const char* pakPath, uint32_t pakHash, uint64_t pakSizeBytes);

    // ── Polling (main thread only) ────────────────────────────────────────
    // Call each frame from the viewport main loop. Drains incoming messages
    // and applies any "apply on main thread" side effects.
    void Tick();

    // ── Status accessors (for UI) ─────────────────────────────────────────
    CollabRole   GetRole() const;
    CollabStatus GetStatus() const;
    const char*  GetStatusText() const;   // short message for toolbar
    int          GetPeerCount() const;    // host: # connected clients; client: 0 or 1
    uint16_t     GetPort() const;
    const char*  GetLocalIpText() const;  // "x.y.z.w" or "(unknown)"
    uint8_t      GetMyGuidPrefix() const; // 0 = host-native, >0 = client-assigned

    // PAK-streaming progress (client side, only meaningful while RECEIVING).
    float        GetXferProgress() const;     // 0.0 .. 1.0
    uint64_t     GetXferTotalBytes() const;   // PAK + BIN total, 0 if not streaming
    uint64_t     GetXferDoneBytes()  const;
    // True for one Tick() after a streamed transfer fully completes -the
    // viewport reads this and triggers LevelReader::Load(temp paths) on its
    // own thread (D3D resources need the main thread). Auto-cleared after read.
    bool         ConsumePakReady(std::string& outPakPath, std::string& outBinPath);

    // ── Chunk 3: Live edit broadcast ──────────────────────────────────────
    // The viewport calls SetLevelReader once at startup so Tick() knows
    // where to apply inbound edits. Without this the receive path has
    // nowhere to write to and the session is a one-way radio. If you forget
    // to wire this up, the client will happily accept every edit, the
    // session log will look perfect, and absolutely nothing will change on
    // screen. Been there. Connect the goddamn pointer.
    void         SetLevelReader(LevelReader* lr);

    // Broadcast methods -called from the LevelReader hook forwarders. The
    // viewport does NOT call these directly; it calls LevelReader which
    // fires the hook which calls these. One path, one choke point, no
    // way to edit state without the broadcast going out.
    //
    // Host:    serialize -> enqueue on every client's outqueue.
    // Client:  serialize -> enqueue on the one socket to host.
    //          Host receives, applies locally, AND re-broadcasts to other
    //          clients (the relay path). Author does not get their own
    //          edit echoed back because the apply path does not fire hooks.
    // No session:  no-op, which is why these are safe to call unconditionally.
    void         BroadcastFieldEdit(const FieldEdit& edit);
    void         BroadcastAddEntity(const PendingGameObj& obj);
    void         BroadcastDeleteGuid(uint32_t guid);
    void         BroadcastClearFieldEdits();

    // Impl is declared opaquely here and defined in the .cpp -kept public so
    // the .cpp file's free/helper functions (HostClientThread, SendMsg, etc.)
    // can access its members without friendship gymnastics.
    struct Impl;
    Impl* m_impl;

private:
    CollabSession();
    ~CollabSession();
    CollabSession(const CollabSession&);
    CollabSession& operator=(const CollabSession&);
};

} // namespace ZeroEngine

#endif // COLLAB_SESSION_H
