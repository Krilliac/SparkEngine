/**
 * @file TestCooldownReal.cpp
 * @brief Real-class tests for Spark::Cooldown and RepeatTimer
 */

#include "TestFramework.h"
#include "Utils/Cooldown.h"

TEST(CooldownReal_DefaultStartsReady)
{
    Spark::Cooldown c(1.0f); // startReady = true by default
    EXPECT_TRUE(c.IsReady());
    EXPECT_NEAR(c.GetProgress(), 1.0f, 0.001f);
}

TEST(CooldownReal_StartNotReady)
{
    Spark::Cooldown c(2.0f, /*startReady*/ false);
    EXPECT_FALSE(c.IsReady());
    EXPECT_NEAR(c.GetProgress(), 0.0f, 0.001f);
    EXPECT_NEAR(c.GetRemaining(), 2.0f, 0.001f);
}

TEST(CooldownReal_UpdateAdvancesElapsed)
{
    Spark::Cooldown c(1.0f, false);
    c.Update(0.5f);
    EXPECT_NEAR(c.GetElapsed(), 0.5f, 0.001f);
    EXPECT_NEAR(c.GetProgress(), 0.5f, 0.001f);
    EXPECT_FALSE(c.IsReady());
}

TEST(CooldownReal_UpdatePastDurationClamps)
{
    Spark::Cooldown c(1.0f, false);
    c.Update(2.0f);                            // Over-advance.
    EXPECT_NEAR(c.GetElapsed(), 1.0f, 0.001f); // Clamped.
    EXPECT_TRUE(c.IsReady());
    EXPECT_NEAR(c.GetRemaining(), 0.0f, 0.001f);
}

TEST(CooldownReal_ResetToZero)
{
    Spark::Cooldown c(1.0f, true);
    EXPECT_TRUE(c.IsReady());
    c.Reset();
    EXPECT_FALSE(c.IsReady());
    EXPECT_NEAR(c.GetElapsed(), 0.0f, 0.001f);
}

TEST(CooldownReal_ResetWithNewDuration)
{
    Spark::Cooldown c(1.0f, true);
    c.Reset(5.0f);
    EXPECT_NEAR(c.GetDuration(), 5.0f, 0.001f);
    EXPECT_FALSE(c.IsReady());
    EXPECT_NEAR(c.GetElapsed(), 0.0f, 0.001f);
}

TEST(CooldownReal_ForceReady)
{
    Spark::Cooldown c(3.0f, false);
    EXPECT_FALSE(c.IsReady());
    c.ForceReady();
    EXPECT_TRUE(c.IsReady());
}

TEST(CooldownReal_SetDurationPreservesElapsed)
{
    Spark::Cooldown c(2.0f, false);
    c.Update(1.0f);
    c.SetDuration(5.0f);
    EXPECT_NEAR(c.GetDuration(), 5.0f, 0.001f);
    EXPECT_NEAR(c.GetElapsed(), 1.0f, 0.001f);
}

TEST(CooldownReal_ZeroDurationAlwaysReady)
{
    Spark::Cooldown c(0.0f, false);
    EXPECT_NEAR(c.GetProgress(), 1.0f, 0.001f);
}
