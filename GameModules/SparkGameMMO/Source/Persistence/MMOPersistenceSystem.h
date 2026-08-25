/**
 * @file MMOPersistenceSystem.h
 * @brief MMO character and world persistence using AsyncDatabasePool
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides save/load for all MMO gameplay state:
 * - Character data (inventory, stats, position, area)
 * - Guild membership and guild data
 * - Reputation standings across factions
 * - Achievement progress and unlocked titles
 * - Crafting skill levels and known recipes
 * - Dungeon lockouts
 * - World boss kill history
 *
 * Uses the engine's AsyncDatabasePool with prepared statements for
 * non-blocking persistence. All writes are async; reads can be
 * sync (login) or async (background refresh).
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Engine/Persistence/AsyncDatabase.h"

#include "Achievement/MMOAchievementSystem.h"
#include "Crafting/MMOCraftingSystem.h"
#include "Dungeon/MMODungeonSystem.h"
#include "Guild/MMOGuildSystem.h"
#include "Inventory/MMOInventorySystem.h"
#include "Party/MMOPartySystem.h"
#include "Reputation/MMOReputationSystem.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace MMO
{

    /// @brief Complete character save data bundle
    struct CharacterSaveData
    {
        // Identity
        uint32_t characterId = 0;
        uint32_t accountId = 0;
        std::string name;
        int level = 1;
        int xp = 0;

        // Location
        uint32_t areaId = 1;
        float posX = 0.0f;
        float posY = 1.0f;
        float posZ = 0.0f;
        float rotY = 0.0f;

        // Stats
        float health = 100.0f;
        float maxHealth = 100.0f;
        float mana = 50.0f;
        float maxMana = 50.0f;
        float playTime = 0.0f;

        // Subsystem state
        InventoryData inventory;
        GuildMembership guildMembership;
        ReputationState reputationState;
        AchievementState achievementState;
        CraftingState craftingState;
        DungeonPlayerState dungeonState;

        // Timestamps
        uint64_t createdAt = 0;
        uint64_t lastLogin = 0;
        uint64_t lastSave = 0;
    };

    /// @brief World-level persistent state (guild data, auction house, etc.)
    struct WorldSaveData
    {
        std::vector<Guild> guilds;
        uint32_t nextGuildId = 1;

        // Boss kill history for lockout tracking
        struct BossKillRecord
        {
            uint32_t bossDefId = 0;
            uint64_t killTime = 0;
            int participantCount = 0;
        };
        std::vector<BossKillRecord> bossKillHistory;
    };

    /// @brief Callback for async load completion
    using CharacterLoadCallback = std::function<void(bool success, CharacterSaveData data)>;

    /// @brief Prepared statement IDs for MMO database operations
    enum class MMOStmtId : uint32_t
    {
        // Schema
        CreateCharacterTable = 1000,
        CreateInventoryTable = 1001,
        CreateReputationTable = 1002,
        CreateAchievementTable = 1003,
        CreateCraftingTable = 1004,
        CreateGuildTable = 1005,
        CreateGuildMemberTable = 1006,
        CreateLockoutTable = 1007,
        CreateBossKillTable = 1008,
        CreateRecipeTable = 1009,

        // Character CRUD
        InsertCharacter = 1100,
        UpdateCharacter = 1101,
        LoadCharacter = 1102,
        DeleteCharacter = 1103,
        ListCharacters = 1104,

        // Inventory
        SaveInventorySlot = 1200,
        LoadInventory = 1201,
        ClearInventory = 1202,
        SaveCurrency = 1203,
        LoadCurrency = 1204,

        // Reputation
        SaveReputation = 1300,
        LoadReputation = 1301,
        ClearReputation = 1302,
        LoadReputationValue = 1303,

        // Achievements
        SaveAchievement = 1400,
        LoadAchievements = 1401,
        SaveAchievementStat = 1402,
        LoadAchievementStats = 1403,
        LoadAchievementStatValue = 1404,

        // Crafting
        SaveCraftingSkill = 1500,
        LoadCraftingSkills = 1501,
        SaveKnownRecipe = 1502,
        LoadKnownRecipes = 1503,

        // Guilds
        SaveGuild = 1600,
        LoadGuilds = 1601,
        SaveGuildMember = 1602,
        LoadGuildMembers = 1603,
        DeleteGuildMember = 1604,

        // Lockouts
        SaveLockout = 1700,
        LoadLockouts = 1701,
        ClearExpiredLockouts = 1702,

        // Boss kills
        SaveBossKill = 1800,
        LoadBossKills = 1801,
    };

    /**
     * @brief Async persistence for MMO character and world data
     *
     * Lifecycle:
     * 1. Initialize() — opens database, creates schema
     * 2. LoadCharacter() — sync load at login
     * 3. SaveCharacter() — async save during gameplay (auto-save interval)
     * 4. SaveWorld() — periodic world state flush
     * 5. Shutdown() — final save, close database
     */
    class MMOPersistenceSystem
    {
      public:
        MMOPersistenceSystem() = default;
        ~MMOPersistenceSystem() = default;

        bool Initialize(Spark::IEngineContext* context, const std::string& dbPath = "mmo_data.db");
        void Update(float dt);
        void Shutdown();
        void RenderDebugUI();

        // === Character Persistence ===

        /// Create a new character record, returns character ID (sync)
        uint32_t CreateCharacter(const std::string& name, uint32_t accountId);

        /// Load character data synchronously (call at login)
        bool LoadCharacter(uint32_t characterId, CharacterSaveData& outData);

        /// Save character data asynchronously (call periodically)
        void SaveCharacterAsync(const CharacterSaveData& data);

        /// Save character data synchronously (call at logout/shutdown)
        bool SaveCharacterSync(const CharacterSaveData& data);

        /// Delete a character and all associated data
        bool DeleteCharacter(uint32_t characterId);

        /// List all characters for an account
        std::vector<std::pair<uint32_t, std::string>> ListCharacters(uint32_t accountId);

        // === World Persistence ===

        /// Save world-level data (guilds, boss history) async
        void SaveWorldAsync(const WorldSaveData& data);

        /// Load world-level data sync (at startup)
        bool LoadWorld(WorldSaveData& outData);

        // === Configuration ===

        void SetAutoSaveInterval(float seconds)
        {
            m_autoSaveInterval = seconds > 0.0f ? seconds : 1.0f;
            if (m_autoSaveTimer <= 0.0f || m_autoSaveTimer > m_autoSaveInterval)
                m_autoSaveTimer = m_autoSaveInterval;
        }
        float GetAutoSaveInterval() const { return m_autoSaveInterval; }
        bool IsAutoSaveDue() const { return m_initialized && m_autoSaveTimer <= 0.0f; }
        void ResetAutoSaveTimer() { m_autoSaveTimer = m_autoSaveInterval; }
        bool IsInitialized() const { return m_initialized; }
        int GetPendingWrites() const;

        std::string GetStatusString() const;

      private:
        void RegisterPreparedStatements();
        void CreateSchema();

        // Subsystem save/load helpers
        void SaveInventory(uint32_t charId, const InventoryData& inv);
        void LoadInventory(uint32_t charId, InventoryData& inv);
        void SaveReputationState(uint32_t charId, const ReputationState& state);
        void LoadReputationState(uint32_t charId, ReputationState& state);
        void SaveAchievementState(uint32_t charId, const AchievementState& state);
        void LoadAchievementState(uint32_t charId, AchievementState& state);
        void SaveCraftingState(uint32_t charId, const CraftingState& state);
        void LoadCraftingState(uint32_t charId, CraftingState& state);
        void SaveLockouts(uint32_t charId, const DungeonPlayerState& state);
        void LoadLockouts(uint32_t charId, DungeonPlayerState& state);

        Spark::IEngineContext* m_context{nullptr};
        std::unique_ptr<Spark::Persistence::AsyncDatabasePool> m_db;
        bool m_initialized{false};

        float m_autoSaveInterval = 300.0f; // 5 minutes
        float m_autoSaveTimer = 0.0f;
        int m_totalSaves = 0;
        int m_totalLoads = 0;
        float m_lastSaveTime = 0.0f;
    };

} // namespace MMO
