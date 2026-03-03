/**
 * @file AnimationSystem.h
 * @brief Skeletal animation system with bone hierarchies, skinning, blending, and state machines
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This file defines the complete skeletal animation pipeline for Spark Engine. It supports
 * everything from simple single-clip playback to layered blending, additive animation, and
 * full Inverse Kinematics (IK) solving.
 *
 * ## Architecture overview
 *
 * ```
 * AnimationManager (asset cache)
 *     └── AnimationClip  (keyframe data, loaded from FBX/GLTF)
 *     └── Skeleton       (bone hierarchy)
 *
 * Per-entity runtime data:
 *     AnimationInstance
 *         ├── AnimationStateMachine  (state transitions)
 *         ├── AnimationLayer[]       (blend layers)
 *         ├── IKChain[]             (IK overrides)
 *         └── BlendResult           (final bone matrices)
 *
 * Processing:
 *     AnimationEvaluator::SampleClip()             → local bone transforms
 *     AnimationEvaluator::BlendTransforms()        → blended local transforms
 *     AnimationEvaluator::ComputeSkinningMatrices()→ GPU skinning matrices
 *     AnimationEvaluator::SolveTwoBoneIK()         → IK correction
 * ```
 *
 * ## Integration with ECS
 * Entities with an `AnimationController` component (see Components.h) have an
 * `AnimationInstance` created by the `AnimationUpdateSystem` on first encounter.
 * The system ticks the state machine, evaluates layers, and uploads the resulting
 * bone matrices to the GPU for hardware skinning.
 *
 * ## Usage example (simple single-clip)
 * @code
 *   // Load skeleton and clips at level load
 *   auto& mgr = Spark::Animation::AnimationManager::GetInstance();
 *   auto skeleton = mgr.LoadSkeleton("Assets/Characters/Soldier.fbx");
 *   auto clips    = mgr.LoadAnimations("Assets/Characters/Soldier_Anims.fbx");
 *   for (auto& c : clips) mgr.RegisterClip(c->name, c);
 *
 *   // Attach component to entity
 *   auto& ctrl = world.AddComponent<AnimationController>(entity);
 *   ctrl.currentAnimation = "Run";
 *   ctrl.playbackSpeed    = 1.0f;
 *   ctrl.loop             = true;
 * @endcode
 *
 * ## Usage example (state machine)
 * @code
 *   AnimationStateMachine& sm = instance.stateMachine;
 *   sm.AddState({"Idle",  "idle_clip",  1.0f, true});
 *   sm.AddState({"Run",   "run_clip",   1.0f, true});
 *   sm.AddState({"Shoot", "shoot_clip", 1.5f, false});
 *
 *   sm.AddTransition({"Idle", "Run",   0.15f, [&]{ return isMoving; }});
 *   sm.AddTransition({"Run",  "Idle",  0.15f, [&]{ return !isMoving; }});
 *
 *   sm.SetDefaultState("Idle");
 * @endcode
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cstdint>
#include <algorithm>

using namespace DirectX;

namespace Spark::Animation {

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
struct Bone {
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
    XMFLOAT4X4 offsetMatrix;       ///< Inverse bind pose matrix

    /**
     * @brief Bone's transform relative to its parent in the bind/rest pose.
     *
     * Stored as a 4x4 row-major matrix. Used as the fallback when no animation clip
     * provides a keyframe for this bone, ensuring the mesh is displayed in its correct
     * rest shape.
     */
    XMFLOAT4X4 localBindPose;      ///< Local bind pose transform
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
struct Skeleton {
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
    int32_t FindBone(const std::string& boneName) const {
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
struct VectorKey {
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
struct QuatKey {
    /** @brief Time in seconds at which this keyframe occurs. Sorted ascending within a BoneAnimation. */
    float time;

    /**
     * @brief Quaternion rotation value at this keyframe stored as (X, Y, Z, W).
     *
     * Must be normalized. SLERP is used for interpolation.
     */
    XMFLOAT4 value;  ///< Quaternion rotation (x, y, z, w)
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
struct BoneAnimation {
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
struct AnimationClip {
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
    const BoneAnimation* FindChannel(const std::string& boneName) const {
        for (const auto& ch : channels)
            if (ch.boneName == boneName) return &ch;
        return nullptr;
    }
};

// =============================================================================
// Animation Blending
// =============================================================================

/**
 * @brief Specifies how an animation layer's result is combined with lower layers.
 */
enum class BlendMode {
    Override,  ///< Fully replace lower layers — no blending, just replacement.
    Additive,  ///< Add the delta from bind pose on top of lower layers.
    Layered    ///< Linearly blend with lower layers using the layer's `weight`.
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
struct AnimationLayer {
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
struct BlendResult {
    /**
     * @brief Per-bone local transforms in bone-parent space (one matrix per bone).
     *
     * Intermediate result before parent-chain multiplication.
     */
    std::vector<XMFLOAT4X4> localTransforms;   ///< Per-bone local transforms

    /**
     * @brief Final per-bone skinning matrices ready for GPU upload (one matrix per bone).
     *
     * Product of parent-chain multiplication and the bone's offset matrix.
     * Upload this to the vertex shader's bone constant buffer.
     */
    std::vector<XMFLOAT4X4> finalTransforms;    ///< Per-bone final (skinning) matrices
};

// =============================================================================
// Animation State Machine
// =============================================================================

/**
 * @brief Defines a transition from one animation state to another.
 *
 * Transitions can be conditional (triggered by a lambda), time-based, or both.
 * Cross-fading blends smoothly between source and destination over `duration` seconds.
 */
struct AnimationTransition {
    /** @brief Source state name. Use "*" to match any currently active state. */
    std::string fromState;

    /** @brief Destination state name. */
    std::string toState;

    /**
     * @brief Duration of the crossfade blend in seconds.
     *
     * 0.1-0.2 s for action animations; 0.3-0.5 s for smooth locomotion transitions.
     */
    float duration = 0.2f;              ///< Crossfade duration in seconds

    /**
     * @brief Predicate function that returns true when this transition should fire.
     *
     * Tested each frame while in `fromState`. First satisfied transition wins.
     *
     * @code
     *   transition.condition = [&]() { return isMoving; };
     * @endcode
     */
    std::function<bool()> condition;    ///< Condition function (returns true to trigger)

    /**
     * @brief When true, waits until the clip reaches `exitTime` before triggering.
     *
     * Useful for one-shot animations that must finish before the machine transitions.
     */
    bool hasExitTime = false;           ///< Wait for animation to finish before transitioning

    /**
     * @brief Normalized exit time threshold in [0, 1]. Only used when `hasExitTime == true`.
     *
     * 0.0 = any time; 1.0 = must reach the very end of the clip.
     */
    float exitTime = 1.0f;             ///< Normalized exit time (0-1)
};

/**
 * @brief Defines an animation state in the state machine.
 *
 * Each state represents a single clip playing at a given speed. States are
 * identified by name; transitions reference states by name.
 */
struct AnimationState {
    /** @brief Unique name within the state machine (e.g. "Idle", "Run", "Shoot"). */
    std::string name;

    /** @brief Name of the AnimationClip to play in this state. Must exist in AnimationManager. */
    std::string clipName;

    /** @brief Playback speed multiplier. 1.5 for sprint, 0.75 for slow walk, etc. */
    float speed = 1.0f;

    /** @brief Whether the clip loops while in this state. Default: true. */
    bool loop = true;
};

/**
 * @class AnimationStateMachine
 * @brief Controls animation playback via a graph of named states and conditional transitions.
 *
 * The state machine is the high-level controller that selects which clip(s) to play
 * based on gameplay conditions. It manages cross-fade blending between states and
 * supports both condition-triggered and exit-time-based transitions.
 *
 * ### Per-frame operation
 * 1. All transitions from the current state are tested; first satisfied fires.
 * 2. If a transition fires, a crossfade begins (`blendFactor` advances 0→1).
 * 3. When the crossfade completes, the target state becomes current.
 *
 * @code
 *   sm.AddState({"Idle", "idle_anim", 1.0f, true});
 *   sm.AddState({"Run",  "run_anim",  1.0f, true});
 *   sm.AddTransition({"Idle", "Run", 0.2f, [&]{ return speed > 0.1f; }});
 *   sm.AddTransition({"Run", "Idle", 0.3f, [&]{ return speed < 0.05f; }});
 *   sm.SetDefaultState("Idle");
 * @endcode
 */
class AnimationStateMachine {
public:
    /**
     * @brief Register an animation state.
     * @param state  State definition to add.
     */
    void AddState(const AnimationState& state);

    /**
     * @brief Register a transition between two states.
     * @param transition  Transition definition including source, destination, and condition.
     */
    void AddTransition(const AnimationTransition& transition);

    /**
     * @brief Set the initial state entered when the machine first runs.
     * @param stateName  Name of the default/entry state.
     */
    void SetDefaultState(const std::string& stateName);

    /**
     * @brief Advance the state machine by one frame.
     *
     * Evaluates conditions, advances clip playback time, and progresses crossfades.
     *
     * @param deltaTime  Time elapsed since the last update (seconds).
     */
    void Update(float deltaTime);

    /**
     * @brief Get the name of the currently active state.
     * @return  Current state name.
     */
    const std::string& GetCurrentStateName() const { return m_currentState; }

    /**
     * @brief Get the current playback time within the active clip (seconds).
     * @return  Elapsed time within the current clip.
     */
    float GetCurrentTime() const { return m_currentTime; }

    /**
     * @brief Get the crossfade blend factor in [0, 1] during a transition.
     *
     * 0 = fully in source state; 1 = fully in target state.
     *
     * @return  Current blend factor.
     */
    float GetBlendFactor() const { return m_blendFactor; }

    /**
     * @brief Check whether a crossfade transition is currently in progress.
     * @return  true if transitioning.
     */
    bool IsTransitioning() const { return m_isTransitioning; }

    /**
     * @brief Get the name of the state being transitioned into.
     * @return  Target state name, or empty string if no transition is active.
     */
    const std::string& GetTargetStateName() const { return m_targetState; }

    /**
     * @brief Immediately switch to a state without crossfade blending.
     *
     * Use for abrupt changes: respawn, teleport, or initial state setup.
     *
     * @param stateName  Name of the state to enter immediately.
     */
    void ForceState(const std::string& stateName);

    /**
     * @brief Return debug information about the current state machine status.
     * @return  Multi-line string with state names, blend factor, and registered states.
     */
    std::string Console_GetStateInfo() const;

private:
    /** @brief All registered states, keyed by name. */
    std::unordered_map<std::string, AnimationState> m_states;

    /** @brief All registered transitions, evaluated in declaration order. */
    std::vector<AnimationTransition> m_transitions;

    std::string m_currentState;   ///< Name of the currently active state.
    std::string m_targetState;    ///< Target state during a crossfade (empty otherwise).
    std::string m_defaultState;   ///< The entry state on first run.

    float m_currentTime = 0.0f;           ///< Playback time within the current state's clip.
    float m_blendFactor = 0.0f;           ///< Crossfade progress (0 = source, 1 = target).
    float m_transitionDuration = 0.0f;    ///< Total duration of the current crossfade.
    float m_transitionElapsed = 0.0f;     ///< Elapsed time within the current crossfade.
    bool m_isTransitioning = false;       ///< Whether a crossfade is currently active.
};

// =============================================================================
// IK (Inverse Kinematics)
// =============================================================================

/**
 * @brief IK solver algorithm variants.
 */
enum class IKType {
    TwoBone,  ///< Analytical two-joint IK — ideal for limbs (arm, leg). Fast and exact.
    LookAt,   ///< Single-bone rotation to face a target — head tracking, turret aiming.
    FABRIK    ///< Forward and Backward Reaching IK — supports chains of arbitrary length.
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
struct IKChain {
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
    XMFLOAT3 poleVector{0, 1, 0};     ///< Hint direction for elbow/knee

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
    int maxIterations = 10;            ///< For FABRIK solver

    /**
     * @brief Distance threshold (metres) at which FABRIK considers itself converged.
     *
     * Default 0.01 m (1 cm). Reduces unnecessary iterations when close to target.
     */
    float tolerance = 0.01f;           ///< Distance threshold
};

// =============================================================================
// Animation Instance (per-entity runtime data)
// =============================================================================

/**
 * @brief All per-entity runtime animation state.
 *
 * Each animated entity has exactly one AnimationInstance, stored via the opaque
 * `animInstanceHandle` in `AnimationController`. It aggregates the state machine,
 * blend layers, IK chains, and the final evaluated bone matrices for this frame.
 *
 * ### Root motion
 * When `enableRootMotion = true`, the root bone's motion is extracted into
 * `rootMotionDelta` rather than applied to the hierarchy. The character controller
 * reads this to drive movement, preventing foot sliding.
 */
struct AnimationInstance {
    /**
     * @brief Non-owning pointer to the shared skeleton.
     *
     * Must remain valid for the lifetime of this instance.
     */
    const Skeleton* skeleton = nullptr;

    /** @brief State machine controlling clip selection and transitions. */
    AnimationStateMachine stateMachine;

    /**
     * @brief Ordered blend layer stack (0 = base, higher = overrides).
     *
     * Add upper-body layers for shooting, breathing cycles, etc.
     */
    std::vector<AnimationLayer> layers;

    /**
     * @brief IK chains applied after layer blending as a post-process.
     *
     * Evaluated in order; later chains overwrite earlier ones on shared bones.
     */
    std::vector<IKChain> ikChains;

    /**
     * @brief Output bone matrices from the most recent evaluation pass.
     *
     * `blendResult.finalTransforms` is uploaded to the GPU each frame.
     */
    BlendResult blendResult;

    /**
     * @brief Translation delta from the root bone this frame (root motion extraction).
     *
     * Only populated when `enableRootMotion = true`. Apply to character position.
     */
    XMFLOAT3 rootMotionDelta{0, 0, 0};

    /**
     * @brief Rotation delta from the root bone this frame as a quaternion (X,Y,Z,W).
     *
     * Combined with `rootMotionDelta` for turn-in-place animations.
     */
    XMFLOAT4 rootMotionRotationDelta{0, 0, 0, 1};

    /**
     * @brief Enable root motion extraction.
     *
     * When true, root bone changes are output in `rootMotionDelta` rather than
     * applied to the skeleton, preventing locomotion mismatch.
     */
    bool enableRootMotion = false;
};

// =============================================================================
// AnimationManager — asset cache
// =============================================================================

/**
 * @class AnimationManager
 * @brief Singleton cache for animation clips and skeletons.
 *
 * Ensures each unique asset (clip or skeleton) is loaded only once. All runtime
 * AnimationInstance objects share clips and skeletons via shared_ptr.
 *
 * @code
 *   auto& mgr = AnimationManager::GetInstance();
 *   auto skeleton = mgr.LoadSkeleton("Assets/Characters/Soldier.fbx");
 *   auto clips    = mgr.LoadAnimations("Assets/Characters/Soldier_Anims.fbx");
 *   for (auto& c : clips) mgr.RegisterClip(c->name, c);
 *   auto runClip = mgr.GetClip("Run");
 * @endcode
 */
class AnimationManager {
public:
    /**
     * @brief Access the global singleton instance.
     * @return  Reference to the AnimationManager.
     */
    static AnimationManager& GetInstance();

    /**
     * @brief Load a skeleton from a 3D asset file (FBX, GLTF, etc.) using Assimp.
     *
     * Cached by filepath; calling twice with the same path returns the cached result.
     *
     * @param filepath  Path to the model file containing skeleton data.
     * @return          Shared pointer to the loaded Skeleton.
     */
    std::shared_ptr<Skeleton> LoadSkeleton(const std::string& filepath);

    /**
     * @brief Load all animation clips from a 3D asset file using Assimp.
     *
     * Does NOT register clips automatically; call `RegisterClip()` for each.
     *
     * @param filepath  Path to the model/animation file.
     * @return          Vector of shared pointers to loaded AnimationClips.
     */
    std::vector<std::shared_ptr<AnimationClip>> LoadAnimations(const std::string& filepath);

    /**
     * @brief Register an animation clip by name for later lookup.
     *
     * Overwrites any existing clip with the same name.
     *
     * @param name  Registration key (e.g. "Run"). Used by `GetClip()`.
     * @param clip  Clip to register.
     */
    void RegisterClip(const std::string& name, std::shared_ptr<AnimationClip> clip);

    /**
     * @brief Retrieve a cached clip by name.
     *
     * @param name  Name the clip was registered under.
     * @return      Shared pointer to the clip, or `nullptr` if not found.
     */
    std::shared_ptr<AnimationClip> GetClip(const std::string& name) const;

    /**
     * @brief Retrieve a cached skeleton by name.
     *
     * @param name  Name or filepath the skeleton was loaded with.
     * @return      Shared pointer to the skeleton, or `nullptr` if not found.
     */
    std::shared_ptr<Skeleton> GetSkeleton(const std::string& name) const;

    /**
     * @brief Release all cached clips and skeletons.
     *
     * Called at level unload. Existing shared_ptr holders retain data until their
     * reference count reaches zero.
     */
    void Clear();

    /** @brief List all registered clip names (console integration). */
    std::string Console_ListAnimations() const;

    /** @brief List all loaded skeleton names (console integration). */
    std::string Console_ListSkeletons() const;

private:
    AnimationManager() = default;
    /** @brief Clips keyed by registration name. */
    std::unordered_map<std::string, std::shared_ptr<AnimationClip>> m_clips;
    /** @brief Skeletons keyed by file path or name. */
    std::unordered_map<std::string, std::shared_ptr<Skeleton>> m_skeletons;
};

// =============================================================================
// AnimationEvaluator — core animation processing
// =============================================================================

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
class AnimationEvaluator {
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
    static void SampleClip(const AnimationClip& clip, const Skeleton& skeleton,
                           float time, std::vector<XMFLOAT4X4>& outLocalTransforms);

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
    static void BlendTransforms(const std::vector<XMFLOAT4X4>& a,
                                const std::vector<XMFLOAT4X4>& b,
                                float blendFactor,
                                std::vector<XMFLOAT4X4>& outResult);

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
    static void ComputeSkinningMatrices(const Skeleton& skeleton,
                                        const std::vector<XMFLOAT4X4>& localTransforms,
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
    static void SolveTwoBoneIK(std::vector<XMFLOAT4X4>& localTransforms,
                                const Skeleton& skeleton,
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
    static void SolveLookAtIK(std::vector<XMFLOAT4X4>& localTransforms,
                               const Skeleton& skeleton,
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
    static void SolveFABRIK(std::vector<XMFLOAT4X4>& localTransforms,
                             const Skeleton& skeleton,
                             const IKChain& chain);
};

} // namespace Spark::Animation
