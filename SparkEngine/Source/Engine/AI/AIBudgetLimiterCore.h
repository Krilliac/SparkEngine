/**
 * @file AIBudgetLimiterCore.h
 * @brief AIBudgetLimiter class — per-frame AI time-budget enforcement (R4.3)
 * @author Spark Engine Team
 * @date 2026
 *
 * Part of the AIBudgetLimiter.h umbrella header. Contains the AIBudgetLimiter
 * class itself; the supporting data types live in AIBudgetTypes.h.
 *
 * @see AIBudgetLimiter.h, AIBudgetTypes.h
 */

#pragma once

#include "AIBudgetTypes.h"

#include <vector>
#include <unordered_map>
#include <chrono>
#include <optional>

namespace Spark::AI
{

    /**
     * @brief Limits AI processing time per frame using a time budget.
     *
     * Prioritizes agents by distance to the player and spreads expensive
     * operations across frames. Ensures no agent is starved by enforcing
     * a maximum number of stale frames.
     */
    class AIBudgetLimiter
    {
      public:
        AIBudgetLimiter() = default;
        ~AIBudgetLimiter() = default;

        // Non-copyable
        AIBudgetLimiter(const AIBudgetLimiter&) = delete;
        AIBudgetLimiter& operator=(const AIBudgetLimiter&) = delete;

        // =====================================================================
        // Configuration
        // =====================================================================

        /**
         * @brief Set the per-frame AI time budget in milliseconds.
         * @param budgetMs  Maximum milliseconds to spend on AI per frame (default 4.0).
         */
        void SetBudgetMs(float budgetMs) { m_budgetMs = budgetMs; }

        /** @brief Get the current budget in milliseconds. */
        float GetBudgetMs() const { return m_budgetMs; }

        /**
         * @brief Set the maximum frames an agent can go without being updated.
         *
         * Agents exceeding this threshold are force-updated even if the budget
         * is exhausted, preventing indefinite starvation.
         *
         * @param maxFrames  Maximum stale frames (default 5).
         */
        void SetMaxStaleFrames(uint32_t maxFrames) { m_maxStaleFrames = maxFrames; }

        /** @brief Get the stale-frame threshold. */
        uint32_t GetMaxStaleFrames() const { return m_maxStaleFrames; }

        /**
         * @brief Set the distance beyond which agents receive reduced priority.
         *
         * Agents farther than this distance are updated less frequently.
         *
         * @param distance  Distance threshold in world units (default 100.0).
         */
        void SetFarDistanceThreshold(float distance) { m_farDistanceThreshold = distance; }

        /**
         * @brief Set a priority bonus multiplied by frames-since-last-update.
         *
         * Higher values make starvation recovery more aggressive. A value of
         * 0.0 disables starvation-aware prioritization (distance-only).
         *
         * @param bonus  Priority bonus per stale frame (default 10.0).
         */
        void SetStarvationBonus(float bonus) { m_starvationBonus = bonus; }

        // =====================================================================
        // Frame Lifecycle
        // =====================================================================

        /**
         * @brief Begin a new frame. Gathers agents, computes priorities, and starts the timer.
         *
         * Must be called once at the start of the AI update phase. Populates the
         * internal priority queue and resets per-frame state.
         *
         * @param playerPosition  Current world-space position of the player camera.
         * @param world           The ECS World to scan for AI agents.
         */
        void BeginFrame(const DirectX::XMFLOAT3& playerPosition, World& world);

        /**
         * @brief Check whether the frame budget has not yet been exhausted.
         * @return True if there is remaining time in the budget.
         */
        bool HasBudgetRemaining() const;

        /**
         * @brief Retrieve the next highest-priority agent to process.
         *
         * Returns the EntityID of the next agent, or std::nullopt if all agents
         * have been processed or the budget is exhausted (except for starved agents).
         *
         * @return EntityID of the next agent, or std::nullopt.
         */
        std::optional<EntityID> GetNextAgent();

        /**
         * @brief Mark an agent as having been processed this frame.
         *
         * Resets the agent's stale-frame counter. Must be called after the
         * caller finishes processing the agent returned by GetNextAgent().
         *
         * @param entityId  The agent that was processed.
         */
        void MarkAgentProcessed(EntityID entityId);

        /**
         * @brief Finalize the frame. Updates stale counters for unprocessed agents.
         *
         * Must be called once at the end of the AI update phase. Increments
         * `framesSinceLastUpdate` for any agent not processed this frame.
         */
        void EndFrame();

        /**
         * @brief Check if a specific agent should be updated this frame.
         *
         * Convenience method that checks both budget availability and
         * starvation thresholds. Useful for integrating with existing
         * per-agent update loops.
         *
         * @param entityId  The agent to check.
         * @return True if the agent should be updated.
         */
        bool ShouldUpdateAgent(EntityID entityId) const;

        // =====================================================================
        // Statistics
        // =====================================================================

        /** @brief Get statistics from the most recently completed frame. */
        const BudgetFrameStats& GetLastFrameStats() const { return m_lastFrameStats; }

        /** @brief Get the total number of tracked agents. */
        uint32_t GetTrackedAgentCount() const { return static_cast<uint32_t>(m_agents.size()); }

      private:
        /**
         * @brief Compute priority score for an agent.
         *
         * Priority = (1.0 / (1.0 + distanceSq / farThresholdSq)) + starvationBonus * staleFrames
         *
         * Agents close to the player and/or starved for updates get higher priority.
         */
        void ComputePriorities(const DirectX::XMFLOAT3& playerPosition);

        /**
         * @brief Sort agents by priority (descending) so highest-priority agents are first.
         */
        void SortByPriority();

        /**
         * @brief Rebuild the entity-to-index lookup table for O(1) agent lookups.
         */
        void RebuildAgentIndex();

        /**
         * @brief Get elapsed time since BeginFrame() in milliseconds.
         */
        float GetElapsedMs() const;

        /// Per-frame time budget in milliseconds
        float m_budgetMs = 4.0f;

        /// Maximum consecutive frames an agent can be skipped before force-update
        uint32_t m_maxStaleFrames = 5;

        /// Distance threshold for reduced update frequency
        float m_farDistanceThreshold = 100.0f;

        /// Priority bonus per stale frame (starvation prevention)
        float m_starvationBonus = 10.0f;

        /// All tracked AI agents (persists across frames for stale tracking)
        std::vector<AgentBudgetEntry> m_agents;

        /// Fast lookup from entity id -> index in m_agents
        std::unordered_map<EntityID, size_t> m_agentIndex;

        /// Index into m_agents for the next agent to process this frame
        uint32_t m_nextAgentIndex = 0;

        /// High-resolution timer start point for the current frame
        std::chrono::high_resolution_clock::time_point m_frameStartTime;

        /// Whether we are currently inside a BeginFrame/EndFrame pair
        bool m_frameActive = false;

        /// Statistics from the last completed frame
        BudgetFrameStats m_lastFrameStats;

        /// Current frame's running statistics
        BudgetFrameStats m_currentFrameStats;
    };

} // namespace Spark::AI
