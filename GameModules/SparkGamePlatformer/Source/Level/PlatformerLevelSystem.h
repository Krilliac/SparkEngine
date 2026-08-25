/**
 * @file PlatformerLevelSystem.h
 * @brief Level management: layout, platforms, themes, progression, and star ratings
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the structure and progression of platformer levels:
 * - Level definitions with platform placements, spawn points, and goals
 * - Level themes (Grasslands, Desert, Snow, etc.) for visual variation
 * - Star rating per level (time, collectibles, deaths)
 * - Level unlock progression
 * - Secret area tracking
 * - Speedrun timer
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/PlatformerEnums.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Platformer
{

    /// @brief A single platform placement within a level
    struct PlatformDef
    {
        PlatformType type{PlatformType::Static};
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float width = 4.0f;
        float height = 0.5f;
        float depth = 4.0f;

        // Moving platform waypoints (for PlatformType::Moving)
        float endX = 0.0f;
        float endY = 0.0f;
        float endZ = 0.0f;
        float moveSpeed = 2.0f;

        // Conveyor direction (normalized)
        float conveyorDirX = 1.0f;
        float conveyorSpeed = 3.0f;

        // Disappearing platform timing
        float visibleTime = 2.0f;
        float hiddenTime = 1.5f;

        // Falling platform delay before collapse
        float fallDelay = 0.5f;

        // Bouncy platform launch force
        float bounceForce = 15.0f;
    };

    /// @brief Spawn point definition
    struct SpawnPoint
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// @brief Star rating thresholds for a level
    struct StarThresholds
    {
        float timeStar1 = 120.0f; ///< Complete within this time for 1 star
        float timeStar2 = 90.0f;  ///< Complete within this time for 2 stars
        float timeStar3 = 60.0f;  ///< Complete within this time for 3 stars
        int maxDeathsStar = 0;    ///< Zero deaths for bonus star consideration
    };

    /// @brief Complete level definition
    struct LevelDef
    {
        uint32_t id = 0;
        std::string name;
        LevelTheme theme{LevelTheme::Grasslands};
        SpawnPoint spawnPoint{};
        SpawnPoint goalPoint{};
        std::vector<PlatformDef> platforms;
        StarThresholds starThresholds{};
        uint32_t requiredStarsToUnlock = 0;
        bool hasSecretArea = false;
    };

    /// @brief Per-level progress tracking
    struct LevelProgress
    {
        bool completed = false;
        bool unlocked = false;
        int starsEarned = 0;
        float bestTime = 0.0f;
        int bestDeaths = 0;
        bool secretFound = false;
    };

    /**
     * @brief Manages level definitions, progression, and active level state
     *
     * Levels are defined as data-driven structures containing platform placements,
     * spawn/goal points, theme info, and star thresholds. The system handles level
     * loading, unlock progression, timer tracking, and star rating calculation.
     */
    class PlatformerLevelSystem
    {
      public:
        PlatformerLevelSystem() = default;
        ~PlatformerLevelSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Render();
        void Shutdown();

        void RenderDebugUI();

        /// @brief Load a level by index (0-based)
        bool LoadLevel(uint32_t index);

        /// @brief Get the number of defined levels
        size_t GetLevelCount() const { return m_levels.size(); }

        /// @brief Get currently active level index
        uint32_t GetCurrentLevelIndex() const { return m_currentLevel; }

        /// @brief Get the current level's spawn point
        SpawnPoint GetCurrentSpawnPoint() const;

        /// @brief Get total stars earned across all levels
        int GetTotalStarsEarned() const;

        /// @brief Mark the current level as completed, calculate star rating
        void CompleteLevel(float completionTime, int deaths);

        /// @brief Complete the active level when the player reaches its projected goal trigger.
        bool TryCompleteAtPosition(float playerX, float playerY, float playerZ, int deaths);

        bool IsLevelActive() const { return m_levelActive; }

        /// @brief Console output: list of all levels with progress
        std::string GetLevelListString() const;

        /// @brief Get current level timer
        float GetLevelTimer() const { return m_levelTimer; }

      private:
        void BuildLevelDefinitions();
        void BuildGrasslandsLevel();
        void BuildDesertLevel();
        void BuildSkyLevel();
        int CalculateStars(uint32_t levelIndex, float time, int deaths) const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<LevelDef> m_levels;
        std::vector<LevelProgress> m_progress;
        uint32_t m_currentLevel{0};
        float m_levelTimer{0.0f};
        int m_levelDeaths{0};
        bool m_levelActive{false};
        bool m_initialized{false};
    };

} // namespace Platformer
