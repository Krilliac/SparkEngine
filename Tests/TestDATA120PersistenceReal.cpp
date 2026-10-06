/**
 * @file TestDATA120PersistenceReal.cpp
 * @brief DATA-120: TERRAFRONT character persistence must commit multi-row
 *        economy changes atomically and fail closed on schema versions it
 *        does not understand.
 *
 * Every test drives the production TFDatabase / TFPlayerMetaStore against a
 * real file. Write failures are injected by occupying the "<db>.tmp" staging
 * path with a directory, the same technique TestTFOnboarding.cpp uses.
 * The Persistence_Durable_* cases pin the SavePaths::WriteDurableReplace
 * commit primitive every TERRAFRONT store writes through.
 */
#include "TestFramework.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFPlayerMeta.h"
#include "Persistence/TFSavePaths.h"
#include "Persistence/TFWorldSave.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace Terrafront;

namespace
{
    namespace fs = std::filesystem;

    fs::path FreshDbPath(const char* name)
    {
        const fs::path path = fs::path("Saves") / name;
        fs::create_directories(path.parent_path());
        fs::remove(path);
        fs::remove_all(fs::path(path.wstring() + L".tmp"));
        return path;
    }

    fs::path StagingBlocker(const fs::path& path)
    {
        return fs::path(path.wstring() + L".tmp");
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void WriteFile(const fs::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    /// Account + one character with a flux balance, as an onboarded player.
    uint64_t SeedCharacter(TFDatabase& db, const char* user, const char* name, uint32_t flux)
    {
        TFAccountRecord account;
        TFCharacterRecord character;
        if (!db.FindAccountByUsername(user, account) && !db.CreateAccount(user, "salt", "hash", account))
            return 0;
        if (!db.CreateCharacter(account.id, name, FactionId::MRA, character))
            return 0;
        if (flux != 0 && !db.SaveCharacterProgress(character.id, 0, 1, flux, 0))
            return 0;
        return character.id;
    }

    /// An unlock purchase: flux debit + new unlock key, which must land together.
    TFCharacterUpdate Purchase(uint64_t charId, uint32_t fluxAfter, const char* unlockKey)
    {
        TFCharacterUpdate update;
        update.charId = charId;
        update.writeProgress = true;
        update.xp = 0;
        update.rank = 1;
        update.flux = fluxAfter;
        update.lastPlayedMs = 77;
        update.writeMeta = true;
        update.unlocks = {unlockKey};
        return update;
    }
} // namespace

TEST(Persistence_Transaction_UnlockPurchaseIsAllOrNothingAcrossRestart)
{
    const fs::path path = FreshDbPath("test_data120_purchase.db");
    uint64_t charId = 0;
    {
        TFDatabase db;
        ASSERT_TRUE(db.Open(path));
        charId = SeedCharacter(db, "buyer", "Buyer", 100);
        ASSERT_TRUE(charId != 0);

        // Crash/IO failure at the commit point: neither the debit nor the
        // unlock may become visible, in memory or on disk.
        ASSERT_TRUE(fs::create_directory(StagingBlocker(path)));
        EXPECT_FALSE(db.CommitCharacterUpdates({Purchase(charId, 40, "smg_basic")}));
        EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::WriteFailed);
        TFCharacterRecord inMemory;
        ASSERT_TRUE(db.FindCharacter(charId, inMemory));
        EXPECT_EQ(inMemory.flux, uint32_t{100});
        EXPECT_TRUE(inMemory.unlocks.empty());
        fs::remove_all(StagingBlocker(path));
        EXPECT_TRUE(db.Close());
    }
    {
        TFDatabase restarted;
        ASSERT_TRUE(restarted.Open(path));
        TFCharacterRecord onDisk;
        ASSERT_TRUE(restarted.FindCharacter(charId, onDisk));
        EXPECT_EQ(onDisk.flux, uint32_t{100});
        EXPECT_TRUE(onDisk.unlocks.empty());

        EXPECT_TRUE(restarted.CommitCharacterUpdates({Purchase(charId, 40, "smg_basic")}));
        EXPECT_TRUE(restarted.Close());
    }
    {
        TFDatabase restarted;
        ASSERT_TRUE(restarted.Open(path));
        TFCharacterRecord onDisk;
        ASSERT_TRUE(restarted.FindCharacter(charId, onDisk));
        EXPECT_EQ(onDisk.flux, uint32_t{40});
        EXPECT_EQ(onDisk.lastPlayedMs, int64_t{77});
        ASSERT_EQ(onDisk.unlocks.size(), size_t{1});
        EXPECT_TRUE(onDisk.unlocks[0] == "smg_basic");
        EXPECT_TRUE(restarted.Close());
    }
    fs::remove(path);
}

TEST(Persistence_Transaction_CurrencyTransferRejectsWholeBatchOnInvalidRow)
{
    const fs::path path = FreshDbPath("test_data120_transfer.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    const uint64_t payer = SeedCharacter(db, "payer", "Payer", 500);
    const uint64_t payee = SeedCharacter(db, "payee", "Payee", 0);
    ASSERT_TRUE(payer != 0 && payee != 0);
    const std::string before = ReadFile(path);

    TFCharacterUpdate debit;
    debit.charId = payer;
    debit.writeProgress = true;
    debit.flux = 450;
    TFCharacterUpdate credit;
    credit.charId = payee;
    credit.writeProgress = true;
    credit.flux = 50;

    // A later row that fails validation (rank 0) must not let the debit land.
    TFCharacterUpdate invalid = credit;
    invalid.rank = 0;
    EXPECT_FALSE(db.CommitCharacterUpdates({debit, invalid}));
    // An unknown character id likewise rejects the whole batch.
    TFCharacterUpdate ghost = credit;
    ghost.charId = 999999;
    EXPECT_FALSE(db.CommitCharacterUpdates({debit, ghost}));
    // The same character twice in one batch is ambiguous and rejected.
    EXPECT_FALSE(db.CommitCharacterUpdates({debit, debit}));

    TFCharacterRecord payerRec;
    ASSERT_TRUE(db.FindCharacter(payer, payerRec));
    EXPECT_EQ(payerRec.flux, uint32_t{500});
    EXPECT_TRUE(ReadFile(path) == before);

    EXPECT_TRUE(db.CommitCharacterUpdates({debit, credit}));
    TFCharacterRecord payeeRec;
    ASSERT_TRUE(db.FindCharacter(payer, payerRec));
    ASSERT_TRUE(db.FindCharacter(payee, payeeRec));
    EXPECT_EQ(payerRec.flux + payeeRec.flux, uint32_t{500});
    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(Persistence_Transaction_MetaSweepCommitsProgressAndMetaInOneWrite)
{
    const fs::path path = FreshDbPath("test_data120_sweep.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    const uint64_t first = SeedCharacter(db, "sweep", "SweepA", 300);
    const uint64_t second = SeedCharacter(db, "sweep", "SweepB", 300);
    ASSERT_TRUE(first != 0 && second != 0);

    TFPlayerMetaStore store;
    TFCharacterRecord rec;
    ASSERT_TRUE(db.FindCharacter(first, rec));
    store.SeedFromRecord(PlayerId{1}, rec);
    ASSERT_TRUE(db.FindCharacter(second, rec));
    store.SeedFromRecord(PlayerId{2}, rec);
    store.Ensure(PlayerId{1}).unlocks.insert("rifle_a");
    store.Ensure(PlayerId{1}).dirty = true;
    store.Ensure(PlayerId{2}).unlocks.insert("rifle_b");
    store.Ensure(PlayerId{2}).dirty = true;

    TFCharacterUpdate firstDebit;
    firstDebit.charId = first;
    firstDebit.writeProgress = true;
    firstDebit.flux = 200;
    TFCharacterUpdate secondDebit = firstDebit;
    secondDebit.charId = second;

    ASSERT_TRUE(fs::create_directory(StagingBlocker(path)));
    EXPECT_FALSE(store.PersistAllDirty(db, {firstDebit, secondDebit}));
    EXPECT_TRUE(store.AnyDirty()); // nothing acknowledged, retry keeps the meta
    TFCharacterRecord unchanged;
    ASSERT_TRUE(db.FindCharacter(first, unchanged));
    EXPECT_EQ(unchanged.flux, uint32_t{300});
    EXPECT_TRUE(unchanged.unlocks.empty());
    fs::remove_all(StagingBlocker(path));

    EXPECT_TRUE(store.PersistAllDirty(db, {firstDebit, secondDebit}));
    EXPECT_FALSE(store.AnyDirty());
    EXPECT_TRUE(db.Close());

    TFDatabase restarted;
    ASSERT_TRUE(restarted.Open(path));
    TFCharacterRecord a;
    TFCharacterRecord b;
    ASSERT_TRUE(restarted.FindCharacter(first, a));
    ASSERT_TRUE(restarted.FindCharacter(second, b));
    EXPECT_EQ(a.flux, uint32_t{200});
    EXPECT_EQ(b.flux, uint32_t{200});
    ASSERT_EQ(a.unlocks.size(), size_t{1});
    ASSERT_EQ(b.unlocks.size(), size_t{1});
    EXPECT_TRUE(a.unlocks[0] == "rifle_a");
    EXPECT_TRUE(b.unlocks[0] == "rifle_b");
    EXPECT_TRUE(restarted.Close());
    fs::remove(path);
}

TEST(Persistence_Migration_LegacyUnversionedFileUpgradesInPlace)
{
    // N-1 fixture: the pre-DATA-120 on-disk shape carries no schemaVersion.
    const fs::path path = FreshDbPath("test_data120_legacy.db");
    WriteFile(path, R"({
  "nextAccountId": 2,
  "nextCharId": 2,
  "accounts": [{"id": 1, "username": "old", "salt": "s", "passwordHash": "h",
                "createdAtMs": 1, "lastLoginMs": 2}],
  "characters": [{"id": 1, "accountId": 1, "name": "Veteran", "faction": 1, "xp": 10,
                  "rank": 2, "flux": 30, "createdAtMs": 1, "lastPlayedMs": 2}]
})");

    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFCharacterRecord veteran;
    ASSERT_TRUE(db.FindCharacterByName("Veteran", veteran));
    EXPECT_EQ(veteran.flux, uint32_t{30});
    EXPECT_TRUE(db.TouchLogin(1, 99));
    EXPECT_TRUE(db.Close());

    // The first write stamps the current schema.
    const std::string upgraded = ReadFile(path);
    EXPECT_TRUE(upgraded.find("\"schemaVersion\": " + std::to_string(TFDatabase::kSchemaVersion)) != std::string::npos);
    EXPECT_TRUE(upgraded.find("appliedOperations") == std::string::npos);
    TFDatabase reopened;
    ASSERT_TRUE(reopened.Open(path));
    ASSERT_TRUE(reopened.FindCharacterByName("Veteran", veteran));
    EXPECT_EQ(veteran.rank, uint16_t{2});
    EXPECT_TRUE(reopened.Close());
    fs::remove(path);
}

TEST(Persistence_Migration_RetiredLedgerSchemaLoadsOnlyWithEmptyLedger)
{
    // b2d2953 wrote schema v3: v2 content plus an "appliedOperations" ledger
    // that no caller ever filled. Such a file loads as v2 content and is
    // rewritten in the current schema; a ledger that holds ids, or a v3 file
    // without the ledger, fails closed rather than being rewritten.
    const std::string body = R"("revision": 4, "nextAccountId": 2, "nextCharId": 2,
  "accounts": [{"id": 1, "username": "cloud", "salt": "s", "passwordHash": "h",
                "createdAtMs": 1, "lastLoginMs": 2}],
  "characters": [{"id": 1, "accountId": 1, "name": "Ledgered", "faction": 1, "xp": 10,
                  "rank": 2, "flux": 30, "createdAtMs": 1, "lastPlayedMs": 2, "revision": 4}])";
    const fs::path path = FreshDbPath("test_data120_v3ledger.db");
    WriteFile(path, R"({"schemaVersion": 3, )" + body + R"(, "appliedOperations": []})");
    {
        TFDatabase db;
        ASSERT_TRUE(db.Open(path));
        TFCharacterRecord row;
        ASSERT_TRUE(db.FindCharacterByName("Ledgered", row));
        EXPECT_EQ(row.flux, uint32_t{30});
        EXPECT_EQ(row.revision, uint64_t{4});
        EXPECT_TRUE(db.TouchLogin(1, 99));
        EXPECT_TRUE(db.Close());
    }
    const std::string rewritten = ReadFile(path);
    EXPECT_TRUE(rewritten.find("\"schemaVersion\": " + std::to_string(TFDatabase::kSchemaVersion)) !=
                std::string::npos);
    EXPECT_TRUE(rewritten.find("appliedOperations") == std::string::npos);
    {
        TFDatabase reopened;
        ASSERT_TRUE(reopened.Open(path));
        TFCharacterRecord row;
        ASSERT_TRUE(reopened.FindCharacterByName("Ledgered", row));
        EXPECT_EQ(row.flux, uint32_t{30});
        EXPECT_EQ(row.rank, uint16_t{2});
        EXPECT_TRUE(reopened.Close());
    }

    for (const char* ledger : {R"(, "appliedOperations": ["purchase-1"])", R"(, "appliedOperations": {})", ""})
    {
        const std::string text = R"({"schemaVersion": 3, )" + body + ledger + "}";
        WriteFile(path, text);
        TFDatabase db;
        EXPECT_FALSE(db.Open(path));
        EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::UnsupportedVersion);
        EXPECT_TRUE(ReadFile(path) == text);
    }
    fs::remove(path);
}

TEST(Persistence_Migration_NewerSchemaFailsClosedWithoutRewrite)
{
    // Rollback guard: an older binary must never load-and-rewrite a file
    // written by a newer schema, which would silently drop the newer fields.
    const fs::path path = FreshDbPath("test_data120_newer.db");
    // Above the retired v3 too, so this exercises the plain newer-schema gate.
    const uint32_t kNewerSchema = std::max(TFDatabase::kSchemaVersion, TFDatabase::kRetiredLedgerSchemaVersion) + 1;
    const std::string newer = std::string(R"({"schemaVersion": )") + std::to_string(kNewerSchema) +
                              R"(, "nextAccountId": 1, "nextCharId": 1, "accounts": [], "characters": [],
  "futureLedger": [{"txn": 1}]})";
    WriteFile(path, newer);

    TFDatabase db;
    EXPECT_FALSE(db.Open(path));
    EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::UnsupportedVersion);
    EXPECT_TRUE(db.RecoveryLatched());
    EXPECT_TRUE(ReadFile(path) == newer);
    TFAccountRecord account;
    EXPECT_FALSE(db.CreateAccount("intruder", "s", "h", account));
    EXPECT_TRUE(ReadFile(path) == newer);

    // Malformed versions are corruption, not "legacy".
    for (const char* bad : {R"("1")", "0", "-1", "1.5"})
    {
        const fs::path badPath = FreshDbPath("test_data120_badversion.db");
        WriteFile(badPath, std::string(R"({"schemaVersion": )") + bad + R"(, "accounts": [], "characters": []})");
        TFDatabase badDb;
        EXPECT_FALSE(badDb.Open(badPath));
        EXPECT_TRUE(badDb.LastStatus() == TFDatabaseStatus::Corrupt);
        for (const auto& entry : fs::directory_iterator(badPath.parent_path()))
            if (entry.path().filename().string().rfind("test_data120_badversion.db.corrupt-", 0) == 0)
                fs::remove(entry.path());
        fs::remove(badPath);
    }
    fs::remove(path);
}

namespace
{
    /// A three-region lattice: an MRA skyanchor, an outpost and a fort.
    std::vector<RegionDef> TerritoryFixtureRegions()
    {
        std::vector<RegionDef> regions(3);
        regions[0].tier = "skyanchor";
        regions[0].homeFaction = FactionId::MRA;
        regions[1].tier = "outpost";
        regions[2].tier = "fort";
        return regions;
    }

    WorldSave::TerritoryDecodeResult DecodeTerritoryFixture(const std::string& text, bool legacySource,
                                                            WorldSave::TerritoryDecode& out)
    {
        Spark::Json::Value root;
        std::string detail;
        if (!Spark::Json::ParseStrict(text, &root, &detail))
            return WorldSave::TerritoryDecodeResult::Invalid;
        const std::vector<RegionDef> regions = TerritoryFixtureRegions();
        return WorldSave::DecodeTerritory(root, regions, legacySource, out, detail);
    }
} // namespace

TEST(Persistence_Migration_TerritoryLegacyUnversionedFileMigrates)
{
    // N-1 fixture: the pre-versioned territory layout, no "version" and no "dominion". Its skyanchor
    // carries an AUC owner, which the v0 schema did not police; the decode coerces it home.
    WorldSave::TerritoryDecode decoded;
    const auto result = DecodeTerritoryFixture(
        R"({"continentKey": "cindral_wastes", "continent": "Cindral Wastes", "regionCount": 3,
            "owners": [2, 3, 0]})",
        false, decoded);
    ASSERT_TRUE(result == WorldSave::TerritoryDecodeResult::Migrate);
    EXPECT_EQ(decoded.sourceVersion, uint32_t{0});
    ASSERT_EQ(decoded.owners.size(), size_t{3});
    EXPECT_TRUE(decoded.owners[0] == FactionId::MRA);
    EXPECT_TRUE(decoded.owners[1] == FactionId::HLX);
    EXPECT_TRUE(decoded.owners[2] == FactionId::None);
    EXPECT_FALSE(decoded.dominion.active);

    // A current-schema document read from the legacy location is also rewritten.
    WorldSave::TerritoryDecode legacy;
    EXPECT_TRUE(DecodeTerritoryFixture(R"({"version": 1, "continent": "Cindral Wastes", "regionCount": 3,
            "owners": [1, 2, 2], "dominion": {"active": false, "faction": 0, "remainingSec": 0}})",
                                       true, legacy) == WorldSave::TerritoryDecodeResult::Migrate);
}

TEST(Persistence_Migration_TerritoryCurrentSchemaLoadsAndRejectsSkyanchorConflict)
{
    WorldSave::TerritoryDecode decoded;
    const auto result = DecodeTerritoryFixture(
        R"({"version": 1, "continentKey": "cindral_wastes", "regionCount": 3, "owners": [1, 2, 1],
            "dominion": {"active": true, "faction": 2, "remainingSec": 120.5}})",
        false, decoded);
    ASSERT_TRUE(result == WorldSave::TerritoryDecodeResult::Loaded);
    EXPECT_EQ(decoded.sourceVersion, WorldSave::kTerritorySchemaVersion);
    EXPECT_TRUE(decoded.owners[1] == FactionId::AUC);
    EXPECT_TRUE(decoded.dominion.active);
    EXPECT_EQ(decoded.dominion.faction, uint32_t{2});

    // In the current schema a skyanchor away from its home faction is corruption, not something to coerce,
    // and so is a missing dominion.
    WorldSave::TerritoryDecode untouched;
    untouched.sourceVersion = 77;
    EXPECT_TRUE(DecodeTerritoryFixture(R"({"version": 1, "regionCount": 3, "owners": [2, 2, 1],
            "dominion": {"active": false, "faction": 0, "remainingSec": 0}})",
                                       false, untouched) == WorldSave::TerritoryDecodeResult::Invalid);
    EXPECT_TRUE(DecodeTerritoryFixture(R"({"version": 1, "regionCount": 3, "owners": [1, 2, 1]})", false, untouched) ==
                WorldSave::TerritoryDecodeResult::Invalid);
    EXPECT_EQ(untouched.sourceVersion, uint32_t{77});
}

TEST(Persistence_Migration_TerritoryNewerSchemaIsRefused)
{
    // Rollback fixture: a file a newer build wrote, with a field this build does not know. It must be
    // refused as a newer schema, not decoded, so the caller latches writes off and never rewrites it.
    const std::string newer = std::string(R"({"version": )") + std::to_string(WorldSave::kTerritorySchemaVersion + 1) +
                              R"(, "continentKey": "cindral_wastes", "regionCount": 3, "owners": [1, 2, 1],
            "dominion": {"active": false, "faction": 0, "remainingSec": 0}, "siegeTimers": [30, 0, 0]})";
    WorldSave::TerritoryDecode decoded;
    EXPECT_TRUE(DecodeTerritoryFixture(newer, false, decoded) == WorldSave::TerritoryDecodeResult::NewerSchema);
    EXPECT_TRUE(decoded.owners.empty());
    EXPECT_TRUE(DecodeTerritoryFixture(newer, true, decoded) == WorldSave::TerritoryDecodeResult::NewerSchema);

    // Malformed versions are corruption, not "legacy".
    for (const char* bad : {R"("1")", "-1", "1.5"})
    {
        const std::string text = std::string(R"({"version": )") + bad +
                                 R"(, "regionCount": 3, "owners": [1, 2, 1],
            "dominion": {"active": false, "faction": 0, "remainingSec": 0}})";
        EXPECT_TRUE(DecodeTerritoryFixture(text, false, decoded) == WorldSave::TerritoryDecodeResult::Invalid);
    }
}

TEST(Persistence_Migration_TerritoryWrongLatticeIsInvalid)
{
    WorldSave::TerritoryDecode decoded;
    const char* const lattices[] = {
        R"({"version": 1, "regionCount": 4, "owners": [1, 2, 1, 0],
            "dominion": {"active": false, "faction": 0, "remainingSec": 0}})",
        R"({"version": 1, "regionCount": 3, "owners": [1, 2],
            "dominion": {"active": false, "faction": 0, "remainingSec": 0}})",
        R"({"version": 1, "regionCount": 3, "owners": [1, 2, 4],
            "dominion": {"active": false, "faction": 0, "remainingSec": 0}})",
        R"({"version": 1, "owners": [1, 2, 1], "dominion": {"active": false, "faction": 0, "remainingSec": 0}})",
    };
    for (const char* text : lattices)
        EXPECT_TRUE(DecodeTerritoryFixture(text, false, decoded) == WorldSave::TerritoryDecodeResult::Invalid);
    EXPECT_TRUE(decoded.owners.empty());
}

TEST(Persistence_Durable_ReplaceCommitsExactBytesAndClearsStaleStaging)
{
    const fs::path path = FreshDbPath("test_data120_durable.json");
    const fs::path staging = StagingBlocker(path);

    std::error_code ec;
    ASSERT_TRUE(SavePaths::WriteDurableReplace(path, "first revision", ec));
    EXPECT_FALSE(static_cast<bool>(ec));
    EXPECT_TRUE(ReadFile(path) == "first revision");
    EXPECT_FALSE(fs::exists(staging));

    // A crashed writer's leftover staging file is discarded, never merged into the commit.
    WriteFile(staging, "partial bytes from a crashed writer that are longer than the new revision");
    ASSERT_TRUE(SavePaths::WriteDurableReplace(path, "second", ec));
    EXPECT_TRUE(ReadFile(path) == "second");
    EXPECT_FALSE(fs::exists(staging));

    // Empty payloads and embedded NULs are committed byte-exactly.
    const std::string binary("a\0b\0c", 5);
    ASSERT_TRUE(SavePaths::WriteDurableReplace(path, binary, ec));
    EXPECT_TRUE(ReadFile(path) == binary);
    ASSERT_TRUE(SavePaths::WriteDurableReplace(path, "", ec));
    EXPECT_EQ(fs::file_size(path), uintmax_t{0});

    EXPECT_FALSE(SavePaths::WriteDurableReplace(fs::path{}, "x", ec));
    EXPECT_TRUE(ec == std::make_error_code(std::errc::invalid_argument));
    fs::remove(path);
}

TEST(Persistence_Durable_FailedStagingLeavesCommittedFileUntouched)
{
    const fs::path path = FreshDbPath("test_data120_durable_fail.json");
    const fs::path staging = StagingBlocker(path);
    std::error_code ec;
    ASSERT_TRUE(SavePaths::WriteDurableReplace(path, "committed", ec));

    ASSERT_TRUE(fs::create_directory(staging));
    EXPECT_FALSE(SavePaths::WriteDurableReplace(path, "must not land", ec));
    EXPECT_TRUE(static_cast<bool>(ec));
    EXPECT_TRUE(ReadFile(path) == "committed");
    EXPECT_TRUE(fs::is_directory(staging));
    fs::remove_all(staging);

    // A missing parent is reported, not created behind the caller's back.
    const fs::path orphan = fs::path("Saves") / "test_data120_no_such_dir" / "store.json";
    fs::remove_all(orphan.parent_path());
    EXPECT_FALSE(SavePaths::WriteDurableReplace(orphan, "x", ec));
    EXPECT_FALSE(fs::exists(orphan.parent_path()));
    fs::remove(path);
}

TEST(Persistence_Durable_StoreCommitUnlinksStaleStagingSymlink)
{
    // TFDatabase::SaveToDisk commits through the durable primitive. On POSIX a stale symlink left at
    // the staging path is unlinked, not followed into (and truncating) the file it names, and the
    // committed file is owner-only because the store holds password hashes. This pins the stale-entry
    // unlink; the O_EXCL|O_NOFOLLOW create flags only matter if an entry is planted between that
    // unlink and open(), which a test cannot schedule without a hook. Windows has no owner-only
    // mode here: the committed file inherits the parent directory's ACL, so only commit and reopen
    // are asserted there.
    const fs::path path = FreshDbPath("test_data120_durable_link.db");
#ifndef _WIN32
    const fs::path staging = StagingBlocker(path);
    const fs::path victim = fs::path("Saves") / "test_data120_durable_victim.txt";
    WriteFile(victim, "victim contents");
    fs::create_symlink(fs::absolute(victim), staging);
#endif

    {
        TFDatabase db;
        ASSERT_TRUE(db.Open(path));
        ASSERT_TRUE(SeedCharacter(db, "durable_user", "Durable", 5) != 0);
    }
    EXPECT_TRUE(ReadFile(path).find("durable_user") != std::string::npos);

#ifndef _WIN32
    EXPECT_TRUE(ReadFile(victim) == "victim contents");
    EXPECT_FALSE(fs::exists(fs::symlink_status(staging)));
    const fs::perms mode = fs::status(path).permissions();
    EXPECT_TRUE((mode & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    fs::remove(victim);
#endif

    TFDatabase reopened;
    ASSERT_TRUE(reopened.Open(path));
    TFAccountRecord account;
    EXPECT_TRUE(reopened.FindAccountByUsername("durable_user", account));
    fs::remove(path);
}

#ifndef _WIN32
TEST(Persistence_Durable_DirectorySyncFailureAfterRenameStillReportsCommit)
{
    // Once the rename lands, the commit is reported as committed even if the parent directory cannot
    // be synced; otherwise TFDatabase would roll its in-memory state back while the disk holds the new
    // revision. A write+search-only directory (0300) lets rename() succeed while open(dir, O_RDONLY)
    // fails with EACCES. Root bypasses permission checks, so the failure cannot be forced there.
    if (::geteuid() == 0)
        SKIP_TEST("directory permission checks do not apply to root");

    const fs::path dir = fs::path("Saves") / "test_data120_durable_nosync";
    fs::remove_all(dir);
    ASSERT_TRUE(fs::create_directories(dir));
    const fs::path path = dir / "store.db";

    std::error_code ec;
    ASSERT_TRUE(SavePaths::WriteDurableReplace(path, "first", ec));
    EXPECT_FALSE(static_cast<bool>(ec));

    // TFDatabase keeps its in-memory state in step with the disk through the same commit.
    bool dbCommitted = false;
    bool committed = false;
    std::error_code syncWarning;
    {
        TFDatabase db;
        const bool opened = db.Open(dir / "accounts.db");
        fs::permissions(dir, fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::replace);
        committed = SavePaths::WriteDurableReplace(path, "second", ec);
        syncWarning = ec;
        dbCommitted = opened && SeedCharacter(db, "nosync_user", "NoSync", 7) != 0;
        TFAccountRecord inMemory;
        dbCommitted = dbCommitted && db.FindAccountByUsername("nosync_user", inMemory);
    }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);

    EXPECT_TRUE(committed);
    EXPECT_TRUE(syncWarning == std::make_error_code(std::errc::permission_denied));
    EXPECT_TRUE(ReadFile(path) == "second");
    EXPECT_FALSE(fs::exists(StagingBlocker(path)));
    EXPECT_TRUE(dbCommitted);

    TFDatabase reopened;
    ASSERT_TRUE(reopened.Open(dir / "accounts.db"));
    TFAccountRecord account;
    EXPECT_TRUE(reopened.FindAccountByUsername("nosync_user", account));
    fs::remove_all(dir);
}
#endif

// ---------------------------------------------------------------------------
// Persistence_Concurrency_*: two writers on one store can neither duplicate nor lose state. The world
// files (territory, progression state) are single-writer behind a lifetime lease; the character database
// is multi-writer, serialized per call, with unique-name creates and revision-checked absolute writes.
// ---------------------------------------------------------------------------

namespace
{
    Spark::Json::Value TerritoryWithOwner(int contestedOwner)
    {
        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        root["version"] = Spark::Json::Value(static_cast<int>(WorldSave::kTerritorySchemaVersion));
        root["continentKey"] = Spark::Json::Value(std::string("concurrency_continent"));
        root["continent"] = Spark::Json::Value(std::string("Concurrency"));
        root["regionCount"] = Spark::Json::Value(3);
        Spark::Json::Value owners = Spark::Json::Value::MakeArray();
        owners.PushBack(Spark::Json::Value(1));
        owners.PushBack(Spark::Json::Value(contestedOwner));
        owners.PushBack(Spark::Json::Value(0));
        root["owners"] = std::move(owners);
        return root;
    }

    int ContestedOwnerOnDisk(const fs::path& path)
    {
        Spark::Json::Value root;
        std::string detail;
        if (WorldSave::ReadJson(path, "concurrency_continent", "Concurrency", false, root, detail) !=
            WorldSave::ReadStatus::Loaded)
            return -1;
        return static_cast<int>(root["owners"][size_t{1}].AsNumber(-1.0));
    }
} // namespace

TEST(Persistence_Concurrency_WorldFileWriteRequiresHeldLease)
{
    const fs::path path = FreshDbPath("test_data120_lease_territory.json");
    const fs::path other = FreshDbPath("test_data120_lease_other.json");
    std::string detail;

    // Never locked: refused before any I/O, nothing created.
    SavePaths::ExclusiveFileLock unlocked;
    EXPECT_TRUE(unlocked.LockedTarget().empty());
    EXPECT_FALSE(WorldSave::WriteJson(unlocked, path, TerritoryWithOwner(2), detail));
    EXPECT_TRUE(detail == "world file writer lease not held");
    EXPECT_FALSE(fs::exists(path));

    // A lease on another file does not authorize this one.
    SavePaths::ExclusiveFileLock wrongFile;
    std::error_code ec;
    ASSERT_TRUE(wrongFile.TryLock(other, ec));
    EXPECT_TRUE(wrongFile.LockedTarget() == other);
    EXPECT_FALSE(WorldSave::WriteJson(wrongFile, path, TerritoryWithOwner(2), detail));
    EXPECT_TRUE(detail == "world file writer lease not held");
    EXPECT_FALSE(fs::exists(path));

    // The held lease commits; once released, the same guard is refused and the bytes stay put.
    SavePaths::ExclusiveFileLock lease;
    ASSERT_TRUE(lease.TryLock(path, ec));
    ASSERT_TRUE(WorldSave::WriteJson(lease, path, TerritoryWithOwner(2), detail));
    const std::string committed = ReadFile(path);
    lease.Unlock();
    EXPECT_TRUE(lease.LockedTarget().empty());
    EXPECT_FALSE(WorldSave::WriteJson(lease, path, TerritoryWithOwner(3), detail));
    EXPECT_TRUE(detail == "world file writer lease not held");
    EXPECT_FALSE(WorldSave::WriteJson(wrongFile, path, TerritoryWithOwner(3), detail));
    EXPECT_TRUE(ReadFile(path) == committed);
    EXPECT_FALSE(fs::exists(StagingBlocker(path)));

    wrongFile.Unlock();
    fs::remove(path);
    fs::remove(other);
}

TEST(Persistence_Concurrency_SecondWorldFileWriterIsRefusedAndSeesFirstCommit)
{
    const fs::path path = FreshDbPath("test_data120_lease_contended.json");
    std::string detail;
    std::error_code ec;

    SavePaths::ExclusiveFileLock first;
    SavePaths::ExclusiveFileLock second;
    ASSERT_TRUE(first.TryLock(path, ec));
    EXPECT_FALSE(second.TryLock(path, ec)); // a second authority for the continent cannot take the lease
    EXPECT_FALSE(second.IsLocked());

    ASSERT_TRUE(WorldSave::WriteJson(first, path, TerritoryWithOwner(2), detail));
    EXPECT_FALSE(WorldSave::WriteJson(second, path, TerritoryWithOwner(3), detail));
    EXPECT_EQ(ContestedOwnerOnDisk(path), 2);
    ASSERT_TRUE(WorldSave::WriteJson(first, path, TerritoryWithOwner(1), detail));
    EXPECT_EQ(ContestedOwnerOnDisk(path), 1);

    // Handover: the next owner starts from the first owner's last commit, so nothing is lost.
    first.Unlock();
    ASSERT_TRUE(second.TryLock(path, ec));
    EXPECT_EQ(ContestedOwnerOnDisk(path), 1);
    EXPECT_FALSE(WorldSave::WriteJson(first, path, TerritoryWithOwner(2), detail));
    ASSERT_TRUE(WorldSave::WriteJson(second, path, TerritoryWithOwner(3), detail));
    EXPECT_EQ(ContestedOwnerOnDisk(path), 3);

    second.Unlock();
    fs::remove(path);
}

TEST(Persistence_Concurrency_ContestedCharacterNameCreatesOnce)
{
    const fs::path path = FreshDbPath("test_data120_contested_create.db");
    TFDatabase continentA;
    TFDatabase continentB;
    // Both open the empty store before either creates, so each one's own view is stale for the other.
    ASSERT_TRUE(continentA.Open(path));
    ASSERT_TRUE(continentB.Open(path));

    TFAccountRecord onA;
    TFAccountRecord onB;
    const int accountsCreated = (continentA.CreateAccount("racer", "salt", "hash", onA) ? 1 : 0) +
                                (continentB.CreateAccount("racer", "salt", "hash", onB) ? 1 : 0);
    EXPECT_EQ(accountsCreated, 1);
    TFAccountRecord account;
    ASSERT_TRUE(continentB.FindAccountByUsername("racer", account));

    TFCharacterRecord charA;
    TFCharacterRecord charB;
    const bool createdOnA = continentA.CreateCharacter(account.id, "Racer", FactionId::MRA, charA);
    const bool createdOnB = continentB.CreateCharacter(account.id, "Racer", FactionId::MRA, charB);
    EXPECT_EQ((createdOnA ? 1 : 0) + (createdOnB ? 1 : 0), 1);
    EXPECT_TRUE(continentA.Close());
    EXPECT_TRUE(continentB.Close());

    TFDatabase observer;
    ASSERT_TRUE(observer.Open(path));
    TFAccountRecord seen;
    ASSERT_TRUE(observer.FindAccountByUsername("racer", seen));
    EXPECT_EQ(seen.id, account.id);
    const std::vector<TFCharacterRecord> characters = observer.ListCharacters(account.id);
    ASSERT_EQ(characters.size(), size_t{1});
    EXPECT_TRUE(characters[0].name == "Racer");
    EXPECT_EQ(characters[0].id, createdOnA ? charA.id : charB.id);
    EXPECT_TRUE(observer.Close());
    fs::remove(path);
}

TEST(Persistence_Concurrency_StaleFluxDebitCannotDoubleSpend)
{
    const fs::path path = FreshDbPath("test_data120_double_spend.db");
    uint64_t charId = 0;
    {
        TFDatabase seed;
        ASSERT_TRUE(seed.Open(path));
        charId = SeedCharacter(seed, "spender", "Spender", 10);
        ASSERT_TRUE(charId != 0);
        EXPECT_TRUE(seed.Close());
    }

    TFDatabase continentA;
    TFDatabase continentB;
    ASSERT_TRUE(continentA.Open(path));
    ASSERT_TRUE(continentB.Open(path));
    TFCharacterRecord onA;
    TFCharacterRecord onB;
    ASSERT_TRUE(continentA.AcquireCharacter(charId, onA));
    ASSERT_TRUE(continentB.AcquireCharacter(charId, onB));
    ASSERT_EQ(onA.flux, uint32_t{10});
    ASSERT_EQ(onB.flux, uint32_t{10});

    // Both spend the same 10 flux, each computing the new balance from its own read.
    EXPECT_TRUE(continentA.CommitCharacterUpdates({Purchase(charId, onA.flux - 10, "smg_basic")}));
    const std::string afterFirst = ReadFile(path);
    EXPECT_FALSE(continentB.CommitCharacterUpdates({Purchase(charId, onB.flux - 10, "shotgun_basic")}));
    EXPECT_TRUE(continentB.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_EQ(continentB.ConflictedCharacter(), charId);
    EXPECT_TRUE(ReadFile(path) == afterFirst);

    // The loser re-acquires and sees the spent wallet, so the second purchase can no longer be afforded.
    TFCharacterRecord reacquired;
    ASSERT_TRUE(continentB.AcquireCharacter(charId, reacquired));
    EXPECT_EQ(reacquired.flux, uint32_t{0});
    ASSERT_EQ(reacquired.unlocks.size(), size_t{1});
    EXPECT_TRUE(reacquired.unlocks[0] == "smg_basic");
    EXPECT_TRUE(continentA.Close());
    EXPECT_TRUE(continentB.Close());
    fs::remove(path);
}
