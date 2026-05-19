// ============================================================================
// MocapRetargeter.h -- Cramming 24 SMPL Joints into 62 Game Bones
// ============================================================================
//
// The human body model (SMPL) has 24 joints. Conquest's skeleton has 62
// bones including individual finger phalanges, forearm roll bones, jaw,
// weapon attachments, and a deeply cursed spine hierarchy. This class
// bridges that gap through direct mapping, spine interpolation, procedural
// bones, and finger curl synthesis.
//
// The skeleton hierarchy is INSANE. UpperBody (bone 4) parents BOTH the
// spine chain AND the thighs. If you map SMPL_SPINE2 to UpperBody the
// entire lower body tilts sideways. Found that one at 3 AM after the
// character turned into a goddamn pretzel. UpperBody stays at identity.
// Lumbar2/3 get interpolated between Lumbar1 and Neck via SLERP.
//
// RetargetedFrame uses plain floats instead of hkQuaternion because
// VS2005's std::vector refuses to work with __declspec(align(16)) types.
// Havok's alignment requirements are a constant source of suffering.
// The stolen SDK giveth and the stolen SDK taketh away.
//
// Coordinate conversion: SMPL is RHS Y-up, game is also Y-up but
// faces opposite Z. Translation gets X and Z negated. Rotations use
// invQ * refPose to get correct bend directions. This took 2 weeks
// and a Python visualization script to figure out. See the debug logs.
//
// VS2005 compatible (C++03). Havok 5.5.0 types throughout.
// ============================================================================

#ifndef MOCAP_RETARGETER_H
#define MOCAP_RETARGETER_H

#include "MocapBridge.h"
#include <Common/Base/hkBase.h>
#include <Common/Base/Math/Quaternion/hkQuaternion.h>
#include <Common/Base/Math/Vector/hkVector4.h>
#include <Animation/Animation/Rig/hkaSkeleton.h>
#include <Animation/Animation/Rig/hkaPose.h>
#include <vector>

// Number of bones in the game skeleton
#define GAME_BONE_COUNT 62

// Number of joints in SMPL
#define SMPL_JOINT_COUNT 24

// SMPL joint indices
enum SMPLJoint
{
    SMPL_PELVIS     = 0,
    SMPL_L_HIP      = 1,
    SMPL_R_HIP      = 2,
    SMPL_SPINE1     = 3,
    SMPL_L_KNEE     = 4,
    SMPL_R_KNEE     = 5,
    SMPL_SPINE2     = 6,
    SMPL_L_ANKLE    = 7,
    SMPL_R_ANKLE    = 8,
    SMPL_SPINE3     = 9,
    SMPL_L_FOOT     = 10,
    SMPL_R_FOOT     = 11,
    SMPL_NECK       = 12,
    SMPL_L_COLLAR   = 13,
    SMPL_R_COLLAR   = 14,
    SMPL_HEAD       = 15,
    SMPL_L_SHOULDER = 16,
    SMPL_R_SHOULDER = 17,
    SMPL_L_ELBOW    = 18,
    SMPL_R_ELBOW    = 19,
    SMPL_L_WRIST    = 20,
    SMPL_R_WRIST    = 21,
    SMPL_L_HAND     = 22,
    SMPL_R_HAND     = 23
};

// Game bone indices (matching the bones[] array in animation JSONs)
enum GameBone
{
    GB_EMPTY            = 0,
    GB_GLOBALSRT        = 1,
    GB_ROOT             = 2,
    GB_LUMBAR1          = 3,
    GB_UPPERBODY        = 4,
    GB_LUMBAR2          = 5,
    GB_LUMBAR3          = 6,
    GB_LSHOULDER        = 7,
    GB_NECK             = 8,
    GB_RSHOULDER        = 9,
    GB_LBICEP           = 10,
    GB_LFOREARM         = 11,
    GB_LFOREARMROLL     = 12,
    GB_LHAND            = 13,
    GB_LHAND_ATTACH     = 14,
    GB_LINDEX1          = 15,
    GB_LMIDDLE1         = 16,
    GB_LPINKY1          = 17,
    GB_LRING1           = 18,
    GB_LTHUMB1          = 19,
    GB_LINDEX2          = 20,
    GB_LINDEX3          = 21,
    GB_LMIDDLE2         = 22,
    GB_LMIDDLE3         = 23,
    GB_LPINKY2          = 24,
    GB_LPINKY3          = 25,
    GB_LRING2           = 26,
    GB_LRING3           = 27,
    GB_LTHUMB2          = 28,
    GB_LTHUMB3          = 29,
    GB_HEAD             = 30,
    GB_JAW              = 31,
    GB_RBICEP           = 32,
    GB_RFOREARM         = 33,
    GB_RFOREARMROLL     = 34,
    GB_RHAND            = 35,
    GB_RHAND_ATTACH     = 36,
    GB_RINDEX1          = 37,
    GB_RMIDDLE1         = 38,
    GB_RPINKY1          = 39,
    GB_RRING1           = 40,
    GB_RTHUMB1          = 41,
    GB_RINDEX2          = 42,
    GB_RINDEX3          = 43,
    GB_RMIDDLE2         = 44,
    GB_RMIDDLE3         = 45,
    GB_RPINKY2          = 46,
    GB_RPINKY3          = 47,
    GB_RRING2           = 48,
    GB_RRING3           = 49,
    GB_RTHUMB2          = 50,
    GB_RTHUMB3          = 51,
    GB_LTHIGH           = 52,
    GB_RTHIGH           = 53,
    GB_LSHIN            = 54,
    GB_LFOOTBONE1       = 55,
    GB_LFOOTBONE2       = 56,
    GB_LFOOTBONE3       = 57,
    GB_RSHIN            = 58,
    GB_RFOOTBONE1       = 59,
    GB_RFOOTBONE2       = 60,
    GB_RFOOTBONE3       = 61
};

// Retargeted frame -- 62 bone quaternions + root translation
// Uses plain floats (not hkQuaternion) to be std::vector-safe on VS2005
// (hkQuaternion has __declspec(align(16)) which VS2005 std::vector rejects)
struct RetargetedFrame
{
    float boneRotations[GAME_BONE_COUNT][4]; // [bone][x,y,z,w] quaternion
    float rootTranslation[3];                // XYZ
    float footContact[4];                    // L_heel, L_toe, R_heel, R_toe
    int   frameIndex;

    // Helpers to get/set as hkQuaternion
    void SetBoneQuat(int boneIdx, const hkQuaternion& q)
    {
        boneRotations[boneIdx][0] = q.m_vec(0);
        boneRotations[boneIdx][1] = q.m_vec(1);
        boneRotations[boneIdx][2] = q.m_vec(2);
        boneRotations[boneIdx][3] = q.m_vec(3);
    }
    hkQuaternion GetBoneQuat(int boneIdx) const
    {
        hkQuaternion q;
        hkVector4 v;
        v.set(boneRotations[boneIdx][0], boneRotations[boneIdx][1],
              boneRotations[boneIdx][2], boneRotations[boneIdx][3]);
        q.m_vec = v;
        return q;
    }
    void SetRootTrans(const hkVector4& t)
    {
        rootTranslation[0] = t(0);
        rootTranslation[1] = t(1);
        rootTranslation[2] = t(2);
    }
};

// Parameters for finger curl
struct FingerCurlParams
{
    float leftGrip;    // 0 = open, 1 = fist
    float rightGrip;   // 0 = open, 1 = fist
    float maxCurlDeg;  // max curl per phalanx (default 60)

    FingerCurlParams() : leftGrip(0.3f), rightGrip(0.3f), maxCurlDeg(60.0f) {}
};

class MocapRetargeter
{
public:
    MocapRetargeter();

    // Initialize mapping tables
    void Initialize();

    // Set reference pose from game skeleton (call once after skeleton is loaded)
    void SetReferencePose(const hkaSkeleton* skeleton);
    bool HasReferencePose() const { return m_hasRefPose; }

    // Convert a single SMPL frame to game skeleton
    // smplPose: 72 floats (24 joints * 3 axis-angle, world coords)
    // smplTrans: 3 floats (root XYZ, world coords)
    // contact: 4 floats (foot contact, can be NULL)
    void RetargetFrame(const float* smplPose, const float* smplTrans,
                       const float* contact, RetargetedFrame& outFrame);

    // Convert all frames from MocapBridge
    void RetargetAll(const MocapBridge& bridge, std::vector<RetargetedFrame>& outFrames);

    // Get a bone rotation for rendering the SMPL skeleton directly
    // (before retargeting, for preview)
    static void AxisAngleToQuaternion(const float* aa, hkQuaternion& outQ);

    // Coordinate system conversion (SMPL RHS -> Game LHS)
    static void ConvertCoordSystem(hkQuaternion& q);
    static void ConvertTranslation(hkVector4& t);

    // Parameters
    FingerCurlParams fingerCurl;

private:
    // SMPL joint -> game bone mapping table
    // -1 means no direct mapping
    int m_smplToGame[SMPL_JOINT_COUNT];

    // Procedural bone sources (bone_idx -> source_bone_idx)
    struct ProceduralBone
    {
        int targetBone;
        int sourceBone;
    };
    std::vector<ProceduralBone> m_proceduralBones;

    // Spine interpolation entries
    struct SpineInterp
    {
        int targetBone;
        int boneA;
        int boneB;
        float t;
    };
    std::vector<SpineInterp> m_spineInterps;

    // Finger bone indices for procedural curl
    struct FingerChain
    {
        int bones[3]; // phalanx 1, 2, 3
    };
    FingerChain m_leftFingers[5];   // index, middle, pinky, ring, thumb
    FingerChain m_rightFingers[5];

    // Internal
    void ApplyDirectMapping(const float* smplPose, hkQuaternion* boneRots);

    // Skeleton reference pose (set from game skeleton before retargeting)
    bool m_hasRefPose;
    hkQuaternion m_refPoseLocal[GAME_BONE_COUNT]; // reference pose local rotations
    void ApplySpineInterpolation(hkQuaternion* boneRots);
    void ApplyProceduralBones(hkQuaternion* boneRots);
    void ApplyFingerCurl(hkQuaternion* boneRots);
    void SetIdentity(hkQuaternion* boneRots, int boneIdx);

    static hkQuaternion Slerp(const hkQuaternion& a, const hkQuaternion& b, float t);
    static hkQuaternion MakeRotationX(float angleDeg);
};

#endif // MOCAP_RETARGETER_H
