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

// Regression: a failed AddItem (capacity exceeded) must be transactional — it
// deposits nothing, so the inventory count is unchanged.
TEST(InventorySystemReal_AddItemFailureLeavesInventoryUnchanged)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    inv.RegisterItem(MakeItem(4001, "Brick", /*maxStack*/ 10));
    const uint32_t player = 200;

    inv.SetMaxSlots(player, 1); // exactly one slot → at most 10 bricks fit
    EXPECT_TRUE(inv.AddItem(player, 4001, 5));
    EXPECT_EQ(inv.GetItemCount(player, 4001), static_cast<uint32_t>(5));

    // 100 cannot fit (only 5 more would); must fail without mutating anything.
    EXPECT_FALSE(inv.AddItem(player, 4001, 100));
    EXPECT_EQ(inv.GetItemCount(player, 4001), static_cast<uint32_t>(5));
}

// Regression for the transfer-duplication path: transferring into a full
// destination must fail and leave BOTH sides untouched (no dupe, no loss).
TEST(InventorySystemReal_TransferIntoFullDestinationNoDuplication)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    inv.RegisterItem(MakeItem(5001, "Coin", /*maxStack*/ 10));
    const uint32_t source = 1;
    const uint32_t dest = 2;

    EXPECT_TRUE(inv.AddItem(source, 5001, 8));
    inv.SetMaxSlots(dest, 1);
    EXPECT_TRUE(inv.AddItem(dest, 5001, 10)); // destination is now full (1 slot × 10)

    EXPECT_FALSE(inv.TransferItem(source, dest, 5001, 5));
    // Source keeps its 8 (not removed), destination keeps its 10 (not grown).
    EXPECT_EQ(inv.GetItemCount(source, 5001), static_cast<uint32_t>(8));
    EXPECT_EQ(inv.GetItemCount(dest, 5001), static_cast<uint32_t>(10));
}

// A successful add that spans multiple stacks deposits exactly the requested
// count (10 + 10 + 5 across three slots).
TEST(InventorySystemReal_AddAcrossStackBoundariesAddsExactCount)
{
    ResetInventory();
    auto& inv = Spark::Gameplay::InventorySystem::GetInstance();
    inv.RegisterItem(MakeItem(6001, "Arrow", /*maxStack*/ 10));
    const uint32_t player = 3;

    EXPECT_TRUE(inv.AddItem(player, 6001, 25));
    EXPECT_EQ(inv.GetItemCount(player, 6001), static_cast<uint32_t>(25));
}
