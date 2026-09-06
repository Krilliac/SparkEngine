/**
 * @file TestSaveSystemRoundTripReal.cpp
 * @brief Real-class round-trip tests for SaveSystem, InventorySystem, QuestSystem and
 *        the AsyncDatabase key-value store.
 *
 * Every test drives the shipping production classes; nothing here re-implements the
 * save format or the gameplay systems.
 */

#include "TestFramework.h"
#include "Core/Reflection.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/FPSComponents.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/Gameplay/InventorySystem.h"
#include "Engine/Gameplay/QuestSystem.h"
#include "Engine/Persistence/AsyncDatabase.h"
#include "Engine/SaveSystem/SaveSystem.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Spark;

namespace
{
    std::string MakeTempDir(const char* name)
    {
        auto dir = std::filesystem::temp_directory_path() / (std::string("spark_save_roundtrip_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir.string();
    }

    EntityID FindNamed(World& world, const std::string& name)
    {
        auto&& entities = world.GetRegistry().storage<entt::entity>();
        for (auto&& [entity] : entities.each())
        {
            const auto* nameComponent = world.GetComponent<NameComponent>(entity);
            if (nameComponent && nameComponent->name == name)
                return entity;
        }
        return entt::null;
    }

    SerializedComponent MakeTransformRecord(const std::string& parentIndex)
    {
        SerializedComponent component;
        component.typeName = "Transform";
        component.properties["px"] = "1.0";
        component.properties["py"] = "2.0";
        component.properties["pz"] = "3.0";
        component.properties["rx"] = "0.0";
        component.properties["ry"] = "0.0";
        component.properties["rz"] = "0.0";
        component.properties["sx"] = "1.0";
        component.properties["sy"] = "1.0";
        component.properties["sz"] = "1.0";
        if (!parentIndex.empty())
            component.properties["parent"] = parentIndex;
        return component;
    }
} // namespace

// ============================================================================
// SaveSystem — hierarchy, coverage and slot handling
// ============================================================================

TEST(SaveSystemRoundTripReal_HierarchyEdgesSurviveASaveAndLoad)
{
    const std::string dir = MakeTempDir("hierarchy");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID sourceParent = source.CreateEntity("hierarchy-parent");
    const EntityID sourceChild = source.CreateEntity("hierarchy-child");
    source.AddComponent<Transform>(sourceParent).position.x = 5.0f;
    source.AddComponent<Transform>(sourceChild).position.y = 2.0f;
    ASSERT_TRUE(source.SetParent(sourceChild, sourceParent));

    SaveMetadata metadata;
    metadata.saveName = "Hierarchy";
    ASSERT_TRUE(saveSystem.Save("hierarchy", source, metadata));

    World loaded;
    ASSERT_TRUE(saveSystem.Load("hierarchy", loaded));
    EXPECT_EQ(loaded.GetEntityCount(), 2u);

    const EntityID loadedParent = FindNamed(loaded, "hierarchy-parent");
    const EntityID loadedChild = FindNamed(loaded, "hierarchy-child");
    ASSERT_TRUE(loadedParent != entt::null);
    ASSERT_TRUE(loadedChild != entt::null);

    const Transform* childTransform = loaded.GetComponent<Transform>(loadedChild);
    const Transform* parentTransform = loaded.GetComponent<Transform>(loadedParent);
    ASSERT_TRUE(childTransform != nullptr);
    ASSERT_TRUE(parentTransform != nullptr);
    EXPECT_TRUE(childTransform->parent == loadedParent);
    EXPECT_EQ(parentTransform->children.size(), 1u);
    EXPECT_TRUE(parentTransform->children.front() == loadedChild);
    EXPECT_TRUE(parentTransform->parent == entt::null);
    EXPECT_NEAR(childTransform->position.y, 2.0f, 0.0001f);

    std::filesystem::remove_all(dir);
}

TEST(SaveSystemRoundTripReal_ComponentsOutsideTheLegacyAllowlistAreSaved)
{
    const std::string dir = MakeTempDir("coverage");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    const EntityID owner = source.CreateEntity("inventory-owner");
    source.AddComponent<Transform>(owner);
    auto& tag = source.AddComponent<InventoryTag>(owner);
    tag.maxSlots = 42;
    tag.currency = 1234;
    tag.maxWeight = 250.5f;

    SaveMetadata metadata;
    metadata.saveName = "Coverage";
    ASSERT_TRUE(saveSystem.Save("coverage", source, metadata));

    World loaded;
    ASSERT_TRUE(saveSystem.Load("coverage", loaded));
    const EntityID loadedOwner = FindNamed(loaded, "inventory-owner");
    ASSERT_TRUE(loadedOwner != entt::null);

    const InventoryTag* loadedTag = loaded.GetComponent<InventoryTag>(loadedOwner);
    ASSERT_TRUE(loadedTag != nullptr);
    EXPECT_EQ(loadedTag->maxSlots, 42);
    EXPECT_EQ(loadedTag->currency, 1234);
    EXPECT_NEAR(loadedTag->maxWeight, 250.5f, 0.0001f);

    std::filesystem::remove_all(dir);
}

TEST(SaveSystemRoundTripReal_NamedEntityWithoutComponentsIsRetained)
{
    const std::string dir = MakeTempDir("named_only");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    source.CreateEntity("marker-only");

    SaveMetadata metadata;
    metadata.saveName = "Named only";
    ASSERT_TRUE(saveSystem.Save("named-only", source, metadata));

    World loaded;
    ASSERT_TRUE(saveSystem.Load("named-only", loaded));
    EXPECT_EQ(loaded.GetEntityCount(), 1u);
    EXPECT_TRUE(FindNamed(loaded, "marker-only") != entt::null);

    std::filesystem::remove_all(dir);
}

TEST(SaveSystemRoundTripReal_SaveCreatesTheConfiguredDirectory)
{
    const std::string dir = MakeTempDir("create_dir");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    const auto nested = std::filesystem::path(dir) / "profile" / "slots";
    EXPECT_FALSE(std::filesystem::exists(nested));
    saveSystem.SetSaveDirectory(nested.string());

    World source;
    source.AddComponent<Transform>(source.CreateEntity("created-dir"));
    SaveMetadata metadata;
    metadata.saveName = "Created directory";

    EXPECT_TRUE(saveSystem.Save("created", source, metadata));
    EXPECT_TRUE(std::filesystem::exists(nested / "created.spark_save"));
    EXPECT_TRUE(saveSystem.SaveExists("created"));

    ASSERT_TRUE(saveSystem.Initialize(dir));
    std::filesystem::remove_all(dir);
}

TEST(SaveSystemRoundTripReal_EnumeratedSlotsCarryTheirSlotName)
{
    const std::string dir = MakeTempDir("slot_names");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    source.AddComponent<Transform>(source.CreateEntity("slot-owner"));
    SaveMetadata metadata;
    metadata.saveName = "Named slot";
    ASSERT_TRUE(saveSystem.Save("alpha-slot", source, metadata));

    const auto slots = saveSystem.GetSaveSlots();
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(slots.front().slotName, std::string("alpha-slot"));
    EXPECT_EQ(slots.front().saveName, std::string("Named slot"));

    // The listed identifier must be usable directly against the slot API.
    World loaded;
    EXPECT_TRUE(saveSystem.Load(slots.front().slotName, loaded));

    SaveMetadata direct;
    EXPECT_TRUE(saveSystem.GetSaveMetadata("alpha-slot", direct));
    EXPECT_EQ(direct.slotName, std::string("alpha-slot"));
    EXPECT_STR_CONTAINS(saveSystem.Console_ListSaves(), "alpha-slot");

    std::filesystem::remove_all(dir);
}

TEST(SaveSystemRoundTripReal_CorruptSlotRecoversFromTheRetainedCopy)
{
    const std::string dir = MakeTempDir("last_good");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World first;
    first.AddComponent<Transform>(first.CreateEntity("first-revision"));
    SaveMetadata firstMetadata;
    firstMetadata.saveName = "First revision";
    ASSERT_TRUE(saveSystem.Save("rollback", first, firstMetadata));

    World second;
    second.AddComponent<Transform>(second.CreateEntity("second-revision-a"));
    second.AddComponent<Transform>(second.CreateEntity("second-revision-b"));
    SaveMetadata secondMetadata;
    secondMetadata.saveName = "Second revision";
    ASSERT_TRUE(saveSystem.Save("rollback", second, secondMetadata));

    const auto slotPath = std::filesystem::path(dir) / "rollback.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "rollback.spark_save.bak";
    ASSERT_TRUE(std::filesystem::exists(backupPath));

    // A torn write leaves an unparsable slot file; the retained copy must carry the
    // previous revision so the player does not lose the slot.
    std::filesystem::resize_file(slotPath, 8);

    World recovered;
    ASSERT_TRUE(saveSystem.Load("rollback", recovered));
    EXPECT_EQ(recovered.GetEntityCount(), 1u);
    EXPECT_TRUE(FindNamed(recovered, "first-revision") != entt::null);

    // Deleting the slot must take the retained copy with it.
    EXPECT_TRUE(saveSystem.DeleteSave("rollback"));
    EXPECT_FALSE(std::filesystem::exists(backupPath));
    World afterDelete;
    EXPECT_FALSE(saveSystem.Load("rollback", afterDelete));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystemRoundTripReal_SlotSurvivingOnlyAsItsRetainedCopyStaysVisibleAndLoadable)
{
    const std::string dir = MakeTempDir("survivor");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    source.AddComponent<Transform>(source.CreateEntity("survivor-entity"));
    SaveMetadata metadata;
    metadata.saveName = "Survivor";
    ASSERT_TRUE(saveSystem.Save("survivor", source, metadata));
    ASSERT_TRUE(saveSystem.Save("survivor", source, metadata));

    const auto slotPath = std::filesystem::path(dir) / "survivor.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "survivor.spark_save.bak";
    ASSERT_TRUE(std::filesystem::exists(backupPath));

    // Reproduce the state a crash between the retention copy and the atomic replace
    // would leave behind: only the retained copy is on disk. Load() recovers such a
    // slot, so every slot-existence surface has to agree that it is still there -
    // otherwise the save UI and quickload declare a recoverable slot missing.
    std::filesystem::remove(slotPath);

    EXPECT_TRUE(saveSystem.SaveExists("survivor"));

    const std::vector<SaveMetadata> slots = saveSystem.GetSaveSlots();
    bool listed = false;
    for (const SaveMetadata& slot : slots)
    {
        if (slot.slotName == "survivor")
            listed = true;
    }
    EXPECT_TRUE(listed);

    World recovered;
    ASSERT_TRUE(saveSystem.Load("survivor", recovered));
    EXPECT_TRUE(FindNamed(recovered, "survivor-entity") != entt::null);

    std::filesystem::remove_all(dir);
}

TEST(SaveSystemRoundTripReal_FailedDeleteKeepsTheRetainedRecoveryCopy)
{
    const std::string dir = MakeTempDir("delete_order");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    World source;
    source.AddComponent<Transform>(source.CreateEntity("delete-order-entity"));
    SaveMetadata metadata;
    metadata.saveName = "Delete order";
    ASSERT_TRUE(saveSystem.Save("delete-order", source, metadata));
    ASSERT_TRUE(saveSystem.Save("delete-order", source, metadata));

    const auto slotPath = std::filesystem::path(dir) / "delete-order.spark_save";
    const auto backupPath = std::filesystem::path(dir) / "delete-order.spark_save.bak";
    ASSERT_TRUE(std::filesystem::exists(slotPath));
    ASSERT_TRUE(std::filesystem::exists(backupPath));

#if defined(_WIN32)
    {
        // An open handle blocks deletion on Windows, so DeleteSave fails on the primary.
        // A failed delete reports "nothing was deleted": the last-good copy must still be
        // there, or the player is left with an undeleted slot and no recovery copy.
        std::ifstream holder(slotPath.string(), std::ios::binary);
        ASSERT_TRUE(holder.is_open());
        EXPECT_FALSE(saveSystem.DeleteSave("delete-order"));
        EXPECT_TRUE(std::filesystem::exists(slotPath));
        EXPECT_TRUE(std::filesystem::exists(backupPath));
    }
#endif // _WIN32

    // With nothing holding the file the delete succeeds and takes both files with it.
    EXPECT_TRUE(saveSystem.DeleteSave("delete-order"));
    EXPECT_FALSE(std::filesystem::exists(slotPath));
    EXPECT_FALSE(std::filesystem::exists(backupPath));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystemRoundTripReal_TransientComponentsAreExcludedFromTheSnapshot)
{
    const std::string dir = MakeTempDir("transient");
    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(dir));

    // Driving the snapshot from the component registry must not widen saves with
    // runtime-only state that the owning system rebuilds anyway.
    EXPECT_TRUE(saveSystem.IsComponentTransient("ProjectileComponent"));
    EXPECT_FALSE(saveSystem.IsComponentTransient("Transform"));

    World source;
    const EntityID flying = source.CreateEntity("in-flight");
    source.AddComponent<Transform>(flying);
    source.AddComponent<ProjectileComponent>(flying);

    SaveMetadata metadata;
    metadata.saveName = "Transient";
    const SaveData data = saveSystem.SerializeWorld(source, metadata);

    bool sawEntity = false;
    bool sawTransform = false;
    bool sawProjectile = false;
    for (const SerializedEntity& entity : data.entities)
    {
        if (entity.name != "in-flight")
            continue;
        sawEntity = true;
        for (const SerializedComponent& component : entity.components)
        {
            if (component.typeName == "Transform")
                sawTransform = true;
            if (component.typeName == "ProjectileComponent")
                sawProjectile = true;
        }
    }

    EXPECT_TRUE(sawEntity);
    EXPECT_TRUE(sawTransform);
    EXPECT_FALSE(sawProjectile);

    std::filesystem::remove_all(dir);
}

TEST(SaveMigration_VersionTwoSnapshotGainsExplicitTransformRoots)
{
    SaveData legacy;
    legacy.metadata.version = 2;
    legacy.metadata.saveName = "Legacy v2 snapshot";

    SerializedEntity entity{};
    entity.name = "legacy-entity";
    entity.components.push_back(MakeTransformRecord(""));
    legacy.entities.push_back(std::move(entity));

    ASSERT_TRUE(SaveSystem::MigrateToCurrentVersion(legacy));
    EXPECT_EQ(legacy.metadata.version, kCurrentSaveVersion);
    ASSERT_EQ(legacy.entities.size(), 1u);
    ASSERT_EQ(legacy.entities[0].components.size(), 1u);
    const auto& properties = legacy.entities[0].components[0].properties;
    ASSERT_EQ(properties.count("parent"), 1u);
    EXPECT_EQ(properties.at("parent"), std::string("-1"));

    // Migrating an already-current snapshot must not change it.
    const std::string once = properties.at("parent");
    EXPECT_TRUE(SaveSystem::MigrateToCurrentVersion(legacy));
    EXPECT_EQ(legacy.entities[0].components[0].properties.at("parent"), once);
}

TEST(SaveMigration_OutOfRangeParentIndexIsRejectedWithoutTouchingTheWorld)
{
    SaveData data;
    data.metadata.version = kCurrentSaveVersion;
    SerializedEntity entity{};
    entity.name = "dangling-parent";
    entity.components.push_back(MakeTransformRecord("7"));
    data.entities.push_back(std::move(entity));

    SaveSystem& saveSystem = SaveSystem::GetInstance();
    World live;
    const EntityID sentinel = live.CreateEntity("live-sentinel");
    live.AddComponent<Transform>(sentinel).position.x = 11.0f;

    EXPECT_FALSE(saveSystem.DeserializeWorld(data, live));
    EXPECT_EQ(live.GetEntityCount(), 1u);
    ASSERT_TRUE(live.GetComponent<Transform>(sentinel) != nullptr);
    EXPECT_NEAR(live.GetComponent<Transform>(sentinel)->position.x, 11.0f, 0.0001f);
}

// ============================================================================
// InventorySystem — snapshot/restore
// ============================================================================

TEST(InventorySystemReal_EntityStateSnapshotRoundTripsAndClears)
{
    auto& inventory = Spark::Gameplay::InventorySystem::GetInstance();
    inventory.Initialize();

    Spark::Gameplay::ItemDefinition potion;
    potion.itemId = 7;
    potion.name = "Potion";
    potion.maxStackSize = 10;
    inventory.RegisterItem(potion);

    constexpr uint32_t kEntity = 4242;
    inventory.SetMaxSlots(kEntity, 6);
    ASSERT_TRUE(inventory.AddItem(kEntity, 7, 3));

    const auto snapshot = inventory.CaptureEntityState(kEntity);
    EXPECT_EQ(snapshot.maxSlots, 6u);
    EXPECT_TRUE(inventory.ValidateEntityState(snapshot));

    inventory.ClearEntityState(kEntity);
    EXPECT_EQ(inventory.GetItemCount(kEntity, 7), 0u);
    EXPECT_TRUE(inventory.CaptureEntityState(kEntity).slots.empty());

    ASSERT_TRUE(inventory.RestoreEntityState(kEntity, snapshot));
    EXPECT_EQ(inventory.GetItemCount(kEntity, 7), 3u);

    // A slot referencing an unregistered item, or overflowing its stack, is refused
    // without disturbing the live inventory.
    Spark::Gameplay::InventorySnapshot corrupt;
    corrupt.maxSlots = 6;
    corrupt.slots.push_back({999, 1});
    EXPECT_FALSE(inventory.ValidateEntityState(corrupt));
    EXPECT_FALSE(inventory.RestoreEntityState(kEntity, corrupt));

    Spark::Gameplay::InventorySnapshot overflowing;
    overflowing.maxSlots = 6;
    overflowing.slots.push_back({7, 11});
    EXPECT_FALSE(inventory.ValidateEntityState(overflowing));
    EXPECT_EQ(inventory.GetItemCount(kEntity, 7), 3u);

    inventory.ClearEntityState(kEntity);
    inventory.Shutdown();
}

// ============================================================================
// QuestSystem — completion rewards and event
// ============================================================================

TEST(QuestSystemReal_CompletionGrantsItemRewardsAndPublishesTheEvent)
{
    auto& quests = Spark::Gameplay::QuestSystem::GetInstance();
    auto& inventory = Spark::Gameplay::InventorySystem::GetInstance();
    quests.SetPolicy(nullptr);
    quests.Initialize();
    inventory.Initialize();

    Spark::Gameplay::ItemDefinition reward;
    reward.itemId = 31;
    reward.name = "Reward";
    reward.maxStackSize = 10;
    inventory.RegisterItem(reward);

    Spark::Gameplay::QuestDefinition definition;
    definition.questId = 900;
    definition.name = "Rat Problem";
    definition.xpReward = 50;
    definition.itemRewards.emplace_back(31u, 2u);
    Spark::Gameplay::QuestObjective objective;
    objective.description = "Kill rats";
    objective.type = Spark::Gameplay::QuestObjective::Type::Kill;
    objective.targetId = 100;
    objective.requiredCount = 2;
    definition.objectives.push_back(objective);
    quests.RegisterQuest(definition);

    uint32_t observedQuestId = 0;
    std::string observedName;
    auto subscription = Spark::EventBus::Global().Subscribe<Spark::QuestCompletedEvent>(
        [&](const Spark::QuestCompletedEvent& event)
        {
            observedQuestId = event.questId;
            observedName = event.questName;
        });

    constexpr uint32_t kPlayer = 77;
    ASSERT_TRUE(quests.StartQuest(kPlayer, 900));
    quests.ReportProgress(kPlayer, Spark::Gameplay::QuestObjective::Type::Kill, 100, 2);
    ASSERT_TRUE(quests.IsQuestComplete(kPlayer, 900));

    quests.CompleteQuest(kPlayer, 900);

    EXPECT_EQ(observedQuestId, 900u);
    EXPECT_EQ(observedName, std::string("Rat Problem"));
    EXPECT_EQ(inventory.GetItemCount(kPlayer, 31), 2u);

    inventory.ClearEntityState(kPlayer);
    quests.ClearEntityState(kPlayer);
    inventory.Shutdown();
    quests.Shutdown();
}

TEST(QuestSystemReal_FullInventoryDefersCompletionInsteadOfDestroyingTheReward)
{
    auto& quests = Spark::Gameplay::QuestSystem::GetInstance();
    auto& inventory = Spark::Gameplay::InventorySystem::GetInstance();
    quests.SetPolicy(nullptr);
    quests.Initialize();
    inventory.Initialize();

    Spark::Gameplay::ItemDefinition reward;
    reward.itemId = 41;
    reward.name = "Deferred Reward";
    reward.maxStackSize = 10;
    inventory.RegisterItem(reward);

    Spark::Gameplay::QuestDefinition definition;
    definition.questId = 901;
    definition.name = "Overflowing Satchel";
    definition.itemRewards.emplace_back(41u, 2u);
    Spark::Gameplay::QuestObjective objective;
    objective.description = "Collect";
    objective.type = Spark::Gameplay::QuestObjective::Type::Collect;
    objective.targetId = 41;
    objective.requiredCount = 1;
    definition.objectives.push_back(objective);
    quests.RegisterQuest(definition);

    constexpr uint32_t kPlayer = 78;

    // One slot, filled to its stack limit: the reward cannot land.
    inventory.SetMaxSlots(kPlayer, 1);
    ASSERT_TRUE(inventory.AddItem(kPlayer, 41, 10));

    ASSERT_TRUE(quests.StartQuest(kPlayer, 901));
    quests.ReportProgress(kPlayer, Spark::Gameplay::QuestObjective::Type::Collect, 41, 1);
    ASSERT_TRUE(quests.IsQuestComplete(kPlayer, 901));

    // A quest completes exactly once, so committing the completion and then dropping the
    // reward with a log line would destroy it permanently. The completion must be
    // refused instead, leaving the quest retryable.
    EXPECT_FALSE(quests.CompleteQuest(kPlayer, 901));
    EXPECT_TRUE(quests.GetQuestState(kPlayer, 901) == Spark::Gameplay::QuestState::Active);
    EXPECT_EQ(inventory.GetItemCount(kPlayer, 41), 10u);

    // Once there is room the same quest can be turned in and the reward really arrives.
    inventory.SetMaxSlots(kPlayer, 4);
    EXPECT_TRUE(quests.CompleteQuest(kPlayer, 901));
    EXPECT_TRUE(quests.GetQuestState(kPlayer, 901) == Spark::Gameplay::QuestState::Completed);
    EXPECT_EQ(inventory.GetItemCount(kPlayer, 41), 12u);

    inventory.ClearEntityState(kPlayer);
    quests.ClearEntityState(kPlayer);
    inventory.Shutdown();
    quests.Shutdown();
}

// ============================================================================
// AsyncDatabase - key-value store durability
// ============================================================================

TEST(AsyncDatabaseReal_KeyValueFlushReplacesTheStoreWithoutResidue)
{
    const std::string dir = MakeTempDir("kv_store");
    const auto dbPath = (std::filesystem::path(dir) / "store.kv").string();

    {
        Spark::Persistence::SQLiteConnection database;
        ASSERT_TRUE(database.Open(dbPath));
        EXPECT_TRUE(database.ExecuteRaw("SET alpha one").success);
        EXPECT_TRUE(database.ExecuteRaw("SET beta two").success);
        EXPECT_TRUE(database.ExecuteRaw("DELETE alpha").success);
        database.Close();
    }

    // The write path goes through a sibling temp file; a completed flush must leave
    // the destination complete and no residue behind.
    EXPECT_TRUE(std::filesystem::exists(dbPath));
    EXPECT_FALSE(std::filesystem::exists(dbPath + ".tmp"));

    {
        Spark::Persistence::SQLiteConnection reopened;
        ASSERT_TRUE(reopened.Open(dbPath));
        const auto beta = reopened.ExecuteRaw("GET beta");
        EXPECT_TRUE(beta.success);
        ASSERT_EQ(beta.rows.size(), 1u);
        const auto alpha = reopened.ExecuteRaw("GET alpha");
        EXPECT_TRUE(alpha.success);
        EXPECT_EQ(alpha.rows.size(), 0u);
        reopened.Close();
    }

    std::filesystem::remove_all(dir);
}
