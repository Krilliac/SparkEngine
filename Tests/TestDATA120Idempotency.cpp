/**
 * @file TestDATA120Idempotency.cpp
 * @brief DATA-120: snapshot retries preserve values and handoff retries preserve operation fences.
 *
 * Each case drives the production TFDatabase, TFPlayerMetaStore and
 * TFCharacterSystem against a real file and replays a write the way a real
 * caller retries it: the save sweep after a Locked or failed commit, the save
 * tick that re-sends every player's progress, a character create whose
 * acknowledgement was lost, and handoff phases retried after failed writes or authority restarts.
 * docs/specs/persistence.md ("Retries and idempotency") states the contract
 * these cases pin and lists the families that already cover the other paths.
 */
#include "TestFramework.h"
#include "Account/TFCharacterSystem.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFPlayerMeta.h"
#include "Persistence/TFSavePaths.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using Terrafront::FactionId;
using Terrafront::PlayerId;
using Terrafront::TFAccountRecord;
using Terrafront::TFCharacterRecord;
using Terrafront::TFCharacterSystem;
using Terrafront::TFCharCreateResult;
using Terrafront::TFCharErr;
using Terrafront::TFDatabase;
using Terrafront::TFDatabaseStatus;
using Terrafront::TFPlayerMetaStore;
namespace SavePaths = Terrafront::SavePaths;

namespace
{
    namespace fs = std::filesystem;

    fs::path FreshIdempotencyDb(const char* name)
    {
        const fs::path path = fs::path("Saves") / name;
        fs::create_directories(path.parent_path());
        fs::remove(path);
        fs::remove(path.string() + ".lock");
        fs::remove(path.string() + ".authority.alpha.lock");
        fs::remove(path.string() + ".authority.beta.lock");
        fs::remove_all(fs::path(path.wstring() + L".tmp"));
        // A quarantined primary from an earlier failed run would make Open demand recovery.
        const std::string quarantinePrefix = path.filename().string() + ".corrupt-";
        for (const fs::directory_entry& entry : fs::directory_iterator(path.parent_path()))
        {
            if (entry.path().filename().string().starts_with(quarantinePrefix))
            {
                fs::remove(entry.path());
            }
        }
        return path;
    }

    /// Occupying the "<db>.tmp" staging path with a directory makes the next commit fail before its rename.
    fs::path StagingBlocker(const fs::path& path)
    {
        return {path.wstring() + L".tmp"};
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.is_open());
        std::ostringstream ss;
        ss << in.rdbuf();
        ASSERT_FALSE(in.bad());
        return ss.str();
    }

    /// Account + one character holding `flux`, as an onboarded player.
    uint64_t SeedCharacter(TFDatabase& db, const char* user, const char* name, uint32_t flux)
    {
        TFAccountRecord account;
        TFCharacterRecord character;
        if (!db.CreateAccount(user, "salt", "hash", account) ||
            !db.CreateCharacter(account.id, name, FactionId::MRA, character))
        {
            return 0;
        }
        if (flux != 0 && !db.SaveCharacterProgress(character.id, 0, 1, flux, 0))
        {
            return 0;
        }
        return character.id;
    }
} // namespace

// A save sweep refused because another process held the persistence lock past its wait bound must keep the
// meta dirty, commit it exactly once on the next sweep, and leave the file alone when nothing is dirty.
TEST(Persistence_Idempotency_SweepRetryAfterLockedCommitsExactlyOnce)
{
    const fs::path path = FreshIdempotencyDb("test_data120_idem_locked.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    const uint64_t charId = SeedCharacter(db, "locked_sweep", "Locked Sweep", 60);
    ASSERT_TRUE(charId != 0);

    TFCharacterRecord seeded;
    ASSERT_TRUE(db.FindCharacter(charId, seeded));
    TFPlayerMetaStore store;
    store.SeedFromRecord(PlayerId{1}, seeded);
    TFPlayerMetaStore::Meta& meta = store.Ensure(PlayerId{1});
    meta.unlocks.insert("smg_basic");
    meta.stats["smg_basic"].kills = 3;
    meta.dirty = true;
    const std::string beforeSweep = ReadFile(path);

    {
        SavePaths::ExclusiveFileLock stuckPeer;
        std::error_code ec;
        ASSERT_TRUE(stuckPeer.TryLock(path, ec));
        EXPECT_FALSE(store.PersistAllDirty(db));
        EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::Locked);
        EXPECT_TRUE(store.IsDirty(PlayerId{1})); // unacknowledged: the next sweep must retry it
        EXPECT_TRUE(ReadFile(path) == beforeSweep);
    }

    EXPECT_TRUE(store.PersistAllDirty(db));
    EXPECT_FALSE(store.IsDirty(PlayerId{1}));
    const std::string afterRetry = ReadFile(path);

    // A sweep with nothing dirty writes nothing, so a repeated save tick cannot re-apply the commit.
    EXPECT_TRUE(store.PersistAllDirty(db));
    EXPECT_TRUE(ReadFile(path) == afterRetry);
    EXPECT_TRUE(db.Close());

    TFDatabase restarted;
    ASSERT_TRUE(restarted.Open(path));
    TFCharacterRecord onDisk;
    ASSERT_TRUE(restarted.FindCharacter(charId, onDisk));
    EXPECT_EQ(onDisk.revision, seeded.revision + 1); // exactly one commit landed across both sweeps
    EXPECT_EQ(onDisk.flux, uint32_t{60});
    ASSERT_EQ(onDisk.unlocks.size(), size_t{1});
    EXPECT_TRUE(onDisk.unlocks[0] == "smg_basic");
    ASSERT_EQ(onDisk.weaponStats.size(), size_t{1});
    EXPECT_EQ(onDisk.weaponStats[0].kills, uint32_t{3});
    EXPECT_TRUE(restarted.Close());
    fs::remove(path);
}

// TFProgressionSystem::SaveNow writes each player's progress through TFCharacterSystem::PersistProgress and
// then sweeps the dirty meta. A purchase whose save failed at the staging write is retried by the next tick,
// and every later tick re-sends the same absolute progress row: the wallet is debited exactly once.
TEST(Persistence_Idempotency_RetriedPurchaseSaveAfterFailedStagingDebitsOnce)
{
    const fs::path path = FreshIdempotencyDb("test_data120_idem_staging.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    const uint64_t charId = SeedCharacter(db, "staging_buyer", "Staging Buyer", 100);
    ASSERT_TRUE(charId != 0);
    TFCharacterSystem characters;
    characters.SetDatabase(&db);

    TFCharacterRecord seeded;
    ASSERT_TRUE(db.FindCharacter(charId, seeded));
    TFPlayerMetaStore store;
    store.SeedFromRecord(PlayerId{1}, seeded);

    // The purchase as the progression system applies it in memory: wallet 100 -> 40, one unlock key.
    constexpr uint32_t kFluxAfterPurchase = 40;
    store.Ensure(PlayerId{1}).unlocks.insert("smg_basic");
    store.Ensure(PlayerId{1}).dirty = true;

    ASSERT_TRUE(fs::create_directory(StagingBlocker(path)));
    EXPECT_FALSE(characters.PersistProgress(charId, 0, 1, kFluxAfterPurchase));
    EXPECT_FALSE(store.PersistAllDirty(db));
    EXPECT_TRUE(store.IsDirty(PlayerId{1}));
    TFCharacterRecord unchanged;
    ASSERT_TRUE(db.FindCharacter(charId, unchanged));
    EXPECT_EQ(unchanged.flux, uint32_t{100});
    EXPECT_TRUE(unchanged.unlocks.empty());
    fs::remove_all(StagingBlocker(path));

    // Next save tick: both writes land.
    EXPECT_TRUE(characters.PersistProgress(charId, 0, 1, kFluxAfterPurchase));
    EXPECT_TRUE(store.PersistAllDirty(db));
    EXPECT_FALSE(store.IsDirty(PlayerId{1}));

    // Later ticks re-send the unchanged absolute row; a replay rewrites the same balance, never a second debit.
    EXPECT_TRUE(characters.PersistProgress(charId, 0, 1, kFluxAfterPurchase));
    EXPECT_TRUE(characters.PersistProgress(charId, 0, 1, kFluxAfterPurchase));
    EXPECT_TRUE(store.PersistAllDirty(db));
    EXPECT_TRUE(db.Close());

    TFDatabase restarted;
    ASSERT_TRUE(restarted.Open(path));
    TFCharacterRecord onDisk;
    ASSERT_TRUE(restarted.FindCharacter(charId, onDisk));
    EXPECT_EQ(onDisk.flux, kFluxAfterPurchase);
    ASSERT_EQ(onDisk.unlocks.size(), size_t{1});
    EXPECT_TRUE(onDisk.unlocks[0] == "smg_basic");
    EXPECT_TRUE(restarted.Close());
    fs::remove(path);
}

// A create whose commit failed is retried and creates the character once; a create whose commit landed but
// whose acknowledgement was lost is refused as NameTaken on retry and never writes a second row. The retry
// does not get the original record back: that is the stated UX limitation, not a duplicate.
TEST(Persistence_Idempotency_RetriedCharacterCreateLeavesExactlyOneRow)
{
    const fs::path path = FreshIdempotencyDb("test_data120_idem_create.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountRecord account;
    ASSERT_TRUE(db.CreateAccount("retry_create", "salt", "hash", account));
    TFCharacterSystem characters;
    characters.SetDatabase(&db);

    ASSERT_TRUE(fs::create_directory(StagingBlocker(path)));
    const TFCharCreateResult failed = characters.Create(account.id, "Retry Create", FactionId::MRA);
    EXPECT_FALSE(failed.ok);
    EXPECT_TRUE(failed.err == TFCharErr::ServerError);
    EXPECT_TRUE(characters.List(account.id).empty());
    fs::remove_all(StagingBlocker(path));

    const TFCharCreateResult created = characters.Create(account.id, "Retry Create", FactionId::MRA);
    ASSERT_TRUE(created.ok);
    const std::string afterCreate = ReadFile(path);

    const TFCharCreateResult replayed = characters.Create(account.id, "Retry Create", FactionId::MRA);
    EXPECT_FALSE(replayed.ok);
    EXPECT_TRUE(replayed.err == TFCharErr::NameTaken);
    EXPECT_TRUE(ReadFile(path) == afterCreate);
    EXPECT_TRUE(db.Close());

    TFDatabase restarted;
    ASSERT_TRUE(restarted.Open(path));
    const std::vector<TFCharacterRecord> rows = restarted.ListCharacters(account.id);
    ASSERT_EQ(rows.size(), size_t{1});
    EXPECT_EQ(rows[0].id, created.charId);
    EXPECT_TRUE(restarted.Close());
    fs::remove(path);
}

// A destination that committed a handoff and then died is replaced by a fresh instance whose bind clears the
// dead authority's residency. Replaying the same commit operation from that instance must be refused and must
// not re-seize the character; the committed operation, its epoch and the row revision stay as they were. The
// bind must also keep the terminal handoff record valid: it used to strip its source and destination, and the
// next read then quarantined the whole database as corrupt.
// (Duplicate commits on a live destination are TF120_HandoffReservation_DuplicateReorderAndCommitAreExactlyOnce.)
TEST(Persistence_Idempotency_RestartedDestinationCannotReplayCommittedHandoff)
{
    const fs::path path = FreshIdempotencyDb("test_data120_idem_handoff.db");
    uint64_t charId = 0;
    {
        TFDatabase seed;
        ASSERT_TRUE(seed.Open(path));
        charId = SeedCharacter(seed, "handoff_replay", "Handoff Replay", 0);
        ASSERT_TRUE(charId != 0);
        EXPECT_TRUE(seed.Close());
    }
    TFDatabase source;
    ASSERT_TRUE(source.Open(path) && source.BindAuthority("alpha"));
    TFCharacterRecord row;
    ASSERT_TRUE(source.ClaimCharacter(charId, row));
    ASSERT_TRUE(source.ReserveMigration(charId, "op-replay", 7, "beta", "state-v1", row));

    {
        TFDatabase crashedDestination;
        ASSERT_TRUE(crashedDestination.Open(path) && crashedDestination.BindAuthority("beta"));
        ASSERT_TRUE(crashedDestination.CommitMigration(charId, "op-replay", row));
        EXPECT_EQ(row.residentContinent, std::string("beta"));
        EXPECT_TRUE(crashedDestination.Close()); // the process dies before acknowledging the source
    }

    TFDatabase destination;
    ASSERT_TRUE(destination.Open(path) && destination.BindAuthority("beta"));
    TFCharacterRecord recovered;
    ASSERT_TRUE(destination.FindCharacter(charId, recovered));
    EXPECT_TRUE(recovered.residentContinent.empty());
    EXPECT_EQ(recovered.migrationLastOperation, std::string("op-replay"));
    EXPECT_EQ(recovered.migrationState, std::string("committed"));
    // The terminal record survives the bind whole; without its source and destination the file fails validation.
    EXPECT_EQ(recovered.migrationSource, std::string("alpha"));
    EXPECT_EQ(recovered.migrationDestination, std::string("beta"));
    const std::string beforeReplay = ReadFile(path);

    TFCharacterRecord replayed;
    EXPECT_FALSE(destination.CommitMigration(charId, "op-replay", replayed));
    EXPECT_TRUE(destination.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_TRUE(ReadFile(path) == beforeReplay);

    // The source's pre-handoff baseline stays fenced, so its stale absolute write cannot land either.
    EXPECT_FALSE(source.SaveCharacterProgress(charId, 5, 1, 5, 1));
    EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);

    TFCharacterRecord after;
    ASSERT_TRUE(destination.FindCharacter(charId, after));
    EXPECT_EQ(after.revision, recovered.revision);
    EXPECT_EQ(after.migrationEpoch, uint64_t{7});
    EXPECT_TRUE(after.residentContinent.empty());
    EXPECT_TRUE(after.migrationOperation.empty());
    EXPECT_TRUE(destination.Close());
    EXPECT_TRUE(source.Close());
    fs::remove(path);
}

// Recovery rolls a dead source's reservation back and preserves an already completed rollback. Both must
// reload with the same operation/epoch/payload fence, reject old phases, and still allow a newer handoff.
TEST(Persistence_Idempotency_RestartedSourcePreservesRollbackFence)
{
    for (const bool finishRollback : std::array{false, true})
    {
        const fs::path path =
            FreshIdempotencyDb(finishRollback ? "test_data120_idem_rollback.db" : "test_data120_idem_reserved.db");
        uint64_t charId = 0;
        TFCharacterRecord row;
        {
            TFDatabase source;
            ASSERT_TRUE(source.Open(path) && source.BindAuthority("alpha"));
            charId = SeedCharacter(source, "source_replay", "Source Replay", 0);
            ASSERT_TRUE(charId != 0);
            ASSERT_TRUE(source.ClaimCharacter(charId, row));
            ASSERT_TRUE(source.ReserveMigration(charId, "op-old", 23, "beta", "saved-payload", row));
            if (finishRollback)
            {
                ASSERT_TRUE(source.AbortMigration(charId, "op-old"));
                const std::string afterRollback = ReadFile(path);
                ASSERT_TRUE(source.AbortMigration(charId, "op-old"));
                EXPECT_EQ(ReadFile(path), afterRollback);
            }
            ASSERT_TRUE(source.Close()); // release the lifetime lock with the character still resident
        }

        TFDatabase source;
        ASSERT_TRUE(source.Open(path) && source.BindAuthority("alpha"));
        ASSERT_TRUE(source.FindCharacter(charId, row)); // reload validates the file written by the bind
        EXPECT_TRUE(row.residentContinent.empty());
        EXPECT_TRUE(row.migrationOperation.empty());
        EXPECT_EQ(row.migrationLastOperation, std::string("op-old"));
        EXPECT_EQ(row.migrationState, std::string("rolled_back"));
        EXPECT_EQ(row.migrationSource, std::string("alpha"));
        EXPECT_EQ(row.migrationDestination, std::string("beta"));
        EXPECT_EQ(row.migrationPayload, std::string("saved-payload"));
        EXPECT_EQ(row.migrationEpoch, uint64_t{23});

        TFDatabase destination;
        ASSERT_TRUE(destination.Open(path) && destination.BindAuthority("beta"));
        const std::string afterRecovery = ReadFile(path);
        EXPECT_FALSE(destination.CommitMigration(charId, "op-old", row));
        EXPECT_TRUE(destination.LastStatus() == TFDatabaseStatus::Conflict);
        EXPECT_FALSE(source.AbortMigration(charId, "op-old")); // no ownership until a fresh claim
        EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);
        EXPECT_EQ(ReadFile(path), afterRecovery);

        ASSERT_TRUE(source.ClaimCharacter(charId, row));
        const std::string afterClaim = ReadFile(path);
        EXPECT_FALSE(source.ReserveMigration(charId, "op-old", 23, "beta", "saved-payload", row));
        EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);
        EXPECT_FALSE(source.ReserveMigration(charId, "op-different", 23, "beta", "saved-payload", row));
        EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);
        ASSERT_TRUE(source.AbortMigration(charId, "op-old")); // terminal acknowledgement, no new write
        EXPECT_EQ(ReadFile(path), afterClaim);

        ASSERT_TRUE(source.ReserveMigration(charId, "op-new", 24, "beta", "new-payload", row));
        const std::string newReservation = ReadFile(path);
        EXPECT_FALSE(source.AbortMigration(charId, "op-old"));
        EXPECT_FALSE(destination.CommitMigration(charId, "op-old", row));
        EXPECT_EQ(ReadFile(path), newReservation);
        ASSERT_TRUE(destination.CommitMigration(charId, "op-new", row));
        EXPECT_EQ(row.residentContinent, std::string("beta"));
        EXPECT_EQ(row.migrationEpoch, uint64_t{24});
        EXPECT_EQ(row.migrationPayload, std::string("new-payload"));
        EXPECT_TRUE(destination.Close());
        EXPECT_TRUE(source.Close());
        fs::remove(path);
    }
}

// Failed reservation/commit writes must not consume the operation. The next delivery lands once, and
// subsequent deliveries acknowledge that same row without another durable write or ownership transition.
TEST(Persistence_Idempotency_HandoffWriteFailureRemainsRetryable)
{
    const fs::path path = FreshIdempotencyDb("test_data120_idem_handoff_retry.db");
    TFDatabase source;
    TFDatabase destination;
    ASSERT_TRUE(source.Open(path) && source.BindAuthority("alpha"));
    const uint64_t charId = SeedCharacter(source, "handoff_retry", "Handoff Retry", 0);
    ASSERT_TRUE(charId != 0);
    TFCharacterRecord row;
    ASSERT_TRUE(source.ClaimCharacter(charId, row));
    const uint64_t claimedRevision = row.revision;
    ASSERT_TRUE(destination.Open(path) && destination.BindAuthority("beta"));
    const std::string beforeReserve = ReadFile(path);

    TFCharacterRecord output;
    output.id = 9999;
    ASSERT_TRUE(fs::create_directory(StagingBlocker(path)));
    EXPECT_FALSE(source.ReserveMigration(charId, "op-retry", 31, "beta", "retry-payload", output));
    EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::WriteFailed);
    EXPECT_EQ(output.id, uint64_t{9999});
    EXPECT_EQ(ReadFile(path), beforeReserve);
    ASSERT_TRUE(fs::remove(StagingBlocker(path)));

    ASSERT_TRUE(source.ReserveMigration(charId, "op-retry", 31, "beta", "retry-payload", row));
    EXPECT_EQ(row.revision, claimedRevision + 1);
    const uint64_t reservedRevision = row.revision;
    const std::string afterReserve = ReadFile(path);
    EXPECT_FALSE(source.ReserveMigration(charId, "op-retry", 31, "beta", "different-payload", output));
    EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);
    ASSERT_TRUE(source.ReserveMigration(charId, "op-retry", 31, "beta", "retry-payload", row));
    EXPECT_EQ(row.revision, reservedRevision);
    EXPECT_EQ(ReadFile(path), afterReserve);

    ASSERT_TRUE(fs::create_directory(StagingBlocker(path)));
    EXPECT_FALSE(destination.CommitMigration(charId, "op-retry", output));
    EXPECT_TRUE(destination.LastStatus() == TFDatabaseStatus::WriteFailed);
    EXPECT_EQ(output.id, uint64_t{9999});
    EXPECT_EQ(ReadFile(path), afterReserve);
    ASSERT_TRUE(fs::remove(StagingBlocker(path)));

    ASSERT_TRUE(destination.CommitMigration(charId, "op-retry", row));
    EXPECT_EQ(row.revision, reservedRevision + 1);
    const uint64_t committedRevision = row.revision;
    const std::string afterCommit = ReadFile(path);
    ASSERT_TRUE(destination.CommitMigration(charId, "op-retry", row));
    EXPECT_EQ(row.revision, committedRevision);
    EXPECT_FALSE(source.AbortMigration(charId, "op-retry"));
    EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_EQ(ReadFile(path), afterCommit);
    EXPECT_TRUE(destination.Close());
    EXPECT_TRUE(source.Close());

    TFDatabase observer;
    ASSERT_TRUE(observer.Open(path) && observer.FindCharacter(charId, row));
    EXPECT_EQ(row.revision, committedRevision);
    EXPECT_EQ(row.residentContinent, std::string("beta"));
    EXPECT_EQ(row.migrationLastOperation, std::string("op-retry"));
    EXPECT_EQ(row.migrationState, std::string("committed"));
    EXPECT_EQ(row.migrationPayload, std::string("retry-payload"));
    EXPECT_TRUE(observer.Close());
    fs::remove(path);
}
