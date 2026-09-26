/**
 * @file PlatformerLevelFlow.h
 * @brief Per-frame platformer level orchestration shared by the module and its tests.
 *
 * SparkGamePlatformerModule forwards OnUpdate/OnFixedUpdate here, so the scripted level-completion tests
 * run exactly the collection, ability-unlock, checkpoint, hazard, wind, and goal logic the game runs.
 * When the engine has a world, the flow also dresses the loaded level with the Blender platformer kit
 * (Assets/Models/Platformer/Kit): one mesh per platform collider, kept in step with it, plus the goal flag.
 */

#pragma once

#include <cstdint>
#include <vector>

class World;

namespace Platformer
{
    class PlatformerCheckpointSystem;
    class PlatformerCollectibleSystem;
    class PlatformerHazardSystem;
    class PlatformerLevelSystem;
    class PlatformerPlayerController;

    /// @brief What one variable-rate level frame changed.
    struct LevelFrameResult
    {
        bool goalReached = false; ///< The player reached the goal and the level was completed this frame
        int hazardDamage = 0;     ///< Lives removed by hazards this frame (0 when blocked by i-frames)
        uint32_t pickups = 0;     ///< Collectibles picked up this frame
    };

    /**
     * @brief Drives one playable level through the gameplay systems the module owns.
     *
     * Holds non-owning references; the systems must outlive the flow. Variable-rate work (input,
     * timers, collection, checkpoints, hazard damage, goal) runs in StepFrame, and deterministic
     * physics (platforms, player movement, wind) runs in StepFixed.
     */
    class PlatformerLevelFlow
    {
      public:
        PlatformerLevelFlow(PlatformerLevelSystem& level, PlatformerPlayerController& player,
                            PlatformerCollectibleSystem& collectibles, PlatformerHazardSystem& hazards,
                            PlatformerCheckpointSystem& checkpoints);

        /// @brief Removes the level kit entities this flow placed.
        ~PlatformerLevelFlow();

        PlatformerLevelFlow(const PlatformerLevelFlow&) = delete;
        PlatformerLevelFlow& operator=(const PlatformerLevelFlow&) = delete;

        /**
         * @brief Load a level and reset its checkpoints and collectibles, then place the player at its spawn.
         *
         * With an engine world available (the level system's engine context), the previous level's kit meshes are replaced by the new
         * level's: a floating_platform per platform (a spring_pad for Bouncy ones) and a goal_flag.
         * @param index 0-based level index
         * @return false when the level does not exist or is still locked
         */
        bool StartLevel(uint32_t index);

        /**
         * @brief Advance one variable-rate frame.
         * @param deltaTime Frame time in seconds; non-finite or non-positive values are ignored
         */
        LevelFrameResult StepFrame(float deltaTime);

        /**
         * @brief Advance one fixed physics step: platforms first so the player rides and collides with this
         *        step's positions (their kit meshes follow), then the player, then wind zones.
         * @param fixedDeltaTime Fixed step in seconds; non-finite or non-positive values are ignored
         */
        void StepFixed(float fixedDeltaTime);

      private:
        void PlaceLevelKit();
        void SyncLevelKit();
        void RemoveLevelKit();

        PlatformerLevelSystem& m_level;
        PlatformerPlayerController& m_player;
        PlatformerCollectibleSystem& m_collectibles;
        PlatformerHazardSystem& m_hazards;
        PlatformerCheckpointSystem& m_checkpoints;

        ::World* m_kitWorld{nullptr};        ///< World the kit entities live in (non-owning)
        std::vector<uint32_t> m_kitEntities; ///< One per platform collider (same index), then the goal flag
    };
} // namespace Platformer
