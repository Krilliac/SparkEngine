/**
 * @file WorldOriginSystem.cpp
 * @brief Floating-point origin rebasing implementation
 */

#include "WorldOriginSystem.h"
#include "../../Core/FaultIsolation.h"
#include "../ECS/Components/CoreComponents.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/Validate.h"

#include <cmath>
#include <entt/entt.hpp>

namespace Spark::World
{

    // ============================================================================
    // Callbacks
    // ============================================================================

    void WorldOriginSystem::RegisterRebaseCallback(OriginRebasedCallback callback)
    {
        if (callback)
        {
            m_callbacks.push_back(std::move(callback));
        }
    }

    // ============================================================================
    // Update
    // ============================================================================

    bool WorldOriginSystem::Update(entt::registry& registry, const DirectX::XMFLOAT3& referencePos)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        if (!m_enabled)
        {
            return false;
        }

        // Validate reference position
        if (!std::isfinite(referencePos.x) || !std::isfinite(referencePos.y) || !std::isfinite(referencePos.z))
        {
            SPARK_LOG_ERROR("WorldOrigin",
                            "Update called with non-finite reference position (%.1f, %.1f, %.1f). "
                            "Skipping — this may indicate a physics or transform bug.",
                            referencePos.x, referencePos.y, referencePos.z);
            return false;
        }

        // Calculate distance from current local origin
        float distSq =
            referencePos.x * referencePos.x + referencePos.y * referencePos.y + referencePos.z * referencePos.z;
        float dist = std::sqrt(distSq);

        // Track peak distance
        if (dist > m_stats.maxDistanceFromOrigin)
        {
            m_stats.maxDistanceFromOrigin = dist;
        }

        // Check if rebasing is needed
        if (dist < m_threshold)
        {
            return false;
        }

        // Rebase: shift everything so the reference position is near the origin
        ForceRebase(registry, referencePos);
        m_stats.lastRebaseDistance = dist;
        return true;
    }

    void WorldOriginSystem::ForceRebase(entt::registry& registry, const DirectX::XMFLOAT3& offset)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        // Validate offset
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
        {
            SPARK_LOG_ERROR("WorldOrigin", "ForceRebase called with non-finite offset (%.1f, %.1f, %.1f). Aborting.",
                            offset.x, offset.y, offset.z);
            return;
        }

        // Skip zero-offset rebases
        if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f)
        {
            SPARK_LOG_DEBUG("WorldOrigin", "ForceRebase called with zero offset. Skipping.");
            return;
        }

        // Shift all entity transforms
        ShiftAllTransforms(registry, offset);

        // Accumulate the offset
        m_accumulatedOffset.x += offset.x;
        m_accumulatedOffset.y += offset.y;
        m_accumulatedOffset.z += offset.z;

        m_stats.totalRebases++;
        m_stats.currentOffset = m_accumulatedOffset;

        // Notify external systems (physics, audio, particles, navmesh)
        NotifyCallbacks(offset);

        SPARK_LOG_INFO("WorldOrigin", "Rebased origin by (%.1f, %.1f, %.1f). Total offset: (%.1f, %.1f, %.1f)",
                       offset.x, offset.y, offset.z, m_accumulatedOffset.x, m_accumulatedOffset.y,
                       m_accumulatedOffset.z);
    }

    // ============================================================================
    // Coordinate Conversion
    // ============================================================================

    DirectX::XMFLOAT3 WorldOriginSystem::LocalToAbsolute(const DirectX::XMFLOAT3& localPos) const
    {
        return {localPos.x + m_accumulatedOffset.x, localPos.y + m_accumulatedOffset.y,
                localPos.z + m_accumulatedOffset.z};
    }

    DirectX::XMFLOAT3 WorldOriginSystem::AbsoluteToLocal(const DirectX::XMFLOAT3& absolutePos) const
    {
        return {absolutePos.x - m_accumulatedOffset.x, absolutePos.y - m_accumulatedOffset.y,
                absolutePos.z - m_accumulatedOffset.z};
    }

    // ============================================================================
    // Reset
    // ============================================================================

    void WorldOriginSystem::Reset()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "WorldOriginSystem resetting accumulated offset");
        m_accumulatedOffset = {0.0f, 0.0f, 0.0f};
        m_stats = {};
    }

    // ============================================================================
    // Internal
    // ============================================================================

    void WorldOriginSystem::ShiftAllTransforms(entt::registry& registry, const DirectX::XMFLOAT3& offset)
    {
        auto view = registry.view<Transform>();
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);

            // Only shift root entities (no parent) — children inherit via hierarchy
            if (transform.parent == entt::null)
            {
                transform.position.x -= offset.x;
                transform.position.y -= offset.y;
                transform.position.z -= offset.z;
            }
        }
    }

    void WorldOriginSystem::NotifyCallbacks(const DirectX::XMFLOAT3& offset)
    {
        for (const auto& callback : m_callbacks)
        {
            SPARK_GUARDED_UPDATE("WorldOrigin:Callback", "Core", { callback(offset); });
        }
    }

} // namespace Spark::World
