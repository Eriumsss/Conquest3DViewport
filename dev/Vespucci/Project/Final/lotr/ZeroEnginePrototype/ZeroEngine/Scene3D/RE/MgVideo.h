// MgVideo.h
// Reconstructed Bink video subsystem.
// Sources: analyzed/Video/
//
// Functions:
//   FUN_0070d950  (BinkGoto)
//   FUN_0070d961  (BinkPause)
//   FUN_0070d9b7  (BinkOpen)
//   FUN_00749f38  (BinkRegisterFrameBuffers helper)
//   FUN_0074a588  (BinkPlayer init/ctor)
//   FUN_0074ac75  (BinkSetMemory small helper)
//   FUN_0074acb3  (RegisterFrameBuffers — sets up D3D surfaces)
//   FUN_0074af89  (per-frame update — BinkDoFrame etc.)
//   FUN_008a1124  (video thread — message dispatch loop)
//
// Bink SDK function pointers are stored in the game's IAT starting at 0x00970440.
// Player array:    0xcd5ca8,  stride 0x250 per player
// Max streams:     4 per player
// Stream stride:   0x128 bytes
// Global slot:     0x00a56158 = current active player slot index

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Bink IAT function pointer slots (filled by Windows loader):
//   [0x00970440] = BinkGetFrameBuffersInfo
//   [0x00970444] = BinkRegisterFrameBuffers
//   [0x00970448] = BinkOpen
//   [0x0097044c] = BinkSetSoundTrack
//   [0x00970450] = BinkDoFrame
//   [0x00970454] = BinkWait
//   [0x00970458] = BinkShouldSkip
//   [0x0097045c] = BinkPause
//   [0x00970460] = BinkGoto
//   [0x00970464] = BinkNextFrame
//   [0x00970468] = BinkSetVolume
//   [0x0097046c] = BinkGetRealtime
//   [0x00970470] = BinkGetFrameBuffersInfo (second copy?)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// MgBinkStream — layout of one stream entry inside MgBinkPlayer.
// Stride = 0x128 bytes.  Up to 4 streams per player.
//
// Recovered field offsets (relative to stream base):
//   +0x00 : stream handle  (BINK* or NULL)
//   +0x04 : D3D surface ptr slot 1   (BinkRegisterFrameBuffers)
//   +0x0c : D3D surface ptr slot 2
//   +0x14 : D3D surface ptr slot 3
//   +0x1c : D3D surface ptr slot 4
//   +0x24 : D3D surface ptr slot 5
//   +0x110: state flag (0=idle, 1=active, 2=paused, 3=stopped)
//   +0x10c: frame ptr  (current decoded frame data)
//   +0x104: some flags (0x4400 seen during init)
//   +0x108: something ptr
// ---------------------------------------------------------------------------
struct MgBinkStream {
    void*    handle;                    // +0x000 BINK* from BinkOpen
    void*    d3d_surface[5];            // +0x004, +0x00c, +0x014, +0x01c, +0x024
    uint8_t  _pad_02c[0xd8];           // +0x02c … +0x103 (unrecovered)
    uint32_t flags_104;                 // +0x104
    void*    ptr_108;                   // +0x108
    void*    frame_ptr;                 // +0x10c
    uint32_t state;                     // +0x110 (0=idle, 1=active, 2=paused, 3=stopped)
    uint8_t  _pad_114[0x14];           // +0x114 … +0x127
};                                      // total = 0x128 bytes

// ---------------------------------------------------------------------------
// MgBinkPlayer — partial layout recovered from FUN_0074a588 and FUN_0074af89.
//
//   +0x000 : vtable ptr  = 0x9e81e0
//   +0x004 : word flags
//   +0x008 : flags & ~0x3
//   +0x00c : 0
//   +0x010 : stream_data[0]  (first of 4 × 0x128-byte stream entries)
//   +0x118 : stream_ptr[4]   (secondary per-stream ptr array, stride 0x128)
//   +0x131 : [same stream area, differently indexed in FUN_0074af89]
//   +0x4b0 : secondary data block (memset 0xa0 bytes)
//   +0x558 : float_array[4]  (stream playback rates, 0x40 bytes)
//   +0x598 : handle_array[4] (BinkGoto targets, 0x40 bytes; -1=none)
//   +0x5d8 : max_streams = 4
//   +0x5ec : 0
//   +0x790 : active flag (byte; 0 = player inactive)
//   +0x791 : byte = 0
// ---------------------------------------------------------------------------
struct MgBinkPlayer {
    void*         vtable;               // +0x000 = 0x9e81e0
    uint16_t      flags;                // +0x004
    uint8_t       _pad06[2];
    uint32_t      flags2;               // +0x008 (& ~3)
    uint32_t      _unk_0c;             // +0x00c = 0
    MgBinkStream  streams[4];           // +0x010 … +0x50f (4 × 0x128)
    uint8_t       _pad_510[0x08];
    void*         stream_ptrs[4];       // +0x518 … ?  (recovered from LEA [EBX+0x118])
    uint8_t       _pad_530[0x81];
    uint8_t       stream_flags[4];      // +0x131 (byte per stream; accessed in FUN_0074af89)
    uint8_t       _pad_135[0x37b];
    float         playback_rates[4];    // +0x558 (4 × float)
    void*         goto_handles[4];      // +0x598 (4 × BINK*; -1 = none)
    uint8_t       _pad_5b8[0x20];
    uint32_t      max_streams;          // +0x5d8 = 4
    uint8_t       _pad_5dc[0x10];
    uint32_t      _unk_5ec;            // +0x5ec = 0
    uint8_t       _pad_5f0[0x1a0];
    uint8_t       active;               // +0x790
    uint8_t       _unk_791;            // +0x791 = 0
};

// ---------------------------------------------------------------------------
// FUN_0070d950 — MgBink_GotoFrame (17 addr-count)
//
// ASM: PUSH 0 / PUSH frame / PUSH handle / CALL [0x00970460]  (BinkGoto)
// Calls BinkGoto(handle, frame, 0).
// ---------------------------------------------------------------------------
void MgBink_GotoFrame(void* bink_handle, uint32_t frame);

// ---------------------------------------------------------------------------
// FUN_0070d961 — MgBink_Pause (12 addr-count)
//
// ASM: PUSH pause_flag / PUSH handle / CALL [0x0097045c]  (BinkPause)
// ---------------------------------------------------------------------------
void MgBink_Pause(void* bink_handle, int pause);

// ---------------------------------------------------------------------------
// FUN_0070d9b7 — MgBink_Open (103 addr-count)
//
// Opens a Bink file by resolving the path through the engine file system.
// Uses a 260-byte (0x104) stack buffer for the resolved path.
//
// ASM flow:
//   memset(path_buf, 0, 0x103)
//   path_resolver = [0xe56aac]          ; global engine FS object
//   resolve_path(path_resolver, path_buf, input_path)   ; 0x0067a17a
//   if [path_resolver + 0x330] == 0:    ; not in-memory mode
//       adjust_path(path_resolver, path_buf)              ; 0x0067a344
//   BinkOpen(path_buf, flags)           ; [0x00970448]
// Returns: BINK handle (from BinkOpen), or NULL on failure.
// ---------------------------------------------------------------------------
void* MgBink_Open(const char* path, uint32_t flags);

// ---------------------------------------------------------------------------
// FUN_0074ac75 — MgBink_SetMemory small helper (62 addr-count)
//
// ASM: PUSH ptr2 / PUSH [0x00970458]  ; BinkSetMemory(param1, param2)
// Sets Bink's memory callbacks.
// ---------------------------------------------------------------------------
void MgBink_SetMemory(void* alloc_fn, void* free_fn);

// ---------------------------------------------------------------------------
// FUN_0074acb3 — MgBink_RegisterFrameBuffers (726 addr-count)
//
// Calls BinkGetFrameBuffersInfo then BinkRegisterFrameBuffers.
// Sets up D3D texture surfaces for each stream's frame buffer slots.
// After registering: calls BinkRegisterFrameBuffers([0x00970444]).
//
// [EBX+0x00] = stream handle (arg)
// [EBX+0x3c] = stream count
// Per-stream surface ptrs: written to [EBX+0x04], [EBX+0x0c], [EBX+0x14], [EBX+0x1c], [EBX+0x24]
// D3D device: [0x00cd808c]
// Returns: EBX (the stream object)
// ---------------------------------------------------------------------------
struct MgBinkStream* MgBink_RegisterFrameBuffers(struct MgBinkStream* stream);

// ---------------------------------------------------------------------------
// FUN_00749f38 — MgBink_RegisterFrameBuffersHelper (140 addr-count)
//
// Wrapper used inside FUN_0074af89.  Calls BinkRegisterFrameBuffers([0x00970444])
// on the current player's active stream.
// ---------------------------------------------------------------------------
void MgBink_RegisterFrameBuffersHelper(struct MgBinkPlayer* player);

// ---------------------------------------------------------------------------
// FUN_0074a588 — MgBinkPlayer_Init (1469 addr-count — __cdecl, [EBP+8] = this)
//
// Initialises the large MgBinkPlayer object:
//   [this+0x00]  = vtable 0x9e81e0
//   [this+0x04]  = 0 (word)
//   [this+0x08] &= ~3
//   [this+0x0c]  = 0
//   [this+0x5d8] = 4    (max streams)
//   [this+0x5ec] = 0
//   [this+0x791] = 0
//   g_current_player_slot (0xa56158) = 0
//   g_player_count       (0xa550ec) = 1
//   memset([this+0x010], 0, 0x4a0)   — stream data (4 × 0x128)
//   memset([this+0x4b0], 0, 0xa0)
//   memset([this+0x558], 0, 0x40)    — playback_rates
//   memset([this+0x598], 0, 0x40)    — goto_handles (set to all-bits-zero; -1 check uses CMP EAX,-1)
//   BinkSetMemory(alloc_fn, free_fn) — called via MgBink_SetMemory
//   … (large further init, partly deferred)
// ---------------------------------------------------------------------------
void MgBinkPlayer_Init(struct MgBinkPlayer* player);

// ---------------------------------------------------------------------------
// FUN_0074af89 — MgBinkPlayer_FrameUpdate (1046 addr-count, __thiscall ECX=player)
//
// Per-frame update loop over all 4 streams:
//   if !active (player[+0x790]==0): return false
//   Phase 1: for each stream slot i in [0..3]:
//     if stream has a handle:
//       allocate new stream buffer (0x114 bytes via 0x0067e639)
//       format name  via snprintf(buf, 0x104, fmt, stream_handle)
//       set stream flags: state=0x4400, ptr_108=alloc, flag=1
//       copy name/format bytes from player config at [EBX+0x4bc/0x4c0/0x4c1]
//   Phase 2: for each stream ptr at [EBX+0x118]+stride 0x128:
//     if ptr && ptr[+0x110] in {0,1,2}:
//       free old frame ptr (0x0067e602)
//       if new frame available: call MgBink_RegisterFrameBuffers
//       call BinkPause([0x0097045c], ptr->handle, 0)
//       if audio rate: compute volume from int[0xa3df80] * [0xa00970], call BinkSetVolume
//   Phase 3: for each stream at [EBX+0x12c]+stride 0x128:
//     if flag bit0 set at [EBX+EAX*8+0x55c]:
//       if stream ptr: call BinkWait/BinkDoFrame/BinkNextFrame/BinkShouldSkip
//         compute playback_rate via BinkGetFrameBuffersInfo / BinkGetRealtime
//       store float at [EBX+EAX*8+0x558]
//       clear bit0 of [EBX+EAX*8+0x55c]
//   call MgBink_FinalizeFrame (0x0074ab45)
//   return 1
// ---------------------------------------------------------------------------
int MgBinkPlayer_FrameUpdate(struct MgBinkPlayer* player);

// ---------------------------------------------------------------------------
// FUN_008a1124 — MgBink_VideoThread (1407 addr-count, __cdecl [EBP+8]=thread_arg)
//
// Video worker thread.  Message dispatch loop over the player array at 0xcd5ca8.
//
// Thread index derived from arg[+0x10], multiplied by stride 0x250.
// Message struct: [player + 0x4][+0x1] = message_type (uint8_t)
//
// Message type dispatch (switch on type − 6):
//   type 0x06 : (delta 0x00) → open/start video
//   type 0x07 : (delta 0x01) → close / BinkClose([0x00970450])
//   type 0x08..0x0a : (delta 0x02..0x04) → seek / frame operation
//   type 0x0b : (delta 0x05) → pause/resume
//   type 0x0c : (delta 0x06) → set volume
//   type 0x0d : (delta 0x07) → call 0x00401775 (create thread / init)
//   type 0x0e : (delta 0x08) → load "%s.mini" sidecar file
//   type >0x0e : no-op / exit
//
// Uses Sleep() between iterations.
// File path format: "%s.mini"
// Global player base: 0xcd5ca8
// ---------------------------------------------------------------------------
uint32_t __stdcall MgBink_VideoThread(void* thread_arg);

#ifdef __cplusplus
} // extern "C"
#endif
