/**
 * @file AIDebugRenderer.cpp
 * @brief AI debug visualization using DebugDraw primitives
 */

#include "AIDebugRenderer.h"
#include "../../Utils/DebugDraw.h"

using namespace DirectX;
namespace Spark::AI
{

    void AIDebugRenderer::RenderNavMesh(const NavMeshData& navMesh, float alpha)
    {
        (void)alpha;

        if (navMesh.triangles.empty() || navMesh.vertices.empty())
            return;

        for (const auto& tri : navMesh.triangles)
        {
            if (tri.indices[0] >= navMesh.vertices.size() || tri.indices[1] >= navMesh.vertices.size() ||
                tri.indices[2] >= navMesh.vertices.size())
                continue;

            const auto& v0 = navMesh.vertices[tri.indices[0]].position;
            const auto& v1 = navMesh.vertices[tri.indices[1]].position;
            const auto& v2 = navMesh.vertices[tri.indices[2]].position;

            // Offset slightly upward to prevent z-fighting with ground
            XMFLOAT3 p0(v0.x, v0.y + 0.05f, v0.z);
            XMFLOAT3 p1(v1.x, v1.y + 0.05f, v1.z);
            XMFLOAT3 p2(v2.x, v2.y + 0.05f, v2.z);

            // Color based on triangle flags (walkable=cyan, hazard=red, water=blue)
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

        DebugColor pathColor{0.0f, 1.0f, 0.0f, 1.0f};
        DebugColor activeColor = DebugColor::Yellow();
        DebugColor visitedColor{0.5f, 0.5f, 0.5f, 1.0f};

        for (size_t i = 0; i < path.size() - 1; ++i)
        {
            XMFLOAT3 from = path[i].position;
            XMFLOAT3 to = path[i + 1].position;
            from.y += 0.1f;
            to.y += 0.1f;

            [[maybe_unused]] DebugColor segColor = (i < currentIndex) ? visitedColor : pathColor;
            DEBUG_DRAW_LINE(from, to, segColor);
        }

        for (size_t i = 0; i < path.size(); ++i)
        {
            XMFLOAT3 pos = path[i].position;
            pos.y += 0.1f;

            [[maybe_unused]] DebugColor color = (i == currentIndex) ? activeColor : pathColor;
            float size = (i == currentIndex) ? 0.3f : 0.15f;
            DEBUG_DRAW_SPHERE(pos, size, color);
        }
    }

    void AIDebugRenderer::RenderPerceptionCone(const XMFLOAT3& position, const XMFLOAT3& direction, float range,
                                               float halfAngle)
    {
        XMFLOAT3 tip = position;
        tip.y += 1.0f;

        DebugColor coneColor{1.0f, 0.8f, 0.0f, 0.5f};
        DEBUG_DRAW_CONE(tip, direction, range, halfAngle, coneColor);
    }

    void AIDebugRenderer::RenderAgentMarker(const XMFLOAT3& position, const XMFLOAT3& direction)
    {
        DebugColor agentColor{0.0f, 1.0f, 0.5f, 1.0f};
        DEBUG_DRAW_CROSS(position, 0.5f, agentColor);

        // Draw direction ray from agent
        XMFLOAT3 rayOrigin(position.x, position.y + 0.5f, position.z);
        XMFLOAT3 rayDir(direction.x * 1.5f, 0.0f, direction.z * 1.5f);
        DEBUG_DRAW_RAY(rayOrigin, rayDir, 1.5f, agentColor);
    }

} // namespace Spark::AI
