// MgTimeline.h
// Reconstructed timeline, cutscene, and animation call-frame dispatch.
//
// Source functions:
//   FUN_0073619b  exSetTime — countdown timer tick (EC_time)
//   FUN_006e2030  MovieImpl::GotoLabeledFrame (EC_time)
//   FUN_006ee382  MovieImpl::GotoLabeledFrame variant (EC_time)
//   FUN_006f37b6  Animation CallFrame dispatcher (EC_time)
//
// exSetTime (FUN_0073619b):
//   ECX = this (some timer object)
//   On each tick: decrements [ESI+0x20] by global delta-time [0x00cd88f4]
//   When countdown reaches 0: clears active flag [ESI+0x1e], calls stop vtable fn
//   Also handles secondary timer at [ESI+0xc4] (offset 0xc8 = active flag)
//   Secondary timer: dispatches a named event via 0x00749d48 when it fires
//   Event dispatch: PUSH entity_id([ESI+0x14]), event_name(0x9eb3bc),
//                   &double_arg([ESP+0xc]), PUSH entity_mgr([0x00cd7fd8])
//
// GotoLabeledFrame (FUN_006e2030, FUN_006ee382):
//   Scaleform/Flash movie implementation.
//   Looks up a frame label in the movie's frame table.
//   On success: calls vtable[0x10] to seek to that frame.
//   On failure: logs "Error: MovieImpl::GotoLabeledFrame('%s') unknown label\n"
//
// CallFrame (FUN_006f37b6):
//   Animation call-frame dispatcher.
//   Reads a CallFrame object (type byte at [ECX] == 0x4).
//   Looks up the frame in the animation timeline.
//   For each matching callback: calls vtable[0x18] (should_fire) → vtable[0x4] (fire).
//   On unknown frame: logs "Error: CallFrame('%s') - unknown frame\n"

#pragma once
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// External dependencies
// ---------------------------------------------------------------------------

// Delta-time global (game: 0x00cd88f4)
extern float* g_MgTimeline_pDeltaTime;

// Global entity manager (game: 0x00cd7fd8)
extern void** g_MgTimeline_pEntityMgr;

// Dispatch timeline event (game: 0x00749d48)
// __cdecl void dispatch(void* entity_mgr, const char* event_name,
//                       double* value_ptr, void* result_buf)
typedef void (__cdecl* PFN_MgTimeline_DispatchEvent)(
    void* entity_mgr, const char* event_name, double* val, void* result);
extern PFN_MgTimeline_DispatchEvent g_pfn_MgTimeline_DispatchEvent;

// ---------------------------------------------------------------------------
// Timer object layout (inferred from FUN_0073619b):
//   +0x14: entity_id (uint32_t) — passed to event dispatch
//   +0x1e: uint8_t primary_timer_active
//   +0x20: float   primary_timer_remaining
//   +0xc4: float   secondary_timer_remaining
//   +0xc8: uint8_t secondary_timer_active
//   vtable[0x20]: stop() method
// ---------------------------------------------------------------------------
struct MgTimerObject {
    uint8_t  _pad_00[0x14];
    uint32_t entity_id;        // +0x14
    uint8_t  _pad_18[0x08];
    uint8_t  primary_active;   // +0x1e
    uint8_t  _pad_1f;
    float    primary_remaining;// +0x20
    uint8_t  _pad_24[0xA0];
    float    secondary_remaining; // +0xc4
    uint8_t  _pad_c8_flag;     // +0xc8
    // vtable at [this+0x00] (implicit)
};

// ---------------------------------------------------------------------------
// Scaleform movie object layout (inferred from FUN_006e2030 / 006ee382):
//   +0x00: vtable*
//   +0x10: frame label hash table (for GotoLabeledFrame alternate path)
//   +0x24: movie_data ptr (NULL → no-op)
//   +0x28: timeline ptr (→ +0x18: frame_table ptr)
//   +0x98: movie_mgr ptr
// ---------------------------------------------------------------------------
struct MgMovieImpl {
    void**  vtable;         // +0x00
    uint8_t _pad_04[0x0C];
    void*   frame_label_table; // +0x10
    uint8_t _pad_14[0x10];
    void*   movie_data;     // +0x24
    void*   timeline;       // +0x28 → +0x18 → frame_table
    uint8_t _pad_2c[0x6C];
    void*   movie_mgr;      // +0x98
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// FUN_0073619b — tick the countdown timers, dispatch events when they fire.
// Call once per frame with the timer object.
void MgTimeline_exSetTime_Tick(MgTimerObject* timer);

// FUN_006e2030 — seek a Scaleform movie to a labeled frame + offset.
// Returns 1 on success, 0 if label not found.
int MgMovieImpl_GotoLabeledFrame(MgMovieImpl* movie,
                                  const char* label,
                                  int frame_offset);

// FUN_006ee382 — simpler GotoLabeledFrame variant (direct vtable dispatch).
// Returns 1 on success, 0 if label not found.
int MgMovieImpl_GotoLabeledFrameSimple(MgMovieImpl* movie,
                                        const char* label);

// FUN_006f37b6 — dispatch a CallFrame event from the animation timeline.
// call_frame_obj: pointer to a CallFrame object (type byte 0x4 at [obj+0]).
// animation_controller: the owning animation controller (this in ECX).
void MgAnim_DispatchCallFrame(void* animation_controller,
                               const void* call_frame_obj);

#ifdef __cplusplus
} // extern "C"
#endif
