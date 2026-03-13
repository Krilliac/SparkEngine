/**
 * @file ParallelPerception.cpp
 * @brief Implementation of the parallelized, Octree-accelerated perception system
 * @author Spark Engine Team
 * @date 2026
 */

#include "ParallelPerception.h"
#include "../ECS/Components.h"
#include "AISystem.h"
#include "../../Utils/Validate.h"

#include <cmath>
#include <algorithm>

namespace Spark::AI
{

    // =========================================================================
    // Initialization
    // =========================================================================

    void ParallelPerceptionSystem::Initialize(const DirectX::XMFLOAT3& worldMin, const DirectX::XMFLOAT3& worldMax,
                                              int octreeMaxDepth)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        SPARK_LOG_INFO(Spark::LogCategory::AI, "ParallelPerceptionSystem::Initialize (octreeMaxDepth=%d).",
                       octreeMaxDepth);
        m_worldMin = worldMin;
        m_worldMax = worldMax;
        m_octreeMaxDepth = octreeMaxDepth;

#ifdef SPARK_PLATFORM_WINDOWS
        m_spatialIndex = std::make_unique<Octree<uint32_t>>(m_worldMin, m_worldMax, m_octreeMaxDepth);
#endif
        m_initialized = true;
    }

    // =========================================================================
    // Spatial Index
    // =========================================================================

    void ParallelPerceptionSystem::RebuildSpatialIndex(World& world)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        if (!m_initialized)
        {
            SPARK_WARN_IF(Spark::LogCategory::AI, true, "RebuildSpatialIndex called before Initialize");
            return;
        }

        // Clear previous frame data
        m_perceivableEntities.clear();
        m_activeSounds.clear();

#ifdef SPARK_PLATFORM_WINDOWS
        m_spatialIndex = std::make_unique<Octree<uint32_t>>(m_worldMin, m_worldMax, m_octreeMaxDepth);
#endif

        // Insert all entities that have a Transform into the spatial index.
        // Each entity gets a sequential index that maps into m_perceivableEntities.
        auto view = world.GetEntitiesWith<Transform>();
        for (auto entity : view)
        {
            const auto* transform = world.GetComponent<Transform>(entity);
            if (transform == nullptr)
            {
                continue;
            }

            auto index = static_cast<uint32_t>(m_perceivableEntities.size());

            PerceivableEntity pe;
            pe.entityId = entity;
            pe.position = transform->position;
            m_perceivableEntities.push_back(pe);

#ifdef SPARK_PLATFORM_WINDOWS
            // Create a small AABB around the entity position for Octree insertion.
            // Using a 0.5m half-extent as a reasonable default for point entities.
            constexpr float kHalfExtent = 0.5f;
            Graphics::AABB entityBounds;
            entityBounds.min = {transform->position.x - kHalfExtent, transform->position.y - kHalfExtent,
                                transform->position.z - kHalfExtent};
            entityBounds.max = {transform->position.x + kHalfExtent, transform->position.y + kHalfExtent,
                                transform->position.z + kHalfExtent};

            m_spatialIndex->Insert(index, entityBounds);
#endif
        }
    }

    void ParallelPerceptionSystem::AddActiveSound(PerceptionEntityID sourceId, const DirectX::XMFLOAT3& position,
                                                  float radius)
    {
        m_activeSounds.push_back({sourceId, position, radius});
    }

    size_t ParallelPerceptionSystem::GetSpatialEntityCount() const
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (m_spatialIndex)
        {
            return m_spatialIndex->Size();
        }
#endif
        return m_perceivableEntities.size();
    }

    // =========================================================================
    // Agent Data Gathering (main thread)
    // =========================================================================

    void ParallelPerceptionSystem::GatherAgentData(World& world)
    {
        m_agentJobs.clear();

        auto view = world.GetEntitiesWith<PerceptionComponent, Transform>();
        for (auto entity : view)
        {
            const auto* perception = world.GetComponent<PerceptionComponent>(entity);
            const auto* transform = world.GetComponent<Transform>(entity);
            if (perception == nullptr || transform == nullptr)
            {
                continue;
            }

            AgentPerceptionJob job;
            job.entityId = entity;
            job.position = transform->position;
            job.forward = ComputeForwardFromRotation(transform->rotation);
            job.perception = *perception; // snapshot copy
            m_agentJobs.push_back(std::move(job));
        }
    }

    // =========================================================================
    // Forward Direction Computation
    // =========================================================================

    DirectX::XMFLOAT3 ParallelPerceptionSystem::ComputeForwardFromRotation(const DirectX::XMFLOAT3& rotation)
    {
        // rotation.x = pitch, rotation.y = yaw, rotation.z = roll
        // Forward vector from pitch and yaw (standard FPS convention)
        const float pitch = rotation.x;
        const float yaw = rotation.y;

        const float cosPitch = std::cos(pitch);
        DirectX::XMFLOAT3 forward;
        forward.x = std::sin(yaw) * cosPitch;
        forward.y = -std::sin(pitch);
        forward.z = std::cos(yaw) * cosPitch;

        // Normalize (should already be unit length from trig, but guard against edge cases)
        const float len = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
        if (len > 1e-6f)
        {
            const float inv = 1.0f / len;
            forward.x *= inv;
            forward.y *= inv;
            forward.z *= inv;
        }
        else
        {
            forward = {0.0f, 0.0f, 1.0f};
        }

        return forward;
    }

    // =========================================================================
    // Brute-force spatial query helper (non-Windows fallback)
    // =========================================================================

    namespace
    {
        /**
         * @brief Brute-force sphere query over the perceivable entity list.
         *
         * Returns indices of entities within the given radius of the center.
         * Used on non-Windows platforms where the Octree is not available.
         */
        std::vector<uint32_t> BruteForceSphereQuery(const std::vector<PerceivableEntity>& entities,
                                                    const DirectX::XMFLOAT3& center, float radius)
        {
            std::vector<uint32_t> results;
            const float radiusSq = radius * radius;

            for (uint32_t i = 0; i < static_cast<uint32_t>(entities.size()); ++i)
            {
                const auto& pos = entities[i].position;
                const float dx = pos.x - center.x;
                const float dy = pos.y - center.y;
                const float dz = pos.z - center.z;
                const float distSq = dx * dx + dy * dy + dz * dz;

                if (distSq <= radiusSq)
                {
                    results.push_back(i);
                }
            }

            return results;
        }
    } // anonymous namespace

    // =========================================================================
    // Per-Agent Perception (worker thread safe)
    // =========================================================================

    void ParallelPerceptionSystem::ProcessAgentPerception(AgentPerceptionJob& job, float currentTime) const
    {
        // Determine the maximum perception radius for this agent (max of sight and hearing).
        const float maxRadius = std::max(job.perception.sightRange, job.perception.hearingRange);

        // --- Spatial query: find nearby candidates ---
#ifdef SPARK_PLATFORM_WINDOWS
        auto nearbyIndices = m_spatialIndex->QuerySphere(job.position, maxRadius);
#else
        auto nearbyIndices = BruteForceSphereQuery(m_perceivableEntities, job.position, maxRadius);
#endif

        // Build target lists from the spatial query results
        std::vector<PerceptionEntityID> targetIds;
        std::vector<DirectX::XMFLOAT3> targetPositions;
        targetIds.reserve(nearbyIndices.size());
        targetPositions.reserve(nearbyIndices.size());

        for (uint32_t index : nearbyIndices)
        {
            if (index >= m_perceivableEntities.size())
            {
                continue;
            }

            const auto& pe = m_perceivableEntities[index];

            // Skip self-perception
            if (pe.entityId == job.entityId)
            {
                continue;
            }

            targetIds.push_back(static_cast<PerceptionEntityID>(pe.entityId));
            targetPositions.push_back(pe.position);
        }

        // --- Filter active sounds by hearing range ---
        std::vector<DirectX::XMFLOAT3> soundPositions;
        std::vector<float> soundRadii;
        std::vector<PerceptionEntityID> soundSourceIds;

        for (const auto& sound : m_activeSounds)
        {
            // Quick distance check: skip sounds that are clearly out of range
            const float dx = sound.position.x - job.position.x;
            const float dy = sound.position.y - job.position.y;
            const float dz = sound.position.z - job.position.z;
            const float distSq = dx * dx + dy * dy + dz * dz;
            const float combinedRange = sound.radius + job.perception.hearingRange;

            if (distSq <= combinedRange * combinedRange)
            {
                soundPositions.push_back(sound.position);
                soundRadii.push_back(sound.radius);
                soundSourceIds.push_back(sound.sourceId);
            }
        }

        // --- Run the standard perception update ---
        Perception::UpdatePerception(job.perception, job.position, job.forward, currentTime, targetIds, targetPositions,
                                     soundPositions, soundRadii, soundSourceIds);
    }

    // =========================================================================
    // Main Update (main thread entry point)
    // =========================================================================

    void ParallelPerceptionSystem::UpdateAllAgents(World& world, float currentTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        if (!m_initialized)
        {
            return;
        }

        // Step 1: Gather agent data (main thread)
        GatherAgentData(world);

        m_lastAgentCount = static_cast<uint32_t>(m_agentJobs.size());

        if (m_agentJobs.empty())
        {
            return;
        }

        // Step 2: Dispatch parallel perception processing
        auto& jobSystem = JobSystem::Get();
        const int agentCount = static_cast<int>(m_agentJobs.size());

        if (jobSystem.IsInitialized() && agentCount >= m_minBatchSize)
        {
            // Parallel path: distribute agents across worker threads
            jobSystem.ParallelFor(
                0, agentCount, [this, currentTime](int i) { ProcessAgentPerception(m_agentJobs[i], currentTime); },
                m_minBatchSize);
        }
        else
        {
            // Sequential fallback for small counts or uninitialized job system
            for (int i = 0; i < agentCount; ++i)
            {
                ProcessAgentPerception(m_agentJobs[i], currentTime);
            }
        }

        // Step 3: Write results back to ECS (main thread)
        WriteBackResults(world);
    }

    // =========================================================================
    // Write-back (main thread)
    // =========================================================================

    void ParallelPerceptionSystem::WriteBackResults(World& world)
    {
        for (const auto& job : m_agentJobs)
        {
            auto* perception = world.GetComponent<PerceptionComponent>(job.entityId);
            if (perception != nullptr)
            {
                *perception = job.perception;
            }
        }
    }

} // namespace Spark::AI
