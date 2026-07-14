/**
 * @file AnimationEvaluator.h
 * @brief Core animation processing: sampling, blending, skinning, and IK solving
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>

#include "Skeleton.h"
#include "AnimationClip.h"
#include "IKSolver.h"


namespace Spark::Animation
{

    /**
 * @class AnimationEvaluator
 * @brief Static utility class for core per-frame animation computations.
 *
 * All methods are pure functions (stateless). The AnimationUpdateSystem calls
 * them in this order each frame:
 *
 * ```
 * SampleClip()              → localTransforms[]
 * BlendTransforms()         → blended localTransforms[]
 * ComputeSkinningMatrices() → finalTransforms[] (upload to GPU)
 * Solve*IK()                → IK corrections applied to finalTransforms[]
 * ```
 */
    class AnimationEvaluator
    {
      public:
        /**
     * @brief Sample all bone transforms from a clip at a given playback time.
     *
     * For each bone in the skeleton, finds the surrounding keyframes via binary
     * search and interpolates. Bones with no channel use their bind pose.
     *
     * @param clip               Source animation clip to sample.
     * @param skeleton           Target skeleton providing bone hierarchy.
     * @param time               Evaluation time in seconds (wrapped to clip duration if looping).
     * @param outLocalTransforms Output array (sized to skeleton.GetBoneCount()). Each element
     *                           is a 4x4 local transform in parent-bone space.
     */
        static void SampleClip(const AnimationClip& clip, const Skeleton& skeleton, float time,
                               std::vector<XMFLOAT4X4>& outLocalTransforms);

        /**
     * @brief Linearly blend two sets of local bone transforms.
     *
     * Component-wise LERP on matrices. Used for cross-fading between two states
     * or blending two animation layers.
     *
     * @param a            Source pose (blend factor 0).
     * @param b            Target pose (blend factor 1).
     * @param blendFactor  Interpolation factor in [0, 1].
     * @param outResult    Output blended transforms (must be pre-sized to bone count).
     */
        static void BlendTransforms(const std::vector<XMFLOAT4X4>& a, const std::vector<XMFLOAT4X4>& b,
                                    float blendFactor, std::vector<XMFLOAT4X4>& outResult);

        /**
     * @brief Compute final GPU-ready skinning matrices from local bone transforms.
     *
     * Walks the hierarchy parent-before-child, multiplies up the chain, then
     * multiplies by each bone's offset matrix: `final[i] = offset[i] * worldBone[i]`.
     *
     * @param skeleton           Skeleton defining hierarchy and offset matrices.
     * @param localTransforms    Per-bone local transforms (from SampleClip or Blend).
     * @param outFinalTransforms Output GPU-ready skinning matrices (one per bone).
     */
        static void ComputeSkinningMatrices(const Skeleton& skeleton, const std::vector<XMFLOAT4X4>& localTransforms,
                                            std::vector<XMFLOAT4X4>& outFinalTransforms);

        /**
     * @brief Apply an analytical two-bone IK solution.
     *
     * `chain.boneIndices` must contain exactly 3 valid bone indices
     * [root, middle, end-effector].
     *
     * @param localTransforms  Per-bone local transforms to modify in place.
     * @param skeleton         The skeleton for bone length computation.
     * @param chain            IK chain with target position and pole vector hint.
     */
        static void SolveTwoBoneIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                   const IKChain& chain);

        /**
     * @brief Apply look-at IK to rotate a single bone towards a target.
     *
     * `chain.boneIndices` must contain exactly 1 bone index.
     *
     * @param localTransforms  Per-bone local transforms to modify in place.
     * @param skeleton         The skeleton for hierarchy information.
     * @param chain            IK chain with target position.
     */
        static void SolveLookAtIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                  const IKChain& chain);

        /**
     * @brief Apply FABRIK IK to an arbitrary-length bone chain.
     *
     * Iterates up to `chain.maxIterations` times or until within `chain.tolerance`
     * of the target. `chain.boneIndices` must have >= 2 entries.
     *
     * @param localTransforms  Per-bone local transforms to modify in place.
     * @param skeleton         The skeleton for chain length computation.
     * @param chain            IK chain with target position, iterations, and tolerance.
     */
        static void SolveFABRIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                const IKChain& chain);
    };

} // namespace Spark::Animation
