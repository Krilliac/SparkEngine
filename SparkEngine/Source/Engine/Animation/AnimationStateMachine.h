/**
 * @file AnimationStateMachine.h
 * @brief Animation state machine with states, transitions, and crossfade blending
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>


namespace Spark::Animation
{

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

} // namespace Spark::Animation
