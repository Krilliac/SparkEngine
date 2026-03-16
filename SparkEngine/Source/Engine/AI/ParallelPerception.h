/**
 * @file ParallelPerception.h
 * @brief Parallelized perception system using JobSystem and Octree spatial partitioning
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements architecture recommendations R4.1 (Parallelize perception queries)
 * and R4.2 (Spatial partitioning for perception).
 *
 * Instead of checking every AI agent against every potential target (O(N*M)),
 * this system uses an Octree to spatially partition perceivable entities and
 * then dispatches per-agent perception queries in parallel via JobSystem::ParallelFor.
 *
 * ## Integration
 * @code
 *   // At initialization:
 *   ParallelPerceptionSystem perceptionSys;
 *   perceptionSys.Initialize({-500,-500,-500}, {500,500,500});
 *
 *   // Each frame:
 *   perceptionSys.RebuildSpatialIndex(world);
 *   perceptionSys.UpdateAllAgents(world, currentTime);
 * @endcode
 *
 * @see JobSystem.h, Octree.h, PerceptionSystem.h, AISystem.h
 */

#pragma once

#include "../../Core/Platform.h"
#include "../../Utils/JobSystem.h"

#include <atomic>
#include "PerceptionSystem.h"
#include "../ECS/Components/CoreComponents.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "../../Utils/Octree.h"
#endif

#include <vector>
#include <mutex>
#include <cstdint>

// Forward declaration — World is defined in ECS/Components.h at global scope
class World;

namespace Spark::AI
{

    /**
     * @brief Describes a perceivable entity in the world for spatial indexing.
     *
     * Stored alongside the Octree so that perception queries can retrieve
     * position and identity without re-querying the ECS registry.
     */
    struct PerceivableEntity
    {
        EntityID entityId = entt::null;
        DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};
    };

    /**
     * @brief Describes an active sound event for hearing checks.
     */
    struct ActiveSound
    {
        PerceptionEntityID sourceId = 0;
        DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
    };

    /**
     * @brief Per-agent data snapshot used during parallel perception updates.
     *
     * Collected on the main thread before dispatching parallel work, so that
     * worker threads never touch the ECS registry directly.
     */
    struct AgentPerceptionJob
    {
        EntityID entityId = entt::null;
        DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 forward{0.0f, 0.0f, 1.0f};
        PerceptionComponent perception;
    };

    /**
     * @brief Parallelized perception system with Octree-accelerated spatial queries.
     *
     * Workflow each frame:
     * 1. **RebuildSpatialIndex** -- main thread inserts all perceivable entities
     *    into the Octree and caches active sounds.
     * 2. **GatherAgentData** -- main thread snapshots per-agent state into
     *    `AgentPerceptionJob` structs (avoids ECS access from worker threads).
     * 3. **UpdateAllAgents** -- dispatches `JobSystem::ParallelFor` over the
     *    agent array. Each worker:
     *      a. Queries the Octree (sphere query) for nearby candidates.
     *      b. Runs sight/hearing checks from PerceptionSystem.h.
     *      c. Writes results into the agent's local `PerceptionComponent` copy.
     * 4. **WriteBackResults** -- main thread copies updated perception data
     *    back into the ECS components.
     */
    class ParallelPerceptionSystem
    {
      public:
        /**
         * @brief Construct the system. Call Initialize() before first use.
         */
        ParallelPerceptionSystem() = default;

        ~ParallelPerceptionSystem() = default;

        // Non-copyable
        ParallelPerceptionSystem(const ParallelPerceptionSystem&) = delete;
        ParallelPerceptionSystem& operator=(const ParallelPerceptionSystem&) = delete;

        /**
         * @brief Initialize the spatial index with world bounds.
         *
         * @param worldMin  Minimum corner of the playable world volume.
         * @param worldMax  Maximum corner of the playable world volume.
         * @param octreeMaxDepth  Maximum Octree subdivision depth (default 6).
         */
        void Initialize(const DirectX::XMFLOAT3& worldMin, const DirectX::XMFLOAT3& worldMax, int octreeMaxDepth = 6);

        /**
         * @brief Rebuild the spatial index from all perceivable entities.
         *
         * Must be called on the main thread before UpdateAllAgents().
         * Clears and re-populates the Octree with every entity that has
         * a Transform component (and is not an AI agent itself, unless
         * agents should perceive each other).
         *
         * @param world  The ECS World to scan for perceivable entities.
         */
        void RebuildSpatialIndex(World& world);

        /**
         * @brief Register an active sound event for this frame.
         *
         * Call once per sound event before UpdateAllAgents(). The sound
         * list is cleared at the start of each RebuildSpatialIndex() call.
         *
         * @param sourceId  Entity that produced the sound.
         * @param position  World-space origin of the sound.
         * @param radius    Maximum audible distance.
         */
        void AddActiveSound(PerceptionEntityID sourceId, const DirectX::XMFLOAT3& position, float radius);

        /**
         * @brief Run perception for all AI agents in parallel.
         *
         * Gathers agent data, dispatches parallel work via JobSystem,
         * and writes results back to the ECS. Safe to call from the main
         * thread only (ECS read/write happens before and after the parallel section).
         *
         * @param world        The ECS World containing AI agents with PerceptionComponent.
         * @param currentTime  Current world time in seconds (for memory timestamps).
         */
        void UpdateAllAgents(World& world, float currentTime);

        /**
         * @brief Get the number of agents processed in the last update.
         */
        uint32_t GetLastAgentCount() const { return m_lastAgentCount; }

        /**
         * @brief Get the number of entities in the spatial index.
         */
        size_t GetSpatialEntityCount() const;

        /**
         * @brief Set the minimum batch size for ParallelFor dispatch.
         *
         * If the agent count is below this threshold, perception runs
         * single-threaded to avoid thread pool overhead.
         *
         * @param batchSize  Minimum agents per parallel batch (default 4).
         */
        void SetMinBatchSize(int batchSize) { m_minBatchSize = batchSize; }

      private:
        /**
         * @brief Gather per-agent snapshots from the ECS into m_agentJobs.
         */
        void GatherAgentData(World& world);

        /**
         * @brief Process a single agent's perception (called from worker threads).
         *
         * @param job          The agent's snapshot data (read/write).
         * @param currentTime  Current world time.
         */
        void ProcessAgentPerception(AgentPerceptionJob& job, float currentTime) const;

        /**
         * @brief Compute forward direction from Euler rotation angles.
         *
         * @param rotation  Euler angles (pitch, yaw, roll) in radians.
         * @return Normalized forward direction vector.
         */
        static DirectX::XMFLOAT3 ComputeForwardFromRotation(const DirectX::XMFLOAT3& rotation);

        /**
         * @brief Write updated perception data back into ECS components.
         */
        void WriteBackResults(World& world);

#ifdef SPARK_PLATFORM_WINDOWS
        /// Octree spatial index for perceivable entities (Windows only)
        std::unique_ptr<Octree<uint32_t>> m_spatialIndex;
#endif

        /// Flat cache of perceivable entity data (indexed by Octree item value)
        std::vector<PerceivableEntity> m_perceivableEntities;

        /// Active sound events for the current frame
        std::vector<ActiveSound> m_activeSounds;

        /// Per-agent job data gathered before parallel dispatch
        std::vector<AgentPerceptionJob> m_agentJobs;

        /// World bounds for Octree initialization
        DirectX::XMFLOAT3 m_worldMin{-500.0f, -500.0f, -500.0f};
        DirectX::XMFLOAT3 m_worldMax{500.0f, 500.0f, 500.0f};
        int m_octreeMaxDepth = 6;

        /// Minimum agents per parallel batch
        int m_minBatchSize = 4;

        /// Number of agents processed in the last update
        uint32_t m_lastAgentCount = 0;

        /// Whether Initialize() has been called
        std::atomic<bool> m_initialized{false};
    };

} // namespace Spark::AI
