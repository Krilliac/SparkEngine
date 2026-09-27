/**
 * @file TestTF120SharedSaveRoot.cpp
 * @brief TF-120: two continent authority processes sharing one TF_SAVE_ROOT
 *        account/character store must not lose updates, must reject stale
 *        absolute writes, and must survive a peer crashing mid-transaction.
 *
 * Every test drives the production TFDatabase against a real file. The
 * multi-process cases (POSIX) spawn fresh SparkTests processes as peer
 * authorities; each opens its own TFDatabase exactly as a second continent
 * server would. Peers are exec'd rather than bare fork()s of this runner: the
 * suite's other threads (async logger, job workers) may hold locks at fork
 * time, and a forked child that logs or allocates would then deadlock.
 */
#include "TestFramework.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFPlayerMeta.h"
#include "Persistence/TFSavePaths.h"
#include "Utils/Process.h"

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

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

using namespace Terrafront;

namespace
{
    namespace fs = std::filesystem;

    fs::path FreshSharedDb(const char* name)
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

    uint64_t SeedCharacter(TFDatabase& db, const char* user, const char* name)
    {
        TFAccountRecord account;
        TFCharacterRecord character;
        if (!db.CreateAccount(user, "salt", "hash", account))
            return 0;
        if (!db.CreateCharacter(account.id, name, FactionId::MRA, character))
            return 0;
        return character.id;
    }

    /// Read-modify-write one character the way a continent authority does:
    /// acquire the row, compute absolute values from it, commit, and retry
    /// from a fresh acquire when another authority won the race.
    enum class RmwResult
    {
        Committed,
        Failed,
    };

    RmwResult AddFluxWithRetry(TFDatabase& db, uint64_t charId, int& conflicts, std::chrono::microseconds think)
    {
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            TFCharacterRecord row;
            if (!db.AcquireCharacter(charId, row))
                return RmwResult::Failed;
            std::this_thread::sleep_for(think); // gameplay time between load and save
            if (db.SaveCharacterProgress(charId, row.xp + 1, row.rank, row.flux + 1, 1))
                return RmwResult::Committed;
            if (db.LastStatus() != TFDatabaseStatus::Conflict)
                return RmwResult::Failed;
            ++conflicts;
        }
        return RmwResult::Failed;
    }

    RmwResult AddUnlockWithRetry(TFDatabase& db, uint64_t charId, const std::string& key, int& conflicts,
                                 std::chrono::microseconds think)
    {
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            TFCharacterRecord row;
            if (!db.AcquireCharacter(charId, row))
                return RmwResult::Failed;
            std::this_thread::sleep_for(think); // gameplay time between load and save
            std::vector<std::string> unlocks = row.unlocks;
            unlocks.push_back(key);
            if (db.SaveCharacterMeta(charId, unlocks, row.loadoutPrimary, row.loadoutSecondary, row.loadoutTool,
                                     row.loadoutGrenade, row.loadoutSuit, row.weaponStats))
                return RmwResult::Committed;
            if (db.LastStatus() != TFDatabaseStatus::Conflict)
                return RmwResult::Failed;
            ++conflicts;
        }
        return RmwResult::Failed;
    }

#ifndef _WIN32
    constexpr const char* kPeerRoleEnv = "SPARK_TF120_PEER_ROLE";
    constexpr const char* kPeerDbEnv = "SPARK_TF120_PEER_DB";
    constexpr const char* kPeerCharEnv = "SPARK_TF120_PEER_CHAR";
    constexpr const char* kPeerTagEnv = "SPARK_TF120_PEER_TAG";
    constexpr const char* kPeerGateEnv = "SPARK_TF120_PEER_GATE";

    std::string EnvOrEmpty(const char* name)
    {
        const char* value = std::getenv(name);
        return value ? value : "";
    }

    fs::path TestBinaryPath()
    {
#ifdef __APPLE__
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            return {};
        return fs::path(buffer.c_str());
#else
        std::error_code error;
        const fs::path self = fs::read_symlink("/proc/self/exe", error);
        return error ? fs::path{} : self;
#endif
    }

    /// Launch a fresh SparkTests process that runs only `testName`. That test
    /// sees `SPARK_TF120_PEER_ROLE` in its environment and plays the peer
    /// authority instead of the coordinator.
    std::expected<Spark::Process, std::string> SpawnPeer(const char* testName, const std::vector<std::string>& settings)
    {
        const fs::path self = TestBinaryPath();
        if (self.empty())
            return std::unexpected(std::string("cannot resolve the test binary path"));

        Spark::Process::Builder builder("env");
        // Drop the parent's test selection so the peer runs exactly one test.
        for (const char* selection :
             {"SPARK_TEST_FILE", "SPARK_TEST_EXPECT_COUNT", "SPARK_TEST_EXCLUDE", "SPARK_TEST_LIMIT"})
        {
            builder.Arg("-u").Arg(selection);
        }
        builder.Arg(std::string("SPARK_TEST_NAME=") + testName);
        for (const std::string& setting : settings)
            builder.Arg(setting);
        builder.Arg(self.string())
            .WorkingDirectory(fs::current_path().string())
            .CaptureStdout()
            .MergeStderrIntoStdout();
        return builder.Launch();
    }

    void DrainPeer(Spark::Process& peer, std::string& log)
    {
        std::string line;
        while (peer.TryReadLine(line))
        {
            log += line;
            log += '\n';
        }
    }

    /// Wait for every peer while draining its output (so a full pipe never
    /// stalls it), killing any still running at the deadline. Returns each
    /// peer's exit code, -1 when it was killed or died on a signal.
    std::vector<int> WaitPeers(std::vector<Spark::Process>& peers, std::vector<std::string>& logs,
                               std::chrono::seconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::vector<int> codes(peers.size(), -1);
        std::vector<bool> finished(peers.size(), false);
        size_t remaining = peers.size();
        while (remaining > 0)
        {
            for (size_t i = 0; i < peers.size(); ++i)
            {
                if (finished[i])
                    continue;
                DrainPeer(peers[i], logs[i]);
                if (const std::optional<int> code = peers[i].GetExitCode())
                {
                    logs[i] += peers[i].ReadAllStdout();
                    codes[i] = *code;
                    finished[i] = true;
                    --remaining;
                }
            }
            if (remaining == 0)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                for (size_t i = 0; i < peers.size(); ++i)
                    if (!finished[i])
                        peers[i].Kill();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return codes;
    }

    /// Value after `key` on the peer's report line, or -1 when absent.
    long long PeerReport(const std::string& log, const std::string& key)
    {
        const size_t at = log.find(key);
        if (at == std::string::npos)
            return -1;
        return std::atoll(log.c_str() + at + key.size());
    }
#endif
} // namespace

TEST(TF120_SharedRoot_TwoInstancesSeeEachOthersCommits)
{
    const fs::path path = FreshSharedDb("test_tf120_shared_visibility.db");
    TFDatabase continentA;
    TFDatabase continentB;
    ASSERT_TRUE(continentA.Open(path));
    ASSERT_TRUE(continentB.Open(path));

    // Registration on one continent is visible to login on the other.
    TFAccountRecord alpha;
    ASSERT_TRUE(continentA.CreateAccount("alpha", "salt", "hash", alpha));
    TFAccountRecord seen;
    ASSERT_TRUE(continentB.FindAccountByUsername("alpha", seen));
    EXPECT_EQ(seen.id, alpha.id);

    // Uniqueness and id allocation span both authorities.
    TFAccountRecord duplicate;
    EXPECT_FALSE(continentB.CreateAccount("alpha", "salt", "hash", duplicate));
    TFAccountRecord beta;
    ASSERT_TRUE(continentB.CreateAccount("beta", "salt", "hash", beta));
    EXPECT_NE(beta.id, alpha.id);

    TFCharacterRecord alphaChar;
    TFCharacterRecord betaChar;
    ASSERT_TRUE(continentA.CreateCharacter(alpha.id, "Alpha One", FactionId::MRA, alphaChar));
    ASSERT_TRUE(continentB.CreateCharacter(beta.id, "Beta One", FactionId::MRA, betaChar));
    EXPECT_NE(alphaChar.id, betaChar.id);
    TFCharacterRecord taken;
    EXPECT_FALSE(continentB.CreateCharacter(beta.id, "Alpha One", FactionId::MRA, taken));

    EXPECT_EQ(continentA.ListCharacters(beta.id).size(), size_t{1});
    EXPECT_TRUE(continentA.TouchLogin(beta.id, 1234));
    ASSERT_TRUE(continentB.FindAccountByUsername("beta", seen));
    EXPECT_EQ(seen.lastLoginMs, int64_t{1234});

    // A delete on one continent is honoured by the other.
    EXPECT_TRUE(continentB.DeleteCharacter(alphaChar.id));
    TFCharacterRecord gone;
    EXPECT_FALSE(continentA.FindCharacter(alphaChar.id, gone));

    EXPECT_TRUE(continentA.Close());
    EXPECT_TRUE(continentB.Close());
}

TEST(TF120_SharedRoot_StaleAbsoluteWriteIsRejectedThenRetried)
{
    const fs::path path = FreshSharedDb("test_tf120_conflict.db");
    TFDatabase continentA;
    TFDatabase continentB;
    ASSERT_TRUE(continentA.Open(path));
    const uint64_t charId = SeedCharacter(continentA, "hopper", "Hopper");
    ASSERT_TRUE(charId != 0);
    ASSERT_TRUE(continentB.Open(path));

    TFCharacterRecord onA;
    TFCharacterRecord onB;
    ASSERT_TRUE(continentA.AcquireCharacter(charId, onA));
    ASSERT_TRUE(continentB.AcquireCharacter(charId, onB));

    // A commits first; B's values were computed from the row A replaced.
    EXPECT_TRUE(continentA.SaveCharacterProgress(charId, 10, 1, 100, 1));
    EXPECT_FALSE(continentB.SaveCharacterProgress(charId, 5, 1, 50, 2));
    EXPECT_TRUE(continentB.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_TRUE(continentB.IsOpen());

    {
        TFDatabase observer;
        ASSERT_TRUE(observer.Open(path));
        TFCharacterRecord durable;
        ASSERT_TRUE(observer.FindCharacter(charId, durable));
        EXPECT_EQ(durable.flux, uint32_t{100});
        EXPECT_EQ(durable.xp, uint32_t{10});
    }

    // A plain read does not move the baseline, so B still cannot commit
    // values derived from its stale session state.
    TFCharacterRecord peek;
    ASSERT_TRUE(continentB.FindCharacter(charId, peek));
    EXPECT_FALSE(continentB.SaveCharacterProgress(charId, 5, 1, 50, 2));
    EXPECT_TRUE(continentB.LastStatus() == TFDatabaseStatus::Conflict);

    // Retry: re-acquire, recompute from the committed row, commit.
    ASSERT_TRUE(continentB.AcquireCharacter(charId, onB));
    EXPECT_EQ(onB.flux, uint32_t{100});
    EXPECT_TRUE(continentB.SaveCharacterProgress(charId, onB.xp + 5, 1, onB.flux + 50, 2));

    // A's meta write is now stale in turn; after re-acquiring it lands on top
    // of B's progress without erasing it.
    EXPECT_FALSE(continentA.SaveCharacterMeta(charId, {"smg_basic"}, "", "", "", "", "", {}));
    EXPECT_TRUE(continentA.LastStatus() == TFDatabaseStatus::Conflict);
    ASSERT_TRUE(continentA.AcquireCharacter(charId, onA));
    EXPECT_TRUE(continentA.SaveCharacterMeta(charId, {"smg_basic"}, "", "", "", "", "", {}));

    // Own successive commits never conflict with themselves.
    EXPECT_TRUE(continentA.SaveCharacterProgress(charId, onA.xp, 1, onA.flux + 1, 3));

    // A character another authority created after Open has no baseline here
    // until it is acquired.
    const uint64_t lateChar = SeedCharacter(continentA, "late", "Late Comer");
    ASSERT_TRUE(lateChar != 0);
    EXPECT_FALSE(continentB.SaveCharacterProgress(lateChar, 1, 1, 1, 1));
    EXPECT_TRUE(continentB.LastStatus() == TFDatabaseStatus::Conflict);

    TFDatabase observer;
    ASSERT_TRUE(observer.Open(path));
    TFCharacterRecord durable;
    ASSERT_TRUE(observer.FindCharacter(charId, durable));
    EXPECT_EQ(durable.flux, uint32_t{151});
    EXPECT_EQ(durable.xp, uint32_t{15});
    ASSERT_EQ(durable.unlocks.size(), size_t{1});
    EXPECT_EQ(durable.unlocks[0], std::string("smg_basic"));
}

TEST(TF120_SharedRoot_LegacyFileUpgradesAndRevisionsAreValidated)
{
    const fs::path path = FreshSharedDb("test_tf120_legacy.db");
    WriteFile(path, R"({"schemaVersion": 1, "nextAccountId": 2, "nextCharId": 2,
        "accounts": [{"id": 1, "username": "old", "salt": "s", "passwordHash": "h",
                      "createdAtMs": 1, "lastLoginMs": 0}],
        "characters": [{"id": 1, "accountId": 1, "name": "Old Timer", "faction": 1, "xp": 7,
                        "rank": 1, "flux": 3, "createdAtMs": 1, "lastPlayedMs": 0}]})");
    {
        TFDatabase db;
        ASSERT_TRUE(db.Open(path));
        TFCharacterRecord row;
        ASSERT_TRUE(db.FindCharacter(1, row));
        EXPECT_EQ(row.revision, uint64_t{0});
        EXPECT_TRUE(db.SaveCharacterProgress(1, 8, 1, 3, 1));
        ASSERT_TRUE(db.FindCharacter(1, row));
        EXPECT_EQ(row.revision, uint64_t{1});
    }
    const std::string upgraded = ReadFile(path);
    EXPECT_TRUE(upgraded.find("\"schemaVersion\": 2") != std::string::npos);
    EXPECT_TRUE(upgraded.find("\"revision\": 1") != std::string::npos);

    // A row claiming a revision newer than its file is corrupt, not trusted.
    WriteFile(path, R"({"schemaVersion": 2, "revision": 1, "nextAccountId": 2, "nextCharId": 2,
        "accounts": [{"id": 1, "username": "old", "salt": "s", "passwordHash": "h",
                      "createdAtMs": 1, "lastLoginMs": 0}],
        "characters": [{"id": 1, "accountId": 1, "name": "Old Timer", "faction": 1, "xp": 7,
                        "rank": 1, "flux": 3, "createdAtMs": 1, "lastPlayedMs": 0, "revision": 9}]})");
    TFDatabase corrupt;
    EXPECT_FALSE(corrupt.Open(path));
    EXPECT_TRUE(corrupt.LastStatus() == TFDatabaseStatus::Corrupt);
    for (const auto& entry : fs::directory_iterator(path.parent_path()))
        if (entry.path().filename().string().starts_with("test_tf120_legacy.db.corrupt-"))
            fs::remove(entry.path());
}

TEST(TF120_SharedRoot_VanishedOrRolledBackPrimaryFailsClosed)
{
    const fs::path path = FreshSharedDb("test_tf120_vanished.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    ASSERT_TRUE(SeedCharacter(db, "vanish", "Vanisher") != 0);
    const std::string older = ReadFile(path);
    TFAccountRecord extra;
    ASSERT_TRUE(db.CreateAccount("extra", "salt", "hash", extra));

    // An older copy restored under a running authority must not be served or
    // written over (that would silently drop "extra").
    WriteFile(path, older);
    TFAccountRecord found;
    EXPECT_FALSE(db.FindAccountByUsername("vanish", found));
    EXPECT_FALSE(db.IsOpen());
    EXPECT_TRUE(db.RecoveryLatched());

    // A committed primary that disappears is never silently recreated empty.
    TFDatabase second;
    ASSERT_TRUE(second.Open(path));
    fs::remove(path);
    TFAccountRecord recreated;
    EXPECT_FALSE(second.CreateAccount("after", "salt", "hash", recreated));
    EXPECT_TRUE(second.LastStatus() == TFDatabaseStatus::Unreadable);
    EXPECT_FALSE(fs::exists(path));
}

TEST(TF120_SharedRoot_LockHeldBeyondTimeoutFailsClosedAsLocked)
{
    const fs::path path = FreshSharedDb("test_tf120_locked.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    const uint64_t charId = SeedCharacter(db, "locked", "Locked Out");
    ASSERT_TRUE(charId != 0);

    SavePaths::ExclusiveFileLock stuckPeer;
    std::error_code ec;
    ASSERT_TRUE(stuckPeer.TryLock(path, ec));

    TFDatabase late;
    EXPECT_FALSE(late.Open(path));
    EXPECT_TRUE(late.LastStatus() == TFDatabaseStatus::Locked);
    EXPECT_FALSE(db.SaveCharacterProgress(charId, 1, 1, 1, 1));
    EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::Locked);

    stuckPeer.Unlock();
    EXPECT_TRUE(db.SaveCharacterProgress(charId, 1, 1, 1, 1));
    EXPECT_TRUE(late.Open(path));
}

TEST(TF120_SharedRoot_StaleCharacterDoesNotBlockOtherCharactersInMetaSweep)
{
    const fs::path path = FreshSharedDb("test_tf120_sweep_conflict.db");
    TFDatabase continentA;
    TFDatabase continentB;
    ASSERT_TRUE(continentA.Open(path));
    ASSERT_TRUE(continentB.Open(path));
    const uint64_t hopper = SeedCharacter(continentA, "hopper", "Hopper");
    const uint64_t stayer = SeedCharacter(continentA, "stayer", "Stayer");
    ASSERT_TRUE(hopper != 0 && stayer != 0);

    // Both players enter world on A.
    TFPlayerMetaStore store;
    TFCharacterRecord rec;
    ASSERT_TRUE(continentA.AcquireCharacter(hopper, rec));
    store.SeedFromRecord(PlayerId{1}, rec);
    ASSERT_TRUE(continentA.AcquireCharacter(stayer, rec));
    store.SeedFromRecord(PlayerId{2}, rec);

    // The hopper buys an unlock, then disconnects while the flush cannot run
    // (no database), so the meta is parked; the character then enters world
    // on B, which commits newer state.
    store.Ensure(PlayerId{1}).unlocks.insert("stale_unlock");
    store.Ensure(PlayerId{1}).dirty = true;
    EXPECT_FALSE(store.Detach(PlayerId{1}, nullptr));
    ASSERT_TRUE(continentB.AcquireCharacter(hopper, rec));
    ASSERT_TRUE(continentB.SaveCharacterProgress(hopper, 40, 1, 77, 5));

    // The parked row alone is rejected with Conflict and names the stale row.
    EXPECT_FALSE(store.PersistAllDirty(continentA));
    EXPECT_TRUE(continentA.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_FALSE(store.AnyDirty()); // discarded, not retried forever
    TFCharacterRecord durable;
    ASSERT_TRUE(continentA.FindCharacter(hopper, durable));
    EXPECT_EQ(durable.flux, uint32_t{77});
    EXPECT_TRUE(durable.unlocks.empty());

    // Park a second stale row, then sweep it together with the stayer's
    // purchase: the stayer's progress and meta still commit.
    ASSERT_TRUE(continentA.AcquireCharacter(hopper, rec));
    store.SeedFromRecord(PlayerId{3}, rec);
    store.Ensure(PlayerId{3}).unlocks.insert("second_stale_unlock");
    store.Ensure(PlayerId{3}).dirty = true;
    EXPECT_FALSE(store.Detach(PlayerId{3}, nullptr));
    ASSERT_TRUE(continentB.AcquireCharacter(hopper, rec));
    ASSERT_TRUE(continentB.SaveCharacterProgress(hopper, 41, 1, 78, 6));

    store.Ensure(PlayerId{2}).unlocks.insert("rifle_b");
    store.Ensure(PlayerId{2}).dirty = true;
    TFCharacterUpdate stayerDebit;
    stayerDebit.charId = stayer;
    stayerDebit.writeProgress = true;
    stayerDebit.rank = 1;
    stayerDebit.flux = 5;
    EXPECT_FALSE(store.PersistAllDirty(continentA, {stayerDebit}));
    EXPECT_FALSE(store.AnyDirty());
    ASSERT_TRUE(continentA.FindCharacter(stayer, durable));
    EXPECT_EQ(durable.flux, uint32_t{5});
    ASSERT_EQ(durable.unlocks.size(), size_t{1});
    EXPECT_EQ(durable.unlocks[0], std::string("rifle_b"));
    ASSERT_TRUE(continentA.FindCharacter(hopper, durable));
    EXPECT_EQ(durable.flux, uint32_t{78});
    EXPECT_TRUE(durable.unlocks.empty());

    // Later sweeps are no longer blocked.
    store.Ensure(PlayerId{2}).unlocks.insert("smg_b");
    store.Ensure(PlayerId{2}).dirty = true;
    EXPECT_TRUE(store.PersistAllDirty(continentA));
    ASSERT_TRUE(continentA.FindCharacter(stayer, durable));
    EXPECT_EQ(durable.unlocks.size(), size_t{2});

    // An in-world row that conflicts is not discarded or overwritten: it
    // stays dirty and keeps reporting failure, while other rows commit.
    ASSERT_TRUE(continentA.AcquireCharacter(hopper, rec));
    store.SeedFromRecord(PlayerId{4}, rec);
    ASSERT_TRUE(continentB.AcquireCharacter(hopper, rec));
    ASSERT_TRUE(continentB.SaveCharacterProgress(hopper, 42, 1, 79, 7));
    store.Ensure(PlayerId{4}).unlocks.insert("double_login_unlock");
    store.Ensure(PlayerId{4}).dirty = true;
    store.Ensure(PlayerId{2}).unlocks.insert("pistol_b");
    store.Ensure(PlayerId{2}).dirty = true;
    EXPECT_FALSE(store.PersistAllDirty(continentA));
    EXPECT_TRUE(store.IsDirty(PlayerId{4}));
    EXPECT_FALSE(store.IsDirty(PlayerId{2}));
    ASSERT_TRUE(continentA.FindCharacter(stayer, durable));
    EXPECT_EQ(durable.unlocks.size(), size_t{3});
    ASSERT_TRUE(continentA.FindCharacter(hopper, durable));
    EXPECT_EQ(durable.flux, uint32_t{79});
    EXPECT_TRUE(durable.unlocks.empty());

    // A row parked by a failed disconnect flush is not re-adopted over a
    // newer row another authority committed before the player came back.
    const fs::path staging(path.wstring() + L".tmp");
    ASSERT_TRUE(continentA.AcquireCharacter(stayer, rec));
    store.SeedFromRecord(PlayerId{5}, rec);
    store.Ensure(PlayerId{5}).unlocks.insert("parked_unlock");
    store.Ensure(PlayerId{5}).dirty = true;
    store.Erase(PlayerId{2}); // one live row per character
    ASSERT_TRUE(fs::create_directory(staging));
    EXPECT_FALSE(store.Detach(PlayerId{5}, &continentA));
    fs::remove_all(staging);
    ASSERT_TRUE(continentB.AcquireCharacter(stayer, rec));
    ASSERT_TRUE(continentB.SaveCharacterProgress(stayer, 9, 1, 6, 8));
    ASSERT_TRUE(continentA.AcquireCharacter(stayer, rec));
    store.SeedFromRecord(PlayerId{6}, rec);
    ASSERT_TRUE(store.Find(PlayerId{6}) != nullptr);
    EXPECT_FALSE(store.Find(PlayerId{6})->unlocks.contains("parked_unlock"));
    EXPECT_FALSE(store.IsDirty(PlayerId{6}));
    EXPECT_EQ(store.Find(PlayerId{6})->unlocks.size(), size_t{3});

    // The database names the stale row of a rejected commit.
    TFCharacterUpdate stale;
    stale.charId = hopper;
    stale.writeProgress = true;
    stale.rank = 1;
    stale.flux = 1;
    EXPECT_FALSE(continentA.CommitCharacterUpdates({stale}));
    EXPECT_TRUE(continentA.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_EQ(continentA.ConflictedCharacter(), hopper);
    EXPECT_TRUE(continentA.Close());
    EXPECT_TRUE(continentB.Close());
}

#ifndef _WIN32
namespace
{
    constexpr int kInterleavePeers = 4;
    constexpr int kRoundsPerPeer = 20; // half flux (+1), half unlock appends

    /// Peer role: one independent continent authority on the shared root.
    void RunInterleavePeer()
    {
        const fs::path path = EnvOrEmpty(kPeerDbEnv);
        const uint64_t sharedChar = std::strtoull(EnvOrEmpty(kPeerCharEnv).c_str(), nullptr, 10);
        const std::string tag = EnvOrEmpty(kPeerTagEnv);
        const fs::path gate = EnvOrEmpty(kPeerGateEnv);
        ASSERT_TRUE(sharedChar != 0 && !tag.empty() && !gate.empty());

        // Hold until every peer is running so the transactions really overlap.
        const auto gateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!fs::exists(gate) && std::chrono::steady_clock::now() < gateDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ASSERT_TRUE(fs::exists(gate));

        TFDatabase continent;
        ASSERT_TRUE(continent.Open(path));
        ASSERT_TRUE(SeedCharacter(continent, ("user_" + tag).c_str(), ("Child " + tag).c_str()) != 0);
        int conflicts = 0;
        constexpr std::chrono::microseconds kThink{500};
        for (int round = 0; round < kRoundsPerPeer; ++round)
        {
            const RmwResult result =
                (round % 2 == 0) ? AddFluxWithRetry(continent, sharedChar, conflicts, kThink)
                                 : AddUnlockWithRetry(continent, sharedChar,
                                                      "unlock_" + tag + "_" + std::to_string(round), conflicts, kThink);
            ASSERT_TRUE(result == RmwResult::Committed);
        }
        std::printf("TF120_PEER_CONFLICTS=%d\n", conflicts);
        std::fflush(stdout);
    }

    /// Peer role: commit as fast as possible until SIGKILLed, so the kill
    /// lands inside a transaction (holding the lock, mid tmp-write or just
    /// before the rename).
    void RunHammerPeer()
    {
        const fs::path path = EnvOrEmpty(kPeerDbEnv);
        const uint64_t charId = std::strtoull(EnvOrEmpty(kPeerCharEnv).c_str(), nullptr, 10);
        TFDatabase peer;
        ASSERT_TRUE(charId != 0 && peer.Open(path));
        for (bool announced = false;;)
        {
            TFCharacterRecord row;
            ASSERT_TRUE(peer.AcquireCharacter(charId, row));
            ASSERT_TRUE(peer.SaveCharacterProgress(charId, row.xp + 1, row.rank, row.flux, 1));
            if (!announced)
            {
                std::printf("TF120_PEER_READY=1\n");
                std::fflush(stdout);
                announced = true;
            }
        }
    }
} // namespace

TEST(TF120_SharedRoot_SpawnedAuthoritiesInterleaveWithoutLostUpdates)
{
    if (EnvOrEmpty(kPeerRoleEnv) == "interleave")
    {
        RunInterleavePeer();
        return;
    }

    const fs::path path = fs::absolute(FreshSharedDb("test_tf120_spawned.db"));
    const fs::path gate = fs::path(path.string() + ".gate");
    fs::remove(gate);
    uint64_t sharedChar = 0;
    {
        TFDatabase seed;
        ASSERT_TRUE(seed.Open(path));
        sharedChar = SeedCharacter(seed, "shared", "Shared Hero");
        ASSERT_TRUE(sharedChar != 0);
    }

    std::vector<Spark::Process> peers;
    for (int peer = 0; peer < kInterleavePeers; ++peer)
    {
        auto launched = SpawnPeer(
            "TF120_SharedRoot_SpawnedAuthoritiesInterleaveWithoutLostUpdates",
            {std::string(kPeerRoleEnv) + "=interleave", std::string(kPeerDbEnv) + "=" + path.string(),
             std::string(kPeerCharEnv) + "=" + std::to_string(sharedChar),
             std::string(kPeerTagEnv) + "=" + std::to_string(peer), std::string(kPeerGateEnv) + "=" + gate.string()});
        if (!launched)
            SKIP_TEST("cannot spawn a peer test process: " + launched.error());
        peers.push_back(std::move(*launched));
    }
    WriteFile(gate, "go"); // release every peer at once

    std::vector<std::string> logs(peers.size());
    const std::vector<int> codes = WaitPeers(peers, logs, std::chrono::seconds(180));
    fs::remove(gate);
    int totalConflicts = 0;
    for (size_t peer = 0; peer < peers.size(); ++peer)
    {
        EXPECT_EQ(codes[peer], 0);
        const long long conflicts = PeerReport(logs[peer], "TF120_PEER_CONFLICTS=");
        EXPECT_TRUE(conflicts >= 0); // the peer really ran its role to completion
        if (codes[peer] != 0 || conflicts < 0)
            std::fprintf(stderr, "---- TF120 peer %zu output ----\n%s\n----\n", peer, logs[peer].c_str());
        totalConflicts += conflicts > 0 ? static_cast<int>(conflicts) : 0;
    }
    std::printf("[TF120] %d spawned authorities: %d stale commits rejected and retried\n", kInterleavePeers,
                totalConflicts);

    TFDatabase observer;
    ASSERT_TRUE(observer.Open(path));
    TFCharacterRecord shared;
    ASSERT_TRUE(observer.FindCharacter(sharedChar, shared));
    // Every successful read-modify-write from every process is present.
    EXPECT_EQ(shared.flux, uint32_t{kInterleavePeers * kRoundsPerPeer / 2});
    EXPECT_EQ(shared.xp, uint32_t{kInterleavePeers * kRoundsPerPeer / 2});
    EXPECT_EQ(shared.unlocks.size(), size_t{kInterleavePeers * kRoundsPerPeer / 2});

    std::vector<uint64_t> ids{sharedChar};
    for (int peer = 0; peer < kInterleavePeers; ++peer)
    {
        const std::string tag = std::to_string(peer);
        TFAccountRecord account;
        EXPECT_TRUE(observer.FindAccountByUsername("user_" + tag, account));
        TFCharacterRecord row;
        ASSERT_TRUE(observer.FindCharacterByName("Child " + tag, row));
        for (const uint64_t id : ids)
            EXPECT_NE(row.id, id);
        ids.push_back(row.id);
    }
}

TEST(TF120_SharedRoot_PeerKilledMidTransactionLeavesCommittedStateUsable)
{
    if (EnvOrEmpty(kPeerRoleEnv) == "hammer")
    {
        RunHammerPeer();
        return;
    }

    const fs::path path = fs::absolute(FreshSharedDb("test_tf120_killed.db"));
    TFDatabase survivor;
    ASSERT_TRUE(survivor.Open(path));
    const uint64_t charId = SeedCharacter(survivor, "crashy", "Crash Test");
    ASSERT_TRUE(charId != 0);

    uint32_t lastXp = 0;
    for (int kill = 0; kill < 8; ++kill)
    {
        auto launched = SpawnPeer("TF120_SharedRoot_PeerKilledMidTransactionLeavesCommittedStateUsable",
                                  {std::string(kPeerRoleEnv) + "=hammer", std::string(kPeerDbEnv) + "=" + path.string(),
                                   std::string(kPeerCharEnv) + "=" + std::to_string(charId)});
        if (!launched)
            SKIP_TEST("cannot spawn a peer test process: " + launched.error());
        Spark::Process& peer = *launched;

        // Wait until the peer is committing, then kill it at a varying offset.
        std::string log;
        const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (PeerReport(log, "TF120_PEER_READY=") < 0 && peer.IsRunning() &&
               std::chrono::steady_clock::now() < readyDeadline)
        {
            DrainPeer(peer, log);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool ready = PeerReport(log, "TF120_PEER_READY=") == 1 && peer.IsRunning();
        if (!ready)
            std::fprintf(stderr, "---- TF120 hammer peer output ----\n%s\n----\n", log.c_str());
        ASSERT_TRUE(ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(15 + 7 * kill));
        peer.Kill();
        EXPECT_FALSE(peer.IsRunning());

        // The OS released the dead peer's lock, the primary is the last
        // complete commit, and the survivor keeps working on top of it.
        TFCharacterRecord row;
        ASSERT_TRUE(survivor.AcquireCharacter(charId, row));
        EXPECT_TRUE(row.xp > lastXp); // the peer committed at least once
        EXPECT_TRUE(survivor.SaveCharacterProgress(charId, row.xp + 1, row.rank, row.flux, 2));
        lastXp = row.xp + 1;
    }

    TFDatabase restarted;
    ASSERT_TRUE(restarted.Open(path));
    TFCharacterRecord durable;
    ASSERT_TRUE(restarted.FindCharacter(charId, durable));
    EXPECT_EQ(durable.xp, lastXp);
}
#endif
