/**
 * @file TFDatabase.h
 * @brief TERRAFRONT account/character persistence (W5 onboarding, Task 1).
 *
 * Backing: atomic-JSON-file (tmp+rename + Spark::Json read-modify-write),
 * NOT the engine's Spark::Persistence::AsyncDatabasePool. That pool's
 * SQLiteConnection fallback is a JSON-key-value store whose ExecuteRaw only
 * understands SET/GET/DELETE/KEYS verbs — it does not parse SQL (INSERT/
 * SELECT/etc. fall into the "Unsupported command" branch), so a real
 * prepared-SQL CRUD layer cannot persist rows through it. Per the W5 plan
 * (docs/specs/terrafront-onboarding-design.md, persistence decisions),
 * TFDatabase pivots straight to the atomic-JSON-file
 * pattern used by TFProgressionSystem (TFProgressionSystem.cpp:385-446) while
 * exposing write failures to Tasks 2-6 instead of reporting in-memory-only
 * mutations as successful.
 *
 * Stores accounts[] + characters[] as JSON in a single caller-selected file.
 * TerraFront authority code passes SavePaths::File("terrafront.db").
 *
 * TF-120 shared-root concurrency: several continent authority processes may
 * open the same file. The file itself has no lifetime lock (only the
 * per-continent authority lock below does). Every call is one
 * transaction under a short exclusive `<file>.lock` (SavePaths::
 * ExclusiveFileLock): reload the committed file, validate it, apply the
 * change to that fresh state, atomically write (tmp+rename), release. Reads
 * take the same lock so they see other authorities' commits. Creates, deletes
 * and login touches are re-applied to the fresh state, so ids and unique names
 * are allocated against every authority's rows.
 *
 * Absolute character writes (progress / meta / CommitCharacterUpdates) carry
 * values the caller computed from an earlier read, so they are optimistic:
 * each character row stores the file revision of its last change, and this
 * instance remembers a per-character baseline revision (set by Open, by
 * AcquireCharacter, by CreateCharacter and by its own successful commits;
 * plain reads never move it). A commit whose row changed since the baseline
 * was written by another authority; it is rejected with status Conflict and
 * nothing is written; ConflictedCharacter() names the stale row so batch
 * callers (TFPlayerMetaStore::PersistAllDirty) can drop it and commit the
 * rest, or re-acquire it and retry. A lock
 * timeout (Locked), unreadable/corrupt/newer-schema file, or a vanished
 * primary fails closed.
 *
 * Every mutating call is durable before it reports success; Close() only
 * marks the instance closed. A fresh TFDatabase that Opens the same path sees
 * every mutation that previously reported success.
 *
 * DATA-120: CommitCharacterUpdates applies several character rows (e.g. an
 * unlock purchase's flux debit + unlock key, or a transfer between two
 * characters) as ONE disk commit, all-or-nothing. The file carries a
 * "schemaVersion"; files without it are the legacy v0 shape and upgrade on
 * the next write, while a file from a newer schema fails closed so an older
 * build can never load-and-rewrite it and silently drop the newer fields.
 * Schema v2 adds the file "revision" and per-character "revision" keys; v0/v1
 * files load with revision 0 and upgrade on the next write.
 * Schema v3 is retired: b2d2953 briefly wrote it with an "appliedOperations"
 * idempotency ledger that no caller ever used, so its files carry an empty
 * ledger and v2 content. Such a file loads as v2 and is rewritten as v2; a v3
 * file with a missing or non-empty ledger fails closed as UnsupportedVersion,
 * because rewriting it would drop recorded operation ids.
 *
 * Schema v4 (TF-120 residency, TFDatabaseResidency.cpp) adds each character's
 * "resident" continent key: empty means the character is in no world. v2
 * files and empty-ledger v3 files load with every character non-resident and
 * are rewritten as v4. An authority process binds this instance to its
 * continent (BindAuthority) by holding the lifetime lock
 * `<file>.authority.<continent>.lock` until Close. Holding it proves every
 * earlier authority for that continent is dead, so the bind clears their
 * residency (own-crash recovery). ClaimCharacter marks a character resident
 * on the bound continent inside one transaction, and refuses (status
 * ResidentElsewhere) while the character is resident on another continent
 * whose authority lock is held. A resident whose authority lock is free is a
 * dead owner and is taken over. DeleteCharacter is refused the same way while
 * the character is resident on any live continent. BindAuthority reports
 * AuthorityHeld only for lock contention; any other lock error is Unreadable.
 * ReleaseCharacter clears the residency. A
 * claim stamps a new row revision, so an authority that was fenced out holds
 * a stale baseline and gets Conflict; on a bound instance every absolute
 * character write also requires the row to be resident on the bound
 * continent. Probing a dead owner is race-free: every claim runs under the
 * per-call file lock, so no two claims interleave. Unbound instances (backup
 * and restore tooling, tests) never read or change residency.
 *
 * Schema v5 adds a bounded handoff checkpoint and a per-character monotonic migration epoch.
 * A reservation freezes ordinary character writes while the source remains the owner. Destination
 * commit atomically moves residency; abort keeps source ownership. Terminal operation metadata and
 * payload remain durable for idempotent retries. Schema v4 rows load with an empty migration fence.
 *
 * Thread affinity: game thread only (no internal synchronization; the file
 * locks serialize processes, not threads). Ownership: the authority lock
 * lives exactly as long as the binding. Allocation: one snapshot per call.
 *
 * DATA-120 backup/restore (TFDatabaseBackup.cpp, recovery point documented in
 * docs/specs/persistence.md): CreateBackup copies the committed file under the
 * authority lock into a durable, re-verified backup plus a sha256sum-format
 * "<backup>.sha256" sidecar. RestoreFromBackup verifies that digest and the
 * full load validation (schema gate included) before it durably replaces the
 * primary, keeps the displaced primary as "<db>.pre-restore-<ms>.bak", and
 * stamps the restored file with a revision above both the backup and the
 * displaced primary so revisions never repeat across the restore.
 */
#pragma once

#include "Persistence/TFSavePaths.h"

#include "Core/TFTypes.h" // FactionId

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Terrafront
{
    enum class TFDatabaseStatus : uint8_t
    {
        Closed,
        ReadyNew,
        ReadyExisting,
        Unreadable,
        Corrupt,
        Locked,
        WriteFailed,
        UnsupportedVersion, ///< file written by a newer schema; left untouched
        Conflict,           ///< another authority changed a row since this instance's baseline
        AuthorityHeld,      ///< BindAuthority: a live authority already owns this continent (lock contention)
        ResidentElsewhere,  ///< ClaimCharacter/DeleteCharacter: the character is in world on a live continent
    };

    /// Outcome of TFDatabase::CreateBackup / RestoreFromBackup. Anything but Ok
    /// changed nothing the caller relies on: the primary is untouched, and a
    /// failed backup is never reported as usable.
    enum class TFBackupStatus : uint8_t
    {
        Ok,
        InvalidPath,              ///< empty path, or the backup would alias the db, its staging file or its lock
        Locked,                   ///< the authority lock was not acquired within the timeout
        SourceMissing,            ///< CreateBackup: no committed database file exists
        SourceUnreadable,         ///< CreateBackup: the committed file could not be read
        SourceCorrupt,            ///< CreateBackup: the committed file fails load validation
        SourceUnsupportedVersion, ///< CreateBackup: the committed file is from a newer schema
        TargetExists,             ///< CreateBackup: the backup or its sidecar exists; backups are never overwritten
        WriteFailed,              ///< a durable write failed
        VerifyFailed,             ///< CreateBackup: the re-read backup or sidecar did not match what was written
        BackupMissing,            ///< RestoreFromBackup: the backup file could not be read
        DigestMissing,            ///< RestoreFromBackup: the sidecar is absent or malformed
        DigestMismatch,           ///< RestoreFromBackup: the backup bytes do not match the sidecar digest
        BackupCorrupt,            ///< RestoreFromBackup: the backup fails load validation
        BackupUnsupportedVersion, ///< RestoreFromBackup: the backup is from a newer schema
    };

    /// What a backup or restore covered, for operator logs and drills.
    struct TFBackupInfo
    {
        uint64_t revision = 0;      ///< backup: revision captured; restore: revision the restored primary carries
        uint32_t schemaVersion = 0; ///< schema of the backup file (0 == legacy unversioned)
        uint64_t sizeBytes = 0;     ///< backup file size
        std::string sha256;         ///< lowercase hex digest of the backup file
        std::filesystem::path displacedPrimary; ///< restore: copy of the replaced primary (empty if none existed)
        /// restore: true when the replaced primary's revision was read, so `revision`
        /// is above every revision it held; false when no primary existed or it was torn
        bool supersedesPrimaryRevision = false;
    };

    struct TFAccountRecord
    {
        uint64_t id = 0;
        std::string username, salt, passwordHash;
        int64_t createdAtMs = 0, lastLoginMs = 0;
    };

    /// Per-weapon lifetime aggregates as persisted on a character row. Keyed by
    /// the durable weapons.json weapon KEY (WeaponId is a load-order index and
    /// would silently re-map if weapons.json were reordered).
    struct TFWeaponStatsRow
    {
        std::string weaponKey;
        uint32_t kills = 0, shots = 0, hits = 0, headshots = 0;
    };

    struct TFCharacterRecord
    {
        uint64_t id = 0, accountId = 0;
        std::string name;
        FactionId faction = FactionId::None;
        uint32_t xp = 0;
        uint16_t rank = 1;
        uint32_t flux = 0;
        int64_t createdAtMs = 0, lastPlayedMs = 0;
        uint64_t revision = 0;         ///< file revision of this row's last change (0 == legacy/never rewritten)
        std::string residentContinent; ///< schema v4 "resident": continent key it is in world on; empty == none
        std::string migrationOperation;     ///< TF-120 in-flight handoff operation identity; empty == none
        std::string migrationSource;        ///< TF-120 source continent for the in-flight handoff
        std::string migrationDestination;   ///< TF-120 destination continent for the in-flight handoff
        std::string migrationPayload;       ///< TF-120 bounded opaque participant state captured by the source
        std::string migrationLastOperation; ///< TF-120 last committed operation, for duplicate commit fencing
        uint64_t migrationEpoch = 0; ///< TF-120 terminal fence epoch, unique within this database revision history
        std::string migrationState;  ///< empty, reserved, committed, or aborted

        // --- W6 progression expansion (additive schema; absent keys on old save
        // files simply load as the empty defaults below) -------------------------
        std::vector<std::string> unlocks; ///< purchased TFUnlockDef keys
        std::string loadoutPrimary;       ///< weapons.json keys; empty ==
        std::string loadoutSecondary;     ///<   class-default slot
        std::string loadoutTool;
        // --- loadout-depth wave (additive; absent on old rows -> empty defaults)
        std::string loadoutGrenade; ///< weapons.json key; empty == frag_grenade
        std::string loadoutSuit;    ///< suits.json key; empty == no passive
        std::vector<TFWeaponStatsRow> weaponStats;
    };

    /// One character's pending changes for TFDatabase::CommitCharacterUpdates.
    /// writeProgress / writeMeta select which half of the row is replaced.
    struct TFCharacterUpdate
    {
        uint64_t charId = 0;

        bool writeProgress = false;
        uint32_t xp = 0;
        uint16_t rank = 1;
        uint32_t flux = 0;
        int64_t lastPlayedMs = 0;

        bool writeMeta = false;
        std::vector<std::string> unlocks;
        std::string loadoutPrimary, loadoutSecondary, loadoutTool, loadoutGrenade, loadoutSuit;
        std::vector<TFWeaponStatsRow> weaponStats;
    };

    class TFDatabase
    {
      public:
        /// On-disk schema written by this build (v5: fenced handoff reservation). Files without the key are v0.
        static constexpr uint32_t kSchemaVersion = 5;
        /// Written only by the withdrawn operation-id ledger build (b2d2953).
        /// Never reuse it.
        static constexpr uint32_t kRetiredLedgerSchemaVersion = 3;
        static_assert(kSchemaVersion != kRetiredLedgerSchemaVersion, "schema v3 is retired");

        TFDatabase() = default;
        ~TFDatabase();

        bool Open(const std::filesystem::path& path); // false on missing parent/lock/stat/read/parse failure
        bool Close();                                 // successful mutations are already durable; releases the binding
        bool IsOpen() const { return m_open; }
        TFDatabaseStatus LastStatus() const { return m_status; }
        bool RecoveryLatched() const { return m_recoveryLatched; }
        /// Character whose stale baseline made the last CommitCharacterUpdates
        /// fail with status Conflict (0 when the last commit did not conflict).
        uint64_t ConflictedCharacter() const { return m_conflictCharId; }
        /// Row revision this instance's absolute writes for `charId` are checked
        /// against, or nullopt when it has no baseline for that character.
        std::optional<uint64_t> BaselineRevision(uint64_t charId) const
        {
            const auto it = m_baseRevisions.find(charId);
            return it != m_baseRevisions.end() ? std::optional<uint64_t>(it->second) : std::nullopt;
        }

        // Accounts
        bool CreateAccount(const std::string& username, const std::string& salt, const std::string& hash,
                           TFAccountRecord& out);                                      // false if username taken
        bool FindAccountByUsername(const std::string& username, TFAccountRecord& out); // false if none
        bool TouchLogin(uint64_t accountId, int64_t nowMs);
        /// Replace an account's stored credential (NET-100 migration of legacy rows).
        bool UpdateAccountPasswordHash(uint64_t accountId, const std::string& hash);

        // Characters
        bool CreateCharacter(uint64_t accountId, const std::string& name, FactionId faction,
                             TFCharacterRecord& out); // false if name taken
        bool FindCharacterByName(const std::string& name, TFCharacterRecord& out);
        std::vector<TFCharacterRecord> ListCharacters(uint64_t accountId);
        bool FindCharacter(uint64_t charId, TFCharacterRecord& out);
        /// FindCharacter that also adopts the row's current revision as this
        /// instance's baseline. Call it where the caller starts working from
        /// the returned values (enter world) and to retry after a Conflict.
        bool AcquireCharacter(uint64_t charId, TFCharacterRecord& out);
        /// Delete a character row. TF-120: refused, writing nothing, with status ResidentElsewhere while the row
        /// is resident on a continent whose authority lock is held (a bound instance's own continent included).
        bool DeleteCharacter(uint64_t charId);

        // --- TF-120 residency (TFDatabaseResidency.cpp; contract in the file header) ---------------------------

        /// Bind this open instance to `continentKey` for its lifetime (until Close). Waits up to the lock
        /// timeout for the continent's authority lock; a live holder fails the bind with AuthorityHeld. On
        /// success, every row still resident on this continent (left by a dead earlier authority) is cleared
        /// in one transaction. False on a closed or already bound instance or an invalid key.
        bool BindAuthority(std::string_view continentKey);
        /// Continent this instance is bound to, empty when unbound.
        const std::string& BoundContinent() const { return m_boundContinent; }
        /// Enter-world claim: make `charId` resident on the bound continent and adopt its row revision as
        /// the baseline (AcquireCharacter for a bound authority). Refused, writing nothing, with status
        /// ResidentElsewhere while another continent whose authority is alive holds the character; a dead
        /// holder is taken over. Claiming a character already resident here only re-adopts the baseline.
        /// A non-zero `expectedAccountId` is checked inside the claim transaction: a row owned by another
        /// account is refused and nothing is written.
        bool ClaimCharacter(uint64_t charId, TFCharacterRecord& out, uint64_t expectedAccountId = 0);
        /// Leave-world release: clear the residency if it is on the bound continent (idempotent otherwise)
        /// and drop this instance's baseline, so no later absolute write lands from here.
        bool ReleaseCharacter(uint64_t charId);
        /// Reserve a handoff while the bound source remains the character's resident owner. Repeating the same
        /// operation is idempotent; a different operation is refused until the existing reservation is resolved.
        bool ReserveMigration(uint64_t charId, std::string_view operationId, uint64_t operationEpoch,
                              std::string_view destinationContinent, std::string_view payload, TFCharacterRecord& out);
        /// Commit a reserved handoff on the bound destination. The operation identity and destination are checked
        /// inside the transaction; a repeated commit of the same operation is idempotent.
        bool CommitMigration(uint64_t charId, std::string_view operationId, TFCharacterRecord& out);
        /// Abort on the bound source. A duplicate succeeds only for that same aborted operation while the source
        /// still owns the character; a committed operation, missing row or I/O failure is refused.
        bool AbortMigration(uint64_t charId, std::string_view operationId);
        bool SaveCharacterProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux, int64_t lastPlayedMs);

        /// W6 progression expansion: overwrite the meta block (unlocks / loadout /
        /// per-weapon stats) of one character and flush. Additive counterpart to
        /// SaveCharacterProgress; no-op if charId is unknown.
        /// loadout-depth wave: grenade/suit are additive params appended at the
        /// end so this stays a pure extension of the W6 signature.
        bool SaveCharacterMeta(uint64_t charId, const std::vector<std::string>& unlocks,
                               const std::string& loadoutPrimary, const std::string& loadoutSecondary,
                               const std::string& loadoutTool, const std::string& loadoutGrenade,
                               const std::string& loadoutSuit, const std::vector<TFWeaponStatsRow>& stats);

        /// Apply every update in one atomic disk commit. Fails (and changes
        /// nothing on disk) if any row is invalid, names an unknown character,
        /// repeats a character, the write fails, or (status Conflict) any row
        /// changed since this instance's baseline for it.
        bool CommitCharacterUpdates(const std::vector<TFCharacterUpdate>& updates);

        /// Snapshot the committed file at `dbPath` into `backupPath` plus the
        /// digest sidecar BackupDigestPath(backupPath). Holds the authority lock
        /// while reading, so the copy is exactly one committed revision; validates
        /// it like Open, writes both files durably, then re-reads and verifies
        /// them. Never overwrites an existing backup. Safe while authorities run.
        static TFBackupStatus CreateBackup(const std::filesystem::path& dbPath, const std::filesystem::path& backupPath,
                                           TFBackupInfo& info);
        /// Replace the database at `dbPath` with the verified contents of
        /// `backupPath` (digest, load validation and schema gate must all pass;
        /// older-schema backups are migrated on write). Holds the authority lock,
        /// preserves the displaced primary, and writes a revision above the
        /// backup's and, when it can be read, the displaced primary's (reported
        /// in info.supersedesPrimaryRevision). Also the recovery path for a
        /// quarantined corrupt primary. Stop every authority on `dbPath` first
        /// and restart them after: when the displaced revision is known a running
        /// one fails closed rather than serving the rollback, but when it is not
        /// (missing or torn primary) later revisions can repeat lost ones and a
        /// running authority's stale row baseline could match them.
        static TFBackupStatus RestoreFromBackup(const std::filesystem::path& backupPath,
                                                const std::filesystem::path& dbPath, TFBackupInfo& info);
        /// "<backupPath>.sha256", the sha256sum-format digest sidecar.
        static std::filesystem::path BackupDigestPath(const std::filesystem::path& backupPath);

      private:
        enum class LoadResult : uint8_t
        {
            Loaded,
            Unreadable,
            Corrupt,
            UnsupportedVersion,
        };

        /// One committed file state. Transactions build a fresh one per call.
        struct Snapshot
        {
            uint64_t revision = 0;
            std::vector<TFAccountRecord> accounts;
            std::vector<TFCharacterRecord> characters;
            uint64_t nextAccountId = 1;
            uint64_t nextCharId = 1;
        };

        /// Mutation callback: edits the fresh snapshot, stamping changed
        /// character rows with `newRevision`. Returning false writes nothing.
        using Mutation = std::function<bool(Snapshot& fresh, uint64_t newRevision)>;

        /// Bound on waiting for another authority's transaction (or for a continent's authority lock).
        /// Transactions are a single small file rewrite, so a longer wait means a stuck peer.
        static constexpr std::chrono::milliseconds kLockTimeout{2000};

        /// `<file>.authority.<continentKey>`; ExclusiveFileLock appends ".lock".
        std::filesystem::path AuthorityLockTarget(std::string_view continentKey) const;
        /// True while some authority (this instance included) holds `continentKey`'s authority lock.
        bool IsHeldByLiveAuthority(std::string_view continentKey) const;

        LoadResult LoadFromDisk(Snapshot& out) const;
        /// Validate and decode committed file bytes (LoadFromDisk after the read).
        LoadResult ParseSnapshot(const std::string& text, Snapshot& out) const;
        bool SaveToDisk(const Snapshot& snapshot) const;
        bool Refresh(Snapshot& fresh);
        void FailClosed(LoadResult load);
        bool Transact(const char* operation, const Mutation& mutation);
        bool RefreshSnapshot();

        std::filesystem::path m_path;
        bool m_open = false;
        bool m_recoveryLatched = false;
        TFDatabaseStatus m_status = TFDatabaseStatus::Closed;

        Snapshot m_snapshot;                                    ///< last committed state this instance saw
        std::unordered_map<uint64_t, uint64_t> m_baseRevisions; ///< charId -> baseline row revision
        uint64_t m_conflictCharId = 0;                          ///< see ConflictedCharacter()

        std::string m_boundContinent;                 ///< TF-120: BindAuthority key, empty when unbound
        SavePaths::ExclusiveFileLock m_authorityLock; ///< held while bound
    };

} // namespace Terrafront
