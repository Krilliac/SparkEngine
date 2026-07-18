/**
 * @file InverseKinematicsFABRIK.cpp
 * @brief IK solver implementation — FABRIK
 *
 * Extracted from InverseKinematics.cpp to keep each file focused on a single responsibility.
 */
#include "../../Core/Platform.h"
#include "AnimationSystem.h"
#include "../../Utils/LogMacros.h"
#include <cmath>

using namespace DirectX;
namespace Spark::Animation
{

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
