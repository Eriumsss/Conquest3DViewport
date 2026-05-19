// ============================================================================
// MocapRetargeter.cpp -- The Bone Mapping Hellscape
// ============================================================================
//
// 600+ lines of skeleton remapping, quaternion math, and sanity loss.
// SMPL gives us 24 world-space axis-angle rotations. We convert each to
// a quaternion, invert it, multiply by the reference pose, and pray.
//
// The formula is: result = invQ * refPose. When SMPL rotation is identity
// you get the reference pose back (correct rest). When SMPL rotates a
// joint, invQ applies the delta in parent-local space. This is backwards
// from what you'd expect and I spent TWO FUCKING WEEKS proving it works
// by dumping every intermediate quaternion to a log file and visualizing
// them in a Python matplotlib script. See mocap_retarget_debug.log.
//
// Root bone is special: it gets yFlip * invQ because the body faces
// backward without the 180-degree Y rotation. The yFlip quaternion
// is (0, 1, 0, 0) -- a 180-degree rotation around Y. Static initialized
// once because VS2005 doesn't have constexpr.
//
// Post-retarget smoothing: Savitzky-Golay filter (quadratic, window=5)
// eliminates WHAM's high-frequency jitter without destroying peaks.
// Quaternion neighborhood correction runs first to prevent sign-flip
// averaging artifacts. Then we re-normalize because SG can denormalize.
//
// Finger curl is procedural -- SMPL doesn't track individual fingers.
// We synthesize grip from a 0-1 slider, curling each phalanx around
// local X with decreasing amplitude. It looks decent enough.
//
// VS2005 compatible (C++03). Havok 5.5.0 math types. Debug logging.
// ============================================================================

#include "MocapRetargeter.h"
#include <math.h>
#include <string.h>

#ifndef HK_REAL_PI
#define HK_REAL_PI 3.14159265358979323846f
#endif

// ============================================================
// Construction
// ============================================================

MocapRetargeter::MocapRetargeter()
    : m_hasRefPose(false)
{
    memset(m_smplToGame, -1, sizeof(m_smplToGame));
    memset(m_leftFingers, 0, sizeof(m_leftFingers));
    memset(m_rightFingers, 0, sizeof(m_rightFingers));
    for (int i = 0; i < GAME_BONE_COUNT; ++i)
        m_refPoseLocal[i].setIdentity();
}

// ============================================================
// Initialize Mapping Tables
// ============================================================

void MocapRetargeter::Initialize()
{
    // --- Direct SMPL -> Game mapping ---
    m_smplToGame[SMPL_PELVIS]     = GB_ROOT;
    m_smplToGame[SMPL_L_HIP]      = GB_LTHIGH;
    m_smplToGame[SMPL_R_HIP]      = GB_RTHIGH;
    m_smplToGame[SMPL_SPINE1]     = GB_LUMBAR1;
    m_smplToGame[SMPL_L_KNEE]     = GB_LSHIN;
    m_smplToGame[SMPL_R_KNEE]     = GB_RSHIN;
    // NOTE: Do NOT map SMPL_SPINE2 to GB_UPPERBODY!
    // UpperBody is the parent of LThigh/RThigh in the game skeleton.
    // Mapping spine rotation here would tilt the entire lower body.
    // SMPL_SPINE2 rotation is used indirectly via spine interpolation instead.
    // m_smplToGame[SMPL_SPINE2] = GB_UPPERBODY;
    m_smplToGame[SMPL_L_ANKLE]    = GB_LFOOTBONE1;
    m_smplToGame[SMPL_R_ANKLE]    = GB_RFOOTBONE1;
    m_smplToGame[SMPL_SPINE3]     = GB_NECK;
    m_smplToGame[SMPL_L_FOOT]     = GB_LFOOTBONE3;
    m_smplToGame[SMPL_R_FOOT]     = GB_RFOOTBONE3;
    m_smplToGame[SMPL_NECK]       = GB_NECK;       // shared with SPINE3
    m_smplToGame[SMPL_L_COLLAR]   = GB_LSHOULDER;
    m_smplToGame[SMPL_R_COLLAR]   = GB_RSHOULDER;
    m_smplToGame[SMPL_HEAD]       = GB_HEAD;
    m_smplToGame[SMPL_L_SHOULDER] = GB_LBICEP;
    m_smplToGame[SMPL_R_SHOULDER] = GB_RBICEP;
    m_smplToGame[SMPL_L_ELBOW]    = GB_LFOREARM;
    m_smplToGame[SMPL_R_ELBOW]    = GB_RFOREARM;
    m_smplToGame[SMPL_L_WRIST]    = GB_LHAND;
    m_smplToGame[SMPL_R_WRIST]    = GB_RHAND;
    m_smplToGame[SMPL_L_HAND]     = GB_LHAND;      // shared with wrist
    m_smplToGame[SMPL_R_HAND]     = GB_RHAND;      // shared with wrist

    // --- Procedural bones (copy parent rotation) ---
    ProceduralBone pb;

    pb.targetBone = GB_LFOREARMROLL; pb.sourceBone = GB_LFOREARM;
    m_proceduralBones.push_back(pb);
    pb.targetBone = GB_RFOREARMROLL; pb.sourceBone = GB_RFOREARM;
    m_proceduralBones.push_back(pb);
    pb.targetBone = GB_LHAND_ATTACH; pb.sourceBone = GB_LHAND;
    m_proceduralBones.push_back(pb);
    pb.targetBone = GB_RHAND_ATTACH; pb.sourceBone = GB_RHAND;
    m_proceduralBones.push_back(pb);
    pb.targetBone = GB_JAW; pb.sourceBone = GB_HEAD;
    m_proceduralBones.push_back(pb);

    // --- Spine interpolation ---
    // Interpolate Lumbar2/3 between Lumbar1 (bottom of spine) and Neck (top).
    // Do NOT use UpperBody here -- it parents the thighs and stays at identity.
    SpineInterp si;

    // Lumbar2 = SLERP(Lumbar1, Neck, 0.33)
    si.targetBone = GB_LUMBAR2; si.boneA = GB_LUMBAR1; si.boneB = GB_NECK; si.t = 0.33f;
    m_spineInterps.push_back(si);

    // Lumbar3 = SLERP(Lumbar1, Neck, 0.66)
    si.targetBone = GB_LUMBAR3; si.boneA = GB_LUMBAR1; si.boneB = GB_NECK; si.t = 0.66f;
    m_spineInterps.push_back(si);

    // FootBone2 = SLERP(FootBone1, FootBone3, 0.5)
    si.targetBone = GB_LFOOTBONE2; si.boneA = GB_LFOOTBONE1; si.boneB = GB_LFOOTBONE3; si.t = 0.5f;
    m_spineInterps.push_back(si);
    si.targetBone = GB_RFOOTBONE2; si.boneA = GB_RFOOTBONE1; si.boneB = GB_RFOOTBONE3; si.t = 0.5f;
    m_spineInterps.push_back(si);

    // --- Finger chains ---
    // Left hand
    m_leftFingers[0].bones[0] = GB_LINDEX1;  m_leftFingers[0].bones[1] = GB_LINDEX2;  m_leftFingers[0].bones[2] = GB_LINDEX3;
    m_leftFingers[1].bones[0] = GB_LMIDDLE1; m_leftFingers[1].bones[1] = GB_LMIDDLE2; m_leftFingers[1].bones[2] = GB_LMIDDLE3;
    m_leftFingers[2].bones[0] = GB_LPINKY1;  m_leftFingers[2].bones[1] = GB_LPINKY2;  m_leftFingers[2].bones[2] = GB_LPINKY3;
    m_leftFingers[3].bones[0] = GB_LRING1;   m_leftFingers[3].bones[1] = GB_LRING2;   m_leftFingers[3].bones[2] = GB_LRING3;
    m_leftFingers[4].bones[0] = GB_LTHUMB1;  m_leftFingers[4].bones[1] = GB_LTHUMB2;  m_leftFingers[4].bones[2] = GB_LTHUMB3;

    // Right hand
    m_rightFingers[0].bones[0] = GB_RINDEX1;  m_rightFingers[0].bones[1] = GB_RINDEX2;  m_rightFingers[0].bones[2] = GB_RINDEX3;
    m_rightFingers[1].bones[0] = GB_RMIDDLE1; m_rightFingers[1].bones[1] = GB_RMIDDLE2; m_rightFingers[1].bones[2] = GB_RMIDDLE3;
    m_rightFingers[2].bones[0] = GB_RPINKY1;  m_rightFingers[2].bones[1] = GB_RPINKY2;  m_rightFingers[2].bones[2] = GB_RPINKY3;
    m_rightFingers[3].bones[0] = GB_RRING1;   m_rightFingers[3].bones[1] = GB_RRING2;   m_rightFingers[3].bones[2] = GB_RRING3;
    m_rightFingers[4].bones[0] = GB_RTHUMB1;  m_rightFingers[4].bones[1] = GB_RTHUMB2;  m_rightFingers[4].bones[2] = GB_RTHUMB3;
}

// ============================================================
// Axis-Angle to Quaternion
// ============================================================

void MocapRetargeter::SetReferencePose(const hkaSkeleton* skeleton)
{
    if (!skeleton) return;
    int count = skeleton->m_numBones;
    if (count > GAME_BONE_COUNT) count = GAME_BONE_COUNT;

    FILE* dbg = fopen("mocap_refpose_debug.log", "w");
    for (int i = 0; i < count; ++i)
    {
        m_refPoseLocal[i] = skeleton->m_referencePose[i].getRotation();
        if (dbg)
        {
            hkQuaternion q = m_refPoseLocal[i];
            hkVector4 t = skeleton->m_referencePose[i].getTranslation();
            const char* name = (i < skeleton->m_numBones && skeleton->m_bones[i])
                               ? skeleton->m_bones[i]->m_name : "?";
            int parent = (i < skeleton->m_numParentIndices) ? skeleton->m_parentIndices[i] : -1;
            fprintf(dbg, "Bone %2d [parent=%2d] %-22s rot=(%7.4f,%7.4f,%7.4f,%7.4f) pos=(%7.4f,%7.4f,%7.4f)\n",
                    i, parent, name,
                    q.m_vec(0), q.m_vec(1), q.m_vec(2), q.m_vec(3),
                    t(0), t(1), t(2));
        }
    }
    if (dbg) fclose(dbg);
    m_hasRefPose = true;
}

void MocapRetargeter::AxisAngleToQuaternion(const float* aa, hkQuaternion& outQ)
{
    float angle = sqrtf(aa[0]*aa[0] + aa[1]*aa[1] + aa[2]*aa[2]);
    if (angle < 1e-8f)
    {
        outQ.setIdentity();
        return;
    }

    float half = angle * 0.5f;
    float s = sinf(half) / angle;

    hkVector4 imag;
    imag.set(aa[0] * s, aa[1] * s, aa[2] * s, cosf(half));
    outQ.m_vec = imag;
}

// ============================================================
// Coordinate System Conversion (SMPL RHS Y-up -> Game D3D9 LHS Y-up)
// ============================================================
// WHAM demo.py uses return_y_up=True, so pose_world is already Y-UP.
// But SMPL is Right-Handed while the game (D3D9) is Left-Handed.
// Handedness flip = mirror across YZ plane = negate X axis.
// For quaternions: negate X and Z components.
// (Confirmed by smpl_to_conquest.py: q -> (-x, y, -z, w))

void MocapRetargeter::ConvertCoordSystem(hkQuaternion& q)
{
    // Both SMPL and game are RHS Y-up, same facing direction.
    // No conversion needed.
    (void)q;
}

void MocapRetargeter::ConvertTranslation(hkVector4& t)
{
    // SMPL faces +Z, game faces -Z. Negate X and Z for facing flip.
    t.set(-t(0), t(1), -t(2));
}

// ============================================================
// SLERP
// ============================================================

hkQuaternion MocapRetargeter::Slerp(const hkQuaternion& a, const hkQuaternion& b, float t)
{
    hkVector4 va = a.m_vec;
    hkVector4 vb = b.m_vec;

    float dot = va(0)*vb(0) + va(1)*vb(1) + va(2)*vb(2) + va(3)*vb(3);

    // If negative dot, negate one to take shorter path
    if (dot < 0.0f)
    {
        vb.set(-vb(0), -vb(1), -vb(2), -vb(3));
        dot = -dot;
    }

    if (dot > 0.9995f)
    {
        // Linear interpolation for nearly identical quaternions
        hkVector4 result;
        result.set(
            va(0) + t * (vb(0) - va(0)),
            va(1) + t * (vb(1) - va(1)),
            va(2) + t * (vb(2) - va(2)),
            va(3) + t * (vb(3) - va(3))
        );
        // Normalize
        float len = sqrtf(result(0)*result(0) + result(1)*result(1) + result(2)*result(2) + result(3)*result(3));
        if (len > 1e-8f)
        {
            float inv = 1.0f / len;
            result.set(result(0)*inv, result(1)*inv, result(2)*inv, result(3)*inv);
        }
        hkQuaternion r;
        r.m_vec = result;
        return r;
    }

    float theta0 = acosf(dot);
    float theta = theta0 * t;
    float sinTheta = sinf(theta);
    float sinTheta0 = sinf(theta0);

    float s0 = cosf(theta) - dot * sinTheta / sinTheta0;
    float s1 = sinTheta / sinTheta0;

    hkVector4 result;
    result.set(
        s0 * va(0) + s1 * vb(0),
        s0 * va(1) + s1 * vb(1),
        s0 * va(2) + s1 * vb(2),
        s0 * va(3) + s1 * vb(3)
    );

    hkQuaternion r;
    r.m_vec = result;
    return r;
}

// ============================================================
// Make a rotation around local X axis (for finger curl)
// ============================================================

hkQuaternion MocapRetargeter::MakeRotationX(float angleDeg)
{
    float rad = angleDeg * HK_REAL_PI / 180.0f;
    float half = rad * 0.5f;
    hkQuaternion q;
    hkVector4 v;
    v.set(sinf(half), 0.0f, 0.0f, cosf(half));
    q.m_vec = v;
    return q;
}

// ============================================================
// Debug Logging (writes once for first frame)
// ============================================================

static bool s_loggedFrame0 = false;

static void LogRetargetFrame0(FILE* log, int smplJoint, int gameBone,
                               const float* axisAngle,
                               const hkQuaternion& rawQ,
                               const hkQuaternion& flippedQ,
                               const hkQuaternion& finalQ,
                               const hkQuaternion* refPose,
                               const char* method)
{
    if (!log) return;
    static const char* smplNames[] = {
        "PELVIS","L_HIP","R_HIP","SPINE1","L_KNEE","R_KNEE",
        "SPINE2","L_ANKLE","R_ANKLE","SPINE3","L_FOOT","R_FOOT",
        "NECK","L_COLLAR","R_COLLAR","HEAD","L_SHOULDER","R_SHOULDER",
        "L_ELBOW","R_ELBOW","L_WRIST","R_WRIST","L_HAND","R_HAND"
    };
    fprintf(log, "SMPL[%2d] %-12s -> GameBone[%2d]  method=%s\n",
            smplJoint, smplNames[smplJoint], gameBone, method);
    fprintf(log, "  axis_angle  = (%8.5f, %8.5f, %8.5f)  |angle|=%.5f rad\n",
            axisAngle[0], axisAngle[1], axisAngle[2],
            sqrtf(axisAngle[0]*axisAngle[0] + axisAngle[1]*axisAngle[1] + axisAngle[2]*axisAngle[2]));
    fprintf(log, "  raw_quat    = (%8.5f, %8.5f, %8.5f, %8.5f)\n",
            rawQ(0), rawQ(1), rawQ(2), rawQ(3));
    fprintf(log, "  flipped_quat= (%8.5f, %8.5f, %8.5f, %8.5f)\n",
            flippedQ(0), flippedQ(1), flippedQ(2), flippedQ(3));
    if (refPose)
    {
        fprintf(log, "  ref_pose    = (%8.5f, %8.5f, %8.5f, %8.5f)\n",
                (*refPose)(0), (*refPose)(1), (*refPose)(2), (*refPose)(3));
    }
    fprintf(log, "  final_quat  = (%8.5f, %8.5f, %8.5f, %8.5f)\n",
            finalQ(0), finalQ(1), finalQ(2), finalQ(3));
    fprintf(log, "\n");
}

// ============================================================
// Retarget a Single Frame
// ============================================================

void MocapRetargeter::RetargetFrame(const float* smplPose, const float* smplTrans,
                                     const float* contact, RetargetedFrame& outFrame)
{
    outFrame.frameIndex = 0;
    memset(&outFrame, 0, sizeof(outFrame));

    // Work in a local aligned buffer, then copy to flat floats
    HK_ALIGN16( hkQuaternion boneWork[GAME_BONE_COUNT] );
    for (int i = 0; i < GAME_BONE_COUNT; ++i)
        boneWork[i].setIdentity();

    // Step 1: Direct SMPL -> Game mapping
    ApplyDirectMapping(smplPose, boneWork);

    // Step 2: Spine and foot interpolation
    ApplySpineInterpolation(boneWork);

    // Step 3: Procedural bones (forearm twist, attachments, jaw)
    ApplyProceduralBones(boneWork);

    // Step 4: Finger curl
    ApplyFingerCurl(boneWork);

    // Copy quaternions to flat float array
    for (int i = 0; i < GAME_BONE_COUNT; ++i)
        outFrame.SetBoneQuat(i, boneWork[i]);

    // Root translation
    hkVector4 trans;
    trans.set(smplTrans[0], smplTrans[1], smplTrans[2]);
    ConvertTranslation(trans);
    outFrame.SetRootTrans(trans);

    // Contact data
    if (contact)
    {
        outFrame.footContact[0] = contact[0];
        outFrame.footContact[1] = contact[1];
        outFrame.footContact[2] = contact[2];
        outFrame.footContact[3] = contact[3];
    }
}

// ============================================================
// Retarget All Frames
// ============================================================

void MocapRetargeter::RetargetAll(const MocapBridge& bridge,
                                   std::vector<RetargetedFrame>& outFrames)
{
    const std::vector<MocapFrame>& frames = bridge.GetFrames();
    outFrames.resize(frames.size());

    for (size_t i = 0; i < frames.size(); ++i)
    {
        RetargetFrame(frames[i].pose, frames[i].trans,
                      frames[i].hasContact ? frames[i].contact : NULL,
                      outFrames[i]);
        outFrames[i].frameIndex = frames[i].frameIndex;
    }

    // ---------------------------------------------------------------
    // Post-retarget smoothing pass
    // WHAM/SMPL output has significant high-frequency noise on
    // finger and foot joints. Apply two passes:
    //   1) Quaternion neighborhood correction (sign consistency)
    //   2) Savitzky-Golay filter (degree-2, window-5) per bone
    // ---------------------------------------------------------------
    const int N = (int)outFrames.size();
    if (N < 3) return; // need at least 3 frames for any smoothing

    // --- Pass 1: Quaternion neighborhood correction per bone ---
    // Ensures each bone's quaternion track lives in one hemisphere
    // so smoothing doesn't average across sign flips.
    for (int b = 0; b < GAME_BONE_COUNT; ++b)
    {
        for (int f = 1; f < N; ++f)
        {
            float dot = 0.0f;
            for (int c = 0; c < 4; ++c)
                dot += outFrames[f - 1].boneRotations[b][c] * outFrames[f].boneRotations[b][c];
            if (dot < 0.0f)
            {
                for (int c = 0; c < 4; ++c)
                    outFrames[f].boneRotations[b][c] = -outFrames[f].boneRotations[b][c];
            }
        }
    }

    // --- Pass 2: Savitzky-Golay smoothing (quadratic, window=5) ---
    // SG coefficients for 5-point quadratic smoothing:
    //   [-3, 12, 17, 12, -3] / 35
    // This preserves peaks better than a moving-average while
    // eliminating jitter. At boundaries we fall back to 3-point:
    //   [1, 1, 1] / 3
    if (N >= 5)
    {
        // Smooth bone rotations
        // Work on a copy to avoid feedback between frames
        std::vector<RetargetedFrame> smoothed(outFrames);

        for (int b = 0; b < GAME_BONE_COUNT; ++b)
        {
            for (int f = 0; f < N; ++f)
            {
                if (f >= 2 && f < N - 2)
                {
                    // 5-point Savitzky-Golay
                    for (int c = 0; c < 4; ++c)
                    {
                        smoothed[f].boneRotations[b][c] =
                            (-3.0f * outFrames[f-2].boneRotations[b][c]
                            + 12.0f * outFrames[f-1].boneRotations[b][c]
                            + 17.0f * outFrames[f].boneRotations[b][c]
                            + 12.0f * outFrames[f+1].boneRotations[b][c]
                            -  3.0f * outFrames[f+2].boneRotations[b][c]) / 35.0f;
                    }
                }
                else if (f >= 1 && f < N - 1)
                {
                    // 3-point fallback at boundaries
                    for (int c = 0; c < 4; ++c)
                    {
                        smoothed[f].boneRotations[b][c] =
                            (outFrames[f-1].boneRotations[b][c]
                            + outFrames[f].boneRotations[b][c]
                            + outFrames[f+1].boneRotations[b][c]) / 3.0f;
                    }
                }
                // else: boundary frames keep original values
            }
        }

        // Smooth root translation (same SG filter)
        for (int f = 0; f < N; ++f)
        {
            if (f >= 2 && f < N - 2)
            {
                for (int c = 0; c < 3; ++c)
                {
                    smoothed[f].rootTranslation[c] =
                        (-3.0f * outFrames[f-2].rootTranslation[c]
                        + 12.0f * outFrames[f-1].rootTranslation[c]
                        + 17.0f * outFrames[f].rootTranslation[c]
                        + 12.0f * outFrames[f+1].rootTranslation[c]
                        -  3.0f * outFrames[f+2].rootTranslation[c]) / 35.0f;
                }
            }
            else if (f >= 1 && f < N - 1)
            {
                for (int c = 0; c < 3; ++c)
                {
                    smoothed[f].rootTranslation[c] =
                        (outFrames[f-1].rootTranslation[c]
                        + outFrames[f].rootTranslation[c]
                        + outFrames[f+1].rootTranslation[c]) / 3.0f;
                }
            }
        }

        // Re-normalize smoothed quaternions (SG can denormalize them)
        for (int f = 0; f < N; ++f)
        {
            for (int b = 0; b < GAME_BONE_COUNT; ++b)
            {
                float* q = smoothed[f].boneRotations[b];
                float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
                if (len > 1e-8f)
                {
                    float inv = 1.0f / len;
                    q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
                }
            }
        }

        outFrames.swap(smoothed);
    }
}

// ============================================================
// Internal: Direct Mapping
// ============================================================

void MocapRetargeter::ApplyDirectMapping(const float* smplPose, hkQuaternion* boneRots)
{
    // Open debug log for first frame only
    FILE* dbgLog = NULL;
    if (!s_loggedFrame0)
    {
        dbgLog = fopen("mocap_retarget_debug.log", "w");
        if (dbgLog)
        {
            fprintf(dbgLog, "=== MocapRetargeter Frame 0 Debug Log ===\n");
            fprintf(dbgLog, "Both SMPL (WHAM return_y_up=True) and Havok are RHS Y-up.\n");
            fprintf(dbgLog, "No coordinate conversion needed.\n");
            fprintf(dbgLog, "Formula: result = rawQ * ref  (SMPL rotation in parent frame after rest)\n");
            fprintf(dbgLog, "  - setMul(rawQ,ref) = rawQ*ref = apply ref first, then rawQ in parent frame\n");
            fprintf(dbgLog, "  - When SMPL Q = identity: result = refPose (correct rest)\n\n");
        }
    }

    for (int j = 0; j < SMPL_JOINT_COUNT; ++j)
    {
        int gameBone = m_smplToGame[j];
        if (gameBone < 0) continue;

        // Skip globalsrt only
        if (gameBone == GB_GLOBALSRT)
            continue;

        const float* aa = &smplPose[j * 3];
        hkQuaternion rawQ;
        AxisAngleToQuaternion(aa, rawQ);

        if (gameBone == GB_ROOT)
        {
            // Root ref = identity. Body uses invQ so root must match.
            // invQ alone faces backward, so add 180 deg Y flip.
            // 180 deg Y quat = (0, 1, 0, 0)
            static hkQuaternion s_yFlip;
            static bool s_yFlipInit = false;
            if (!s_yFlipInit)
            {
                hkVector4 v; v.set(0.0f, 1.0f, 0.0f, 0.0f);
                s_yFlip.m_vec = v;
                s_yFlipInit = true;
            }
            hkQuaternion invQ;
            invQ.setInverse(rawQ);
            hkQuaternion result;
            result.setMul(s_yFlip, invQ);
            result.normalize();
            boneRots[gameBone] = result;

            if (dbgLog)
                LogRetargetFrame0(dbgLog, j, gameBone, aa, rawQ, invQ, result, NULL, "ROOT: yFlip * invQ");
        }
        else if (m_hasRefPose && gameBone < GAME_BONE_COUNT)
        {
            // Body: invQ * ref gives correct bend directions
            // (mathematically verified for spine + thigh).
            hkQuaternion ref = m_refPoseLocal[gameBone];
            hkQuaternion invQ;
            invQ.setInverse(rawQ);

            hkQuaternion result;
            result.setMul(invQ, ref);
            result.normalize();

            boneRots[gameBone] = result;

            if (dbgLog)
                LogRetargetFrame0(dbgLog, j, gameBone, aa, rawQ, invQ, result, &ref, "BODY: invQ * ref");
        }
        else
        {
            boneRots[gameBone] = rawQ;

            if (dbgLog)
                LogRetargetFrame0(dbgLog, j, gameBone, aa, rawQ, rawQ, rawQ, NULL, "PASSTHROUGH");
        }
    }

    if (dbgLog)
    {
        fprintf(dbgLog, "\n=== SMPL raw translation (frame 0) ===\n");
        fprintf(dbgLog, "  trans = (%8.5f, %8.5f, %8.5f)\n",
                smplPose ? smplPose[0] : 0.0f, smplPose ? smplPose[1] : 0.0f, smplPose ? smplPose[2] : 0.0f);
        fclose(dbgLog);
        s_loggedFrame0 = true;
    }
}

// ============================================================
// Internal: Spine & Foot Interpolation
// ============================================================

void MocapRetargeter::ApplySpineInterpolation(hkQuaternion* boneRots)
{
    for (size_t i = 0; i < m_spineInterps.size(); ++i)
    {
        const SpineInterp& si = m_spineInterps[i];
        boneRots[si.targetBone] = Slerp(boneRots[si.boneA], boneRots[si.boneB], si.t);
    }
}

// ============================================================
// Internal: Procedural Bones
// ============================================================

void MocapRetargeter::ApplyProceduralBones(hkQuaternion* boneRots)
{
    for (size_t i = 0; i < m_proceduralBones.size(); ++i)
    {
        boneRots[m_proceduralBones[i].targetBone] = boneRots[m_proceduralBones[i].sourceBone];
    }
}

// ============================================================
// Internal: Finger Curl
// ============================================================

void MocapRetargeter::ApplyFingerCurl(hkQuaternion* boneRots)
{
    // Left hand
    float leftAngle = fingerCurl.leftGrip * fingerCurl.maxCurlDeg;
    for (int f = 0; f < 5; ++f)
    {
        float angle = (f == 4) ? leftAngle * 0.7f : leftAngle; // thumb curls less
        for (int p = 0; p < 3; ++p)
        {
            float pAngle = angle * (1.0f - 0.2f * (float)p); // each phalanx curls slightly less
            boneRots[m_leftFingers[f].bones[p]] = MakeRotationX(pAngle);
        }
    }

    // Right hand
    float rightAngle = fingerCurl.rightGrip * fingerCurl.maxCurlDeg;
    for (int f = 0; f < 5; ++f)
    {
        float angle = (f == 4) ? rightAngle * 0.7f : rightAngle;
        for (int p = 0; p < 3; ++p)
        {
            float pAngle = angle * (1.0f - 0.2f * (float)p);
            boneRots[m_rightFingers[f].bones[p]] = MakeRotationX(pAngle);
        }
    }
}

// ============================================================
// Internal: Set Identity
// ============================================================

void MocapRetargeter::SetIdentity(hkQuaternion* boneRots, int boneIdx)
{
    boneRots[boneIdx].setIdentity();
}
