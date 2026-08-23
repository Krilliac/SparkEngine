// TestScopedTimer.cpp - Tests for Spark::ScopedTimer
#include "TestFramework.h"
#include "Utils/ScopedTimer.h"
#include <cstdint>
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

TEST(ScopedTimer_NamePassedCorrectly)
{
    std::string reportedName;
    {
        Spark::ScopedTimer timer("MyScope", [&](const char* name, float) { reportedName = name; });
    }
    EXPECT_EQ(reportedName, "MyScope");
}

TEST(ScopedTimer_ZeroWorkMinimalTime)
{
    float elapsed = -1.0f;
    {
        Spark::ScopedTimer timer("FastScope", [&](const char*, float ms) { elapsed = ms; });
        // No work
    }
    EXPECT_GE(elapsed, 0.0f);
    EXPECT_LT(elapsed, 100.0f); // Should be well under 100ms for no work
}

TEST(ScopedTimer_MultipleTimers)
{
    float t1 = 0.0f, t2 = 0.0f;
    {
        Spark::ScopedTimer outer("Outer", [&](const char*, float ms) { t1 = ms; });
        {
            Spark::ScopedTimer inner("Inner", [&](const char*, float ms) { t2 = ms; });
            volatile uint64_t sum = 0;
            for (int i = 0; i < 1000; ++i)
                sum = sum + static_cast<uint64_t>(i);
        }
        // Inner completes first
        EXPECT_GE(t2, 0.0f);
    }
    // Outer includes inner time
    EXPECT_GE(t1, t2);
}

TEST(ScopedTimer_ElapsedIncreases)
{
    Spark::ScopedTimer timer("Growth", [](const char*, float) {});
    float e1 = timer.ElapsedMs();
    volatile uint64_t sum = 0;
    for (int i = 0; i < 100000; ++i)
        sum = sum + static_cast<uint64_t>(i);
    float e2 = timer.ElapsedMs();
    EXPECT_GE(e2, e1);
}
