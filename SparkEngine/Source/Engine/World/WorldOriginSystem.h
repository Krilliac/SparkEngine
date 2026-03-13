/**
 * @file WorldOriginSystem.h
 * @brief Floating-point origin rebasing system for large worlds
 * @author Spark Engine Team
 * @date 2026
 *
 * Inspired by HeroEngine's per-area local coordinate origins, this system
 * prevents floating-point precision loss (jitter, physics instability) when
 * the player moves far from the world origin (0,0,0).
 *
 * When the player crosses a configurable distance threshold, all entity
 * transforms, physics bodies, audio sources, and particle emitters are
 * shifted so the player is near the origin again. This is transparent to
 * gameplay code — positions remain semantically correct in world space via
 * the accumulated origin offset.
 *
 * ## Usage
 * @code
 *   WorldOriginSystem originSystem;
 *   originSystem.SetRebasingThreshold(5000.0f);
 *
 *   // In the game loop:
 *   originSystem.Update(registry, playerPosition);
 *
 *   // To convert between local and absolute coordinates:
 *   auto absolute = originSystem.LocalToAbsolute(localPos);
 *   auto local    = originSystem.AbsoluteToLocal(absolutePos);
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#else
#include "../../Core/Platform.h"
#endif

#include <cstdint>
#include <functional>
#include <vector>

#include <entt/entt.hpp>

namespace Spark::World
{

    /**
     * @brief Callback invoked when the world origin is rebased
     *
     * Systems that maintain their own world-space positions (physics, audio,
     * particles, navmesh) should register a callback to shift their data.
     *
     * @param offset The offset that was subtracted from all positions
     */
    using OriginRebasedCallback = std::function<void(const DirectX::XMFLOAT3& offset)>;

    /**
     * @brief Statistics about origin rebasing
     */
    struct OriginRebasingStats
    {
        uint32_t totalRebases = 0;                            ///< Total times origin was rebased
        DirectX::XMFLOAT3 currentOffset = {0.0f, 0.0f, 0.0f}; ///< Accumulated origin offset
        float lastRebaseDistance = 0.0f;                      ///< Distance that triggered last rebase
        float maxDistanceFromOrigin = 0.0f;                   ///< Peak distance observed
    };

    /**
     * @brief Manages floating-point origin rebasing for large worlds
     *
     * Tracks the accumulated world origin offset and triggers rebasing when
     * the reference position (typically the player camera) exceeds a distance
     * threshold from the current local origin.
     */
    class WorldOriginSystem
    {
      public:
        WorldOriginSystem() = default;
        ~WorldOriginSystem() = default;

        // -- Configuration --

        /**
         * @brief Set the distance threshold that triggers origin rebasing
         * @param threshold Distance in world units (default: 5000.0)
         */
        void SetRebasingThreshold(float threshold) { m_threshold = threshold > 0.0f ? threshold : m_threshold; }

        /**
         * @brief Get the current rebasing threshold
         */
        float GetRebasingThreshold() const { return m_threshold; }

        /**
         * @brief Enable or disable origin rebasing
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        /**
         * @brief Check if origin rebasing is enabled
         */
        bool IsEnabled() const { return m_enabled; }

        // -- Callbacks --

        /**
         * @brief Register a callback for origin rebase events
         *
         * External systems (physics, audio, particles, navmesh) should
         * register here to shift their world-space data when rebasing occurs.
         *
         * @param callback Function called with the rebase offset
         */
        void RegisterRebaseCallback(OriginRebasedCallback callback);

        // -- Update --

        /**
         * @brief Check if rebasing is needed and perform it
         *
         * Call this once per frame. If the reference position exceeds the
         * threshold distance from the local origin, all entity Transform
         * positions in the registry are shifted, callbacks are fired, and
         * the accumulated offset is updated.
         *
         * @param registry    The ECS registry containing Transform components
         * @param referencePos The reference position (e.g., player/camera)
         * @return true if rebasing occurred this frame
         */
        bool Update(entt::registry& registry, const DirectX::XMFLOAT3& referencePos);

        /**
         * @brief Force an immediate origin rebase with the given offset
         * @param registry The ECS registry
         * @param offset   The offset to subtract from all positions
         */
        void ForceRebase(entt::registry& registry, const DirectX::XMFLOAT3& offset);

        // -- Coordinate conversion --

        /**
         * @brief Convert a local-space position to absolute world coordinates
         * @param localPos Position in local (rebased) space
         * @return Position in absolute world space
         */
        DirectX::XMFLOAT3 LocalToAbsolute(const DirectX::XMFLOAT3& localPos) const;

        /**
         * @brief Convert an absolute world position to local (rebased) space
         * @param absolutePos Position in absolute world space
         * @return Position in local (rebased) space
         */
        DirectX::XMFLOAT3 AbsoluteToLocal(const DirectX::XMFLOAT3& absolutePos) const;

        /**
         * @brief Get the accumulated origin offset
         *
         * This is the total displacement from the original world origin
         * to the current local origin. Adding this to any local position
         * yields the absolute world position.
         */
        const DirectX::XMFLOAT3& GetAccumulatedOffset() const { return m_accumulatedOffset; }

        /**
         * @brief Get rebasing statistics
         */
        const OriginRebasingStats& GetStats() const { return m_stats; }

        /**
         * @brief Reset the origin system to initial state
         */
        void Reset();

      private:
        /**
         * @brief Apply an offset to all entity transforms in the registry
         */
        void ShiftAllTransforms(entt::registry& registry, const DirectX::XMFLOAT3& offset);

        /**
         * @brief Notify all registered callbacks of a rebase
         */
        void NotifyCallbacks(const DirectX::XMFLOAT3& offset);

        float m_threshold = 5000.0f;
        bool m_enabled = true;
        DirectX::XMFLOAT3 m_accumulatedOffset = {0.0f, 0.0f, 0.0f};

        std::vector<OriginRebasedCallback> m_callbacks;
        OriginRebasingStats m_stats;
    };

} // namespace Spark::World
