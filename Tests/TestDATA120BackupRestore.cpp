/**
 * @file TestDATA120BackupRestore.cpp
 * @brief DATA-120: TFDatabase backup/restore and the forced-failure recovery drill.
 *
 * Persistence_BackupRestore_* drive the production TFDatabase::CreateBackup /
 * RestoreFromBackup against real files: round trip, digest and schema gates,
 * N-1 backups, never-overwrite and alias refusal, the authority lock, recovery
 * of a quarantined corrupt primary, and a running authority that must not
 * serve the rolled-back rows.
 *
 * Persistence_RecoveryDrill_* (POSIX and Windows) spawn a fresh SparkTests
 * process (exec, not a bare fork of this multi-threaded runner) that really
 * dies inside a commit: it _exit()s at a SavePaths::DurableCommitStage
 * (between staging and rename, and between rename and directory sync), or is
 * killed (SIGKILL / TerminateProcess) while committing in a loop. The parent
 * must then reopen the store at the documented recovery point
 * (docs/specs/persistence.md) and keep committing. The TFDatabase drills are
 * joined by one per other durable store: TFOutfitStore, the TFSocialSystem
 * store and the WorldSave territory file.
 */
#include "TestFramework.h"
#include "Account/TFCrypto.h"
#include "Game/TFSocialSystem.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFOutfitStore.h"
#include "Persistence/TFSavePaths.h"
#include "Persistence/TFWorldSave.h"
#include "Utils/JsonUtils.h"
#include "Utils/Process.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

using namespace Terrafront;

namespace
{
    namespace fs = std::filesystem;

    /// Empty per-test directory under Saves/ (the other DATA-120 fixtures' root).
    fs::path FreshDir(const char* name)
    {
        const fs::path dir = fs::path("Saves") / name;
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
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

    fs::path WithSuffix(const fs::path& path, const char* suffix)
    {
        fs::path out = path;
        out += suffix;
        return out;
    }

    /// Write `text` as a backup with a correct sidecar, bypassing CreateBackup.
    void WriteBackupWithDigest(const fs::path& backup, const std::string& text)
    {
        WriteFile(backup, text);
        const Crypto::Sha256Digest digest = Crypto::Sha256(text);
        WriteFile(TFDatabase::BackupDigestPath(backup),
                  Crypto::ToHex(digest.data(), digest.size()) + "  " + backup.filename().string() + "\n");
    }

    /// Account + character with a flux balance; returns the character id (0 on failure).
    uint64_t Seed(TFDatabase& db, const char* user, const char* name, uint32_t flux)
    {
        TFAccountRecord account;
        TFCharacterRecord character;
        if (!db.CreateAccount(user, "salt", "hash", account) ||
            !db.CreateCharacter(account.id, name, FactionId::MRA, character))
            return 0;
        return db.SaveCharacterProgress(character.id, 0, 1, flux, 0) ? character.id : 0;
    }

    /// Flux of `charId` as a fresh authority sees it, or -1 if it cannot open/find it.
    int64_t FluxOnDisk(const fs::path& path, uint64_t charId)
    {
        TFDatabase db;
        TFCharacterRecord row;
        if (!db.Open(path) || !db.FindCharacter(charId, row))
            return -1;
        return row.flux;
    }

    uint64_t FileRevision(const fs::path& path)
    {
        Spark::Json::Value root;
        if (!Spark::Json::ParseStrict(ReadFile(path), &root) || !root["revision"].IsNumber())
            return 0;
        return static_cast<uint64_t>(root["revision"].AsNumber(0.0));
    }
} // namespace

TEST(Persistence_BackupRestore_RoundTripRestoresBackupPointAndKeepsDisplacedPrimary)
{
    const fs::path dir = FreshDir("test_data120_backup_roundtrip");
    const fs::path db = dir / "terrafront.db";
    const fs::path backup = dir / "backups" / "terrafront.db.bak";

    uint64_t charId = 0;
    uint64_t revisionBeforeRestore = 0;
    TFBackupInfo backupInfo;
    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        charId = Seed(authority, "restore_user", "Restorer", 100);
        ASSERT_TRUE(charId != 0);

        ASSERT_TRUE(TFDatabase::CreateBackup(db, backup, backupInfo) == TFBackupStatus::Ok);
        EXPECT_EQ(backupInfo.revision, FileRevision(db));
        EXPECT_EQ(backupInfo.schemaVersion, TFDatabase::kSchemaVersion);
        EXPECT_TRUE(ReadFile(backup) == ReadFile(db));
        EXPECT_EQ(backupInfo.sizeBytes, static_cast<uint64_t>(fs::file_size(backup)));
        EXPECT_TRUE(ReadFile(TFDatabase::BackupDigestPath(backup)) == backupInfo.sha256 + "  terrafront.db.bak\n");
        EXPECT_EQ(backupInfo.sha256.size(), size_t{64});

        // Everything after the backup point is what a restore rolls back.
        ASSERT_TRUE(authority.SaveCharacterProgress(charId, 0, 1, 25, 0));
        TFCharacterRecord later;
        TFAccountRecord laterAccount;
        ASSERT_TRUE(authority.CreateAccount("late_user", "salt", "hash", laterAccount));
        ASSERT_TRUE(authority.CreateCharacter(laterAccount.id, "Latecomer", FactionId::MRA, later));
        revisionBeforeRestore = FileRevision(db);
        EXPECT_TRUE(authority.Close());
    }
    const std::string displacedBytes = ReadFile(db);

    TFBackupInfo restoreInfo;
    ASSERT_TRUE(TFDatabase::RestoreFromBackup(backup, db, restoreInfo) == TFBackupStatus::Ok);
    EXPECT_GT(restoreInfo.revision, revisionBeforeRestore);
    EXPECT_EQ(FileRevision(db), restoreInfo.revision);
    EXPECT_TRUE(restoreInfo.sha256 == backupInfo.sha256);
    EXPECT_TRUE(restoreInfo.supersedesPrimaryRevision);
    ASSERT_FALSE(restoreInfo.displacedPrimary.empty());
    EXPECT_TRUE(ReadFile(restoreInfo.displacedPrimary) == displacedBytes);

    TFDatabase reopened;
    ASSERT_TRUE(reopened.Open(db));
    TFCharacterRecord row;
    ASSERT_TRUE(reopened.FindCharacter(charId, row));
    EXPECT_EQ(row.flux, uint32_t{100});
    EXPECT_FALSE(reopened.FindCharacterByName("Latecomer", row));
    TFAccountRecord account;
    EXPECT_TRUE(reopened.FindAccountByUsername("restore_user", account));
    EXPECT_FALSE(reopened.FindAccountByUsername("late_user", account));

    // The restored database keeps serving writes, and revisions keep moving forward.
    TFAccountRecord fresh;
    EXPECT_TRUE(reopened.CreateAccount("post_restore", "salt", "hash", fresh));
    EXPECT_TRUE(reopened.SaveCharacterProgress(charId, 0, 1, 130, 0));
    EXPECT_GT(FileRevision(db), restoreInfo.revision);
    fs::remove_all(dir);
}

TEST(Persistence_BackupRestore_TamperedOrUndigestedBackupIsRefusedAndPrimaryUntouched)
{
    const fs::path dir = FreshDir("test_data120_backup_tamper");
    const fs::path db = dir / "terrafront.db";
    const fs::path backup = dir / "terrafront.db.bak";
    uint64_t charId = 0;
    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        charId = Seed(authority, "tamper_user", "Tamper", 100);
        ASSERT_TRUE(charId != 0);
        TFBackupInfo info;
        ASSERT_TRUE(TFDatabase::CreateBackup(db, backup, info) == TFBackupStatus::Ok);
        ASSERT_TRUE(authority.SaveCharacterProgress(charId, 0, 1, 60, 0));
    }
    const std::string primary = ReadFile(db);

    // A still-valid JSON edit (flux 100 -> 900) is caught by the digest, not the parser.
    std::string edited = ReadFile(backup);
    const size_t at = edited.find("\"flux\": 100");
    ASSERT_TRUE(at != std::string::npos);
    edited.replace(at, 11, "\"flux\": 900");
    WriteFile(backup, edited);

    TFBackupInfo info;
    EXPECT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::DigestMismatch);
    EXPECT_TRUE(ReadFile(db) == primary);

    WriteFile(TFDatabase::BackupDigestPath(backup), "not a digest\n");
    EXPECT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::DigestMissing);
    fs::remove(TFDatabase::BackupDigestPath(backup));
    EXPECT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::DigestMissing);
    EXPECT_TRUE(TFDatabase::RestoreFromBackup(dir / "absent.bak", db, info) == TFBackupStatus::BackupMissing);

    // Digest-consistent garbage still fails the load validation.
    WriteBackupWithDigest(backup, "{\"accounts\": [], \"characters\": [");
    EXPECT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::BackupCorrupt);

    EXPECT_TRUE(ReadFile(db) == primary);
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{60});
    size_t displaced = 0;
    for (const auto& entry : fs::directory_iterator(dir))
        displaced += entry.path().filename().string().find(".pre-restore-") != std::string::npos ? 1 : 0;
    EXPECT_EQ(displaced, size_t{0});
    fs::remove_all(dir);
}

TEST(Persistence_BackupRestore_SchemaGateRefusesNewerAndMigratesOlderBackup)
{
    const fs::path dir = FreshDir("test_data120_backup_schema");
    const fs::path db = dir / "terrafront.db";
    const fs::path backup = dir / "terrafront.db.bak";
    const std::string rows = "\"nextAccountId\": 2, \"nextCharId\": 2, "
                             "\"accounts\": [{\"id\": 1, \"username\": \"n1_user\", \"salt\": \"s\", "
                             "\"passwordHash\": \"h\", \"createdAtMs\": 1, \"lastLoginMs\": 2}], "
                             "\"characters\": [{\"id\": 1, \"accountId\": 1, \"name\": \"Elder\", \"faction\": 1, "
                             "\"xp\": 5, \"rank\": 1, \"flux\": 42, \"createdAtMs\": 1, \"lastPlayedMs\": 2}]";

    // A newer build's backup must never be loaded and rewritten by this build.
    const std::string newer =
        "{\"schemaVersion\": " + std::to_string(TFDatabase::kSchemaVersion + 1) + ", \"revision\": 3, " + rows + "}";
    WriteBackupWithDigest(backup, newer);
    TFBackupInfo info;
    EXPECT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::BackupUnsupportedVersion);
    EXPECT_FALSE(fs::exists(db));

    // Nor may a newer-schema primary be backed up as if this build understood it.
    WriteFile(db, newer);
    EXPECT_TRUE(TFDatabase::CreateBackup(db, dir / "newer.bak", info) == TFBackupStatus::SourceUnsupportedVersion);
    EXPECT_FALSE(fs::exists(dir / "newer.bak"));
    fs::remove(db);

    // An N-1 backup restores and is written back in the current schema. N-1 is v2: v3 is retired.
    fs::remove(backup);
    fs::remove(TFDatabase::BackupDigestPath(backup));
    constexpr uint32_t kPreviousSchema = 2;
    const std::string previous =
        "{\"schemaVersion\": " + std::to_string(kPreviousSchema) + ", \"revision\": 3, " + rows + "}";
    WriteBackupWithDigest(backup, previous);
    ASSERT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::Ok);
    EXPECT_EQ(info.schemaVersion, kPreviousSchema);
    EXPECT_EQ(info.revision, uint64_t{4});
    EXPECT_TRUE(info.displacedPrimary.empty());
    EXPECT_FALSE(info.supersedesPrimaryRevision);

    Spark::Json::Value root;
    ASSERT_TRUE(Spark::Json::ParseStrict(ReadFile(db), &root));
    EXPECT_EQ(static_cast<uint32_t>(root["schemaVersion"].AsNumber(0.0)), TFDatabase::kSchemaVersion);
    EXPECT_EQ(FluxOnDisk(db, 1), int64_t{42});
    fs::remove_all(dir);
}

TEST(Persistence_BackupRestore_NeverOverwritesAliasesOrBacksUpNothing)
{
    const fs::path dir = FreshDir("test_data120_backup_refusals");
    const fs::path db = dir / "terrafront.db";
    TFBackupInfo info;
    EXPECT_TRUE(TFDatabase::CreateBackup(db, dir / "a.bak", info) == TFBackupStatus::SourceMissing);
    EXPECT_FALSE(fs::exists(dir / "a.bak"));

    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        ASSERT_TRUE(Seed(authority, "alias_user", "Alias", 10) != 0);
    }
    ASSERT_TRUE(TFDatabase::CreateBackup(db, dir / "a.bak", info) == TFBackupStatus::Ok);
    const std::string firstBackup = ReadFile(dir / "a.bak");
    EXPECT_TRUE(TFDatabase::CreateBackup(db, dir / "a.bak", info) == TFBackupStatus::TargetExists);
    EXPECT_TRUE(ReadFile(dir / "a.bak") == firstBackup);

    // An orphan sidecar also blocks the name: the pair must be written together.
    WriteFile(TFDatabase::BackupDigestPath(dir / "b.bak"), "orphan");
    EXPECT_TRUE(TFDatabase::CreateBackup(db, dir / "b.bak", info) == TFBackupStatus::TargetExists);
    EXPECT_FALSE(fs::exists(dir / "b.bak"));

    // The db itself, its staging file (unlinked by the next commit) and its lock are never backup targets.
    EXPECT_TRUE(TFDatabase::CreateBackup(db, db, info) == TFBackupStatus::InvalidPath);
    EXPECT_TRUE(TFDatabase::CreateBackup(db, WithSuffix(db, ".tmp"), info) == TFBackupStatus::InvalidPath);
    EXPECT_TRUE(TFDatabase::CreateBackup(db, dir / "." / "terrafront.db.lock", info) == TFBackupStatus::InvalidPath);
    EXPECT_TRUE(TFDatabase::CreateBackup(db, fs::path{}, info) == TFBackupStatus::InvalidPath);
    EXPECT_TRUE(TFDatabase::RestoreFromBackup(db, db, info) == TFBackupStatus::InvalidPath);
    fs::remove_all(dir);
}

TEST(Persistence_BackupRestore_BackupAndRestoreWaitForTheAuthorityLock)
{
    const fs::path dir = FreshDir("test_data120_backup_lock");
    const fs::path db = dir / "terrafront.db";
    const fs::path backup = dir / "terrafront.db.bak";
    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        ASSERT_TRUE(Seed(authority, "lock_user", "Locker", 10) != 0);
    }
    TFBackupInfo info;
    ASSERT_TRUE(TFDatabase::CreateBackup(db, backup, info) == TFBackupStatus::Ok);
    const std::string primary = ReadFile(db);
    {
        // An in-flight transaction of another authority holds this lock.
        SavePaths::ExclusiveFileLock transaction;
        std::error_code ec;
        ASSERT_TRUE(transaction.TryLock(db, ec));
        EXPECT_TRUE(TFDatabase::CreateBackup(db, dir / "blocked.bak", info) == TFBackupStatus::Locked);
        EXPECT_FALSE(fs::exists(dir / "blocked.bak"));
        EXPECT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::Locked);
        EXPECT_TRUE(ReadFile(db) == primary);
    }
    EXPECT_TRUE(TFDatabase::CreateBackup(db, dir / "blocked.bak", info) == TFBackupStatus::Ok);
    fs::remove_all(dir);
}

TEST(Persistence_BackupRestore_RestoreRecoversQuarantinedCorruptPrimary)
{
    const fs::path dir = FreshDir("test_data120_backup_corrupt");
    const fs::path db = dir / "terrafront.db";
    const fs::path backup = dir / "terrafront.db.bak";
    uint64_t charId = 0;
    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        charId = Seed(authority, "corrupt_user", "Survivor", 70);
        ASSERT_TRUE(charId != 0);
    }
    TFBackupInfo info;
    ASSERT_TRUE(TFDatabase::CreateBackup(db, backup, info) == TFBackupStatus::Ok);
    const uint64_t backupRevision = info.revision;

    // Torn primary with a revision far past the backup's.
    const std::string torn = "{\"schemaVersion\": 1, \"revision\": 500, \"accounts\": [";
    WriteFile(db, torn);
    TFDatabase broken;
    EXPECT_FALSE(broken.Open(db));
    EXPECT_TRUE(broken.LastStatus() == TFDatabaseStatus::Corrupt);
    EXPECT_TRUE(TFDatabase::CreateBackup(db, dir / "torn.bak", info) == TFBackupStatus::SourceCorrupt);

    ASSERT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::Ok);
    EXPECT_TRUE(ReadFile(info.displacedPrimary) == torn);
    // The torn file's strict parse fails, so its revision is unknown: the restore
    // can only move past the backup, and it says so instead of claiming to
    // supersede the lost history (operators must restart every authority).
    EXPECT_EQ(info.revision, backupRevision + 1);
    EXPECT_FALSE(info.supersedesPrimaryRevision);
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{70});

    // A primary that parses but fails validation still carries a readable
    // revision, and the restore stamps above it.
    const std::string invalid = "{\"schemaVersion\": " + std::to_string(TFDatabase::kSchemaVersion) +
                                ", \"revision\": 900, \"accounts\": \"not-an-array\"}";
    WriteFile(db, invalid);
    TFDatabase rejected;
    EXPECT_FALSE(rejected.Open(db));
    ASSERT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::Ok);
    EXPECT_TRUE(ReadFile(info.displacedPrimary) == invalid);
    EXPECT_EQ(info.revision, uint64_t{901});
    EXPECT_TRUE(info.supersedesPrimaryRevision);
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{70});
    fs::remove_all(dir);
}

TEST(Persistence_BackupRestore_RunningAuthorityCannotOverwriteRestoredRows)
{
    const fs::path dir = FreshDir("test_data120_backup_running");
    const fs::path db = dir / "terrafront.db";
    const fs::path backup = dir / "terrafront.db.bak";

    TFDatabase authority;
    ASSERT_TRUE(authority.Open(db));
    const uint64_t charId = Seed(authority, "running_user", "Runner", 100);
    ASSERT_TRUE(charId != 0);
    TFBackupInfo info;
    ASSERT_TRUE(TFDatabase::CreateBackup(db, backup, info) == TFBackupStatus::Ok);
    ASSERT_TRUE(authority.SaveCharacterProgress(charId, 0, 1, 20, 0));

    ASSERT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::Ok);

    // Its baseline is the post-backup row; the restored row carries an older revision, so the
    // stale absolute write is a Conflict instead of silently undoing the restore.
    EXPECT_FALSE(authority.SaveCharacterProgress(charId, 0, 1, 5, 0));
    EXPECT_TRUE(authority.LastStatus() == TFDatabaseStatus::Conflict);
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{100});

    TFCharacterRecord reacquired;
    ASSERT_TRUE(authority.AcquireCharacter(charId, reacquired));
    EXPECT_EQ(reacquired.flux, uint32_t{100});
    EXPECT_TRUE(authority.SaveCharacterProgress(charId, 0, 1, 90, 0));
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{90});
    authority.Close();
    fs::remove_all(dir);
}

namespace
{
    constexpr int kCrashExitCode = 86;
    constexpr int kAckTarget = 40;

    constexpr const char* kDrillRoleEnv = "SPARK_DATA120_DRILL_ROLE";
    constexpr const char* kDrillDbEnv = "SPARK_DATA120_DRILL_DB";
    constexpr const char* kDrillBackupEnv = "SPARK_DATA120_DRILL_BACKUP";
    constexpr const char* kDrillCharEnv = "SPARK_DATA120_DRILL_CHAR";
    /// The parent's test selection, dropped so the child runs exactly one test.
    constexpr const char* kParentSelection[] = {"SPARK_TEST_FILE", "SPARK_TEST_EXPECT_COUNT", "SPARK_TEST_EXCLUDE",
                                                "SPARK_TEST_LIMIT"};

    SavePaths::DurableCommitStage s_crashStage = SavePaths::DurableCommitStage::StagedAndSynced;
    fs::path s_crashTarget;

    std::string EnvOrEmpty(const char* name)
    {
        const char* value = std::getenv(name);
        return value ? value : "";
    }

    fs::path TestBinaryPath()
    {
#ifdef _WIN32
        std::wstring buffer(512, L'\0');
        for (;;)
        {
            const DWORD size = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (size == 0)
                return {};
            if (size < buffer.size() - 1)
            {
                buffer.resize(size);
                return fs::path(buffer);
            }
            if (buffer.size() >= 32768)
                return {};
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__APPLE__)
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

#ifdef _WIN32
    /// Sets (or, for an empty value, removes) one variable of this process's environment and restores it.
    /// Spark::Process::Builder has no environment API; a child launched inside the scope inherits it.
    class ScopedEnvironmentVariable
    {
      public:
        ScopedEnvironmentVariable(const char* name, const std::string& value) : m_name(name)
        {
            if (const char* previous = std::getenv(name))
            {
                m_wasSet = true;
                m_previous = previous;
            }
            _putenv_s(m_name, value.c_str());
        }
        ~ScopedEnvironmentVariable() { _putenv_s(m_name, m_wasSet ? m_previous.c_str() : ""); }
        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

      private:
        const char* m_name;
        bool m_wasSet = false;
        std::string m_previous;
    };

    /// UTF-8 form of a path, as Spark::Process::Builder expects on Windows.
    std::string Utf8(const fs::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }
#endif

    /// Launch a fresh SparkTests process that runs only `testName`, which sees
    /// `role` in SPARK_DATA120_DRILL_ROLE and plays the dying writer. The child
    /// is exec'd rather than a bare fork() of this runner: the runner's other
    /// threads (async logger, job workers) may hold locks at fork time, and a
    /// forked child that logs or allocates could deadlock. On Windows the child
    /// inherits a scoped copy of this process's environment (the drill CTests
    /// run RUN_SERIAL, so nothing else reads it meanwhile); on POSIX it goes
    /// through env(1).
    std::expected<Spark::Process, std::string> SpawnDrillChild(const char* testName, const std::string& role,
                                                               const fs::path& db, uint64_t charId,
                                                               const fs::path& backup = {})
    {
        const fs::path self = TestBinaryPath();
        if (self.empty())
            return std::unexpected(std::string("cannot resolve the test binary path"));

        const std::pair<const char*, std::string> drillEnvironment[] = {
            {"SPARK_TEST_NAME", testName},
            {kDrillRoleEnv, role},
            {kDrillDbEnv, fs::absolute(db).string()},
            {kDrillBackupEnv, backup.empty() ? std::string() : fs::absolute(backup).string()},
            {kDrillCharEnv, std::to_string(charId)},
        };
#ifdef _WIN32
        std::vector<std::unique_ptr<ScopedEnvironmentVariable>> scoped;
        for (const char* selection : kParentSelection)
            scoped.push_back(std::make_unique<ScopedEnvironmentVariable>(selection, std::string()));
        for (const auto& [name, value] : drillEnvironment)
            scoped.push_back(std::make_unique<ScopedEnvironmentVariable>(name, value));
        Spark::Process::Builder builder(Utf8(self));
        builder.WorkingDirectory(Utf8(fs::current_path()));
#else
        Spark::Process::Builder builder("env");
        for (const char* selection : kParentSelection)
            builder.Arg("-u").Arg(selection);
        for (const auto& [name, value] : drillEnvironment)
            builder.Arg(std::string(name) + "=" + value);
        builder.Arg(self.string()).WorkingDirectory(fs::current_path().string());
#endif
        builder.CaptureStdout().MergeStderrIntoStdout();
        return builder.Launch();
    }

    /// Wait up to 60 s for the child while draining its output (so a full pipe
    /// never stalls it); SIGKILL it at the deadline. Returns its exit code, or
    /// -1 when it was killed or died on a signal.
    int WaitForChild(Spark::Process& child, std::string& log)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        std::string line;
        while (std::chrono::steady_clock::now() < deadline)
        {
            while (child.TryReadLine(line))
                log += line + '\n';
            if (const std::optional<int> code = child.GetExitCode())
            {
                log += child.ReadAllStdout();
                return *code;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        child.Kill();
        log += "(killed at the 60 s deadline)\n";
        return -1;
    }

    /// Child side: die at `stage` of the next durable commit to `target`.
    void ArmCrashAt(SavePaths::DurableCommitStage stage, const fs::path& target)
    {
        s_crashStage = stage;
        s_crashTarget = target;
        SavePaths::DurableCommitObserver() = [](SavePaths::DurableCommitStage reached, const fs::path& destination)
        {
            if (reached == s_crashStage && destination == s_crashTarget)
                ::_exit(kCrashExitCode);
        };
    }

    /// Child side of the crash drills. Returns normally (exit 0) only when the
    /// commit returned without reaching the crash point.
    void RunCrashRole(const std::string& role)
    {
        const fs::path db = EnvOrEmpty(kDrillDbEnv);
        const uint64_t charId = std::strtoull(EnvOrEmpty(kDrillCharEnv).c_str(), nullptr, 10);
        // "<store>-staged" dies before the rename, "<store>-renamed" (and plain "renamed") after it.
        const SavePaths::DurableCommitStage stage = role.ends_with("renamed")
                                                        ? SavePaths::DurableCommitStage::Renamed
                                                        : SavePaths::DurableCommitStage::StagedAndSynced;
        ArmCrashAt(stage, db);
        if (role == "restore")
        {
            TFBackupInfo info;
            (void)TFDatabase::RestoreFromBackup(EnvOrEmpty(kDrillBackupEnv), db, info);
        }
        else if (role.starts_with("outfit-"))
        {
            // The store takes its lifetime lock in Open; the process dies holding it.
            TFOutfitStore store;
            if (store.Open(db) && store.AddMember(1, 2002, "Vex", TFOutfitRank::Member, 7))
                (void)store.SaveNow();
        }
        else if (role.starts_with("social-"))
        {
            // Load the committed v0 store and write it back through the production serializer, which
            // stamps the current schema: the new commit differs from the old one byte for byte.
            (void)TFSocialSystem::ResaveStoreForTesting(db);
        }
        else if (role.starts_with("territory-"))
        {
            Spark::Json::Value root;
            std::string detail;
            if (WorldSave::ReadJson(db, "drill_continent", "Drill", false, root, detail) ==
                WorldSave::ReadStatus::Loaded)
            {
                root["owners"][size_t{1}] = Spark::Json::Value(3);
                (void)WorldSave::WriteJson(db, root, detail);
            }
        }
        else
        {
            TFDatabase child;
            if (child.Open(db))
                (void)child.SaveCharacterProgress(charId, 0, 1, 200, 0);
        }
    }

    /// Run `testName` in a child playing `role`; returns its exit code and
    /// prints the child's output when it is not the expected crash.
    int RunCrashChild(const char* testName, const std::string& role, const fs::path& db, uint64_t charId,
                      const fs::path& backup = {})
    {
        auto launched = SpawnDrillChild(testName, role, db, charId, backup);
        if (!launched)
        {
            std::fprintf(stderr, "[DATA120] cannot spawn the drill child: %s\n", launched.error().c_str());
            return -1;
        }
        std::string log;
        const int code = WaitForChild(*launched, log);
        if (code != kCrashExitCode)
            std::fprintf(stderr, "---- DATA120 drill child (%s) exit %d ----\n%s\n----\n", role.c_str(), code,
                         log.c_str());
        return code;
    }

    /// Child side of the SIGKILL drill: commit flux = 2, 3, ... and acknowledge
    /// each on stdout only after it reported success.
    void RunWriterRole()
    {
        const fs::path db = EnvOrEmpty(kDrillDbEnv);
        const uint64_t charId = std::strtoull(EnvOrEmpty(kDrillCharEnv).c_str(), nullptr, 10);
        TFDatabase writer;
        ASSERT_TRUE(charId != 0 && writer.Open(db));
        for (uint32_t flux = 2; flux < 100000; ++flux)
        {
            ASSERT_TRUE(writer.SaveCharacterProgress(charId, 0, 1, flux, 0));
            std::printf("DATA120_ACK=%u\n", flux);
            std::fflush(stdout);
        }
    }

    /// Highest acknowledged flux in `line`, folded into `lastAck`.
    void FoldAck(const std::string& line, uint32_t& lastAck)
    {
        constexpr std::string_view kKey = "DATA120_ACK=";
        const size_t at = line.find(kKey);
        if (at != std::string::npos)
            lastAck =
                std::max(lastAck, static_cast<uint32_t>(std::strtoul(line.c_str() + at + kKey.size(), nullptr, 10)));
    }
} // namespace

TEST(Persistence_RecoveryDrill_CrashBeforeRenameReopensToPreviousCommit)
{
    if (const std::string role = EnvOrEmpty(kDrillRoleEnv); !role.empty())
    {
        RunCrashRole(role);
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_staged");
    const fs::path db = dir / "terrafront.db";
    uint64_t charId = 0;
    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        charId = Seed(authority, "drill_user", "Driller", 100);
        ASSERT_TRUE(charId != 0);
    }
    const std::string committed = ReadFile(db);

    ASSERT_EQ(RunCrashChild("Persistence_RecoveryDrill_CrashBeforeRenameReopensToPreviousCommit", "staged", db, charId),
              kCrashExitCode);

    // The dead writer left its complete staging file; the primary is the last commit, byte for byte.
    EXPECT_TRUE(fs::exists(WithSuffix(db, ".tmp")));
    EXPECT_TRUE(ReadFile(db) == committed);
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{100});

    // Its lock died with it, and the next commit replaces the stale staging file.
    TFDatabase survivor;
    ASSERT_TRUE(survivor.Open(db));
    EXPECT_TRUE(survivor.SaveCharacterProgress(charId, 0, 1, 300, 0));
    EXPECT_FALSE(fs::exists(WithSuffix(db, ".tmp")));
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{300});
    survivor.Close();
    fs::remove_all(dir);
}

TEST(Persistence_RecoveryDrill_CrashAfterRenameReopensToNewCommit)
{
    if (const std::string role = EnvOrEmpty(kDrillRoleEnv); !role.empty())
    {
        RunCrashRole(role);
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_renamed");
    const fs::path db = dir / "terrafront.db";
    uint64_t charId = 0;
    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        charId = Seed(authority, "drill_user", "Driller", 100);
        ASSERT_TRUE(charId != 0);
    }
    const uint64_t revisionBefore = FileRevision(db);

    ASSERT_EQ(RunCrashChild("Persistence_RecoveryDrill_CrashAfterRenameReopensToNewCommit", "renamed", db, charId),
              kCrashExitCode);

    // A process death after the rename keeps the new commit (only power loss can lose it before the
    // directory sync, and then the previous commit survives intact).
    EXPECT_FALSE(fs::exists(WithSuffix(db, ".tmp")));
    EXPECT_EQ(FileRevision(db), revisionBefore + 1);
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{200});

    TFDatabase survivor;
    ASSERT_TRUE(survivor.Open(db));
    EXPECT_TRUE(survivor.SaveCharacterProgress(charId, 0, 1, 300, 0));
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{300});
    survivor.Close();
    fs::remove_all(dir);
}

TEST(Persistence_RecoveryDrill_CrashDuringRestoreLeavesPrimaryIntact)
{
    if (const std::string role = EnvOrEmpty(kDrillRoleEnv); !role.empty())
    {
        RunCrashRole(role);
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_restore");
    const fs::path db = dir / "terrafront.db";
    const fs::path backup = dir / "terrafront.db.bak";
    uint64_t charId = 0;
    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        charId = Seed(authority, "drill_user", "Driller", 100);
        ASSERT_TRUE(charId != 0);
        TFBackupInfo info;
        ASSERT_TRUE(TFDatabase::CreateBackup(db, backup, info) == TFBackupStatus::Ok);
        ASSERT_TRUE(authority.SaveCharacterProgress(charId, 0, 1, 55, 0));
    }
    const std::string primary = ReadFile(db);

    ASSERT_EQ(
        RunCrashChild("Persistence_RecoveryDrill_CrashDuringRestoreLeavesPrimaryIntact", "restore", db, charId, backup),
        kCrashExitCode);
    EXPECT_TRUE(ReadFile(db) == primary);
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{55});

    // The interrupted restore is simply run again.
    TFBackupInfo info;
    ASSERT_TRUE(TFDatabase::RestoreFromBackup(backup, db, info) == TFBackupStatus::Ok);
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{100});
    fs::remove_all(dir);
}

TEST(Persistence_RecoveryDrill_KilledWriterReopensToLastAcknowledgedCommit)
{
    if (EnvOrEmpty(kDrillRoleEnv) == "writer")
    {
        RunWriterRole();
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_sigkill");
    const fs::path db = dir / "terrafront.db";
    uint64_t charId = 0;
    {
        TFDatabase authority;
        ASSERT_TRUE(authority.Open(db));
        charId = Seed(authority, "drill_user", "Driller", 1);
        ASSERT_TRUE(charId != 0);
    }

    auto launched =
        SpawnDrillChild("Persistence_RecoveryDrill_KilledWriterReopensToLastAcknowledgedCommit", "writer", db, charId);
    if (!launched)
        SKIP_TEST("cannot spawn the drill child: " + launched.error());
    Spark::Process& writer = *launched;

    // Let it get well into its commit loop, then kill it mid-flight. Bounded:
    // a wedged child fails this test instead of hanging the whole run.
    uint32_t lastAck = 1;
    std::string log;
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (lastAck < kAckTarget && writer.IsRunning() && std::chrono::steady_clock::now() < deadline)
    {
        while (writer.TryReadLine(line))
        {
            FoldAck(line, lastAck);
            log += line + '\n';
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool stillCommitting = writer.IsRunning();
    writer.Kill();
    // Acknowledgements already written before the kill still count.
    std::istringstream rest(writer.ReadAllStdout());
    while (std::getline(rest, line))
    {
        FoldAck(line, lastAck);
        log += line + '\n';
    }
    if (!stillCommitting || lastAck < kAckTarget)
        std::fprintf(stderr, "---- DATA120 writer output ----\n%s\n----\n", log.c_str());
    ASSERT_TRUE(stillCommitting);
    ASSERT_TRUE(lastAck >= kAckTarget);

    // Every acknowledged commit survived; at most the one in flight when it died is also visible.
    const int64_t flux = FluxOnDisk(db, charId);
    EXPECT_TRUE(flux == static_cast<int64_t>(lastAck) || flux == static_cast<int64_t>(lastAck) + 1);

    TFDatabase survivor;
    ASSERT_TRUE(survivor.Open(db));
    EXPECT_TRUE(survivor.SaveCharacterProgress(charId, 0, 1, 7, 0));
    EXPECT_EQ(FluxOnDisk(db, charId), int64_t{7});
    survivor.Close();
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// The other stores that commit through WriteDurableReplace: the outfit store,
// the social store and the WorldSave territory file. Each child opens the real
// store, commits one mutation and dies at a commit stage.
// ---------------------------------------------------------------------------

namespace
{
    /// Seed outfit 1 with its leader only, committed and closed.
    bool SeedOutfitStore(const fs::path& path)
    {
        TFOutfitStore store;
        return store.Open(path) && store.Create("Iron Vultures", "IVLT", 1001, "Raska", 5) != nullptr && store.Close();
    }

    /// Members of outfit 1 as a fresh authority sees them, or -1 when the store cannot open.
    int OutfitMembersOnDisk(const fs::path& path)
    {
        TFOutfitStore store;
        if (!store.Open(path))
            return -1;
        const TFOutfitRecord* outfit = store.FindById(1);
        const int members = outfit ? static_cast<int>(outfit->members.size()) : -1;
        return store.Close() ? members : -1;
    }

    /// A pre-versioning (v0) social store with one character row.
    constexpr const char* kSocialV0 =
        R"({"characters":[{"charId":42,"friends":["Alice One"],"blocked":[],"recent":[]}]})";

    Spark::Json::Value TerritoryDocument()
    {
        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        root["version"] = Spark::Json::Value(static_cast<int>(WorldSave::kTerritorySchemaVersion));
        root["continentKey"] = Spark::Json::Value("drill_continent");
        root["continent"] = Spark::Json::Value("Drill");
        root["regionCount"] = Spark::Json::Value(3);
        Spark::Json::Value owners = Spark::Json::Value::MakeArray();
        for (const int owner : {1, 2, 1})
            owners.PushBack(Spark::Json::Value(owner));
        root["owners"] = std::move(owners);
        Spark::Json::Value dominion = Spark::Json::Value::MakeObject();
        dominion["active"] = Spark::Json::Value(false);
        dominion["faction"] = Spark::Json::Value(0);
        dominion["remainingSec"] = Spark::Json::Value(0.0);
        root["dominion"] = std::move(dominion);
        return root;
    }

    /// Owner of region 1 in the committed territory file, or -1 when it does not load.
    int TerritoryOwnerOnDisk(const fs::path& path)
    {
        Spark::Json::Value root;
        std::string detail;
        uint32_t owner = 0;
        if (WorldSave::ReadJson(path, "drill_continent", "Drill", false, root, detail) !=
                WorldSave::ReadStatus::Loaded ||
            !WorldSave::ReadUint32(root["owners"][size_t{1}], owner))
            return -1;
        return static_cast<int>(owner);
    }
} // namespace

TEST(Persistence_RecoveryDrill_OutfitCrashBeforeRename)
{
    if (const std::string role = EnvOrEmpty(kDrillRoleEnv); !role.empty())
    {
        RunCrashRole(role);
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_outfit_staged");
    const fs::path store = dir / "outfits.json";
    ASSERT_TRUE(SeedOutfitStore(store));
    const std::string committed = ReadFile(store);

    ASSERT_EQ(RunCrashChild("Persistence_RecoveryDrill_OutfitCrashBeforeRename", "outfit-staged", store, 0),
              kCrashExitCode);

    // The previous commit is intact, and the dead writer's lifetime lock died with it.
    EXPECT_TRUE(fs::exists(WithSuffix(store, ".tmp")));
    EXPECT_TRUE(ReadFile(store) == committed);
    EXPECT_EQ(OutfitMembersOnDisk(store), 1);

    TFOutfitStore survivor;
    ASSERT_TRUE(survivor.Open(store));
    EXPECT_TRUE(survivor.AddMember(1, 2003, "Zap", TFOutfitRank::Member, 8));
    EXPECT_TRUE(survivor.Close());
    EXPECT_FALSE(fs::exists(WithSuffix(store, ".tmp")));
    EXPECT_EQ(OutfitMembersOnDisk(store), 2);
    fs::remove_all(dir);
}

TEST(Persistence_RecoveryDrill_OutfitCrashAfterRename)
{
    if (const std::string role = EnvOrEmpty(kDrillRoleEnv); !role.empty())
    {
        RunCrashRole(role);
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_outfit_renamed");
    const fs::path store = dir / "outfits.json";
    ASSERT_TRUE(SeedOutfitStore(store));

    ASSERT_EQ(RunCrashChild("Persistence_RecoveryDrill_OutfitCrashAfterRename", "outfit-renamed", store, 0),
              kCrashExitCode);

    // A process death after the rename keeps the new commit, and the lock is free again.
    EXPECT_FALSE(fs::exists(WithSuffix(store, ".tmp")));
    EXPECT_EQ(OutfitMembersOnDisk(store), 2);

    TFOutfitStore survivor;
    ASSERT_TRUE(survivor.Open(store));
    EXPECT_TRUE(survivor.AddMember(1, 2003, "Zap", TFOutfitRank::Member, 8));
    EXPECT_TRUE(survivor.Close());
    EXPECT_EQ(OutfitMembersOnDisk(store), 3);
    fs::remove_all(dir);
}

TEST(Persistence_RecoveryDrill_SocialCrashBeforeRename)
{
    if (const std::string role = EnvOrEmpty(kDrillRoleEnv); !role.empty())
    {
        RunCrashRole(role);
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_social_staged");
    const fs::path store = dir / "terrafront_social.json";
    WriteFile(store, kSocialV0);

    ASSERT_EQ(RunCrashChild("Persistence_RecoveryDrill_SocialCrashBeforeRename", "social-staged", store, 0),
              kCrashExitCode);

    EXPECT_TRUE(fs::exists(WithSuffix(store, ".tmp")));
    EXPECT_TRUE(ReadFile(store) == kSocialV0);
    const auto reopened = TFSocialSystem::LoadStoreForTesting(store);
    EXPECT_TRUE(reopened.accepted);
    EXPECT_EQ(reopened.recordCount, size_t{1});

    // The next commit replaces the stale staging file.
    ASSERT_TRUE(TFSocialSystem::ResaveStoreForTesting(store));
    EXPECT_FALSE(fs::exists(WithSuffix(store, ".tmp")));
    EXPECT_TRUE(ReadFile(store).find("schemaVersion") != std::string::npos);
    fs::remove_all(dir);
}

TEST(Persistence_RecoveryDrill_SocialCrashAfterRename)
{
    if (const std::string role = EnvOrEmpty(kDrillRoleEnv); !role.empty())
    {
        RunCrashRole(role);
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_social_renamed");
    const fs::path store = dir / "terrafront_social.json";
    WriteFile(store, kSocialV0);

    ASSERT_EQ(RunCrashChild("Persistence_RecoveryDrill_SocialCrashAfterRename", "social-renamed", store, 0),
              kCrashExitCode);

    EXPECT_FALSE(fs::exists(WithSuffix(store, ".tmp")));
    EXPECT_TRUE(ReadFile(store).find("schemaVersion") != std::string::npos);
    const auto reopened = TFSocialSystem::LoadStoreForTesting(store);
    EXPECT_TRUE(reopened.accepted);
    EXPECT_EQ(reopened.recordCount, size_t{1});
    EXPECT_TRUE(TFSocialSystem::ResaveStoreForTesting(store));
    fs::remove_all(dir);
}

TEST(Persistence_RecoveryDrill_TerritoryWriteCrashAfterRename)
{
    if (const std::string role = EnvOrEmpty(kDrillRoleEnv); !role.empty())
    {
        RunCrashRole(role);
        return;
    }

    const fs::path dir = FreshDir("test_data120_drill_territory");
    const fs::path territory = dir / "terrafront_territory.drill_continent.json";
    std::string detail;
    ASSERT_TRUE(WorldSave::WriteJson(territory, TerritoryDocument(), detail));
    ASSERT_EQ(TerritoryOwnerOnDisk(territory), 2);

    ASSERT_EQ(
        RunCrashChild("Persistence_RecoveryDrill_TerritoryWriteCrashAfterRename", "territory-renamed", territory, 0),
        kCrashExitCode);

    // The region flip the dead authority committed is what a restarted authority loads.
    EXPECT_FALSE(fs::exists(WithSuffix(territory, ".tmp")));
    EXPECT_EQ(TerritoryOwnerOnDisk(territory), 3);
    ASSERT_TRUE(WorldSave::WriteJson(territory, TerritoryDocument(), detail));
    EXPECT_EQ(TerritoryOwnerOnDisk(territory), 2);
    fs::remove_all(dir);
}
