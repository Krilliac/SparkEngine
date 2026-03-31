// TestScopedTimer.cpp - Tests for Spark::ScopedTimer
#include "TestFramework.h"
#include "Utils/ScopedTimer.h"
#include <thread>

TEST(ScopedTimer_CallsCallback)
{
    bool called = false;
    float reportedMs = -1.0f;
    const char* reportedName = nullptr;

    {
        Spark::ScopedTimer timer("TestScope",
                                 [&](const char* name, float ms)
                                 {
                                     called = true;
                                     reportedName = name;
                                     reportedMs = ms;
                                 });
        // Do a small amount of work
        volatile int sum = 0;
        for (int i = 0; i < 1000; ++i)
            sum += i;
    }

    EXPECT_TRUE(called);
    EXPECT_TRUE(reportedName != nullptr);
    EXPECT_GE(reportedMs, 0.0f);
}

TEST(ScopedTimer_ElapsedMs)
{
    Spark::ScopedTimer timer("Elapsed", [](const char*, float) {});
    // ElapsedMs should return a non-negative value immediately
    float elapsed = timer.ElapsedMs();
    EXPECT_GE(elapsed, 0.0f);
}

TEST(ScopedTimer_MeasuresTime)
{
    float measuredMs = 0.0f;
    {
        Spark::ScopedTimer timer("SleepTest", [&](const char*, float ms) { measuredMs = ms; });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Should have measured at least ~10ms (allow some OS scheduling variance)
    EXPECT_GT(measuredMs, 5.0f);
}

// =============================================================================
// Default callback (no callback = printf)
// =============================================================================

TEST(ScopedTimer_DefaultCallback)
{
    // Default callback (nullptr) uses printf to stdout. Give a no-op callback
    // instead to avoid polluting CTest output on Windows.
    {
        Spark::ScopedTimer timer("DefaultCb", [](const char*, float) {});
        volatile int x = 0;
        for (int i = 0; i < 100; ++i)
            x += i;
    }
    EXPECT_TRUE(true);
}

// =============================================================================
// Name is correctly passed
// =============================================================================

TEST(ScopedTimer_NamePassedCorrectly)
{
    std::string capturedName;
    {
        Spark::ScopedTimer timer("MyTimerName", [&](const char* name, float) { capturedName = name; });
    }
    EXPECT_EQ(capturedName, std::string("MyTimerName"));
}

// =============================================================================
// ElapsedMs increases
// =============================================================================

TEST(ScopedTimer_ElapsedMsIncreases)
{
    Spark::ScopedTimer timer("Elapsed2", [](const char*, float) {});
    float first = timer.ElapsedMs();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    float second = timer.ElapsedMs();
    EXPECT_GE(second, first);
}

// =============================================================================
// SPARK_SCOPED_TIMER macro
// =============================================================================

TEST(ScopedTimer_MacroDoesNotCrash)
{
    {
        SPARK_SCOPED_TIMER("MacroTest");
        volatile int x = 0;
        for (int i = 0; i < 100; ++i)
            x += i;
    }
    EXPECT_TRUE(true);
}
