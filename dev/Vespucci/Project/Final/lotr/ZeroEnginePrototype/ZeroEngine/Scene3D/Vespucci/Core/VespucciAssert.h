// VespucciAssert.h
// =============================================================================
// VESPUCCI assert / check / unreachable macros and the diagnostic dump path
// =============================================================================
// Written by: Eriumsss
//
// We do not use <cassert>'s assert() for two reasons:
//  1. It compiles to nothing in NDEBUG release builds. Vespucci ships with
//     asserts ALWAYS LIVE because the cost of a wrong suggestion getting
//     auto-applied silently is much higher than the cost of one branch
//     per VESPUCCI_CHECK call.
//  2. assert() prints to stderr and dies. We want a real message dumped
//     to the editor's log file with snapshot version, focus entity, and
//     a small backtrace of the last 32 perf-scope frames.
//
// Three macros, three contracts:
//
//   VESPUCCI_ASSERT(expr, fmt, ...)
//     Same as assert() but always live. Triggers AssertHandler with the
//     formatted message. Default handler logs + breakpoint-traps in
//     debug, logs + continues in release.
//
//   VESPUCCI_CHECK(expr)
//     Hot-path-cheap variant. No format string. The branch is still
//     present in release builds but compiles to a single conditional
//     jump on the success path. Use this in tight inner loops where
//     the formatter call would dominate.
//
//   VESPUCCI_UNREACHABLE(reason)
//     Marker for code paths the type system can't prove dead. Same
//     as ASSERT(false, reason) but reads better at the call site.
//
// All three funnel through Vespucci::Core::AssertHandler. Tests
// override the handler to count fires; the editor's debug overlay
// shows the last 16 fires in a ring buffer.
// =============================================================================

#ifndef VESPUCCI_CORE_VESPUCCIASSERT_H_
#define VESPUCCI_CORE_VESPUCCIASSERT_H_

#include "VespucciTypes.h"

namespace Vespucci {
namespace Core {

// AssertContext travels with every fire. The default handler stamps
// the args struct's snapshotVersion + the focus entity onto each fire
// so the log entry is self-contained. UI overlays read this struct
// via GetLastAssertContext() to render the assert ring.
struct AssertContext {
    const char* file;
    i32         line;
    const char* func;
    const char* expr;       // stringified expression that failed
    const char* message;    // formatted user message (may be empty)
    u32         snapshotVersion;
    Guid        focusEntity;
    u64         frameNumber;
};

// AssertHandler signature. Return true to break-trap (debug builds),
// false to continue (release builds, or tests that want to keep going).
typedef bool (*AssertHandler)(const AssertContext& ctx, void* userdata);

// Install a custom handler. Pass NULL to restore default. The default
// handler logs to Core::Logging at level Error and triggers a debugger
// break in debug builds.
void SetAssertHandler(AssertHandler fn, void* userdata);

// The default handler. Exposed so test fixtures can chain into it after
// counting their own fires.
bool DefaultAssertHandler(const AssertContext& ctx, void* userdata);

// Read the most recent assert that fired. UI debug overlay polls this
// each frame to update its assert ring.
const AssertContext& GetLastAssertContext();

// Number of asserts that have fired since boot. Useful for tests.
u64 GetAssertFireCount();

// Internal entry called by the macros. Do not call directly.
bool FireAssert(const char* file, i32 line, const char* func,
                const char* expr, const char* fmt, ...);

// Internal: variant without format string for the hot-path CHECK macro.
bool FireCheck(const char* file, i32 line, const char* func,
               const char* expr);

// Internal: per-frame snapshot/focus/frame stamping. Called from
// Vespucci::Update so AssertContext can read consistent values when
// firing inside a request handler.
void StampFrameContext(u32 snapshotVersion, Guid focusEntity, u64 frameNumber);

} // namespace Core
} // namespace Vespucci

// ── Macros ────────────────────────────────────────────────────────────
// __FUNCSIG__ is MSVC-only and gives the full signature including the
// namespace. We prefer it over __func__ for the diagnostic message.
// Branches are written so the success path is a single conditional jump.

#if defined(_MSC_VER)
    #define VESPUCCI_FUNC __FUNCSIG__
    #define VESPUCCI_DEBUG_BREAK() __debugbreak()
#else
    #define VESPUCCI_FUNC __PRETTY_FUNCTION__
    #define VESPUCCI_DEBUG_BREAK() ((void)0)
#endif

// VESPUCCI_ASSERT(expr, fmt, ...) — formatted, always live.
#define VESPUCCI_ASSERT(expr, ...) \
    do { \
        if (!(expr)) { \
            if (::Vespucci::Core::FireAssert(__FILE__, __LINE__, VESPUCCI_FUNC, \
                                             #expr, __VA_ARGS__)) { \
                VESPUCCI_DEBUG_BREAK(); \
            } \
        } \
    } while (0)

// VESPUCCI_CHECK(expr) — bare, hot-path-cheap.
#define VESPUCCI_CHECK(expr) \
    do { \
        if (!(expr)) { \
            if (::Vespucci::Core::FireCheck(__FILE__, __LINE__, VESPUCCI_FUNC, #expr)) { \
                VESPUCCI_DEBUG_BREAK(); \
            } \
        } \
    } while (0)

// VESPUCCI_UNREACHABLE(reason) — marker macro for dead-code paths.
// Compiles to an unconditional fire so static analysis cannot prove
// the surrounding switch/if exhaustive (which we want — coverage tools
// will surface unreachable branches if we ever miss one).
#define VESPUCCI_UNREACHABLE(reason) \
    do { \
        if (::Vespucci::Core::FireAssert(__FILE__, __LINE__, VESPUCCI_FUNC, \
                                         "unreachable", "unreachable: %s", reason)) { \
            VESPUCCI_DEBUG_BREAK(); \
        } \
    } while (0)

// VESPUCCI_VERIFY(expr) — like CHECK but the expression is evaluated
// EVEN IN A "STRIPPED" BUILD where check fires are ignored. Use this
// when the expression has side effects you must not skip (e.g. a
// FileIO read whose return value you also assert on).
#define VESPUCCI_VERIFY(expr) \
    do { \
        bool _v_result_ = (expr); \
        if (!_v_result_) { \
            if (::Vespucci::Core::FireCheck(__FILE__, __LINE__, VESPUCCI_FUNC, #expr)) { \
                VESPUCCI_DEBUG_BREAK(); \
            } \
        } \
    } while (0)

#endif // VESPUCCI_CORE_VESPUCCIASSERT_H_
