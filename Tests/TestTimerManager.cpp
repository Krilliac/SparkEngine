// TestTimerManager.cpp - Tests for Spark::TimerManager
#include "TestFramework.h"
#include "Utils/TimerManager.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(TimerManager_Initialize)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();
    EXPECT_EQ(static_cast<size_t>(0), mgr.GetTimerCount());
    mgr.Shutdown();
}

// ============================================================================
// One-shot timers
// ============================================================================

TEST(TimerManager_OneShotFires)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    bool fired = false;
    mgr.SetTimer("oneshot", 1.0f, false, [&]() { fired = true; });
    EXPECT_TRUE(mgr.IsTimerActive("oneshot"));

    mgr.Update(0.5f);
    EXPECT_FALSE(fired);

    mgr.Update(0.6f);
    EXPECT_TRUE(fired);
    EXPECT_EQ(static_cast<uint32_t>(1), mgr.GetFireCount("oneshot"));

    // One-shot should be marked for removal after next Update
    mgr.Update(0.0f);
    EXPECT_FALSE(mgr.TimerExists("oneshot"));

    mgr.Shutdown();
}

// ============================================================================
// Looping timers
// ============================================================================

TEST(TimerManager_LoopingTimerFiresMultipleTimes)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    int count = 0;
    mgr.SetTimer("loop", 1.0f, true, [&]() { ++count; });

    mgr.Update(1.1f);
    EXPECT_EQ(1, count);

    mgr.Update(1.1f);
    EXPECT_EQ(2, count);

    mgr.Update(1.1f);
    EXPECT_EQ(3, count);

    mgr.Shutdown();
}

// ============================================================================
// Pause and Resume
// ============================================================================

TEST(TimerManager_PauseResume)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    bool fired = false;
    mgr.SetTimer("pausable", 2.0f, false, [&]() { fired = true; });

    mgr.Update(1.0f);
    EXPECT_FALSE(fired);

    mgr.PauseTimer("pausable");
    EXPECT_TRUE(mgr.IsTimerPaused("pausable"));

    // Time passes but timer is paused
    mgr.Update(5.0f);
    EXPECT_FALSE(fired);

    mgr.ResumeTimer("pausable");
    EXPECT_TRUE(mgr.IsTimerActive("pausable"));

    // Still needs ~1 more second to fire (elapsed was 1.0 when paused)
    mgr.Update(0.5f);
    EXPECT_FALSE(fired);

    mgr.Update(0.6f);
    EXPECT_TRUE(fired);

    mgr.Shutdown();
}

// ============================================================================
// Clear and query
// ============================================================================

TEST(TimerManager_ClearTimer)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    mgr.SetTimer("temp", 10.0f, false, []() {});
    EXPECT_TRUE(mgr.TimerExists("temp"));

    mgr.ClearTimer("temp");
    mgr.Update(0.0f); // Process pending removals
    EXPECT_FALSE(mgr.TimerExists("temp"));

    mgr.Shutdown();
}

TEST(TimerManager_GetRemainingTime)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    mgr.SetTimer("timed", 5.0f, false, []() {});
    mgr.Update(2.0f);

    float remaining = mgr.GetRemainingTime("timed");
    EXPECT_NEAR(3.0f, remaining, 0.01f);

    EXPECT_NEAR(0.0f, mgr.GetRemainingTime("nonexistent"), 0.001f);

    mgr.Shutdown();
}

TEST(TimerManager_FireCount)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    mgr.SetTimer("counter", 0.5f, true, []() {});

    mgr.Update(0.6f);
    mgr.Update(0.6f);
    mgr.Update(0.6f);

    EXPECT_EQ(static_cast<uint32_t>(3), mgr.GetFireCount("counter"));
    EXPECT_EQ(static_cast<uint32_t>(0), mgr.GetFireCount("nonexistent"));

    mgr.Shutdown();
}

TEST(TimerManager_SetTimerRate)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    int count = 0;
    mgr.SetTimer("rate_test", 10.0f, true, [&]() { ++count; });

    mgr.Update(5.0f);
    EXPECT_EQ(0, count);

    // Change rate to fire sooner
    mgr.SetTimerRate("rate_test", 1.0f);
    // After rate change, elapsed is already 5.0 which exceeds new rate of 1.0
    mgr.Update(0.0f);
    // The elapsed (5.0) >= rate (1.0) so it should fire on next real update
    // Actually the rate just changed but elapsed stays; next Update will check
    mgr.Update(0.01f);
    EXPECT_TRUE(count >= 1);

    mgr.Shutdown();
}

TEST(TimerManager_ClearAllTimers)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    mgr.SetTimer("a", 1.0f, false, []() {});
    mgr.SetTimer("b", 2.0f, false, []() {});
    mgr.SetTimer("c", 3.0f, false, []() {});
    EXPECT_EQ(static_cast<size_t>(3), mgr.GetTimerCount());

    mgr.ClearAllTimers();
    EXPECT_EQ(static_cast<size_t>(0), mgr.GetTimerCount());

    mgr.Shutdown();
}

TEST(TimerManager_GetTimerNames)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    mgr.SetTimer("alpha", 1.0f, false, []() {});
    mgr.SetTimer("beta", 1.0f, false, []() {});

    auto names = mgr.GetTimerNames();
    EXPECT_EQ(static_cast<size_t>(2), names.size());

    mgr.Shutdown();
}

TEST(TimerManager_ResetTimer)
{
    auto& mgr = Spark::TimerManager::GetInstance();
    mgr.Initialize();

    bool fired = false;
    mgr.SetTimer("resettable", 2.0f, false, [&]() { fired = true; });
    mgr.Update(1.5f);
    EXPECT_FALSE(fired);

    mgr.ResetTimer("resettable");
    EXPECT_NEAR(2.0f, mgr.GetRemainingTime("resettable"), 0.01f);

    mgr.Update(1.5f);
    EXPECT_FALSE(fired);

    mgr.Update(0.6f);
    EXPECT_TRUE(fired);

    mgr.Shutdown();
}
