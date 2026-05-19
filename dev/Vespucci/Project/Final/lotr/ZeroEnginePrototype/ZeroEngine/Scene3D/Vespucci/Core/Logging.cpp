// Logging.cpp
// =============================================================================
// VESPUCCI logging — file sink, ring sink, counters, the works
// =============================================================================
// Written by: Eriumsss
//
// Existing engine had logging spread across six different printf-spam
// patterns and zero filtering. Wanted to read what Vespucci was doing
// without drowning in mocap pipeline puke and Havok shape-info chatter,
// so this is a self-contained logger that the rest of the codebase
// outside Vespucci/ does not need to know exists.
//
// Single-threaded by design — ImGui side is single-threaded, snapshot
// rebuild is single-threaded, the corpus tools are CLI-only. If we
// ever go multi-threaded the sink list becomes a flat-spinlock guarded
// vector; for now it's a fixed-cap array because malloc-during-log is
// the universe's worst footgun.
//
// File sink rotates at ~8 MB to keep editor sessions from filling the
// hard drive when someone leaves Vespucci running over a long weekend
// of layout work. Ring sink is fixed at 1024 lines because the debug
// overlay only ever reads the tail and 1024 is plenty to backtrack
// from a Bad Day.
// =============================================================================

#include "Logging.h"
#include "VespucciAssert.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace Vespucci {
namespace Core {
namespace Logging {

namespace {
    // Hard caps. Going beyond these means somebody is using logging
    // wrong and they need to fix that, not us. Add another sink? Use
    // tags. Want longer lines? Don't — wrap manually or split events.
    static const i32 kMaxSinks       = 16;
    static const i32 kFmtBufSize     = 2048;
    static const i32 kRingLineLen    = 240;     // ring stores fixed-len lines
    static const u32 kFileRotateBudget = 8u * 1024u * 1024u; // 8 MB

    struct SinkEntry {
        Sink   fn;
        void*  ud;
    };

    // ── File sink state ───────────────────────────────────────────
    FILE*  s_fileFp        = 0;
    char   s_filePath[260] = {0};
    u32    s_fileBytesOut  = 0;
    u32    s_fileRotateAt  = kFileRotateBudget;

    // ── Ring sink state ───────────────────────────────────────────
    bool   s_ringInstalled = false;
    char   s_ringLines[kLogRingCap][kRingLineLen];
    Level  s_ringLevels[kLogRingCap];
    i32    s_ringHead      = 0;
    i32    s_ringSize      = 0;

    // ── Sink list ─────────────────────────────────────────────────
    SinkEntry s_sinks[kMaxSinks];
    i32       s_sinkCount  = 0;

    // ── Filter ────────────────────────────────────────────────────
    Level s_minLevel = LEVEL_Info;

    // ── Counters ──────────────────────────────────────────────────
    Counters s_counters = {0,0,0,0,0};

    // ── Reentrancy guard ──────────────────────────────────────────
    // If a sink calls back into Logging (which it should not, but
    // we are not the police), we silently drop the inner call.
    // Better than infinite recursion that nukes the stack.
    bool s_inDispatch = false;

    const char* LevelTag(Level l) {
        switch (l) {
            case LEVEL_Trace: return "TRC";
            case LEVEL_Debug: return "DBG";
            case LEVEL_Info:  return "INF";
            case LEVEL_Warn:  return "WRN";
            case LEVEL_Error: return "ERR";
            default:          return "???";
        }
    }

    // Built-in file sink — opens lazily, rotates by rename, handles
    // disk-full gracefully (silently drops; we are not going to crash
    // the editor because /tmp is full).
    void FileSinkFn(Level lvl, const char* line, void* /*ud*/) {
        if (!s_fileFp) return;
        char stamp[32];
        time_t t = time(0);
        std::tm* tm = std::localtime(&t);
        if (tm) {
            std::strftime(stamp, sizeof(stamp), "%H:%M:%S", tm);
        } else {
            std::strcpy(stamp, "??:??:??");
        }
        int n = std::fprintf(s_fileFp, "[%s] %s %s\n", stamp, LevelTag(lvl), line);
        if (n > 0) {
            s_fileBytesOut += (u32)n;
            std::fflush(s_fileFp);
        }
        if (s_fileBytesOut >= s_fileRotateAt) {
            // Roll: close, rename .log -> .log.old (clobbering any
            // prior .old), reopen. We do NOT keep more than one
            // backup because the disk gods do not love us either.
            std::fclose(s_fileFp);
            char old[280];
            std::snprintf(old, sizeof(old), "%s.old", s_filePath);
#if defined(_WIN32)
            DeleteFileA(old);
            MoveFileA(s_filePath, old);
#else
            std::remove(old);
            std::rename(s_filePath, old);
#endif
            s_fileFp = std::fopen(s_filePath, "ab");
            s_fileBytesOut = 0;
        }
    }

    // Built-in ring sink — fixed-size circular buffer of fixed-len
    // line slots. We truncate over-long lines because the alternative
    // is a malloc per log call and that is a non-starter for an
    // editor that wants to log thousands of lines per second during
    // corpus build.
    void RingSinkFn(Level lvl, const char* line, void* /*ud*/) {
        if (!s_ringInstalled) return;
        char* dst = s_ringLines[s_ringHead];
        std::strncpy(dst, line, kRingLineLen - 1);
        dst[kRingLineLen - 1] = 0;
        s_ringLevels[s_ringHead] = lvl;
        s_ringHead = (s_ringHead + 1) % kLogRingCap;
        if (s_ringSize < kLogRingCap) s_ringSize++;
    }

    void DispatchSinks(Level lvl, const char* line) {
        if (s_inDispatch) return; // re-entry: drop
        s_inDispatch = true;
        for (i32 i = 0; i < s_sinkCount; ++i) {
            if (s_sinks[i].fn) s_sinks[i].fn(lvl, line, s_sinks[i].ud);
        }
        s_inDispatch = false;
    }
} // namespace

// ── Public API ────────────────────────────────────────────────────────

void SetMinLevel(Level lvl) { s_minLevel = lvl; }
Level GetMinLevel()         { return s_minLevel; }

void AddSink(Sink fn, void* userdata) {
    if (s_sinkCount >= kMaxSinks) return; // hit the cap, sucks
    s_sinks[s_sinkCount].fn = fn;
    s_sinks[s_sinkCount].ud = userdata;
    s_sinkCount++;
}

void RemoveSink(Sink fn, void* userdata) {
    for (i32 i = 0; i < s_sinkCount; ++i) {
        if (s_sinks[i].fn == fn && s_sinks[i].ud == userdata) {
            // Shift left to keep order. We do not have many sinks;
            // the memmove cost is irrelevant.
            for (i32 j = i + 1; j < s_sinkCount; ++j) s_sinks[j-1] = s_sinks[j];
            s_sinkCount--;
            return;
        }
    }
}

void ClearSinks() { s_sinkCount = 0; }

void InstallFileSink(const char* path, u32 rotateBytes) {
    if (s_fileFp) UninstallFileSink();
    if (!path || !path[0]) return;
    std::strncpy(s_filePath, path, sizeof(s_filePath) - 1);
    s_filePath[sizeof(s_filePath) - 1] = 0;
    s_fileRotateAt = rotateBytes ? rotateBytes : kFileRotateBudget;
    s_fileFp = std::fopen(path, "ab");
    if (!s_fileFp) {
        // Cannot log the failure (we ARE the logger), so just bail
        // silently. The user will notice when the file is missing.
        return;
    }
    // Determine starting size for rotation accounting.
    std::fseek(s_fileFp, 0, SEEK_END);
    long sz = std::ftell(s_fileFp);
    s_fileBytesOut = (sz > 0) ? (u32)sz : 0;
    AddSink(&FileSinkFn, 0);
}

void UninstallFileSink() {
    if (s_fileFp) {
        std::fclose(s_fileFp);
        s_fileFp = 0;
    }
    RemoveSink(&FileSinkFn, 0);
}

void InstallRingSink() {
    if (s_ringInstalled) return;
    s_ringInstalled = true;
    s_ringHead = 0;
    s_ringSize = 0;
    AddSink(&RingSinkFn, 0);
}

void UninstallRingSink() {
    s_ringInstalled = false;
    RemoveSink(&RingSinkFn, 0);
}

i32 SnapshotRing(const char** ringOut, Level* lvlOut, i32 cap) {
    if (!ringOut || cap <= 0) return 0;
    i32 n = (s_ringSize < cap) ? s_ringSize : cap;
    // Walk from oldest to newest. Oldest is at (head - size) mod cap.
    i32 idx = (s_ringHead - s_ringSize + kLogRingCap) % kLogRingCap;
    for (i32 i = 0; i < n; ++i) {
        ringOut[i] = s_ringLines[idx];
        if (lvlOut) lvlOut[i] = s_ringLevels[idx];
        idx = (idx + 1) % kLogRingCap;
    }
    return n;
}

Counters GetCounters() { return s_counters; }
void     ResetCounters() {
    s_counters.trace = 0;
    s_counters.debug = 0;
    s_counters.info  = 0;
    s_counters.warn  = 0;
    s_counters.error = 0;
}

// ── Core dispatch ─────────────────────────────────────────────────────

void LogV(Level lvl, const char* fmt, va_list ap) {
    if (lvl < s_minLevel) return;

    // Bump counter regardless of sink presence — counters reflect
    // actual call activity, not sink visibility.
    switch (lvl) {
        case LEVEL_Trace: s_counters.trace++; break;
        case LEVEL_Debug: s_counters.debug++; break;
        case LEVEL_Info:  s_counters.info++;  break;
        case LEVEL_Warn:  s_counters.warn++;  break;
        case LEVEL_Error: s_counters.error++; break;
        default: break;
    }

    if (s_sinkCount == 0) return;

    char buf[kFmtBufSize];
    std::vsnprintf(buf, kFmtBufSize, fmt ? fmt : "(null fmt)", ap);
    buf[kFmtBufSize - 1] = 0;

    DispatchSinks(lvl, buf);
}

void Log(Level lvl, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    LogV(lvl, fmt, ap);
    va_end(ap);
}

void Trace(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(LEVEL_Trace, fmt, ap); va_end(ap); }
void Debug(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(LEVEL_Debug, fmt, ap); va_end(ap); }
void Info (const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(LEVEL_Info,  fmt, ap); va_end(ap); }
void Warn (const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(LEVEL_Warn,  fmt, ap); va_end(ap); }
void Error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(LEVEL_Error, fmt, ap); va_end(ap); }

} // namespace Logging
} // namespace Core
} // namespace Vespucci
