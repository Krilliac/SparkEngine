// TestReflectedScene.cpp - Headless round-trip test for the reflection-driven
// scene serializer (SceneManager/ReflectedSceneSerializer.h). Builds a World in
// code, serializes it to JSON, deserializes into a fresh World, and asserts
// every field + parent link survived.

#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Engine/ECS/Components/CollisionMaskComponents.h"
#include "Core/Reflection.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <nlohmann_json.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

using namespace Spark;

namespace
{
    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    class TemporaryReflectedScene
    {
      public:
        TemporaryReflectedScene()
        {
            const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            m_path =
                std::filesystem::temp_directory_path() / ("spark_reflected_scene_" + std::to_string(nonce) + ".json");
        }

        ~TemporaryReflectedScene()
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
            std::filesystem::remove(Sibling(".bak"), ignored);
            std::filesystem::remove(Sibling(".tmp"), ignored);
            std::filesystem::remove(Sibling(".bak.tmp"), ignored);
        }

        const std::filesystem::path& Path() const { return m_path; }

        /// LoadWorld/SaveWorld take UTF-8; path::string() is the ANSI code page on Windows.
        std::string Utf8() const { return PathToUtf8(m_path); }

        std::filesystem::path Sibling(const char* suffix) const
        {
            std::filesystem::path sibling = m_path;
            sibling += suffix;
            return sibling;
        }

      private:
        std::filesystem::path m_path;
    };

    std::string ReadSceneText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
} // namespace

TEST(ReflectedScene_RoundTrip_TransformAndMesh)
{
    World src;
    EntityID ground = src.CreateEntity("Ground");
    Transform& gt = src.AddComponent<Transform>(ground);
    gt.position = {1.0f, 2.0f, 3.0f};
    gt.scale = {10.0f, 1.0f, 10.0f};
    MeshRenderer& gm = src.AddComponent<MeshRenderer>(ground);
    gm.meshPath = "Assets/Models/x.obj";
    gm.materialPath = "Assets/Materials/y.json";

    EntityID child = src.CreateEntity("Prop");
    Transform& ct = src.AddComponent<Transform>(child);
    ct.position = {5.0f, 0.0f, -5.0f};
    ct.parent = ground; // child of ground

    const std::string json = SerializeWorld(src);

    World dst;
    EXPECT_TRUE(DeserializeInto(dst, json));

    // Same entity count.
    EXPECT_EQ(dst.GetEntityCount(), (size_t)2);

    // Find the entity named "Ground" and verify its fields survived.
    bool foundGround = false, foundChildParented = false;
    for (auto e : dst.GetEntitiesWith<Transform>())
    {
        const NameComponent* nc = dst.GetComponent<NameComponent>(e);
        const Transform* t = dst.GetComponent<Transform>(e);
        if (nc && nc->name == "Ground")
        {
            foundGround = true;
            EXPECT_NEAR(t->position.x, 1.0f, 0.001f);
            EXPECT_NEAR(t->position.y, 2.0f, 0.001f);
            EXPECT_NEAR(t->position.z, 3.0f, 0.001f);
            EXPECT_NEAR(t->scale.x, 10.0f, 0.001f);
            const MeshRenderer* mr = dst.GetComponent<MeshRenderer>(e);
            ASSERT_TRUE(mr != nullptr);
            EXPECT_STR_CONTAINS(mr->meshPath, "x.obj");
            EXPECT_STR_CONTAINS(mr->materialPath, "y.json");
        }
        if (nc && nc->name == "Prop")
        {
            EXPECT_TRUE(t->parent != entt::null); // parent resolved
            foundChildParented = true;
        }
    }
    EXPECT_TRUE(foundGround);
    EXPECT_TRUE(foundChildParented);
}

TEST(ReflectedScene_FieldCoverage_ScalarsAndVectors)
{
    World w;
    EntityID e = w.CreateEntity("Probe");
    MeshRenderer& mr = w.AddComponent<MeshRenderer>(e);
    mr.visible = false;        // Bool
    mr.meshPath = "a/b/c.obj"; // String
    Transform& t = w.AddComponent<Transform>(e);
    t.rotation = {15.0f, 30.0f, 45.0f}; // Vector3

    const std::string json = SerializeWorld(w);
    World w2;
    EXPECT_TRUE(DeserializeInto(w2, json));

    EntityID e2 = *w2.GetEntitiesWith<MeshRenderer>().begin();
    const MeshRenderer* mr2 = w2.GetComponent<MeshRenderer>(e2);
    EXPECT_FALSE(mr2->visible);
    EXPECT_STR_CONTAINS(mr2->meshPath, "c.obj");
    const Transform* t2 = w2.GetComponent<Transform>(e2);
    EXPECT_NEAR(t2->rotation.y, 30.0f, 0.001f);
}

// Regression test for the reflection-serial data-loss lane:
//  - RigidBodyComponent::type/motionQuality are enum class fields that used to
//    be unregistered (and even once registered, FieldType::Enum had no
//    Set/GetFieldFromString implementation), so they silently reset to their
//    default-member-initializer values on every scene round-trip.
//  - CollisionMaskComponent registered zero fields at all, so fromMask/intoMask
//    always came back as the 0xFFFFFFFF default regardless of what was set.
TEST(ReflectedScene_RoundTrip_EnumAndMaskFieldsSurvive)
{
    World src;
    EntityID e = src.CreateEntity("Crate");

    RigidBodyComponent& rb = src.AddComponent<RigidBodyComponent>(e);
    rb.type = RigidBodyComponent::Type::Kinematic;                    // non-default (was Dynamic)
    rb.motionQuality = RigidBodyComponent::MotionQuality::LinearCast; // non-default (was Discrete)

    CollisionMaskComponent& cm = src.AddComponent<CollisionMaskComponent>(e);
    cm.fromMask = CollisionLayer::Enemy;                                // non-default (was All)
    cm.intoMask = CollisionLayer::Player | CollisionLayer::Environment; // non-default (was All)

    const std::string json = SerializeWorld(src);

    World dst;
    EXPECT_TRUE(DeserializeInto(dst, json));

    bool found = false;
    for (auto ent : dst.GetEntitiesWith<RigidBodyComponent>())
    {
        const NameComponent* nc = dst.GetComponent<NameComponent>(ent);
        if (!nc || nc->name != "Crate")
            continue;
        found = true;

        const RigidBodyComponent* rb2 = dst.GetComponent<RigidBodyComponent>(ent);
        ASSERT_TRUE(rb2 != nullptr);
        EXPECT_TRUE(rb2->type == RigidBodyComponent::Type::Kinematic);
        EXPECT_TRUE(rb2->motionQuality == RigidBodyComponent::MotionQuality::LinearCast);

        const CollisionMaskComponent* cm2 = dst.GetComponent<CollisionMaskComponent>(ent);
        ASSERT_TRUE(cm2 != nullptr);
        EXPECT_TRUE(cm2->fromMask == CollisionLayer::Enemy);
        EXPECT_TRUE(cm2->intoMask == (CollisionLayer::Player | CollisionLayer::Environment));
    }
    EXPECT_TRUE(found);
}

TEST(ReflectedScene_LoadsLegacyProjectManagerSceneSchema)
{
    const std::string legacy = R"json({
      "sceneVersion": 1,
      "entities": [
        {"id": 1, "name": "Directional Light", "components": [
          {"type": "Transform", "position": [0, 10, 0], "rotation": [50, -30, 0], "scale": [1, 1, 1]},
          {"type": "DirectionalLight", "color": [1, 0.95, 0.8], "intensity": 1.2}
        ]},
        {"id": 2, "name": "Main Camera", "components": [
          {"type": "Transform", "position": [0, 2, -5], "rotation": [10, 0, 0], "scale": [1, 1, 1]},
          {"type": "Camera", "fov": 70, "nearClip": 0.2, "farClip": 500}
        ]},
        {"id": 3, "name": "Player", "components": [
          {"type": "CharacterController", "height": 1.9, "radius": 0.4}
        ]}
      ]
    })json";

    World world;
    EXPECT_TRUE(Spark::DeserializeInto(world, legacy));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(3));
    const auto lights = world.GetEntitiesWith<LightComponent>();
    size_t lightCount = 0;
    for ([[maybe_unused]] auto entity : lights)
        ++lightCount;
    EXPECT_EQ(lightCount, static_cast<size_t>(1));
    EXPECT_EQ(static_cast<int>(lights.get<LightComponent>(*lights.begin()).type),
              static_cast<int>(LightComponent::Type::Directional));
    const auto cameras = world.GetEntitiesWith<Camera>();
    size_t cameraCount = 0;
    for ([[maybe_unused]] auto entity : cameras)
        ++cameraCount;
    EXPECT_EQ(cameraCount, static_cast<size_t>(1));
    EXPECT_TRUE(cameras.get<Camera>(*cameras.begin()).isMainCamera);
    EXPECT_NEAR(cameras.get<Camera>(*cameras.begin()).nearPlane, 0.2f, 0.001f);
    size_t controllerCount = 0;
    for ([[maybe_unused]] auto entity : world.GetEntitiesWith<CharacterControllerComponent>())
        ++controllerCount;
    EXPECT_EQ(controllerCount, static_cast<size_t>(1));
}

TEST(ReflectedScene_MixedExplicitAndImplicitIdsRemainDistinct)
{
    const std::string mixed = R"json({
      "version": 1,
      "entities": [
        {"name": "Implicit", "parent": 7, "components": []},
        {"id": 0, "name": "Explicit Zero", "parent": -1, "components": []},
        {"id": 7, "name": "Parent", "parent": -1,
         "components": [{"type": "Transform", "fields": {}}]}
      ]
    })json";

    World world;
    EXPECT_TRUE(Spark::DeserializeInto(world, mixed));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(3));

    entt::entity implicit = entt::null;
    entt::entity explicitZero = entt::null;
    entt::entity parent = entt::null;
    for (auto entity : world.GetEntitiesWith<NameComponent>())
    {
        const auto& name = *world.GetComponent<NameComponent>(entity);
        if (name.name == "Implicit")
            implicit = entity;
        else if (name.name == "Explicit Zero")
            explicitZero = entity;
        else if (name.name == "Parent")
            parent = entity;
    }

    EXPECT_TRUE(implicit != entt::null);
    EXPECT_TRUE(explicitZero != entt::null);
    EXPECT_TRUE(parent != entt::null);
    EXPECT_TRUE(implicit != explicitZero);
    EXPECT_EQ(static_cast<uint32_t>(explicitZero), 0u);
    EXPECT_EQ(static_cast<uint32_t>(parent), 7u);
    const Transform* implicitTransform = world.GetComponent<Transform>(implicit);
    ASSERT_TRUE(implicitTransform != nullptr);
    EXPECT_TRUE(implicitTransform->parent == parent);
}

TEST(ReflectedScene_RejectsDuplicateExplicitIdsWithoutMutation)
{
    const std::string duplicate = R"json({
      "version": 1,
      "entities": [
        {"id": 3, "name": "First", "components": []},
        {"id": 3, "name": "Second", "components": []}
      ]
    })json";

    World world;
    EXPECT_FALSE(Spark::DeserializeInto(world, duplicate));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(0));
}

// SAVE-230: an entt id carries version bits above its 20-bit slot index, so a
// slot recycled 2048 times already has an id >= 2^31. The writer used to emit
// ids and parents as signed 32-bit (a world with such an entity saved but its
// reload was rejected for a negative id) and the reader read parents as int.
TEST(ReflectedScene_RoundTripKeepsHighVersionIdsAndParents)
{
    constexpr uint32_t kParentId = 0x80000005u; // version 0x800, slot 5
    constexpr uint32_t kChildId = 0xFFE00002u;  // version 0xFFE, slot 2

    World src;
    entt::registry& registry = src.GetRegistry();
    const EntityID parent = registry.create(static_cast<entt::entity>(kParentId));
    const EntityID child = registry.create(static_cast<entt::entity>(kChildId));
    ASSERT_EQ(static_cast<uint32_t>(parent), kParentId);
    ASSERT_EQ(static_cast<uint32_t>(child), kChildId);
    src.AddComponent<NameComponent>(parent).name = "High Parent";
    src.AddComponent<NameComponent>(child).name = "High Child";
    src.AddComponent<Transform>(parent).position = {1.0f, 2.0f, 3.0f};
    src.AddComponent<Transform>(child).position = {-4.0f, 0.5f, 6.0f};
    ASSERT_TRUE(src.SetParent(child, parent));

    // Ids and parents are written as their full unsigned value, never negative.
    const std::string text = SerializeWorld(src);
    const nlohmann::json document = nlohmann::json::parse(text);
    bool wroteParent = false;
    bool wroteChild = false;
    for (const nlohmann::json& entity : document["entities"])
    {
        if (entity["name"].get<std::string>() == "High Parent")
        {
            wroteParent = true;
            EXPECT_EQ(entity["id"].get<uint64_t>(), static_cast<uint64_t>(kParentId));
            EXPECT_EQ(entity["parent"].get<int64_t>(), static_cast<int64_t>(-1));
        }
        else if (entity["name"].get<std::string>() == "High Child")
        {
            wroteChild = true;
            EXPECT_EQ(entity["id"].get<uint64_t>(), static_cast<uint64_t>(kChildId));
            EXPECT_EQ(entity["parent"].get<uint64_t>(), static_cast<uint64_t>(kParentId));
        }
    }
    EXPECT_TRUE(wroteParent && wroteChild);

    // Both the editor load path and the fail-closed crash-recovery path restore the
    // same ids and the > 2^31 parent edge.
    for (const SceneDeserializeMode mode : {SceneDeserializeMode::Permissive, SceneDeserializeMode::StrictRecovery})
    {
        World dst;
        std::string error;
        ASSERT_TRUE(DeserializeInto(dst, text, mode, &error));
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(dst.GetEntityCount(), static_cast<size_t>(2));
        const entt::entity loadedParent = static_cast<entt::entity>(kParentId);
        const entt::entity loadedChild = static_cast<entt::entity>(kChildId);
        ASSERT_TRUE(dst.GetRegistry().valid(loadedParent));
        ASSERT_TRUE(dst.GetRegistry().valid(loadedChild));
        const NameComponent* parentName = dst.GetComponent<NameComponent>(loadedParent);
        const NameComponent* childName = dst.GetComponent<NameComponent>(loadedChild);
        ASSERT_TRUE(parentName != nullptr && childName != nullptr);
        EXPECT_EQ(parentName->name, std::string("High Parent"));
        EXPECT_EQ(childName->name, std::string("High Child"));
        const Transform* childTransform = dst.GetComponent<Transform>(loadedChild);
        ASSERT_TRUE(childTransform != nullptr);
        EXPECT_TRUE(childTransform->parent == loadedParent);
        EXPECT_NEAR(childTransform->position.z, 6.0f, 0.0001f);
        const Transform* parentTransform = dst.GetComponent<Transform>(loadedParent);
        ASSERT_TRUE(parentTransform != nullptr);
        EXPECT_TRUE(parentTransform->parent == entt::null);
        EXPECT_TRUE(parentTransform->children == std::vector<EntityID>{loadedChild});
    }
}

// A parent outside the entity id range used to be read as int: 4294967299 wrapped
// onto entity 3 and other negatives were silently treated as "no parent".
TEST(ReflectedScene_RejectsOutOfRangeParentIdsWithoutMutation)
{
    const char* wrapsOntoThree = R"json({"version":1,"entities":[
        {"id":3,"name":"Three","parent":-1,"components":[{"type":"Transform","fields":{}}]},
        {"id":4,"name":"Child","parent":4294967299,"components":[{"type":"Transform","fields":{}}]}]})json";
    const char* negative = R"json({"version":1,"entities":[
        {"id":4,"name":"Child","parent":-5,"components":[]}]})json";
    const char* nullEntity = R"json({"version":1,"entities":[
        {"id":4,"name":"Child","parent":4294967295,"components":[]}]})json";

    World world;
    world.CreateEntity("KeepMe");
    std::string error;
    EXPECT_FALSE(DeserializeInto(world, wrapsOntoThree, SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "entity #1 ('Child') has parent 4294967299");
    EXPECT_FALSE(DeserializeInto(world, negative, SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "entity #0 ('Child') has parent -5");
    EXPECT_FALSE(DeserializeInto(world, nullEntity, SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "entity #0 ('Child') has parent 4294967295");
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
}

TEST(ReflectedScene_RejectsUnknownAndAmbiguousVersionsWithoutMutation)
{
    World world;
    world.CreateEntity("KeepMe");

    EXPECT_FALSE(DeserializeInto(world, R"json({"version":999,"entities":[]})json"));
    EXPECT_FALSE(DeserializeInto(world, R"json({"version":1,"sceneVersion":1,"entities":[]})json"));
    EXPECT_FALSE(DeserializeInto(world, R"json({"entities":[]})json"));
    EXPECT_FALSE(DeserializeInto(world, R"json({"sceneVersion":"1","entities":[]})json"));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
}

TEST(ReflectedScene_VersionRejectionNamesFileVersionAndSupportedWindow)
{
    World world;
    world.CreateEntity("KeepMe");
    std::string error;

    EXPECT_FALSE(
        DeserializeInto(world, R"json({"version":999,"entities":[]})json", SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "'version' 999 is unsupported");
    EXPECT_STR_CONTAINS(error, "reads reflected scene version 1");
    EXPECT_STR_CONTAINS(error, "'sceneVersion': 1");
    EXPECT_STR_CONTAINS(error, "writes version 1");
    EXPECT_STR_CONTAINS(error, "newer SparkEngine build");

    EXPECT_FALSE(
        DeserializeInto(world, R"json({"version":0,"entities":[]})json", SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "'version' 0 is unsupported");
    EXPECT_STR_CONTAINS(error, "no migration exists");

    EXPECT_FALSE(DeserializeInto(world, R"json({"sceneVersion":2,"entities":[]})json", SceneDeserializeMode::Permissive,
                                 &error));
    EXPECT_STR_CONTAINS(error, "'sceneVersion' 2 is unsupported");

    EXPECT_FALSE(DeserializeInto(world, R"json({"version":1,"sceneVersion":1,"entities":[]})json",
                                 SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "both 'version' and 'sceneVersion'");

    EXPECT_FALSE(DeserializeInto(world, R"json({"entities":[]})json", SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "no 'version' field");

    EXPECT_FALSE(DeserializeInto(world, R"json({"sceneVersion":"1","entities":[]})json",
                                 SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "'sceneVersion' must be an integer, found string");

    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));

    // A successful load clears a stale diagnostic from an earlier rejection.
    World accepted;
    EXPECT_TRUE(
        DeserializeInto(accepted, R"json({"version":1,"entities":[]})json", SceneDeserializeMode::Permissive, &error));
    EXPECT_TRUE(error.empty());
}

TEST(ReflectedScene_SchemaRejectionExplainsTheOffendingElement)
{
    World world;
    std::string error;

    EXPECT_FALSE(DeserializeInto(world, "[]", SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "root must be a JSON object, found array");

    EXPECT_FALSE(DeserializeInto(world, "null", SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "root must be a JSON object, found null");

    // Text that is not JSON at all is a parse failure, not a null root: the strict
    // vendored parser throws instead of yielding a partial (null) value.
    EXPECT_FALSE(DeserializeInto(world, "not a scene", SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "scene could not be read");
    EXPECT_STR_CONTAINS(error, "invalid literal");

    EXPECT_FALSE(DeserializeInto(world, R"json({"version":1})json", SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "no 'entities' array");

    EXPECT_FALSE(
        DeserializeInto(world, R"json({"version":1,"entities":[{"id":3,"name":"First"},{"id":3,"name":"Second"}]})json",
                        SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "entity #1 ('Second') repeats id 3");

    EXPECT_FALSE(DeserializeInto(world, R"json({"version":1,"entities":[{"id":-4,"name":"Neg"}]})json",
                                 SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "entity #0 ('Neg') has id -4");

    EXPECT_FALSE(DeserializeInto(world, R"json({"version":1,"entities":[{"id":1,"parent":"x"}]})json",
                                 SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "entity #0 has parent \"x\"");

    const char* unknownComponent = R"json({"version":1,"entities":[
        {"id":1,"name":"Crate","parent":-1,"components":[{"type":"NoSuchComponent","fields":{}}]}]})json";
    EXPECT_FALSE(DeserializeInto(world, unknownComponent, SceneDeserializeMode::StrictRecovery, &error));
    EXPECT_STR_CONTAINS(error, "crash-recovery record entity #0 ('Crate')");
    EXPECT_STR_CONTAINS(error, "'NoSuchComponent'");

    const char* missingField = R"json({"version":1,"entities":[
        {"id":1,"name":"Crate","parent":-1,"components":[{"type":"Transform","fields":{}}]}]})json";
    EXPECT_FALSE(DeserializeInto(world, missingField, SceneDeserializeMode::StrictRecovery, &error));
    EXPECT_STR_CONTAINS(error, "is missing field 'Transform.");

    const char* legacyRecovery = R"json({"sceneVersion":1,"entities":[]})json";
    EXPECT_FALSE(DeserializeInto(world, legacyRecovery, SceneDeserializeMode::StrictRecovery, &error));
    EXPECT_STR_CONTAINS(error, "legacy 'sceneVersion' dialect");

    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(0));
}

TEST(ReflectedScene_LoadWorldReportsPrimaryVersionAndBackupReason)
{
    TemporaryReflectedScene file;
    {
        std::ofstream future(file.Path(), std::ios::binary | std::ios::trunc);
        future << R"({"version":7,"entities":[]})";
    }

    World world;
    world.CreateEntity("Unchanged");
    std::string error;
    EXPECT_FALSE(LoadWorld(world, file.Utf8(), &error));
    EXPECT_STR_CONTAINS(error, file.Utf8());
    EXPECT_STR_CONTAINS(error, "'version' 7 is unsupported");
    EXPECT_STR_CONTAINS(error, "writes version 1");
    EXPECT_STR_CONTAINS(error, ".bak' was not usable: file does not exist");
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));

    {
        std::ofstream backup(file.Sibling(".bak"), std::ios::binary | std::ios::trunc);
        backup << R"({"sceneVersion":"one","entities":[]})";
    }
    EXPECT_FALSE(LoadWorld(world, file.Utf8(), &error));
    EXPECT_STR_CONTAINS(error, "'version' 7 is unsupported");
    EXPECT_STR_CONTAINS(error, ".bak' was not usable: scene 'sceneVersion' must be an integer");

    const std::filesystem::path missing = file.Sibling(".absent");
    EXPECT_FALSE(LoadWorld(world, PathToUtf8(missing), &error));
    EXPECT_STR_CONTAINS(error, "was rejected: file does not exist");
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));

    {
        std::ofstream valid(file.Path(), std::ios::binary | std::ios::trunc);
        valid << R"({"version":1,"entities":[{"id":0,"name":"Loaded"}]})";
    }
    EXPECT_TRUE(LoadWorld(world, file.Utf8(), &error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
}

TEST(ReflectedScene_SaveIsAtomicAndLoadRecoversPreviousGoodBackup)
{
    TemporaryReflectedScene file;

    World first;
    first.CreateEntity("First");
    EXPECT_TRUE(SaveWorld(first, file.Utf8()));
    EXPECT_FALSE(std::filesystem::exists(file.Sibling(".bak")));

    const std::string firstImage = ReadSceneText(file.Path());
    EXPECT_STR_CONTAINS(firstImage, "First");

    World second;
    second.CreateEntity("Second");
    EXPECT_TRUE(SaveWorld(second, file.Utf8()));
    EXPECT_STR_CONTAINS(ReadSceneText(file.Path()), "Second");
    EXPECT_EQ(ReadSceneText(file.Sibling(".bak")), firstImage);
    EXPECT_FALSE(std::filesystem::exists(file.Sibling(".tmp")));
    EXPECT_FALSE(std::filesystem::exists(file.Sibling(".bak.tmp")));

    {
        std::ofstream corrupt(file.Path(), std::ios::binary | std::ios::trunc);
        corrupt << R"({"version":999,"entities":[]})";
    }

    World recovered;
    EXPECT_TRUE(LoadWorld(recovered, file.Utf8()));
    EXPECT_EQ(recovered.GetEntityCount(), static_cast<size_t>(1));
    const auto names = recovered.GetEntitiesWith<NameComponent>();
    ASSERT_TRUE(names.begin() != names.end());
    EXPECT_EQ(names.get<NameComponent>(*names.begin()).name, std::string("First"));
}

TEST(ReflectedScene_LoadFallbackDoesNotAppendPartiallyReadPrimary)
{
    TemporaryReflectedScene file;

    World first;
    first.CreateEntity("First");
    ASSERT_TRUE(SaveWorld(first, file.Utf8()));

    World replacement;
    replacement.CreateEntity("Replacement");
    ASSERT_TRUE(SaveWorld(replacement, file.Utf8()));

    // The second entity is created before its invalid parent value throws.
    // LoadWorld must not retain either partially-read primary entity when it
    // falls back to the previous-good backup.
    {
        std::ofstream corrupt(file.Path(), std::ios::binary | std::ios::trunc);
        corrupt << R"json({
            "version": 1,
            "entities": [
                {"id": 1, "name": "Partial", "components": []},
                {"id": 2, "name": "Invalid", "parent": "not-an-integer", "components": []}
            ]
        })json";
    }

    World recovered;
    ASSERT_TRUE(LoadWorld(recovered, file.Utf8()));
    EXPECT_EQ(recovered.GetEntityCount(), static_cast<size_t>(1));
    const auto names = recovered.GetEntitiesWith<NameComponent>();
    ASSERT_TRUE(names.begin() != names.end());
    EXPECT_EQ(names.get<NameComponent>(*names.begin()).name, std::string("First"));
}

// A torn or truncated scene write used to parse as a valid (empty) document:
// the vendored JSON parser returned whatever it had read so far, so
// `{"version": 1, "entities": [` loaded as a scene with zero entities.
TEST(ReflectedScene_RejectsTruncatedAndTrailingGarbageDocumentsWithoutMutation)
{
    World world;
    world.CreateEntity("KeepMe");

    EXPECT_FALSE(DeserializeInto(world, "{\"version\": 1, \"entities\": [\n"));
    EXPECT_FALSE(DeserializeInto(world, R"json({"version": 1, "entities": [{"id": 1, "name": "Cut)json"));
    EXPECT_FALSE(DeserializeInto(world, R"json({"version": 1, "entities": [{"id": 1, "name": "A", )json"));
    EXPECT_FALSE(DeserializeInto(world, R"json({"version": 1, "entities": []}garbage)json"));
    EXPECT_FALSE(DeserializeInto(world, R"json({"version": 1, "entities": [], })json"));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));

    // The same document, complete, still loads.
    World complete;
    EXPECT_TRUE(DeserializeInto(complete, "{\"version\": 1, \"entities\": []}\n"));
}

TEST(ReflectedScene_LoadRecoversPreviousGoodBackupFromTruncatedPrimary)
{
    TemporaryReflectedScene file;

    World first;
    first.CreateEntity("First");
    ASSERT_TRUE(SaveWorld(first, file.Utf8()));

    World replacement;
    replacement.CreateEntity("Replacement");
    ASSERT_TRUE(SaveWorld(replacement, file.Utf8()));
    ASSERT_TRUE(std::filesystem::exists(file.Sibling(".bak")));

    {
        std::ofstream truncated(file.Path(), std::ios::binary | std::ios::trunc);
        truncated << "{\"version\": 1, \"entities\": [\n";
    }

    World recovered;
    ASSERT_TRUE(LoadWorld(recovered, file.Utf8()));
    EXPECT_EQ(recovered.GetEntityCount(), static_cast<size_t>(1));
    const auto names = recovered.GetEntitiesWith<NameComponent>();
    ASSERT_TRUE(names.begin() != names.end());
    EXPECT_EQ(names.get<NameComponent>(*names.begin()).name, std::string("First"));

    // With no previous-good backup the truncated primary must fail the load
    // and leave the caller's world untouched.
    std::filesystem::remove(file.Sibling(".bak"));
    World untouched;
    untouched.CreateEntity("Existing");
    EXPECT_FALSE(LoadWorld(untouched, file.Utf8()));
    EXPECT_EQ(untouched.GetEntityCount(), static_cast<size_t>(1));
}

TEST(World_DestroyEntityRepairsHierarchyLinks)
{
    World world;
    const auto parent = world.CreateEntity("Parent");
    const auto child = world.CreateEntity("Child");
    const auto sibling = world.CreateEntity("Sibling");
    auto& parentTransform = world.AddComponent<Transform>(parent);
    auto& childTransform = world.AddComponent<Transform>(child);
    auto& siblingTransform = world.AddComponent<Transform>(sibling);
    parentTransform.children = {child, sibling};
    childTransform.parent = parent;
    siblingTransform.parent = parent;

    world.DestroyEntity(child);
    EXPECT_TRUE(world.GetRegistry().valid(parent));
    EXPECT_TRUE(world.GetRegistry().valid(sibling));
    const Transform* repairedParent = world.GetComponent<Transform>(parent);
    EXPECT_EQ(repairedParent->children.size(), static_cast<size_t>(1));
    EXPECT_TRUE(repairedParent->children.front() == sibling);

    world.DestroyEntity(parent);
    EXPECT_TRUE(world.GetComponent<Transform>(sibling)->parent == entt::null);
}
