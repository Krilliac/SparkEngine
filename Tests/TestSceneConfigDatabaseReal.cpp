/**
 * @file TestSceneConfigDatabaseReal.cpp
 * @brief Real-class tests for Spark::SceneConfigDatabase
 */

#include "TestFramework.h"
#include "SceneManager/SceneConfigDatabase.h"

namespace
{
    void ResetDB()
    {
        auto& db = Spark::SceneConfigDatabase::GetInstance();
        db.Shutdown();
        db.Initialize();
    }

    Spark::SceneConfigEntry MakeEntry(const std::string& id, const std::string& name = "")
    {
        Spark::SceneConfigEntry e;
        e.sceneId = id;
        e.displayName = name.empty() ? id : name;
        e.description = "Test scene";
        return e;
    }
} // namespace

TEST(SceneConfigDatabaseReal_SingletonStable)
{
    auto& a = Spark::SceneConfigDatabase::GetInstance();
    auto& b = Spark::SceneConfigDatabase::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(SceneConfigDatabaseReal_EmptyAfterInit)
{
    ResetDB();
    auto& db = Spark::SceneConfigDatabase::GetInstance();
    EXPECT_TRUE(db.IsInitialized());
    EXPECT_EQ(db.GetEntryCount(), size_t(0));
}

TEST(SceneConfigDatabaseReal_RegisterAndGet)
{
    ResetDB();
    auto& db = Spark::SceneConfigDatabase::GetInstance();
    db.RegisterScene(MakeEntry("level_01", "Tutorial"));

    EXPECT_TRUE(db.HasConfig("level_01"));
    auto* cfg = db.GetConfig("level_01");
    EXPECT_TRUE(cfg != nullptr);
    EXPECT_EQ(cfg->displayName, std::string("Tutorial"));
}

TEST(SceneConfigDatabaseReal_GetMissing)
{
    ResetDB();
    auto& db = Spark::SceneConfigDatabase::GetInstance();
    EXPECT_FALSE(db.HasConfig("nonexistent"));
    EXPECT_TRUE(db.GetConfig("nonexistent") == nullptr);
}

TEST(SceneConfigDatabaseReal_UnregisterScene)
{
    ResetDB();
    auto& db = Spark::SceneConfigDatabase::GetInstance();
    db.RegisterScene(MakeEntry("level_01"));
    db.UnregisterScene("level_01");
    EXPECT_FALSE(db.HasConfig("level_01"));
    EXPECT_EQ(db.GetEntryCount(), size_t(0));
}

TEST(SceneConfigDatabaseReal_CustomOverrides)
{
    ResetDB();
    auto& db = Spark::SceneConfigDatabase::GetInstance();
    auto entry = MakeEntry("level_02");
    entry.customOverrides["fog_color"] = "#AABBCC";
    entry.customOverrides["wind_speed"] = "5.0";
    db.RegisterScene(entry);

    auto val = db.GetCustomOverride("level_02", "fog_color");
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(val.value(), std::string("#AABBCC"));

    auto missing = db.GetCustomOverride("level_02", "nonexistent_key");
    EXPECT_FALSE(missing.has_value());

    auto noScene = db.GetCustomOverride("no_scene", "fog_color");
    EXPECT_FALSE(noScene.has_value());
}

TEST(SceneConfigDatabaseReal_RenderOverrides)
{
    ResetDB();
    auto& db = Spark::SceneConfigDatabase::GetInstance();
    auto entry = MakeEntry("level_03");
    entry.render.shadowDistance = 200.0f;
    entry.render.enableSSAO = true;
    db.RegisterScene(entry);

    auto* cfg = db.GetConfig("level_03");
    EXPECT_TRUE(cfg != nullptr);
    EXPECT_TRUE(cfg->render.shadowDistance.has_value());
    EXPECT_NEAR(cfg->render.shadowDistance.value(), 200.0f, 0.01f);
    EXPECT_TRUE(cfg->render.enableSSAO.value());
    EXPECT_FALSE(cfg->render.enableSSR.has_value());
}

TEST(SceneConfigDatabaseReal_GetAllSceneIds)
{
    ResetDB();
    auto& db = Spark::SceneConfigDatabase::GetInstance();
    db.RegisterScene(MakeEntry("a"));
    db.RegisterScene(MakeEntry("b"));
    db.RegisterScene(MakeEntry("c"));

    auto ids = db.GetAllSceneIds();
    EXPECT_EQ(ids.size(), size_t(3));
}

TEST(SceneConfigDatabaseReal_ConsoleReport)
{
    ResetDB();
    auto& db = Spark::SceneConfigDatabase::GetInstance();
    db.RegisterScene(MakeEntry("level_01", "Tutorial"));
    std::string report = db.Console_GetReport();
    EXPECT_STR_CONTAINS(report, "Scene Config Database");
    EXPECT_STR_CONTAINS(report, "level_01");
}
