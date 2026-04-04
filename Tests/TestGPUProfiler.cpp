/**
 * @file TestGPUProfiler.cpp
 * @brief Tests for GPU profiling system
 */

#include "TestFramework.h"
#include "Graphics/GPUProfiler.h"

using namespace Spark::Graphics;

TEST(GPUProfiler_InitializeAndShutdown)
{
    auto& profiler = GPUProfiler::GetInstance();
    profiler.Initialize();
    EXPECT_TRUE(profiler.IsEnabled());
    profiler.Shutdown();
    EXPECT_FALSE(profiler.IsEnabled());
}

TEST(GPUProfiler_FrameHistoryRingBuffer)
{
    auto& profiler = GPUProfiler::GetInstance();
    profiler.Initialize();

    // Record several frames
    for (int i = 0; i < 10; ++i)
    {
        profiler.BeginFrame();
        profiler.EndFrame();
    }

    auto history = profiler.GetFrameHistory(5);
    EXPECT_EQ(history.size(), static_cast<size_t>(5));

    profiler.Shutdown();
}

TEST(GPUProfiler_EventNesting)
{
    auto& profiler = GPUProfiler::GetInstance();
    profiler.Initialize();

    profiler.BeginFrame();
    profiler.BeginEvent("Outer");
    profiler.BeginEvent("Inner");
    profiler.EndEvent(); // Inner
    profiler.EndEvent(); // Outer
    profiler.EndFrame();

    auto& frame = profiler.GetLastFrame();
    EXPECT_EQ(frame.timestamps.size(), static_cast<size_t>(2));

    // Inner event should have depth 1
    bool foundInner = false;
    for (const auto& ts : frame.timestamps)
    {
        if (ts.name == "Inner")
        {
            EXPECT_EQ(ts.depth, static_cast<uint32_t>(1));
            foundInner = true;
        }
    }
    EXPECT_TRUE(foundInner);

    profiler.Shutdown();
}

TEST(GPUProfiler_AverageCalculation)
{
    auto& profiler = GPUProfiler::GetInstance();
    profiler.Initialize();

    for (int i = 0; i < 5; ++i)
    {
        profiler.BeginFrame();
        profiler.EndFrame();
    }

    float avg = profiler.GetAverageGPUTime(5);
    // Average should be non-negative
    EXPECT_TRUE(avg >= 0.0f);

    profiler.Shutdown();
}

TEST(GPUProfiler_EnableDisable)
{
    auto& profiler = GPUProfiler::GetInstance();
    profiler.Initialize();

    profiler.SetEnabled(false);
    EXPECT_FALSE(profiler.IsEnabled());

    profiler.SetEnabled(true);
    EXPECT_TRUE(profiler.IsEnabled());

    profiler.Shutdown();
}

TEST(GPUProfiler_JSONExportFormat)
{
    auto& profiler = GPUProfiler::GetInstance();
    profiler.Initialize();

    profiler.BeginFrame();
    profiler.BeginEvent("TestEvent");
    profiler.EndEvent();
    profiler.EndFrame();

    auto json = profiler.ExportToJSON();
    EXPECT_TRUE(json.find("traceEvents") != std::string::npos);
    EXPECT_TRUE(json.find("TestEvent") != std::string::npos);

    profiler.Shutdown();
}

TEST(GPUProfiler_ConsoleStatus)
{
    auto& profiler = GPUProfiler::GetInstance();
    profiler.Initialize();

    auto status = profiler.Console_GetStatus();
    EXPECT_TRUE(status.find("GPUProfiler") != std::string::npos);

    profiler.Shutdown();
}

TEST(GPUProfiler_EmptyFrameHistory)
{
    auto& profiler = GPUProfiler::GetInstance();
    profiler.Initialize();

    // No frames recorded yet
    float avg = profiler.GetAverageGPUTime(10);
    EXPECT_NEAR(avg, 0.0f, 0.001f);

    profiler.Shutdown();
}
