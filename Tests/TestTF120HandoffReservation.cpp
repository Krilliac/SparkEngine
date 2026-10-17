/**
 * @file TestTF120HandoffReservation.cpp
 * @brief TF-120 durable migration reservation and exactly-once commit coverage.
 */
#include "TestFramework.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFSavePaths.h"

#include <filesystem>

using namespace Terrafront;

namespace
{
    constexpr const char* kSource = "cindral_wastes";
    constexpr const char* kDestination = "veyra_highlands";

    std::filesystem::path FreshPath()
    {
        const std::filesystem::path path = std::filesystem::path("Saves") / "test_tf120_handoff_reservation.db";
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::remove(path);
        std::filesystem::remove_all(std::filesystem::path(path.wstring() + L".tmp"));
        return path;
    }
} // namespace

TEST(TF120_HandoffReservation_DuplicateReorderAndCommitAreExactlyOnce)
{
    const std::filesystem::path path = FreshPath();
    TFDatabase source;
    TFDatabase destination;
    ASSERT_TRUE(source.Open(path) && source.BindAuthority(kSource));
    ASSERT_TRUE(destination.Open(path) && destination.BindAuthority(kDestination));

    TFAccountRecord account;
    TFCharacterRecord character;
    ASSERT_TRUE(source.CreateAccount("handoff", "salt", "hash", account));
    ASSERT_TRUE(source.CreateCharacter(account.id, "Handoff", FactionId::MRA, character));
    ASSERT_TRUE(source.ClaimCharacter(character.id, character));

    TFCharacterRecord reserved;
    ASSERT_TRUE(source.ReserveMigration(character.id, "op-1", 10, kDestination, "move-state-v1", reserved));
    const std::filesystem::path reservedPath = path;
    const auto reservedRevision = reserved.revision;
    EXPECT_FALSE(source.SaveCharacterProgress(character.id, 88, 2, 9, 1));
    EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_FALSE(source.ReleaseCharacter(character.id));
    EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);

    TFCharacterRecord duplicate;
    ASSERT_TRUE(source.ReserveMigration(character.id, "op-1", 10, kDestination, "move-state-v1", duplicate));
    EXPECT_EQ(duplicate.revision, reservedRevision);

    // Reordered destination enter messages cannot steal a reserved row.
    TFCharacterRecord claimedEarly;
    EXPECT_FALSE(destination.ClaimCharacter(character.id, claimedEarly));
    EXPECT_TRUE(destination.LastStatus() == TFDatabaseStatus::ResidentElsewhere);

    TFCharacterRecord committed;
    ASSERT_TRUE(destination.CommitMigration(character.id, "op-1", committed));
    EXPECT_EQ(committed.residentContinent, std::string(kDestination));
    EXPECT_EQ(committed.migrationPayload, std::string("move-state-v1"));
    EXPECT_TRUE(committed.migrationOperation.empty());

    // Duplicate commit is an idempotent acknowledgement, not a second ownership transition.
    TFCharacterRecord duplicateCommit;
    ASSERT_TRUE(destination.CommitMigration(character.id, "op-1", duplicateCommit));
    EXPECT_EQ(duplicateCommit.revision, committed.revision);
    EXPECT_EQ(duplicateCommit.residentContinent, std::string(kDestination));

    // The source's pre-handoff baseline is fenced after destination commit.
    EXPECT_FALSE(source.SaveCharacterProgress(character.id, 99, 2, 10, 1));
    EXPECT_TRUE(source.LastStatus() == TFDatabaseStatus::Conflict);

    TFDatabase observer;
    TFCharacterRecord durable;
    ASSERT_TRUE(observer.Open(reservedPath) && observer.FindCharacter(character.id, durable));
    EXPECT_EQ(durable.residentContinent, std::string(kDestination));
    EXPECT_TRUE(durable.migrationOperation.empty());
}

TEST(TF120_HandoffReservation_AbortReleasesSourceOnly)
{
    const std::filesystem::path path = FreshPath();
    TFDatabase source;
    TFDatabase destination;
    ASSERT_TRUE(source.Open(path) && source.BindAuthority(kSource));
    ASSERT_TRUE(destination.Open(path) && destination.BindAuthority(kDestination));

    TFAccountRecord account;
    TFCharacterRecord character;
    ASSERT_TRUE(source.CreateAccount("abort", "salt", "hash", account));
    ASSERT_TRUE(source.CreateCharacter(account.id, "Abort", FactionId::MRA, character));
    ASSERT_TRUE(source.ClaimCharacter(character.id, character));
    ASSERT_TRUE(source.ReserveMigration(character.id, "op-abort", 20, kDestination, "abort-state", character));
    ASSERT_TRUE(source.AbortMigration(character.id, "op-abort"));
    ASSERT_TRUE(source.AbortMigration(character.id, "op-abort"));
    EXPECT_FALSE(source.ReserveMigration(character.id, "op-abort", 20, kDestination, "abort-state", character));
    ASSERT_TRUE(source.ReserveMigration(character.id, "op-next", 21, kDestination, "next-state", character));
    ASSERT_TRUE(source.AbortMigration(character.id, "op-next"));

    TFCharacterRecord row;
    ASSERT_TRUE(source.FindCharacter(character.id, row));
    EXPECT_EQ(row.residentContinent, std::string(kSource));
    EXPECT_TRUE(row.migrationOperation.empty());
    EXPECT_FALSE(destination.CommitMigration(character.id, "op-abort", row));
}

TEST(TF120_HandoffReservation_AbortMissingCharacterFails)
{
    const std::filesystem::path path = FreshPath();
    TFDatabase source;
    ASSERT_TRUE(source.Open(path) && source.BindAuthority(kSource));
    EXPECT_FALSE(source.AbortMigration(9999, "missing"));
}
