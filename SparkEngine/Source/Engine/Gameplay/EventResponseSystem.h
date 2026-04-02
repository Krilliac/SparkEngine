/**
 * @file EventResponseSystem.h
 * @brief Data-driven "When/If/Then" event response rule engine for no-code gameplay
 *
 * Enables non-coders to create gameplay logic via composable rules:
 *   WHEN [Event] → IF [Condition] → THEN [Action(s)]
 *
 * Integrates with the existing EventBus for triggers and ConditionSystem for
 * evaluation. Rules are defined as data (JSON), no scripting required.
 *
 * @see ConditionSystem.h for condition evaluation
 * @see EventSystem.h for event types
 */

#pragma once

#include "ConditionSystem.h"
#include "Engine/Events/EventSystem.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Spark::Gameplay
{

    // ========================================================================
    // Event Trigger Types
    // ========================================================================

    /**
     * @brief Types of events that can trigger a rule
     *
     * Each maps to an existing EventBus event type or internal mechanism.
     */
    enum class EventTriggerType : uint16_t
    {
        OnTriggerEnter,    ///< Entity entered a trigger volume (TriggerEnterEvent)
        OnTriggerExit,     ///< Entity exited a trigger volume (TriggerExitEvent)
        OnDamaged,         ///< Entity took damage (EntityDamagedEvent)
        OnKilled,          ///< Entity was killed (EntityKilledEvent)
        OnItemPickup,      ///< Item was picked up (ItemPickedUpEvent)
        OnKeyPress,        ///< Input key pressed (InputActionEvent, triggerParam = action name)
        OnKeyRelease,      ///< Input key released (InputActionEvent, triggerParam = action name)
        OnCollision,       ///< Physics collision (CollisionEvent)
        OnQuestComplete,   ///< Quest completed (QuestCompletedEvent)
        OnTimer,           ///< Repeating timer (triggerParam = interval in seconds)
        OnStart,           ///< Fires once when the rule is loaded
        OnWeatherChange,   ///< Weather changed (WeatherChangedEvent)
        OnTimeOfDay,       ///< Time of day changed (TimeOfDayChangedEvent)
        OnEntityCreated,   ///< Entity created (EntityCreatedEvent)
        OnEntityDestroyed, ///< Entity destroyed (EntityDestroyedEvent)
        OnCustom,          ///< Custom string event (triggerParam = event name)

        Count
    };

    // ========================================================================
    // Action Types
    // ========================================================================

    /**
     * @brief Types of actions that a rule can execute
     */
    enum class ActionType : uint16_t
    {
        // Entity lifecycle
        SpawnEntity,   ///< params: [string name]
        DestroyEntity, ///< params: [uint32 entityId] (0 = self)
        EnableEntity,  ///< params: [uint32 entityId]
        DisableEntity, ///< params: [uint32 entityId]

        // Transform
        SetPosition,    ///< params: [double x, double y, double z]
        MoveToward,     ///< params: [double x, double y, double z, double speed]
        TeleportEntity, ///< params: [uint32 entityId, double x, double y, double z]
        RotateEntity,   ///< params: [double pitch, double yaw, double roll]

        // Health / Combat
        SetHealth,  ///< params: [double value]
        DealDamage, ///< params: [double amount]
        HealEntity, ///< params: [double amount]

        // Audio
        PlaySound, ///< params: [string soundPath]
        StopSound, ///< params: [string soundPath]

        // Animation
        PlayAnimation, ///< params: [string animationName]

        // Physics
        ApplyForce,   ///< params: [double x, double y, double z]
        ApplyImpulse, ///< params: [double x, double y, double z]

        // UI / Dialogue
        ShowDialogue, ///< params: [string dialogueId]
        ShowMessage,  ///< params: [string message]

        // World state
        SetWeather,       ///< params: [int64 weatherType]
        SetTimeOfDay,     ///< params: [double hour]
        SetWorldVariable, ///< params: [string name, int64 value]
        SetWorldFlag,     ///< params: [string name, int64 value (0/1)]

        // Flow
        Delay,           ///< params: [double seconds] — delays subsequent actions
        FireCustomEvent, ///< params: [string eventName]

        Count
    };

    // ========================================================================
    // Action Parameter and Rule Structures
    // ========================================================================

    using ActionParam = std::variant<std::monostate, int64_t, double, std::string, uint32_t>;

    /**
     * @brief A single action to execute when a rule fires
     */
    struct GameplayAction
    {
        ActionType type = ActionType::ShowMessage;
        std::vector<ActionParam> params;
    };

    /**
     * @brief A complete event response rule: When + If + Then
     */
    struct EventResponseRule
    {
        std::string name;            ///< Human-readable rule name
        uint32_t sourceEntityId = 0; ///< Entity this rule applies to (0 = global)
        EventTriggerType trigger = EventTriggerType::OnStart;
        std::string triggerParam;            ///< Trigger-specific parameter (key name, timer interval, etc.)
        ConditionSet conditions;             ///< Conditions to check (AND/OR groups, reuses ConditionSystem)
        std::vector<GameplayAction> actions; ///< Actions to execute in order
        bool enabled = true;                 ///< Whether the rule is active
        bool oneShot = false;                ///< If true, disables after firing once
    };

    // ========================================================================
    // Delayed Action (internal)
    // ========================================================================

    struct DelayedActionBatch
    {
        float remainingSeconds = 0.0f;
        uint32_t contextEntity = 0;
        std::vector<GameplayAction> actions; ///< Actions to execute after delay
    };

    // ========================================================================
    // Event Response System
    // ========================================================================

    /**
     * @brief Data-driven gameplay rule engine
     *
     * Subscribes to EventBus events and evaluates rules each time an event
     * fires. If conditions pass, actions are executed immediately (or after
     * a Delay action). Rules are serializable to/from JSON.
     *
     * @code
     *   auto& ers = EventResponseSystem::GetInstance();
     *   ers.Initialize();
     *
     *   EventResponseRule rule;
     *   rule.name = "Door Open";
     *   rule.trigger = EventTriggerType::OnTriggerEnter;
     *   rule.actions.push_back({ActionType::PlayAnimation, {"door_open"}});
     *   ers.AddRule(std::move(rule));
     * @endcode
     */
    class EventResponseSystem
    {
      public:
        static EventResponseSystem& GetInstance();

        /**
         * @brief Initialize the system and subscribe to EventBus events
         */
        void Initialize();

        /**
         * @brief Shutdown and unsubscribe from all events
         */
        void Shutdown();

        /**
         * @brief Process timer-based rules and delayed actions
         * @param deltaTime Frame delta time in seconds
         */
        void Update(float deltaTime);

        // -- Rule management --

        void AddRule(EventResponseRule rule);
        void RemoveRule(const std::string& name);
        void SetRuleEnabled(const std::string& name, bool enabled);
        const std::vector<EventResponseRule>& GetRules() const { return m_rules; }
        std::vector<EventResponseRule>& GetRulesMutable() { return m_rules; }
        void ClearRules();

        // -- Action execution --

        /**
         * @brief Execute a list of actions in the context of an entity
         * @param actions Actions to execute
         * @param contextEntity The entity context (for self-referencing actions)
         */
        void ExecuteActions(const std::vector<GameplayAction>& actions, uint32_t contextEntity);

        // -- Serialization --

        bool SaveToJson(const std::string& path) const;
        bool LoadFromJson(const std::string& path);

        // -- Custom event support --

        /**
         * @brief Fire a custom named event (matched by OnCustom rules)
         * @param eventName The custom event name
         * @param sourceEntity The entity that fired the event
         */
        void FireCustomEvent(const std::string& eventName, uint32_t sourceEntity = 0);

        // -- Statistics --

        uint32_t GetRuleCount() const { return static_cast<uint32_t>(m_rules.size()); }
        uint32_t GetTimesFirered() const { return m_totalFired; }

      private:
        EventResponseSystem() = default;

        void SubscribeToEvents();
        void UnsubscribeFromEvents();

        /**
         * @brief Evaluate and fire matching rules for a given trigger type
         * @param trigger The trigger type that occurred
         * @param triggerParam Trigger-specific parameter
         * @param contextEntity The entity involved in the event
         */
        void EvaluateRules(EventTriggerType trigger, const std::string& triggerParam, uint32_t contextEntity);

        /**
         * @brief Execute a single action
         * @param action The action to execute
         * @param contextEntity The entity context
         * @param remainingActions Remaining actions (for Delay to defer)
         * @param actionIndex Current index in the action list
         * @return true if execution should continue to next action
         */
        bool ExecuteSingleAction(const GameplayAction& action, uint32_t contextEntity,
                                 const std::vector<GameplayAction>& allActions, size_t actionIndex);

        std::vector<EventResponseRule> m_rules;
        std::vector<DelayedActionBatch> m_delayedActions;
        std::vector<Spark::SubscriptionHandle> m_subscriptions;

        // Timer tracking for OnTimer rules
        struct TimerState
        {
            float interval = 1.0f;
            float elapsed = 0.0f;
        };
        std::unordered_map<std::string, TimerState> m_timers; ///< Keyed by rule name

        uint32_t m_totalFired = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Gameplay
