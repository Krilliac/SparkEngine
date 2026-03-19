/**
 * @file AIComponents.h
 * @brief ECS AI component: AIComponent with behavior tree and pathfinding
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/OpaqueHandle.h"
#include "../../../Utils/Cooldown.h"
#include "../../../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <entt/entt.hpp>
#include <string>
#include <vector>

using EntityID = entt::entity;

// =============================================================================
// AIComponent
// =============================================================================

/**
 * @brief AI agent state, perception data, and pathfinding for an NPC entity.
 *
 * The AIUpdateSystem ticks the behavior tree, advances perception timers,
 * and moves the entity along its current navmesh path each frame.
 */
struct AIComponent
{
    /// High-level behavioral state (drives animation, perception thresholds, etc.).
    enum class State
    {
        Idle,       ///< Standing still, no threat detected.
        Patrolling, ///< Following a patrol route waypoint list.
        Alert,      ///< Heard/saw something suspicious; searching.
        Combat,     ///< Actively engaging a target.
        Fleeing,    ///< Retreating from danger (low health, outnumbered).
        Dead        ///< No longer processing AI; awaiting cleanup.
    };

    State state = State::Idle;                    ///< Current behavioral state.
    std::string behaviorTreeName;                 ///< Name of the BT asset to load.
    Spark::BehaviorTreeHandle behaviorTreeHandle; ///< Opaque handle to the active BehaviorTree instance.
    Spark::NavQueryHandle navQueryHandle;         ///< Opaque handle to a Detour navmesh query for pathfinding.

    // --- Perception ---
    EntityID targetEntity = entt::null;            ///< Currently tracked target entity (player, threat).
    DirectX::XMFLOAT3 lastKnownTargetPos{0, 0, 0}; ///< World-space position where the target was last seen.
    float timeSinceLastSeen = 0.0f;                ///< Seconds since the target was last in line-of-sight.
    float alertTimer = 0.0f;                       ///< Seconds spent in Alert state (for timeout transitions).

    /// Cooldown between attack actions (driven by config.reactionTime)
    Spark::Cooldown attackCooldown{0.5f, true};

    /// Cooldown for perception updates (avoids expensive checks every frame)
    Spark::Cooldown perceptionCooldown{0.1f, true};

    // --- Pathfinding ---
    std::vector<DirectX::XMFLOAT3> currentPath; ///< Navmesh path waypoints (world-space).
    size_t currentPathIndex = 0;                ///< Index of the next waypoint to reach.

    /// Tunable AI parameters (typically set per-archetype, not per-instance).
    struct Config
    {
        float detectionRange = 30.0f; ///< Max distance to detect targets (meters).
        float attackRange = 15.0f;    ///< Distance at which the agent can attack (meters).
        float moveSpeed = 5.0f;       ///< Movement speed along the navmesh path (m/s).
        float reactionTime = 0.5f;    ///< Minimum delay between attack actions (seconds).
        float accuracy = 0.7f;        ///< Hit probability [0, 1]; affects aim spread.

        /**
         * @brief Validate that AI config parameters are within sane ranges.
         * @return true if all parameters are valid.
         */
        bool Validate() const
        {
            ASSERT_MSG(detectionRange > 0.0f, "AI detectionRange must be positive");
            ASSERT_MSG(attackRange > 0.0f, "AI attackRange must be positive");
            ASSERT_MSG(attackRange <= detectionRange, "AI attackRange should not exceed detectionRange");
            ASSERT_MSG(moveSpeed >= 0.0f, "AI moveSpeed must be non-negative");
            ASSERT_MSG(reactionTime >= 0.0f, "AI reactionTime must be non-negative");
            ASSERT_MSG(accuracy >= 0.0f && accuracy <= 1.0f, "AI accuracy must be in [0, 1]");
            return detectionRange > 0.0f && attackRange > 0.0f && moveSpeed >= 0.0f && reactionTime >= 0.0f &&
                   accuracy >= 0.0f && accuracy <= 1.0f;
        }
    } config;
};

// NetworkIdentity has been moved to NetworkComponents.h (R6.2)
