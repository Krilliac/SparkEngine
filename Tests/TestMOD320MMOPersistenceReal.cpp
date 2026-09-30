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
#include "Persistence/MMOCharacterRecord.h"
#include "Persistence/MMOPersistenceSystem.h"

#ifdef SPARK_TEST_HAS_IMGUI
#include "Character/MMOCharacterSystem.h"
#endif

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
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

    MMO::GuildMember Member(uint32_t playerId, const std::string& name, MMO::GuildRank rank)
    {
        MMO::GuildMember member;
        member.playerId = playerId;
        member.name = name;
        member.rank = rank;
        return member;
    }

    /// Run raw key-value commands against a store file, bypassing the module's
    /// encoders (records an older or damaged build could have left behind).
    void WriteRawRecords(const fs::path& path, const std::vector<std::string>& commands)
    {
        Spark::Persistence::AsyncDatabasePool raw;
        ASSERT_TRUE(raw.Open(path.string(), 1));
        for (size_t i = 0; i < commands.size(); ++i)
        {
            const auto id = static_cast<Spark::Persistence::PreparedStatementID>(i + 1);
            raw.PrepareStatement(id, commands[i]);
            ASSERT_TRUE(raw.SyncQuery(id).success);
        }
        raw.Close();
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

TEST(MMOPersistence_RemovedProgressionDoesNotReturnAfterRestart)
{
    const fs::path path = FreshPath("test_mod320_removed_progression.db");
    MMO::CharacterSaveData save = MakeSave(0, 25, "Forgetful");
    save.reputationState.standings[1].reputation = 500;
    save.reputationState.standings[2].reputation = -4500;
    save.reputationState.standings[3].reputation = 12000;
    for (auto& [factionId, standing] : save.reputationState.standings)
        standing.factionId = factionId;
    save.achievementState.completedIds = {7, 8};
    save.achievementState.stats["boss's_bane"] = 3;
    save.achievementState.stats["mobs_killed"] = 44;
    MMO::CraftingSkill cooking;
    cooking.discipline = MMO::CraftingDiscipline::Cooking;
    cooking.level = 3;
    cooking.currentXP = 10;
    save.craftingState.skills[static_cast<int>(cooking.discipline)] = cooking;
    save.craftingState.skills[0].level = 7;
    save.craftingState.skills[0].currentXP = 345;
    save.craftingState.knownRecipes = {100, 101};
    MMO::LootLockout kept;
    kept.dungeonDefId = 3;
    kept.difficulty = MMO::DungeonDifficulty::Heroic;
    kept.remaining = 1234.5f;
    MMO::LootLockout cleared = kept;
    cleared.dungeonDefId = 4;
    save.dungeonState.lockouts = {kept, cleared};
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        save.characterId = persistence.CreateCharacter(save.name, save.accountId);
        ASSERT_NE(save.characterId, uint32_t{0});

        // The removal is queued behind the save that still holds everything, so
        // no store scan could see the records it has to delete.
        persistence.SaveCharacterAsync(save);
        save.reputationState.standings.erase(2);
        save.achievementState.completedIds.erase(8);
        save.achievementState.stats.erase("mobs_killed");
        save.craftingState.skills.erase(static_cast<int>(cooking.discipline));
        save.craftingState.knownRecipes = {100};
        save.dungeonState.lockouts = {kept};
        persistence.SaveCharacterAsync(save);
        persistence.Shutdown();
    }

    MMO::CharacterSaveData loaded;
    {
        MMO::MMOPersistenceSystem restarted;
        ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
        ASSERT_TRUE(restarted.LoadCharacter(save.characterId, loaded));
        ASSERT_EQ(loaded.reputationState.standings.size(), size_t{2});
        for (const uint32_t factionId : {1u, 3u})
        {
            const MMO::FactionStanding& expected = save.reputationState.standings[factionId];
            const MMO::FactionStanding& actual = loaded.reputationState.standings[factionId];
            EXPECT_EQ(actual.factionId, factionId);
            EXPECT_EQ(actual.reputation, expected.reputation);
            EXPECT_TRUE(actual.tier == MMO::FactionStanding::GetTierForValue(expected.reputation));
        }
        EXPECT_TRUE(loaded.achievementState.completedIds == std::unordered_set<uint32_t>{7});
        ASSERT_EQ(loaded.achievementState.stats.size(), size_t{1});
        EXPECT_EQ(loaded.achievementState.stats["boss's_bane"], 3);
        ASSERT_EQ(loaded.craftingState.skills.size(), size_t{1});
        EXPECT_EQ(loaded.craftingState.skills[0].level, 7);
        EXPECT_EQ(loaded.craftingState.skills[0].currentXP, 345);
        EXPECT_TRUE(loaded.craftingState.knownRecipes == std::vector<uint32_t>{100});
        ASSERT_EQ(loaded.dungeonState.lockouts.size(), size_t{1});
        EXPECT_EQ(loaded.dungeonState.lockouts[0].dungeonDefId, uint32_t{3});
        EXPECT_NEAR(loaded.dungeonState.lockouts[0].remaining, 1234.5f, 0.01f);

        // A removal in a later run, whose first save must find the stored records.
        loaded.reputationState.standings.erase(3);
        ASSERT_TRUE(restarted.SaveCharacterSync(loaded));
        restarted.Shutdown();
    }

    MMO::MMOPersistenceSystem third;
    ASSERT_TRUE(third.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData latest;
    ASSERT_TRUE(third.LoadCharacter(save.characterId, latest));
    ASSERT_EQ(latest.reputationState.standings.size(), size_t{1});
    EXPECT_EQ(latest.reputationState.standings[1].reputation, 500);
    third.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_ExpiredAndOutOfRangeProgressionIsNotRestored)
{
    const fs::path path = FreshPath("test_mod320_progression_bounds.db");
    constexpr uint32_t kCharacterId = 77;
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        ASSERT_TRUE(persistence.SaveCharacterSync(MakeSave(kCharacterId, 26, "Tamperer")));
        persistence.Shutdown();
    }

    // Records the module never writes: an expired lockout, out-of-range skill
    // levels, disciplines and difficulties, and unparsable values.
    WriteRawRecords(path, {"SET lockout_77_3_1 1234.5", "SET lockout_77_4_0 -5.0", "SET lockout_77_5_0 nan",
                           "SET lockout_77_6_9 100", "SET craft_77_0 7|345", "SET craft_77_1 250|3",
                           "SET craft_77_2 0|10", "SET craft_77_9 5|5", "SET rep_77_4 abc", "SET rep_77_5 -700"});

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData loaded;
    ASSERT_TRUE(restarted.LoadCharacter(kCharacterId, loaded));
    ASSERT_EQ(loaded.dungeonState.lockouts.size(), size_t{1});
    EXPECT_EQ(loaded.dungeonState.lockouts[0].dungeonDefId, uint32_t{3});
    EXPECT_NEAR(loaded.dungeonState.lockouts[0].remaining, 1234.5f, 0.01f);
    ASSERT_EQ(loaded.craftingState.skills.size(), size_t{1});
    EXPECT_EQ(loaded.craftingState.skills[0].level, 7);
    ASSERT_EQ(loaded.reputationState.standings.size(), size_t{1});
    EXPECT_EQ(loaded.reputationState.standings[5].reputation, -700);

    // The next save drops every record the load refused.
    ASSERT_TRUE(restarted.SaveCharacterSync(loaded));
    restarted.Shutdown();
    std::set<std::string> familyKeys;
    for (const std::string& key : StoredKeys(path))
    {
        if (key.starts_with("lockout_77_") || key.starts_with("craft_77_") || key.starts_with("rep_77_"))
            familyKeys.insert(key);
    }
    const std::set<std::string> expectedKeys = {"craft_77_0", "lockout_77_3_1", "rep_77_5"};
    EXPECT_TRUE(familyKeys == expectedKeys);
    fs::remove(path);
}

TEST(MMOPersistence_LoadWorldIsNotANoOp)
{
    const fs::path path = FreshPath("test_mod320_world_load.db");
    MMO::WorldSaveData world;
    world.nextGuildId = 9;
    MMO::Guild guild;
    guild.id = 4;
    guild.name = "Pipe|Dream 100%";
    guild.tag = "P|D";
    guild.motd = "Raid at 8 | bring 50% more pots, it's '%7C' night";
    guild.leaderId = 21;
    guild.bankCurrency = 7500;
    guild.maxMembers = 30;
    guild.members = {Member(21, "Lead|er", MMO::GuildRank::Leader), Member(22, "O'Brien", MMO::GuildRank::Officer)};
    world.guilds = {guild};
    MMO::Guild quiet = guild;
    quiet.id = 6;
    quiet.name = "Quiet";
    quiet.motd.clear();
    quiet.leaderId = 31;
    quiet.members = {Member(31, "Hush", MMO::GuildRank::Leader)};
    world.guilds.push_back(quiet);
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        ASSERT_TRUE(persistence.SaveWorldSync(world));
        persistence.Shutdown();
    }

    MMO::MMOPersistenceSystem restarted;
    ASSERT_TRUE(restarted.Initialize(nullptr, path.string()));
    MMO::WorldSaveData loaded;
    ASSERT_TRUE(restarted.LoadWorld(loaded));
    EXPECT_EQ(loaded.nextGuildId, uint32_t{9});
    ASSERT_EQ(loaded.guilds.size(), size_t{2});
    const MMO::Guild& first = loaded.guilds[0];
    EXPECT_EQ(first.id, uint32_t{4});
    EXPECT_EQ(first.name, guild.name);
    EXPECT_EQ(first.tag, guild.tag);
    EXPECT_EQ(first.motd, guild.motd);
    EXPECT_EQ(first.leaderId, uint32_t{21});
    EXPECT_EQ(first.bankCurrency, 7500);
    EXPECT_EQ(first.maxMembers, 30);
    ASSERT_EQ(first.members.size(), size_t{2});
    EXPECT_EQ(first.members[0].playerId, uint32_t{21});
    EXPECT_EQ(first.members[0].name, std::string("Lead|er"));
    EXPECT_TRUE(first.members[0].rank == MMO::GuildRank::Leader);
    EXPECT_EQ(first.members[1].name, std::string("O'Brien"));
    EXPECT_TRUE(first.members[1].rank == MMO::GuildRank::Officer);
    EXPECT_EQ(loaded.guilds[1].id, uint32_t{6});
    EXPECT_TRUE(loaded.guilds[1].motd.empty());
    EXPECT_EQ(loaded.guilds[1].members.size(), size_t{1});
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_MalformedWorldRecordFailsLoad)
{
    // Each store holds one record no build writes; LoadWorld must refuse the
    // whole world rather than hand back a partial one a save would then persist.
    const std::vector<std::string> damaged = {"SET guild_4 1|Name", "SET guild_5 1||tag|1|0|50|",
                                              "SET guild_6 1|Bad%zz|t|1|0|50|m", "SET gm_4_21 1|9|Ghost",
                                              "SET meta_next_guild_id zero"};
    for (const std::string& record : damaged)
    {
        const fs::path path = FreshPath("test_mod320_world_malformed.db");
        WriteRawRecords(path, {"SET guild_4 1|Keep|K|21|0|50|hello", "SET gm_4_21 1|4|Lead", record});
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        MMO::WorldSaveData loaded;
        loaded.nextGuildId = 42;
        const bool ok = persistence.LoadWorld(loaded);
        if (ok)
            std::cerr << "  accepted malformed record: " << record << "\n";
        EXPECT_FALSE(ok);
        EXPECT_TRUE(loaded.guilds.empty());
        EXPECT_EQ(loaded.nextGuildId, uint32_t{1});
        persistence.Shutdown();
        fs::remove(path);
    }

    // Boss kills are not world state this build restores, so a stray or
    // unparsable bosskill_* record must not refuse the guilds (which would
    // disable persistence for the run).
    const fs::path path = FreshPath("test_mod320_world_stray_bosskill.db");
    WriteRawRecords(path, {"SET guild_4 1|Keep|K|21|0|50|hello", "SET gm_4_21 1|4|Lead", "SET bosskill_7_x 3"});
    MMO::MMOPersistenceSystem persistence;
    ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
    MMO::WorldSaveData loaded;
    EXPECT_TRUE(persistence.LoadWorld(loaded));
    ASSERT_EQ(loaded.guilds.size(), size_t{1});
    EXPECT_EQ(loaded.guilds[0].name, std::string("Keep"));
    persistence.Shutdown();
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

namespace
{
    MMO::WorldSaveData CaptureWorld(const MMO::MMOGuildSystem& guilds)
    {
        MMO::WorldSaveData world;
        world.guilds = guilds.CaptureGuilds();
        world.nextGuildId = guilds.GetNextGuildId();
        return world;
    }

    /// A fresh guild system restored from the world stored at @p path, as the
    /// module does on load.
    void RestoreFromStore(const fs::path& path, MMO::MMOGuildSystem& guilds)
    {
        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        MMO::WorldSaveData world;
        ASSERT_TRUE(persistence.LoadWorld(world));
        std::string error;
        ASSERT_TRUE(guilds.RestoreGuilds(std::move(world.guilds), world.nextGuildId, &error));
        EXPECT_TRUE(error.empty());
        persistence.Shutdown();
    }
} // namespace

TEST(MMOPersistence_GuildsAndMembersSurviveColdRestart)
{
    const fs::path path = FreshPath("test_mod320_guilds.db");
    uint32_t raiders = 0;
    uint32_t crafters = 0;
    {
        MMO::MMOGuildSystem guilds;
        ASSERT_TRUE(guilds.Initialize(nullptr));
        raiders = guilds.CreateGuild("Raiders", "RDR", 21, "Lead");
        crafters = guilds.CreateGuild("Crafters", "CRF", 31, "Smith");
        ASSERT_TRUE(raiders != 0 && crafters != 0);
        ASSERT_TRUE(guilds.InviteMember(raiders, 21, 22, "Second"));
        ASSERT_TRUE(guilds.InviteMember(raiders, 21, 23, "Third"));
        ASSERT_TRUE(guilds.PromoteMember(raiders, 21, 22));
        ASSERT_TRUE(guilds.SetMotd(raiders, 21, "Raid at 8 | 100% attendance"));
        ASSERT_TRUE(guilds.DepositCurrency(raiders, 640));

        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        ASSERT_TRUE(persistence.SaveWorldSync(CaptureWorld(guilds)));
        persistence.Shutdown();
    }

    MMO::MMOGuildSystem restored;
    ASSERT_TRUE(restored.Initialize(nullptr));
    RestoreFromStore(path, restored);
    ASSERT_EQ(restored.GetGuildCount(), size_t{2});
    const MMO::Guild* guild = restored.GetGuild(raiders);
    ASSERT_TRUE(guild != nullptr);
    EXPECT_EQ(guild->name, std::string("Raiders"));
    EXPECT_EQ(guild->tag, std::string("RDR"));
    EXPECT_EQ(guild->motd, std::string("Raid at 8 | 100% attendance"));
    EXPECT_EQ(guild->leaderId, uint32_t{21});
    EXPECT_EQ(guild->bankCurrency, 640);
    ASSERT_EQ(guild->members.size(), size_t{3});
    EXPECT_EQ(guild->members[1].playerId, uint32_t{22});
    EXPECT_EQ(guild->members[1].name, std::string("Second"));
    EXPECT_TRUE(guild->members[1].rank == MMO::GuildRank::Member);
    EXPECT_TRUE(guild->members[2].rank == MMO::GuildRank::Initiate);
    EXPECT_EQ(restored.GetGuild(crafters)->members.size(), size_t{1});

    // Permissions are live again, and the ID counter does not reissue an ID.
    EXPECT_TRUE(restored.HasPerm(raiders, 21, MMO::GuildPermission::Invite));
    EXPECT_FALSE(restored.HasPerm(raiders, 23, MMO::GuildPermission::Invite));
    EXPECT_TRUE(restored.WithdrawCurrency(raiders, 21, 40));
    EXPECT_GT(restored.CreateGuild("Newcomers", "NEW", 41, "Fresh"), std::max(raiders, crafters));
    restored.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_DisbandedGuildDoesNotReturnAfterRestart)
{
    const fs::path path = FreshPath("test_mod320_disband.db");
    uint32_t doomed = 0;
    uint32_t kept = 0;
    {
        MMO::MMOGuildSystem guilds;
        ASSERT_TRUE(guilds.Initialize(nullptr));
        doomed = guilds.CreateGuild("Doomed", "DMD", 21, "Lead");
        kept = guilds.CreateGuild("Kept", "KPT", 31, "Stays");
        ASSERT_TRUE(guilds.InviteMember(kept, 31, 32, "Kicked"));

        MMO::MMOPersistenceSystem persistence;
        ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
        // The disband is queued behind the save that still holds the guild.
        persistence.SaveWorldAsync(CaptureWorld(guilds));
        ASSERT_TRUE(guilds.DisbandGuild(doomed, 21));
        ASSERT_TRUE(guilds.RemoveMember(kept, 31, 32));
        persistence.SaveWorldAsync(CaptureWorld(guilds));
        persistence.Shutdown();
    }

    for (const std::string& key : StoredKeys(path))
    {
        EXPECT_FALSE(key.starts_with("guild_" + std::to_string(doomed)));
        EXPECT_FALSE(key.starts_with("gm_" + std::to_string(doomed) + "_"));
        EXPECT_NE(key, "gm_" + std::to_string(kept) + "_32");
    }
    MMO::MMOGuildSystem restored;
    ASSERT_TRUE(restored.Initialize(nullptr));
    RestoreFromStore(path, restored);
    EXPECT_EQ(restored.GetGuildCount(), size_t{1});
    EXPECT_TRUE(restored.GetGuild(doomed) == nullptr);
    ASSERT_TRUE(restored.GetGuild(kept) != nullptr);
    EXPECT_EQ(restored.GetGuild(kept)->members.size(), size_t{1});
    restored.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_InvalidGuildRestoreLeavesStateUnchanged)
{
    MMO::MMOGuildSystem guilds;
    ASSERT_TRUE(guilds.Initialize(nullptr));
    const uint32_t existing = guilds.CreateGuild("Existing", "EXT", 5, "Owner");
    ASSERT_NE(existing, uint32_t{0});

    MMO::Guild valid;
    valid.id = 8;
    valid.name = "Valid";
    valid.leaderId = 21;
    valid.members = {Member(21, "Lead", MMO::GuildRank::Leader)};

    MMO::Guild leaderless = valid;
    leaderless.id = 9;
    leaderless.name = "Leaderless";
    leaderless.leaderId = 99;
    MMO::Guild demotedLeader = valid;
    demotedLeader.id = 10;
    demotedLeader.name = "Demoted";
    demotedLeader.members[0].rank = MMO::GuildRank::Officer;
    MMO::Guild duplicateId = valid;
    duplicateId.name = "Twin";
    MMO::Guild duplicateName = valid;
    duplicateName.id = 11;
    MMO::Guild overFull = valid;
    overFull.id = 12;
    overFull.name = "Crowded";
    overFull.maxMembers = 1;
    overFull.members.push_back(Member(22, "Extra", MMO::GuildRank::Member));
    MMO::Guild repeatedMember = valid;
    repeatedMember.id = 13;
    repeatedMember.name = "Echo";
    repeatedMember.members.push_back(Member(21, "Lead again", MMO::GuildRank::Member));

    for (const MMO::Guild& bad : {leaderless, demotedLeader, duplicateId, duplicateName, overFull, repeatedMember})
    {
        std::string error;
        EXPECT_FALSE(guilds.RestoreGuilds({valid, bad}, 20, &error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(guilds.GetGuildCount(), size_t{1});
        EXPECT_TRUE(guilds.GetGuild(existing) != nullptr);
        EXPECT_TRUE(guilds.GetGuild(valid.id) == nullptr);
        EXPECT_EQ(guilds.GetNextGuildId(), existing + 1);
    }

    // The valid set alone is accepted, and the counter never falls behind a restored ID.
    std::string error;
    ASSERT_TRUE(guilds.RestoreGuilds({valid}, 2, &error));
    EXPECT_TRUE(guilds.GetGuild(existing) == nullptr);
    EXPECT_EQ(guilds.GetNextGuildId(), uint32_t{9});
    guilds.Shutdown();
}
#endif

TEST(MMOPersistence_CharacterRowKeepsExactFloatsThroughColdRestart)
{
    // SparkFuzzMMOCharacterRecord's regression-lossy-float.row seed: the row used to be
    // written with iostream's 6 significant digits, so every save moved the character.
    const fs::path path = FreshPath("test_mod320_exact_floats.db");
    MMO::CharacterSaveData save = MakeSave(0, 31, "Surveyor");
    save.posX = 12345.678f;
    save.posZ = -0.1f;
    save.health = 99.99999f;
    save.playTime = 86400.125f;
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
    EXPECT_EQ(loaded.posX, save.posX);
    EXPECT_EQ(loaded.posZ, save.posZ);
    EXPECT_EQ(loaded.health, save.health);
    EXPECT_EQ(loaded.playTime, save.playTime);
    restarted.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_UnstorableCharacterIsNotSaved)
{
    // A NaN stat or a name holding the '|' separator would write a row LoadCharacter
    // refuses (or misreads); the save fails and the stored row is kept.
    const fs::path path = FreshPath("test_mod320_unstorable.db");
    MMO::MMOPersistenceSystem persistence;
    ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData save = MakeSave(0, 32, "Keeper");
    save.characterId = persistence.CreateCharacter(save.name, save.accountId);
    ASSERT_NE(save.characterId, uint32_t{0});
    ASSERT_TRUE(persistence.SaveCharacterSync(save));

    MMO::CharacterSaveData nan = save;
    nan.level = 9;
    nan.health = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(persistence.SaveCharacterSync(nan));
    MMO::CharacterSaveData injected = save;
    injected.level = 9;
    injected.name = "Keeper|99";
    EXPECT_FALSE(persistence.SaveCharacterSync(injected));

    MMO::CharacterSaveData loaded;
    ASSERT_TRUE(persistence.LoadCharacter(save.characterId, loaded));
    EXPECT_EQ(loaded.level, 4);
    EXPECT_EQ(loaded.name, std::string("Keeper"));
    persistence.Shutdown();
    fs::remove(path);
}

TEST(MMOPersistence_CharacterRowDecoderRejectsDamagedRows)
{
    // SparkFuzzMMOCharacterRecord's regression seeds. Each row used to load: std::stoul
    // wraps "-1", std::stoi stops at "12abc", std::stof reads "nan", and rows with extra
    // fields were silently truncated. A rejected row leaves the output untouched.
    const std::string good = "Alice|7|3|120|2|1.5|0|2.25|90|100|100|50|50|12.5|40";
    MMO::CharacterRecordFields decoded;
    ASSERT_TRUE(MMO::DecodeCharacterRecord(good, decoded));
    EXPECT_EQ(decoded.accountId, uint32_t{7});
    EXPECT_EQ(decoded.posX, 1.5f);
    ASSERT_TRUE(MMO::DecodeCharacterRecord("'Bob'|8|1|0|1|0.0|1.0|0.0|0.0|100.0|100.0|50.0|50.0|0.0|0", decoded));
    EXPECT_EQ(decoded.name, std::string("Bob"));
    ASSERT_TRUE(MMO::DecodeCharacterRecord("Legacy|3|120|2|1.5|0|2.25|90|100|100|50|50|12.5|40", decoded));
    EXPECT_EQ(decoded.accountId, uint32_t{0});

    const std::string damaged[] = {
        "Alice|-1|3|120|2|1.5|0|2.25|90|100|100|50|50|12.5|40",
        "Alice|7|12abc|120|2|1.5|0|2.25|90|100|100|50|50|12.5|40",
        "Alice|7|3|120|2|nan|0|2.25|90|100|100|50|50|12.5|40",
        "Alice|7|3|120|2|1.5|0|2.25|90|inf|100|50|50|12.5|40",
        "Alice|7|3|120|2|1.5|0|2.25|90|100|100|50|50|12.5|40|extra",
        "|7|3|120|2|1.5|0|2.25|90|100|100|50|50|12.5|40",
    };
    for (const std::string& row : damaged)
    {
        MMO::CharacterRecordFields sentinel;
        sentinel.name = "Sentinel";
        sentinel.level = 77;
        EXPECT_FALSE(MMO::DecodeCharacterRecord(row, sentinel));
        EXPECT_EQ(sentinel.name, std::string("Sentinel"));
        EXPECT_EQ(sentinel.level, 77);
    }
}
