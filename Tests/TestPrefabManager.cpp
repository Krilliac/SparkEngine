/**
 * @file TestPrefabManager.cpp
 * @brief Tests for the prefab system
 *
 * Standalone reimplementation for CI testing without engine dependencies.
 */

#include "TestFramework.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone reimplementation
// ============================================================================

namespace PrefabTest
{

    using PropertyMap = std::unordered_map<std::string, std::string>;

    struct PrefabData
    {
        std::string name;
        PropertyMap properties;
        std::vector<std::string> childPrefabNames; // nested prefab references
    };

    class PrefabInstance
    {
      public:
        explicit PrefabInstance(const PrefabData& source)
            : m_sourceName(source.name), m_baseProperties(source.properties), m_properties(source.properties)
        {
        }

        std::string GetProperty(const std::string& key) const
        {
            auto it = m_properties.find(key);
            return (it != m_properties.end()) ? it->second : "";
        }

        void SetOverride(const std::string& key, const std::string& value)
        {
            m_overrides[key] = value;
            m_properties[key] = value;
        }

        bool HasOverride(const std::string& key) const { return m_overrides.count(key) > 0; }

        void RevertOverride(const std::string& key)
        {
            m_overrides.erase(key);
            auto baseIt = m_baseProperties.find(key);
            if (baseIt != m_baseProperties.end())
            {
                m_properties[key] = baseIt->second;
            }
            else
            {
                m_properties.erase(key);
            }
        }

        const std::string& GetSourceName() const { return m_sourceName; }
        const PropertyMap& GetProperties() const { return m_properties; }

        std::vector<std::unique_ptr<PrefabInstance>>& GetChildren() { return m_children; }
        const std::vector<std::unique_ptr<PrefabInstance>>& GetChildren() const { return m_children; }

        void AddChild(std::unique_ptr<PrefabInstance> child) { m_children.push_back(std::move(child)); }

      private:
        std::string m_sourceName;
        PropertyMap m_baseProperties;
        PropertyMap m_properties;
        PropertyMap m_overrides;
        std::vector<std::unique_ptr<PrefabInstance>> m_children;
    };

    class PrefabManager
    {
      public:
        void RegisterPrefab(const std::string& name, PrefabData data) { m_prefabs[name] = std::move(data); }

        std::unique_ptr<PrefabInstance> Instantiate(const std::string& name) const
        {
            auto it = m_prefabs.find(name);
            if (it == m_prefabs.end())
            {
                return nullptr;
            }

            auto instance = std::make_unique<PrefabInstance>(it->second);

            // Recursively instantiate child prefabs
            for (const auto& childName : it->second.childPrefabNames)
            {
                auto child = Instantiate(childName);
                if (child)
                {
                    instance->AddChild(std::move(child));
                }
            }

            return instance;
        }

      private:
        std::unordered_map<std::string, PrefabData> m_prefabs;
    };

} // namespace PrefabTest

// ============================================================================
// Tests
// ============================================================================

TEST(Prefab_Instantiation)
{
    using namespace PrefabTest;

    PrefabManager mgr;
    mgr.RegisterPrefab("Soldier", {
                                      "Soldier",
                                      {{"health", "100"}, {"speed", "5.0"}, {"armor", "heavy"}},
                                      {},
                                  });

    auto instance = mgr.Instantiate("Soldier");
    EXPECT_TRUE(instance != nullptr);
    EXPECT_TRUE(instance->GetSourceName() == "Soldier");
    EXPECT_TRUE(instance->GetProperty("health") == "100");
    EXPECT_TRUE(instance->GetProperty("speed") == "5.0");
    EXPECT_TRUE(instance->GetProperty("armor") == "heavy");
}

TEST(Prefab_PropertyOverrides)
{
    using namespace PrefabTest;

    PrefabManager mgr;
    mgr.RegisterPrefab("Soldier", {
                                      "Soldier",
                                      {{"health", "100"}, {"speed", "5.0"}},
                                      {},
                                  });

    auto instance = mgr.Instantiate("Soldier");

    // Override health
    instance->SetOverride("health", "150");
    EXPECT_TRUE(instance->GetProperty("health") == "150");
    EXPECT_TRUE(instance->HasOverride("health"));

    // Speed should remain unchanged
    EXPECT_TRUE(instance->GetProperty("speed") == "5.0");
    EXPECT_FALSE(instance->HasOverride("speed"));
}

TEST(Prefab_NestedPrefabs)
{
    using namespace PrefabTest;

    PrefabManager mgr;

    // Register a weapon prefab
    mgr.RegisterPrefab("Sword", {
                                    "Sword",
                                    {{"damage", "25"}, {"weight", "3.0"}},
                                    {},
                                });

    // Register a shield prefab
    mgr.RegisterPrefab("Shield", {
                                     "Shield",
                                     {{"defense", "15"}, {"weight", "5.0"}},
                                     {},
                                 });

    // Register a soldier that contains sword and shield
    mgr.RegisterPrefab("ArmedSoldier", {
                                           "ArmedSoldier",
                                           {{"health", "100"}},
                                           {"Sword", "Shield"},
                                       });

    auto instance = mgr.Instantiate("ArmedSoldier");
    EXPECT_TRUE(instance != nullptr);
    EXPECT_TRUE(instance->GetProperty("health") == "100");

    // Verify children were instantiated
    EXPECT_EQ(instance->GetChildren().size(), 2u);
    EXPECT_TRUE(instance->GetChildren()[0]->GetSourceName() == "Sword");
    EXPECT_TRUE(instance->GetChildren()[0]->GetProperty("damage") == "25");
    EXPECT_TRUE(instance->GetChildren()[1]->GetSourceName() == "Shield");
    EXPECT_TRUE(instance->GetChildren()[1]->GetProperty("defense") == "15");
}

TEST(Prefab_OverrideRevert)
{
    using namespace PrefabTest;

    PrefabManager mgr;
    mgr.RegisterPrefab("Soldier", {
                                      "Soldier",
                                      {{"health", "100"}, {"speed", "5.0"}},
                                      {},
                                  });

    auto instance = mgr.Instantiate("Soldier");

    // Override then revert
    instance->SetOverride("health", "200");
    EXPECT_TRUE(instance->GetProperty("health") == "200");
    EXPECT_TRUE(instance->HasOverride("health"));

    instance->RevertOverride("health");
    EXPECT_TRUE(instance->GetProperty("health") == "100");
    EXPECT_FALSE(instance->HasOverride("health"));
}
