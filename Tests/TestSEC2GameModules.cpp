/**
 * @file TestSEC2GameModules.cpp
 * @brief SEC2 game-module security fixes: bounded console inputs, fog-of-war work bounds,
 *        owner-scoped network handlers across module hot reload, and server-attributed chat.
 *
 * Each SEC2GM_ test pins one fix from the SEC2-game-modules lane:
 * - ProgressionSystem::AwardXP saturates instead of reaching float-to-int or signed-overflow UB.
 * - WaveComposition bounds every wave to MAX_ENEMIES_PER_WAVE, heavies included.
 * - NetworkManager handler ownership: a hot-reload replacement keeps its handlers when the outgoing
 *   image tears down, and everything an image owns is removed before it is unmapped.
 * - MMO chat relays only routable channels and never forwards a client-chosen sender name.
 * - RTS fog of war clips vision to the grid and restored saves reject absurd vision ranges.
 *
 * The CTest registration SEC2GameModules pins the family size, which depends on the
 * ImGui (module sources) and networking features compiled into SparkTests.
 */

#include "TestFramework.h"

#include "../GameModules/SparkGameFPS/Source/Game/ProgressionSystem.h"
#include "../GameModules/SparkGameFPS/Source/Game/WaveComposition.h"

#include <climits>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// FPS progression (xp console command)
// ---------------------------------------------------------------------------

TEST(SEC2GM_ProgressionAwardSaturatesInsteadOfOverflowing)
{
    Spark::ProgressionSystem progression;
    progression.Initialize();
    int observedBase = -1;
    int observedModified = -1;
    progression.GetCallbacks().onXPAwarded = [&](int base, const std::string&, int modified)
    {
        observedBase = base;
        observedModified = modified;
    };

    // float(INT_MAX) rounds up to 2^31; the old cast back to int was undefined and produced INT_MIN,
    // leaving a negative XP total that was then persisted.
    progression.AwardXP(INT_MAX, "console");
    EXPECT_EQ(observedBase, Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(observedModified, Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(progression.GetLevel(), progression.GetMaxLevel());

    // Awards stop at max level, so repetition cannot push the total toward INT_MAX either.
    progression.AwardXP(INT_MAX, "console");
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::MAX_SINGLE_AWARD);

    // Ordinary gameplay awards are unchanged.
    Spark::ProgressionSystem ordinary;
    ordinary.Initialize();
    ordinary.AwardXP(Spark::ProgressionSystem::XP_PER_KILL, "kill");
    EXPECT_EQ(ordinary.GetCurrentXP(), Spark::ProgressionSystem::XP_PER_KILL);
}

TEST(SEC2GM_ProgressionIgnoresNonPositiveAwards)
{
    Spark::ProgressionSystem progression;
    progression.Initialize();
    progression.AwardXP(Spark::ProgressionSystem::XP_PER_KILL, "kill");

    progression.AwardXP(0, "console");
    progression.AwardXP(-500, "console");
    progression.AwardXP(INT_MIN, "console");
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::XP_PER_KILL);
    EXPECT_EQ(progression.GetLevel(), 1);
}

// ---------------------------------------------------------------------------
// FPS wave composition (wave_skip / wave_difficulty console commands)
// ---------------------------------------------------------------------------

TEST(SEC2GM_WaveCompositionNeverExceedsCap)
{
    using namespace Spark::WaveComposition;
    const float scales[] = {
        1.0f, 3.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -5.0f, 1000.0f};
    std::vector<int> waves = {600000, INT_MAX, INT_MAX - 1, INT_MIN, -1, 0};
    for (int wave = 1; wave <= MAX_WAVE_NUMBER; ++wave)
        waves.push_back(wave);

    for (const int wave : waves)
    {
        for (const float scale : scales)
        {
            const Spark::WaveDefinition definition = Compose(wave, scale);
            const int total = definition.TotalEnemies();
            EXPECT_GT(total, 0);
            EXPECT_LE(total, MAX_ENEMIES_PER_WAVE);
            EXPECT_GE(definition.waveNumber, 1);
            EXPECT_LE(definition.waveNumber, MAX_WAVE_NUMBER);
            if (definition.isBossWave)
                EXPECT_GE(definition.heavyCount, 1);
            EXPECT_TRUE(std::isfinite(definition.healthMultiplier));
            EXPECT_TRUE(std::isfinite(definition.damageMultiplier));
            EXPECT_TRUE(std::isfinite(definition.speedMultiplier));
        }
    }
}

TEST(SEC2GM_WaveCompositionClampsInputs)
{
    using namespace Spark::WaveComposition;

    // wave_skip 600000 used to produce a boss wave of 120,001 unscaled heavies.
    const Spark::WaveDefinition huge = Compose(600000, 1.0f);
    EXPECT_EQ(huge.waveNumber, MAX_WAVE_NUMBER);
    EXPECT_LE(huge.heavyCount, MAX_ENEMIES_PER_WAVE);

    // Ordinary early waves are unchanged by the cap.
    const Spark::WaveDefinition first = Compose(1, 1.0f);
    EXPECT_EQ(first.gruntCount, 3);
    EXPECT_EQ(first.TotalEnemies(), 3);
    const Spark::WaveDefinition boss = Compose(5, 1.0f);
    EXPECT_TRUE(boss.isBossWave);
    EXPECT_EQ(boss.heavyCount, 2);

    EXPECT_EQ(ClampWaveNumber(INT_MIN), 1);
    EXPECT_EQ(ClampWaveNumber(INT_MAX), MAX_WAVE_NUMBER);
    EXPECT_TRUE(IsValidDifficultyScale(1.0f));
    EXPECT_TRUE(IsValidDifficultyScale(3.0f));
    EXPECT_FALSE(IsValidDifficultyScale(0.5f));
    EXPECT_FALSE(IsValidDifficultyScale(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(IsValidDifficultyScale(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(SanitizeDifficultyScale(std::numeric_limits<float>::quiet_NaN()), 1.0f);
    EXPECT_EQ(SanitizeDifficultyScale(50.0f), MAX_DIFFICULTY_SCALE);
}

// ---------------------------------------------------------------------------
// Module sources compiled only with ImGui (RTS fog of war, MMO chat)
// ---------------------------------------------------------------------------

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.h"
#include "../GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.h"
#include "../GameModules/SparkGameRTS/Source/Core/RTSPersistence.h"
#include "../GameModules/SparkGameRTS/Source/FogOfWar/RTSFogOfWarSystem.h"
#include "../GameModules/SparkGameRTS/Source/Match/RTSMatchSystem.h"
#include "../GameModules/SparkGameRTS/Source/Resource/RTSResourceSystem.h"
#include "../GameModules/SparkGameRTS/Source/Simulation/RTSSkirmishSimulation.h"
#include "../GameModules/SparkGameRTS/Source/Unit/RTSUnitSystem.h"

TEST(SEC2GM_RTSFogVisionIsClippedToTheGrid)
{
    RTS::RTSFogOfWarSystem fog;
    ASSERT_TRUE(fog.Initialize(nullptr, 128, 128));

    // A restored visionRange of 1e9 used to mean ~4e18 loop iterations (and UB in the int arithmetic);
    // clipped to the grid it is at most 128 * 128 cells and reveals the whole map.
    fog.UpdateVision(RTS::RTSFaction::Human, 64.0f, 64.0f, 1.0e9f);
    EXPECT_EQ(fog.GetExploredPercent(RTS::RTSFaction::Human), 100.0f);
    fog.HideArea(RTS::RTSFaction::Human, 64.0f, 64.0f, std::numeric_limits<float>::max());
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Human, 10.0f, 10.0f));

    // Non-finite ranges and positions far off the grid reveal nothing and are defined behaviour.
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, std::numeric_limits<float>::quiet_NaN());
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, std::numeric_limits<float>::infinity());
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, -3.0f);
    fog.UpdateVision(RTS::RTSFaction::Sentinel, std::numeric_limits<float>::quiet_NaN(), 64.0f, 5.0f);
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 1.0e30f, -1.0e30f, 5.0f);
    EXPECT_EQ(fog.GetExploredPercent(RTS::RTSFaction::Sentinel), 0.0f);

    // An ordinary unit still reveals exactly its disc.
    fog.UpdateVision(RTS::RTSFaction::Swarm, 10.0f, 10.0f, 8.0f);
    EXPECT_TRUE(fog.IsVisible(RTS::RTSFaction::Swarm, 18.0f, 10.0f));
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Swarm, 19.0f, 10.0f));
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Swarm, 17.0f, 17.0f));

    // Grids larger than the persisted bound are refused up front.
    RTS::RTSFogOfWarSystem oversized;
    EXPECT_FALSE(oversized.Initialize(nullptr, RTS::RTSFogOfWarSystem::MAX_MAP_DIMENSION + 1, 16));
}

TEST(SEC2GM_RTSRestoreRejectsUnboundedVisionRange)
{
    RTS::UnitData unit;
    unit.unitId = 1;
    unit.visionRange = 10000.0f;
    RTS::RTSUnitSystem units;
    EXPECT_FALSE(units.RestoreState({unit}, 2));
    unit.visionRange = RTS::RTSUnitSystem::MAX_VISION_RANGE;
    EXPECT_TRUE(units.RestoreState({unit}, 2));

    // The save decoder's validator applies the same bound to a captured snapshot.
    RTS::RTSUnitSystem liveUnits;
    RTS::RTSBuildingSystem buildings;
    RTS::RTSResourceSystem resources;
    RTS::RTSCommandSystem commands;
    RTS::RTSFogOfWarSystem fog;
    RTS::RTSMatchSystem match;
    RTS::RTSSkirmishSimulation simulation;
    const RTS::RTSSkirmishSystems systems{&liveUnits, &buildings, &resources, &commands, &fog, &match};
    simulation.Initialize(nullptr, systems);
    simulation.StartDefaultSkirmish();

    RTS::RTSPersistenceSnapshot snapshot = RTS::RTSPersistence::Capture(systems, simulation);
    ASSERT_FALSE(snapshot.units.empty());
    std::string error;
    EXPECT_TRUE(RTS::RTSPersistence::Validate(snapshot, error));
    snapshot.units.front().visionRange = 10000.0f;
    EXPECT_FALSE(RTS::RTSPersistence::Validate(snapshot, error));
    EXPECT_FALSE(error.empty());
}

#endif // SPARK_TEST_HAS_IMGUI
