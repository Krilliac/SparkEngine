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
#include "Utils/JsonUtils.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <variant>
#include <vector>
#include <chrono>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    EXPECT_NO_THROW(ers.Initialize());
    EXPECT_NO_THROW(ers.Shutdown());
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

// ============================================================================
// Rule files (SEC-120 event-response-definitions target)
// ============================================================================

namespace
{
    using Spark::Gameplay::ActionParam;
    using Spark::Gameplay::ActionType;
    using Spark::Gameplay::EventResponseRule;
    using Spark::Gameplay::EventTriggerType;

    std::vector<EventResponseRule> ParseRules(const std::string& json, bool expectAccepted = true)
    {
        std::vector<EventResponseRule> rules;
        EXPECT_EQ(Spark::Gameplay::ParseEventResponseRules(json, rules, "test"), expectAccepted);
        return rules;
    }

    std::string WriteRules(const std::vector<EventResponseRule>& rules)
    {
        std::ostringstream out;
        Spark::Gameplay::WriteEventResponseRules(out, rules);
        return out.str();
    }
} // namespace

TEST(EventResponseSystemPhaseEE_CustomEventCycleIsBounded)
{
    ResetERS();
    auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();

    // An OnCustom "ping" rule whose action fires "ping" again recursed until the stack overflowed.
    EventResponseRule rule;
    rule.name = "Echo";
    rule.trigger = EventTriggerType::OnCustom;
    rule.triggerParam = "ping";
    rule.actions.push_back({ActionType::FireCustomEvent, {std::string("ping")}});
    ers.AddRule(std::move(rule));

    ers.FireCustomEvent("ping");
    EXPECT_EQ(ers.GetTimesFirered(), Spark::Gameplay::EventResponseSystem::kMaxCustomEventDepth);
    ers.ClearRules();
}

TEST(EventResponseRules_ParseKeepsEntityIdsAboveIntMax)
{
    // AsInt() turned 3000000000 into 0, which made this entity-scoped rule global.
    const auto rules = ParseRules(R"({"rules":[{"name":"scoped","sourceEntityId":3000000000,"trigger":"OnDamaged"}]})");
    ASSERT_EQ(rules.size(), size_t(1));
    EXPECT_EQ(rules[0].sourceEntityId, uint32_t(3000000000u));
    EXPECT_TRUE(rules[0].trigger == EventTriggerType::OnDamaged);
}

TEST(EventResponseRules_ParseSkipsRulesWithInvalidEntityIds)
{
    const auto rules = ParseRules(R"({"rules":[
        {"name":"negative","sourceEntityId":-1},
        {"name":"fraction","sourceEntityId":1.5},
        {"name":"too-big","sourceEntityId":4294967296},
        {"name":"text","sourceEntityId":"7"},
        {"name":"kept","sourceEntityId":7},
        {"name":"global"}
    ]})");
    ASSERT_EQ(rules.size(), size_t(2));
    EXPECT_TRUE(rules[0].name == "kept");
    EXPECT_EQ(rules[0].sourceEntityId, uint32_t(7));
    EXPECT_TRUE(rules[1].name == "global");
    EXPECT_EQ(rules[1].sourceEntityId, uint32_t(0));
}

TEST(EventResponseRules_ParseFailureLeavesRulesUntouched)
{
    std::vector<EventResponseRule> rules(1);
    rules[0].name = "kept";
    EXPECT_FALSE(Spark::Gameplay::ParseEventResponseRules(R"({"rules":{}})", rules, "test"));
    EXPECT_FALSE(Spark::Gameplay::ParseEventResponseRules("not json", rules, "test"));
    ASSERT_EQ(rules.size(), size_t(1));
    EXPECT_TRUE(rules[0].name == "kept");
}

TEST(EventResponseRules_WriteEscapesControlCharacters)
{
    std::vector<EventResponseRule> rules(1);
    rules[0].name = std::string("tab\there\rcr\x01one\"q\b");
    rules[0].actions.push_back({ActionType::ShowMessage, {std::string("line\nbreak\x1f")}});
    const std::string written = WriteRules(rules);

    // A raw control character is not JSON, so a strict reader refused the whole file.
    for (const char c : written)
    {
        EXPECT_TRUE(c == '\n' || static_cast<unsigned char>(c) >= 0x20);
    }
    Spark::Json::Value document;
    EXPECT_TRUE(Spark::Json::ParseStrict(written, &document));

    const auto reloaded = ParseRules(written);
    ASSERT_EQ(reloaded.size(), size_t(1));
    EXPECT_TRUE(reloaded[0].name == rules[0].name);
    ASSERT_EQ(reloaded[0].actions.size(), size_t(1));
    EXPECT_TRUE(Spark::Gameplay::ActionParamToString(reloaded[0].actions[0].params[0]) == "line\nbreak\x1f");
}

TEST(EventResponseRules_WriteKeepsDoublePrecision)
{
    std::vector<EventResponseRule> rules(1);
    rules[0].actions.push_back({ActionType::SetPosition, {1234567.0, 0.1, -2.5e-300, 18446744073709551616.0}});
    rules[0].actions.push_back({ActionType::SetHealth, {std::numeric_limits<double>::quiet_NaN()}});

    // The stream's default six significant digits wrote 1234567.0 as 1.23457e+06. 2^64 must
    // not be written as a bare integer token, which Json::Parse reads into an int64.
    const auto reloaded = ParseRules(WriteRules(rules));
    ASSERT_EQ(reloaded.size(), size_t(1));
    ASSERT_EQ(reloaded[0].actions.size(), size_t(2));
    const auto& position = reloaded[0].actions[0].params;
    ASSERT_EQ(position.size(), size_t(4));
    EXPECT_TRUE(Spark::Gameplay::ActionParamToDouble(position[0]) == 1234567.0);
    EXPECT_TRUE(Spark::Gameplay::ActionParamToDouble(position[1]) == 0.1);
    EXPECT_TRUE(Spark::Gameplay::ActionParamToDouble(position[2]) == -2.5e-300);
    EXPECT_TRUE(Spark::Gameplay::ActionParamToDouble(position[3]) == 18446744073709551616.0);
    // NaN has no JSON spelling: it is written as null and reads back as an empty parameter.
    ASSERT_EQ(reloaded[0].actions[1].params.size(), size_t(1));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(reloaded[0].actions[1].params[0]));
}

TEST(EventResponseRules_ActionParamToInt64IsDefinedForEveryValue)
{
    using Spark::Gameplay::ActionParamToInt64;
    // static_cast<int64_t> of these doubles is undefined behaviour; they read as 0.
    EXPECT_EQ(ActionParamToInt64(ActionParam{1.0e300}), int64_t(0));
    EXPECT_EQ(ActionParamToInt64(ActionParam{-1.0e300}), int64_t(0));
    EXPECT_EQ(ActionParamToInt64(ActionParam{std::numeric_limits<double>::quiet_NaN()}), int64_t(0));
    EXPECT_EQ(ActionParamToInt64(ActionParam{9223372036854775808.0}), int64_t(0));
    EXPECT_EQ(ActionParamToInt64(ActionParam{-9223372036854775808.0}), std::numeric_limits<int64_t>::min());
    EXPECT_EQ(ActionParamToInt64(ActionParam{42.9}), int64_t(42));
    EXPECT_EQ(ActionParamToInt64(ActionParam{int64_t(-5)}), int64_t(-5));
    // uint32 is the documented entity-id parameter kind; the old reader returned 0 for it.
    EXPECT_EQ(ActionParamToInt64(ActionParam{uint32_t(100)}), int64_t(100));
    EXPECT_EQ(ActionParamToInt64(ActionParam{std::string("7")}), int64_t(0));
}

TEST(EventResponseSystemPhaseEE_RejectsOversizedRulesFile)
{
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        std::filesystem::temp_directory_path() / ("spark-event-response-oversize-" + std::to_string(suffix) + ".json");
    {
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << std::string(Spark::Json::JsonLimits{}.maxBytes + 1, ' ');
    }

    auto& system = Spark::Gameplay::EventResponseSystem::GetInstance();
    system.Shutdown();
    system.Initialize();
    EventResponseRule existing;
    existing.name = "kept-after-rejection";
    system.AddRule(std::move(existing));
    EXPECT_FALSE(system.LoadFromJson(path.string()));
    EXPECT_EQ(system.GetRuleCount(), uint32_t(1));
    std::filesystem::remove(path);
}

#ifndef _WIN32
TEST(EventResponseSystemPhaseEE_RejectsFifoWithoutBlocking)
{
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        std::filesystem::temp_directory_path() / ("spark-event-response-fifo-" + std::to_string(suffix) + ".json");
    ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);

    auto& system = Spark::Gameplay::EventResponseSystem::GetInstance();
    system.Shutdown();
    system.Initialize();
    EXPECT_FALSE(system.LoadFromJson(path.string()));
    EXPECT_EQ(::unlink(path.c_str()), 0);
}
#endif
