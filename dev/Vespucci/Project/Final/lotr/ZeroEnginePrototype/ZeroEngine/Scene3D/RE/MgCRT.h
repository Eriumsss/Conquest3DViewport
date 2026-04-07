// MgCRT.h
// Reconstructed C runtime wrappers embedded in the game binary.
//
// Source functions:
//   _asctime_006167D5  — thread-safe asctime() wrapper
//
// The game embeds its own CRT functions. This mirrors those implementations
// so that the reconstruction uses the same time-formatting behavior.

#pragma once
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// _asctime (game address: 006167d5, __cdecl, 1 param)
//
// ASM:
//   PUSH ESI / PUSH EDI
//   MOV EDI, 0xa48408       ; thread-local storage slot
//   CALL 0x00617987          ; get_tls_slot(EDI) → ESI = tls_block*
//   TEST ESI,ESI / JZ use_default_buf
//   CMP  dword[ESI+0x3c], 0  ; check if per-thread buffer allocated
//   JNZ  buf_ready
//   PUSH 1 / PUSH 0x1a (26)  ; alloc 26 bytes
//   CALL 0x00617d64           ; calloc(1, 26) → EAX
//   TEST EAX,EAX / JZ use_default_buf
//   MOV  dword[ESI+0x3c], EAX
// buf_ready:
//   MOV EDI, dword[ESI+0x3c] ; EDI = per-thread 26-byte buffer
// use_default_buf:
//   PUSH [ESP+0xc] / PUSH 0x1a / PUSH EDI
//   CALL 0x006165e7           ; asctime_s(buf, 26, tm_ptr) → 0 on success
//   NEG EAX / SBB EAX,EAX / NOT EAX / AND EAX,EDI  ; return EDI on success, NULL on fail
//
// Our implementation delegates to standard asctime_s with a per-thread buffer.
// ---------------------------------------------------------------------------
char* Mg_asctime(const struct tm* tm_ptr);

#ifdef __cplusplus
} // extern "C"
#endif
