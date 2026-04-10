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
#include "AnimationTypes.h"

#include <memory>
#include <algorithm>


namespace Spark::Animation
{

    // Thread safety: Main thread only. Animation state machines, blending,
    // and IK solving must be called from the main update thread.

    // =============================================================================
    // Animation State Machine
    // =============================================================================

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
    class AnimationStateMachine
    {
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
        float GetCurrentPlaybackTime() const { return m_currentTime; }

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
     * @brief Get the playback time within the target state's clip during a crossfade.
     * @return  Elapsed time within the target clip.
     */
        float GetTargetTime() const { return m_targetTime; }

        /**
     * @brief Get the clip name for the current state.
     * @return  Clip name, or empty string if the state is not found.
     */
        std::string GetCurrentClipName() const;

        /**
     * @brief Get the clip name for the target state during a crossfade.
     * @return  Clip name, or empty string if no transition is active.
     */
        std::string GetTargetClipName() const;

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

        std::string m_currentState; ///< Name of the currently active state.
        std::string m_targetState;  ///< Target state during a crossfade (empty otherwise).
        std::string m_defaultState; ///< The entry state on first run.

        float m_currentTime = 0.0f;        ///< Playback time within the current state's clip.
        float m_targetTime = 0.0f;         ///< Playback time within the target state's clip during crossfade.
        float m_blendFactor = 0.0f;        ///< Crossfade progress (0 = source, 1 = target).
        float m_transitionDuration = 0.0f; ///< Total duration of the current crossfade.
        float m_transitionElapsed = 0.0f;  ///< Elapsed time within the current crossfade.
        bool m_isTransitioning = false;    ///< Whether a crossfade is currently active.
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
    struct AnimationInstance
    {
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

        /**
     * @brief Advance the animation instance by one frame.
     *
     * Executes the full per-entity animation pipeline:
     * 1. Update the state machine (transition evaluation, crossfade blending).
     * 2. Advance layer playback times.
     * 3. Sample clips for each layer and blend them together.
     * 4. Compute final skinning matrices from the blended local transforms.
     * 5. Solve IK chains as a post-process pass.
     * 6. Extract root motion if enabled.
     *
     * @param deltaTime  Time elapsed since the last frame (seconds).
     */
        void Update(float deltaTime);

        /**
     * @brief Advance layer playback times and sample/blend all layers.
     *
     * Processes the layer stack bottom-to-top. Each layer's clip is sampled at
     * its current time, then blended into the result based on blend mode, weight,
     * and optional bone mask.
     *
     * @param deltaTime  Time elapsed since the last frame (seconds).
     */
        void UpdateLayers(float deltaTime);
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
    /**
     * @class AnimationManager
     * @brief Registry singleton for loaded skeletons and animation clips.
     *
     * @note **Intentional demand-driven registry** — `AnimationManager` has
     *       no `Initialize()` / `Update()` / `Shutdown()` lifecycle and is
     *       intentionally not wired into `GameplayLifecycleShared.cpp`.
     *       The ECS `AnimationUpdateSystem` (registered at
     *       `EngineSetup.h:187`, `Phase::Animation`) calls into
     *       `GetClip()` / `GetSkeleton()` on demand each frame to drive
     *       per-entity playback, and asset importers call `RegisterClip()`
     *       / `LoadSkeleton()` / `LoadAnimations()` at load time. The
     *       complementary `AnimNotifyManager` (wired at
     *       `GameplayLifecycleShared.cpp:445,1032`) handles event
     *       delivery — it is orthogonal to this registry, not a
     *       duplicate. `Clear()` is safe to call at level unload but is
     *       not a lifecycle requirement because cached entries use
     *       `shared_ptr` and outlive this map while anyone holds a ref.
     *       Exercised by `Tests/TestAnimationSystem.cpp` (895 lines).
     */
    class AnimationManager
    {
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

    /**
     * @brief Type alias so EngineContext/EngineSetup can reference "AnimationSystem"
     *        when the actual singleton class is AnimationManager.
     */
    using AnimationSystem = AnimationManager;

} // namespace Spark::Animation
