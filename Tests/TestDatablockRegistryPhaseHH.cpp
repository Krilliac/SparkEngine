/**
 * @file TestDatablockRegistryPhaseHH.cpp
 * @brief Phase HH Theme 3D tests for Spark::Net::DatablockRegistry
 */

#include "TestFramework.h"
#include "Engine/Networking/DatablockRegistry.h"

namespace
{
    void ResetRegistry()
    {
        Spark::Net::DatablockRegistry::Get().Clear();
    }
} // namespace

TEST(DatablockRegistryPhaseHH_SingletonStable)
{
    auto& a = Spark::Net::DatablockRegistry::Get();
    auto& b = Spark::Net::DatablockRegistry::Get();
    EXPECT_TRUE(&a == &b);
}

TEST(DatablockRegistryPhaseHH_EmptyOnClear)
{
    ResetRegistry();
    auto& reg = Spark::Net::DatablockRegistry::Get();
    EXPECT_TRUE(reg.Find("anything") == nullptr);
}

TEST(DatablockRegistryPhaseHH_RegisterAndFind)
{
    ResetRegistry();
    auto& reg = Spark::Net::DatablockRegistry::Get();

    std::unordered_map<std::string, Spark::Net::DatablockValue> props;
    props["damage"] = 42.5f;
    props["fireRate"] = 0.1f;
    props["magSize"] = static_cast<int32_t>(30);
    const auto id = reg.Register("weapon.rifle", props);
    EXPECT_TRUE(id != Spark::Net::INVALID_DATABLOCK);

    const auto* block = reg.Find("weapon.rifle");
    EXPECT_TRUE(block != nullptr);
    if (block)
    {
        EXPECT_NEAR(block->GetFloat("damage"), 42.5f, 0.001f);
        EXPECT_NEAR(block->GetFloat("fireRate"), 0.1f, 0.001f);
        EXPECT_EQ(block->GetInt("magSize"), 30);
    }
}

TEST(DatablockRegistryPhaseHH_FindUnknownReturnsNull)
{
    ResetRegistry();
    auto& reg = Spark::Net::DatablockRegistry::Get();
    EXPECT_TRUE(reg.Find("nonexistent.datablock") == nullptr);
}

TEST(DatablockRegistryPhaseHH_FindByIdMatchesByName)
{
    ResetRegistry();
    auto& reg = Spark::Net::DatablockRegistry::Get();

    std::unordered_map<std::string, Spark::Net::DatablockValue> props;
    props["hp"] = static_cast<int32_t>(100);
    const auto id = reg.Register("enemy.soldier", props);

    const auto* byName = reg.Find("enemy.soldier");
    const auto* byId = reg.FindByID(id);
    EXPECT_TRUE(byName != nullptr);
    EXPECT_TRUE(byId != nullptr);
    EXPECT_TRUE(byName == byId);
}

TEST(DatablockRegistryPhaseHH_SerializeDeserializeRoundTrip)
{
    ResetRegistry();
    auto& reg = Spark::Net::DatablockRegistry::Get();

    {
        std::unordered_map<std::string, Spark::Net::DatablockValue> props;
        props["damage"] = 10.0f;
        props["name"] = std::string("Shotgun");
        reg.Register("weapon.shotgun", props);
    }
    {
        std::unordered_map<std::string, Spark::Net::DatablockValue> props;
        props["speed"] = 50.0f;
        reg.Register("vehicle.car", props);
    }

    auto buffer = reg.SerializeAll();
    EXPECT_TRUE(!buffer.empty());

    reg.Clear();
    EXPECT_TRUE(reg.Find("weapon.shotgun") == nullptr);

    const bool ok = reg.DeserializeAll(buffer);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(reg.Find("weapon.shotgun") != nullptr);
    EXPECT_TRUE(reg.Find("vehicle.car") != nullptr);
}

TEST(DatablockRegistryPhaseHH_ClearRemovesAll)
{
    ResetRegistry();
    auto& reg = Spark::Net::DatablockRegistry::Get();

    std::unordered_map<std::string, Spark::Net::DatablockValue> props;
    props["dmg"] = 5.0f;
    reg.Register("weapon.pistol", props);

    reg.Clear();
    EXPECT_TRUE(reg.Find("weapon.pistol") == nullptr);
}

TEST(DatablockRegistryPhaseHH_DatablockGetFloatFallback)
{
    ResetRegistry();
    auto& reg = Spark::Net::DatablockRegistry::Get();

    std::unordered_map<std::string, Spark::Net::DatablockValue> props;
    props["real_field"] = 1.5f;
    reg.Register("fallback.test", props);

    const auto* block = reg.Find("fallback.test");
    EXPECT_TRUE(block != nullptr);
    if (block)
    {
        EXPECT_NEAR(block->GetFloat("real_field", 0.0f), 1.5f, 0.001f);
        EXPECT_NEAR(block->GetFloat("missing_field", 99.0f), 99.0f, 0.001f);
    }
}
