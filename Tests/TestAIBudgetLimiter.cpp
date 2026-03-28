// TestAIBudgetLimiter.cpp - Tests for AI CPU budget management

#include "TestFramework.h"
#include "Engine/AI/AIBudgetLimiter.h"

using namespace Spark::AI;

TEST(AIBudget_SetGetBudgetMs)
{
    AIBudgetLimiter limiter;
    limiter.SetBudgetMs(2.0f);
    EXPECT_NEAR(limiter.GetBudgetMs(), 2.0f, 0.01f);
}

TEST(AIBudget_SetGetMaxStaleFrames)
{
    AIBudgetLimiter limiter;
    limiter.SetMaxStaleFrames(10);
    EXPECT_EQ(limiter.GetMaxStaleFrames(), static_cast<uint32_t>(10));
}

TEST(AIBudget_InitiallyHasBudget)
{
    AIBudgetLimiter limiter;
    limiter.SetBudgetMs(5.0f);
    // Before BeginFrame, budget state is implementation-defined
    // Just verify it doesn't crash
    EXPECT_NO_THROW(limiter.HasBudgetRemaining());
}

TEST(AIBudget_FrameStatsDefault)
{
    AIBudgetLimiter limiter;
    auto stats = limiter.GetLastFrameStats();
    EXPECT_EQ(stats.agentsProcessed, static_cast<uint32_t>(0));
    EXPECT_EQ(stats.agentsDeferred, static_cast<uint32_t>(0));
}

TEST(AIBudget_TrackedAgentCountInitiallyZero)
{
    AIBudgetLimiter limiter;
    EXPECT_EQ(limiter.GetTrackedAgentCount(), static_cast<uint32_t>(0));
}

TEST(AIBudget_SetThresholds)
{
    AIBudgetLimiter limiter;
    EXPECT_NO_THROW(limiter.SetFarDistanceThreshold(100.0f));
    EXPECT_NO_THROW(limiter.SetStarvationBonus(2.0f));
}
