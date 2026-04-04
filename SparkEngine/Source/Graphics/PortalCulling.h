/**
 * @file PortalCulling.h
 * @brief Portal-based visibility culling for indoor/architectural scenes
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a cell-and-portal visibility system where the world is divided
 * into cells (rooms/sectors) connected by portals (doorways/windows). Given
 * the camera's position, the system determines which cell it occupies, then
 * recursively traverses portals to find all visible cells. The camera frustum
 * is progressively clipped to each portal's screen-space bounds, rejecting
 * cells that cannot be seen through any chain of portals.
 *
 * ## Usage
 * @code
 *   Spark::Graphics::PortalCullingSystem portalSys;
 *   portalSys.Initialize();
 *
 *   // Define cells and portals
 *   auto cellA = portalSys.AddCell(boundsA);
 *   auto cellB = portalSys.AddCell(boundsB);
 *   portalSys.AddPortal(cellA, cellB, portalVertices);
 *
 *   // Each frame: determine visible cells
 *   portalSys.DetermineVisibility(cameraPos, viewProjMatrix);
 *   bool visible = portalSys.IsCellVisible(cellB);
 * @endcode
 *
 * @see FrustumCulling, OcclusionCulling, SceneRenderer
 */

#pragma once

#include "../Core/Platform.h"
#include "FrustumCulling.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

    /** @brief Maximum recursion depth when traversing portal chains. */
    constexpr int kMaxPortalDepth = 8;

    /** @brief Maximum vertices in a portal polygon. */
    constexpr int kMaxPortalVertices = 8;

    /** @brief Unique identifier for a cell in the portal graph. */
    using CellId = uint32_t;

    /** @brief Sentinel value for an invalid/unassigned cell. */
    constexpr CellId kInvalidCell = UINT32_MAX;

    // =========================================================================
    // Portal
    // =========================================================================

    /**
     * @brief A convex polygon connecting two cells (e.g. a doorway or window).
     *
     * Portals are defined by a convex polygon (up to kMaxPortalVertices vertices)
     * and reference the two cells they connect. The normal points from cellA
     * toward cellB.
     */
    struct Portal
    {
        std::array<DirectX::XMFLOAT3, kMaxPortalVertices> vertices{}; ///< Polygon vertices (world-space)
        int vertexCount = 0;                                          ///< Actual vertex count (3..kMaxPortalVertices)
        CellId cellA = kInvalidCell;                                  ///< Cell on the front side of the portal
        CellId cellB = kInvalidCell;                                  ///< Cell on the back side of the portal
        DirectX::XMFLOAT3 normal{0, 0, 0};                            ///< Portal plane normal (cellA → cellB)
        DirectX::XMFLOAT3 center{0, 0, 0};                            ///< Centroid of the portal polygon
        bool enabled = true;                                          ///< Can be disabled (e.g. closed door)
    };

    // =========================================================================
    // Cell
    // =========================================================================

    /**
     * @brief A room or sector in the portal graph, defined by an AABB.
     *
     * Each cell has a bounding box and references to its portals.
     */
    struct Cell
    {
        AABB bounds;                         ///< Axis-aligned bounding box of the cell
        std::vector<uint32_t> portalIndices; ///< Indices into the system's portal array
    };

    // =========================================================================
    // PortalCullingStats
    // =========================================================================

    /**
     * @brief Per-frame statistics for portal culling.
     */
    struct PortalCullingStats
    {
        uint32_t totalCells = 0;       ///< Total cells in the graph
        uint32_t visibleCells = 0;     ///< Cells determined visible this frame
        uint32_t portalsTraversed = 0; ///< Portal traversals performed
        uint32_t portalsCulled = 0;    ///< Portals rejected (outside reduced frustum)
        uint32_t maxDepthReached = 0;  ///< Deepest recursion level reached
    };

    // =========================================================================
    // Screen-space rect for frustum reduction
    // =========================================================================

    /**
     * @brief 2D screen-space bounding rectangle used for frustum narrowing.
     *
     * When the camera looks through a portal, the visible region narrows to
     * the portal's screen-space projection. This rect tracks that narrowing.
     */
    struct ScreenRect
    {
        float minX = -1.0f;
        float minY = -1.0f;
        float maxX = 1.0f;
        float maxY = 1.0f;

        /** @brief Check if this rect has any area. */
        bool IsValid() const { return minX < maxX && minY < maxY; }

        /** @brief Intersect with another rect, returning the overlap. */
        ScreenRect Intersect(const ScreenRect& other) const
        {
            return {std::max(minX, other.minX), std::max(minY, other.minY), std::min(maxX, other.maxX),
                    std::min(maxY, other.maxY)};
        }
    };

    // =========================================================================
    // PortalCullingSystem
    // =========================================================================

    /**
     * @class PortalCullingSystem
     * @brief Determines cell visibility by recursively traversing portals.
     *
     * Each frame:
     * 1. DetermineVisibility() locates the camera's cell and walks portals.
     * 2. IsCellVisible() queries visibility for any cell.
     * 3. GetVisibleCells() returns all visible cell IDs.
     */
    class PortalCullingSystem
    {
      public:
        PortalCullingSystem() = default;
        ~PortalCullingSystem() = default;

        /** @brief Initialize the portal culling system. */
        bool Initialize();

        /** @brief Release all cells and portals. */
        void Shutdown();

        // ====================================================================
        // Graph construction
        // ====================================================================

        /**
         * @brief Add a cell (room) to the portal graph.
         * @param bounds  AABB of the cell volume.
         * @return The new cell's ID.
         */
        CellId AddCell(const AABB& bounds);

        /**
         * @brief Add a portal connecting two cells.
         * @param cellA     Cell on the front side.
         * @param cellB     Cell on the back side.
         * @param vertices  Convex polygon vertices (world-space, max kMaxPortalVertices).
         * @param count     Number of vertices.
         * @return Index of the new portal, or UINT32_MAX on error.
         */
        uint32_t AddPortal(CellId cellA, CellId cellB, const DirectX::XMFLOAT3* vertices, int count);

        /**
         * @brief Enable or disable a portal (e.g. when a door opens/closes).
         * @param portalIndex  Index returned by AddPortal().
         * @param enabled      True to allow traversal.
         */
        void SetPortalEnabled(uint32_t portalIndex, bool enabled);

        /** @brief Remove all cells and portals. */
        void Clear();

        // ====================================================================
        // Per-frame visibility
        // ====================================================================

        /**
         * @brief Determine which cells are visible from the camera.
         *
         * Finds the cell containing cameraPos, then recursively walks through
         * visible portals, narrowing the screen-space rect at each step.
         *
         * @param cameraPos     Camera world position.
         * @param viewProjMatrix Combined view-projection matrix.
         */
        void DetermineVisibility(const DirectX::XMFLOAT3& cameraPos, const DirectX::XMFLOAT4X4& viewProjMatrix);

        /** @brief Check if a cell was visible this frame. */
        bool IsCellVisible(CellId cellId) const;

        /** @brief Get all visible cell IDs from the last DetermineVisibility call. */
        const std::vector<CellId>& GetVisibleCells() const { return m_visibleCells; }

        /** @brief Get the cell the camera is currently in (kInvalidCell if outside all cells). */
        CellId GetCameraCell() const { return m_cameraCell; }

        /** @brief Get per-frame statistics. */
        const PortalCullingStats& GetStats() const { return m_stats; }

        // ====================================================================
        // Accessors
        // ====================================================================

        /** @brief Get total cell count. */
        uint32_t GetCellCount() const { return static_cast<uint32_t>(m_cells.size()); }

        /** @brief Get total portal count. */
        uint32_t GetPortalCount() const { return static_cast<uint32_t>(m_portals.size()); }

        /** @brief Check if the system is initialized. */
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief Find which cell contains a point (brute-force AABB test).
         * @param point  World-space point.
         * @return CellId or kInvalidCell.
         */
        CellId FindCell(const DirectX::XMFLOAT3& point) const;

      private:
        /**
         * @brief Recursively traverse portals from a cell, narrowing visibility.
         * @param cellId       Current cell being explored.
         * @param screenRect   Current visible screen-space region.
         * @param depth        Current recursion depth.
         */
        void TraversePortals(CellId cellId, const ScreenRect& screenRect, int depth);

        /**
         * @brief Project a portal polygon to screen space and compute its bounding rect.
         * @param portal  The portal to project.
         * @param outRect Output screen-space bounding rect.
         * @return True if the portal projects to a valid screen rect (at least partially in front).
         */
        bool ProjectPortalToScreen(const Portal& portal, ScreenRect& outRect) const;

        bool m_initialized = false;
        std::vector<Cell> m_cells;
        std::vector<Portal> m_portals;
        std::vector<CellId> m_visibleCells;
        std::vector<bool> m_cellVisibility; ///< Indexed by CellId for O(1) lookup
        CellId m_cameraCell = kInvalidCell;
        DirectX::XMFLOAT4X4 m_viewProj{};
        PortalCullingStats m_stats{};
    };

} // namespace Spark::Graphics
