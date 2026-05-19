// ============================================================================
// imgui_mocap_panel.h -- The "Mocap Studio" UI That Has No Business Existing
// ============================================================================
//
// An ImGui panel for controlling a real-time motion capture pipeline
// inside a viewer for a dead 2009 game. Load video, watch progress bars,
// scrub the timeline, adjust finger grip, export to native Conquest JSON.
// Pandemic Studios had a full Maya-based animation department. We have
// a webcam and sheer fucking willpower.
//
// This is compiled as part of the ImGui DLL (VS2022 C++17) which
// communicates with the VS2005 engine core through ImGuiGlueFrameArgs.
// The mocap fields were added at the END of that struct because moving
// members around causes .obj layout mismatch crashes. Learned that one
// the hard way. Twice.
//
// Separated from imgui_glue_dll.cpp because that file was becoming
// a 3000-line monster and I have SOME standards. Not many. But some.
// ============================================================================

#pragma once
#include "imgui_glue.h"

// Forward declare to avoid pulling in imgui.h here
// (imgui.h is included in the .cpp)

// Mocap state passed through ImGuiGlueFrameArgs
// (fields added at END of struct to preserve layout)

// Draw the Mocap Studio panel. Called from ImGuiGlue_DrawFrame.
void DrawMocapStudioPanel(const ImGuiGlueFrameArgs* args);
