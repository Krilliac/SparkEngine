#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file PortalCulling.cpp
 * @brief Portal-based visibility culling — cell graph traversal and frustum narrowing
 *
 * Implements the PortalCullingSystem: builds a cell-and-portal graph, locates
 * the camera's cell each frame, and recursively walks through visible portals
 * to determine which cells can be seen. Each portal narrows the visible
 * screen-space region, quickly rejecting rooms behind walls or closed doors.
 */

#include "PortalCulling.h"
#include "../Utils/Validate.h"
#include <algorithm>
#include <cmath>

namespace Spark::Graphics
{

    // =========================================================================
    // Initialize / Shutdown
    // =========================================================================

    bool PortalCullingSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "PortalCullingSystem initialized");
        m_initialized = true;
        return true;
    }

    void PortalCullingSystem::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        Clear();
        m_initialized = false;
    }

    // =========================================================================
    // Graph construction
    // =========================================================================

    CellId PortalCullingSystem::AddCell(const AABB& bounds)
    {
        CellId id = static_cast<CellId>(m_cells.size());
        Cell cell;
        cell.bounds = bounds;
        m_cells.push_back(std::move(cell));
        return id;
    }

    uint32_t PortalCullingSystem::AddPortal(CellId cellA, CellId cellB, const DirectX::XMFLOAT3* vertices, int count)
    {
        if (cellA >= m_cells.size() || cellB >= m_cells.size())
        {
            return UINT32_MAX;
        }
        if (count < 3 || count > kMaxPortalVertices || vertices == nullptr)
        {
            return UINT32_MAX;
        }

        Portal portal;
        portal.cellA = cellA;
        portal.cellB = cellB;
        portal.vertexCount = count;

        // Copy vertices and compute centroid
        DirectX::XMFLOAT3 centroid{0, 0, 0};
        for (int i = 0; i < count; ++i)
        {
            portal.vertices[i] = vertices[i];
            centroid.x += vertices[i].x;
            centroid.y += vertices[i].y;
            centroid.z += vertices[i].z;
        }
        float invCount = 1.0f / static_cast<float>(count);
        portal.center = {centroid.x * invCount, centroid.y * invCount, centroid.z * invCount};

        // Compute normal from first two edges (Newell's method for robustness)
        DirectX::XMFLOAT3 normal{0, 0, 0};
        for (int i = 0; i < count; ++i)
        {
            const auto& cur = vertices[i];
            const auto& next = vertices[(i + 1) % count];
            normal.x += (cur.y - next.y) * (cur.z + next.z);
            normal.y += (cur.z - next.z) * (cur.x + next.x);
            normal.z += (cur.x - next.x) * (cur.y + next.y);
        }
        float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (len > 0.0f)
        {
            portal.normal = {normal.x / len, normal.y / len, normal.z / len};
        }

        uint32_t portalIndex = static_cast<uint32_t>(m_portals.size());
        m_portals.push_back(portal);

        // Register portal with both cells
        m_cells[cellA].portalIndices.push_back(portalIndex);
        m_cells[cellB].portalIndices.push_back(portalIndex);

        return portalIndex;
    }

    void PortalCullingSystem::SetPortalEnabled(uint32_t portalIndex, bool enabled)
    {
        if (portalIndex < m_portals.size())
        {
            m_portals[portalIndex].enabled = enabled;
        }
    }

    void PortalCullingSystem::Clear()
    {
        m_cells.clear();
        m_portals.clear();
        m_visibleCells.clear();
        m_cellVisibility.clear();
        m_cameraCell = kInvalidCell;
        m_stats = {};
    }

    // =========================================================================
    // Per-frame visibility
    // =========================================================================

    void PortalCullingSystem::DetermineVisibility(const DirectX::XMFLOAT3& cameraPos,
                                                  const DirectX::XMFLOAT4X4& viewProjMatrix)
    {
        m_stats = {};
        m_stats.totalCells = static_cast<uint32_t>(m_cells.size());
        m_visibleCells.clear();
        m_cellVisibility.assign(m_cells.size(), false);
        m_viewProj = viewProjMatrix;

        // Find camera cell
        m_cameraCell = FindCell(cameraPos);
        if (m_cameraCell == kInvalidCell)
        {
            // Camera outside all cells — mark all visible (conservative fallback)
            for (CellId i = 0; i < static_cast<CellId>(m_cells.size()); ++i)
            {
                m_cellVisibility[i] = true;
                m_visibleCells.push_back(i);
            }
            m_stats.visibleCells = static_cast<uint32_t>(m_cells.size());
            return;
        }

        // Camera cell is always visible
        m_cellVisibility[m_cameraCell] = true;
        m_visibleCells.push_back(m_cameraCell);

        // Full screen rect — start with entire view
        ScreenRect fullScreen{-1.0f, -1.0f, 1.0f, 1.0f};
        TraversePortals(m_cameraCell, fullScreen, 0);

        m_stats.visibleCells = static_cast<uint32_t>(m_visibleCells.size());
    }

    bool PortalCullingSystem::IsCellVisible(CellId cellId) const
    {
        if (cellId >= m_cellVisibility.size())
        {
            return false;
        }
        return m_cellVisibility[cellId];
    }

    // =========================================================================
    // Private helpers
    // =========================================================================

    CellId PortalCullingSystem::FindCell(const DirectX::XMFLOAT3& point) const
    {
        for (CellId i = 0; i < static_cast<CellId>(m_cells.size()); ++i)
        {
            const auto& b = m_cells[i].bounds;
            if (point.x >= b.min.x && point.x <= b.max.x && point.y >= b.min.y && point.y <= b.max.y &&
                point.z >= b.min.z && point.z <= b.max.z)
            {
                return i;
            }
        }
        return kInvalidCell;
    }

    void PortalCullingSystem::TraversePortals(CellId cellId, const ScreenRect& screenRect, int depth)
    {
        if (depth >= kMaxPortalDepth)
        {
            return;
        }
        if (static_cast<uint32_t>(depth) > m_stats.maxDepthReached)
        {
            m_stats.maxDepthReached = static_cast<uint32_t>(depth);
        }

        const Cell& cell = m_cells[cellId];

        for (uint32_t portalIdx : cell.portalIndices)
        {
            const Portal& portal = m_portals[portalIdx];
            if (!portal.enabled)
            {
                ++m_stats.portalsCulled;
                continue;
            }

            // Determine which cell is on the other side
            CellId otherCell = (portal.cellA == cellId) ? portal.cellB : portal.cellA;
            if (m_cellVisibility[otherCell])
            {
                continue; // Already visited
            }

            ++m_stats.portalsTraversed;

            // Project portal to screen space
            ScreenRect portalRect;
            if (!ProjectPortalToScreen(portal, portalRect))
            {
                ++m_stats.portalsCulled;
                continue; // Portal entirely behind camera or degenerate
            }

            // Intersect with current visible region
            ScreenRect narrowed = screenRect.Intersect(portalRect);
            if (!narrowed.IsValid())
            {
                ++m_stats.portalsCulled;
                continue; // Portal outside visible region
            }

            // Mark cell visible and recurse
            m_cellVisibility[otherCell] = true;
            m_visibleCells.push_back(otherCell);
            TraversePortals(otherCell, narrowed, depth + 1);
        }
    }

    bool PortalCullingSystem::ProjectPortalToScreen(const Portal& portal, ScreenRect& outRect) const
    {
        DirectX::XMMATRIX vp = DirectX::XMLoadFloat4x4(&m_viewProj);

        float minX = 1.0f, minY = 1.0f;
        float maxX = -1.0f, maxY = -1.0f;
        int validCount = 0;

        for (int i = 0; i < portal.vertexCount; ++i)
        {
            const auto& v = portal.vertices[i];
            DirectX::XMVECTOR pos = DirectX::XMVectorSet(v.x, v.y, v.z, 1.0f);
            DirectX::XMVECTOR clip = DirectX::XMVector4Transform(pos, vp);

            float w = DirectX::XMVectorGetW(clip);
            if (w <= 0.001f)
            {
                // Vertex behind camera — expand to full screen on that axis
                // (portal straddles the near plane, so it fills the view)
                minX = -1.0f;
                minY = -1.0f;
                maxX = 1.0f;
                maxY = 1.0f;
                outRect = {minX, minY, maxX, maxY};
                return true;
            }

            float ndcX = DirectX::XMVectorGetX(clip) / w;
            float ndcY = DirectX::XMVectorGetY(clip) / w;

            // Clamp to NDC range
            ndcX = std::clamp(ndcX, -1.0f, 1.0f);
            ndcY = std::clamp(ndcY, -1.0f, 1.0f);

            minX = std::min(minX, ndcX);
            minY = std::min(minY, ndcY);
            maxX = std::max(maxX, ndcX);
            maxY = std::max(maxY, ndcY);
            ++validCount;
        }

        if (validCount < 3)
        {
            return false;
        }

        outRect = {minX, minY, maxX, maxY};
        return outRect.IsValid();
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
