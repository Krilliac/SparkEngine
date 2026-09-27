/**
 * @file TestMOD320MMOPersistenceReal.cpp
 * @brief MOD-320: MMO character identity and per-character state survive a cold
 *        restart of the real key-value store.
 *
 * Every test drives the production MMOPersistenceSystem over AsyncDatabase
 * against a real file, shuts it down, and reopens the same file with a fresh
 * instance, exactly as a restarted server does.
 */
#include "TestFramework.h"
#include "Persistence/MMOPersistenceSystem.h"

#ifdef SPARK_TEST_HAS_IMGUI
#include "Character/MMOCharacterSystem.h"
#endif

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    fs::path FreshPath(const char* name)
    {
        const fs::path path = fs::path("Saves") / name;
        fs::create_directories(path.parent_path());
        fs::remove(path);
        fs::remove(fs::path(path.string() + ".tmp"));
        return path;
    }

    /// Keys of the committed store file (one "key<TAB>value" line per entry).
    std::vector<std::string> StoredKeys(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<std::string> keys;
        std::string line;
        while (std::getline(in, line))
        {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos)
                keys.push_back(line.substr(0, tab));
        }
        return keys;
    }

    MMO::CharacterSaveData MakeSave(uint32_t characterId, uint32_t accountId, const std::string& name)
    {
        MMO::CharacterSaveData save;
        save.characterId = characterId;
        save.accountId = accountId;
        save.name = name;
        save.level = 4;
        return save;
    }

    MMO::ItemStack Stack(uint32_t itemDefId, int count)
    {
        MMO::ItemStack stack;
        stack.itemDefId = itemDefId;
        stack.count = count;
        return stack;
    }
} // namespace

TEST(MMOPersistence_CharacterIdsAreUniqueWithinOneSecond)
{
    const fs::path path = FreshPath("test_mod320_unique_ids.db");
    MMO::MMOPersistenceSystem persistence;
    ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));

    // Far more creates than fit in one wall-clock second's worth of distinct timestamps.
    std::set<uint32_t> ids;
    for (int i = 0; i < 100; ++i)
    {
        const uint32_t id = persistence.CreateCharacter("Burst" + std::to_string(i), 7);
        ASSERT_NE(id, uint32_t{0});
        EXPECT_TRUE(ids.insert(id).second);
    }
    EXPECT_EQ(ids.size(), size_t{100});

    // No create overwrote an earlier one.
    MMO::CharacterSaveData first;
    ASSERT_TRUE(persistence.LoadCharacter(*ids.begin(), first));
    EXPECT_EQ(first.name, std::string("Burst0"));
    persistence.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_CharacterIdCounterSurvivesColdRestart)
{
    const fs::path path = FreshPath("test_mod320_id_restart.db");
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t deleted = 0;
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        first = persistence.CreateCharacter("First", 7);
        second = persistence.CreateCharacter("Second", 8);
        deleted = persistence.CreateCharacter("Doomed", 8);
        ASSERT_TRUE(first != 0 && second != 0 && deleted != 0);
        // The highest ID's record is gone, so only the durable counter still knows it was used.
        EXPECT_TRUE(persistence.DeleteCharacter(deleted));
        persistence.Shutdown();
    }

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    const uint32_t third = restarted.CreateCharacter("Third", 9);
    EXPECT_GT(third, std::max({first, second, deleted}));

    MMO::CharacterSaveData loaded;
    ASSERT_TRUE(restarted.LoadCharacter(first, loaded));
    EXPECT_EQ(loaded.name, std::string("First"));
    EXPECT_EQ(loaded.accountId, uint32_t{7});
    ASSERT_TRUE(restarted.LoadCharacter(second, loaded));
    EXPECT_EQ(loaded.name, std::string("Second"));
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_LegacyTimestampIdsSeedCounter)
{
    // Earlier builds derived IDs from the wall clock and kept no counter.
    const fs::path path = FreshPath("test_mod320_legacy_ids.db");
    constexpr uint32_t kLegacyId = 1790000000u;
    {
        MMO::MMOPersistenceSystem legacy;
        ASSERT_TRUE(legacy.Initialize(nullptr, path.string()));
        ASSERT_TRUE(legacy.SaveCharacterSync(MakeSave(kLegacyId, 3, "Veteran")));
        legacy.Shutdown();
    }

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    const uint32_t fresh = restarted.CreateCharacter("Newcomer", 4);
    EXPECT_GT(fresh, kLegacyId);
    MMO::CharacterSaveData veteran;
    ASSERT_TRUE(restarted.LoadCharacter(kLegacyId, veteran));
    EXPECT_EQ(veteran.name, std::string("Veteran"));
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_ListCharactersFiltersByAccountAndReturnsNames)
{
    const fs::path path = FreshPath("test_mod320_list.db");
    uint32_t alice = 0;
    uint32_t alicesSecond = 0;
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        alice = persistence.CreateCharacter("Alice", 11);
        ASSERT_NE(persistence.CreateCharacter("Bob", 12), uint32_t{0});
        alicesSecond = persistence.CreateCharacter("Alicia", 11);
        persistence.Shutdown();
    }

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    auto owned = restarted.ListCharacters(11);
    std::sort(owned.begin(), owned.end());
    ASSERT_EQ(owned.size(), size_t{2});
    EXPECT_EQ(owned[0].first, alice);
    EXPECT_EQ(owned[0].second, std::string("Alice"));
    EXPECT_EQ(owned[1].first, alicesSecond);
    EXPECT_EQ(owned[1].second, std::string("Alicia"));
    EXPECT_EQ(restarted.ListCharacters(12).size(), size_t{1});
    EXPECT_TRUE(restarted.ListCharacters(13).empty());
    EXPECT_TRUE(restarted.ListCharacters(0).empty());
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_InventoryRoundTripsExactSlotsThroughColdRestart)
{
    const fs::path path = FreshPath("test_mod320_inventory.db");
    MMO::CharacterSaveData save = MakeSave(0, 21, "Packrat");
    save.inventory.maxSlots = 40;
    save.inventory.maxWeight = 172.5f;
    save.inventory.currency = 1234;
    save.inventory.slots.assign(30, MMO::ItemStack{});
    save.inventory.slots[0] = Stack(101, 3);
    save.inventory.slots[4] = Stack(205, 1);
    save.inventory.slots[29] = Stack(9, 20);
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        save.characterId = persistence.CreateCharacter(save.name, save.accountId);
        ASSERT_NE(save.characterId, uint32_t{0});
        ASSERT_TRUE(persistence.SaveCharacterSync(save));
        persistence.Shutdown();
    }

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData loaded;
    ASSERT_TRUE(restarted.LoadCharacter(save.characterId, loaded));
    EXPECT_EQ(loaded.inventory.currency, 1234);
    EXPECT_EQ(loaded.inventory.maxSlots, 40);
    EXPECT_NEAR(loaded.inventory.maxWeight, 172.5f, 0.001f);
    ASSERT_EQ(loaded.inventory.slots.size(), size_t{30});
    for (size_t i = 0; i < save.inventory.slots.size(); ++i)
    {
        EXPECT_EQ(loaded.inventory.slots[i].itemDefId, save.inventory.slots[i].itemDefId);
        EXPECT_EQ(loaded.inventory.slots[i].count, save.inventory.slots[i].count);
    }
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_RemovedItemDoesNotReappearAfterRestart)
{
    const fs::path path = FreshPath("test_mod320_removed_item.db");
    MMO::CharacterSaveData save = MakeSave(0, 22, "Seller");
    save.inventory.slots.assign(10, MMO::ItemStack{});
    save.inventory.slots[2] = Stack(55, 1);
    save.inventory.slots[7] = Stack(56, 4);
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        save.characterId = persistence.CreateCharacter(save.name, save.accountId);
        ASSERT_TRUE(persistence.SaveCharacterSync(save));

        // The item in slot 7 is sold and the bag shrinks; the next save must win.
        save.inventory.slots[7] = MMO::ItemStack{};
        save.inventory.slots.resize(8);
        ASSERT_TRUE(persistence.SaveCharacterSync(save));
        persistence.Shutdown();
    }

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData loaded;
    ASSERT_TRUE(restarted.LoadCharacter(save.characterId, loaded));
    ASSERT_EQ(loaded.inventory.slots.size(), size_t{8});
    EXPECT_EQ(loaded.inventory.slots[2].itemDefId, uint32_t{55});
    EXPECT_TRUE(loaded.inventory.slots[7].IsEmpty());
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_AsyncSaveIsDurableAfterShutdown)
{
    const fs::path path = FreshPath("test_mod320_async.db");
    MMO::CharacterSaveData save = MakeSave(0, 23, "Autosaver");
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        save.characterId = persistence.CreateCharacter(save.name, save.accountId);
        ASSERT_NE(save.characterId, uint32_t{0});

        // A burst of auto-saves, each newer than the last, then an immediate
        // shutdown: every queued save drains and the newest lands last.
        for (int generation = 1; generation <= 20; ++generation)
        {
            save.level = generation;
            save.inventory.slots.assign(static_cast<size_t>(generation), MMO::ItemStack{});
            save.inventory.slots.back() = Stack(300 + static_cast<uint32_t>(generation), generation);
            persistence.SaveCharacterAsync(save);
        }
        persistence.Shutdown();
    }

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData loaded;
    ASSERT_TRUE(restarted.LoadCharacter(save.characterId, loaded));
    EXPECT_EQ(loaded.level, 20);
    ASSERT_EQ(loaded.inventory.slots.size(), size_t{20});
    EXPECT_EQ(loaded.inventory.slots[19].itemDefId, uint32_t{320});
    EXPECT_EQ(loaded.inventory.slots[19].count, 20);
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_DeleteCharacterRemovesInventoryAndReputationKeys)
{
    const fs::path path = FreshPath("test_mod320_delete.db");
    MMO::MMOPersistenceSystem persistence;
    ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));

    // IDs 1 and 10: every per-character key of 1 is also a string prefix of 10's.
    std::vector<uint32_t> ids;
    for (int i = 0; i < 10; ++i)
        ids.push_back(persistence.CreateCharacter("Twin" + std::string(1, static_cast<char>('A' + i)), 31));
    ASSERT_EQ(ids.front(), uint32_t{1});
    ASSERT_EQ(ids.back(), uint32_t{10});

    for (const uint32_t id : {ids.front(), ids.back()})
    {
        MMO::CharacterSaveData save = MakeSave(id, 31, "Twin");
        save.inventory.slots = {Stack(5, 2)};
        save.reputationState.standings[4].factionId = 4;
        save.reputationState.standings[4].reputation = 900;
        save.achievementState.completedIds.insert(12);
        save.achievementState.stats["mobs_killed"] = 44;
        save.craftingState.skills[1] = MMO::CraftingSkill{};
        save.craftingState.knownRecipes.push_back(100);
        MMO::LootLockout lockout;
        lockout.dungeonDefId = 6;
        lockout.remaining = 50.0f;
        save.dungeonState.lockouts.push_back(lockout);
        ASSERT_TRUE(persistence.SaveCharacterSync(save));
    }

    EXPECT_TRUE(persistence.DeleteCharacter(ids.front()));
    persistence.Shutdown();

    const std::vector<std::string> keys = StoredKeys(path);
    ASSERT_FALSE(keys.empty());
    for (const std::string& key : keys)
    {
        for (const char* family : {"inv_1_", "rep_1_", "ach_1_", "achstat_1_", "craft_1_", "recipe_1_", "lockout_1_"})
            EXPECT_FALSE(key.starts_with(family));
        EXPECT_NE(key, std::string("character_1"));
        EXPECT_NE(key, std::string("inventory_1"));
        EXPECT_NE(key, std::string("currency_1"));
    }

    // The neighbour whose keys share the deleted character's prefix is untouched.
    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData gone;
    EXPECT_FALSE(restarted.LoadCharacter(ids.front(), gone));
    MMO::CharacterSaveData kept;
    ASSERT_TRUE(restarted.LoadCharacter(ids.back(), kept));
    ASSERT_EQ(kept.inventory.slots.size(), size_t{1});
    EXPECT_EQ(kept.inventory.slots[0].itemDefId, uint32_t{5});
    EXPECT_EQ(kept.reputationState.standings[4].reputation, 900);
    EXPECT_EQ(kept.achievementState.stats["mobs_killed"], 44);
    EXPECT_EQ(kept.dungeonState.lockouts.size(), size_t{1});
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_CraftingAndLockoutValuesSurviveColdRestart)
{
    const fs::path path = FreshPath("test_mod320_crafting_lockouts.db");
    MMO::CharacterSaveData save = MakeSave(0, 24, "Smith");
    MMO::CraftingSkill smithing;
    smithing.discipline = MMO::CraftingDiscipline::Weaponsmithing;
    smithing.level = 17;
    smithing.currentXP = 345;
    save.craftingState.skills[static_cast<int>(smithing.discipline)] = smithing;
    MMO::LootLockout lockout;
    lockout.dungeonDefId = 3;
    lockout.difficulty = MMO::DungeonDifficulty::Heroic;
    lockout.remaining = 1234.5f;
    save.dungeonState.lockouts.push_back(lockout);
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        save.characterId = persistence.CreateCharacter(save.name, save.accountId);
        ASSERT_TRUE(persistence.SaveCharacterSync(save));
        persistence.Shutdown();
    }

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData loaded;
    ASSERT_TRUE(restarted.LoadCharacter(save.characterId, loaded));
    const auto skill = loaded.craftingState.skills.find(static_cast<int>(smithing.discipline));
    ASSERT_TRUE(skill != loaded.craftingState.skills.end());
    EXPECT_EQ(skill->second.level, 17);
    EXPECT_EQ(skill->second.currentXP, 345);
    ASSERT_EQ(loaded.dungeonState.lockouts.size(), size_t{1});
    EXPECT_EQ(loaded.dungeonState.lockouts[0].dungeonDefId, uint32_t{3});
    EXPECT_TRUE(loaded.dungeonState.lockouts[0].difficulty == MMO::DungeonDifficulty::Heroic);
    EXPECT_NEAR(loaded.dungeonState.lockouts[0].remaining, 1234.5f, 0.01f);
    restarted.Shutdown();
    fs::remove(path);
}

#ifdef SPARK_TEST_HAS_IMGUI
TEST(MMOPersistence_CharacterSystemAllocatesDurableIdsAndDeletesRecords)
{
    const fs::path path = FreshPath("test_mod320_character_system.db");
    MMO::CharacterCreateRequest request;
    request.race = MMO::RaceId::Human;
    request.classId = MMO::ClassId::Warrior;

    uint32_t before = 0;
    uint32_t removed = 0;
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        MMO::MMOCharacterSystem characters;
        ASSERT_TRUE(characters.Initialize(nullptr));
        characters.SetPersistence(&persistence);

        request.name = "Keeper";
        const auto kept = characters.CreateCharacter(41, request);
        ASSERT_TRUE(kept.success);
        before = kept.characterId;
        ASSERT_TRUE(persistence.SaveCharacterSync(MakeSave(before, 41, "Keeper")));

        request.name = "Leaver";
        const auto left = characters.CreateCharacter(41, request);
        ASSERT_TRUE(left.success);
        removed = left.characterId;
        ASSERT_TRUE(persistence.SaveCharacterSync(MakeSave(removed, 41, "Leaver")));
        EXPECT_TRUE(characters.DeleteCharacter(41, removed));
        characters.Shutdown();
        persistence.Shutdown();
    }

    // A restarted server's in-memory character registry starts empty; the
    // persisted counter keeps it from reissuing either earlier ID.
    MMO::MMOPersistenceSystem persistence;
    ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
    MMO::MMOCharacterSystem characters;
    ASSERT_TRUE(characters.Initialize(nullptr));
    characters.SetPersistence(&persistence);
    request.name = "Stranger";
    const auto fresh = characters.CreateCharacter(42, request);
    ASSERT_TRUE(fresh.success);
    EXPECT_GT(fresh.characterId, std::max(before, removed));

    MMO::CharacterSaveData loaded;
    ASSERT_TRUE(persistence.LoadCharacter(before, loaded));
    EXPECT_EQ(loaded.name, std::string("Keeper"));
    EXPECT_FALSE(persistence.LoadCharacter(removed, loaded));

    // Without a usable store, creation fails instead of inventing a reusable ID.
    persistence.Shutdown();
    request.name = "Orphan";
    const auto refused = characters.CreateCharacter(42, request);
    EXPECT_FALSE(refused.success);
    EXPECT_EQ(refused.errorMessage, std::string("Character id allocation failed"));
    characters.Shutdown();
    fs::remove(path);
}
#endif
