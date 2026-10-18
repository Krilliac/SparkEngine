/**
 * @file ARPGDungeonSystem.h
 * @brief Procedural dungeon level generation with difficulty tiers
 * @author Spark Engine Team
 * @date 2026
 *
 * Generates dungeon floors with configurable monster density, elite pack
 * chances, and boss encounters. Difficulty scales across four tiers
 * (Normal, Nightmare, Hell, Inferno) with HP/damage/XP multipliers. With a
 * world available it also dresses the crypt entry room with the Blender-authored
 * dungeon kit (Assets/Models/ARPG/Kit) and the ModuleKits ARPG landmarks
 * (Assets/Models/ModuleKits/ARPG) as Transform + MeshRenderer entities it owns.
 * Game thread only.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/ARPGEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ARPG
{

    /// @brief Configuration for a dungeon difficulty tier
    struct DungeonTierConfig
    {
        ARPGDungeonTier tier = ARPGDungeonTier::Normal;
        std::string name;
        float hpMultiplier = 1.0f;
        float damageMultiplier = 1.0f;
        float xpMultiplier = 1.0f;
        float lootBonusChance = 0.0f; ///< Extra % chance for better rarity
        int minMonsterLevel = 1;
        int maxMonsterLevel = 30;
    };

    /// @brief A single generated dungeon floor
    struct DungeonLevel
    {
        ARPGDungeonTier tier = ARPGDungeonTier::Normal;
        int floorNumber = 1;
        uint32_t layoutSeed = 0;     ///< Seed for deterministic layout
        float monsterDensity = 1.0f; ///< Multiplier on base monster count
        bool hasElitePack = false;
        bool hasBoss = false;
        int monsterLevel = 1;
    };

    /**
     * @brief Procedural dungeon generation with tiered difficulty
     */
    class ARPGDungeonSystem
    {
      public:
        ARPGDungeonSystem() = default;
        ~ARPGDungeonSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void Update(float deltaTime);
        void RenderDebugUI();

        // === Dungeon navigation ===
        DungeonLevel GenerateFloor(ARPGDungeonTier tier, int floorNumber);
        const DungeonLevel* GetCurrentFloor() const;
        void DescendToNextFloor();
        void SetDungeonTier(ARPGDungeonTier tier);

        // === Queries ===
        const DungeonTierConfig* GetTierConfig(ARPGDungeonTier tier) const;
        size_t GetTierCount() const { return m_tierConfigs.size(); }
        int GetCurrentFloorNumber() const;
        std::string GetDungeonStatusString() const;

        /// Floors between boss encounters; the first boss floor is also the demo run's final floor.
        static constexpr int BOSS_FLOOR_INTERVAL = 5;
        /// Every crypt kit entity is named with this prefix.
        static constexpr const char* CRYPT_PROP_PREFIX = "Crypt_";

        /// Crypt kit prop entities (full EnTT identifiers) currently placed in the World.
        const std::vector<uint32_t>& GetCryptKitEntities() const { return m_kitEntities; }

        /**
         * @brief Re-place the crypt kit after SaveSystem replaced the World's entities.
         *
         * The load assigns fresh entity identifiers, so the cached ones are dropped without being destroyed,
         * every restored entity named CRYPT_PROP_PREFIX* is removed and the current kit is placed again.
         */
        void RebuildCryptKitAfterWorldLoad();

      private:
        void RegisterTierConfigs();
        void PlaceCryptKit();
        void RemoveCryptKit();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<uint32_t> m_kitEntities; ///< Crypt kit props (MeshRenderer entities) owned by this system
        std::vector<DungeonTierConfig> m_tierConfigs;
        std::vector<DungeonLevel> m_floors;
        ARPGDungeonTier m_currentTier = ARPGDungeonTier::Normal;
        int m_currentFloorIndex = -1; ///< -1 = not in dungeon

        static constexpr float BASE_ELITE_CHANCE = 0.15f;
        static constexpr float ELITE_CHANCE_PER_FLOOR = 0.03f;
    };

} // namespace ARPG
