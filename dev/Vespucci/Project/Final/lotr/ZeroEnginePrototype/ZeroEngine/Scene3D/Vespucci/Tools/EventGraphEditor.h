// EventGraphEditor.h
// ============================================================================
// THE MOTHERFUCKING NERVE CENTER FOR EVENT WIRES
// ============================================================================
// Written by: Eriumsss
//
// This file exists because the old UI tried to explain one event graph three
// different ways in one monstrous DLL function, then acted shocked when the
// result felt like a scrap pile with buttons glued to it. Flow, Chain, and the
// standalone wiring panel all walked the same owner -> Output -> target corpse,
// each with its own little table of colors, focus rules, and half-remembered
// selection behavior. That is how tools rot. That is how users start wondering
// if the level format itself is insane, when the real failure is the UI making
// the same GUID truth wear three different masks.
//
// So the wiring editor gets a real file now. One state owner. One event color
// table. One F4 overlay focus. One panel that treats Pandemic's exported event
// graph like authored gameplay instead of a JSON autopsy. The renderer has
// files for spatial math. Splines have files for curve surgery. Event wiring
// gets the same respect, because this shit is not decorative. It is the logic
// nervous system of the map.
//
// If you are here to add another "small" event graph view directly into
// imgui_glue_dll.cpp, close the file and walk away. That 20K-line bastard has
// suffered enough. Put the logic here, keep the bridge clean, and do not touch
// ImGuiGlueFrameArgs unless the struct-layout-end commandment has been obeyed
// in blood.
// ============================================================================
#pragma once

struct ImGuiGlueFrameArgs;

namespace EventGraphEditor
{
    // Existing DLL-owned F4 state. This binder avoids a struct grow while still
    // letting the module write the same overlay focus the renderer already eats.
    void BindEditorState(unsigned int* editorCategoryMask,
                         bool* eventWiresShow,
                         unsigned int* eventWireFocusGuid,
                         void (*saveFn)());

    void Update(ImGuiGlueFrameArgs& args);
    void DrawPanel(ImGuiGlueFrameArgs& args);
}
