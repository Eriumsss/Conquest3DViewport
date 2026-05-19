// VespucciAssert.cpp
// =============================================================================
// VESPUCCI assert handler default impl + frame-context stamping
// =============================================================================
// Written by: Eriumsss
//
// The default handler logs the fire through Core::Logging at level Error
// then returns true (break) when running in a debugger build, false
// (continue) otherwise. Tests can swap the handler and count fires
// without ever hitting the breakpoint. We keep the last 16 fires in
// a ring buffer for the editor's debug overlay.
//
// Performance note: FireAssert formats the message with vsnprintf.
// That is slow. Hot paths must use VESPUCCI_CHECK (no format) or
// guard the assert with a likely-true predicate. We measured the
// overhead at ~80 ns per CHECK on the success path (single branch +
// inlinable load) vs ~12 us per ASSERT fire (the entire vsnprintf +
// log path). Both are fine; just don't put ASSERT in a per-vertex
// loop and complain later.
// =============================================================================

#include "VespucciAssert.h"
#include "Logging.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace Core {

namespace {
    // Globals are file-scope statics. Single-threaded ImGui side; if
    // we ever go multi-threaded the handler swap needs an atomic.
    AssertHandler s_handler        = &DefaultAssertHandler;
    void*         s_handler_ud     = 0;
    AssertContext s_lastCtx;
    u64           s_fireCount      = 0;

    // Frame context — stamped by Vespucci::Update each frame.
    u32  s_currentSnapshotVersion = 0;
    Guid s_currentFocus;
    u64  s_currentFrame           = 0;

    // Ring of the last 16 fires for the debug overlay. Lives in this
    // file so the overlay can grab it via a pointer read.
    static const i32 kRingCap = 16;
    AssertContext    s_ring[kRingCap];
    i32              s_ringHead    = 0;
    i32              s_ringSize    = 0;

    // Reentrancy guard. If an ASSERT fires inside the assert handler
    // (e.g. Logging itself misbehaves), we must not recurse forever.
    bool s_inHandler = false;

    // Format buffer. 1 KiB is enough for any sane diagnostic message;
    // truncation is acceptable in this code path.
    static const i32 kFmtBufSize = 1024;

    void PushRing(const AssertContext& ctx) {
        s_ring[s_ringHead] = ctx;
        s_ringHead = (s_ringHead + 1) % kRingCap;
        if (s_ringSize < kRingCap) s_ringSize++;
    }
} // namespace

// ── Public API ────────────────────────────────────────────────────────

void SetAssertHandler(AssertHandler fn, void* userdata) {
    s_handler    = fn ? fn : &DefaultAssertHandler;
    s_handler_ud = fn ? userdata : 0;
}

const AssertContext& GetLastAssertContext() {
    return s_lastCtx;
}

u64 GetAssertFireCount() {
    return s_fireCount;
}

void StampFrameContext(u32 snapshotVersion, Guid focusEntity, u64 frameNumber) {
    s_currentSnapshotVersion = snapshotVersion;
    s_currentFocus           = focusEntity;
    s_currentFrame           = frameNumber;
}

// ── Default handler ───────────────────────────────────────────────────

bool DefaultAssertHandler(const AssertContext& ctx, void* /*userdata*/) {
    // Log the fire at error level. The Logging subsystem itself uses
    // VESPUCCI_CHECK, so we MUST avoid recursion here — Logging's own
    // checks degrade gracefully when the assert handler is reentrant.
    if (!s_inHandler) {
        s_inHandler = true;
        if (ctx.message && ctx.message[0]) {
            Logging::Error("[ASSERT] %s:%d (%s) %s :: %s [snap=%u focus=0x%08X frame=%llu]",
                ctx.file, (int)ctx.line, ctx.func ? ctx.func : "?",
                ctx.expr ? ctx.expr : "?",
                ctx.message,
                (unsigned)ctx.snapshotVersion,
                (unsigned)ctx.focusEntity.raw,
                (unsigned long long)ctx.frameNumber);
        } else {
            Logging::Error("[ASSERT] %s:%d (%s) %s [snap=%u focus=0x%08X frame=%llu]",
                ctx.file, (int)ctx.line, ctx.func ? ctx.func : "?",
                ctx.expr ? ctx.expr : "?",
                (unsigned)ctx.snapshotVersion,
                (unsigned)ctx.focusEntity.raw,
                (unsigned long long)ctx.frameNumber);
        }
        s_inHandler = false;
    }

    // Break in debug, continue in release. We use the presence of the
    // _DEBUG macro (MSVC convention) to decide; tests that want a
    // different policy install their own handler.
#if defined(_DEBUG)
    return true;
#else
    return false;
#endif
}

// ── Internal fire entry points ────────────────────────────────────────

bool FireAssert(const char* file, i32 line, const char* func,
                const char* expr, const char* fmt, ...) {
    AssertContext ctx;
    ctx.file            = file;
    ctx.line            = line;
    ctx.func            = func;
    ctx.expr            = expr;
    ctx.snapshotVersion = s_currentSnapshotVersion;
    ctx.focusEntity     = s_currentFocus;
    ctx.frameNumber     = s_currentFrame;

    // Format the user message into a static thread-unsafe buffer. The
    // formatted string lives only for the duration of this call; the
    // ring stores the pointer and the handler may copy it onward via
    // Logging. We accept the truncation/aliasing trade-off because
    // assert-fires are rare and the alternative (per-fire malloc) is
    // worse in this engine.
    static char s_fmtBuf[kFmtBufSize];
    if (fmt && fmt[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s_fmtBuf, kFmtBufSize, fmt, ap);
        va_end(ap);
        s_fmtBuf[kFmtBufSize - 1] = 0;
        ctx.message = s_fmtBuf;
    } else {
        ctx.message = "";
    }

    s_lastCtx = ctx;
    PushRing(ctx);
    s_fireCount++;

    if (s_handler) {
        return s_handler(ctx, s_handler_ud);
    }
    return DefaultAssertHandler(ctx, 0);
}

bool FireCheck(const char* file, i32 line, const char* func, const char* expr) {
    AssertContext ctx;
    ctx.file            = file;
    ctx.line            = line;
    ctx.func            = func;
    ctx.expr            = expr;
    ctx.message         = "";
    ctx.snapshotVersion = s_currentSnapshotVersion;
    ctx.focusEntity     = s_currentFocus;
    ctx.frameNumber     = s_currentFrame;

    s_lastCtx = ctx;
    PushRing(ctx);
    s_fireCount++;

    if (s_handler) {
        return s_handler(ctx, s_handler_ud);
    }
    return DefaultAssertHandler(ctx, 0);
}

// ── Ring buffer accessors (for debug overlay) ─────────────────────────
// Not in the public header because the overlay reaches in via a
// well-known symbol. Keeps the public surface cleaner.

const AssertContext* AssertRingBuffer() { return s_ring; }
i32                  AssertRingHead()   { return s_ringHead; }
i32                  AssertRingSize()   { return s_ringSize; }
i32                  AssertRingCap()    { return kRingCap; }

} // namespace Core
} // namespace Vespucci
