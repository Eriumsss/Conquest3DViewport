// Timeline.cpp
// =============================================================================
// HORIZONTAL TIMELINE SCRUBBER FOR THE REPLAY VIEWER
// =============================================================================
// Written by: Eriumsss
//
// Renders every loaded event as a single tick on a wallclock-mapped
// horizontal axis. Color codes by event kind (snap = gray, ranker
// query = cyan, apply = green, undo = orange, preview = blue, level
// load = purple). Click anywhere on the strip to jump the cursor to
// the nearest event. Click + drag to scrub.
//
// Below the scrubber is a "tape" that lists the visible events as a
// scroll list — for designers who want the textual breakdown next to
// the visual timeline.
//
// Filtered out events render as faint background ticks rather than
// vanishing — that way the user sees "I'm filtering 80% of this away"
// at a glance and can broaden the filter before getting confused.
// =============================================================================

#include "Panel.h"

#include "../../Core/Logging.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace ReplayViewer {

namespace {
    ImU32 ColorForKind(QA::ReplayEventKind k) {
        switch (k) {
            case QA::REPLAY_SnapshotChanged: return IM_COL32(140, 145, 160, 255);
            case QA::REPLAY_RankerQuery:     return IM_COL32( 80, 200, 220, 255);
            case QA::REPLAY_ApplyFired:      return IM_COL32( 90, 220, 110, 255);
            case QA::REPLAY_UndoFired:       return IM_COL32(240, 160,  60, 255);
            case QA::REPLAY_PreviewStaged:   return IM_COL32(120, 160, 240, 255);
            case QA::REPLAY_LevelLoaded:     return IM_COL32(200, 130, 240, 255);
        }
        return IM_COL32(180, 180, 180, 255);
    }

    bool KindIsVisible(const PanelState& s, QA::ReplayEventKind k) {
        switch (k) {
            case QA::REPLAY_SnapshotChanged: return s.showSnapshotEvents;
            case QA::REPLAY_RankerQuery:     return s.showQueryEvents;
            case QA::REPLAY_ApplyFired:      return s.showApplyEvents;
            case QA::REPLAY_UndoFired:       return s.showUndoEvents;
            case QA::REPLAY_PreviewStaged:   return s.showPreviewEvents;
            case QA::REPLAY_LevelLoaded:     return s.showLevelLoadEvents;
        }
        return true;
    }

    bool GuidFilterPasses(const PanelState& s, const QA::ReplayEvent& e) {
        if (!s.guidFilter[0]) return true;
        // Match against guidA OR guidB hex prefix.
        char a[16], b[16];
        std::snprintf(a, sizeof(a), "%08x", (unsigned)e.guidA.raw);
        std::snprintf(b, sizeof(b), "%08x", (unsigned)e.guidB.raw);
        usize fn = std::strlen(s.guidFilter);
        // Case-insensitive substring scan against both.
        const char* fields[2] = { a, b };
        for (i32 fi = 0; fi < 2; ++fi) {
            const char* f = fields[fi];
            for (usize i = 0; i + fn <= 8; ++i) {
                bool ok = true;
                for (usize k = 0; k < fn; ++k) {
                    char ca = f[i + k];
                    char cb = s.guidFilter[k];
                    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                    if (ca != cb) { ok = false; break; }
                }
                if (ok) return true;
            }
        }
        return false;
    }

    bool PayloadFilterPasses(const PanelState& s, const QA::ReplayEvent& e) {
        if (!s.payloadFilter[0]) return true;
        // Substring scan into the 64-char payload buffer (NUL-terminated
        // payloads or short ones — we walk until first NUL or end).
        usize fn = std::strlen(s.payloadFilter);
        if (fn == 0) return true;
        usize plen = 0;
        while (plen < sizeof(e.payload) && e.payload[plen]) plen++;
        for (usize i = 0; i + fn <= plen; ++i) {
            bool ok = true;
            for (usize k = 0; k < fn; ++k) {
                char ca = e.payload[i + k];
                char cb = s.payloadFilter[k];
                if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                if (ca != cb) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }

    bool EventPassesAllFilters(const PanelState& s, const QA::ReplayEvent& e) {
        if (!KindIsVisible(s, e.kind))   return false;
        if (!GuidFilterPasses(s, e))     return false;
        if (!PayloadFilterPasses(s, e))  return false;
        return true;
    }
} // namespace

void RenderTimelineTab(PanelState& s) {
    if (s.events.empty()) {
        ImGui::TextDisabled("(no events to scrub)");
        return;
    }

    // Wallclock span across all events. Used to map index -> X.
    u64 firstSec = s.events.front().unixSec;
    u64 lastSec  = s.events.back().unixSec;
    u64 spanSec  = (lastSec > firstSec) ? (lastSec - firstSec) : 1;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursorOrigin = ImGui::GetCursorScreenPos();
    f32 stripWidth  = ImGui::GetContentRegionAvail().x - 8.0f;
    if (stripWidth < 64.0f) stripWidth = 64.0f;
    f32 stripHeight = 56.0f;
    ImVec2 stripMin = cursorOrigin;
    ImVec2 stripMax = ImVec2(stripMin.x + stripWidth, stripMin.y + stripHeight);

    // Strip background.
    dl->AddRectFilled(stripMin, stripMax,
        IM_COL32(28, 30, 38, 255), 4.0f);
    dl->AddRect(stripMin, stripMax,
        IM_COL32(70, 75, 90, 255), 4.0f, 0, 1.5f);

    // Time axis tick at quarters.
    for (i32 q = 1; q <= 3; ++q) {
        f32 fx = stripMin.x + stripWidth * (f32)q / 4.0f;
        dl->AddLine(ImVec2(fx, stripMin.y + stripHeight - 6.0f),
                     ImVec2(fx, stripMax.y),
                     IM_COL32(80, 84, 100, 255), 1.0f);
    }

    // Faint tick for filtered-out events so the user sees what's hidden.
    for (size_t i = 0; i < s.events.size(); ++i) {
        const QA::ReplayEvent& e = s.events[i];
        if (EventPassesAllFilters(s, e)) continue;
        u64 dt = (e.unixSec >= firstSec) ? (e.unixSec - firstSec) : 0;
        f32 fx = stripMin.x + stripWidth * (f32)dt / (f32)spanSec;
        dl->AddLine(ImVec2(fx, stripMax.y - 4.0f),
                     ImVec2(fx, stripMax.y),
                     IM_COL32(60, 65, 78, 220), 1.0f);
    }

    // Visible events as colored ticks.
    for (size_t i = 0; i < s.events.size(); ++i) {
        const QA::ReplayEvent& e = s.events[i];
        if (!EventPassesAllFilters(s, e)) continue;
        u64 dt = (e.unixSec >= firstSec) ? (e.unixSec - firstSec) : 0;
        f32 fx = stripMin.x + stripWidth * (f32)dt / (f32)spanSec;
        ImU32 col = ColorForKind(e.kind);
        f32 thickness = ((i32)i == s.cursor) ? 3.0f : 1.6f;
        dl->AddLine(ImVec2(fx, stripMin.y + 4.0f),
                     ImVec2(fx, stripMax.y - 4.0f),
                     col, thickness);
    }

    // Cursor marker — bigger triangle pointing down.
    if (s.cursor >= 0 && s.cursor < (i32)s.events.size()) {
        const QA::ReplayEvent& e = s.events[s.cursor];
        u64 dt = (e.unixSec >= firstSec) ? (e.unixSec - firstSec) : 0;
        f32 fx = stripMin.x + stripWidth * (f32)dt / (f32)spanSec;
        dl->AddTriangleFilled(
            ImVec2(fx - 5.0f, stripMin.y - 1.0f),
            ImVec2(fx + 5.0f, stripMin.y - 1.0f),
            ImVec2(fx,         stripMin.y + 8.0f),
            IM_COL32(245, 235, 110, 255));
    }

    // Click-to-scrub interaction.
    ImGui::InvisibleButton("##rv_strip", ImVec2(stripWidth, stripHeight));
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        f32 mx = ImGui::GetMousePos().x;
        f32 t  = (mx - stripMin.x) / stripWidth;
        if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
        // Find the event whose unixSec offset is closest to t * span.
        u64 wantOff = (u64)((f64)spanSec * (f64)t);
        i32 best = 0;
        u64 bestDist = (u64)-1;
        for (size_t i = 0; i < s.events.size(); ++i) {
            u64 off = (s.events[i].unixSec >= firstSec)
                      ? (s.events[i].unixSec - firstSec) : 0;
            u64 dist = (off > wantOff) ? (off - wantOff) : (wantOff - off);
            if (dist < bestDist) { bestDist = dist; best = (i32)i; }
        }
        s.cursor = best;
        s.playing = false;
    }

    ImGui::Spacing();

    // Tape — visible events as a scroll list.
    if (ImGui::BeginChild("##rv_tape", ImVec2(0, 240.0f), true)) {
        for (size_t i = 0; i < s.events.size(); ++i) {
            const QA::ReplayEvent& e = s.events[i];
            if (!EventPassesAllFilters(s, e)) continue;
            char row[200];
            std::snprintf(row, sizeof(row),
                "[%4zu] kind=%d  ver=%u  guidA=0x%08X  guidB=0x%08X##rv_tape_%zu",
                i, (int)e.kind, e.snapshotVersion,
                (unsigned)e.guidA.raw, (unsigned)e.guidB.raw, i);
            bool selected = (s.cursor == (i32)i);
            ImGui::PushStyleColor(ImGuiCol_Text, ColorForKind(e.kind));
            if (ImGui::Selectable(row, selected)) {
                s.cursor = (i32)i;
                s.playing = false;
            }
            ImGui::PopStyleColor();
            // Right-click to set frame-diff anchors.
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Set as diff anchor A")) s.diffAnchorA = (i32)i;
                if (ImGui::MenuItem("Set as diff anchor B")) s.diffAnchorB = (i32)i;
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();
}

} // namespace ReplayViewer
} // namespace UI
} // namespace Vespucci
