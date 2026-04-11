/**
 * @file TestInventorySystemReal.cpp
 * @brief Real-class tests for Spark::Gameplay::InventorySystem
 */

#include "TestFramework.h"
#include "Engine/Gameplay/InventorySystem.h"

namespace
{
    void ResetInventory()
    {
        auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
        inv.Shutdown();
        inv.Initialize();
    }

    Spark::Gameplay::ItemDefinition MakeItem(uint32_t id, const std::string& name, uint32_t maxStack = 99)
    {
        Spark::Gameplay::ItemDefinition def;
        def.itemId = id;
        def.name = name;
        def.maxStackSize = maxStack;
        return def;
    }
} // namespace

TEST(InventorySystemReal_SingletonStable)
{
    auto& a = Spark::Gameplay::InventorySystem::GetInstance();
    auto& b = Spark::Gameplay::InventorySystem::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(InventorySystemReal_InitializeShutdown)
{
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    inv.Shutdown();
    inv.Initialize();
    inv.Shutdown();
    inv.Initialize();
}

TEST(InventorySystemReal_RegisterAndLookupItem)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    inv.RegisterItem(MakeItem(1001, "Potion"));
    const auto* def = inv.GetItemDef(1001);
    EXPECT_TRUE(def != nullptr);
    if (def)
        EXPECT_EQ(def->name, std::string("Potion"));
}

TEST(InventorySystemReal_GetItemDefUnknownReturnsNull)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    EXPECT_TRUE(inv.GetItemDef(99999) == nullptr);
}

TEST(InventorySystemReal_AddAndRemoveItem)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    inv.RegisterItem(MakeItem(2001, "Sword"));
    const uint32_t player = 100;

    EXPECT_TRUE(inv.AddItem(player, 2001, 5));
    EXPECT_EQ(inv.GetItemCount(player, 2001), static_cast<uint32_t>(5));
    EXPECT_TRUE(inv.HasItem(player, 2001, 3));
    EXPECT_FALSE(inv.HasItem(player, 2001, 10));

    EXPECT_TRUE(inv.RemoveItem(player, 2001, 2));
    EXPECT_EQ(inv.GetItemCount(player, 2001), static_cast<uint32_t>(3));
}

TEST(InventorySystemReal_GetItemCountUnknownReturnsZero)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    EXPECT_EQ(inv.GetItemCount(999, 888), static_cast<uint32_t>(0));
}

TEST(InventorySystemReal_TransferItem)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    inv.RegisterItem(MakeItem(3001, "Gem"));

    inv.AddItem(1, 3001, 10);
    EXPECT_TRUE(inv.TransferItem(1, 2, 3001, 4));
    EXPECT_EQ(inv.GetItemCount(1, 3001), static_cast<uint32_t>(6));
    EXPECT_EQ(inv.GetItemCount(2, 3001), static_cast<uint32_t>(4));
}

TEST(InventorySystemReal_ConsoleStatusReturnsString)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    const auto status = inv.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
}
