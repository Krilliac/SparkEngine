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

    void RemoveOutfitRecoveryArtifacts(const fs::path& path)
    {
        std::error_code ec;
        fs::remove(path, ec);
        const fs::path dir = path.parent_path();
        const std::string prefix = path.filename().string() + ".corrupt-";
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->path().filename().string().rfind(prefix, 0) == 0)
                fs::remove(it->path(), ec);
        }
    }

    void ExpectOutfitDocumentRejected(const char* stem, const std::string& document)
    {
        const fs::path path = TempStorePath(stem);
        RemoveOutfitRecoveryArtifacts(path);
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << document;
        }

        TFOutfitStore store;
        EXPECT_FALSE(store.Open(path));
        EXPECT_FALSE(store.IsOpen());
        // A rejected later row must not expose earlier rows through queries.
        EXPECT_EQ(store.OutfitCount(), size_t{0});
        EXPECT_EQ(store.FindById(1), nullptr);
        EXPECT_TRUE(fs::exists(path));

        int backups = 0;
        std::error_code ec;
        const std::string prefix = path.filename().string() + ".corrupt-";
        for (fs::directory_iterator it(path.parent_path(), ec), end; !ec && it != end; it.increment(ec))
            if (it->path().filename().string().rfind(prefix, 0) == 0)
                ++backups;
        EXPECT_EQ(backups, 1);
        RemoveOutfitRecoveryArtifacts(path);
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

TEST(TFOutfitStore_UnicodePathRoundTrip)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path("Saves") / L"outfit_保存_тест";
    const fs::path path = dir / L"战队.json";
    fs::remove_all(dir);
    {
        TFOutfitStore store;
        EXPECT_TRUE(store.Open(path));
        EXPECT_TRUE(store.Create("Unicode Path", "UNI", 7001, "Leader", 1) != nullptr);
        EXPECT_TRUE(store.SaveNow());
        store.Close();
    }
    {
        TFOutfitStore store;
        EXPECT_TRUE(store.Open(path));
        EXPECT_TRUE(store.FindByTag("UNI") != nullptr);
        store.Close();
    }
    fs::remove_all(dir);
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

    // Keep the corrupt primary in place so a fresh process cannot reinterpret
    // its disappearance as an empty outfit database.
    EXPECT_TRUE(fs::exists(path));
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

    // Both the same instance and a fresh process fail closed until an operator
    // explicitly restores or removes the corrupt primary.
    EXPECT_FALSE(store.Open(path));
    {
        TFOutfitStore recoveredAfterRestart;
        EXPECT_FALSE(recoveredAfterRestart.Open(path));
    }
    fs::remove(path);
    {
        TFOutfitStore missingPrimaryAfterLegacyQuarantine;
        EXPECT_FALSE(missingPrimaryAfterLegacyQuarantine.Open(path));
    }
    for (const auto& e : fs::directory_iterator(dir))
    {
        if (e.path().filename().string().rfind(stem + ".corrupt-", 0) == 0)
            fs::remove(e.path());
    }
    fs::remove(path);
}

TEST(TFOutfitStore_InvalidSchemaIsTransactionalAndRejectsRosterInvariants)
{
    ExpectOutfitDocumentRejected("tfoutfit_duplicate_name",
                                 R"json({"nextOutfitId":3,"outfits":[
          {"id":1,"name":"Iron Vultures","tag":"IVLT","createdAtMs":1,
           "members":[{"charId":1001,"name":"Raska","rank":2,"joinedAtMs":1}]},
          {"id":2,"name":"IRON VULTURES","tag":"SEAG","createdAtMs":2,
           "members":[{"charId":1002,"name":"Vex","rank":2,"joinedAtMs":2}]}
        ]})json");

    ExpectOutfitDocumentRejected("tfoutfit_duplicate_tag",
                                 R"json({"nextOutfitId":3,"outfits":[
          {"id":1,"name":"Iron Vultures","tag":"IVLT","createdAtMs":1,
           "members":[{"charId":1001,"name":"Raska","rank":2,"joinedAtMs":1}]},
          {"id":2,"name":"Steel Eagles","tag":"ivlt","createdAtMs":2,
           "members":[{"charId":1002,"name":"Vex","rank":2,"joinedAtMs":2}]}
        ]})json");

    ExpectOutfitDocumentRejected("tfoutfit_empty_roster",
                                 R"json({"nextOutfitId":2,"outfits":[
          {"id":1,"name":"Iron Vultures","tag":"IVLT","createdAtMs":1,"members":[]}
        ]})json");

    ExpectOutfitDocumentRejected("tfoutfit_missing_leader",
                                 R"json({"nextOutfitId":2,"outfits":[
          {"id":1,"name":"Iron Vultures","tag":"IVLT","createdAtMs":1,
           "members":[{"charId":1001,"name":"Raska","rank":1,"joinedAtMs":1}]}
        ]})json");

    ExpectOutfitDocumentRejected("tfoutfit_multiple_leaders",
                                 R"json({"nextOutfitId":2,"outfits":[
          {"id":1,"name":"Iron Vultures","tag":"IVLT","createdAtMs":1,
           "members":[{"charId":1001,"name":"Raska","rank":2,"joinedAtMs":1},
                      {"charId":1002,"name":"Vex","rank":2,"joinedAtMs":2}]}
        ]})json");

    std::string oversizedRoster =
        R"json({"nextOutfitId":2,"outfits":[{"id":1,"name":"Iron Vultures","tag":"IVLT","createdAtMs":1,"members":[)json";
    for (uint32_t i = 0; i < 129; ++i)
    {
        if (i != 0)
            oversizedRoster += ',';
        oversizedRoster += "{\"charId\":" + std::to_string(1000 + i) +
                           ",\"name\":\"Member\",\"rank\":" + (i == 0 ? "2" : "0") + ",\"joinedAtMs\":1}";
    }
    oversizedRoster += "]}]}";
    ExpectOutfitDocumentRejected("tfoutfit_oversized_roster", oversizedRoster);
}

TEST(TFOutfitStore_ExclusivePersistenceLockRejectsSecondAuthority)
{
    const std::string path = TempStorePath("tfoutfit_exclusive_lock");
    fs::remove(path);

    TFOutfitStore first;
    TFOutfitStore second;
    EXPECT_TRUE(first.Open(path));
    EXPECT_FALSE(second.Open(path));
    EXPECT_TRUE(first.Close());
    EXPECT_TRUE(second.Open(path));
    EXPECT_TRUE(second.Close());

    fs::remove(path);
}

TEST(TFOutfitStore_FailedClosePreservesDirtyOpenStateForRetry)
{
    const std::string path = TempStorePath("tfoutfit_close_retry");
    const fs::path blocker = fs::path(path).wstring() + std::wstring(L".tmp");
    fs::remove(path);
    fs::remove_all(blocker);

    TFOutfitStore store;
    TFOutfitStore contender;
    EXPECT_TRUE(store.Open(path));
    EXPECT_NE(store.Create("Retry Guard", "RTGY", 7001, "Keeper", 1), nullptr);
    EXPECT_TRUE(fs::create_directory(blocker));
    EXPECT_FALSE(store.Close());
    EXPECT_TRUE(store.IsOpen());
    EXPECT_FALSE(contender.Open(path));

    fs::remove_all(blocker);
    EXPECT_TRUE(store.Close());
    EXPECT_TRUE(contender.Open(path));
    EXPECT_NE(contender.FindByName("Retry Guard"), nullptr);
    EXPECT_TRUE(contender.Close());
    fs::remove(path);
}

// ---------------------------------------------------------------------------
// W12 outfit-leaderboards: ISO week key + competition score fields
// ---------------------------------------------------------------------------

namespace
{
    constexpr int64_t kMsPerDay = 86400000;
} // namespace

TEST(TFOutfitStore_ISOWeekKey)
{
    // Known ISO-8601 vectors (days since 1970-01-01; 1970-01-01 was a Thursday).
    // 2026-01-01 (Thu, day 20454) -> W1 of 2026; 2026 has 53 ISO weeks.
    EXPECT_EQ(TFOutfitISOWeekKey(20454 * kMsPerDay), 202601u);
    // 2026-01-04 (Sun) still W1; 2026-01-05 (Mon) starts W2.
    EXPECT_EQ(TFOutfitISOWeekKey(20457 * kMsPerDay), 202601u);
    EXPECT_EQ(TFOutfitISOWeekKey(20458 * kMsPerDay), 202602u);
    // 2027-01-01 (Fri, day 20819) belongs to W53 of ISO year 2026.
    EXPECT_EQ(TFOutfitISOWeekKey(20819 * kMsPerDay), 202653u);
    // Classic Wikipedia vectors: 2005-01-01 (Sat, day 12784) -> 2004-W53;
    // 2005-12-31 (Sat, day 13148) -> 2005-W52.
    EXPECT_EQ(TFOutfitISOWeekKey(12784 * kMsPerDay), 200453u);
    EXPECT_EQ(TFOutfitISOWeekKey(13148 * kMsPerDay), 200552u);
    // Sub-day precision: last ms of a Sunday vs first ms of the Monday after.
    EXPECT_EQ(TFOutfitISOWeekKey(20458 * kMsPerDay - 1), 202601u);
    EXPECT_EQ(TFOutfitISOWeekKey(20458 * kMsPerDay + 1), 202602u);
}

TEST(TFOutfitStore_ScoreAccumulationAndRollover)
{
    const std::string path = TempStorePath("tfoutfit_score");
    fs::remove(path);

    TFOutfitStore store;
    EXPECT_TRUE(store.Open(path));
    const TFOutfitRecord* created = store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000);
    EXPECT_NE(created, nullptr);
    if (!created)
        return;
    const uint32_t id = created->id;

    // Fresh outfit: zero scores, no week stamped.
    EXPECT_EQ(created->weeklyScore, uint64_t{0});
    EXPECT_EQ(created->allTimeScore, uint64_t{0});
    EXPECT_EQ(created->weekKey, uint32_t{0});

    // Accumulate inside one week.
    EXPECT_TRUE(store.AddScore(id, 1, 202628));  // kill
    EXPECT_TRUE(store.AddScore(id, 10, 202628)); // capture
    const TFOutfitRecord* rec = store.FindById(id);
    EXPECT_NE(rec, nullptr);
    if (!rec)
        return;
    EXPECT_EQ(rec->weeklyScore, uint64_t{11});
    EXPECT_EQ(rec->allTimeScore, uint64_t{11});
    EXPECT_EQ(rec->weekKey, uint32_t{202628});

    // Self-healing rollover: a score landing in the NEXT week resets weekly
    // first, all-time keeps accumulating.
    EXPECT_TRUE(store.AddScore(id, 100, 202629)); // alert win, new week
    rec = store.FindById(id);
    EXPECT_NE(rec, nullptr);
    if (!rec)
        return;
    EXPECT_EQ(rec->weeklyScore, uint64_t{100});
    EXPECT_EQ(rec->allTimeScore, uint64_t{111});
    EXPECT_EQ(rec->weekKey, uint32_t{202629});

    // Explicit sweep (server load / tick boundary): weekly zeroed, stamp moves.
    EXPECT_EQ(store.RolloverWeek(202630), size_t{1});
    EXPECT_EQ(store.RolloverWeek(202630), size_t{0}); // idempotent, no dirty churn
    rec = store.FindById(id);
    EXPECT_NE(rec, nullptr);
    if (rec)
    {
        EXPECT_EQ(rec->weeklyScore, uint64_t{0});
        EXPECT_EQ(rec->allTimeScore, uint64_t{111});
        EXPECT_EQ(rec->weekKey, uint32_t{202630});
    }

    // Rejections: unknown outfit, zero points, closed store.
    EXPECT_FALSE(store.AddScore(id + 100, 5, 202630));
    EXPECT_FALSE(store.AddScore(id, 0, 202630));
    store.Close();
    EXPECT_FALSE(store.AddScore(id, 5, 202630));
    EXPECT_EQ(store.RolloverWeek(202631), size_t{0});

    fs::remove(path);
}

TEST(TFOutfitStore_ScoreDiskRoundTripAndAdditiveDefaults)
{
    const std::string path = TempStorePath("tfoutfit_score_rt");
    fs::remove(path);

    uint32_t id = 0;
    {
        TFOutfitStore store;
        EXPECT_TRUE(store.Open(path));
        const TFOutfitRecord* created = store.Create("Iron Vultures", "IVLT", 1001, "Raska", 1000);
        EXPECT_NE(created, nullptr);
        if (!created)
            return;
        id = created->id;
        EXPECT_TRUE(store.AddScore(id, 111, 202628));
        EXPECT_TRUE(store.SaveNow());
        store.Close();
    }
    {
        TFOutfitStore store;
        EXPECT_TRUE(store.Open(path));
        const TFOutfitRecord* rec = store.FindById(id);
        EXPECT_NE(rec, nullptr);
        if (rec)
        {
            EXPECT_EQ(rec->weeklyScore, uint64_t{111});
            EXPECT_EQ(rec->allTimeScore, uint64_t{111});
            EXPECT_EQ(rec->weekKey, uint32_t{202628});
        }
        store.Close();
    }
    fs::remove(path);

    // Pre-W12 file without the score keys: additive defaults load as zero.
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "{\n  \"nextOutfitId\": 2,\n  \"outfits\": [\n    {\n      \"id\": 1,\n      \"name\": \"Old Guard\",\n"
             "      \"tag\": \"OLDG\",\n      \"createdAtMs\": 1000,\n      \"members\": [\n        {\n"
             "          \"charId\": 1001, \"name\": \"Raska\", \"rank\": 2, \"joinedAtMs\": 1000\n        }\n"
             "      ]\n    }\n  ]\n}\n";
    }
    {
        TFOutfitStore store;
        EXPECT_TRUE(store.Open(path));
        const TFOutfitRecord* rec = store.FindById(1);
        EXPECT_NE(rec, nullptr);
        if (rec)
        {
            EXPECT_EQ(rec->weeklyScore, uint64_t{0});
            EXPECT_EQ(rec->allTimeScore, uint64_t{0});
            EXPECT_EQ(rec->weekKey, uint32_t{0});
        }
        store.Close();
    }
    fs::remove(path);
}

TEST(TFOutfitStore_MalformedAdditiveScoreFieldsAreRejected)
{
    const auto expectRejected = [](const char* stem, const char* scoreFields)
    {
        const std::string path = TempStorePath(stem);
        const fs::path dir = fs::path(path).parent_path();
        const std::string filename = fs::path(path).filename().string();
        fs::remove(path);
        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (entry.path().filename().string().rfind(filename + ".corrupt-", 0) == 0)
                fs::remove(entry.path());
        }

        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << "{\n  \"nextOutfitId\": 2,\n  \"outfits\": [{\n"
                 "    \"id\": 1, \"name\": \"Old Guard\", \"tag\": \"OLDG\", \"createdAtMs\": 1000,\n"
              << "    " << scoreFields << ",\n"
              << "    \"members\": [{\"charId\": 1001, \"name\": \"Raska\", \"rank\": 2, "
                 "\"joinedAtMs\": 1000}]\n"
                 "  }]\n}\n";
        }

        TFOutfitStore store;
        EXPECT_FALSE(store.Open(path));
        EXPECT_FALSE(store.IsOpen());
        EXPECT_TRUE(fs::exists(path));

        int backups = 0;
        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (entry.path().filename().string().rfind(filename + ".corrupt-", 0) == 0)
            {
                ++backups;
                fs::remove(entry.path());
            }
        }
        EXPECT_EQ(backups, 1);
        fs::remove(path);
    };

    expectRejected("tfoutfit_fractional_score", "\"weeklyScore\": 1.5");
    expectRejected("tfoutfit_rounded_score", "\"weeklyScore\": 1.0000000000000001");
    expectRejected("tfoutfit_negative_score", "\"allTimeScore\": -1");
    expectRejected("tfoutfit_oversize_week", "\"weekKey\": 4294967296");
    expectRejected("tfoutfit_string_score", "\"weeklyScore\": \"1\"");
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
