/**
 * @file TestAIDirectorPhaseII.cpp
 * @brief Phase II Theme 3D tests for Spark::AI::AIDirector
 *
 * AIDirector is a per-instance Left 4 Dead-style dynamic difficulty
 * system. These tests lock down the configuration + query API so any
 * game module that owns one finds a stable contract.
 *
 * Update(World&, dt) requires a real ECS World which is out of scope
 * for these unit tests — we only exercise the configuration and
 * query methods that don't touch the world.
 */

#include "TestFramework.h"
#include "Engine/AI/AIDirector.h"

TEST(AIDirectorPhaseII_DefaultConstructs)
{
    Spark::AI::AIDirector director;
    EXPECT_TRUE(director.IsEnabled());
}

TEST(AIDirectorPhaseII_SetEnabledToggles)
{
    Spark::AI::AIDirector director;
    director.SetEnabled(false);
    EXPECT_FALSE(director.IsEnabled());
    director.SetEnabled(true);
    EXPECT_TRUE(director.IsEnabled());
}

TEST(AIDirectorPhaseII_SetDesiredIntensityRange)
{
    Spark::AI::AIDirector director;
    director.SetDesiredIntensityRange(0.2f, 0.8f);
    // Initial intensity defaults to something in range.
    const float intensity = director.GetCurrentIntensity();
    EXPECT_TRUE(intensity >= 0.0f);
    EXPECT_TRUE(intensity <= 1.0f);
}

TEST(AIDirectorPhaseII_ConsoleSetIntensity)
{
    Spark::AI::AIDirector director;
    director.Console_SetIntensity(0.5f);
    EXPECT_NEAR(director.GetCurrentIntensity(), 0.5f, 0.01f);

    director.Console_SetIntensity(0.0f);
    EXPECT_NEAR(director.GetCurrentIntensity(), 0.0f, 0.01f);

    director.Console_SetIntensity(1.0f);
    EXPECT_NEAR(director.GetCurrentIntensity(), 1.0f, 0.01f);
}

TEST(AIDirectorPhaseII_ConsoleForcePhase)
{
    Spark::AI::AIDirector director;
    director.Console_ForcePhase(Spark::AI::DirectorPhase::Peak);
    EXPECT_EQ(static_cast<int>(director.GetCurrentPhase()), static_cast<int>(Spark::AI::DirectorPhase::Peak));

    director.Console_ForcePhase(Spark::AI::DirectorPhase::Relax);
    EXPECT_EQ(static_cast<int>(director.GetCurrentPhase()), static_cast<int>(Spark::AI::DirectorPhase::Relax));
}

TEST(AIDirectorPhaseII_DurationSetters)
{
    Spark::AI::AIDirector director;
    // These don't have getters directly, but calling them should not
    // crash and the Console_GetStatus should still return a string.
    director.SetBuildUpDuration(45.0f);
    director.SetPeakDuration(15.0f);
    director.SetRelaxDuration(20.0f);

    const std::string status = director.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
}

TEST(AIDirectorPhaseII_GetDifficultyParams)
{
    Spark::AI::AIDirector director;
    const auto& params = director.GetDifficultyParams();
    // Default parameters exist — verify GetCurrentSpawnRate is non-negative.
    EXPECT_TRUE(params.enemySpawnRate >= 0.0f);
    EXPECT_TRUE(director.GetCurrentSpawnRate() >= 0.0f);
    EXPECT_TRUE(director.GetDamageMultiplier() >= 0.0f);
    EXPECT_TRUE(director.GetMaxConcurrentEnemies() >= 0);
}

TEST(AIDirectorPhaseII_ConsoleStatusReturnsString)
{
    Spark::AI::AIDirector director;
    const std::string status = director.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
}
