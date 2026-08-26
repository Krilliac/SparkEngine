/**
 * @file DynamicResponseSystem.cpp
 * @brief Dynamic response system implementation
 */

#include "DynamicResponseSystem.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>

namespace Spark::Dialogue
{

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void DynamicResponseSystem::Initialize()
    {
        m_variables.clear();
        m_rules.clear();
        m_actionScheduler.ClearAll();
        m_gameTime = 0.0f;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "DynamicResponseSystem initialized");
    }

    void DynamicResponseSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "DynamicResponseSystem shutting down (%zu rules, %zu variables)",
                       m_rules.size(), m_variables.size());
        m_variables.clear();
        m_rules.clear();
        m_actionScheduler.ClearAll();
        m_initialized = false;
    }

    // =========================================================================
    // Variables
    // =========================================================================

    void DynamicResponseSystem::SetVariable(const std::string& name, float value)
    {
        m_variables[name] = value;
    }

    float DynamicResponseSystem::GetVariable(const std::string& name) const
    {
        auto it = m_variables.find(name);
        return (it != m_variables.end()) ? it->second : 0.0f;
    }

    // =========================================================================
    // Rules
    // =========================================================================

    void DynamicResponseSystem::RegisterRule(const ResponseRule& rule)
    {
        if (!m_initialized)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "DynamicResponseSystem::RegisterRule called before Initialize");
            return;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Core, "DynamicResponseSystem: registered rule for signal '%s' (priority %d)",
                       rule.signalName.c_str(), rule.priority);
        m_rules.push_back(rule);
    }

    // =========================================================================
    // Signal Processing
    // =========================================================================

    void DynamicResponseSystem::SendSignal(const std::string& signalName, uint32_t senderEntity)
    {
        if (!m_initialized)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "DynamicResponseSystem::SendSignal('%s') called before Initialize",
                           signalName.c_str());
            return;
        }

        // Guard against unbounded recursion. A SendSignal action re-enters SendSignal
        // (see ExecuteAction), so a rule that emits a signal matching itself — or two
        // rules that mutually trigger — would recurse until the stack overflows. The
        // default 0s cooldown does not stop this (m_gameTime never advances during the
        // synchronous recursion), so cap the depth here. Deferred (Wait) actions run via
        // the scheduler on later frames and are unaffected. thread_local keeps the count
        // correct if signals are dispatched from more than one thread.
        static thread_local int s_signalDepth = 0;
        static constexpr int kMaxSignalDepth = 32;
        if (s_signalDepth >= kMaxSignalDepth)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "DynamicResponseSystem: signal '%s' exceeded max recursion depth (%d) — aborting to "
                           "prevent stack overflow",
                           signalName.c_str(), kMaxSignalDepth);
            return;
        }
        ++s_signalDepth;
        struct DepthGuard
        {
            int& depth;
            ~DepthGuard() { --depth; }
        } depthGuard{s_signalDepth};

        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "DynamicResponseSystem: signal '%s' from entity %u",
                        signalName.c_str(), senderEntity);

        // Find all rules matching this signal, pick highest priority that passes conditions
        ResponseRule* bestRule = nullptr;

        for (auto& rule : m_rules)
        {
            if (rule.signalName != signalName)
            {
                continue;
            }

            // Check cooldown
            if (rule.cooldown > 0.0f && (m_gameTime - rule.lastTriggeredTime) < rule.cooldown)
            {
                continue;
            }

            // Check conditions
            if (!EvaluateConditions(rule.conditions))
            {
                continue;
            }

            // Pick highest priority
            if (bestRule == nullptr || rule.priority > bestRule->priority)
            {
                bestRule = &rule;
            }
        }

        if (bestRule == nullptr)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "DynamicResponseSystem: no matching rule for signal '%s' — executing default response",
                           signalName.c_str());
            // Execute a default fallback action so the caller gets a response
            ResponseAction fallback;
            fallback.type = ResponseAction::Type::Speak;
            fallback.stringParam = "[No response available]";
            ExecuteAction(fallback, senderEntity);
            return;
        }

        // Mark the rule as triggered, then snapshot its actions before invoking
        // any user callback. A Custom action may legitimately register another
        // rule or shut the system down; either operation can reallocate/clear
        // m_rules and would invalidate bestRule and references into its action
        // vector while this dispatch is still on the stack.
        bestRule->lastTriggeredTime = m_gameTime;
        const std::vector<ResponseAction> actions = bestRule->actions;

        // Execute actions (Wait actions queue remaining actions as pending)
        for (size_t i = 0; i < actions.size(); ++i)
        {
            if (!m_initialized)
                break;

            const auto& action = actions[i];

            if (action.type == ResponseAction::Type::Wait)
            {
                // Queue all remaining actions with accumulated delay
                float delay = action.floatParam;
                for (size_t j = i + 1; j < actions.size(); ++j)
                {
                    const auto& deferred = actions[j];
                    if (deferred.type == ResponseAction::Type::Wait)
                    {
                        delay += deferred.floatParam;
                    }
                    else
                    {
                        ResponseAction deferredCopy = deferred;
                        uint32_t sender = senderEntity;
                        (void)m_actionScheduler.Schedule([this, action = std::move(deferredCopy), sender]
                                                         { ExecuteAction(action, sender); }, delay);
                    }
                }
                break; // All remaining actions are now pending
            }

            ExecuteAction(action, senderEntity);
        }
    }

    // =========================================================================
    // Update
    // =========================================================================

    void DynamicResponseSystem::Update(float deltaTime)
    {
        if (!m_initialized)
        {
            return;
        }

        m_gameTime += deltaTime;

        // Process pending (delayed) actions via scheduler
        SPARK_GUARDED_UPDATE("Dialogue:ActionScheduler", "Dialogue", { m_actionScheduler.Update(deltaTime); });
    }

    // =========================================================================
    // Condition Evaluation
    // =========================================================================

    bool DynamicResponseSystem::EvaluateConditions(const std::vector<ResponseCondition>& conditions) const
    {
        for (const auto& cond : conditions)
        {
            float varValue = GetVariable(cond.variableName);

            bool passes = false;
            switch (cond.op)
            {
            case ResponseCondition::Op::Equal:
                passes = (std::fabs(varValue - cond.value) < 0.0001f);
                break;
            case ResponseCondition::Op::NotEqual:
                passes = (std::fabs(varValue - cond.value) >= 0.0001f);
                break;
            case ResponseCondition::Op::GreaterThan:
                passes = (varValue > cond.value);
                break;
            case ResponseCondition::Op::LessThan:
                passes = (varValue < cond.value);
                break;
            case ResponseCondition::Op::InRange:
                passes = (varValue >= cond.value && varValue <= cond.rangeMax);
                break;
            }

            if (!passes)
            {
                return false;
            }
        }

        return true; // All conditions passed (empty conditions = always true)
    }

    // =========================================================================
    // Action Execution
    // =========================================================================

    void DynamicResponseSystem::ExecuteAction(const ResponseAction& action, uint32_t senderEntity)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "DynamicResponseSystem: executing action type %d for entity %u",
                        static_cast<int>(action.type), senderEntity);
        switch (action.type)
        {
        case ResponseAction::Type::Speak:
            // In a full integration, this would send the text to the UI system
            // or audio system for playback. For now, the action is acknowledged.
            break;

        case ResponseAction::Type::SetVariable:
            SetVariable(action.stringParam, action.floatParam);
            break;

        case ResponseAction::Type::SendSignal:
            // Recursive signal — triggers another round of rule evaluation
            SendSignal(action.stringParam, senderEntity);
            break;

        case ResponseAction::Type::Wait:
            // Wait is handled in SendSignal's action loop; should not reach here
            break;

        case ResponseAction::Type::Custom:
            if (action.customAction)
            {
                action.customAction(senderEntity);
            }
            break;
        }
    }

} // namespace Spark::Dialogue
