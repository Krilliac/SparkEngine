/**
 * @file AIDebugRenderer.h
 * @brief Debug visualization for AI systems: NavMesh, paths, perception cones
 *
 * Uses the engine's DebugDraw system to render AI debug overlays when enabled
 * in the editor's DebugVisualizerPanel. All rendering is conditional on the
 * panel's toggle flags (m_showNavMesh, m_showNavPaths, etc.).
 *
 * @threadsafety Main thread only (DebugDraw submission).
 */

#pragma once

#include "../../Core/Platform.h"
#include "NavMesh.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <vector>

namespace Spark::AI
{

    /**
     * @brief Renders AI debug visualizations using the DebugDraw system.
     *
     * Call RenderNavMesh/RenderPaths/RenderPerception each frame when the
     * corresponding debug flags are enabled. These methods submit debug
     * primitives to DebugDrawManager for rendering on the next frame.
     */
    class AIDebugRenderer
    {
      public:
        /**
         * @brief Render NavMesh triangles as wireframe overlay.
         * @param navMesh  NavMesh data to visualize.
         * @param alpha    Triangle fill opacity (0-1).
         */
        static void RenderNavMesh(const NavMeshData& navMesh, float alpha = 0.25f);

        /**
         * @brief Render NavMesh bounding box.
         * @param navMesh  NavMesh data.
         */
        static void RenderNavMeshBounds(const NavMeshData& navMesh);

        /**
         * @brief Render an AI agent's current path as a line strip with waypoint markers.
         * @param path         Waypoints to render.
         * @param currentIndex Current waypoint index (highlighted differently).
         */
        static void RenderPath(const std::vector<PathPoint>& path, size_t currentIndex);

        /**
         * @brief Render a perception cone for an AI agent.
         * @param position    Agent position.
         * @param direction   Agent facing direction.
         * @param range       Perception range.
         * @param halfAngle   Half-angle of the cone in radians.
         */
        static void RenderPerceptionCone(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& direction,
                                         float range, float halfAngle);

        /**
         * @brief Render agent position marker with direction arrow.
         * @param position   Agent world position.
         * @param direction  Agent facing direction (normalized).
         */
        static void RenderAgentMarker(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& direction);
    };

} // namespace Spark::AI
