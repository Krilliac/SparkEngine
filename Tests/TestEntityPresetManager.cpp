// TestEntityPresetManager.cpp - Tests for entity preset registry
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestEPM
{

    struct ComponentPreset
    {
        std::string componentType;
        std::unordered_map<std::string, std::string> properties;
    };

    struct EntityPreset
    {
        std::string name;
        std::string category;
        std::string description;
        std::vector<ComponentPreset> components;
    };

    class EntityPresetManager
    {
      public:
        void RegisterPreset(EntityPreset preset) { m_presets.push_back(std::move(preset)); }

        const std::vector<EntityPreset>& GetPresets() const { return m_presets; }

        const EntityPreset* FindPreset(const std::string& name) const
        {
            for (const auto& p : m_presets)
            {
                if (p.name == name)
                    return &p;
            }
            return nullptr;
        }

        std::unordered_map<std::string, std::vector<std::string>> GetPresetsByCategory() const
        {
            std::unordered_map<std::string, std::vector<std::string>> result;
            for (const auto& p : m_presets)
            {
                result[p.category].push_back(p.name);
            }
            return result;
        }

        void RegisterBuiltinPresets()
        {
            RegisterPreset(
                {"Player",
                 "Character",
                 "Player character",
                 {{"Transform", {}}, {"HealthComponent", {{"maxHealth", "100"}}}, {"Camera", {{"fov", "75"}}}}});
            RegisterPreset({"Enemy",
                            "Character",
                            "AI enemy",
                            {{"Transform", {}},
                             {"HealthComponent", {{"maxHealth", "50"}}},
                             {"AIComponent", {{"behaviorTree", "patrol"}}}}});
            RegisterPreset({"NPC", "Character", "Dialogue NPC", {{"Transform", {}}, {"AIComponent", {}}}});
            RegisterPreset({"Door",
                            "Prop",
                            "Interactive door",
                            {{"Transform", {}}, {"MeshRenderer", {}}, {"TriggerVolumeComponent", {}}}});
            RegisterPreset({"Health Pickup",
                            "Pickup",
                            "Restores health",
                            {{"Transform", {}}, {"TriggerVolumeComponent", {}}, {"ParticleEmitterComponent", {}}}});
            RegisterPreset({"Sound Source", "Effect", "3D audio", {{"Transform", {}}, {"AudioSourceComponent", {}}}});
        }

      private:
        std::vector<EntityPreset> m_presets;
    };

} // namespace TestEPM

// ============================================================================
// Tests
// ============================================================================

TEST(EntityPresetManager_RegisterAndFind)
{
    using namespace TestEPM;
    EntityPresetManager mgr;

    mgr.RegisterPreset({"TestEntity", "TestCategory", "A test preset", {{"Transform", {{"position", "1,2,3"}}}}});

    auto* found = mgr.FindPreset("TestEntity");
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(found->name, std::string("TestEntity"));
    EXPECT_EQ(found->category, std::string("TestCategory"));
    EXPECT_EQ(found->components.size(), size_t(1));
}

TEST(EntityPresetManager_FindReturnsNullForUnknown)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    auto* found = mgr.FindPreset("NonExistent");
    EXPECT_TRUE(found == nullptr);
}

TEST(EntityPresetManager_BuiltinPresetsCount)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    mgr.RegisterBuiltinPresets();
    EXPECT_EQ(mgr.GetPresets().size(), size_t(6));
}

TEST(EntityPresetManager_BuiltinPlayerPreset)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    mgr.RegisterBuiltinPresets();

    auto* player = mgr.FindPreset("Player");
    ASSERT_TRUE(player != nullptr);
    EXPECT_EQ(player->category, std::string("Character"));
    EXPECT_TRUE(player->components.size() >= 3);

    // Check it has Transform, HealthComponent, Camera
    bool hasTransform = false;
    bool hasHealth = false;
    bool hasCamera = false;
    for (const auto& comp : player->components)
    {
        if (comp.componentType == "Transform")
            hasTransform = true;
        if (comp.componentType == "HealthComponent")
            hasHealth = true;
        if (comp.componentType == "Camera")
            hasCamera = true;
    }
    EXPECT_TRUE(hasTransform);
    EXPECT_TRUE(hasHealth);
    EXPECT_TRUE(hasCamera);
}

TEST(EntityPresetManager_BuiltinEnemyPreset)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    mgr.RegisterBuiltinPresets();

    auto* enemy = mgr.FindPreset("Enemy");
    ASSERT_TRUE(enemy != nullptr);
    EXPECT_EQ(enemy->category, std::string("Character"));

    bool hasAI = false;
    for (const auto& comp : enemy->components)
    {
        if (comp.componentType == "AIComponent")
            hasAI = true;
    }
    EXPECT_TRUE(hasAI);
}

TEST(EntityPresetManager_GetPresetsByCategory)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    mgr.RegisterBuiltinPresets();

    auto categories = mgr.GetPresetsByCategory();
    EXPECT_TRUE(categories.count("Character") > 0);
    EXPECT_TRUE(categories.count("Prop") > 0);
    EXPECT_TRUE(categories.count("Pickup") > 0);
    EXPECT_TRUE(categories.count("Effect") > 0);

    // Characters should have Player, Enemy, NPC
    EXPECT_EQ(categories["Character"].size(), size_t(3));
}

TEST(EntityPresetManager_ComponentProperties)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    mgr.RegisterBuiltinPresets();

    auto* player = mgr.FindPreset("Player");
    ASSERT_TRUE(player != nullptr);

    for (const auto& comp : player->components)
    {
        if (comp.componentType == "HealthComponent")
        {
            EXPECT_TRUE(comp.properties.count("maxHealth") > 0);
            EXPECT_EQ(comp.properties.at("maxHealth"), std::string("100"));
        }
    }
}

TEST(EntityPresetManager_DuplicateNamesAllowed)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    mgr.RegisterPreset({"Dupe", "Cat", "First", {}});
    mgr.RegisterPreset({"Dupe", "Cat", "Second", {}});

    // FindPreset returns first match
    auto* found = mgr.FindPreset("Dupe");
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(found->description, std::string("First"));
    EXPECT_EQ(mgr.GetPresets().size(), size_t(2));
}

TEST(EntityPresetManager_EmptyPreset)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    mgr.RegisterPreset({"Empty", "Test", "No components", {}});

    auto* found = mgr.FindPreset("Empty");
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(found->components.size(), size_t(0));
}

TEST(EntityPresetManager_MultipleComponentsPerPreset)
{
    using namespace TestEPM;
    EntityPresetManager mgr;
    mgr.RegisterBuiltinPresets();

    auto* door = mgr.FindPreset("Door");
    ASSERT_TRUE(door != nullptr);
    EXPECT_TRUE(door->components.size() >= 3);
}
