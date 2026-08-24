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

using namespace Spark;

TEST(ReflectedScene_RoundTrip_TransformAndMesh)
{
    World src;
    EntityID ground = src.CreateEntity("Ground");
    Transform& gt = src.AddComponent<Transform>(ground);
    gt.position = {1.0f, 2.0f, 3.0f};
    gt.scale    = {10.0f, 1.0f, 10.0f};
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
    for (auto e : dst.GetEntitiesWith<Transform>()) {
        const NameComponent* nc = dst.GetComponent<NameComponent>(e);
        const Transform* t = dst.GetComponent<Transform>(e);
        if (nc && nc->name == "Ground") {
            foundGround = true;
            EXPECT_NEAR(t->position.x, 1.0f, 0.001f);
            EXPECT_NEAR(t->position.y, 2.0f, 0.001f);
            EXPECT_NEAR(t->position.z, 3.0f, 0.001f);
            EXPECT_NEAR(t->scale.x, 10.0f, 0.001f);
            const MeshRenderer* mr = dst.GetComponent<MeshRenderer>(e);
            EXPECT_TRUE(mr != nullptr);
            EXPECT_STR_CONTAINS(mr->meshPath, "x.obj");
            EXPECT_STR_CONTAINS(mr->materialPath, "y.json");
        }
        if (nc && nc->name == "Prop") {
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
    mr.visible = false;               // Bool
    mr.meshPath = "a/b/c.obj";        // String
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
    cm.fromMask = CollisionLayer::Enemy;                       // non-default (was All)
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
        EXPECT_TRUE(rb2 != nullptr);
        EXPECT_TRUE(rb2->type == RigidBodyComponent::Type::Kinematic);
        EXPECT_TRUE(rb2->motionQuality == RigidBodyComponent::MotionQuality::LinearCast);

        const CollisionMaskComponent* cm2 = dst.GetComponent<CollisionMaskComponent>(ent);
        EXPECT_TRUE(cm2 != nullptr);
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
    EXPECT_TRUE(implicitTransform != nullptr);
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
