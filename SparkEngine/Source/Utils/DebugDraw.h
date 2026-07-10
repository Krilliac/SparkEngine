/**
 * @file DebugDraw.h
 * @brief 3D debug shape rendering system for runtime visualization
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides immediate-mode debug drawing for visualizing collision volumes,
 * raycasts, navigation paths, AI sight cones, and other spatial data
 * directly in the 3D viewport. All draw calls are batched per-frame and
 * rendered as an overlay after the main scene pass.
 *
 * Features:
 * - Lines, rays, arrows, circles, spheres, boxes, capsules, frustums
 * - Configurable color, thickness, and lifetime (persistent or single-frame)
 * - Depth-tested and always-on-top modes
 * - Automatic expiry of timed shapes
 * - Thread-safe submission from any system
 *
 * Typical usage:
 * @code
 *   // Draw a red ray from the player's weapon
 *   DEBUG_DRAW_RAY(origin, direction, 100.0f, {1,0,0,1});
 *
 *   // Draw a green sphere at an AI waypoint for 3 seconds
 *   DEBUG_DRAW_SPHERE(waypointPos, 0.5f, {0,1,0,1}, 3.0f);
 *
 *   // Draw a box around a collision volume
 *   DEBUG_DRAW_AABB(minBounds, maxBounds, {1,1,0,1});
 * @endcode
 *
 * @see Profiler.h, SparkConsole.h
 */

#pragma once

#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif
#include <vector>
#include <string>
#include <mutex>
#include <cmath>

namespace Spark
{

    /**
 * @brief RGBA color for debug shapes
 */
    struct DebugColor
    {
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

        static DebugColor Red() { return {1.0f, 0.0f, 0.0f, 1.0f}; }
        static DebugColor Green() { return {0.0f, 1.0f, 0.0f, 1.0f}; }
        static DebugColor Blue() { return {0.0f, 0.0f, 1.0f, 1.0f}; }
        static DebugColor Yellow() { return {1.0f, 1.0f, 0.0f, 1.0f}; }
        static DebugColor Cyan() { return {0.0f, 1.0f, 1.0f, 1.0f}; }
        static DebugColor Magenta() { return {1.0f, 0.0f, 1.0f, 1.0f}; }
        static DebugColor White() { return {1.0f, 1.0f, 1.0f, 1.0f}; }
        static DebugColor Orange() { return {1.0f, 0.65f, 0.0f, 1.0f}; }
    };

    /**
 * @brief Type of debug shape primitive
 */
    enum class DebugShapeType
    {
        Line,
        Ray,
        Arrow,
        Circle,
        Sphere,
        AABB,
        OBB,
        Capsule,
        Frustum,
        Cross,  // 3D cross-hair marker
        Text3D, // World-space text label
        Triangle,
        Plane,
        Cone,
        Grid
    };

    /**
 * @brief Single debug draw request
 */
    struct DebugDrawRequest
    {
        DebugShapeType type = DebugShapeType::Line;

        // Geometry data (usage depends on type)
        DirectX::XMFLOAT3 p0 = {0, 0, 0}; // Start/center/min point
        DirectX::XMFLOAT3 p1 = {0, 0, 0}; // End/max point / direction
        DirectX::XMFLOAT3 p2 = {0, 0, 0}; // Extra point (for triangles, OBB axes, etc.)
        float radius = 0.0f;              // Radius/size parameter
        float height = 0.0f;              // Height for capsules/cones

        // Appearance
        DebugColor color;
        float thickness = 1.0f;

        // Lifetime
        float remainingTime = 0.0f; // 0 = single frame, > 0 = seconds
        bool depthTested = true;    // false = always on top

        // Text (for Text3D type)
        std::string text;
    };

    /**
 * @brief Singleton debug draw manager
 *
 * Collects debug draw requests from any thread and provides them
 * to the renderer in a batched, thread-safe manner. Call Flush()
 * at the end of each frame to advance timers and discard expired shapes.
 */
    class DebugDrawManager
    {
      public:
        static DebugDrawManager& GetInstance()
        {
            static DebugDrawManager instance;
            return instance;
        }

        // ========================================================================
        // Primitive submission
        // ========================================================================

        /** @brief Draw a line segment between two points */
        void DrawLine(const DirectX::XMFLOAT3& start, const DirectX::XMFLOAT3& end,
                      const DebugColor& color = DebugColor::White(), float lifetime = 0.0f, float thickness = 1.0f,
                      bool depthTested = true)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Line;
            req.p0 = start;
            req.p1 = end;
            req.color = color;
            req.remainingTime = lifetime;
            req.thickness = thickness;
            req.depthTested = depthTested;
            Submit(req);
        }

        /** @brief Draw a ray from origin along direction for given length */
        void DrawRay(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction, float length,
                     const DebugColor& color = DebugColor::Green(), float lifetime = 0.0f, bool depthTested = true)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Ray;
            req.p0 = origin;
            req.p1 = direction;
            req.radius = length;
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = depthTested;
            Submit(req);
        }

        /** @brief Draw a directional arrow */
        void DrawArrow(const DirectX::XMFLOAT3& start, const DirectX::XMFLOAT3& end, float headSize,
                       const DebugColor& color = DebugColor::Yellow(), float lifetime = 0.0f, bool depthTested = true)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Arrow;
            req.p0 = start;
            req.p1 = end;
            req.radius = headSize;
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = depthTested;
            Submit(req);
        }

        /** @brief Draw a wire sphere at center with radius */
        void DrawSphere(const DirectX::XMFLOAT3& center, float radius, const DebugColor& color = DebugColor::Cyan(),
                        float lifetime = 0.0f, int segments = 16, bool depthTested = true)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Sphere;
            req.p0 = center;
            req.radius = radius;
            req.height = static_cast<float>(segments);
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = depthTested;
            Submit(req);
        }

        /** @brief Draw an axis-aligned bounding box */
        void DrawAABB(const DirectX::XMFLOAT3& min, const DirectX::XMFLOAT3& max,
                      const DebugColor& color = DebugColor::Yellow(), float lifetime = 0.0f, bool depthTested = true)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::AABB;
            req.p0 = min;
            req.p1 = max;
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = depthTested;
            Submit(req);
        }

        /** @brief Draw a capsule (cylinder + hemisphere caps) */
        void DrawCapsule(const DirectX::XMFLOAT3& base, float radius, float height,
                         const DebugColor& color = DebugColor::Magenta(), float lifetime = 0.0f,
                         bool depthTested = true)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Capsule;
            req.p0 = base;
            req.radius = radius;
            req.height = height;
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = depthTested;
            Submit(req);
        }

        /** @brief Draw a 3D cross-hair marker */
        void DrawCross(const DirectX::XMFLOAT3& position, float size, const DebugColor& color = DebugColor::Red(),
                       float lifetime = 0.0f, bool depthTested = true)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Cross;
            req.p0 = position;
            req.radius = size;
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = depthTested;
            Submit(req);
        }

        /** @brief Draw a wire cone (useful for spotlights, AI vision cones) */
        void DrawCone(const DirectX::XMFLOAT3& apex, const DirectX::XMFLOAT3& direction, float height, float angle,
                      const DebugColor& color = DebugColor::Orange(), float lifetime = 0.0f, bool depthTested = true)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Cone;
            req.p0 = apex;
            req.p1 = direction;
            req.height = height;
            req.radius = angle;
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = depthTested;
            Submit(req);
        }

        /** @brief Draw a world-space text label */
        void DrawText3D(const DirectX::XMFLOAT3& position, const std::string& text,
                        const DebugColor& color = DebugColor::White(), float lifetime = 0.0f)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Text3D;
            req.p0 = position;
            req.text = text;
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = false;
            Submit(req);
        }

        /** @brief Draw a wire grid on the XZ plane */
        void DrawGrid(const DirectX::XMFLOAT3& center, float cellSize, int cellCount,
                      const DebugColor& color = {0.3f, 0.3f, 0.3f, 0.5f}, float lifetime = 0.0f)
        {
            DebugDrawRequest req;
            req.type = DebugShapeType::Grid;
            req.p0 = center;
            req.radius = cellSize;
            req.height = static_cast<float>(cellCount);
            req.color = color;
            req.remainingTime = lifetime;
            req.depthTested = true;
            Submit(req);
        }

        /** @brief Draw coordinate axes at a position (R=X, G=Y, B=Z) */
        void DrawAxes(const DirectX::XMFLOAT3& position, float length = 1.0f, float lifetime = 0.0f,
                      bool depthTested = true)
        {
            DrawArrow(position, {position.x + length, position.y, position.z}, length * 0.1f, DebugColor::Red(),
                      lifetime, depthTested);
            DrawArrow(position, {position.x, position.y + length, position.z}, length * 0.1f, DebugColor::Green(),
                      lifetime, depthTested);
            DrawArrow(position, {position.x, position.y, position.z + length}, length * 0.1f, DebugColor::Blue(),
                      lifetime, depthTested);
        }

        // ========================================================================
        // Frame management
        // ========================================================================

        /**
     * @brief Advance timers, expire old shapes, and prepare for next frame
     * @param deltaTime Elapsed time since last frame (seconds)
     */
        void Flush(float deltaTime)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Remove expired persistent shapes
            for (auto it = m_requests.begin(); it != m_requests.end();)
            {
                if (it->remainingTime > 0.0f)
                {
                    it->remainingTime -= deltaTime;
                    if (it->remainingTime <= 0.0f)
                    {
                        it = m_requests.erase(it);
                        continue;
                    }
                }
                else
                {
                    // Single-frame shapes: mark for removal
                    it = m_requests.erase(it);
                    continue;
                }
                ++it;
            }
        }

        /**
     * @brief Get all active draw requests for rendering
     * @return Const reference to the request list
     */
        const std::vector<DebugDrawRequest>& GetRequests() const { return m_requests; }

        /** @brief Clear all pending draw requests */
        void Clear()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_requests.clear();
        }

        /** @brief Check if debug drawing is enabled */
        bool IsEnabled() const { return m_enabled; }

        /** @brief Enable or disable debug drawing */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        /** @brief Toggle debug drawing */
        void Toggle() { m_enabled = !m_enabled; }

        /** @brief Get number of active draw requests */
        size_t GetRequestCount() const { return m_requests.size(); }

        // ========================================================================
        // Physics Debug Heatmap
        // ========================================================================

        /**
         * @brief Draw a grid-based heatmap of collision pair density
         *
         * Renders a flat grid on the XZ plane where each cell is colored
         * based on the number of collision pairs in that region. Colors
         * gradient from green (0 collisions) through yellow to red (hot).
         *
         * @param center       World-space center of the heatmap grid
         * @param cellSize     Size of each grid cell in world units
         * @param gridExtent   Number of cells along each axis (total grid = 2*gridExtent+1)
         * @param densityData  Flat array of collision counts per cell, row-major.
         *                     Size must be (2*gridExtent+1) * (2*gridExtent+1).
         * @param maxDensity   Density value that maps to full red. Values above are clamped.
         * @param yOffset      Y offset above the grid center for the overlay
         * @param lifetime     Display duration (0 = single frame)
         */
        void DrawPhysicsHeatmap(const DirectX::XMFLOAT3& center, float cellSize, int gridExtent,
                                const std::vector<uint32_t>& densityData, uint32_t maxDensity, float yOffset = 0.1f,
                                float lifetime = 0.0f)
        {
            if (!m_enabled || maxDensity == 0)
                return;

            int side = 2 * gridExtent + 1;
            if (static_cast<int>(densityData.size()) != side * side)
                return;

            for (int row = 0; row < side; ++row)
            {
                for (int col = 0; col < side; ++col)
                {
                    const size_t densityIndex =
                        static_cast<size_t>(row) * static_cast<size_t>(side) + static_cast<size_t>(col);
                    uint32_t density = densityData[densityIndex];
                    if (density == 0)
                        continue;

                    // Normalized intensity [0, 1]
                    float t = static_cast<float>(std::min(density, maxDensity)) / static_cast<float>(maxDensity);

                    // Green -> Yellow -> Red gradient
                    DebugColor cellColor;
                    if (t < 0.5f)
                    {
                        // Green to Yellow
                        float s = t * 2.0f;
                        cellColor = {s, 1.0f, 0.0f, 0.6f};
                    }
                    else
                    {
                        // Yellow to Red
                        float s = (t - 0.5f) * 2.0f;
                        cellColor = {1.0f, 1.0f - s, 0.0f, 0.6f};
                    }

                    // Compute cell corners on the XZ plane
                    float cx = center.x + (col - gridExtent) * cellSize;
                    float cz = center.z + (row - gridExtent) * cellSize;
                    float y = center.y + yOffset;

                    DirectX::XMFLOAT3 p0{cx, y, cz};
                    DirectX::XMFLOAT3 p1{cx + cellSize, y, cz};
                    DirectX::XMFLOAT3 p2{cx + cellSize, y, cz + cellSize};
                    DirectX::XMFLOAT3 p3{cx, y, cz + cellSize};

                    // Draw the cell as four line segments (wireframe quad)
                    DrawLine(p0, p1, cellColor, lifetime, 2.0f, false);
                    DrawLine(p1, p2, cellColor, lifetime, 2.0f, false);
                    DrawLine(p2, p3, cellColor, lifetime, 2.0f, false);
                    DrawLine(p3, p0, cellColor, lifetime, 2.0f, false);
                }
            }
        }

      private:
        DebugDrawManager() = default;
        DebugDrawManager(const DebugDrawManager&) = delete;
        DebugDrawManager& operator=(const DebugDrawManager&) = delete;

        void Submit(DebugDrawRequest& req)
        {
            if (!m_enabled)
                return;
            std::lock_guard<std::mutex> lock(m_mutex);
            m_requests.push_back(std::move(req));
        }

        std::vector<DebugDrawRequest> m_requests;
        std::mutex m_mutex;
        bool m_enabled = true;
    };

} // namespace Spark

// ============================================================================
// Convenience macros -- compiled out in Release unless SPARK_DEBUG_DRAW_ALWAYS
// ============================================================================

#if defined(_DEBUG) || defined(DEBUG) || defined(SPARK_DEBUG_DRAW_ALWAYS)
#define DEBUG_DRAW_LINE(start, end, color) Spark::DebugDrawManager::GetInstance().DrawLine(start, end, color)
#define DEBUG_DRAW_LINE_T(start, end, color, time)                                                                     \
    Spark::DebugDrawManager::GetInstance().DrawLine(start, end, color, time)
#define DEBUG_DRAW_RAY(origin, dir, len, color) Spark::DebugDrawManager::GetInstance().DrawRay(origin, dir, len, color)
#define DEBUG_DRAW_RAY_T(origin, dir, len, color, time)                                                                \
    Spark::DebugDrawManager::GetInstance().DrawRay(origin, dir, len, color, time)
#define DEBUG_DRAW_ARROW(start, end, headSize, color)                                                                  \
    Spark::DebugDrawManager::GetInstance().DrawArrow(start, end, headSize, color)
#define DEBUG_DRAW_SPHERE(center, radius, color)                                                                       \
    Spark::DebugDrawManager::GetInstance().DrawSphere(center, radius, color)
#define DEBUG_DRAW_SPHERE_T(center, radius, color, time)                                                               \
    Spark::DebugDrawManager::GetInstance().DrawSphere(center, radius, color, time)
#define DEBUG_DRAW_AABB(mn, mx, color) Spark::DebugDrawManager::GetInstance().DrawAABB(mn, mx, color)
#define DEBUG_DRAW_AABB_T(mn, mx, color, time) Spark::DebugDrawManager::GetInstance().DrawAABB(mn, mx, color, time)
#define DEBUG_DRAW_CAPSULE(base, radius, height, color)                                                                \
    Spark::DebugDrawManager::GetInstance().DrawCapsule(base, radius, height, color)
#define DEBUG_DRAW_CROSS(pos, size, color) Spark::DebugDrawManager::GetInstance().DrawCross(pos, size, color)
#define DEBUG_DRAW_CONE(apex, dir, height, angle, color)                                                               \
    Spark::DebugDrawManager::GetInstance().DrawCone(apex, dir, height, angle, color)
#define DEBUG_DRAW_TEXT3D(pos, text, color) Spark::DebugDrawManager::GetInstance().DrawText3D(pos, text, color)
#define DEBUG_DRAW_GRID(center, cellSize, cellCount, color)                                                            \
    Spark::DebugDrawManager::GetInstance().DrawGrid(center, cellSize, cellCount, color)
#define DEBUG_DRAW_AXES(pos, length) Spark::DebugDrawManager::GetInstance().DrawAxes(pos, length)
#else
#define DEBUG_DRAW_LINE(start, end, color) ((void)0)
#define DEBUG_DRAW_LINE_T(start, end, color, time) ((void)0)
#define DEBUG_DRAW_RAY(origin, dir, len, color) ((void)0)
#define DEBUG_DRAW_RAY_T(origin, dir, len, color, time) ((void)0)
#define DEBUG_DRAW_ARROW(start, end, headSize, color) ((void)0)
#define DEBUG_DRAW_SPHERE(center, radius, color) ((void)0)
#define DEBUG_DRAW_SPHERE_T(center, radius, color, time) ((void)0)
#define DEBUG_DRAW_AABB(mn, mx, color) ((void)0)
#define DEBUG_DRAW_AABB_T(mn, mx, color, time) ((void)0)
#define DEBUG_DRAW_CAPSULE(base, radius, height, color) ((void)0)
#define DEBUG_DRAW_CROSS(pos, size, color) ((void)0)
#define DEBUG_DRAW_CONE(apex, dir, height, angle, color) ((void)0)
#define DEBUG_DRAW_TEXT3D(pos, text, color) ((void)0)
#define DEBUG_DRAW_GRID(center, cellSize, cellCount, color) ((void)0)
#define DEBUG_DRAW_AXES(pos, length) ((void)0)
#endif
