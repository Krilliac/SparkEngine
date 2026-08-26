// Test_gameplay_response.cpp — regression test for DynamicResponseSystem recursion guard.
//
// A SendSignal action re-enters SendSignal, so two mutually-triggering rules recursed
// forever and overflowed the stack (the default 0s cooldown never stopped it). The fix
// adds a bounded depth guard. Before the fix this test crashes the process; after it, the
// recursion unwinds cleanly and control returns.

#include "../TestFramework.h"
#include "Engine/Dialogue/DynamicResponseSystem.h"

#include <utility>

using namespace Spark::Dialogue;

TEST(DynamicResponse_MutualRecursion_DoesNotStackOverflow)
{
    auto& drs = DynamicResponseSystem::GetInstance();
    drs.Initialize();

    ResponseRule ruleA;
    ruleA.signalName = "sigA";
    {
        ResponseAction act;
        act.type = ResponseAction::Type::SendSignal;
        act.stringParam = "sigB";
        ruleA.actions.push_back(act);
    }
    drs.RegisterRule(ruleA);

    ResponseRule ruleB;
    ruleB.signalName = "sigB";
    {
        ResponseAction act;
        act.type = ResponseAction::Type::SendSignal;
        act.stringParam = "sigA";
        ruleB.actions.push_back(act);
    }
    drs.RegisterRule(ruleB);

    drs.SendSignal("sigA", 1);

    EXPECT_TRUE(true); // reaching this line means the depth guard prevented an overflow
}

TEST(DynamicResponse_CustomActionMayRegisterRuleWithoutInvalidatingDispatch)
{
    auto& drs = DynamicResponseSystem::GetInstance();
    drs.Initialize();

    ResponseRule rule;
    rule.signalName = "mutate";

    ResponseAction mutate;
    mutate.type = ResponseAction::Type::Custom;
    mutate.customAction = [&drs](uint32_t)
    {
        // Force m_rules to grow while the selected rule is dispatching. The
        // dispatcher must not retain a pointer/reference into that vector.
        for (int i = 0; i < 128; ++i)
        {
            ResponseRule added;
            added.signalName = "added-" + std::to_string(i);
            drs.RegisterRule(added);
        }
    };
    rule.actions.push_back(std::move(mutate));

    ResponseAction continuation;
    continuation.type = ResponseAction::Type::SetVariable;
    continuation.stringParam = "continued";
    continuation.floatParam = 1.0f;
    rule.actions.push_back(std::move(continuation));
    drs.RegisterRule(rule);

    drs.SendSignal("mutate", 7);

    EXPECT_EQ(drs.GetVariable("continued"), 1.0f);
    EXPECT_EQ(drs.GetRuleCount(), size_t{129});
    drs.Shutdown();
}

TEST(DynamicResponse_CustomActionMayShutdownWithoutUsingClearedStorage)
{
    auto& drs = DynamicResponseSystem::GetInstance();
    drs.Initialize();

    bool actionAfterShutdownRan = false;
    ResponseRule rule;
    rule.signalName = "shutdown";

    ResponseAction shutdown;
    shutdown.type = ResponseAction::Type::Custom;
    shutdown.customAction = [&drs](uint32_t) { drs.Shutdown(); };
    rule.actions.push_back(std::move(shutdown));

    ResponseAction shouldNotRun;
    shouldNotRun.type = ResponseAction::Type::Custom;
    shouldNotRun.customAction = [&actionAfterShutdownRan](uint32_t) { actionAfterShutdownRan = true; };
    rule.actions.push_back(std::move(shouldNotRun));
    drs.RegisterRule(rule);

    drs.SendSignal("shutdown", 9);

    EXPECT_FALSE(actionAfterShutdownRan);
    EXPECT_EQ(drs.GetRuleCount(), size_t{0});
}
