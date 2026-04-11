/**
 * @file TestEventResponseSystemPhaseEE.cpp
 * @brief Phase EE Theme 3D tests for Spark::Gameplay::EventResponseSystem
 *
 * The existing Tests/TestEventResponseSystem.cpp is fake-coverage.
 * Phase EE ships real-class tests and wires Initialize / Update /
 * Shutdown into GameplayLifecycleShared.cpp.
 */

#include "TestFramework.h"
#include "Engine/Gameplay/EventResponseSystem.h"

namespace
{
    void ResetERS()
    {
        auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();
        ers.Shutdown();
        ers.Initialize();
        ers.ClearRules();
    }
} // namespace

TEST(EventResponseSystemPhaseEE_SingletonStable)
{
    auto& a = Spark::Gameplay::EventResponseSystem::GetInstance();
    auto& b = Spark::Gameplay::EventResponseSystem::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(EventResponseSystemPhaseEE_InitializeShutdown)
{
    auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();
    ers.Shutdown();
    ers.Initialize();
    ers.Shutdown();
    ers.Initialize();
}

TEST(EventResponseSystemPhaseEE_AddRuleIncreasesCount)
{
    ResetERS();
    auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();

    Spark::Gameplay::EventResponseRule rule;
    rule.name = "TestRule";
    rule.trigger = Spark::Gameplay::EventTriggerType::OnStart;
    rule.enabled = true;
    ers.AddRule(std::move(rule));

    EXPECT_EQ(ers.GetRuleCount(), static_cast<uint32_t>(1));
}

TEST(EventResponseSystemPhaseEE_RemoveRule)
{
    ResetERS();
    auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();

    Spark::Gameplay::EventResponseRule rule;
    rule.name = "Removable";
    rule.trigger = Spark::Gameplay::EventTriggerType::OnCustom;
    ers.AddRule(std::move(rule));
    EXPECT_EQ(ers.GetRuleCount(), static_cast<uint32_t>(1));

    ers.RemoveRule("Removable");
    EXPECT_EQ(ers.GetRuleCount(), static_cast<uint32_t>(0));
}

TEST(EventResponseSystemPhaseEE_SetRuleEnabledToggle)
{
    ResetERS();
    auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();

    Spark::Gameplay::EventResponseRule rule;
    rule.name = "Toggle";
    rule.trigger = Spark::Gameplay::EventTriggerType::OnStart;
    rule.enabled = true;
    ers.AddRule(std::move(rule));

    ers.SetRuleEnabled("Toggle", false);
    const auto& rules = ers.GetRules();
    EXPECT_EQ(rules.size(), static_cast<size_t>(1));
    EXPECT_FALSE(rules[0].enabled);

    ers.SetRuleEnabled("Toggle", true);
    EXPECT_TRUE(ers.GetRules()[0].enabled);
}

TEST(EventResponseSystemPhaseEE_ClearRules)
{
    ResetERS();
    auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();

    for (int i = 0; i < 5; ++i)
    {
        Spark::Gameplay::EventResponseRule rule;
        rule.name = "Rule_" + std::to_string(i);
        rule.trigger = Spark::Gameplay::EventTriggerType::OnStart;
        ers.AddRule(std::move(rule));
    }
    EXPECT_EQ(ers.GetRuleCount(), static_cast<uint32_t>(5));

    ers.ClearRules();
    EXPECT_EQ(ers.GetRuleCount(), static_cast<uint32_t>(0));
}

TEST(EventResponseSystemPhaseEE_UpdateWithNoRulesIsSafe)
{
    ResetERS();
    auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();
    ers.Update(0.016f);
    ers.Update(0.016f);
    EXPECT_EQ(ers.GetRuleCount(), static_cast<uint32_t>(0));
}

TEST(EventResponseSystemPhaseEE_RemoveUnknownRuleIsSafe)
{
    ResetERS();
    auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();
    ers.RemoveRule("NonexistentRule");
    EXPECT_EQ(ers.GetRuleCount(), static_cast<uint32_t>(0));
}
