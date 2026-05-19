// FrameDiff.cpp
// =============================================================================
// COMPARE TWO ANCHORED EVENTS - SHOW WHAT THE DESIGNER DID BETWEEN THEM
// =============================================================================
// Written by: Eriumsss
//
// Designer right-clicks event N as "anchor A" and event M as "anchor B"
// in the timeline tape. This tab summarizes what happened between them
// in plain English:
//
//   "Between event 412 and event 489 (duration 18 seconds):
//     - 47 snapshot version bumps
//     - 12 ranker queries against 4 distinct entities
//     - 3 wires applied (entities 0x4A2B → 0x9F1C, ...)
//     - 1 undo
//     - 0 previews / 0 level loads"
//
// Plus a per-kind raw count table for the cockpit users.
// =============================================================================

#include "Panel.h"

#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace Vespucci {
namespace UI {
namespace ReplayViewer {

// Provide an Event detail view too — single-event drill-down.
void RenderEventDetailTab(PanelState& s) {
    if (s.events.empty()) {
        ImGui::TextDisabled("(no events loaded)");
        return;
    }
    if (s.cursor < 0 || s.cursor >= (i32)s.events.size()) {
        ImGui::TextDisabled("(scrub the timeline to position the cursor)");
        return;
    }
    const QA::ReplayEvent& e = s.events[(size_t)s.cursor];
    ImGui::Text("Event index: %d / %d", s.cursor, (i32)s.events.size() - 1);
    ImGui::Separator();

    const char* kindStr = "?";
    switch (e.kind) {
        case QA::REPLAY_SnapshotChanged: kindStr = "Snapshot version bump"; break;
        case QA::REPLAY_RankerQuery:     kindStr = "Ranker query fired";     break;
        case QA::REPLAY_ApplyFired:      kindStr = "Wire applied";            break;
        case QA::REPLAY_UndoFired:       kindStr = "Undo fired";              break;
        case QA::REPLAY_PreviewStaged:   kindStr = "Preview ghost staged";    break;
        case QA::REPLAY_LevelLoaded:     kindStr = "Level loaded";            break;
    }
    ImGui::Text("Kind: %s", kindStr);
    ImGui::Text("Wallclock (unix sec): %llu", (unsigned long long)e.unixSec);
    ImGui::Text("Snapshot version at event: %u", (unsigned)e.snapshotVersion);
    ImGui::Text("GUID A: 0x%08X", (unsigned)e.guidA.raw);
    ImGui::Text("GUID B: 0x%08X", (unsigned)e.guidB.raw);

    // Payload as printable string + hex fallback.
    ImGui::Separator();
    ImGui::Text("Payload (64 bytes):");
    char preview[80];
    i32 n = 0;
    for (i32 i = 0; i < 63 && e.payload[i]; ++i) {
        char c = e.payload[i];
        preview[n++] = (c >= 32 && c < 127) ? c : '.';
    }
    preview[n] = 0;
    ImGui::TextDisabled("ascii:  %s", preview);

    char hexline[200];
    i32 hpos = 0;
    for (i32 i = 0; i < 32 && hpos + 4 < (i32)sizeof(hexline); ++i) {
        std::snprintf(hexline + hpos, sizeof(hexline) - (size_t)hpos,
            "%02X ", (unsigned)(unsigned char)e.payload[i]);
        hpos += 3;
    }
    hexline[hpos] = 0;
    ImGui::TextDisabled("hex:    %s", hexline);
}

void RenderFrameDiffTab(PanelState& s) {
    if (s.events.empty()) {
        ImGui::TextDisabled("(no events loaded)");
        return;
    }
    bool aValid = (s.diffAnchorA >= 0 && s.diffAnchorA < (i32)s.events.size());
    bool bValid = (s.diffAnchorB >= 0 && s.diffAnchorB < (i32)s.events.size());

    if (!aValid || !bValid) {
        ImGui::TextDisabled("Right-click events in the Timeline tape to set anchor A and anchor B. "
                              "The diff between them shows up here.");
        return;
    }

    // Make sure A < B for sane reading. Swap visually if reversed.
    i32 lo = s.diffAnchorA, hi = s.diffAnchorB;
    if (lo > hi) { i32 t = lo; lo = hi; hi = t; }
    const QA::ReplayEvent& evtLo = s.events[(size_t)lo];
    const QA::ReplayEvent& evtHi = s.events[(size_t)hi];

    ImGui::Text("Anchor A: event %d   (snap version %u)",
        lo, (unsigned)evtLo.snapshotVersion);
    ImGui::Text("Anchor B: event %d   (snap version %u)",
        hi, (unsigned)evtHi.snapshotVersion);
    ImGui::Separator();

    u64 durSec = (evtHi.unixSec >= evtLo.unixSec)
                 ? (evtHi.unixSec - evtLo.unixSec) : 0;
    i32 nBetween = hi - lo;
    ImGui::Text("Span: %d events over %llu seconds", nBetween,
        (unsigned long long)durSec);

    // Per-kind tally.
    i32 kindCount[8] = {0};
    std::unordered_set<u32> distinctEntities;
    for (i32 i = lo; i <= hi; ++i) {
        const QA::ReplayEvent& e = s.events[(size_t)i];
        if ((i32)e.kind >= 0 && (i32)e.kind < 8) kindCount[(i32)e.kind]++;
        if (e.guidA.raw != 0) distinctEntities.insert(e.guidA.raw);
        if (e.guidB.raw != 0) distinctEntities.insert(e.guidB.raw);
    }
    i32 versionDelta = (i32)evtHi.snapshotVersion - (i32)evtLo.snapshotVersion;

    ImGui::Separator();
    ImGui::TextWrapped("Plain-English summary:");
    ImGui::BulletText("Snapshot stepped by %+d versions (%d bumps observed)",
        versionDelta, kindCount[QA::REPLAY_SnapshotChanged]);
    ImGui::BulletText("%d ranker queries fired",     kindCount[QA::REPLAY_RankerQuery]);
    ImGui::BulletText("%d wires applied",            kindCount[QA::REPLAY_ApplyFired]);
    ImGui::BulletText("%d undos fired",              kindCount[QA::REPLAY_UndoFired]);
    ImGui::BulletText("%d previews staged",          kindCount[QA::REPLAY_PreviewStaged]);
    ImGui::BulletText("%d level loads",              kindCount[QA::REPLAY_LevelLoaded]);
    ImGui::BulletText("%d distinct entities touched", (i32)distinctEntities.size());

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Raw counts table")) {
        ImGui::Columns(2, "##rv_raw_cols", true);
        ImGui::Text("Kind"); ImGui::NextColumn();
        ImGui::Text("Count"); ImGui::NextColumn();
        ImGui::Separator();
        const char* names[6] = {
            "SnapshotChanged", "RankerQuery", "ApplyFired",
            "UndoFired", "PreviewStaged", "LevelLoaded"
        };
        for (i32 i = 0; i < 6; ++i) {
            ImGui::TextUnformatted(names[i]); ImGui::NextColumn();
            ImGui::Text("%d", kindCount[i]); ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }
}

} // namespace ReplayViewer
} // namespace UI
} // namespace Vespucci
