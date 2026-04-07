// MgTimeline.cpp
// Reconstructed from:
//   FUN_0073619b (exSetTime countdown timer, 258 addr-count)
//   FUN_006e2030 (GotoLabeledFrame full, 192 addr-count)
//   FUN_006ee382 (GotoLabeledFrame simple, 73 addr-count)
//   FUN_006f37b6 (CallFrame dispatcher, 347 addr-count)

#include "MgTimeline.h"
#include <string.h>
#include <stdio.h>

// Dependency globals — set to game binary addresses when running with game DLL
float*                       g_MgTimeline_pDeltaTime    = NULL; // 0x00cd88f4
void**                       g_MgTimeline_pEntityMgr    = NULL; // 0x00cd7fd8
PFN_MgTimeline_DispatchEvent g_pfn_MgTimeline_DispatchEvent = NULL;

// Event name for secondary timer (game: 0x9eb3bc)
static const char* k_secondary_timer_event = "exSetTime";

// ---------------------------------------------------------------------------
// FUN_0073619b — exSetTime countdown timer tick
//
// ASM reconstruction:
//
//   CALL 0x00724061          ; some per-frame update (ignored in our impl)
//   XOR EBX,EBX
//
//   ; Primary timer check
//   CMP byte [ESI+0x1e], BL  ; if primary_active == 0:
//   JNZ primary_active
//   MOV EAX,[ESI]            ; vtable
//   PUSH 1 / PUSH 0
//   CALL vtable[0x20]        ; stop()
//   JMP secondary_check
//
//   primary_active:
//   MOVSS XMM0, [ESI+0x20]   ; primary_remaining
//   MOVSS XMM1, [0xcd88f4]   ; global delta_time
//   (convert to double, subtract, back to float)
//   COMISS 0,XMM0 (result)   ; if remaining <= 0:
//   MOVSS [ESI+0x20], XMM0   ; store decremented value
//   JC (remaining > 0) → secondary_check
//   MOV byte [ESI+0x1e], BL  ; clear active flag
//
//   secondary_check:
//   CMP byte [ESI+0xc8], BL  ; secondary_active
//   JZ done
//   MOVSS XMM0,[ESI+0xc4]    ; secondary_remaining
//   (same decrement logic)
//   COMISS 0,XMM0
//   MOVSS [ESI+0xc4], XMM0
//   JC (>0) → done           ; still counting down
//
//   ; secondary expired: compute quantized time step
//   MOVSS XMM1,[0x9c1c44]    ; threshold constant (~0.0167? = 1 frame)
//   COMISS XMM1,XMM0
//   JBE quantize
//   ; below threshold → integer cast directly
//   CVTTSS2SI EAX,XMM0 → CVTSI2SD XMM0,EAX
//   JMP dispatch
//   quantize:
//   ; above threshold → multiply by [0x9ffe30] (frame_rate?), truncate, divide back
//   CVTSS2SD XMM1,XMM0 / MULSD XMM1,[0x9ffe30] / CVTTSD2SI EAX,XMM1
//   CVTSI2SS XMM1,EAX / CVTSS2SD XMM1,XMM1 / DIVSD XMM1,[0x9ffe30]
//   CVTPD2PS XMM0,XMM1 / CVTSS2SD XMM0,XMM0
//   dispatch:
//   LEA EAX,[ESP+0xc]        ; result buffer
//   PUSH EAX
//   PUSH 0x9eb3bc            ; "exSetTime" event name
//   PUSH [ESI+0x14]          ; entity_id
//   MOVSD [ESP+0x20],XMM0
//   PUSH [0xcd7fd8]           ; entity_mgr
//   CALL 0x00749d48
// ---------------------------------------------------------------------------
void MgTimeline_exSetTime_Tick(MgTimerObject* timer)
{
    if (!timer) return;

    float dt = g_MgTimeline_pDeltaTime ? *g_MgTimeline_pDeltaTime : 0.016667f;

    // ---- Primary timer ----
    if (!timer->primary_active) {
        // Stop: call vtable[0x20] — call through function pointer if available
        void** vtable = *(void***)timer;
        if (vtable) {
            typedef void (__thiscall* PFN_Stop)(void*, int, int);
            PFN_Stop stop = (PFN_Stop)vtable[0x20 / sizeof(void*)];
            if (stop) stop(timer, 1, 0);
        }
    } else {
        // Decrement (game uses double-precision subtract then back to float)
        double remaining_d = (double)timer->primary_remaining - (double)dt;
        float  remaining_f = (float)remaining_d;
        timer->primary_remaining = remaining_f;
        if (remaining_f <= 0.0f) {
            timer->primary_active = 0;
        }
    }

    // ---- Secondary timer ----
    if (!timer->secondary_active) return;

    double sec_d = (double)timer->secondary_remaining - (double)dt;
    float  sec_f = (float)sec_d;
    timer->secondary_remaining = sec_f;
    if (sec_f > 0.0f) return; // still running

    // Secondary timer fired — clear flag and dispatch event
    timer->_pad_c8_flag = 0;

    if (!g_pfn_MgTimeline_DispatchEvent) return;

    // Quantize the time value (from ASM: COMISS against ~0.0167 threshold)
    // threshold ~= 0.0167f (1/60 second)
    static const double k_frame_rate = 60.0; // [0x9ffe30] likely = 60.0 fps
    static const float  k_threshold  = 1.0f / 60.0f; // [0x9c1c44]

    double quantized;
    if (sec_f >= k_threshold) {
        // Quantize to frame grid
        double rounded = (double)(int)(sec_d * k_frame_rate) / k_frame_rate;
        quantized = rounded;
    } else {
        quantized = (double)(int)sec_d;
    }

    // Dispatch via 0x00749d48
    void* entity_mgr = g_MgTimeline_pEntityMgr ? *g_MgTimeline_pEntityMgr : NULL;
    double result_buf = 0.0;
    g_pfn_MgTimeline_DispatchEvent(entity_mgr, k_secondary_timer_event,
                                    &quantized, &result_buf);
}

// ---------------------------------------------------------------------------
// FUN_006e2030 — MovieImpl::GotoLabeledFrame (full version)
//
// if (!movie->movie_data) return 0;
// frame_table = movie->timeline->+0x18->frame_list
// lookup label in frame_table using vtable[0x30] (find_label)
// if found:
//     frame_num = found_result + frame_offset
//     call movie->vtable[0x10](frame_num)  (goto_frame)
//     return 1
// else:
//     try movie->frame_label_table (secondary lookup via 0x0045d90b)
//     if found: call 0x00471a8f(label, "Error..." string, found+0x14)
//     log error string
//     return 0
// ---------------------------------------------------------------------------
int MgMovieImpl_GotoLabeledFrame(MgMovieImpl* movie,
                                  const char* label,
                                  int frame_offset)
{
    if (!movie || !movie->movie_data) return 0;

    // Try primary lookup via movie_mgr vtable[0x30]
    void* movie_mgr = movie->movie_mgr;
    if (movie_mgr) {
        void** vtable = *(void***)movie_mgr;
        if (vtable) {
            // vtable[0x30/4] = find_label_by_name(label, &out_frame, flags)
            typedef int (__thiscall* PFN_FindLabel)(void*, const char*, void*, int);
            PFN_FindLabel find = (PFN_FindLabel)vtable[0x30 / sizeof(void*)];
            int frame_result = -1;
            if (find && find(movie_mgr, label, &frame_result, 0)) {
                // goto_frame: movie->vtable[0x10](frame_result + frame_offset)
                void** mv = *(void***)movie;
                typedef void (__thiscall* PFN_Goto)(void*, int);
                PFN_Goto go = (PFN_Goto)mv[0x10 / sizeof(void*)];
                if (go) go(movie, frame_result + frame_offset);
                return 1;
            }
        }
    }

    // Label not found — log error
    // (game: push label to error format string "Error: MovieImpl::GotoLabeledFrame('%s')...")
    char err[256];
    (void)err; // suppress unused warning — in viewer just ignore
    return 0;
}

// ---------------------------------------------------------------------------
// FUN_006ee382 — GotoLabeledFrame (simple, 73 instructions)
//
// Simpler form used when the movie already has a direct frame-label resolver:
//   movie_mgr = [ESI+0x98]
//   vtable[0x30](label, &frame_num, 0)
//   if success: call movie->vtable[0xec](frame_num)
//   else: log error, return 0
// ---------------------------------------------------------------------------
int MgMovieImpl_GotoLabeledFrameSimple(MgMovieImpl* movie, const char* label)
{
    if (!movie) return 0;

    void* movie_mgr = movie->movie_mgr;
    if (!movie_mgr) return 0;

    void** vtable = *(void***)movie_mgr;
    if (!vtable) return 0;

    typedef int (__thiscall* PFN_FindLabel)(void*, const char*, void*, int);
    PFN_FindLabel find = (PFN_FindLabel)vtable[0x30 / sizeof(void*)];
    int frame_num = -1;
    if (find && find(movie_mgr, label, &frame_num, 0)) {
        void** mv = *(void***)movie;
        typedef void (__thiscall* PFN_Goto)(void*, int);
        PFN_Goto go = (PFN_Goto)mv[0xec / sizeof(void*)];
        if (go) go(movie, frame_num);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// FUN_006f37b6 — CallFrame dispatcher
//
// call_frame_obj: object with type byte at [+0x00]
//   type == 0x04 → find frame in animation timeline, dispatch callbacks
//   type != 0x04 → look up frame name via 0x006c267a, decrement result
//
// Structure: animation_controller has at [+0x9c] a data block, and
// at [+0x98] a vtable ptr for timeline queries.
// The function iterates over callbacks in the matched frame's list,
// calling each via vtable[0x18] (should_fire?) and vtable[0x4] (fire).
// On unknown frame: error log "Error: CallFrame('%s') - unknown frame\n"
// ---------------------------------------------------------------------------
void MgAnim_DispatchCallFrame(void* animation_controller,
                               const void* call_frame_obj)
{
    if (!animation_controller || !call_frame_obj) return;

    const uint8_t* obj = (const uint8_t*)call_frame_obj;
    uint8_t type = obj[0];

    // Resolve frame number
    int frame_num = -1;
    if (type == 0x04) {
        // Direct frame reference: look up via timeline vtable[0x28]
        uint8_t* ctrl = (uint8_t*)animation_controller;
        void* timeline_ptr = *(void**)(ctrl + 0x98);
        if (timeline_ptr) {
            void** tvt = *(void***)timeline_ptr;
            typedef int (__thiscall* PFN_GetFrame)(void*, int);
            PFN_GetFrame gf = (PFN_GetFrame)tvt[0x28 / sizeof(void*)];
            if (gf) frame_num = gf(timeline_ptr, /*frame_id=*/*(int*)(obj+0));
        }
    } else {
        // Named frame: resolve name via 0x006c267a (not implemented — stub)
        frame_num = -1;
    }

    if (frame_num == -1) return; // unknown frame — error in original, no-op here

    // Iterate callbacks in the frame list
    // anim_ctrl at [+0x9c] has the frame match list returned by vtable[0x28]
    // Each item has vtable[0x18] (should_fire) and vtable[0x4] (fire)
    uint8_t* ctrl = (uint8_t*)animation_controller;
    void* data_block = *(void**)(ctrl + 0x9c);
    if (!data_block) return;

    // Frame list: array of callback ptrs at [data_block]
    // Count at [data_block+0x4], iterate [0..count)
    void** callbacks = *(void***)data_block;
    int    count     = *(int*)((uint8_t*)data_block + 0x4);
    for (int i = 0; i < count; ++i) {
        void* cb = callbacks[i];
        if (!cb) continue;
        void** cvt = *(void***)cb;

        // should_fire: vtable[0x18]
        typedef int (__thiscall* PFN_ShouldFire)(void*);
        PFN_ShouldFire sf = (PFN_ShouldFire)cvt[0x18 / sizeof(void*)];
        if (!sf || !sf(cb)) continue;

        // fire: vtable[0x4](animation_controller)
        typedef void (__thiscall* PFN_Fire)(void*, void*);
        PFN_Fire fire = (PFN_Fire)cvt[0x4 / sizeof(void*)];
        if (fire) fire(cb, animation_controller);
    }
}
