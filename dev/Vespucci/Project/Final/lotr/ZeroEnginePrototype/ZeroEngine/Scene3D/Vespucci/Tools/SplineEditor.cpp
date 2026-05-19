// SplineEditor.cpp - FABRIK, Handles, Arrows, And A Long Goddamn List Of
//                    Mouse Math That Somebody Had To Write Eventually
// ───────────────────────────────────────────────────────────────────────
// Written by: Eriumsss
//
// This file is the spline editor the codebase should have shipped with
// from day one. The old approach was 400 lines of DragFloat3 per node.
// Pull up a 50-node camera spline, drag fifty separate sliders one
// component at a time, hate your fucking life. This is the rewrite.
//
// Lives in its own translation unit on purpose, because the viewport
// file ZeroEngine3DViewport.cpp is a war zone that multiple agents
// are stomping on at the same time. The cost of every new line added
// to that file is "maybe-merge-conflict" per line, and this editor
// is not worth that tax. Instead: we touch the viewport with exactly
// one include and one function call per frame. Everything below -
// FABRIK, picking, curvature bias, arrow add/remove, drag state
// machine, render - lives HERE. If someone eventually needs to
// extend the spline editor, they come to this file. If they add
// code to the viewport to do it instead, they are wrong and I will
// personally rewrite their commit history. Stay in this file.
//
// ── Fuck EA ── This file exists because the original LOTR Conquest
// cinematic tools got vaporized when EA shut Pandemic Studios down
// in 2009 mid-project. The artists on that team wrote the real
// tooling; we are reconstructing what got torched from the outside
// based on PAK file archaeology. Every DragFloat3 slider we rip out
// is a small apology to the animators who never got the editor
// they deserved.

#include "SplineEditor.h"

#include <d3d9.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <algorithm>

#include "imgui_glue.h"
#include "LevelScene.h"
#include "LevelReader.h"

// ─────────────────────────────────────────────────────────────────────────
//  Internal module state
// ─────────────────────────────────────────────────────────────────────────
// File-scope static. Not threadsafe. Should be invoked only from the
// main (render) thread of the viewport. If you get a concurrency
// failure here you have already done something monstrously stupid.
namespace {

enum DragType {
    DRAG_NONE       = 0,
    DRAG_NODE       = 1,
    DRAG_HEAD_ARROW = 2,
    DRAG_TAIL_ARROW = 3
};

struct EditorState {
    // Which spline we are currently editing, as identified by its GUID.
    // This resets to 0 whenever edit-mode toggles off, the selected
    // camera changes, OR the track (pos vs tgt) changes. Tracking by
    // GUID instead of pointer because the LevelScene::m_splines vector
    // can reallocate on push_back and turn any cached pointer into a
    // dangling fucking trap.
    uint32_t editSplineGuid;

    // Drag state machine. Only one drag active at a time - node OR
    // arrow, never both. Release resets everything to DRAG_NONE.
    int      activeDragType;
    int      activeNodeIdx;       // which node index if DRAG_NODE

    // Drag plane (for projecting 2D mouse motion into 3D world motion).
    // Built at click-start: plane passes through the grabbed handle,
    // normal = camera forward direction. Ray-cast each frame through
    // mouse position, intersect with this plane, that is the handle's
    // new world position. Classic move. Works as long as the user does
    // not try to drag a handle directly toward or away from the camera,
    // in which case the plane is edge-on and the intersection is
    // numerically unstable. Fine for normal use. Advanced users can
    // orbit the camera to a better angle.
    float    dragPlaneNormal[3];
    float    dragPlaneOrigin[3];

    // For arrow drag: axis along which we project mouse motion.
    // Pointing AWAY from the chain (outward from head/tail).
    float    dragArrowAxis[3];
    float    dragArrowBase[3];    // world pos of the endpoint node where arrow attaches
    float    dragArrowSegLen;     // baseline segment length for add/remove threshold

    // Snapshot of segment lengths at drag-start. FABRIK needs these to
    // preserve the chain's original gaps. If we recomputed them every
    // frame from the current (mid-drag) node positions, they would
    // drift and the chain would compress or stretch with each frame.
    std::vector<float> origSegmentLengths;

    // Hover state for drawing the highlight. Recomputed every frame.
    int      hoverType;
    int      hoverNodeIdx;

    // The commit buffer. When we apply a change we populate this and
    // point args.cineNodeEditData at its data pointer. It has to stay
    // alive until the viewport's writeback path reads it, which
    // happens in the SAME frame, so a per-call static is fine. If you
    // ever make the writeback asynchronous, this goes boom quietly.
    std::vector<float> commitBuf;

    // Output accessors for the DLL (numeric list auto-scroll, count sync).
    int      clickedNodeIdx;
    int      lastNodeCountDelta;

    EditorState() {
        editSplineGuid = 0;
        activeDragType = DRAG_NONE;
        activeNodeIdx = -1;
        memset(dragPlaneNormal, 0, sizeof(dragPlaneNormal));
        memset(dragPlaneOrigin, 0, sizeof(dragPlaneOrigin));
        memset(dragArrowAxis, 0, sizeof(dragArrowAxis));
        memset(dragArrowBase, 0, sizeof(dragArrowBase));
        dragArrowSegLen = 1.0f;
        hoverType = DRAG_NONE;
        hoverNodeIdx = -1;
        clickedNodeIdx = -1;
        lastNodeCountDelta = 0;
    }
};

static EditorState g_state;

// ─────────────────────────────────────────────────────────────────────────
//  Math helpers - dot/cross/normalize + screen projection
// ─────────────────────────────────────────────────────────────────────────
// All of this should be in some shared ZeroMath header but every time I
// go looking for where helpers should live I find seven copies of these
// four functions scattered across seven files. Inline-static here. Move
// them to ZeroMath.h when you can stomach the global grep-and-replace.
static inline float Dot3(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static inline void Cross3(const float* a, const float* b, float* out) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}
static inline float Len3(const float* a) {
    return sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
}
static inline void Normalize3(float* a) {
    float L = Len3(a);
    if (L > 1e-9f) { a[0]/=L; a[1]/=L; a[2]/=L; }
}
static inline void Sub3(const float* a, const float* b, float* out) {
    out[0] = a[0]-b[0]; out[1] = a[1]-b[1]; out[2] = a[2]-b[2];
}
static inline void Add3(const float* a, const float* b, float* out) {
    out[0] = a[0]+b[0]; out[1] = a[1]+b[1]; out[2] = a[2]+b[2];
}
static inline void Scale3(const float* a, float s, float* out) {
    out[0] = a[0]*s; out[1] = a[1]*s; out[2] = a[2]*s;
}

// Project world position into screen-space pixel coordinates.
// viewMat, projMat: D3D9 row-major 4x4 matrices.
// Returns (screenX, screenY, wClip). wClip < 0 means behind-camera,
// in which case the screen position is meaningless and picking should
// skip the handle. Standard projection pipeline: world -> view ->
// clip -> NDC -> screen. Nothing mysterious, just a goddamn lot of
// arithmetic for one function because D3DX would do it in one call
// and we do not link D3DX here.
static bool WorldToScreen(const float world[3],
                          const float viewMat[16],
                          const float projMat[16],
                          int vpW, int vpH,
                          float outPx[2], float* outWClip)
{
    // D3D row-major: v' = v * M, so v'_c = sum_r v_r * M[r*4+c]. And a
    // position is [x, y, z, 1]. Apply view then proj.
    float vx = world[0]*viewMat[0] + world[1]*viewMat[4] + world[2]*viewMat[8]  + viewMat[12];
    float vy = world[0]*viewMat[1] + world[1]*viewMat[5] + world[2]*viewMat[9]  + viewMat[13];
    float vz = world[0]*viewMat[2] + world[1]*viewMat[6] + world[2]*viewMat[10] + viewMat[14];
    float vw = world[0]*viewMat[3] + world[1]*viewMat[7] + world[2]*viewMat[11] + viewMat[15];

    float cx = vx*projMat[0] + vy*projMat[4] + vz*projMat[8]  + vw*projMat[12];
    float cy = vx*projMat[1] + vy*projMat[5] + vz*projMat[9]  + vw*projMat[13];
    float cz = vx*projMat[2] + vy*projMat[6] + vz*projMat[10] + vw*projMat[14];
    float cw = vx*projMat[3] + vy*projMat[7] + vz*projMat[11] + vw*projMat[15];

    if (outWClip) *outWClip = cw;
    if (cw < 1e-6f) { outPx[0] = outPx[1] = -99999.0f; return false; }

    float ndcX = cx / cw;
    float ndcY = cy / cw;

    outPx[0] = (ndcX * 0.5f + 0.5f) * (float)vpW;
    outPx[1] = (1.0f - (ndcY * 0.5f + 0.5f)) * (float)vpH; // flip Y for screen coords
    return true;
}

// Intersect a ray with a plane defined by (origin, normal).
// Returns true + fills outHit if the ray crosses the plane in front of
// rayOrigin. False if the ray is parallel (degenerate) or crossing
// behind the origin (t<0).
static bool RayPlaneIntersect(const float rayOrigin[3], const float rayDir[3],
                              const float planeOrigin[3], const float planeNormal[3],
                              float outHit[3])
{
    float denom = Dot3(rayDir, planeNormal);
    if (fabsf(denom) < 1e-6f) return false; // ray parallel to plane
    float diff[3]; Sub3(planeOrigin, rayOrigin, diff);
    float t = Dot3(diff, planeNormal) / denom;
    if (t < 0.0f) return false;
    float scaled[3]; Scale3(rayDir, t, scaled);
    Add3(rayOrigin, scaled, outHit);
    return true;
}

// Screen-space distance squared (cheaper than sqrt for hit-testing).
static inline float ScreenDistSq(const float a[2], float mx, float my) {
    float dx = a[0] - mx, dy = a[1] - my;
    return dx*dx + dy*dy;
}

// Picking threshold in pixels. 14px is generous enough to be forgiving
// at distance but tight enough that adjacent nodes on a dense spline do
// not eat each other. Square it once, use squared distance everywhere.
static const float kPickPixelRadius   = 14.0f;
static const float kPickPixelRadiusSq = kPickPixelRadius * kPickPixelRadius;

// ═════════════════════════════════════════════════════════════════════════
//  THE GODDAMN FABRIK SOLVER - Forward And Backward Reaching IK
// ═════════════════════════════════════════════════════════════════════════
//
// This is the motherfucking algorithm that turns a broken "drag one node
// and the whole spline stays put like a cursed static diagram" UX into
// "drag one node and the entire chain flows behind it like a real rope."
// FABRIK is the standard for fixed-length chain IK - every animation rig
// built since roughly 2011 uses it because spring sims are slower, less
// stable, and produce non-deterministic results that make animators
// want to punch their own monitors.
//
// pinnedIdx: the node the user is DRAGGING. Stays pinned. Every other
//            node slides to maintain the original segment lengths.
// origLens:  snapshot of inter-node distances at drag start. If we
//            recomputed these live, they would drift every frame and
//            the chain would slowly stretch or compress into nothing
//            like somebody forgot to lock the timeline. Snapshot.
// N:         total count.
//
// Algorithm:
//   1. Forward pass: from pinnedIdx+1 to the end, each node slides
//      along the direction FROM its predecessor, at the fixed segment
//      length. Walks toward the tail.
//   2. Backward pass: from pinnedIdx-1 back to zero, each node slides
//      along the direction FROM its successor, at the fixed segment
//      length. Walks toward the head.
//
// The pinned node never fucking moves during either pass. That is the
// whole point. The rest of the chain gets yanked along like a rigid-
// segment rope. Way cheaper than any spring sim, 100% deterministic,
// same input same output every goddamn time. One iteration is plenty
// for single-node pin, five is overkill.
static void SolveFABRIK(std::vector<LevelSpline::Node>& nodes,
                        int pinnedIdx,
                        const std::vector<float>& origLens,
                        int iterations)
{
    int N = (int)nodes.size();
    if (N < 2 || pinnedIdx < 0 || pinnedIdx >= N) return;
    if ((int)origLens.size() < N - 1) return;

    for (int iter = 0; iter < iterations; ++iter) {
        // Forward: pinned toward tail
        for (int i = pinnedIdx + 1; i < N; ++i) {
            float prev[3] = { nodes[i-1].x, nodes[i-1].y, nodes[i-1].z };
            float cur[3]  = { nodes[i].x,   nodes[i].y,   nodes[i].z };
            float dir[3];  Sub3(cur, prev, dir);
            float L = Len3(dir);
            if (L < 1e-9f) {
                // Degenerate: node sitting on its predecessor. Break the
                // tie by picking +X. Happens when a node is dragged
                // exactly onto its neighbor, which is stupid user
                // behavior but we do not crash over it.
                dir[0] = 1.0f; dir[1] = 0.0f; dir[2] = 0.0f;
            } else {
                dir[0]/=L; dir[1]/=L; dir[2]/=L;
            }
            nodes[i].x = prev[0] + dir[0] * origLens[i-1];
            nodes[i].y = prev[1] + dir[1] * origLens[i-1];
            nodes[i].z = prev[2] + dir[2] * origLens[i-1];
        }
        // Backward: pinned toward head
        for (int i = pinnedIdx - 1; i >= 0; --i) {
            float nxt[3] = { nodes[i+1].x, nodes[i+1].y, nodes[i+1].z };
            float cur[3] = { nodes[i].x,   nodes[i].y,   nodes[i].z };
            float dir[3]; Sub3(cur, nxt, dir);
            float L = Len3(dir);
            if (L < 1e-9f) { dir[0] = -1.0f; dir[1] = 0.0f; dir[2] = 0.0f; }
            else           { dir[0]/=L; dir[1]/=L; dir[2]/=L; }
            nodes[i].x = nxt[0] + dir[0] * origLens[i];
            nodes[i].y = nxt[1] + dir[1] * origLens[i];
            nodes[i].z = nxt[2] + dir[2] * origLens[i];
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
//  CURVATURE BIAS - bend the goddamn chain into crescent / S / snake shapes
// ═════════════════════════════════════════════════════════════════════════
//
// After FABRIK lays the chain out as a straight-line-preserving-segment
// solution, we rip into it AGAIN and shove the intermediate nodes sideways
// to bend the whole fucking thing into the shape the user actually wants.
// Then we run FABRIK one more time to snap segment lengths back to
// whatever the snapshot said. Double-pass IK with a perpendicular bias
// in between. Deterministic, fast, works on any chain length.
//
// amount ∈ [-1, +1]: how hard the bend. 0 = no bend (straight FABRIK),
//                   ±1 = max bend (~25% of chord length sideways).
// freq   ∈ [1, 5]:   how many full bends across the chain.
//                   1 = crescent arc (single hump).
//                   2 = S-curve (one peak, one trough).
//                   3+ = full-on serpentine motherfucking snake.
//
// ONLY intermediate nodes move. Head and tail stay fucking planted,
// always, no exceptions. The bend is a sine wave of normalized
// position along the chain, amplitude scaled by chord length * amount,
// direction perpendicular to the chord in the view-up plane. Simple
// math, visually satisfying result.
static void ApplyCurvatureBias(std::vector<LevelSpline::Node>& nodes,
                               float amount, int freq,
                               const float upHint[3])
{
    int N = (int)nodes.size();
    if (N < 3) return;
    if (fabsf(amount) < 1e-4f) return;
    if (freq < 1) freq = 1;
    if (freq > 5) freq = 5;

    // Chord = straight line from head to tail.
    float chord[3];
    chord[0] = nodes[N-1].x - nodes[0].x;
    chord[1] = nodes[N-1].y - nodes[0].y;
    chord[2] = nodes[N-1].z - nodes[0].z;
    float chordLen = Len3(chord);
    if (chordLen < 1e-6f) return; // head and tail on top of each other, nothing to bend

    // Perpendicular axis: cross(chord, up). Normalize. If up is
    // parallel to chord (rare) fall back to world-X.
    float perp[3];
    Cross3(chord, upHint, perp);
    float perpLen = Len3(perp);
    if (perpLen < 1e-6f) {
        perp[0] = 1.0f; perp[1] = 0.0f; perp[2] = 0.0f;
    } else {
        perp[0]/=perpLen; perp[1]/=perpLen; perp[2]/=perpLen;
    }

    float amplitude = chordLen * 0.25f * amount;
    static const float PI = 3.14159265358979323846f;

    for (int i = 1; i < N - 1; ++i) {
        float t = (float)i / (float)(N - 1);  // 0..1
        float phase = PI * (float)freq * t;
        float offset = amplitude * sinf(phase);
        nodes[i].x += perp[0] * offset;
        nodes[i].y += perp[1] * offset;
        nodes[i].z += perp[2] * offset;
    }
}

// Recompute cumulative arc-length (s field) along the chain after any
// mutation. Head gets s=0; each subsequent node's s is previous + segment
// distance. This field is what the cinematic playback code reads to
// interpolate position vs time, so it has to stay consistent with the
// actual node positions or camera animations go sideways.
static void RecomputeArcLengths(std::vector<LevelSpline::Node>& nodes) {
    if (nodes.empty()) return;
    nodes[0].s = 0.0f;
    for (size_t i = 1; i < nodes.size(); ++i) {
        float dx = nodes[i].x - nodes[i-1].x;
        float dy = nodes[i].y - nodes[i-1].y;
        float dz = nodes[i].z - nodes[i-1].z;
        nodes[i].s = nodes[i-1].s + sqrtf(dx*dx + dy*dy + dz*dz);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Spline resolution - find the LevelSpline matching the current edit mode
// ─────────────────────────────────────────────────────────────────────────
// Returns pointer to the spline the user is currently editing, based on
// args.cineSplineEditMode (1=pos track, 2=tgt track) and the selected
// camera's track GUIDs. Phase 6 grafted on a SECOND entry: if the
// general spline-handles mode (splineHandlesEnabled) is on, we resolve
// against splineHandlesFocusGuid instead. The cinematic editor still
// wins if both modes happen to be live at the same time, because the
// cinematic flow is the one with FieldEdit-grade write paths already
// proven, and we are NOT entertaining a cross-mode shootout.
// NULL means "stand the fuck down, no spline to edit".
static LevelSpline* ResolveEditSpline(const ImGuiGlueFrameArgs& args,
                                                   LevelScene* scene)
{
    if (!scene) return NULL;
    uint32_t wantGuid = 0;
    if (args.cineSplineEditMode == 1)        wantGuid = args.cineSelPosTrackGuid;
    else if (args.cineSplineEditMode == 2)   wantGuid = args.cineSelTgtTrackGuid;
    else if (args.splineHandlesEnabled != 0) wantGuid = args.splineHandlesFocusGuid;
    if (wantGuid == 0) return NULL;

    std::vector<LevelSpline>& splines = scene->getSplinesMut();
    for (size_t i = 0; i < splines.size(); ++i) {
        if (splines[i].guid == wantGuid) return &splines[i];
    }
    return NULL;
}

// Phase 6 helper: closest point on a 3D segment (A,B) to a ray
// (rayO, rayD). Returns t in [0,1] along the segment of the closest
// point AND the squared world-space distance between segment-point
// and ray-point. Both used by shift-click segment insertion and by
// hover-highlight of which segment the new node would land on.
static void ClosestSegmentRay(const float A[3], const float B[3],
                              const float rayO[3], const float rayD[3],
                              float& outT, float& outDistSq)
{
    float seg[3] = { B[0]-A[0], B[1]-A[1], B[2]-A[2] };
    float w[3]   = { A[0]-rayO[0], A[1]-rayO[1], A[2]-rayO[2] };
    float a = seg[0]*seg[0] + seg[1]*seg[1] + seg[2]*seg[2];
    float b = seg[0]*rayD[0] + seg[1]*rayD[1] + seg[2]*rayD[2];
    float c = rayD[0]*rayD[0] + rayD[1]*rayD[1] + rayD[2]*rayD[2];
    float d = seg[0]*w[0] + seg[1]*w[1] + seg[2]*w[2];
    float e = rayD[0]*w[0] + rayD[1]*w[1] + rayD[2]*w[2];
    float denom = a*c - b*b;
    if (denom < 1e-6f) { outT = 0.0f; outDistSq = 1e30f; return; }
    float sParam = (b*e - c*d) / denom;
    if (sParam < 0.0f) sParam = 0.0f;
    else if (sParam > 1.0f) sParam = 1.0f;
    float tParam = (a*e - b*d) / denom;
    float pSeg[3] = { A[0]+seg[0]*sParam, A[1]+seg[1]*sParam, A[2]+seg[2]*sParam };
    float pRay[3] = { rayO[0]+rayD[0]*tParam, rayO[1]+rayD[1]*tParam, rayO[2]+rayD[2]*tParam };
    float dx = pSeg[0]-pRay[0], dy = pSeg[1]-pRay[1], dz = pSeg[2]-pRay[2];
    outT = sParam;
    outDistSq = dx*dx + dy*dy + dz*dz;
}

// Find/alloc the arrow tangent direction for an endpoint. For the HEAD
// arrow: points OUT from the chain, i.e. opposite of (nodes[1] - nodes[0]).
// For the TAIL arrow: points OUT = (nodes[N-1] - nodes[N-2]) direction.
// If the chain has only one node this is undefined; we return +X as a
// last resort because zero vector breaks everything downstream.
static void ComputeArrowAxis(const LevelSpline& spline, bool head, float out[3]) {
    int N = (int)spline.nodes.size();
    if (N < 2) { out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f; return; }
    if (head) {
        float d[3];
        d[0] = spline.nodes[0].x - spline.nodes[1].x;
        d[1] = spline.nodes[0].y - spline.nodes[1].y;
        d[2] = spline.nodes[0].z - spline.nodes[1].z;
        float L = Len3(d);
        if (L < 1e-6f) { out[0] = -1.0f; out[1] = 0.0f; out[2] = 0.0f; }
        else { out[0] = d[0]/L; out[1] = d[1]/L; out[2] = d[2]/L; }
    } else {
        float d[3];
        d[0] = spline.nodes[N-1].x - spline.nodes[N-2].x;
        d[1] = spline.nodes[N-1].y - spline.nodes[N-2].y;
        d[2] = spline.nodes[N-1].z - spline.nodes[N-2].z;
        float L = Len3(d);
        if (L < 1e-6f) { out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f; }
        else { out[0] = d[0]/L; out[1] = d[1]/L; out[2] = d[2]/L; }
    }
}

// Length of the arrow glyph in world units - scaled to the spline's
// typical segment length so arrows look proportional on tiny chains AND
// on chains the size of Helm's Deep.
static float ComputeArrowLength(const LevelSpline& spline) {
    int N = (int)spline.nodes.size();
    if (N < 2) return 1.0f;
    // Use the LAST segment as the length reference. It is usually a good
    // proxy for "typical segment size" and it is the segment the arrow
    // is attached to anyway.
    float dx, dy, dz;
    if (N >= 2) {
        dx = spline.nodes[N-1].x - spline.nodes[N-2].x;
        dy = spline.nodes[N-1].y - spline.nodes[N-2].y;
        dz = spline.nodes[N-1].z - spline.nodes[N-2].z;
    } else { dx = 1.0f; dy = 0.0f; dz = 0.0f; }
    float L = sqrtf(dx*dx + dy*dy + dz*dz);
    if (L < 0.1f) L = 0.1f;
    return L * 1.2f;  // slightly longer than segment so it reads as "more"
}

// Snapshot every segment length in the current spline. Called on
// drag-start so FABRIK has the reference to preserve.
static void SnapshotSegmentLengths(const LevelSpline& spline,
                                    std::vector<float>& out)
{
    int N = (int)spline.nodes.size();
    out.clear();
    if (N < 2) return;
    out.resize(N - 1);
    for (int i = 0; i < N - 1; ++i) {
        float dx = spline.nodes[i+1].x - spline.nodes[i].x;
        float dy = spline.nodes[i+1].y - spline.nodes[i].y;
        float dz = spline.nodes[i+1].z - spline.nodes[i].z;
        out[i] = sqrtf(dx*dx + dy*dy + dz*dz);
    }
}

// Populate args.cineNodeEdit* so the viewport's existing writeback
// block applies our mutation via AddFieldEdit (kind=7, stride=4).
// The spline's nodes have been modified in place already by the time
// we call this; we just hand a pointer to our own persistent buffer.
static void CommitEdit(ImGuiGlueFrameArgs& args,
                       const LevelSpline& spline)
{
    g_state.commitBuf.clear();
    g_state.commitBuf.reserve(spline.nodes.size() * 4);
    for (size_t i = 0; i < spline.nodes.size(); ++i) {
        g_state.commitBuf.push_back(spline.nodes[i].x);
        g_state.commitBuf.push_back(spline.nodes[i].y);
        g_state.commitBuf.push_back(spline.nodes[i].z);
        g_state.commitBuf.push_back(spline.nodes[i].s);
    }
    args.cineNodeEditRequested = 1;
    args.cineNodeEditGuid      = spline.guid;
    args.cineNodeEditCount     = (int)spline.nodes.size();
    args.cineNodeEditData      = g_state.commitBuf.empty() ? NULL : &g_state.commitBuf[0];
}

// ─────────────────────────────────────────────────────────────────────────
//  Picking - test mouse against handle spheres and extend arrows
// ─────────────────────────────────────────────────────────────────────────
// Screen-space distance test. Project each candidate world position
// through view*proj, compare to mouse, pick closest within threshold.
// Also honors the screen-space tip of each arrow so the user can grab
// the arrow glyph and not just the endpoint it is attached to.
//
// outHitType / outHitIdx will be filled with the best match; DRAG_NONE
// if nothing within threshold.
static void DoPick(const LevelSpline& spline,
                   const float viewMat[16], const float projMat[16],
                   int vpW, int vpH,
                   float mouseX, float mouseY,
                   int& outHitType, int& outHitIdx)
{
    outHitType = DRAG_NONE;
    outHitIdx  = -1;
    float bestDistSq = kPickPixelRadiusSq;

    int N = (int)spline.nodes.size();
    if (N == 0) return;

    // Test every node. Head and tail are special but still compete for
    // pick. Head always wins ties for intermediate picks because we
    // iterate in order and "<" comparison keeps the first match at
    // equal distance. Fine for UX.
    for (int i = 0; i < N; ++i) {
        float w[3] = { spline.nodes[i].x, spline.nodes[i].y, spline.nodes[i].z };
        float px[2]; float wClip;
        if (!WorldToScreen(w, viewMat, projMat, vpW, vpH, px, &wClip)) continue;
        if (wClip <= 0.0f) continue; // behind camera
        float d2 = ScreenDistSq(px, mouseX, mouseY);
        if (d2 < bestDistSq) {
            bestDistSq = d2;
            outHitType = DRAG_NODE;
            outHitIdx = i;
        }
    }

    // Test arrow tips (head and tail). Arrow tip world pos = endpoint +
    // axis * arrowLen. If edit mode is active and there are enough nodes
    // to have a tangent, arrows are hit-testable.
    if (N >= 2) {
        float arrowLen = ComputeArrowLength(spline);
        // Head arrow
        {
            float axis[3]; ComputeArrowAxis(spline, true, axis);
            float tipW[3];
            tipW[0] = spline.nodes[0].x + axis[0] * arrowLen;
            tipW[1] = spline.nodes[0].y + axis[1] * arrowLen;
            tipW[2] = spline.nodes[0].z + axis[2] * arrowLen;
            float tipPx[2]; float wClip;
            if (WorldToScreen(tipW, viewMat, projMat, vpW, vpH, tipPx, &wClip) && wClip > 0) {
                float d2 = ScreenDistSq(tipPx, mouseX, mouseY);
                if (d2 < bestDistSq) {
                    bestDistSq = d2;
                    outHitType = DRAG_HEAD_ARROW;
                    outHitIdx = 0;
                }
            }
        }
        // Tail arrow
        {
            float axis[3]; ComputeArrowAxis(spline, false, axis);
            float tipW[3];
            tipW[0] = spline.nodes[N-1].x + axis[0] * arrowLen;
            tipW[1] = spline.nodes[N-1].y + axis[1] * arrowLen;
            tipW[2] = spline.nodes[N-1].z + axis[2] * arrowLen;
            float tipPx[2]; float wClip;
            if (WorldToScreen(tipW, viewMat, projMat, vpW, vpH, tipPx, &wClip) && wClip > 0) {
                float d2 = ScreenDistSq(tipPx, mouseX, mouseY);
                if (d2 < bestDistSq) {
                    bestDistSq = d2;
                    outHitType = DRAG_TAIL_ARROW;
                    outHitIdx = N - 1;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Ray-based drag update - compute the new world-space position from mouse
// ─────────────────────────────────────────────────────────────────────────
// For node drag: intersect the current mouse ray with the drag plane
// snapshotted at drag-start. Returns the intersection in outWorldPos.
// If the ray misses (plane is edge-on, bad camera angle), keep the old
// position - better than a glitch jump.
static bool GetMouseWorldOnDragPlane(LevelScene* scene,
                                      float mouseX, float mouseY,
                                      int vpW, int vpH,
                                      float outWorld[3])
{
    float origin[3], dir[3];
    scene->screenToRay((int)mouseX, (int)mouseY, vpW, vpH, origin, dir);
    return RayPlaneIntersect(origin, dir,
                             g_state.dragPlaneOrigin,
                             g_state.dragPlaneNormal,
                             outWorld);
}

// For arrow drag: project the mouse world position onto the arrow axis
// to get a signed parameter t along the axis. Outward = positive.
static float GetArrowDragT(LevelScene* scene,
                            float mouseX, float mouseY,
                            int vpW, int vpH)
{
    float world[3];
    if (!GetMouseWorldOnDragPlane(scene, mouseX, mouseY, vpW, vpH, world))
        return 0.0f;
    float delta[3];
    Sub3(world, g_state.dragArrowBase, delta);
    return Dot3(delta, g_state.dragArrowAxis);
}

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═════════════════════════════════════════════════════════════════════════

namespace SplineEditor {

int GetClickedNodeIndex()   { return g_state.clickedNodeIdx; }
int GetLastNodeCountDelta() { return g_state.lastNodeCountDelta; }

// Reset per-frame outputs at the top of every Update.
static void ResetFrameOutputs() {
    g_state.clickedNodeIdx = -1;
    g_state.lastNodeCountDelta = 0;
}

// ─────────────────────────────────────────────────────────────────────────
//  Update - called once per frame from the viewport main loop
// ─────────────────────────────────────────────────────────────────────────
void Update(ImGuiGlueFrameArgs& args,
            LevelScene* scene,
            ZeroEngine::LevelReader* /*reader*/,
            float mouseX, float mouseY,
            int viewportW, int viewportH,
            bool mouseDown, bool mouseClicked)
{
    ResetFrameOutputs();

    // Clear hover state every frame (recomputed below when applicable).
    g_state.hoverType = DRAG_NONE;
    g_state.hoverNodeIdx = -1;

    // Early out: edit mode off, or no scene. Cancel any in-flight
    // drag too. Leaving edit mode mid-drag would leave the state
    // machine pointing at a spline pointer that may not be valid
    // next frame, and that is the kind of motherfucking dangling
    // pointer that crashes the editor at the WORST possible time
    // (right after the user spent twenty minutes shaping a curve).
    bool anyEditMode = (args.cineSplineEditMode != 0) ||
                       (args.splineHandlesEnabled != 0);
    if (!anyEditMode || scene == NULL) {
        g_state.activeDragType = DRAG_NONE;
        g_state.editSplineGuid = 0;
        return;
    }

    LevelSpline* spline = ResolveEditSpline(args, scene);
    if (spline == NULL) {
        g_state.activeDragType = DRAG_NONE;
        g_state.editSplineGuid = 0;
        return;
    }

    // If the user swapped to a different spline (different camera /
    // different track) while dragging the old one, bail out of the drag
    // silently. Committing half a drag to a spline the user no longer
    // has selected would be surprise-mutation, that is a bug class I
    // am not inviting into this file.
    if (g_state.activeDragType != DRAG_NONE && g_state.editSplineGuid != spline->guid) {
        g_state.activeDragType = DRAG_NONE;
    }
    g_state.editSplineGuid = spline->guid;

    // Build view and projection matrices from the SCENE camera. We need
    // these for screen projection (picking) and for reading the current
    // camera's forward axis (drag plane normal). LevelScene does not
    // expose view/proj directly but we can get the camera via the
    // screenToRay helper... which takes SCREEN coords and returns a
    // RAY. We use that for ray casting but need an explicit proj matrix
    // for screen-space projection during picking. Fortunately D3D9
    // tracks them on the device via GetTransform.
    //
    // We get them via the device state. The viewport sets them up every
    // frame during the main scene render, so by the time Update() is
    // called (after DrawFrame), they are the current scene transforms.
    // If that ever changes, this WILL silently break.
    IDirect3DDevice9* dev = NULL;
    // No direct way to get the device from here without threading a
    // pointer through. Use D3DXmatrix math on the camera instead - we
    // project via screenToRay for picking (does the right thing
    // internally) and we build our drag plane normal from the two
    // endpoints of a ray through the center of the viewport.
    (void)dev;

    // Drag-plane normal = camera forward direction, computed from a
    // ray cast through the viewport center. This avoids needing direct
    // access to the view matrix.
    float centerOrigin[3], centerDir[3];
    scene->screenToRay(viewportW / 2, viewportH / 2, viewportW, viewportH,
                       centerOrigin, centerDir);

    // Build view/proj matrices from the camera by casting two auxiliary
    // rays through known screen coords. Actually, we do not need the
    // matrices directly - we can do screen-space picking differently.
    // Shortcut: for picking, instead of manually projecting each node,
    // we use an approximate pixel radius in world space based on depth.
    // For each node:
    //   dist = length(node - cameraPos)
    //   world_radius = dist * pixelRadius / viewportH * fovFactor
    // Rough but works.
    //
    // NO - this is too hacky. Let me do it right: grab the view and proj
    // directly from the D3D device. We need a device pointer. Get one
    // via the scene... it holds m_device internally but does not expose
    // it. The simplest path is a second parameter to Update that
    // supplies the device. Since we control the viewport call site,
    // that is fine. See the header: Update signature stays the same
    // because the scene pointer gives us access to screenToRay, and
    // we will switch to ray-based picking that does NOT need matrices.

    // NEW APPROACH: ray-based picking. For each handle, construct a ray
    // from the camera through that handle's screen projection by using
    // a small helper: cast a ray through mouse, and test which handle
    // the ray passes closest to in world space. Distance-to-ray is
    // cheap and does not need projection matrices.
    float mouseRayO[3], mouseRayD[3];
    scene->screenToRay((int)mouseX, (int)mouseY, viewportW, viewportH,
                       mouseRayO, mouseRayD);

    // Pick by ray-to-point distance. Threshold scales with world-space
    // distance so handles stay ~14px wide regardless of zoom.
    int   hitType = DRAG_NONE;
    int   hitIdx  = -1;
    float bestProxy = 1e30f;

    int N = (int)spline->nodes.size();
    for (int i = 0; i < N; ++i) {
        float p[3] = { spline->nodes[i].x, spline->nodes[i].y, spline->nodes[i].z };
        // Vector from ray origin to point.
        float op[3]; Sub3(p, mouseRayO, op);
        // Project onto ray direction.
        float t = Dot3(op, mouseRayD);
        if (t < 0.0f) continue; // behind camera
        // Perpendicular component = sqrt(|op|^2 - t^2).
        float opLen2 = Dot3(op, op);
        float perpLen2 = opLen2 - t*t;
        if (perpLen2 < 0.0f) perpLen2 = 0.0f;
        // Threshold scales with t (distance): closer = tighter, farther
        // = more forgiving. Calibrated so that at t=10m threshold is
        // ~0.3m, at t=100m ~3m. Feels like ~14px in screen space.
        float radius = t * 0.03f;
        if (radius < 0.1f) radius = 0.1f;
        if (perpLen2 < radius * radius) {
            // Closest in terms of how centered the ray passes.
            if (perpLen2 < bestProxy) {
                bestProxy = perpLen2;
                hitType = DRAG_NODE;
                hitIdx = i;
            }
        }
    }

    // Test arrow tips with the same ray method.
    if (N >= 2) {
        float arrowLen = ComputeArrowLength(*spline);
        for (int side = 0; side < 2; ++side) {
            bool head = (side == 0);
            float axis[3]; ComputeArrowAxis(*spline, head, axis);
            float base[3] = { head ? spline->nodes[0].x : spline->nodes[N-1].x,
                              head ? spline->nodes[0].y : spline->nodes[N-1].y,
                              head ? spline->nodes[0].z : spline->nodes[N-1].z };
            float tip[3] = { base[0] + axis[0]*arrowLen,
                             base[1] + axis[1]*arrowLen,
                             base[2] + axis[2]*arrowLen };
            float op[3]; Sub3(tip, mouseRayO, op);
            float t = Dot3(op, mouseRayD);
            if (t < 0.0f) continue;
            float opLen2 = Dot3(op, op);
            float perpLen2 = opLen2 - t*t;
            if (perpLen2 < 0.0f) perpLen2 = 0.0f;
            float radius = t * 0.04f;
            if (radius < 0.15f) radius = 0.15f;
            if (perpLen2 < radius * radius) {
                if (perpLen2 < bestProxy) {
                    bestProxy = perpLen2;
                    hitType = head ? DRAG_HEAD_ARROW : DRAG_TAIL_ARROW;
                    hitIdx = head ? 0 : (N - 1);
                }
            }
        }
    }

    g_state.hoverType = hitType;
    g_state.hoverNodeIdx = hitIdx;

    // ── Phase 6: Shift+click segment insertion ─────────────────────────
    //
    // Hold Shift, click on a segment between two nodes, BANG, new node
    // lands on the segment at the point closest to the click ray. The
    // user can then drag it like any other node.
    //
    // We only fire when:
    //   - General-spline mode is on (cinematic flow has its own
    //     head/tail arrows for adding nodes, we are not stepping on
    //     that flow)
    //   - Shift is held
    //   - Mouse JUST clicked (not held mid-drag)
    //   - Click did NOT hit an existing handle (no hijacking node-drag)
    //   - We have at least 2 nodes (need a segment to bisect)
    //
    // Pick the segment with the smallest squared distance to the ray.
    // Threshold scales with node spacing so tiny chains and giant ones
    // are both grabbable. Insert at the 0..1 t-value found by
    // ClosestSegmentRay so the new node lands exactly where the user
    // pointed.
    if (args.splineHandlesEnabled != 0 &&
        args.splineHandleShiftHeld != 0 &&
        mouseClicked &&
        g_state.activeDragType == DRAG_NONE &&
        hitType == DRAG_NONE &&
        N >= 2)
    {
        int   bestSeg = -1;
        float bestSegT = 0.0f;
        float bestSegDistSq = 1e30f;
        for (int i = 0; i < N - 1; ++i) {
            float A[3] = { spline->nodes[i  ].x, spline->nodes[i  ].y, spline->nodes[i  ].z };
            float B[3] = { spline->nodes[i+1].x, spline->nodes[i+1].y, spline->nodes[i+1].z };
            // Threshold: half the segment length, to keep clicks landing
            // on the FAR side of the spline from selecting random
            // segments. Safety floor for tiny segments.
            float dx = B[0]-A[0], dy = B[1]-A[1], dz = B[2]-A[2];
            float segLen = sqrtf(dx*dx + dy*dy + dz*dz);
            float thresh = segLen * 0.5f;
            if (thresh < 1.0f) thresh = 1.0f;
            float threshSq = thresh * thresh;
            float t, dsq;
            ClosestSegmentRay(A, B, mouseRayO, mouseRayD, t, dsq);
            if (dsq < threshSq && dsq < bestSegDistSq) {
                bestSegDistSq = dsq;
                bestSegT = t;
                bestSeg = i;
            }
        }
        if (bestSeg >= 0) {
            // Bake the insertion. The new node lands on the segment at
            // bestSegT in [0,1]. Re-run arc-length recompute so the
            // s-values stay consistent for whatever consumer reads them.
            LevelSpline::Node ins;
            ins.x = spline->nodes[bestSeg  ].x +
                    (spline->nodes[bestSeg+1].x - spline->nodes[bestSeg].x) * bestSegT;
            ins.y = spline->nodes[bestSeg  ].y +
                    (spline->nodes[bestSeg+1].y - spline->nodes[bestSeg].y) * bestSegT;
            ins.z = spline->nodes[bestSeg  ].z +
                    (spline->nodes[bestSeg+1].z - spline->nodes[bestSeg].z) * bestSegT;
            ins.s = 0.0f;
            spline->nodes.insert(spline->nodes.begin() + (bestSeg + 1), ins);
            RecomputeArcLengths(spline->nodes);
            CommitEdit(args, *spline);
            g_state.lastNodeCountDelta = +1;
            g_state.clickedNodeIdx = bestSeg + 1;
            // Done for this frame. The new node will be pickable next
            // frame; the user can shift-release and immediately drag
            // it normally.
            return;
        }
    }

    // ── Phase 6: Delete pressed → drop selected intermediate node ─────
    //
    // Delete key removes the most-recently-clicked node, but ONLY if
    // it's an interior point. Head and tail are ARROW territory in
    // the cinematic flow; pulling them via the Delete key would
    // confuse the user and bypass the FABRIK chain-shortening that
    // the arrow drag handles cleanly. Refuse with a no-op when the
    // selection is at either end. Floor at 2 nodes total, a
    // "spline" of one point is a fucking dot, not a curve.
    if (args.splineHandlesEnabled != 0 &&
        args.splineHandleDeletePressed != 0 &&
        g_state.activeDragType == DRAG_NONE)
    {
        int idx = g_state.clickedNodeIdx;
        if (idx > 0 && idx < N - 1 && N > 2) {
            spline->nodes.erase(spline->nodes.begin() + idx);
            RecomputeArcLengths(spline->nodes);
            CommitEdit(args, *spline);
            g_state.lastNodeCountDelta = -1;
            g_state.clickedNodeIdx = -1;
            return;
        }
    }

    // ── Drag state machine ─────────────────────────────────────────────
    if (g_state.activeDragType == DRAG_NONE) {
        if (mouseClicked && hitType != DRAG_NONE) {
            // Start a drag on the hovered handle/arrow.
            g_state.activeDragType = hitType;
            g_state.activeNodeIdx  = hitIdx;

            // Snapshot segment lengths now so FABRIK has the baseline.
            SnapshotSegmentLengths(*spline, g_state.origSegmentLengths);

            // Click-to-focus: emit the clicked node index so the DLL's
            // numeric list can auto-scroll.
            if (hitType == DRAG_NODE) {
                g_state.clickedNodeIdx = hitIdx;
            }

            // Build drag plane: passes through the handle, normal = inverse
            // of the ray through the handle (so the plane is view-aligned).
            // We approximate "view-aligned" as "perpendicular to the mouse
            // ray direction at the moment of click", which is identical
            // for orthographic but slightly off for perspective. Close
            // enough for a dragging UX.
            float anchor[3];
            if (hitType == DRAG_NODE) {
                anchor[0] = spline->nodes[hitIdx].x;
                anchor[1] = spline->nodes[hitIdx].y;
                anchor[2] = spline->nodes[hitIdx].z;
            } else {
                int endIdx = (hitType == DRAG_HEAD_ARROW) ? 0 : (N - 1);
                anchor[0] = spline->nodes[endIdx].x;
                anchor[1] = spline->nodes[endIdx].y;
                anchor[2] = spline->nodes[endIdx].z;
            }
            g_state.dragPlaneOrigin[0] = anchor[0];
            g_state.dragPlaneOrigin[1] = anchor[1];
            g_state.dragPlaneOrigin[2] = anchor[2];
            g_state.dragPlaneNormal[0] = -mouseRayD[0];
            g_state.dragPlaneNormal[1] = -mouseRayD[1];
            g_state.dragPlaneNormal[2] = -mouseRayD[2];
            Normalize3(g_state.dragPlaneNormal);

            // Arrow-specific: record the axis and base.
            if (hitType == DRAG_HEAD_ARROW || hitType == DRAG_TAIL_ARROW) {
                ComputeArrowAxis(*spline, hitType == DRAG_HEAD_ARROW,
                                 g_state.dragArrowAxis);
                g_state.dragArrowBase[0] = anchor[0];
                g_state.dragArrowBase[1] = anchor[1];
                g_state.dragArrowBase[2] = anchor[2];
                g_state.dragArrowSegLen = (N >= 2) ? ComputeArrowLength(*spline) / 1.2f : 1.0f;
            }
        }
    } else {
        // Drag in progress. Update the spline based on current mouse.
        if (!mouseDown) {
            // Mouse released: commit (if anything changed) and end drag.
            // The LevelScene spline was updated in place during the drag
            // frames; we just do the FieldEdit writeback here so save
            // persistence sees the final state.
            CommitEdit(args, *spline);
            g_state.activeDragType = DRAG_NONE;
        } else {
            // Active drag - apply the motion.
            if (g_state.activeDragType == DRAG_NODE) {
                float world[3];
                if (GetMouseWorldOnDragPlane(scene, mouseX, mouseY, viewportW, viewportH, world)) {
                    int idx = g_state.activeNodeIdx;
                    if (idx >= 0 && idx < (int)spline->nodes.size()) {
                        spline->nodes[idx].x = world[0];
                        spline->nodes[idx].y = world[1];
                        spline->nodes[idx].z = world[2];
                        // FABRIK to redistribute the rest of the chain.
                        int iters = 3; // 3 iterations is plenty for single-node pin
                        SolveFABRIK(spline->nodes, idx, g_state.origSegmentLengths, iters);
                        // Curvature bias (if user set amount != 0).
                        if (fabsf(args.cineCurvatureAmount) > 1e-3f) {
                            float up[3] = { 0.0f, 1.0f, 0.0f };
                            ApplyCurvatureBias(spline->nodes,
                                               args.cineCurvatureAmount,
                                               args.cineCurvatureFreq > 0 ? args.cineCurvatureFreq : 1,
                                               up);
                            // Re-run FABRIK once to snap lengths back after bias.
                            SolveFABRIK(spline->nodes, idx, g_state.origSegmentLengths, 2);
                        }
                        RecomputeArcLengths(spline->nodes);
                    }
                }
            } else if (g_state.activeDragType == DRAG_HEAD_ARROW ||
                       g_state.activeDragType == DRAG_TAIL_ARROW)
            {
                // Arrow drag: measure distance along axis from drag start.
                // Every segLen of outward drag = spawn one new node.
                // Every segLen of inward drag (past end node) = remove one.
                float t = GetArrowDragT(scene, mouseX, mouseY, viewportW, viewportH);
                float segLen = g_state.dragArrowSegLen;
                if (segLen < 0.01f) segLen = 0.01f;

                bool isHead = (g_state.activeDragType == DRAG_HEAD_ARROW);

                // ADD case: t exceeds segLen. Spawn a new node at
                // base + axis * segLen, then advance the base and keep
                // going for multi-spawn in one drag.
                while (t >= segLen) {
                    LevelSpline::Node nn;
                    nn.x = g_state.dragArrowBase[0] + g_state.dragArrowAxis[0] * segLen;
                    nn.y = g_state.dragArrowBase[1] + g_state.dragArrowAxis[1] * segLen;
                    nn.z = g_state.dragArrowBase[2] + g_state.dragArrowAxis[2] * segLen;
                    nn.s = 0.0f;
                    if (isHead) {
                        spline->nodes.insert(spline->nodes.begin(), nn);
                    } else {
                        spline->nodes.push_back(nn);
                    }
                    g_state.lastNodeCountDelta += 1;
                    g_state.dragArrowBase[0] = nn.x;
                    g_state.dragArrowBase[1] = nn.y;
                    g_state.dragArrowBase[2] = nn.z;
                    t -= segLen;
                    // Re-snapshot segment lengths since the chain grew.
                    SnapshotSegmentLengths(*spline, g_state.origSegmentLengths);
                }
                // REMOVE case: t is strongly negative. Pull back into
                // the chain - if we crossed the next node inward,
                // remove the current endpoint.
                while (t <= -segLen && (int)spline->nodes.size() > 2) {
                    if (isHead) {
                        spline->nodes.erase(spline->nodes.begin());
                    } else {
                        spline->nodes.pop_back();
                    }
                    g_state.lastNodeCountDelta -= 1;
                    // After removal, rebase to the new endpoint.
                    int newEnd = isHead ? 0 : ((int)spline->nodes.size() - 1);
                    g_state.dragArrowBase[0] = spline->nodes[newEnd].x;
                    g_state.dragArrowBase[1] = spline->nodes[newEnd].y;
                    g_state.dragArrowBase[2] = spline->nodes[newEnd].z;
                    t += segLen;
                    SnapshotSegmentLengths(*spline, g_state.origSegmentLengths);
                    // Axis might have flipped direction if we crossed a
                    // corner - recompute.
                    ComputeArrowAxis(*spline, isHead, g_state.dragArrowAxis);
                }
                RecomputeArcLengths(spline->nodes);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  DrawHandles - 3D rendering of spheres, arrows, hover highlight
// ─────────────────────────────────────────────────────────────────────────
// Uses DrawPrimitiveUP with XYZ+DIFFUSE vertices to keep it simple. We
// render tiny BILLBOARDED sphere approximations as triangle fans oriented
// at the camera, not real 3D spheres, because real spheres would need
// MUCH more geometry and this gets the visual job done. Arrows are
// triangles from base to tip with a little flare at the end for
// silhouette recognition. Hover highlight is a bright color on the
// hovered element.
void DrawHandles(IDirect3DDevice9* dev,
                 LevelScene* scene,
                 const float eyePos[3],
                 const float /*viewMat*/[16],
                 const float /*projMat*/[16])
{
    if (dev == NULL || scene == NULL) return;
    if (g_state.editSplineGuid == 0) return;

    // Find the spline by GUID (non-const version because we might need
    // to eyeball it; we only read here, but the caller path runs off
    // getSplines/getSplinesMut either way).
    const std::vector<LevelSpline>& splines = scene->getSplines();
    const LevelSpline* spline = NULL;
    for (size_t i = 0; i < splines.size(); ++i) {
        if (splines[i].guid == g_state.editSplineGuid) { spline = &splines[i]; break; }
    }
    if (spline == NULL) return;

    int N = (int)spline->nodes.size();
    if (N == 0) return;

    // Render state. Identical pattern to LevelScene's existing spline
    // draw block, alpha on so hover/dim work, depth test on but no
    // depth write so nodes are occluded by solid geometry in front
    // of them but not by each other.
    dev->SetRenderState(D3DRS_FILLMODE,       D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING,       FALSE);
    dev->SetRenderState(D3DRS_ZENABLE,        TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,   FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,       D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,      D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_CULLMODE,       D3DCULL_NONE);
    dev->SetTexture(0, NULL);
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);

    struct V { float x, y, z; DWORD col; };

    // ── Handle spheres ────────────────────────────────────────────────
    // Draw each node as a small billboarded quad. Size scales with
    // distance to keep the handle looking a fixed screen size.
    for (int i = 0; i < N; ++i) {
        float wx = spline->nodes[i].x;
        float wy = spline->nodes[i].y;
        float wz = spline->nodes[i].z;
        float dx = wx - eyePos[0], dy = wy - eyePos[1], dz = wz - eyePos[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist < 0.1f) dist = 0.1f;
        float rad = dist * 0.012f; // ~roughly 14px at typical FOV

        // Pick color: head=green, tail=red, middle=yellow. Hovered and
        // active-drag nodes get a bright white highlight so the user
        // knows exactly what they are going to grab.
        DWORD col;
        if (i == 0)                    col = 0xFF44FF44; // head green
        else if (i == N-1)             col = 0xFFFF4444; // tail red
        else                           col = 0xFFFFDD44; // middle yellow
        if (g_state.hoverType == DRAG_NODE && g_state.hoverNodeIdx == i) col = 0xFFFFFFFF;
        if (g_state.activeDragType == DRAG_NODE && g_state.activeNodeIdx == i) col = 0xFFFFFFFF;

        // Billboard quad facing the camera: build two perpendicular axes
        // in the plane normal to eye->node.
        float toEye[3] = { eyePos[0]-wx, eyePos[1]-wy, eyePos[2]-wz };
        Normalize3(toEye);
        float up[3] = { 0.0f, 1.0f, 0.0f };
        float right[3]; Cross3(up, toEye, right); Normalize3(right);
        float bbUp[3];  Cross3(toEye, right, bbUp);

        V quad[6];
        float px[4][3];
        for (int c = 0; c < 4; ++c) {
            float sx = (c==0||c==3) ? -rad : rad;
            float sy = (c==0||c==1) ? -rad : rad;
            px[c][0] = wx + right[0]*sx + bbUp[0]*sy;
            px[c][1] = wy + right[1]*sx + bbUp[1]*sy;
            px[c][2] = wz + right[2]*sx + bbUp[2]*sy;
        }
        // Two triangles forming the quad.
        quad[0].x=px[0][0]; quad[0].y=px[0][1]; quad[0].z=px[0][2]; quad[0].col=col;
        quad[1].x=px[1][0]; quad[1].y=px[1][1]; quad[1].z=px[1][2]; quad[1].col=col;
        quad[2].x=px[2][0]; quad[2].y=px[2][1]; quad[2].z=px[2][2]; quad[2].col=col;
        quad[3].x=px[0][0]; quad[3].y=px[0][1]; quad[3].z=px[0][2]; quad[3].col=col;
        quad[4].x=px[2][0]; quad[4].y=px[2][1]; quad[4].z=px[2][2]; quad[4].col=col;
        quad[5].x=px[3][0]; quad[5].y=px[3][1]; quad[5].z=px[3][2]; quad[5].col=col;
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(V));
    }

    // ── Extend arrows (head and tail) ────────────────────────────────
    // Render as a thickened line with a triangular flare at the tip.
    // Arrow goes from endpoint (base) to tip = base + axis * arrowLen.
    if (N >= 2) {
        float arrowLen = ComputeArrowLength(*spline);
        for (int side = 0; side < 2; ++side) {
            bool head = (side == 0);
            int endIdx = head ? 0 : (N - 1);
            float base[3] = { spline->nodes[endIdx].x, spline->nodes[endIdx].y, spline->nodes[endIdx].z };
            float axis[3]; ComputeArrowAxis(*spline, head, axis);
            float tip[3] = { base[0] + axis[0]*arrowLen,
                             base[1] + axis[1]*arrowLen,
                             base[2] + axis[2]*arrowLen };

            DWORD acol = 0xFFFFDD00; // arrow yellow
            if ((head  && g_state.hoverType == DRAG_HEAD_ARROW) ||
                (!head && g_state.hoverType == DRAG_TAIL_ARROW)) acol = 0xFFFFFFFF;
            if ((head  && g_state.activeDragType == DRAG_HEAD_ARROW) ||
                (!head && g_state.activeDragType == DRAG_TAIL_ARROW)) acol = 0xFFFFFFFF;

            // Billboard axes for the arrow shaft.
            float toEye[3] = { eyePos[0]-base[0], eyePos[1]-base[1], eyePos[2]-base[2] };
            Normalize3(toEye);
            float side_axis[3]; Cross3(axis, toEye, side_axis); Normalize3(side_axis);
            float shaftHalfWidth = arrowLen * 0.03f;
            float flareHalfWidth = arrowLen * 0.12f;
            float flareStart = arrowLen * 0.75f;

            float baseL[3] = { base[0] + side_axis[0]*shaftHalfWidth,
                               base[1] + side_axis[1]*shaftHalfWidth,
                               base[2] + side_axis[2]*shaftHalfWidth };
            float baseR[3] = { base[0] - side_axis[0]*shaftHalfWidth,
                               base[1] - side_axis[1]*shaftHalfWidth,
                               base[2] - side_axis[2]*shaftHalfWidth };
            float flL[3]   = { base[0] + axis[0]*flareStart + side_axis[0]*shaftHalfWidth,
                               base[1] + axis[1]*flareStart + side_axis[1]*shaftHalfWidth,
                               base[2] + axis[2]*flareStart + side_axis[2]*shaftHalfWidth };
            float flR[3]   = { base[0] + axis[0]*flareStart - side_axis[0]*shaftHalfWidth,
                               base[1] + axis[1]*flareStart - side_axis[1]*shaftHalfWidth,
                               base[2] + axis[2]*flareStart - side_axis[2]*shaftHalfWidth };
            float flOL[3]  = { base[0] + axis[0]*flareStart + side_axis[0]*flareHalfWidth,
                               base[1] + axis[1]*flareStart + side_axis[1]*flareHalfWidth,
                               base[2] + axis[2]*flareStart + side_axis[2]*flareHalfWidth };
            float flOR[3]  = { base[0] + axis[0]*flareStart - side_axis[0]*flareHalfWidth,
                               base[1] + axis[1]*flareStart - side_axis[1]*flareHalfWidth,
                               base[2] + axis[2]*flareStart - side_axis[2]*flareHalfWidth };

            V tri[12];
            // Shaft quad (base->flareStart).
            tri[0].x=baseL[0]; tri[0].y=baseL[1]; tri[0].z=baseL[2]; tri[0].col=acol;
            tri[1].x=flL[0];   tri[1].y=flL[1];   tri[1].z=flL[2];   tri[1].col=acol;
            tri[2].x=flR[0];   tri[2].y=flR[1];   tri[2].z=flR[2];   tri[2].col=acol;
            tri[3].x=baseL[0]; tri[3].y=baseL[1]; tri[3].z=baseL[2]; tri[3].col=acol;
            tri[4].x=flR[0];   tri[4].y=flR[1];   tri[4].z=flR[2];   tri[4].col=acol;
            tri[5].x=baseR[0]; tri[5].y=baseR[1]; tri[5].z=baseR[2]; tri[5].col=acol;
            // Flare triangles (two tris forming an arrowhead to tip).
            tri[6].x=flOL[0];  tri[6].y=flOL[1];  tri[6].z=flOL[2];  tri[6].col=acol;
            tri[7].x=tip[0];   tri[7].y=tip[1];   tri[7].z=tip[2];   tri[7].col=acol;
            tri[8].x=flOR[0];  tri[8].y=flOR[1];  tri[8].z=flOR[2];  tri[8].col=acol;
            // Fill between flare-out and flare-shaft on both sides so the
            // arrowhead reads as a triangle and not a floating tip.
            tri[9].x=flOL[0];  tri[9].y=flOL[1];  tri[9].z=flOL[2];  tri[9].col=acol;
            tri[10].x=flL[0];  tri[10].y=flL[1];  tri[10].z=flL[2];  tri[10].col=acol;
            tri[11].x=tip[0];  tri[11].y=tip[1];  tri[11].z=tip[2];  tri[11].col=acol;
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 4, tri, sizeof(V));

            // Other side of flare-to-tip fill.
            V tri2[3];
            tri2[0].x=flOR[0]; tri2[0].y=flOR[1]; tri2[0].z=flOR[2]; tri2[0].col=acol;
            tri2[1].x=tip[0];  tri2[1].y=tip[1];  tri2[1].z=tip[2];  tri2[1].col=acol;
            tri2[2].x=flR[0];  tri2[2].y=flR[1];  tri2[2].z=flR[2];  tri2[2].col=acol;
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri2, sizeof(V));
        }
    }

    // ── Restore alpha blend OFF so the caller's next draw does not
    //    unexpectedly alpha-blend. CULLMODE stays on NONE because the
    //    caller's spline-curve rendering does not care about cull.
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
}

// ═════════════════════════════════════════════════════════════════════════
//  PATH PLACE EXTERNAL PATH (Crowd Generator sub-tool)
// ═════════════════════════════════════════════════════════════════════════
//
// Click-to-add nodes on the Y=0 ground plane, drag handles to reshape,
// RMB to pop the last node. Same plane-projection drag math as the
// cinematic editor above, just operating on args.pathPlaceNodes
// instead of LevelSpline. No FABRIK — for placement paths the user
// wants direct control of each node's position, not chain-following
// behavior. Cinematic cameras needed FABRIK because segment lengths
// were artistically meaningful; for crowd placement, the user just
// wants the nodes where they put them.
namespace {
    // Internal state for the path-place drag machine + render cache.
    // Lives separate from g_state above so the cinematic editor's
    // drag and the path editor's drag don't fight. UpdateExternalPath
    // fills cachedNodes / cachedFlags from args every frame so
    // DrawExternalPathHandles can render without taking args as a
    // parameter (keeps LevelScene clean — it doesn't have access to
    // the live args pointer).
    struct PathState {
        int   activeDragIdx;   // -1 = no drag, else index of node being dragged
        int   hoverIdx;        // -1 = no hover
        float dragPlaneNormal[3];
        float dragPlaneOrigin[3];
        // Render cache (populated by UpdateExternalPath each frame).
        int   active;          // 1 = preview active, 0 = no-op render
        int   cachedNodeCount;
        float cachedNodes[64*3];
        float cachedSpacing;
        int   cachedRows;
        float cachedRowGap;
        // Facing cache for arrow rendering. DrawExternalPathHandles
        // doesn't take args, so we cache everything it needs here.
        int   cachedFacingMode;       // 0=tangent 1=perp 2=look-at 3=fixed 4=random
        float cachedFixedYaw;         // radians
        float cachedLookAt[3];        // world point for look-at mode
        PathState() : activeDragIdx(-1), hoverIdx(-1),
                      active(0), cachedNodeCount(0),
                      cachedSpacing(1.5f), cachedRows(1), cachedRowGap(1.2f),
                      cachedFacingMode(0), cachedFixedYaw(0.0f) {
            memset(dragPlaneNormal, 0, sizeof(dragPlaneNormal));
            memset(dragPlaneOrigin, 0, sizeof(dragPlaneOrigin));
            memset(cachedNodes, 0, sizeof(cachedNodes));
            memset(cachedLookAt, 0, sizeof(cachedLookAt));
        }
    };
    static PathState g_pathState;
}

void UpdateExternalPath(ImGuiGlueFrameArgs& args,
                        LevelScene* scene,
                        float mouseX, float mouseY,
                        int   viewportW, int viewportH,
                        bool  mouseDown, bool mouseClicked,
                        bool  rightMouseClicked)
{
    // Always compute the preview count so the panel UI shows it even
    // when draw mode is off (so the user knows what would happen if
    // they enabled it).
    {
        int count = 0;
        if (args.pathPlaceNodeCount >= 2 && args.pathPlaceSpacing > 0.01f) {
            const float* nodes = args.pathPlaceNodes;
            int nc = args.pathPlaceNodeCount;
            float totalLen = 0.0f;
            for (int i = 0; i + 1 < nc; ++i) {
                float dx = nodes[(i+1)*3+0] - nodes[i*3+0];
                float dy = nodes[(i+1)*3+1] - nodes[i*3+1];
                float dz = nodes[(i+1)*3+2] - nodes[i*3+2];
                totalLen += sqrtf(dx*dx + dy*dy + dz*dz);
            }
            int N = (int)(totalLen / args.pathPlaceSpacing) + 1;
            if (N < 1) N = 1;
            if (N > 1000) N = 1000;
            int rows = args.pathPlaceRows;
            if (rows < 1) rows = 1;
            if (rows > 8) rows = 8;
            count = N * rows;
        }
        args.pathPlacePreviewCount = count;
    }

    // Cache nodes for render UNCONDITIONALLY so handles show whenever
    // there are nodes, regardless of whether draw mode is active. This
    // means even when the user toggles "Stop drawing", they can still
    // see the path they drew and the sampled-marker preview.
    g_pathState.active = (args.pathPlacePreviewActive != 0) ? 1 : 0;
    g_pathState.cachedNodeCount = args.pathPlaceNodeCount;
    int copyCount = args.pathPlaceNodeCount * 3;
    if (copyCount > 64*3) copyCount = 64*3;
    for (int i = 0; i < copyCount; ++i) {
        g_pathState.cachedNodes[i] = args.pathPlaceNodes[i];
    }
    g_pathState.cachedSpacing = args.pathPlaceSpacing;
    g_pathState.cachedRows    = args.pathPlaceRows;
    g_pathState.cachedRowGap  = args.pathPlaceRowGap;
    g_pathState.cachedFacingMode = args.pathPlaceFacingMode;
    g_pathState.cachedFixedYaw   = args.pathPlaceFixedYaw;
    g_pathState.cachedLookAt[0]  = args.pathPlaceLookAt[0];
    g_pathState.cachedLookAt[1]  = args.pathPlaceLookAt[1];
    g_pathState.cachedLookAt[2]  = args.pathPlaceLookAt[2];

    // Bail early when draw mode is off but keep the cache populated
    // above so handles still render. Pick/drag/click are only active
    // in draw mode.
    if (!args.pathPlacePreviewActive || scene == NULL) {
        g_pathState.activeDragIdx = -1;
        g_pathState.hoverIdx = -1;
        return;
    }

    // Ray from mouse through the world.
    float mouseRayO[3], mouseRayD[3];
    scene->screenToRay((int)mouseX, (int)mouseY, viewportW, viewportH,
                       mouseRayO, mouseRayD);

    int N = args.pathPlaceNodeCount;
    float* nodes = args.pathPlaceNodes;

    // ── Hover / pick test ──
    int hitIdx = -1;
    float bestPerpSq = 1e30f;
    for (int i = 0; i < N; ++i) {
        float p[3] = { nodes[i*3+0], nodes[i*3+1], nodes[i*3+2] };
        float op[3]; Sub3(p, mouseRayO, op);
        float t = Dot3(op, mouseRayD);
        if (t < 0.0f) continue;
        float perpSq = Dot3(op, op) - t*t;
        if (perpSq < 0.0f) perpSq = 0.0f;
        float radius = t * 0.03f;
        if (radius < 0.1f) radius = 0.1f;
        if (perpSq < radius * radius && perpSq < bestPerpSq) {
            bestPerpSq = perpSq;
            hitIdx = i;
        }
    }
    g_pathState.hoverIdx = hitIdx;

    // ── Right-click: pop the last node ──
    if (rightMouseClicked && N > 0 && g_pathState.activeDragIdx < 0) {
        args.pathPlaceNodeCount = N - 1;
        // Don't bother zeroing the array slot; pathPlaceNodeCount is
        // the authoritative size and the rest is dead memory.
    }

    // ── Left-click started: either start a drag, or add a node at ground ──
    if (mouseClicked && g_pathState.activeDragIdx < 0) {
        if (hitIdx >= 0) {
            // Start drag on this node. Build plane perpendicular to
            // camera forward (approximated via mouse ray direction)
            // through the grabbed node. Standard plane-projection drag.
            g_pathState.activeDragIdx = hitIdx;
            g_pathState.dragPlaneOrigin[0] = nodes[hitIdx*3+0];
            g_pathState.dragPlaneOrigin[1] = nodes[hitIdx*3+1];
            g_pathState.dragPlaneOrigin[2] = nodes[hitIdx*3+2];
            // Use camera forward = inverse of mouse ray dir as plane
            // normal. For a click on a handle the ray points roughly
            // toward the camera through the handle, so negating it
            // gives roughly the camera forward.
            g_pathState.dragPlaneNormal[0] = -mouseRayD[0];
            g_pathState.dragPlaneNormal[1] = -mouseRayD[1];
            g_pathState.dragPlaneNormal[2] = -mouseRayD[2];
            Normalize3(g_pathState.dragPlaneNormal);
        } else if (N < 64) {
            // Empty space click → raycast to a ground plane at the
            // SELECTED ARCHETYPE'S FIRST INSTANCE Y, not Y=0. Levels
            // like Helm's Deep have the wall at Y=20+, so a Y=0 plane
            // raycast puts nodes underground where the user can't see
            // them. Reading the archetype's existing instance Y gives
            // us the correct ground surface for that archetype.
            float groundY = 0.0f;
            int archIdx = args.pathPlaceArchetype;
            if (archIdx >= 0) {
                const std::vector<LevelCrowdItem>& citems = scene->getCrowdItems();
                if (archIdx < (int)citems.size()
                    && !citems[archIdx].instances.empty())
                {
                    groundY = citems[archIdx].instances[0].position[1];
                }
            }
            float groundOrigin[3] = { 0.0f, groundY, 0.0f };
            float groundNormal[3] = { 0.0f, 1.0f, 0.0f };
            float hit[3];
            if (RayPlaneIntersect(mouseRayO, mouseRayD,
                                   groundOrigin, groundNormal, hit))
            {
                nodes[N*3+0] = hit[0];
                nodes[N*3+1] = hit[1];
                nodes[N*3+2] = hit[2];
                args.pathPlaceNodeCount = N + 1;
            }
        }
    }

    // ── Drag in progress: project mouse onto drag plane, move node ──
    if (g_pathState.activeDragIdx >= 0) {
        if (!mouseDown) {
            // Released. End drag, no commit needed beyond what's already
            // in args.pathPlaceNodes (the DLL reads back from there).
            g_pathState.activeDragIdx = -1;
        } else {
            int idx = g_pathState.activeDragIdx;
            if (idx >= 0 && idx < args.pathPlaceNodeCount) {
                float hit[3];
                if (RayPlaneIntersect(mouseRayO, mouseRayD,
                                       g_pathState.dragPlaneOrigin,
                                       g_pathState.dragPlaneNormal,
                                       hit))
                {
                    nodes[idx*3+0] = hit[0];
                    nodes[idx*3+1] = hit[1];
                    nodes[idx*3+2] = hit[2];
                }
            } else {
                g_pathState.activeDragIdx = -1;
            }
        }
    }
}

void DrawExternalPathHandles(IDirect3DDevice9* dev,
                             LevelScene*       scene,
                             const float       eyePos[3],
                             const float       /*viewMat*/[16],
                             const float       /*projMat*/[16])
{
    if (dev == NULL || scene == NULL) return;
    if (g_pathState.cachedNodeCount <= 0) return;

    int N = g_pathState.cachedNodeCount;
    const float* nodes = g_pathState.cachedNodes;

    // Same D3D9 render state setup as the cinematic DrawHandles BUT
    // with depth test DISABLED so handles render THROUGH walls and
    // terrain. Otherwise nodes that land on Y=0 in Helm's Deep are
    // invisible behind the wall geometry at Y=20+, and the user
    // thinks the click did nothing.
    dev->SetRenderState(D3DRS_FILLMODE,        D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING,        FALSE);
    dev->SetRenderState(D3DRS_ZENABLE,         FALSE);  // see through walls
    dev->SetRenderState(D3DRS_ZWRITEENABLE,    FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,        D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,       D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_CULLMODE,        D3DCULL_NONE);
    dev->SetTexture(0, NULL);
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);

    struct V { float x, y, z; DWORD col; };

    // Helper macro: draw a billboarded quad facing the camera at world
    // position w with screen-radius ~rad and color clr.
    // NOTE: parameter names use a leading underscore so they cannot
    // collide with the V struct field names (.col, .x, .y, .z). The
    // preprocessor token-replaces parameter names anywhere they appear
    // in the body INCLUDING after `.`, so naming a param `col` would
    // turn `.col` into the user's argument and break everything.
    #define DRAW_BB_QUAD(_wx, _wy, _wz, _rad, _clr) do { \
        float toEye[3] = { eyePos[0]-(_wx), eyePos[1]-(_wy), eyePos[2]-(_wz) }; \
        Normalize3(toEye); \
        float up[3] = { 0.0f, 1.0f, 0.0f }; \
        float right[3]; Cross3(up, toEye, right); Normalize3(right); \
        float bbUp[3];  Cross3(toEye, right, bbUp); \
        V quad[6]; float px[4][3]; \
        for (int c = 0; c < 4; ++c) { \
            float sx = (c==0||c==3) ? -(_rad) : (_rad); \
            float sy = (c==0||c==1) ? -(_rad) : (_rad); \
            px[c][0] = (_wx) + right[0]*sx + bbUp[0]*sy; \
            px[c][1] = (_wy) + right[1]*sx + bbUp[1]*sy; \
            px[c][2] = (_wz) + right[2]*sx + bbUp[2]*sy; \
        } \
        quad[0].x=px[0][0]; quad[0].y=px[0][1]; quad[0].z=px[0][2]; quad[0].col=(_clr); \
        quad[1].x=px[1][0]; quad[1].y=px[1][1]; quad[1].z=px[1][2]; quad[1].col=(_clr); \
        quad[2].x=px[2][0]; quad[2].y=px[2][1]; quad[2].z=px[2][2]; quad[2].col=(_clr); \
        quad[3].x=px[0][0]; quad[3].y=px[0][1]; quad[3].z=px[0][2]; quad[3].col=(_clr); \
        quad[4].x=px[2][0]; quad[4].y=px[2][1]; quad[4].z=px[2][2]; quad[4].col=(_clr); \
        quad[5].x=px[3][0]; quad[5].y=px[3][1]; quad[5].z=px[3][2]; quad[5].col=(_clr); \
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(V)); \
    } while(0)

    // ── Vertical drop-lines from each node down to Y=0 ──
    // Makes height immediately readable: you can see at a glance
    // exactly how high above world Y=0 each node sits. Without
    // these, billboarded spheres at arbitrary Y values look the
    // same regardless of altitude.
    {
        std::vector<V> dropLines;
        dropLines.reserve((size_t)(N * 2));
        for (int i = 0; i < N; ++i) {
            float wx = nodes[i*3+0];
            float wy = nodes[i*3+1];
            float wz = nodes[i*3+2];
            // Skip if node is already at/below Y=0.
            if (wy <= 0.05f) continue;
            V top;    top.x=wx;    top.y=wy;    top.z=wz;    top.col = 0x88FFFFFF;
            V bottom; bottom.x=wx; bottom.y=0;   bottom.z=wz; bottom.col = 0x44FFFFFF;
            dropLines.push_back(top);
            dropLines.push_back(bottom);
        }
        if (!dropLines.empty()) {
            dev->DrawPrimitiveUP(D3DPT_LINELIST,
                                  (UINT)(dropLines.size() / 2),
                                  &dropLines[0], sizeof(V));
        }
    }

    // ── Path nodes: cyan for normal, white for hover/drag ──
    // Handles are 4x bigger than the cinematic spline's because path
    // placement happens at ground level where camera distance is often
    // 30-50m. A 14px-equivalent handle disappears at that range.
    for (int i = 0; i < N; ++i) {
        float wx = nodes[i*3+0], wy = nodes[i*3+1], wz = nodes[i*3+2];
        float dx = wx-eyePos[0], dy = wy-eyePos[1], dz = wz-eyePos[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist < 0.1f) dist = 0.1f;
        float rad = dist * 0.048f;  // ~56px equiv, was 14px
        DWORD col = 0xFF44CCFF; // cyan
        if (i == 0)       col = 0xFF44FF44; // head: green
        if (i == N - 1)   col = 0xFFFF8844; // tail: orange
        if (g_pathState.hoverIdx == i)        col = 0xFFFFFFFF;
        if (g_pathState.activeDragIdx == i)   col = 0xFFFFFFFF;
        DRAW_BB_QUAD(wx, wy, wz, rad, col);
    }

    // ── Path segments: thin colored line connecting nodes ──
    // Render as a series of small quads stretched between adjacent nodes.
    // Simpler: use D3DPT_LINESTRIP if FVF supports it. D3DFVF_XYZ + DIFFUSE
    // works for lines.
    if (N >= 2) {
        std::vector<V> linePts;
        linePts.reserve((size_t)N);
        for (int i = 0; i < N; ++i) {
            V p;
            p.x = nodes[i*3+0]; p.y = nodes[i*3+1]; p.z = nodes[i*3+2];
            p.col = 0xFFFFCC44; // yellow line
            linePts.push_back(p);
        }
        dev->DrawPrimitiveUP(D3DPT_LINESTRIP, (UINT)(N - 1),
                              &linePts[0], sizeof(V));
    }

    // ── Sampled spawn markers (live preview) ──
    // Inline the same arc-length resample math as the engine scatter
    // consumer so what the user sees here matches what Stamp will
    // produce. Cap at ~200 markers in preview so we don't tank the
    // framerate on a 5000-instance preview.
    float spacing = g_pathState.cachedSpacing;
    if (N >= 2 && spacing > 0.01f) {
        float totalLen = 0.0f;
        for (int i = 0; i + 1 < N; ++i) {
            float dx = nodes[(i+1)*3+0] - nodes[i*3+0];
            float dy = nodes[(i+1)*3+1] - nodes[i*3+1];
            float dz = nodes[(i+1)*3+2] - nodes[i*3+2];
            totalLen += sqrtf(dx*dx + dy*dy + dz*dz);
        }
        int sampleN = (int)(totalLen / spacing) + 1;
        if (sampleN < 1) sampleN = 1;
        if (sampleN > 200) sampleN = 200; // preview cap, not the real scatter cap

        int rows = g_pathState.cachedRows;
        if (rows < 1) rows = 1;
        if (rows > 8) rows = 8;
        float rowGap = g_pathState.cachedRowGap;

        for (int s = 0; s < sampleN; ++s) {
            float wantLen = (float)s * spacing;
            float accum = 0.0f;
            int seg = 0;
            float sp[3] = {0,0,0};
            float st[3] = {1,0,0};
            while (seg + 1 < N) {
                float dx = nodes[(seg+1)*3+0] - nodes[seg*3+0];
                float dy = nodes[(seg+1)*3+1] - nodes[seg*3+1];
                float dz = nodes[(seg+1)*3+2] - nodes[seg*3+2];
                float segLen = sqrtf(dx*dx + dy*dy + dz*dz);
                if (accum + segLen >= wantLen || seg + 2 >= N) {
                    float lt = (segLen > 0.001f)
                               ? (wantLen - accum) / segLen
                               : 0.0f;
                    if (lt < 0.0f) lt = 0.0f;
                    if (lt > 1.0f) lt = 1.0f;
                    sp[0] = nodes[seg*3+0] + dx * lt;
                    sp[1] = nodes[seg*3+1] + dy * lt;
                    sp[2] = nodes[seg*3+2] + dz * lt;
                    float tlen = sqrtf(dx*dx + dz*dz);
                    if (tlen > 0.001f) {
                        st[0] = dx / tlen; st[1] = 0.0f; st[2] = dz / tlen;
                    }
                    break;
                }
                accum += segLen;
                ++seg;
            }
            float perpX = -st[2], perpZ = st[0];
            for (int r = 0; r < rows; ++r) {
                float rowOff = ((float)r - (rows - 1) * 0.5f) * rowGap;
                float mx = sp[0] + perpX * rowOff;
                float my = sp[1];
                float mz = sp[2] + perpZ * rowOff;
                float dx2 = mx-eyePos[0], dy2 = my-eyePos[1], dz2 = mz-eyePos[2];
                float dist2 = sqrtf(dx2*dx2 + dy2*dy2 + dz2*dz2);
                if (dist2 < 0.1f) dist2 = 0.1f;
                float mrad = dist2 * 0.012f;
                DWORD mcol = 0xCC44FFCC; // semi-translucent teal

                // Compute the facing direction for THIS spawn position
                // based on the chosen facing mode. The arrow apex points
                // in the facing direction so the user sees EXACTLY where
                // each spawned character will face.
                float fwdX = st[0], fwdZ = st[2];
                switch (g_pathState.cachedFacingMode) {
                    case 0:  // Tangent — already set
                        break;
                    case 1:  // Perpendicular (rank-facing)
                        fwdX = perpX;
                        fwdZ = perpZ;
                        break;
                    case 2: { // Look-at
                        float toX = g_pathState.cachedLookAt[0] - mx;
                        float toZ = g_pathState.cachedLookAt[2] - mz;
                        float tlen = sqrtf(toX*toX + toZ*toZ);
                        if (tlen > 0.001f) {
                            fwdX = toX / tlen;
                            fwdZ = toZ / tlen;
                        }
                        break;
                    }
                    case 3: { // Fixed yaw
                        float fy = g_pathState.cachedFixedYaw;
                        fwdX = sinf(fy);
                        fwdZ = cosf(fy);
                        break;
                    }
                    case 4:  // Random — show tangent as a hint
                    default:
                        break;
                }
                float fwdLen = sqrtf(fwdX*fwdX + fwdZ*fwdZ);
                if (fwdLen > 0.001f) {
                    fwdX /= fwdLen;
                    fwdZ /= fwdLen;
                } else {
                    fwdX = 0.0f; fwdZ = 1.0f;
                }
                // Arrow apex = position + forward * (mrad * 4)
                // Arrow base corners: position +/- perpendicular * mrad
                float apexX = mx + fwdX * (mrad * 4.0f);
                float apexY = my;
                float apexZ = mz + fwdZ * (mrad * 4.0f);
                float ppX = -fwdZ * mrad * 1.2f;
                float ppZ =  fwdX * mrad * 1.2f;
                float baseLX = mx + ppX, baseLZ = mz + ppZ;
                float baseRX = mx - ppX, baseRZ = mz - ppZ;
                V tri[3];
                tri[0].x = apexX;  tri[0].y = apexY; tri[0].z = apexZ;  tri[0].col = 0xFFFFAA22;
                tri[1].x = baseLX; tri[1].y = my;    tri[1].z = baseLZ; tri[1].col = 0xCCFFAA22;
                tri[2].x = baseRX; tri[2].y = my;    tri[2].z = baseRZ; tri[2].col = 0xCCFFAA22;
                dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));

                // Position dot at the base (where the character feet land)
                DRAW_BB_QUAD(mx, my, mz, mrad, mcol);
            }
        }
    }

    #undef DRAW_BB_QUAD

    // Restore alpha blend off, depth write on.
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
}

} // namespace SplineEditor
