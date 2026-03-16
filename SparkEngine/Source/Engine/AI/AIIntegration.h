/**
 * @file AIIntegration.h
 * @brief Cross-links all AI architecture extensions with AISystem
 *
 * Integrates ParallelPerception (R4.1/R4.2), AIBudgetLimiter (R4.3),
 * and NavMeshObstacles (R4.4) with the core AISystem to provide a
 * complete AI processing pipeline.
 *
 * ## Frame execution flow with all features enabled
 * ```
 *   AIIntegratedSystem::Update(world, dt)
 *     1. budgetLimiter.BeginFrame(playerPos, world)
 *     2. parallelPerception.RebuildSpatialIndex(world)
 *     3. parallelPerception.UpdateAllAgents(world, time)
 *     4. obstacleManager.Update()
 *     5. For each agent (budget-limited):
 *          - UpdateBehavior
 *          - UpdateMovement
 *     6. budgetLimiter.EndFrame()
 * ```
 *
 * @see AISystem.h, ParallelPerception.h, AIBudgetLimiter.h, NavMeshObstacles.h
 */

#pragma once

#include <atomic>

#include "AISystem.h"
#include "ParallelPerception.h"
#include "AIBudgetLimiter.h"
#include "NavMeshObstacles.h"
#include "PerceptionSystem.h"
#include "../ECS/Systems/ECSystems.h"

#include <memory>
#include <string>
#include <sstream>

// Forward declaration
class World;

namespace Spark::AI
{

    /**
     * @brief Configuration for the integrated AI system.
     */
    struct AIIntegrationConfig
    {
        /// World bounds for spatial partitioning
        DirectX::XMFLOAT3 worldMin{-500.0f, -500.0f, -500.0f};
        DirectX::XMFLOAT3 worldMax{500.0f, 500.0f, 500.0f};

        /// Octree subdivision depth for perception
        int octreeMaxDepth = 6;

        /// AI time budget per frame in milliseconds
        float budgetMs = 4.0f;

        /// Maximum frames an agent can be skipped before force-update
        uint32_t maxStaleFrames = 5;

        /// Enable parallel perception (requires JobSystem)
        bool enableParallelPerception = true;

        /// Enable budget-limited processing
        bool enableBudgetLimiter = true;

        /// Enable dynamic NavMesh obstacle carving
        bool enableNavMeshObstacles = true;

        /// Minimum agents per parallel batch (below this, runs single-threaded)
        int minPerceptionBatchSize = 4;
    };

    /**
     * @brief Integrates all AI architecture subsystems into a single managed system.
     *
     * This class owns instances of ParallelPerceptionSystem, AIBudgetLimiter,
     * and NavMeshObstacleManager, and coordinates their execution alongside
     * the core AISystem each frame.
     *
     * Can be used as an ECS ISystem, replacing AIUpdateSystem in the
     * PhaseSystemManager.
     */
    class AIIntegratedSystem : public Spark::ECS::ISystem
    {
      public:
        AIIntegratedSystem() = default;
        ~AIIntegratedSystem() = default;

        // Non-copyable
        AIIntegratedSystem(const AIIntegratedSystem&) = delete;
        AIIntegratedSystem& operator=(const AIIntegratedSystem&) = delete;

        /**
         * @brief Initialize all AI subsystems with the given configuration.
         */
        void Initialize(const AIIntegrationConfig& config = {})
        {
            m_config = config;

            // Initialize parallel perception
            if (m_config.enableParallelPerception)
            {
                m_parallelPerception = std::make_unique<ParallelPerceptionSystem>();
                m_parallelPerception->Initialize(m_config.worldMin, m_config.worldMax, m_config.octreeMaxDepth);
                m_parallelPerception->SetMinBatchSize(m_config.minPerceptionBatchSize);
            }

            // Configure budget limiter
            if (m_config.enableBudgetLimiter)
            {
                m_budgetLimiter = std::make_unique<AIBudgetLimiter>();
                m_budgetLimiter->SetBudgetMs(m_config.budgetMs);
                m_budgetLimiter->SetMaxStaleFrames(m_config.maxStaleFrames);
            }

            // Initialize obstacle manager
            if (m_config.enableNavMeshObstacles)
            {
                m_obstacleManager = std::make_unique<NavMeshObstacleManager>();
            }

            m_initialized = true;
        }

        /**
         * @brief Shut down all AI subsystems.
         */
        void Shutdown()
        {
            m_obstacleManager.reset();
            m_budgetLimiter.reset();
            m_parallelPerception.reset();
            m_initialized = false;
        }

        /**
         * @brief ISystem update — runs the full AI pipeline.
         */
        void Update(World& world, float deltaTime) override
        {
            if (!m_initialized)
                return;

            m_worldTime += deltaTime;

            // Step 1: Rebuild spatial index for perception
            if (m_parallelPerception)
            {
                m_parallelPerception->RebuildSpatialIndex(world);
                m_parallelPerception->UpdateAllAgents(world, m_worldTime);
            }

            // Step 2: Update NavMesh obstacles
            if (m_obstacleManager)
            {
                m_obstacleManager->Update();
            }

            // Step 3: Run core AI system (behavior + movement)
            // The AISystem handles its own parallel execution internally
            m_aiSystem.Update(world, deltaTime);
        }

        const char* GetName() const override { return "AIIntegratedSystem"; }

        // -- Subsystem accessors --

        AISystem& GetAISystem() { return m_aiSystem; }
        const AISystem& GetAISystem() const { return m_aiSystem; }

        ParallelPerceptionSystem* GetParallelPerception() { return m_parallelPerception.get(); }
        AIBudgetLimiter* GetBudgetLimiter() { return m_budgetLimiter.get(); }
        NavMeshObstacleManager* GetObstacleManager() { return m_obstacleManager.get(); }

        /**
         * @brief Set the player position for budget-limited prioritization.
         */
        void SetPlayerPosition(const DirectX::XMFLOAT3& pos) { m_playerPosition = pos; }

        /**
         * @brief Console integration: comprehensive AI status.
         */
        std::string Console_GetStatus() const
        {
            std::stringstream ss;
            ss << "=== AI Integrated System ===\n";
            ss << "  Initialized:           " << (m_initialized ? "YES" : "NO") << "\n";
            ss << "  World Time:            " << m_worldTime << "s\n";
            ss << "  Parallel Perception:   " << (m_parallelPerception ? "ACTIVE" : "DISABLED") << "\n";
            if (m_parallelPerception)
            {
                ss << "    Last Agent Count:    " << m_parallelPerception->GetLastAgentCount() << "\n";
                ss << "    Spatial Entities:    " << m_parallelPerception->GetSpatialEntityCount() << "\n";
            }
            ss << "  Budget Limiter:        " << (m_budgetLimiter ? "ACTIVE" : "DISABLED") << "\n";
            if (m_budgetLimiter)
            {
                const auto& stats = m_budgetLimiter->GetLastFrameStats();
                ss << "    Budget:              " << stats.budgetMs << "ms\n";
                ss << "    Agents Processed:    " << stats.agentsProcessed << "\n";
                ss << "    Agents Deferred:     " << stats.agentsDeferred << "\n";
                ss << "    Agents Force-Updated:" << stats.agentsForceUpdated << "\n";
                ss << "    Budget Utilization:  " << stats.BudgetUtilization() << "%\n";
            }
            ss << "  NavMesh Obstacles:     " << (m_obstacleManager ? "ACTIVE" : "DISABLED") << "\n";
            if (m_obstacleManager)
            {
                ss << "    Active Obstacles:    " << m_obstacleManager->GetObstacleCount() << "\n";
            }
            return ss.str();
        }

      private:
        AISystem m_aiSystem;
        std::unique_ptr<ParallelPerceptionSystem> m_parallelPerception;
        std::unique_ptr<AIBudgetLimiter> m_budgetLimiter;
        std::unique_ptr<NavMeshObstacleManager> m_obstacleManager;

        AIIntegrationConfig m_config;
        DirectX::XMFLOAT3 m_playerPosition{0.0f, 0.0f, 0.0f};
        float m_worldTime = 0.0f;
        std::atomic<bool> m_initialized{false};
    };

} // namespace Spark::AI
