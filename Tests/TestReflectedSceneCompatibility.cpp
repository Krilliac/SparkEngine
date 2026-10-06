// TestReflectedSceneCompatibility.cpp - SAVE-230 / OD-03 on-disk compatibility
// fixtures for the reflected-World scene dialect (editor File > Open/Save and
// -scene, SceneManager/ReflectedSceneSerializer.h).
//
// The reflected dialect reads `version: 1` and the pre-reflection editor
// `sceneVersion: 1` dialect, and writes `version: 1` only. Every test here reads
// a committed file from Tests/Fixtures/Compatibility/ReflectedScene in place
// through the production LoadWorld path and requires that the file is never
// rewritten and that no recovery or staging sibling appears next to it.

#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CollisionMaskComponents.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <nlohmann_json.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Spark;

namespace
{
    /// The narrow SPARK_TEST_SOURCE_DIR literal would be decoded with the ANSI code
    /// page on Windows; the wide one keeps a non-ASCII checkout path intact.
    std::filesystem::path ReflectedFixture(const char* name)
    {
#if defined(_WIN32) && defined(SPARK_TEST_SOURCE_DIR_WIDE)
        const std::filesystem::path sourceDir(SPARK_TEST_SOURCE_DIR_WIDE);
#else
        const std::filesystem::path sourceDir(SPARK_TEST_SOURCE_DIR);
#endif
        return sourceDir / "Tests" / "Fixtures" / "Compatibility" / "ReflectedScene" / name;
    }

    /// LoadWorld/SaveWorld take UTF-8; path::string() is the ANSI code page on Windows.
    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::filesystem::path WithSuffix(std::filesystem::path path, const char* suffix)
    {
        path += suffix;
        return path;
    }

    std::string ReadFixtureText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    /// LoadWorld reads `<path>.bak` as a fallback; reading a fixture must never create
    /// a backup or staging file beside it.
    bool HasRecoverySiblings(const std::filesystem::path& path)
    {
        return std::filesystem::exists(WithSuffix(path, ".bak")) || std::filesystem::exists(WithSuffix(path, ".tmp")) ||
               std::filesystem::exists(WithSuffix(path, ".bak.tmp"));
    }

    class TemporaryReflectedSceneCopy
    {
      public:
        TemporaryReflectedSceneCopy()
        {
            const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path() /
                     ("spark_reflected_compat_" + std::to_string(nonce) + ".sparkscene");
        }

        ~TemporaryReflectedSceneCopy()
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
            std::filesystem::remove(WithSuffix(m_path, ".bak"), ignored);
            std::filesystem::remove(WithSuffix(m_path, ".tmp"), ignored);
            std::filesystem::remove(WithSuffix(m_path, ".bak.tmp"), ignored);
        }

        TemporaryReflectedSceneCopy(const TemporaryReflectedSceneCopy&) = delete;
        TemporaryReflectedSceneCopy& operator=(const TemporaryReflectedSceneCopy&) = delete;

        const std::filesystem::path& Path() const { return m_path; }

      private:
        std::filesystem::path m_path;
    };

    EntityID FindNamed(World& world, const std::string& name)
    {
        for (auto entity : world.GetEntitiesWith<NameComponent>())
        {
            if (world.GetComponent<NameComponent>(entity)->name == name)
                return entity;
        }
        return entt::null;
    }

    /// The entity with its components array ordered by component type. The loader
    /// matches components by type, so their order within an entity is not part of
    /// the format: v1-reflected-courtyard was written by a libstdc++ build whose
    /// unordered_map iteration order differs from MSVC's.
    nlohmann::json WithComponentsByType(const nlohmann::json& entity)
    {
        std::vector<nlohmann::json> components(entity["components"].begin(), entity["components"].end());
        std::stable_sort(components.begin(), components.end(), [](const nlohmann::json& a, const nlohmann::json& b)
                         { return a["type"].get<std::string>() < b["type"].get<std::string>(); });
        nlohmann::json ordered = nlohmann::json::array();
        for (const nlohmann::json& component : components)
            ordered.push_back(component);
        nlohmann::json canonical = entity;
        canonical["components"] = ordered;
        return canonical;
    }

    /// True when every entity's components array is in ascending type order, the
    /// deterministic order SerializeWorld writes on every platform.
    bool ComponentsWrittenInTypeOrder(const nlohmann::json& document)
    {
        for (const nlohmann::json& entity : document["entities"])
        {
            const nlohmann::json& components = entity["components"];
            for (size_t index = 1; index < components.size(); ++index)
            {
                if (!(components[index - 1]["type"].get<std::string>() < components[index]["type"].get<std::string>()))
                    return false;
            }
        }
        return true;
    }

    /// True when both reflected documents declare the same entities with identical
    /// JSON, matched by serialized id. Entity array order follows ECS storage order
    /// and component order within an entity is keyed by type; neither is part of
    /// the format. Every entity field and every component's fields must match exactly.
    bool SameEntitiesById(const nlohmann::json& expected, const nlohmann::json& actual)
    {
        if (expected["entities"].size() != actual["entities"].size())
            return false;
        for (const nlohmann::json& declared : expected["entities"])
        {
            bool matched = false;
            for (const nlohmann::json& written : actual["entities"])
            {
                if (written["id"] == declared["id"])
                    matched = WithComponentsByType(written) == WithComponentsByType(declared);
            }
            if (!matched)
                return false;
        }
        return true;
    }
} // namespace


TEST(SceneMigration_ReflectedV1FixtureLoadsDeclaredStateWithoutRewritingSource)
{
    const std::filesystem::path fixturePath = ReflectedFixture("v1-reflected-courtyard.sparkscene");
    const std::string fixtureBefore = ReadFixtureText(fixturePath);
    ASSERT_TRUE(fixtureBefore.find("\"version\": 1") != std::string::npos);
    ASSERT_FALSE(HasRecoverySiblings(fixturePath));

    World world;
    std::string error;
    ASSERT_TRUE(LoadWorld(world, PathToUtf8(fixturePath), &error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(4));

    // Serialized ids are restored as live entity ids.
    const EntityID root = FindNamed(world, "Courtyard Root");
    const EntityID fountain = FindNamed(world, "Fountain");
    const EntityID sun = FindNamed(world, "Sun");
    const EntityID camera = FindNamed(world, "Main Camera");
    ASSERT_TRUE(root != entt::null && fountain != entt::null && sun != entt::null && camera != entt::null);
    EXPECT_EQ(static_cast<uint32_t>(root), 0u);
    EXPECT_EQ(static_cast<uint32_t>(fountain), 1u);
    EXPECT_EQ(static_cast<uint32_t>(sun), 2u);
    EXPECT_EQ(static_cast<uint32_t>(camera), 3u);

    const Transform* rootTransform = world.GetComponent<Transform>(root);
    ASSERT_TRUE(rootTransform != nullptr);
    EXPECT_NEAR(rootTransform->position.z, 3.0f, 0.0001f);
    EXPECT_NEAR(rootTransform->rotation.y, 45.0f, 0.0001f);
    EXPECT_NEAR(rootTransform->scale.x, 2.0f, 0.0001f);
    EXPECT_TRUE(rootTransform->parent == entt::null);

    const MeshRenderer* rootMesh = world.GetComponent<MeshRenderer>(root);
    ASSERT_TRUE(rootMesh != nullptr);
    EXPECT_EQ(rootMesh->meshPath, std::string("Assets/Models/courtyard.obj"));
    EXPECT_EQ(rootMesh->materialPath, std::string("Assets/Materials/stone.json"));
    EXPECT_FALSE(rootMesh->castShadows);
    EXPECT_TRUE(rootMesh->receiveShadows);

    const RigidBodyComponent* rootBody = world.GetComponent<RigidBodyComponent>(root);
    ASSERT_TRUE(rootBody != nullptr);
    EXPECT_TRUE(rootBody->type == RigidBodyComponent::Type::Static);
    EXPECT_NEAR(rootBody->friction, 0.8f, 0.0001f);

    const CollisionMaskComponent* rootMask = world.GetComponent<CollisionMaskComponent>(root);
    ASSERT_TRUE(rootMask != nullptr);
    EXPECT_EQ(rootMask->fromMask, CollisionLayer::Environment);
    EXPECT_EQ(rootMask->intoMask, CollisionLayer::Player | CollisionLayer::Enemy);

    // The parent edge is resolved through the World hierarchy, not only copied.
    const Transform* fountainTransform = world.GetComponent<Transform>(fountain);
    ASSERT_TRUE(fountainTransform != nullptr);
    EXPECT_TRUE(fountainTransform->parent == root);
    EXPECT_NEAR(fountainTransform->position.x, -4.5f, 0.0001f);
    EXPECT_NEAR(fountainTransform->position.y, 0.125f, 0.0001f);
    const LightComponent* fountainLight = world.GetComponent<LightComponent>(fountain);
    ASSERT_TRUE(fountainLight != nullptr);
    EXPECT_TRUE(fountainLight->type == LightComponent::Type::Point);
    EXPECT_NEAR(fountainLight->color.y, 0.4f, 0.0001f);
    EXPECT_NEAR(fountainLight->intensity, 3.5f, 0.0001f);
    EXPECT_NEAR(fountainLight->range, 12.0f, 0.0001f);

    const LightComponent* sunLight = world.GetComponent<LightComponent>(sun);
    ASSERT_TRUE(sunLight != nullptr);
    EXPECT_TRUE(sunLight->type == LightComponent::Type::Directional);
    EXPECT_TRUE(sunLight->castShadows);
    EXPECT_EQ(sunLight->shadowMapResolution, 2048);

    const Camera* cameraComponent = world.GetComponent<Camera>(camera);
    ASSERT_TRUE(cameraComponent != nullptr);
    EXPECT_NEAR(cameraComponent->fov, 75.0f, 0.0001f);
    EXPECT_NEAR(cameraComponent->nearPlane, 0.25f, 0.0001f);
    EXPECT_NEAR(cameraComponent->farPlane, 1500.0f, 0.0001f);
    EXPECT_TRUE(cameraComponent->isMainCamera);

    // Reading never rewrites the checked-in document or leaves recovery files beside it.
    EXPECT_EQ(ReadFixtureText(fixturePath), fixtureBefore);
    EXPECT_FALSE(HasRecoverySiblings(fixturePath));

    // Re-saving the loaded world reproduces every entity the fixture declares: the
    // current writer still emits exactly this version-1 document.
    TemporaryReflectedSceneCopy resaved;
    ASSERT_TRUE(SaveWorld(world, PathToUtf8(resaved.Path())));
    const nlohmann::json resavedDocument = nlohmann::json::parse(ReadFixtureText(resaved.Path()));
    const nlohmann::json fixtureDocument = nlohmann::json::parse(fixtureBefore);
    EXPECT_TRUE(resavedDocument["version"] == fixtureDocument["version"]);
    EXPECT_TRUE(SameEntitiesById(fixtureDocument, resavedDocument));
    // The writer's component order is deterministic across standard libraries, so
    // the same world writes components in the same order on Windows and Linux.
    EXPECT_TRUE(ComponentsWrittenInTypeOrder(resavedDocument));
}

TEST(SceneMigration_ReflectedLegacyEditorFixtureMigratesAndResavesAsCurrentVersion)
{
    const std::filesystem::path fixturePath = ReflectedFixture("legacy-editor-sceneVersion1.sparkscene");
    const std::string fixtureBefore = ReadFixtureText(fixturePath);
    ASSERT_TRUE(fixtureBefore.find("\"sceneVersion\": 1") != std::string::npos);
    ASSERT_FALSE(HasRecoverySiblings(fixturePath));

    World world;
    std::string error;
    ASSERT_TRUE(LoadWorld(world, PathToUtf8(fixturePath), &error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(4));

    // Legacy component names and inline values migrate to the reflected components.
    const EntityID light = FindNamed(world, "Directional Light");
    const EntityID camera = FindNamed(world, "Main Camera");
    const EntityID player = FindNamed(world, "Player");
    const EntityID crate = FindNamed(world, "Crate");
    ASSERT_TRUE(light != entt::null && camera != entt::null && player != entt::null && crate != entt::null);

    const LightComponent* lightComponent = world.GetComponent<LightComponent>(light);
    ASSERT_TRUE(lightComponent != nullptr);
    EXPECT_TRUE(lightComponent->type == LightComponent::Type::Directional);
    EXPECT_NEAR(lightComponent->color.z, 0.8f, 0.0001f);
    EXPECT_NEAR(lightComponent->intensity, 1.2f, 0.0001f);
    EXPECT_TRUE(lightComponent->castShadows);
    const Transform* lightTransform = world.GetComponent<Transform>(light);
    ASSERT_TRUE(lightTransform != nullptr);
    EXPECT_NEAR(lightTransform->rotation.x, 50.0f, 0.0001f);
    EXPECT_NEAR(lightTransform->rotation.y, -30.0f, 0.0001f);

    const Camera* cameraComponent = world.GetComponent<Camera>(camera);
    ASSERT_TRUE(cameraComponent != nullptr);
    EXPECT_NEAR(cameraComponent->fov, 70.0f, 0.0001f);
    EXPECT_NEAR(cameraComponent->nearPlane, 0.2f, 0.0001f);
    EXPECT_NEAR(cameraComponent->farPlane, 500.0f, 0.0001f);
    EXPECT_TRUE(cameraComponent->isMainCamera);

    const CharacterControllerComponent* controller = world.GetComponent<CharacterControllerComponent>(player);
    ASSERT_TRUE(controller != nullptr);
    EXPECT_NEAR(controller->height, 1.9f, 0.0001f);
    EXPECT_NEAR(controller->radius, 0.4f, 0.0001f);

    const MeshRenderer* crateMesh = world.GetComponent<MeshRenderer>(crate);
    ASSERT_TRUE(crateMesh != nullptr);
    EXPECT_EQ(crateMesh->meshPath, std::string("Assets/Models/crate.obj"));
    EXPECT_EQ(crateMesh->materialPath, std::string("Assets/Materials/wood.json"));
    const Transform* crateTransform = world.GetComponent<Transform>(crate);
    ASSERT_TRUE(crateTransform != nullptr);
    EXPECT_TRUE(crateTransform->parent == player);
    EXPECT_NEAR(crateTransform->scale.y, 0.5f, 0.0001f);

    // Migration happens in memory only; the legacy document is left exactly as authored.
    EXPECT_EQ(ReadFixtureText(fixturePath), fixtureBefore);
    EXPECT_FALSE(HasRecoverySiblings(fixturePath));

    // The next explicit save writes the current dialect only, and reloading that save
    // restores the same hierarchy and component values.
    TemporaryReflectedSceneCopy resaved;
    ASSERT_TRUE(SaveWorld(world, PathToUtf8(resaved.Path())));
    const nlohmann::json resavedDocument = nlohmann::json::parse(ReadFixtureText(resaved.Path()));
    EXPECT_TRUE(resavedDocument.contains("version"));
    EXPECT_EQ(resavedDocument["version"].get<int>(), 1);
    EXPECT_FALSE(resavedDocument.contains("sceneVersion"));

    World reloaded;
    ASSERT_TRUE(LoadWorld(reloaded, PathToUtf8(resaved.Path()), &error));
    EXPECT_EQ(reloaded.GetEntityCount(), static_cast<size_t>(4));
    const EntityID reloadedCrate = FindNamed(reloaded, "Crate");
    const EntityID reloadedPlayer = FindNamed(reloaded, "Player");
    const EntityID reloadedLight = FindNamed(reloaded, "Directional Light");
    ASSERT_TRUE(reloadedCrate != entt::null && reloadedPlayer != entt::null && reloadedLight != entt::null);
    const Transform* reloadedCrateTransform = reloaded.GetComponent<Transform>(reloadedCrate);
    ASSERT_TRUE(reloadedCrateTransform != nullptr);
    EXPECT_TRUE(reloadedCrateTransform->parent == reloadedPlayer);

    // Every migrated value survives the round trip: saving the reloaded world again
    // reproduces the first current-dialect document entity for entity.
    TemporaryReflectedSceneCopy resavedAgain;
    ASSERT_TRUE(SaveWorld(reloaded, PathToUtf8(resavedAgain.Path())));
    const nlohmann::json resavedAgainDocument = nlohmann::json::parse(ReadFixtureText(resavedAgain.Path()));
    EXPECT_TRUE(resavedAgainDocument["version"] == resavedDocument["version"]);
    EXPECT_TRUE(SameEntitiesById(resavedDocument, resavedAgainDocument));
}

TEST(SceneMigration_ReflectedFutureVersionFixtureFailsClosedWithVersionedError)
{
    const std::filesystem::path fixturePath = ReflectedFixture("v2-future.sparkscene");
    const std::string fixtureBefore = ReadFixtureText(fixturePath);
    ASSERT_TRUE(fixtureBefore.find("\"version\": 2") != std::string::npos);
    ASSERT_FALSE(HasRecoverySiblings(fixturePath));

    World world;
    world.CreateEntity("Open Document");
    std::string error;
    EXPECT_FALSE(LoadWorld(world, PathToUtf8(fixturePath), &error));
    EXPECT_STR_CONTAINS(error, PathToUtf8(fixturePath));
    EXPECT_STR_CONTAINS(error, "'version' 2 is unsupported");
    EXPECT_STR_CONTAINS(error, "reads reflected scene version 1");
    EXPECT_STR_CONTAINS(error, "'sceneVersion': 1");
    EXPECT_STR_CONTAINS(error, "writes version 1");
    EXPECT_STR_CONTAINS(error, "newer SparkEngine build");

    // The caller's open document is untouched and the newer file is not downgraded or rewritten.
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
    EXPECT_TRUE(FindNamed(world, "Open Document") != entt::null);
    EXPECT_EQ(ReadFixtureText(fixturePath), fixtureBefore);
    EXPECT_FALSE(HasRecoverySiblings(fixturePath));
}
