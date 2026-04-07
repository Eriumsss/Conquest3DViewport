// ============================================================================
// imgui_mocap_panel.cpp -- Full Mocap Control Panel in Dear ImGui
// ============================================================================
//
// State machine UI: IDLE -> LOADING -> PROCESSING -> STREAMING -> DONE.
// Each state shows different controls. IDLE has "Load Video" and
// "Convert from PKL". Processing shows progress bars. DONE gives you
// playback controls, a timeline scrubber, finger curl sliders, and
// export buttons. ERROR shows what went wrong (usually Python).
//
// Communication with the engine is through const_cast hackery on
// ImGuiGlueFrameArgs because the struct is passed as const from the
// D3D9 side but we need to write request flags back. It's disgusting.
// The engine polls these flags each frame and acts on them. Request
// fields get cleared after processing. It works. Don't judge me.
//
// The "Convert to Conquest JSON" button triggers the full pipeline:
// retarget all frames, encode ThreeComp40 quaternions, write the
// animation file. From webcam video to game-ready animation in one
// click. Pandemic's animators rolling in their unemployment graves.
//
// VS2022 C++17. ImGui immediate mode. sprintf because I'm tired.
// Does not work properly and its shit
// ============================================================================
#include "imgui_mocap_panel.h"
#include "imgui/imgui.h"
#include <stdio.h>
#include <string.h>

// ============================================================
// Mocap Studio Panel
// ============================================================

void DrawMocapStudioPanel(const ImGuiGlueFrameArgs* args)
{
    // Access mocap-specific fields from args (added at end of struct)
    int   mocapState      = args->mocapState;
    float mocapProgress   = args->mocapProgress;
    const char* mocapStatus = args->mocapStatusMsg ? args->mocapStatusMsg : "";
    const char* mocapError  = args->mocapErrorMsg  ? args->mocapErrorMsg  : "";
    int   mocapTotalFrames  = args->mocapTotalFrames;
    int   mocapRecvFrames   = args->mocapReceivedFrames;
    float mocapFps          = args->mocapFps;
    int   mocapSubjectCount = args->mocapSubjectCount;
    float mocapPlayTime     = args->mocapPlaybackTime;
    float mocapDuration     = args->mocapDuration;
    int   mocapPlaying      = args->mocapPlaying;
    float mocapFingerL      = args->mocapFingerCurlL;
    float mocapFingerR      = args->mocapFingerCurlR;

    // Cast away const for output fields
    ImGuiGlueFrameArgs* out = const_cast<ImGuiGlueFrameArgs*>(args);

    if (!ImGui::Begin("Mocap Studio"))
    {
        ImGui::End();
        return;
    }

    // ---- Header ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Motion Capture Pipeline");
    ImGui::Separator();

    // ---- State: IDLE ----
    if (mocapState == 0) // MOCAP_IDLE
    {
        ImGui::Text("Load a video to begin motion capture.");
        ImGui::Spacing();

        if (ImGui::Button("Load Video...", ImVec2(200, 30)))
        {
            out->mocapRequestLoadVideo = 1;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("Supports MP4, MOV, AVI");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Or convert a pre-processed WHAM capture:");
        ImGui::TextDisabled("Run run_wham.bat first, then click Convert");
        if (ImGui::Button("Convert from PKL", ImVec2(200, 30)))
        {
            out->mocapRequestConvert = 1;
        }
    }
    // ---- State: LOADING / PROCESSING / STREAMING ----
    else if (mocapState >= 1 && mocapState <= 3) // LOADING, PROCESSING, STREAMING
    {
        const char* stateNames[] = { "", "Loading Models", "Processing Video", "Receiving Frames" };
        ImGui::Text("State: %s", stateNames[mocapState]);
        ImGui::Spacing();

        // Progress bar
        char overlay[128];
        sprintf(overlay, "%.0f%%", mocapProgress * 100.0f);
        ImGui::ProgressBar(mocapProgress, ImVec2(-1, 20), overlay);

        ImGui::TextWrapped("%s", mocapStatus);

        if (mocapState == 3) // STREAMING
        {
            ImGui::Text("Frames: %d / %d", mocapRecvFrames, mocapTotalFrames);
        }

        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
        {
            out->mocapRequestCancel = 1;
        }
    }
    // ---- State: DONE ----
    else if (mocapState == 4) // MOCAP_DONE
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Capture Complete!");
        ImGui::Text("Frames: %d  |  FPS: %.1f  |  Duration: %.1fs  |  Subjects: %d",
                     mocapTotalFrames, mocapFps,
                     mocapDuration, mocapSubjectCount);
        ImGui::Separator();

        // ---- Playback Controls ----
        ImGui::Text("Preview");

        // Play/Pause
        if (ImGui::Button(mocapPlaying ? "Pause" : "Play", ImVec2(80, 0)))
        {
            out->mocapRequestTogglePlay = 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(60, 0)))
        {
            out->mocapRequestSetTime = 1;
            out->mocapRequestedTime = 0.0f;
        }

        // Timeline scrubber
        float t = mocapPlayTime;
        if (ImGui::SliderFloat("Time", &t, 0.0f, mocapDuration, "%.2fs"))
        {
            out->mocapRequestSetTime = 1;
            out->mocapRequestedTime = t;
        }

        ImGui::Separator();

        // ---- Retargeting Controls ----
        ImGui::Text("Retargeting");

        // Finger curl sliders
        float fL = mocapFingerL;
        float fR = mocapFingerR;
        if (ImGui::SliderFloat("Left Grip", &fL, 0.0f, 1.0f))
        {
            out->mocapRequestSetFingerCurl = 1;
            out->mocapRequestedFingerCurlL = fL;
            out->mocapRequestedFingerCurlR = fR;
        }
        if (ImGui::SliderFloat("Right Grip", &fR, 0.0f, 1.0f))
        {
            out->mocapRequestSetFingerCurl = 1;
            out->mocapRequestedFingerCurlL = fL;
            out->mocapRequestedFingerCurlR = fR;
        }

        ImGui::Separator();

        // ---- Convert & Export ----
        ImGui::Text("Export");

        if (ImGui::Button("Convert to Conquest JSON", ImVec2(-1, 30)))
        {
            out->mocapRequestConvert = 1;
        }

        if (ImGui::Button("Apply to Current Model", ImVec2(-1, 30)))
        {
            out->mocapRequestApplyToModel = 1;
        }

        ImGui::Spacing();

        // New video button
        if (ImGui::Button("New Video...", ImVec2(120, 0)))
        {
            out->mocapRequestLoadVideo = 1;
        }
    }
    // ---- State: ERROR ----
    else if (mocapState == 5) // MOCAP_ERROR
    {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error!");
        ImGui::TextWrapped("%s", mocapError);
        ImGui::Spacing();
        if (ImGui::Button("Try Again", ImVec2(120, 0)))
        {
            out->mocapRequestLoadVideo = 1;
        }
    }

    ImGui::End();
}
