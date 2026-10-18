/**
 * @file RTSGridPathfinder.h
 * @brief Deterministic A* over the skirmish map grid, with building footprints as blocked cells
 * @author Spark Engine Team
 * @date 2026
 *
 * The skirmish map is a GRID_SIZE x GRID_SIZE grid of unit cells; cell (x, y) covers [x, x+1) x [y, y+1). Every
 * building blocks the cells under its square footprint. Search costs are integers (10 per orthogonal step, 14 per
 * diagonal step, octile heuristic) and the open list is ordered by (f, h, cell index), a strict total order, so the
 * same grid and endpoints yield the same path on every run, compiler, and standard library. The cell path is then
 * shortened by keeping only the waypoints a straight segment cannot skip; that segment test uses only correctly
 * rounded IEEE-754 single-precision operations.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RTS
{
    class RTSBuildingSystem;

    /// @brief A point a moving unit walks to in a straight line
    struct RTSWaypoint
    {
        float x = 0.0f;
        float y = 0.0f;

        bool operator==(const RTSWaypoint&) const = default;
    };

    /**
     * @brief Grid obstacle map plus deterministic path search for move and attack-move orders
     */
    class RTSGridPathfinder
    {
      public:
        static constexpr int GRID_SIZE = 96; ///< Matches RTSSkirmishSimulation::MAP_SIZE
        static constexpr int CELL_COUNT = GRID_SIZE * GRID_SIZE;
        static constexpr float BUILDING_HALF_EXTENT = 2.0f; ///< Footprint is [pos - 2, pos + 2) on both axes
        /// Upper bound on a planned path: one waypoint per grid cell plus the exact destination.
        static constexpr size_t MAX_WAYPOINTS = static_cast<size_t>(CELL_COUNT) + 1;

        RTSGridPathfinder();

        /** @brief Unblock every cell. */
        void ClearObstacles();

        /** @brief Block the footprint cells of a building centred at (@p centerX, @p centerY). */
        void BlockFootprint(float centerX, float centerY);

        /** @brief Replace the obstacle map with the footprints of every building (complete or not). */
        void RebuildObstacles(const RTSBuildingSystem& buildings);

        /** @return true if the cell is inside the grid and blocked; everything outside the grid is open ground. */
        [[nodiscard]] bool IsBlocked(int cellX, int cellY) const;

        /** @return true if the point lies in a blocked cell. */
        [[nodiscard]] bool IsBlockedAt(float x, float y) const;

        /**
         * @brief Check that a straight segment crosses no blocked cell.
         * @param margin  Distance the segment is widened by on both axes before the test; 0 tests the segment
         *                itself, so a segment running exactly along a footprint edge is clear.
         */
        [[nodiscard]] bool IsSegmentClear(float ax, float ay, float bx, float by, float margin) const;

        /**
         * @brief Plan a route from a unit's position to an order's target.
         *
         * A clear straight line is returned as the single target waypoint. Otherwise the route follows A* over
         * the grid (no diagonal corner cutting). A target inside a footprint is replaced by the centre of the
         * nearest free cell, and an unreachable one by the centre of the reachable cell nearest to it. A unit
         * standing inside a footprint may walk out of it but never into another one.
         * @return The waypoints after the start position, ending at the destination; empty if the unit cannot make
         *         progress towards the target.
         */
        [[nodiscard]] std::vector<RTSWaypoint> FindPath(float startX, float startY, float goalX, float goalY) const;

        /**
         * @brief Check that a planned route is still walkable from (@p startX, @p startY) with margin 0.
         *
         * Used to replan when a structure appears across a route after it was planned.
         */
        [[nodiscard]] bool IsRouteClear(float startX, float startY, const std::vector<RTSWaypoint>& route) const;

        /// Widening used while shortening a path, so walking a leg never drifts into a footprint.
        static constexpr float PLANNING_MARGIN = 1.0f / 128.0f;

      private:
        [[nodiscard]] bool IsBlockedIndex(int cell) const;

        std::vector<uint8_t> m_blocked; ///< CELL_COUNT entries, row-major (index = y * GRID_SIZE + x)
    };

} // namespace RTS
