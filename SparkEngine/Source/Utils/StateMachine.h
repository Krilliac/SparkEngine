/**
 * @file StateMachine.h
 * @brief Generic template-based finite state machine with enter/update/exit callbacks
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a lightweight, template-based Finite State Machine (FSM) suitable for
 * game entity AI, UI navigation, game phase management, and other state-driven systems.
 * Simpler than BehaviorTree for sequential, non-hierarchical states.
 *
 * ## Design
 * - States are identified by a user-supplied enum or integer type `StateID`.
 * - Each state has optional `OnEnter`, `OnUpdate(float dt)`, and `OnExit` callbacks.
 * - Transitions are evaluated in registration order; the first matching transition wins.
 * - `Tick(dt)` evaluates all transitions from the current state and calls `OnUpdate`.
 * - Changing state is also possible explicitly via `TransitionTo(id)`.
 * - No external dependencies; header-only.
 *
 * ## Thread safety
 * `StateMachine` is **not thread-safe**. Use from a single thread or provide
 * external synchronization.
 *
 * ## Usage
 * @code
 *   enum class EnemyState { Idle, Patrol, Chase, Attack, Dead };
 *
 *   Spark::StateMachine<EnemyState> fsm;
 *
 *   fsm.AddState(EnemyState::Idle,
 *       []{ PlayAnimation("idle"); },                  // OnEnter
 *       [](float dt){ WaitForPlayer(dt); },            // OnUpdate
 *       []{ });                                        // OnExit
 *
 *   fsm.AddState(EnemyState::Chase,
 *       []{ PlayAnimation("run"); },
 *       [&](float dt){ MoveTowardPlayer(dt); },
 *       []{ });
 *
 *   fsm.AddTransition(EnemyState::Idle, EnemyState::Chase,
 *       []{ return PlayerInRange(20.0f); });
 *
 *   fsm.AddTransition(EnemyState::Chase, EnemyState::Attack,
 *       []{ return PlayerInRange(2.0f); });
 *
 *   fsm.Start(EnemyState::Idle);
 *
 *   // Every frame:
 *   fsm.Tick(deltaTime);
 * @endcode
 */

#pragma once

#include "ContainerUtils.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @class StateMachine
     * @brief Template finite state machine parameterized by state identifier type.
     *
     * @tparam StateID  Type used to identify states. Must be hashable (usable as
     *                  `std::unordered_map` key). Typically an `enum class`.
     */
    template <typename StateID> class StateMachine
    {
      public:
        using EnterFn = std::function<void()>;
        using UpdateFn = std::function<void(float)>;
        using ExitFn = std::function<void()>;
        using ConditionFn = std::function<bool()>;

        StateMachine() = default;

        // =====================================================================
        // State registration
        // =====================================================================

        /**
         * @brief Register a state with optional enter, update, and exit callbacks.
         *
         * All callbacks are optional; pass `nullptr` to skip any of them.
         * Calling `AddState` for an existing StateID overwrites the previous definition.
         *
         * @param id      State identifier.
         * @param onEnter Called once when this state becomes active.
         * @param onUpdate Called every `Tick(dt)` while this state is active.
         * @param onExit  Called once when leaving this state.
         */
        void AddState(StateID id, EnterFn onEnter = nullptr, UpdateFn onUpdate = nullptr, ExitFn onExit = nullptr)
        {
            m_states[id] = State{std::move(onEnter), std::move(onUpdate), std::move(onExit)};
        }

        // =====================================================================
        // Transition registration
        // =====================================================================

        /**
         * @brief Register a conditional transition from one state to another.
         *
         * Transitions are evaluated in the order they are added. The first
         * transition whose `condition()` returns true is taken.
         *
         * @param from      Source state.
         * @param to        Target state (must be registered before `Start()` is called).
         * @param condition Callable returning `true` when the transition should fire.
         */
        void AddTransition(StateID from, StateID to, ConditionFn condition)
        {
            m_transitions[from].push_back({to, std::move(condition)});
        }

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /**
         * @brief Start the state machine in the given initial state.
         *
         * Calls `OnEnter` for the initial state. Must be called before `Tick()`.
         *
         * @param initialState  The state to activate first.
         * @throws std::invalid_argument if `initialState` was not registered.
         */
        void Start(StateID initialState)
        {
            if (!Spark::ContainerUtils::Contains(m_states, initialState))
                throw std::invalid_argument("StateMachine::Start: initial state not registered");

            m_current = initialState;
            m_running = true;
            CallEnter(initialState);
        }

        /**
         * @brief Stop the state machine, calling `OnExit` on the current state.
         *
         * After `Stop()`, `Tick()` is a no-op until `Start()` is called again.
         */
        void Stop()
        {
            if (!m_running)
                return;
            if (m_current.has_value())
                CallExit(*m_current);
            m_current.reset();
            m_running = false;
        }

        /**
         * @brief Evaluate transitions and update the current state.
         *
         * Called once per frame. Steps:
         * 1. Evaluate all registered transitions from the current state.
         * 2. If any condition is true, exit the current state and enter the next.
         * 3. Call `OnUpdate(dt)` on the (possibly new) current state.
         *
         * @param dt  Delta time in seconds since the last tick.
         */
        void Tick(float dt)
        {
            if (!m_running || !m_current.has_value())
                return;

            // Check transitions
            auto transIt = m_transitions.find(*m_current);
            if (transIt != m_transitions.end())
            {
                for (auto& trans : transIt->second)
                {
                    if (trans.condition && trans.condition())
                    {
                        TransitionTo(trans.to);
                        break; // Only one transition per tick
                    }
                }
            }

            // Update current state (may have changed due to transition above)
            if (m_current.has_value())
                CallUpdate(*m_current, dt);
        }

        /**
         * @brief Force an immediate transition to the given state.
         *
         * Bypasses condition checking. Calls `OnExit` on the current state
         * and `OnEnter` on the target state.
         *
         * @param to  Target state (must be registered).
         * @throws std::invalid_argument if `to` was not registered.
         */
        void TransitionTo(StateID to)
        {
            if (!Spark::ContainerUtils::Contains(m_states, to))
                throw std::invalid_argument("StateMachine::TransitionTo: target state not registered");

            if (m_current.has_value())
                CallExit(*m_current);

            m_current = to;
            CallEnter(to);
        }

        // =====================================================================
        // Queries
        // =====================================================================

        /**
         * @brief Return the currently active state.
         *
         * @return  Current state ID, or `std::nullopt` if not started / stopped.
         */
        [[nodiscard]] std::optional<StateID> GetCurrentState() const noexcept { return m_current; }

        /**
         * @brief Return true if the machine is in the specified state.
         * @param id  State to test.
         * @return    true if `id` is the active state.
         */
        [[nodiscard]] bool IsInState(StateID id) const noexcept { return m_current.has_value() && *m_current == id; }

        /**
         * @brief Return true if the machine is running (started and not stopped).
         * @return  true if active.
         */
        [[nodiscard]] bool IsRunning() const noexcept { return m_running; }

        /**
         * @brief Return true if `id` has been registered as a state.
         * @param id  State to check.
         * @return    true if registered.
         */
        [[nodiscard]] bool HasState(StateID id) const { return Spark::ContainerUtils::Contains(m_states, id); }

        /**
         * @brief Remove all registered states and transitions, and stop the machine.
         */
        void Reset()
        {
            Stop();
            m_states.clear();
            m_transitions.clear();
        }

      private:
        struct State
        {
            EnterFn onEnter;
            UpdateFn onUpdate;
            ExitFn onExit;
        };

        struct Transition
        {
            StateID to;
            ConditionFn condition;
        };

        void CallEnter(StateID id)
        {
            auto it = m_states.find(id);
            if (it != m_states.end() && it->second.onEnter)
                it->second.onEnter();
        }

        void CallUpdate(StateID id, float dt)
        {
            auto it = m_states.find(id);
            if (it != m_states.end() && it->second.onUpdate)
                it->second.onUpdate(dt);
        }

        void CallExit(StateID id)
        {
            auto it = m_states.find(id);
            if (it != m_states.end() && it->second.onExit)
                it->second.onExit();
        }

        std::unordered_map<StateID, State> m_states;
        std::unordered_map<StateID, std::vector<Transition>> m_transitions;
        std::optional<StateID> m_current;
        bool m_running = false;
    };

} // namespace Spark
