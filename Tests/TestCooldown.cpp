// TestCooldown.cpp - Tests for Spark::Cooldown and Spark::RepeatTimer
#include "TestFramework.h"
#include "Utils/Cooldown.h"

TEST(Cooldown_StartsReady)
{
    Spark::Cooldown cd(1.0f, true);
    EXPECT_TRUE(cd.IsReady());
    EXPECT_NEAR(cd.GetProgress(), 1.0f, 0.001f);
}

TEST(Cooldown_StartsNotReady)
{
    Spark::Cooldown cd(1.0f, false);
    EXPECT_FALSE(cd.IsReady());
    EXPECT_NEAR(cd.GetProgress(), 0.0f, 0.001f);
}

TEST(Cooldown_UpdateToReady)
{
    Spark::Cooldown cd(1.0f, false);
    cd.Update(0.5f);
    EXPECT_FALSE(cd.IsReady());
    EXPECT_NEAR(cd.GetProgress(), 0.5f, 0.001f);
    cd.Update(0.5f);
    EXPECT_TRUE(cd.IsReady());
    EXPECT_NEAR(cd.GetProgress(), 1.0f, 0.001f);
}

TEST(Cooldown_Reset)
{
    Spark::Cooldown cd(1.0f, true);
    EXPECT_TRUE(cd.IsReady());
    cd.Reset();
    EXPECT_FALSE(cd.IsReady());
    EXPECT_NEAR(cd.GetElapsed(), 0.0f, 0.001f);
}

TEST(Cooldown_ResetWithNewDuration)
{
    Spark::Cooldown cd(1.0f, true);
    cd.Reset(2.0f);
    EXPECT_NEAR(cd.GetDuration(), 2.0f, 0.001f);
    EXPECT_FALSE(cd.IsReady());
}

TEST(Cooldown_ForceReady)
{
    Spark::Cooldown cd(5.0f, false);
    cd.ForceReady();
    EXPECT_TRUE(cd.IsReady());
}

TEST(Cooldown_GetRemaining)
{
    Spark::Cooldown cd(2.0f, false);
    EXPECT_NEAR(cd.GetRemaining(), 2.0f, 0.001f);
    cd.Update(0.5f);
    EXPECT_NEAR(cd.GetRemaining(), 1.5f, 0.001f);
    cd.Update(2.0f);
    EXPECT_NEAR(cd.GetRemaining(), 0.0f, 0.001f);
}

TEST(Cooldown_DoesNotOvershoot)
{
    Spark::Cooldown cd(1.0f, false);
    cd.Update(5.0f);
    EXPECT_NEAR(cd.GetElapsed(), 1.0f, 0.001f);
    EXPECT_NEAR(cd.GetProgress(), 1.0f, 0.001f);
}

TEST(RepeatTimer_NoFireBeforeInterval)
{
    Spark::RepeatTimer timer(1.0f);
    EXPECT_EQ(timer.Update(0.5f), 0);
}

TEST(RepeatTimer_FiresOnInterval)
{
    Spark::RepeatTimer timer(1.0f);
    EXPECT_EQ(timer.Update(1.0f), 1);
}

TEST(RepeatTimer_MultipleFiresOnLargeDelta)
{
    Spark::RepeatTimer timer(0.5f);
    int fires = timer.Update(2.5f);
    EXPECT_EQ(fires, 5);
}

TEST(RepeatTimer_Progress)
{
    Spark::RepeatTimer timer(1.0f);
    timer.Update(0.3f);
    EXPECT_NEAR(timer.GetProgress(), 0.3f, 0.001f);
}

TEST(RepeatTimer_Reset)
{
    Spark::RepeatTimer timer(1.0f);
    timer.Update(0.7f);
    timer.Reset();
    EXPECT_NEAR(timer.GetProgress(), 0.0f, 0.001f);
}

TEST(RepeatTimer_ZeroInterval)
{
    Spark::RepeatTimer timer(0.0f);
    EXPECT_EQ(timer.Update(1.0f), 0);
}
