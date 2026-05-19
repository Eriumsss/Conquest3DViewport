// Sinks.cpp
// =============================================================================
// TELEMETRY SINK - APPEND-ONLY JSON LINES TO vespucci_telemetry.jsonl
// =============================================================================
// Written by: Eriumsss
//
// Every panel-action event the user performs (Apply, Undo, Ignore,
// Preview, Why-tooltip-shown, etc.) gets one JSON line written to
// the local telemetry file. Doc-locked: this substitutes for the
// AAA acceptance-rate dashboard since Vespucci is single-user. The
// user can ship the file with a bug report and we get a precise
// reproduction of what the editor was doing.
// =============================================================================

#include "../Core/FileIO.h"
#include "../Core/JsonLite.h"
#include "../Core/Logging.h"
#include "../Core/PathUtils.h"

#include <cstdio>
#include <ctime>
#include <string>

namespace Vespucci {
namespace Telemetry {

namespace {
    static std::string s_currentSinkPath;
}

void OpenTelemetrySink(const char* path) {
    s_currentSinkPath = path ? path : "";
    Core::Logging::Info("Telemetry: sink open at %s", s_currentSinkPath.c_str());
}

void CloseTelemetrySink() {
    s_currentSinkPath.clear();
}

void EmitTelemetryEvent(const char* eventName, const char* payloadJson) {
    if (s_currentSinkPath.empty() || !eventName) return;
    char line[1024];
    std::snprintf(line, sizeof(line),
        "{\"t\":%llu,\"event\":\"%s\",\"data\":%s}\n",
        (unsigned long long)std::time(0),
        eventName,
        payloadJson ? payloadJson : "{}");
    Core::AppendBlob(s_currentSinkPath.c_str(), line, (i64)std::strlen(line));
}

void EmitSimpleEvent(const char* eventName) {
    EmitTelemetryEvent(eventName, "{}");
}

} // namespace Telemetry
} // namespace Vespucci
