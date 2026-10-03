#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "SceneManager/SceneManager.h"

#include <nlohmann_json.h>
#include <chrono>
#include <filesystem>
#include <fstream>

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
    document["entities"][0]["components"].push_back({{"type", "UnknownGameplay"}, {"fields", {}}});
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
    document["entities"][1]["parent"] = document["entities"][0]["id"];
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
