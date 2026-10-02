/**
 * @file TestTF120Migration.cpp
 * @brief TF-120 durable migration reservation and checkpoint tests.
 *
 * These tests exercise both real TFHandoffParticipant instances against the
 * real TFDatabase file. The authority adapter only supplies the game-thread
 * capture/install seam; it does not replace persistence or handoff ordering.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TestFramework.h"

#include "Net/TFHandoffParticipant.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFSavePaths.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Terrafront;
using Spark::Net::AreaID;
using Spark::Net::HandoffRequest;
using Spark::Net::HandoffResult;

namespace
{
    namespace fs = std::filesystem;

    fs::path FreshPath(const char* name)
    {
        const fs::path path = fs::path("Saves") / name;
        fs::create_directories(path.parent_path());
        fs::remove(path);
        fs::remove(path.string() + ".lock");
        fs::remove(path.string() + ".authority.alpha.lock");
        fs::remove(path.string() + ".authority.beta.lock");
        fs::remove(path.string() + ".tmp");
        return path;
    }

    struct Authority final : TFHandoffParticipant::IAuthority
    {
        bool ResolveContinent(AreaID area, std::string& key) const override
        {
            if (area == 1)
            {
                key = "alpha";
            }
            else if (area == 2)
            {
                key = "beta";
            }
            else
            {
                return false;
            }
            return true;
        }

        bool Capture(uint64_t character, TFHandoffState& state) override
        {
            state.player = static_cast<PlayerId>(character);
            state.cls = ClassId::Striker;
            state.position[0] = 12.0f;
            state.position[1] = 2.0f;
            state.position[2] = -4.0f;
            state.velocity[0] = 1.0f;
            state.yaw = 0.5f;
            state.health = 90.0f;
            state.shield = 20.0f;
            state.lastSequence = 9;
            state.grounded = true;
            ++captureCount;
            return state.IsValid();
        }

        bool CanInstall(uint64_t, const TFHandoffState& state) const override { return state.IsValid(); }

        bool Suspend(uint64_t character) override
        {
            ++suspendCount[character];
            return true;
        }

        bool Install(const TFCharacterRecord& character, const TFHandoffState& state) override
        {
            if (!state.IsValid())
            {
                return false;
            }
            if (installedStates.contains(character.id))
            {
                return true;
            }
            installedStates.emplace(character.id, state);
            ++installCount;
            return true;
        }

        void Retire(uint64_t character) override
        {
            retiredCharacter = character;
            ++retireCount;
        }

        int captureCount = 0;
        int installCount = 0;
        int retireCount = 0;
        uint64_t retiredCharacter = 0;
        std::unordered_map<uint64_t, TFHandoffState> installedStates;
        std::unordered_map<uint64_t, int> suspendCount;
    };

    uint64_t Seed(TFDatabase& db)
    {
        TFAccountRecord account;
        TFCharacterRecord character;
        if (!db.CreateAccount("handoff-user", "salt", "hash", account) ||
            !db.CreateCharacter(account.id, "Handoff Hero", FactionId::MRA, character))
        {
            return 0;
        }
        return character.id;
    }

    HandoffRequest Request(uint64_t character, uint64_t epoch = 1)
    {
        HandoffRequest request;
        request.sessionId = TFHandoffParticipant::SessionId(character);
        request.epoch = epoch;
        request.sourceArea = 1;
        request.targetArea = 2;
        return request;
    }

    bool Bind(TFDatabase& db, const char* continent)
    {
        return db.BindAuthority(continent);
    }
} // namespace

TEST(TF120_Migration_DuplicatePrepareAndCommitHaveExactlyOneDurableOwner)
{
    const fs::path path = FreshPath("test_tf120_migration_duplicate.db");
    TFDatabase sourceDb;
    TFDatabase destinationDb;
    ASSERT_TRUE(sourceDb.Open(path));
    const uint64_t character = Seed(sourceDb);
    ASSERT_TRUE(character != 0);
    ASSERT_TRUE(sourceDb.Close());
    ASSERT_TRUE(sourceDb.Open(path));
    ASSERT_TRUE(destinationDb.Open(path));
    ASSERT_TRUE(Bind(sourceDb, "alpha"));
    ASSERT_TRUE(Bind(destinationDb, "beta"));
    TFCharacterRecord claimed;
    ASSERT_TRUE(sourceDb.ClaimCharacter(character, claimed));

    Authority sourceAuthority;
    Authority destinationAuthority;
    TFHandoffParticipant source(sourceDb, sourceAuthority);
    TFHandoffParticipant destination(destinationDb, destinationAuthority);
    const HandoffRequest request = Request(character);

    EXPECT_TRUE(source.Prepare(request) == HandoffResult::Applied);
    EXPECT_TRUE(source.Prepare(request) == HandoffResult::Duplicate);
    EXPECT_TRUE(destination.Prepare(request) == HandoffResult::Applied);
    EXPECT_TRUE(destination.Commit(request) == HandoffResult::Applied);
    EXPECT_TRUE(destination.Commit(request) == HandoffResult::Duplicate);
    EXPECT_TRUE(source.Acknowledge(request) == HandoffResult::Applied);
    EXPECT_TRUE(source.Acknowledge(request) == HandoffResult::Applied);
    EXPECT_EQ(sourceAuthority.captureCount, 1);
    EXPECT_EQ(destinationAuthority.installCount, 1);

    TFDatabase observer;
    ASSERT_TRUE(observer.Open(path));
    TFCharacterRecord row;
    ASSERT_TRUE(observer.FindCharacter(character, row));
    EXPECT_EQ(row.residentContinent, std::string("beta"));
    EXPECT_TRUE(row.migrationOperation.empty());
    EXPECT_EQ(row.migrationLastOperation, std::string("tf/") + std::to_string(character) + "/1");
    EXPECT_TRUE(observer.Close());
    EXPECT_TRUE(destinationDb.Close());
    EXPECT_TRUE(sourceDb.Close());
    fs::remove(path);
}

TEST(TF120_Migration_ReorderedAndDroppedPhasesRetainSourceOwner)
{
    const fs::path path = FreshPath("test_tf120_migration_reordered.db");
    TFDatabase sourceDb;
    TFDatabase destinationDb;
    ASSERT_TRUE(sourceDb.Open(path));
    const uint64_t character = Seed(sourceDb);
    ASSERT_TRUE(character != 0);
    ASSERT_TRUE(sourceDb.Close());
    ASSERT_TRUE(sourceDb.Open(path));
    ASSERT_TRUE(destinationDb.Open(path));
    ASSERT_TRUE(Bind(sourceDb, "alpha"));
    ASSERT_TRUE(Bind(destinationDb, "beta"));
    TFCharacterRecord claimed;
    ASSERT_TRUE(sourceDb.ClaimCharacter(character, claimed));
    Authority sourceAuthority;
    Authority destinationAuthority;
    TFHandoffParticipant source(sourceDb, sourceAuthority);
    TFHandoffParticipant destination(destinationDb, destinationAuthority);
    const HandoffRequest request = Request(character);

    EXPECT_TRUE(destination.Commit(request) == HandoffResult::Rejected);
    EXPECT_TRUE(destination.Transfer(request) == HandoffResult::Rejected);
    EXPECT_TRUE(destination.Acknowledge(request) == HandoffResult::Rejected);
    EXPECT_EQ(destinationAuthority.installCount, 0);

    TFDatabase observer;
    ASSERT_TRUE(observer.Open(path));
    TFCharacterRecord row;
    ASSERT_TRUE(observer.FindCharacter(character, row));
    EXPECT_EQ(row.residentContinent, std::string("alpha"));
    EXPECT_TRUE(row.migrationOperation.empty());
    EXPECT_TRUE(observer.Close());
    EXPECT_TRUE(destinationDb.Close());
    EXPECT_TRUE(sourceDb.Close());
    fs::remove(path);
}

TEST(TF120_Migration_AbortIsTerminalAndRetryCanReserveAgain)
{
    const fs::path path = FreshPath("test_tf120_migration_abort.db");
    TFDatabase sourceDb;
    TFDatabase destinationDb;
    ASSERT_TRUE(sourceDb.Open(path));
    const uint64_t character = Seed(sourceDb);
    ASSERT_TRUE(character != 0);
    ASSERT_TRUE(sourceDb.Close());
    ASSERT_TRUE(sourceDb.Open(path));
    ASSERT_TRUE(destinationDb.Open(path));
    ASSERT_TRUE(Bind(sourceDb, "alpha"));
    ASSERT_TRUE(Bind(destinationDb, "beta"));
    TFCharacterRecord claimed;
    ASSERT_TRUE(sourceDb.ClaimCharacter(character, claimed));
    Authority sourceAuthority;
    Authority destinationAuthority;
    TFHandoffParticipant source(sourceDb, sourceAuthority);
    TFHandoffParticipant destination(destinationDb, destinationAuthority);
    const HandoffRequest first = Request(character, 1);
    const HandoffRequest retry = Request(character, 2);

    EXPECT_TRUE(source.Prepare(first) == HandoffResult::Applied);
    EXPECT_TRUE(source.Abort(first) == HandoffResult::Applied);
    EXPECT_TRUE(source.Abort(first) == HandoffResult::Applied);
    ASSERT_TRUE(sourceDb.ReleaseCharacter(character));
    EXPECT_TRUE(source.Abort(first) == HandoffResult::Duplicate);
    EXPECT_TRUE(destination.Abort(first) == HandoffResult::Duplicate);
    ASSERT_TRUE(sourceDb.ClaimCharacter(character, claimed));
    EXPECT_TRUE(source.Prepare(retry) == HandoffResult::Applied);
    EXPECT_TRUE(destination.Commit(retry) == HandoffResult::Applied);
    EXPECT_EQ(destinationAuthority.installCount, 1);
    EXPECT_TRUE(sourceDb.Close());
    EXPECT_TRUE(destinationDb.Close());
    fs::remove(path);
}

// A source that dies holding a reservation is recovered by its own restart (BindAuthority). The recovered
// abort must keep the gateway epoch it was reserved under: the gateway retries the lost Abort at that epoch,
// and the next handoff arrives at epoch + 1, not at a database revision.
TEST(TF120_Migration_SourceRestartRecoveryKeepsGatewayEpoch)
{
    const fs::path path = FreshPath("test_tf120_migration_restart.db");
    TFDatabase seedDb;
    ASSERT_TRUE(seedDb.Open(path));
    const uint64_t character = Seed(seedDb);
    ASSERT_TRUE(character != 0);
    ASSERT_TRUE(seedDb.Close());

    const HandoffRequest reserved = Request(character, 2);
    {
        TFDatabase crashedSourceDb;
        ASSERT_TRUE(crashedSourceDb.Open(path));
        ASSERT_TRUE(Bind(crashedSourceDb, "alpha"));
        TFCharacterRecord claimed;
        ASSERT_TRUE(crashedSourceDb.ClaimCharacter(character, claimed));
        Authority crashedAuthority;
        TFHandoffParticipant crashedSource(crashedSourceDb, crashedAuthority);
        ASSERT_TRUE(crashedSource.Prepare(reserved) == HandoffResult::Applied);
        // The process dies here: no abort, no release.
        EXPECT_TRUE(crashedSourceDb.Close());
    }

    TFDatabase sourceDb;
    TFDatabase destinationDb;
    ASSERT_TRUE(sourceDb.Open(path));
    ASSERT_TRUE(destinationDb.Open(path));
    ASSERT_TRUE(Bind(sourceDb, "alpha")); // restart recovery aborts the orphaned reservation
    ASSERT_TRUE(Bind(destinationDb, "beta"));
    TFCharacterRecord row;
    ASSERT_TRUE(sourceDb.FindCharacter(character, row));
    EXPECT_EQ(row.migrationState, std::string("rolled_back"));
    EXPECT_EQ(row.migrationEpoch, reserved.epoch);
    EXPECT_TRUE(row.residentContinent.empty());

    Authority sourceAuthority;
    Authority destinationAuthority;
    TFHandoffParticipant source(sourceDb, sourceAuthority);
    TFHandoffParticipant destination(destinationDb, destinationAuthority);
    EXPECT_TRUE(destination.Commit(reserved) == HandoffResult::Rejected);
    EXPECT_TRUE(source.Abort(reserved) == HandoffResult::Duplicate);

    TFCharacterRecord claimed;
    ASSERT_TRUE(sourceDb.ClaimCharacter(character, claimed));
    const HandoffRequest next = Request(character, reserved.epoch + 1);
    EXPECT_TRUE(source.Prepare(next) == HandoffResult::Applied);
    EXPECT_TRUE(destination.Commit(next) == HandoffResult::Applied);
    EXPECT_EQ(destinationAuthority.installCount, 1);
    EXPECT_TRUE(sourceDb.Close());
    EXPECT_TRUE(destinationDb.Close());
    fs::remove(path);
}

TEST(TF120_Migration_CorruptCheckpointLeavesDecodeOutputUntouched)
{
    TFHandoffState state;
    state.player = 77;
    state.cls = ClassId::Striker;
    state.health = 100.0f;
    state.shield = 10.0f;
    const TFHandoffState before = state;
    EXPECT_FALSE(TFHandoffState::Decode("TFH1", state));
    EXPECT_EQ(state.player, before.player);
    EXPECT_EQ(static_cast<int>(state.cls), static_cast<int>(before.cls));
    EXPECT_EQ(state.health, before.health);
    EXPECT_EQ(state.shield, before.shield);
}

// Every character the module can hold migrates through the production participants and database. Every tenth
// Commit request is lost before delivery (the gateway retries it) and every tenth reply is lost after the
// destination applied it (the gateway redelivers it). Neither may lose a character or give it two owners.
TEST(TF120_Migration_FullCapacityWithLostDeliveriesHasOneOwnerEach)
{
    const fs::path path = FreshPath("test_tf120_migration_capacity.db");
    TFDatabase sourceDb;
    TFDatabase destinationDb;
    ASSERT_TRUE(sourceDb.Open(path));
    std::vector<uint64_t> characters;
    characters.reserve(kMaxPlayers);
    for (uint32_t index = 0; index < kMaxPlayers; ++index)
    {
        TFAccountRecord account;
        TFCharacterRecord character;
        const std::string suffix = std::to_string(index);
        ASSERT_TRUE(sourceDb.CreateAccount("capacity-user-" + suffix, "salt", "hash", account));
        ASSERT_TRUE(sourceDb.CreateCharacter(account.id, "Capacity Hero " + suffix, FactionId::MRA, character));
        characters.push_back(character.id);
    }
    ASSERT_TRUE(sourceDb.Close());
    ASSERT_TRUE(sourceDb.Open(path));
    ASSERT_TRUE(destinationDb.Open(path));
    ASSERT_TRUE(Bind(sourceDb, "alpha"));
    ASSERT_TRUE(Bind(destinationDb, "beta"));

    Authority sourceAuthority;
    Authority destinationAuthority;
    TFHandoffParticipant source(sourceDb, sourceAuthority);
    TFHandoffParticipant destination(destinationDb, destinationAuthority);
    uint32_t lostRequests = 0;
    uint32_t lostReplies = 0;
    for (size_t index = 0; index < characters.size(); ++index)
    {
        TFCharacterRecord claimed;
        ASSERT_TRUE(sourceDb.ClaimCharacter(characters[index], claimed));
        const HandoffRequest request = Request(characters[index]);
        EXPECT_TRUE(source.Prepare(request) == HandoffResult::Applied);
        EXPECT_TRUE(destination.Prepare(request) == HandoffResult::Applied);
        EXPECT_TRUE(source.Transfer(request) == HandoffResult::Applied);
        EXPECT_TRUE(destination.Transfer(request) == HandoffResult::Applied);
        EXPECT_TRUE(source.Commit(request) == HandoffResult::Applied);
        if (index % 10 == 0)
        {
            // The destination never saw this Commit: the source still owns the reserved character.
            ++lostRequests;
            TFCharacterRecord stillOwned;
            ASSERT_TRUE(sourceDb.FindCharacter(characters[index], stillOwned));
            EXPECT_EQ(stillOwned.residentContinent, std::string("alpha"));
            EXPECT_EQ(stillOwned.migrationState, std::string("reserved"));
        }
        EXPECT_TRUE(destination.Commit(request) == HandoffResult::Applied);
        if (index % 10 == 5)
        {
            ++lostReplies;
            EXPECT_TRUE(destination.Commit(request) == HandoffResult::Duplicate);
        }
        EXPECT_TRUE(source.Acknowledge(request) == HandoffResult::Applied);
        EXPECT_TRUE(destination.Acknowledge(request) == HandoffResult::Applied);
    }

    std::printf("[TF120] characters=%u lostRequests=%u lostReplies=%u owners=%zu installs=%d\n", kMaxPlayers,
                lostRequests, lostReplies, destinationAuthority.installedStates.size(),
                destinationAuthority.installCount);
    EXPECT_TRUE(lostRequests > 0 && lostReplies > 0);
    EXPECT_EQ(destinationAuthority.installedStates.size(), static_cast<size_t>(kMaxPlayers));
    EXPECT_EQ(destinationAuthority.installCount, static_cast<int>(kMaxPlayers));
    EXPECT_EQ(sourceAuthority.captureCount, static_cast<int>(kMaxPlayers));

    TFDatabase observer;
    ASSERT_TRUE(observer.Open(path));
    for (const uint64_t character : characters)
    {
        TFCharacterRecord row;
        ASSERT_TRUE(observer.FindCharacter(character, row));
        EXPECT_EQ(row.residentContinent, std::string("beta"));
        EXPECT_TRUE(row.migrationOperation.empty());
        EXPECT_EQ(row.migrationState, std::string("committed"));
        TFHandoffState checkpoint;
        ASSERT_TRUE(TFHandoffState::Decode(row.migrationPayload, checkpoint));
        ASSERT_TRUE(destinationAuthority.installedStates.contains(character));
        EXPECT_EQ(checkpoint.Encode(), destinationAuthority.installedStates.at(character).Encode());
    }
    EXPECT_TRUE(observer.Close());
    EXPECT_TRUE(destinationDb.Close());
    EXPECT_TRUE(sourceDb.Close());
    fs::remove(path);
}
