/**
 * @file TestChromeTracing.cpp
 * @brief Tests for Spark::ChromeTracing profiler
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/ChromeTracing.h"
#include <thread>
#include <cstdio>

TEST(ChromeTracing_StartStop)
{
    auto& tracer = Spark::ChromeTracing::GetInstance();
    tracer.Clear();

    EXPECT_FALSE(tracer.IsActive());
    tracer.Start();
    EXPECT_TRUE(tracer.IsActive());
    tracer.Stop();
    EXPECT_FALSE(tracer.IsActive());
}

TEST(ChromeTracing_RecordEvents)
{
    auto& tracer = Spark::ChromeTracing::GetInstance();
    tracer.Start();

    tracer.BeginEvent("TestSection", "test");
    tracer.EndEvent("TestSection", "test");
    tracer.InstantEvent("Marker", "test");

    tracer.Stop();
    EXPECT_TRUE(tracer.EventCount() >= 3);
    tracer.Clear();
}

TEST(ChromeTracing_ScopeGuard)
{
    auto& tracer = Spark::ChromeTracing::GetInstance();
    tracer.Start();

    {
        SPARK_TRACE_SCOPE("ScopedTest");
        // Some work
        volatile int x = 0;
        for (int i = 0; i < 100; i++)
            x += i;
    }

    tracer.Stop();
    EXPECT_TRUE(tracer.EventCount() >= 2); // Begin + End
    tracer.Clear();
}

TEST(ChromeTracing_NotActiveSkipsEvents)
{
    auto& tracer = Spark::ChromeTracing::GetInstance();
    tracer.Clear();
    tracer.Stop(); // Ensure stopped

    tracer.BeginEvent("ShouldNotRecord");
    tracer.EndEvent("ShouldNotRecord");

    EXPECT_EQ(tracer.EventCount(), static_cast<size_t>(0));
}

TEST(ChromeTracing_SaveToFile)
{
    auto& tracer = Spark::ChromeTracing::GetInstance();
    tracer.Start();
    tracer.BeginEvent("TestSave");
    tracer.EndEvent("TestSave");
    tracer.Stop();

    const char* path = "/tmp/spark_test_trace.json";
    bool ok = tracer.SaveToFile(path);
    EXPECT_TRUE(ok);

    // Cleanup
    std::remove(path);
    tracer.Clear();
}
