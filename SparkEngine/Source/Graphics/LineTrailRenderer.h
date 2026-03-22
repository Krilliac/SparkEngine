/**
 * @file LineTrailRenderer.h
 * @brief World-space line rendering and trail effects
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides two rendering primitives common across game engines:
 * - LineRenderer: renders a polyline from a list of world-space points
 * - TrailRenderer: renders a fading trail behind a moving entity
 *
 * Both generate camera-facing triangle strips for efficient GPU rendering.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // LineRenderer — World-space polyline
    // =========================================================================

    /**
     * @brief Renders a polyline from world-space points as a camera-facing strip.
     */
    struct LineRendererData
    {
        std::vector<XMFLOAT3> points;    ///< Polyline control points.
        float startWidth = 0.1f;         ///< Width at the first point.
        float endWidth = 0.1f;           ///< Width at the last point.
        XMFLOAT4 startColor{1, 1, 1, 1}; ///< Color at the first point.
        XMFLOAT4 endColor{1, 1, 1, 1};   ///< Color at the last point.
        std::string materialPath;        ///< Material/texture for the line.
        int cornerVertices = 0;          ///< Extra vertices at corners for smooth bends.
        bool useWorldSpace = true;       ///< Points in world or local space.
        bool loop = false;               ///< Connect last point to first.
        int sortingLayer = 0;            ///< Render sort order.
    };

    // =========================================================================
    // TrailRenderer — Fading trail behind moving entity
    // =========================================================================

    /**
     * @brief A single point in the trail history.
     */
    struct TrailPoint
    {
        XMFLOAT3 position{0, 0, 0};
        float timeCreated = 0.0f;
    };

    /**
     * @brief Renders a fading trail behind a moving entity.
     *
     * Each frame, the entity's current position is appended to the trail.
     * Old points are removed when they exceed the trail lifetime. The trail
     * is rendered as a camera-facing triangle strip with width/color gradients.
     */
    struct TrailRendererData
    {
        float lifetime = 2.0f;           ///< How long each trail segment persists (seconds).
        float minVertexDistance = 0.1f;  ///< Min distance before adding a new trail point.
        float startWidth = 0.5f;         ///< Width at the newest point.
        float endWidth = 0.0f;           ///< Width at the oldest point (0 = taper to nothing).
        XMFLOAT4 startColor{1, 1, 1, 1}; ///< Color at the newest point.
        XMFLOAT4 endColor{1, 1, 1, 0};   ///< Color at the oldest point (alpha 0 = fade out).
        std::string materialPath;        ///< Material/texture for the trail.
        bool emitting = true;            ///< Whether to add new points.
        bool autodestruct = false;       ///< Destroy entity when trail fully fades.
        int sortingLayer = 0;            ///< Render sort order.

        // Runtime state
        std::deque<TrailPoint> trailPoints; ///< Active trail point history.
        float currentTime = 0.0f;           ///< Accumulated time for aging points.

        /**
         * @brief Update the trail: add new point if moved enough, remove expired.
         */
        void Update(const XMFLOAT3& currentPos, float deltaTime)
        {
            currentTime += deltaTime;

            // Remove expired points
            while (!trailPoints.empty() && (currentTime - trailPoints.front().timeCreated) > lifetime)
            {
                trailPoints.pop_front();
            }

            // Add new point if emitting and moved enough
            if (emitting)
            {
                bool shouldAdd = trailPoints.empty();
                if (!shouldAdd)
                {
                    const auto& last = trailPoints.back().position;
                    float dx = currentPos.x - last.x;
                    float dy = currentPos.y - last.y;
                    float dz = currentPos.z - last.z;
                    shouldAdd = (dx * dx + dy * dy + dz * dz) >= (minVertexDistance * minVertexDistance);
                }
                if (shouldAdd)
                {
                    trailPoints.push_back({currentPos, currentTime});
                }
            }
        }

        /** @brief Get the number of active trail segments. */
        size_t GetActivePointCount() const { return trailPoints.size(); }

        /** @brief Clear all trail points. */
        void Clear() { trailPoints.clear(); }
    };

} // namespace Spark::Graphics
