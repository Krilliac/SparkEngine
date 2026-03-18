/**
 * @file AnimationTypes.h
 * @brief Core animation data types: bones, skeletons, keyframes, clips, blending, IK, and instances
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Extracted from AnimationSystem.h to separate type definitions from system classes.
 * Contains all enums, structs, and lightweight data types used by the animation pipeline.
 *
 * @see AnimationSystem.h for AnimationStateMachine, AnimationManager, and AnimationEvaluator.
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>


namespace Spark::Animation
{

    // =============================================================================
    // Bone & Skeleton
    // =============================================================================

    /**
 * @brief Represents a single bone in a skeletal hierarchy.
 *
 * Bones are the fundamental building blocks of a skeleton. Each bone has:
 * - A **parent index** that defines the hierarchy (the root bone has `parentIndex == -1`).
 * - An **offset matrix** (inverse bind pose): transforms vertices from model space to
 *   bone space, allowing the bone to deform the mesh relative to its rest position.
 * - A **local bind pose**: the bone's transform relative to its parent in the rest pose.
 *
 * During animation evaluation, each bone's local transform is overridden by the
 * interpolated keyframe data and the resulting chain is multiplied with parent transforms
 * to produce the final world-space skinning matrices.
 *
 * @note Bone names must be unique within a Skeleton. The `boneNameToIndex` map in
 *       `Skeleton` provides O(1) lookup by name.
 */
    struct Bone
    {
        /** @brief Human-readable name matching the name exported from the 3D authoring tool (e.g. "Bip01_R_Hand"). */
        std::string name;

        /**
     * @brief Index of this bone's parent within the `Skeleton::bones` array.
     *
     * The root bone has `parentIndex == -1`. All other bones have a valid parent index.
     * The hierarchy forms a tree rooted at the single root bone.
     */
        int32_t parentIndex = -1;

        /**
     * @brief Inverse bind pose matrix (mesh-to-bone space transform).
     *
     * Stored as a 4x4 row-major matrix. This matrix transforms a vertex from its
     * original model-space position into the local space of this bone in the rest pose.
     * During skinning: `finalMatrix = offsetMatrix * localAnimatedTransform * parentChain`.
     */
        XMFLOAT4X4 offsetMatrix; ///< Inverse bind pose matrix

        /**
     * @brief Bone's transform relative to its parent in the bind/rest pose.
     *
     * Stored as a 4x4 row-major matrix. Used as the fallback when no animation clip
     * provides a keyframe for this bone, ensuring the mesh is displayed in its correct
     * rest shape.
     */
        XMFLOAT4X4 localBindPose; ///< Local bind pose transform
    };

    /**
 * @brief Complete bone hierarchy for a skinned character or object.
 *
 * A Skeleton is a shared asset loaded once and referenced by multiple
 * `AnimationInstance` objects. It defines the fixed hierarchy of bones but does
 * NOT contain any animation state — that lives in `AnimationInstance`.
 *
 * ### Loading
 * Use `AnimationManager::LoadSkeleton()` to load a Skeleton from an FBX or GLTF file.
 * The manager caches skeletons by file path so that multiple instances of the same
 * character share a single Skeleton in memory.
 *
 * @code
 *   auto skeleton = AnimationManager::GetInstance().LoadSkeleton("Assets/Soldier.fbx");
 *   int32_t spineIdx = skeleton->FindBone("Bip01_Spine");
 * @endcode
 */
    struct Skeleton
    {
        /** @brief Human-readable name, typically derived from the source file. */
        std::string name;

        /**
     * @brief Flat array of all bones, ordered such that every bone appears AFTER its parent.
     *
     * This ordering ensures that when bone transforms are computed in index order,
     * a bone's parent world transform is always already computed before the bone itself.
     */
        std::vector<Bone> bones;

        /**
     * @brief Map from bone name to its index in the `bones` array.
     *
     * Provides O(1) lookup by name, which is needed when matching animation channels
     * to skeleton bones. Built automatically when the skeleton is loaded.
     */
        std::unordered_map<std::string, int32_t> boneNameToIndex;

        /**
     * @brief Look up a bone index by name.
     * @param boneName  Name of the bone to find.
     * @return          Index in `bones`, or -1 if the bone was not found.
     */
        int32_t FindBone(const std::string& boneName) const
        {
            auto it = boneNameToIndex.find(boneName);
            return (it != boneNameToIndex.end()) ? it->second : -1;
        }

        /**
     * @brief Return the total number of bones in the skeleton.
     * @return  Size of the `bones` array.
     */
        size_t GetBoneCount() const { return bones.size(); }
    };

    // =============================================================================
    // Keyframes & Animation Clips
    // =============================================================================

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

    // =============================================================================
    // Animation Blending
    // =============================================================================

    /**
 * @brief Specifies how an animation layer's result is combined with lower layers.
 */
    enum class BlendMode
    {
        Override, ///< Fully replace lower layers — no blending, just replacement.
        Additive, ///< Add the delta from bind pose on top of lower layers.
        Layered   ///< Linearly blend with lower layers using the layer's `weight`.
    };

    /**
 * @brief A single animation layer in a multi-layer blending stack.
 *
 * Layers are processed bottom-to-top (index 0 = base). Each layer can affect
 * all bones or only a masked subset, enabling "upper/lower body split" blending.
 *
 * @code
 *   AnimationLayer shootLayer;
 *   shootLayer.clipName  = "Shoot";
 *   shootLayer.weight    = 1.0f;
 *   shootLayer.blendMode = BlendMode::Override;
 *   shootLayer.boneMask  = {spineIdx, armIdx, handIdx};  // upper body only
 * @endcode
 */
    struct AnimationLayer
    {
        /** @brief Name of the AnimationClip this layer plays. Must be registered in AnimationManager. */
        std::string clipName;

        /**
     * @brief Blend weight in [0, 1]. Only used when `blendMode == BlendMode::Layered`.
     *
     * Animate this over time to cross-fade between animations smoothly.
     */
        float weight = 1.0f;

        /** @brief Current playback position within the clip (seconds). Written by the AnimationUpdateSystem. */
        float currentTime = 0.0f;

        /**
     * @brief Playback speed multiplier.
     *
     * 1.0 = normal; >1 = faster; <0 = reverse.
     */
        float speed = 1.0f;

        /** @brief How this layer's output is combined with lower layers. Default: Override. */
        BlendMode blendMode = BlendMode::Override;

        /** @brief Whether this layer is currently advancing its `currentTime`. Default: true. */
        bool playing = true;

        /** @brief Whether the clip loops when it reaches the end. Default: true. */
        bool loop = true;

        /**
     * @brief Optional set of bone indices this layer affects.
     *
     * If empty, ALL bones are affected. If non-empty, only the listed bone indices
     * receive this layer's contribution. Use `Skeleton::FindBone()` to obtain indices.
     */
        std::vector<int32_t> boneMask;
    };

    /**
 * @brief Output of the animation evaluation pass: per-bone transformation matrices.
 *
 * `finalTransforms` is uploaded to the GPU as a constant buffer for skinning in the
 * vertex shader each frame.
 */
    struct BlendResult
    {
        /**
     * @brief Per-bone local transforms in bone-parent space (one matrix per bone).
     *
     * Intermediate result before parent-chain multiplication.
     */
        std::vector<XMFLOAT4X4> localTransforms; ///< Per-bone local transforms

        /**
     * @brief Final per-bone skinning matrices ready for GPU upload (one matrix per bone).
     *
     * Product of parent-chain multiplication and the bone's offset matrix.
     * Upload this to the vertex shader's bone constant buffer.
     */
        std::vector<XMFLOAT4X4> finalTransforms; ///< Per-bone final (skinning) matrices
    };

    // =============================================================================
    // Animation State Machine Types
    // =============================================================================

    /**
 * @brief Defines a transition from one animation state to another.
 *
 * Transitions can be conditional (triggered by a lambda), time-based, or both.
 * Cross-fading blends smoothly between source and destination over `duration` seconds.
 */
    struct AnimationTransition
    {
        /** @brief Source state name. Use "*" to match any currently active state. */
        std::string fromState;

        /** @brief Destination state name. */
        std::string toState;

        /**
     * @brief Duration of the crossfade blend in seconds.
     *
     * 0.1-0.2 s for action animations; 0.3-0.5 s for smooth locomotion transitions.
     */
        float duration = 0.2f; ///< Crossfade duration in seconds

        /**
     * @brief Predicate function that returns true when this transition should fire.
     *
     * Tested each frame while in `fromState`. First satisfied transition wins.
     *
     * @code
     *   transition.condition = [&]() { return isMoving; };
     * @endcode
     */
        std::function<bool()> condition; ///< Condition function (returns true to trigger)

        /**
     * @brief When true, waits until the clip reaches `exitTime` before triggering.
     *
     * Useful for one-shot animations that must finish before the machine transitions.
     */
        bool hasExitTime = false; ///< Wait for animation to finish before transitioning

        /**
     * @brief Normalized exit time threshold in [0, 1]. Only used when `hasExitTime == true`.
     *
     * 0.0 = any time; 1.0 = must reach the very end of the clip.
     */
        float exitTime = 1.0f; ///< Normalized exit time (0-1)
    };

    /**
 * @brief Defines an animation state in the state machine.
 *
 * Each state represents a single clip playing at a given speed. States are
 * identified by name; transitions reference states by name.
 */
    struct AnimationState
    {
        /** @brief Unique name within the state machine (e.g. "Idle", "Run", "Shoot"). */
        std::string name;

        /** @brief Name of the AnimationClip to play in this state. Must exist in AnimationManager. */
        std::string clipName;

        /** @brief Playback speed multiplier. 1.5 for sprint, 0.75 for slow walk, etc. */
        float speed = 1.0f;

        /** @brief Whether the clip loops while in this state. Default: true. */
        bool loop = true;
    };

    // =============================================================================
    // IK (Inverse Kinematics)
    // =============================================================================

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
