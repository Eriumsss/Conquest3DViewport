// Scene3DBoneEditor.cpp — The Puppet Master's Workshop
// ============================================================================
// Written by: Eriumsss
//
// Full bone-level animation editor built from scratch. Select a bone,
// grab the gizmo, drag to rotate or translate, snap to grid, record
// keyframes, undo/redo with full pose snapshots, save poses to a
// library, export edited clips to JSON. This is a mini-DCC tool
// bolted onto a reverse-engineered game engine viewer.
//
// Pandemic's animators had Maya + Havok Content Tools for this.
// We have an ImGui gizmo and hand-rolled euler↔quaternion conversion.
// The gizmo axis-picking uses screen-space projection against the
// bone's world-space axes — same technique used in Maya's rotate
// manipulator. I didn't copy Maya's code. I copied the IDEA and
// then spent a week debugging gimbal lock edge cases that Maya
// handles internally with magic I'll never understand.
//
// Undo/redo stores full PoseSnapshot objects (quaternion + translation
// for every bone). Memory-hungry? Yes. Reliable? Also yes. I tried
// delta-based undo once and it accumulated floating point error until
// the skeleton drifted into a pose that looked like modern art.
// ============================================================================

#include "Scene3DRendererInternal.h"
#include "GameModelLoader.h"
#include "MocapExporter.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

// ============================================================================
// Static helpers
// ============================================================================

static void CopyVectorToHkArray(const std::vector<Scene3DRenderer::PoseRot>& src, hkArray<hkQuaternion>& dst)
{
    const int count = (int)src.size();
    dst.setSize(count);
    for (int i = 0; i < count; ++i)
    {
        const Scene3DRenderer::PoseRot& r = src[i];
        dst[i].set(r.x, r.y, r.z, r.w);
    }
}

static Scene3DRenderer::PoseSnapshot MakePoseSnapshot(const hkArray<hkQuaternion>& rot, const std::vector<Scene3DRenderer::EditorTransOverride>& trans)
{
    Scene3DRenderer::PoseSnapshot snap;
    snap.rot.clear();
    snap.rot.reserve(rot.getSize());
    for (int i = 0; i < rot.getSize(); ++i)
    {
        Scene3DRenderer::PoseRot r;
        const hkQuaternion& q = rot[i];
        r.x = q(0);
        r.y = q(1);
        r.z = q(2);
        r.w = q(3);
        snap.rot.push_back(r);
    }
    snap.trans = trans;
    return snap;
}

// ---------------------------------------------------------------------------
// Auto-compute Catmull-Rom tangents for all keys in an editor float curve.
// Called after any key insertion or value change.
// tangent_i = 0.5 * (v_{i+1} - v_{i-1}) / (t_{i+1} - t_{i-1})  [in value/sec]
// Boundary keys get one-sided finite difference.
// Only modifies keys that are in CURVE_CUBIC mode (leaves LINEAR/CONSTANT alone).
// ---------------------------------------------------------------------------
static void RecomputeAutoTangents(std::vector<Scene3DRenderer::EditorFloatKey>& keys)
{
    const int n = (int)keys.size();
    if (n < 2) return;

    for (int i = 0; i < n; ++i)
    {
        if (keys[i].interpMode != Scene3DRenderer::CURVE_CUBIC)
            continue;

        float tangent = 0.0f;
        if (i == 0)
        {
            // Forward difference
            float dt = (keys[1].timeMs - keys[0].timeMs) / 1000.0f;
            if (dt > 1e-6f)
                tangent = (keys[1].value - keys[0].value) / dt;
        }
        else if (i == n - 1)
        {
            // Backward difference
            float dt = (keys[n-1].timeMs - keys[n-2].timeMs) / 1000.0f;
            if (dt > 1e-6f)
                tangent = (keys[n-1].value - keys[n-2].value) / dt;
        }
        else
        {
            // Central difference (Catmull-Rom)
            float dt = (keys[i+1].timeMs - keys[i-1].timeMs) / 1000.0f;
            if (dt > 1e-6f)
                tangent = (keys[i+1].value - keys[i-1].value) / dt;
        }
        keys[i].inTangent = tangent;
        keys[i].outTangent = tangent;
    }
}

static float ComputeSnappedDelta(float& accum, float& applied, float step)
{
    if (step < 1e-6f)
    {
        float d = accum;
        accum = 0.0f;
        return d;
    }
    float snapped = 0.0f;
    if (accum >= 0.0f)
    {
        snapped = floorf(accum / step) * step;
    }
    else
    {
        snapped = ceilf(accum / step) * step;
    }
    float delta = snapped - applied;
    applied = snapped;
    return delta;
}

static float DistPointToSegment2D(float px, float py, float ax, float ay, float bx, float by)
{
    float abx = bx - ax;
    float aby = by - ay;
    float apx = px - ax;
    float apy = py - ay;
    float abLen2 = abx * abx + aby * aby;
    float t = 0.0f;
    if (abLen2 > 1e-6f)
    {
        t = (apx * abx + apy * aby) / abLen2;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float cx = ax + abx * t;
    float cy = ay + aby * t;
    float dx = px - cx;
    float dy = py - cy;
    return sqrtf(dx * dx + dy * dy);
}

static void MirrorPoseSnapshotX(Scene3DRenderer::PoseSnapshot& snap)
{
    for (size_t i = 0; i < snap.rot.size(); ++i)
    {
        Scene3DRenderer::PoseRot& r = snap.rot[i];
        hkQuaternion q;
        q.set(r.x, r.y, r.z, r.w);
        float qx = q(0);
        float qy = q(1);
        float qz = q(2);
        float qw = q(3);
        q.set(-qx, qy, -qz, -qw);
        q.normalize();
        r.x = q(0);
        r.y = q(1);
        r.z = q(2);
        r.w = q(3);
    }
    for (size_t i = 0; i < snap.trans.size(); ++i)
    {
        snap.trans[i].x = -snap.trans[i].x;
    }
}

// ============================================================================
// Undo infrastructure
// ============================================================================

void Scene3DRenderer::pushUndoSnapshot()
{
    Scene3DRenderer::PoseSnapshot snap = MakePoseSnapshot(m_editorOverrideRot, m_editorOverrideTrans);
    m_undoStack.push_back(snap);
    if (m_undoStack.size() > 32)
    {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_redoStack.clear();
}

// ============================================================================
// Bone selection
// ============================================================================

void Scene3DRenderer::setSelectedBoneIndex(int index)
{
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        m_selectedBoneIndex = -1;
        return;
    }
    if (index < 0 || index >= m_gameModel->skeleton->m_numBones)
    {
        m_selectedBoneIndex = -1;
        return;
    }
    m_selectedBoneIndex = index;
}

int Scene3DRenderer::getSelectedBoneIndex() const
{
    return m_selectedBoneIndex;
}

// ============================================================================
// Gizmo settings
// ============================================================================

void Scene3DRenderer::setGizmoMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 1) mode = 1;
    m_gizmoMode = static_cast<GizmoMode>(mode);
}

void Scene3DRenderer::toggleGizmoSpace()
{
    m_gizmoSpace = (m_gizmoSpace == GIZMO_LOCAL) ? GIZMO_WORLD : GIZMO_LOCAL;
}

void Scene3DRenderer::setRotateSnapEnabled(bool enabled)
{
    m_rotateSnapEnabled = enabled;
}

void Scene3DRenderer::setMoveSnapEnabled(bool enabled)
{
    m_moveSnapEnabled = enabled;
}

void Scene3DRenderer::setRotateSnapDegrees(float degrees)
{
    if (degrees < 0.1f) degrees = 0.1f;
    if (degrees > 180.0f) degrees = 180.0f;
    m_rotateSnapDegrees = degrees;
}

void Scene3DRenderer::setMoveSnapUnits(float units)
{
    if (units < 0.0001f) units = 0.0001f;
    if (units > 10.0f) units = 10.0f;
    m_moveSnapUnits = units;
}

void Scene3DRenderer::setEditorInterpolationMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 1) mode = 1;
    m_editorInterpolationMode = static_cast<InterpMode>(mode);
}

void Scene3DRenderer::setEditorDefaultEasing(int easingType, float cp1x, float cp1y, float cp2x, float cp2y)
{
    if (easingType < 0) easingType = 0;
    if (easingType >= EASING_COUNT) easingType = EASING_COUNT - 1;
    m_editorDefaultEasingType = easingType;
    m_editorDefaultEasingCp1x = cp1x;
    m_editorDefaultEasingCp1y = cp1y;
    m_editorDefaultEasingCp2x = cp2x;
    m_editorDefaultEasingCp2y = cp2y;
}

void Scene3DRenderer::getEditorDefaultEasingCP(float& cp1x, float& cp1y, float& cp2x, float& cp2y) const
{
    cp1x = m_editorDefaultEasingCp1x;
    cp1y = m_editorDefaultEasingCp1y;
    cp2x = m_editorDefaultEasingCp2x;
    cp2y = m_editorDefaultEasingCp2y;
}

static float GetEditorKeyTimeMsFallback(float timeMs, int frame, float frameTimeMs)
{
    if (timeMs >= 0.1f)
        return timeMs;
    if (frame < 0) frame = 0;
    return (float)frame * frameTimeMs;
}

int Scene3DRenderer::findEditorRotKeyAtOrBeforeTime(int boneIndex, float timeSeconds) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorRotKeys.size())
        return -1;
    const std::vector<EditorKey>& keys = m_editorRotKeys[boneIndex];
    if (keys.empty())
        return -1;

    const float frameTimeMs = ((m_editorFrameTime > 0.0f) ? m_editorFrameTime : (1.0f / 30.0f)) * 1000.0f;
    const float timeMs = timeSeconds * 1000.0f;

    int best = -1;
    for (int i = 0; i < (int)keys.size(); ++i)
    {
        float kt = GetEditorKeyTimeMsFallback(keys[i].timeMs, keys[i].frame, frameTimeMs);
        if (kt <= timeMs + 0.001f)
            best = i;
        else
            break;
    }
    return best;
}

int Scene3DRenderer::findEditorTransKeyAtOrBeforeTime(int boneIndex, float timeSeconds) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return -1;
    const std::vector<EditorFloatKey>& keysX = m_editorPosKeysX[boneIndex];
    if (keysX.empty())
        return -1;

    const float frameTimeMs = ((m_editorFrameTime > 0.0f) ? m_editorFrameTime : (1.0f / 30.0f)) * 1000.0f;
    const float timeMs = timeSeconds * 1000.0f;

    int best = -1;
    for (int i = 0; i < (int)keysX.size(); ++i)
    {
        float kt = GetEditorKeyTimeMsFallback(keysX[i].timeMs, keysX[i].frame, frameTimeMs);
        if (kt <= timeMs + 0.001f)
            best = i;
        else
            break;
    }
    return best;
}

int Scene3DRenderer::findEditorScaleKeyAtOrBeforeTime(int boneIndex, float timeSeconds) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return -1;
    const std::vector<EditorFloatKey>& keysX = m_editorScaleKeysX[boneIndex];
    if (keysX.empty())
        return -1;

    const float frameTimeMs = ((m_editorFrameTime > 0.0f) ? m_editorFrameTime : (1.0f / 30.0f)) * 1000.0f;
    const float timeMs = timeSeconds * 1000.0f;

    int best = -1;
    for (int i = 0; i < (int)keysX.size(); ++i)
    {
        float kt = GetEditorKeyTimeMsFallback(keysX[i].timeMs, keysX[i].frame, frameTimeMs);
        if (kt <= timeMs + 0.001f)
            best = i;
        else
            break;
    }
    return best;
}

bool Scene3DRenderer::getEditorRotKeyEasing(int boneIndex, int keyIndex, int& outType, float& outCp1x, float& outCp1y, float& outCp2x, float& outCp2y) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorRotKeys.size())
        return false;
    const std::vector<EditorKey>& keys = m_editorRotKeys[boneIndex];
    if (keyIndex < 0 || keyIndex >= (int)keys.size())
        return false;
    outType = keys[keyIndex].easingType;
    outCp1x = keys[keyIndex].easingCp1x;
    outCp1y = keys[keyIndex].easingCp1y;
    outCp2x = keys[keyIndex].easingCp2x;
    outCp2y = keys[keyIndex].easingCp2y;
    return true;
}

bool Scene3DRenderer::getEditorTransKeyEasing(int boneIndex, int keyIndex, int& outType, float& outCp1x, float& outCp1y, float& outCp2x, float& outCp2y) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return false;
    const std::vector<EditorFloatKey>& keysX = m_editorPosKeysX[boneIndex];
    if (keyIndex < 0 || keyIndex >= (int)keysX.size())
        return false;
    outType = keysX[keyIndex].easingType;
    outCp1x = keysX[keyIndex].easingCp1x;
    outCp1y = keysX[keyIndex].easingCp1y;
    outCp2x = keysX[keyIndex].easingCp2x;
    outCp2y = keysX[keyIndex].easingCp2y;
    return true;
}

bool Scene3DRenderer::getEditorScaleKeyEasing(int boneIndex, int keyIndex, int& outType, float& outCp1x, float& outCp1y, float& outCp2x, float& outCp2y) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return false;
    const std::vector<EditorFloatKey>& keysX = m_editorScaleKeysX[boneIndex];
    if (keyIndex < 0 || keyIndex >= (int)keysX.size())
        return false;
    outType = keysX[keyIndex].easingType;
    outCp1x = keysX[keyIndex].easingCp1x;
    outCp1y = keysX[keyIndex].easingCp1y;
    outCp2x = keysX[keyIndex].easingCp2x;
    outCp2y = keysX[keyIndex].easingCp2y;
    return true;
}

bool Scene3DRenderer::setEditorRotKeyEasing(int boneIndex, int keyIndex, int type, float cp1x, float cp1y, float cp2x, float cp2y)
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorRotKeys.size())
        return false;
    std::vector<EditorKey>& keys = m_editorRotKeys[boneIndex];
    if (keyIndex < 0 || keyIndex >= (int)keys.size())
        return false;
    if (type < 0) type = 0;
    if (type >= EASING_COUNT) type = EASING_COUNT - 1;
    keys[keyIndex].easingType = type;
    keys[keyIndex].easingCp1x = cp1x;
    keys[keyIndex].easingCp1y = cp1y;
    keys[keyIndex].easingCp2x = cp2x;
    keys[keyIndex].easingCp2y = cp2y;
    return true;
}

bool Scene3DRenderer::setEditorTransKeyEasing(int boneIndex, int keyIndex, int type, float cp1x, float cp1y, float cp2x, float cp2y)
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return false;
    if (type < 0) type = 0;
    if (type >= EASING_COUNT) type = EASING_COUNT - 1;
    std::vector<EditorFloatKey>* curves[3] = { &m_editorPosKeysX[boneIndex], &m_editorPosKeysY[boneIndex], &m_editorPosKeysZ[boneIndex] };
    for (int c = 0; c < 3; ++c)
    {
        if (!curves[c]) continue;
        if (keyIndex < 0 || keyIndex >= (int)curves[c]->size())
            continue;
        (*curves[c])[keyIndex].easingType = type;
        (*curves[c])[keyIndex].easingCp1x = cp1x;
        (*curves[c])[keyIndex].easingCp1y = cp1y;
        (*curves[c])[keyIndex].easingCp2x = cp2x;
        (*curves[c])[keyIndex].easingCp2y = cp2y;
    }
    return true;
}

bool Scene3DRenderer::setEditorScaleKeyEasing(int boneIndex, int keyIndex, int type, float cp1x, float cp1y, float cp2x, float cp2y)
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return false;
    if (type < 0) type = 0;
    if (type >= EASING_COUNT) type = EASING_COUNT - 1;
    std::vector<EditorFloatKey>* curves[3] = { &m_editorScaleKeysX[boneIndex], &m_editorScaleKeysY[boneIndex], &m_editorScaleKeysZ[boneIndex] };
    for (int c = 0; c < 3; ++c)
    {
        if (!curves[c]) continue;
        if (keyIndex < 0 || keyIndex >= (int)curves[c]->size())
            continue;
        (*curves[c])[keyIndex].easingType = type;
        (*curves[c])[keyIndex].easingCp1x = cp1x;
        (*curves[c])[keyIndex].easingCp1y = cp1y;
        (*curves[c])[keyIndex].easingCp2x = cp2x;
        (*curves[c])[keyIndex].easingCp2y = cp2y;
    }
    return true;
}

bool Scene3DRenderer::getEditorTransKeyTangents(int boneIndex, int keyIndex,
                                               float& outInX, float& outOutX,
                                               float& outInY, float& outOutY,
                                               float& outInZ, float& outOutZ) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return false;
    const std::vector<EditorFloatKey>& kx = m_editorPosKeysX[boneIndex];
    const std::vector<EditorFloatKey>& ky = m_editorPosKeysY[boneIndex];
    const std::vector<EditorFloatKey>& kz = m_editorPosKeysZ[boneIndex];
    if (keyIndex < 0 ||
        keyIndex >= (int)kx.size() ||
        keyIndex >= (int)ky.size() ||
        keyIndex >= (int)kz.size())
        return false;

    outInX = kx[keyIndex].inTangent;
    outOutX = kx[keyIndex].outTangent;
    outInY = ky[keyIndex].inTangent;
    outOutY = ky[keyIndex].outTangent;
    outInZ = kz[keyIndex].inTangent;
    outOutZ = kz[keyIndex].outTangent;
    return true;
}

bool Scene3DRenderer::getEditorScaleKeyTangents(int boneIndex, int keyIndex,
                                               float& outInX, float& outOutX,
                                               float& outInY, float& outOutY,
                                               float& outInZ, float& outOutZ) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return false;
    const std::vector<EditorFloatKey>& kx = m_editorScaleKeysX[boneIndex];
    const std::vector<EditorFloatKey>& ky = m_editorScaleKeysY[boneIndex];
    const std::vector<EditorFloatKey>& kz = m_editorScaleKeysZ[boneIndex];
    if (keyIndex < 0 ||
        keyIndex >= (int)kx.size() ||
        keyIndex >= (int)ky.size() ||
        keyIndex >= (int)kz.size())
        return false;

    outInX = kx[keyIndex].inTangent;
    outOutX = kx[keyIndex].outTangent;
    outInY = ky[keyIndex].inTangent;
    outOutY = ky[keyIndex].outTangent;
    outInZ = kz[keyIndex].inTangent;
    outOutZ = kz[keyIndex].outTangent;
    return true;
}

bool Scene3DRenderer::setEditorTransKeyTangents(int boneIndex, int keyIndex,
                                               float inX, float outX,
                                               float inY, float outY,
                                               float inZ, float outZ)
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return false;
    std::vector<EditorFloatKey>& kx = m_editorPosKeysX[boneIndex];
    std::vector<EditorFloatKey>& ky = m_editorPosKeysY[boneIndex];
    std::vector<EditorFloatKey>& kz = m_editorPosKeysZ[boneIndex];
    if (keyIndex < 0 ||
        keyIndex >= (int)kx.size() ||
        keyIndex >= (int)ky.size() ||
        keyIndex >= (int)kz.size())
        return false;

    kx[keyIndex].inTangent = inX;
    kx[keyIndex].outTangent = outX;
    ky[keyIndex].inTangent = inY;
    ky[keyIndex].outTangent = outY;
    kz[keyIndex].inTangent = inZ;
    kz[keyIndex].outTangent = outZ;
    return true;
}

bool Scene3DRenderer::setEditorScaleKeyTangents(int boneIndex, int keyIndex,
                                               float inX, float outX,
                                               float inY, float outY,
                                               float inZ, float outZ)
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return false;
    std::vector<EditorFloatKey>& kx = m_editorScaleKeysX[boneIndex];
    std::vector<EditorFloatKey>& ky = m_editorScaleKeysY[boneIndex];
    std::vector<EditorFloatKey>& kz = m_editorScaleKeysZ[boneIndex];
    if (keyIndex < 0 ||
        keyIndex >= (int)kx.size() ||
        keyIndex >= (int)ky.size() ||
        keyIndex >= (int)kz.size())
        return false;

    kx[keyIndex].inTangent = inX;
    kx[keyIndex].outTangent = outX;
    ky[keyIndex].inTangent = inY;
    ky[keyIndex].outTangent = outY;
    kz[keyIndex].inTangent = inZ;
    kz[keyIndex].outTangent = outZ;
    return true;
}

int Scene3DRenderer::getEditorTransKeyInterpolationMode(int boneIndex, int keyIndex) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return CURVE_LINEAR;
    const std::vector<EditorFloatKey>& kx = m_editorPosKeysX[boneIndex];
    if (keyIndex < 0 || keyIndex >= (int)kx.size())
        return CURVE_LINEAR;
    int mode = kx[keyIndex].interpMode;
    if (mode < CURVE_CONSTANT) mode = CURVE_CONSTANT;
    if (mode > CURVE_CUBIC) mode = CURVE_CUBIC;
    return mode;
}

int Scene3DRenderer::getEditorScaleKeyInterpolationMode(int boneIndex, int keyIndex) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return CURVE_LINEAR;
    const std::vector<EditorFloatKey>& kx = m_editorScaleKeysX[boneIndex];
    if (keyIndex < 0 || keyIndex >= (int)kx.size())
        return CURVE_LINEAR;
    int mode = kx[keyIndex].interpMode;
    if (mode < CURVE_CONSTANT) mode = CURVE_CONSTANT;
    if (mode > CURVE_CUBIC) mode = CURVE_CUBIC;
    return mode;
}

bool Scene3DRenderer::setEditorTransKeyInterpolationMode(int boneIndex, int keyIndex, int interpMode)
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return false;
    if (interpMode < CURVE_CONSTANT) interpMode = CURVE_CONSTANT;
    if (interpMode > CURVE_CUBIC) interpMode = CURVE_CUBIC;

    std::vector<EditorFloatKey>* curves[3] = { &m_editorPosKeysX[boneIndex], &m_editorPosKeysY[boneIndex], &m_editorPosKeysZ[boneIndex] };
    for (int c = 0; c < 3; ++c)
    {
        if (!curves[c]) continue;
        if (keyIndex < 0 || keyIndex >= (int)curves[c]->size())
            continue;
        (*curves[c])[keyIndex].interpMode = interpMode;
    }
    return true;
}

bool Scene3DRenderer::setEditorScaleKeyInterpolationMode(int boneIndex, int keyIndex, int interpMode)
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return false;
    if (interpMode < CURVE_CONSTANT) interpMode = CURVE_CONSTANT;
    if (interpMode > CURVE_CUBIC) interpMode = CURVE_CUBIC;

    std::vector<EditorFloatKey>* curves[3] = { &m_editorScaleKeysX[boneIndex], &m_editorScaleKeysY[boneIndex], &m_editorScaleKeysZ[boneIndex] };
    for (int c = 0; c < 3; ++c)
    {
        if (!curves[c]) continue;
        if (keyIndex < 0 || keyIndex >= (int)curves[c]->size())
            continue;
        (*curves[c])[keyIndex].interpMode = interpMode;
    }
    return true;
}
// ============================================================================
// Editor commit / cancel / pending
// ============================================================================

void Scene3DRenderer::editorCommitCurrent(float timeSeconds)
{
    ensureEditorArrays();
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return;
    }
    const int boneCount = m_gameModel->skeleton->m_numBones;
    if (m_editorLastLocalPose.getSize() < boneCount)
    {
        return;
    }

    for (int idx = 0; idx < boneCount; ++idx)
    {
        if (idx < m_editorOverrideRot.getSize() && !IsIdentityQuatApprox(m_editorOverrideRot[idx]))
        {
            hkQuaternion base = m_editorLastLocalPose[idx].getRotation();
            hkQuaternion finalRot;
            finalRot.setMul(base, m_editorOverrideRot[idx]);
            finalRot.normalize();
            recordEditorKey(idx, timeSeconds, finalRot);
            m_editorOverrideRot[idx].setIdentity();
        }

        if (idx < (int)m_editorOverrideTrans.size() && HasTransOverride(m_editorOverrideTrans[idx]))
        {
            float ox = m_editorOverrideTrans[idx].x;
            float oy = m_editorOverrideTrans[idx].y;
            float oz = m_editorOverrideTrans[idx].z;
            hkVector4 base = m_editorLastLocalPose[idx].getTranslation();
            base(0) += ox;
            base(1) += oy;
            base(2) += oz;
            recordEditorTransKey(idx, timeSeconds, base);
            m_editorOverrideTrans[idx].x = 0.0f;
            m_editorOverrideTrans[idx].y = 0.0f;
            m_editorOverrideTrans[idx].z = 0.0f;
        }
    }

    m_editorDragging = false;
    m_editorDraggingTrans = false;
}

void Scene3DRenderer::editorCancelCurrent()
{
    ensureEditorArrays();
    int idx = m_selectedBoneIndex;
    if (idx >= 0)
    {
        if (idx < m_editorOverrideRot.getSize())
        {
            m_editorOverrideRot[idx].setIdentity();
        }
        if (idx < (int)m_editorOverrideTrans.size())
        {
            m_editorOverrideTrans[idx].x = 0.0f;
            m_editorOverrideTrans[idx].y = 0.0f;
            m_editorOverrideTrans[idx].z = 0.0f;
        }
    }
    m_editorDragging = false;
    m_editorDraggingTrans = false;
}

bool Scene3DRenderer::hasSelectedBonePendingEdit() const
{
    int idx = m_selectedBoneIndex;
    if (idx < 0)
    {
        return false;
    }
    if (idx < m_editorOverrideRot.getSize() && !IsIdentityQuatApprox(m_editorOverrideRot[idx]))
    {
        return true;
    }
    if (idx < (int)m_editorOverrideTrans.size() && HasTransOverride(m_editorOverrideTrans[idx]))
    {
        return true;
    }
    return false;
}


// ============================================================================
// Bone TRS queries
// ============================================================================

bool Scene3DRenderer::getSelectedBoneLocalTRS(float& tx, float& ty, float& tz, float& rxDeg, float& ryDeg, float& rzDeg) const
{
    float sx = 1.0f, sy = 1.0f, sz = 1.0f;
    return getSelectedBoneLocalTRSScale(tx, ty, tz, rxDeg, ryDeg, rzDeg, sx, sy, sz);
}

bool Scene3DRenderer::getSelectedBoneLocalTRSScale(float& tx, float& ty, float& tz,
                                                   float& rxDeg, float& ryDeg, float& rzDeg,
                                                   float& sx, float& sy, float& sz) const
{
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return false;
    }
    int idx = m_selectedBoneIndex;
    if (idx < 0 || idx >= m_gameModel->skeleton->m_numBones || idx >= m_editorLastLocalPose.getSize())
    {
        return false;
    }
    const hkQsTransform& t = m_editorLastLocalPose[idx];
    hkVector4 v = t.getTranslation();
    hkVector4 s = t.getScale();
    tx = v(0);
    ty = v(1);
    tz = v(2);
    sx = s(0);
    sy = s(1);
    sz = s(2);
    QuatToEulerDegreesXYZ(t.getRotation(), rxDeg, ryDeg, rzDeg);
    return true;
}

bool Scene3DRenderer::keySelectedBoneLocalTRS(float tx, float ty, float tz, float rxDeg, float ryDeg, float rzDeg, float timeSeconds, bool keyRot, bool keyTrans)
{
    return keySelectedBoneLocalTRSScale(tx, ty, tz, rxDeg, ryDeg, rzDeg, 1.0f, 1.0f, 1.0f, timeSeconds, keyRot, keyTrans, false);
}

bool Scene3DRenderer::keySelectedBoneLocalTRSScale(float tx, float ty, float tz,
                                                   float rxDeg, float ryDeg, float rzDeg,
                                                   float sx, float sy, float sz,
                                                   float timeSeconds, bool keyRot, bool keyTrans, bool keyScale)
{
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return false;
    }
    int idx = m_selectedBoneIndex;
    if (idx < 0 || idx >= m_gameModel->skeleton->m_numBones)
    {
        return false;
    }
    ensureEditorArrays();
    if (keyRot)
    {
        hkQuaternion q = EulerDegreesToQuatXYZ(rxDeg, ryDeg, rzDeg);
        recordEditorKey(idx, timeSeconds, q);
    }
    if (keyTrans)
    {
        hkVector4 t;
        t.set(tx, ty, tz);
        recordEditorTransKey(idx, timeSeconds, t);
    }
    if (keyScale)
    {
        hkVector4 s;
        s.set(sx, sy, sz);
        recordEditorScaleKey(idx, timeSeconds, s);
    }
    return keyRot || keyTrans || keyScale;
}

// ============================================================================
// Skeleton queries
// ============================================================================

int Scene3DRenderer::getSkeletonBoneCount() const
{
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return 0;
    }
    return m_gameModel->skeleton->m_numBones;
}

const char* Scene3DRenderer::getSkeletonBoneName(int index) const
{
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return NULL;
    }
    if (index < 0 || index >= m_gameModel->skeleton->m_numBones)
    {
        return NULL;
    }
    const hkaBone* bone = m_gameModel->skeleton->m_bones[index];
    return bone ? bone->m_name : NULL;
}

int Scene3DRenderer::getSkeletonParentIndex(int index) const
{
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return -1;
    }
    if (index < 0 || index >= m_gameModel->skeleton->m_numBones)
    {
        return -1;
    }
    if (m_gameModel->skeleton->m_parentIndices)
    {
        return m_gameModel->skeleton->m_parentIndices[index];
    }
    return -1;
}

const hkaSkeleton* Scene3DRenderer::getModelSkeleton() const
{
    if (!m_gameModel)
    {
        return NULL;
    }
    return m_gameModel->skeleton;
}

float Scene3DRenderer::getCreatureVar(int index, int source) const
{
    if (index < 0 || index >= 64)
    {
        return 0.0f;
    }
    return m_creatureVars[index];
}

void Scene3DRenderer::setCreatureVar(int index, int source, float value)
{
    if (index < 0 || index >= 64)
    {
        return;
    }
    m_creatureVars[index] = value;
}

void Scene3DRenderer::setCreatureDataNamed(const char* name, float value)
{
    if (!name || !name[0])
        return;
    m_creatureData[name] = value;
}

float Scene3DRenderer::getCreatureDataNamed(const char* name) const
{
    if (!name || !name[0])
        return 0.0f;
    std::map<std::string, float>::const_iterator it = m_creatureData.find(name);
    if (it != m_creatureData.end())
        return it->second;
    return 0.0f;
}

float Scene3DRenderer::getLocalVar(const char* name) const
{
    if (!name || !name[0])
        return 0.0f;
    std::map<std::string, float>::const_iterator it = m_localVars.find(name);
    if (it != m_localVars.end())
        return it->second;
    return 0.0f;
}

void Scene3DRenderer::setLocalVar(const char* name, float value)
{
    if (!name || !name[0])
        return;
    m_localVars[name] = value;
}

// ============================================================================
// Editor recording state
// ============================================================================

void Scene3DRenderer::setEditorRecording(bool recording)
{
    m_editorRecording = recording;
}

bool Scene3DRenderer::isEditorRecording() const
{
    return m_editorRecording;
}

void Scene3DRenderer::ensureEditorArrays()
{
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return;
    }
    int boneCount = m_gameModel->skeleton->m_numBones;
    if ((int)m_editorRotKeys.size() != boneCount)
    {
        m_editorRotKeys.clear();
        m_editorRotKeys.resize(boneCount);
    }
    if ((int)m_editorPosKeysX.size() != boneCount ||
        (int)m_editorPosKeysY.size() != boneCount ||
        (int)m_editorPosKeysZ.size() != boneCount)
    {
        m_editorPosKeysX.clear();
        m_editorPosKeysY.clear();
        m_editorPosKeysZ.clear();
        m_editorPosKeysX.resize(boneCount);
        m_editorPosKeysY.resize(boneCount);
        m_editorPosKeysZ.resize(boneCount);
    }
    if ((int)m_editorScaleKeysX.size() != boneCount ||
        (int)m_editorScaleKeysY.size() != boneCount ||
        (int)m_editorScaleKeysZ.size() != boneCount)
    {
        m_editorScaleKeysX.clear();
        m_editorScaleKeysY.clear();
        m_editorScaleKeysZ.clear();
        m_editorScaleKeysX.resize(boneCount);
        m_editorScaleKeysY.resize(boneCount);
        m_editorScaleKeysZ.resize(boneCount);
    }
    if (m_editorOverrideRot.getSize() != boneCount)
    {
        m_editorOverrideRot.setSize(boneCount);
        for (int i = 0; i < boneCount; i++)
        {
            m_editorOverrideRot[i].setIdentity();
        }
    }
    if (m_editorLastLocalPose.getSize() != boneCount)
    {
        m_editorLastLocalPose.setSize(boneCount);
    }
    if ((int)m_editorOverrideTrans.size() != boneCount)
    {
        m_editorOverrideTrans.clear();
        m_editorOverrideTrans.resize(boneCount);
        for (int i = 0; i < boneCount; i++)
        {
            m_editorOverrideTrans[i].x = 0.0f;
            m_editorOverrideTrans[i].y = 0.0f;
            m_editorOverrideTrans[i].z = 0.0f;
        }
    }
}

// ============================================================================
// Key recording
// ============================================================================

void Scene3DRenderer::recordEditorKey(int boneIndex, float timeSeconds, const hkQuaternion& rot)
{
    if (boneIndex < 0) return;
    ensureEditorArrays();
    float ft = (m_editorFrameTime > 0.0f) ? m_editorFrameTime : (1.0f / 30.0f);
    int frame = (int)floor((timeSeconds / ft) + 0.5f);
    if (frame < 0) frame = 0;
    float timeMs = timeSeconds * 1000.0f;

    std::vector<Scene3DRenderer::EditorKey>& keys = m_editorRotKeys[boneIndex];
    for (size_t i = 0; i < keys.size(); i++)
    {
        if (keys[i].frame == frame)
        {
            StoreEditorQuat(keys[i], rot);
            keys[i].timeMs = timeMs;
            return;
        }
        if (keys[i].frame > frame)
        {
            Scene3DRenderer::EditorKey k;
            k.frame = frame;
            k.timeMs = timeMs;
            StoreEditorQuat(k, rot);
            // Initialize easing from editor defaults
            k.easingType = m_editorDefaultEasingType;
            k.easingCp1x = m_editorDefaultEasingCp1x;
            k.easingCp1y = m_editorDefaultEasingCp1y;
            k.easingCp2x = m_editorDefaultEasingCp2x;
            k.easingCp2y = m_editorDefaultEasingCp2y;
            keys.insert(keys.begin() + (int)i, k);
            return;
        }
    }
    Scene3DRenderer::EditorKey k;
    k.frame = frame;
    k.timeMs = timeMs;
    StoreEditorQuat(k, rot);
    // Initialize easing from editor defaults
    k.easingType = m_editorDefaultEasingType;
    k.easingCp1x = m_editorDefaultEasingCp1x;
    k.easingCp1y = m_editorDefaultEasingCp1y;
    k.easingCp2x = m_editorDefaultEasingCp2x;
    k.easingCp2y = m_editorDefaultEasingCp2y;
    keys.push_back(k);
}

void Scene3DRenderer::recordEditorTransKey(int boneIndex, float timeSeconds, const hkVector4& t)
{
    if (boneIndex < 0) return;
    ensureEditorArrays();
    float ft = (m_editorFrameTime > 0.0f) ? m_editorFrameTime : (1.0f / 30.0f);
    int frame = (int)floor((timeSeconds / ft) + 0.5f);
    if (frame < 0) frame = 0;
    float timeMs = timeSeconds * 1000.0f;

    std::vector<EditorFloatKey>& kx = m_editorPosKeysX[boneIndex];
    std::vector<EditorFloatKey>& ky = m_editorPosKeysY[boneIndex];
    std::vector<EditorFloatKey>& kz = m_editorPosKeysZ[boneIndex];

    // Keep XYZ curves aligned by inserting/updating keys at the same indices.
    for (size_t i = 0; i < kx.size(); i++)
    {
        if (kx[i].frame == frame)
        {
            kx[i].value = t(0); kx[i].timeMs = timeMs;
            if (i < ky.size()) { ky[i].value = t(1); ky[i].timeMs = timeMs; }
            if (i < kz.size()) { kz[i].value = t(2); kz[i].timeMs = timeMs; }
            RecomputeAutoTangents(kx);
            RecomputeAutoTangents(ky);
            RecomputeAutoTangents(kz);
            return;
        }
        if (kx[i].frame > frame)
        {
            EditorFloatKey ax;
            ax.frame = frame;
            ax.timeMs = timeMs;
            ax.value = t(0);
            ax.inTangent = 0.0f;
            ax.outTangent = 0.0f;
            ax.interpMode = CURVE_CUBIC;
            ax.easingType = m_editorDefaultEasingType;
            ax.easingCp1x = m_editorDefaultEasingCp1x;
            ax.easingCp1y = m_editorDefaultEasingCp1y;
            ax.easingCp2x = m_editorDefaultEasingCp2x;
            ax.easingCp2y = m_editorDefaultEasingCp2y;

            EditorFloatKey ay = ax; ay.value = t(1);
            EditorFloatKey az = ax; az.value = t(2);

            kx.insert(kx.begin() + (int)i, ax);
            ky.insert(ky.begin() + (int)i, ay);
            kz.insert(kz.begin() + (int)i, az);
            RecomputeAutoTangents(kx);
            RecomputeAutoTangents(ky);
            RecomputeAutoTangents(kz);
            return;
        }
    }
    EditorFloatKey ax;
    ax.frame = frame;
    ax.timeMs = timeMs;
    ax.value = t(0);
    ax.inTangent = 0.0f;
    ax.outTangent = 0.0f;
    ax.interpMode = CURVE_CUBIC;
    ax.easingType = m_editorDefaultEasingType;
    ax.easingCp1x = m_editorDefaultEasingCp1x;
    ax.easingCp1y = m_editorDefaultEasingCp1y;
    ax.easingCp2x = m_editorDefaultEasingCp2x;
    ax.easingCp2y = m_editorDefaultEasingCp2y;

    EditorFloatKey ay = ax; ay.value = t(1);
    EditorFloatKey az = ax; az.value = t(2);

    kx.push_back(ax);
    ky.push_back(ay);
    kz.push_back(az);
    RecomputeAutoTangents(kx);
    RecomputeAutoTangents(ky);
    RecomputeAutoTangents(kz);
}

void Scene3DRenderer::recordEditorScaleKey(int boneIndex, float timeSeconds, const hkVector4& s)
{
    if (boneIndex < 0) return;
    ensureEditorArrays();
    float ft = (m_editorFrameTime > 0.0f) ? m_editorFrameTime : (1.0f / 30.0f);
    int frame = (int)floor((timeSeconds / ft) + 0.5f);
    if (frame < 0) frame = 0;
    float timeMs = timeSeconds * 1000.0f;

    std::vector<EditorFloatKey>& kx = m_editorScaleKeysX[boneIndex];
    std::vector<EditorFloatKey>& ky = m_editorScaleKeysY[boneIndex];
    std::vector<EditorFloatKey>& kz = m_editorScaleKeysZ[boneIndex];

    for (size_t i = 0; i < kx.size(); i++)
    {
        if (kx[i].frame == frame)
        {
            kx[i].value = s(0); kx[i].timeMs = timeMs;
            if (i < ky.size()) { ky[i].value = s(1); ky[i].timeMs = timeMs; }
            if (i < kz.size()) { kz[i].value = s(2); kz[i].timeMs = timeMs; }
            RecomputeAutoTangents(kx);
            RecomputeAutoTangents(ky);
            RecomputeAutoTangents(kz);
            return;
        }
        if (kx[i].frame > frame)
        {
            EditorFloatKey ax;
            ax.frame = frame;
            ax.timeMs = timeMs;
            ax.value = s(0);
            ax.inTangent = 0.0f;
            ax.outTangent = 0.0f;
            ax.interpMode = CURVE_CUBIC;
            ax.easingType = m_editorDefaultEasingType;
            ax.easingCp1x = m_editorDefaultEasingCp1x;
            ax.easingCp1y = m_editorDefaultEasingCp1y;
            ax.easingCp2x = m_editorDefaultEasingCp2x;
            ax.easingCp2y = m_editorDefaultEasingCp2y;

            EditorFloatKey ay = ax; ay.value = s(1);
            EditorFloatKey az = ax; az.value = s(2);

            kx.insert(kx.begin() + (int)i, ax);
            ky.insert(ky.begin() + (int)i, ay);
            kz.insert(kz.begin() + (int)i, az);
            RecomputeAutoTangents(kx);
            RecomputeAutoTangents(ky);
            RecomputeAutoTangents(kz);
            return;
        }
    }
    EditorFloatKey ax;
    ax.frame = frame;
    ax.timeMs = timeMs;
    ax.value = s(0);
    ax.inTangent = 0.0f;
    ax.outTangent = 0.0f;
    ax.interpMode = CURVE_CUBIC;
    ax.easingType = m_editorDefaultEasingType;
    ax.easingCp1x = m_editorDefaultEasingCp1x;
    ax.easingCp1y = m_editorDefaultEasingCp1y;
    ax.easingCp2x = m_editorDefaultEasingCp2x;
    ax.easingCp2y = m_editorDefaultEasingCp2y;

    EditorFloatKey ay = ax; ay.value = s(1);
    EditorFloatKey az = ax; az.value = s(2);

    kx.push_back(ax);
    ky.push_back(ay);
    kz.push_back(az);
    RecomputeAutoTangents(kx);
    RecomputeAutoTangents(ky);
    RecomputeAutoTangents(kz);
}

// ============================================================================
// Rotation drag operations
// ============================================================================

void Scene3DRenderer::editorBeginDrag()
{
    ensureEditorArrays();
    pushUndoSnapshot();
    m_editorDragging = true;
    m_editorRotateAxisLock = AXIS_FREE;
    m_rotateSnapAccum = 0.0f;
    m_rotateSnapApplied = 0.0f;
    m_rotateSnapAxis = 0;
}

void Scene3DRenderer::editorUpdateDrag(float dx, float dy, int axisLock)
{
    if (!m_editorDragging) return;
    ensureEditorArrays();
    int idx = m_selectedBoneIndex;
    if (idx < 0 || idx >= m_editorOverrideRot.getSize()) return;

    if (axisLock < 0 || axisLock > 3) axisLock = 0;
    m_editorRotateAxisLock = static_cast<AxisLock>(axisLock);

    hkQuaternion delta;
    if (axisLock == AXIS_FREE)
    {
        float yaw = dx * 0.01f;
        float pitch = dy * 0.01f;

        hkVector4 axisX; axisX.set(1.0f, 0.0f, 0.0f);
        hkVector4 axisY; axisY.set(0.0f, 1.0f, 0.0f);
        hkQuaternion qx(axisX, pitch);
        hkQuaternion qy(axisY, yaw);
        delta.setMul(qy, qx);
        delta.normalize();
    }
    else
    {
        if (m_rotateSnapAxis != axisLock)
        {
            m_rotateSnapAxis = axisLock;
            m_rotateSnapAccum = 0.0f;
            m_rotateSnapApplied = 0.0f;
        }

        float primary = (fabsf(dx) >= fabsf(dy)) ? dx : -dy;
        float angle = primary * 0.01f;
        bool snapBypass = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0; // Alt to bypass snap
        if (m_rotateSnapEnabled && !snapBypass)
        {
            float stepRad = m_rotateSnapDegrees * 0.0174532925f;
            float stepPrimary = stepRad / 0.01f;
            m_rotateSnapAccum += primary;
            float snappedPrimaryDelta = ComputeSnappedDelta(m_rotateSnapAccum, m_rotateSnapApplied, stepPrimary);
            if (fabsf(snappedPrimaryDelta) < 1e-6f)
            {
                return;
            }
            angle = snappedPrimaryDelta * 0.01f;
        }
        hkVector4 axis;
        if (axisLock == AXIS_X) axis.set(1.0f, 0.0f, 0.0f);
        else if (axisLock == AXIS_Y) axis.set(0.0f, 1.0f, 0.0f);
        else axis.set(0.0f, 0.0f, 1.0f);
        delta.setAxisAngle(axis, angle);
        delta.normalize();
    }

    hkQuaternion next;
    next.setMul(delta, m_editorOverrideRot[idx]);
    next.normalize();
    m_editorOverrideRot[idx] = next;
}

void Scene3DRenderer::editorEndDrag(float timeSeconds)
{
    if (!m_editorDragging) return;
    ensureEditorArrays();
    int idx = m_selectedBoneIndex;
    if (idx >= 0 && idx < m_editorOverrideRot.getSize())
    {
        if (m_editorRecording &&
            idx < m_editorLastLocalPose.getSize() &&
            !IsIdentityQuatApprox(m_editorOverrideRot[idx]))
        {
            hkQuaternion base = m_editorLastLocalPose[idx].getRotation();
            hkQuaternion finalRot;
            finalRot.setMul(base, m_editorOverrideRot[idx]);
            finalRot.normalize();
            recordEditorKey(idx, timeSeconds, finalRot);
            m_editorOverrideRot[idx].setIdentity();
        }
    }
    m_editorDragging = false;
    m_editorRotateAxisLock = AXIS_FREE;
}

// ============================================================================
// Translation drag operations
// ============================================================================

void Scene3DRenderer::editorBeginTranslate()
{
    ensureEditorArrays();
    pushUndoSnapshot();
    m_editorDraggingTrans = true;
    m_editorTranslateAxisLock = AXIS_FREE;
    m_moveSnapAccum[0] = m_moveSnapAccum[1] = m_moveSnapAccum[2] = 0.0f;
    m_moveSnapApplied[0] = m_moveSnapApplied[1] = m_moveSnapApplied[2] = 0.0f;
    m_moveSnapAxis = 0;
}

void Scene3DRenderer::editorUpdateTranslate(float dx, float dy, float dz, int axisLock)
{
    if (!m_editorDraggingTrans) return;
    ensureEditorArrays();
    int idx = m_selectedBoneIndex;
    if (idx < 0 || idx >= (int)m_editorOverrideTrans.size()) return;

    if (axisLock < 0 || axisLock > 3) axisLock = 0;
    m_editorTranslateAxisLock = static_cast<AxisLock>(axisLock);

    const float scale = 0.005f;
    float addX = 0.0f;
    float addY = 0.0f;
    float addZ = 0.0f;
    if (axisLock == AXIS_X)
    {
        float primary = (fabsf(dx) >= fabsf(dy)) ? dx : -dy;
        addX = primary * scale;
    }
    else if (axisLock == AXIS_Y)
    {
        float primary = (fabsf(dx) >= fabsf(dy)) ? dx : -dy;
        addY = primary * scale;
    }
    else if (axisLock == AXIS_Z)
    {
        float primary = (fabsf(dx) >= fabsf(dy)) ? dx : -dy;
        addZ = primary * scale;
    }
    else
    {
        addX = dx * scale;
        addY = dy * scale;
        addZ = dz * scale;
    }

    bool snapBypass = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0; // Alt disables snap
    if (m_moveSnapEnabled && !snapBypass)
    {
        if (m_moveSnapAxis != axisLock)
        {
            m_moveSnapAxis = axisLock;
            m_moveSnapAccum[0] = m_moveSnapAccum[1] = m_moveSnapAccum[2] = 0.0f;
            m_moveSnapApplied[0] = m_moveSnapApplied[1] = m_moveSnapApplied[2] = 0.0f;
        }

        m_moveSnapAccum[0] += addX;
        m_moveSnapAccum[1] += addY;
        m_moveSnapAccum[2] += addZ;
        addX = ComputeSnappedDelta(m_moveSnapAccum[0], m_moveSnapApplied[0], m_moveSnapUnits);
        addY = ComputeSnappedDelta(m_moveSnapAccum[1], m_moveSnapApplied[1], m_moveSnapUnits);
        addZ = ComputeSnappedDelta(m_moveSnapAccum[2], m_moveSnapApplied[2], m_moveSnapUnits);
    }

    m_editorOverrideTrans[idx].x += addX;
    m_editorOverrideTrans[idx].y += addY;
    m_editorOverrideTrans[idx].z += addZ;
}

void Scene3DRenderer::editorEndTranslate(float timeSeconds)
{
    if (!m_editorDraggingTrans) return;
    ensureEditorArrays();
    int idx = m_selectedBoneIndex;
    if (idx >= 0 && idx < (int)m_editorOverrideTrans.size())
    {
        if (m_editorRecording &&
            idx < m_editorLastLocalPose.getSize() &&
            HasTransOverride(m_editorOverrideTrans[idx]))
        {
            hkVector4 base = m_editorLastLocalPose[idx].getTranslation();
            base(0) += m_editorOverrideTrans[idx].x;
            base(1) += m_editorOverrideTrans[idx].y;
            base(2) += m_editorOverrideTrans[idx].z;
            recordEditorTransKey(idx, timeSeconds, base);
            m_editorOverrideTrans[idx].x = 0.0f;
            m_editorOverrideTrans[idx].y = 0.0f;
            m_editorOverrideTrans[idx].z = 0.0f;
        }
    }
    m_editorDraggingTrans = false;
    m_moveSnapAxis = 0;
    m_editorTranslateAxisLock = AXIS_FREE;
}

// ============================================================================
// Export editor clip
// ============================================================================

struct ExportScaleKey
{
    int frame;
    float sx, sy, sz;
};

struct ExportScaleKeyLess
{
    bool operator()(const ExportScaleKey& a, const ExportScaleKey& b) const { return a.frame < b.frame; }
};

static void EncodeScaleType2Block(const std::vector<ExportScaleKey>& keys, int blockBase, JsonType2Block& out)
{
    out = JsonType2Block();
    if (keys.empty())
        return;

    float minX = keys[0].sx, maxX = keys[0].sx;
    float minY = keys[0].sy, maxY = keys[0].sy;
    float minZ = keys[0].sz, maxZ = keys[0].sz;
    for (size_t i = 1; i < keys.size(); ++i)
    {
        if (keys[i].sx < minX) minX = keys[i].sx;
        if (keys[i].sx > maxX) maxX = keys[i].sx;
        if (keys[i].sy < minY) minY = keys[i].sy;
        if (keys[i].sy > maxY) maxY = keys[i].sy;
        if (keys[i].sz < minZ) minZ = keys[i].sz;
        if (keys[i].sz > maxZ) maxZ = keys[i].sz;
    }

    const int frameCount = (int)keys.size();
    out.flags = 0x70; // animate X/Y/Z
    out.s1 = frameCount - 1;
    out.s2 = 0;

    out.data.clear();
    out.data.reserve(frameCount + 4);
    out.data.push_back(0);
    out.data.push_back(0);
    out.data.push_back(0);
    out.data.push_back(0);
    for (int i = 0; i < frameCount; ++i)
    {
        out.data.push_back(keys[i].frame - blockBase);
    }

    out.valsA.clear();
    out.valsA.push_back(minX);
    out.valsA.push_back(maxX);
    out.valsA.push_back(minY);
    out.valsA.push_back(maxY);
    out.valsA.push_back(minZ);
    out.valsA.push_back(maxZ);

    out.valsType2.clear();
    out.valsType2.resize(frameCount * 3);
    const float rangeX = maxX - minX;
    const float rangeY = maxY - minY;
    const float rangeZ = maxZ - minZ;
    for (int i = 0; i < frameCount; ++i)
    {
        float nx = (rangeX > 1e-9f) ? ((keys[i].sx - minX) / rangeX) : 0.0f;
        float ny = (rangeY > 1e-9f) ? ((keys[i].sy - minY) / rangeY) : 0.0f;
        float nz = (rangeZ > 1e-9f) ? ((keys[i].sz - minZ) / rangeZ) : 0.0f;

        if (nx < 0.0f) nx = 0.0f; if (nx > 1.0f) nx = 1.0f;
        if (ny < 0.0f) ny = 0.0f; if (ny > 1.0f) ny = 1.0f;
        if (nz < 0.0f) nz = 0.0f; if (nz > 1.0f) nz = 1.0f;

        int qx = (int)(nx * 65535.0f + 0.5f);
        int qy = (int)(ny * 65535.0f + 0.5f);
        int qz = (int)(nz * 65535.0f + 0.5f);
        if (qx < 0) qx = 0; if (qx > 65535) qx = 65535;
        if (qy < 0) qy = 0; if (qy > 65535) qy = 65535;
        if (qz < 0) qz = 0; if (qz > 65535) qz = 65535;
        out.valsType2[i * 3 + 0] = (unsigned short)qx;
        out.valsType2[i * 3 + 1] = (unsigned short)qy;
        out.valsType2[i * 3 + 2] = (unsigned short)qz;
    }
}

bool Scene3DRenderer::exportEditorClip(const char* path)
{
    if (!path || !m_gameModel || !m_gameModel->skeleton)
    {
        return false;
    }

    ensureEditorArrays();
    const char* logPath = "export_anim_log.txt";
    FILE* exportLog = fopen(logPath, "w");
    if (exportLog)
    {
        fprintf(exportLog, "Export Log: %s\n", path);
    }

    struct ExportKey
    {
        int frame;
        JsonTrack::Quat4 q;
    };

    std::vector<std::string> exportBoneNames;
    if (m_jsonAnim && !m_jsonAnim->boneNames.empty())
    {
        exportBoneNames = m_jsonAnim->boneNames;
    }
    else
    {
        exportBoneNames.reserve(m_gameModel->skeleton->m_numBones);
        for (int i = 0; i < m_gameModel->skeleton->m_numBones; i++)
        {
            const hkaBone* bone = m_gameModel->skeleton->m_bones[i];
            exportBoneNames.push_back((bone && bone->m_name) ? bone->m_name : "");
        }
    }

    int boneCount = (int)exportBoneNames.size();
    std::vector<int> exportBoneToSkeleton;
    exportBoneToSkeleton.assign(boneCount, -1);
    for (int i = 0; i < boneCount; i++)
    {
        const std::string& name = exportBoneNames[i];
        if (!name.empty())
        {
            exportBoneToSkeleton[i] = FindSkeletonBoneIndex(m_gameModel->skeleton, name.c_str());
            if (exportBoneToSkeleton[i] < 0)
            {
                char msg[256];
                sprintf_s(msg, "WARN: Export bone '%s' not found in skeleton", name.c_str());
                RendererLog(msg);
            }
        }
    }
    bool useSourceFrames = (m_jsonAnim && m_jsonAnim->frameCount > 0);
    int maxFrame = -1;
    int skeletonBoneCount = m_gameModel->skeleton->m_numBones;
    bool hasAnyEditorKeys = false;
    std::vector<int> allEditFrames;
    for (int i = 0; i < skeletonBoneCount; ++i)
    {
        const char* boneName = (m_gameModel->skeleton->m_bones[i] && m_gameModel->skeleton->m_bones[i]->m_name)
                                   ? m_gameModel->skeleton->m_bones[i]->m_name
                                   : "(unknown)";
        if (i < (int)m_editorRotKeys.size() && !m_editorRotKeys[i].empty())
        {
            hasAnyEditorKeys = true;
            for (size_t k = 0; k < m_editorRotKeys[i].size(); k++)
            {
                allEditFrames.push_back(m_editorRotKeys[i][k].frame);
                if (m_editorRotKeys[i][k].frame > maxFrame)
                {
                    maxFrame = m_editorRotKeys[i][k].frame;
                }
            }
            if (exportLog)
            {
                fprintf(exportLog, "RotKeys %s (idx %d): ", boneName, i);
                for (size_t k = 0; k < m_editorRotKeys[i].size(); k++)
                {
                    fprintf(exportLog, "%d%s", m_editorRotKeys[i][k].frame,
                            (k + 1 < m_editorRotKeys[i].size()) ? "," : "");
                }
                fprintf(exportLog, "\n");
            }
        }
        if (i < (int)m_editorPosKeysX.size() && !m_editorPosKeysX[i].empty())
        {
            hasAnyEditorKeys = true;
            for (size_t k = 0; k < m_editorPosKeysX[i].size(); k++)
            {
                allEditFrames.push_back(m_editorPosKeysX[i][k].frame);
                if (m_editorPosKeysX[i][k].frame > maxFrame)
                {
                    maxFrame = m_editorPosKeysX[i][k].frame;
                }
            }
            if (exportLog)
            {
                fprintf(exportLog, "TransKeys %s (idx %d): ", boneName, i);
                for (size_t k = 0; k < m_editorPosKeysX[i].size(); k++)
                {
                    fprintf(exportLog, "%d%s", m_editorPosKeysX[i][k].frame,
                            (k + 1 < m_editorPosKeysX[i].size()) ? "," : "");
                }
                fprintf(exportLog, "\n");
            }
        }
        if (i < (int)m_editorScaleKeysX.size() && !m_editorScaleKeysX[i].empty())
        {
            hasAnyEditorKeys = true;
            for (size_t k = 0; k < m_editorScaleKeysX[i].size(); k++)
            {
                allEditFrames.push_back(m_editorScaleKeysX[i][k].frame);
                if (m_editorScaleKeysX[i][k].frame > maxFrame)
                {
                    maxFrame = m_editorScaleKeysX[i][k].frame;
                }
            }
            if (exportLog)
            {
                fprintf(exportLog, "ScaleKeys %s (idx %d): ", boneName, i);
                for (size_t k = 0; k < m_editorScaleKeysX[i].size(); k++)
                {
                    fprintf(exportLog, "%d%s", m_editorScaleKeysX[i][k].frame,
                            (k + 1 < m_editorScaleKeysX[i].size()) ? "," : "");
                }
                fprintf(exportLog, "\n");
            }
        }
    }

    if (hasAnyEditorKeys)
    {
        std::sort(allEditFrames.begin(), allEditFrames.end());
        allEditFrames.erase(std::unique(allEditFrames.begin(), allEditFrames.end()), allEditFrames.end());
        if (exportLog)
        {
            fprintf(exportLog, "Unique edited frames: %d\n", (int)allEditFrames.size());
        }
        if ((int)allEditFrames.size() <= 1 && !useSourceFrames)
        {
            if (exportLog)
            {
                fprintf(exportLog, "ERROR: Only one frame recorded without source animation. Export aborted.\n");
            }
            if (exportLog) fclose(exportLog);
            return false;
        }
        else if ((int)allEditFrames.size() <= 1 && exportLog)
        {
            fprintf(exportLog, "WARN: Single edited frame; source data will be preserved for stability.\n");
        }
    }

    float frameTime = (m_jsonAnim && m_jsonAnim->frameTime > 0.0f) ? m_jsonAnim->frameTime :
                      ((m_editorFrameTime > 0.0f) ? m_editorFrameTime : (1.0f / 30.0f));
    int sourceFrameCount = useSourceFrames ? m_jsonAnim->frameCount : 0;
    int frameCount = sourceFrameCount;
    if (frameCount <= 0 && maxFrame >= 0)
    {
        frameCount = maxFrame + 1;
    }
    if (maxFrame >= 0 && frameCount < (maxFrame + 1))
    {
        frameCount = maxFrame + 1;
    }
    if (m_editorTimelineDuration > 0.0f && frameTime > 0.0f)
    {
        int timelineFrames = (int)floor((m_editorTimelineDuration / frameTime) + 0.5f) + 1;
        if (timelineFrames > frameCount)
        {
            frameCount = timelineFrames;
        }
    }
    if (frameCount <= 0)
    {
        if (exportLog) fclose(exportLog);
        return false;
    }
    float duration = 0.0f;
    if (m_editorTimelineDuration > 0.0f)
    {
        duration = m_editorTimelineDuration;
    }
    else if (useSourceFrames && m_jsonAnim->duration > 0.0f && frameCount == sourceFrameCount)
    {
        duration = m_jsonAnim->duration;
    }
    else
    {
        duration = (frameCount > 1) ? frameTime * (float)(frameCount - 1) : frameTime;
    }
    int maxFramesPerBlock = (m_jsonAnim && m_jsonAnim->maxFramesPerBlock > 0) ? m_jsonAnim->maxFramesPerBlock : 256;
    int blockCount = (frameCount + maxFramesPerBlock - 1) / maxFramesPerBlock;

    if (exportLog)
    {
        fprintf(exportLog, "Export params: sourceFrameCount=%d, frameCount=%d, duration=%.4f, "
                "editorTimelineDuration=%.4f, frameTime=%.6f, blockCount=%d, maxFramesPerBlock=%d\n",
                sourceFrameCount, frameCount, duration,
                m_editorTimelineDuration, frameTime, blockCount, maxFramesPerBlock);
    }

    std::vector<int> sourceTrackByBone;
    std::vector<int> sourceScaleTrackByBone;
    if (m_jsonAnim)
    {
        sourceTrackByBone.assign(skeletonBoneCount, -1);
        for (size_t i = 0; i < m_jsonAnim->tracks.size(); i++)
        {
            int boneIdx = m_jsonAnim->tracks[i].boneIndex;
            if (boneIdx >= 0 && boneIdx < skeletonBoneCount)
            {
                if (sourceTrackByBone[boneIdx] < 0)
                {
                    sourceTrackByBone[boneIdx] = (int)i;
                }
            }
        }

        sourceScaleTrackByBone.assign(skeletonBoneCount, -1);
        for (size_t i = 0; i < m_jsonAnim->scaleTracks.size(); i++)
        {
            int boneIdx = m_jsonAnim->scaleTracks[i].boneIndex;
            if (boneIdx >= 0 && boneIdx < skeletonBoneCount)
            {
                if (sourceScaleTrackByBone[boneIdx] < 0)
                {
                    sourceScaleTrackByBone[boneIdx] = (int)i;
                }
            }
        }
    }

    // Check if source and destination are the same file
    bool isSameFile = false;
    if (m_jsonAnimPath[0] != '\0')
    {
        char srcFull[MAX_PATH] = {0};
        char dstFull[MAX_PATH] = {0};
        GetFullPathNameA(m_jsonAnimPath, MAX_PATH, srcFull, NULL);
        GetFullPathNameA(path, MAX_PATH, dstFull, NULL);
        isSameFile = (_stricmp(srcFull, dstFull) == 0);
    }

    // Check if any blend layers are active — if so, use bake export (evaluates full pipeline)
    bool hasActiveBlendLayers = false;
    for (int li = 0; li < MAX_BLEND_LAYERS; ++li)
    {
        if (m_blendLayers[li].active && m_blendLayers[li].clip && m_blendLayers[li].weight > 0.0f)
        {
            hasActiveBlendLayers = true;
            break;
        }
    }
    // Also check primary blend
    if (m_jsonBlendEnabled && m_useJsonBlendAnim && m_jsonBlendAnim && m_jsonBlendAlpha > 0.0f)
        hasActiveBlendLayers = true;

    // Saved editor keys for restore after bake export (declared at function scope)
    std::vector<std::vector<EditorKey> > savedRotKeys;
    std::vector<std::vector<EditorFloatKey> > savedPosKeysX, savedPosKeysY, savedPosKeysZ;
    std::vector<std::vector<EditorFloatKey> > savedScaleKeysX, savedScaleKeysY, savedScaleKeysZ;

    // When blend layers are active, force the existing export to use baked poses.
    // We pre-bake ALL frames by evaluating buildPoseFromJson, storing the result
    // in m_editorRotKeys so the normal export path picks them up as editor keys.
    // This way events, metadata, obj2, etc. are all handled by the existing code.
    if (hasActiveBlendLayers && m_jsonAnim && m_gameModel && m_gameModel->skeleton)
    {
        if (exportLog)
            fprintf(exportLog, "BAKE: injecting blended poses as editor keys for export.\n");

        float bakeFrameTime = (m_jsonAnim->frameTime > 0.0f) ? m_jsonAnim->frameTime : (1.0f / 30.0f);
        int bakeFrameCount = (m_jsonAnim->frameCount > 0) ? m_jsonAnim->frameCount : 1;
        if (m_editorTimelineDuration > 0.0f)
        {
            int tlFrames = (int)(m_editorTimelineDuration / bakeFrameTime + 0.5f) + 1;
            if (tlFrames > bakeFrameCount) bakeFrameCount = tlFrames;
        }

        hkaPose bakePose(m_gameModel->skeleton);

        // Save timing state
        float savedAnimTime = m_jsonAnimTime;
        float savedBlendTime = m_jsonBlendTime;
        float savedLayerTimes[16];
        for (int li = 0; li < MAX_BLEND_LAYERS; ++li)
            savedLayerTimes[li] = m_blendLayers[li].time;
        bool savedLoopRegion = m_loopRegionEnabled;
        m_loopRegionEnabled = false;
        // Disable IK, physics, pose snapshot, root path during bake
        bool savedIKEnabled = m_ikEnabled;
        bool savedFootIK = m_footIkEnabled;
        bool savedPhysEnabled = m_physicalAnimEnabled;
        bool savedPoseSnapValid = m_poseSnapshotValid;
        bool savedPoseSnapBlend = m_poseSnapshotBlendActive;
        bool savedRootPath = m_rootPathEnabled;
        m_ikEnabled = false;
        m_footIkEnabled = false;
        m_physicalAnimEnabled = false;
        m_poseSnapshotValid = false;
        m_poseSnapshotBlendActive = false;
        m_rootPathEnabled = false;

        // Save and CLEAR existing editor keys — if we don't, buildPoseFromJson
        // will apply our injected keys from previous frames via applyEditorOverridesToLocalPose,
        // corrupting the bake (all frames become identical to frame 0).
        ensureEditorArrays();
        savedRotKeys = m_editorRotKeys;
        savedPosKeysX = m_editorPosKeysX;
        savedPosKeysY = m_editorPosKeysY;
        savedPosKeysZ = m_editorPosKeysZ;
        savedScaleKeysX = m_editorScaleKeysX;
        savedScaleKeysY = m_editorScaleKeysY;
        savedScaleKeysZ = m_editorScaleKeysZ;
        // Clear them so bake evaluates pure animation + blend layers
        for (size_t i = 0; i < m_editorRotKeys.size(); ++i) m_editorRotKeys[i].clear();
        for (size_t i = 0; i < m_editorPosKeysX.size(); ++i) { m_editorPosKeysX[i].clear(); m_editorPosKeysY[i].clear(); m_editorPosKeysZ[i].clear(); }
        for (size_t i = 0; i < m_editorScaleKeysX.size(); ++i) { m_editorScaleKeysX[i].clear(); m_editorScaleKeysY[i].clear(); m_editorScaleKeysZ[i].clear(); }

        // BAKE PASS: evaluate full pipeline at every frame into a SEPARATE buffer.
        // We must NOT inject into m_editorRotKeys during the loop, because
        // buildPoseFromJson -> applyEditorOverridesToLocalPose would see injected
        // keys from prior frames and corrupt the result.
        std::vector<std::vector<EditorKey> > bakedRotKeys(skeletonBoneCount);
        std::vector<std::vector<EditorFloatKey> > bakedPosKeysX(skeletonBoneCount);
        std::vector<std::vector<EditorFloatKey> > bakedPosKeysY(skeletonBoneCount);
        std::vector<std::vector<EditorFloatKey> > bakedPosKeysZ(skeletonBoneCount);

        for (int f = 0; f < bakeFrameCount; ++f)
        {
            float t = (float)f * bakeFrameTime;
            float timeMs = t * 1000.0f;

            m_jsonAnimTime = t;
            m_jsonBlendTime = t;
            for (int li = 0; li < MAX_BLEND_LAYERS; ++li)
                if (m_blendLayers[li].active && m_blendLayers[li].clip)
                    m_blendLayers[li].time = t;

            buildPoseFromJson(bakePose, t);
            const hkQsTransform* local = bakePose.getPoseLocalSpace().begin();

            for (int bi = 0; bi < skeletonBoneCount; ++bi)
            {
                hkQuaternion q = local[bi].getRotation();
                EditorKey ek;
                ek.frame = f;
                ek.timeMs = timeMs;
                ek.rot[0] = q.m_vec(0);
                ek.rot[1] = q.m_vec(1);
                ek.rot[2] = q.m_vec(2);
                ek.rot[3] = q.m_vec(3);
                ek.easingType = 0;
                ek.easingCp1x = 0.25f; ek.easingCp1y = 0.0f;
                ek.easingCp2x = 0.75f; ek.easingCp2y = 1.0f;
                bakedRotKeys[bi].push_back(ek);

                hkVector4 trans = local[bi].getTranslation();
                hkVector4 refT = m_gameModel->skeleton->m_referencePose[bi].getTranslation();
                float dist = (trans(0)-refT(0))*(trans(0)-refT(0)) +
                             (trans(1)-refT(1))*(trans(1)-refT(1)) +
                             (trans(2)-refT(2))*(trans(2)-refT(2));
                if (dist > 1e-8f)
                {
                    EditorFloatKey ax;
                    ax.frame = f; ax.timeMs = timeMs; ax.value = trans(0);
                    ax.inTangent = 0.0f; ax.outTangent = 0.0f;
                    ax.interpMode = CURVE_CUBIC; ax.easingType = 0;
                    ax.easingCp1x = 0.25f; ax.easingCp1y = 0.0f;
                    ax.easingCp2x = 0.75f; ax.easingCp2y = 1.0f;
                    EditorFloatKey ay = ax; ay.value = trans(1);
                    EditorFloatKey az = ax; az.value = trans(2);
                    bakedPosKeysX[bi].push_back(ax);
                    bakedPosKeysY[bi].push_back(ay);
                    bakedPosKeysZ[bi].push_back(az);
                }
            }
        }

        // Restore ALL saved state
        m_jsonAnimTime = savedAnimTime;
        m_jsonBlendTime = savedBlendTime;
        for (int li = 0; li < MAX_BLEND_LAYERS; ++li)
            m_blendLayers[li].time = savedLayerTimes[li];
        m_loopRegionEnabled = savedLoopRegion;
        m_ikEnabled = savedIKEnabled;
        m_footIkEnabled = savedFootIK;
        m_physicalAnimEnabled = savedPhysEnabled;
        m_poseSnapshotValid = savedPoseSnapValid;
        m_poseSnapshotBlendActive = savedPoseSnapBlend;
        m_rootPathEnabled = savedRootPath;

        // NOW inject the baked keys into m_editorRotKeys for the export path
        for (int bi = 0; bi < skeletonBoneCount && bi < (int)m_editorRotKeys.size(); ++bi)
            m_editorRotKeys[bi] = bakedRotKeys[bi];
        for (int bi = 0; bi < skeletonBoneCount && bi < (int)m_editorPosKeysX.size(); ++bi)
        {
            m_editorPosKeysX[bi] = bakedPosKeysX[bi];
            m_editorPosKeysY[bi] = bakedPosKeysY[bi];
            m_editorPosKeysZ[bi] = bakedPosKeysZ[bi];
        }

        hasAnyEditorKeys = true;
        maxFrame = bakeFrameCount - 1;
        allEditFrames.clear();
        for (int f = 0; f < bakeFrameCount; ++f)
            allEditFrames.push_back(f);

        if (exportLog)
        {
            fprintf(exportLog, "Baked %d frames x %d bones into editor keys.\n", bakeFrameCount, skeletonBoneCount);
            // Debug: log skeleton bone names + exportBoneToSkeleton mapping
            fprintf(exportLog, "Skeleton has %d bones, export has %d bones\n", skeletonBoneCount, (int)exportBoneNames.size());
            for (int dbi = 0; dbi < skeletonBoneCount && dbi < 5; ++dbi)
            {
                const char* sn = (m_gameModel->skeleton->m_bones[dbi] && m_gameModel->skeleton->m_bones[dbi]->m_name)
                                 ? m_gameModel->skeleton->m_bones[dbi]->m_name : "(null)";
                fprintf(exportLog, "  skel[%d] = %s\n", dbi, sn);
            }
            for (int dbi = 0; dbi < (int)exportBoneNames.size() && dbi < 5; ++dbi)
            {
                int skelIdx = (dbi < (int)exportBoneToSkeleton.size()) ? exportBoneToSkeleton[dbi] : -99;
                fprintf(exportLog, "  export[%d] = %s -> skelIdx=%d\n", dbi, exportBoneNames[dbi].c_str(), skelIdx);
            }
            // Log baked data for specific bones
            for (int dbi = 2; dbi <= 10; dbi += 4)
            {
                if (dbi >= skeletonBoneCount) break;
                const char* dbName = (m_gameModel->skeleton->m_bones[dbi] && m_gameModel->skeleton->m_bones[dbi]->m_name)
                                     ? m_gameModel->skeleton->m_bones[dbi]->m_name : "?";
                fprintf(exportLog, "  Bone %d (%s): %d baked keys\n", dbi, dbName, (int)bakedRotKeys[dbi].size());
                for (int df = 0; df < (int)bakedRotKeys[dbi].size() && df < 5; ++df)
                {
                    const EditorKey& dk = bakedRotKeys[dbi][df];
                    fprintf(exportLog, "    [%d] q=(%.5f, %.5f, %.5f, %.5f)\n", dk.frame, dk.rot[0], dk.rot[1], dk.rot[2], dk.rot[3]);
                }
            }
        }
    }

    bool needRestoreEditorKeys = hasActiveBlendLayers;

    bool canCopyThrough = (!hasAnyEditorKeys && m_jsonAnim && m_jsonAnimPath[0] != '\0');
    // Disable copy-through when events may have been modified (events are serialized by
    // the full export path) or when source == destination (opening dst for write truncates
    // the source before it can be read).
    if (canCopyThrough)
    {
        const JsonAnimClip* evtClip = getActiveJsonClipForEdit();
        if (!evtClip) evtClip = m_jsonAnim;
        if (evtClip && !evtClip->events.empty())
        {
            canCopyThrough = false;
            if (exportLog)
            {
                fprintf(exportLog, "Copy-through disabled (clip has events to serialize).\n");
            }
        }
    }
    if (canCopyThrough && isSameFile)
    {
        canCopyThrough = false;
        if (exportLog)
        {
            fprintf(exportLog, "Copy-through disabled (source == destination).\n");
        }
    }
    if (canCopyThrough)
    {
        float sourceDuration = m_jsonAnim->duration;
        if (sourceDuration <= 0.0f)
        {
            sourceDuration = (sourceFrameCount > 1) ? frameTime * (float)(sourceFrameCount - 1) : frameTime;
        }
        bool sameLength = (sourceFrameCount > 0 && frameCount == sourceFrameCount &&
                           fabs(duration - sourceDuration) <= 0.0001f);
        if (!sameLength)
        {
            canCopyThrough = false;
            if (exportLog)
            {
                fprintf(exportLog, "Copy-through disabled (timeline/frame-count changed).\n");
            }
        }
    }

    if (canCopyThrough)
    {
        if (exportLog)
        {
            fprintf(exportLog, "No editor keys detected. Copy-through export from %s\n", m_jsonAnimPath);
        }
        FILE* src = fopen(m_jsonAnimPath, "rb");
        FILE* dst = fopen(path, "wb");
        if (!src || !dst)
        {
            if (src) fclose(src);
            if (dst) fclose(dst);
            if (exportLog) fclose(exportLog);
            return false;
        }
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0)
        {
            fwrite(buffer, 1, n, dst);
        }
        fclose(src);
        fclose(dst);
        if (exportLog) fclose(exportLog);
        return true;
    }

    FILE* file = fopen(path, "w");
    if (!file)
    {
        if (exportLog) fclose(exportLog);
        return false;
    }

    JsonAnimInfo info;
    InitJsonAnimInfo(info);
    if (m_jsonAnim)
    {
        info = m_jsonAnim->info;
    }
    bool preserveMeta = (m_jsonAnim != NULL);

    // Derive key from output filename
    if (!preserveMeta)
    {
        const char* base = path;
        const char* slash = strrchr(path, '\\');
        if (!slash) slash = strrchr(path, '/');
        if (slash && slash[1] != '\0') base = slash + 1;
        char keyBuf[128];
        strcpy_s(keyBuf, base);
        char* dot = strrchr(keyBuf, '.');
        if (dot) *dot = '\0';
        strcpy_s(info.key, keyBuf);
    }

    if (!preserveMeta)
    {
        info.t_scale = frameTime;
    }
    info.unk_5 = duration;
    info.vala = frameCount;
    info.unk_11 = maxFramesPerBlock;
    info.vals_num = boneCount;
    info.vals2_num = 0;
    info.unk_8 = 0;
    info.bones_num1 = boneCount;
    info.unk_29 = boneCount;
    info.block_starts_num = blockCount;
    info.block_starts2_num = blockCount;
    info.obj1_num = 0;
    int rootIdx = (m_jsonAnim && m_jsonAnim->rootBoneIndex >= 0) ? m_jsonAnim->rootBoneIndex : 0;
    bool hasEditorRoot = (rootIdx >= 0 && rootIdx < (int)m_editorPosKeysX.size() && !m_editorPosKeysX[rootIdx].empty());
    bool hasJsonRoot = (m_jsonAnim && !m_jsonAnim->rootTranslations.empty());
    bool writeObj2 = (hasEditorRoot || hasJsonRoot);
    info.obj2_num = writeObj2 ? frameCount : 0;
    info.obj3_num = 0;
    info.obj_c3_num = 0;
    info.obj_c4_num = 0;
    if (!m_jsonAnim)
    {
        info.offset = 0;
        info.size = 0;
        info.data_offset = 0;
        info.block_starts_offset = 0;
        info.block_starts2_offset = 0;
        info.obj_c3_offset = 0;
        info.obj_c4_offset = 0;
        info.block_offset = 0;
        info.block_size = 0;
        info.obj3_offset = 0;
        info.bones_offset = 0;
        info.obj1_offset = 0;
        info.obj2_offset = 0;
        info.obj5_offset = 0;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"info\": {\n");
    fprintf(file, "    \"key\": \"%s\",\n", info.key);
    fprintf(file, "    \"gamemodemask\": %d,\n", info.gamemodemask);
    fprintf(file, "    \"offset\": %d,\n", info.offset);
    fprintf(file, "    \"size\": %d,\n", info.size);
    fprintf(file, "    \"kind\": %d,\n", info.kind);
    fprintf(file, "    \"unk_5\": %.9g,\n", info.unk_5);
    fprintf(file, "    \"vals_num\": %d,\n", info.vals_num);
    fprintf(file, "    \"vals2_num\": %d,\n", info.vals2_num);
    fprintf(file, "    \"unk_8\": %d,\n", info.unk_8);
    fprintf(file, "    \"vala\": %d,\n", info.vala);
    fprintf(file, "    \"unk_10\": %d,\n", info.unk_10);
    fprintf(file, "    \"unk_11\": %d,\n", info.unk_11);
    fprintf(file, "    \"data_offset\": %d,\n", info.data_offset);
    fprintf(file, "    \"unk_13\": %.9g,\n", info.unk_13);
    fprintf(file, "    \"unk_14\": %.9g,\n", info.unk_14);
    fprintf(file, "    \"t_scale\": %.9g,\n", info.t_scale);
    fprintf(file, "    \"block_starts_offset\": %d,\n", info.block_starts_offset);
    fprintf(file, "    \"block_starts_num\": %d,\n", info.block_starts_num);
    fprintf(file, "    \"block_starts2_offset\": %d,\n", info.block_starts2_offset);
    fprintf(file, "    \"block_starts2_num\": %d,\n", info.block_starts2_num);
    fprintf(file, "    \"obj_c3_offset\": %d,\n", info.obj_c3_offset);
    fprintf(file, "    \"obj_c3_num\": %d,\n", info.obj_c3_num);
    fprintf(file, "    \"obj_c4_offset\": %d,\n", info.obj_c4_offset);
    fprintf(file, "    \"obj_c4_num\": %d,\n", info.obj_c4_num);
    fprintf(file, "    \"block_offset\": %d,\n", info.block_offset);
    fprintf(file, "    \"block_size\": %d,\n", info.block_size);
    fprintf(file, "    \"obj3_num\": %d,\n", info.obj3_num);
    fprintf(file, "    \"obj3_offset\": %d,\n", info.obj3_offset);
    fprintf(file, "    \"bones_num1\": %d,\n", info.bones_num1);
    fprintf(file, "    \"unk_29\": %d,\n", info.unk_29);
    fprintf(file, "    \"obj1_num\": %d,\n", info.obj1_num);
    fprintf(file, "    \"bones_offset\": %d,\n", info.bones_offset);
    fprintf(file, "    \"unk_32\": %d,\n", info.unk_32);
    fprintf(file, "    \"obj1_offset\": %d,\n", info.obj1_offset);
    fprintf(file, "    \"obj2_offset\": %d,\n", info.obj2_offset);
    fprintf(file, "    \"obj2_num\": %d,\n", info.obj2_num);
    fprintf(file, "    \"obj5_offset\": %d\n", info.obj5_offset);
    fprintf(file, "  },\n");

    fprintf(file, "  \"obj1\": [],\n");
    if (!writeObj2)
    {
        fprintf(file, "  \"obj2\": [],\n");
    }
    else
    {
        fprintf(file, "  \"obj2\": [\n");
        for (int f = 0; f < frameCount; ++f)
        {
            JsonVec3 t;
            t.x = 0.0f;
            t.y = 0.0f;
            t.z = 0.0f;
             bool usedEditor = false;
             if (rootIdx >= 0 &&
                rootIdx < (int)m_editorPosKeysX.size() &&
                !m_editorPosKeysX[rootIdx].empty())
             {
                 hkVector4 sampleT;
                if (SampleEditorTransKey(m_editorPosKeysX[rootIdx], m_editorPosKeysY[rootIdx], m_editorPosKeysZ[rootIdx], (float)f, m_editorInterpolationMode, sampleT))
                 {
                     t.x = sampleT(0);
                     t.y = sampleT(1);
                     t.z = sampleT(2);
                     usedEditor = true;
                 }
             }
            if (!usedEditor && m_jsonAnim && m_gameModel && m_gameModel->skeleton &&
                m_jsonAnim->rootBoneIndex >= 0 &&
                !m_jsonAnim->rootFrames.empty() &&
                m_jsonAnim->rootFrames.size() == m_jsonAnim->rootTranslations.size())
            {
                int keyCount = (int)m_jsonAnim->rootFrames.size();
                float frame = (float)f;
                if (keyCount == 1)
                {
                    t = m_jsonAnim->rootTranslations[0];
                }
                else if (frame <= (float)m_jsonAnim->rootFrames[0])
                {
                    t = m_jsonAnim->rootTranslations[0];
                }
                else if (frame >= (float)m_jsonAnim->rootFrames[keyCount - 1])
                {
                    t = m_jsonAnim->rootTranslations[keyCount - 1];
                }
                else
                {
                    for (int k = 0; k < keyCount - 1; k++)
                    {
                        int f0 = m_jsonAnim->rootFrames[k];
                        int f1 = m_jsonAnim->rootFrames[k + 1];
                        if (frame >= (float)f0 && frame <= (float)f1)
                        {
                            float span = (float)(f1 - f0);
                            float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                            const JsonVec3& a = m_jsonAnim->rootTranslations[k];
                            const JsonVec3& b = m_jsonAnim->rootTranslations[k + 1];
                            t.x = a.x + (b.x - a.x) * alpha;
                            t.y = a.y + (b.y - a.y) * alpha;
                            t.z = a.z + (b.z - a.z) * alpha;
                            break;
                        }
                    }
                }
            }
            fprintf(file, "    {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f, \"w\": 0.0}%s\n",
                    t.x, t.y, t.z, (f == frameCount - 1) ? "" : ",");
        }
        fprintf(file, "  ],\n");
    }

    // Serialize events from the active clip (includes both original and user-added events)
    {
        const JsonAnimClip* evtClip = getActiveJsonClipForEdit();
        if (!evtClip) evtClip = m_jsonAnim;
        const std::vector<JsonAnimEvent>* evts = evtClip ? &evtClip->events : NULL;

        if (!evts || evts->empty())
        {
            fprintf(file, "  \"events\": [],\n");
        }
        else
        {
            fprintf(file, "  \"events\": [\n");
            for (size_t ei = 0; ei < evts->size(); ++ei)
            {
                const JsonAnimEvent& evt = (*evts)[ei];
                fprintf(file, "    {\"event\": \"%s\", \"t\": %.9g, \"vals\": [", evt.event, evt.t);
                for (size_t vi = 0; vi < evt.vals.size(); ++vi)
                {
                    const JsonAnimEventVal& v = evt.vals[vi];
                    switch (v.type)
                    {
                    case JsonAnimEventVal::EVT_FLOAT:
                        fprintf(file, "{\"Float\": %.9g}", v.floatVal);
                        break;
                    case JsonAnimEventVal::EVT_CRC:
                        fprintf(file, "{\"CRC\": \"%s\"}", v.crcVal);
                        break;
                    case JsonAnimEventVal::EVT_INT:
                    default:
                        fprintf(file, "{\"Int\": %d}", v.intVal);
                        break;
                    }
                    if (vi + 1 < evt.vals.size())
                        fprintf(file, ", ");
                }
                fprintf(file, "]}%s\n", (ei + 1 < evts->size()) ? "," : "");
            }
            fprintf(file, "  ],\n");
        }

        if (exportLog && evts && !evts->empty())
        {
            fprintf(exportLog, "Exported %d events\n", (int)evts->size());
        }
    }

    fprintf(file, "  \"bones\": [\n");
    for (int b = 0; b < boneCount; b++)
    {
        const std::string& name = exportBoneNames[b];
        fprintf(file, "    \"%s\"%s\n", name.c_str(), (b == boneCount - 1) ? "" : ",");
    }
    fprintf(file, "  ],\n");

    fprintf(file, "  \"obj5_a\": [],\n");
    fprintf(file, "  \"obj5_b\": [],\n");
    fprintf(file, "  \"obj_c3\": [],\n");
    fprintf(file, "  \"obj_c4\": [],\n");

    fprintf(file, "  \"blocks\": [\n");
    for (int block = 0; block < blockCount; block++)
    {
        int blockBase = block * maxFramesPerBlock;
        int blockEnd = blockBase + maxFramesPerBlock;
        if (blockEnd > frameCount) blockEnd = frameCount;

        fprintf(file, "    [\n");
        fprintf(file, "      [\n");
        for (int b = 0; b < boneCount; b++)
        {
            int skeletonIdx = (b >= 0 && b < (int)exportBoneToSkeleton.size()) ? exportBoneToSkeleton[b] : -1;
            bool hasEditorRotKeys = false;
            std::vector<ExportKey> editorKeys;
            bool hasEditorScaleKeys = false;
            std::vector<ExportScaleKey> editorScaleKeys;

            if (skeletonIdx >= 0 && skeletonIdx < (int)m_editorRotKeys.size())
            {
                const std::vector<EditorKey>& keys = m_editorRotKeys[skeletonIdx];
                if (!keys.empty())
                {
                    hasEditorRotKeys = true;
                    editorKeys.reserve(keys.size());
                    for (size_t k = 0; k < keys.size(); k++)
                    {
                        JsonTrack::Quat4 q = EditorKeyToQuat4(keys[k]);
                        q = ApplyQuatAxisMapInverse(q, m_rotAxisMode, m_rotSignMask);
                        ExportKey ek;
                        ek.frame = keys[k].frame;
                        ek.q = q;
                        editorKeys.push_back(ek);
                    }
                }
            }

            if (skeletonIdx >= 0 && skeletonIdx < (int)m_editorScaleKeysX.size())
            {
                const std::vector<EditorFloatKey>& kx = m_editorScaleKeysX[skeletonIdx];
                if (!kx.empty())
                {
                    hasEditorScaleKeys = true;
                    editorScaleKeys.reserve(kx.size());
                    const std::vector<EditorFloatKey>& ky = m_editorScaleKeysY[skeletonIdx];
                    const std::vector<EditorFloatKey>& kz = m_editorScaleKeysZ[skeletonIdx];
                    for (size_t k = 0; k < kx.size(); k++)
                    {
                        ExportScaleKey sk;
                        sk.frame = kx[k].frame;
                        sk.sx = kx[k].value;
                        sk.sy = (k < ky.size()) ? ky[k].value : 1.0f;
                        sk.sz = (k < kz.size()) ? kz[k].value : 1.0f;
                        editorScaleKeys.push_back(sk);
                    }
                }
            }

            std::vector<ExportScaleKey> mergedScaleKeys;
            if (hasEditorScaleKeys)
            {
                if (m_jsonAnim && skeletonIdx >= 0 && skeletonIdx < (int)sourceScaleTrackByBone.size())
                {
                    int sTrackIdx = sourceScaleTrackByBone[skeletonIdx];
                    if (sTrackIdx >= 0 && sTrackIdx < (int)m_jsonAnim->scaleTracks.size())
                    {
                        const JsonScaleTrack& sTrack = m_jsonAnim->scaleTracks[sTrackIdx];
                        for (size_t k = 0; k < sTrack.frames.size() && k < sTrack.scales.size(); ++k)
                        {
                            ExportScaleKey sk;
                            sk.frame = sTrack.frames[k];
                            sk.sx = sTrack.scales[k].x;
                            sk.sy = sTrack.scales[k].y;
                            sk.sz = sTrack.scales[k].z;
                            mergedScaleKeys.push_back(sk);
                        }
                    }
                }

                for (size_t k = 0; k < editorScaleKeys.size(); ++k)
                {
                    bool replaced = false;
                    for (size_t m = 0; m < mergedScaleKeys.size(); ++m)
                    {
                        if (mergedScaleKeys[m].frame == editorScaleKeys[k].frame)
                        {
                            mergedScaleKeys[m] = editorScaleKeys[k];
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced)
                    {
                        mergedScaleKeys.push_back(editorScaleKeys[k]);
                    }
                }

                std::sort(mergedScaleKeys.begin(), mergedScaleKeys.end(), ExportScaleKeyLess());
            }

            bool hasObj1 = (m_jsonAnim &&
                            block < (int)m_jsonAnim->rotObj1.size() &&
                            b < (int)m_jsonAnim->rotObj1[block].size() &&
                            m_jsonAnim->rotObj1[block][b].valid);

            std::vector<ExportKey> mergedKeys;
            if (!hasObj1)
            {
                if (skeletonIdx >= 0 &&
                    m_jsonAnim &&
                    skeletonIdx < (int)sourceTrackByBone.size())
                {
                    int trackIdx = sourceTrackByBone[skeletonIdx];
                    if (trackIdx >= 0 && trackIdx < (int)m_jsonAnim->tracks.size())
                    {
                        const JsonTrack& track = m_jsonAnim->tracks[trackIdx];
                        mergedKeys.resize(track.frames.size());
                        for (size_t k = 0; k < track.frames.size(); k++)
                        {
                            mergedKeys[k].frame = track.frames[k];
                            mergedKeys[k].q = SelectQuatForMode(track, (int)k, 0);
                        }
                    }
                }

                if (hasEditorRotKeys)
                {
                    for (size_t k = 0; k < editorKeys.size(); k++)
                    {
                        bool replaced = false;
                        for (size_t m = 0; m < mergedKeys.size(); m++)
                        {
                            if (mergedKeys[m].frame == editorKeys[k].frame)
                            {
                                mergedKeys[m].q = editorKeys[k].q;
                                replaced = true;
                            }
                        }
                        if (!replaced && mergedKeys.empty())
                        {
                            mergedKeys.push_back(editorKeys[k]);
                        }
                    }
                }
            }

            std::vector<ExportKey> blockKeys;
            if (hasEditorRotKeys)
            {
                for (size_t m = 0; m < editorKeys.size(); m++)
                {
                    if (editorKeys[m].frame >= blockBase && editorKeys[m].frame < blockEnd)
                    {
                        blockKeys.push_back(editorKeys[m]);
                    }
                }
                // Hold behavior: if we have a previous editor key before this block,
                // seed block start so pose persists across untouched ranges.
                int prevIdx = -1;
                for (size_t m = 0; m < editorKeys.size(); m++)
                {
                    if (editorKeys[m].frame < blockBase)
                    {
                        prevIdx = (int)m;
                    }
                    else
                    {
                        break;
                    }
                }
                if (prevIdx >= 0)
                {
                    bool hasStartKey = (!blockKeys.empty() && blockKeys[0].frame == blockBase);
                    if (!hasStartKey)
                    {
                        ExportKey holdKey = editorKeys[prevIdx];
                        holdKey.frame = blockBase;
                        blockKeys.insert(blockKeys.begin(), holdKey);
                    }
                }
            }
            else if (!hasObj1)
            {
                for (size_t m = 0; m < mergedKeys.size(); m++)
                {
                    if (mergedKeys[m].frame >= blockBase && mergedKeys[m].frame < blockEnd)
                    {
                        blockKeys.push_back(mergedKeys[m]);
                    }
                }
                // Hold last frame: if this extended block has no keys from the
                // source animation (all source keys are in earlier blocks), insert
                // the last known rotation at blockBase so the block isn't empty.
                if (blockKeys.empty() && !mergedKeys.empty())
                {
                    ExportKey holdKey = mergedKeys.back();
                    holdKey.frame = blockBase;
                    blockKeys.push_back(holdKey);
                }
            }

            fprintf(file, "      [\n");
            // Obj 0: translation (preserve source if available)
            bool hasObj0 = (m_jsonAnim &&
                            block < (int)m_jsonAnim->type2Obj0.size() &&
                            b < (int)m_jsonAnim->type2Obj0[block].size() &&
                            m_jsonAnim->type2Obj0[block][b].valid);
            if (hasObj0)
            {
                const JsonType2Block& t = m_jsonAnim->type2Obj0[block][b];
                fprintf(file, "        {\"nbytes\":%d,\"flags\":%d,\"s1\":%d,\"s2\":%d,\"data\":[",
                        t.nbytes, t.flags, t.s1, t.s2);
                for (size_t i = 0; i < t.data.size(); ++i)
                {
                    fprintf(file, "%d%s", t.data[i], (i + 1 < t.data.size()) ? "," : "");
                }
                fprintf(file, "],\"vals_a\":[");
                if (!t.valsAStr.empty())
                {
                    for (size_t i = 0; i < t.valsAStr.size(); ++i)
                    {
                        fprintf(file, "%s%s", t.valsAStr[i].c_str(), (i + 1 < t.valsAStr.size()) ? "," : "");
                    }
                }
                else
                {
                    for (size_t i = 0; i < t.valsA.size(); ++i)
                    {
                        fprintf(file, "%.9g%s", t.valsA[i], (i + 1 < t.valsA.size()) ? "," : "");
                    }
                }
                fprintf(file, "],\"vals\":{\"Type2\":[");
                for (size_t i = 0; i < t.valsType2.size(); ++i)
                {
                    fprintf(file, "%d%s", t.valsType2[i], (i + 1 < t.valsType2.size()) ? "," : "");
                }
                fprintf(file, "]}},\n");
            }
            else
            {
                fprintf(file, "        {\"nbytes\":0,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[],\"vals_a\":[],\"vals\":{\"Type2\":[]}},\n");
            }

            // Obj 1: rotation keys (preserve source if available)
            if (hasObj1)
            {
                JsonRotBlock r = m_jsonAnim->rotObj1[block][b];
                if (!hasEditorRotKeys)
                {
                    fprintf(file, "        {\"nbytes\":%d,\"flags\":%d,\"s1\":%d,\"s2\":%d,\"data\":[",
                            r.nbytes, r.flags, r.s1, r.s2);
                    for (size_t i = 0; i < r.data.size(); ++i)
                    {
                        fprintf(file, "%d%s", r.data[i], (i + 1 < r.data.size()) ? "," : "");
                    }
                    fprintf(file, "],\"vals\":{\"ThreeComp40\":[");
                    for (size_t i = 0; i < r.vals.size(); ++i)
                    {
                        const ThreeComp40& v = r.vals[i];
                        fprintf(file, "{\"a\":%d,\"b\":%d,\"c\":%d,\"d\":%d,\"e\":%d}%s",
                                v.a, v.b, v.c, v.d, v.e,
                                (i + 1 < r.vals.size()) ? "," : "");
                    }
                    fprintf(file, "]}},\n");
                }
                else
                {
                int valueCount = (int)r.vals.size();
                int frameStart = 0;
                if ((int)r.data.size() >= valueCount + 4)
                {
                    frameStart = 4;
                }
                else if ((int)r.data.size() >= valueCount)
                {
                    frameStart = 0;
                }
                else
                {
                    frameStart = -1;
                }

                // Apply editor hold-keys onto existing source keyframes.
                const std::vector<EditorKey>* srcEditorKeys = NULL;
                if (skeletonIdx >= 0 && skeletonIdx < (int)m_editorRotKeys.size())
                {
                    srcEditorKeys = &m_editorRotKeys[skeletonIdx];
                }
                for (int i = 0; i < valueCount; i++)
                {
                    if (!srcEditorKeys || srcEditorKeys->empty())
                    {
                        break;
                    }
                    int f = (frameStart >= 0 && (int)r.data.size() >= frameStart + valueCount)
                                ? r.data[frameStart + i]
                                : i;
                    f += blockBase;
                    hkQuaternion sampleQ;
                    if (!SampleEditorKey(*srcEditorKeys, (float)f, m_editorInterpolationMode,
                                         m_rotInterpMode == ROT_INTERP_NLERP, sampleQ))
                    {
                        continue;
                    }
                    JsonTrack::Quat4 qMapped = QuatToQuat4(sampleQ);
                    qMapped = ApplyQuatAxisMapInverse(qMapped, m_rotAxisMode, m_rotSignMask);
                    hkQuaternion qForExport = MakeQuaternion(qMapped);

                    JsonTrack::Quat4 q4 = QuatToQuat4(qForExport);
                    JsonTrack::Quat4 orig = DecodeThreeComp40Havok(r.vals[i]);
                    if (Quat4Error(q4, orig) >= 1e-5f)
                    {
                        ThreeComp40 updated = EncodeThreeComp40Strict(qForExport, &r.vals[i]);
                        r.vals[i] = updated;
                    }
                }

                fprintf(file, "        {\"nbytes\":%d,\"flags\":%d,\"s1\":%d,\"s2\":%d,\"data\":[",
                        r.nbytes, r.flags, r.s1, r.s2);
                for (size_t i = 0; i < r.data.size(); ++i)
                {
                    fprintf(file, "%d%s", r.data[i], (i + 1 < r.data.size()) ? "," : "");
                }
                fprintf(file, "],\"vals\":{\"ThreeComp40\":[");
                for (size_t i = 0; i < r.vals.size(); ++i)
                {
                    const ThreeComp40& v = r.vals[i];
                    fprintf(file, "{\"a\":%d,\"b\":%d,\"c\":%d,\"d\":%d,\"e\":%d}%s",
                            v.a, v.b, v.c, v.d, v.e,
                            (i + 1 < r.vals.size()) ? "," : "");
                }
                fprintf(file, "]}},\n");
                }
            }
            else
            {
                fprintf(file, "        {\"nbytes\":%d,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[",
                        (int)blockKeys.size() * 5);
                for (size_t k = 0; k < blockKeys.size(); k++)
                {
                    int relFrame = blockKeys[k].frame - blockBase;
                    fprintf(file, "%d%s", relFrame, (k + 1 < blockKeys.size()) ? "," : "");
                }
                fprintf(file, "],\"times\":[");
                // NEW: Export millisecond times for sub-frame precision
                for (size_t k = 0; k < blockKeys.size(); k++)
                {
                    // Get timeMs from editor keys
                    float timeMs = 0.0f;
                    if (hasEditorRotKeys && skeletonIdx >= 0 && skeletonIdx < (int)m_editorRotKeys.size())
                    {
                        const std::vector<EditorKey>& keys = m_editorRotKeys[skeletonIdx];
                        for (size_t ki = 0; ki < keys.size(); ++ki)
                        {
                            if (keys[ki].frame == blockKeys[k].frame)
                            {
                                timeMs = keys[ki].timeMs;
                                break;
                            }
                        }
                    }
                    // Fallback: calculate from frame if no timeMs stored
                    if (timeMs < 0.001f)
                    {
                        float frameTimeMs = frameTime * 1000.0f;
                        timeMs = blockKeys[k].frame * frameTimeMs;
                    }
                    fprintf(file, "%.2f%s", timeMs, (k + 1 < blockKeys.size()) ? "," : "");
                }
                fprintf(file, "],\"vals\":{\"ThreeComp40\":[");
                for (size_t k = 0; k < blockKeys.size(); k++)
                {
                    hkQuaternion q = MakeQuaternion(blockKeys[k].q);
                    ThreeComp40 v = EncodeThreeComp40Strict(q, NULL);
                    fprintf(file, "{\"a\":%d,\"b\":%d,\"c\":%d,\"d\":%d,\"e\":%d}%s",
                            v.a, v.b, v.c, v.d, v.e,
                            (k + 1 < blockKeys.size()) ? "," : "");
                }
                fprintf(file, "]}},\n");
            }

            // Obj 2: scale (preserve source if no editor scale keys)
            bool hasObj2 = (m_jsonAnim &&
                            block < (int)m_jsonAnim->type2Obj2.size() &&
                            b < (int)m_jsonAnim->type2Obj2[block].size() &&
                            m_jsonAnim->type2Obj2[block][b].valid);
            if (hasEditorScaleKeys)
            {
                std::vector<ExportScaleKey> blockScaleKeys;
                for (size_t m = 0; m < mergedScaleKeys.size(); ++m)
                {
                    if (mergedScaleKeys[m].frame >= blockBase && mergedScaleKeys[m].frame < blockEnd)
                    {
                        blockScaleKeys.push_back(mergedScaleKeys[m]);
                    }
                }
                int prevIdx = -1;
                for (size_t m = 0; m < mergedScaleKeys.size(); ++m)
                {
                    if (mergedScaleKeys[m].frame < blockBase)
                    {
                        prevIdx = (int)m;
                    }
                    else
                    {
                        break;
                    }
                }
                if (prevIdx >= 0)
                {
                    bool hasStartKey = (!blockScaleKeys.empty() && blockScaleKeys[0].frame == blockBase);
                    if (!hasStartKey)
                    {
                        ExportScaleKey holdKey = mergedScaleKeys[prevIdx];
                        holdKey.frame = blockBase;
                        blockScaleKeys.insert(blockScaleKeys.begin(), holdKey);
                    }
                }

                JsonType2Block t;
                EncodeScaleType2Block(blockScaleKeys, blockBase, t);
                if (t.valid)
                {
                    fprintf(file, "        {\"nbytes\":%d,\"flags\":%d,\"s1\":%d,\"s2\":%d,\"data\":[",
                            t.nbytes, t.flags, t.s1, t.s2);
                    for (size_t i = 0; i < t.data.size(); ++i)
                    {
                        fprintf(file, "%d%s", t.data[i], (i + 1 < t.data.size()) ? "," : "");
                    }
                    fprintf(file, "],\"vals_a\":[");
                    for (size_t i = 0; i < t.valsA.size(); ++i)
                    {
                        fprintf(file, "%.9g%s", t.valsA[i], (i + 1 < t.valsA.size()) ? "," : "");
                    }
                    fprintf(file, "],\"vals\":{\"Type2\":[");
                    for (size_t i = 0; i < t.valsType2.size(); ++i)
                    {
                        fprintf(file, "%d%s", t.valsType2[i], (i + 1 < t.valsType2.size()) ? "," : "");
                    }
                    fprintf(file, "]}}");
                }
                else
                {
                    fprintf(file, "        {\"nbytes\":0,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[],\"vals_a\":[],\"vals\":{\"Type2\":[]}}");
                }
            }
            else if (hasObj2)
            {
                const JsonType2Block& t = m_jsonAnim->type2Obj2[block][b];
                fprintf(file, "        {\"nbytes\":%d,\"flags\":%d,\"s1\":%d,\"s2\":%d,\"data\":[",
                        t.nbytes, t.flags, t.s1, t.s2);
                for (size_t i = 0; i < t.data.size(); ++i)
                {
                    fprintf(file, "%d%s", t.data[i], (i + 1 < t.data.size()) ? "," : "");
                }
                fprintf(file, "],\"vals_a\":[");
                if (!t.valsAStr.empty())
                {
                    for (size_t i = 0; i < t.valsAStr.size(); ++i)
                    {
                        fprintf(file, "%s%s", t.valsAStr[i].c_str(), (i + 1 < t.valsAStr.size()) ? "," : "");
                    }
                }
                else
                {
                    for (size_t i = 0; i < t.valsA.size(); ++i)
                    {
                        fprintf(file, "%.9g%s", t.valsA[i], (i + 1 < t.valsA.size()) ? "," : "");
                    }
                }
                fprintf(file, "],\"vals\":{\"Type2\":[");
                for (size_t i = 0; i < t.valsType2.size(); ++i)
                {
                    fprintf(file, "%d%s", t.valsType2[i], (i + 1 < t.valsType2.size()) ? "," : "");
                }
                fprintf(file, "]}}");
            }
            else
            {
                fprintf(file, "        {\"nbytes\":0,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[],\"vals_a\":[],\"vals\":{\"Type2\":[]}}");
            }
            fprintf(file, "\n");
            fprintf(file, "      ]%s\n", (b == boneCount - 1) ? "" : ",");
        }
        fprintf(file, "      ],\n");
        fprintf(file, "      []\n");
        fprintf(file, "    ]%s\n", (block == blockCount - 1) ? "" : ",");
    }
    fprintf(file, "  ]\n");
    fprintf(file, "}\n");

    fclose(file);
    if (exportLog) fclose(exportLog);

    // Restore saved editor keys after bake export
    if (needRestoreEditorKeys)
    {
        m_editorRotKeys = savedRotKeys;
        m_editorPosKeysX = savedPosKeysX;
        m_editorPosKeysY = savedPosKeysY;
        m_editorPosKeysZ = savedPosKeysZ;
        m_editorScaleKeysX = savedScaleKeysX;
        m_editorScaleKeysY = savedScaleKeysY;
        m_editorScaleKeysZ = savedScaleKeysZ;
    }

    return true;
}

// ============================================================================
// Bone reset, undo/redo, pose library, keyframe query
// ============================================================================

void Scene3DRenderer::resetSelectedBoneToRest()
{
    if (!m_gameModel || !m_gameModel->skeleton) return;
    int bone = m_selectedBoneIndex;
    if (bone < 0 || bone >= m_gameModel->skeleton->m_numBones) return;
    pushUndoSnapshot();
    if (bone < m_editorOverrideRot.getSize())
    {
        m_editorOverrideRot[bone].setIdentity();
    }
    if (bone < (int)m_editorOverrideTrans.size())
    {
        m_editorOverrideTrans[bone].x = 0.0f;
        m_editorOverrideTrans[bone].y = 0.0f;
        m_editorOverrideTrans[bone].z = 0.0f;
    }
}

void Scene3DRenderer::resetAllBonesToRest()
{
    pushUndoSnapshot();
    m_editorOverrideRot.setSize(0);
    m_editorOverrideTrans.clear();
}

void Scene3DRenderer::undoPoseEdit()
{
    if (m_undoStack.empty()) return;
    Scene3DRenderer::PoseSnapshot last = m_undoStack.back();
    m_undoStack.pop_back();
    m_redoStack.push_back(MakePoseSnapshot(m_editorOverrideRot, m_editorOverrideTrans));
    CopyVectorToHkArray(last.rot, m_editorOverrideRot);
    m_editorOverrideTrans = last.trans;
}

void Scene3DRenderer::redoPoseEdit()
{
    if (m_redoStack.empty()) return;
    Scene3DRenderer::PoseSnapshot snap = m_redoStack.back();
    m_redoStack.pop_back();
    m_undoStack.push_back(MakePoseSnapshot(m_editorOverrideRot, m_editorOverrideTrans));
    CopyVectorToHkArray(snap.rot, m_editorOverrideRot);
    m_editorOverrideTrans = snap.trans;
}

void Scene3DRenderer::savePoseSlot(int slot, const char* name)
{
    if (slot < 1 || slot > 5) return;
    ensureEditorArrays();
    Scene3DRenderer::PoseLibraryEntry& e = m_poseLibrary[slot - 1];
    e.pose = MakePoseSnapshot(m_editorOverrideRot, m_editorOverrideTrans);
    e.valid = true;
    if (name && name[0] != '\0')
        strncpy_s(e.name, name, _TRUNCATE);
    else
        sprintf_s(e.name, "Pose%d", slot);
    char msg[128];
    sprintf_s(msg, "Saved pose slot %d (%s)", slot, e.name);
    RendererLog(msg);
}

bool Scene3DRenderer::applyPoseSlot(int slot, bool mirrorX)
{
    if (slot < 1 || slot > 5) return false;
    Scene3DRenderer::PoseLibraryEntry& e = m_poseLibrary[slot - 1];
    if (!e.valid) return false;
    pushUndoSnapshot();
    Scene3DRenderer::PoseSnapshot use = e.pose;
    if (mirrorX) MirrorPoseSnapshotX(use);
    CopyVectorToHkArray(use.rot, m_editorOverrideRot);
    m_editorOverrideTrans = use.trans;
    char msg[128];
    sprintf_s(msg, "Applied pose slot %d (%s)%s", slot, e.name, mirrorX ? " [mirrored]" : "");
    RendererLog(msg);
    return true;
}

void Scene3DRenderer::getSelectedBoneKeyFrames(std::vector<int>& rotFrames, std::vector<int>& transFrames) const
{
    rotFrames.clear();
    transFrames.clear();
    if (!m_gameModel || !m_gameModel->skeleton) return;
    int idx = m_selectedBoneIndex;
    if (idx < 0 || idx >= (int)m_editorRotKeys.size()) return;
    if (idx < (int)m_editorRotKeys.size())
    {
        for (size_t i = 0; i < m_editorRotKeys[idx].size(); ++i)
            rotFrames.push_back(m_editorRotKeys[idx][i].frame);
    }
    if (idx < (int)m_editorPosKeysX.size())
    {
        for (size_t i = 0; i < m_editorPosKeysX[idx].size(); ++i)
            transFrames.push_back(m_editorPosKeysX[idx][i].frame);
    }
    std::sort(rotFrames.begin(), rotFrames.end());
    rotFrames.erase(std::unique(rotFrames.begin(), rotFrames.end()), rotFrames.end());
    std::sort(transFrames.begin(), transFrames.end());
    transFrames.erase(std::unique(transFrames.begin(), transFrames.end()), transFrames.end());
}

// ============================================================================
// Skeleton and gizmo rendering, gizmo picking
// ============================================================================

void Scene3DRenderer::renderSkeletonFromPose(const hkaPose& pose)
{
    if (!m_gameModel || !m_gameModel->skeleton || !m_context) {
        return;
    }

    // Skeleton/gizmo pass must be pure vertex color; prevent texture modulation.
    m_context->setTexture2DState(false);
    m_context->setLightingState(false);
    m_context->setBlendState(false);

    const hkaSkeleton* skeleton = m_gameModel->skeleton;
    const hkInt16* parentIndices = skeleton->m_parentIndices;
    int numBones = skeleton->m_numBones;

    float rootOffsetX = 0.0f;
    float rootOffsetY = 0.0f;
    float rootOffsetZ = 0.0f;
    if (m_rootMotionMode == ROOT_MOTION_EXTRACT)
    {
        rootOffsetX = m_rootMotionOffset(0);
        rootOffsetY = m_rootMotionOffset(1);
        rootOffsetZ = m_rootMotionOffset(2);
    }

    m_context->beginGroup(HKG_IMM_LINES);

    float boneColor[4] = {1.0f, 1.0f, 0.0f, 1.0f}; // Yellow bones
    m_context->setCurrentColor4(boneColor);

    // Root motion trail capture
    if (m_showRootTrail && m_gameModel->skeleton->m_numBones > 0)
    {
        const hkVector4& rootPos = pose.getBoneModelSpace(0).getTranslation();
        hkVector4 wp = rootPos;
        wp(0) += rootOffsetX;
        wp(1) += rootOffsetY;
        wp(2) += rootOffsetZ;
        m_rootTrail.pushBack(wp);
        if (m_rootTrail.getSize() > 120) m_rootTrail.removeAtAndCopy(0);
    }

    for (int i = 0; i < numBones; i++) {
        int parentIdx = parentIndices[i];
        if (parentIdx < 0) {
            continue;
        }

        const hkVector4& childPos = pose.getBoneModelSpace(i).getTranslation();
        const hkVector4& parentPos = pose.getBoneModelSpace(parentIdx).getTranslation();

        float childPosArray[3];
        childPos.store3(childPosArray);
        childPosArray[0] += rootOffsetX;
        childPosArray[1] += m_modelBaseOffsetY + rootOffsetY;
        childPosArray[2] += rootOffsetZ;
        if (m_groundClampMode != GROUND_CLAMP_OFF)
        {
            childPosArray[1] += m_groundOffsetY;
        }
        m_context->setCurrentPosition(childPosArray);

        float parentPosArray[3];
        parentPos.store3(parentPosArray);
        parentPosArray[0] += rootOffsetX;
        parentPosArray[1] += m_modelBaseOffsetY + rootOffsetY;
        parentPosArray[2] += rootOffsetZ;
        if (m_groundClampMode != GROUND_CLAMP_OFF)
        {
            parentPosArray[1] += m_groundOffsetY;
        }
        m_context->setCurrentPosition(parentPosArray);
    }

    m_context->endGroup();

    renderBoneGizmo(pose);

    m_context->beginGroup(HKG_IMM_POINTS);

    float jointColor[4] = {1.0f, 0.0f, 0.0f, 1.0f}; // Red joints
    m_context->setCurrentColor4(jointColor);

    for (int i = 0; i < numBones; i++) {
        const hkVector4& pos = pose.getBoneModelSpace(i).getTranslation();
        float posArray[3];
        pos.store3(posArray);
        posArray[0] += rootOffsetX;
        posArray[1] += m_modelBaseOffsetY + rootOffsetY;
        posArray[2] += rootOffsetZ;
        if (m_groundClampMode != GROUND_CLAMP_OFF)
        {
            posArray[1] += m_groundOffsetY;
        }
        m_context->setCurrentPosition(posArray);
    }

    m_context->endGroup();
    m_context->flush();

    // Draw root motion trail (line strip)
    if (m_showRootTrail && m_rootTrail.getSize() >= 2)
    {
        float trailColor[4] = {0.2f, 1.0f, 0.2f, 1.0f};
        m_context->setCurrentColor4(trailColor);
        m_context->beginGroup(HKG_IMM_LINES);
        for (int i = 0; i + 1 < m_rootTrail.getSize(); ++i)
        {
            hkVector4 p0 = m_rootTrail[i];
            hkVector4 p1 = m_rootTrail[i + 1];
            float a[3] = { p0(0), p0(1), p0(2) };
            float b[3] = { p1(0), p1(1), p1(2) };
            m_context->setCurrentPosition(a);
            m_context->setCurrentPosition(b);
        }
        m_context->endGroup();
        m_context->flush();
    }
}

void Scene3DRenderer::renderBoneGizmo(const hkaPose& pose)
{
    if (!m_context || !m_gameModel || !m_gameModel->skeleton)
    {
        return;
    }
    int numBones = m_gameModel->skeleton->m_numBones;
    if (m_selectedBoneIndex < 0 || m_selectedBoneIndex >= numBones)
    {
        return;
    }

    const hkQsTransform& boneT = pose.getBoneModelSpace(m_selectedBoneIndex);
    hkVector4 pos = boneT.getTranslation();
    const hkQuaternion& q = boneT.getRotation();

    float p[3];
    pos.store3(p);
    float rootOffsetX = 0.0f;
    float rootOffsetY = 0.0f;
    float rootOffsetZ = 0.0f;
    if (m_rootMotionMode == ROOT_MOTION_EXTRACT)
    {
        rootOffsetX = m_rootMotionOffset(0);
        rootOffsetY = m_rootMotionOffset(1);
        rootOffsetZ = m_rootMotionOffset(2);
    }
    p[0] += rootOffsetX;
    p[1] += m_modelBaseOffsetY + rootOffsetY;
    p[2] += rootOffsetZ;
    if (m_groundClampMode != GROUND_CLAMP_OFF)
    {
        p[1] += m_groundOffsetY;
    }

    hkVector4 axisX; axisX.set(1.0f, 0.0f, 0.0f);
    hkVector4 axisY; axisY.set(0.0f, 1.0f, 0.0f);
    hkVector4 axisZ; axisZ.set(0.0f, 0.0f, 1.0f);
    if (m_gizmoSpace == GIZMO_LOCAL)
    {
        hkRotation rot;
        rot.set(q);
        axisX = rot.getColumn(0);
        axisY = rot.getColumn(1);
        axisZ = rot.getColumn(2);
    }

    float gizmoScale = m_cameraDistance * 0.08f;
    if (gizmoScale < 0.25f) gizmoScale = 0.25f;
    if (gizmoScale > 2.5f) gizmoScale = 2.5f;

    m_gizmoCacheValid = true;
    m_gizmoCachePos.set(p[0], p[1], p[2]);
    m_gizmoCacheAxisX = axisX;
    m_gizmoCacheAxisY = axisY;
    m_gizmoCacheAxisZ = axisZ;
    m_gizmoCacheScale = gizmoScale;

    float red[4] = {1.0f, 0.2f, 0.2f, 1.0f};
    float green[4] = {0.2f, 1.0f, 0.2f, 1.0f};
    float blue[4] = {0.2f, 0.7f, 1.0f, 1.0f};
    float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float yellow[4] = {1.0f, 1.0f, 0.2f, 1.0f};
    float cyan[4] = {0.2f, 1.0f, 1.0f, 1.0f};
    float magenta[4] = {1.0f, 0.2f, 1.0f, 1.0f};

    if (m_gizmoMode == GIZMO_TRANSLATE)
    {
        // Move gizmo: X/Y/Z arrows.
        hkVector4 axes[3] = { axisX, axisY, axisZ };
        float* colors[3] = { red, green, blue };

        m_context->beginGroup(HKG_IMM_LINES);
        for (int a = 0; a < 3; ++a)
        {
            float* c = colors[a];
            if (m_editorTranslateAxisLock == (a + 1))
            {
                c = yellow;
            }
            else if (m_editorDraggingTrans && m_moveSnapAxis == (a + 1))
            {
                c = cyan; // Snap feedback
            }
            m_context->setCurrentColor4(c);

            hkVector4 tip = axes[a];
            tip.mul4(gizmoScale);
            float t[3] = { p[0] + tip(0), p[1] + tip(1), p[2] + tip(2) };

            m_context->setCurrentPosition(p);
            m_context->setCurrentPosition(t);

            // Small arrow cap
            hkVector4 cap1 = axes[a];
            hkVector4 cap2 = axes[a];
            cap1.mul4(gizmoScale * 0.85f);
            cap2.mul4(gizmoScale * 0.85f);

            hkVector4 ortho = (a == 0) ? axisY : axisX;
            hkVector4 ortho2 = (a == 2) ? axisY : axisZ;
            ortho.mul4(gizmoScale * 0.12f);
            ortho2.mul4(gizmoScale * 0.12f);

            float c1[3] = { p[0] + cap1(0) + ortho(0), p[1] + cap1(1) + ortho(1), p[2] + cap1(2) + ortho(2) };
            float c2[3] = { t[0], t[1], t[2] };
            float c3[3] = { p[0] + cap2(0) + ortho2(0), p[1] + cap2(1) + ortho2(1), p[2] + cap2(2) + ortho2(2) };

            m_context->setCurrentPosition(c1);
            m_context->setCurrentPosition(c2);
            m_context->setCurrentPosition(c3);
            m_context->setCurrentPosition(c2);
        }
        m_context->setCurrentColor4(white);
        m_context->setCurrentPosition(p);
        m_context->endGroup();
    }
    else
    {
        // Rotate gizmo: X/Y/Z rings.
        const int segments = 48;
        const float r = gizmoScale * 0.9f;

        hkVector4 ringU[3] = { axisY, axisX, axisX };
        hkVector4 ringV[3] = { axisZ, axisZ, axisY };
        float* colors[3] = { red, green, blue };

        m_context->beginGroup(HKG_IMM_LINES);
        for (int a = 0; a < 3; ++a)
        {
            float* c = colors[a];
            if (m_editorRotateAxisLock == (a + 1))
            {
                c = yellow;
            }
            else if (m_editorDragging && m_rotateSnapAxis == (a + 1))
            {
                c = magenta; // Snap feedback
            }
            m_context->setCurrentColor4(c);

            for (int s = 0; s < segments; ++s)
            {
                float t0 = (6.2831853f * (float)s) / (float)segments;
                float t1 = (6.2831853f * (float)(s + 1)) / (float)segments;
                float c0 = cosf(t0), s0 = sinf(t0);
                float c1 = cosf(t1), s1 = sinf(t1);

                float p0[3] =
                {
                    p[0] + (ringU[a](0) * c0 + ringV[a](0) * s0) * r,
                    p[1] + (ringU[a](1) * c0 + ringV[a](1) * s0) * r,
                    p[2] + (ringU[a](2) * c0 + ringV[a](2) * s0) * r
                };
                float p1[3] =
                {
                    p[0] + (ringU[a](0) * c1 + ringV[a](0) * s1) * r,
                    p[1] + (ringU[a](1) * c1 + ringV[a](1) * s1) * r,
                    p[2] + (ringU[a](2) * c1 + ringV[a](2) * s1) * r
                };

                m_context->setCurrentPosition(p0);
                m_context->setCurrentPosition(p1);
            }
        }
        m_context->setCurrentColor4(white);
        m_context->setCurrentPosition(p);
        m_context->endGroup();
    }
}

Scene3DRenderer::AxisLock Scene3DRenderer::pickGizmoAxis(int mouseX, int mouseY) const
{
    int vpW = m_windowWidth;
    int vpH = m_windowHeight;
    if (m_imguiViewportActive && m_imguiViewportW > 1 && m_imguiViewportH > 1)
    {
        vpW = m_imguiViewportW;
        vpH = m_imguiViewportH;
    }
    if (!m_gizmoCacheValid || vpW <= 1 || vpH <= 1)
    {
        return AXIS_FREE;
    }

    hkVector4 camPos;
    camPos.set(
        m_cameraDistance * cosf(m_cameraPitch) * sinf(m_cameraYaw) + m_cameraTarget(0),
        m_cameraDistance * sinf(m_cameraPitch) + m_cameraTarget(1),
        m_cameraDistance * cosf(m_cameraPitch) * cosf(m_cameraYaw) + m_cameraTarget(2));

    hkVector4 forward;
    forward.set(m_cameraTarget(0) - camPos(0), m_cameraTarget(1) - camPos(1), m_cameraTarget(2) - camPos(2));
    float fLen = sqrtf(forward(0) * forward(0) + forward(1) * forward(1) + forward(2) * forward(2));
    if (fLen < 1e-5f) return AXIS_FREE;
    forward(0) /= fLen; forward(1) /= fLen; forward(2) /= fLen;

    hkVector4 worldUp; worldUp.set(0.0f, 1.0f, 0.0f);
    hkVector4 right;
    right.set(
        forward(1) * worldUp(2) - forward(2) * worldUp(1),
        forward(2) * worldUp(0) - forward(0) * worldUp(2),
        forward(0) * worldUp(1) - forward(1) * worldUp(0));
    float rLen = sqrtf(right(0) * right(0) + right(1) * right(1) + right(2) * right(2));
    if (rLen < 1e-5f) return AXIS_FREE;
    right(0) /= rLen; right(1) /= rLen; right(2) /= rLen;

    hkVector4 up;
    up.set(
        right(1) * forward(2) - right(2) * forward(1),
        right(2) * forward(0) - right(0) * forward(2),
        right(0) * forward(1) - right(1) * forward(0));

    float tanHalf = tanf((m_cameraFovDegrees * 0.5f) * 0.0174532925f);
    if (tanHalf < 1e-5f) tanHalf = 0.57735f;
    float aspect = (float)vpW / (float)vpH;
    if (aspect < 0.01f) aspect = 1.0f;

    struct ScreenPt { float x, y; bool ok; };
    ScreenPt center = {0, 0, false};

    const hkVector4 axes[3] = { m_gizmoCacheAxisX, m_gizmoCacheAxisY, m_gizmoCacheAxisZ };

    // local lambda-like helper (VS2005-safe pattern)
    struct Projector
    {
        static ScreenPt project(const hkVector4& p, const hkVector4& camPos, const hkVector4& right, const hkVector4& up, const hkVector4& forward,
                                float tanHalf, float aspect, int w, int h)
        {
            ScreenPt s = {0, 0, false};
            float vx = p(0) - camPos(0);
            float vy = p(1) - camPos(1);
            float vz = p(2) - camPos(2);
            float z = vx * forward(0) + vy * forward(1) + vz * forward(2);
            if (z <= 0.001f) return s;
            float x = vx * right(0) + vy * right(1) + vz * right(2);
            float y = vx * up(0) + vy * up(1) + vz * up(2);
            float ndcX = x / (z * tanHalf * aspect);
            float ndcY = y / (z * tanHalf);
            s.x = (ndcX * 0.5f + 0.5f) * (float)w;
            s.y = (0.5f - ndcY * 0.5f) * (float)h;
            s.ok = true;
            return s;
        }
    };

    center = Projector::project(m_gizmoCachePos, camPos, right, up, forward, tanHalf, aspect, vpW, vpH);
    if (!center.ok) return AXIS_FREE;

    float mx = (float)mouseX;
    float my = (float)mouseY;
    float bestDist = 1e9f;
    int bestAxis = 0;
    const float moveThreshold = 18.0f;
    const float rotThreshold = 15.0f;

    if (m_gizmoMode == GIZMO_TRANSLATE)
    {
        for (int a = 0; a < 3; ++a)
        {
            hkVector4 tip = axes[a];
            tip.mul4(m_gizmoCacheScale);
            tip(0) += m_gizmoCachePos(0);
            tip(1) += m_gizmoCachePos(1);
            tip(2) += m_gizmoCachePos(2);
            ScreenPt st = Projector::project(tip, camPos, right, up, forward, tanHalf, aspect, vpW, vpH);
            if (!st.ok) continue;
            float d = DistPointToSegment2D(mx, my, center.x, center.y, st.x, st.y);
            if (d < bestDist)
            {
                bestDist = d;
                bestAxis = a + 1;
            }
        }
        if (bestDist <= moveThreshold) return static_cast<AxisLock>(bestAxis);
        return AXIS_FREE;
    }

    // Rotate rings
    const int segments = 48;
    const float r = m_gizmoCacheScale * 0.9f;
    hkVector4 ringU[3] = { m_gizmoCacheAxisY, m_gizmoCacheAxisX, m_gizmoCacheAxisX };
    hkVector4 ringV[3] = { m_gizmoCacheAxisZ, m_gizmoCacheAxisZ, m_gizmoCacheAxisY };

    for (int a = 0; a < 3; ++a)
    {
        for (int s = 0; s < segments; ++s)
        {
            float t0 = (6.2831853f * (float)s) / (float)segments;
            float t1 = (6.2831853f * (float)(s + 1)) / (float)segments;
            float c0 = cosf(t0), si0 = sinf(t0);
            float c1 = cosf(t1), si1 = sinf(t1);

            hkVector4 p0;
            p0.set(
                m_gizmoCachePos(0) + (ringU[a](0) * c0 + ringV[a](0) * si0) * r,
                m_gizmoCachePos(1) + (ringU[a](1) * c0 + ringV[a](1) * si0) * r,
                m_gizmoCachePos(2) + (ringU[a](2) * c0 + ringV[a](2) * si0) * r);

            hkVector4 p1;
            p1.set(
                m_gizmoCachePos(0) + (ringU[a](0) * c1 + ringV[a](0) * si1) * r,
                m_gizmoCachePos(1) + (ringU[a](1) * c1 + ringV[a](1) * si1) * r,
                m_gizmoCachePos(2) + (ringU[a](2) * c1 + ringV[a](2) * si1) * r);

            ScreenPt s0 = Projector::project(p0, camPos, right, up, forward, tanHalf, aspect, vpW, vpH);
            ScreenPt s1 = Projector::project(p1, camPos, right, up, forward, tanHalf, aspect, vpW, vpH);
            if (!s0.ok || !s1.ok) continue;

            float d = DistPointToSegment2D(mx, my, s0.x, s0.y, s1.x, s1.y);
            if (d < bestDist)
            {
                bestDist = d;
                bestAxis = a + 1;
            }
        }
    }

    if (bestDist <= rotThreshold) return static_cast<AxisLock>(bestAxis);
    return AXIS_FREE;
}

// ============================================================================
// Editor keyframe accessors for timeline visualization
// ============================================================================

int Scene3DRenderer::getEditorRotKeyCount(int boneIndex) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorRotKeys.size())
        return 0;
    return (int)m_editorRotKeys[boneIndex].size();
}

int Scene3DRenderer::getEditorTransKeyCount(int boneIndex) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return 0;
    return (int)m_editorPosKeysX[boneIndex].size();
}

int Scene3DRenderer::getEditorScaleKeyCount(int boneIndex) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return 0;
    return (int)m_editorScaleKeysX[boneIndex].size();
}

float Scene3DRenderer::getEditorRotKeyTime(int boneIndex, int keyIndex) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorRotKeys.size())
        return 0.0f;
    if (keyIndex < 0 || keyIndex >= (int)m_editorRotKeys[boneIndex].size())
        return 0.0f;
    return m_editorRotKeys[boneIndex][keyIndex].timeMs;
}

float Scene3DRenderer::getEditorTransKeyTime(int boneIndex, int keyIndex) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorPosKeysX.size())
        return 0.0f;
    if (keyIndex < 0 || keyIndex >= (int)m_editorPosKeysX[boneIndex].size())
        return 0.0f;
    return m_editorPosKeysX[boneIndex][keyIndex].timeMs;
}

float Scene3DRenderer::getEditorScaleKeyTime(int boneIndex, int keyIndex) const
{
    if (boneIndex < 0 || boneIndex >= (int)m_editorScaleKeysX.size())
        return 0.0f;
    if (keyIndex < 0 || keyIndex >= (int)m_editorScaleKeysX[boneIndex].size())
        return 0.0f;
    return m_editorScaleKeysX[boneIndex][keyIndex].timeMs;
}

// Initialize millisecond times for all keyframes if not already set
void Scene3DRenderer::initializeEditorKeyTimes()
{
    if (!m_gameModel || !m_gameModel->skeleton)
        return;

    float frameTimeMs = (m_editorFrameTime > 0.0f) ? m_editorFrameTime : (1.0f / 30.0f);
    frameTimeMs *= 1000.0f;  // Convert to milliseconds

    // Initialize rotation keyframes
    for (int b = 0; b < m_gameModel->skeleton->m_numBones && b < (int)m_editorRotKeys.size(); ++b)
    {
        for (size_t k = 0; k < m_editorRotKeys[b].size(); ++k)
        {
            // Only set if not already initialized (timeMs is 0 or very small)
            if (m_editorRotKeys[b][k].timeMs < 0.1f)
            {
                m_editorRotKeys[b][k].timeMs = (float)m_editorRotKeys[b][k].frame * frameTimeMs;
            }
        }
    }

    // Initialize translation keyframes (XYZ curves)
    const int boneCount = m_gameModel->skeleton->m_numBones;
    for (int b = 0; b < boneCount && b < (int)m_editorPosKeysX.size(); ++b)
    {
        std::vector<EditorFloatKey>* curves[3] = { &m_editorPosKeysX[b], &m_editorPosKeysY[b], &m_editorPosKeysZ[b] };
        for (int c = 0; c < 3; ++c)
        {
            if (!curves[c]) continue;
            for (size_t k = 0; k < curves[c]->size(); ++k)
            {
                if ((*curves[c])[k].timeMs < 0.1f)
                {
                    (*curves[c])[k].timeMs = (float)(*curves[c])[k].frame * frameTimeMs;
                }
            }
        }
    }

    // Initialize scale keyframes (XYZ curves)
    for (int b = 0; b < boneCount && b < (int)m_editorScaleKeysX.size(); ++b)
    {
        std::vector<EditorFloatKey>* curves[3] = { &m_editorScaleKeysX[b], &m_editorScaleKeysY[b], &m_editorScaleKeysZ[b] };
        for (int c = 0; c < 3; ++c)
        {
            if (!curves[c]) continue;
            for (size_t k = 0; k < curves[c]->size(); ++k)
            {
                if ((*curves[c])[k].timeMs < 0.1f)
                {
                    (*curves[c])[k].timeMs = (float)(*curves[c])[k].frame * frameTimeMs;
                }
            }
        }
    }
}
