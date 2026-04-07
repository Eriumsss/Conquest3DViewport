// MgCRT.cpp
// Reconstructed from _asctime_006167D5 (__cdecl, 71 addr-count)
//
// Thread-safe asctime() using a per-thread 26-byte buffer, allocated
// via calloc on first use. Exactly mirrors the game's CRT implementation.

#include "MgCRT.h"
#include <string.h>
#include <stdlib.h>

// Per-thread buffer (mirroring game's TLS slot at 0xa48408 + [tls+0x3c])
#ifdef _WIN32
#include <windows.h>
static DWORD  s_tls_slot = TLS_OUT_OF_INDEXES;
static void   init_tls(void) {
    if (s_tls_slot == TLS_OUT_OF_INDEXES)
        s_tls_slot = TlsAlloc();
}
static char* get_tls_buf(void) {
    if (s_tls_slot == TLS_OUT_OF_INDEXES) init_tls();
    char* buf = (char*)TlsGetValue(s_tls_slot);
    if (!buf) {
        buf = (char*)calloc(1, 26);
        if (buf) TlsSetValue(s_tls_slot, buf);
    }
    return buf;
}
#else
// POSIX fallback
static __thread char s_tls_buf[26];
static char* get_tls_buf(void) { return s_tls_buf; }
#endif

// ---------------------------------------------------------------------------
// Mg_asctime — mirrors _asctime_006167D5
//
// The game's asctime_s call (0x006165e7) writes the formatted time string
// into the 26-byte buffer and returns 0 on success.
// The NEG/SBB/NOT/AND idiom: if EAX==0 → returns EDI (the buffer pointer),
//                            if EAX!=0 → returns NULL.
// This is exactly what asctime_s semantics imply.
// ---------------------------------------------------------------------------
char* Mg_asctime(const struct tm* tm_ptr)
{
    if (!tm_ptr) return NULL;

    char* buf = get_tls_buf();
    if (!buf) return NULL;

#ifdef _WIN32
    // asctime_s(buf, 26, tm_ptr) — returns 0 on success
    if (asctime_s(buf, 26, tm_ptr) != 0)
        return NULL;
#else
    // POSIX asctime_r
    if (!asctime_r(tm_ptr, buf))
        return NULL;
#endif
    return buf;
}
