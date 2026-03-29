/**
 * @file AnimationStateMachine.cpp
 * @brief AnimationStateMachine implementation — state transitions, crossfade blending, state management
 *
 * Extracted from AnimationSystem.cpp to keep each file focused on a single responsibility.
 */
#include "../../Core/Platform.h"
#include "AnimationSystem.h"
#include "../../Utils/Validate.h"
#include <sstream>

using namespace DirectX;
namespace Spark::Animation
{

    // ============================================================================
    // AnimationStateMachine
    // ============================================================================

    void AnimationStateMachine::AddState(const AnimationState& state)
    {
        m_states[state.name] = state;
        SPARK_LOG_DEBUG(LogCategory::Animation, "State machine: added state '%s' (clip='%s', speed=%.2f)",
                        state.name.c_str(), state.clipName.c_str(), state.speed);
        if (m_defaultState.empty())
            m_defaultState = state.name;
        if (m_currentState.empty())
            m_currentState = state.name;
    }

    void AnimationStateMachine::AddTransition(const AnimationTransition& transition)
    {
        m_transitions.push_back(transition);
    }

    void AnimationStateMachine::SetDefaultState(const std::string& stateName)
    {
        m_defaultState = stateName;
        if (m_currentState.empty())
            m_currentState = stateName;
    }

    void AnimationStateMachine::Update(float deltaTime)
    {
        SPARK_WARN_IF(LogCategory::Animation, deltaTime < 0.0f,
                      "AnimationStateMachine::Update called with negative deltaTime");

        if (m_currentState.empty())
        {
            if (!m_defaultState.empty())
            {
                m_currentState = m_defaultState;
            }
            else
            {
                return;
            }
        }

        // Advance current state playback time
        auto currentIt = m_states.find(m_currentState);
        if (currentIt != m_states.end())
        {
            m_currentTime += deltaTime * currentIt->second.speed;
        }

        if (m_isTransitioning)
        {
            // Advance the target state's playback time during the crossfade
            auto targetIt = m_states.find(m_targetState);
            if (targetIt != m_states.end())
            {
                m_targetTime += deltaTime * targetIt->second.speed;
            }

            m_transitionElapsed += deltaTime;
            m_blendFactor =
                (m_transitionDuration > 0.0f) ? (std::min)(m_transitionElapsed / m_transitionDuration, 1.0f) : 1.0f;

            if (m_blendFactor >= 1.0f)
            {
                // Transition complete: target becomes current
                SPARK_LOG_INFO(LogCategory::Animation, "State machine transition complete: now in '%s'",
                               m_targetState.c_str());
                m_currentState = m_targetState;
                m_currentTime = m_targetTime;
                m_targetState.clear();
                m_targetTime = 0.0f;
                m_isTransitioning = false;
                m_blendFactor = 0.0f;
                m_transitionElapsed = 0.0f;
                m_transitionDuration = 0.0f;
            }
            return;
        }

        // Evaluate transitions from the current state
        // Compute normalized time for exit-time checks
        float normalizedTime = 0.0f;
        if (currentIt != m_states.end())
        {
            auto clipPtr = AnimationManager::GetInstance().GetClip(currentIt->second.clipName);
            if (clipPtr && clipPtr->duration > 0.0f)
            {
                normalizedTime = m_currentTime / clipPtr->duration;
            }
        }

        for (const auto& t : m_transitions)
        {
            bool fromMatch = (t.fromState == m_currentState) || (t.fromState == "*");
            if (!fromMatch)
                continue;

            // Check exit time requirement
            if (t.hasExitTime && normalizedTime < t.exitTime)
                continue;

            // Check condition
            if (t.condition && !t.condition())
                continue;

            // Fire the transition
            SPARK_LOG_INFO(LogCategory::Animation, "State machine transition: '%s' -> '%s' (duration=%.2fs)",
                           m_currentState.c_str(), t.toState.c_str(), t.duration);
            m_targetState = t.toState;
            m_transitionDuration = t.duration;
            m_transitionElapsed = 0.0f;
            m_targetTime = 0.0f;
            m_isTransitioning = true;
            m_blendFactor = 0.0f;
            break;
        }
    }

    std::string AnimationStateMachine::GetCurrentClipName() const
    {
        auto it = m_states.find(m_currentState);
        if (it != m_states.end())
        {
            return it->second.clipName;
        }
        return {};
    }

    std::string AnimationStateMachine::GetTargetClipName() const
    {
        if (!m_isTransitioning)
            return {};
        auto it = m_states.find(m_targetState);
        if (it != m_states.end())
        {
            return it->second.clipName;
        }
        return {};
    }

    void AnimationStateMachine::ForceState(const std::string& stateName)
    {
        if (m_states.contains(stateName))
        {
            SPARK_LOG_INFO(LogCategory::Animation, "State machine: forced state '%s' (was '%s')", stateName.c_str(),
                           m_currentState.c_str());
            m_currentState = stateName;
            m_isTransitioning = false;
            m_currentTime = 0.0f;
            m_blendFactor = 0.0f;
        }
        else
        {
            SPARK_LOG_WARN(LogCategory::Animation, "State machine: ForceState('%s') failed - state not found",
                           stateName.c_str());
        }
    }

    std::string AnimationStateMachine::Console_GetStateInfo() const
    {
        std::ostringstream ss;
        ss << "Current State: " << m_currentState << "\n";
        ss << "Time: " << m_currentTime << "s\n";
        if (m_isTransitioning)
        {
            ss << "Transitioning to: " << m_targetState << "\n";
            ss << "Blend: " << (m_blendFactor * 100.0f) << "%\n";
        }
        ss << "States (" << m_states.size() << "): ";
        for (const auto& [name, _] : m_states)
            ss << name << " ";
        ss << "\n";
        return ss.str();
    }

} // namespace Spark::Animation
