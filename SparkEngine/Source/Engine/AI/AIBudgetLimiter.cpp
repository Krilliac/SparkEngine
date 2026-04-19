/**
 * @file AIBudgetLimiter.cpp
 * @brief Implementation of the AI frame-budget limiter
 * @author Spark Engine Team
 * @date 2026
 */

#include "AIBudgetLimiter.h"
#include "AISystem.h"
#include "../ECS/Components.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>

namespace Spark::AI
{

    // =========================================================================
    // Frame Lifecycle
    // =========================================================================

    void AIBudgetLimiter::BeginFrame(const DirectX::XMFLOAT3& playerPosition, World& world)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        m_frameStartTime = std::chrono::high_resolution_clock::now();
        m_frameActive = true;
        m_nextAgentIndex = 0;

        // Reset current frame stats
        m_currentFrameStats = BudgetFrameStats{};
        m_currentFrameStats.budgetMs = m_budgetMs;

        // Gather all AI agents from the world and reconcile with persistent tracking data.
        // We need to preserve framesSinceLastUpdate across frames, so we merge rather
        // than rebuild from scratch.

        // Build a set of currently active agents from the ECS
        auto view = world.GetEntitiesWith<AIComponent, Transform>();
        std::vector<EntityID> activeAgents;
        for (auto entity : view)
        {
            const auto* ai = world.GetComponent<AIComponent>(entity);
            if (ai != nullptr && ai->state != AIComponent::State::Dead)
            {
                activeAgents.push_back(static_cast<EntityID>(entity));
            }
        }

        // Reconcile: keep existing entries (preserving stale counters), add new ones, remove dead.
        // Build a per-frame index of old entries to avoid O(n²) scans on large crowds.
        std::unordered_map<EntityID, size_t> previousIndex;
        previousIndex.reserve(m_agents.size());
        for (size_t i = 0; i < m_agents.size(); ++i)
        {
            previousIndex[m_agents[i].entityId] = i;
        }

        std::vector<AgentBudgetEntry> newAgents;
        newAgents.reserve(activeAgents.size());

        for (EntityID entityId : activeAgents)
        {
            // Reuse existing entry if present
            const auto oldIndex = previousIndex.find(entityId);
            if (oldIndex != previousIndex.end())
            {
                // Preserve stale counter, update position
                AgentBudgetEntry entry = m_agents[oldIndex->second];
                const auto* transform = world.GetComponent<Transform>(static_cast<entt::entity>(entityId));
                if (transform != nullptr)
                {
                    entry.position = transform->position;
                }
                entry.processedThisFrame = false;
                newAgents.push_back(entry);
            }
            else
            {
                // New agent
                AgentBudgetEntry entry;
                entry.entityId = entityId;
                entry.framesSinceLastUpdate = 0;
                entry.processedThisFrame = false;

                const auto* transform = world.GetComponent<Transform>(static_cast<entt::entity>(entityId));
                if (transform != nullptr)
                {
                    entry.position = transform->position;
                }
                newAgents.push_back(entry);
            }
        }

        m_agents = std::move(newAgents);

        // Compute priorities and sort
        ComputePriorities(playerPosition);
        SortByPriority();
        RebuildAgentIndex();
    }

    bool AIBudgetLimiter::HasBudgetRemaining() const
    {
        if (!m_frameActive)
        {
            return false;
        }
        return GetElapsedMs() < m_budgetMs;
    }

    std::optional<EntityID> AIBudgetLimiter::GetNextAgent()
    {
        if (!m_frameActive)
        {
            return std::nullopt;
        }

        const auto agentCount = static_cast<uint32_t>(m_agents.size());

        // First pass: return the next unprocessed agent if budget remains
        while (m_nextAgentIndex < agentCount)
        {
            auto& agent = m_agents[m_nextAgentIndex];
            ++m_nextAgentIndex;

            if (agent.processedThisFrame)
            {
                continue;
            }

            // Check if this agent is starved and needs a force-update
            const bool isStarved = agent.framesSinceLastUpdate >= m_maxStaleFrames;

            if (isStarved)
            {
                // Force-update regardless of budget
                ++m_currentFrameStats.agentsForceUpdated;
                SPARK_LOG_INFO(Spark::LogCategory::AI,
                               "AIBudgetLimiter: starvation promotion for entity %u (stale %u frames)",
                               static_cast<uint32_t>(agent.entityId), agent.framesSinceLastUpdate);
                return agent.entityId;
            }

            if (HasBudgetRemaining())
            {
                return agent.entityId;
            }

            // Budget exhausted and agent is not starved -- stop here.
            // Back up the index so EndFrame can count this as deferred.
            --m_nextAgentIndex;
            return std::nullopt;
        }

        return std::nullopt;
    }

    void AIBudgetLimiter::MarkAgentProcessed(EntityID entityId)
    {
        const auto it = m_agentIndex.find(entityId);
        if (it == m_agentIndex.end())
        {
            return;
        }

        auto& agent = m_agents[it->second];
        agent.processedThisFrame = true;
        agent.framesSinceLastUpdate = 0;
        ++m_currentFrameStats.agentsProcessed;
    }

    void AIBudgetLimiter::EndFrame()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        if (!m_frameActive)
        {
            return;
        }

        // Update stale counters and count deferred agents
        for (auto& agent : m_agents)
        {
            if (!agent.processedThisFrame)
            {
                ++agent.framesSinceLastUpdate;
                ++m_currentFrameStats.agentsDeferred;
            }
        }

        m_currentFrameStats.timeConsumedMs = GetElapsedMs();

        // Log when agents were deferred due to budget exhaustion
        if (m_currentFrameStats.agentsDeferred > 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::AI,
                           "AIBudgetLimiter: %u agents deferred (budget %.2fms, consumed %.2fms)",
                           m_currentFrameStats.agentsDeferred, m_budgetMs, m_currentFrameStats.timeConsumedMs);
        }

        // Commit stats
        m_lastFrameStats = m_currentFrameStats;
        m_frameActive = false;
    }

    bool AIBudgetLimiter::ShouldUpdateAgent(EntityID entityId) const
    {
        const auto it = m_agentIndex.find(entityId);
        if (it != m_agentIndex.end())
        {
            const auto& agent = m_agents[it->second];
            // Force-update if starved
            if (agent.framesSinceLastUpdate >= m_maxStaleFrames)
            {
                return true;
            }
            // Otherwise only if budget remains
            return HasBudgetRemaining();
        }
        // Unknown agent -- default to updating if budget allows
        return HasBudgetRemaining();
    }

    // =========================================================================
    // Priority Computation
    // =========================================================================

    void AIBudgetLimiter::ComputePriorities(const DirectX::XMFLOAT3& playerPosition)
    {
        const float farThresholdSq = m_farDistanceThreshold * m_farDistanceThreshold;

        for (auto& agent : m_agents)
        {
            // Compute squared distance to player
            const float dx = agent.position.x - playerPosition.x;
            const float dy = agent.position.y - playerPosition.y;
            const float dz = agent.position.z - playerPosition.z;
            agent.distanceToPlayerSq = dx * dx + dy * dy + dz * dz;

            // Distance-based priority: inverse distance weighting
            // Agents at the player's position get priority ~1.0, agents at
            // farDistanceThreshold get priority ~0.5, farther agents taper off
            const float distancePriority = 1.0f / (1.0f + agent.distanceToPlayerSq / farThresholdSq);

            // Starvation bonus: linearly increases with frames since last update
            const float starvationPriority = m_starvationBonus * static_cast<float>(agent.framesSinceLastUpdate);

            agent.priority = distancePriority + starvationPriority;
        }
    }

    void AIBudgetLimiter::SortByPriority()
    {
        std::sort(m_agents.begin(), m_agents.end(),
                  [](const AgentBudgetEntry& a, const AgentBudgetEntry& b) { return a.priority > b.priority; });
    }

    void AIBudgetLimiter::RebuildAgentIndex()
    {
        m_agentIndex.clear();
        m_agentIndex.reserve(m_agents.size());
        for (size_t i = 0; i < m_agents.size(); ++i)
        {
            m_agentIndex[m_agents[i].entityId] = i;
        }
    }

    // =========================================================================
    // Timer
    // =========================================================================

    float AIBudgetLimiter::GetElapsedMs() const
    {
        const auto now = std::chrono::high_resolution_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_frameStartTime);
        return static_cast<float>(elapsed.count()) / 1000.0f;
    }

} // namespace Spark::AI
