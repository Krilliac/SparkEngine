/**
 * @file TestSparkGameFPSLoopReal.cpp
 * @brief Real-class tests for the SparkGameFPS stable-v1 single-player slice:
 *        the death -> respawn -> score loop, local profile persistence, the
 *        single asset root, progression restore, and the FPS state-validation
 *        predicates.
 *
 * Every test exercises production classes compiled from
 * GameModules/SparkGameFPS/Source; nothing here re-implements module logic.
 */

#include "TestFramework.h"

#include "Game/FPSAssetPaths.h"
#include "Game/FPSLocalProfile.h"
#include "Game/FPSStateRules.h"
#include "Game/GameMechanics.h"
#include "Game/ProgressionSystem.h"

#include "Engine/Events/EventSystem.h"
#include "Utils/EventBus.h"

#include <ctime>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Spark;

namespace
{
    /// Build a temporary directory tree unique to one test, cleaned up by the caller.
    std::filesystem::path MakeTempDir(const std::string& tag)
    {
        std::error_code error;
        const std::filesystem::path base =
            std::filesystem::temp_directory_path(error) / ("spark_fps_" + tag + "_" + std::to_string(::time(nullptr)));
        std::filesystem::remove_all(base, error);
        std::filesystem::create_directories(base, error);
        return base;
    }

    void RemoveTree(const std::filesystem::path& path)
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
} // namespace

// ============================================================================
// Local profile persistence (mod-fps-04, mod-fps-22, ecs-gameplay-save-01)
// ============================================================================

TEST(FPSLocalProfile_RoundTripsThroughCustomState)
{
    FPSLocalProfile saved;
    saved.progressionLevel = 7;
    saved.progressionXP = 4321;
    saved.playerClass = 3;
    saved.weapon = 2;
    saved.kills = 19;
    saved.deaths = 4;
    saved.score = 950;
    saved.playTimeSeconds = 123.5f;
    saved.health = 62.5f;
    saved.armor = 25.0f;

    std::unordered_map<std::string, std::string> customState;
    saved.WriteTo(customState);

    FPSLocalProfile loaded;
    std::string error;
    ASSERT_TRUE(loaded.ReadFrom(customState, error));
    EXPECT_TRUE(error.empty());

    EXPECT_EQ(loaded.progressionLevel, 7);
    EXPECT_EQ(loaded.progressionXP, 4321);
    EXPECT_EQ(loaded.playerClass, 3);
    EXPECT_EQ(loaded.weapon, 2);
    EXPECT_EQ(loaded.kills, 19);
    EXPECT_EQ(loaded.deaths, 4);
    EXPECT_EQ(loaded.score, 950);
    EXPECT_NEAR(loaded.playTimeSeconds, 123.5f, 0.01f);
    EXPECT_NEAR(loaded.health, 62.5f, 0.01f);
    EXPECT_NEAR(loaded.armor, 25.0f, 0.01f);
}

TEST(FPSLocalProfile_RejectsCustomStateWithoutProfileBlock)
{
    std::unordered_map<std::string, std::string> customState;
    customState["some.other.module"] = "42";

    FPSLocalProfile loaded;
    loaded.progressionXP = 99;
    std::string error;

    EXPECT_FALSE(loaded.ReadFrom(customState, error));
    EXPECT_STR_CONTAINS(error, "missing key");
    // A rejected read must not partially overwrite the caller's profile.
    EXPECT_EQ(loaded.progressionXP, 99);
}

TEST(FPSLocalProfile_RejectsProfileFromNewerModule)
{
    FPSLocalProfile saved;
    saved.progressionXP = 10;
    std::unordered_map<std::string, std::string> customState;
    saved.WriteTo(customState);
    customState[std::string(FPSLocalProfile::kKeyPrefix) + "version"] = std::to_string(FPSLocalProfile::kVersion + 1);

    FPSLocalProfile loaded;
    std::string error;
    EXPECT_FALSE(loaded.ReadFrom(customState, error));
    EXPECT_STR_CONTAINS(error, "newer module");
}

TEST(FPSLocalProfile_RejectsUnparseableField)
{
    FPSLocalProfile saved;
    std::unordered_map<std::string, std::string> customState;
    saved.WriteTo(customState);
    customState[std::string(FPSLocalProfile::kKeyPrefix) + "xp"] = "not-a-number";

    FPSLocalProfile loaded;
    std::string error;
    EXPECT_FALSE(loaded.ReadFrom(customState, error));
    EXPECT_STR_CONTAINS(error, "unparseable");
}

TEST(FPSLocalProfile_LeavesUnrelatedCustomStateAlone)
{
    std::unordered_map<std::string, std::string> customState;
    customState["template.encounter"] = "boss_02";

    FPSLocalProfile saved;
    saved.WriteTo(customState);

    EXPECT_EQ(customState["template.encounter"], std::string("boss_02"));
    EXPECT_TRUE(customState.count(std::string(FPSLocalProfile::kKeyPrefix) + "xp") == 1u);
}

// ============================================================================
// Single asset root (mod-fps-11)
// ============================================================================

TEST(FPSAssets_FindAssetRootPicksFirstBaseThatHasModels)
{
    const std::filesystem::path temp = MakeTempDir("assetroot");
    const std::filesystem::path emptyBase = temp / "staged";
    const std::filesystem::path goodBase = temp / "repo";

    std::error_code error;
    std::filesystem::create_directories(emptyBase, error);
    std::filesystem::create_directories(goodBase / "Assets" / "Models", error);

    const std::filesystem::path found = FPSAssets::FindAssetRoot({emptyBase, goodBase});
    EXPECT_TRUE(found == (goodBase / "Assets"));

    RemoveTree(temp);
}

TEST(FPSAssets_FindAssetRootPrefersEarlierBase)
{
    const std::filesystem::path temp = MakeTempDir("assetorder");
    const std::filesystem::path first = temp / "exe";
    const std::filesystem::path second = temp / "cwd";

    std::error_code error;
    std::filesystem::create_directories(first / "Assets" / "Models", error);
    std::filesystem::create_directories(second / "Assets" / "Models", error);

    const std::filesystem::path found = FPSAssets::FindAssetRoot({first, second});
    EXPECT_TRUE(found == (first / "Assets"));

    RemoveTree(temp);
}

TEST(FPSAssets_FindAssetRootReturnsEmptyWhenNothingMatches)
{
    const std::filesystem::path temp = MakeTempDir("assetnone");
    std::error_code error;
    std::filesystem::create_directories(temp / "no_assets_here", error);

    const std::filesystem::path found = FPSAssets::FindAssetRoot({temp, temp / "no_assets_here"});
    EXPECT_TRUE(found.empty());

    RemoveTree(temp);
}

TEST(FPSAssets_ResolveIsRelativeToTheAssetRootNotTheParentOfTheCwd)
{
    const std::wstring resolved = FPSAssets::Resolve(L"Models/pistol.obj");
    const std::filesystem::path resolvedPath(resolved);

    // The resolved path must sit under the discovered root, and must never be the
    // old parent-of-working-directory form that broke staged and installed layouts.
    EXPECT_TRUE(resolvedPath.parent_path() == (FPSAssets::Root() / "Models"));
    EXPECT_TRUE(resolvedPath.filename() == std::filesystem::path("pistol.obj"));
    EXPECT_TRUE(resolved.find(L"../Assets") == std::wstring::npos);
}

// ============================================================================
// Death -> respawn -> score loop (mod-fps-02, mod-fps-21)
// ============================================================================

TEST(FPSRespawn_DeathScoresAndArmsTheRespawnTimer)
{
    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    respawn.SetRespawnDelay(5.0f);

    EXPECT_FALSE(respawn.IsWaitingForRespawn());
    EXPECT_EQ(respawn.GetPlayerScore().deaths, 0);

    respawn.OnPlayerDeath("Enemy", "Rifle", false);

    EXPECT_TRUE(respawn.IsWaitingForRespawn());
    EXPECT_EQ(respawn.GetPlayerScore().deaths, 1);
    EXPECT_EQ(respawn.GetPlayerScore().currentStreak, 0);
    EXPECT_NEAR(respawn.GetRespawnTimeRemaining(), 5.0f, 0.001f);
    EXPECT_EQ(static_cast<int>(respawn.GetKillHistory().size()), 1);
}

TEST(FPSRespawn_UpdatePublishesRespawnEventAfterTheDelay)
{
    EventBus bus;
    int respawnCount = 0;
    PlayerRespawnEvent lastEvent{};
    auto subscription = bus.Subscribe<PlayerRespawnEvent>(
        [&respawnCount, &lastEvent](const PlayerRespawnEvent& e)
        {
            respawnCount++;
            lastEvent = e;
        });

    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    respawn.SetEventBus(&bus);
    respawn.SetRespawnDelay(2.0f);

    RespawnPoint preferred;
    preferred.name = "North";
    preferred.position = {11.0f, 2.0f, -13.0f};
    preferred.priority = 10;
    EXPECT_GE(respawn.AddSpawnPoint(preferred), 0);

    respawn.OnPlayerDeath("Enemy", "Rifle", false);

    respawn.Update(1.0f);
    EXPECT_EQ(respawnCount, 0);
    EXPECT_TRUE(respawn.IsWaitingForRespawn());

    respawn.Update(1.5f);
    EXPECT_EQ(respawnCount, 1);
    EXPECT_FALSE(respawn.IsWaitingForRespawn());
    EXPECT_NEAR(lastEvent.spawnX, 11.0f, 0.001f);
    EXPECT_NEAR(lastEvent.spawnY, 2.0f, 0.001f);
    EXPECT_NEAR(lastEvent.spawnZ, -13.0f, 0.001f);

    // The loop must not keep firing once the respawn has happened.
    respawn.Update(10.0f);
    EXPECT_EQ(respawnCount, 1);
}

TEST(FPSRespawn_RespawnWithoutAPendingDeathDoesNothing)
{
    EventBus bus;
    int respawnCount = 0;
    auto subscription =
        bus.Subscribe<PlayerRespawnEvent>([&respawnCount](const PlayerRespawnEvent&) { respawnCount++; });

    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    respawn.SetEventBus(&bus);

    EXPECT_FALSE(respawn.RespawnPlayer());
    EXPECT_EQ(respawnCount, 0);
}

TEST(FPSRespawn_ManualRespawnPublishesAndClearsThePendingDeath)
{
    EventBus bus;
    int respawnCount = 0;
    auto subscription =
        bus.Subscribe<PlayerRespawnEvent>([&respawnCount](const PlayerRespawnEvent&) { respawnCount++; });

    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    respawn.SetEventBus(&bus);
    respawn.SetAutoRespawn(false);
    respawn.OnPlayerDeath("Enemy", "Rifle", true);

    // Auto-respawn is off, so time alone must not respawn the player.
    respawn.Update(60.0f);
    EXPECT_EQ(respawnCount, 0);
    EXPECT_TRUE(respawn.IsWaitingForRespawn());

    EXPECT_TRUE(respawn.RespawnPlayer());
    EXPECT_EQ(respawnCount, 1);
    EXPECT_FALSE(respawn.IsWaitingForRespawn());
}

// ============================================================================
// FPS state-validation predicates (mod-fps-12)
// ============================================================================

TEST(FPSStateRules_DeadActorMovingIsReported)
{
    EXPECT_TRUE(FPSStateRules::DeadActorIsMoving(0.0f, 3.0f, 0.0f));
    EXPECT_TRUE(FPSStateRules::DeadActorIsMoving(-10.0f, 0.0f, -2.5f));
}

TEST(FPSStateRules_LivingOrStationaryActorsAreNotReported)
{
    EXPECT_FALSE(FPSStateRules::DeadActorIsMoving(100.0f, 12.0f, 12.0f));
    EXPECT_FALSE(FPSStateRules::DeadActorIsMoving(0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(FPSStateRules::DeadActorIsMoving(0.0f, 0.5f, 0.5f));
}

TEST(FPSStateRules_DeadActorStillActiveIsReported)
{
    EXPECT_TRUE(FPSStateRules::DeadActorStillActive(0.0f, true));
    EXPECT_FALSE(FPSStateRules::DeadActorStillActive(0.0f, false));
    EXPECT_FALSE(FPSStateRules::DeadActorStillActive(50.0f, true));
}

// ============================================================================
// Progression restore (mod-fps-22)
// ============================================================================

TEST(FPSProgression_RestoreRebuildsLevelAndUnlocksFromSavedXP)
{
    ProgressionSystem earned;
    earned.Initialize();
    earned.AwardXP(6000, "test");

    const int earnedLevel = earned.GetLevel();
    const int earnedXP = earned.GetCurrentXP();
    const size_t earnedUnlocks = earned.GetEarnedUnlocks().size();
    EXPECT_GT(earnedLevel, 1);

    ProgressionSystem restored;
    restored.Initialize();
    restored.RestoreProgress(earnedXP);

    EXPECT_EQ(restored.GetLevel(), earnedLevel);
    EXPECT_EQ(restored.GetCurrentXP(), earnedXP);
    EXPECT_EQ(static_cast<int>(restored.GetEarnedUnlocks().size()), static_cast<int>(earnedUnlocks));
}

TEST(FPSProgression_RestoreDoesNotFireLevelUpCallbacks)
{
    ProgressionSystem restored;
    restored.Initialize();

    int levelUpCalls = 0;
    restored.GetCallbacks().onLevelUp = [&levelUpCalls](int, const LevelBonuses&) { levelUpCalls++; };

    restored.RestoreProgress(6000);
    EXPECT_GT(restored.GetLevel(), 1);
    EXPECT_EQ(levelUpCalls, 0);

    // The live callback must survive the restore and still fire during play.
    restored.AwardXP(100000, "test");
    EXPECT_GT(levelUpCalls, 0);
}

TEST(FPSProgression_RestoreClampsNegativeXP)
{
    ProgressionSystem restored;
    restored.Initialize();
    restored.RestoreProgress(-500);

    EXPECT_EQ(restored.GetLevel(), 1);
    EXPECT_EQ(restored.GetCurrentXP(), 0);
}
