// TestRuntimePrefab.cpp - Tests for Spark::ECS::RuntimePrefab and PrefabRegistry
#include "TestFramework.h"
#include "Utils/Serializer.h"
#include "Engine/ECS/RuntimePrefab.h"

#include <memory>
#include <string>

// ============================================================================
// RuntimePrefab — construction and name
// ============================================================================

TEST(RuntimePrefab_Construction_GetName)
{
    Spark::ECS::RuntimePrefab prefab("EnemyGuard");
    EXPECT_TRUE(prefab.GetName() == "EnemyGuard");
}

TEST(RuntimePrefab_EmptyComponents)
{
    Spark::ECS::RuntimePrefab prefab("Empty");
    EXPECT_TRUE(prefab.GetComponents().empty());
}

// ============================================================================
// AddComponent and HasComponent
// ============================================================================

TEST(RuntimePrefab_AddComponent)
{
    Spark::ECS::RuntimePrefab prefab("Entity");
    prefab.AddComponent("Transform", {{"posX", "0"}, {"posY", "10"}});
    EXPECT_TRUE(prefab.HasComponent("Transform"));
    EXPECT_EQ(prefab.GetComponents().size(), 1u);
}

TEST(RuntimePrefab_HasComponent_False)
{
    Spark::ECS::RuntimePrefab prefab("Entity");
    prefab.AddComponent("Transform", {});
    EXPECT_FALSE(prefab.HasComponent("MeshRenderer"));
}

TEST(RuntimePrefab_AddMultipleComponents)
{
    Spark::ECS::RuntimePrefab prefab("Entity");
    prefab.AddComponent("Transform", {});
    prefab.AddComponent("MeshRenderer", {{"mesh", "guard.fbx"}});
    prefab.AddComponent("Collider", {{"type", "box"}});
    EXPECT_EQ(prefab.GetComponents().size(), 3u);
    EXPECT_TRUE(prefab.HasComponent("Transform"));
    EXPECT_TRUE(prefab.HasComponent("MeshRenderer"));
    EXPECT_TRUE(prefab.HasComponent("Collider"));
}

// ============================================================================
// RemoveComponent
// ============================================================================

TEST(RuntimePrefab_RemoveComponent_Existing)
{
    Spark::ECS::RuntimePrefab prefab("Entity");
    prefab.AddComponent("Transform", {});
    prefab.AddComponent("MeshRenderer", {});
    EXPECT_TRUE(prefab.RemoveComponent("Transform"));
    EXPECT_FALSE(prefab.HasComponent("Transform"));
    EXPECT_EQ(prefab.GetComponents().size(), 1u);
}

TEST(RuntimePrefab_RemoveComponent_NonExisting)
{
    Spark::ECS::RuntimePrefab prefab("Entity");
    prefab.AddComponent("Transform", {});
    EXPECT_FALSE(prefab.RemoveComponent("Physics"));
    EXPECT_EQ(prefab.GetComponents().size(), 1u);
}

// ============================================================================
// GetComponents
// ============================================================================

TEST(RuntimePrefab_GetComponents_ReturnsAdded)
{
    Spark::ECS::RuntimePrefab prefab("Entity");
    prefab.AddComponent("Transform", {{"posX", "5"}});
    prefab.AddComponent("Audio", {{"clip", "bang.wav"}});

    const auto& comps = prefab.GetComponents();
    EXPECT_EQ(comps.size(), 2u);
    EXPECT_TRUE(comps[0].typeName == "Transform");
    EXPECT_TRUE(comps[1].typeName == "Audio");
}

TEST(RuntimePrefab_GetComponents_Properties)
{
    Spark::ECS::RuntimePrefab prefab("Entity");
    prefab.AddComponent("Transform", {{"posX", "42"}, {"posY", "7"}});

    const auto& props = prefab.GetComponents()[0].properties;
    auto it = props.find("posX");
    EXPECT_TRUE(it != props.end());
    EXPECT_TRUE(it->second == "42");
}

// ============================================================================
// Clone
// ============================================================================

TEST(RuntimePrefab_Clone_IndependentCopy)
{
    Spark::ECS::RuntimePrefab original("Guard");
    original.AddComponent("Transform", {{"posX", "0"}});
    original.AddComponent("MeshRenderer", {{"mesh", "guard.fbx"}});

    auto clone = original.Clone();
    EXPECT_TRUE(clone != nullptr);
    EXPECT_TRUE(clone->GetName() == "Guard");
    EXPECT_EQ(clone->GetComponents().size(), 2u);
    EXPECT_TRUE(clone->HasComponent("Transform"));

    // Modify clone, original should be unchanged
    clone->RemoveComponent("Transform");
    EXPECT_FALSE(clone->HasComponent("Transform"));
    EXPECT_TRUE(original.HasComponent("Transform"));
}

TEST(RuntimePrefab_Clone_PreservesParent)
{
    Spark::ECS::RuntimePrefab parent("BaseEnemy");
    Spark::ECS::RuntimePrefab child("Guard");
    child.SetParent(&parent);

    auto clone = child.Clone();
    EXPECT_TRUE(clone->GetParent() == &parent);
}

// ============================================================================
// PrefabRegistry — RegisterPrefab and GetPrefab
// ============================================================================

TEST(PrefabRegistry_RegisterPrefab_ById)
{
    auto& reg = Spark::ECS::PrefabRegistry::GetInstance();
    // Reset state by removing all existing prefabs
    while (reg.GetRegisteredCount() > 0)
    {
        auto names = reg.GetAllPrefabNames();
        if (names.empty())
        {
            break;
        }
        // Find an ID to remove via name lookup
        const auto* p = reg.GetPrefab(names[0]);
        if (!p)
        {
            break;
        }
        // We need the ID; iterate to find it
        break;
    }

    auto prefab = std::make_unique<Spark::ECS::RuntimePrefab>("Barrel");
    prefab->AddComponent("Transform", {});
    uint32_t id = reg.RegisterPrefab(std::move(prefab));
    EXPECT_GT(id, 0u);

    const auto* found = reg.GetPrefab(id);
    ASSERT_TRUE(found != nullptr);
    EXPECT_TRUE(found->GetName() == "Barrel");

    reg.RemovePrefab(id);
}

TEST(PrefabRegistry_GetPrefab_ByName)
{
    auto& reg = Spark::ECS::PrefabRegistry::GetInstance();
    auto prefab = std::make_unique<Spark::ECS::RuntimePrefab>("Crate");
    prefab->AddComponent("MeshRenderer", {{"mesh", "crate.fbx"}});
    uint32_t id = reg.RegisterPrefab(std::move(prefab));

    const auto* found = reg.GetPrefab("Crate");
    ASSERT_TRUE(found != nullptr);
    EXPECT_TRUE(found->HasComponent("MeshRenderer"));

    reg.RemovePrefab(id);
}

TEST(PrefabRegistry_GetPrefab_NotFound)
{
    auto& reg = Spark::ECS::PrefabRegistry::GetInstance();
    EXPECT_TRUE(reg.GetPrefab("NonExistent_xyz") == nullptr);
    EXPECT_TRUE(reg.GetPrefab(99999u) == nullptr);
}

// ============================================================================
// PrefabRegistry — RemovePrefab
// ============================================================================

TEST(PrefabRegistry_RemovePrefab)
{
    auto& reg = Spark::ECS::PrefabRegistry::GetInstance();
    auto prefab = std::make_unique<Spark::ECS::RuntimePrefab>("Temporary");
    uint32_t id = reg.RegisterPrefab(std::move(prefab));

    EXPECT_TRUE(reg.GetPrefab(id) != nullptr);
    EXPECT_TRUE(reg.RemovePrefab(id));
    EXPECT_TRUE(reg.GetPrefab(id) == nullptr);
}

TEST(PrefabRegistry_RemovePrefab_NonExisting)
{
    auto& reg = Spark::ECS::PrefabRegistry::GetInstance();
    EXPECT_FALSE(reg.RemovePrefab(99999u));
}

// ============================================================================
// PrefabRegistry — GetRegisteredCount and GetAllPrefabNames
// ============================================================================

TEST(PrefabRegistry_GetRegisteredCount)
{
    auto& reg = Spark::ECS::PrefabRegistry::GetInstance();
    uint32_t countBefore = reg.GetRegisteredCount();

    auto p1 = std::make_unique<Spark::ECS::RuntimePrefab>("CountA");
    auto p2 = std::make_unique<Spark::ECS::RuntimePrefab>("CountB");
    uint32_t id1 = reg.RegisterPrefab(std::move(p1));
    uint32_t id2 = reg.RegisterPrefab(std::move(p2));

    EXPECT_EQ(reg.GetRegisteredCount(), countBefore + 2);

    reg.RemovePrefab(id1);
    reg.RemovePrefab(id2);
    EXPECT_EQ(reg.GetRegisteredCount(), countBefore);
}

TEST(PrefabRegistry_GetAllPrefabNames)
{
    auto& reg = Spark::ECS::PrefabRegistry::GetInstance();
    auto p1 = std::make_unique<Spark::ECS::RuntimePrefab>("NameA");
    auto p2 = std::make_unique<Spark::ECS::RuntimePrefab>("NameB");
    uint32_t id1 = reg.RegisterPrefab(std::move(p1));
    uint32_t id2 = reg.RegisterPrefab(std::move(p2));

    auto names = reg.GetAllPrefabNames();
    bool foundA = false;
    bool foundB = false;
    for (const auto& n : names)
    {
        if (n == "NameA")
        {
            foundA = true;
        }
        if (n == "NameB")
        {
            foundB = true;
        }
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);

    reg.RemovePrefab(id1);
    reg.RemovePrefab(id2);
}

TEST(PrefabRegistry_RegisterNull_ReturnsZero)
{
    auto& reg = Spark::ECS::PrefabRegistry::GetInstance();
    uint32_t id = reg.RegisterPrefab(nullptr);
    EXPECT_EQ(id, 0u);
}
