// ============================================================================
// Logging - Core logging functionality
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include "../include/debugger.h"
#include "../include/console.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace ConquestDebugger {

static LogLevel g_minLogLevel = LogLevel::Info;
static FILE* g_logFile = nullptr;

static const char* LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO ";
        case LogLevel::Warning:  return "WARN ";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRIT ";
        default:                 return "?????";
    }
}

DEBUGGER_API void Log(LogLevel level, const char* format, ...) {
    if (level < g_minLogLevel) return;

    // Get timestamp
    time_t now = time(nullptr);
    struct tm timeInfo;
    localtime_s(&timeInfo, &now);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeInfo);

    // Format message
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Build full log line
    char fullLine[2200];
    snprintf(fullLine, sizeof(fullLine), "[%s] [%s] %s",
        timeBuf, LogLevelToString(level), message);

    // Output to console with color
    Console::Instance().WriteLog(level, fullLine);
}

DEBUGGER_API void SetLogLevel(LogLevel minLevel) {
    g_minLogLevel = minLevel;
}

DEBUGGER_API void SetLogFile(const char* path) {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    if (path) {
        g_logFile = fopen(path, "a");
    }
}

} // namespace ConquestDebugger

