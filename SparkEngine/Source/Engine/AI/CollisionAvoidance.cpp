/**
 * @file CollisionAvoidance.cpp
 * @brief Implementation of the simplified ORCA collision avoidance system
 */

#include "CollisionAvoidance.h"

#include <algorithm>
#include <cmath>

namespace Spark::AI
{

    // =========================================================================
    // Helpers
    // =========================================================================

    static float Dot2D(float ax, float az, float bx, float bz)
    {
        return ax * bx + az * bz;
    }

    static float Length2D(float x, float z)
    {
        return std::sqrt(x * x + z * z);
    }

    /// Clamp a 2D velocity vector to a maximum magnitude
    static void ClampSpeed(float& vx, float& vz, float maxSpeed)
    {
        float speed = Length2D(vx, vz);
        if (speed > maxSpeed && speed > 0.0001f)
        {
            float scale = maxSpeed / speed;
            vx *= scale;
            vz *= scale;
        }
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void CollisionAvoidanceSystem::Initialize()
    {
        m_agents.clear();
        m_initialized = true;
    }

    void CollisionAvoidanceSystem::Shutdown()
    {
        m_agents.clear();
        m_initialized = false;
    }

    // =========================================================================
    // Agent Registration
    // =========================================================================

    void CollisionAvoidanceSystem::RegisterAgent(uint32_t entityID, float radius, float maxSpeed)
    {
        if (!m_initialized)
        {
            return;
        }

        AvoidanceAgent agent;
        agent.entityID = entityID;
        agent.radius = radius;
        agent.maxSpeed = maxSpeed;
        m_agents[entityID] = agent;
    }

    void CollisionAvoidanceSystem::UnregisterAgent(uint32_t entityID)
    {
        m_agents.erase(entityID);
    }

    void CollisionAvoidanceSystem::SetPreferredVelocity(uint32_t entityID, const XMFLOAT3& velocity)
    {
        auto it = m_agents.find(entityID);
        if (it != m_agents.end())
        {
            it->second.preferredVelocity = velocity;
        }
    }

    void CollisionAvoidanceSystem::UpdateAgentState(uint32_t entityID, const XMFLOAT3& position,
                                                    const XMFLOAT3& currentVelocity)
    {
        auto it = m_agents.find(entityID);
        if (it != m_agents.end())
        {
            it->second.position = position;
            it->second.velocity = currentVelocity;
        }
    }

    bool CollisionAvoidanceSystem::HasAgent(uint32_t entityID) const
    {
        return m_agents.contains(entityID);
    }

    // =========================================================================
    // ORCA Solver
    // =========================================================================

    std::vector<AvoidanceResult> CollisionAvoidanceSystem::Solve([[maybe_unused]] float deltaTime) const
    {
        std::vector<AvoidanceResult> results;
        results.reserve(m_agents.size());

        if (!m_initialized || m_agents.empty())
        {
            return results;
        }

        // Collect agents into a flat list for pair iteration
        std::vector<const AvoidanceAgent*> agentList;
        agentList.reserve(m_agents.size());
        for (const auto& [id, agent] : m_agents)
        {
            agentList.push_back(&agent);
        }

        // For each agent, accumulate avoidance adjustments from nearby agents
        for (size_t i = 0; i < agentList.size(); ++i)
        {
            const auto& self = *agentList[i];

            // Start with the preferred velocity
            float safeVx = self.preferredVelocity.x;
            float safeVz = self.preferredVelocity.z;

            for (size_t j = 0; j < agentList.size(); ++j)
            {
                if (i == j)
                {
                    continue;
                }

                const auto& other = *agentList[j];

                // Relative position (other relative to self)
                float relPosX = other.position.x - self.position.x;
                float relPosZ = other.position.z - self.position.z;
                float dist = Length2D(relPosX, relPosZ);

                // Skip agents outside interaction range
                if (dist > kInteractionRange)
                {
                    continue;
                }

                float combinedRadius = self.radius + other.radius;

                // If agents are overlapping, push directly apart
                if (dist < combinedRadius && dist > 0.0001f)
                {
                    float pushStrength = (combinedRadius - dist) * 2.0f;
                    float invDist = 1.0f / dist;
                    safeVx -= relPosX * invDist * pushStrength;
                    safeVz -= relPosZ * invDist * pushStrength;
                    continue;
                }

                // Simplified ORCA: compute the velocity obstacle from other agent.
                // The VO is a cone in velocity space centered on the relative velocity
                // toward the other agent. If our preferred velocity falls inside the
                // cone, we adjust by the minimum displacement to escape the cone.

                // Time horizon — how far ahead to consider collisions
                float timeHorizon = 2.0f;

                // Relative velocity (what self is doing relative to other)
                float relVelX = self.preferredVelocity.x - other.velocity.x;
                float relVelZ = self.preferredVelocity.z - other.velocity.z;

                // Vector from self to other, scaled by time horizon
                float scaledRelPosX = relPosX / timeHorizon;
                float scaledRelPosZ = relPosZ / timeHorizon;
                float scaledRadius = combinedRadius / timeHorizon;

                // Check if relative velocity is inside the velocity obstacle circle
                float toVoX = relVelX - scaledRelPosX;
                float toVoZ = relVelZ - scaledRelPosZ;
                float distToVo = Length2D(toVoX, toVoZ);

                if (distToVo < scaledRadius && distToVo > 0.0001f)
                {
                    // Inside the VO — compute the minimum adjustment to escape
                    float penetration = scaledRadius - distToVo;
                    float invDistVo = 1.0f / distToVo;

                    // Push velocity outward from VO center (half for reciprocal sharing)
                    float adjustX = toVoX * invDistVo * penetration * 0.5f;
                    float adjustZ = toVoZ * invDistVo * penetration * 0.5f;

                    safeVx += adjustX;
                    safeVz += adjustZ;
                }
            }

            // Clamp to max speed
            ClampSpeed(safeVx, safeVz, self.maxSpeed);

            AvoidanceResult result;
            result.entityID = self.entityID;
            result.safeVelocity = {safeVx, self.preferredVelocity.y, safeVz};
            results.push_back(result);
        }

        return results;
    }

} // namespace Spark::AI
