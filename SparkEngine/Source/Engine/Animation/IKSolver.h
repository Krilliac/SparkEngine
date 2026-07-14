/**
 * @file IKSolver.h
 * @brief Inverse Kinematics types and chain definitions
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <cstdint>


namespace Spark::Animation
{

    /**
 * @brief IK solver algorithm variants.
 */
    enum class IKType
    {
        TwoBone, ///< Analytical two-joint IK — ideal for limbs (arm, leg). Fast and exact.
        LookAt,  ///< Single-bone rotation to face a target — head tracking, turret aiming.
        FABRIK   ///< Forward and Backward Reaching IK — supports chains of arbitrary length.
    };

    /**
 * @brief Defines an IK chain applied as a post-processing pass after animation blending.
 *
 * IK chains allow individual body parts to reach specific world-space targets even if
 * the underlying animation does not place them there. Common uses:
 * - Foot placement on uneven terrain (TwoBone on each leg).
 * - Aim override (LookAt on spine/head).
 * - Procedural reach (FABRIK on arm chain).
 *
 * A `weight` value of 0–1 blends between the fully animated pose and the IK-solved pose,
 * enabling smooth enable/disable of IK effects.
 */
    struct IKChain
    {
        /** @brief Human-readable name (e.g. "RightArm", "LeftLeg", "Head"). */
        std::string name;

        /** @brief Solver algorithm to use for this chain. */
        IKType type = IKType::TwoBone;

        /**
     * @brief Ordered list of bone indices from root to end-effector.
     *
     * TwoBone: exactly 3 indices [shoulder, elbow, hand].
     * LookAt:  exactly 1 index [the bone to rotate].
     * FABRIK:  2 or more indices.
     */
        std::vector<int32_t> boneIndices;

        /**
     * @brief World-space position the end-effector should reach.
     *
     * Updated each frame by gameplay systems (ground raycast for foot, aim raycast for arm).
     */
        XMFLOAT3 targetPosition{0, 0, 0};

        /**
     * @brief Direction hint for intermediate joint bend (TwoBone only).
     *
     * Must be non-zero and not parallel to the chain axis. Determines which way
     * the elbow/knee bends. Typical: world right for right arm, world forward for left knee.
     */
        XMFLOAT3 poleVector{0, 1, 0}; ///< Hint direction for elbow/knee

        /**
     * @brief Blend weight of this chain's influence in [0, 1].
     *
     * 0 = IK has no effect; 1 = full IK. Fade in/out smoothly for natural transitions.
     */
        float weight = 1.0f;

        /** @brief When false, this chain is skipped during IK solving. */
        bool enabled = true;

        /**
     * @brief Maximum FABRIK solver iterations per frame.
     *
     * Higher values improve accuracy at the cost of CPU time. 10–20 is typical.
     */
        int maxIterations = 10; ///< For FABRIK solver

        /**
     * @brief Distance threshold (metres) at which FABRIK considers itself converged.
     *
     * Default 0.01 m (1 cm). Reduces unnecessary iterations when close to target.
     */
        float tolerance = 0.01f; ///< Distance threshold
    };

} // namespace Spark::Animation
