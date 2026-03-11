/**
 * @file AnimationClip.h
 * @brief Keyframe data structures and animation clip definitions
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <cstdint>


namespace Spark::Animation
{

    /**
 * @brief A single position or scale keyframe in an animation channel.
 *
 * Keyframes store a (time, value) pair. The animation evaluator interpolates
 * between adjacent keyframes using the `time` field to find the current value
 * at any point in the clip's timeline.
 */
    struct VectorKey
    {
        /**
     * @brief Time in seconds at which this keyframe occurs.
     *
     * Keyframes within a `BoneAnimation` must be sorted in ascending time order.
     */
        float time;
        /** @brief 3D vector value at this keyframe (position or scale). */
        XMFLOAT3 value;
    };

    /**
 * @brief A single rotation keyframe stored as a quaternion.
 *
 * Using quaternions for rotation avoids gimbal lock and enables smooth spherical
 * linear interpolation (SLERP) between keyframes.
 */
    struct QuatKey
    {
        /** @brief Time in seconds at which this keyframe occurs. Sorted ascending within a BoneAnimation. */
        float time;

        /**
     * @brief Quaternion rotation value at this keyframe stored as (X, Y, Z, W).
     *
     * Must be normalized. SLERP is used for interpolation.
     */
        XMFLOAT4 value; ///< Quaternion rotation (x, y, z, w)
    };

    /**
 * @brief Animation data for a single bone: position, rotation, and scale tracks.
 *
 * Each `BoneAnimation` corresponds to one bone/channel in an `AnimationClip`.
 * It contains three independent keyframe tracks that are independently interpolated
 * and then combined into a local bone transform matrix.
 *
 * ### Interpolation
 * - Positions and scales use **linear interpolation** (LERP).
 * - Rotations use **spherical linear interpolation** (SLERP) via quaternions.
 *
 * If a track is empty (no keyframes), the bone's bind pose value for that channel
 * is used, allowing partial animation clips that only animate a subset of channels.
 */
    struct BoneAnimation
    {
        /** @brief Name of the bone this channel animates. Must match a bone in the target Skeleton. */
        std::string boneName;

        /**
     * @brief Pre-resolved index of the bone in the skeleton, cached for performance.
     *
     * Set to -1 until the clip is bound to a Skeleton. The AnimationEvaluator resolves
     * bone names to indices once during initialization to avoid string lookups per frame.
     */
        int32_t boneIndex = -1;

        /**
     * @brief Sorted list of position keyframes for this bone.
     *
     * May be empty if the bone has no position animation (stays at bind pose position).
     */
        std::vector<VectorKey> positionKeys;

        /**
     * @brief Sorted list of rotation keyframes (as quaternions) for this bone.
     *
     * May be empty if the bone has no rotation animation.
     */
        std::vector<QuatKey> rotationKeys;

        /**
     * @brief Sorted list of scale keyframes for this bone.
     *
     * May be empty if the bone has no scale animation.
     */
        std::vector<VectorKey> scaleKeys;

        /**
     * @brief Sample the interpolated position at the given time.
     *
     * Uses binary search to find the surrounding keyframes and linearly interpolates.
     * Returns the first/last keyframe if `time` is out of range.
     *
     * @param time  Time in seconds within the clip's timeline.
     * @return      Interpolated position value.
     */
        XMFLOAT3 InterpolatePosition(float time) const;

        /**
     * @brief Sample the interpolated rotation at the given time using SLERP.
     *
     * @param time  Time in seconds within the clip's timeline.
     * @return      Normalized interpolated quaternion (X, Y, Z, W).
     */
        XMFLOAT4 InterpolateRotation(float time) const;

        /**
     * @brief Sample the interpolated scale at the given time.
     *
     * @param time  Time in seconds within the clip's timeline.
     * @return      Interpolated scale value.
     */
        XMFLOAT3 InterpolateScale(float time) const;
    };

    /**
 * @brief A complete animation clip containing keyframe data for all animated bones.
 *
 * An AnimationClip is the shareable animation asset — one clip can drive many
 * character instances simultaneously. Clips are loaded by
 * `AnimationManager::LoadAnimations()` and cached by name.
 *
 * ### Timeline
 * The clip's timeline spans [0, `duration`] seconds. The `ticksPerSecond` value
 * is used to convert the source file's native tick units to seconds.
 */
    struct AnimationClip
    {
        /** @brief Human-readable name (e.g. "Run", "Idle"). Used as the key in AnimationManager. */
        std::string name;

        /**
     * @brief Total duration of the clip in seconds.
     *
     * Set to `totalTicks / ticksPerSecond`.
     */
        float duration = 0.0f;

        /**
     * @brief Playback rate in ticks per second for the source asset.
     *
     * Typically 24 (film), 30 (NTSC), or 60 (high-fidelity).
     */
        float ticksPerSecond = 24.0f;

        /**
     * @brief Per-bone animation channels.
     *
     * Each element contains the keyframe tracks for one bone. Bones not present
     * in this vector use the skeleton's bind pose.
     */
        std::vector<BoneAnimation> channels;

        /**
     * @brief Whether the clip automatically loops when it reaches the end.
     *
     * Default: true. Set false for one-shot animations (death, land, shoot once).
     */
        bool loop = true;

        /**
     * @brief Find the animation channel for the named bone.
     *
     * @param boneName  Name of the bone to look up.
     * @return          Const pointer to the BoneAnimation channel, or `nullptr` if not found.
     */
        const BoneAnimation* FindChannel(const std::string& boneName) const
        {
            for (const auto& ch : channels)
                if (ch.boneName == boneName)
                    return &ch;
            return nullptr;
        }
    };

} // namespace Spark::Animation
