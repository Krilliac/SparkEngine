/**
 * @file TestTimerReal.cpp
 * @brief Real-class tests for ::Timer
 */

#include "TestFramework.h"
#include "Utils/Timer.h"

#include <chrono>
#include <thread>

TEST(TimerReal_DefaultConstructionRunning)
{
    // Default constructor: m_paused = false (running) with totalTime = 0.
    // The doc comment says "stopped state" but the impl disagrees.
    Timer t;
    EXPECT_FALSE(t.IsPaused());
    EXPECT_NEAR(t.GetTotalTime(), 0.0f, 0.001f);
}

TEST(TimerReal_StartUnpauses)
{
    Timer t;
    t.Start();
    EXPECT_FALSE(t.IsPaused());
}

TEST(TimerReal_StopPauses)
{
    Timer t;
    t.Start();
    EXPECT_FALSE(t.IsPaused());
    t.Stop();
    EXPECT_TRUE(t.IsPaused());
}

TEST(TimerReal_ResetClearsTotalTime)
{
    Timer t;
    t.Start();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    t.GetDeltaTime();
    t.Reset();
    // Reset brings total time back to 0 and leaves the running state
    // alone (m_paused stays false so Reset() + GetDeltaTime() works
    // without a follow-up Start()).
    EXPECT_NEAR(t.GetTotalTime(), 0.0f, 0.001f);
}

TEST(TimerReal_GetDeltaTimeWhenStopped)
{
    Timer t;
    // Not started — delta should be 0.
    float dt = t.GetDeltaTime();
    EXPECT_NEAR(dt, 0.0f, 0.001f);
}

TEST(TimerReal_GetDeltaTimeAfterStart)
{
    Timer t;
    t.Start();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    float dt = t.GetDeltaTime();
    EXPECT_TRUE(dt >= 0.0f);
}
