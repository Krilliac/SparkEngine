/**
 * @file InverseKinematics.cpp
 * @brief IK solver implementations — TwoBoneIK, LookAtIK, FABRIK
 *
 * Extracted from AnimationSystem.cpp to keep each file focused on a single responsibility.
 */
#include "../../Core/Platform.h"
#include "AnimationSystem.h"
#include "../../Utils/LogMacros.h"
#include <cmath>

using namespace DirectX;
namespace Spark::Animation
{

    void AnimationEvaluator::SolveTwoBoneIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                            const IKChain& chain)
    {
        if (!chain.enabled || chain.boneIndices.size() < 3)
            return;

        const int32_t rootIdx = chain.boneIndices[0];
        const int32_t midIdx = chain.boneIndices[1];
        const int32_t endIdx = chain.boneIndices[2];
        const size_t boneCount = skeleton.bones.size();

        if (rootIdx < 0 || midIdx < 0 || endIdx < 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Animation,
                           "TwoBoneIK: negative bone index in chain (root=%d mid=%d end=%d)", rootIdx, midIdx, endIdx);
            return;
        }
        if (static_cast<size_t>(rootIdx) >= boneCount || static_cast<size_t>(midIdx) >= boneCount ||
            static_cast<size_t>(endIdx) >= boneCount)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Animation,
                           "TwoBoneIK: bone index out of range (root=%d mid=%d end=%d, count=%zu)", rootIdx, midIdx,
                           endIdx, boneCount);
            return;
        }

        // Step 1: Compute world (global) transforms for all bones so we can
        // extract the three joint world positions accurately.
        std::vector<XMMATRIX> globalTransforms(boneCount);
        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);
            int32_t parentIdx = skeleton.bones[i].parentIndex;
            if (parentIdx >= 0 && static_cast<size_t>(parentIdx) < boneCount)
            {
                globalTransforms[i] = local * globalTransforms[parentIdx];
            }
            else
            {
                globalTransforms[i] = local;
            }
        }

        // Extract world-space positions of root, mid (elbow/knee), and end (hand/foot)
        XMVECTOR rootPos = globalTransforms[rootIdx].r[3];
        XMVECTOR midPos = globalTransforms[midIdx].r[3];
        XMVECTOR endPos = globalTransforms[endIdx].r[3];
        XMVECTOR target = XMLoadFloat3(&chain.targetPosition);
        XMVECTOR poleVec = XMLoadFloat3(&chain.poleVector);

        // Compute upper and lower bone lengths from the current pose
        float upperLen = XMVectorGetX(XMVector3Length(XMVectorSubtract(midPos, rootPos)));
        float lowerLen = XMVectorGetX(XMVector3Length(XMVectorSubtract(endPos, midPos)));

        if (upperLen < 1e-6f || lowerLen < 1e-6f)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Animation, "TwoBoneIK: degenerate bone lengths (upper=%.6f lower=%.6f)",
                           upperLen, lowerLen);
            return;
        }

        XMVECTOR rootToTarget = XMVectorSubtract(target, rootPos);
        float targetDist = XMVectorGetX(XMVector3Length(rootToTarget));

        if (targetDist < 1e-6f)
            return;

        // Clamp target distance to the reachable range so the cosine law stays valid
        float maxReach = upperLen + lowerLen - 1e-4f;
        float minReach = std::fabs(upperLen - lowerLen) + 1e-4f;
        float clampedDist = (std::max)(minReach, (std::min)(maxReach, targetDist));

        // Step 2: Use the cosine law to find the angle at root and mid joints.
        // cos(angleAtRoot) = (upper^2 + dist^2 - lower^2) / (2 * upper * dist)
        float cosAngleRoot =
            (upperLen * upperLen + clampedDist * clampedDist - lowerLen * lowerLen) / (2.0f * upperLen * clampedDist);
        cosAngleRoot = (std::max)(-1.0f, (std::min)(1.0f, cosAngleRoot));
        float angleAtRoot = std::acos(cosAngleRoot);

        // Step 3: Compute the IK plane using the pole vector.
        // The chain lies in the plane defined by (rootToTarget direction, pole hint).
        XMVECTOR targetDir = XMVector3Normalize(rootToTarget);

        // Project pole vector onto the plane perpendicular to targetDir
        XMVECTOR poleDir = XMVectorSubtract(poleVec, rootPos);
        float poleDot = XMVectorGetX(XMVector3Dot(poleDir, targetDir));
        XMVECTOR poleOnPlane = XMVectorSubtract(poleDir, XMVectorScale(targetDir, poleDot));
        float poleOnPlaneLen = XMVectorGetX(XMVector3Length(poleOnPlane));

        XMVECTOR bendAxis;
        if (poleOnPlaneLen > 1e-6f)
        {
            bendAxis = XMVector3Normalize(poleOnPlane);
        }
        else
        {
            // Fallback: use an arbitrary perpendicular vector
            XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            bendAxis = XMVector3Cross(targetDir, up);
            if (XMVectorGetX(XMVector3LengthSq(bendAxis)) < 1e-6f)
            {
                up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
                bendAxis = XMVector3Cross(targetDir, up);
            }
            bendAxis = XMVector3Normalize(bendAxis);
        }

        // The normal of the IK plane
        XMVECTOR ikPlaneNormal = XMVector3Normalize(XMVector3Cross(targetDir, bendAxis));

        // Step 4: Compute the desired mid-joint (elbow/knee) world position.
        // Rotate targetDir by angleAtRoot around ikPlaneNormal to find the upper bone direction.
        XMMATRIX rootRot = XMMatrixRotationAxis(ikPlaneNormal, angleAtRoot);
        XMVECTOR upperDir = XMVector3Normalize(XMVector3TransformCoord(targetDir, rootRot));
        XMVECTOR desiredMidPos = XMVectorAdd(rootPos, XMVectorScale(upperDir, upperLen));

        // Step 5: Apply the rotation to the root bone.
        // Compute the rotation from the current root->mid direction to the desired direction.
        XMVECTOR currentRootToMid = XMVector3Normalize(XMVectorSubtract(midPos, rootPos));
        XMVECTOR desiredRootToMid = XMVector3Normalize(XMVectorSubtract(desiredMidPos, rootPos));

        // Build a delta rotation quaternion from currentRootToMid to desiredRootToMid
        XMVECTOR rootRotAxis = XMVector3Cross(currentRootToMid, desiredRootToMid);
        float rootRotDot = XMVectorGetX(XMVector3Dot(currentRootToMid, desiredRootToMid));
        rootRotDot = (std::max)(-1.0f, (std::min)(1.0f, rootRotDot));

        if (XMVectorGetX(XMVector3LengthSq(rootRotAxis)) > 1e-10f)
        {
            float rootRotAngle = std::acos(rootRotDot);
            XMMATRIX rootDeltaRot = XMMatrixRotationAxis(XMVector3Normalize(rootRotAxis), rootRotAngle);

            // Convert world-space rotation to local-space:
            // newLocal = localTransform * inverse(parentWorld) * deltaRotWorld * parentWorld
            // Simplified: apply delta rotation in world space then convert back
            XMMATRIX rootLocal = XMLoadFloat4x4(&localTransforms[rootIdx]);
            int32_t rootParent = skeleton.bones[rootIdx].parentIndex;
            if (rootParent >= 0 && static_cast<size_t>(rootParent) < boneCount)
            {
                XMMATRIX parentWorldInv = XMMatrixInverse(nullptr, globalTransforms[rootParent]);
                XMMATRIX newGlobal = rootDeltaRot * globalTransforms[rootIdx];
                XMMATRIX newLocal = newGlobal * parentWorldInv;
                // Preserve the local translation (bone position doesn't change, only rotation)
                newLocal.r[3] = rootLocal.r[3];
                XMStoreFloat4x4(&localTransforms[rootIdx], newLocal);
            }
            else
            {
                XMMATRIX newLocal = rootDeltaRot * rootLocal;
                newLocal.r[3] = rootLocal.r[3];
                XMStoreFloat4x4(&localTransforms[rootIdx], newLocal);
            }
        }

        // Step 6: Recompute global transforms after root bone modification
        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);
            int32_t parentIdx = skeleton.bones[i].parentIndex;
            if (parentIdx >= 0 && static_cast<size_t>(parentIdx) < boneCount)
            {
                globalTransforms[i] = local * globalTransforms[parentIdx];
            }
            else
            {
                globalTransforms[i] = local;
            }
        }

        // Step 7: Apply the rotation to the mid bone (elbow/knee bend).
        // Compute the desired end-effector position from the new mid position.
        XMVECTOR newMidPos = globalTransforms[midIdx].r[3];
        XMVECTOR currentMidToEnd = XMVector3Normalize(XMVectorSubtract(endPos, newMidPos));

        // The desired direction from mid to end effector: toward target from the new mid position
        XMVECTOR desiredEndPos = target;
        XMVECTOR desiredMidToEnd = XMVector3Normalize(XMVectorSubtract(desiredEndPos, newMidPos));

        XMVECTOR midRotAxis = XMVector3Cross(currentMidToEnd, desiredMidToEnd);
        float midRotDot = XMVectorGetX(XMVector3Dot(currentMidToEnd, desiredMidToEnd));
        midRotDot = (std::max)(-1.0f, (std::min)(1.0f, midRotDot));

        if (XMVectorGetX(XMVector3LengthSq(midRotAxis)) > 1e-10f)
        {
            float midRotAngle = std::acos(midRotDot);
            XMMATRIX midDeltaRot = XMMatrixRotationAxis(XMVector3Normalize(midRotAxis), midRotAngle);

            XMMATRIX midLocal = XMLoadFloat4x4(&localTransforms[midIdx]);
            int32_t midParent = skeleton.bones[midIdx].parentIndex;
            if (midParent >= 0 && static_cast<size_t>(midParent) < boneCount)
            {
                XMMATRIX parentWorldInv = XMMatrixInverse(nullptr, globalTransforms[midParent]);
                XMMATRIX newGlobal = midDeltaRot * globalTransforms[midIdx];
                XMMATRIX newLocal = newGlobal * parentWorldInv;
                newLocal.r[3] = midLocal.r[3];
                XMStoreFloat4x4(&localTransforms[midIdx], newLocal);
            }
            else
            {
                XMMATRIX newLocal = midDeltaRot * midLocal;
                newLocal.r[3] = midLocal.r[3];
                XMStoreFloat4x4(&localTransforms[midIdx], newLocal);
            }
        }

        // Note: IK weight blending is handled by the caller (AnimationInstance::Update())
        // which saves pre-IK transforms and blends per-bone after solving. This allows
        // the solver to operate at full strength and the blend to be applied cleanly.
    }

    void AnimationEvaluator::SolveLookAtIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                           const IKChain& chain)
    {
        if (!chain.enabled || chain.boneIndices.empty())
            return;

        const int32_t boneIdx = chain.boneIndices[0];
        const size_t boneCount = skeleton.bones.size();
        if (boneIdx < 0 || static_cast<size_t>(boneIdx) >= boneCount || localTransforms.size() < boneCount)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Animation, "LookAtIK: bone index %d out of range (bones=%zu, xforms=%zu)",
                           boneIdx, boneCount, localTransforms.size());
            return;
        }

        // Step 1: Build world (global) transforms so the aim is computed in world space.
        // The previous implementation treated the bone's *local* translation as a world
        // position and overwrote the bone's rotation/scale — wrong for any non-root bone.
        std::vector<XMMATRIX> globalTransforms(boneCount);
        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);
            int32_t parentIdx = skeleton.bones[i].parentIndex;
            if (parentIdx >= 0 && static_cast<size_t>(parentIdx) < boneCount)
            {
                globalTransforms[i] = local * globalTransforms[parentIdx];
            }
            else
            {
                globalTransforms[i] = local;
            }
        }

        // Step 2: Aim the bone's world forward (+Z) at the target in world space.
        XMVECTOR worldPos = globalTransforms[boneIdx].r[3];
        XMVECTOR target = XMLoadFloat3(&chain.targetPosition);
        XMVECTOR toTarget = XMVectorSubtract(target, worldPos);
        if (XMVectorGetX(XMVector3LengthSq(toTarget)) < 1e-12f)
            return; // Target coincides with the bone; nothing to aim at.
        toTarget = XMVector3Normalize(toTarget);

        // The bone's world forward is the image of its local +Z axis under the global
        // transform's rotation — i.e. the third basis row (row-vector convention).
        XMVECTOR worldForward = XMVector3Normalize(globalTransforms[boneIdx].r[2]);

        XMVECTOR rotAxis = XMVector3Cross(worldForward, toTarget);
        float dot = XMVectorGetX(XMVector3Dot(worldForward, toTarget));
        dot = (std::max)(-1.0f, (std::min)(1.0f, dot));

        if (XMVectorGetX(XMVector3LengthSq(rotAxis)) > 1e-8f)
        {
            // Step 3: Build the world-space delta rotation, blended by chain.weight, and
            // convert it back to local space while preserving the bone's original
            // rotation, scale, and translation (only the orientation is nudged).
            float angle = std::acos(dot) * chain.weight;
            XMMATRIX deltaRot = XMMatrixRotationAxis(XMVector3Normalize(rotAxis), angle);

            XMMATRIX boneLocal = XMLoadFloat4x4(&localTransforms[boneIdx]);
            int32_t parentIdx = skeleton.bones[boneIdx].parentIndex;
            if (parentIdx >= 0 && static_cast<size_t>(parentIdx) < boneCount)
            {
                XMMATRIX parentWorldInv = XMMatrixInverse(nullptr, globalTransforms[parentIdx]);
                XMMATRIX newGlobal = deltaRot * globalTransforms[boneIdx];
                XMMATRIX newLocal = newGlobal * parentWorldInv;
                newLocal.r[3] = boneLocal.r[3]; // preserve local translation
                XMStoreFloat4x4(&localTransforms[boneIdx], newLocal);
            }
            else
            {
                XMMATRIX newLocal = deltaRot * boneLocal;
                newLocal.r[3] = boneLocal.r[3];
                XMStoreFloat4x4(&localTransforms[boneIdx], newLocal);
            }
        }
    }

    void AnimationEvaluator::SolveFABRIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                         const IKChain& chain)
    {
        if (!chain.enabled || chain.boneIndices.size() < 2)
            return;

        SPARK_LOG_DEBUG(Spark::LogCategory::Animation, "FABRIK: solving chain with %zu joints, maxIter=%d, tol=%.4f",
                        chain.boneIndices.size(), chain.maxIterations, chain.tolerance);

        const size_t numJoints = chain.boneIndices.size();

        // Validate all bone indices
        for (size_t i = 0; i < numJoints; ++i)
        {
            int32_t idx = chain.boneIndices[i];
            if (idx < 0 || idx >= static_cast<int32_t>(localTransforms.size()))
                return;
        }

        // Step 1: Compute global (world) transforms for all bones in the skeleton
        // so we can extract joint world positions
        size_t boneCount = skeleton.bones.size();
        std::vector<XMMATRIX> globalTransforms(boneCount);
        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);
            int32_t parentIdx = skeleton.bones[i].parentIndex;

            if (parentIdx >= 0 && parentIdx < static_cast<int32_t>(boneCount))
            {
                globalTransforms[i] = local * globalTransforms[parentIdx];
            }
            else
            {
                globalTransforms[i] = local;
            }
        }

        // Step 2: Extract world positions of each joint in the IK chain
        std::vector<XMVECTOR> positions(numJoints);
        for (size_t i = 0; i < numJoints; ++i)
        {
            int32_t boneIdx = chain.boneIndices[i];
            positions[i] = globalTransforms[boneIdx].r[3];
        }

        // Step 3: Compute distances (bone lengths) between consecutive joints
        std::vector<float> boneLengths(numJoints - 1);
        float totalLength = 0.0f;
        for (size_t i = 0; i < numJoints - 1; ++i)
        {
            XMVECTOR diff = XMVectorSubtract(positions[i + 1], positions[i]);
            boneLengths[i] = XMVectorGetX(XMVector3Length(diff));
            totalLength += boneLengths[i];
        }

        XMVECTOR target = XMLoadFloat3(&chain.targetPosition);
        XMVECTOR rootPos = positions[0];

        // Check if the target is reachable
        XMVECTOR rootToTarget = XMVectorSubtract(target, rootPos);
        float distToTarget = XMVectorGetX(XMVector3Length(rootToTarget));

        if (distToTarget > totalLength)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Animation, "FABRIK: target unreachable (dist=%.3f > reach=%.3f)",
                            distToTarget, totalLength);
            // Target is unreachable: stretch the chain toward the target
            XMVECTOR dir = XMVector3Normalize(rootToTarget);
            for (size_t i = 0; i < numJoints - 1; ++i)
            {
                positions[i + 1] = XMVectorAdd(positions[i], XMVectorScale(dir, boneLengths[i]));
            }
        }
        else
        {
            // FABRIK iterative solver
            for (int iteration = 0; iteration < chain.maxIterations; ++iteration)
            {
                // Check convergence: end effector close enough to target
                XMVECTOR endToTarget = XMVectorSubtract(target, positions[numJoints - 1]);
                float error = XMVectorGetX(XMVector3Length(endToTarget));
                if (error < chain.tolerance)
                    break;

                // Forward pass: move end effector to target, adjust chain toward root
                positions[numJoints - 1] = target;
                for (int i = static_cast<int>(numJoints) - 2; i >= 0; --i)
                {
                    XMVECTOR dir = XMVectorSubtract(positions[i], positions[i + 1]);
                    dir = XMVector3Normalize(dir);
                    positions[i] = XMVectorAdd(positions[i + 1], XMVectorScale(dir, boneLengths[i]));
                }

                // Backward pass: pin root, adjust chain toward end effector
                positions[0] = rootPos;
                for (size_t i = 0; i < numJoints - 1; ++i)
                {
                    XMVECTOR dir = XMVectorSubtract(positions[i + 1], positions[i]);
                    dir = XMVector3Normalize(dir);
                    positions[i + 1] = XMVectorAdd(positions[i], XMVectorScale(dir, boneLengths[i]));
                }
            }
        }

        // Step 4: Apply weight blending between original and solved positions
        if (chain.weight < 1.0f)
        {
            std::vector<XMVECTOR> origPositions(numJoints);
            for (size_t i = 0; i < numJoints; ++i)
            {
                int32_t boneIdx = chain.boneIndices[i];
                origPositions[i] = globalTransforms[boneIdx].r[3];
            }
            for (size_t i = 0; i < numJoints; ++i)
            {
                positions[i] = XMVectorLerp(origPositions[i], positions[i], chain.weight);
            }
        }

        // Step 5: Update bone local transforms from the solved world positions.
        // For each joint, compute the rotation needed to point from this joint
        // to the next joint in the new configuration, and update the local transform.
        for (size_t i = 0; i < numJoints - 1; ++i)
        {
            int32_t boneIdx = chain.boneIndices[i];
            int32_t childIdx = chain.boneIndices[i + 1];

            // Compute original direction from this joint to the next in world space
            XMVECTOR origDir =
                XMVector3Normalize(XMVectorSubtract(globalTransforms[childIdx].r[3], globalTransforms[boneIdx].r[3]));

            // Compute new direction from solved positions
            XMVECTOR newDir = XMVector3Normalize(XMVectorSubtract(positions[i + 1], positions[i]));

            // Compute rotation from original direction to new direction
            XMVECTOR rotAxis = XMVector3Cross(origDir, newDir);
            float dotProduct = XMVectorGetX(XMVector3Dot(origDir, newDir));
            dotProduct = (std::max)(-1.0f, (std::min)(1.0f, dotProduct));

            if (XMVectorGetX(XMVector3LengthSq(rotAxis)) > 1e-8f)
            {
                float angle = std::acos(dotProduct);
                XMMATRIX rotation = XMMatrixRotationAxis(XMVector3Normalize(rotAxis), angle);

                // Apply the world-space rotation to the bone's local transform
                XMMATRIX local = XMLoadFloat4x4(&localTransforms[boneIdx]);

                // Decompose local to preserve translation and scale
                XMVECTOR localPos = local.r[3];
                XMMATRIX rotatedLocal = local * rotation;
                // Restore the original local translation (position stays the same)
                rotatedLocal.r[3] = localPos;

                XMStoreFloat4x4(&localTransforms[boneIdx], rotatedLocal);
            }
        }
    }

} // namespace Spark::Animation
