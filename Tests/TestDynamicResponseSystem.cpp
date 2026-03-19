// TestDynamicResponseSystem.cpp - Tests for variable storage, rule registration, and signal dispatch
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace TestDRS
{

    using EntityID = uint32_t;
    using VariableValue = std::variant<int, float, bool, std::string>;

    struct Condition
    {
        std::string variableName;
        VariableValue expectedValue;
    };

    struct ResponseRule
    {
        std::string signalName;
        std::vector<Condition> conditions;
        std::string responseAction;
        int priority = 0;
    };

    struct DynamicResponseSystem
    {
        // Per-entity variable storage
        std::unordered_map<EntityID, std::unordered_map<std::string, VariableValue>> variables;
        std::vector<ResponseRule> rules;
        std::vector<std::string> firedResponses;

        void SetVariable(EntityID entity, const std::string& name, VariableValue value)
        {
            variables[entity][name] = std::move(value);
        }

        VariableValue GetVariable(EntityID entity, const std::string& name) const
        {
            auto eit = variables.find(entity);
            if (eit == variables.end())
                return 0;
            auto vit = eit->second.find(name);
            if (vit == eit->second.end())
                return 0;
            return vit->second;
        }

        void RegisterRule(const ResponseRule& rule) { rules.push_back(rule); }

        void SendSignal(const std::string& signalName, EntityID entity)
        {
            firedResponses.clear();
            const ResponseRule* bestRule = nullptr;
            for (const auto& rule : rules)
            {
                if (rule.signalName != signalName)
                    continue;

                bool allMatch = true;
                for (const auto& cond : rule.conditions)
                {
                    auto val = GetVariable(entity, cond.variableName);
                    if (val != cond.expectedValue)
                    {
                        allMatch = false;
                        break;
                    }
                }

                if (allMatch && (!bestRule || rule.priority > bestRule->priority))
                    bestRule = &rule;
            }

            if (bestRule)
                firedResponses.push_back(bestRule->responseAction);
        }
    };

} // namespace TestDRS

// ============================================================================
// Variable Storage
// ============================================================================

TEST(DRS_SetAndGetVariable)
{
    TestDRS::DynamicResponseSystem sys;
    sys.SetVariable(1, "health", 75);
    sys.SetVariable(1, "isAlerted", true);

    auto health = std::get<int>(sys.GetVariable(1, "health"));
    EXPECT_EQ(health, 75);

    auto alerted = std::get<bool>(sys.GetVariable(1, "isAlerted"));
    EXPECT_TRUE(alerted);
}

TEST(DRS_GetVariable_Missing)
{
    TestDRS::DynamicResponseSystem sys;
    auto val = sys.GetVariable(99, "nonexistent");
    EXPECT_EQ(std::get<int>(val), 0);
}

// ============================================================================
// Rule Registration
// ============================================================================

TEST(DRS_RegisterRule)
{
    TestDRS::DynamicResponseSystem sys;
    TestDRS::ResponseRule rule;
    rule.signalName = "OnSeeEnemy";
    rule.responseAction = "Shout_Alarm";
    rule.priority = 5;
    sys.RegisterRule(rule);
    EXPECT_EQ(static_cast<int>(sys.rules.size()), 1);
}

// ============================================================================
// Signal Dispatch with Conditions
// ============================================================================

TEST(DRS_SendSignal_ConditionMet)
{
    TestDRS::DynamicResponseSystem sys;
    sys.SetVariable(1, "mood", std::string("angry"));

    TestDRS::ResponseRule rule;
    rule.signalName = "OnGreet";
    rule.conditions = {{"mood", TestDRS::VariableValue(std::string("angry"))}};
    rule.responseAction = "Response_Growl";
    rule.priority = 1;
    sys.RegisterRule(rule);

    sys.SendSignal("OnGreet", 1);
    EXPECT_EQ(static_cast<int>(sys.firedResponses.size()), 1);
    EXPECT_EQ(sys.firedResponses[0], std::string("Response_Growl"));
}

TEST(DRS_SendSignal_ConditionNotMet)
{
    TestDRS::DynamicResponseSystem sys;
    sys.SetVariable(1, "mood", std::string("happy"));

    TestDRS::ResponseRule rule;
    rule.signalName = "OnGreet";
    rule.conditions = {{"mood", TestDRS::VariableValue(std::string("angry"))}};
    rule.responseAction = "Response_Growl";
    sys.RegisterRule(rule);

    sys.SendSignal("OnGreet", 1);
    EXPECT_EQ(static_cast<int>(sys.firedResponses.size()), 0);
}

TEST(DRS_SendSignal_PrioritySelection)
{
    TestDRS::DynamicResponseSystem sys;
    sys.SetVariable(1, "rank", 5);

    TestDRS::ResponseRule lowPri;
    lowPri.signalName = "OnCommand";
    lowPri.conditions = {{"rank", TestDRS::VariableValue(5)}};
    lowPri.responseAction = "Response_Acknowledge";
    lowPri.priority = 1;

    TestDRS::ResponseRule highPri;
    highPri.signalName = "OnCommand";
    highPri.conditions = {{"rank", TestDRS::VariableValue(5)}};
    highPri.responseAction = "Response_SirYesSir";
    highPri.priority = 10;

    sys.RegisterRule(lowPri);
    sys.RegisterRule(highPri);

    sys.SendSignal("OnCommand", 1);
    EXPECT_EQ(static_cast<int>(sys.firedResponses.size()), 1);
    EXPECT_EQ(sys.firedResponses[0], std::string("Response_SirYesSir"));
}
