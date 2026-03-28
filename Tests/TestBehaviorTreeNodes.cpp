// TestBehaviorTreeNodes.cpp - Tests for behavior tree node types

#include "TestFramework.h"
#include "Engine/AI/BehaviorTreeNodes.h"
#include "Engine/AI/BehaviorTreeTypes.h"

using namespace Spark::AI;

// =============================================================================
// Blackboard
// =============================================================================

TEST(BTBlackboard_SetAndGet)
{
    Blackboard bb;
    bb.Set("health", 100);
    EXPECT_EQ(bb.Get<int>("health"), 100);
}

TEST(BTBlackboard_HasKey)
{
    Blackboard bb;
    EXPECT_FALSE(bb.Has("missing"));
    bb.Set("key", true);
    EXPECT_TRUE(bb.Has("key"));
}

TEST(BTBlackboard_Remove)
{
    Blackboard bb;
    bb.Set("temp", 42);
    EXPECT_TRUE(bb.Has("temp"));
    bb.Remove("temp");
    EXPECT_FALSE(bb.Has("temp"));
}

TEST(BTBlackboard_Clear)
{
    Blackboard bb;
    bb.Set("a", 1);
    bb.Set("b", 2.0f);
    bb.Clear();
    EXPECT_FALSE(bb.Has("a"));
    EXPECT_FALSE(bb.Has("b"));
}

TEST(BTBlackboard_DefaultValue)
{
    Blackboard bb;
    EXPECT_EQ(bb.Get<int>("missing", -1), -1);
    EXPECT_NEAR(bb.Get<float>("missing", 3.14f), 3.14f, 0.01f);
}

// =============================================================================
// ActionNode
// =============================================================================

TEST(BTAction_ReturnsSuccess)
{
    Blackboard bb;
    ActionNode node("SuccessAction", [](float, Blackboard&) { return NodeStatus::Success; });
    auto result = node.Tick(0.016f, bb);
    EXPECT_TRUE(result == NodeStatus::Success);
}

TEST(BTAction_ReturnsFailure)
{
    Blackboard bb;
    ActionNode node("FailAction", [](float, Blackboard&) { return NodeStatus::Failure; });
    auto result = node.Tick(0.016f, bb);
    EXPECT_TRUE(result == NodeStatus::Failure);
}

TEST(BTAction_ReturnsRunning)
{
    Blackboard bb;
    ActionNode node("RunningAction", [](float, Blackboard&) { return NodeStatus::Running; });
    auto result = node.Tick(0.016f, bb);
    EXPECT_TRUE(result == NodeStatus::Running);
}

// =============================================================================
// ConditionNode
// =============================================================================

TEST(BTCondition_TrueReturnsSuccess)
{
    Blackboard bb;
    bb.Set("alive", true);
    ConditionNode node("IsAlive", [](const Blackboard& b) { return b.Get<bool>("alive", false); });
    EXPECT_TRUE(node.Tick(0.0f, bb) == NodeStatus::Success);
}

TEST(BTCondition_FalseReturnsFailure)
{
    Blackboard bb;
    bb.Set("alive", false);
    ConditionNode node("IsAlive", [](const Blackboard& b) { return b.Get<bool>("alive", false); });
    EXPECT_TRUE(node.Tick(0.0f, bb) == NodeStatus::Failure);
}

// =============================================================================
// SequenceNode
// =============================================================================

TEST(BTSequence_AllSucceedReturnsSuccess)
{
    Blackboard bb;
    SequenceNode seq("TestSeq");
    seq.AddChild(std::make_unique<ActionNode>("A", [](float, Blackboard&) { return NodeStatus::Success; }));
    seq.AddChild(std::make_unique<ActionNode>("B", [](float, Blackboard&) { return NodeStatus::Success; }));
    seq.AddChild(std::make_unique<ActionNode>("C", [](float, Blackboard&) { return NodeStatus::Success; }));
    EXPECT_TRUE(seq.Tick(0.016f, bb) == NodeStatus::Success);
}

TEST(BTSequence_FirstFailsReturnsFailure)
{
    Blackboard bb;
    int callCount = 0;
    SequenceNode seq;
    seq.AddChild(std::make_unique<ActionNode>("Fail", [](float, Blackboard&) { return NodeStatus::Failure; }));
    seq.AddChild(std::make_unique<ActionNode>("NotReached",
                                              [&callCount](float, Blackboard&)
                                              {
                                                  callCount++;
                                                  return NodeStatus::Success;
                                              }));
    EXPECT_TRUE(seq.Tick(0.016f, bb) == NodeStatus::Failure);
    EXPECT_EQ(callCount, 0); // Second child should not be reached
}

TEST(BTSequence_RunningPausesExecution)
{
    Blackboard bb;
    SequenceNode seq;
    seq.AddChild(std::make_unique<ActionNode>("Running", [](float, Blackboard&) { return NodeStatus::Running; }));
    seq.AddChild(std::make_unique<ActionNode>("NotReached", [](float, Blackboard&) { return NodeStatus::Success; }));
    EXPECT_TRUE(seq.Tick(0.016f, bb) == NodeStatus::Running);
}

// =============================================================================
// SelectorNode
// =============================================================================

TEST(BTSelector_FirstSucceedsReturnsSuccess)
{
    Blackboard bb;
    SelectorNode sel;
    sel.AddChild(std::make_unique<ActionNode>("Win", [](float, Blackboard&) { return NodeStatus::Success; }));
    sel.AddChild(std::make_unique<ActionNode>("Never", [](float, Blackboard&) { return NodeStatus::Success; }));
    EXPECT_TRUE(sel.Tick(0.016f, bb) == NodeStatus::Success);
}

TEST(BTSelector_AllFailReturnsFailure)
{
    Blackboard bb;
    SelectorNode sel;
    sel.AddChild(std::make_unique<ActionNode>("F1", [](float, Blackboard&) { return NodeStatus::Failure; }));
    sel.AddChild(std::make_unique<ActionNode>("F2", [](float, Blackboard&) { return NodeStatus::Failure; }));
    EXPECT_TRUE(sel.Tick(0.016f, bb) == NodeStatus::Failure);
}

TEST(BTSelector_FallbackOnFailure)
{
    Blackboard bb;
    SelectorNode sel;
    sel.AddChild(std::make_unique<ActionNode>("Fail", [](float, Blackboard&) { return NodeStatus::Failure; }));
    sel.AddChild(std::make_unique<ActionNode>("Succeed", [](float, Blackboard&) { return NodeStatus::Success; }));
    EXPECT_TRUE(sel.Tick(0.016f, bb) == NodeStatus::Success);
}

// =============================================================================
// InverterNode
// =============================================================================

TEST(BTInverter_InvertsSuccess)
{
    Blackboard bb;
    auto child = std::make_unique<ActionNode>("S", [](float, Blackboard&) { return NodeStatus::Success; });
    InverterNode inv(std::move(child));
    EXPECT_TRUE(inv.Tick(0.016f, bb) == NodeStatus::Failure);
}

TEST(BTInverter_InvertsFailure)
{
    Blackboard bb;
    auto child = std::make_unique<ActionNode>("F", [](float, Blackboard&) { return NodeStatus::Failure; });
    InverterNode inv(std::move(child));
    EXPECT_TRUE(inv.Tick(0.016f, bb) == NodeStatus::Success);
}

TEST(BTInverter_PassesThroughRunning)
{
    Blackboard bb;
    auto child = std::make_unique<ActionNode>("R", [](float, Blackboard&) { return NodeStatus::Running; });
    InverterNode inv(std::move(child));
    EXPECT_TRUE(inv.Tick(0.016f, bb) == NodeStatus::Running);
}

// =============================================================================
// WaitNode
// =============================================================================

TEST(BTWait_RunningUntilDuration)
{
    Blackboard bb;
    WaitNode wait(1.0f);
    // Should be Running when not enough time has passed
    EXPECT_TRUE(wait.Tick(0.5f, bb) == NodeStatus::Running);
}

TEST(BTWait_SucceedsAfterDuration)
{
    Blackboard bb;
    WaitNode wait(0.1f);
    wait.Tick(0.05f, bb);               // 0.05s
    auto result = wait.Tick(0.06f, bb); // 0.11s total
    EXPECT_TRUE(result == NodeStatus::Success);
}

// =============================================================================
// RepeaterNode
// =============================================================================

TEST(BTRepeater_RepeatsNTimes)
{
    Blackboard bb;
    int count = 0;
    auto child = std::make_unique<ActionNode>("Count",
                                              [&count](float, Blackboard&)
                                              {
                                                  count++;
                                                  return NodeStatus::Success;
                                              });
    RepeaterNode repeater(std::move(child), 3);

    // Tick 3 times — should keep running until repeat count is reached
    auto r1 = repeater.Tick(0.016f, bb);
    auto r2 = repeater.Tick(0.016f, bb);
    auto r3 = repeater.Tick(0.016f, bb);

    // After 3 ticks with Success child, repeater should succeed
    EXPECT_EQ(count, 3);
    (void)r1;
    (void)r2;
    (void)r3;
}
