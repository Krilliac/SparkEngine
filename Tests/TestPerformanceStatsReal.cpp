/**
 * @file TestPerformanceStatsReal.cpp
 * @brief Real-class tests for Spark::PerformanceStats
 */

#include "TestFramework.h"
#include "Utils/PerformanceStats.h"

#include <thread>

TEST(PerformanceStatsReal_DefaultConstruction)
{
    Spark::PerformanceStats stats;
    EXPECT_EQ(stats.GetTotalFrames(), static_cast<uint64_t>(0));
    EXPECT_NEAR(stats.GetAverageFrameTime(), 0.0f, 0.001f);
}

TEST(PerformanceStatsReal_BeginEndFrameIncrements)
{
    Spark::PerformanceStats stats;
    stats.BeginFrame();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    stats.EndFrame();
    EXPECT_EQ(stats.GetTotalFrames(), static_cast<uint64_t>(1));
    EXPECT_TRUE(stats.GetAverageFrameTime() >= 0.0f);
}

TEST(PerformanceStatsReal_MultipleFramesAggregate)
{
    Spark::PerformanceStats stats;
    for (int i = 0; i < 5; ++i)
    {
        stats.BeginFrame();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        stats.EndFrame();
    }
    EXPECT_EQ(stats.GetTotalFrames(), static_cast<uint64_t>(5));
}

TEST(PerformanceStatsReal_MinMaxFPSInitiallyZero)
{
    Spark::PerformanceStats stats;
    EXPECT_NEAR(stats.GetMinFPS(), 0.0f, 0.001f);
    EXPECT_NEAR(stats.GetMaxFPS(), 0.0f, 0.001f);
}
