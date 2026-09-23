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
 * TerraFront authority code passes SavePaths::File("terrafront.db"). Every
 * mutating call flushes to disk immediately (tmp+rename) and rolls back its
 * in-memory change if that write fails. Close() therefore only releases the
 * authority lock; a fresh TFDatabase instance that re-Opens the same path sees
 * every mutation that previously reported success.
 *
 * DATA-120: CommitCharacterUpdates applies several character rows (e.g. an
 * unlock purchase's flux debit + unlock key, or a transfer between two
 * characters) as ONE disk commit, all-or-nothing. The file carries a
 * "schemaVersion"; files without it are the legacy v0 shape and upgrade on
 * the next write, while a file from a newer schema fails closed so an older
 * build can never load-and-rewrite it and silently drop the newer fields.
 */
#pragma once

#include "Persistence/TFSavePaths.h"

#include "Core/TFTypes.h" // FactionId

#include <cstdint>
#include <filesystem>
#include <string>
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
        /// On-disk schema written by this build. Files without the key are v0.
        static constexpr uint32_t kSchemaVersion = 1;

        TFDatabase() = default;
        ~TFDatabase();

        bool Open(const std::filesystem::path& path); // false on missing parent/stat/read/parse failure
        bool Close();                                 // releases the lock; successful mutations are already durable
        bool IsOpen() const { return m_open; }
        TFDatabaseStatus LastStatus() const { return m_status; }
        bool RecoveryLatched() const { return m_recoveryLatched; }

        // Accounts
        bool CreateAccount(const std::string& username, const std::string& salt, const std::string& hash,
                           TFAccountRecord& out);                                      // false if username taken
        bool FindAccountByUsername(const std::string& username, TFAccountRecord& out); // false if none
        bool TouchLogin(uint64_t accountId, int64_t nowMs);

        // Characters
        bool CreateCharacter(uint64_t accountId, const std::string& name, FactionId faction,
                             TFCharacterRecord& out); // false if name taken
        bool FindCharacterByName(const std::string& name, TFCharacterRecord& out);
        std::vector<TFCharacterRecord> ListCharacters(uint64_t accountId);
        bool FindCharacter(uint64_t charId, TFCharacterRecord& out);
        bool DeleteCharacter(uint64_t charId);
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
        /// nothing, in memory or on disk) if any row is invalid, names an
        /// unknown character, repeats a character, or the write fails.
        bool CommitCharacterUpdates(const std::vector<TFCharacterUpdate>& updates);

      private:
        enum class LoadResult : uint8_t
        {
            Loaded,
            Unreadable,
            Corrupt,
            UnsupportedVersion,
        };
        LoadResult LoadFromDisk();
        bool SaveToDisk() const;

        std::filesystem::path m_path;
        bool m_open = false;
        bool m_recoveryLatched = false;
        TFDatabaseStatus m_status = TFDatabaseStatus::Closed;
        SavePaths::ExclusiveFileLock m_fileLock;

        std::vector<TFAccountRecord> m_accounts;
        std::vector<TFCharacterRecord> m_characters;
        uint64_t m_nextAccountId = 1;
        uint64_t m_nextCharId = 1;
    };

} // namespace Terrafront
