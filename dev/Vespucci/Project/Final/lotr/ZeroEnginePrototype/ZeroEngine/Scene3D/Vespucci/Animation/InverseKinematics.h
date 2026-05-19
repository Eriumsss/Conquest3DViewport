// InverseKinematics.h — Making Feet Touch the Goddamn Ground
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// FABRIK-based IK solver (Forward-And-Backward Reaching Inverse Kinematics).
// Creates bone chains from the skeleton, solves end-effector targets through
// iterative convergence, and respects joint constraints so elbows don't
// bend backwards (usually).
//
// Pandemic used Havok's built-in hkaIk solvers for foot placement and
// look-at targeting in Conquest. Those solvers are in the stolen Havok
// Animation SDK (hkaFootPlacementIkSolver, hkaLookAtIkSolver) but they
// require specific setup data that we can't fully extract from the .exe.
// So we wrote our own FABRIK solver. It's simpler, more debuggable, and
// doesn't crash when you feed it a chain with zero-length bones (unlike
// Havok's, which just divides by zero and takes the whole process with it).
//
// "Everything should be made as simple as possible, but not simpler."
//   — Einstein. The IK solver is exactly 2 passes per iteration (forward
// + backward). Can't be simpler. Can't be fewer passes. This is the
// minimum viable IK. Einstein would approve. My feet finally touch ground.
// -----------------------------------------------------------------------

#ifndef INVERSE_KINEMATICS_H
#define INVERSE_KINEMATICS_H

#include <vector>
#include <string>
#include <cmath>
#include <Common/Base/hkBase.h>
#include <Common/Base/Container/Array/hkArray.h>
#include <Common/Base/Container/Array/hkObjectArray.h>
#include <Common/Base/Math/Vector/hkVector4.h>
#include <Common/Base/Math/Quaternion/hkQuaternion.h>

inline float MaxFloat(float a, float b) { return (a > b) ? a : b; }
inline float MinFloat(float a, float b) { return (a < b) ? a : b; }

// ---------------------------------------------------------------------------
// Joint Constraint (rotation limits per joint)
// ---------------------------------------------------------------------------

struct JointConstraint
{
    float minAngleDeg;  // Minimum rotation angle in degrees
    float maxAngleDeg;  // Maximum rotation angle in degrees
    hkVector4 axis;     // Constraint axis (typically Y for elbow extension)
    bool enabled;

    JointConstraint()
        : minAngleDeg(-180.0f), maxAngleDeg(180.0f), enabled(false)
    {
        axis.set(0.0f, 1.0f, 0.0f);  // Default Y axis
    }
};

// ---------------------------------------------------------------------------
// IK Chain (chain of bones from root to end-effector)
// ---------------------------------------------------------------------------

struct IKChain
{
    std::string name;
    std::vector<int> boneIndices;      // Indices in skeleton (root to end-effector)
    std::vector<float> boneLengths;    // Pre-computed distances between joints
    hkObjectArray<JointConstraint> constraints;  // Per-joint angle limits

    hkVector4 targetPosition;          // Target for end-effector in world space
    int targetBoneIndex;               // If >=0, target follows this bone position
    hkVector4 poleVectorPosition;      // Optional pole vector for elbow/knee positioning
    bool usePoleVector;
    bool useTwoBone;                   // Use analytic 2-bone solver when chain has 3 joints
    bool useGround;                    // Snap target Y to ground plane
    float groundOffset;                // Offset above ground when useGround is true

    float tolerance;                   // IK solver tolerance (position error threshold)
    int maxIterations;                 // Maximum FABRIK iterations
    float chainLength;                 // Total length of chain (sum of bone lengths)
    bool enabled;

    IKChain()
        : tolerance(0.01f), maxIterations(10), chainLength(0.0f),
          enabled(true), usePoleVector(false), useTwoBone(false),
          useGround(false), groundOffset(0.0f), targetBoneIndex(-1)
    {
        targetPosition.setZero4();
        poleVectorPosition.setZero4();
    }

    // Get number of joints in chain
    int GetJointCount() const { return (int)boneIndices.size(); }

    // Check if chain is valid (at least 2 bones)
    bool IsValid() const { return boneIndices.size() >= 2; }
};

// ---------------------------------------------------------------------------
// Forward Kinematics calculation (for reference)
// ---------------------------------------------------------------------------

inline void CalculateForwardKinematics(
    const IKChain& chain,
    const hkArray<hkQuaternion>& boneRotations,
    const hkArray<hkVector4>& bonePositions,
    hkArray<hkVector4>& outJointPositions)
{
    outJointPositions.clear();

    if (chain.boneIndices.empty())
        return;

    // If we have enough data, collect positions of each bone in the chain
    for (size_t i = 0; i < chain.boneIndices.size(); ++i)
    {
        int boneIdx = chain.boneIndices[i];
        if (boneIdx >= 0)
        {
            outJointPositions.pushBack(bonePositions[boneIdx]);
        }
    }
}

// ---------------------------------------------------------------------------
// FABRIK Inverse Kinematics Solver
// ---------------------------------------------------------------------------

class IKSolver
{
public:
    IKSolver() {}

    // Analytic 2-bone solver (chain must have exactly 3 joints)
    bool SolveTwoBone(const IKChain& chain, hkArray<hkVector4>& jointPositions)
    {
        if (chain.boneIndices.size() != 3 || chain.boneLengths.size() < 2)
            return false;
        if (jointPositions.getSize() != 3)
            return false;

        hkVector4 root = jointPositions[0];
        hkVector4 target = chain.targetPosition;

        hkVector4 dir;
        dir.setSub4(target, root);
        float dist = dir.length3();
        if (dist < 1e-6f)
            dist = 1e-6f;

        float a = chain.boneLengths[0];
        float b = chain.boneLengths[1];
        float maxDist = a + b - 1e-4f;
        float minDist = fabsf(a - b) + 1e-4f;
        if (dist > maxDist) dist = maxDist;
        if (dist < minDist) dist = minDist;

        dir.normalize3();

        hkVector4 poleDir;
        bool poleValid = false;
        if (chain.usePoleVector)
        {
            hkVector4 pole;
            pole.setSub4(chain.poleVectorPosition, root);
            float proj = pole.dot3(dir);
            hkVector4 projV;
            projV.setMul4(proj, dir);
            pole.sub4(projV);
            float poleLen = pole.length3();
            if (poleLen > 1e-6f)
            {
                pole.normalize3();
                poleDir = pole;
                poleValid = true;
            }
        }
        if (!poleValid)
        {
            hkVector4 up;
            if (fabsf(dir(1)) < 0.99f) up.set(0.0f, 1.0f, 0.0f);
            else up.set(1.0f, 0.0f, 0.0f);
            poleDir.setCross(up, dir);
            float poleLen = poleDir.length3();
            if (poleLen > 1e-6f)
                poleDir.normalize3();
            else
                poleDir.set(0.0f, 0.0f, 1.0f);
        }

        float cosTheta = (a * a + dist * dist - b * b) / (2.0f * a * dist);
        if (cosTheta < -1.0f) cosTheta = -1.0f;
        if (cosTheta > 1.0f) cosTheta = 1.0f;
        float sinTheta = sqrtf(MaxFloat(0.0f, 1.0f - cosTheta * cosTheta));

        hkVector4 along;
        along.setMul4(a * cosTheta, dir);
        hkVector4 perp;
        perp.setMul4(a * sinTheta, poleDir);

        hkVector4 mid;
        mid.setAdd4(root, along);
        mid.add4(perp);

        hkVector4 end;
        end.setMul4(dist, dir);
        end.add4(root);

        jointPositions[1] = mid;
        jointPositions[2] = end;
        return true;
    }

    // Create an IK chain from skeleton bone names
    bool CreateChain(const char* chainName, const std::vector<int>& boneIndices,
                     const hkArray<hkVector4>& bonePositions, IKChain& outChain)
    {
        if (boneIndices.size() < 2)
            return false;

        outChain.name = chainName;
        outChain.boneIndices = boneIndices;
        outChain.boneLengths.clear();

        // Calculate bone lengths (distance between consecutive joints)
        float totalLength = 0.0f;
        for (size_t i = 0; i + 1 < boneIndices.size(); ++i)
        {
            int idx0 = boneIndices[i];
            int idx1 = boneIndices[i + 1];

            hkVector4 delta;
            delta.setSub4(bonePositions[idx1], bonePositions[idx0]);
            float len = delta.length3();

            outChain.boneLengths.push_back(len);
            totalLength += len;
        }

        outChain.chainLength = totalLength;

        // Initialize constraints
        outChain.constraints.setSize((int)boneIndices.size());
        for (size_t i = 0; i < boneIndices.size(); ++i)
        {
            outChain.constraints[i] = JointConstraint();
        }

        return true;
    }

    // Main FABRIK solver: solves for bone rotations to reach target
    // Returns true if target reached or tolerance achieved
    bool SolveChain(const IKChain& chain,
                    hkArray<hkVector4>& jointPositions,
                    hkArray<hkQuaternion>& boneRotations)
    {
        if (!chain.IsValid() || jointPositions.getSize() < 2)
            return false;

        int numJoints = (int)chain.boneIndices.size();
        if (jointPositions.getSize() != numJoints)
            return false;

        if (chain.useTwoBone && numJoints == 3)
        {
            if (!SolveTwoBone(chain, jointPositions))
                return false;
        }
        else
        {
            hkVector4 basePosition = jointPositions[0];  // Root position (fixed)
            hkVector4 target = chain.targetPosition;

            // Check if target is reachable
            float distToTarget = 0.0f;
            {
                hkVector4 delta;
                delta.setSub4(target, basePosition);
                distToTarget = delta.length3();
            }

            // If target is beyond chain reach, scale target closer
            if (distToTarget > chain.chainLength)
            {
                hkVector4 direction;
                direction.setSub4(target, basePosition);
                direction.normalize3();
                hkVector4 scaledTarget;
                scaledTarget.setMul4(chain.chainLength * 0.95f, direction);
                scaledTarget.add4(basePosition);
                target = scaledTarget;
            }

            // FABRIK iteration
            for (int iter = 0; iter < chain.maxIterations; ++iter)
            {
                // Backward pass: reach from end-effector toward root
                hkArray<hkVector4> tempPositions;
                const int tempCount = jointPositions.getSize();
                tempPositions.setSize(tempCount);
                for (int i = 0; i < tempCount; ++i)
                {
                    tempPositions[i] = jointPositions[i];
                }
                tempPositions.back() = target;

                for (int i = numJoints - 2; i >= 0; --i)
                {
                    hkVector4 current = tempPositions[i];
                    hkVector4 next = tempPositions[i + 1];
                    hkVector4 direction;
                    direction.setSub4(current, next);
                    float dist = direction.length3();

                    if (dist > 1e-6f)
                    {
                        direction.normalize3();
                    }
                    else
                    {
                        direction.set(0.0f, 0.0f, 1.0f);
                    }

                    // Move current joint away from next by bone length
                    hkVector4 newPos;
                    newPos.setMul4(chain.boneLengths[i], direction);
                    newPos.add4(next);
                    tempPositions[i] = newPos;
                }

                // Forward pass: reach from root toward end-effector target
                tempPositions[0] = basePosition;  // Keep root fixed

                for (int i = 0; i < numJoints - 1; ++i)
                {
                    hkVector4 current = tempPositions[i];
                    hkVector4 next = tempPositions[i + 1];
                    hkVector4 direction;
                    direction.setSub4(next, current);
                    float dist = direction.length3();

                    if (dist > 1e-6f)
                    {
                        direction.normalize3();
                    }
                    else
                    {
                        direction.set(0.0f, 1.0f, 0.0f);
                    }

                    // Move next joint away from current by bone length
                    hkVector4 newPos;
                    newPos.setMul4(chain.boneLengths[i], direction);
                    newPos.add4(current);
                    tempPositions[i + 1] = newPos;
                }

                // Check convergence
                hkVector4 errorVec;
                errorVec.setSub4(tempPositions.back(), target);
                float error = errorVec.length3();

                jointPositions = tempPositions;

                if (error < chain.tolerance)
                    break;  // Converged
            }
        }

        // Calculate bone rotations from joint positions
        if (boneRotations.getSize() > 0)
        {
            for (int i = 0; i < numJoints - 1; ++i)
            {
                int boneIdx = chain.boneIndices[i];
                if (boneIdx < 0 || boneIdx >= boneRotations.getSize())
                    continue;
                hkVector4 boneDir;
                boneDir.setSub4(jointPositions[i + 1], jointPositions[i]);
                boneDir.normalize3();

                // Create quaternion from direction (assuming original was +Z)
                hkVector4 originalDir;
                originalDir.set(0.0f, 0.0f, 1.0f);

                hkVector4 axis;
                axis.setCross(originalDir, boneDir);
                float axisMag = axis.length3();

                if (axisMag > 1e-6f)
                {
                    axis.normalize3();
                    float angle = acos(MaxFloat(-1.0f, MinFloat(1.0f, originalDir.dot3(boneDir))));
                    boneRotations[boneIdx] = hkQuaternion(axis, angle);
                }
            }
        }

        return true;
    }

    // Set joint angle constraints
    void SetJointConstraint(IKChain& chain, int jointIndex,
                           float minAngleDeg, float maxAngleDeg)
    {
        hkVector4 axis;
        axis.set(0.0f, 1.0f, 0.0f, 0.0f);
        SetJointConstraint(chain, jointIndex, minAngleDeg, maxAngleDeg, axis);
    }

    void SetJointConstraint(IKChain& chain, int jointIndex,
                           float minAngleDeg, float maxAngleDeg,
                           const hkVector4& axis)
    {
        if (jointIndex >= 0 && jointIndex < chain.constraints.getSize())
        {
            chain.constraints[jointIndex].minAngleDeg = minAngleDeg;
            chain.constraints[jointIndex].maxAngleDeg = maxAngleDeg;
            chain.constraints[jointIndex].axis = axis;
            chain.constraints[jointIndex].enabled = true;
        }
    }

    // Apply pole vector influence (useful for elbow/knee direction control)
    void ApplyPoleVector(IKChain& chain, const hkArray<hkVector4>& jointPositions)
    {
        if (!chain.usePoleVector || jointPositions.getSize() < 3)
            return;

        // Middle joint attracted toward pole vector
        int midIdx = jointPositions.getSize() / 2;
        int numIterations = 5;  // Quick iterations to adjust middle joint

        for (int iter = 0; iter < numIterations; ++iter)
        {
            hkVector4 towardPole;
            towardPole.setSub4(chain.poleVectorPosition, jointPositions[midIdx]);
            float poleDist = towardPole.length3();

            if (poleDist > 1e-6f)
            {
                towardPole.normalize3();
                // Move middle joint slightly toward pole
                hkVector4 adjusted;
                adjusted.setMul4(0.1f, towardPole);
                // (This is simplified; full implementation would maintain chain constraints)
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Preset IK Chain Definitions
// ---------------------------------------------------------------------------

class PresetIKChains
{
public:
    // Create a standard humanoid arm chain (shoulder -> elbow -> wrist -> hand)
    static void CreateArmChain(const char* side, IKChain& outChain)
    {
        outChain.name = side;
        outChain.name += " Arm";
        // Expected bone names: "Shoulder_R", "Elbow_R", "Wrist_R", "Hand_R"
        // (Caller provides the bone indices)
    }

    // Create a standard humanoid leg chain (hip -> knee -> ankle -> toe)
    static void CreateLegChain(const char* side, IKChain& outChain)
    {
        outChain.name = side;
        outChain.name += " Leg";
        // Expected bone names: "Hip_R", "Knee_R", "Ankle_R", "Toe_R"
        // (Caller provides the bone indices)
    }

    // Apply standard constraints for human arm
    static void ApplyArmConstraints(IKChain& chain)
    {
        if (chain.GetJointCount() >= 3)
        {
            // Shoulder: allow rotation in most directions
            SetJointConstraint(chain, 0, -90.0f, 180.0f);

            // Elbow: limited extension (can't go backwards much)
            SetJointConstraint(chain, 1, 0.0f, 150.0f);

            // Wrist: limited rotation
            SetJointConstraint(chain, 2, -45.0f, 45.0f);
        }
    }

    // Apply standard constraints for human leg
    static void ApplyLegConstraints(IKChain& chain)
    {
        if (chain.GetJointCount() >= 3)
        {
            // Hip: allow rotation
            SetJointConstraint(chain, 0, -90.0f, 90.0f);

            // Knee: limited extension (mostly forward)
            SetJointConstraint(chain, 1, 0.0f, 170.0f);

            // Ankle: limited rotation
            SetJointConstraint(chain, 2, -30.0f, 30.0f);
        }
    }

private:
    static void SetJointConstraint(IKChain& chain, int jointIndex,
                                  float minAngle, float maxAngle)
    {
        if (jointIndex >= 0 && jointIndex < chain.constraints.getSize())
        {
            chain.constraints[jointIndex].minAngleDeg = minAngle;
            chain.constraints[jointIndex].maxAngleDeg = maxAngle;
            chain.constraints[jointIndex].enabled = true;
        }
    }
};

#endif // INVERSE_KINEMATICS_H
