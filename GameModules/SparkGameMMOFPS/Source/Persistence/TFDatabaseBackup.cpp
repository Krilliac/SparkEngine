/**
 * @file TFDatabaseBackup.cpp
 * @brief DATA-120 verified backup and restore of the TERRAFRONT account/character database.
 *
 * A backup is two files written with SavePaths::WriteDurableReplace: an exact
 * copy of one committed database revision, read under the authority lock, and
 * a sha256sum-format sidecar ("<hex>  <name>\n") so operators can also check
 * it with `sha256sum -c`. Restore trusts nothing it has not verified: the
 * digest, then the same validation Open applies (schema gate included), and
 * only then replaces the primary. The recovery point is documented in
 * docs/specs/persistence.md.
 */
#include "Persistence/TFDatabase.h"
#include "Account/TFCrypto.h"
#include "Persistence/TFSavePaths.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>

namespace Terrafront
{
    namespace
    {
        namespace fs = std::filesystem;

        /// Same bound TFDatabase transactions use: a longer wait means a stuck peer.
        constexpr std::chrono::milliseconds kBackupLockTimeout{2000};

        /// 2^53 - 1, the largest exactly representable JSON integer. TFDatabase
        /// reserves it as the exhausted sentinel, so a stored revision stays below it.
        constexpr uint64_t kExhaustedRevision = 9007199254740991ULL;

        /// A sidecar is one short line; anything larger is not ours.
        constexpr std::streamoff kMaxSidecarBytes = 4096;

        constexpr size_t kSha256HexLength = 64;

        bool ReadBytes(const fs::path& path, std::string& out, std::streamoff maxBytes = -1)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return false;
            if (maxBytes >= 0)
            {
                in.seekg(0, std::ios::end);
                const std::streamoff size = in.tellg();
                if (size < 0 || size > maxBytes)
                    return false;
                in.seekg(0, std::ios::beg);
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            if (in.bad())
                return false;
            out = ss.str();
            return true;
        }

        std::string DigestHex(const std::string& bytes)
        {
            const Crypto::Sha256Digest digest = Crypto::Sha256(bytes);
            return Crypto::ToHex(digest.data(), digest.size());
        }

        std::string SidecarText(const std::string& digest, const fs::path& backupPath)
        {
            return digest + "  " + SavePaths::Utf8ForLog(backupPath.filename()) + "\n";
        }

        /// Accept exactly one sha256sum line: 64 lowercase hex digits, the text
        /// (two spaces) or binary (" *") separator, a non-empty name, optional "\n".
        bool ParseSidecarDigest(std::string_view text, std::string& digest)
        {
            if (text.ends_with('\n'))
                text.remove_suffix(1);
            if (text.size() < kSha256HexLength + 3 || text.find_first_of("\r\n") != std::string_view::npos)
                return false;
            const std::string_view hex = text.substr(0, kSha256HexLength);
            if (!std::all_of(hex.begin(), hex.end(),
                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                return false;
            const std::string_view separator = text.substr(kSha256HexLength, 2);
            if (separator != "  " && separator != " *")
                return false;
            digest.assign(hex);
            return true;
        }

        bool PathExists(const fs::path& path)
        {
            std::error_code ec;
            // symlink_status so a dangling link still counts as occupied.
            const fs::file_status status = fs::symlink_status(path, ec);
            return ec ? status.type() != fs::file_type::not_found : fs::exists(status);
        }

        fs::path Resolved(const fs::path& path)
        {
            std::error_code ec;
            const fs::path resolved = fs::weakly_canonical(path, ec);
            return ec ? fs::absolute(path, ec).lexically_normal() : resolved;
        }

        /// A backup file or sidecar must never alias the database, its staging
        /// file (the next commit unlinks it) or its lock file.
        bool AliasesDatabase(const fs::path& candidate, const fs::path& dbPath)
        {
            const fs::path resolved = Resolved(candidate);
            for (const char* suffix : {"", ".tmp", ".lock"})
            {
                fs::path reserved = dbPath;
                reserved += suffix;
                if (resolved == Resolved(reserved))
                    return true;
            }
            return false;
        }

        bool ValidPathPair(const fs::path& dbPath, const fs::path& backupPath, const fs::path& digestPath)
        {
            return !dbPath.empty() && dbPath.has_filename() && !backupPath.empty() && backupPath.has_filename() &&
                   !AliasesDatabase(backupPath, dbPath) && !AliasesDatabase(digestPath, dbPath);
        }

        /// Schema of already-validated file bytes (0 == legacy unversioned).
        uint32_t SchemaVersionOf(const std::string& bytes)
        {
            Spark::Json::Value root;
            if (!Spark::Json::ParseStrict(bytes, &root, nullptr) || !root.IsObject() || !root.HasKey("schemaVersion"))
                return 0;
            return static_cast<uint32_t>(root["schemaVersion"].AsNumber(0.0));
        }

        /// Best-effort revision of a primary that failed validation, so a restore
        /// over a damaged file still moves past any revision an authority saw.
        /// Empty when the bytes do not carry a readable revision (a torn file).
        std::optional<uint64_t> RevisionHint(const std::string& bytes)
        {
            Spark::Json::Value root;
            if (!Spark::Json::ParseStrict(bytes, &root, nullptr) || !root.IsObject() || !root.HasKey("revision") ||
                !root["revision"].IsNumber())
                return std::nullopt;
            const double revision = root["revision"].AsNumber(-1.0);
            if (!std::isfinite(revision) || revision < 0.0 || revision >= static_cast<double>(kExhaustedRevision) ||
                std::trunc(revision) != revision)
                return std::nullopt;
            return static_cast<uint64_t>(revision);
        }

        int64_t NowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        /// "<db>.pre-restore-<ms>[-n].bak"; never an existing file.
        fs::path DisplacedPrimaryPath(const fs::path& dbPath)
        {
            const std::string stamp = std::to_string(NowMs());
            for (int attempt = 0;; ++attempt)
            {
                fs::path candidate = dbPath;
                candidate += ".pre-restore-" + stamp + (attempt == 0 ? "" : "-" + std::to_string(attempt)) + ".bak";
                if (!PathExists(candidate))
                    return candidate;
            }
        }
    } // namespace

    fs::path TFDatabase::BackupDigestPath(const fs::path& backupPath)
    {
        fs::path digestPath = backupPath;
        digestPath += ".sha256";
        return digestPath;
    }

    TFBackupStatus TFDatabase::CreateBackup(const fs::path& dbPath, const fs::path& backupPath, TFBackupInfo& info)
    {
        info = TFBackupInfo{};
        const fs::path digestPath = BackupDigestPath(backupPath);
        if (!ValidPathPair(dbPath, backupPath, digestPath))
            return TFBackupStatus::InvalidPath;

        // Held until return: the bytes copied are exactly one committed revision.
        SavePaths::ExclusiveFileLock lock;
        std::error_code ec;
        if (!lock.Lock(dbPath, kBackupLockTimeout, ec))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] backup of %s refused: lock not acquired (%s)",
                            SavePaths::Utf8ForLog(dbPath).c_str(), ec.message().c_str());
            return TFBackupStatus::Locked;
        }
        if (!PathExists(dbPath))
            return TFBackupStatus::SourceMissing;
        if (PathExists(backupPath) || PathExists(digestPath))
            return TFBackupStatus::TargetExists;

        std::string bytes;
        if (!ReadBytes(dbPath, bytes))
            return TFBackupStatus::SourceUnreadable;
        TFDatabase source;
        source.m_path = dbPath;
        Snapshot snapshot;
        switch (source.ParseSnapshot(bytes, snapshot))
        {
        case LoadResult::Loaded:
            break;
        case LoadResult::UnsupportedVersion:
            return TFBackupStatus::SourceUnsupportedVersion;
        case LoadResult::Unreadable:
            return TFBackupStatus::SourceUnreadable;
        case LoadResult::Corrupt:
            return TFBackupStatus::SourceCorrupt;
        }

        const std::string digest = DigestHex(bytes);
        const std::string sidecar = SidecarText(digest, backupPath);
        if (!backupPath.parent_path().empty())
        {
            fs::create_directories(backupPath.parent_path(), ec);
            if (ec)
                return TFBackupStatus::WriteFailed;
        }
        if (!SavePaths::WriteDurableReplace(backupPath, bytes, ec))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] backup %s could not be written (%s)",
                            SavePaths::Utf8ForLog(backupPath).c_str(), ec.message().c_str());
            return TFBackupStatus::WriteFailed;
        }
        std::error_code removeEc;
        if (!SavePaths::WriteDurableReplace(digestPath, sidecar, ec))
        {
            // A backup without its digest can never be restored; do not leave it looking usable.
            fs::remove(backupPath, removeEc);
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] backup digest %s could not be written (%s)",
                            SavePaths::Utf8ForLog(digestPath).c_str(), ec.message().c_str());
            return TFBackupStatus::WriteFailed;
        }

        std::string backupBytes;
        std::string sidecarBytes;
        if (!ReadBytes(backupPath, backupBytes) || DigestHex(backupBytes) != digest ||
            !ReadBytes(digestPath, sidecarBytes, kMaxSidecarBytes) || sidecarBytes != sidecar)
        {
            fs::remove(backupPath, removeEc);
            fs::remove(digestPath, removeEc);
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] backup %s failed read-back verification; removed",
                            SavePaths::Utf8ForLog(backupPath).c_str());
            return TFBackupStatus::VerifyFailed;
        }

        info.revision = snapshot.revision;
        info.schemaVersion = SchemaVersionOf(bytes);
        info.sizeBytes = bytes.size();
        info.sha256 = digest;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] backup %s of %s: revision %llu, schema v%u, sha256 %s",
                       SavePaths::Utf8ForLog(backupPath).c_str(), SavePaths::Utf8ForLog(dbPath).c_str(),
                       static_cast<unsigned long long>(info.revision), info.schemaVersion, digest.c_str());
        return TFBackupStatus::Ok;
    }

    TFBackupStatus TFDatabase::RestoreFromBackup(const fs::path& backupPath, const fs::path& dbPath, TFBackupInfo& info)
    {
        info = TFBackupInfo{};
        const fs::path digestPath = BackupDigestPath(backupPath);
        if (!ValidPathPair(dbPath, backupPath, digestPath))
            return TFBackupStatus::InvalidPath;

        // Verify everything about the backup before touching the primary.
        std::string bytes;
        if (!ReadBytes(backupPath, bytes))
            return TFBackupStatus::BackupMissing;
        std::string sidecar;
        std::string expectedDigest;
        if (!ReadBytes(digestPath, sidecar, kMaxSidecarBytes) || !ParseSidecarDigest(sidecar, expectedDigest))
            return TFBackupStatus::DigestMissing;
        const std::string digest = DigestHex(bytes);
        if (digest != expectedDigest)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] restore refused: %s does not match its digest %s",
                            SavePaths::Utf8ForLog(backupPath).c_str(), SavePaths::Utf8ForLog(digestPath).c_str());
            return TFBackupStatus::DigestMismatch;
        }
        TFDatabase backup;
        backup.m_path = backupPath;
        Snapshot restored;
        switch (backup.ParseSnapshot(bytes, restored))
        {
        case LoadResult::Loaded:
            break;
        case LoadResult::UnsupportedVersion:
            return TFBackupStatus::BackupUnsupportedVersion;
        case LoadResult::Unreadable:
        case LoadResult::Corrupt:
            return TFBackupStatus::BackupCorrupt;
        }

        std::error_code ec;
        if (!dbPath.parent_path().empty())
        {
            fs::create_directories(dbPath.parent_path(), ec);
            if (ec)
                return TFBackupStatus::WriteFailed;
        }
        SavePaths::ExclusiveFileLock lock;
        if (!lock.Lock(dbPath, kBackupLockTimeout, ec))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] restore of %s refused: lock not acquired (%s)",
                            SavePaths::Utf8ForLog(dbPath).c_str(), ec.message().c_str());
            return TFBackupStatus::Locked;
        }

        // Keep the primary being replaced, whatever its state, so the restore is itself reversible.
        TFDatabase target;
        target.m_path = dbPath;
        std::optional<uint64_t> primaryRevision;
        if (PathExists(dbPath))
        {
            std::string primaryBytes;
            if (!ReadBytes(dbPath, primaryBytes))
                return TFBackupStatus::SourceUnreadable;
            Snapshot primary;
            primaryRevision = target.ParseSnapshot(primaryBytes, primary) == LoadResult::Loaded
                                  ? std::optional<uint64_t>(primary.revision)
                                  : RevisionHint(primaryBytes);
            info.displacedPrimary = DisplacedPrimaryPath(dbPath);
            if (!SavePaths::WriteDurableReplace(info.displacedPrimary, primaryBytes, ec))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "[TF] restore of %s refused: the current primary could not be preserved (%s)",
                                SavePaths::Utf8ForLog(dbPath).c_str(), ec.message().c_str());
                info.displacedPrimary.clear();
                return TFBackupStatus::WriteFailed;
            }
        }

        // Stamp above the displaced primary so an authority whose baseline
        // predates the restore sees a Conflict (or a backwards file) instead of
        // a row that merely reuses a revision number it once knew. That holds
        // only when the displaced revision is known: with no primary, or a torn
        // one, the lost history's revisions are unknown and later commits may
        // reuse them, so every authority must be restarted (see the header).
        const uint64_t newRevision = std::max(restored.revision, primaryRevision.value_or(0)) + 1;
        if (newRevision >= kExhaustedRevision)
            return TFBackupStatus::WriteFailed;
        restored.revision = newRevision;
        if (!target.SaveToDisk(restored))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] restore of %s from %s failed to write; primary unchanged",
                            SavePaths::Utf8ForLog(dbPath).c_str(), SavePaths::Utf8ForLog(backupPath).c_str());
            return TFBackupStatus::WriteFailed;
        }

        std::string written;
        Snapshot check;
        if (!ReadBytes(dbPath, written) || target.ParseSnapshot(written, check) != LoadResult::Loaded ||
            check.revision != newRevision)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] restored %s failed read-back verification",
                            SavePaths::Utf8ForLog(dbPath).c_str());
            return TFBackupStatus::VerifyFailed;
        }

        info.revision = newRevision;
        info.supersedesPrimaryRevision = primaryRevision.has_value();
        info.schemaVersion = SchemaVersionOf(bytes);
        info.sizeBytes = bytes.size();
        info.sha256 = digest;
        SPARK_LOG_WARN(Spark::LogCategory::Game,
                       "[TF] %s restored from %s (backup revision stamped %llu, schema v%u); previous primary kept "
                       "as %s",
                       SavePaths::Utf8ForLog(dbPath).c_str(), SavePaths::Utf8ForLog(backupPath).c_str(),
                       static_cast<unsigned long long>(newRevision), info.schemaVersion,
                       info.displacedPrimary.empty() ? "(none)" : SavePaths::Utf8ForLog(info.displacedPrimary).c_str());
        if (!info.supersedesPrimaryRevision)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] %s: the replaced primary's revision was unknown (missing or unreadable), so later "
                           "commits may reuse revisions from the lost history; restart every authority on this "
                           "database before serving",
                           SavePaths::Utf8ForLog(dbPath).c_str());
        }
        return TFBackupStatus::Ok;
    }
} // namespace Terrafront
