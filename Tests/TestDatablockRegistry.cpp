// TestDatablockRegistry.cpp - Tests for immutable shared-data object registry

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Networking/DatablockRegistry.h"

using namespace Spark::Net;

// Helper to get a fresh registry (since it's a singleton, clear between tests)
static DatablockRegistry& FreshRegistry()
{
    auto& reg = DatablockRegistry::Get();
    reg.Clear();
    return reg;
}

TEST(DatablockRegistry_RegisterAndFind)
{
    auto& reg = FreshRegistry();

    auto id = reg.Register("weapon.rifle", {{"damage", 35.0f}, {"magSize", int32_t(30)}, {"automatic", true}});

    EXPECT_NE(id, INVALID_DATABLOCK);

    auto* db = reg.Find("weapon.rifle");
    EXPECT_NE(db, nullptr);
    EXPECT_EQ(db->name, "weapon.rifle");
    EXPECT_EQ(db->GetFloat("damage"), 35.0f);
    EXPECT_EQ(db->GetInt("magSize"), 30);
    EXPECT_EQ(db->GetBool("automatic"), true);
}

TEST(DatablockRegistry_FindByID)
{
    auto& reg = FreshRegistry();

    auto id = reg.Register("item.potion", {{"heal", 50.0f}});
    auto* db = reg.FindByID(id);

    EXPECT_NE(db, nullptr);
    EXPECT_EQ(db->name, "item.potion");
    EXPECT_EQ(db->GetFloat("heal"), 50.0f);
}

TEST(DatablockRegistry_DuplicateNameRejected)
{
    auto& reg = FreshRegistry();

    auto id1 = reg.Register("weapon.rifle", {{"damage", 35.0f}});
    auto id2 = reg.Register("weapon.rifle", {{"damage", 50.0f}});

    EXPECT_NE(id1, INVALID_DATABLOCK);
    EXPECT_EQ(id2, INVALID_DATABLOCK);

    // Original value preserved
    EXPECT_EQ(reg.Find("weapon.rifle")->GetFloat("damage"), 35.0f);
}

TEST(DatablockRegistry_NotFound)
{
    auto& reg = FreshRegistry();
    EXPECT_EQ(reg.Find("nonexistent"), nullptr);
    EXPECT_EQ(reg.FindByID(9999), nullptr);
    EXPECT_EQ(reg.GetID("nonexistent"), INVALID_DATABLOCK);
}

TEST(DatablockRegistry_DefaultValues)
{
    auto& reg = FreshRegistry();
    reg.Register("test", {});

    auto* db = reg.Find("test");
    EXPECT_NE(db, nullptr);
    EXPECT_EQ(db->GetFloat("missing"), 0.0f);
    EXPECT_EQ(db->GetFloat("missing", 42.0f), 42.0f);
    EXPECT_EQ(db->GetInt("missing"), 0);
    EXPECT_EQ(db->GetBool("missing"), false);
    EXPECT_EQ(db->GetString("missing"), "");
    EXPECT_EQ(db->GetString("missing", "fallback"), "fallback");
}

TEST(DatablockRegistry_StringProperty)
{
    auto& reg = FreshRegistry();
    reg.Register("npc.guard", {{"greeting", std::string("Halt!")}, {"level", int32_t(5)}});

    auto* db = reg.Find("npc.guard");
    EXPECT_EQ(db->GetString("greeting"), "Halt!");
    EXPECT_EQ(db->GetInt("level"), 5);
}

TEST(DatablockRegistry_SerializeDeserialize)
{
    auto& reg = FreshRegistry();

    reg.Register("weapon.rifle", {{"damage", 35.0f}, {"magSize", int32_t(30)}, {"automatic", true}});
    reg.Register("weapon.pistol", {{"damage", 20.0f}, {"magSize", int32_t(12)}, {"automatic", false}});
    reg.Register("item.ammo", {{"count", int32_t(60)}, {"type", std::string("556")}});

    auto buffer = reg.SerializeAll();
    EXPECT_GT(buffer.size(), 0u);

    // Deserialize into fresh state
    reg.Clear();
    EXPECT_EQ(reg.Count(), 0u);

    bool ok = reg.DeserializeAll(buffer);
    EXPECT_TRUE(ok);
    EXPECT_EQ(reg.Count(), 3u);

    auto* rifle = reg.Find("weapon.rifle");
    EXPECT_NE(rifle, nullptr);
    EXPECT_EQ(rifle->GetFloat("damage"), 35.0f);
    EXPECT_EQ(rifle->GetInt("magSize"), 30);
    EXPECT_EQ(rifle->GetBool("automatic"), true);

    auto* ammo = reg.Find("item.ammo");
    EXPECT_NE(ammo, nullptr);
    EXPECT_EQ(ammo->GetInt("count"), 60);
    EXPECT_EQ(ammo->GetString("type"), "556");
}

TEST(DatablockRegistry_Clear)
{
    auto& reg = FreshRegistry();
    reg.Register("test1", {});
    reg.Register("test2", {});
    EXPECT_EQ(reg.Count(), 2u);

    reg.Clear();
    EXPECT_EQ(reg.Count(), 0u);
    EXPECT_EQ(reg.Find("test1"), nullptr);
}

TEST(DatablockRegistry_GetAll)
{
    auto& reg = FreshRegistry();
    reg.Register("a", {});
    reg.Register("b", {});
    reg.Register("c", {});

    auto all = reg.GetAll();
    EXPECT_EQ(all.size(), 3u);
}

TEST(DatablockRegistry_WrongTypeReturnsDefault)
{
    auto& reg = FreshRegistry();
    reg.Register("test", {{"val", 42.0f}});

    auto* db = reg.Find("test");
    // val is float, requesting int should return default
    EXPECT_EQ(db->GetInt("val"), 0);
    // val is float, requesting float should work
    EXPECT_EQ(db->GetFloat("val"), 42.0f);
}
