// Test_gameplay_inventory.cpp — regression tests for InventorySystem transactional AddItem.
//
// Guards the P0 item-duplication exploit: AddItem must be all-or-nothing. Previously a
// partially-successful AddItem mutated the destination and THEN returned false when it hit
// the capacity guard mid-loop, so TransferItem (which only removes from the source on a
// true return) left the added items in the destination while keeping the full source stack
// — duplicating items. These tests fail before the fix and pass after it.

#include "../TestFramework.h"
#include "Engine/Gameplay/InventorySystem.h"

using namespace Spark::Gameplay;

namespace
{
    ItemDefinition MakeItem(uint32_t id, uint32_t maxStack)
    {
        ItemDefinition def;
        def.itemId = id;
        def.name = "item" + std::to_string(id);
        def.maxStackSize = maxStack;
        return def;
    }
} // namespace

TEST(Inventory_AddItem_IsAtomic_WhenOverCapacity)
{
    auto& inv = InventorySystem::GetInstance();
    inv.Initialize();
    inv.RegisterItem(MakeItem(1, 10)); // stack size 10

    const uint32_t entity = 100;
    inv.SetMaxSlots(entity, 2); // capacity = 2 slots * 10 = 20

    // Request more than fits (21 > 20). AddItem must add NOTHING and return false.
    // (Before the fix it filled both slots to 20 and then returned false.)
    EXPECT_FALSE(inv.AddItem(entity, 1, 21));
    EXPECT_EQ(inv.GetItemCount(entity, 1), 0u);
    EXPECT_EQ(inv.GetUsedSlots(entity), 0u);

    // Exactly-fits should still succeed.
    EXPECT_TRUE(inv.AddItem(entity, 1, 20));
    EXPECT_EQ(inv.GetItemCount(entity, 1), 20u);
}

TEST(Inventory_Transfer_DoesNotDuplicate_WhenDestinationCannotFit)
{
    auto& inv = InventorySystem::GetInstance();
    inv.Initialize();
    inv.RegisterItem(MakeItem(1, 5)); // stack size 5

    const uint32_t src = 1;
    const uint32_t dst = 2;

    inv.SetMaxSlots(src, 10);
    inv.AddItem(src, 1, 8); // src holds 8

    inv.SetMaxSlots(dst, 2);
    inv.AddItem(dst, 1, 3); // dst holds 3 in one partial slot; total dst capacity = (5-3) + 1 new slot*5 = 7

    const uint32_t srcBefore = inv.GetItemCount(src, 1);
    const uint32_t dstBefore = inv.GetItemCount(dst, 1);

    // Transferring 8 cannot fit into dst's remaining capacity of 7 → must fail cleanly.
    // Before the fix, AddItem would deposit 7 into dst then return false, leaving src at 8
    // (7 duplicated). After the fix, nothing moves.
    EXPECT_FALSE(inv.TransferItem(src, dst, 1, 8));

    EXPECT_EQ(inv.GetItemCount(src, 1), srcBefore);
    EXPECT_EQ(inv.GetItemCount(dst, 1), dstBefore);
    EXPECT_EQ(inv.GetItemCount(src, 1) + inv.GetItemCount(dst, 1), srcBefore + dstBefore);
}
