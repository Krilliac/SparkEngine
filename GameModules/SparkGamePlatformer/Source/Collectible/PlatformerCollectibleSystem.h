/**
 * @file PlatformerCollectibleSystem.h
 * @brief Collectible management: coins, gems, stars, keys, power-ups
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages all collectible items in platformer levels:
 * - Coins (basic currency, abundant)
 * - Gems (bonus score, harder to reach)
 * - Stars (3 per level, gate progression)
 * - Health pickups and extra lives
 * - Ability orbs (permanent ability unlock)
 * - Keys (open locked doors/gates)
 * - Per-level collection tracking
 * - Magnet power-up attraction radius
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/PlatformerEnums.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Platformer
{

    /// @brief A single collectible instance placed in a level
    struct CollectibleInstance
    {
        uint32_t id = 0;
        CollectibleType type{CollectibleType::Coin};
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float collectionRadius = 1.0f;
        bool collected = false;

        /// Value depends on type: coins = 1, gems = 5, etc.
        int value = 1;

        /// For AbilityOrb: which ability this unlocks
        PowerUpType abilityType{PowerUpType::DoubleJump};

        /// For power-ups: duration of the temporary effect
        float powerUpDuration = 10.0f;
    };

    /// @brief Per-level collection summary
    struct LevelCollectionStats
    {
        int totalCoins = 0;
        int collectedCoins = 0;
        int totalGems = 0;
        int collectedGems = 0;
        int totalStars = 0;
        int collectedStars = 0;
        int totalKeys = 0;
        int collectedKeys = 0;
    };

    /**
     * @brief Manages collectible items across all levels
     *
     * Collectibles are placed by the level system and tracked per-level.
     * The magnet power-up increases the collection radius, attracting
     * nearby items toward the player. Stars gate level progression.
     */
    class PlatformerCollectibleSystem
    {
      public:
        PlatformerCollectibleSystem() = default;
        ~PlatformerCollectibleSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Render();
        void Shutdown();

        void RenderDebugUI();

        /// @brief Total collectibles placed across all levels
        size_t GetTotalCollectibleCount() const { return m_collectibles.size(); }

        /// @brief Total coins collected (global counter)
        int GetCoinsCollected() const { return m_coinsCollected; }

        /// @brief Total stars collected (global counter, gates progression)
        int GetStarsCollected() const { return m_starsCollected; }

        /// @brief Check collection for a given player position and magnet state
        void CheckCollection(float playerX, float playerY, float playerZ, bool magnetActive);

        /// @brief Reset collectibles for a level reload
        void ResetLevel(uint32_t levelIndex);

        /// @brief Get current level's collection stats
        LevelCollectionStats GetCurrentLevelStats() const;

        /// @brief Console output: collection summary
        std::string GetCollectionString() const;

      private:
        void BuildDemoCollectibles();
        void SpawnCollectibleLine(float startX, float y, float z, int count, float spacing, CollectibleType type);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<CollectibleInstance> m_collectibles;
        uint32_t m_nextId{1};

        // Global counters
        int m_coinsCollected{0};
        int m_gemsCollected{0};
        int m_starsCollected{0};
        int m_keysCollected{0};

        float m_magnetRadius{8.0f};   ///< Collection radius when magnet is active
        float m_bobAmplitude{0.3f};   ///< Vertical bobbing animation amplitude
        float m_bobFrequency{2.0f};   ///< Bobbing cycles per second
        float m_spinSpeed{180.0f};    ///< Rotation degrees per second
        float m_animationTimer{0.0f}; ///< Shared animation timer

        bool m_initialized{false};
    };

} // namespace Platformer
