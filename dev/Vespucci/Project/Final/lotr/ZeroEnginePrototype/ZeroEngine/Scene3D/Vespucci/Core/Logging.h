// Logging.h
// =============================================================================
// VESPUCCI logging — leveled, sinkable, format-string-checked
// =============================================================================
// Written by: Eriumsss
//
// Five levels: Trace / Debug / Info / Warn / Error. Sinks accept the
// formatted line and decide where it goes. We ship two sinks: a file
// sink (rolling at 8 MB) and a ring-buffer sink (last 1024 lines, read
// by the debug overlay's "recent log" tab). Tests can install a count-
// only sink to assert "no Error fires this run".
//
// Format string is checked at compile time on MSVC via the inherited
// _Printf_format_string_ annotation. Mismatched format/arg pairs become
// build warnings (W4) rather than runtime corruption.
//
// Performance: each Vespucci::Core::Logging::* call performs ONE
// vsnprintf (~1-2 us) plus N sink dispatches. The hot-path expectation
// is a few Info lines per second from steady-state and Warn/Error only
// on failure. Do NOT call Logging::Trace from a per-vertex loop — use
// HeatTimer perf-scopes instead, those are nanosecond-cost.
// =============================================================================

#ifndef VESPUCCI_CORE_LOGGING_H_
#define VESPUCCI_CORE_LOGGING_H_

#include "VespucciTypes.h"

namespace Vespucci {
namespace Core {
namespace Logging {

enum Level {
    LEVEL_Trace = 0,
    LEVEL_Debug = 1,
    LEVEL_Info  = 2,
    LEVEL_Warn  = 3,
    LEVEL_Error = 4,
    LEVEL_Off   = 5
};

// Sink callback. Receives the formatted line (without trailing newline)
// and the level it was emitted at. Implementations must be reentrant-
// safe because asserts can fire mid-log.
typedef void (*Sink)(Level lvl, const char* line, void* userdata);

// Public API. Use the convenience macros below in caller code; these
// raw functions are exposed for tests and tooling.
void Log(Level lvl, const char* fmt, ...);
void LogV(Level lvl, const char* fmt, va_list ap);

// Convenience wrappers.
void Trace(const char* fmt, ...);
void Debug(const char* fmt, ...);
void Info (const char* fmt, ...);
void Warn (const char* fmt, ...);
void Error(const char* fmt, ...);

// Configuration.
void SetMinLevel(Level lvl);
Level GetMinLevel();

void AddSink(Sink fn, void* userdata);
void RemoveSink(Sink fn, void* userdata);
void ClearSinks();

// Built-in sinks. Caller passes the path and the rotate-at-bytes
// budget. NULL path disables.
void InstallFileSink(const char* path, u32 rotateBytes);
void UninstallFileSink();

// Ring sink — last 1024 lines, FIFO. The debug overlay reads via the
// accessor below.
void InstallRingSink();
void UninstallRingSink();

// Read the ring buffer. ringOut must hold at least kRingCap pointers;
// returns the number of valid entries. Pointers are stable until the
// ring rolls (write-N times after this call).
static const i32 kLogRingCap = 1024;
i32 SnapshotRing(const char** ringOut, Level* lvlOut, i32 cap);

// Counters for tests / metrics dashboards.
struct Counters {
    u64 trace;
    u64 debug;
    u64 info;
    u64 warn;
    u64 error;
};
Counters GetCounters();
void     ResetCounters();

} // namespace Logging
} // namespace Core
} // namespace Vespucci

// ── Compact convenience macros ────────────────────────────────────────
// These wrap the namespaced functions. Callers prefer these because
// the level is explicit at the call site without the "Vespucci::Core::"
// noise. The level filter check is done inside Log() so we don't gain
// much by guarding the macros.

#define VESPUCCI_LOG_TRACE(...) ::Vespucci::Core::Logging::Trace(__VA_ARGS__)
#define VESPUCCI_LOG_DEBUG(...) ::Vespucci::Core::Logging::Debug(__VA_ARGS__)
#define VESPUCCI_LOG_INFO(...)  ::Vespucci::Core::Logging::Info(__VA_ARGS__)
#define VESPUCCI_LOG_WARN(...)  ::Vespucci::Core::Logging::Warn(__VA_ARGS__)
#define VESPUCCI_LOG_ERROR(...) ::Vespucci::Core::Logging::Error(__VA_ARGS__)

#endif // VESPUCCI_CORE_LOGGING_H_
