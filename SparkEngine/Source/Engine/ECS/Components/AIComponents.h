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

struct AIComponent
{
    enum class State
    {
        Idle,
        Patrolling,
        Alert,
        Combat,
        Fleeing,
        Dead
    };

    State state = State::Idle;
    std::string behaviorTreeName;
    Spark::BehaviorTreeHandle behaviorTreeHandle;
    Spark::NavQueryHandle navQueryHandle;

    // Perception
    EntityID targetEntity = entt::null;
    DirectX::XMFLOAT3 lastKnownTargetPos{0, 0, 0};
    float timeSinceLastSeen = 0.0f;
    float alertTimer = 0.0f;

    /// Cooldown between attack actions (driven by config.reactionTime)
    Spark::Cooldown attackCooldown{0.5f, true};

    /// Cooldown for perception updates (avoids expensive checks every frame)
    Spark::Cooldown perceptionCooldown{0.1f, true};

    // Pathfinding
    std::vector<DirectX::XMFLOAT3> currentPath;
    size_t currentPathIndex = 0;

    // Config
    struct Config
    {
        float detectionRange = 30.0f;
        float attackRange = 15.0f;
        float moveSpeed = 5.0f;
        float reactionTime = 0.5f;
        float accuracy = 0.7f;

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
