/**
 * @file CollisionAvoidance.h
 * @brief ORCA collision avoidance for AI crowds
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Implements a simplified ORCA (Optimal Reciprocal Collision Avoidance)
 * algorithm for steering AI agents through crowds without overlapping.
 * Each registered agent has a preferred velocity (from pathfinding/steering)
 * and the solver computes a collision-free velocity that stays as close
 * to the preferred velocity as possible.
 *
 * ## Architecture
 * ```
 * CollisionAvoidanceSystem (singleton)
 *   ├── m_agents           (registered agents, keyed by entity ID)
 *   └── Solve(dt)
 *         ├── For each agent pair within range:
 *         │     └── Compute velocity obstacle (VO)
 *         └── For each agent:
 *               └── Find nearest velocity outside all VOs
 * ```
 *
 * ## Usage
 * @code
 *   auto& cas = Spark::AI::CollisionAvoidanceSystem::GetInstance();
 *   cas.Initialize();
 *
 *   cas.RegisterAgent(entityA, 0.5f, 5.0f);
 *   cas.RegisterAgent(entityB, 0.5f, 5.0f);
 *
 *   // Each frame:
 *   cas.UpdateAgentState(entityA, posA, velA);
 *   cas.SetPreferredVelocity(entityA, desiredVelA);
 *   auto results = cas.Solve(deltaTime);
 *   for (const auto& r : results)
 *   {
 *       // Apply r.safeVelocity to entity r.entityID
 *   }
 * @endcode
 *
 * @see MovementSystem.h (applies avoidance velocities to entity transforms)
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Spark::AI
{

    // =========================================================================
    // Avoidance Data Structures
    // =========================================================================

    /**
     * @brief State of a single collision avoidance agent.
     *
     * Tracks position, velocity, and preferred velocity for the ORCA solver.
     * Updated externally each frame before calling Solve().
     */
    struct AvoidanceAgent
    {
        uint32_t entityID = 0;
        XMFLOAT3 position{0.0f, 0.0f, 0.0f};
        XMFLOAT3 velocity{0.0f, 0.0f, 0.0f};
        XMFLOAT3 preferredVelocity{0.0f, 0.0f, 0.0f};
        float radius = 0.5f;   ///< Agent collision radius
        float maxSpeed = 5.0f; ///< Maximum allowed speed
    };

    /**
     * @brief Result of the avoidance solver for a single agent.
     *
     * Contains the collision-free velocity that the agent should use
     * instead of its preferred velocity.
     */
    struct AvoidanceResult
    {
        uint32_t entityID = 0;
        XMFLOAT3 safeVelocity{0.0f, 0.0f, 0.0f};
    };

    // =========================================================================
    // CollisionAvoidanceSystem
    // =========================================================================

    /**
     * @class CollisionAvoidanceSystem
     * @brief Singleton implementing simplified ORCA collision avoidance.
     *
     * For each pair of agents within interaction range, computes a velocity
     * obstacle and adjusts each agent's velocity to avoid collision while
     * remaining as close to the preferred velocity as possible.
     */
    class CollisionAvoidanceSystem
    {
      public:
        /** @brief Get the singleton instance. */
        [[deprecated("Use EngineContext::Get()->GetSystem<CollisionAvoidanceSystem>() instead")]]
        static CollisionAvoidanceSystem& GetInstance()
        {
            static CollisionAvoidanceSystem s;
            return s;
        }

        /** @brief Initialize the system. */
        void Initialize();

        /** @brief Shut down and unregister all agents. */
        void Shutdown();

        /**
         * @brief Register an agent for collision avoidance.
         * @param entityID Entity to register.
         * @param radius   Collision radius.
         * @param maxSpeed Maximum movement speed.
         */
        void RegisterAgent(uint32_t entityID, float radius = 0.5f, float maxSpeed = 5.0f);

        /**
         * @brief Unregister an agent.
         * @param entityID Entity to remove.
         */
        void UnregisterAgent(uint32_t entityID);

        /**
         * @brief Set the desired velocity for an agent (from pathfinding).
         * @param entityID Entity to update.
         * @param velocity Desired movement velocity.
         */
        void SetPreferredVelocity(uint32_t entityID, const XMFLOAT3& velocity);

        /**
         * @brief Update an agent's current position and velocity.
         * @param entityID        Entity to update.
         * @param position        Current world-space position.
         * @param currentVelocity Current movement velocity.
         */
        void UpdateAgentState(uint32_t entityID, const XMFLOAT3& position, const XMFLOAT3& currentVelocity);

        /**
         * @brief Run the ORCA solver and compute safe velocities for all agents.
         * @param deltaTime Frame time in seconds.
         * @return Vector of results with collision-free velocities.
         */
        std::vector<AvoidanceResult> Solve(float deltaTime) const;

        /** @brief Get the number of registered agents. */
        size_t GetAgentCount() const { return m_agents.size(); }

        /**
         * @brief Check if an entity is registered for avoidance.
         * @param entityID Entity to check.
         * @return true if registered.
         */
        bool HasAgent(uint32_t entityID) const;

      private:
        CollisionAvoidanceSystem() = default;

        /** @brief Interaction range beyond which agent pairs are ignored. */
        static constexpr float kInteractionRange = 15.0f;

        std::unordered_map<uint32_t, AvoidanceAgent> m_agents;
        bool m_initialized = false;
    };

} // namespace Spark::AI
