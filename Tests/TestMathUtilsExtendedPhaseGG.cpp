/**
 * @file TestMathUtilsExtendedPhaseGG.cpp
 * @brief Phase GG Theme 3D tests for MathUtilsExtended static utilities
 */

#include "TestFramework.h"
#include "Utils/MathUtilsExtended.h"

TEST(MathUtilsExtendedPhaseGG_InitializeRandomIsSafe)
{
    // Call twice — must not crash or corrupt state.
    MathUtilsExtended::InitializeRandom();
    MathUtilsExtended::InitializeRandom();
}

TEST(MathUtilsExtendedPhaseGG_RandomFloatInRange)
{
    MathUtilsExtended::InitializeRandom();
    for (int i = 0; i < 100; ++i)
    {
        const float f = MathUtilsExtended::RandomFloat(0.0f, 1.0f);
        EXPECT_TRUE(f >= 0.0f);
        EXPECT_TRUE(f <= 1.0f);
    }
}

TEST(MathUtilsExtendedPhaseGG_RandomFloatCustomRange)
{
    MathUtilsExtended::InitializeRandom();
    for (int i = 0; i < 100; ++i)
    {
        const float f = MathUtilsExtended::RandomFloat(-5.0f, 5.0f);
        EXPECT_TRUE(f >= -5.0f);
        EXPECT_TRUE(f <= 5.0f);
    }
}

TEST(MathUtilsExtendedPhaseGG_RandomIntInRange)
{
    MathUtilsExtended::InitializeRandom();
    for (int i = 0; i < 100; ++i)
    {
        const int n = MathUtilsExtended::RandomInt(0, 10);
        EXPECT_TRUE(n >= 0);
        EXPECT_TRUE(n <= 10);
    }
}

TEST(MathUtilsExtendedPhaseGG_RandomFloatSpreadsValues)
{
    MathUtilsExtended::InitializeRandom();
    // Verify the random generator doesn't just return the same value
    // by sampling many times and checking at least one varies.
    const float first = MathUtilsExtended::RandomFloat(0.0f, 100.0f);
    bool foundDifferent = false;
    for (int i = 0; i < 50; ++i)
    {
        const float next = MathUtilsExtended::RandomFloat(0.0f, 100.0f);
        if (next != first)
        {
            foundDifferent = true;
            break;
        }
    }
    EXPECT_TRUE(foundDifferent);
}
