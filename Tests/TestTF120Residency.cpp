/**
 * @file TestTF120Residency.cpp
 * @brief TF-120: a character is in world on at most one continent authority.
 *
 * Every test drives the production TFDatabase (and TFCharacterSystem /
 * TFPlayerMetaStore) against a real file. Two TFDatabase instances bound to
 * different continent keys model two continent authority processes on one
 * TF_SAVE_ROOT: the continent authority lock (SavePaths::ExclusiveFileLock)
 * conflicts between two instances of one process exactly as it does between
 * processes, and destroying an instance releases it the way a process death
 * does. The POSIX-only cases at the end make that real: a peer SparkTests
 * process (TF120PeerProcess.h) holds a continent and is SIGKILLed.
 */
#include "TestFramework.h"
#include "Account/TFCharacterSystem.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFPlayerMeta.h"
#include "Persistence/TFSavePaths.h"
#include "TF120PeerProcess.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Terrafront;

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kCindral = "cindral_wastes";
    constexpr const char* kVeyra = "veyra_highlands";

    fs::path FreshResidencyDb(const char* name)
    {
        const fs::path path = fs::path("Saves") / name;
        fs::create_directories(path.parent_path());
        fs::remove(path);
        fs::remove_all(fs::path(path.wstring() + L".tmp"));
        return path;
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

    struct Seeded
    {
        uint64_t accountId = 0;
        uint64_t charId = 0;
    };

    Seeded SeedCharacter(TFDatabase& db, const char* user, const char* name)
    {
        TFAccountRecord account;
        TFCharacterRecord character;
        if (!db.CreateAccount(user, "salt", "hash", account) ||
            !db.CreateCharacter(account.id, name, FactionId::MRA, character))
            return {};
        return {account.id, character.id};
    }

    /// Residency as an unbound observer (backup tooling, a restarted process) reads it from disk.
    std::string ResidentOnDisk(const fs::path& path, uint64_t charId)
    {
        TFDatabase observer;
        TFCharacterRecord row;
        if (!observer.Open(path) || !observer.FindCharacter(charId, row))
            return "<unreadable>";
        return row.residentContinent;
    }
} // namespace

TEST(TF120_Residency_ClaimIsExclusiveAcrossContinents)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_exclusive.db");
    TFDatabase cindral;
    TFDatabase veyra;
    ASSERT_TRUE(cindral.Open(path) && cindral.BindAuthority(kCindral));
    ASSERT_TRUE(veyra.Open(path) && veyra.BindAuthority(kVeyra));
    const Seeded hero = SeedCharacter(cindral, "exclusive", "Exclusive");
    ASSERT_TRUE(hero.charId != 0);

    TFCharacterRecord row;
    ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, row));
    EXPECT_EQ(row.residentContinent, std::string(kCindral));
    const std::string committed = ReadFile(path);

    // The other live continent is refused and writes nothing.
    TFCharacterRecord refused;
    EXPECT_FALSE(veyra.ClaimCharacter(hero.charId, refused));
    EXPECT_TRUE(veyra.LastStatus() == TFDatabaseStatus::ResidentElsewhere);
    EXPECT_TRUE(ReadFile(path) == committed);
    EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string(kCindral));

    // Without a claim it has no baseline, so it cannot write the character either.
    EXPECT_FALSE(veyra.SaveCharacterProgress(hero.charId, 1, 1, 1, 1));
    EXPECT_TRUE(veyra.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_TRUE(cindral.SaveCharacterProgress(hero.charId, 5, 1, 9, 1));
}

TEST(TF120_Residency_DuplicateClaimIsIdempotent)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_duplicate.db");
    TFDatabase cindral;
    ASSERT_TRUE(cindral.Open(path) && cindral.BindAuthority(kCindral));
    const Seeded hero = SeedCharacter(cindral, "duplicate", "Duplicate");
    ASSERT_TRUE(hero.charId != 0);

    TFCharacterRecord first;
    ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, first));
    const std::string afterFirst = ReadFile(path);

    // A duplicated enter-world message claims nothing new: no write, same row, same baseline.
    TFCharacterRecord second;
    ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, second));
    EXPECT_TRUE(ReadFile(path) == afterFirst);
    EXPECT_EQ(second.revision, first.revision);
    EXPECT_EQ(second.residentContinent, std::string(kCindral));
    ASSERT_TRUE(cindral.BaselineRevision(hero.charId).has_value());
    EXPECT_EQ(*cindral.BaselineRevision(hero.charId), first.revision);

    // A duplicated leave-world message is equally harmless.
    EXPECT_TRUE(cindral.ReleaseCharacter(hero.charId));
    const std::string afterRelease = ReadFile(path);
    EXPECT_TRUE(cindral.ReleaseCharacter(hero.charId));
    EXPECT_TRUE(ReadFile(path) == afterRelease);
    EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string());
}

TEST(TF120_Residency_ReleaseThenOtherContinentClaimsSameProgress)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_handoff.db");
    TFDatabase cindral;
    TFDatabase veyra;
    ASSERT_TRUE(cindral.Open(path) && cindral.BindAuthority(kCindral));
    ASSERT_TRUE(veyra.Open(path) && veyra.BindAuthority(kVeyra));
    const Seeded hero = SeedCharacter(cindral, "handoff", "Handoff");
    ASSERT_TRUE(hero.charId != 0);

    TFCharacterRecord row;
    ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, row));
    ASSERT_TRUE(cindral.SaveCharacterProgress(hero.charId, 250, 2, 60, 7));
    ASSERT_TRUE(cindral.SaveCharacterMeta(hero.charId, {"smg_basic"}, "mra_rifle", "", "", "", "", {}));
    ASSERT_TRUE(cindral.ReleaseCharacter(hero.charId));

    TFCharacterRecord arrived;
    ASSERT_TRUE(veyra.ClaimCharacter(hero.charId, arrived));
    EXPECT_EQ(arrived.xp, uint32_t{250});
    EXPECT_EQ(arrived.flux, uint32_t{60});
    ASSERT_EQ(arrived.unlocks.size(), size_t{1});
    EXPECT_EQ(arrived.unlocks[0], std::string("smg_basic"));
    EXPECT_EQ(arrived.loadoutPrimary, std::string("mra_rifle"));
    EXPECT_TRUE(veyra.SaveCharacterProgress(hero.charId, 260, 2, 61, 8));

    // The continent it left can no longer write it. The values are valid (flux stays under
    // kFluxWalletCap), so only the residency/revision fence can refuse the write.
    EXPECT_FALSE(cindral.SaveCharacterProgress(hero.charId, 999, 2, 99, 9));
    EXPECT_TRUE(cindral.LastStatus() == TFDatabaseStatus::Conflict);
    TFCharacterRecord durable;
    ASSERT_TRUE(veyra.FindCharacter(hero.charId, durable));
    EXPECT_EQ(durable.xp, uint32_t{260});
}

TEST(TF120_Residency_SecondAuthoritySameContinentFailsToBind)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_bind.db");
    TFDatabase first;
    ASSERT_TRUE(first.Open(path) && first.BindAuthority(kCindral));
    EXPECT_EQ(first.BoundContinent(), std::string(kCindral));

    TFDatabase second;
    ASSERT_TRUE(second.Open(path));
    EXPECT_FALSE(second.BindAuthority(kCindral));
    EXPECT_TRUE(second.LastStatus() == TFDatabaseStatus::AuthorityHeld);
    EXPECT_TRUE(second.BoundContinent().empty());

    // Binding is once per instance, and keys must name a valid lock file.
    EXPECT_FALSE(first.BindAuthority(kVeyra));
    TFDatabase invalid;
    ASSERT_TRUE(invalid.Open(path));
    EXPECT_FALSE(invalid.BindAuthority("../escape"));
    EXPECT_FALSE(invalid.BindAuthority(""));

    // Once the first authority is gone, the continent can be served again.
    EXPECT_TRUE(first.Close());
    EXPECT_TRUE(second.BindAuthority(kCindral));
}

TEST(TF120_Residency_RebindRecoversOwnStaleResidency)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_rebind.db");
    Seeded hero;
    {
        // An authority that dies with a character in world (no release).
        TFDatabase crashed;
        ASSERT_TRUE(crashed.Open(path) && crashed.BindAuthority(kCindral));
        hero = SeedCharacter(crashed, "rebind", "Rebind");
        ASSERT_TRUE(hero.charId != 0);
        TFCharacterRecord row;
        ASSERT_TRUE(crashed.ClaimCharacter(hero.charId, row));
    }
    EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string(kCindral));

    // Its restart holds the continent lock, so every row it left resident is stale and cleared.
    TFDatabase restarted;
    ASSERT_TRUE(restarted.Open(path) && restarted.BindAuthority(kCindral));
    EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string());
    TFCharacterRecord row;
    ASSERT_TRUE(restarted.ClaimCharacter(hero.charId, row));
    EXPECT_EQ(row.residentContinent, std::string(kCindral));
}

TEST(TF120_Residency_DeadOwnerResidencyIsReclaimed)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_dead_owner.db");
    Seeded hero;
    {
        TFDatabase owner;
        ASSERT_TRUE(owner.Open(path) && owner.BindAuthority(kCindral));
        hero = SeedCharacter(owner, "deadowner", "Dead Owner");
        ASSERT_TRUE(hero.charId != 0);
        TFCharacterRecord row;
        ASSERT_TRUE(owner.ClaimCharacter(hero.charId, row));
        ASSERT_TRUE(owner.SaveCharacterProgress(hero.charId, 77, 1, 33, 5));
        // Destroyed without ReleaseCharacter: its authority lock is freed, its residency is not.
    }
    EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string(kCindral));

    TFDatabase veyra;
    ASSERT_TRUE(veyra.Open(path) && veyra.BindAuthority(kVeyra));
    TFCharacterRecord taken;
    ASSERT_TRUE(veyra.ClaimCharacter(hero.charId, taken));
    EXPECT_EQ(taken.residentContinent, std::string(kVeyra));
    EXPECT_EQ(taken.xp, uint32_t{77}); // the dead owner's last commit, not a stale copy
    EXPECT_EQ(taken.flux, uint32_t{33});
    EXPECT_TRUE(veyra.SaveCharacterProgress(hero.charId, 78, 1, 34, 6));
    EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string(kVeyra));
}

TEST(TF120_Residency_FencedOutAuthorityCommitConflicts)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_fenced.db");
    TFDatabase cindral;
    TFDatabase veyra;
    ASSERT_TRUE(cindral.Open(path) && cindral.BindAuthority(kCindral));
    ASSERT_TRUE(veyra.Open(path) && veyra.BindAuthority(kVeyra));
    const Seeded hero = SeedCharacter(cindral, "fenced", "Fenced");
    ASSERT_TRUE(hero.charId != 0);

    TFCharacterRecord row;
    ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, row));
    ASSERT_TRUE(cindral.ReleaseCharacter(hero.charId));
    ASSERT_TRUE(veyra.ClaimCharacter(hero.charId, row));

    // Even with a baseline at the current revision (a plain AcquireCharacter), a bound authority may not
    // write a character that is in world on another continent: the residency fence, not only the revision.
    TFCharacterRecord current;
    ASSERT_TRUE(cindral.AcquireCharacter(hero.charId, current));
    EXPECT_EQ(current.residentContinent, std::string(kVeyra));
    EXPECT_FALSE(cindral.SaveCharacterProgress(hero.charId, 500, 1, 500, 1));
    EXPECT_TRUE(cindral.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_EQ(cindral.ConflictedCharacter(), hero.charId);

    // An unbound instance (tooling) is not fenced: it keeps the revision check only.
    TFDatabase tooling;
    ASSERT_TRUE(tooling.Open(path));
    ASSERT_TRUE(tooling.AcquireCharacter(hero.charId, current));
    EXPECT_TRUE(tooling.SaveCharacterProgress(hero.charId, current.xp, current.rank, current.flux + 1, 2));
    EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string(kVeyra));
}

TEST(TF120_Residency_V2FileLoadsWithoutResidencyAndUpgradesToV4)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_v2.db");
    WriteFile(path, R"({"schemaVersion": 2, "revision": 3, "nextAccountId": 2, "nextCharId": 2,
        "accounts": [{"id": 1, "username": "old", "salt": "s", "passwordHash": "h",
                      "createdAtMs": 1, "lastLoginMs": 0}],
        "characters": [{"id": 1, "accountId": 1, "name": "Old Timer", "faction": 1, "xp": 7,
                        "rank": 1, "flux": 3, "createdAtMs": 1, "lastPlayedMs": 0, "revision": 3}]})");

    TFDatabase cindral;
    ASSERT_TRUE(cindral.Open(path));
    TFCharacterRecord row;
    ASSERT_TRUE(cindral.FindCharacter(1, row));
    EXPECT_TRUE(row.residentContinent.empty());
    ASSERT_TRUE(cindral.BindAuthority(kCindral));
    ASSERT_TRUE(cindral.ClaimCharacter(1, row));
    EXPECT_EQ(row.xp, uint32_t{7});

    const std::string upgraded = ReadFile(path);
    EXPECT_STR_CONTAINS(upgraded, "\"schemaVersion\": " + std::to_string(TFDatabase::kSchemaVersion));
    EXPECT_STR_CONTAINS(upgraded, std::string("\"resident\": \"") + kCindral + "\"");
    EXPECT_EQ(TFDatabase::kSchemaVersion, uint32_t{4});

    // A resident key that could not name a lock file is corruption.
    WriteFile(path, R"({"schemaVersion": 4, "revision": 3, "nextAccountId": 2, "nextCharId": 2,
        "accounts": [{"id": 1, "username": "old", "salt": "s", "passwordHash": "h",
                      "createdAtMs": 1, "lastLoginMs": 0}],
        "characters": [{"id": 1, "accountId": 1, "name": "Old Timer", "faction": 1, "xp": 7,
                        "rank": 1, "flux": 3, "createdAtMs": 1, "lastPlayedMs": 0, "revision": 3,
                        "resident": "../escape"}]})");
    TFDatabase corrupt;
    EXPECT_FALSE(corrupt.Open(path));
    EXPECT_TRUE(corrupt.LastStatus() == TFDatabaseStatus::Corrupt);
    for (const auto& entry : fs::directory_iterator(path.parent_path()))
        if (entry.path().filename().string().starts_with("test_tf120_residency_v2.db.corrupt-"))
            fs::remove(entry.path());
}

// ---------------------------------------------------------------------------
// Enter-world gate: TFCharacterSystem claims on EnterWorld and releases on
// LeaveWorld; a disconnect flush that parks meta keeps the residency.
// ---------------------------------------------------------------------------

TEST(TF120_Residency_EnterWorldRefusedWhileResidentOnOtherContinent)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_enter_refused.db");
    TFDatabase cindralDb;
    TFDatabase veyraDb;
    ASSERT_TRUE(cindralDb.Open(path) && cindralDb.BindAuthority(kCindral));
    ASSERT_TRUE(veyraDb.Open(path) && veyraDb.BindAuthority(kVeyra));
    TFCharacterSystem cindral;
    TFCharacterSystem veyra;
    cindral.SetDatabase(&cindralDb);
    veyra.SetDatabase(&veyraDb);
    const Seeded hero = SeedCharacter(cindralDb, "enterrefused", "Enter Refused");
    ASSERT_TRUE(hero.charId != 0);

    TFCharacterRecord rec;
    ASSERT_TRUE(cindral.EnterWorld(hero.accountId, hero.charId, rec));
    EXPECT_FALSE(veyra.EnterWorld(hero.accountId, hero.charId, rec));
    EXPECT_TRUE(veyraDb.LastStatus() == TFDatabaseStatus::ResidentElsewhere);

    // Ownership is still checked first: another account cannot claim it on its own continent either.
    EXPECT_FALSE(cindral.EnterWorld(hero.accountId + 1, hero.charId, rec));
    EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string(kCindral));
}

TEST(TF120_Residency_EnterWorldAfterReleaseCarriesProgress)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_enter_carry.db");
    TFDatabase cindralDb;
    TFDatabase veyraDb;
    ASSERT_TRUE(cindralDb.Open(path) && cindralDb.BindAuthority(kCindral));
    ASSERT_TRUE(veyraDb.Open(path) && veyraDb.BindAuthority(kVeyra));
    TFCharacterSystem cindral;
    TFCharacterSystem veyra;
    cindral.SetDatabase(&cindralDb);
    veyra.SetDatabase(&veyraDb);
    const Seeded hero = SeedCharacter(cindralDb, "entercarry", "Enter Carry");
    ASSERT_TRUE(hero.charId != 0);

    TFCharacterRecord rec;
    ASSERT_TRUE(cindral.EnterWorld(hero.accountId, hero.charId, rec));
    ASSERT_TRUE(cindral.PersistProgress(hero.charId, 1200, 3, 45));
    ASSERT_TRUE(cindral.LeaveWorld(hero.charId));
    EXPECT_TRUE(cindral.LeaveWorld(hero.charId)); // a duplicated leave is harmless

    ASSERT_TRUE(veyra.EnterWorld(hero.accountId, hero.charId, rec));
    EXPECT_EQ(rec.xp, uint32_t{1200});
    EXPECT_EQ(rec.rank, uint16_t{3});
    EXPECT_EQ(rec.flux, uint32_t{45});
    EXPECT_EQ(rec.residentContinent, std::string(kVeyra));
    EXPECT_TRUE(veyra.PersistProgress(hero.charId, 1300, 3, 46));
    EXPECT_FALSE(cindral.PersistProgress(hero.charId, 1, 1, 1));
}

TEST(TF120_Residency_ParkedRowKeepsResidencyUntilSweepResolvesIt)
{
    const fs::path path = FreshResidencyDb("test_tf120_residency_parked.db");
    TFDatabase cindralDb;
    TFDatabase veyraDb;
    ASSERT_TRUE(cindralDb.Open(path) && cindralDb.BindAuthority(kCindral));
    ASSERT_TRUE(veyraDb.Open(path) && veyraDb.BindAuthority(kVeyra));
    TFCharacterSystem cindral;
    TFCharacterSystem veyra;
    cindral.SetDatabase(&cindralDb);
    veyra.SetDatabase(&veyraDb);
    const Seeded hero = SeedCharacter(cindralDb, "parked", "Parked Hero");
    ASSERT_TRUE(hero.charId != 0);

    TFPlayerMetaStore store;
    TFCharacterRecord rec;
    ASSERT_TRUE(cindral.EnterWorld(hero.accountId, hero.charId, rec));
    store.SeedFromRecord(PlayerId{1}, rec);
    store.Ensure(PlayerId{1}).unlocks.insert("parked_unlock");
    store.Ensure(PlayerId{1}).dirty = true;

    // The disconnect flush of the meta fails, so the row is parked and the character must stay resident:
    // releasing it now would let the other continent start without the unlock.
    const fs::path staging(path.wstring() + L".tmp");
    ASSERT_TRUE(fs::create_directory(staging));
    EXPECT_FALSE(store.Detach(PlayerId{1}, &cindralDb));
    fs::remove_all(staging);
    EXPECT_TRUE(store.IsParked(hero.charId));
    EXPECT_TRUE(store.TakeResolvedParked().empty());
    EXPECT_FALSE(veyra.EnterWorld(hero.accountId, hero.charId, rec));
    EXPECT_TRUE(veyraDb.LastStatus() == TFDatabaseStatus::ResidentElsewhere);

    // The next sweep commits the parked row and reports it resolved; releasing it (TFProgressionSystem::
    // SaveNow does exactly this) hands the character over with the unlock.
    EXPECT_TRUE(store.PersistAllDirty(cindralDb));
    EXPECT_FALSE(store.IsParked(hero.charId));
    const std::vector<uint64_t> resolved = store.TakeResolvedParked();
    ASSERT_EQ(resolved.size(), size_t{1});
    EXPECT_EQ(resolved[0], hero.charId);
    EXPECT_TRUE(store.TakeResolvedParked().empty());
    ASSERT_TRUE(cindral.LeaveWorld(resolved[0]));

    ASSERT_TRUE(veyra.EnterWorld(hero.accountId, hero.charId, rec));
    EXPECT_TRUE(std::find(rec.unlocks.begin(), rec.unlocks.end(), "parked_unlock") != rec.unlocks.end());
}

// ---------------------------------------------------------------------------
// Forced source/destination crashes (POSIX): a peer SparkTests process is the
// veyra_highlands authority and is SIGKILLed; this process is cindral_wastes.
// ---------------------------------------------------------------------------

#ifndef _WIN32
namespace
{
    constexpr const char* kPeerReady = "TF120_PEER_READY=";

    /// Stay alive (holding the continent) until the coordinator kills this process; bounded so a lost
    /// coordinator cannot leave it running forever.
    void HoldUntilKilled()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    /// Peer role "resident": the veyra authority claims the character, commits xp = the tag, reports ready
    /// and stays in world until it is killed.
    void RunResidentPeer()
    {
        const fs::path path = TF120Peer::EnvOrEmpty(TF120Peer::kDbEnv);
        const uint64_t charId = std::strtoull(TF120Peer::EnvOrEmpty(TF120Peer::kCharEnv).c_str(), nullptr, 10);
        const auto xp =
            static_cast<uint32_t>(std::strtoul(TF120Peer::EnvOrEmpty(TF120Peer::kTagEnv).c_str(), nullptr, 10));
        TFDatabase peer;
        ASSERT_TRUE(charId != 0 && peer.Open(path) && peer.BindAuthority(kVeyra));
        TFCharacterRecord row;
        ASSERT_TRUE(peer.ClaimCharacter(charId, row));
        ASSERT_TRUE(peer.SaveCharacterProgress(charId, xp, row.rank, row.flux, 1));
        std::printf("%s1\n", kPeerReady);
        std::fflush(stdout);
        HoldUntilKilled();
    }

    /// Peer role "claim-hammer": claim, commit, release as fast as possible until killed, so the kill lands
    /// inside a claim or release transaction.
    void RunClaimHammerPeer()
    {
        const fs::path path = TF120Peer::EnvOrEmpty(TF120Peer::kDbEnv);
        const uint64_t charId = std::strtoull(TF120Peer::EnvOrEmpty(TF120Peer::kCharEnv).c_str(), nullptr, 10);
        TFDatabase peer;
        ASSERT_TRUE(charId != 0 && peer.Open(path) && peer.BindAuthority(kVeyra));
        for (bool announced = false;;)
        {
            TFCharacterRecord row;
            ASSERT_TRUE(peer.ClaimCharacter(charId, row));
            ASSERT_TRUE(peer.SaveCharacterProgress(charId, row.xp + 1, row.rank, row.flux, 1));
            ASSERT_TRUE(peer.ReleaseCharacter(charId));
            if (!announced)
            {
                std::printf("%s1\n", kPeerReady);
                std::fflush(stdout);
                announced = true;
            }
        }
    }

    std::expected<Spark::Process, std::string> SpawnResidencyPeer(const char* testName, const char* role,
                                                                  const fs::path& path, uint64_t charId,
                                                                  uint32_t tag = 0)
    {
        return TF120Peer::SpawnPeer(testName, {std::string(TF120Peer::kRoleEnv) + "=" + role,
                                               std::string(TF120Peer::kDbEnv) + "=" + path.string(),
                                               std::string(TF120Peer::kCharEnv) + "=" + std::to_string(charId),
                                               std::string(TF120Peer::kTagEnv) + "=" + std::to_string(tag)});
    }

    bool PeerIsReady(Spark::Process& peer)
    {
        std::string log;
        const bool ready = TF120Peer::WaitPeerReady(peer, log, kPeerReady, std::chrono::seconds(60));
        if (!ready)
            std::fprintf(stderr, "---- TF120 residency peer output ----\n%s\n----\n", log.c_str());
        return ready;
    }
} // namespace

TEST(TF120_Residency_SourceKilledWhileResidentIsReclaimedWithLastCommit)
{
    if (TF120Peer::EnvOrEmpty(TF120Peer::kRoleEnv) == "resident")
    {
        RunResidentPeer();
        return;
    }

    const fs::path path = fs::absolute(FreshResidencyDb("test_tf120_residency_kill_source.db"));
    TFDatabase cindral;
    ASSERT_TRUE(cindral.Open(path) && cindral.BindAuthority(kCindral));
    const Seeded hero = SeedCharacter(cindral, "killsource", "Kill Source");
    ASSERT_TRUE(hero.charId != 0);

    auto launched = SpawnResidencyPeer("TF120_Residency_SourceKilledWhileResidentIsReclaimedWithLastCommit", "resident",
                                       path, hero.charId, 321);
    if (!launched)
        SKIP_TEST("cannot spawn a peer test process: " + launched.error());
    Spark::Process& source = *launched;
    ASSERT_TRUE(PeerIsReady(source));

    // While the source process lives, the character cannot enter world here.
    TFCharacterRecord row;
    EXPECT_FALSE(cindral.ClaimCharacter(hero.charId, row));
    EXPECT_TRUE(cindral.LastStatus() == TFDatabaseStatus::ResidentElsewhere);

    // The source dies without releasing; the character comes back with its last committed state.
    source.Kill();
    EXPECT_FALSE(source.IsRunning());
    ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, row));
    EXPECT_EQ(row.xp, uint32_t{321});
    EXPECT_EQ(row.residentContinent, std::string(kCindral));
    EXPECT_TRUE(cindral.SaveCharacterProgress(hero.charId, 322, row.rank, row.flux, 2));
}

TEST(TF120_Residency_DestinationKilledAfterClaimSourceReclaims)
{
    if (TF120Peer::EnvOrEmpty(TF120Peer::kRoleEnv) == "resident")
    {
        RunResidentPeer();
        return;
    }

    const fs::path path = fs::absolute(FreshResidencyDb("test_tf120_residency_kill_destination.db"));
    TFDatabase cindral;
    ASSERT_TRUE(cindral.Open(path) && cindral.BindAuthority(kCindral));
    const Seeded hero = SeedCharacter(cindral, "killdest", "Kill Destination");
    ASSERT_TRUE(hero.charId != 0);

    // The source hands the character over: final commit, then release.
    TFCharacterRecord row;
    ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, row));
    ASSERT_TRUE(cindral.SaveCharacterProgress(hero.charId, 10, row.rank, row.flux, 1));
    ASSERT_TRUE(cindral.ReleaseCharacter(hero.charId));

    auto launched = SpawnResidencyPeer("TF120_Residency_DestinationKilledAfterClaimSourceReclaims", "resident", path,
                                       hero.charId, 20);
    if (!launched)
        SKIP_TEST("cannot spawn a peer test process: " + launched.error());
    Spark::Process& destination = *launched;
    ASSERT_TRUE(PeerIsReady(destination));
    EXPECT_FALSE(cindral.ClaimCharacter(hero.charId, row));
    EXPECT_TRUE(cindral.LastStatus() == TFDatabaseStatus::ResidentElsewhere);

    // The destination crashes after its claim and one commit; the source takes the character back with the
    // destination's commit, neither duplicated nor lost.
    destination.Kill();
    EXPECT_FALSE(destination.IsRunning());
    ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, row));
    EXPECT_EQ(row.xp, uint32_t{20});
    EXPECT_TRUE(cindral.SaveCharacterProgress(hero.charId, 21, row.rank, row.flux, 3));
}

TEST(TF120_Residency_PeerKilledMidClaimLeavesExactlyOneValidResident)
{
    if (TF120Peer::EnvOrEmpty(TF120Peer::kRoleEnv) == "claim-hammer")
    {
        RunClaimHammerPeer();
        return;
    }

    const fs::path path = fs::absolute(FreshResidencyDb("test_tf120_residency_kill_hammer.db"));
    TFDatabase cindral;
    ASSERT_TRUE(cindral.Open(path) && cindral.BindAuthority(kCindral));
    const Seeded hero = SeedCharacter(cindral, "killhammer", "Kill Hammer");
    ASSERT_TRUE(hero.charId != 0);

    uint32_t lastXp = 0;
    for (int kill = 0; kill < 6; ++kill)
    {
        auto launched = SpawnResidencyPeer("TF120_Residency_PeerKilledMidClaimLeavesExactlyOneValidResident",
                                           "claim-hammer", path, hero.charId);
        if (!launched)
            SKIP_TEST("cannot spawn a peer test process: " + launched.error());
        Spark::Process& peer = *launched;
        ASSERT_TRUE(PeerIsReady(peer));
        std::this_thread::sleep_for(std::chrono::milliseconds(15 + 7 * kill));
        peer.Kill();
        EXPECT_FALSE(peer.IsRunning());

        // The file loads, and the character is out of world or resident on the dead continent only.
        const std::string resident = ResidentOnDisk(path, hero.charId);
        EXPECT_TRUE(resident.empty() || resident == kVeyra);

        // A restart of the killed continent clears its own rows before serving anyone.
        {
            TFDatabase restarted;
            ASSERT_TRUE(restarted.Open(path) && restarted.BindAuthority(kVeyra));
            EXPECT_EQ(ResidentOnDisk(path, hero.charId), std::string());
        }

        // The survivor claims the character with every commit the peer acknowledged, then hands it back.
        TFCharacterRecord row;
        ASSERT_TRUE(cindral.ClaimCharacter(hero.charId, row));
        EXPECT_TRUE(row.xp > lastXp); // the peer committed at least once
        ASSERT_TRUE(cindral.SaveCharacterProgress(hero.charId, row.xp + 1, row.rank, row.flux, 2));
        lastXp = row.xp + 1;
        ASSERT_TRUE(cindral.ReleaseCharacter(hero.charId));
    }

    TFCharacterRecord durable;
    ASSERT_TRUE(cindral.FindCharacter(hero.charId, durable));
    EXPECT_EQ(durable.xp, lastXp);
    EXPECT_TRUE(durable.residentContinent.empty());
}
#endif
