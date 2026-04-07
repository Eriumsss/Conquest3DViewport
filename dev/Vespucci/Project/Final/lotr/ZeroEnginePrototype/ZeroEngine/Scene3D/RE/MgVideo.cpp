// MgVideo.cpp
// Reconstructed from:
//   FUN_0070d950  (BinkGoto,   17 addr)
//   FUN_0070d961  (BinkPause,  12 addr)
//   FUN_0070d9b7  (BinkOpen,  103 addr)
//   FUN_00749f38  (RegisterFrameBuffers helper, 140 addr)
//   FUN_0074a588  (BinkPlayerInit, 1469 addr)
//   FUN_0074ac75  (SetMemory small, 62 addr)
//   FUN_0074acb3  (RegisterFrameBuffers, 726 addr)
//   FUN_0074af89  (FrameUpdate, 1046 addr)
//   FUN_008a1124  (VideoThread, 1407 addr)

#include "MgVideo.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Bink SDK IAT function pointers — filled by the Windows loader.
// In standalone mode these are NULL and all functions return safely.
// ---------------------------------------------------------------------------
typedef void* (__cdecl* PFN_BinkOpen)(const char* path, uint32_t flags);
typedef void  (__cdecl* PFN_BinkClose)(void* bink);
typedef int   (__cdecl* PFN_BinkGoto)(void* bink, uint32_t frame, int flags);
typedef int   (__cdecl* PFN_BinkPause)(void* bink, int pause);
typedef int   (__cdecl* PFN_BinkSetMemory)(void* alloc_fn, void* free_fn);
typedef int   (__cdecl* PFN_BinkDoFrame)(void* bink);
typedef int   (__cdecl* PFN_BinkNextFrame)(void* bink);
typedef int   (__cdecl* PFN_BinkShouldSkip)(void* bink);
typedef int   (__cdecl* PFN_BinkWait)(void* bink);
typedef int   (__cdecl* PFN_BinkSetSoundTrack)(void* bink, int track_count, int* track_ids);
typedef int   (__cdecl* PFN_BinkSetVolume)(void* bink, int track_id, int volume);
typedef int   (__cdecl* PFN_BinkGetRealtime)(void* bink, void* realtime_out, int flags);
typedef int   (__cdecl* PFN_BinkGetFrameBuffersInfo)(void* bink, void* fbinfo);
typedef int   (__cdecl* PFN_BinkRegisterFrameBuffers)(void* bink, void* fbinfo);

// IAT slot function pointers (set when game DLL is loaded)
PFN_BinkGetFrameBuffersInfo  g_pfn_BinkGetFrameBuffersInfo  = NULL; // [0x00970440]
PFN_BinkRegisterFrameBuffers g_pfn_BinkRegisterFrameBuffers = NULL; // [0x00970444]
PFN_BinkOpen                 g_pfn_BinkOpen                 = NULL; // [0x00970448]
PFN_BinkSetSoundTrack        g_pfn_BinkSetSoundTrack        = NULL; // [0x0097044c]
PFN_BinkDoFrame              g_pfn_BinkDoFrame              = NULL; // [0x00970450]
PFN_BinkWait                 g_pfn_BinkWait                 = NULL; // [0x00970454]
PFN_BinkShouldSkip           g_pfn_BinkShouldSkip           = NULL; // [0x00970458]
PFN_BinkPause                g_pfn_BinkPause                = NULL; // [0x0097045c]
PFN_BinkGoto                 g_pfn_BinkGoto                 = NULL; // [0x00970460]
PFN_BinkNextFrame            g_pfn_BinkNextFrame            = NULL; // [0x00970464]
PFN_BinkSetVolume            g_pfn_BinkSetVolume            = NULL; // [0x00970468]
PFN_BinkGetRealtime          g_pfn_BinkGetRealtime          = NULL; // [0x0097046c]

// Engine path-resolver global (game: [0xe56aac])
static void* g_path_resolver = NULL;

// Engine FS calls
typedef void (__cdecl* PFN_ResolvePath)(void* resolver, char* out_buf, const char* in_path);
typedef void (__cdecl* PFN_AdjustPath) (void* resolver, char* buf);
static PFN_ResolvePath g_pfn_ResolvePath = NULL; // 0x0067a17a
static PFN_AdjustPath  g_pfn_AdjustPath  = NULL; // 0x0067a344

// ---------------------------------------------------------------------------
// FUN_0070d950 — MgBink_GotoFrame (17 addr-count)
//
// PUSH 0 / PUSH frame / PUSH handle / CALL [0x00970460]
// ---------------------------------------------------------------------------
void MgBink_GotoFrame(void* bink_handle, uint32_t frame)
{
    if (g_pfn_BinkGoto)
        g_pfn_BinkGoto(bink_handle, frame, 0);
}

// ---------------------------------------------------------------------------
// FUN_0070d961 — MgBink_Pause (12 addr-count)
//
// PUSH pause_flag / PUSH handle / CALL [0x0097045c]
// ---------------------------------------------------------------------------
void MgBink_Pause(void* bink_handle, int pause)
{
    if (g_pfn_BinkPause)
        g_pfn_BinkPause(bink_handle, pause);
}

// ---------------------------------------------------------------------------
// FUN_0070d9b7 — MgBink_Open (103 addr-count)
//
// ASM:
//   SUB  ESP, 0x108         ; 264-byte stack frame
//   memset(path_buf, 0, 0x103)  ; 260-char NUL-filled buffer
//   resolver = [0xe56aac]
//   ResolvePath(resolver, path_buf, input_path)   ; 0x0067a17a
//   if [resolver + 0x330] == 0:
//       AdjustPath(resolver, path_buf)             ; 0x0067a344
//   CALL [0x00970448]   ; BinkOpen(path_buf, flags)
//   LEAVE / RET
// ---------------------------------------------------------------------------
void* MgBink_Open(const char* path, uint32_t flags)
{
    if (!g_pfn_BinkOpen) return NULL;

    char path_buf[260];
    memset(path_buf, 0, sizeof(path_buf));

    if (g_pfn_ResolvePath && g_path_resolver) {
        g_pfn_ResolvePath(g_path_resolver, path_buf, path);

        // if resolver is not in "in-memory" mode ([resolver+0x330] == 0):
        uint8_t in_memory = *(const uint8_t*)((const uint8_t*)g_path_resolver + 0x330);
        if (!in_memory && g_pfn_AdjustPath)
            g_pfn_AdjustPath(g_path_resolver, path_buf);
    } else {
        // No resolver — use path directly
        strncpy(path_buf, path, sizeof(path_buf) - 1);
    }

    return g_pfn_BinkOpen(path_buf, flags);
}

// ---------------------------------------------------------------------------
// FUN_0074ac75 — MgBink_SetMemory (62 addr-count)
//
// ASM: PUSH free_fn / PUSH alloc_fn / CALL [0x00970458] (BinkSetMemory)
// Note: the IAT slot [0x00970458] is mapped to _BinkSetMemory@8 (stdcall, 2 args).
// ---------------------------------------------------------------------------
void MgBink_SetMemory(void* alloc_fn, void* free_fn)
{
    if (g_pfn_BinkWait) {
        // The actual call in FUN_0074ac75 is to _BinkSetMemory@8 which is in
        // slot [0x00970458]. Our g_pfn_BinkWait maps to [0x00970454] (BinkWait).
        // BinkSetMemory occupies [0x00970458] — we expose a separate pointer.
        (void)alloc_fn; (void)free_fn;
        // TODO: add g_pfn_BinkSetMemory when IAT is fully wired.
    }
}

// ---------------------------------------------------------------------------
// FUN_0074acb3 — MgBink_RegisterFrameBuffers (726 addr-count)
//
// Sets up D3D texture surfaces for Bink frame output.
// [arg = stream ptr; stream->handle at [arg+0x00]]
// Stream count at [arg+0x3c].
//
// Per stream (up to count):
//   1. Get frame buffer desc via BinkGetFrameBuffersInfo
//   2. Allocate D3D texture via IDirect3DDevice9::CreateTexture (vtable+0x18)
//      and CreateTexture (vtable+0x1c) for each surface
//   3. Store surface ptrs at [stream+0x04], [+0x0c], [+0x14], [+0x1c], [+0x24]
//   4. Call BinkRegisterFrameBuffers with device from [0x00cd808c]
//
// After all streams: call BinkRegisterFrameBuffers one more time at top level.
// Returns: stream pointer (EBX on exit = [EBP+8]).
// ---------------------------------------------------------------------------
struct MgBinkStream* MgBink_RegisterFrameBuffers(struct MgBinkStream* stream)
{
    if (!stream) return NULL;
    if (!g_pfn_BinkGetFrameBuffersInfo || !g_pfn_BinkRegisterFrameBuffers) return stream;

    // NOTE: Full D3D texture allocation and stream registration require
    // IDirect3DDevice9 from [0x00cd808c] and vtable dispatch.
    // Stub: call the SDK functions with available data.
    if (stream->handle) {
        // BinkGetFrameBuffersInfo fills a local descriptor on stack (0x3c bytes)
        uint8_t fb_info[0x3c];
        memset(fb_info, 0, sizeof(fb_info));
        g_pfn_BinkGetFrameBuffersInfo(stream->handle, fb_info);

        // BinkRegisterFrameBuffers registers the D3D surfaces
        g_pfn_BinkRegisterFrameBuffers(stream->handle, fb_info);
    }
    return stream;
}

// ---------------------------------------------------------------------------
// FUN_00749f38 — MgBink_RegisterFrameBuffersHelper (140 addr-count)
//
// Called from FUN_0074af89 Phase 2 to re-register frame buffers after
// a stream state change.  Calls BinkRegisterFrameBuffers([0x00970444]).
// ---------------------------------------------------------------------------
void MgBink_RegisterFrameBuffersHelper(struct MgBinkPlayer* player)
{
    if (!player) return;
    // In the game, this accesses the current slot via [0xa36984] and calls
    // BinkRegisterFrameBuffers on the active stream.
    // Stub: iterate all 4 streams and re-register non-NULL handles.
    for (int i = 0; i < 4; i++) {
        MgBinkStream* s = &player->streams[i];
        if (s->handle)
            MgBink_RegisterFrameBuffers(s);
    }
}

// ---------------------------------------------------------------------------
// FUN_0074a588 — MgBinkPlayer_Init (1469 addr-count)
//
// ASM highlights:
//   [EBX+0x00]  = 0x9e81e0  (vtable)
//   [EBX+0x04]  = 0 (word)
//   [EBX+0x08] &= ~3
//   [EBX+0x0c]  = 0
//   [EBX+0x5d8] = 4
//   [EBX+0x5ec] = 0
//   [EBX+0x791] = 0
//   [0xa56158]  = 0  (global player slot index)
//   [0xa550ec]  = 1  (global player count)
//   memset([EBX+0x010], 0, 0x4a0)
//   memset([EBX+0x4b0], 0, 0xa0)
//   memset([EBX+0x558], 0, 0x40)
//   memset([EBX+0x598], 0, 0x40)   — goto_handles (-1 check uses CMP; after memset = 0)
//   [EBX+0x608] initialised via 0x0068ebf3 (list init)
//   BinkSetMemory called via 0x0074ac75
//   … large further init (stream slot creation, vtable inits) — deferred
// ---------------------------------------------------------------------------
void MgBinkPlayer_Init(struct MgBinkPlayer* player)
{
    if (!player) return;

    player->flags2     = player->flags2 & ~(uint32_t)3;
    player->flags      = 0;
    player->_unk_0c    = 0;
    player->max_streams = 4;
    player->_unk_5ec   = 0;
    player->active     = 0;
    player->_unk_791   = 0;

    // Zero stream data areas
    memset(&player->streams[0], 0, sizeof(player->streams));        // 4 × 0x128
    memset((uint8_t*)player + 0x4b0, 0, 0xa0);
    memset(player->playback_rates, 0, sizeof(player->playback_rates));
    memset(player->goto_handles,   0, sizeof(player->goto_handles));

    // Set all goto_handles to "no pending goto" (-1 in uint32 = 0xffffffff)
    // Note: game checks CMP EAX,-1 so handles start at NULL (0) which means
    // "no pending goto" is represented differently at init vs runtime.

    // BinkSetMemory registration (game: calls 0x0074ac75)
    MgBink_SetMemory(NULL, NULL);

    // Note: additional per-stream slot initialisation follows in the game
    // (loop at 0x0074a5eb that inits audio tracks, format descriptors, etc.)
    // — deferred pending further analysis of sub-functions.
}

// ---------------------------------------------------------------------------
// FUN_0074af89 — MgBinkPlayer_FrameUpdate (1046 addr-count, thiscall ECX=player)
//
// See header for full phase breakdown.  This reconstruction implements
// the structure accurately; sub-calls that require game-binary vtables are
// replaced with the matching IAT calls where available.
// ---------------------------------------------------------------------------
int MgBinkPlayer_FrameUpdate(struct MgBinkPlayer* player)
{
    if (!player) return 0;
    if (!player->active) return 0;

    // Phase 1: process stream handles (allocate buffers, init format)
    // (involves game-internal alloc at 0x0067e639 and snprintf for debug names)
    // Phase 2: advance active streams, manage pause state, register frame bufs
    for (int i = 0; i < 4; i++) {
        MgBinkStream* s = &player->streams[i];
        if (!s->handle) continue;

        uint32_t state = s->state;
        if (state == 0 || state == 1 || state == 2) {
            // Free old frame ptr (game: 0x0067e602)
            // s->frame_ptr = NULL;  // not freed here in standalone

            // Re-register frame buffers if stream is active
            MgBink_RegisterFrameBuffers(s);

            // BinkPause(handle, 0) — unpause
            if (g_pfn_BinkPause)
                g_pfn_BinkPause(s->handle, 0);
        }
    }

    // Phase 3: per-stream decode loop
    for (int i = 0; i < 4; i++) {
        MgBinkStream* s = &player->streams[i];
        if (!s->handle) continue;

        if (g_pfn_BinkWait)
            g_pfn_BinkWait(s->handle);

        if (g_pfn_BinkDoFrame)
            g_pfn_BinkDoFrame(s->handle);

        int should_skip = 0;
        if (g_pfn_BinkShouldSkip)
            should_skip = g_pfn_BinkShouldSkip(s->handle);

        if (!should_skip && g_pfn_BinkNextFrame)
            g_pfn_BinkNextFrame(s->handle);
    }

    return 1;
}

// ---------------------------------------------------------------------------
// FUN_008a1124 — MgBink_VideoThread (1407 addr-count)
//
// Message loop skeleton.
// Thread arg struct layout (partial):
//   arg[+0x10] = thread slot index
//
// Player array: 0xcd5ca8, stride 0x250.
// Message at: player[+0x04][+0x01] = type byte.
//
// Message types:
//   0x06  open / start player
//   0x07  close (BinkClose)
//   0x08..0x0a  seek / frame control
//   0x0b  pause / resume
//   0x0c  set volume
//   0x0d  thread create / init
//   0x0e  load "%s.mini" sidecar file
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
#define MG_SLEEP(ms) Sleep(ms)
#else
#include <time.h>
static void MG_SLEEP(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

// Message type constants
#define MG_BINK_MSG_OPEN        0x06
#define MG_BINK_MSG_CLOSE       0x07
#define MG_BINK_MSG_SEEK_A      0x08
#define MG_BINK_MSG_SEEK_B      0x09
#define MG_BINK_MSG_SEEK_C      0x0a
#define MG_BINK_MSG_PAUSE       0x0b
#define MG_BINK_MSG_VOLUME      0x0c
#define MG_BINK_MSG_INIT        0x0d
#define MG_BINK_MSG_LOAD_MINI   0x0e

uint32_t __stdcall MgBink_VideoThread(void* thread_arg)
{
    if (!thread_arg) return 0;

    // thread_arg[+0x10] = slot index
    uint32_t slot = *(uint32_t*)((uint8_t*)thread_arg + 0x10);

    // player = ((MgBinkPlayer*)0xcd5ca8) + slot * 0x250
    // (Only valid with game binary loaded at correct base address)
    // In standalone mode we have no access to the player array.
    (void)slot;

    // Message dispatch loop (game: while(1) { process_message(); Sleep(N); })
    // Each message struct lives at player[+0x4], type byte at [+0x01].
    //
    // The loop body is reconstructed as a switch on the message type.
    // Full per-message logic requires vtable access to the player object —
    // deferred pending game binary hookup.
    //
    // Documented message handling summary:
    //   MG_BINK_MSG_OPEN     → MgBinkPlayer_Init + MgBink_Open
    //   MG_BINK_MSG_CLOSE    → BinkClose([0x00970450]) on all open streams
    //   MG_BINK_MSG_SEEK_*   → BinkGoto on specified stream
    //   MG_BINK_MSG_PAUSE    → BinkPause toggle
    //   MG_BINK_MSG_VOLUME   → BinkSetVolume
    //   MG_BINK_MSG_INIT     → calls 0x00401775 (thread subsystem init)
    //   MG_BINK_MSG_LOAD_MINI→ snprintf(path, "%s.mini"); MgBink_Open
    //
    // Standalone: return immediately.
    return 0;
}
