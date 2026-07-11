/**
 * @file TestTFOutfitStore.cpp
 * @brief TERRAFRONT outfit persistence (W7/W8): JSON round-trip, uniqueness,
 *        roster mutations, corrupt-file quarantine, and the 2 s dirty-debounce
 *        flush of TFOutfitStore.
 *
 * Persistence/TFOutfitStore.cpp is compiled into SparkTests explicitly (the
 * TFDatabase.cpp pattern — see Tests/CMakeLists.txt): it is deliberately
 * minimal-dependency (Utils/JsonUtils.h + LogMacros only) so it links without
 * the rest of the game module. All files live under the OS temp directory.
 *
 * Contract reminder exercised throughout: record pointers returned by the
 * store are valid only until the next mutation — every test captures ids
 * immediately and re-queries instead of holding pointers across mutations.
 */
#include "TestFramework.h"

#include "Persistence/TFOutfitStore.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace Terrafront;

namespace
{
    /// Unique writable path under a dedicated temp subdir (removed by each
    /// test; the subdir keeps quarantine-bak sweeps off the global temp dir).
    std::string TempStorePath(const char* stem)
    {
        const fs::path dir = fs::temp_directory_path() / "spark_tfoutfit_tests";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return (dir / (std::string(stem) + ".json")).string();
    }
} // namespace

TEST(TFOutfitStore_CreateAndQuery)
{
    const std::string path = TempStorePath("tfoutfit_create");
    fs::remove(path);

    TFOutfitStore store;
    EXPECT_FALSE(store.IsOpen());
    EXPECT_TRUE(store.Open(path));
    EXPECT_TRUE(store.IsOpen());

    const TFOutfitRecord* outfit = store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000);
    EXPECT_NE(outfit, nullptr);
    if (!outfit)
        return;
    EXPECT_NE(outfit->id, 0u);
    EXPECT_EQ(store.OutfitCount(), size_t{1});

    // Case-insensitive lookups all resolve to the same record.
    EXPECT_EQ(store.FindByName("iron vultures"), outfit);
    EXPECT_EQ(store.FindByTag("ivlt"), outfit);
    EXPECT_EQ(store.FindByCharacter(1001), outfit);
    EXPECT_EQ(store.FindByCharacter(999), nullptr);

    const TFOutfitMemberRecord* leader = outfit->Leader();
    EXPECT_NE(leader, nullptr);
    if (leader)
    {
        EXPECT_EQ(leader->charId, uint64_t{1001});
        EXPECT_TRUE(leader->rank == TFOutfitRank::Leader);
        EXPECT_TRUE(leader->name == "Raska");
    }

    store.Close();
    EXPECT_FALSE(store.IsOpen());
    fs::remove(path);
}

TEST(TFOutfitStore_UniqueNameAndTagCaseInsensitive)
{
    const std::string path = TempStorePath("tfoutfit_unique");
    fs::remove(path);

    TFOutfitStore store;
    EXPECT_TRUE(store.Open(path));

    EXPECT_NE(store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000), nullptr);
    // Same name (different case), different tag: refused.
    EXPECT_EQ(store.Create("IRON VULTURES", "XXXX", 1002, "Vex", 1001), nullptr);
    // Same tag (different case), different name: refused.
    EXPECT_EQ(store.Create("Steel Eagles", "ivlt", 1002, "Vex", 1001), nullptr);
    // Both unique: accepted.
    EXPECT_NE(store.Create("Steel Eagles", "SEAG", 1002, "Vex", 1001), nullptr);
    EXPECT_EQ(store.OutfitCount(), size_t{2});

    store.Close();
    fs::remove(path);
}

TEST(TFOutfitStore_RosterMutations)
{
    const std::string path = TempStorePath("tfoutfit_roster");
    fs::remove(path);

    TFOutfitStore store;
    EXPECT_TRUE(store.Open(path));

    const TFOutfitRecord* created = store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000);
    EXPECT_NE(created, nullptr);
    if (!created)
        return;
    const uint32_t id = created->id; // capture before further mutations invalidate the pointer

    EXPECT_TRUE(store.AddMember(id, 2002, "Vex", TFOutfitRank::Member, 1001));
    EXPECT_FALSE(store.AddMember(id, 2002, "Vex", TFOutfitRank::Member, 1001));       // duplicate member
    EXPECT_FALSE(store.AddMember(id + 100, 2003, "Zap", TFOutfitRank::Member, 1001)); // unknown outfit
    EXPECT_FALSE(store.AddMember(id, 0, "Nul", TFOutfitRank::Member, 1001));          // charId 0 invalid

    EXPECT_TRUE(store.SetMemberRank(id, 2002, TFOutfitRank::Officer));
    EXPECT_TRUE(store.SetMemberRank(id, 2002, TFOutfitRank::Officer));  // idempotent, still true
    EXPECT_FALSE(store.SetMemberRank(id, 7777, TFOutfitRank::Officer)); // unknown member

    const TFOutfitRecord* rec = store.FindById(id);
    EXPECT_NE(rec, nullptr);
    if (rec)
    {
        const TFOutfitMemberRecord* vex = rec->FindMember(2002);
        EXPECT_NE(vex, nullptr);
        if (vex)
            EXPECT_TRUE(vex->rank == TFOutfitRank::Officer);
    }

    EXPECT_TRUE(store.RemoveMember(id, 2002));
    EXPECT_FALSE(store.RemoveMember(id, 2002)); // already gone
    EXPECT_EQ(store.FindByCharacter(2002), nullptr);

    store.Close();
    fs::remove(path);
}

TEST(TFOutfitStore_DiskRoundTrip)
{
    const std::string path = TempStorePath("tfoutfit_roundtrip");
    fs::remove(path);

    uint32_t firstId = 0;
    {
        TFOutfitStore store;
        EXPECT_TRUE(store.Open(path));
        const TFOutfitRecord* created = store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000);
        EXPECT_NE(created, nullptr);
        if (!created)
            return;
        firstId = created->id;
        EXPECT_TRUE(store.AddMember(firstId, 2002, "Vex", TFOutfitRank::Member, 1001));
        EXPECT_TRUE(store.AddMember(firstId, 2003, "Zap", TFOutfitRank::Officer, 1002));
        EXPECT_TRUE(store.SaveNow());
        store.Close();
    }

    { // fresh instance, same path: everything survives the JSON round-trip
        TFOutfitStore store;
        EXPECT_TRUE(store.Open(path));
        EXPECT_EQ(store.OutfitCount(), size_t{1});

        const TFOutfitRecord* rec = store.FindByName("iron vultures");
        EXPECT_NE(rec, nullptr);
        if (!rec)
            return;
        EXPECT_EQ(rec->id, firstId);
        EXPECT_TRUE(rec->name == "Iron Vultures");
        EXPECT_TRUE(rec->tag == "IVLT");
        EXPECT_EQ(rec->createdAtMs, int64_t{1000});
        EXPECT_EQ(rec->members.size(), size_t{3});

        const TFOutfitMemberRecord* raska = rec->FindMember(1001);
        EXPECT_NE(raska, nullptr);
        if (raska)
        {
            EXPECT_TRUE(raska->name == "Raska");
            EXPECT_TRUE(raska->rank == TFOutfitRank::Leader);
            EXPECT_EQ(raska->joinedAtMs, int64_t{1000});
        }
        const TFOutfitMemberRecord* vex = rec->FindMember(2002);
        EXPECT_NE(vex, nullptr);
        if (vex)
        {
            EXPECT_TRUE(vex->name == "Vex");
            EXPECT_TRUE(vex->rank == TFOutfitRank::Member);
            EXPECT_EQ(vex->joinedAtMs, int64_t{1001});
        }
        const TFOutfitMemberRecord* zap = rec->FindMember(2003);
        EXPECT_NE(zap, nullptr);
        if (zap)
        {
            EXPECT_TRUE(zap->rank == TFOutfitRank::Officer);
            EXPECT_EQ(zap->joinedAtMs, int64_t{1002});
        }

        // The id allocator survived the reload: new outfits never reuse ids.
        const TFOutfitRecord* fresh = store.Create("Steel Eagles", "SEAG", 1004, "Grym", 1003);
        EXPECT_NE(fresh, nullptr);
        if (fresh)
            EXPECT_GT(fresh->id, firstId);
        store.Close();
    }
    fs::remove(path);
}

TEST(TFOutfitStore_DisbandAndUpdateMemberName)
{
    const std::string path = TempStorePath("tfoutfit_disband");
    fs::remove(path);

    TFOutfitStore store;
    EXPECT_TRUE(store.Open(path));

    const TFOutfitRecord* first = store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000);
    EXPECT_NE(first, nullptr);
    if (!first)
        return;
    const uint32_t firstId = first->id; // capture: the next Create invalidates the pointer
    const TFOutfitRecord* second = store.Create("Steel Eagles", "SEAG", 1005, "Grym", 1003);
    EXPECT_NE(second, nullptr);
    if (!second)
        return;
    const uint32_t secondId = second->id;

    EXPECT_TRUE(store.Disband(firstId));
    EXPECT_FALSE(store.Disband(firstId)); // already gone
    EXPECT_EQ(store.FindById(firstId), nullptr);
    EXPECT_EQ(store.FindByCharacter(1001), nullptr);
    EXPECT_EQ(store.OutfitCount(), size_t{1});

    // Rename propagation into the surviving roster (enter-world refresh path).
    store.UpdateMemberName(1005, "GrymTheRelentless");
    const TFOutfitRecord* rec = store.FindById(secondId);
    EXPECT_NE(rec, nullptr);
    if (rec)
    {
        const TFOutfitMemberRecord* grym = rec->FindMember(1005);
        EXPECT_NE(grym, nullptr);
        if (grym)
            EXPECT_TRUE(grym->name == "GrymTheRelentless");
    }

    store.Close();
    fs::remove(path);
}

TEST(TFOutfitStore_ClosedStoreRejectsMutations)
{
    const std::string path = TempStorePath("tfoutfit_closed");
    fs::remove(path);

    TFOutfitStore store;
    EXPECT_TRUE(store.Open(path));
    const TFOutfitRecord* created = store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000);
    EXPECT_NE(created, nullptr);
    if (!created)
        return;
    const uint32_t id = created->id;
    store.Close();

    EXPECT_EQ(store.Create("Steel Eagles", "SEAG", 1002, "Vex", 1001), nullptr);
    EXPECT_FALSE(store.AddMember(id, 2002, "Vex", TFOutfitRank::Member, 1001));
    EXPECT_FALSE(store.RemoveMember(id, 1001));
    EXPECT_FALSE(store.SetMemberRank(id, 1001, TFOutfitRank::Member));
    EXPECT_FALSE(store.Disband(id));

    fs::remove(path);
}

TEST(TFOutfitStore_CorruptFileQuarantinedNotWiped)
{
    const std::string path = TempStorePath("tfoutfit_corrupt");
    fs::remove(path);
    // Sweep quarantine baks from previous runs so the count below is exact.
    const fs::path dir = fs::path(path).parent_path();
    const std::string stem = fs::path(path).filename().string();
    for (const auto& e : fs::directory_iterator(dir))
    {
        const std::string fn = e.path().filename().string();
        if (fn.rfind(stem + ".corrupt-", 0) == 0)
            fs::remove(e.path());
    }

    {
        // NB: Spark::Json::Parse is lenient with '{'-prefixed truncated text
        // (it still yields an object), so the payload must not look like an
        // object at all for LoadFromDisk to refuse it (integrator, W8).
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "\x01\x02binary garbage, definitely not a JSON object";
    }

    TFOutfitStore store;
    EXPECT_FALSE(store.Open(path)); // refused, never silently wiped
    EXPECT_FALSE(store.IsOpen());

    // The unreadable file was quarantined to <path>.corrupt-<ms>.bak.
    EXPECT_FALSE(fs::exists(path));
    int baks = 0;
    for (const auto& e : fs::directory_iterator(dir))
    {
        const std::string fn = e.path().filename().string();
        if (fn.rfind(stem + ".corrupt-", 0) == 0)
        {
            ++baks;
            fs::remove(e.path());
        }
    }
    EXPECT_EQ(baks, 1);

    // A second Open on the now-clean path starts an empty store normally.
    EXPECT_TRUE(store.Open(path));
    EXPECT_EQ(store.OutfitCount(), size_t{0});
    store.Close();
    fs::remove(path);
}

TEST(TFOutfitStore_DebouncedFlush)
{
    const std::string path = TempStorePath("tfoutfit_debounce");
    fs::remove(path);

    TFOutfitStore store;
    EXPECT_TRUE(store.Open(path));
    const TFOutfitRecord* created = store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000);
    EXPECT_NE(created, nullptr);
    if (!created)
        return;
    const uint32_t id = created->id;

    // Dirty but inside the 2 s debounce window: nothing on disk yet.
    store.Tick(1.0f);
    EXPECT_FALSE(fs::exists(path));

    // Crossing the window flushes.
    store.Tick(1.5f);
    EXPECT_TRUE(fs::exists(path));

    // A fresh mutation restarts the window from zero.
    EXPECT_TRUE(store.AddMember(id, 2002, "Vex", TFOutfitRank::Member, 1001));
    fs::remove(path);
    store.Tick(1.9f);
    EXPECT_FALSE(fs::exists(path)); // still debouncing
    store.Tick(0.2f);
    EXPECT_TRUE(fs::exists(path)); // 2.1 s since the mutation: flushed

    store.Close();
    fs::remove(path);
}
