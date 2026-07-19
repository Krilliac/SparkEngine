/**
 * @file AIBudgetTypes.h
 * @brief Shared data types for the AI budget limiter (R4.3)
 * @author Spark Engine Team
 * @date 2026
 *
 * Part of the AIBudgetLimiter.h umbrella header. Contains the AI-local
 * EntityID alias, the per-agent tracking entry, and the per-frame budget
 * statistics used by AIBudgetLimiter.
 *
 * @see AIBudgetLimiter.h, AIBudgetLimiterCore.h
 */

#pragma once

#include "../../Core/Platform.h"
#include "../ECS/Components/CoreComponents.h"

#include <cstdint>

// Forward declaration — World is defined in ECS/Components.h at global scope
class World;

namespace Spark::AI
{

    // AI uses a uint32_t EntityID alias (matches MovementSystem.h). Redeclared
    // locally so that unqualified `EntityID` resolves deterministically inside
    // this namespace regardless of whether ::EntityID (entt::entity, from
    // CoreComponents.h) has also been pulled into the TU. Without this, the
    // field type below silently differs between TUs → ODR violation under LTO.
    using EntityID = uint32_t;

    /**
     * @brief Tracking data for a single AI agent within the budget system.
     */
    struct AgentBudgetEntry
    {
        // entt::null is an entt::entity, so use the unsigned-int sentinel.
        EntityID entityId = static_cast<EntityID>(-1);

        /// World-space position of the agent (cached for distance computation)
        DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};

        /// Squared distance to the player (lower = higher priority)
        float distanceToPlayerSq = 0.0f;

        /// Computed priority score (higher = more important, updated each frame)
        float priority = 0.0f;

        /// Number of consecutive frames this agent has been skipped
        uint32_t framesSinceLastUpdate = 0;

        /// Whether this agent was processed in the current frame
        bool processedThisFrame = false;
    };

    /**
     * @brief Statistics from the most recent frame's budget allocation.
     */
    struct BudgetFrameStats
    {
        /// Number of agents that were fully processed
        uint32_t agentsProcessed = 0;

        /// Number of agents that were deferred (budget exhausted)
        uint32_t agentsDeferred = 0;

        /// Number of agents force-updated due to starvation
        uint32_t agentsForceUpdated = 0;

        /// Actual time consumed in milliseconds
        float timeConsumedMs = 0.0f;

        /// Configured budget in milliseconds
        float budgetMs = 0.0f;

        /// Percentage of budget consumed (0..100+)
        float BudgetUtilization() const { return (budgetMs > 0.0f) ? (timeConsumedMs / budgetMs) * 100.0f : 0.0f; }
    };

} // namespace Spark::AI
