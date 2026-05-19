// Panel.cpp
// =============================================================================
// REPLAY VIEWER ORCHESTRATOR - LOAD, SCRUB, FILTER, DRILL DOWN
// =============================================================================
// Written by: Eriumsss
//
// Three sub-views live behind tabs:
//   1. Timeline    — horizontal scrubber, draws every event as a tick
//                    on a time axis. Click to set the cursor.
//   2. Event detail — fully-decoded view of the current cursor event.
//   3. Frame diff  — compare two anchored events; show what shifted
//                    between them in plain English ("3 ranker queries,
//                    2 wires applied, 1 undo, snapshot version stepped
//                    +47 between these two points").
//
// Patrick mode default: only the "Open replay..." button + a one-line
// summary ("Loaded 12,847 events from MyReplay.vespreplay") render.
// Click "Advanced" to show the tabs and scrubber.
// =============================================================================

#include "Panel.h"

#include "../../Core/FileIO.h"
#include "../../Core/Logging.h"
#include "../../Core/PathUtils.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"
#include "../Common/SearchBox.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace ReplayViewer {

// Forward decls — sub-renderers live in Timeline.cpp and FrameDiff.cpp.
void RenderTimelineTab(PanelState& s);
void RenderEventDetailTab(PanelState& s);
void RenderFrameDiffTab(PanelState& s);

namespace {
    const char* KindGlyph(QA::ReplayEventKind k) {
        switch (k) {
            case QA::REPLAY_SnapshotChanged: return "snap";
            case QA::REPLAY_RankerQuery:     return "rank";
            case QA::REPLAY_ApplyFired:      return "wire";
            case QA::REPLAY_UndoFired:       return "undo";
            case QA::REPLAY_PreviewStaged:   return "ghst";
            case QA::REPLAY_LevelLoaded:     return "load";
        }
        return "????";
    }
} // namespace

void InitPanelState(PanelState& s) {
    s.visible = true;             // surface flag controls actual show/hide
    s.loadedPath[0] = 0;
    s.events.clear();
    s.cursor = 0;
    s.playing = false;
    s.playSpeed = 4.0f;            // 4 events/sec default — readable scroll
    s.accumulatorSec = 0.0;
    s.showSnapshotEvents  = true;
    s.showQueryEvents     = true;
    s.showApplyEvents     = true;
    s.showUndoEvents      = true;
    s.showPreviewEvents   = true;
    s.showLevelLoadEvents = true;
    s.guidFilter[0] = 0;
    s.payloadFilter[0] = 0;
    s.diffAnchorA = -1;
    s.diffAnchorB = -1;
    s.showAdvanced = false;
}

bool LoadReplayFileIntoPanel(PanelState& s, const char* path) {
    s.events.clear();
    s.cursor = 0;
    s.playing = false;
    s.diffAnchorA = -1;
    s.diffAnchorB = -1;

    if (!path || !path[0]) {
        Core::Logging::Warn("ReplayViewer: empty path passed to load");
        return false;
    }

    Core::Mapped m;
    if (!Core::MapReadOnly(path, m)) {
        Core::Logging::Error("ReplayViewer: cannot map '%s'", path);
        return false;
    }

    bool ok = false;
    if (m.size >= (i64)(sizeof(u32) * 2)) {
        const u32* hdr = (const u32*)m.data;
        // Magic 'R','E','P','R' little-endian = 0x52455052.
        if (hdr[0] == 0x52455052u && hdr[1] == 1) {
            const QA::ReplayEvent* events =
                (const QA::ReplayEvent*)(m.data + sizeof(u32) * 2);
            i64 nEvents = (m.size - (i64)(sizeof(u32) * 2)) / (i64)sizeof(QA::ReplayEvent);
            s.events.reserve((size_t)nEvents);
            for (i64 i = 0; i < nEvents; ++i) s.events.push_back(events[i]);
            std::strncpy(s.loadedPath, path, sizeof(s.loadedPath) - 1);
            s.loadedPath[sizeof(s.loadedPath) - 1] = 0;
            ok = true;
            Core::Logging::Info("ReplayViewer: loaded %lld events from %s",
                (long long)nEvents, path);
        } else {
            Core::Logging::Error("ReplayViewer: '%s' has wrong magic 0x%08X (want 0x52455052) "
                "or wrong version %u (want 1) — not a vespreplay file",
                path, hdr[0], hdr[1]);
        }
    }
    Core::Unmap(m);
    return ok;
}

namespace {
    // Auto-advance the cursor when "play" is on. Wallclock-based so the
    // playback speed is independent of frame rate.
    void TickPlaybackOrIdle(PanelState& s) {
        if (!s.playing || s.events.empty()) return;
        // ImGui::GetIO().DeltaTime is wallclock seconds since last frame.
        f64 dt = (f64)ImGui::GetIO().DeltaTime;
        s.accumulatorSec += dt;
        f64 stepSec = (s.playSpeed > 0.001f) ? (1.0 / (f64)s.playSpeed) : 1.0;
        while (s.accumulatorSec >= stepSec) {
            s.accumulatorSec -= stepSec;
            s.cursor++;
            if (s.cursor >= (i32)s.events.size()) {
                s.cursor = (i32)s.events.size() - 1;
                s.playing = false;
                break;
            }
        }
    }

    // Render the Patrick-mode default — one big load button + summary line.
    // Returns true if the user requested a load via the button (caller
    // shows the file picker / pastes a hardcoded path).
    bool RenderPatrickDefault(PanelState& s) {
        bool wantLoad = false;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.40f, 0.60f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.52f, 0.78f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.32f, 0.64f, 0.92f, 1.0f));
        if (ImGui::Button("Open replay file...##rv_open", ImVec2(-1.0f, 40.0f))) {
            wantLoad = true;
        }
        ImGui::PopStyleColor(3);

        if (s.events.empty()) {
            ImGui::TextDisabled("No replay loaded. Drop a `.vespreplay` file here, "
                                  "or click Open replay above.");
        } else {
            const char* fname = s.loadedPath[0] ? s.loadedPath : "(unknown)";
            // Show only the basename — full path is noise.
            const char* base = fname;
            for (const char* p = fname; *p; ++p) {
                if (*p == '/' || *p == '\\') base = p + 1;
            }
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                "Loaded %d events from %s", (i32)s.events.size(), base);

            // Mini-summary by event kind.
            i32 counts[8] = {0};
            for (size_t i = 0; i < s.events.size(); ++i) {
                i32 k = (i32)s.events[i].kind;
                if (k >= 0 && k < 8) counts[k]++;
            }
            ImGui::TextDisabled("Snapshot bumps: %d   Ranker queries: %d   Wires applied: %d   "
                                  "Undos: %d   Previews: %d   Level loads: %d",
                counts[QA::REPLAY_SnapshotChanged],
                counts[QA::REPLAY_RankerQuery],
                counts[QA::REPLAY_ApplyFired],
                counts[QA::REPLAY_UndoFired],
                counts[QA::REPLAY_PreviewStaged],
                counts[QA::REPLAY_LevelLoaded]);
        }
        return wantLoad;
    }
} // namespace

void RenderReplayViewerPanel(PanelState& s, ImGuiGlueFrameArgs& /*args*/) {
    if (!s.visible) return;
    Common::PushVespucciStyleScope();

    if (ImGui::Begin("Vespucci Replay Viewer", &s.visible)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Common::Theme::TextPrimary);
        ImGui::Text("Replay Viewer");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        f32 toggleWidth = 96.0f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - toggleWidth);
        ImGui::Checkbox("Advanced##rv_adv", &s.showAdvanced);
        ImGui::Separator();

        bool wantLoad = RenderPatrickDefault(s);
        if (wantLoad) {
            // V1 has no native file picker bound — designer types the
            // path. Power users will hit this through Advanced anyway.
            // We offer a default path probe and bail to the field otherwise.
            ImGui::OpenPopup("##rv_open_path");
        }

        // Inline path field (so the user can paste a path and hit Enter).
        if (ImGui::BeginPopup("##rv_open_path")) {
            ImGui::Text("Paste a `.vespreplay` path:");
            static char s_pathField[280];
            ImGui::SetNextItemWidth(420.0f);
            bool fired = ImGui::InputText("##rv_path_in", s_pathField, sizeof(s_pathField),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Load##rv_path_load") || fired) {
                if (LoadReplayFileIntoPanel(s, s_pathField)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##rv_path_cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (s.showAdvanced && !s.events.empty()) {
            TickPlaybackOrIdle(s);

            ImGui::Spacing();
            ImGui::Separator();

            // Transport row — Play / Pause / Step buttons + speed slider.
            if (ImGui::Button(s.playing ? "Pause##rv_pp" : "Play##rv_pp")) {
                s.playing = !s.playing;
            }
            ImGui::SameLine();
            if (ImGui::Button("|<<##rv_first")) { s.cursor = 0; s.accumulatorSec = 0.0; }
            ImGui::SameLine();
            if (ImGui::Button("<<##rv_back")) {
                s.cursor -= 10;
                if (s.cursor < 0) s.cursor = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button(">>##rv_fwd")) {
                s.cursor += 10;
                i32 maxIdx = (i32)s.events.size() - 1;
                if (s.cursor > maxIdx) s.cursor = maxIdx;
            }
            ImGui::SameLine();
            if (ImGui::Button(">>|##rv_last")) {
                s.cursor = (i32)s.events.size() - 1;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("speed##rv_speed", &s.playSpeed, 0.5f, 60.0f, "%.1f evt/s");

            // Filter row — kind toggles + GUID/payload substring.
            ImGui::Separator();
            ImGui::Text("Filter:");
            ImGui::SameLine(); ImGui::Checkbox("snap##rv_f_snap", &s.showSnapshotEvents);
            ImGui::SameLine(); ImGui::Checkbox("rank##rv_f_q",    &s.showQueryEvents);
            ImGui::SameLine(); ImGui::Checkbox("wire##rv_f_a",    &s.showApplyEvents);
            ImGui::SameLine(); ImGui::Checkbox("undo##rv_f_u",    &s.showUndoEvents);
            ImGui::SameLine(); ImGui::Checkbox("ghst##rv_f_p",    &s.showPreviewEvents);
            ImGui::SameLine(); ImGui::Checkbox("load##rv_f_l",    &s.showLevelLoadEvents);
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputTextWithHint("##rv_guid_filter", "guid hex prefix",
                s.guidFilter, sizeof(s.guidFilter));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##rv_payload_filter", "payload substring",
                s.payloadFilter, sizeof(s.payloadFilter));

            ImGui::Separator();

            if (ImGui::BeginTabBar("##rv_tabs")) {
                if (ImGui::BeginTabItem("Timeline")) {
                    RenderTimelineTab(s);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Event detail")) {
                    RenderEventDetailTab(s);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Frame diff")) {
                    RenderFrameDiffTab(s);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
    }
    ImGui::End();
    Common::PopVespucciStyleScope();
    (void)&KindGlyph; // suppress unused-static warning (shared with sub-renderers)
}

} // namespace ReplayViewer
} // namespace UI
} // namespace Vespucci
