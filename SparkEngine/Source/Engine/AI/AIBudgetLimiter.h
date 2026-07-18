/**
 * @file AIBudgetLimiter.h
 * @brief Frame-budget limiter for AI processing (R4.3)
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements architecture recommendation R4.3: AI Budget Limiter.
 *
 * Limits the total AI computation time per frame to a configurable budget
 * (default 4ms). When the budget is exhausted mid-frame, remaining agents
 * are deferred to subsequent frames. Agents are prioritized by distance
 * to the player camera so that nearby (visually important) agents always
 * get updated first.
 *
 * ## Design
 *
 * The limiter maintains a priority queue of AI agents sorted by distance to
 * the player. Each frame:
 * 1. All agents are scored and sorted by priority.
 * 2. The highest-priority agents are processed until the time budget expires.
 * 3. Remaining agents receive a "stale" tick (simplified update) or are
 *    skipped entirely and deferred to the next frame.
 *
 * Agents that have not been updated for too long (exceeding `m_maxStaleFrames`)
 * are force-updated regardless of budget, ensuring no agent is starved
 * indefinitely.
 *
 * ## Integration
 * @code
 *   AIBudgetLimiter limiter;
 *   limiter.SetBudgetMs(4.0f);
 *
 *   // Each frame:
 *   limiter.BeginFrame(playerPosition);
 *   while (limiter.HasBudgetRemaining())
 *   {
 *       auto agent = limiter.GetNextAgent();
 *       if (!agent.has_value()) break;
 *       ProcessAgent(*agent);
 *       limiter.MarkAgentProcessed(*agent);
 *   }
 *   limiter.EndFrame();
 * @endcode
 *
 * @see AISystem.h, ParallelPerception.h
 */

#pragma once

// Umbrella header — the implementation is split across sibling headers so each
// stays under the size threshold. Include this file to get everything, as before.
#include "AIBudgetTypes.h"       // EntityID alias, AgentBudgetEntry, BudgetFrameStats
#include "AIBudgetLimiterCore.h" // AIBudgetLimiter class
