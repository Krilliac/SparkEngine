// TestEventResponseSystem.cpp - Tests for the data-driven event response rule engine
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace TestERS
{

    // ========================================================================
    // Standalone types (mirrors engine types for CI without full engine deps)
    // ========================================================================

    enum class EventTriggerType : uint16_t
    {
        OnTriggerEnter,
        OnTriggerExit,
        OnDamaged,
        OnKilled,
        OnItemPickup,
        OnKeyPress,
        OnKeyRelease,
        OnCollision,
        OnQuestComplete,
        OnTimer,
        OnStart,
        OnWeatherChange,
        OnTimeOfDay,
        OnEntityCreated,
        OnEntityDestroyed,
        OnCustom,
        Count
    };

    enum class ActionType : uint16_t
    {
        SpawnEntity,
        DestroyEntity,
        EnableEntity,
        DisableEntity,
        SetPosition,
        MoveToward,
        TeleportEntity,
        RotateEntity,
        SetHealth,
        DealDamage,
        HealEntity,
        PlaySound,
        StopSound,
        PlayAnimation,
        ApplyForce,
        ApplyImpulse,
        ShowDialogue,
        ShowMessage,
        SetWeather,
        SetTimeOfDay,
        SetWorldVariable,
        SetWorldFlag,
        Delay,
        FireCustomEvent,
        Count
    };

    using ActionParam = std::variant<std::monostate, int64_t, double, std::string, uint32_t>;

    struct GameplayAction
    {
        ActionType type = ActionType::ShowMessage;
        std::vector<ActionParam> params;
    };

    struct EventResponseRule
    {
        std::string name;
        uint32_t sourceEntityId = 0;
        EventTriggerType trigger = EventTriggerType::OnStart;
        std::string triggerParam;
        std::vector<GameplayAction> actions;
        bool enabled = true;
        bool oneShot = false;
    };

    // Simplified rule evaluator
    struct ActionLog
    {
        ActionType type;
        uint32_t contextEntity;
    };

    class SimpleEventResponseSystem
    {
      public:
        std::vector<EventResponseRule> rules;
        std::vector<ActionLog> actionLog;
        uint32_t totalFired = 0;

        void AddRule(EventResponseRule rule)
        {
            if (rule.trigger == EventTriggerType::OnStart && rule.enabled)
            {
                ExecuteActions(rule.actions, rule.sourceEntityId);
                ++totalFired;
                if (rule.oneShot)
                {
                    return;
                }
            }
            rules.push_back(std::move(rule));
        }

        void EvaluateRules(EventTriggerType trigger, const std::string& triggerParam, uint32_t contextEntity)
        {
            std::vector<std::string> toDisable;

            for (auto& rule : rules)
            {
                if (!rule.enabled || rule.trigger != trigger)
                {
                    continue;
                }
                if (rule.sourceEntityId != 0 && rule.sourceEntityId != contextEntity)
                {
                    continue;
                }
                if (trigger == EventTriggerType::OnKeyPress || trigger == EventTriggerType::OnCustom)
                {
                    if (!rule.triggerParam.empty() && rule.triggerParam != triggerParam)
                    {
                        continue;
                    }
                }

                ExecuteActions(rule.actions, contextEntity);
                ++totalFired;

                if (rule.oneShot)
                {
                    toDisable.push_back(rule.name);
                }
            }

            for (const auto& name : toDisable)
            {
                for (auto& r : rules)
                {
                    if (r.name == name)
                    {
                        r.enabled = false;
                    }
                }
            }
        }

        void ExecuteActions(const std::vector<GameplayAction>& actions, uint32_t contextEntity)
        {
            for (const auto& action : actions)
            {
                if (action.type == ActionType::Delay)
                {
                    break; // Stop at delay
                }
                actionLog.push_back({action.type, contextEntity});
            }
        }

        void FireCustomEvent(const std::string& eventName, uint32_t sourceEntity)
        {
            EvaluateRules(EventTriggerType::OnCustom, eventName, sourceEntity);
        }
    };

} // namespace TestERS

// ============================================================================
// Tests
// ============================================================================

TEST(EventResponseSystem_BasicRuleAddition)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Test Rule";
    rule.trigger = EventTriggerType::OnDamaged;
    rule.actions.push_back({ActionType::PlaySound, {std::string("hit.wav")}});
    sys.AddRule(std::move(rule));

    EXPECT_EQ(sys.rules.size(), size_t(1));
    EXPECT_EQ(sys.rules[0].name, std::string("Test Rule"));
}

TEST(EventResponseSystem_OnStartFiresImmediately)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Start Rule";
    rule.trigger = EventTriggerType::OnStart;
    rule.actions.push_back({ActionType::ShowMessage, {std::string("Hello")}});
    sys.AddRule(std::move(rule));

    EXPECT_EQ(sys.totalFired, uint32_t(1));
    EXPECT_EQ(sys.actionLog.size(), size_t(1));
    EXPECT_EQ(static_cast<int>(sys.actionLog[0].type), static_cast<int>(ActionType::ShowMessage));
}

TEST(EventResponseSystem_TriggerMatchByType)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Damage Rule";
    rule.trigger = EventTriggerType::OnDamaged;
    rule.actions.push_back({ActionType::PlaySound, {std::string("hit.wav")}});
    sys.AddRule(std::move(rule));

    // Should NOT fire on wrong event
    sys.EvaluateRules(EventTriggerType::OnKilled, "", 1);
    EXPECT_EQ(sys.totalFired, uint32_t(0));

    // Should fire on correct event
    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 1);
    EXPECT_EQ(sys.totalFired, uint32_t(1));
    EXPECT_EQ(sys.actionLog.size(), size_t(1));
}

TEST(EventResponseSystem_EntityFiltering)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Entity-Specific";
    rule.trigger = EventTriggerType::OnDamaged;
    rule.sourceEntityId = 42;
    rule.actions.push_back({ActionType::HealEntity, {10.0}});
    sys.AddRule(std::move(rule));

    // Wrong entity — should not fire
    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 99);
    EXPECT_EQ(sys.totalFired, uint32_t(0));

    // Correct entity
    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 42);
    EXPECT_EQ(sys.totalFired, uint32_t(1));
}

TEST(EventResponseSystem_OneShotRule)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "OneShot";
    rule.trigger = EventTriggerType::OnDamaged;
    rule.oneShot = true;
    rule.actions.push_back({ActionType::SpawnEntity, {std::string("Loot")}});
    sys.AddRule(std::move(rule));

    // First fire
    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 1);
    EXPECT_EQ(sys.totalFired, uint32_t(1));

    // Second fire — should NOT fire (one-shot disabled)
    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 1);
    EXPECT_EQ(sys.totalFired, uint32_t(1)); // Still 1
}

TEST(EventResponseSystem_KeyPressParamFiltering)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Jump Rule";
    rule.trigger = EventTriggerType::OnKeyPress;
    rule.triggerParam = "Jump";
    rule.actions.push_back({ActionType::ApplyForce, {0.0, 10.0, 0.0}});
    sys.AddRule(std::move(rule));

    // Wrong key
    sys.EvaluateRules(EventTriggerType::OnKeyPress, "Fire", 0);
    EXPECT_EQ(sys.totalFired, uint32_t(0));

    // Correct key
    sys.EvaluateRules(EventTriggerType::OnKeyPress, "Jump", 0);
    EXPECT_EQ(sys.totalFired, uint32_t(1));
}

TEST(EventResponseSystem_CustomEvents)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Custom Handler";
    rule.trigger = EventTriggerType::OnCustom;
    rule.triggerParam = "door_opened";
    rule.actions.push_back({ActionType::PlaySound, {std::string("chime.wav")}});
    sys.AddRule(std::move(rule));

    sys.FireCustomEvent("door_opened", 5);
    EXPECT_EQ(sys.totalFired, uint32_t(1));
    EXPECT_EQ(sys.actionLog.size(), size_t(1));
}

TEST(EventResponseSystem_MultipleActions)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "MultiAction";
    rule.trigger = EventTriggerType::OnKilled;
    rule.actions.push_back({ActionType::PlaySound, {std::string("death.wav")}});
    rule.actions.push_back({ActionType::SpawnEntity, {std::string("Loot")}});
    rule.actions.push_back({ActionType::DestroyEntity, {}});
    sys.AddRule(std::move(rule));

    sys.EvaluateRules(EventTriggerType::OnKilled, "", 10);
    EXPECT_EQ(sys.actionLog.size(), size_t(3));
    EXPECT_EQ(static_cast<int>(sys.actionLog[0].type), static_cast<int>(ActionType::PlaySound));
    EXPECT_EQ(static_cast<int>(sys.actionLog[1].type), static_cast<int>(ActionType::SpawnEntity));
    EXPECT_EQ(static_cast<int>(sys.actionLog[2].type), static_cast<int>(ActionType::DestroyEntity));
}

TEST(EventResponseSystem_DisabledRuleDontFire)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Disabled";
    rule.trigger = EventTriggerType::OnDamaged;
    rule.enabled = false;
    rule.actions.push_back({ActionType::ShowMessage, {std::string("Ouch")}});
    sys.AddRule(std::move(rule));

    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 1);
    EXPECT_EQ(sys.totalFired, uint32_t(0));
}

TEST(EventResponseSystem_DelayStopsExecution)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Delayed";
    rule.trigger = EventTriggerType::OnDamaged;
    rule.actions.push_back({ActionType::PlaySound, {std::string("hit.wav")}});
    rule.actions.push_back({ActionType::Delay, {2.0}});
    rule.actions.push_back({ActionType::DestroyEntity, {}});
    sys.AddRule(std::move(rule));

    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 1);
    // Only PlaySound should have executed; Delay stops further processing
    EXPECT_EQ(sys.actionLog.size(), size_t(1));
    EXPECT_EQ(static_cast<int>(sys.actionLog[0].type), static_cast<int>(ActionType::PlaySound));
}

TEST(EventResponseSystem_GlobalRuleMatchesAnyEntity)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule;
    rule.name = "Global";
    rule.trigger = EventTriggerType::OnDamaged;
    rule.sourceEntityId = 0; // Global
    rule.actions.push_back({ActionType::ShowMessage, {std::string("Someone hurt")}});
    sys.AddRule(std::move(rule));

    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 1);
    EXPECT_EQ(sys.totalFired, uint32_t(1));

    sys.EvaluateRules(EventTriggerType::OnDamaged, "", 999);
    EXPECT_EQ(sys.totalFired, uint32_t(2));
}

TEST(EventResponseSystem_MultipleRulesSameEvent)
{
    using namespace TestERS;
    SimpleEventResponseSystem sys;

    EventResponseRule rule1;
    rule1.name = "Rule A";
    rule1.trigger = EventTriggerType::OnKilled;
    rule1.actions.push_back({ActionType::PlaySound, {std::string("a.wav")}});
    sys.AddRule(std::move(rule1));

    EventResponseRule rule2;
    rule2.name = "Rule B";
    rule2.trigger = EventTriggerType::OnKilled;
    rule2.actions.push_back({ActionType::SpawnEntity, {std::string("B")}});
    sys.AddRule(std::move(rule2));

    sys.EvaluateRules(EventTriggerType::OnKilled, "", 1);
    EXPECT_EQ(sys.totalFired, uint32_t(2));
    EXPECT_EQ(sys.actionLog.size(), size_t(2));
}

TEST(EventResponseSystem_ActionParamVariants)
{
    using namespace TestERS;
    GameplayAction action;
    action.type = ActionType::SetPosition;
    action.params.push_back(1.5);
    action.params.push_back(int64_t(42));
    action.params.push_back(std::string("test"));
    action.params.push_back(uint32_t(100));
    action.params.push_back(std::monostate{});

    EXPECT_EQ(action.params.size(), size_t(5));
    EXPECT_TRUE(std::holds_alternative<double>(action.params[0]));
    EXPECT_TRUE(std::holds_alternative<int64_t>(action.params[1]));
    EXPECT_TRUE(std::holds_alternative<std::string>(action.params[2]));
    EXPECT_TRUE(std::holds_alternative<uint32_t>(action.params[3]));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(action.params[4]));
}

TEST(EventResponseSystem_TriggerTypeEnumCoverage)
{
    using namespace TestERS;
    EXPECT_EQ(static_cast<int>(EventTriggerType::Count), 16);
}

TEST(EventResponseSystem_ActionTypeEnumCoverage)
{
    using namespace TestERS;
    EXPECT_EQ(static_cast<int>(ActionType::Count), 24);
}
