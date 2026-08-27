/**
 * @file TestSceneConfigDatabase.cpp
 * @brief Tests for per-scene configuration override database
 */

#include "TestFramework.h"
#include <string>
#include <unordered_map>

namespace
{

    struct TestSceneConfig
    {
        std::string sceneId;
        std::string displayName;
        float shadowDistance = 100.0f;
        int shadowCascades = 4;
        std::string reverbPreset;
        std::unordered_map<std::string, std::string> custom;
    };

    class TestSceneDB
    {
      public:
        void Register(const TestSceneConfig& config) { m_entries[config.sceneId] = config; }
        void Unregister(const std::string& id) { m_entries.erase(id); }

        const TestSceneConfig* Get(const std::string& id) const
        {
            auto it = m_entries.find(id);
            return it != m_entries.end() ? &it->second : nullptr;
        }

        bool Has(const std::string& id) const { return m_entries.count(id) > 0; }
        size_t Count() const { return m_entries.size(); }

      private:
        std::unordered_map<std::string, TestSceneConfig> m_entries;
    };

} // anonymous namespace

TEST(SceneConfigDB_RegisterLookup)
{
    TestSceneDB db;
    TestSceneConfig config;
    config.sceneId = "level01";
    config.displayName = "The Beginning";
    config.shadowDistance = 200.0f;
    config.reverbPreset = "Cave";

    db.Register(config);
    EXPECT_TRUE(db.Has("level01"));
    EXPECT_FALSE(db.Has("level02"));

    auto* found = db.Get("level01");
    ASSERT_TRUE(found != nullptr);
    EXPECT_NEAR(found->shadowDistance, 200.0f, 0.01f);
    EXPECT_EQ(found->reverbPreset, std::string("Cave"));
}

TEST(SceneConfigDB_Unregister)
{
    TestSceneDB db;
    TestSceneConfig config;
    config.sceneId = "arena";
    db.Register(config);
    EXPECT_EQ(db.Count(), 1u);

    db.Unregister("arena");
    EXPECT_EQ(db.Count(), 0u);
    EXPECT_FALSE(db.Has("arena"));
}

TEST(SceneConfigDB_CustomOverrides)
{
    TestSceneDB db;
    TestSceneConfig config;
    config.sceneId = "boss_room";
    config.custom["fog_density"] = "0.8";
    config.custom["particle_limit"] = "5000";
    db.Register(config);

    auto* found = db.Get("boss_room");
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(found->custom.at("fog_density"), std::string("0.8"));
    EXPECT_EQ(found->custom.at("particle_limit"), std::string("5000"));
}
