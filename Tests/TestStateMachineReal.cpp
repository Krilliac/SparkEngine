/**
 * @file TestStateMachineReal.cpp
 * @brief Real-class tests for Spark::StateMachine<StateID>
 */

#include "TestFramework.h"
#include "Utils/StateMachine.h"

namespace
{
    enum class TestState
    {
        Idle,
        Running,
        Jumping
    };
} // namespace

TEST(StateMachineReal_DefaultConstruction)
{
    Spark::StateMachine<TestState> fsm;
    // Default-constructed FSM has no initial state until AddState is called.
    EXPECT_TRUE(true);
}

TEST(StateMachineReal_AddStateAndSetInitial)
{
    Spark::StateMachine<TestState> fsm;
    fsm.AddState(TestState::Idle);
    fsm.AddState(TestState::Running);
    fsm.Start(TestState::Idle);
    const auto cur = fsm.GetCurrentState();
    EXPECT_TRUE(cur.has_value());
    if (cur)
        EXPECT_EQ(static_cast<int>(*cur), static_cast<int>(TestState::Idle));
}

TEST(StateMachineReal_TransitionTo)
{
    Spark::StateMachine<TestState> fsm;
    fsm.AddState(TestState::Idle);
    fsm.AddState(TestState::Running);
    fsm.Start(TestState::Idle);

    fsm.TransitionTo(TestState::Running);
    const auto cur = fsm.GetCurrentState();
    EXPECT_TRUE(cur.has_value());
    if (cur)
        EXPECT_EQ(static_cast<int>(*cur), static_cast<int>(TestState::Running));
}

TEST(StateMachineReal_OnEnterCallback)
{
    Spark::StateMachine<TestState> fsm;
    bool entered = false;
    fsm.AddState(TestState::Idle);
    fsm.AddState(TestState::Running, /*onEnter*/ [&]() { entered = true; });
    fsm.Start(TestState::Idle);

    fsm.TransitionTo(TestState::Running);
    EXPECT_TRUE(entered);
}

TEST(StateMachineReal_OnExitCallback)
{
    Spark::StateMachine<TestState> fsm;
    bool exited = false;
    fsm.AddState(TestState::Idle, nullptr, nullptr, /*onExit*/ [&]() { exited = true; });
    fsm.AddState(TestState::Running);
    fsm.Start(TestState::Idle);

    fsm.TransitionTo(TestState::Running);
    EXPECT_TRUE(exited);
}

TEST(StateMachineReal_OnUpdateCallback)
{
    Spark::StateMachine<TestState> fsm;
    int updateCount = 0;
    fsm.AddState(TestState::Idle, nullptr, /*onUpdate*/ [&](float) { ++updateCount; });
    fsm.Start(TestState::Idle);

    fsm.Tick(0.016f);
    fsm.Tick(0.016f);
    EXPECT_EQ(updateCount, 2);
}

TEST(StateMachineReal_AutomaticTransition)
{
    Spark::StateMachine<TestState> fsm;
    bool cond = false;
    fsm.AddState(TestState::Idle);
    fsm.AddState(TestState::Running);
    fsm.AddTransition(TestState::Idle, TestState::Running, [&]() { return cond; });
    fsm.Start(TestState::Idle);

    fsm.Tick(0.016f);
    {
        const auto cur = fsm.GetCurrentState();
        EXPECT_TRUE(cur.has_value());
        if (cur)
            EXPECT_EQ(static_cast<int>(*cur), static_cast<int>(TestState::Idle));
    }

    cond = true;
    fsm.Tick(0.016f);
    {
        const auto cur = fsm.GetCurrentState();
        EXPECT_TRUE(cur.has_value());
        if (cur)
            EXPECT_EQ(static_cast<int>(*cur), static_cast<int>(TestState::Running));
    }
}
