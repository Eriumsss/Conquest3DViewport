// LevelInspector.cpp — Poking Dead Objects With a Ray and Hoping They Respond
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Mouse picking, hover detection, selection state, drag logic for level
// objects. Casts rays from screen space into the level's bounding box
// hierarchy. When you click on a barrel in Minas Tirith, this code tells
// you which barrel, what model, what CRC, what transform. The ImGui
// rendering is in the DLL layer. This file is VS2005 C++03 because the
// ray math links against the stolen Havok SDK's hkVector4 and I can't
// mix compiler versions without the ABI shitting the bed.
// -----------------------------------------------------------------------

#include "LevelInspector.h"
#include "LevelScene.h"
#include <math.h>

LevelInspector::LevelInspector()
    : m_open(true), m_lastHovered(-1), m_lastSelected(-1),
      m_editorDragging(false), m_editorObjMoved(false), m_dragPlaneD(0),
      m_rightClickHit(false)
{
    m_dragStartOrigin[0] = m_dragStartOrigin[1] = m_dragStartOrigin[2] = 0;
    m_dragStartDir[0] = m_dragStartDir[1] = m_dragStartDir[2] = 0;
    m_dragObjStartPos[0] = m_dragObjStartPos[1] = m_dragObjStartPos[2] = 0;
    m_rightClickWorldPos[0] = m_rightClickWorldPos[1] = m_rightClickWorldPos[2] = 0;
}

// Intersect ray with horizontal plane (Y=planeY). Returns parameter t.
static bool RayPlaneY(const float o[3], const float d[3], float planeY, float& t)
{
    if (fabsf(d[1]) < 1e-7f) return false;
    t = (planeY - o[1]) / d[1];
    return t > 0;
}

void LevelInspector::update(LevelScene* scene,
                             int mouseX, int mouseY,
                             int vpX, int vpY, int vpW, int vpH,
                             bool mouseDown, bool mouseInViewport,
                             bool mouseDragging)
{
    if (!scene || !scene->isLoaded()) return;
    if (!mouseInViewport) {
        scene->setHoveredInstance(-1);
        scene->setHoveredEditorObj(-1);
        if (!mouseDragging) m_editorDragging = false;
        return;
    }

    // Cast ray from mouse position
    float origin[3], dir[3];
    scene->screenToRay(mouseX - vpX, mouseY - vpY, vpW, vpH, origin, dir);

    // --- Drag mode: move selected editor object along ground plane ---
    if (m_editorDragging && mouseDragging)
    {
        int selEO = scene->selectedEditorObj();
        if (selEO >= 0)
        {
            float t;
            if (RayPlaneY(origin, dir, m_dragObjStartPos[1], t))
            {
                float hitX = origin[0] + dir[0] * t;
                float hitZ = origin[2] + dir[2] * t;

                float t0;
                if (RayPlaneY(m_dragStartOrigin, m_dragStartDir, m_dragObjStartPos[1], t0))
                {
                    float startHitX = m_dragStartOrigin[0] + m_dragStartDir[0] * t0;
                    float startHitZ = m_dragStartOrigin[2] + m_dragStartDir[2] * t0;

                    float newX = m_dragObjStartPos[0] + (hitX - startHitX);
                    float newZ = m_dragObjStartPos[2] + (hitZ - startHitZ);

                    scene->setEditorObjPosition(selEO, newX, m_dragObjStartPos[1], newZ);
                    m_editorObjMoved = true;
                }
            }
        }
        return;
    }

    if (!mouseDragging) m_editorDragging = false;

    // Pick both — always
    float edDist = 1e30f;
    int edHit = scene->pickEditorObj(origin, dir, &edDist);
    int hit = scene->pickInstance(origin, dir);

    // Both are available — editor obj wins (they are small on-top markers)
    if (edHit >= 0) {
        scene->setHoveredEditorObj(edHit);
        scene->setHoveredInstance(-1);
    } else {
        scene->setHoveredEditorObj(-1);
        scene->setHoveredInstance(hit);
    }
    m_lastHovered = hit;

    if (mouseDown && edHit >= 0) {
        // Editor object clicked
        if (scene->selectedEditorObj() == edHit)
        {
            scene->setSelectedEditorObj(-1);
            m_editorDragging = false;
        }
        else
        {
            scene->setSelectedEditorObj(edHit);
            const LevelEditorObj* eo = scene->getEditorObj(edHit);
            if (eo)
            {
                m_dragStartOrigin[0] = origin[0]; m_dragStartOrigin[1] = origin[1]; m_dragStartOrigin[2] = origin[2];
                m_dragStartDir[0] = dir[0]; m_dragStartDir[1] = dir[1]; m_dragStartDir[2] = dir[2];
                m_dragObjStartPos[0] = eo->mat[12]; m_dragObjStartPos[1] = eo->mat[13]; m_dragObjStartPos[2] = eo->mat[14];
                m_editorDragging = true;
            }
        }
        scene->setSelectedInstance(-1);
    }
    else if (mouseDown && hit >= 0) {
        if (scene->selectedInstance() == hit)
            scene->setSelectedInstance(-1);
        else
            scene->setSelectedInstance(hit);
        m_lastSelected = scene->selectedInstance();
        scene->setSelectedEditorObj(-1);
        m_editorDragging = false;
    }
}

void LevelInspector::updateRightClick(LevelScene* scene,
                                       int mouseX, int mouseY,
                                       int vpX, int vpY, int vpW, int vpH)
{
    if (!scene || !scene->isLoaded()) return;

    float origin[3], dir[3];
    scene->screenToRay(mouseX - vpX, mouseY - vpY, vpW, vpH, origin, dir);

    // Try Y=0 ground plane first (most reliable for flat levels)
    float t;
    if (RayPlaneY(origin, dir, 0.0f, t) && t > 0.0f)
    {
        m_rightClickWorldPos[0] = origin[0] + dir[0] * t;
        m_rightClickWorldPos[1] = 0.0f;
        m_rightClickWorldPos[2] = origin[2] + dir[2] * t;
        m_rightClickHit = true;
        return;
    }

    // Fallback: place at a fixed distance along the ray (20 units in front of camera)
    float fallbackDist = 20.0f;
    m_rightClickWorldPos[0] = origin[0] + dir[0] * fallbackDist;
    m_rightClickWorldPos[1] = origin[1] + dir[1] * fallbackDist;
    m_rightClickWorldPos[2] = origin[2] + dir[2] * fallbackDist;
    m_rightClickHit = true;
}
