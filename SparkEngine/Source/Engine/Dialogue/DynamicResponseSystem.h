/**
 * @file DynamicResponseSystem.h
 * @brief Signal/condition-driven dynamic response system
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Provides a data-driven response system where signals trigger context-sensitive
 * actions (dialogue lines, variable changes, follow-up signals) based on a set
 * of rules with conditions and priorities. This is separate from the tree-based
 * DialogueSystem — it handles reactive, one-shot contextual responses (barks,
 * ambient chatter, scripted reactions).
 *
 * ## Architecture
 * ```
 * DynamicResponseSystem (singleton)
 *   ├── m_variables      (named float variables for conditions)
 *   ├── m_rules          (response rules triggered by signal names)
 *   ├── m_pendingActions  (actions waiting to execute, e.g., after Wait delays)
 *   └── Update(dt)
 *         └── Process pending actions (execute delayed/queued actions)
 * ```
 *
 * ## CryEngine Comparison
 * CryEngine's DRS uses concept/signal/response tables. This system uses
 * signal-triggered rules with condition predicates and prioritized actions.
 *
 * ## Usage
 * @code
 *   auto& drs = Spark::Dialogue::DynamicResponseSystem::GetInstance();
 *   drs.Initialize();
 *
 *   drs.SetVariable("player_health_low", 0.0f);
 *
 *   ResponseRule rule;
 *   rule.signalName = "OnPlayerSpotted";
 *   rule.conditions = {{"player_health_low", ResponseCondition::Op::Equal, 0.0f}};
 *   rule.actions = {{ResponseAction::Type::Speak, "There he is!", 0.0f}};
 *   rule.priority = 10;
 *   rule.cooldown = 5.0f;
 *   drs.RegisterRule(rule);
 *
 *   // When an NPC spots the player:
 *   drs.SendSignal("OnPlayerSpotted", npcEntity);
 * @endcode
 *
 * @see DialogueSystem.h (tree-based dialogue for longer conversations)
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Dialogue
{

    // =========================================================================
    // Callback Type
    // =========================================================================

    /** @brief Callback for custom response actions. */
    using ResponseCallback = std::function<void(uint32_t speakerEntity)>;

    // =========================================================================
    // Condition
    // =========================================================================

    /**
     * @brief A single condition that must pass for a response rule to fire.
     *
     * Evaluates a named float variable against an operator and value.
     * All conditions in a rule must pass (logical AND).
     */
    struct ResponseCondition
    {
        std::string variableName; ///< Name of the variable to test

        /** @brief Comparison operator for the condition. */
        enum class Op : uint8_t
        {
            Equal,
            NotEqual,
            GreaterThan,
            LessThan,
            InRange ///< value <= variable <= rangeMax
        };

        Op op = Op::Equal;
        float value = 0.0f;    ///< Comparison value (or range min for InRange)
        float rangeMax = 0.0f; ///< Upper bound for InRange operator
    };

    // =========================================================================
    // Action
    // =========================================================================

    /**
     * @brief A single action executed when a response rule fires.
     *
     * Actions execute sequentially. The Wait type introduces a delay
     * before subsequent actions in the same rule.
     */
    struct ResponseAction
    {
        /** @brief Types of actions a response rule can execute. */
        enum class Type : uint8_t
        {
            Speak,       ///< Display/play a dialogue line (stringParam = text)
            SetVariable, ///< Set a DRS variable (stringParam = name, floatParam = value)
            SendSignal,  ///< Fire another signal (stringParam = signal name)
            Wait,        ///< Delay before next action (floatParam = seconds)
            Custom       ///< Invoke a custom callback
        };

        Type type = Type::Speak;
        std::string stringParam;       ///< Context-dependent string parameter
        float floatParam = 0.0f;       ///< Context-dependent float parameter
        ResponseCallback customAction; ///< Callback for Custom type
    };

    // =========================================================================
    // Rule
    // =========================================================================

    /**
     * @brief A response rule: when signal X is received and conditions pass,
     *        execute the highest-priority valid rule's actions.
     */
    struct ResponseRule
    {
        std::string signalName;                    ///< Signal that triggers this rule
        std::vector<ResponseCondition> conditions; ///< All must pass (AND logic)
        std::vector<ResponseAction> actions;       ///< Actions to execute in order
        float cooldown = 0.0f;                     ///< Minimum seconds between triggers
        int priority = 0;                          ///< Higher = preferred when multiple rules match
        float lastTriggeredTime = -999.0f;         ///< Internal: last time this rule fired
    };

    // =========================================================================
    // DynamicResponseSystem
    // =========================================================================

    /**
     * @class DynamicResponseSystem
     * @brief Singleton managing signal-driven contextual responses.
     *
     * Evaluates rules when signals arrive, executes the highest-priority
     * matching rule, and processes delayed actions each frame.
     */
    class DynamicResponseSystem
    {
      public:
        /** @brief Get the singleton instance. */
        static DynamicResponseSystem& GetInstance()
        {
            static DynamicResponseSystem s;
            return s;
        }

        /** @brief Initialize the system. */
        void Initialize();

        /** @brief Shut down and clear all rules and variables. */
        void Shutdown();

        /**
         * @brief Set a named variable value.
         * @param name  Variable name.
         * @param value New value.
         */
        void SetVariable(const std::string& name, float value);

        /**
         * @brief Get a named variable value.
         * @param name Variable name.
         * @return Current value, or 0.0f if not set.
         */
        float GetVariable(const std::string& name) const;

        /**
         * @brief Register a response rule.
         * @param rule The rule to register.
         */
        void RegisterRule(const ResponseRule& rule);

        /**
         * @brief Send a signal, triggering the highest-priority matching rule.
         * @param signalName   Name of the signal.
         * @param senderEntity Entity that originated the signal.
         */
        void SendSignal(const std::string& signalName, uint32_t senderEntity);

        /**
         * @brief Process pending delayed actions.
         * @param deltaTime Frame time in seconds.
         */
        void Update(float deltaTime);

        /** @brief Get the number of registered rules. */
        size_t GetRuleCount() const { return m_rules.size(); }

        /** @brief Get the number of pending actions. */
        size_t GetPendingActionCount() const { return m_pendingActions.size(); }

      private:
        DynamicResponseSystem() = default;

        /** @brief Check if all conditions in a rule pass. */
        bool EvaluateConditions(const std::vector<ResponseCondition>& conditions) const;

        /** @brief Execute a single action immediately. */
        void ExecuteAction(const ResponseAction& action, uint32_t senderEntity);

        /** @brief A delayed action waiting to execute. */
        struct PendingAction
        {
            ResponseAction action;
            uint32_t senderEntity = 0;
            float remainingDelay = 0.0f;
        };

        std::unordered_map<std::string, float> m_variables;
        std::vector<ResponseRule> m_rules;
        std::vector<PendingAction> m_pendingActions;
        float m_gameTime = 0.0f;
        bool m_initialized = false;
    };

} // namespace Spark::Dialogue
