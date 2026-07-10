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
 * (docs/superpowers/plans/2026-07-06-terrafront-onboarding.md, Task 1 Step
 * 4 / Global Constraints), TFDatabase pivots straight to the atomic-JSON-file
 * pattern used by TFProgressionSystem (TFProgressionSystem.cpp:385-446) while
 * keeping this public interface identical to what Tasks 2-6 depend on.
 *
 * Stores accounts[] + characters[] as JSON in a single file (default
 * "Saves/terrafront.db"). Every mutating call flushes to disk immediately
 * (tmp+rename) so Close() is not required for durability — a fresh
 * TFDatabase instance that re-Opens the same path sees prior writes.
 */
#pragma once

#include "Core/TFTypes.h"   // FactionId

#include <cstdint>
#include <string>
#include <vector>

namespace Terrafront {

struct TFAccountRecord
{
    uint64_t    id = 0;
    std::string username, salt, passwordHash;
    int64_t     createdAtMs = 0, lastLoginMs = 0;
};

struct TFCharacterRecord
{
    uint64_t    id = 0, accountId = 0;
    std::string name;
    FactionId   faction = FactionId::None;
    uint32_t    xp = 0;
    uint16_t    rank = 1;
    uint32_t    flux = 0;
    int64_t     createdAtMs = 0, lastPlayedMs = 0;
};

class TFDatabase
{
  public:
    TFDatabase() = default;
    ~TFDatabase();

    bool Open(const std::string& path);   // e.g. "Saves/terrafront.db"; false on failure
    void Close();
    bool IsOpen() const { return m_open; }

    // Accounts
    bool CreateAccount(const std::string& username, const std::string& salt, const std::string& hash,
                        TFAccountRecord& out);   // false if username taken
    bool FindAccountByUsername(const std::string& username, TFAccountRecord& out);   // false if none
    void TouchLogin(uint64_t accountId, int64_t nowMs);

    // Characters
    bool CreateCharacter(uint64_t accountId, const std::string& name, FactionId faction,
                          TFCharacterRecord& out);   // false if name taken
    bool FindCharacterByName(const std::string& name, TFCharacterRecord& out);
    std::vector<TFCharacterRecord> ListCharacters(uint64_t accountId);
    bool FindCharacter(uint64_t charId, TFCharacterRecord& out);
    bool DeleteCharacter(uint64_t charId);
    void SaveCharacterProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux, int64_t lastPlayedMs);

  private:
    bool LoadFromDisk();
    bool SaveToDisk() const;

    std::string m_path;
    bool m_open = false;

    std::vector<TFAccountRecord>   m_accounts;
    std::vector<TFCharacterRecord> m_characters;
    uint64_t m_nextAccountId = 1;
    uint64_t m_nextCharId    = 1;
};

} // namespace Terrafront
