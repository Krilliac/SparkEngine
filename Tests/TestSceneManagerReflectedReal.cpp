#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/Events/EventSystem.h"
#include "Game/GameMechanics.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "SceneManager/SceneManager.h"
#include "Utils/EventBus.h"

#include <nlohmann_json.h>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <utility>

namespace
{
    struct ReflectedGameplayFixture
    {
        std::filesystem::path root;
        std::filesystem::path scene;
        std::string original;

        ReflectedGameplayFixture()
        {
            root = std::filesystem::temp_directory_path() /
                   ("spark_reflected_gameplay_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root / "Assets/Meshes");
            scene = root / "Startup.sparkscene";
            std::ofstream(root / "Assets/Meshes/triangle.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
            World world;
            auto mesh = world.CreateEntity("Authored \"mesh\" \xc3\xa9");
            auto& transform = world.AddComponent<Transform>(mesh);
            transform.position = {3, 2, 5};
            transform.rotation = {0, 30, 0};
            transform.scale = {2, 3, 4};
            world.AddComponent<MeshRenderer>(mesh).meshPath = "Assets/Meshes/triangle.obj";
            auto camera = world.CreateEntity("Camera");
            world.AddComponent<Transform>(camera).position = {7, 11, -13};
            auto& lens = world.AddComponent<Camera>(camera);
            lens.isMainCamera = true;
            lens.fov = 70;
            ASSERT_TRUE(Spark::TrySerializeWorld(world, original));
            Write(original);
        }

        ~ReflectedGameplayFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const std::string& text) const { std::ofstream(scene, std::ios::binary) << text; }

        std::string WithSpawns(int count = 2) const
        {
            World world;
            ASSERT_TRUE(Spark::DeserializeInto(world, original, Spark::SceneDeserializeMode::StrictRecovery));
            for (int i = 0; i < count; ++i)
            {
                auto entity = world.CreateEntity("AuthoredSpawn" + std::to_string(i));
                auto& transform = world.AddComponent<Transform>(entity);
                transform.position = {7.0f + i, 11.0f, -13.0f};
                transform.rotation = {10.0f, 90.0f, 0.0f};
                auto& spawn = world.AddComponent<SpawnPointComponent>(entity);
                spawn.spawnRadius = 0.0f;
                spawn.respawnDelay = 0.0f;
                spawn.priority = i;
            }
            std::string result;
            ASSERT_TRUE(Spark::TrySerializeWorld(world, result));
            return result;
        }

        void ExpectRejectedWithRollback(const std::string& text)
        {
            SceneManager manager(nullptr, nullptr);
            ASSERT_TRUE(manager.LoadScene(scene.wstring()));
            manager.MarkDirty();
            const auto oldPath = manager.GetCurrentFilePath();
            const auto oldName = manager.GetNode(0)->name;
            Write(text);
            ASSERT_FALSE(manager.LoadScene(scene.wstring()));
            EXPECT_EQ(manager.GetNodeCount(), 2);
            EXPECT_EQ(manager.GetNode(0)->name, oldName);
            EXPECT_TRUE(manager.IsDirty());
            EXPECT_TRUE(manager.GetCurrentFilePath() == oldPath);
        }
    };
} // namespace

TEST(SceneManager_ReflectedGameplayPreservesMeshTransformAndCamera)
{
    ReflectedGameplayFixture fixture;
    SceneManager scene(nullptr, nullptr);
    ASSERT_TRUE(scene.LoadScene(fixture.scene.wstring()));
    ASSERT_EQ(scene.GetNodeCount(), 2);
    const auto* mesh = scene.GetNode(scene.FindNode("Authored \"mesh\" \xc3\xa9"));
    ASSERT_TRUE(mesh != nullptr);
    EXPECT_EQ(mesh->position.x, 3.0f);
    EXPECT_EQ(mesh->rotation.y, 30.0f);
    EXPECT_EQ(mesh->scale.z, 4.0f);
    const auto* camera = scene.GetNode(scene.FindNode("Camera"));
    ASSERT_TRUE(camera != nullptr);
    EXPECT_EQ(camera->position.z, -13.0f);
    EXPECT_EQ(camera->properties.at("fov"), std::string("70.000000"));
}

TEST(SceneManager_ReflectedGameplayRejectsMalformedPrimaryDespiteBackup)
{
    ReflectedGameplayFixture fixture;
    std::ofstream(fixture.scene.string() + ".bak") << fixture.original;
    fixture.ExpectRejectedWithRollback("{broken");
}

TEST(SceneManager_ReflectedGameplayRejectsUnknownComponent)
{
    ReflectedGameplayFixture fixture;
    auto document = nlohmann::json::parse(fixture.original);
    document["entities"][std::size_t{0}]["components"].push_back({{"type", "UnknownGameplay"}, {"fields", {}}});
    fixture.ExpectRejectedWithRollback(document.dump());
}

TEST(SceneManager_ReflectedGameplayRejectsMissingMesh)
{
    ReflectedGameplayFixture fixture;
    SceneManager scene(nullptr, nullptr);
    ASSERT_TRUE(scene.LoadScene(fixture.scene.wstring()));
    const auto oldPath = scene.GetCurrentFilePath();
    std::filesystem::remove(fixture.root / "Assets/Meshes/triangle.obj");
    EXPECT_FALSE(scene.LoadScene(fixture.scene.wstring()));
    EXPECT_EQ(scene.GetNodeCount(), 2);
    EXPECT_TRUE(scene.GetCurrentFilePath() == oldPath);
}

TEST(SceneManager_ReflectedGameplayRejectsHierarchy)
{
    ReflectedGameplayFixture fixture;
    auto document = nlohmann::json::parse(fixture.original);
    document["entities"][1]["parent"] = document["entities"][std::size_t{0}]["id"];
    fixture.ExpectRejectedWithRollback(document.dump());
}

TEST(SceneManager_ReflectedGameplayRejectsMaterialLoss)
{
    ReflectedGameplayFixture fixture;
    auto document = nlohmann::json::parse(fixture.original);
    for (auto& entity : document["entities"])
        for (auto& component : entity["components"])
            if (component["type"] == "MeshRenderer")
                component["fields"]["materialPath"] = "Assets/Materials/authored.json";
    fixture.ExpectRejectedWithRollback(document.dump());
}

TEST(SceneManager_ReflectedGameplaySpawnSelectionAndRespawnUseAuthoredPoint)
{
    ReflectedGameplayFixture fixture;
    fixture.Write(fixture.WithSpawns());
    SceneManager scene(nullptr, nullptr);
    ASSERT_TRUE(scene.LoadScene(fixture.scene.wstring()));
    Spark::RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    ASSERT_EQ(respawn.BindSpawnPoints(Spark::RespawnSystem::CollectAuthoredSpawnPoints(scene)), 2);
    // StartWaves (F11's production action) uses this exact selector. This is a
    // real-class selection/event test, not keyboard or rendered-game evidence.
    const auto selected = respawn.GetBestSpawnPoint(-1);
    EXPECT_EQ(selected.name, std::string("AuthoredSpawn1"));
    EXPECT_EQ(selected.position.x, 8.0f);
    EXPECT_EQ(selected.position.y, 11.0f);
    EXPECT_EQ(selected.position.z, -13.0f);
    EXPECT_EQ(selected.rotation.y, 90.0f);

    Spark::EventBus bus;
    Spark::PlayerRespawnEvent event{};
    int published = 0;
    auto subscription = bus.Subscribe<Spark::PlayerRespawnEvent>(
        [&](const Spark::PlayerRespawnEvent& value)
        {
            event = value;
            ++published;
        });
    respawn.SetEventBus(&bus);
    respawn.OnPlayerDeath("Enemy", "Rifle", false);
    const float delay = respawn.GetRespawnDelay();
    EXPECT_TRUE(delay > 0.0f); // Point cooldown=0 must not override the death timer.
    respawn.Update(delay / 2.0f);
    EXPECT_EQ(published, 0);
    respawn.Update(delay);
    EXPECT_EQ(published, 1);
    EXPECT_EQ(event.spawnX, selected.position.x);
    EXPECT_EQ(event.spawnY, selected.position.y);
    EXPECT_EQ(event.spawnZ, selected.position.z);
    ASSERT_TRUE(respawn.HasLastRespawnPoint());
    EXPECT_EQ(respawn.GetLastRespawnPoint().rotation.y, selected.rotation.y);
    auto tied = Spark::RespawnSystem::CollectAuthoredSpawnPoints(scene);
    for (auto& spawn : tied)
        spawn.priority = 4;
    ASSERT_EQ(respawn.BindSpawnPoints(tied), 2);
    EXPECT_EQ(respawn.GetBestSpawnPoint(-1).name, tied.front().name);
}

TEST(SceneManager_ReflectedGameplayRejectsUnsupportedSpawnSemantics)
{
    ReflectedGameplayFixture fixture;
    const std::pair<const char*, const char*> unsupported[] = {
        {"spawnTag", "wave_spawn"}, {"enabled", "false"},  {"teamID", "1"},
        {"spawnRadius", "1"},       {"respawnDelay", "5"}, {"maxConcurrent", "1"}};
    for (const auto& [field, value] : unsupported)
    {
        fixture.Write(fixture.original);
        auto document = nlohmann::json::parse(fixture.WithSpawns(1));
        for (auto& entity : document["entities"])
            for (auto& component : entity["components"])
                if (component["type"] == "SpawnPointComponent")
                    component["fields"][field] = value;
        fixture.ExpectRejectedWithRollback(document.dump());
    }
    // Missing fields cannot silently become supported zero-radius/cooldown values.
    fixture.Write(fixture.original);
    auto omitted = nlohmann::json::parse(fixture.WithSpawns(1));
    for (auto& entity : omitted["entities"])
        for (auto& component : entity["components"])
            if (component["type"] == "SpawnPointComponent")
                component["fields"].erase("spawnRadius");
    fixture.ExpectRejectedWithRollback(omitted.dump());
}

TEST(SceneManager_ReflectedGameplayRejectsSpawnTruncationAndMeshlessScene)
{
    ReflectedGameplayFixture fixture;
    fixture.Write(fixture.WithSpawns(32));
    SceneManager boundary(nullptr, nullptr);
    ASSERT_TRUE(boundary.LoadScene(fixture.scene.wstring()));
    Spark::RespawnSystem respawn;
    ASSERT_TRUE(respawn.Initialize());
    EXPECT_EQ(respawn.BindSpawnPoints(Spark::RespawnSystem::CollectAuthoredSpawnPoints(boundary)), 32);
    fixture.Write(fixture.original);
    fixture.ExpectRejectedWithRollback(fixture.WithSpawns(33));
    fixture.Write(fixture.original);
    auto document = nlohmann::json::parse(fixture.WithSpawns(1));
    auto& entities = document["entities"];
    for (auto it = entities.begin(); it != entities.end();)
    {
        bool mesh = false;
        for (const auto& component : (*it)["components"])
            mesh = mesh || component["type"] == "MeshRenderer";
        if (mesh)
            it = entities.erase(it);
        else
            ++it;
    }
    fixture.ExpectRejectedWithRollback(document.dump());
}

TEST(SceneManager_ReflectedGameplayRejectsSpawnShapeAndUnexpectedFields)
{
    ReflectedGameplayFixture fixture;
    for (int variant = 0; variant < 4; ++variant)
    {
        World world;
        ASSERT_TRUE(Spark::DeserializeInto(world, fixture.WithSpawns(1), Spark::SceneDeserializeMode::StrictRecovery));
        for (auto entity : world.GetEntitiesWith<SpawnPointComponent>())
        {
            if (variant == 0)
                world.GetComponent<Transform>(entity)->scale = {2, 1, 1};
            else if (variant == 1)
                world.GetComponent<Transform>(entity)->rotation.x = 90.0f;
            else if (variant == 2)
                world.AddComponent<MeshRenderer>(entity).meshPath = "Assets/Meshes/triangle.obj";
            else
                world.AddComponent<Camera>(entity).isMainCamera = true;
        }
        std::string serialized;
        ASSERT_TRUE(Spark::TrySerializeWorld(world, serialized));
        fixture.Write(fixture.original);
        fixture.ExpectRejectedWithRollback(serialized);
    }
    fixture.Write(fixture.original);
    auto runtimeField = nlohmann::json::parse(fixture.WithSpawns(1));
    for (auto& entity : runtimeField["entities"])
        for (auto& component : entity["components"])
            if (component["type"] == "SpawnPointComponent")
                component["fields"]["lastUsedTime"] = "0";
    fixture.ExpectRejectedWithRollback(runtimeField.dump());
}
