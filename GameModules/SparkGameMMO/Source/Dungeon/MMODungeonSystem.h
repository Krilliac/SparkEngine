/**
 * @file MMODungeonSystem.h
 * @brief Instanced dungeon system with boss encounters and loot lockouts
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMO
{

    /// @brief Boss encounter within a dungeon
    struct DungeonBoss
    {
        uint32_t id = 0;
        std::string name;
        float maxHealth = 10000.0f;
        float currentHealth = 10000.0f;
        int enrageTimer = 600;
        bool defeated = false;
        int lootTableId = 0;
        float GetHealthPct() const { return maxHealth > 0 ? currentHealth / maxHealth : 0.0f; }
    };

    /// @brief Dungeon template definition
    struct DungeonDef
    {
        uint32_t id = 0;
        std::string name;
        std::string description;
        std::string scenePath;
        int minLevel = 1;
        int recommendedPlayers = 5;
        int maxPlayers = 5;
        float lockoutDuration = 604800.0f; // 1 week
        std::vector<DungeonBoss> bosses;
        float heroicHealthMult = 1.5f;
        float mythicHealthMult = 2.5f;
        float legendaryHealthMult = 4.0f;
    };

    /// @brief Instance state
    enum class InstanceState : uint8_t
    {
        Loading,
        Active,
        Completed,
        Failed,
        Expired
    };

    /// @brief A live dungeon instance
    struct DungeonInstance
    {
        uint32_t instanceId = 0;
        uint32_t dungeonDefId = 0;
        DungeonDifficulty difficulty = DungeonDifficulty::Normal;
        InstanceState state = InstanceState::Loading;
        std::unordered_set<uint32_t> playerIds;
        std::vector<DungeonBoss> bosses;
        float elapsed = 0.0f;
        float maxDuration = 7200.0f;
        int wipeCount = 0;
        bool AllBossesDefeated() const;
    };

    /// @brief Per-player lockout
    struct LootLockout
    {
        uint32_t dungeonDefId = 0;
        DungeonDifficulty difficulty = DungeonDifficulty::Normal;
        std::unordered_set<uint32_t> defeatedBossIds;
        float remaining = 0.0f;
    };

    /// @brief Per-player dungeon state
    struct DungeonPlayerState
    {
        uint32_t currentInstanceId = 0;
        std::vector<LootLockout> lockouts;
        int dungeonsCompleted = 0;
        int bossesKilled = 0;
        bool InDungeon() const { return currentInstanceId != 0; }
    };

    /**
     * @brief Dungeon instance management
     */
    class MMODungeonSystem
    {
      public:
        MMODungeonSystem() = default;
        ~MMODungeonSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float dt);
        void Shutdown();
        void RenderDebugUI();

        void RegisterDungeon(const DungeonDef& def);
        const DungeonDef* GetDungeon(uint32_t id) const;

        uint32_t CreateInstance(uint32_t dungeonDefId, DungeonDifficulty diff, const std::vector<uint32_t>& playerIds);
        bool EnterInstance(uint32_t instanceId, uint32_t playerId);
        bool LeaveInstance(uint32_t instanceId, uint32_t playerId);
        bool DefeatBoss(uint32_t instanceId, uint32_t bossId);
        void RecordWipe(uint32_t instanceId);
        void DestroyInstance(uint32_t instanceId);

        const DungeonInstance* GetInstance(uint32_t instanceId) const;
        bool HasLockout(const DungeonPlayerState& ps, uint32_t dungeonId, DungeonDifficulty diff) const;
        void AddBossLockout(DungeonPlayerState& ps, uint32_t dungeonId, DungeonDifficulty diff, uint32_t bossId,
                            float duration);
        void UpdateLockouts(DungeonPlayerState& ps, float dt);
        float GetHealthMult(DungeonDifficulty diff, uint32_t dungeonId) const;

        size_t GetDungeonCount() const { return m_dungeons.size(); }
        size_t GetInstanceCount() const { return m_instances.size(); }
        std::string GetDungeonListString() const;
        std::string GetInstanceInfoString(uint32_t instanceId) const;

      private:
        void RegisterDefaultDungeons();

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, DungeonDef> m_dungeons;
        std::unordered_map<uint32_t, DungeonInstance> m_instances;
        uint32_t m_nextInstanceId = 1;

        static constexpr int MAX_INSTANCES = 50;
    };

} // namespace MMO
