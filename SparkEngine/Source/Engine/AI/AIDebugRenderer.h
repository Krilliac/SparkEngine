/**
 * @file AIDebugRenderer.h
 * @brief Debug visualization for AI systems: NavMesh, paths, perception, behavior trees
 * @author Spark Engine Team
 * @date 2026
 *
 * Uses the engine's DebugDraw system to render AI debug overlays when enabled.
 * Supports NavMesh wireframe, path visualization, perception cones, behavior
 * tree state, cover points, and formation display.
 *
 * @threadsafety Main thread only (DebugDraw submission).
 */

#pragma once

#include "../../Core/Platform.h"
#include "NavMesh.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::AI
{

    // Forward declarations
    struct CoverPoint;

    // ============================================================================
    // Debug Draw Options
    // ============================================================================

    /**
     * @brief Configuration for AI debug visualization
     */
    struct DebugDrawOptions
    {
        bool drawNavMesh = true;
        bool drawNavMeshBounds = false;
        bool drawPaths = true;
        bool drawPerceptionCones = true;
        bool drawBehaviorTreeState = false;
        bool drawFormations = true;
        bool drawCoverPoints = true;
        bool drawTacticalPoints = false;

        float navMeshOpacity = 0.3f;
        float pathLineWidth = 2.0f;

        // Colors (ARGB packed)
        uint32_t navMeshColor = 0x4000FF00;        ///< Semi-transparent green
        uint32_t pathColor = 0xFFFFFF00;           ///< Yellow
        uint32_t perceptionConeColor = 0x400000FF; ///< Semi-transparent blue
        uint32_t coverPointColor = 0xFFFF0000;     ///< Red
        uint32_t activeNodeColor = 0xFF00FF00;     ///< Green (active BT node)
    };

    // ============================================================================
    // AIDebugRenderer
    // ============================================================================

    /**
     * @brief Renders AI debug visualizations using the DebugDraw system
     *
     * Singleton that manages AI debug overlay state. Enable/disable globally,
     * select a specific agent to highlight, and configure which overlays are drawn.
     */
    class AIDebugRenderer
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the global AIDebugRenderer
         */
        static AIDebugRenderer& GetInstance();

        /**
         * @brief Initialize the debug renderer
         */
        void Initialize();

        /**
         * @brief Per-frame update; draws all enabled visualizations
         * @param deltaTime Frame time in seconds
         */
        void Update(float deltaTime);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        // ====================================================================
        // NavMesh visualization
        // ====================================================================

        /**
         * @brief Render NavMesh triangles as wireframe overlay
         * @param navMesh NavMesh data to visualize
         */
        void DrawNavMesh(const NavMeshData& navMesh);

        /**
         * @brief Render a single NavMesh polygon
         * @param vertices Polygon vertex positions
         * @param color    ARGB packed color
         */
        void DrawNavMeshPolygon(const std::vector<XMFLOAT3>& vertices, uint32_t color);

        /**
         * @brief Render a path as a line strip with arrows
         * @param waypoints Path waypoints
         * @param color     ARGB packed color
         */
        void DrawPath(const std::vector<XMFLOAT3>& waypoints, uint32_t color);

        // ====================================================================
        // AI agent visualization
        // ====================================================================

        /**
         * @brief Render a perception cone for an AI agent
         * @param position  Agent world position
         * @param forward   Agent facing direction (normalized)
         * @param range     Perception range in world units
         * @param halfAngle Half-angle of the cone in radians
         * @param color     ARGB packed color
         */
        void DrawPerceptionCone(const XMFLOAT3& position, const XMFLOAT3& forward, float range, float halfAngle,
                                uint32_t color);

        /**
         * @brief Render behavior tree state overlay at an agent's position
         * @param worldPos    Agent world position for text placement
         * @param activeNode  Name of the currently active behavior tree node
         */
        void DrawBehaviorTreeState(const XMFLOAT3& worldPos, const std::string& activeNode);

        // ====================================================================
        // Tactical visualization
        // ====================================================================

        /**
         * @brief Render cover point markers
         * @param positions Cover point world positions
         */
        void DrawCoverPoints(const std::vector<XMFLOAT3>& positions);

        /**
         * @brief Render formation lines connecting positions to center
         * @param center    Formation center point
         * @param positions Formation member positions
         */
        void DrawFormation(const XMFLOAT3& center, const std::vector<XMFLOAT3>& positions);

        // ====================================================================
        // Static convenience methods (backward compatible)
        // ====================================================================

        /**
         * @brief Render NavMesh triangles as wireframe overlay (static)
         * @param navMesh NavMesh data to visualize
         * @param alpha   Triangle fill opacity (0-1)
         */
        static void RenderNavMesh(const NavMeshData& navMesh, float alpha = 0.25f);

        /** @brief Render NavMesh bounding box (static) */
        static void RenderNavMeshBounds(const NavMeshData& navMesh);

        /** @brief Render a path as a line strip with waypoint markers (static) */
        static void RenderPath(const std::vector<PathPoint>& path, size_t currentIndex);

        /** @brief Render a perception cone (static) */
        static void RenderPerceptionCone(const XMFLOAT3& position, const XMFLOAT3& direction, float range,
                                         float halfAngle);

        /** @brief Render agent position marker with direction arrow (static) */
        static void RenderAgentMarker(const XMFLOAT3& position, const XMFLOAT3& direction);

        // ====================================================================
        // Options and state
        // ====================================================================

        /**
         * @brief Set debug draw options
         * @param options New options to apply
         */
        void SetOptions(const DebugDrawOptions& options) { m_options = options; }

        /**
         * @brief Get current debug draw options
         * @return Const reference to current options
         */
        const DebugDrawOptions& GetOptions() const { return m_options; }

        /**
         * @brief Enable or disable all AI debug rendering
         * @param enabled true to enable
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        /**
         * @brief Check if debug rendering is enabled
         * @return true if enabled
         */
        bool IsEnabled() const { return m_enabled; }

        /**
         * @brief Select a specific agent to highlight
         * @param entityId Entity ID to highlight (0 = none)
         */
        void SetSelectedAgent(uint32_t entityId) { m_selectedAgent = entityId; }

        /**
         * @brief Get the currently selected agent
         * @return Entity ID of selected agent (0 = none)
         */
        uint32_t GetSelectedAgent() const { return m_selectedAgent; }

        /**
         * @brief Get a status string for console display
         * @return Formatted status string
         */
        std::string Console_GetStatus() const;

      private:
        AIDebugRenderer() = default;

        DebugDrawOptions m_options;
        bool m_enabled = false;
        bool m_initialized = false;
        uint32_t m_selectedAgent = 0;
    };

} // namespace Spark::AI
