// Test_gameplay_response.cpp — regression test for DynamicResponseSystem recursion guard.
//
// A SendSignal action re-enters SendSignal, so two mutually-triggering rules recursed
// forever and overflowed the stack (the default 0s cooldown never stopped it). The fix
// adds a bounded depth guard. Before the fix this test crashes the process; after it, the
// recursion unwinds cleanly and control returns.

#include "../TestFramework.h"
#include "Engine/Dialogue/DynamicResponseSystem.h"

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
