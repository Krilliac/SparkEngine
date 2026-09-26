/**
 * @file PlatformerProgress.h
 * @brief Versioned, bounded save codec for platformer progress (levels, collectibles, checkpoints, lives, abilities).
 *
 * The snapshot is stored as one SaveSystem custom-state entry (StateKey) next to the ECS world snapshot. It holds
 * every level's LevelProgress, the ids of collected items and the global collection counters, the activated
 * checkpoints and the respawn checkpoint, and the player's lives and unlocked abilities. Collectible and checkpoint
 * ids are assigned in a fixed build order, so the same id names the same item in every run of this build.
 *
 * Floats are written as their IEEE-754 bit patterns so a round trip is exact. The decoder rejects any other format
 * version, oversized or truncated input, trailing data and out-of-range values without touching its output, and
 * Validate rejects a snapshot that does not fit the live systems (level count, unknown ids, impossible unlocks).
 */

#pragma once

#include "Checkpoint/PlatformerCheckpointSystem.h"
#include "Collectible/PlatformerCollectibleSystem.h"
#include "Level/PlatformerLevelSystem.h"
#include "Player/PlatformerPlayerController.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Platformer
{
    /// @brief Non-owning pointers to the gameplay systems whose progress is persisted.
    struct PlatformerProgressSystems
    {
        PlatformerLevelSystem* level = nullptr;
        PlatformerCollectibleSystem* collectibles = nullptr;
        PlatformerCheckpointSystem* checkpoints = nullptr;
        PlatformerPlayerController* player = nullptr;

        /// @brief True when every system is bound.
        bool IsComplete() const { return level && collectibles && checkpoints && player; }
    };

    /// @brief Everything a platformer save restores.
    struct PlatformerProgressSnapshot
    {
        std::vector<LevelProgress> levels; ///< Indexed like the level definitions
        CollectionProgress collection;
        CheckpointProgress checkpoints;
        PlayerProgress player;
    };

    /// @brief Capture, encode, decode, validate, and apply platformer progress.
    class PlatformerProgress
    {
      public:
        /// Save custom-state key; a future incompatible layout uses a new key.
        static constexpr std::string_view StateKey = "SparkGamePlatformer.progress.v1";
        static constexpr uint32_t FormatVersion = 1;

        static constexpr size_t MAX_ENCODED_BYTES = 32768; ///< Well under SaveSystem's 64 KiB string limit
        static constexpr size_t MAX_LEVELS = 256;
        static constexpr size_t MAX_IDS = 2048;           ///< Per list (collected items, activated checkpoints)
        static constexpr int MAX_COUNTER = 1000000;       ///< Collection counters and best-death counts
        static constexpr float MAX_BEST_TIME = 360000.0f; ///< 100 hours
        static constexpr int MAX_STARS_PER_LEVEL = 3;

        /// @brief Slot names are 1-64 characters of [A-Za-z0-9_-].
        static bool IsValidSlotName(std::string_view slotName);

        /// @pre systems.IsComplete()
        static PlatformerProgressSnapshot Capture(const PlatformerProgressSystems& systems);

        /// @brief Encode a snapshot. @p snapshot is expected to satisfy the decoder's bounds.
        static std::string Serialize(const PlatformerProgressSnapshot& snapshot);

        /**
         * @brief Decode and bounds-check an encoded snapshot.
         * @return false with @p error set, leaving @p outSnapshot unchanged, on any malformed or out-of-range input
         */
        static bool Deserialize(std::string_view text, PlatformerProgressSnapshot& outSnapshot, std::string& error);

        /**
         * @brief Check a decoded snapshot against the live systems: level count, unlock rules, and that every
         *        collectible and checkpoint id exists.
         * @pre systems.IsComplete()
         */
        static bool Validate(const PlatformerProgressSnapshot& snapshot, const PlatformerProgressSystems& systems,
                             std::string& error);

        /**
         * @brief Validate, then restore every system. Nothing changes when validation fails.
         * @pre systems.IsComplete()
         */
        static bool Apply(const PlatformerProgressSnapshot& snapshot, const PlatformerProgressSystems& systems,
                          std::string& error);
    };
} // namespace Platformer
