// Panel.h
// =============================================================================
// REPLAY VIEWER - LOAD A RECORDED SESSION, SCRUB IT, INSPECT EVENTS
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked. The QA pipeline records every editor session into a
// `.vespreplay` binary blob: snapshot version bumps, ranker queries,
// Apply / Preview / Undo fires, level loads. This panel is the
// designer-facing way to:
//
//   1. Load any `.vespreplay` from disk and see what happened.
//   2. Scrub a timeline scrubber to walk through the session at any
//      pace — wallclock, tick, or per-event step.
//   3. Inspect the diff between two snapshot versions side by side
//      (event count delta, ranker-query delta, what entities the
//      designer touched between the two points).
//   4. Re-run the recorded events through the live editor for repro
//      of a reported bug ("here's the .vespreplay where it goes weird").
//
// Patrick mode default: a single big "Open replay..." button. The
// scrubber, frame-diff, and per-event drill-down stay collapsed
// behind expanders. Power users open Advanced and get the full
// cockpit.
// =============================================================================

#ifndef VESPUCCI_UI_REPLAYVIEWER_PANEL_H_
#define VESPUCCI_UI_REPLAYVIEWER_PANEL_H_

#include "../../Core/VespucciTypes.h"
#include "../../QA/ReplayRecorder.h"

#include <string>
#include <vector>

struct ImGuiGlueFrameArgs;

namespace Vespucci {
namespace UI {
namespace ReplayViewer {

// Per-session UI state. One per Forge tab instance. Owns the loaded
// event vector outright — the file is read once at load time and
// cached here so scrubbing is RAM-cheap.
struct PanelState {
    bool                          visible;
    char                          loadedPath[260];

    // The loaded event timeline. Sorted ascending by unixSec then by
    // insertion order — recorder writes them in order so the file
    // already gives us this property.
    std::vector<QA::ReplayEvent>  events;

    // Scrubber position. `cursor` is the index of the currently-focused
    // event; `playing` makes the panel auto-advance one event per frame
    // when set; `playSpeed` is events-per-second.
    i32                           cursor;
    bool                          playing;
    f32                           playSpeed;
    f64                           accumulatorSec;     // wallclock since last advance

    // Filter inputs — only applied when Advanced is on.
    bool                          showSnapshotEvents;
    bool                          showQueryEvents;
    bool                          showApplyEvents;
    bool                          showUndoEvents;
    bool                          showPreviewEvents;
    bool                          showLevelLoadEvents;
    char                          guidFilter[32];     // hex prefix substring
    char                          payloadFilter[64];

    // Frame-diff anchors. Set by clicking two events in the timeline;
    // the FrameDiff tab shows what changed between them.
    i32                           diffAnchorA;
    i32                           diffAnchorB;

    // UX gate.
    bool                          showAdvanced;
};

void InitPanelState(PanelState& s);
void RenderReplayViewerPanel(PanelState& s, ImGuiGlueFrameArgs& args);

// Load a replay file from disk into PanelState. Returns true on
// success; on failure leaves `events` empty and writes an Error log line.
bool LoadReplayFileIntoPanel(PanelState& s, const char* path);

} // namespace ReplayViewer
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_REPLAYVIEWER_PANEL_H_
