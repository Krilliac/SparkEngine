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
 * - Platform simulation (moving, falling, disappearing, rotating) and the
 *   module-local AABB colliders the player controller resolves against
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/PlatformerEnums.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Platformer
{

    /**
     * @brief A single platform placement within a level
     *
     * posX/posZ are the centre of the platform footprint and posY is its top
     * (walkable) surface; the box extends @c height below that surface.
     */
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

        // Rotating platform spin about the vertical axis, in degrees per second
        float rotationSpeed = 45.0f;
    };

    /**
     * @brief World-space collision box for one platform of the loaded level
     *
     * Platformer collision is module-local axis-aligned boxes, not Jolt bodies.
     * Colliders are indexed identically to LevelDef::platforms.
     */
    struct PlatformCollider
    {
        PlatformType type{PlatformType::Static};
        float minX = 0.0f;
        float maxX = 0.0f;
        float minY = 0.0f;
        float maxY = 0.0f; ///< Top (walkable) surface
        float minZ = 0.0f;
        float maxZ = 0.0f;
        float deltaX = 0.0f; ///< Displacement during the last StepPlatforms call (carries riders)
        float deltaY = 0.0f;
        float deltaZ = 0.0f;
        float surfaceVelocityX = 0.0f; ///< Conveyor belt surface speed along X
        float bounceForce = 0.0f;      ///< Launch speed for Bouncy platforms (0 for every other type)
        bool solid = true;             ///< False while a Disappearing platform is hidden
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
        float killPlaneY = -15.0f; ///< Falling below this height costs a life and respawns the player
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

        /// @brief Stars required before level @p index unlocks (0 for an out-of-range index)
        uint32_t GetRequiredStarsToUnlock(uint32_t index) const;

        /// @brief Per-level progress, indexed identically to the level definitions
        const std::vector<LevelProgress>& GetProgress() const { return m_progress; }

        /**
         * @brief Replace every level's progress with a saved copy.
         *
         * The caller validates the values (PlatformerProgress::Validate); this only refuses a copy whose level
         * count differs from the defined levels.
         * @return false, leaving progress unchanged, when @p progress has the wrong size
         */
        bool RestoreProgress(const std::vector<LevelProgress>& progress);

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

        /// @brief Advance moving, falling, disappearing, and rotating platforms by one fixed step.
        void StepPlatforms(float fixedDeltaTime);

        /// @brief Colliders of the loaded level (empty until LoadLevel succeeds).
        const std::vector<PlatformCollider>& GetActiveColliders() const { return m_colliders; }

        /// @brief Height below which the player is out of the loaded level.
        float GetKillPlaneY() const;

        /// @brief Report that the player is standing on a platform (starts a Falling platform's collapse).
        void NotifyPlatformStoodOn(size_t colliderIndex);

      private:
        /// @brief Simulation state for one platform of the loaded level
        struct PlatformRuntime
        {
            float x = 0.0f; ///< Current footprint centre / top surface
            float y = 0.0f;
            float z = 0.0f;
            float travel = 0.0f;          ///< Moving: distance travelled along the start->end segment
            float travelDirection = 1.0f; ///< Moving: +1 towards end, -1 back to start
            bool triggered = false;       ///< Falling: player has stood on it
            float collapseTimer = 0.0f;   ///< Falling: time since triggered, then time since collapse
            bool collapsed = false;       ///< Falling: dropping away
            float fallVelocity = 0.0f;    ///< Falling: downward speed while collapsed
            float cycleTimer = 0.0f;      ///< Disappearing: position within the visible+hidden cycle
            float angleDegrees = 0.0f;    ///< Rotating: yaw of the footprint
        };

        void ResetPlatformRuntime();
        void RebuildColliders();
        void BuildLevelDefinitions();
        void BuildGrasslandsLevel();
        void BuildDesertLevel();
        void BuildSkyLevel();
        int CalculateStars(uint32_t levelIndex, float time, int deaths) const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<LevelDef> m_levels;
        std::vector<LevelProgress> m_progress;
        std::vector<PlatformRuntime> m_platformRuntime;
        std::vector<PlatformCollider> m_colliders;
        uint32_t m_currentLevel{0};
        float m_levelTimer{0.0f};
        int m_levelDeaths{0};
        bool m_levelActive{false};
        bool m_initialized{false};
    };

} // namespace Platformer
