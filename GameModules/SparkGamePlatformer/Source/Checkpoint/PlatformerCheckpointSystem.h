/**
 * @file PlatformerCheckpointSystem.h
 * @brief Checkpoint and respawn system for platformer levels
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages checkpoint flags placed throughout levels:
 * - Checkpoint activation on player proximity
 * - Respawn at last activated checkpoint
 * - Visual feedback (flag animation on activation)
 * - Per-level checkpoint tracking
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Player/PlatformerPlayerController.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Platformer
{

    /// @brief A single checkpoint placed in a level
    struct CheckpointData
    {
        uint32_t id = 0;
        uint32_t levelIndex = 0;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float activationRadius = 2.0f;
        bool activated = false;
        float animationTimer = 0.0f; ///< Timer for flag raise animation
    };

    /**
     * @brief Manages checkpoint placement, activation, and respawn positions
     *
     * Checkpoints are placed by the level system at key progress points.
     * When the player enters a checkpoint's activation radius, it becomes
     * the active respawn point. The last activated checkpoint is used for
     * respawning after death.
     */
    class PlatformerCheckpointSystem
    {
      public:
        PlatformerCheckpointSystem() = default;
        ~PlatformerCheckpointSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// @brief Total checkpoints across all levels
        size_t GetCheckpointCount() const { return m_checkpoints.size(); }

        /// @brief Number of activated checkpoints
        size_t GetActivatedCount() const;

        /// @brief Check if player is near any unactivated checkpoint
        void CheckActivation(float playerX, float playerY, float playerZ);

        /// @brief Get the position of the last activated checkpoint
        PlayerPosition GetLastCheckpointPosition() const;

        /// @brief Reset all checkpoints for a level reload
        void ResetLevel(uint32_t levelIndex);

        /// @brief Set the initial spawn point (used before any checkpoint is activated)
        void SetLevelSpawn(float x, float y, float z);

      private:
        void BuildDemoCheckpoints();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<CheckpointData> m_checkpoints;
        uint32_t m_lastActivatedId{0};
        uint32_t m_nextId{1};
        PlayerPosition m_levelSpawn{0.0f, 1.0f, 0.0f};
        bool m_initialized{false};
    };

} // namespace Platformer
