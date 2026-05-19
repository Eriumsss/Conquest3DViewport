// ReplayRecorder.cpp
// =============================================================================
// Append-only binary log of replay events. One u32 magic + one u32
// version + N fixed-size ReplayEvent records.
// =============================================================================
// Written by: Eriumsss

#include "ReplayRecorder.h"

#include "../Core/FileIO.h"
#include "../Core/Logging.h"

#include <string>

namespace Vespucci {
namespace QA {

namespace {
    static const u32 kReplayMagic   = 0x52455052u; // 'REPR'
    static const u32 kReplayVersion = 1;
    static std::string s_path;
    static bool        s_recording = false;
    static i32         s_count = 0;
} // namespace

void StartRecordingToFile(const char* path) {
    if (!path || !*path) return;
    s_path = path;
    s_recording = true;
    s_count = 0;
    // Write header.
    u32 hdr[2] = { kReplayMagic, kReplayVersion };
    Core::WriteAtomic(path, hdr, sizeof(hdr));
    Core::Logging::Info("ReplayRecorder: recording to %s", path);
}

void StopRecording() {
    s_recording = false;
    Core::Logging::Info("ReplayRecorder: stopped (%d events written)", s_count);
}

bool IsRecording() { return s_recording; }

void AppendReplayEvent(const ReplayEvent& evt) {
    if (!s_recording || s_path.empty()) return;
    Core::AppendBlob(s_path.c_str(), &evt, (i64)sizeof(evt));
    s_count++;
}

i32 GetReplayEventsThisSession() { return s_count; }

} // namespace QA
} // namespace Vespucci
