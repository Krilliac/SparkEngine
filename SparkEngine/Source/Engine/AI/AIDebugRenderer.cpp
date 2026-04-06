/**
 * @file AIDebugRenderer.cpp
 * @brief AI debug visualization using DebugDraw primitives
 */

#include "AIDebugRenderer.h"
#include "../../Utils/DebugDraw.h"
#include "../../Utils/LogMacros.h"

#include <cmath>

using namespace DirectX;
namespace Spark::AI
{

    AIDebugRenderer& AIDebugRenderer::GetInstance()
    {
        static AIDebugRenderer instance;
        return instance;
    }

    void AIDebugRenderer::Initialize()
    {
        m_initialized = true;
        m_enabled = false;
        m_selectedAgent = 0;
        m_options = DebugDrawOptions{};
    }

    void AIDebugRenderer::Update(float deltaTime)
    {
        (void)deltaTime;
        // When enabled, the caller drives visualization by calling Draw* methods
        // based on the current AI state. This method exists for future per-frame
        // state management (e.g. fade animations, stat accumulation).
    }

    void AIDebugRenderer::Shutdown()
    {
        m_enabled = false;
        m_initialized = false;
        m_selectedAgent = 0;
    }

    // ============================================================================
    // Instance drawing methods
    // ============================================================================

    void AIDebugRenderer::DrawNavMesh(const NavMeshData& navMesh)
    {
        if (!m_enabled || navMesh.triangles.empty() || navMesh.vertices.empty())
            return;

        for (const auto& tri : navMesh.triangles)
        {
            if (tri.indices[0] >= navMesh.vertices.size() || tri.indices[1] >= navMesh.vertices.size() ||
                tri.indices[2] >= navMesh.vertices.size())
                continue;

            const auto& v0 = navMesh.vertices[tri.indices[0]].position;
            const auto& v1 = navMesh.vertices[tri.indices[1]].position;
            const auto& v2 = navMesh.vertices[tri.indices[2]].position;

            XMFLOAT3 p0(v0.x, v0.y + 0.05f, v0.z);
            XMFLOAT3 p1(v1.x, v1.y + 0.05f, v1.z);
            XMFLOAT3 p2(v2.x, v2.y + 0.05f, v2.z);

            DebugColor color{0.0f, 0.8f, 0.8f, m_options.navMeshOpacity};
            if (tri.flags & 0x02)
                color = {1.0f, 0.2f, 0.2f, m_options.navMeshOpacity};
            else if (tri.flags & 0x04)
                color = {0.2f, 0.4f, 1.0f, m_options.navMeshOpacity};

            DEBUG_DRAW_LINE(p0, p1, color);
            DEBUG_DRAW_LINE(p1, p2, color);
            DEBUG_DRAW_LINE(p2, p0, color);
        }
    }

    void AIDebugRenderer::DrawNavMeshPolygon(const std::vector<XMFLOAT3>& vertices, uint32_t color)
    {
        if (!m_enabled || vertices.size() < 3)
            return;

        float r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
        float g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
        float b = static_cast<float>(color & 0xFF) / 255.0f;
        float a = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
        DebugColor dc{r, g, b, a};

        for (size_t i = 0; i < vertices.size(); ++i)
        {
            size_t next = (i + 1) % vertices.size();
            DEBUG_DRAW_LINE(vertices[i], vertices[next], dc);
        }
    }

    void AIDebugRenderer::DrawPath(const std::vector<XMFLOAT3>& waypoints, uint32_t color)
    {
        if (!m_enabled || waypoints.size() < 2)
            return;

        float r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
        float g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
        float b = static_cast<float>(color & 0xFF) / 255.0f;
        DebugColor dc{r, g, b, 1.0f};

        for (size_t i = 0; i < waypoints.size() - 1; ++i)
        {
            XMFLOAT3 from = waypoints[i];
            XMFLOAT3 to = waypoints[i + 1];
            from.y += 0.1f;
            to.y += 0.1f;
            DEBUG_DRAW_LINE(from, to, dc);
        }

        // Draw waypoint markers
        for (const auto& wp : waypoints)
        {
            XMFLOAT3 pos = wp;
            pos.y += 0.1f;
            DEBUG_DRAW_SPHERE(pos, 0.15f, dc);
        }
    }

    void AIDebugRenderer::DrawPerceptionCone(const XMFLOAT3& position, const XMFLOAT3& forward, float range,
                                             float halfAngle, uint32_t color)
    {
        if (!m_enabled)
            return;

        XMFLOAT3 tip = position;
        tip.y += 1.0f;

        float r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
        float g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
        float b = static_cast<float>(color & 0xFF) / 255.0f;
        float a = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
        DebugColor dc{r, g, b, a};

        DEBUG_DRAW_CONE(tip, forward, range, halfAngle, dc);
    }

    void AIDebugRenderer::DrawBehaviorTreeState(const XMFLOAT3& worldPos, const std::string& activeNode)
    {
        if (!m_enabled || activeNode.empty())
            return;

        XMFLOAT3 textPos = worldPos;
        textPos.y += 2.5f;

        float r = static_cast<float>((m_options.activeNodeColor >> 16) & 0xFF) / 255.0f;
        float g = static_cast<float>((m_options.activeNodeColor >> 8) & 0xFF) / 255.0f;
        float b = static_cast<float>(m_options.activeNodeColor & 0xFF) / 255.0f;
        DebugColor dc{r, g, b, 1.0f};

        DEBUG_DRAW_CROSS(textPos, 0.2f, dc);
    }

    void AIDebugRenderer::DrawCoverPoints(const std::vector<XMFLOAT3>& positions)
    {
        if (!m_enabled || positions.empty())
            return;

        float r = static_cast<float>((m_options.coverPointColor >> 16) & 0xFF) / 255.0f;
        float g = static_cast<float>((m_options.coverPointColor >> 8) & 0xFF) / 255.0f;
        float b = static_cast<float>(m_options.coverPointColor & 0xFF) / 255.0f;
        DebugColor dc{r, g, b, 1.0f};

        for (const auto& pos : positions)
        {
            DEBUG_DRAW_SPHERE(pos, 0.3f, dc);
        }
    }

    void AIDebugRenderer::DrawFormation(const XMFLOAT3& center, const std::vector<XMFLOAT3>& positions)
    {
        if (!m_enabled || positions.empty())
            return;

        DebugColor lineColor{0.8f, 0.8f, 0.0f, 1.0f};
        DebugColor pointColor{1.0f, 1.0f, 0.0f, 1.0f};

        DEBUG_DRAW_SPHERE(center, 0.25f, pointColor);

        for (const auto& pos : positions)
        {
            DEBUG_DRAW_LINE(center, pos, lineColor);
            DEBUG_DRAW_SPHERE(pos, 0.15f, pointColor);
        }
    }

    // ============================================================================
    // Static convenience methods (backward compatible)
    // ============================================================================

    void AIDebugRenderer::RenderNavMesh(const NavMeshData& navMesh, float alpha)
    {
        (void)alpha;
        if (navMesh.triangles.empty() || navMesh.vertices.empty())
            return;

        SPARK_LOG_ONCE(Spark::LogLevel::Debug, Spark::LogCategory::AI,
                       "Rendering navmesh debug: %zu triangles, %zu vertices", navMesh.triangles.size(),
                       navMesh.vertices.size());

        for (const auto& tri : navMesh.triangles)
        {
            if (tri.indices[0] >= navMesh.vertices.size() || tri.indices[1] >= navMesh.vertices.size() ||
                tri.indices[2] >= navMesh.vertices.size())
                continue;

            const auto& v0 = navMesh.vertices[tri.indices[0]].position;
            const auto& v1 = navMesh.vertices[tri.indices[1]].position;
            const auto& v2 = navMesh.vertices[tri.indices[2]].position;

            XMFLOAT3 p0(v0.x, v0.y + 0.05f, v0.z);
            XMFLOAT3 p1(v1.x, v1.y + 0.05f, v1.z);
            XMFLOAT3 p2(v2.x, v2.y + 0.05f, v2.z);

            DebugColor color{0.0f, 0.8f, 0.8f, 1.0f};
            if (tri.flags & 0x02)
                color = {1.0f, 0.2f, 0.2f, 1.0f};
            else if (tri.flags & 0x04)
                color = {0.2f, 0.4f, 1.0f, 1.0f};

            DEBUG_DRAW_LINE(p0, p1, color);
            DEBUG_DRAW_LINE(p1, p2, color);
            DEBUG_DRAW_LINE(p2, p0, color);
        }
    }

    void AIDebugRenderer::RenderNavMeshBounds(const NavMeshData& navMesh)
    {
        DEBUG_DRAW_AABB(navMesh.boundsMin, navMesh.boundsMax, DebugColor::Yellow());
    }

    void AIDebugRenderer::RenderPath(const std::vector<PathPoint>& path, size_t currentIndex)
    {
        if (path.size() < 2)
            return;

        SPARK_LOG_TRACE(Spark::LogCategory::AI, "Rendering debug path: %zu waypoints, current=%zu", path.size(),
                        currentIndex);

        DebugColor pathColor{0.0f, 1.0f, 0.0f, 1.0f};
        DebugColor activeColor = DebugColor::Yellow();
        DebugColor visitedColor{0.5f, 0.5f, 0.5f, 1.0f};

        for (size_t i = 0; i < path.size() - 1; ++i)
        {
            XMFLOAT3 from = path[i].position;
            XMFLOAT3 to = path[i + 1].position;
            from.y += 0.1f;
            to.y += 0.1f;

            DebugColor segColor = (i < currentIndex) ? visitedColor : pathColor;
            DEBUG_DRAW_LINE(from, to, segColor);
        }

        for (size_t i = 0; i < path.size(); ++i)
        {
            XMFLOAT3 pos = path[i].position;
            pos.y += 0.1f;

            DebugColor color = (i == currentIndex) ? activeColor : pathColor;
            float size = (i == currentIndex) ? 0.3f : 0.15f;
            DEBUG_DRAW_SPHERE(pos, size, color);
        }
    }

    void AIDebugRenderer::RenderPerceptionCone(const XMFLOAT3& position, const XMFLOAT3& direction, float range,
                                               float halfAngle)
    {
        SPARK_LOG_TRACE(Spark::LogCategory::AI, "Rendering perception cone: range=%.1f, halfAngle=%.2f", range,
                        halfAngle);
        XMFLOAT3 tip = position;
        tip.y += 1.0f;

        DebugColor coneColor{1.0f, 0.8f, 0.0f, 0.5f};
        DEBUG_DRAW_CONE(tip, direction, range, halfAngle, coneColor);
    }

    void AIDebugRenderer::RenderAgentMarker(const XMFLOAT3& position, const XMFLOAT3& direction)
    {
        DebugColor agentColor{0.0f, 1.0f, 0.5f, 1.0f};
        DEBUG_DRAW_CROSS(position, 0.5f, agentColor);

        XMFLOAT3 rayOrigin(position.x, position.y + 0.5f, position.z);
        XMFLOAT3 rayDir(direction.x * 1.5f, 0.0f, direction.z * 1.5f);
        DEBUG_DRAW_RAY(rayOrigin, rayDir, 1.5f, agentColor);
    }

    // ============================================================================
    // Console integration
    // ============================================================================

    std::string AIDebugRenderer::Console_GetStatus() const
    {
        std::string status = "AIDebugRenderer: ";
        status += m_enabled ? "ENABLED" : "DISABLED";
        if (m_selectedAgent != 0)
        {
            status += " selectedAgent=" + std::to_string(m_selectedAgent);
        }
        status += " navMesh=" + std::string(m_options.drawNavMesh ? "on" : "off");
        status += " paths=" + std::string(m_options.drawPaths ? "on" : "off");
        status += " perception=" + std::string(m_options.drawPerceptionCones ? "on" : "off");
        status += " btState=" + std::string(m_options.drawBehaviorTreeState ? "on" : "off");
        status += " cover=" + std::string(m_options.drawCoverPoints ? "on" : "off");
        status += " formations=" + std::string(m_options.drawFormations ? "on" : "off");
        return status;
    }

} // namespace Spark::AI
