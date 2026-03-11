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
    } config;
};

// NetworkIdentity has been moved to NetworkComponents.h (R6.2)
