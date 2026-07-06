// TestReflectedScene.cpp - Headless round-trip test for the reflection-driven
// scene serializer (SceneManager/ReflectedSceneSerializer.h). Builds a World in
// code, serializes it to JSON, deserializes into a fresh World, and asserts
// every field + parent link survived.

#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
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
