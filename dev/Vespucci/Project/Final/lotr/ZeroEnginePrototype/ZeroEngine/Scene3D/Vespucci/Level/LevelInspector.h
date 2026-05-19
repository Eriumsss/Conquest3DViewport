// LevelInspector.h — Staring Into the Guts of Every Level Object
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// ImGui panel for inspecting every placed object in a loaded Conquest
// level. Hover over a mesh instance → see its CRC, model name, material
// bindings, world transform, game mode mask. Click → select it, highlight
// it, drill into its entity fields. It's like an X-ray for Pandemic's
// dead levels. You can see EVERYTHING their designers placed: the spawn
// points they agonized over, the trigger volumes they set up for
// scripted events, the path networks they hand-drew for AI navigation.
//
// Mouse picking uses D3D9 ray-casting against the level's bounding boxes.
// Simple but effective. Pandemic's internal editor ("ZeroEdit") had the
// same feature — we found raycast function stubs in the .exe's RTTI.
//
// "Be kind, for everyone you meet is fighting a hard battle." — Plato
// Every object in this inspector represents someone's work at Pandemic.
// A level designer placed that barrel. An environment artist textured
// that wall. A lighting artist tuned that sun angle. They were fighting
// their own hard battles — crunch, deadlines, an impending studio
// closure. Their work lives on here. In our inspector. In our engine.
// -----------------------------------------------------------------------
#pragma once

class LevelScene;
struct IDirect3DDevice9;

class LevelInspector
{
public:
    LevelInspector();

    // Call once per frame from the main render loop.
    // mouseX/mouseY: current cursor position in viewport pixels
    // vpX/vpY/vpW/vpH: viewport rectangle (for ray unprojection)
    // mouseDown: true if left mouse button was just clicked this frame
    // mouseDragging: true if left mouse button is held and mouse moved
    void update(LevelScene* scene,
                int mouseX, int mouseY,
                int vpX, int vpY, int vpW, int vpH,
                bool mouseDown, bool mouseInViewport,
                bool mouseDragging = false);

    // ImGui rendering is done in imgui_glue_dll.cpp (needs C++11 compiler).
    // This class only handles picking logic (VS2005-compatible).

    bool isOpen() const { return m_open; }
    void setOpen(bool v) { m_open = v; }
    int  lastHovered()  const { return m_lastHovered; }
    int  lastSelected() const { return m_lastSelected; }

    // Drag state for editor objects
    bool isEditorObjDragging() const { return m_editorDragging; }
    bool editorObjMoved() const { return m_editorObjMoved; }
    void clearEditorObjMoved() { m_editorObjMoved = false; }

    // Right-click entity creation
    void updateRightClick(LevelScene* scene,
                          int mouseX, int mouseY,
                          int vpX, int vpY, int vpW, int vpH);
    bool hasRightClickHit() const { return m_rightClickHit; }
    const float* rightClickWorldPos() const { return m_rightClickWorldPos; }
    void clearRightClickHit() { m_rightClickHit = false; }

private:
    bool m_open;
    int  m_lastHovered;
    int  m_lastSelected;

    // Editor object drag state
    bool  m_editorDragging;
    bool  m_editorObjMoved;
    float m_dragStartOrigin[3];
    float m_dragStartDir[3];
    float m_dragObjStartPos[3];
    float m_dragPlaneD;         // distance along ray to drag plane

    // Right-click entity creation state
    bool  m_rightClickHit;
    float m_rightClickWorldPos[3];
};
