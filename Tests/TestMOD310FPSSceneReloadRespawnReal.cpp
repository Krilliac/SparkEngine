/**
 * @file TestMOD310FPSSceneReloadRespawnReal.cpp
 * @brief MOD-310 / #581: authored FPS spawns must drive the death -> respawn
 *        path, and a console scene_load must rebind them without discarding
 *        the live match state.
 *
 * Every test drives the production RespawnSystem compiled from
 * GameModules/SparkGameFPS/Source/Game/GameMechanicsRespawn.cpp and the real
 * SceneManager legacy-scene parser; nothing here re-implements module logic.
 *
 * Game::RefreshAuthoredSceneRuntimeState (scene_load) re-runs
 * Game::InitializeRespawnAndVehicles, which binds spawns through
 * RespawnSystem::CollectAuthoredSpawnPoints + BindSpawnPoints. Before this fix
 * that path destroyed and recreated the RespawnSystem, so a scene_load during
 * the respawn countdown silently dropped the pending death (the player stayed
 * dead forever) and zeroed the scoreboard.
 */

#include "TestFramework.h"

#include "Game/FPSAssetPaths.h"
#include "Game/GameMechanics.h"

#include "Engine/Events/EventSystem.h"
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "SceneManager/SceneManager.h"
#include "Utils/EventBus.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Spark;

namespace
{
    std::filesystem::path MakeMod310TempDir(const std::string& tag)
    {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error) /
                                           ("spark_mod310_" + tag + "_" + std::to_string(::time(nullptr)));
        std::filesystem::remove_all(base, error);
        std::filesystem::create_directories(base, error);
        return base;
    }

    void RemoveMod310Tree(const std::filesystem::path& path)
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    void WriteScene(const std::filesystem::path& path, const std::string& body)
    {
        std::ofstream file(path);
        file << "[Scene]\nname=" << path.stem().string() << "\n"
             << "[Object]\ntype=plane\nname=Floor\nposition=0,0,0\n"
             << body;
    }
} // namespace

// ============================================================================
// Authored spawn collection from the real legacy scene parser
// ============================================================================

TEST(FPSRespawn_CollectsShippedLevelDefaultSpawnsOnly)
{
    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    ASSERT_TRUE(scene.LoadScene(FPSAssets::Resolve(L"Scenes/level1.scene")));

    const std::vector<RespawnPoint> spawns = RespawnSystem::CollectAuthoredSpawnPoints(scene);
    // level1.scene authors four tag=default player spawns and ten wave_spawn
    // entries; wave spawns belong to the WaveSpawner, never to the player.
    ASSERT_EQ(static_cast<int>(spawns.size()), 4);
    EXPECT_EQ(spawns[0].name, std::string("North_Spawn"));
    EXPECT_EQ(spawns[0].priority, 1);

    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    EXPECT_EQ(respawn.BindSpawnPoints(spawns), 4);
    const RespawnPoint preferred = respawn.GetBestSpawnPoint(-1);
    EXPECT_EQ(preferred.name, std::string("North_Spawn"));
    EXPECT_NEAR(preferred.position.z, -20.0f, 0.001f);
}

TEST(FPSRespawn_CollectSkipsMalformedPriorityAndWaveSpawns)
{
    // (An untagged [SpawnPoint] already fails the whole SceneManager load, so
    // the collector only ever sees tagged entries.)
    const std::filesystem::path temp = MakeMod310TempDir("collect");
    const std::filesystem::path scenePath = temp / "collect.scene";
    WriteScene(scenePath, "[SpawnPoint]\nname=BadPriority\ntag=default\npriority=high\nposition=1,2,3\n"
                          "[SpawnPoint]\nname=TrailingPriority\ntag=default\npriority=4x\nposition=4,2,6\n"
                          "[SpawnPoint]\nname=Wave\ntag=wave_spawn\nposition=7,2,9\n"
                          "[SpawnPoint]\nname=Good\ntag=default\npriority=3\nposition=10,2,12\n"
                          "rotation=5,90,0\n");

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    ASSERT_TRUE(scene.LoadScene(scenePath.wstring()));

    const std::vector<RespawnPoint> spawns = RespawnSystem::CollectAuthoredSpawnPoints(scene);
    ASSERT_EQ(static_cast<int>(spawns.size()), 1);
    EXPECT_EQ(spawns[0].name, std::string("Good"));
    EXPECT_EQ(spawns[0].priority, 3);
    EXPECT_NEAR(spawns[0].rotation.x, 5.0f, 0.001f);
    EXPECT_NEAR(spawns[0].rotation.y, 90.0f, 0.001f);

    RemoveMod310Tree(temp);
}

// ============================================================================
// Death -> respawn uses the authored spawn, including its rotation
// ============================================================================

TEST(FPSRespawn_DeathRespawnReportsAuthoredSpawnRotation)
{
    const std::filesystem::path temp = MakeMod310TempDir("rotation");
    const std::filesystem::path scenePath = temp / "rotation.scene";
    WriteScene(scenePath, "[SpawnPoint]\nname=Facing_East\ntag=default\npriority=2\nposition=7,2,-17\n"
                          "rotation=10,90,0\n");

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    ASSERT_TRUE(scene.LoadScene(scenePath.wstring()));

    EventBus bus;
    int respawnCount = 0;
    PlayerRespawnEvent lastEvent{};
    auto subscription = bus.Subscribe<PlayerRespawnEvent>(
        [&respawnCount, &lastEvent](const PlayerRespawnEvent& e)
        {
            ++respawnCount;
            lastEvent = e;
        });

    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    respawn.SetEventBus(&bus);
    respawn.SetRespawnDelay(1.0f);
    ASSERT_EQ(respawn.BindSpawnPoints(RespawnSystem::CollectAuthoredSpawnPoints(scene)), 1);

    EXPECT_FALSE(respawn.HasLastRespawnPoint());
    respawn.OnPlayerDeath("Enemy", "Rifle", false);
    respawn.Update(1.5f);

    ASSERT_EQ(respawnCount, 1);
    EXPECT_NEAR(lastEvent.spawnX, 7.0f, 0.001f);
    EXPECT_NEAR(lastEvent.spawnZ, -17.0f, 0.001f);
    // PlayerRespawnEvent carries only a position; the host reads the facing
    // from the spawn the respawn actually used.
    ASSERT_TRUE(respawn.HasLastRespawnPoint());
    const RespawnPoint& used = respawn.GetLastRespawnPoint();
    EXPECT_EQ(used.name, std::string("Facing_East"));
    EXPECT_NEAR(used.rotation.x, 10.0f, 0.001f);
    EXPECT_NEAR(used.rotation.y, 90.0f, 0.001f);

    RemoveMod310Tree(temp);
}

// ============================================================================
// scene_load rebind keeps the live match state
// ============================================================================

TEST(FPSRespawn_SceneReloadRebindKeepsPendingDeathScoreAndSettings)
{
    const std::filesystem::path temp = MakeMod310TempDir("reload");
    const std::filesystem::path firstPath = temp / "first.scene";
    const std::filesystem::path secondPath = temp / "second.scene";
    WriteScene(firstPath, "[SpawnPoint]\nname=First_Spawn\ntag=default\npriority=1\nposition=1,2,-5\n");
    WriteScene(secondPath, "[SpawnPoint]\nname=Second_Spawn\ntag=default\npriority=1\nposition=4,2,-19\n"
                           "rotation=0,180,0\n");

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    ASSERT_TRUE(scene.LoadScene(firstPath.wstring()));

    EventBus bus;
    int respawnCount = 0;
    PlayerRespawnEvent lastEvent{};
    auto subscription = bus.Subscribe<PlayerRespawnEvent>(
        [&respawnCount, &lastEvent](const PlayerRespawnEvent& e)
        {
            ++respawnCount;
            lastEvent = e;
        });

    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    respawn.SetEventBus(&bus);
    respawn.SetRespawnDelay(3.0f);
    ASSERT_EQ(respawn.BindSpawnPoints(RespawnSystem::CollectAuthoredSpawnPoints(scene)), 1);

    respawn.GetPlayerScore().kills = 6;
    respawn.OnPlayerDeath("Enemy", "Rifle", true);
    respawn.Update(1.0f);
    ASSERT_TRUE(respawn.IsWaitingForRespawn());

    // Console scene_load mid-countdown: the new scene's spawns replace the old
    // ones, but the death, the countdown, the score, and the delay are live
    // match state and must survive.
    ASSERT_TRUE(scene.LoadScene(secondPath.wstring()));
    ASSERT_EQ(respawn.BindSpawnPoints(RespawnSystem::CollectAuthoredSpawnPoints(scene)), 1);

    EXPECT_TRUE(respawn.IsWaitingForRespawn());
    EXPECT_NEAR(respawn.GetRespawnTimeRemaining(), 2.0f, 0.001f);
    EXPECT_NEAR(respawn.GetRespawnDelay(), 3.0f, 0.001f);
    EXPECT_EQ(respawn.GetPlayerScore().kills, 6);
    EXPECT_EQ(respawn.GetPlayerScore().deaths, 1);
    EXPECT_EQ(static_cast<int>(respawn.GetKillHistory().size()), 1);
    ASSERT_EQ(static_cast<int>(respawn.GetSpawnPoints().size()), 1);
    EXPECT_EQ(respawn.GetSpawnPoints()[0].name, std::string("Second_Spawn"));

    respawn.Update(2.5f);
    ASSERT_EQ(respawnCount, 1);
    EXPECT_NEAR(lastEvent.spawnX, 4.0f, 0.001f);
    EXPECT_NEAR(lastEvent.spawnZ, -19.0f, 0.001f);
    ASSERT_TRUE(respawn.HasLastRespawnPoint());
    EXPECT_NEAR(respawn.GetLastRespawnPoint().rotation.y, 180.0f, 0.001f);

    RemoveMod310Tree(temp);
}

TEST(FPSRespawn_RebindWithoutAuthoredSpawnsRestoresFallbackNotStaleSpawn)
{
    RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());

    RespawnPoint authored;
    authored.name = "Old_Level_Spawn";
    authored.position = {50.0f, 2.0f, 50.0f};
    ASSERT_EQ(respawn.BindSpawnPoints({authored}), 1);
    ASSERT_EQ(respawn.GetBestSpawnPoint(-1).name, std::string("Old_Level_Spawn"));

    // A replacement scene with no valid player spawns must not keep teleporting
    // the player into the previous level's coordinates.
    EXPECT_EQ(respawn.BindSpawnPoints({}), 0);
    const RespawnPoint fallback = respawn.GetBestSpawnPoint(-1);
    EXPECT_EQ(fallback.name, std::string("Default Spawn"));
    EXPECT_NEAR(fallback.position.z, -20.0f, 0.001f);
    EXPECT_EQ(static_cast<int>(respawn.GetSpawnPoints().size()), 1);
}

// ============================================================================
// Headless arena: data-only SceneManager without GraphicsEngine or InputManager
// ============================================================================

TEST(FPSScene_DataOnlyLoadNeedsNoGraphicsOrInput)
{
    // The headless FPS arena constructs SceneManager with null graphics and
    // input; the constructor used to reject that before any load could run.
    SceneManager scene(nullptr, nullptr);
    ASSERT_TRUE(scene.LoadScene(FPSAssets::Resolve(L"Scenes/level1.scene")));
    EXPECT_TRUE(scene.GetNodeCount() > 0);
    EXPECT_EQ(scene.GetObjects().size(), static_cast<size_t>(scene.GetNodeCount()));
    for (const auto& object : scene.GetObjects())
        EXPECT_TRUE(object == nullptr);
    EXPECT_EQ(RespawnSystem::CollectAuthoredSpawnPoints(scene).size(), static_cast<size_t>(4));

    // The legacy space-delimited format needs a device and must be refused,
    // leaving the loaded arena intact.
    const std::filesystem::path temp = MakeMod310TempDir("data_only_legacy");
    const std::filesystem::path legacyPath = temp / "legacy.scene";
    {
        std::ofstream legacy(legacyPath);
        legacy << "Cube 0 0 0 1\n";
    }
    const int arenaNodes = scene.GetNodeCount();
    EXPECT_FALSE(scene.LoadScene(legacyPath.wstring()));
    EXPECT_EQ(scene.GetNodeCount(), arenaNodes);
    RemoveMod310Tree(temp);
}
