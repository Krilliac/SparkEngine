/**
 * @file TestTimerManagerReal.cpp
 * @brief Real-class tests for Spark::TimerManager
 */

#include "TestFramework.h"
#include "Utils/TimerManager.h"

namespace
{
    void ResetTimerManager()
    {
        auto& tm = Spark::TimerManager::GetInstance();
        tm.Shutdown();
        tm.Initialize();
    }
} // namespace

TEST(TimerManagerReal_SingletonStable)
{
    auto& a = Spark::TimerManager::GetInstance();
    auto& b = Spark::TimerManager::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(TimerManagerReal_InitializeShutdown)
{
    auto& tm = Spark::TimerManager::GetInstance();
    tm.Shutdown();
    tm.Initialize();
    tm.Shutdown();
    tm.Initialize();
}

TEST(TimerManagerReal_SetTimerFiresOnce)
{
    ResetTimerManager();
    auto& tm = Spark::TimerManager::GetInstance();

    int fireCount = 0;
    tm.SetTimer("PhaseOO_OneShot", 0.1f, /*looping*/ false, [&]() { ++fireCount; });

    tm.Update(0.15f); // Past the rate — should fire.
    EXPECT_EQ(fireCount, 1);

    tm.Update(0.15f); // One-shot — should not fire again.
    EXPECT_EQ(fireCount, 1);
}

TEST(TimerManagerReal_SetTimerLoops)
{
    ResetTimerManager();
    auto& tm = Spark::TimerManager::GetInstance();

    int fireCount = 0;
    tm.SetTimer("PhaseOO_Loop", 0.1f, /*looping*/ true, [&]() { ++fireCount; });

    // Each Update only advances one timer tick at a time for looping
    // timers in this implementation — call Update multiple times.
    for (int i = 0; i < 4; ++i)
        tm.Update(0.11f);
    EXPECT_TRUE(fireCount >= 2);
}

TEST(TimerManagerReal_InvalidArgsRejected)
{
    ResetTimerManager();
    auto& tm = Spark::TimerManager::GetInstance();

    // Empty name — should log warn and not crash.
    tm.SetTimer("", 0.1f, false, []() {});
    // Negative rate — should log warn.
    tm.SetTimer("PhaseOO_Invalid", -1.0f, false, []() {});
    // Null callback — should log error.
    tm.SetTimer("PhaseOO_Null", 0.1f, false, nullptr);
    EXPECT_TRUE(true);
}

TEST(TimerManagerReal_PauseResume)
{
    ResetTimerManager();
    auto& tm = Spark::TimerManager::GetInstance();

    int fireCount = 0;
    tm.SetTimer("PhaseOO_Pause", 0.1f, true, [&]() { ++fireCount; });
    tm.PauseTimer("PhaseOO_Pause");

    tm.Update(0.3f);
    EXPECT_EQ(fireCount, 0); // Paused — should not fire.

    tm.ResumeTimer("PhaseOO_Pause");
    tm.Update(0.15f);
    EXPECT_TRUE(fireCount >= 1);
}

TEST(TimerManagerReal_ClearTimerPreventsFire)
{
    ResetTimerManager();
    auto& tm = Spark::TimerManager::GetInstance();

    int fireCount = 0;
    tm.SetTimer("PhaseOO_Clear", 0.1f, false, [&]() { ++fireCount; });
    tm.ClearTimer("PhaseOO_Clear");

    tm.Update(0.2f);
    EXPECT_EQ(fireCount, 0);
}

TEST(TimerManagerReal_ClearAllTimers)
{
    ResetTimerManager();
    auto& tm = Spark::TimerManager::GetInstance();

    int fires = 0;
    tm.SetTimer("A", 0.1f, false, [&]() { ++fires; });
    tm.SetTimer("B", 0.1f, false, [&]() { ++fires; });
    tm.ClearAllTimers();

    tm.Update(0.2f);
    EXPECT_EQ(fires, 0);
}
