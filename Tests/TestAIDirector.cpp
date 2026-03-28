// TestAIDirector.cpp - Tests for dynamic difficulty/pacing AI director

#include "TestFramework.h"
#include "Engine/AI/AIDirector.h"

using namespace Spark::AI;

TEST(AIDirector_DefaultState)
{
    AIDirector director;
    EXPECT_TRUE(director.IsEnabled());
    EXPECT_GE(director.GetCurrentIntensity(), 0.0f);
    EXPECT_LE(director.GetCurrentIntensity(), 1.0f);
}

TEST(AIDirector_EnableDisable)
{
    AIDirector director;
    director.SetEnabled(false);
    EXPECT_FALSE(director.IsEnabled());
    director.SetEnabled(true);
    EXPECT_TRUE(director.IsEnabled());
}

TEST(AIDirector_IntensityRange)
{
    AIDirector director;
    director.SetDesiredIntensityRange(0.2f, 0.8f);
    // Intensity should stay within the engine's overall bounds
    EXPECT_GE(director.GetCurrentIntensity(), 0.0f);
    EXPECT_LE(director.GetCurrentIntensity(), 1.0f);
}

TEST(AIDirector_PhaseDurations)
{
    AIDirector director;
    EXPECT_NO_THROW(director.SetBuildUpDuration(30.0f));
    EXPECT_NO_THROW(director.SetPeakDuration(15.0f));
    EXPECT_NO_THROW(director.SetRelaxDuration(20.0f));
}

TEST(AIDirector_DifficultyParams)
{
    AIDirector director;
    const auto& params = director.GetDifficultyParams();
    EXPECT_GT(params.enemySpawnRate, 0.0f);
    EXPECT_GT(params.enemyHealthMultiplier, 0.0f);
    EXPECT_GT(params.maxConcurrentEnemies, 0);
}

TEST(AIDirector_SpawnRatePositive)
{
    AIDirector director;
    EXPECT_GE(director.GetCurrentSpawnRate(), 0.0f);
}

TEST(AIDirector_DamageMultiplierPositive)
{
    AIDirector director;
    EXPECT_GT(director.GetDamageMultiplier(), 0.0f);
}

TEST(AIDirector_MaxConcurrentEnemies)
{
    AIDirector director;
    EXPECT_GT(director.GetMaxConcurrentEnemies(), 0);
}

TEST(AIDirector_ConsoleStatus)
{
    AIDirector director;
    std::string status = director.Console_GetStatus();
    EXPECT_FALSE(status.empty());
}

TEST(AIDirector_ForcePhase)
{
    AIDirector director;
    director.Console_ForcePhase(DirectorPhase::Peak);
    EXPECT_TRUE(director.GetCurrentPhase() == DirectorPhase::Peak);

    director.Console_ForcePhase(DirectorPhase::Relax);
    EXPECT_TRUE(director.GetCurrentPhase() == DirectorPhase::Relax);
}

TEST(AIDirector_SetIntensity)
{
    AIDirector director;
    director.Console_SetIntensity(0.5f);
    EXPECT_NEAR(director.GetCurrentIntensity(), 0.5f, 0.01f);
}
