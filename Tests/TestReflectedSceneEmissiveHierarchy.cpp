// TestReflectedSceneEmissiveHierarchy.cpp - W9 editor-inspector lane: headless
// round-trip tests for the reflection-driven scene serializer covering
//   1. MeshRenderer::emissive (new float field, registered with range 0..2)
//      survives serialize -> deserialize (non-default value preserved,
//      default 0 stays 0).
//   2. Transform parent/child hierarchy: a 3-deep chain round-trips with the
//      parent links resolved AND the parents' children vectors rebuilt.
//   3. Camera + Script field regression: all registered fields survive.
//
// Same CI-safe pattern as TestReflectedScene.cpp — builds a World in code,
// serializes to a JSON string, deserializes into a fresh World, compares.

#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Core/Reflection.h"
#include "SceneManager/ReflectedSceneSerializer.h"

using namespace Spark;

namespace
{
    /// Find the (first) entity with the given NameComponent name, or entt::null.
    EntityID FindByName(World& world, const char* name)
    {
        for (auto e : world.GetEntitiesWith<NameComponent>())
        {
            const NameComponent* nc = world.GetComponent<NameComponent>(e);
            if (nc && nc->name == name)
                return e;
        }
        return entt::null;
    }
} // namespace

TEST(ReflectedScene_RoundTrip_EmissiveSurvives)
{
    World src;

    EntityID glow = src.CreateEntity("GlowStrip");
    src.AddComponent<Transform>(glow);
    MeshRenderer& gm = src.AddComponent<MeshRenderer>(glow);
    gm.meshPath = "Assets/Models/strip.obj";
    gm.emissive = 1.5f; // non-default (default 0.0f)

    EntityID crate = src.CreateEntity("Crate");
    src.AddComponent<Transform>(crate);
    MeshRenderer& cm = src.AddComponent<MeshRenderer>(crate);
    cm.meshPath = "Assets/Models/crate.obj"; // emissive left at default 0.0f

    const std::string json = SerializeWorld(src);
    // The field must actually be in the JSON (registration, not just defaults).
    EXPECT_STR_CONTAINS(json, "emissive");

    World dst;
    EXPECT_TRUE(DeserializeInto(dst, json));

    EntityID glow2 = FindByName(dst, "GlowStrip");
    EXPECT_TRUE(glow2 != entt::null);
    const MeshRenderer* gm2 = dst.GetComponent<MeshRenderer>(glow2);
    EXPECT_TRUE(gm2 != nullptr);
    EXPECT_NEAR(gm2->emissive, 1.5f, 0.001f);

    EntityID crate2 = FindByName(dst, "Crate");
    EXPECT_TRUE(crate2 != entt::null);
    const MeshRenderer* cm2 = dst.GetComponent<MeshRenderer>(crate2);
    EXPECT_TRUE(cm2 != nullptr);
    EXPECT_NEAR(cm2->emissive, 0.0f, 0.001f);
}

TEST(ReflectedScene_RoundTrip_ParentChainAndChildrenRebuilt)
{
    World src;

    // Root -> Child -> Grandchild, each with an emissive MeshRenderer so the
    // two features are exercised together (the W9 acceptance scenario).
    // NOTE: all components are added first, then linked via re-fetched
    // pointers — an AddComponent on another entity can reallocate the entt
    // pool and dangle earlier references.
    EntityID root = src.CreateEntity("Root");
    EntityID child = src.CreateEntity("Child");
    EntityID grand = src.CreateEntity("Grandchild");
    src.AddComponent<Transform>(root);
    src.AddComponent<Transform>(child);
    src.AddComponent<Transform>(grand);
    src.AddComponent<MeshRenderer>(child);
    src.AddComponent<MeshRenderer>(grand);

    Transform* rt = src.GetComponent<Transform>(root);
    Transform* ct = src.GetComponent<Transform>(child);
    Transform* gt = src.GetComponent<Transform>(grand);
    rt->position = {0.0f, 10.0f, 0.0f};
    ct->position = {1.0f, 0.0f, 0.0f};
    ct->parent = root;
    rt->children.push_back(child);
    gt->position = {0.0f, 0.0f, 2.0f};
    gt->parent = child;
    ct->children.push_back(grand);
    src.GetComponent<MeshRenderer>(child)->emissive = 0.75f;
    src.GetComponent<MeshRenderer>(grand)->emissive = 2.0f; // top of the editor slider range

    const std::string json = SerializeWorld(src);

    World dst;
    EXPECT_TRUE(DeserializeInto(dst, json));
    EXPECT_EQ(dst.GetEntityCount(), (size_t)3);

    EntityID root2 = FindByName(dst, "Root");
    EntityID child2 = FindByName(dst, "Child");
    EntityID grand2 = FindByName(dst, "Grandchild");
    EXPECT_TRUE(root2 != entt::null);
    EXPECT_TRUE(child2 != entt::null);
    EXPECT_TRUE(grand2 != entt::null);

    // Parent links resolved to the correct entities.
    const Transform* ct2 = dst.GetComponent<Transform>(child2);
    EXPECT_TRUE(ct2 != nullptr);
    EXPECT_TRUE(ct2->parent == root2);
    const Transform* gt2 = dst.GetComponent<Transform>(grand2);
    EXPECT_TRUE(gt2 != nullptr);
    EXPECT_TRUE(gt2->parent == child2);

    // Children vectors rebuilt on load (serializer stores only parent ids).
    const Transform* rt2 = dst.GetComponent<Transform>(root2);
    EXPECT_TRUE(rt2 != nullptr);
    bool rootHasChild = false;
    for (EntityID c : rt2->children)
        rootHasChild = rootHasChild || (c == child2);
    EXPECT_TRUE(rootHasChild);
    bool childHasGrand = false;
    for (EntityID c : ct2->children)
        childHasGrand = childHasGrand || (c == grand2);
    EXPECT_TRUE(childHasGrand);

    // Root stays a root.
    EXPECT_TRUE(rt2->parent == entt::null);

    // Emissive values survive alongside the hierarchy.
    EXPECT_NEAR(dst.GetComponent<MeshRenderer>(child2)->emissive, 0.75f, 0.001f);
    EXPECT_NEAR(dst.GetComponent<MeshRenderer>(grand2)->emissive, 2.0f, 0.001f);

    // Local position fields survive on the parented entities.
    EXPECT_NEAR(ct2->position.x, 1.0f, 0.001f);
    EXPECT_NEAR(gt2->position.z, 2.0f, 0.001f);
}

TEST(World_DestroyEntityRepairsImmediateHierarchyLinks)
{
    World world;
    const EntityID root = world.CreateEntity("Root");
    const EntityID parent = world.CreateEntity("Parent");
    const EntityID child = world.CreateEntity("Child");
    world.AddComponent<Transform>(root);
    world.AddComponent<Transform>(parent);
    world.AddComponent<Transform>(child);

    Transform* rootTransform = world.GetComponent<Transform>(root);
    Transform* parentTransform = world.GetComponent<Transform>(parent);
    Transform* childTransform = world.GetComponent<Transform>(child);
    parentTransform->parent = root;
    rootTransform->children.push_back(parent);
    childTransform->parent = parent;
    parentTransform->children.push_back(child);

    world.DestroyEntity(parent);

    EXPECT_FALSE(world.GetRegistry().valid(parent));
    rootTransform = world.GetComponent<Transform>(root);
    childTransform = world.GetComponent<Transform>(child);
    EXPECT_TRUE(rootTransform != nullptr);
    EXPECT_TRUE(childTransform != nullptr);
    EXPECT_TRUE(std::find(rootTransform->children.begin(), rootTransform->children.end(), parent) ==
                rootTransform->children.end());
    EXPECT_TRUE(childTransform->parent == entt::null);
}

TEST(World_DestroyLeafRemovesEveryDuplicateParentBacklink)
{
    World world;
    const EntityID parent = world.CreateEntity("Parent");
    const EntityID child = world.CreateEntity("Child");
    world.AddComponent<Transform>(parent);
    world.AddComponent<Transform>(child);

    Transform* parentTransform = world.GetComponent<Transform>(parent);
    Transform* childTransform = world.GetComponent<Transform>(child);
    childTransform->parent = parent;
    parentTransform->children = {child, child};

    world.DestroyEntity(child);

    parentTransform = world.GetComponent<Transform>(parent);
    EXPECT_TRUE(parentTransform != nullptr);
    EXPECT_TRUE(parentTransform->children.empty());
}

TEST(World_SetParentMaintainsReciprocalLinksAndRejectsCycles)
{
    World world;
    const EntityID root = world.CreateEntity("Root");
    const EntityID other = world.CreateEntity("Other");
    const EntityID child = world.CreateEntity("Child");

    EXPECT_TRUE(world.SetParent(child, root));
    const Transform* childTransform = world.GetComponent<Transform>(child);
    const Transform* rootTransform = world.GetComponent<Transform>(root);
    EXPECT_TRUE(childTransform && childTransform->parent == root);
    EXPECT_TRUE(rootTransform && rootTransform->children == std::vector<EntityID>{child});

    EXPECT_TRUE(world.SetParent(child, other));
    rootTransform = world.GetComponent<Transform>(root);
    const Transform* otherTransform = world.GetComponent<Transform>(other);
    EXPECT_TRUE(rootTransform && rootTransform->children.empty());
    EXPECT_TRUE(otherTransform && otherTransform->children == std::vector<EntityID>{child});
    EXPECT_FALSE(world.SetParent(other, child));
}

// Regression pass for the W9 lane's item 3: Camera and Script registered
// fields still round-trip (guards the field registrations the Inspector's
// World-backed path renders from).
TEST(ReflectedScene_RoundTrip_CameraAndScriptFields)
{
    World src;

    EntityID camEnt = src.CreateEntity("MainCam");
    Camera& cam = src.AddComponent<Camera>(camEnt);
    cam.fov = 75.0f;       // non-default (60)
    cam.nearPlane = 0.25f; // non-default (0.1)
    cam.farPlane = 512.0f; // non-default (1000)
    cam.isMainCamera = true;

    EntityID scriptEnt = src.CreateEntity("Scripted");
    Script& sc = src.AddComponent<Script>(scriptEnt);
    sc.scriptPath = "Scripts/turret.as";
    sc.className = "TurretBrain";
    sc.moduleName = "gameplay";
    sc.enabled = false; // non-default (true)

    const std::string json = SerializeWorld(src);

    World dst;
    EXPECT_TRUE(DeserializeInto(dst, json));

    EntityID camEnt2 = FindByName(dst, "MainCam");
    EXPECT_TRUE(camEnt2 != entt::null);
    const Camera* cam2 = dst.GetComponent<Camera>(camEnt2);
    EXPECT_TRUE(cam2 != nullptr);
    EXPECT_NEAR(cam2->fov, 75.0f, 0.001f);
    EXPECT_NEAR(cam2->nearPlane, 0.25f, 0.001f);
    EXPECT_NEAR(cam2->farPlane, 512.0f, 0.001f);
    EXPECT_TRUE(cam2->isMainCamera);

    EntityID scriptEnt2 = FindByName(dst, "Scripted");
    EXPECT_TRUE(scriptEnt2 != entt::null);
    const Script* sc2 = dst.GetComponent<Script>(scriptEnt2);
    EXPECT_TRUE(sc2 != nullptr);
    EXPECT_STR_CONTAINS(sc2->scriptPath, "turret.as");
    EXPECT_STR_CONTAINS(sc2->className, "TurretBrain");
    EXPECT_STR_CONTAINS(sc2->moduleName, "gameplay");
    EXPECT_FALSE(sc2->enabled);
    // 'started' is a runtime flag, intentionally unregistered — must come
    // back as its default (false) so scripts re-init on load.
    EXPECT_FALSE(sc2->started);
}
