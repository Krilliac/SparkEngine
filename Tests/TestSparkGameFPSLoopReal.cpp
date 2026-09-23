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
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "SceneManager/SceneManager.h"
#include "Utils/EventBus.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
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

TEST(FPSAssets_ResolveScenePathAcceptsPackageRelativeForms)
{
    std::filesystem::path resolved;
    std::string error;
    ASSERT_TRUE(FPSAssets::ResolveScenePath("level1.scene", resolved, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(resolved == std::filesystem::weakly_canonical(FPSAssets::Root() / "Scenes" / "level1.scene"));

    std::filesystem::path prefixed;
    ASSERT_TRUE(FPSAssets::ResolveScenePath("Assets/Scenes/level1.scene", prefixed, error));
    EXPECT_TRUE(prefixed == resolved);
}

TEST(FPSAssets_ResolveScenePathRejectsEscapesAndUnsupportedFiles)
{
    std::filesystem::path resolved;
    std::string error;
    EXPECT_FALSE(FPSAssets::ResolveScenePath("../Models/pistol.obj", resolved, error));
    EXPECT_TRUE(!error.empty());
    EXPECT_FALSE(FPSAssets::ResolveScenePath("Scenes/../Models/pistol.obj", resolved, error));
    EXPECT_TRUE(!error.empty());
    EXPECT_FALSE(FPSAssets::ResolveScenePath("C:/outside.scene", resolved, error));
    EXPECT_STR_CONTAINS(error, "absolute");
    EXPECT_FALSE(FPSAssets::ResolveScenePath("missing.scene", resolved, error));
    EXPECT_STR_CONTAINS(error, "does not exist");
    EXPECT_FALSE(FPSAssets::ResolveScenePath("level1.json", resolved, error));
    EXPECT_STR_CONTAINS(error, ".scene");
    EXPECT_FALSE(FPSAssets::ResolveScenePath("Scenes/不存在.scene", resolved, error));
    EXPECT_STR_CONTAINS(error, "does not exist");
    EXPECT_FALSE(FPSAssets::ResolveScenePath(std::string(4097, 'a'), resolved, error));
    EXPECT_STR_CONTAINS(error, "too long");
}

// ============================================================================
// Death -> respawn -> score loop (mod-fps-02, mod-fps-21)
// ============================================================================

TEST(FPSScene_AuthoredCameraAndSpawnPointsSurviveHeadlessLoad)
{
    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    ASSERT_TRUE(scene.LoadScene(FPSAssets::Resolve(L"Scenes/level1.scene")));

    const int cameraIndex = scene.FindNode("MainCamera");
    ASSERT_TRUE(cameraIndex >= 0);
    const SceneNode* camera = scene.GetNode(cameraIndex);
    ASSERT_TRUE(camera != nullptr);
    EXPECT_EQ(camera->type, std::string("Camera"));
    EXPECT_NEAR(camera->position.x, 0.0f, 0.001f);
    EXPECT_NEAR(camera->position.y, 2.0f, 0.001f);
    EXPECT_NEAR(camera->position.z, -20.0f, 0.001f);

    int defaultSpawns = 0;
    int waveSpawns = 0;
    for (int i = 0; i < scene.GetNodeCount(); ++i)
    {
        const SceneNode* node = scene.GetNode(i);
        if (!node || node->type != "SpawnPoint")
            continue;
        const auto tag = node->properties.find("tag");
        if (tag != node->properties.end() && tag->second == "default")
            ++defaultSpawns;
        else if (tag != node->properties.end() && tag->second == "wave_spawn")
            ++waveSpawns;
    }
    EXPECT_EQ(defaultSpawns, 4);
    EXPECT_EQ(waveSpawns, 10);

    const int floorIndex = scene.FindNode("Arena_Floor_Main");
    ASSERT_TRUE(floorIndex >= 0);
    EXPECT_EQ(scene.GetNode(floorIndex)->type, std::string("plane"));
    EXPECT_EQ(scene.GetObjects().size(), static_cast<size_t>(scene.GetNodeCount()));
    EXPECT_TRUE(std::all_of(scene.GetObjects().begin(), scene.GetObjects().end(),
                            [](const auto& object) { return object == nullptr; }));
}

TEST(FPSScene_MalformedAuthoredLocationsCannotBecomeOriginSpawns)
{
    const std::filesystem::path temp = MakeTempDir("invalid_locations");
    const std::filesystem::path scenePath = temp / "invalid.scene";
    {
        std::ofstream sceneFile(scenePath);
        sceneFile << "[Scene]\nname=Invalid locations\n"
                     "[Object]\ntype=plane\nname=Floor\nposition=0,0,0\n"
                     "[Object]\ntype=plane\nname=LegacyTrailing\nposition=1,2,3 # legacy annotation\n"
                     "[Camera]\nname=MalformedCamera\nposition=bad,2,-20\n"
                     "[Camera]\nname=MalformedRotation\nposition=0,2,-20\nrotation=0,NaN,0\n"
                     "[SpawnPoint]\nname=MalformedSpawn\ntag=default\nposition=0,2,wrong\n"
                     "[SpawnPoint]\nname=MalformedSpawnRotation\ntag=default\nposition=0,2,-10\n"
                     "rotation=0,bad,0\n"
                     "[SpawnPoint]\nname=NoTag\nposition=0,2,-10\n"
                     "[SpawnPoint]\nname=ValidSpawn\ntag=default\nposition=3,2,-10\n";
    }

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    ASSERT_TRUE(scene.LoadScene(scenePath.wstring()));
    EXPECT_TRUE(scene.FindNode("Floor") >= 0);
    const int legacyIndex = scene.FindNode("LegacyTrailing");
    ASSERT_TRUE(legacyIndex >= 0);
    EXPECT_NEAR(scene.GetNode(legacyIndex)->position.x, 1.0f, 0.001f);
    EXPECT_NEAR(scene.GetNode(legacyIndex)->position.y, 2.0f, 0.001f);
    EXPECT_NEAR(scene.GetNode(legacyIndex)->position.z, 3.0f, 0.001f);
    EXPECT_EQ(scene.FindNode("MalformedCamera"), -1);
    EXPECT_EQ(scene.FindNode("MalformedRotation"), -1);
    EXPECT_EQ(scene.FindNode("MalformedSpawn"), -1);
    EXPECT_EQ(scene.FindNode("MalformedSpawnRotation"), -1);
    EXPECT_EQ(scene.FindNode("NoTag"), -1);
    const int validIndex = scene.FindNode("ValidSpawn");
    ASSERT_TRUE(validIndex >= 0);
    EXPECT_NEAR(scene.GetNode(validIndex)->position.x, 3.0f, 0.001f);
    EXPECT_EQ(scene.GetObjects().size(), static_cast<size_t>(scene.GetNodeCount()));
    EXPECT_TRUE(std::all_of(scene.GetObjects().begin(), scene.GetObjects().end(),
                            [](const auto& object) { return object == nullptr; }));

    RemoveTree(temp);
}

TEST(FPSScene_CameraAndSpawnOnlyINIStillLoadsAsSceneData)
{
    const std::filesystem::path temp = MakeTempDir("camera_spawn_only");
    const std::filesystem::path scenePath = temp / "data.scene";
    {
        std::ofstream sceneFile(scenePath);
        sceneFile << "[Camera]\nname=OnlyCamera\nposition=7,3,-17\n"
                     "[SpawnPoint]\nname=OnlySpawn\ntag=default\nposition=4,2,-19\n";
    }

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    ASSERT_TRUE(scene.LoadScene(scenePath.wstring()));
    EXPECT_EQ(scene.GetNodeCount(), 2);
    const int cameraIndex = scene.FindNode("OnlyCamera");
    const int spawnIndex = scene.FindNode("OnlySpawn");
    ASSERT_TRUE(cameraIndex >= 0);
    ASSERT_TRUE(spawnIndex >= 0);
    EXPECT_NEAR(scene.GetNode(cameraIndex)->position.x, 7.0f, 0.001f);
    EXPECT_NEAR(scene.GetNode(spawnIndex)->position.x, 4.0f, 0.001f);
    EXPECT_EQ(scene.GetObjects().size(), static_cast<size_t>(scene.GetNodeCount()));
    RemoveTree(temp);
}

TEST(FPSRespawn_LowestIntegerPriorityStillSelectsAuthoredSpawn)
{
    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    respawn.RemoveSpawnPoint(0);

    RespawnPoint authored;
    authored.name = "Very low priority";
    authored.position = {4.0f, 2.0f, -19.0f};
    authored.priority = std::numeric_limits<int>::min();
    ASSERT_TRUE(respawn.AddSpawnPoint(authored) >= 0);

    const RespawnPoint selected = respawn.GetBestSpawnPoint(-1);
    EXPECT_EQ(selected.name, authored.name);
    EXPECT_NEAR(selected.position.x, 4.0f, 0.001f);
    EXPECT_NEAR(selected.position.z, -19.0f, 0.001f);
}

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
