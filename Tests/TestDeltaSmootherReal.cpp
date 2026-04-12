/**
 * @file TestDeltaSmootherReal.cpp
 * @brief Real-class tests for Spark::DeltaSmoother
 */

#include "TestFramework.h"
#include "Utils/DeltaSmoother.h"

TEST(DeltaSmootherReal_DefaultConstruction)
{
    Spark::DeltaSmoother s;
    // Default window — average of zero-filled samples is 0.
    EXPECT_NEAR(s.GetAverage(), 0.0f, 0.001f);
}

TEST(DeltaSmootherReal_SingleSampleAverages)
{
    Spark::DeltaSmoother s(1);
    s.Smooth(0.016f);
    EXPECT_NEAR(s.GetAverage(), 0.016f, 0.001f);
}

TEST(DeltaSmootherReal_MultipleSamplesAverage)
{
    Spark::DeltaSmoother s(3);
    s.Smooth(0.010f);
    s.Smooth(0.020f);
    s.Smooth(0.030f);
    EXPECT_NEAR(s.GetAverage(), 0.020f, 0.001f);
}

TEST(DeltaSmootherReal_RingBufferOverwrite)
{
    Spark::DeltaSmoother s(3);
    s.Smooth(0.010f);
    s.Smooth(0.020f);
    s.Smooth(0.030f);
    s.Smooth(0.040f); // Overwrites oldest (0.010).
    // Window: [0.020, 0.030, 0.040] → avg = 0.030
    EXPECT_NEAR(s.GetAverage(), 0.030f, 0.001f);
}

TEST(DeltaSmootherReal_SmoothReturnsAverage)
{
    Spark::DeltaSmoother s(2);
    const float smoothed = s.Smooth(0.020f);
    EXPECT_NEAR(smoothed, s.GetAverage(), 0.001f);
}

TEST(DeltaSmootherReal_GetSmoothedFPS)
{
    Spark::DeltaSmoother s(1);
    s.Smooth(1.0f / 60.0f); // 60 FPS
    EXPECT_NEAR(s.GetSmoothedFPS(), 60.0f, 1.0f);
}

TEST(DeltaSmootherReal_Reset)
{
    Spark::DeltaSmoother s(2);
    s.Smooth(1.0f);
    s.Smooth(2.0f);
    EXPECT_TRUE(s.GetAverage() > 0.0f);

    s.Reset();
    EXPECT_NEAR(s.GetAverage(), 0.0f, 0.001f);
}

TEST(DeltaSmootherReal_ZeroWindowSizeClampsToOne)
{
    // Constructor should clamp window size to 1.
    Spark::DeltaSmoother s(0);
    s.Smooth(0.016f);
    EXPECT_NEAR(s.GetAverage(), 0.016f, 0.001f);
}
