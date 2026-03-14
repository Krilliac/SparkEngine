// TestStateMachine.cpp - Tests for Spark::StateMachine
#include "TestFramework.h"
#include "Utils/StateMachine.h"

#include <stdexcept>
#include <string>
#include <vector>

enum class State
{
    Idle,
    Running,
    Jumping,
    Dead
};

// ============================================================================
// Basic lifecycle
// ============================================================================

TEST(StateMachine_Start_SetsCurrentState)
{
    Spark::StateMachine<State> fsm;
    fsm.AddState(State::Idle);
    fsm.Start(State::Idle);
    EXPECT_TRUE(fsm.IsInState(State::Idle));
    EXPECT_TRUE(fsm.IsRunning());
}

TEST(StateMachine_Start_CallsOnEnter)
{
    Spark::StateMachine<State> fsm;
    int enterCount = 0;
    fsm.AddState(State::Idle, [&] { ++enterCount; });
    fsm.Start(State::Idle);
    EXPECT_EQ(enterCount, 1);
}

TEST(StateMachine_Stop_CallsOnExit)
{
    Spark::StateMachine<State> fsm;
    int exitCount = 0;
    fsm.AddState(State::Idle, nullptr, nullptr, [&] { ++exitCount; });
    fsm.Start(State::Idle);
    fsm.Stop();
    EXPECT_EQ(exitCount, 1);
    EXPECT_FALSE(fsm.IsRunning());
}

TEST(StateMachine_Start_UnknownState_Throws)
{
    Spark::StateMachine<State> fsm;
    EXPECT_THROW(fsm.Start(State::Idle), std::invalid_argument);
}

TEST(StateMachine_GetCurrentState_NotStarted)
{
    Spark::StateMachine<State> fsm;
    EXPECT_FALSE(fsm.GetCurrentState().has_value());
}

// ============================================================================
// Tick with update callback
// ============================================================================

TEST(StateMachine_Tick_CallsOnUpdate)
{
    Spark::StateMachine<State> fsm;
    float accumulated = 0.0f;
    fsm.AddState(State::Running, nullptr, [&](float dt) { accumulated += dt; });
    fsm.Start(State::Running);
    fsm.Tick(0.016f);
    fsm.Tick(0.016f);
    EXPECT_NEAR(accumulated, 0.032f, 0.0001f);
}

TEST(StateMachine_Tick_NotRunning_IsNoop)
{
    Spark::StateMachine<State> fsm;
    int count = 0;
    fsm.AddState(State::Idle, nullptr, [&](float) { ++count; });
    // Never started — Tick should be a no-op
    fsm.Tick(0.016f);
    EXPECT_EQ(count, 0);
}

// ============================================================================
// Transitions
// ============================================================================

TEST(StateMachine_Transition_AutomaticOnCondition)
{
    Spark::StateMachine<State> fsm;
    bool shouldTransition = false;

    fsm.AddState(State::Idle);
    fsm.AddState(State::Running);
    fsm.AddTransition(State::Idle, State::Running, [&] { return shouldTransition; });
    fsm.Start(State::Idle);

    fsm.Tick(0.016f);
    EXPECT_TRUE(fsm.IsInState(State::Idle));

    shouldTransition = true;
    fsm.Tick(0.016f);
    EXPECT_TRUE(fsm.IsInState(State::Running));
}

TEST(StateMachine_Transition_CallsExitAndEnter)
{
    Spark::StateMachine<State> fsm;
    std::vector<std::string> log;
    bool trigger = false;

    fsm.AddState(State::Idle, [&] { log.push_back("idle_enter"); }, nullptr, [&] { log.push_back("idle_exit"); });
    fsm.AddState(State::Running, [&] { log.push_back("run_enter"); });
    fsm.AddTransition(State::Idle, State::Running, [&] { return trigger; });

    fsm.Start(State::Idle);
    trigger = true;
    fsm.Tick(0.016f);

    EXPECT_EQ(log.size(), 3u);
    EXPECT_TRUE(log[0] == "idle_enter");
    EXPECT_TRUE(log[1] == "idle_exit");
    EXPECT_TRUE(log[2] == "run_enter");
}

TEST(StateMachine_Transition_OnlyOnePerTick)
{
    Spark::StateMachine<State> fsm;
    int transitions = 0;

    fsm.AddState(State::Idle);
    fsm.AddState(State::Running, [&] { ++transitions; });
    fsm.AddState(State::Jumping, [&] { ++transitions; });
    fsm.AddTransition(State::Idle, State::Running, [] { return true; });
    fsm.AddTransition(State::Idle, State::Jumping, [] { return true; });

    fsm.Start(State::Idle);
    fsm.Tick(0.016f);

    EXPECT_EQ(transitions, 1); // Only one transition taken per tick
}

// ============================================================================
// Manual TransitionTo
// ============================================================================

TEST(StateMachine_TransitionTo_Direct)
{
    Spark::StateMachine<State> fsm;
    fsm.AddState(State::Idle);
    fsm.AddState(State::Dead);
    fsm.Start(State::Idle);
    fsm.TransitionTo(State::Dead);
    EXPECT_TRUE(fsm.IsInState(State::Dead));
}

TEST(StateMachine_TransitionTo_Unknown_Throws)
{
    Spark::StateMachine<State> fsm;
    fsm.AddState(State::Idle);
    fsm.Start(State::Idle);
    EXPECT_THROW(fsm.TransitionTo(State::Dead), std::invalid_argument);
}

// ============================================================================
// HasState and Reset
// ============================================================================

TEST(StateMachine_HasState)
{
    Spark::StateMachine<State> fsm;
    EXPECT_FALSE(fsm.HasState(State::Idle));
    fsm.AddState(State::Idle);
    EXPECT_TRUE(fsm.HasState(State::Idle));
}

TEST(StateMachine_Reset_ClearsAll)
{
    Spark::StateMachine<State> fsm;
    fsm.AddState(State::Idle);
    fsm.AddState(State::Running);
    fsm.AddTransition(State::Idle, State::Running, [] { return true; });
    fsm.Start(State::Idle);
    fsm.Reset();

    EXPECT_FALSE(fsm.IsRunning());
    EXPECT_FALSE(fsm.GetCurrentState().has_value());
    EXPECT_FALSE(fsm.HasState(State::Idle));
}

TEST(StateMachine_IntStateID)
{
    // StateMachine should work with non-enum state IDs too
    Spark::StateMachine<int> fsm;
    bool trigger = false;
    fsm.AddState(0);
    fsm.AddState(1);
    fsm.AddTransition(0, 1, [&] { return trigger; });
    fsm.Start(0);
    EXPECT_TRUE(fsm.IsInState(0));
    trigger = true;
    fsm.Tick(0.016f);
    EXPECT_TRUE(fsm.IsInState(1));
}

TEST(StateMachine_NullCallbacks_NoCrash)
{
    Spark::StateMachine<State> fsm;
    // All callbacks null — should not crash
    fsm.AddState(State::Idle, nullptr, nullptr, nullptr);
    EXPECT_NO_THROW(fsm.Start(State::Idle));
    EXPECT_NO_THROW(fsm.Tick(0.016f));
    EXPECT_NO_THROW(fsm.Stop());
}
