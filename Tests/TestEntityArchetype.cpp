/**
 * @file TestEntityArchetype.cpp
 * @brief Production tests for archetype registration, lookup, and lifecycle.
 */

#include "TestFramework.h"
#include "Engine/ECS/EntityArchetype.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Spark::ECS::Archetype;
    using Spark::ECS::EntityArchetypeSystem;

    struct EntityArchetypeFixture
    {
        EntityArchetypeSystem& system = EntityArchetypeSystem::GetInstance();

        void SetUp() { system.Initialize(); }
        void TearDown() { system.Shutdown(); }
    };

    Archetype MakeArchetype(std::string name, std::string category)
    {
        Archetype archetype;
        archetype.name = std::move(name);
        archetype.category = std::move(category);
        archetype.components = {
            {"Transform", {{"posX", "1"}, {"posY", "2"}}},
            {"Health", {{"maximum", "100"}}},
        };
        return archetype;
    }
} // namespace

TEST(EntityArchetype_RejectsRegistrationBeforeInitialize)
{
    auto& system = EntityArchetypeSystem::GetInstance();
    system.Shutdown();

    system.RegisterArchetype(MakeArchetype("Soldier", "NPC"));

    EXPECT_FALSE(system.HasArchetype("Soldier"));
    EXPECT_EQ(system.GetArchetypeCount(), static_cast<size_t>(0));
}

TEST_F(EntityArchetypeFixture, RegistersAndRetrievesProductionArchetype)
{
    system.RegisterArchetype(MakeArchetype("Soldier", "NPC"));

    const Archetype* archetype = system.GetArchetype("Soldier");
    ASSERT_TRUE(archetype != nullptr);
    EXPECT_EQ(archetype->name, std::string("Soldier"));
    EXPECT_EQ(archetype->category, std::string("NPC"));
    ASSERT_EQ(archetype->components.size(), static_cast<size_t>(2));
    EXPECT_EQ(archetype->components[0].typeName, std::string("Transform"));
    EXPECT_EQ(archetype->components[0].properties.at("posY"), std::string("2"));
    EXPECT_TRUE(system.HasArchetype("Soldier"));
    EXPECT_TRUE(system.GetArchetype("Missing") == nullptr);
}

TEST_F(EntityArchetypeFixture, RejectsEmptyNameAndReplacesMatchingName)
{
    system.RegisterArchetype(MakeArchetype("", "Invalid"));
    EXPECT_EQ(system.GetArchetypeCount(), static_cast<size_t>(0));

    system.RegisterArchetype(MakeArchetype("Soldier", "NPC"));
    Archetype replacement = MakeArchetype("Soldier", "EliteNPC");
    replacement.components.resize(1);
    system.RegisterArchetype(replacement);

    EXPECT_EQ(system.GetArchetypeCount(), static_cast<size_t>(1));
    const Archetype* archetype = system.GetArchetype("Soldier");
    ASSERT_TRUE(archetype != nullptr);
    EXPECT_EQ(archetype->category, std::string("EliteNPC"));
    EXPECT_EQ(archetype->components.size(), static_cast<size_t>(1));
}

TEST_F(EntityArchetypeFixture, QueriesCategoriesAndAllNames)
{
    system.RegisterArchetype(MakeArchetype("Soldier", "NPC"));
    system.RegisterArchetype(MakeArchetype("Civilian", "NPC"));
    system.RegisterArchetype(MakeArchetype("HealthPack", "Pickup"));

    auto npcs = system.GetArchetypesByCategory("NPC");
    ASSERT_EQ(npcs.size(), static_cast<size_t>(2));
    EXPECT_TRUE(
        std::all_of(npcs.begin(), npcs.end(), [](const Archetype* archetype) { return archetype->category == "NPC"; }));
    EXPECT_TRUE(system.GetArchetypesByCategory("Vehicle").empty());

    auto names = system.GetAllArchetypeNames();
    std::sort(names.begin(), names.end());
    const std::vector<std::string> expected = {"Civilian", "HealthPack", "Soldier"};
    ASSERT_EQ(names.size(), expected.size());
    EXPECT_TRUE(std::equal(names.begin(), names.end(), expected.begin()));
}

TEST_F(EntityArchetypeFixture, RemovesArchetypeAndShutdownClearsRegistry)
{
    system.RegisterArchetype(MakeArchetype("Barrel", "Prop"));
    system.RegisterArchetype(MakeArchetype("Crate", "Prop"));

    system.RemoveArchetype("Barrel");
    EXPECT_FALSE(system.HasArchetype("Barrel"));
    EXPECT_TRUE(system.HasArchetype("Crate"));
    system.RemoveArchetype("Missing");
    EXPECT_EQ(system.GetArchetypeCount(), static_cast<size_t>(1));

    system.Shutdown();
    EXPECT_EQ(system.GetArchetypeCount(), static_cast<size_t>(0));
    EXPECT_TRUE(system.GetAllArchetypeNames().empty());
}
