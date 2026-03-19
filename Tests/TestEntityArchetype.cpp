// TestEntityArchetype.cpp - Tests for archetype registration, lookup, and category queries
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestArchetype
{

    using ArchetypeID = uint32_t;

    struct ComponentDefault
    {
        std::string componentName;
        std::string defaultValue;
    };

    struct EntityArchetype
    {
        ArchetypeID id = 0;
        std::string name;
        std::string category;
        std::vector<ComponentDefault> components;
    };

    struct EntityArchetypeRegistry
    {
        std::unordered_map<ArchetypeID, EntityArchetype> archetypes;
        std::unordered_map<std::string, ArchetypeID> nameIndex;

        bool RegisterArchetype(const EntityArchetype& archetype)
        {
            if (archetypes.count(archetype.id) > 0)
                return false;
            if (nameIndex.count(archetype.name) > 0)
                return false;
            archetypes[archetype.id] = archetype;
            nameIndex[archetype.name] = archetype.id;
            return true;
        }

        const EntityArchetype* GetArchetype(ArchetypeID id) const
        {
            auto it = archetypes.find(id);
            return it != archetypes.end() ? &it->second : nullptr;
        }

        const EntityArchetype* GetArchetypeByName(const std::string& name) const
        {
            auto nit = nameIndex.find(name);
            if (nit == nameIndex.end())
                return nullptr;
            return GetArchetype(nit->second);
        }

        bool HasArchetype(ArchetypeID id) const { return archetypes.count(id) > 0; }

        std::vector<const EntityArchetype*> GetArchetypesByCategory(const std::string& category) const
        {
            std::vector<const EntityArchetype*> result;
            for (const auto& [id, arch] : archetypes)
            {
                if (arch.category == category)
                    result.push_back(&arch);
            }
            return result;
        }
    };

} // namespace TestArchetype

// ============================================================================
// Registration
// ============================================================================

TEST(EntityArchetype_RegisterArchetype)
{
    TestArchetype::EntityArchetypeRegistry reg;
    TestArchetype::EntityArchetype arch{1, "Soldier", "NPC", {{"Health", "100"}, {"Weapon", "Rifle"}}};
    EXPECT_TRUE(reg.RegisterArchetype(arch));
    EXPECT_FALSE(reg.RegisterArchetype(arch)); // Duplicate ID
}

TEST(EntityArchetype_RegisterDuplicateName)
{
    TestArchetype::EntityArchetypeRegistry reg;
    reg.RegisterArchetype({1, "Soldier", "NPC", {}});
    EXPECT_FALSE(reg.RegisterArchetype({2, "Soldier", "NPC", {}})); // Duplicate name
}

// ============================================================================
// Lookup
// ============================================================================

TEST(EntityArchetype_GetArchetype)
{
    TestArchetype::EntityArchetypeRegistry reg;
    reg.RegisterArchetype({1, "Soldier", "NPC", {{"Health", "100"}}});

    const auto* arch = reg.GetArchetype(1);
    EXPECT_TRUE(arch != nullptr);
    EXPECT_EQ(arch->name, std::string("Soldier"));
    EXPECT_EQ(static_cast<int>(arch->components.size()), 1);

    EXPECT_TRUE(reg.GetArchetype(999) == nullptr);
}

TEST(EntityArchetype_GetArchetypeByName)
{
    TestArchetype::EntityArchetypeRegistry reg;
    reg.RegisterArchetype({5, "HealthPickup", "Pickup", {}});

    const auto* arch = reg.GetArchetypeByName("HealthPickup");
    EXPECT_TRUE(arch != nullptr);
    EXPECT_EQ(arch->id, (uint32_t)5);

    EXPECT_TRUE(reg.GetArchetypeByName("Nonexistent") == nullptr);
}

TEST(EntityArchetype_HasArchetype)
{
    TestArchetype::EntityArchetypeRegistry reg;
    reg.RegisterArchetype({1, "Barrel", "Prop", {}});
    EXPECT_TRUE(reg.HasArchetype(1));
    EXPECT_FALSE(reg.HasArchetype(99));
}

// ============================================================================
// Category Queries
// ============================================================================

TEST(EntityArchetype_GetArchetypesByCategory)
{
    TestArchetype::EntityArchetypeRegistry reg;
    reg.RegisterArchetype({1, "Soldier", "NPC", {}});
    reg.RegisterArchetype({2, "Civilian", "NPC", {}});
    reg.RegisterArchetype({3, "HealthPack", "Pickup", {}});
    reg.RegisterArchetype({4, "AmmoCrate", "Pickup", {}});
    reg.RegisterArchetype({5, "Barrel", "Prop", {}});

    auto npcs = reg.GetArchetypesByCategory("NPC");
    EXPECT_EQ(static_cast<int>(npcs.size()), 2);

    auto pickups = reg.GetArchetypesByCategory("Pickup");
    EXPECT_EQ(static_cast<int>(pickups.size()), 2);

    auto vehicles = reg.GetArchetypesByCategory("Vehicle");
    EXPECT_EQ(static_cast<int>(vehicles.size()), 0);
}
