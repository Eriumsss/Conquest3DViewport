// SplineEditor.h - The Spline That Finally Fucking Bends When You Pull It
// ───────────────────────────────────────────────────────────────────────
// Written by: Eriumsss
//
// For years the cinematic spline editor in this codebase was 400 lines of
// DragFloat3 sliders. One row per control point. Fifty-node spline? Fifty
// mouse drags, one axis at a time, staring at a goddamn numeric list while
// the 3D viewport rendered the result 800 pixels away. Zero spatial
// intuition. Zero "pull the head and watch the tail follow." Zero
// "squeeze the chain into a crescent shape with a slider." Just lists.
// Lists of numbers. Like editing a Figma layer by typing pixel coordinates
// into a spreadsheet.
//
// This file is the rewrite the editor should have had from day one:
//
//   - Head and tail of the spline get 3D draggable SPHERES in the
//     viewport (green for head, red for tail). Intermediate control
//     points get smaller yellow dots, also draggable.
//
//   - When the user drags any node, FABRIK (Forward And Backward
//     Reaching IK - the standard fixed-length-chain solver used in
//     every animation rig since roughly 2011) redistributes the
//     other nodes so segment lengths are PRESERVED. Pull the head,
//     every other node slides into place like you yanked on a
//     physical rope. Segment spacing is snapshotted at drag-start
//     so the chain preserves whatever artistic intent was there.
//
//   - Each endpoint also gets an EXTEND ARROW sticking outward. Drag
//     the arrow past one segment-length and a new node spawns. Drag
//     it back into the chain and the end node gets removed. Floor
//     at 2 nodes because a "spline" of one node is not a fucking
//     spline, it is just a point with aspirations.
//
//   - Two shape sliders on the cinematic panel bias the FABRIK
//     solution: Curvature Amount (-1..+1, how much sideways bend)
//     and Curvature Frequency (1..5, 1=crescent arc, 2=S-curve,
//     3+=serpentine snake). These feed target positions into the
//     solver so the chain naturally settles into the requested shape
//     between its endpoints.
//
//   - The old numeric list UI stays as a synced fallback for
//     precision work. Click a node in 3D, its row scrolls into view.
//     Edit a row by hand, the 3D scene re-solves.
//
// EVERYTHING else in this file exists because the code that wants to
// call this module lives in two separate processes (the VS2005 exe
// and the VS2022 ImGui DLL) that communicate via C structs in
// imgui_glue.h. This module runs INSIDE THE EXE and is called by
// ZeroEngine3DViewport.cpp once per frame. It reads edit-mode state
// from ImGuiGlueFrameArgs, runs picking against the currently
// selected spline, drives the drag state machine, runs FABRIK, and
// writes the resulting node array back via the EXISTING
// cineNodeEditRequested / cineNodeEditData args fields so the normal
// writeback path in the viewport persists the edit. Zero new
// plumbing. One existing edit path. Clean as shit.
//
// Design constraint: the viewport file is CONTESTED - other agents
// work on it too. This module exists specifically to keep new code
// OUT of the viewport. The viewport gets ~5 new lines total: one
// include, one Update() call per frame, one DrawHandles() call in
// LevelScene's spline draw block. Everything else lives HERE. If
// you find yourself needing to add a whole new subsystem to the
// viewport to make the spline editor work, STOP, come back to this
// file, add it here instead. The viewport is a war zone. This file
// is home.

#ifndef SPLINE_EDITOR_H
#define SPLINE_EDITOR_H

#include <d3d9.h>

struct ImGuiGlueFrameArgs;  // declared in imgui_glue.h

// LevelScene and LevelSpline live in the ROOT namespace (despite the file
// being called LevelScene.h and sitting inside the ZeroEngine directory -
// Pandemic's naming conventions were never goddamn consistent). Only
// LevelReader is inside `namespace ZeroEngine`, so THAT is the one that
// needs qualification, nothing else. Took one failed compile to catch
// this - updating the forward decls so they match the real thing.
class LevelScene;

namespace ZeroEngine {
    class LevelReader;
}

namespace SplineEditor {

// Called once per frame from the viewport main loop, after the DLL has
// filled in args but BEFORE the existing cinematic writeback block that
// persists cineNodeEditRequested / cineNodeEditData. This function
// handles:
//   - Reading args.cineSplineEditMode, args.cineCurvatureAmount,
//     args.cineCurvatureFreq to know what the user wants.
//   - Running mouse picking against the currently selected spline's
//     handles and extend arrows (via LevelScene::screenToRay).
//   - Driving the drag state machine: hover, click-to-grab, drag-
//     to-update, release-to-commit.
//   - Running FABRIK to redistribute the chain.
//   - Applying curvature bias to the solver's targets.
//   - Handling arrow drag to add/remove nodes.
//   - On any commit, populating args.cineNodeEditRequested,
//     cineNodeEditGuid, cineNodeEditCount, cineNodeEditData so the
//     EXISTING viewport writeback path persists the change via
//     AddFieldEdit (kind=7 floatArray, stride=4).
//
// mouseX/mouseY are in VIEWPORT pixel coordinates (not screen-global).
// mouseDown is "left button currently held". mouseClicked is "left
// button just transitioned from up to down this frame." This is the
// standard two-signal input model and the viewport already tracks
// both of these as g_leftMouseDown / g_leftMouseClicked file-scope
// statics. Caller passes them in, we stay stateless across calls
// except for our own internal drag state.
void Update(ImGuiGlueFrameArgs& args,
            LevelScene* scene,
            ZeroEngine::LevelReader* reader,
            float mouseX, float mouseY,
            int viewportW, int viewportH,
            bool mouseDown, bool mouseClicked);

// Called once per frame from inside LevelScene's spline-draw block,
// AFTER the existing curve+node-marker rendering has already drawn
// the spline polyline. This draws on top: the handle spheres, the
// extend arrows, the drag preview, any hover highlight. Uses the
// same DrawPrimitiveUP pattern as the existing spline rendering so
// we do not invent new render plumbing.
//
// Only does anything if edit mode is currently active AND a spline
// is selected. Otherwise no-op. Safe to call unconditionally.
void DrawHandles(IDirect3DDevice9* dev,
                 LevelScene* scene,
                 const float eyePos[3],
                 const float viewMat[16],
                 const float projMat[16]);

// Set by Update() when the user clicks on a 3D handle, read by the
// DLL-side numeric list so it can auto-scroll the clicked node's row
// into view. Zero means "no click this frame / no node hit."
int GetClickedNodeIndex();

// Set by Update() when it commits an add/remove. Read by the DLL so
// it can resize its local node buffer to match. Zero means no mutation
// this frame.
int GetLastNodeCountDelta();

// ─────────────────────────────────────────────────────────────────────────
//  PATH PLACE EXTERNAL PATH API (crowd generator sub-tool)
// ─────────────────────────────────────────────────────────────────────────
// The crowd generator wants to scatter characters along a user-drawn
// polyline. The DLL holds the nodes; we run FABRIK + picking against
// them every frame the user is in draw mode. Identical machinery to
// the cinematic spline editor above, only the node array source and
// the writeback target are different.
//
// args.pathPlacePreviewActive == 1 means draw mode is on. args
// .pathPlaceNodeCount + args.pathPlaceNodes[] are the current
// polyline. On LMB-click in empty space, we append a node. On a node
// hit, we run the same drag flow as the cinematic editor. On RMB-
// click we pop the last node. We write the new node array back into
// args.pathPlaceNodes so the DLL picks up FABRIK mutations next
// frame.
//
// We ALSO compute the sampled spawn positions every frame and write
// the count back to args.pathPlacePreviewCount so the panel UI can
// show "X instances will be stamped". The DrawExternalPathHandles
// function renders the handles + ghost markers at the sampled spawn
// positions during the same DrawHandles pass the cinematic editor
// uses.
void UpdateExternalPath(ImGuiGlueFrameArgs& args,
                        LevelScene* scene,
                        float mouseX, float mouseY,
                        int viewportW, int viewportH,
                        bool mouseDown, bool mouseClicked,
                        bool rightMouseClicked);

// Render the path's control handles + ghost-marker preview at the
// sampled spawn positions. Called once per frame from inside the
// LevelScene spline-draw block, after the cinematic DrawHandles call.
// Reads from a module-scope cache populated by UpdateExternalPath
// earlier this frame, so LevelScene doesn't need access to args.
// No-op when path-place mode is inactive.
void DrawExternalPathHandles(IDirect3DDevice9* dev,
                             LevelScene* scene,
                             const float eyePos[3],
                             const float viewMat[16],
                             const float projMat[16]);

} // namespace SplineEditor

#endif // SPLINE_EDITOR_H
