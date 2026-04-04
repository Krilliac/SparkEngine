/**
 * @file TestPortalCulling.cpp
 * @brief Tests for portal-based visibility culling (cell graph, portal traversal, frustum narrowing)
 *
 * Uses standalone implementations for cross-platform CI testing without DirectX headers.
 */

#include "TestFramework.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace TestPortal
{

    using CellId = uint32_t;
    constexpr CellId kInvalidCell = UINT32_MAX;
    constexpr int kMaxPortalDepth = 8;
    constexpr int kMaxPortalVerts = 8;

    struct Vec3
    {
        float x, y, z;
    };

    struct AABB
    {
        Vec3 min, max;

        bool Contains(const Vec3& p) const
        {
            return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
        }
    };

    struct ScreenRect
    {
        float minX = -1.0f, minY = -1.0f;
        float maxX = 1.0f, maxY = 1.0f;

        bool IsValid() const { return minX < maxX && minY < maxY; }

        ScreenRect Intersect(const ScreenRect& o) const
        {
            return {std::max(minX, o.minX), std::max(minY, o.minY), std::min(maxX, o.maxX), std::min(maxY, o.maxY)};
        }
    };

    struct Portal
    {
        CellId cellA, cellB;
        std::array<Vec3, kMaxPortalVerts> vertices{};
        int vertexCount = 0;
        Vec3 center{};
        bool enabled = true;
    };

    struct Cell
    {
        AABB bounds;
        std::vector<uint32_t> portalIndices;
    };

    struct PortalCullingStats
    {
        uint32_t totalCells = 0;
        uint32_t visibleCells = 0;
        uint32_t portalsTraversed = 0;
        uint32_t portalsCulled = 0;
        uint32_t maxDepthReached = 0;
    };

    class PortalCullingSystem
    {
      public:
        bool Initialize()
        {
            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            Clear();
            m_initialized = false;
        }

        CellId AddCell(const AABB& bounds)
        {
            CellId id = static_cast<CellId>(m_cells.size());
            Cell cell;
            cell.bounds = bounds;
            m_cells.push_back(std::move(cell));
            return id;
        }

        uint32_t AddPortal(CellId a, CellId b, const Vec3* verts, int count)
        {
            if (a >= m_cells.size() || b >= m_cells.size() || count < 3 || count > kMaxPortalVerts)
                return UINT32_MAX;

            Portal p;
            p.cellA = a;
            p.cellB = b;
            p.vertexCount = count;
            Vec3 c{0, 0, 0};
            for (int i = 0; i < count; ++i)
            {
                p.vertices[i] = verts[i];
                c.x += verts[i].x;
                c.y += verts[i].y;
                c.z += verts[i].z;
            }
            float inv = 1.0f / static_cast<float>(count);
            p.center = {c.x * inv, c.y * inv, c.z * inv};

            uint32_t idx = static_cast<uint32_t>(m_portals.size());
            m_portals.push_back(p);
            m_cells[a].portalIndices.push_back(idx);
            m_cells[b].portalIndices.push_back(idx);
            return idx;
        }

        void SetPortalEnabled(uint32_t idx, bool enabled)
        {
            if (idx < m_portals.size())
                m_portals[idx].enabled = enabled;
        }

        void Clear()
        {
            m_cells.clear();
            m_portals.clear();
            m_visibleCells.clear();
            m_cellVisibility.clear();
            m_cameraCell = kInvalidCell;
            m_stats = {};
        }

        void DetermineVisibility(const Vec3& cameraPos)
        {
            m_stats = {};
            m_stats.totalCells = static_cast<uint32_t>(m_cells.size());
            m_visibleCells.clear();
            m_cellVisibility.assign(m_cells.size(), false);

            m_cameraCell = FindCell(cameraPos);
            if (m_cameraCell == kInvalidCell)
            {
                // Conservative: mark all visible
                for (CellId i = 0; i < static_cast<CellId>(m_cells.size()); ++i)
                {
                    m_cellVisibility[i] = true;
                    m_visibleCells.push_back(i);
                }
                m_stats.visibleCells = static_cast<uint32_t>(m_cells.size());
                return;
            }

            m_cellVisibility[m_cameraCell] = true;
            m_visibleCells.push_back(m_cameraCell);

            ScreenRect full{-1, -1, 1, 1};
            TraversePortals(m_cameraCell, full, 0);
            m_stats.visibleCells = static_cast<uint32_t>(m_visibleCells.size());
        }

        bool IsCellVisible(CellId id) const { return id < m_cellVisibility.size() && m_cellVisibility[id]; }

        CellId FindCell(const Vec3& p) const
        {
            for (CellId i = 0; i < static_cast<CellId>(m_cells.size()); ++i)
            {
                if (m_cells[i].bounds.Contains(p))
                    return i;
            }
            return kInvalidCell;
        }

        const std::vector<CellId>& GetVisibleCells() const { return m_visibleCells; }
        CellId GetCameraCell() const { return m_cameraCell; }
        const PortalCullingStats& GetStats() const { return m_stats; }
        uint32_t GetCellCount() const { return static_cast<uint32_t>(m_cells.size()); }
        uint32_t GetPortalCount() const { return static_cast<uint32_t>(m_portals.size()); }
        bool IsInitialized() const { return m_initialized; }

      private:
        void TraversePortals(CellId cellId, const ScreenRect& rect, int depth)
        {
            if (depth >= kMaxPortalDepth)
                return;
            if (static_cast<uint32_t>(depth) > m_stats.maxDepthReached)
                m_stats.maxDepthReached = static_cast<uint32_t>(depth);

            for (uint32_t portalIdx : m_cells[cellId].portalIndices)
            {
                const Portal& portal = m_portals[portalIdx];
                if (!portal.enabled)
                {
                    ++m_stats.portalsCulled;
                    continue;
                }

                CellId other = (portal.cellA == cellId) ? portal.cellB : portal.cellA;
                if (m_cellVisibility[other])
                    continue;

                ++m_stats.portalsTraversed;

                // Simplified: assume all enabled portals are visible (no projection in test)
                ScreenRect portalRect{-0.5f, -0.5f, 0.5f, 0.5f};
                ScreenRect narrowed = rect.Intersect(portalRect);
                if (!narrowed.IsValid())
                {
                    ++m_stats.portalsCulled;
                    continue;
                }

                m_cellVisibility[other] = true;
                m_visibleCells.push_back(other);
                TraversePortals(other, narrowed, depth + 1);
            }
        }

        bool m_initialized = false;
        std::vector<Cell> m_cells;
        std::vector<Portal> m_portals;
        std::vector<CellId> m_visibleCells;
        std::vector<bool> m_cellVisibility;
        CellId m_cameraCell = kInvalidCell;
        PortalCullingStats m_stats{};
    };

} // namespace TestPortal

// ============================================================================
// Initialization
// ============================================================================

TEST(PortalCulling_InitializeAndShutdown)
{
    TestPortal::PortalCullingSystem sys;
    EXPECT_TRUE(sys.Initialize());
    EXPECT_TRUE(sys.IsInitialized());
    EXPECT_EQ(sys.GetCellCount(), 0u);
    EXPECT_EQ(sys.GetPortalCount(), 0u);
    sys.Shutdown();
    EXPECT_FALSE(sys.IsInitialized());
}

// ============================================================================
// Graph construction
// ============================================================================

TEST(PortalCulling_AddCellsAndPortals)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    auto cellA = sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    auto cellB = sys.AddCell({{10, 0, 0}, {20, 5, 10}});
    EXPECT_EQ(cellA, 0u);
    EXPECT_EQ(cellB, 1u);
    EXPECT_EQ(sys.GetCellCount(), 2u);

    TestPortal::Vec3 verts[] = {{10, 0, 2}, {10, 5, 2}, {10, 5, 8}, {10, 0, 8}};
    uint32_t portalIdx = sys.AddPortal(cellA, cellB, verts, 4);
    EXPECT_NE(portalIdx, UINT32_MAX);
    EXPECT_EQ(sys.GetPortalCount(), 1u);

    sys.Shutdown();
}

TEST(PortalCulling_AddPortalInvalidCells)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();
    sys.AddCell({{0, 0, 0}, {10, 5, 10}});

    TestPortal::Vec3 verts[] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
    uint32_t result = sys.AddPortal(0, 99, verts, 3); // cellB doesn't exist
    EXPECT_EQ(result, UINT32_MAX);

    sys.Shutdown();
}

TEST(PortalCulling_AddPortalTooFewVertices)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();
    sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    sys.AddCell({{10, 0, 0}, {20, 5, 10}});

    TestPortal::Vec3 verts[] = {{0, 0, 0}, {1, 0, 0}};
    uint32_t result = sys.AddPortal(0, 1, verts, 2); // Too few vertices
    EXPECT_EQ(result, UINT32_MAX);

    sys.Shutdown();
}

// ============================================================================
// Camera cell detection
// ============================================================================

TEST(PortalCulling_FindCellContainingCamera)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    sys.AddCell({{10, 0, 0}, {20, 5, 10}});

    EXPECT_EQ(sys.FindCell({5, 2, 5}), 0u);
    EXPECT_EQ(sys.FindCell({15, 2, 5}), 1u);
    EXPECT_EQ(sys.FindCell({25, 2, 5}), TestPortal::kInvalidCell);

    sys.Shutdown();
}

// ============================================================================
// Visibility determination — two rooms connected by a portal
// ============================================================================

TEST(PortalCulling_TwoRoomsConnected)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    auto roomA = sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    auto roomB = sys.AddCell({{10, 0, 0}, {20, 5, 10}});

    TestPortal::Vec3 doorway[] = {{10, 0, 2}, {10, 5, 2}, {10, 5, 8}, {10, 0, 8}};
    sys.AddPortal(roomA, roomB, doorway, 4);

    // Camera in room A — both rooms should be visible
    sys.DetermineVisibility({5, 2, 5});
    EXPECT_EQ(sys.GetCameraCell(), roomA);
    EXPECT_TRUE(sys.IsCellVisible(roomA));
    EXPECT_TRUE(sys.IsCellVisible(roomB));
    EXPECT_EQ(sys.GetStats().visibleCells, 2u);

    sys.Shutdown();
}

TEST(PortalCulling_TwoRoomsDisabledPortal)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    auto roomA = sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    auto roomB = sys.AddCell({{10, 0, 0}, {20, 5, 10}});

    TestPortal::Vec3 doorway[] = {{10, 0, 2}, {10, 5, 2}, {10, 5, 8}, {10, 0, 8}};
    uint32_t portalIdx = sys.AddPortal(roomA, roomB, doorway, 4);

    // Disable the portal (closed door)
    sys.SetPortalEnabled(portalIdx, false);

    sys.DetermineVisibility({5, 2, 5});
    EXPECT_TRUE(sys.IsCellVisible(roomA));
    EXPECT_FALSE(sys.IsCellVisible(roomB)); // Hidden behind closed door
    EXPECT_EQ(sys.GetStats().visibleCells, 1u);
    EXPECT_EQ(sys.GetStats().portalsCulled, 1u);

    sys.Shutdown();
}

// ============================================================================
// Three rooms in a chain
// ============================================================================

TEST(PortalCulling_ThreeRoomsChain)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    auto roomA = sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    auto roomB = sys.AddCell({{10, 0, 0}, {20, 5, 10}});
    auto roomC = sys.AddCell({{20, 0, 0}, {30, 5, 10}});

    TestPortal::Vec3 door1[] = {{10, 0, 2}, {10, 5, 2}, {10, 5, 8}, {10, 0, 8}};
    TestPortal::Vec3 door2[] = {{20, 0, 2}, {20, 5, 2}, {20, 5, 8}, {20, 0, 8}};
    sys.AddPortal(roomA, roomB, door1, 4);
    sys.AddPortal(roomB, roomC, door2, 4);

    // Camera in room A — should see A, B, and C through the chain of portals
    sys.DetermineVisibility({5, 2, 5});
    EXPECT_TRUE(sys.IsCellVisible(roomA));
    EXPECT_TRUE(sys.IsCellVisible(roomB));
    EXPECT_TRUE(sys.IsCellVisible(roomC));
    EXPECT_EQ(sys.GetStats().visibleCells, 3u);

    sys.Shutdown();
}

// ============================================================================
// Camera outside all cells — conservative fallback
// ============================================================================

TEST(PortalCulling_CameraOutsideAllCells)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    sys.AddCell({{10, 0, 0}, {20, 5, 10}});

    // Camera outside both cells
    sys.DetermineVisibility({50, 50, 50});
    EXPECT_EQ(sys.GetCameraCell(), TestPortal::kInvalidCell);
    // Conservative: all cells visible
    EXPECT_EQ(sys.GetStats().visibleCells, 2u);

    sys.Shutdown();
}

// ============================================================================
// Isolated room (no portals)
// ============================================================================

TEST(PortalCulling_IsolatedRoom)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    auto roomA = sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    auto roomB = sys.AddCell({{20, 0, 0}, {30, 5, 10}}); // No portal to A

    sys.DetermineVisibility({5, 2, 5});
    EXPECT_TRUE(sys.IsCellVisible(roomA));
    EXPECT_FALSE(sys.IsCellVisible(roomB)); // No way to see B from A
    EXPECT_EQ(sys.GetStats().visibleCells, 1u);

    sys.Shutdown();
}

// ============================================================================
// ScreenRect intersection
// ============================================================================

TEST(PortalCulling_ScreenRectIntersect)
{
    TestPortal::ScreenRect a{-1, -1, 1, 1};
    TestPortal::ScreenRect b{-0.5f, -0.5f, 0.5f, 0.5f};
    auto r = a.Intersect(b);
    EXPECT_NEAR(r.minX, -0.5f, 0.001f);
    EXPECT_NEAR(r.maxX, 0.5f, 0.001f);
    EXPECT_TRUE(r.IsValid());
}

TEST(PortalCulling_ScreenRectNoOverlap)
{
    TestPortal::ScreenRect a{-1, -1, -0.5f, -0.5f};
    TestPortal::ScreenRect b{0.5f, 0.5f, 1, 1};
    auto r = a.Intersect(b);
    EXPECT_FALSE(r.IsValid());
}

// ============================================================================
// Statistics tracking
// ============================================================================

TEST(PortalCulling_StatsTracking)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    sys.AddCell({{10, 0, 0}, {20, 5, 10}});
    TestPortal::Vec3 door[] = {{10, 0, 2}, {10, 5, 2}, {10, 5, 8}, {10, 0, 8}};
    sys.AddPortal(0, 1, door, 4);

    sys.DetermineVisibility({5, 2, 5});
    auto stats = sys.GetStats();
    EXPECT_EQ(stats.totalCells, 2u);
    EXPECT_EQ(stats.visibleCells, 2u);
    EXPECT_TRUE(stats.portalsTraversed > 0);

    sys.Shutdown();
}

// ============================================================================
// Clear resets everything
// ============================================================================

TEST(PortalCulling_ClearResetsState)
{
    TestPortal::PortalCullingSystem sys;
    sys.Initialize();

    sys.AddCell({{0, 0, 0}, {10, 5, 10}});
    sys.AddCell({{10, 0, 0}, {20, 5, 10}});
    EXPECT_EQ(sys.GetCellCount(), 2u);

    sys.Clear();
    EXPECT_EQ(sys.GetCellCount(), 0u);
    EXPECT_EQ(sys.GetPortalCount(), 0u);
    EXPECT_TRUE(sys.IsInitialized()); // Clear doesn't uninitialize

    sys.Shutdown();
}
