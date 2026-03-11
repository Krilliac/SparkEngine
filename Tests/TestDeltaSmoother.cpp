// TestDeltaSmoother.cpp - Tests for Spark::DeltaSmoother
#include "TestFramework.h"
#include "Utils/DeltaSmoother.h"

TEST(DeltaSmoother_InitialAverageIsZero)
{
    Spark::DeltaSmoother smoother(5);
    EXPECT_NEAR(smoother.GetAverage(), 0.0f, 0.001f);
}

TEST(DeltaSmoother_SingleSample)
{
    Spark::DeltaSmoother smoother(1);
    float result = smoother.Smooth(0.016f);
    EXPECT_NEAR(result, 0.016f, 0.001f);
}

TEST(DeltaSmoother_AveragesOverWindow)
{
    Spark::DeltaSmoother smoother(4);
    smoother.Smooth(0.010f);
    smoother.Smooth(0.020f);
    smoother.Smooth(0.010f);
    smoother.Smooth(0.020f);
    // Average of 10, 20, 10, 20 = 15
    EXPECT_NEAR(smoother.GetAverage(), 0.015f, 0.001f);
}

TEST(DeltaSmoother_ClampsLargeDeltas)
{
    Spark::DeltaSmoother smoother(1);
    float result = smoother.Smooth(1.0f); // way above 0.25 max
    EXPECT_NEAR(result, 0.25f, 0.001f);
}

TEST(DeltaSmoother_SmoothedFPS)
{
    Spark::DeltaSmoother smoother(1);
    smoother.Smooth(0.016f);
    float fps = smoother.GetSmoothedFPS();
    EXPECT_NEAR(fps, 62.5f, 1.0f);
}

TEST(DeltaSmoother_GetRange)
{
    Spark::DeltaSmoother smoother(4);
    smoother.Smooth(0.010f);
    smoother.Smooth(0.030f);
    smoother.Smooth(0.020f);
    smoother.Smooth(0.015f);
    auto [minVal, maxVal] = smoother.GetRange();
    EXPECT_NEAR(minVal, 0.010f, 0.001f);
    EXPECT_NEAR(maxVal, 0.030f, 0.001f);
}

TEST(DeltaSmoother_Reset)
{
    Spark::DeltaSmoother smoother(4);
    smoother.Smooth(0.016f);
    smoother.Smooth(0.016f);
    smoother.Reset();
    EXPECT_NEAR(smoother.GetAverage(), 0.0f, 0.001f);
}

TEST(DeltaSmoother_WindowSize)
{
    Spark::DeltaSmoother smoother(10);
    EXPECT_EQ(smoother.GetWindowSize(), 10u);
}

TEST(DeltaSmoother_MinWindowSize)
{
    Spark::DeltaSmoother smoother(0); // should clamp to 1
    EXPECT_EQ(smoother.GetWindowSize(), 1u);
}

TEST(DeltaSmoother_WrapsAround)
{
    Spark::DeltaSmoother smoother(3);
    // Fill the buffer
    smoother.Smooth(0.010f);
    smoother.Smooth(0.020f);
    smoother.Smooth(0.030f);
    // Overwrite oldest
    smoother.Smooth(0.040f);
    // Window now contains: 0.020, 0.030, 0.040 → avg = 0.030
    EXPECT_NEAR(smoother.GetAverage(), 0.030f, 0.001f);
}
