/**
 * @file MMOGameplaySession.h
 * @brief Playable vertical slice that connects the MMO module's gameplay systems.
 */

#pragma once

#include "Achievement/MMOAchievementSystem.h"
#include "Crafting/MMOCraftingSystem.h"
#include "Dungeon/MMODungeonSystem.h"
#include "Inventory/MMOInventorySystem.h"
#include "Reputation/MMOReputationSystem.h"

#include <cstdint>
#include <string>
#include <unordered_set>

namespace MMO
{

    class MMOPlayerSystem;
    class MMOWorldSetup;
    class MMOWorldBossSystem;
    struct CharacterSaveData;

    /**
     * @brief Owns one local character's runtime state and exposes a compact MMO loop.
     *
     * The registries in the individual MMO systems are useful on their own, but a
     * showcase also needs a live player state that consumes them. This session ties
     * movement, gathering, crafting, inventory, dungeons, bosses, achievements,
     * reputation, defeat/respawn, and persistence into one deterministic flow.
     */
    class MMOGameplaySession
    {
      public:
        bool Initialize(MMOPlayerSystem* playerSystem, MMOWorldSetup* worldSetup, MMOInventorySystem* inventorySystem,
                        MMOCraftingSystem* craftingSystem, MMOAchievementSystem* achievementSystem,
                        MMOReputationSystem* reputationSystem, MMODungeonSystem* dungeonSystem,
                        MMOWorldBossSystem* worldBossSystem);
        void Shutdown();
        void Update(float deltaTime);
        void RenderDebugUI();

        std::string ResetDemo();
        std::string ActivateCharacter(uint32_t accountId, uint32_t characterId, const std::string& name, int level,
                                      float maxHealth, uint32_t areaId, float x, float y, float z);
        std::string Travel(uint32_t areaId);
        std::string Gather(uint32_t itemId, int count = 1);
        std::string Craft(uint32_t recipeId);
        std::string UseItem(uint32_t itemId);
        std::string AttackWorldBoss(uint32_t bossId);
        std::string EnterDungeon(uint32_t dungeonId);
        std::string DefeatNextDungeonBoss();
        std::string TakeDamage(float amount);
        std::string Respawn();

        [[nodiscard]] bool IsActive() const { return m_active; }
        [[nodiscard]] const InventoryData& GetInventory() const { return m_inventory; }
        [[nodiscard]] const CraftingState& GetCraftingState() const { return m_crafting; }
        [[nodiscard]] const AchievementState& GetAchievementState() const { return m_achievements; }
        [[nodiscard]] const ReputationState& GetReputationState() const { return m_reputation; }
        [[nodiscard]] const DungeonPlayerState& GetDungeonState() const { return m_dungeonState; }
        [[nodiscard]] std::string GetStatusString() const;
        [[nodiscard]] CharacterSaveData BuildSaveData() const;
        bool LoadSaveData(const CharacterSaveData& data);

      private:
        void InitializePlayerProgress();
        void AwardLoot(const std::string& lootTable, int rolls);
        static float SpawnCoordinate(float minimum, float maximum);

        MMOPlayerSystem* m_playerSystem = nullptr;
        MMOWorldSetup* m_worldSetup = nullptr;
        MMOInventorySystem* m_inventorySystem = nullptr;
        MMOCraftingSystem* m_craftingSystem = nullptr;
        MMOAchievementSystem* m_achievementSystem = nullptr;
        MMOReputationSystem* m_reputationSystem = nullptr;
        MMODungeonSystem* m_dungeonSystem = nullptr;
        MMOWorldBossSystem* m_worldBossSystem = nullptr;

        InventoryData m_inventory;
        CraftingState m_crafting;
        AchievementState m_achievements;
        ReputationState m_reputation;
        DungeonPlayerState m_dungeonState;

        uint32_t m_accountId = 0;
        uint32_t m_characterId = 1;
        float m_playTime = 0.0f;
        float m_respawnTimer = 0.0f;
        float m_lastPlayerX = 0.0f;
        float m_lastPlayerZ = 0.0f;
        float m_distanceTraveled = 0.0f;
        int m_areaVisitCountFloor = 0;
        std::unordered_set<uint32_t> m_visitedAreas;
        std::string m_lastEvent;
        bool m_active = false;

        static constexpr float RESPAWN_DELAY = 3.0f;
        static constexpr float DEMO_BOSS_DAMAGE = 50000.0f;
    };

} // namespace MMO
