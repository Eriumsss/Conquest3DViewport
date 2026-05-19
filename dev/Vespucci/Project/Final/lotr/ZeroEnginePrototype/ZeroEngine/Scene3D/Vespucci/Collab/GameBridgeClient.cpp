// Vespucci <-> running-game bridge client.  VC8 / C++03 compatible.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <vector>

#include "GameBridgeClient.h"
#include "../Engine/DLL/D3D9Proxy/src/vespucci_bridge.h"

#pragma comment(lib, "d3d9.lib")

namespace GameBridge {

namespace {

HANDLE           g_pipe      = INVALID_HANDLE_VALUE;
HANDLE           g_thread    = NULL;
volatile LONG    g_stopFlag  = 0;

CRITICAL_SECTION g_stateCs;
bool             g_stateInit = false;
Snapshot         g_inflight;          // I/O thread writes
Snapshot         g_current;           // main thread reads / renders

CRITICAL_SECTION g_outCs;
std::deque<std::vector<unsigned char> > g_outQueue;

FILE*            g_log       = NULL;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); fputc('\n', g_log); fflush(g_log);
    va_end(ap);
}

void OpenLog() {
    if (g_log) return;
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(path, "vespucci_bridge_client.log");
    fopen_s(&g_log, path, "w");
    Log("[vbridge] client log opened");
}

inline unsigned char* VecPtr(std::vector<unsigned char>& v) {
    return v.empty() ? (unsigned char*)NULL : &v[0];
}

// ── Framed I/O ───────────────────────────────────────────────────────────
bool ReadExact(HANDLE h, void* buf, DWORD n) {
    DWORD total = 0, got = 0;
    while (total < n) {
        if (!ReadFile(h, (char*)buf + total, n - total, &got, NULL)) return false;
        if (got == 0) return false;
        total += got;
    }
    return true;
}

bool ReadFrame(HANDLE h, uint32_t& msgType, std::vector<unsigned char>& payload) {
    VbMsgHeader hdr;
    if (!ReadExact(h, &hdr, sizeof(hdr))) return false;
    msgType = hdr.type;
    payload.clear();
    if (hdr.size == 0) return true;
    if (hdr.size > 16u * 1024u * 1024u) return false;
    payload.resize(hdr.size);
    return ReadExact(h, &payload[0], hdr.size);
}

void Enqueue(uint32_t msgType, const void* payload, uint32_t size) {
    std::vector<unsigned char> buf(sizeof(VbMsgHeader) + size);
    VbMsgHeader hdr;
    hdr.type = msgType;
    hdr.size = size;
    memcpy(&buf[0], &hdr, sizeof(hdr));
    if (payload && size) memcpy(&buf[0] + sizeof(hdr), payload, size);
    EnterCriticalSection(&g_outCs);
    if (g_outQueue.size() > 64) g_outQueue.pop_front();
    g_outQueue.push_back(buf);  // copy; no std::move in C++03
    LeaveCriticalSection(&g_outCs);
}

// ── Message parsing into g_inflight ─────────────────────────────────────
void HandleMessage(uint32_t msgType, const unsigned char* payload, uint32_t size) {
    EnterCriticalSection(&g_stateCs);
    switch (msgType) {
    case VB_MSG_HELLO: {
        if (size >= sizeof(VbHello)) {
            const VbHello* h = reinterpret_cast<const VbHello*>(payload);
            g_inflight.gamePid = h->gamePid;
            strncpy_s(g_inflight.gameExe, h->exeName, _TRUNCATE);
            g_inflight.connected = true;
            Log("[vbridge] HELLO pid=%u exe=%s", h->gamePid, h->exeName);
        }
        break;
    }
    case VB_MSG_FRAME_BEGIN: {
        if (size >= sizeof(VbFrameBegin)) {
            const VbFrameBegin* f = reinterpret_cast<const VbFrameBegin*>(payload);
            g_inflight.lastFrameIdx = f->frameIdx;
        }
        break;
    }
    case VB_MSG_CAMERA: {
        if (size >= sizeof(VbCamera)) {
            const VbCamera* c = reinterpret_cast<const VbCamera*>(payload);
            memcpy(g_inflight.camPos, c->pos, sizeof(c->pos));
            memcpy(g_inflight.camFwd, c->fwd, sizeof(c->fwd));
            g_inflight.camFov     = c->fovY;
            g_inflight.viewportW  = c->viewportW;
            g_inflight.viewportH  = c->viewportH;
        }
        break;
    }
    case VB_MSG_PLAYER: {
        if (size >= sizeof(VbPlayer)) {
            const VbPlayer* p = reinterpret_cast<const VbPlayer*>(payload);
            memcpy(g_inflight.playerPos, p->pos, sizeof(p->pos));
            g_inflight.playerHp          = p->health;
            g_inflight.playerTeam        = p->team;
            g_inflight.playerCreaturePtr = p->creaturePtr;
            g_inflight.playerValid       = true;
        }
        break;
    }
    case VB_MSG_ENTITY_SNAPSHOT: {
        if (size < sizeof(VbEntitySnapshot)) break;
        const VbEntitySnapshot* hdr = reinterpret_cast<const VbEntitySnapshot*>(payload);
        uint32_t count = hdr->count;
        uint32_t need = sizeof(VbEntitySnapshot) + count * sizeof(VbEntityEntry);
        if (size < need) break;
        const VbEntityEntry* entries = reinterpret_cast<const VbEntityEntry*>(
            payload + sizeof(VbEntitySnapshot));
        g_inflight.entities.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            const VbEntityEntry& e = entries[i];
            EntitySnap& o = g_inflight.entities[i];
            o.addr        = e.addr;
            memcpy(o.pos, e.pos, sizeof(e.pos));
            memcpy(o.velocity, e.velocity, sizeof(e.velocity));
            o.health      = e.health;
            o.team        = e.team;
            o.flags       = e.flags;
        }
        break;
    }
    case VB_MSG_LEVEL_LOADED: {
        if (size >= sizeof(VbLevelLoaded)) {
            const VbLevelLoaded* l = reinterpret_cast<const VbLevelLoaded*>(payload);
            strncpy_s(g_inflight.levelName, l->levelName, _TRUNCATE);
            Log("[vbridge] level loaded: %s", l->levelName);
        }
        break;
    }
    case VB_MSG_LEVEL_UNLOADED:
        g_inflight.levelName[0] = '\0';
        break;
    default:
        break;
    }
    LeaveCriticalSection(&g_stateCs);
}

// ── I/O thread ───────────────────────────────────────────────────────────
DWORD WINAPI IoThread(LPVOID) {
    OpenLog();
    Log("[vbridge] I/O thread started");

    while (!InterlockedCompareExchange(&g_stopFlag, 0, 0)) {
        HANDLE h = CreateFileA(VESPUCCI_PIPE_NAME,
                                GENERIC_READ | GENERIC_WRITE,
                                0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        g_pipe = h;
        Log("[vbridge] connected to game");

        EnterCriticalSection(&g_stateCs);
        g_inflight.connected = true;
        LeaveCriticalSection(&g_stateCs);

        while (!InterlockedCompareExchange(&g_stopFlag, 0, 0)) {
            // Drain outbound queue first.
            for (;;) {
                std::vector<unsigned char> msg;
                EnterCriticalSection(&g_outCs);
                if (!g_outQueue.empty()) {
                    msg = g_outQueue.front();
                    g_outQueue.pop_front();
                }
                LeaveCriticalSection(&g_outCs);
                if (msg.empty()) break;
                DWORD w = 0;
                if (!WriteFile(h, &msg[0], (DWORD)msg.size(), &w, NULL)) {
                    Log("[vbridge] write failed err=%lu", GetLastError());
                    goto disconnect;
                }
            }

            // Peek for inbound.
            DWORD avail = 0;
            if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
                Log("[vbridge] peek failed err=%lu", GetLastError());
                break;
            }
            if (avail > 0) {
                uint32_t mt = 0;
                std::vector<unsigned char> payload;
                if (!ReadFrame(h, mt, payload)) {
                    Log("[vbridge] read failed — disconnect");
                    break;
                }
                HandleMessage(mt, payload.empty() ? (const unsigned char*)NULL : &payload[0],
                              (uint32_t)payload.size());
            } else {
                Sleep(2);
            }
        }
    disconnect:
        CloseHandle(h);
        g_pipe = INVALID_HANDLE_VALUE;

        EnterCriticalSection(&g_stateCs);
        g_inflight.connected = false;
        g_inflight.entities.clear();
        g_inflight.playerValid = false;
        LeaveCriticalSection(&g_stateCs);

        Log("[vbridge] disconnected, retrying");
        Sleep(500);
    }

    if (g_log) { fclose(g_log); g_log = NULL; }
    return 0;
}

}  // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────
void Init() {
    if (g_thread) return;
    memset(&g_inflight, 0, sizeof(g_inflight));
    memset(&g_current,  0, sizeof(g_current));
    // Reset vectors (memset zeroed their internal state, which is fine for empty
    // vectors; we re-init here via assignment in case of weird STL).
    g_inflight.entities = std::vector<EntitySnap>();
    g_current.entities  = std::vector<EntitySnap>();
    InitializeCriticalSection(&g_stateCs);
    InitializeCriticalSection(&g_outCs);
    g_stateInit = true;
    g_thread = CreateThread(NULL, 0, IoThread, NULL, 0, NULL);
}

void Shutdown() {
    InterlockedExchange(&g_stopFlag, 1);
    // Force-close the pipe handle to unblock any read — no CancelIoEx on VC8.
    HANDLE h = g_pipe;
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stateInit) {
        DeleteCriticalSection(&g_stateCs);
        DeleteCriticalSection(&g_outCs);
        g_stateInit = false;
    }
}

void Tick() {
    if (!g_stateInit) return;
    EnterCriticalSection(&g_stateCs);
    g_current = g_inflight;
    LeaveCriticalSection(&g_stateCs);
}

const Snapshot& CurrentSnapshot() { return g_current; }

void SendCommand(uint32_t msgType, const void* payload, uint32_t size) {
    if (!g_stateInit) return;
    Enqueue(msgType, payload, size);
}

// ── D3D9 ghost rendering ────────────────────────────────────────────────
namespace {

struct ColorVertex {
    float    x, y, z;
    uint32_t color;
};
const DWORD COLOR_FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;

uint32_t TeamColor(uint32_t team, uint32_t flags) {
    if (flags & 0x1)      return 0xFFFFFFFFu;   // player — white
    switch (team) {
    case 1:  return 0xFF00FF00u;   // good
    case 2:  return 0xFFFF0000u;   // evil
    case 0:  return 0xFFFFFF00u;   // neutral
    default: return 0xFFAAAAAAu;   // unknown
    }
}

inline void Push(std::vector<ColorVertex>& out, float x, float y, float z, uint32_t c) {
    ColorVertex v;
    v.x = x; v.y = y; v.z = z; v.color = c;
    out.push_back(v);
}

void DrawCrossLines(std::vector<ColorVertex>& out, const float p[3],
                    float halfSize, uint32_t color) {
    const float s = halfSize;
    Push(out, p[0] - s, p[1],     p[2],     color);
    Push(out, p[0] + s, p[1],     p[2],     color);
    Push(out, p[0],     p[1] - s, p[2],     color);
    Push(out, p[0],     p[1] + s, p[2],     color);
    Push(out, p[0],     p[1],     p[2] - s, color);
    Push(out, p[0],     p[1],     p[2] + s, color);
}

void DrawSegment(std::vector<ColorVertex>& out, const float a[3],
                 const float b[3], uint32_t color) {
    Push(out, a[0], a[1], a[2], color);
    Push(out, b[0], b[1], b[2], color);
}

}  // anonymous namespace

void Render(IDirect3DDevice9* device) {
    if (!device || !g_stateInit) return;
    const Snapshot& s = g_current;
    if (!s.connected) return;

    std::vector<ColorVertex> verts;
    verts.reserve(s.entities.size() * 6 + 12);

    for (size_t i = 0; i < s.entities.size(); ++i) {
        const EntitySnap& e = s.entities[i];
        DrawCrossLines(verts, e.pos, 4.0f, TeamColor(e.team, e.flags));
    }

    if (s.camPos[0] != 0.0f || s.camPos[1] != 0.0f || s.camPos[2] != 0.0f) {
        const uint32_t camCol = 0xFF00FFFFu;
        DrawCrossLines(verts, s.camPos, 2.0f, camCol);
        float tip[3];
        tip[0] = s.camPos[0] + s.camFwd[0] * 6.0f;
        tip[1] = s.camPos[1] + s.camFwd[1] * 6.0f;
        tip[2] = s.camPos[2] + s.camFwd[2] * 6.0f;
        DrawSegment(verts, s.camPos, tip, 0xFFFF00FFu);
    }

    if (verts.empty()) return;

    DWORD prevLighting = 0, prevZEnable = 0, prevAlpha = 0, prevFVF = 0;
    IDirect3DVertexShader9* prevVS = NULL;
    IDirect3DPixelShader9*  prevPS = NULL;
    device->GetRenderState(D3DRS_LIGHTING,     &prevLighting);
    device->GetRenderState(D3DRS_ZENABLE,      &prevZEnable);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &prevAlpha);
    device->GetFVF(&prevFVF);
    device->GetVertexShader(&prevVS);
    device->GetPixelShader(&prevPS);

    device->SetVertexShader(NULL);
    device->SetPixelShader(NULL);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE,  D3DZB_TRUE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetFVF(COLOR_FVF);

    device->DrawPrimitiveUP(D3DPT_LINELIST,
                            (UINT)(verts.size() / 2),
                            &verts[0],
                            sizeof(ColorVertex));

    device->SetRenderState(D3DRS_LIGHTING,     prevLighting);
    device->SetRenderState(D3DRS_ZENABLE,      prevZEnable);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, prevAlpha);
    device->SetFVF(prevFVF);
    device->SetVertexShader(prevVS);
    device->SetPixelShader(prevPS);
    if (prevVS) prevVS->Release();
    if (prevPS) prevPS->Release();
}

}  // namespace GameBridge
