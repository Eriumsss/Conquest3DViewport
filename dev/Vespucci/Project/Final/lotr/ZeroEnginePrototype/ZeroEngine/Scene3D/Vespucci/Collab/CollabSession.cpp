// CollabSession.cpp -Chunk 1: TCP transport + handshake + PAK-match gate.
//
// Structure mirrors vespucci_bridge.cpp (named-pipe version) -same per-
// client thread model, same broadcast queue, same framing. Swaps named-pipe
// I/O for Winsock.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "CollabSession.h"
#include "LevelReader.h"

#pragma comment(lib, "ws2_32.lib")

namespace ZeroEngine {

// ── Wire format ──────────────────────────────────────────────────────────
// [u32 msgType][u32 payloadSize][payload bytes]
// Max payload: 16 MB per message.
#pragma pack(push, 1)
struct CsHeader {
    uint32_t type;
    uint32_t size;
};
#pragma pack(pop)
static const uint32_t kMaxPayload = 16 * 1024 * 1024;

// Protocol v3: Chunk 3 added CM_FIELD_EDIT / CM_ADD_ENTITY / CM_DELETE_GUID /
// CM_CLEAR_FIELD_EDITS on top of v2's handshake+streaming. The HELLO/WELCOME
// structs themselves did not grow - we could have slid new messages in
// without bumping the version byte - but any peer that speaks v2 has ZERO
// handler for the new types and would trip RecvMsg-with-unknown-type on
// the first wire that gets edited. Fail fast at the handshake instead of
// bleeding weird behavior for hours. No backwards compat with v2. Both
// peers ship with the same Vespucci build or they don't talk, period.
// We're two laptops on a LAN, not Steam's matchmaker trying to keep 2009
// clients happy. Bump this number every time the wire surface changes and
// do not lose any sleep over it.
static const uint32_t kProtocolVersion = 3;

#pragma pack(push, 1)
struct CsHello {
    uint32_t protocolVersion;
    uint32_t pakHash;       // 0 if needPakStream=1 (we have nothing to hash)
    uint64_t pakSizeBytes;  // 0 if needPakStream=1
    char     peerName[64];
    uint8_t  needPakStream; // 1 = "I'm a fresh peer, send me your PAK + BIN"
    uint8_t  pad[7];
};
struct CsWelcome {
    uint32_t protocolVersion;
    uint8_t  assignedGuidPrefix;
    uint8_t  willStreamPak;  // 1 = host is about to fire CM_PAK_BEGIN
    uint8_t  pad[2];
    char     hostName[64];
};
// PAK_BEGIN: "I'm about to send N bytes for file X". Sent twice per transfer
// (once for PAK, once for BIN). Each followed by N CHUNKs and one END.
struct CsPakBegin {
    uint8_t  fileKind;       // 0=PAK, 1=BIN
    uint8_t  pad[3];
    uint64_t totalBytes;
    uint32_t fileHash;       // simple sanity hash; not crypto, just "did we get all of it"
    char     fileName[128];  // basename only -no paths, no traversal funny business
};
// PAK_CHUNK: a slice. Variable length -header + `size` bytes of file data.
struct CsPakChunk {
    uint8_t  fileKind;
    uint8_t  pad[3];
    uint64_t offset;
    uint32_t size;
    // ... `size` bytes of payload follow
};
// PAK_END: "done with this file" or "all files done"
struct CsPakEnd {
    uint8_t fileKind;        // 0=PAK done, 1=BIN done, 2=ALL done (transfer complete, switch to CONNECTED)
    uint8_t pad[3];
};
#pragma pack(pop)

// 64KB chunks. Small enough that the progress bar actually moves between
// frames and the user doesn't think we shit the bed. Big enough that we're
// not drowning the network thread in framing overhead for no fucking reason.
// TCP coalesces writes anyway so this number is really about progress-bar
// granularity, not throughput. Don't go above 1MB (header sizeof in the
// CsPakChunk will start lying) and don't go below 4KB (framing crushes you).
static const uint32_t kChunkSize = 64 * 1024;

// ═════════════════════════════════════════════════════════════════════════
//  Chunk 3: outbound/inbound queues + blob-based framing
// ═════════════════════════════════════════════════════════════════════════
//
// A CsPacket is one fully-framed message, header GLUED to payload with
// fucking concrete, ready to be spat down the socket as a single send().
// Why pre-glue? Because I spent a genuinely embarrassing amount of late
// nights chasing a bug where one thread called send(header), the OS
// yielded, ANOTHER thread called send(different_header), and by the time
// thread #1 came back to send its payload the TCP stream was corrupted
// past the fucking event horizon. The receiver parsed header A followed
// by payload B, the size field lied, half a megabyte of what should
// have been a FieldEdit got chewed up as a PAK chunk, every subsequent
// message was offset by a random goddamn number of bytes, and the
// session went from "working fine" to "catastrophic garbage spewing
// out of every socket" in exactly ONE multi-threaded send collision.
//
// The solution is stupidly simple and the kind of thing I should have
// done from the start: ONE send() per packet, header + payload fused
// into one contiguous blob. The TCP layer either takes the whole blob
// atomically or returns an error and we tear the fucking session down.
// No middle ground. No torn writes. No race where two threads fight
// over the socket at 3am. And most importantly, no more me staring at
// Wireshark at some ungodly hour trying to figure out why FieldEdit
// messages are coming out of the other end shaped like cursed PAK
// chunks. NEVER. FUCKING. AGAIN.
//
// vector<uint8_t> costs a heap allocation per packet. Sixty allocs per
// second at 60 Hz slider drag, which is absolutely nothing - malloc on
// a modern Windows handles this shit in its sleep, Windows allocator
// is not your fucking bottleneck. If some lunatic ever runs an edit
// firehose that overwhelms that, we pool. Until then, premature
// optimization is the root of all evil and I've got bigger monsters to
// slay in this codebase than a sixty-byte heap alloc.
struct CsPacket {
    std::vector<uint8_t> bytes;
};

// Forward-decl - SendAll's definition lives further down with the rest of
// the socket plumbing. The queue drainer needs it a few functions earlier
// and reordering the whole file for one compile-order nitpick is worse
// than a one-line forward decl.
static bool SendAll(SOCKET s, const void* data, int size);

// Pop the whole queue under the lock, then send outside the lock. This
// keeps the critical section tight (microseconds) and lets the send()
// calls run concurrently with any enqueue coming from the main thread.
// If the socket blocks mid-batch we still drop the rest of this batch
// on the floor and return false - caller (the per-client thread) then
// tears down the connection. Nothing fancy.
static bool DrainOutqueue(SOCKET s, std::vector<CsPacket>& queue, CRITICAL_SECTION& cs) {
    std::vector<CsPacket> local;
    EnterCriticalSection(&cs);
    local.swap(queue);
    LeaveCriticalSection(&cs);
    for (size_t i = 0; i < local.size(); ++i) {
        const std::vector<uint8_t>& b = local[i].bytes;
        if (b.empty()) continue;
        if (!SendAll(s, &b[0], (int)b.size())) return false;
    }
    return true;
}

// Build a framed packet into a byte vector. Result is ready to shove onto
// any outqueue. Keeps the header+payload contiguous - one send, no tearing.
static void BuildPacket(std::vector<uint8_t>& out, uint32_t type, const void* payload, uint32_t size) {
    out.resize(sizeof(CsHeader) + size);
    CsHeader h; h.type = type; h.size = size;
    memcpy(&out[0], &h, sizeof(h));
    if (size > 0 && payload) memcpy(&out[sizeof(CsHeader)], payload, size);
}

// ═════════════════════════════════════════════════════════════════════════
//  Write helpers: append-to-byte-vector builders for our wire blobs
// ═════════════════════════════════════════════════════════════════════════
// All little-endian x86-to-x86, NO FUCKING endian swaps. This editor runs
// on Windows x86 talking to Windows x86 because that is what Pandemic
// built the goddamn game on and that is what we are reverse-engineering.
// If some intrepid masochist ever tries to port this to ARM or to some
// cursed PowerPC box they will need to wrap every WrU32 / RdU32 in
// htonl / ntohl and audit the float byte order while they are at it.
// Not my problem. Not today's problem. Not this decade's problem. If
// you are reading this in the year 2035 on a RISC-V laptop trying to
// figure out why the session hangs, hello, good luck, I am sorry.
static void WrU32(std::vector<uint8_t>& b, uint32_t v) {
    size_t n = b.size(); b.resize(n + 4); memcpy(&b[n], &v, 4);
}
static void WrI32(std::vector<uint8_t>& b, int32_t v) {
    size_t n = b.size(); b.resize(n + 4); memcpy(&b[n], &v, 4);
}
static void WrF32(std::vector<uint8_t>& b, float v) {
    size_t n = b.size(); b.resize(n + 4); memcpy(&b[n], &v, 4);
}
static void WrBytes(std::vector<uint8_t>& b, const void* p, size_t n) {
    size_t o = b.size(); b.resize(o + n); if (n) memcpy(&b[o], p, n);
}
static void WrStr(std::vector<uint8_t>& b, const std::string& s) {
    WrU32(b, (uint32_t)s.size());
    WrBytes(b, s.empty() ? NULL : s.data(), s.size());
}

// Read helpers. Return false if you would run off the end of the buffer -
// callers bail and log. These are NOT paranoid about alignment because
// x86 handles unaligned loads natively; if this codebase ever ends up on
// a platform that does not, everything else breaks first.
static bool RdU32(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    if (end - p < 4) return false; memcpy(&out, p, 4); p += 4; return true;
}
static bool RdI32(const uint8_t*& p, const uint8_t* end, int32_t& out) {
    if (end - p < 4) return false; memcpy(&out, p, 4); p += 4; return true;
}
static bool RdF32(const uint8_t*& p, const uint8_t* end, float& out) {
    if (end - p < 4) return false; memcpy(&out, p, 4); p += 4; return true;
}
static bool RdBytes(const uint8_t*& p, const uint8_t* end, void* out, size_t n) {
    if ((size_t)(end - p) < n) return false; memcpy(out, p, n); p += n; return true;
}
static bool RdStr(const uint8_t*& p, const uint8_t* end, std::string& out) {
    uint32_t len = 0; if (!RdU32(p, end, len)) return false;
    if ((size_t)(end - p) < len) return false;
    out.assign((const char*)p, len); p += len; return true;
}

// ═════════════════════════════════════════════════════════════════════════
//  Serialize: FieldEdit, PendingGameObj, DeleteGuid
// ═════════════════════════════════════════════════════════════════════════
//
// Wire layout mirrors the struct one-to-one. Everything that can differ
// in size (strings, the floatArray kind=7, fieldData, every override
// map) is length-prefixed. Simple. Flat. Boring on purpose.
//
// CRITICAL WARNING WITH CAPS LOCK ATTACHED: if you change ANYTHING in
// FieldEdit or PendingGameObj - add a field, remove a field, change a
// type from uint32 to int32, whatever - you MUST change BOTH the
// Serialize AND the Deserialize function AND bump kProtocolVersion.
// ALL THREE. Not two. Not "I will do the version later". ALL THREE.
// Skip one and here is the failure mode:
//
//   1. You add a field, update Serialize, forget Deserialize. Session
//      starts fine. Handshake passes. Streaming works. First edit fires
//      and the deserializer reads bytes OFF THE END of what it thinks
//      is this message and into the next message. Every subsequent
//      packet is parsed at a random offset. The session looks CONNECTED
//      the whole time because the TCP layer is fine. Users watch
//      random values flying into random fields, entities getting
//      deleted that should not be, no error message anywhere. Total
//      chaos dressed up as a working session.
//
//   2. You update both sides but forget the version bump. Now one peer
//      is speaking wire-vN and the other is speaking wire-vN-0.5 and
//      the handshake agrees because both said "3" and everybody thinks
//      they are compatible. Same chaos as above, just without the
//      consolation prize of "at least the version check caught it".
//
// So: ALL THREE. Serialize, Deserialize, version. Pair them in commits.
// If this comment one day ends with "except for that one time I
// forgot", that is not a celebration, that is a warning from the
// future about a bug we are STILL chasing.
static void SerializeFieldEdit(const FieldEdit& e, std::vector<uint8_t>& out) {
    WrU32(out, e.entityGuid);
    WrStr(out, e.fieldName);
    WrI32(out, e.kind);
    // Serialize only the value for this kind - no reason to ship 16 floats
    // of matrix data for an int edit. Saves bytes and keeps the parser
    // straightforward. Kind dispatch on deserialize matches this exactly.
    switch (e.kind) {
    case 0: WrI32(out, e.intVal);    break;
    case 1: WrF32(out, e.floatVal);  break;
    case 2: WrU32(out, e.guidVal);   break;
    case 3: for (int i = 0; i < 3;  ++i) WrF32(out, e.vec3Val[i]);   break;
    case 4: for (int i = 0; i < 16; ++i) WrF32(out, e.matrixVal[i]); break;
    case 5: WrStr(out, e.stringVal); break;
    case 6: WrI32(out, e.listIndex); break;
    case 7: {
        WrI32(out, e.arrayStride);
        WrU32(out, (uint32_t)e.arrayVal.size());
        for (size_t i = 0; i < e.arrayVal.size(); ++i) WrF32(out, e.arrayVal[i]);
        break;
    }
    default: break; // unknown kind - sent as-is header, body empty. Receiver logs+drops.
    }
}
static bool DeserializeFieldEdit(const uint8_t* p, const uint8_t* end, FieldEdit& e) {
    if (!RdU32(p, end, e.entityGuid))   return false;
    if (!RdStr(p, end, e.fieldName))    return false;
    if (!RdI32(p, end, e.kind))         return false;
    e.intVal = 0; e.floatVal = 0; e.guidVal = 0;
    memset(e.vec3Val, 0, sizeof(e.vec3Val));
    memset(e.matrixVal, 0, sizeof(e.matrixVal));
    e.stringVal.clear(); e.listIndex = 0; e.arrayVal.clear(); e.arrayStride = 0;
    switch (e.kind) {
    case 0: return RdI32(p, end, e.intVal);
    case 1: return RdF32(p, end, e.floatVal);
    case 2: return RdU32(p, end, e.guidVal);
    case 3: for (int i = 0; i < 3;  ++i) if (!RdF32(p, end, e.vec3Val[i]))   return false; return true;
    case 4: for (int i = 0; i < 16; ++i) if (!RdF32(p, end, e.matrixVal[i])) return false; return true;
    case 5: return RdStr(p, end, e.stringVal);
    case 6: return RdI32(p, end, e.listIndex);
    case 7: {
        if (!RdI32(p, end, e.arrayStride)) return false;
        uint32_t count = 0;
        if (!RdU32(p, end, count)) return false;
        e.arrayVal.resize(count);
        for (uint32_t i = 0; i < count; ++i) if (!RdF32(p, end, e.arrayVal[i])) return false;
        return true;
    }
    default: return true; // unknown kind kept as zeroes; apply side will noop or log.
    }
}

static void SerializePendingGameObj(const PendingGameObj& o, std::vector<uint8_t>& out) {
    // POD first - constant offsets, fast to memcpy in if we ever optimize.
    WrU32(out, o.guid);
    WrU32(out, o.parent_guid);
    WrU32(out, o.layer_guid);
    WrU32(out, o.name_crc);
    WrU32(out, o.type_crc);
    WrI32(out, o.gamemodemask);
    for (int i = 0; i < 16; ++i) WrF32(out, o.world_transform[i]);
    WrI32(out, o.type_def_index);
    WrU32(out, o.mesh_crc);
    WrU32(out, o.target_guid);
    WrI32(out, o.sticky);
    WrU32(out, o.owner_guid);
    // Then the variable-length stuff. Every one is length-prefixed so a
    // newer peer that added a field after layerGuids can still parse an
    // older peer's payload up to the point it stops recognizing - though
    // with kProtocolVersion matching strictly that should never happen.
    WrStr(out, o.name_str);
    WrStr(out, o.output_event);
    WrStr(out, o.input_action);
    WrU32(out, (uint32_t)o.fieldData.size());
    if (!o.fieldData.empty()) WrBytes(out, &o.fieldData[0], o.fieldData.size());
    // intOverrides
    WrU32(out, (uint32_t)o.intOverrides.size());
    for (std::map<uint32_t,uint32_t>::const_iterator it = o.intOverrides.begin(); it != o.intOverrides.end(); ++it) {
        WrU32(out, it->first); WrU32(out, it->second);
    }
    // floatOverrides
    WrU32(out, (uint32_t)o.floatOverrides.size());
    for (std::map<uint32_t,float>::const_iterator it = o.floatOverrides.begin(); it != o.floatOverrides.end(); ++it) {
        WrU32(out, it->first); WrF32(out, it->second);
    }
    // stringOverrides
    WrU32(out, (uint32_t)o.stringOverrides.size());
    for (std::map<uint32_t,std::string>::const_iterator it = o.stringOverrides.begin(); it != o.stringOverrides.end(); ++it) {
        WrU32(out, it->first); WrStr(out, it->second);
    }
    // layerGuids
    WrU32(out, (uint32_t)o.layerGuids.size());
    for (size_t i = 0; i < o.layerGuids.size(); ++i) WrU32(out, o.layerGuids[i]);
}

static bool DeserializePendingGameObj(const uint8_t* p, const uint8_t* end, PendingGameObj& o) {
    if (!RdU32(p, end, o.guid))        return false;
    if (!RdU32(p, end, o.parent_guid)) return false;
    if (!RdU32(p, end, o.layer_guid))  return false;
    if (!RdU32(p, end, o.name_crc))    return false;
    if (!RdU32(p, end, o.type_crc))    return false;
    if (!RdI32(p, end, o.gamemodemask)) return false;
    for (int i = 0; i < 16; ++i) if (!RdF32(p, end, o.world_transform[i])) return false;
    if (!RdI32(p, end, o.type_def_index)) return false;
    if (!RdU32(p, end, o.mesh_crc))    return false;
    if (!RdU32(p, end, o.target_guid)) return false;
    if (!RdI32(p, end, o.sticky))      return false;
    if (!RdU32(p, end, o.owner_guid))  return false;
    if (!RdStr(p, end, o.name_str))      return false;
    if (!RdStr(p, end, o.output_event))  return false;
    if (!RdStr(p, end, o.input_action))  return false;
    uint32_t fdLen = 0;
    if (!RdU32(p, end, fdLen)) return false;
    if ((size_t)(end - p) < fdLen) return false;
    o.fieldData.assign(p, p + fdLen); p += fdLen;
    uint32_t nInt = 0; if (!RdU32(p, end, nInt)) return false;
    o.intOverrides.clear();
    for (uint32_t i = 0; i < nInt; ++i) {
        uint32_t k, v; if (!RdU32(p, end, k) || !RdU32(p, end, v)) return false;
        o.intOverrides[k] = v;
    }
    uint32_t nFl = 0; if (!RdU32(p, end, nFl)) return false;
    o.floatOverrides.clear();
    for (uint32_t i = 0; i < nFl; ++i) {
        uint32_t k; float v;
        if (!RdU32(p, end, k) || !RdF32(p, end, v)) return false;
        o.floatOverrides[k] = v;
    }
    uint32_t nStr = 0; if (!RdU32(p, end, nStr)) return false;
    o.stringOverrides.clear();
    for (uint32_t i = 0; i < nStr; ++i) {
        uint32_t k; std::string v;
        if (!RdU32(p, end, k) || !RdStr(p, end, v)) return false;
        o.stringOverrides[k] = v;
    }
    uint32_t nLay = 0; if (!RdU32(p, end, nLay)) return false;
    o.layerGuids.resize(nLay);
    for (uint32_t i = 0; i < nLay; ++i) if (!RdU32(p, end, o.layerGuids[i])) return false;
    return true;
}

// ── Logging helper -writes to collab.log for cross-process debugging ────
static void CsLog(const char* fmt, ...) {
    FILE* f = NULL;
    fopen_s(&f, "collab.log", "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f); fclose(f);
}

// ── Per-client state (host side) ─────────────────────────────────────────
struct CsClient {
    SOCKET           sock;
    HANDLE           thread;
    uint8_t          assignedPrefix;
    bool             helloReceived;
    bool             markedForClose;
    char             peerName[64];

    // Per-client outbound queue. Anything the host wants to send to THIS
    // client lands here: broadcast edits originating on the host, relay
    // edits originating on other clients, whatever. The per-client thread
    // drains this each select() iteration. Separate queue per client so
    // one slow client does not stall the others - each client's socket
    // blocks only its own send loop.
    CRITICAL_SECTION outCs;
    std::vector<CsPacket> outQueue;

    CsClient() : sock(INVALID_SOCKET), thread(NULL), assignedPrefix(0),
                 helloReceived(false), markedForClose(false) {
        peerName[0] = '\0';
        InitializeCriticalSection(&outCs);
    }
    ~CsClient() { DeleteCriticalSection(&outCs); }
private:
    CsClient(const CsClient&);
    CsClient& operator=(const CsClient&);
};

// ── Impl -all internal state kept off the public header ────────────────
struct CollabSession::Impl {
    // Role / status.
    CollabRole                       role;
    CollabStatus                     status;
    char                             statusText[256];
    uint16_t                         boundPort;
    uint8_t                          myGuidPrefix;

    // PAK identity.
    std::string                      pakPath;
    uint32_t                         pakHash;
    uint64_t                         pakSizeBytes;

    // Host side.
    SOCKET                           hostListenSock;
    HANDLE                           hostAcceptThread;
    std::vector<CsClient*>           hostClients;
    CRITICAL_SECTION                 hostClientsCs;
    uint8_t                          nextGuidPrefix;  // 0xC1, 0xC2, ...

    // Client side.
    SOCKET                           clientSock;
    HANDLE                           clientThread;
    std::string                      clientHostIp;
    uint16_t                         clientHostPort;
    bool                             clientNeedsPakStream;

    // ── Client-side PAK streaming guts ─────────────────────────────────
    //
    // volatile LONG instead of std::atomic<uint64_t> because this codebase
    // is fucking welded to VS2005, and <atomic> didn't ship until C++11.
    // InterlockedExchange / InterlockedExchangeAdd are the pre-atomic
    // workhorses; they give us "writes are seen, eventually" but no real
    // memory barrier between the network thread (writer) and the main
    // thread (reader). For a goddamn progress bar that's perfect. If the
    // number flickers by a frame nobody dies. We're not building Patriot
    // missile firmware, we're showing a download bar to a dude editing
    // Helm's Deep at home.
    //
    // Also: LONG is 32-bit on Win32. A 150MB PAK is well under 4GB so we
    // never overflow, but if Pandemic ever shipped a 5GB level (they
    // didn't, the game ran on Xbox 360 with 512MB RAM) we'd have to swap
    // to InterlockedExchange64. Not today's problem.
    //
    // Temp files instead of in-memory buffers because:
    //   1. A 150MB std::vector<uint8_t> living in RAM during a streaming
    //      join is a fucking waste of address space.
    //   2. If the socket shits the bed mid-transfer we want a partial file
    //      on disk we can nuke with one DeleteFileA, not a dangling heap
    //      allocation that leaks until the whole session restarts.
    //   3. LevelReader::Load takes file paths anyway. Writing to disk and
    //      handing off the path is one less serialization layer than
    //      "load from memory buffer" would need.
    volatile LONG                    xferTotalBytes;     // PAK size + BIN size, set on each BEGIN
    volatile LONG                    xferDoneBytes;      // bumped by each CHUNK
    bool                             xferPakReady;       // main thread picks up via ConsumePakReady
    std::string                      xferTempPakPath;    // %TEMP%\vespucci_collab\<basename>.PAK
    std::string                      xferTempBinPath;    // ditto .BIN
    FILE*                            xferOutPak;         // open during streaming, NULL otherwise
    FILE*                            xferOutBin;

    // Shutdown.
    volatile LONG                    stopFlag;

    // ── Chunk 3: live-edit plumbing ──────────────────────────────────────
    //
    // levelReader is the main-thread pointer we need to APPLY inbound
    // edits. NULL until the viewport calls SetLevelReader. If it stays
    // NULL the session still functions - handshake, streaming, all of it
    // still works - but inbound edits just pile up in inQueue forever and
    // NOTHING changes on screen. That is the bug I spent 40 minutes on
    // the first time. Set the goddamn pointer.
    LevelReader*                     levelReader;

    // Client-side outbound queue. Client has exactly one socket (to host)
    // so one queue is all we need. Drained by ClientThread each select
    // iteration (same pattern as the per-client queue on the host side).
    CRITICAL_SECTION                 clientOutCs;
    std::vector<CsPacket>            clientOutQueue;

    // Shared INBOUND queue. Network threads (host's per-client workers and
    // the client's worker) push received edit packets here. Main thread
    // Tick() drains it and calls LevelReader::ApplyBroadcast*. This MUST
    // be drained on the main thread - ApplyBroadcast writes to LevelReader
    // which can trigger UI rebuilds and all kinds of non-thread-safe
    // bullshit. Do NOT apply from the network thread. I know, it's
    // tempting. Don't.
    CRITICAL_SECTION                 inCs;
    std::vector<CsPacket>            inQueue;

    Impl() : role(COLLAB_ROLE_NONE), status(COLLAB_STATUS_IDLE),
             boundPort(0), myGuidPrefix(0),
             pakHash(0), pakSizeBytes(0),
             hostListenSock(INVALID_SOCKET), hostAcceptThread(NULL),
             nextGuidPrefix(0xC1),
             clientSock(INVALID_SOCKET), clientThread(NULL),
             clientHostPort(0), clientNeedsPakStream(false),
             xferTotalBytes(0), xferDoneBytes(0), xferPakReady(false),
             xferOutPak(NULL), xferOutBin(NULL),
             stopFlag(0),
             levelReader(NULL)
    {
        statusText[0] = '\0';
        InitializeCriticalSection(&hostClientsCs);
        InitializeCriticalSection(&clientOutCs);
        InitializeCriticalSection(&inCs);
    }
    ~Impl() {
        DeleteCriticalSection(&hostClientsCs);
        DeleteCriticalSection(&clientOutCs);
        DeleteCriticalSection(&inCs);
    }
};

// ─────────────────────────────────────────────────────────────────────────
//  Forward decls for the Chunk 3 machinery
// ─────────────────────────────────────────────────────────────────────────
// All the broadcast/apply/hook-forwarder functions are defined WAY down
// near the bottom of this file (with the rest of the CollabSession
// public methods). The host per-client thread and the client thread are
// both defined further up and they CALL these fuckers. Forward decls
// close the circle so the compiler stops shitting itself with C3861
// "identifier not found" errors. If you move any of these functions
// around, keep the forward decls pointing at the right signature or
// the build falls on its face at three different translation units
// simultaneously.
static void FanoutToAllClients(CollabSession::Impl* p, const std::vector<uint8_t>& framed, CsClient* exceptClient);
static void EnqueueInbound(CollabSession::Impl* p, uint32_t type, const uint8_t* payload, uint32_t size);
static void CollabOnFieldEdit(const FieldEdit& e);
static void CollabOnAddEntity(const PendingGameObj& o);
static void CollabOnDeleteGuid(uint32_t g);
static void CollabOnClearFieldEdits();

// ── Helpers ──────────────────────────────────────────────────────────────
static bool SetStatus(CollabSession::Impl* p, CollabStatus s, const char* fmt, ...) {
    p->status = s;
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(p->statusText, sizeof(p->statusText), _TRUNCATE, fmt, ap);
    va_end(ap);
    CsLog("status=%d %s", (int)s, p->statusText);
    return true;
}

// Send full `size` bytes or fail. Returns true on success.
static bool SendAll(SOCKET s, const void* data, int size) {
    const char* p = (const char*)data;
    while (size > 0) {
        int n = ::send(s, p, size, 0);
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}

// Receive full `size` bytes or fail.
static bool RecvAll(SOCKET s, void* data, int size) {
    char* p = (char*)data;
    while (size > 0) {
        int n = ::recv(s, p, size, 0);
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}

// Send a framed message.
static bool SendMsg(SOCKET s, uint32_t type, const void* payload, uint32_t size) {
    CsHeader h; h.type = type; h.size = size;
    if (!SendAll(s, &h, sizeof(h))) return false;
    if (size > 0 && !SendAll(s, payload, (int)size)) return false;
    return true;
}

// Receive a framed message. Fills *outType; payload is read into `buf` (up
// to `bufMax`). Returns -1 on error, -2 on oversized message, >=0 = payload size.
static int RecvMsg(SOCKET s, uint32_t* outType, void* buf, uint32_t bufMax) {
    CsHeader h;
    if (!RecvAll(s, &h, sizeof(h))) return -1;
    if (h.size > kMaxPayload) return -2;
    if (h.size > bufMax) {
        // Drain and drop to keep the stream aligned.
        std::vector<char> junk(h.size);
        if (h.size > 0 && !RecvAll(s, &junk[0], (int)h.size)) return -1;
        *outType = h.type;
        return (int)h.size;  // signalled to caller but not written to buf
    }
    if (h.size > 0 && !RecvAll(s, buf, (int)h.size)) return -1;
    *outType = h.type;
    return (int)h.size;
}

// Compute a cached local-IPv4 display string.
static std::string QueryLocalIp() {
    char host[256] = {0};
    if (gethostname(host, sizeof(host)) != 0) return "(unknown)";
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return "(unknown)";
    std::string out = "(unknown)";
    for (struct addrinfo* a = res; a; a = a->ai_next) {
        struct sockaddr_in* sa = (struct sockaddr_in*)a->ai_addr;
        unsigned long addr = ntohl(sa->sin_addr.s_addr);
        // skip loopback
        if ((addr >> 24) == 127) continue;
        // inet_ntoa is deprecated but works on Win2003/XP SDK. Returns pointer
        // to a per-thread static buffer; copy before another call.
        const char* ip = inet_ntoa(sa->sin_addr);
        if (ip && *ip) { out = ip; break; }
    }
    freeaddrinfo(res);
    return out;
}
static std::string s_cachedLocalIp = "(unknown)";

// Push one file (PAK or BIN) down the socket as BEGIN + CHUNK*N + END.
// Blocks the caller's thread, which is fine because we run on the per-client
// worker, not the main thread. Returns false on any I/O failure: bad path,
// short read off disk, send() error, the network falling over, whatever.
//
// Heap buffer (std::vector) instead of a stack buffer because allocating
// 64KB on the stack works on Windows (default stack is 1MB) but feels
// wrong, and the std::vector path lets us reuse the alloc across all
// chunks for free. The vector outlives the loop body, so no per-chunk
// new/delete churn and no fragmentation.
//
// Why no compression: TCP coalesces small writes anyway, and PAK files
// are already zlib-compressed internally (Block1 + Block2 are deflated
// blobs). Compressing the framing on top would just burn CPU on both
// sides for sub-1% size win. If we ever stream uncompressed data, revisit.
static bool SendFileStream(SOCKET sock, const char* path, uint8_t fileKind) {
    FILE* fp = NULL; fopen_s(&fp, path, "rb");
    if (!fp) { CsLog("host stream: can't open '%s'", path); return false; }
    fseek(fp, 0, SEEK_END);
    uint64_t total = (uint64_t)_ftelli64(fp);
    fseek(fp, 0, SEEK_SET);

    // BEGIN
    CsPakBegin begin; memset(&begin, 0, sizeof(begin));
    begin.fileKind   = fileKind;
    begin.totalBytes = total;
    begin.fileHash   = 0;  // wire-level sanity only; no crypto, don't ask
    const char* base = path;
    for (const char* pp2 = path; *pp2; ++pp2) {
        if (*pp2 == '\\' || *pp2 == '/') base = pp2 + 1;
    }
    strncpy_s(begin.fileName, sizeof(begin.fileName), base, _TRUNCATE);
    if (!SendMsg(sock, CM_PAK_BEGIN, &begin, sizeof(begin))) { fclose(fp); return false; }

    std::vector<uint8_t> buf(sizeof(CsPakChunk) + kChunkSize);
    uint64_t off = 0;
    while (off < total) {
        uint32_t want = (uint32_t)((total - off) < kChunkSize ? (total - off) : kChunkSize);
        CsPakChunk ch; memset(&ch, 0, sizeof(ch));
        ch.fileKind = fileKind;
        ch.offset   = off;
        ch.size     = want;
        size_t rd = fread(&buf[sizeof(CsPakChunk)], 1, want, fp);
        if (rd != want) {
            CsLog("host stream: short read at off=%llu (%u wanted, %u got)",
                  (unsigned long long)off, (unsigned)want, (unsigned)rd);
            fclose(fp); return false;
        }
        memcpy(&buf[0], &ch, sizeof(ch));
        if (!SendMsg(sock, CM_PAK_CHUNK, &buf[0], sizeof(CsPakChunk) + want)) {
            CsLog("host stream: send failed at off=%llu", (unsigned long long)off);
            fclose(fp); return false;
        }
        off += want;
    }
    fclose(fp);

    CsPakEnd end; memset(&end, 0, sizeof(end));
    end.fileKind = fileKind;
    if (!SendMsg(sock, CM_PAK_END, &end, sizeof(end))) return false;
    CsLog("host stream: sent '%s' (%llu bytes, kind=%u)",
          base, (unsigned long long)total, (unsigned)fileKind);
    return true;
}

// ── Host: per-client thread ──────────────────────────────────────────────
struct HostClientThreadArgs {
    CollabSession::Impl* p;
    CsClient* c;
};

static DWORD WINAPI HostClientThread(LPVOID param) {
    HostClientThreadArgs* args = (HostClientThreadArgs*)param;
    CollabSession::Impl* p = args->p;
    CsClient* c = args->c;
    delete args;

    CsLog("host: client thread start sock=%d", (int)c->sock);

    // Read HELLO.
    std::vector<char> buf(65536);
    uint32_t type = 0;
    int n = RecvMsg(c->sock, &type, &buf[0], (uint32_t)buf.size());
    if (n < 0 || type != CM_HELLO || n < (int)sizeof(CsHello)) {
        CsLog("host: client sent garbage fucking HELLO packet (type=0x%X, %d bytes). Either they are running a stale build, a different game, or their network layer shit itself. Dropping the socket.", (unsigned)type, n);
        closesocket(c->sock); c->sock = INVALID_SOCKET;
        return 1;
    }
    CsHello h; memcpy(&h, &buf[0], sizeof(h));

    // Version gate.
    if (h.protocolVersion != kProtocolVersion) {
        CsLog("host: protocol version mismatch - client talking v%u, we speak v%u. One of you is running a stale-ass build. Rebuild BOTH laptops and try again, and DO NOT skip the DLL rebuild either, protocol lives in there too. Closing this socket, we ain't entertaining cross-version bullshit.",
              h.protocolVersion, kProtocolVersion);
        closesocket(c->sock); c->sock = INVALID_SOCKET;
        return 2;
    }
    // PAK match gate - only enforced when the client swears it has a local
    // PAK. If they set needPakStream=1 they are openly admitting they got
    // nothing, and we're about to shove our whole goddamn PAK+BIN down the
    // wire after WELCOME. Either path is fine, we just need to know which
    // one we are on.
    bool willStream = (h.needPakStream != 0);
    if (!willStream) {
        if (h.pakHash != p->pakHash || h.pakSizeBytes != p->pakSizeBytes) {
            CsLog("host: PAK HASH MISMATCH - client=0x%08X %llu bytes vs host=0x%08X %llu bytes. You are editing TWO DIFFERENT FILES and calling it collab. Load the same level on both laptops (file name AND content) or pass needPakStream=1 so we stream ours. Closing socket, no way in hell we are syncing edits between mismatched PAKs.",
                  h.pakHash, (unsigned long long)h.pakSizeBytes,
                  p->pakHash, (unsigned long long)p->pakSizeBytes);
            SendMsg(c->sock, CM_ERR_PAK_MISMATCH, NULL, 0);
            closesocket(c->sock); c->sock = INVALID_SOCKET;
            return 3;
        }
    } else {
        // Client wants us to stream, but we don't have shit loaded. Bail with
        // a clear error instead of sending a zero-byte stream and confusing
        // everybody downstream.
        if (p->pakHash == 0 || p->pakPath.empty()) {
            CsLog("host: client wants a PAK stream but the host has NOTHING fucking loaded. You hit Host Session without loading a level first. Load a PAK on this laptop first, THEN accept joiners. Closing socket before we make a bigger mess.");
            SendMsg(c->sock, CM_ERR_NOTHING_LOADED, NULL, 0);
            closesocket(c->sock); c->sock = INVALID_SOCKET;
            return 9;
        }
    }
    // Assign prefix.
    EnterCriticalSection(&p->hostClientsCs);
    c->assignedPrefix = p->nextGuidPrefix++;
    if (p->nextGuidPrefix == 0) p->nextGuidPrefix = 0xC1; // wrap guard
    LeaveCriticalSection(&p->hostClientsCs);

    // Send WELCOME.
    CsWelcome w; memset(&w, 0, sizeof(w));
    w.protocolVersion    = kProtocolVersion;
    w.assignedGuidPrefix = c->assignedPrefix;
    w.willStreamPak      = willStream ? 1 : 0;
    strncpy_s(w.hostName, sizeof(w.hostName), "host", _TRUNCATE);
    if (!SendMsg(c->sock, CM_WELCOME, &w, sizeof(w))) {
        CsLog("host: SendMsg WELCOME failed");
        closesocket(c->sock); c->sock = INVALID_SOCKET;
        return 4;
    }
    strncpy_s(c->peerName, sizeof(c->peerName), h.peerName, _TRUNCATE);
    c->helloReceived = true;
    CsLog("host: client '%s' welcomed (prefix=0x%02X, stream=%d)",
          c->peerName, c->assignedPrefix, (int)willStream);

    // Client needed a stream -push PAK + BIN down the wire RIGHT NOW,
    // blocking, before we enter the normal event loop. Client is parked in
    // its ClientThread's RecvMsg waiting. If the send dies mid-transfer the
    // socket shits itself, both sides bounce back to IDLE, no zombie state,
    // no half-loaded level on the client. Clean failure mode, the one thing
    // I got right the first time.
    if (willStream) {
        // Derive BIN path from PAK path. Pandemic's convention: same
        // basename, swap .PAK for .BIN. EVERY Conquest level ships this
        // way. If some maniac mods a level with mismatched naming fopen
        // will return NULL, SendFileStream returns false, and we bail on
        // the stream with a clean "host stream failed" log entry. No
        // half-transferred garbage gets loaded into anybody's viewer.
        std::string pakPath = p->pakPath;
        std::string binPath = pakPath;
        size_t dot = binPath.find_last_of('.');
        if (dot != std::string::npos) {
            binPath.replace(dot, std::string::npos, ".BIN");
        }
        bool pakOk = SendFileStream(c->sock, pakPath.c_str(), 0);
        bool binOk = pakOk ? SendFileStream(c->sock, binPath.c_str(), 1) : false;
        if (pakOk && binOk) {
            CsPakEnd end; memset(&end, 0, sizeof(end));
            end.fileKind = 2;  // ALL done -client flips to CONNECTED and we enter the event loop
            SendMsg(c->sock, CM_PAK_END, &end, sizeof(end));
            CsLog("host: streamed PAK+BIN to '%s'", c->peerName);
        } else {
            CsLog("host: PAK+BIN stream to '%s' DIED halfway the fuck through. Network cable, Wi-Fi spike, client crash, who the hell knows. Socket is cooked, killing the connection. Client needs to reconnect and redo the whole streaming dance.", c->peerName);
            closesocket(c->sock); c->sock = INVALID_SOCKET;
            return 10;
        }
    }

    // Main read loop. Was a dumb blocking RecvMsg in Chunk 1 because all we
    // cared about was keeping the goddamn socket open. Chunk 3 changes the
    // whole deal - this thread now also needs to push OUTBOUND edits (from
    // host's main thread) down the wire. So we switch to select() with a
    // short timeout, drain the outqueue every tick, and read inbound only
    // when the socket says it has data waiting. 20ms timeout means the
    // worst case from "main thread enqueues an edit" to "bytes on the wire"
    // is 20ms. A human cannot feel 20ms. Good enough.
    while (!InterlockedCompareExchange(&p->stopFlag, 0, 0) && !c->markedForClose) {
        // Kick the outqueue first. If the socket ate itself mid-send, bail.
        if (!DrainOutqueue(c->sock, c->outQueue, c->outCs)) {
            CsLog("host: client send died draining the outqueue - socket is fucking toast, tearing the connection down. Either the peer disappeared or the OS dropped it. Edits that were queued for this peer and never went out are lost, sorry.");
            break;
        }

        // Poll for inbound with a short timeout. This replaces the old
        // blocking RecvMsg so this loop iterates every ~20ms regardless.
        fd_set rs; FD_ZERO(&rs); FD_SET(c->sock, &rs);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 20000;  // 20ms
        int sel = select(0, &rs, NULL, NULL, &tv);
        if (sel <= 0) continue;

        int m = RecvMsg(c->sock, &type, &buf[0], (uint32_t)buf.size());
        if (m < 0) { CsLog("host: client disconnected"); break; }

        if (type == CM_PING) {
            SendMsg(c->sock, CM_PONG, NULL, 0);
        }
        // Chunk 3 messages from the client. We do TWO things:
        //   1) Queue the payload for the MAIN THREAD to apply via Tick().
        //      Host's own LevelReader needs this edit too - host is a peer
        //      in the collab room, not just a glorified router.
        //   2) Relay the exact same packet to EVERY OTHER client. This is
        //      the fanout-except-sender path: the client who sent it
        //      already has the edit applied locally (they called
        //      LevelReader::AddFieldEdit which fired the hook which
        //      broadcast it in the first place), so bouncing it right
        //      back at them would be a useless duplicate.
        // Both sides get the SAME wire bytes. No re-serialization. No
        // chance of the host's copy disagreeing with the relayed copy
        // because of some stupid encode bug that treats host and clients
        // differently. One source of truth: what the sender sent.
        else if (type == CM_FIELD_EDIT || type == CM_ADD_ENTITY ||
                 type == CM_DELETE_GUID || type == CM_CLEAR_FIELD_EDITS) {
            // Queue for host's own main-thread apply.
            EnqueueInbound(p, type, (uint8_t*)&buf[0], (uint32_t)m);
            // Relay to everyone EXCEPT the sender. Rebuild the frame from
            // type+payload because buf[] has ONLY the payload at this point
            // (RecvMsg stripped the header). Cheap - a few dozen bytes
            // memcpy - and keeps the relay path symmetric with the normal
            // broadcast builder.
            std::vector<uint8_t> framed;
            BuildPacket(framed, type, &buf[0], (uint32_t)m);
            FanoutToAllClients(p, framed, c);
        }
        // Anything else unknown? Log once at recv time and move on. Not
        // going to kill a session over a message type we do not recognize -
        // could be a future peer, could be stream noise, whatever.
    }

    closesocket(c->sock); c->sock = INVALID_SOCKET;
    return 0;
}

// ── Host: accept thread ─────────────────────────────────────────────────
static DWORD WINAPI HostAcceptThread(LPVOID param) {
    CollabSession::Impl* p = (CollabSession::Impl*)param;
    CsLog("host: accept thread start on port %u", (unsigned)p->boundPort);
    while (!InterlockedCompareExchange(&p->stopFlag, 0, 0)) {
        struct sockaddr_in addr; int alen = (int)sizeof(addr);
        SOCKET s = accept(p->hostListenSock, (struct sockaddr*)&addr, &alen);
        if (s == INVALID_SOCKET) { Sleep(50); continue; }
        CsLog("host: new connection");
        CsClient* c = new CsClient();
        c->sock = s;
        EnterCriticalSection(&p->hostClientsCs);
        p->hostClients.push_back(c);
        LeaveCriticalSection(&p->hostClientsCs);
        HostClientThreadArgs* args = new HostClientThreadArgs;
        args->p = p; args->c = c;
        c->thread = CreateThread(NULL, 0, HostClientThread, args, 0, NULL);
    }
    CsLog("host: accept thread exit");
    return 0;
}

// ── Client: worker thread ──────────────────────────────────────────────
static DWORD WINAPI ClientThread(LPVOID param) {
    CollabSession::Impl* p = (CollabSession::Impl*)param;
    CsLog("client: thread start -> %s:%u", p->clientHostIp.c_str(), (unsigned)p->clientHostPort);

    // Create socket, connect.
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        SetStatus(p, COLLAB_STATUS_ERROR, "socket() failed");
        return 1;
    }
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(p->clientHostPort);
    // inet_addr is pre-Vista compatible. Returns INADDR_NONE on bad input.
    unsigned long ipN = inet_addr(p->clientHostIp.c_str());
    if (ipN == INADDR_NONE) {
        SetStatus(p, COLLAB_STATUS_ERROR, "invalid IP");
        closesocket(s); return 2;
    }
    a.sin_addr.s_addr = ipN;
    if (connect(s, (struct sockaddr*)&a, (int)sizeof(a)) != 0) {
        SetStatus(p, COLLAB_STATUS_ERROR, "connect failed (host down?)");
        closesocket(s); return 3;
    }
    p->clientSock = s;
    SetStatus(p, COLLAB_STATUS_HANDSHAKE, "connected, saying hello");

    // Send HELLO. If client is joining without a local copy, needPakStream=1
    // and hash/size are left at 0 (they're meaningless in that mode anyway).
    CsHello h; memset(&h, 0, sizeof(h));
    h.protocolVersion = kProtocolVersion;
    h.pakHash         = p->clientNeedsPakStream ? 0 : p->pakHash;
    h.pakSizeBytes    = p->clientNeedsPakStream ? 0 : p->pakSizeBytes;
    h.needPakStream   = p->clientNeedsPakStream ? 1 : 0;
    strncpy_s(h.peerName, sizeof(h.peerName), "client", _TRUNCATE);
    if (!SendMsg(s, CM_HELLO, &h, sizeof(h))) {
        SetStatus(p, COLLAB_STATUS_ERROR, "send HELLO failed");
        closesocket(s); p->clientSock = INVALID_SOCKET; return 4;
    }

    // Receive reply: WELCOME, CM_ERR_PAK_MISMATCH, or CM_ERR_NOTHING_LOADED.
    std::vector<char> buf(sizeof(CsPakChunk) + kChunkSize + 16);
    uint32_t type = 0;
    int n = RecvMsg(s, &type, &buf[0], (uint32_t)buf.size());
    if (n < 0) { SetStatus(p, COLLAB_STATUS_ERROR, "host dropped during handshake"); closesocket(s); p->clientSock=INVALID_SOCKET; return 5; }
    if (type == CM_ERR_PAK_MISMATCH) {
        SetStatus(p, COLLAB_STATUS_ERROR, "PAK mismatch with host");
        closesocket(s); p->clientSock = INVALID_SOCKET; return 6;
    }
    if (type == CM_ERR_NOTHING_LOADED) {
        SetStatus(p, COLLAB_STATUS_ERROR, "host has no level loaded to stream");
        closesocket(s); p->clientSock = INVALID_SOCKET; return 8;
    }
    if (type != CM_WELCOME || n < (int)sizeof(CsWelcome)) {
        SetStatus(p, COLLAB_STATUS_ERROR, "bad WELCOME from host");
        closesocket(s); p->clientSock = INVALID_SOCKET; return 7;
    }
    CsWelcome w; memcpy(&w, &buf[0], sizeof(w));
    p->myGuidPrefix = w.assignedGuidPrefix;

    // Host is about to dump the PAK + BIN on us. Park right here, blocking,
    // draining CM_PAK_BEGIN / CHUNK*N / END messages until we finally see
    // END(fileKind=2) which means "all done, switch to CONNECTED." Temp
    // files land in %TEMP%\vespucci_collab\ because Windows %TEMP% is
    // guaranteed writable by every shitty corporate IT policy ever written
    // and we don't need permission to drop files there. We don't sweat
    // cleanup either -the next join overwrites, and Windows sweeps %TEMP%
    // occasionally on its own. If you're paranoid, delete the folder.
    if (w.willStreamPak) {
        SetStatus(p, COLLAB_STATUS_RECEIVING, "receiving PAK from host...");
        char tempDir[MAX_PATH]; GetTempPathA(MAX_PATH, tempDir);
        strcat_s(tempDir, MAX_PATH, "vespucci_collab\\");
        CreateDirectoryA(tempDir, NULL);

        p->xferTempPakPath.clear();
        p->xferTempBinPath.clear();
        p->xferOutPak = NULL;
        p->xferOutBin = NULL;
        InterlockedExchange(&p->xferTotalBytes, 0);
        InterlockedExchange(&p->xferDoneBytes, 0);

        bool streamDone = false;
        bool streamFailed = false;
        while (!streamDone && !streamFailed) {
            uint32_t tt = 0;
            int nn = RecvMsg(s, &tt, &buf[0], (uint32_t)buf.size());
            if (nn < 0) { streamFailed = true; break; }
            if (tt == CM_PAK_BEGIN && nn >= (int)sizeof(CsPakBegin)) {
                CsPakBegin b; memcpy(&b, &buf[0], sizeof(b));
                char outPath[MAX_PATH];
                _snprintf_s(outPath, sizeof(outPath), _TRUNCATE, "%s%s", tempDir, b.fileName);
                FILE* fp = NULL; fopen_s(&fp, outPath, "wb");
                if (!fp) { CsLog("client xfer: can't create '%s'", outPath); streamFailed = true; break; }
                if (b.fileKind == 0) { p->xferOutPak = fp; p->xferTempPakPath = outPath; }
                else                 { p->xferOutBin = fp; p->xferTempBinPath = outPath; }
                // Accumulate total so the progress bar knows the full scope.
                InterlockedExchangeAdd(&p->xferTotalBytes, (LONG)b.totalBytes);
                CsLog("client xfer: incoming '%s' (%llu bytes, kind=%u) -> %s",
                      b.fileName, (unsigned long long)b.totalBytes, (unsigned)b.fileKind, outPath);
            }
            else if (tt == CM_PAK_CHUNK && nn >= (int)sizeof(CsPakChunk)) {
                CsPakChunk ch; memcpy(&ch, &buf[0], sizeof(ch));
                FILE* fp = (ch.fileKind == 0) ? p->xferOutPak : p->xferOutBin;
                if (!fp) { CsLog("client xfer: CHUNK for unopened file kind %u", (unsigned)ch.fileKind); streamFailed = true; break; }
                if (nn < (int)(sizeof(CsPakChunk) + ch.size)) { CsLog("client xfer: truncated CHUNK"); streamFailed = true; break; }
                size_t wr = fwrite(&buf[sizeof(CsPakChunk)], 1, ch.size, fp);
                if (wr != ch.size) { CsLog("client xfer: fwrite short"); streamFailed = true; break; }
                InterlockedExchangeAdd(&p->xferDoneBytes, (LONG)ch.size);
            }
            else if (tt == CM_PAK_END && nn >= (int)sizeof(CsPakEnd)) {
                CsPakEnd e; memcpy(&e, &buf[0], sizeof(e));
                if (e.fileKind == 0 && p->xferOutPak) { fclose(p->xferOutPak); p->xferOutPak = NULL; }
                if (e.fileKind == 1 && p->xferOutBin) { fclose(p->xferOutBin); p->xferOutBin = NULL; }
                if (e.fileKind == 2) {
                    streamDone = true;
                    p->xferPakReady = true;
                    CsLog("client xfer: all done, temp PAK=%s BIN=%s",
                          p->xferTempPakPath.c_str(), p->xferTempBinPath.c_str());
                }
            }
            else {
                CsLog("client xfer: unexpected msg type=0x%X n=%d during stream", (unsigned)tt, nn);
                streamFailed = true;
            }
        }
        if (streamFailed) {
            if (p->xferOutPak) { fclose(p->xferOutPak); p->xferOutPak = NULL; }
            if (p->xferOutBin) { fclose(p->xferOutBin); p->xferOutBin = NULL; }
            SetStatus(p, COLLAB_STATUS_ERROR, "PAK transfer failed");
            closesocket(s); p->clientSock = INVALID_SOCKET; return 11;
        }
    }

    SetStatus(p, COLLAB_STATUS_CONNECTED, "connected (guid prefix 0x%02X)", (unsigned)w.assignedGuidPrefix);
    CsLog("client: welcomed, prefix=0x%02X", (unsigned)w.assignedGuidPrefix);

    // Main loop. Chunk 1 only cared about keeping the socket alive with
    // PINGs. Chunk 3 turns this thread into a real bidirectional pipe: we
    // drain OUR outqueue (local edits heading to host) every iteration,
    // and we read INBOUND edits (other peers' work, fanned out by host)
    // and shove them at the main thread. Select timeout dropped from 250ms
    // to 20ms so the drain cycle is tight - an edit sitting in the outqueue
    // for 250ms before going out the wire would feel like ass when you are
    // dragging a slider and watching the other laptop for the change.
    DWORD lastPing = GetTickCount();
    while (!InterlockedCompareExchange(&p->stopFlag, 0, 0)) {
        // Drain outqueue. If the send dies, the session is dead - connection
        // to host is gone, no recovery at this layer, bail out.
        if (!DrainOutqueue(s, p->clientOutQueue, p->clientOutCs)) {
            CsLog("client: our send to the host just dropped DEAD mid-drain. Host probably vanished (network, process crash, laptop lid closed, power, whatever). No point retrying - socket is cooked. Kicking ourselves back to IDLE.");
            break;
        }

        // Periodic ping so a silently dead host gets noticed. Goes through
        // SendMsg directly (not the outqueue) because PING should not queue
        // up behind a backlog of edits - it has its own heartbeat rhythm.
        if (GetTickCount() - lastPing > 5000) {
            if (!SendMsg(s, CM_PING, NULL, 0)) break;
            lastPing = GetTickCount();
        }

        // Inbound poll with a short timeout.
        fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 20000;  // 20ms
        int sel = select(0, &rs, NULL, NULL, &tv);
        if (sel <= 0) continue;

        int m = RecvMsg(s, &type, &buf[0], (uint32_t)buf.size());
        if (m < 0) { CsLog("client: host disconnected"); break; }

        // Chunk 3: edit messages from host (either host-originated OR
        // relayed from another client). Just queue for main-thread apply.
        // Client does NOT relay - client has no peers to relay TO, host
        // is the only one we talk to. That is the entire topology.
        if (type == CM_FIELD_EDIT || type == CM_ADD_ENTITY ||
            type == CM_DELETE_GUID || type == CM_CLEAR_FIELD_EDITS) {
            EnqueueInbound(p, type, (uint8_t*)&buf[0], (uint32_t)m);
        }
        else if (type == CM_PONG) {
            // Keepalive reply. Nothing to do - receiving anything means
            // the host is alive.
        }
        // Anything else: drop on the floor. Log noise would be worse than
        // the unknown message. Real problems surface other ways.
    }

    closesocket(s); p->clientSock = INVALID_SOCKET;
    SetStatus(p, COLLAB_STATUS_IDLE, "disconnected");
    return 0;
}

// ── CollabSession implementation ─────────────────────────────────────────
CollabSession::CollabSession() : m_impl(new Impl()) {}
CollabSession::~CollabSession() { Shutdown(); delete m_impl; }

CollabSession& CollabSession::Instance() {
    static CollabSession s_inst;
    return s_inst;
}

bool CollabSession::Init() {
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        SetStatus(m_impl, COLLAB_STATUS_ERROR, "WSAStartup failed (%d)", r);
        return false;
    }
    s_cachedLocalIp = QueryLocalIp();
    CsLog("init: local IP = %s", s_cachedLocalIp.c_str());
    return true;
}

void CollabSession::Shutdown() {
    Stop();
    WSACleanup();
}

bool CollabSession::HostStart(uint16_t port) {
    if (m_impl->role != COLLAB_ROLE_NONE) { SetStatus(m_impl, COLLAB_STATUS_ERROR, "already in a session"); return false; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { SetStatus(m_impl, COLLAB_STATUS_ERROR, "socket() failed"); return false; }
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr*)&a, (int)sizeof(a)) != 0) {
        SetStatus(m_impl, COLLAB_STATUS_ERROR, "bind port %u failed (in use?)", (unsigned)port);
        closesocket(s); return false;
    }
    if (listen(s, 8) != 0) {
        SetStatus(m_impl, COLLAB_STATUS_ERROR, "listen failed");
        closesocket(s); return false;
    }
    m_impl->hostListenSock = s;
    m_impl->boundPort = port;
    m_impl->role = COLLAB_ROLE_HOST;
    m_impl->myGuidPrefix = 0; // host uses native GUID space
    SetStatus(m_impl, COLLAB_STATUS_HOSTING, "hosting %s:%u", s_cachedLocalIp.c_str(), (unsigned)port);
    m_impl->stopFlag = 0;
    // Wire LevelReader's broadcast hooks. From this moment on, every
    // AddFieldEdit / AddPendingEntity / AddDeletedGuid / ClearFieldEdits
    // anywhere in this process triggers an outbound broadcast. Forget to
    // do this step and you have a handshake, a pretty status indicator,
    // and ZERO ACTUAL COLLABORATION. Just two people staring at each
    // other over TCP. Useless.
    LevelReader::SetCollabHooks(&CollabOnFieldEdit, &CollabOnAddEntity,
                                &CollabOnDeleteGuid, &CollabOnClearFieldEdits);
    m_impl->hostAcceptThread = CreateThread(NULL, 0, HostAcceptThread, m_impl, 0, NULL);
    return true;
}

bool CollabSession::JoinStart(const char* ip, uint16_t port, bool needPakStream) {
    if (m_impl->role != COLLAB_ROLE_NONE) { SetStatus(m_impl, COLLAB_STATUS_ERROR, "already in a session"); return false; }
    if (!ip || !*ip) { SetStatus(m_impl, COLLAB_STATUS_ERROR, "empty IP"); return false; }
    m_impl->clientHostIp        = ip;
    m_impl->clientHostPort      = port;
    m_impl->clientNeedsPakStream= needPakStream;
    m_impl->role                = COLLAB_ROLE_CLIENT;
    m_impl->stopFlag            = 0;
    m_impl->xferPakReady        = false;
    InterlockedExchange(&m_impl->xferTotalBytes, 0);
    InterlockedExchange(&m_impl->xferDoneBytes, 0);
    SetStatus(m_impl, COLLAB_STATUS_CONNECTING, "connecting to %s:%u%s",
              ip, (unsigned)port, needPakStream ? " (will receive PAK)" : "");
    // Same hook registration as HostStart. Client's local edits fire the
    // hook, the hook routes to BroadcastFieldEdit which sees role=CLIENT
    // and shoves the frame at the host. Host then relays to everybody
    // else. Omit this and the client becomes a passive viewer - it will
    // receive other people's edits but its OWN edits go nowhere. Silent
    // diverge. Pain.
    LevelReader::SetCollabHooks(&CollabOnFieldEdit, &CollabOnAddEntity,
                                &CollabOnDeleteGuid, &CollabOnClearFieldEdits);
    m_impl->clientThread = CreateThread(NULL, 0, ClientThread, m_impl, 0, NULL);
    return true;
}

// Returns the download progress as 0..1. The DLL polls this every frame
// to draw the bar. Network thread writes the counters via Interlocked*,
// main thread reads via the same. No mutex because (a) we don't have
// std::atomic on VS2005 and (b) for a download bar we tolerate one frame
// of staleness or a torn read. If you ever needed sub-frame accuracy
// here you'd be insane and probably writing the wrong program.
float CollabSession::GetXferProgress() const {
    LONG total = InterlockedCompareExchange((LONG*)&m_impl->xferTotalBytes, 0, 0);
    LONG done  = InterlockedCompareExchange((LONG*)&m_impl->xferDoneBytes, 0, 0);
    if (total <= 0) return 0.0f;
    float f = (float)done / (float)total;
    if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
    return f;
}
uint64_t CollabSession::GetXferTotalBytes() const {
    return (uint64_t)(uint32_t)InterlockedCompareExchange((LONG*)&m_impl->xferTotalBytes, 0, 0);
}
uint64_t CollabSession::GetXferDoneBytes() const {
    return (uint64_t)(uint32_t)InterlockedCompareExchange((LONG*)&m_impl->xferDoneBytes, 0, 0);
}
// Handoff: network thread finished receiving PAK+BIN, sets xferPakReady=true
// with the temp paths filled. Main thread polls this each frame; on the one
// frame where it's ready, ConsumePakReady returns true, fills the paths,
// clears the flag. Main thread then LevelReader::Load()s them.
//
// Why this pattern instead of just calling Load() from the network thread:
// LevelReader::Load touches D3D via LevelScene (vertex buffers, textures,
// shader cache). D3D9 objects ARE NOT thread-safe; creating a vertex buffer
// off the render thread crashes or corrupts state that only blows up much
// later. So: network thread does disk I/O, main thread does D3D. Classic
// producer/consumer with a flag and two strings. Fuck threading.
bool CollabSession::ConsumePakReady(std::string& outPakPath, std::string& outBinPath) {
    if (!m_impl->xferPakReady) return false;
    outPakPath = m_impl->xferTempPakPath;
    outBinPath = m_impl->xferTempBinPath;
    m_impl->xferPakReady = false;
    return true;
}

void CollabSession::Stop() {
    InterlockedExchange(&m_impl->stopFlag, 1);
    // Close sockets to unblock threads.
    if (m_impl->hostListenSock != INVALID_SOCKET) {
        closesocket(m_impl->hostListenSock);
        m_impl->hostListenSock = INVALID_SOCKET;
    }
    EnterCriticalSection(&m_impl->hostClientsCs);
    for (size_t i = 0; i < m_impl->hostClients.size(); ++i) {
        CsClient* c = m_impl->hostClients[i];
        if (c->sock != INVALID_SOCKET) { shutdown(c->sock, SD_BOTH); closesocket(c->sock); c->sock = INVALID_SOCKET; }
    }
    LeaveCriticalSection(&m_impl->hostClientsCs);
    if (m_impl->clientSock != INVALID_SOCKET) {
        shutdown(m_impl->clientSock, SD_BOTH);
        closesocket(m_impl->clientSock);
        m_impl->clientSock = INVALID_SOCKET;
    }
    // Wait for threads.
    if (m_impl->hostAcceptThread) { WaitForSingleObject(m_impl->hostAcceptThread, 3000); CloseHandle(m_impl->hostAcceptThread); m_impl->hostAcceptThread = NULL; }
    if (m_impl->clientThread)     { WaitForSingleObject(m_impl->clientThread, 3000);     CloseHandle(m_impl->clientThread);     m_impl->clientThread = NULL; }
    EnterCriticalSection(&m_impl->hostClientsCs);
    for (size_t i = 0; i < m_impl->hostClients.size(); ++i) {
        CsClient* c = m_impl->hostClients[i];
        if (c->thread) { WaitForSingleObject(c->thread, 1000); CloseHandle(c->thread); }
        delete c;
    }
    m_impl->hostClients.clear();
    LeaveCriticalSection(&m_impl->hostClientsCs);
    m_impl->role = COLLAB_ROLE_NONE;
    m_impl->myGuidPrefix = 0;
    m_impl->boundPort = 0;
    // Kill the broadcast hooks. Back to solo mode. If we left these
    // pointing at our trampolines, a later AddFieldEdit after Stop() would
    // trampoline into BroadcastFieldEdit, see role=NONE, and return - but
    // that is one wasted function call on every edit, FOREVER, for
    // nothing. Clear them. It is one line.
    LevelReader::ClearCollabHooks();
    // Nuke any inbound edits that never got drained. Session is over.
    // Applying half-delivered edits from a dead session into a fresh one
    // would be a wonderful source of ghost-entity bugs. No thanks.
    EnterCriticalSection(&m_impl->inCs);
    m_impl->inQueue.clear();
    LeaveCriticalSection(&m_impl->inCs);
    EnterCriticalSection(&m_impl->clientOutCs);
    m_impl->clientOutQueue.clear();
    LeaveCriticalSection(&m_impl->clientOutCs);
    SetStatus(m_impl, COLLAB_STATUS_IDLE, "idle");
}

void CollabSession::SetLoadedPak(const char* pakPath, uint32_t pakHash, uint64_t pakSizeBytes) {
    // Viewport pumps this EVERY FUCKING FRAME now (so host can load a level
    // after clicking Host and still advertise it to joiners). If we logged
    // on every call, the log would hit 2+ MB in one debug session and fuck
    // up the user's search-grep workflow. So: only log when something
    // actually changed. Idempotent no-op on 99% of frames.
    const std::string newPath = pakPath ? pakPath : "";
    if (m_impl->pakHash == pakHash &&
        m_impl->pakSizeBytes == pakSizeBytes &&
        m_impl->pakPath == newPath) {
        return;
    }
    m_impl->pakPath      = newPath;
    m_impl->pakHash      = pakHash;
    m_impl->pakSizeBytes = pakSizeBytes;
    CsLog("SetLoadedPak hash=0x%08X size=%llu path=%s",
          pakHash, (unsigned long long)pakSizeBytes, m_impl->pakPath.c_str());
}

// ═════════════════════════════════════════════════════════════════════════
//  THE MOTHERFUCKING HEART OF COLLAB
//  Chunk 3: broadcast + apply implementation
// ═════════════════════════════════════════════════════════════════════════
//
// This is the bit that turns two laptops into one goddamn editor.
// Everything that came before - the TCP socket plumbing, the handshake
// dance, the protocol version gate, the GUID prefix partitioning we had
// to invent because Pandemic's original GUID allocator would shit itself
// the moment two peers tried to mint entities concurrently, the 150MB
// PAK streaming over a shitty home Wi-Fi router, the temp-file handoff,
// the "client arrives naked with no local PAK" support, ALL of it - was
// building up to this file. Without Chunk 3, everything above it is an
// overengineered file-transfer tool with a fancy status bar. Expensive
// Dropbox. And we sure as shit did not spend all these late nights
// resurrecting a dead studio's dev pipeline to ship expensive Dropbox.
//
// Side rant, since we are already here: the ONLY reason this editor has
// to exist is because EA bought Pandemic in 2007, milked them dry on
// Mercenaries 2 and Lord of the Rings Conquest, then shut the studio
// down in 2009 while the staff were still mid-project. Fuck EA. Every
// feature in this file, every fucking workaround for Pandemic's weird
// GUID scheme, every handshake quirk to keep the binary layout happy -
// none of this would be necessary if EA had not murdered the one team
// that actually understood this goddamn engine. We are reverse-
// engineering the tools those people would have shipped themselves if
// some suit had not pulled the plug. So this collab session is, in a
// small stubborn way, a middle finger aimed at a corporate decision
// made fifteen years ago. Good. Let it stay that way.
//
// If this section breaks, the user sees:
//   - Handshake green.
//   - Peer connected, prefix assigned.
//   - Status reads CONNECTED in the toolbar.
//   - Asset tree populated, 3D scene rendering, everything feels alive.
//   - They drag a slider on Laptop B.
//   - Laptop A does absolutely fucking nothing.
//
// They think the session is dead. They restart both editors. They check
// the fucking firewall. They swap cables. They yell at me on Discord.
// And it was ALWAYS THIS FILE. Every goddamn time. Because TCP looks
// fine from the socket side and ImGui looks fine from the UI side and
// the problem is ALWAYS the greasy little pipeline between them that
// carries edits from one to the other. That pipeline is what we are
// building right here, right now. Every single function below has
// exactly ONE job: do not drop edits. Do not echo edits. Do not reorder
// edits in a way that corrupts state. Apply edits in the right place,
// in the right order, on the right motherfucking thread.
//
// TAKE IT SERIOUSLY. If a function below looks "simpler than it needs
// to be", it is probably missing a failure mode I already hit and
// already documented and you are about to silently delete.

// Shove a packet onto every client's outqueue. Optionally skip ONE client
// so we do not bounce their own edit right back at their face like some
// drunk-ass assistant repeating every word you just said. The exceptClient
// parameter is the safety pin on the grenade - pull it out by passing
// NULL and you fan the packet to everybody including the sender, which
// is fine for edits that originate on the host, but SUICIDAL for relayed
// client edits because you would echo the edit back at the originator
// and trigger the same thermonuclear echo loop the Apply* comments over
// in LevelReader.cpp warn about. Pass the sending client as the exception
// when relaying. Pass NULL when broadcasting host-local edits. Get it
// wrong and the first slider drag on a client will ping-pong forever
// between host and client at maximum send() throughput until both
// editors hang and Windows throws up a "this app is not responding"
// dialog. Threadsafe because each per-client outqueue has its own
// critical section, so one slow peer choking on their socket does
// NOT freeze the fanout for the rest of the room.
static void FanoutToAllClients(CollabSession::Impl* p, const std::vector<uint8_t>& framed, CsClient* exceptClient) {
    EnterCriticalSection(&p->hostClientsCs);
    for (size_t i = 0; i < p->hostClients.size(); ++i) {
        CsClient* c = p->hostClients[i];
        if (c == exceptClient) continue;
        if (c->sock == INVALID_SOCKET || !c->helloReceived) continue;
        EnterCriticalSection(&c->outCs);
        c->outQueue.push_back(CsPacket());
        c->outQueue.back().bytes = framed;  // copy - each client send is independent
        LeaveCriticalSection(&c->outCs);
    }
    LeaveCriticalSection(&p->hostClientsCs);
}

// Client side. ONE socket, ONE queue, dead simple. If you are wondering
// why it is not as fancy as the host fanout - because we do not need it
// to be. Client has one peer (the host) and the host deals with relay
// to everyone else. Client just yells at host and host handles the room.
static void PushClientOut(CollabSession::Impl* p, const std::vector<uint8_t>& framed) {
    EnterCriticalSection(&p->clientOutCs);
    p->clientOutQueue.push_back(CsPacket());
    p->clientOutQueue.back().bytes = framed;
    LeaveCriticalSection(&p->clientOutCs);
}

// Common broadcast entry point. Builds the frame ONCE, then routes based
// on role. NO SESSION ACTIVE? Silent no-op, one branch, done. This is
// critical because LevelReader's hook path gets called on every single
// DragFloat tick the user does - we are talking potentially THOUSANDS of
// calls a second while somebody is scrubbing a slider. If this function
// were anything less than a one-compare-and-return in the no-session
// case, the editor would feel like wading through molasses in solo mode.
static void RouteBroadcast(CollabSession::Impl* p, uint32_t type, const uint8_t* payload, uint32_t size) {
    if (p->role == COLLAB_ROLE_NONE) return;
    std::vector<uint8_t> framed;
    BuildPacket(framed, type, payload, size);
    if (p->role == COLLAB_ROLE_HOST) {
        FanoutToAllClients(p, framed, NULL);
    } else if (p->role == COLLAB_ROLE_CLIENT) {
        PushClientOut(p, framed);
    }
}

void CollabSession::BroadcastFieldEdit(const FieldEdit& edit) {
    std::vector<uint8_t> payload;
    SerializeFieldEdit(edit, payload);
    RouteBroadcast(m_impl, CM_FIELD_EDIT,
                   payload.empty() ? NULL : &payload[0],
                   (uint32_t)payload.size());
}
void CollabSession::BroadcastAddEntity(const PendingGameObj& obj) {
    std::vector<uint8_t> payload;
    SerializePendingGameObj(obj, payload);
    RouteBroadcast(m_impl, CM_ADD_ENTITY,
                   payload.empty() ? NULL : &payload[0],
                   (uint32_t)payload.size());
}
void CollabSession::BroadcastDeleteGuid(uint32_t guid) {
    uint8_t payload[4]; memcpy(payload, &guid, 4);
    RouteBroadcast(m_impl, CM_DELETE_GUID, payload, 4);
}
void CollabSession::BroadcastClearFieldEdits() {
    RouteBroadcast(m_impl, CM_CLEAR_FIELD_EDITS, NULL, 0);
}

void CollabSession::SetLevelReader(LevelReader* lr) {
    m_impl->levelReader = lr;
}

// ── Hook forwarders - trampoline from LevelReader to the session ────────
// LevelReader fires these whenever a LOCAL edit happens. C-style function
// pointers, because LevelReader::SetCollabHooks takes raw function
// pointers and I am NOT dragging std::bind / std::function into this
// codebase to dodge that. VS2005 barely has the STL we already use; any
// C++11+ horseshit would detonate the build on contact. Plain old
// static functions, zero vtable, zero indirection beyond the one call.
// That is the whole reason the hook path is fast enough to live inside
// AddFieldEdit without the user noticing the cost.
static void CollabOnFieldEdit(const FieldEdit& e) {
    CollabSession::Instance().BroadcastFieldEdit(e);
}
static void CollabOnAddEntity(const PendingGameObj& o) {
    CollabSession::Instance().BroadcastAddEntity(o);
}
static void CollabOnDeleteGuid(uint32_t g) {
    CollabSession::Instance().BroadcastDeleteGuid(g);
}
static void CollabOnClearFieldEdits() {
    CollabSession::Instance().BroadcastClearFieldEdits();
}

// ── Inbound apply - MAIN THREAD ONLY, called from Tick() ────────────────
// DO NOT CALL THIS FROM A NETWORK THREAD. I fucking mean it. Read the
// words. LevelReader::Apply* reaches into m_fieldEdits, m_pendingObjs,
// m_gameObjs, and m_block1 - the exact same vectors the viewport's
// render loop is iterating over every single frame to draw the scene,
// fill the properties panel, rebuild the Chain graph, refresh the asset
// tree, all of it. Mutate those vectors from a network thread while
// the main thread is mid-iteration and you get the iterator-invalidation
// crash of your dreams: deep stack, call frames you have never heard of,
// unwinding through STL internals, with ZERO useful breadcrumbs pointing
// at the actual offending write. I have debugged exactly this bug
// enough fucking times that the memory of it physically hurts. Main
// thread only. That is the whole fucking point of Tick(). That is WHY
// Tick() exists as a separate method.
//
// Treat bad packets like duds, not detonators. Malformed wire from the
// other peer? Log the type + size, move on with our lives. The session
// does NOT die because the other laptop shipped bullshit. If a peer is
// consistently pumping out garbage we have a bigger problem than this
// function and we will address it when the symptom shows up, not by
// preemptively nuking the session every time something looks slightly
// off. Resilient by default, paranoid where it matters.
static void ApplyInboundPacket(CollabSession::Impl* p, const CsPacket& pkt) {
    if (pkt.bytes.size() < sizeof(CsHeader)) {
        CsLog("apply: got a runt-ass packet, %u bytes - smaller than the fucking header itself. Somebody sent half a message, or the socket is giving us torn reads. Dropping.", (unsigned)pkt.bytes.size());
        return;
    }
    CsHeader h; memcpy(&h, &pkt.bytes[0], sizeof(h));
    const uint8_t* payload = &pkt.bytes[sizeof(CsHeader)];
    const uint8_t* end     = payload + h.size;
    if (!p->levelReader) {
        // No reader wired up yet. Drop silently with a log so it is visible
        // but not spammy - this should never happen after init.
        CsLog("apply: got inbound msg type=0x%X but the LevelReader pointer is fucking NULL. Somebody started a collab session without calling cs.SetLevelReader(&g_levelReader). Fix the viewport init. Silently dropping edit, which is obviously wrong - this is the reason the peer's changes are not showing up.", (unsigned)h.type);
        return;
    }
    switch (h.type) {
    case CM_FIELD_EDIT: {
        FieldEdit e;
        if (!DeserializeFieldEdit(payload, end, e)) {
            CsLog("apply: CM_FIELD_EDIT payload is fucking malformed (%u bytes). Either the peer is running a different wire version (somebody forgot to rebuild one side) or the packet got corrupted in transit. Edit gets dropped on the floor.", (unsigned)h.size);
            return;
        }
        p->levelReader->ApplyBroadcastFieldEdit(e);
        break;
    }
    case CM_ADD_ENTITY: {
        PendingGameObj o;
        if (!DeserializePendingGameObj(payload, end, o)) {
            CsLog("apply: CM_ADD_ENTITY payload is garbage (%u bytes). This one hurts because an entity-add that fails to deserialize means a peer tried to spawn something and we just lost it forever. Version mismatch, corruption, or a PendingGameObj struct change that did not get ALL the updates (serialize + deserialize + protocol version bump).", (unsigned)h.size);
            return;
        }
        // Pass trustIncomingGuid=true so the originator's GUID is honored
        // and the hook DOES NOT re-fire. This is the whole point of the
        // apply path: receive and commit, do not bounce the edit back.
        p->levelReader->AddPendingEntity(o, true);
        break;
    }
    case CM_DELETE_GUID: {
        if (h.size < 4) { CsLog("apply: CM_DELETE_GUID is too small, somebody sent less than 4 goddamn bytes when a delete is literally one u32. That is not a corrupted packet, that is somebody sending absolute horseshit. Dropping."); return; }
        uint32_t g = 0; memcpy(&g, payload, 4);
        p->levelReader->ApplyBroadcastDeleteGuid(g);
        break;
    }
    case CM_CLEAR_FIELD_EDITS: {
        p->levelReader->ApplyBroadcastClearFieldEdits();
        break;
    }
    default:
        CsLog("apply: got a msg type=0x%X (%u bytes) that this build has NEVER FUCKING HEARD OF. Either a peer is running a newer protocol that knows a message this build doesn't, or somebody's shoving random bytes at our port. We drop it and move on - no way we try to parse an unknown opcode and crash for our trouble.", (unsigned)h.type, (unsigned)h.size);
        break;
    }
}

// Called by network threads when they receive a CM_* edit. Copies the raw
// wire bytes into the shared inQueue for the main thread Tick() to drain.
// Takes just type + payload so the network thread does not have to build
// a full frame - this helper handles the header rebuild.
static void EnqueueInbound(CollabSession::Impl* p, uint32_t type, const uint8_t* payload, uint32_t size) {
    CsPacket pkt;
    BuildPacket(pkt.bytes, type, payload, size);
    EnterCriticalSection(&p->inCs);
    p->inQueue.push_back(pkt);
    LeaveCriticalSection(&p->inCs);
}

void CollabSession::Tick() {
    // MAIN THREAD ONLY. Read the comments around ApplyInboundPacket if you
    // forgot why. This is where inbound edits become reality: we drain the
    // queue, hand each packet to the reader, and the reader mutates vectors
    // the render thread is about to read next frame. Touch that sequence
    // from a network thread and you will eat an iterator-invalidation
    // crash at a stack depth so ridiculous the debugger will refuse to
    // make sense of it. Been there, paid the price, wrote this comment.
    //
    // Capped at 32 edits per frame because if some automated tool pukes
    // ten thousand edits into the wire in one burst, we are NOT eating
    // a second-long frame hitch while the grinder grinds through them.
    // We smear the work across frames, the UI keeps breathing, the user
    // sees a tiny delay instead of a full-screen freeze, and the cap
    // never actually trips during real human editing because nobody
    // drags sliders faster than 60 Hz. This is the seatbelt for the
    // pathological-case crash nobody thinks will happen until it happens.
    if (!m_impl->levelReader) return;
    std::vector<CsPacket> batch;
    EnterCriticalSection(&m_impl->inCs);
    const size_t kMaxPerFrame = 32;
    size_t take = m_impl->inQueue.size();
    if (take > kMaxPerFrame) take = kMaxPerFrame;
    if (take > 0) {
        batch.assign(m_impl->inQueue.begin(), m_impl->inQueue.begin() + take);
        m_impl->inQueue.erase(m_impl->inQueue.begin(), m_impl->inQueue.begin() + take);
    }
    LeaveCriticalSection(&m_impl->inCs);
    for (size_t i = 0; i < batch.size(); ++i) ApplyInboundPacket(m_impl, batch[i]);
}

CollabRole   CollabSession::GetRole()        const { return m_impl->role; }
CollabStatus CollabSession::GetStatus()      const { return m_impl->status; }
const char*  CollabSession::GetStatusText()  const { return m_impl->statusText; }
int          CollabSession::GetPeerCount()   const {
    int n = 0;
    EnterCriticalSection(&m_impl->hostClientsCs);
    for (size_t i = 0; i < m_impl->hostClients.size(); ++i) {
        if (m_impl->hostClients[i]->sock != INVALID_SOCKET && m_impl->hostClients[i]->helloReceived) ++n;
    }
    LeaveCriticalSection(&m_impl->hostClientsCs);
    if (m_impl->role == COLLAB_ROLE_CLIENT)
        return (m_impl->status == COLLAB_STATUS_CONNECTED) ? 1 : 0;
    return n;
}
uint16_t     CollabSession::GetPort()        const { return m_impl->boundPort; }
const char*  CollabSession::GetLocalIpText() const { return s_cachedLocalIp.c_str(); }
uint8_t      CollabSession::GetMyGuidPrefix() const { return m_impl->myGuidPrefix; }

} // namespace ZeroEngine
