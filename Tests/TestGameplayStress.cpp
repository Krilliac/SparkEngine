// TestGameplayStress.cpp - Stress tests for gameplay systems
// Uses standalone structs matching TestInventorySystem.cpp, TestQuestSystem.cpp,
// TestWeaponSystem.cpp, TestAbilitySystem.cpp patterns. Includes real EventBus header.

#include "TestFramework.h"
#include "Utils/EventBus.h"
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone Inventory (mirrors TestInventorySystem.cpp)
// ============================================================================

namespace StressInventory
{

    struct ItemDef
    {
        uint32_t id = 0;
        std::string name;
        int maxStackSize = 1;
        float weight = 1.0f;
    };

    struct ItemStack
    {
        uint32_t itemDefId = 0;
        int count = 0;
        bool IsEmpty() const { return count <= 0 || itemDefId == 0; }
    };

    struct InventoryComponent
    {
        std::vector<ItemStack> slots;
        int maxSlots = 20;
        float maxWeight = 100.0f;
    };

    class ItemRegistry
    {
      public:
        void RegisterItem(const ItemDef& def) { m_items[def.id] = def; }
        const ItemDef* GetItem(uint32_t id) const
        {
            auto it = m_items.find(id);
            return (it != m_items.end()) ? &it->second : nullptr;
        }

      private:
        std::unordered_map<uint32_t, ItemDef> m_items;
    };

    namespace Ops
    {

        inline float GetTotalWeight(const InventoryComponent& inv, const ItemRegistry& reg)
        {
            float total = 0.0f;
            for (const auto& slot : inv.slots)
            {
                if (!slot.IsEmpty())
                {
                    if (auto* def = reg.GetItem(slot.itemDefId))
                        total += def->weight * slot.count;
                }
            }
            return total;
        }

        inline int AddItem(InventoryComponent& inv, const ItemRegistry& reg, uint32_t itemId, int count = 1)
        {
            const ItemDef* def = reg.GetItem(itemId);
            if (!def || count <= 0)
                return 0;

            float currentWeight = GetTotalWeight(inv, reg);
            float addedWeight = def->weight * count;
            if (currentWeight + addedWeight > inv.maxWeight)
            {
                int fittable = static_cast<int>((inv.maxWeight - currentWeight) / def->weight);
                if (fittable <= 0)
                    return 0;
                count = std::min(count, fittable);
            }

            int remaining = count;
            for (auto& slot : inv.slots)
            {
                if (remaining <= 0)
                    break;
                if (slot.itemDefId == itemId && slot.count < def->maxStackSize)
                {
                    int canAdd = std::min(remaining, def->maxStackSize - slot.count);
                    slot.count += canAdd;
                    remaining -= canAdd;
                }
            }
            while (remaining > 0 && static_cast<int>(inv.slots.size()) < inv.maxSlots)
            {
                int stackSize = std::min(remaining, def->maxStackSize);
                inv.slots.push_back({itemId, stackSize});
                remaining -= stackSize;
            }
            return count - remaining;
        }

        inline int RemoveItem(InventoryComponent& inv, uint32_t itemId, int count = 1)
        {
            if (count <= 0)
                return 0;
            int remaining = count;
            for (auto it = inv.slots.begin(); it != inv.slots.end() && remaining > 0;)
            {
                if (it->itemDefId == itemId)
                {
                    int toRemove = std::min(remaining, it->count);
                    it->count -= toRemove;
                    remaining -= toRemove;
                    if (it->count <= 0)
                    {
                        it = inv.slots.erase(it);
                        continue;
                    }
                }
                ++it;
            }
            return count - remaining;
        }

        inline int CountItem(const InventoryComponent& inv, uint32_t itemId)
        {
            int total = 0;
            for (const auto& slot : inv.slots)
                if (slot.itemDefId == itemId)
                    total += slot.count;
            return total;
        }

    } // namespace Ops

} // namespace StressInventory

// ============================================================================
// Standalone Quest (mirrors TestQuestSystem.cpp)
// ============================================================================

namespace StressQuest
{

    enum class QuestStatus
    {
        NotStarted,
        Active,
        Completed,
        Failed
    };

    struct QuestObjective
    {
        std::string description;
        int requiredCount = 1;
        bool optional = false;
    };

    struct ObjectiveProgress
    {
        int currentCount = 0;
        bool completed = false;
    };

    struct QuestDef
    {
        uint32_t id = 0;
        std::string name;
        std::vector<QuestObjective> objectives;
        std::vector<uint32_t> requiredQuests;
    };

    struct ActiveQuest
    {
        uint32_t questId = 0;
        QuestStatus status = QuestStatus::Active;
        std::vector<ObjectiveProgress> objectiveProgress;
    };

    struct QuestJournal
    {
        std::vector<ActiveQuest> activeQuests;
        std::unordered_map<uint32_t, QuestStatus> completedQuests;
    };

    class QuestRegistry
    {
      public:
        void RegisterQuest(const QuestDef& def) { m_quests[def.id] = def; }
        const QuestDef* GetQuest(uint32_t id) const
        {
            auto it = m_quests.find(id);
            return (it != m_quests.end()) ? &it->second : nullptr;
        }

      private:
        std::unordered_map<uint32_t, QuestDef> m_quests;
    };

    namespace Ops
    {

        inline bool StartQuest(QuestJournal& journal, const QuestRegistry& reg, uint32_t questId)
        {
            const QuestDef* def = reg.GetQuest(questId);
            if (!def)
                return false;
            // Check if already active
            for (const auto& aq : journal.activeQuests)
                if (aq.questId == questId && aq.status == QuestStatus::Active)
                    return false;
            // Check prerequisites
            for (uint32_t reqId : def->requiredQuests)
            {
                auto it = journal.completedQuests.find(reqId);
                if (it == journal.completedQuests.end() || it->second != QuestStatus::Completed)
                    return false;
            }

            ActiveQuest aq;
            aq.questId = questId;
            aq.objectiveProgress.resize(def->objectives.size());
            journal.activeQuests.push_back(std::move(aq));
            return true;
        }

        inline bool UpdateObjective(QuestJournal& journal, const QuestRegistry& reg, uint32_t questId, int objIndex,
                                    int increment = 1)
        {
            const QuestDef* def = reg.GetQuest(questId);
            if (!def)
                return false;
            for (auto& aq : journal.activeQuests)
            {
                if (aq.questId == questId && aq.status == QuestStatus::Active)
                {
                    if (objIndex < 0 || objIndex >= static_cast<int>(aq.objectiveProgress.size()))
                        return false;
                    auto& prog = aq.objectiveProgress[objIndex];
                    if (prog.completed)
                        return false;
                    prog.currentCount += increment;
                    if (prog.currentCount >= def->objectives[objIndex].requiredCount)
                    {
                        prog.currentCount = def->objectives[objIndex].requiredCount;
                        prog.completed = true;
                    }
                    // Check if all required objectives are done
                    bool allDone = true;
                    for (int i = 0; i < static_cast<int>(def->objectives.size()); ++i)
                    {
                        if (!def->objectives[i].optional && !aq.objectiveProgress[i].completed)
                        {
                            allDone = false;
                            break;
                        }
                    }
                    if (allDone)
                    {
                        aq.status = QuestStatus::Completed;
                        journal.completedQuests[questId] = QuestStatus::Completed;
                    }
                    return true;
                }
            }
            return false;
        }

    } // namespace Ops

} // namespace StressQuest

// ============================================================================
// Standalone Weapon (mirrors TestWeaponSystem.cpp)
// ============================================================================

namespace StressWeapon
{

    struct WeaponInstance
    {
        int currentAmmo = 30;
        int reserveAmmo = 120;
        int magazineSize = 30;
        float fireCooldown = 0.0f;
        float reloadTime = 2.0f;

        bool HasAmmo() const { return currentAmmo > 0; }

        bool Fire()
        {
            if (currentAmmo <= 0)
                return false;
            --currentAmmo;
            return true;
        }

        bool Reload()
        {
            if (currentAmmo >= magazineSize || reserveAmmo <= 0)
                return false;
            int needed = magazineSize - currentAmmo;
            int available = std::min(needed, reserveAmmo);
            currentAmmo += available;
            reserveAmmo -= available;
            return true;
        }
    };

} // namespace StressWeapon

// ============================================================================
// Standalone Ability (mirrors TestAbilitySystem.cpp)
// ============================================================================

namespace StressAbility
{

    using AbilityID = uint32_t;
    using AuraID = uint32_t;
    using EntityID = uint32_t;

    struct AbilityDefinition
    {
        AbilityID id = 0;
        std::string name;
        float cooldown = 0.0f;
        float resourceCost = 0.0f;
    };

    struct AuraDefinition
    {
        AuraID id = 0;
        std::string name;
        int maxStacks = 1;
        float duration = 10.0f;
    };

    struct ActiveAura
    {
        AuraID definitionID = 0;
        EntityID target = 0;
        int stacks = 1;
        float remainingDuration = 0.0f;
    };

    struct AbilitySystem
    {
        std::unordered_map<AbilityID, AbilityDefinition> abilities;
        std::unordered_map<AuraID, AuraDefinition> auraDefs;
        std::vector<ActiveAura> activeAuras;
        std::unordered_map<AbilityID, float> cooldowns;
        float currentResource = 100.0f;

        void RegisterAbility(const AbilityDefinition& def) { abilities[def.id] = def; }
        void RegisterAura(const AuraDefinition& def) { auraDefs[def.id] = def; }

        bool Cast(AbilityID id)
        {
            auto it = abilities.find(id);
            if (it == abilities.end())
                return false;
            auto cdIt = cooldowns.find(id);
            if (cdIt != cooldowns.end() && cdIt->second > 0.0f)
                return false;
            if (currentResource < it->second.resourceCost)
                return false;
            currentResource -= it->second.resourceCost;
            cooldowns[id] = it->second.cooldown;
            return true;
        }

        void ApplyAura(AuraID auraID, EntityID target)
        {
            auto it = auraDefs.find(auraID);
            if (it == auraDefs.end())
                return;
            // Check for existing aura on target to stack
            for (auto& aura : activeAuras)
            {
                if (aura.definitionID == auraID && aura.target == target)
                {
                    if (aura.stacks < it->second.maxStacks)
                        aura.stacks++;
                    aura.remainingDuration = it->second.duration;
                    return;
                }
            }
            activeAuras.push_back({auraID, target, 1, it->second.duration});
        }

        int GetAuraCount(EntityID target) const
        {
            int count = 0;
            for (const auto& aura : activeAuras)
                if (aura.target == target)
                    ++count;
            return count;
        }

        void UpdateCooldowns(float dt)
        {
            for (auto& [id, cd] : cooldowns)
            {
                cd -= dt;
                if (cd < 0.0f)
                    cd = 0.0f;
            }
        }
    };

} // namespace StressAbility

// ============================================================================
// Inventory Stress Tests
// ============================================================================

TEST(GameplayStress_InventoryOverflow)
{
    // Try to add 10000 items to an inventory with capacity 100
    StressInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 99, 0.01f}); // High stack, low weight
    StressInventory::InventoryComponent inv;
    inv.maxSlots = 100;
    inv.maxWeight = 1000.0f;

    int added = StressInventory::Ops::AddItem(inv, reg, 1, 10000);

    // Should be capped by slot count * max stack size = 100 * 99 = 9900
    EXPECT_LE(added, 9900);
    EXPECT_GT(added, 0);
    EXPECT_LE(static_cast<int>(inv.slots.size()), 100);
}

TEST(GameplayStress_InventoryNegativeQuantity)
{
    // Adding a negative quantity should add nothing
    StressInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    StressInventory::InventoryComponent inv;

    int added = StressInventory::Ops::AddItem(inv, reg, 1, -1);
    EXPECT_EQ(added, 0);
    EXPECT_TRUE(inv.slots.empty());
}

TEST(GameplayStress_InventoryRemoveMoreThanExists)
{
    // Remove 999 items when only 5 exist
    StressInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    StressInventory::InventoryComponent inv;

    StressInventory::Ops::AddItem(inv, reg, 1, 5);
    EXPECT_EQ(StressInventory::Ops::CountItem(inv, 1), 5);

    int removed = StressInventory::Ops::RemoveItem(inv, 1, 999);
    EXPECT_EQ(removed, 5);
    EXPECT_EQ(StressInventory::Ops::CountItem(inv, 1), 0);
    EXPECT_TRUE(inv.slots.empty());
}

TEST(GameplayStress_InventoryInvalidItemID)
{
    // Lookup and operations on a non-existent item ID
    StressInventory::ItemRegistry reg;
    reg.RegisterItem({1, "Potion", 10, 0.5f});
    StressInventory::InventoryComponent inv;

    // Add non-existent item
    int added = StressInventory::Ops::AddItem(inv, reg, 9999, 5);
    EXPECT_EQ(added, 0);

    // Count non-existent item
    EXPECT_EQ(StressInventory::Ops::CountItem(inv, 9999), 0);

    // Remove non-existent item from empty inventory
    int removed = StressInventory::Ops::RemoveItem(inv, 9999, 1);
    EXPECT_EQ(removed, 0);
}

// ============================================================================
// Quest Stress Tests
// ============================================================================

TEST(GameplayStress_QuestDoubleComplete)
{
    // Complete the same quest twice — second completion should be a no-op
    StressQuest::QuestRegistry reg;
    StressQuest::QuestDef def;
    def.id = 1;
    def.name = "Test Quest";
    def.objectives.push_back({"Kill 1", 1, false});
    reg.RegisterQuest(def);

    StressQuest::QuestJournal journal;
    StressQuest::Ops::StartQuest(journal, reg, 1);
    StressQuest::Ops::UpdateObjective(journal, reg, 1, 0, 1);

    // Quest should be completed
    EXPECT_EQ(static_cast<int>(journal.activeQuests[0].status), static_cast<int>(StressQuest::QuestStatus::Completed));

    // Trying to update again should return false (already completed)
    bool result = StressQuest::Ops::UpdateObjective(journal, reg, 1, 0, 1);
    EXPECT_FALSE(result);
}

TEST(GameplayStress_QuestInvalidObjective)
{
    // Update a non-existent objective index
    StressQuest::QuestRegistry reg;
    StressQuest::QuestDef def;
    def.id = 1;
    def.name = "Test";
    def.objectives.push_back({"Kill 1", 1, false});
    reg.RegisterQuest(def);

    StressQuest::QuestJournal journal;
    StressQuest::Ops::StartQuest(journal, reg, 1);

    // Objective index 5 does not exist (only index 0)
    bool result = StressQuest::Ops::UpdateObjective(journal, reg, 1, 5, 1);
    EXPECT_FALSE(result);

    // Negative index
    result = StressQuest::Ops::UpdateObjective(journal, reg, 1, -1, 1);
    EXPECT_FALSE(result);
}

TEST(GameplayStress_QuestMassiveObjectives)
{
    // Quest with 1000 objectives
    StressQuest::QuestRegistry reg;
    StressQuest::QuestDef def;
    def.id = 1;
    def.name = "Epic Quest";
    for (int i = 0; i < 1000; ++i)
    {
        def.objectives.push_back({"Objective " + std::to_string(i), 1, false});
    }
    reg.RegisterQuest(def);

    StressQuest::QuestJournal journal;
    EXPECT_TRUE(StressQuest::Ops::StartQuest(journal, reg, 1));

    // Complete all 1000 objectives
    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_TRUE(StressQuest::Ops::UpdateObjective(journal, reg, 1, i, 1));
    }

    EXPECT_EQ(static_cast<int>(journal.activeQuests[0].status), static_cast<int>(StressQuest::QuestStatus::Completed));
}

TEST(GameplayStress_QuestCircularPrereqs)
{
    // Quest A requires B, Quest B requires A — neither should be startable
    StressQuest::QuestRegistry reg;

    StressQuest::QuestDef questA;
    questA.id = 1;
    questA.name = "Quest A";
    questA.objectives.push_back({"Test", 1, false});
    questA.requiredQuests = {2}; // Requires quest B
    reg.RegisterQuest(questA);

    StressQuest::QuestDef questB;
    questB.id = 2;
    questB.name = "Quest B";
    questB.objectives.push_back({"Test", 1, false});
    questB.requiredQuests = {1}; // Requires quest A
    reg.RegisterQuest(questB);

    StressQuest::QuestJournal journal;

    // Neither quest can be started because each requires the other
    EXPECT_FALSE(StressQuest::Ops::StartQuest(journal, reg, 1));
    EXPECT_FALSE(StressQuest::Ops::StartQuest(journal, reg, 2));
    EXPECT_TRUE(journal.activeQuests.empty());
}

// ============================================================================
// Weapon Stress Tests
// ============================================================================

TEST(GameplayStress_WeaponFireNoAmmo)
{
    // Firing with 0 ammo should return false and not decrement below 0
    StressWeapon::WeaponInstance weapon;
    weapon.currentAmmo = 0;
    weapon.reserveAmmo = 0;

    bool fired = weapon.Fire();
    EXPECT_FALSE(fired);
    EXPECT_EQ(weapon.currentAmmo, 0);
}

TEST(GameplayStress_WeaponRapidFireReload)
{
    // Alternate between firing and reloading 1000 times
    StressWeapon::WeaponInstance weapon;
    weapon.currentAmmo = 30;
    weapon.reserveAmmo = 120;
    weapon.magazineSize = 30;

    int totalFired = 0;
    for (int i = 0; i < 1000; ++i)
    {
        if (weapon.HasAmmo())
        {
            weapon.Fire();
            ++totalFired;
        }
        else
        {
            weapon.Reload();
        }
    }

    // Should have fired all available ammo (30 + 120 = 150 total)
    EXPECT_EQ(totalFired, 150);
    EXPECT_EQ(weapon.currentAmmo, 0);
    EXPECT_EQ(weapon.reserveAmmo, 0);
}

// ============================================================================
// Ability Stress Tests
// ============================================================================

TEST(GameplayStress_AbilitySpamCast)
{
    // Cast an instant ability 1000 times (no cooldown, no cost)
    StressAbility::AbilitySystem sys;
    sys.RegisterAbility({1, "QuickStrike", 0.0f, 0.0f});

    int successCount = 0;
    for (int i = 0; i < 1000; ++i)
    {
        if (sys.Cast(1))
            ++successCount;
    }

    // All casts should succeed (no cooldown, no resource cost)
    EXPECT_EQ(successCount, 1000);
}

TEST(GameplayStress_AbilityStackOverflow)
{
    // Apply 100 different auras on one entity
    StressAbility::AbilitySystem sys;

    for (uint32_t i = 1; i <= 100; ++i)
    {
        sys.RegisterAura({i, "Aura_" + std::to_string(i), 1, 60.0f});
    }

    StressAbility::EntityID target = 42;
    for (uint32_t i = 1; i <= 100; ++i)
    {
        sys.ApplyAura(i, target);
    }

    EXPECT_EQ(sys.GetAuraCount(target), 100);
    EXPECT_EQ(static_cast<int>(sys.activeAuras.size()), 100);
}

// ============================================================================
// EventBus Stress Tests (uses real Spark::EventBus)
// ============================================================================

namespace
{
    struct StressTestEvent
    {
        int value = 0;
    };
} // namespace

TEST(GameplayStress_EventBusMassiveSubscribers)
{
    // Subscribe 10000 handlers to the same event type
    Spark::EventBus bus;
    std::vector<Spark::SubscriptionHandle> handles;
    handles.reserve(10000);

    int callCount = 0;
    for (int i = 0; i < 10000; ++i)
    {
        handles.push_back(bus.Subscribe<StressTestEvent>([&callCount](const StressTestEvent&) { ++callCount; }));
    }

    EXPECT_EQ(bus.SubscriberCount<StressTestEvent>(), 10000u);

    // Publish one event — all 10000 handlers should fire
    bus.Publish(StressTestEvent{42});
    EXPECT_EQ(callCount, 10000);
}

TEST(GameplayStress_EventBusPublishEmpty)
{
    // Publish an event with no subscribers — should not crash
    Spark::EventBus bus;
    EXPECT_EQ(bus.SubscriberCount<StressTestEvent>(), 0u);

    // This should be a no-op, not a crash
    bus.Publish(StressTestEvent{99});

    // Still no subscribers
    EXPECT_EQ(bus.SubscriberCount<StressTestEvent>(), 0u);
}

TEST(GameplayStress_EventBusRapidSubUnsub)
{
    // Subscribe and unsubscribe 5000 times rapidly
    Spark::EventBus bus;

    for (int i = 0; i < 5000; ++i)
    {
        auto handle = bus.Subscribe<StressTestEvent>([](const StressTestEvent&) {});
        EXPECT_TRUE(handle.IsActive());
        handle.Unsubscribe();
        EXPECT_FALSE(handle.IsActive());
    }

    // After all subscribe/unsubscribe cycles, no subscribers should remain
    EXPECT_EQ(bus.SubscriberCount<StressTestEvent>(), 0u);
}
