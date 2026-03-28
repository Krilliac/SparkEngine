// TestProfiler.cpp - Tests for the frame profiling system

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/Profiler.h"
#include <thread>
#include <chrono>

// =============================================================================
// FrameTimingHistory
// =============================================================================

TEST(Profiler_FrameTimingHistoryPush)
{
    FrameTimingHistory history;

    history.Push(16.0f);
    history.Push(17.0f);
    history.Push(15.0f);

    EXPECT_NEAR(history.minTime, 15.0f, 0.01f);
    EXPECT_NEAR(history.maxTime, 17.0f, 0.01f);
    EXPECT_GT(history.avgTime, 0.0f);
}

TEST(Profiler_FrameTimingHistoryWraps)
{
    FrameTimingHistory history;

    // Push more than HISTORY_SIZE entries to test wrapping
    for (int i = 0; i < FrameTimingHistory::HISTORY_SIZE + 50; ++i)
    {
        history.Push(static_cast<float>(i % 100));
    }

    // Should not crash and stats should be valid
    EXPECT_GE(history.minTime, 0.0f);
    EXPECT_GT(history.maxTime, 0.0f);
    EXPECT_GT(history.avgTime, 0.0f);
}

TEST(Profiler_FrameTimingHistoryStats)
{
    FrameTimingHistory history;

    // Push exactly 10 known values
    for (int i = 1; i <= 10; ++i)
    {
        history.Push(static_cast<float>(i));
    }

    EXPECT_NEAR(history.minTime, 1.0f, 0.01f);
    EXPECT_NEAR(history.maxTime, 10.0f, 0.01f);
    // Average of 1..10 = 5.5
    EXPECT_NEAR(history.avgTime, 5.5f, 0.1f);
}

// =============================================================================
// Section Timing
// =============================================================================

TEST(Profiler_BeginEndSection)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    p.BeginSection("TestSection", ProfileCategory::GameLogic);

    // Small busy-wait to ensure measurable time
    volatile int x = 0;
    for (int i = 0; i < 100000; ++i)
        x += i;

    p.EndSection("TestSection");

    double time = p.GetSectionTimeMs("TestSection");
    EXPECT_GE(time, 0.0);
}

TEST(Profiler_MultipleSections)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    p.BeginSection("SectionA", ProfileCategory::Render);
    p.EndSection("SectionA");

    p.BeginSection("SectionB", ProfileCategory::Physics);
    p.EndSection("SectionB");

    double timeA = p.GetSectionTimeMs("SectionA");
    double timeB = p.GetSectionTimeMs("SectionB");
    EXPECT_GE(timeA, 0.0);
    EXPECT_GE(timeB, 0.0);
}

TEST(Profiler_UnknownSectionReturnsZero)
{
    auto& p = Profiler::GetInstance();
    double time = p.GetSectionTimeMs("NonexistentSection");
    EXPECT_NEAR(time, 0.0, 0.001);
}

// =============================================================================
// Frame Timing
// =============================================================================

TEST(Profiler_BeginEndFrame)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    p.BeginFrame();

    volatile int x = 0;
    for (int i = 0; i < 10000; ++i)
        x += i;

    p.EndFrame();

    // Frame time should be non-negative
    float frameTime = p.GetFrameTimeMs();
    EXPECT_GE(frameTime, 0.0f);
}

TEST(Profiler_FPSCalculation)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    // Push known frame times to get a predictable FPS
    for (int i = 0; i < 10; ++i)
    {
        p.BeginFrame();
        // Can't guarantee exact timing, just check it doesn't crash
        p.EndFrame();
    }

    float fps = p.GetFPS();
    // FPS should be positive after some frames
    EXPECT_GE(fps, 0.0f);
}

// =============================================================================
// Memory Tracking
// =============================================================================

TEST(Profiler_RecordAllocation)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    p.RecordAllocation("Textures", 1024);
    p.RecordAllocation("Textures", 2048);
    p.RecordAllocation("Audio", 512);

    // Should not crash
    std::string report = p.Console_GetMemoryReport();
    EXPECT_FALSE(report.empty());
}

TEST(Profiler_RecordDeallocation)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    p.RecordAllocation("TestCat", 1000);
    p.RecordDeallocation("TestCat", 500);

    // Memory report should reflect tracking
    std::string report = p.Console_GetMemoryReport();
    EXPECT_FALSE(report.empty());
}

// =============================================================================
// Category Timing
// =============================================================================

TEST(Profiler_CategoryTimeMs)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    p.BeginSection("RenderWork", ProfileCategory::Render);
    volatile int x = 0;
    for (int i = 0; i < 10000; ++i)
        x += i;
    p.EndSection("RenderWork");

    double renderTime = p.GetCategoryTimeMs(ProfileCategory::Render);
    EXPECT_GE(renderTime, 0.0);
}

// =============================================================================
// Display Control
// =============================================================================

TEST(Profiler_EnableDisable)
{
    auto& p = Profiler::GetInstance();

    p.SetEnabled(false);
    EXPECT_FALSE(p.IsEnabled());

    p.SetEnabled(true);
    EXPECT_TRUE(p.IsEnabled());

    p.Toggle();
    EXPECT_FALSE(p.IsEnabled());

    p.Toggle();
    EXPECT_TRUE(p.IsEnabled());
}

TEST(Profiler_OverlayVisibility)
{
    auto& p = Profiler::GetInstance();

    p.SetOverlayVisible(false);
    EXPECT_FALSE(p.IsOverlayVisible());

    p.SetOverlayVisible(true);
    EXPECT_TRUE(p.IsOverlayVisible());

    p.ToggleOverlay();
    EXPECT_FALSE(p.IsOverlayVisible());
}

// =============================================================================
// Console Reports
// =============================================================================

TEST(Profiler_ConsoleGetReport)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    p.BeginSection("ReportTest", ProfileCategory::Custom);
    p.EndSection("ReportTest");

    std::string report = p.Console_GetReport();
    EXPECT_FALSE(report.empty());
}

TEST(Profiler_ConsoleGetGPUReport)
{
    auto& p = Profiler::GetInstance();
    std::string report = p.Console_GetGPUReport();
    // On non-Windows, this may return a placeholder message
    EXPECT_FALSE(report.empty());
}

// =============================================================================
// Frame History Access
// =============================================================================

TEST(Profiler_GetFrameHistory)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    for (int i = 0; i < 5; ++i)
    {
        p.BeginFrame();
        p.EndFrame();
    }

    const auto& history = p.GetFrameHistory();
    // History should have been populated
    EXPECT_GE(history.avgTime, 0.0f);
}

// =============================================================================
// GPU Timing (non-Windows fallback)
// =============================================================================

TEST(Profiler_GetGPUTimeMsReturnsZeroWithoutInit)
{
    auto& p = Profiler::GetInstance();
    double gpuTime = p.GetGPUTimeMs("SomeName");
    EXPECT_NEAR(gpuTime, 0.0, 0.001);
}

// =============================================================================
// ScopedProfileTimer
// =============================================================================

TEST(Profiler_ScopedProfileTimer)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    {
        ScopedProfileTimer timer("ScopedTest", ProfileCategory::GameLogic);
        volatile int x = 0;
        for (int i = 0; i < 10000; ++i)
            x += i;
    }

    double time = p.GetSectionTimeMs("ScopedTest");
    EXPECT_GE(time, 0.0);
}

// =============================================================================
// Shutdown
// =============================================================================

TEST(Profiler_Shutdown)
{
    auto& p = Profiler::GetInstance();
    p.SetEnabled(true);

    p.BeginSection("WillBeCleared");
    p.EndSection("WillBeCleared");

    EXPECT_NO_THROW(p.Shutdown());
}
