/**
 * @file RTSGridPathfinder.cpp
 * @brief Deterministic A* over the skirmish map grid, with building footprints as blocked cells
 */

#include "RTSGridPathfinder.h"

#include "Building/RTSBuildingSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <queue>

namespace RTS
{
    namespace
    {
        constexpr int ORTHOGONAL_COST = 10;
        constexpr int DIAGONAL_COST = 14;

        /// Below this horizontal run a segment is treated as vertical and tested over its whole height.
        constexpr float MIN_SLOPE_RUN = 1.0f / 4096.0f;

        struct Neighbor
        {
            int dx;
            int dy;
            int cost;
        };
        /// Fixed expansion order: orthogonal steps first, then diagonals.
        constexpr Neighbor NEIGHBORS[] = {{1, 0, ORTHOGONAL_COST},  {-1, 0, ORTHOGONAL_COST}, {0, 1, ORTHOGONAL_COST},
                                          {0, -1, ORTHOGONAL_COST}, {1, 1, DIAGONAL_COST},    {-1, 1, DIAGONAL_COST},
                                          {1, -1, DIAGONAL_COST},   {-1, -1, DIAGONAL_COST}};

        struct OpenEntry
        {
            int f;
            int h;
            int cell;
        };

        /// Min-heap order on (f, h, cell): a strict total order, so the pop sequence never depends on the heap.
        struct OpenAfter
        {
            bool operator()(const OpenEntry& lhs, const OpenEntry& rhs) const
            {
                if (lhs.f != rhs.f)
                    return lhs.f > rhs.f;
                if (lhs.h != rhs.h)
                    return lhs.h > rhs.h;
                return lhs.cell > rhs.cell;
            }
        };

        /// Grid coordinate of @p value: -1 below the grid (or NaN), GRID_SIZE at or beyond its far edge.
        int CellCoord(float value)
        {
            if (!(value >= 0.0f))
                return -1;
            if (value >= static_cast<float>(RTSGridPathfinder::GRID_SIZE))
                return RTSGridPathfinder::GRID_SIZE;
            return static_cast<int>(value);
        }

        int ClampedCellCoord(float value)
        {
            return std::clamp(CellCoord(value), 0, RTSGridPathfinder::GRID_SIZE - 1);
        }

        int CellX(int cell)
        {
            return cell % RTSGridPathfinder::GRID_SIZE;
        }

        int CellY(int cell)
        {
            return cell / RTSGridPathfinder::GRID_SIZE;
        }

        RTSWaypoint CellCenter(int cell)
        {
            return {static_cast<float>(CellX(cell)) + 0.5f, static_cast<float>(CellY(cell)) + 0.5f};
        }

        /// Octile distance in search-cost units: admissible and consistent for the 10/14 step costs.
        int Heuristic(int cell, int goal)
        {
            const int dx = std::abs(CellX(cell) - CellX(goal));
            const int dy = std::abs(CellY(cell) - CellY(goal));
            return ORTHOGONAL_COST * std::max(dx, dy) + (DIAGONAL_COST - ORTHOGONAL_COST) * std::min(dx, dy);
        }
    } // namespace

    RTSGridPathfinder::RTSGridPathfinder() : m_blocked(CELL_COUNT, 0) {}

    void RTSGridPathfinder::ClearObstacles()
    {
        std::ranges::fill(m_blocked, uint8_t{0});
    }

    void RTSGridPathfinder::BlockFootprint(float centerX, float centerY)
    {
        if (!std::isfinite(centerX) || !std::isfinite(centerY))
            return;

        // Cells whose [x, x+1) span overlaps the half-open footprint [center - extent, center + extent).
        const int firstX = std::max(CellCoord(std::floor(centerX - BUILDING_HALF_EXTENT)), 0);
        const int lastX = std::min(CellCoord(std::ceil(centerX + BUILDING_HALF_EXTENT) - 1.0f), GRID_SIZE - 1);
        const int firstY = std::max(CellCoord(std::floor(centerY - BUILDING_HALF_EXTENT)), 0);
        const int lastY = std::min(CellCoord(std::ceil(centerY + BUILDING_HALF_EXTENT) - 1.0f), GRID_SIZE - 1);
        for (int y = firstY; y <= lastY; ++y)
        {
            for (int x = firstX; x <= lastX; ++x)
                m_blocked[static_cast<size_t>(y * GRID_SIZE + x)] = 1;
        }
    }

    void RTSGridPathfinder::RebuildObstacles(const RTSBuildingSystem& buildings)
    {
        ClearObstacles();
        for (int faction = 0; faction < static_cast<int>(RTSFaction::Count); ++faction)
        {
            for (uint32_t buildingId : buildings.GetBuildingsByFaction(static_cast<RTSFaction>(faction)))
            {
                if (const BuildingData* building = buildings.GetBuilding(buildingId))
                    BlockFootprint(building->posX, building->posY);
            }
        }
    }

    bool RTSGridPathfinder::IsBlocked(int cellX, int cellY) const
    {
        if (cellX < 0 || cellY < 0 || cellX >= GRID_SIZE || cellY >= GRID_SIZE)
            return false;
        return IsBlockedIndex(cellY * GRID_SIZE + cellX);
    }

    bool RTSGridPathfinder::IsBlockedAt(float x, float y) const
    {
        return IsBlocked(CellCoord(x), CellCoord(y));
    }

    bool RTSGridPathfinder::IsBlockedIndex(int cell) const
    {
        return m_blocked[static_cast<size_t>(cell)] != 0;
    }

    bool RTSGridPathfinder::IsSegmentClear(float ax, float ay, float bx, float by, float margin) const
    {
        const float minX = std::min(ax, bx);
        const float maxX = std::max(ax, bx);
        const float minY = std::min(ay, by);
        const float maxY = std::max(ay, by);
        const int firstColumn = std::max(CellCoord(minX - margin), 0);
        const int lastColumn = std::min(CellCoord(maxX + margin), GRID_SIZE - 1);

        const float run = bx - ax;
        const bool vertical = !(std::fabs(run) >= MIN_SLOPE_RUN);
        const float slope = vertical ? 0.0f : (by - ay) / run;

        for (int column = firstColumn; column <= lastColumn; ++column)
        {
            // Height the segment spans while inside this (widened) column; a near-vertical one spans all of it.
            float lowY = minY;
            float highY = maxY;
            if (!vertical)
            {
                const float x0 = std::max(static_cast<float>(column) - margin, minX);
                const float x1 = std::min(static_cast<float>(column + 1) + margin, maxX);
                const float rise0 = (x0 - ax) * slope;
                const float rise1 = (x1 - ax) * slope;
                const float y0 = ay + rise0;
                const float y1 = ay + rise1;
                lowY = std::max(std::min(y0, y1), minY);
                highY = std::min(std::max(y0, y1), maxY);
            }

            const int firstRow = std::max(CellCoord(lowY - margin), 0);
            const int lastRow = std::min(CellCoord(highY + margin), GRID_SIZE - 1);
            for (int row = firstRow; row <= lastRow; ++row)
            {
                if (IsBlockedIndex(row * GRID_SIZE + column))
                    return false;
            }
        }
        return true;
    }

    bool RTSGridPathfinder::IsRouteClear(float startX, float startY, const std::vector<RTSWaypoint>& route) const
    {
        float fromX = startX;
        float fromY = startY;
        for (const RTSWaypoint& waypoint : route)
        {
            if (!IsSegmentClear(fromX, fromY, waypoint.x, waypoint.y, 0.0f))
                return false;
            fromX = waypoint.x;
            fromY = waypoint.y;
        }
        return true;
    }

    std::vector<RTSWaypoint> RTSGridPathfinder::FindPath(float startX, float startY, float goalX, float goalY) const
    {
        if (IsSegmentClear(startX, startY, goalX, goalY, PLANNING_MARGIN))
            return {{goalX, goalY}};

        const int startCell = ClampedCellCoord(startY) * GRID_SIZE + ClampedCellCoord(startX);
        int goalCell = ClampedCellCoord(goalY) * GRID_SIZE + ClampedCellCoord(goalX);
        bool exactGoal = true;

        // A target under a footprint moves to the nearest free cell (squared cell distance, then lowest index).
        if (IsBlockedIndex(goalCell))
        {
            exactGoal = false;
            int bestCell = -1;
            int bestDistance = std::numeric_limits<int>::max();
            const int goalX0 = CellX(goalCell);
            const int goalY0 = CellY(goalCell);
            // A cell at ring r is at least r*r away, so stop once no further ring can beat the best found.
            for (int ring = 1; ring < GRID_SIZE && (bestCell < 0 || ring * ring <= bestDistance); ++ring)
            {
                for (int y = std::max(goalY0 - ring, 0); y <= std::min(goalY0 + ring, GRID_SIZE - 1); ++y)
                {
                    for (int x = std::max(goalX0 - ring, 0); x <= std::min(goalX0 + ring, GRID_SIZE - 1); ++x)
                    {
                        const int dx = x - goalX0;
                        const int dy = y - goalY0;
                        if (std::max(std::abs(dx), std::abs(dy)) != ring)
                            continue;
                        const int cell = y * GRID_SIZE + x;
                        const int distance = dx * dx + dy * dy;
                        if (!IsBlockedIndex(cell) &&
                            (distance < bestDistance || (distance == bestDistance && cell < bestCell)))
                        {
                            bestDistance = distance;
                            bestCell = cell;
                        }
                    }
                }
            }
            if (bestCell < 0)
                return {};
            goalCell = bestCell;
        }

        const RTSWaypoint destination = exactGoal ? RTSWaypoint{goalX, goalY} : CellCenter(goalCell);
        if (startCell == goalCell)
            return {destination};

        std::vector<int> costSoFar(CELL_COUNT, std::numeric_limits<int>::max());
        std::vector<int> parent(CELL_COUNT, -1);
        std::vector<uint8_t> closed(CELL_COUNT, 0);
        std::priority_queue<OpenEntry, std::vector<OpenEntry>, OpenAfter> open;

        const auto passable = [this](int fromCell, int toCell)
        { return !IsBlockedIndex(toCell) || IsBlockedIndex(fromCell); };

        costSoFar[static_cast<size_t>(startCell)] = 0;
        open.push({Heuristic(startCell, goalCell), Heuristic(startCell, goalCell), startCell});
        int nearestCell = startCell;
        int nearestHeuristic = Heuristic(startCell, goalCell);
        bool reached = false;

        while (!open.empty())
        {
            const OpenEntry current = open.top();
            open.pop();
            if (closed[static_cast<size_t>(current.cell)])
                continue;
            closed[static_cast<size_t>(current.cell)] = 1;

            if (current.h < nearestHeuristic || (current.h == nearestHeuristic && current.cell < nearestCell))
            {
                nearestHeuristic = current.h;
                nearestCell = current.cell;
            }
            if (current.cell == goalCell)
            {
                reached = true;
                break;
            }

            const int x = CellX(current.cell);
            const int y = CellY(current.cell);
            for (const Neighbor& neighbor : NEIGHBORS)
            {
                const int nx = x + neighbor.dx;
                const int ny = y + neighbor.dy;
                if (nx < 0 || ny < 0 || nx >= GRID_SIZE || ny >= GRID_SIZE)
                    continue;
                const int next = ny * GRID_SIZE + nx;
                if (closed[static_cast<size_t>(next)] || !passable(current.cell, next))
                    continue;
                // No corner cutting: a diagonal step needs both orthogonal cells it squeezes between.
                if (neighbor.dx != 0 && neighbor.dy != 0 &&
                    (!passable(current.cell, y * GRID_SIZE + nx) || !passable(current.cell, ny * GRID_SIZE + x)))
                    continue;

                const int cost = costSoFar[static_cast<size_t>(current.cell)] + neighbor.cost;
                if (cost < costSoFar[static_cast<size_t>(next)])
                {
                    costSoFar[static_cast<size_t>(next)] = cost;
                    parent[static_cast<size_t>(next)] = current.cell;
                    const int h = Heuristic(next, goalCell);
                    open.push({cost + h, h, next});
                }
            }
        }

        // An enclosed target is approached as closely as the grid allows.
        const int endCell = reached ? goalCell : nearestCell;
        if (endCell == startCell)
            return {};

        std::vector<int> cells;
        for (int cell = endCell; cell != startCell; cell = parent[static_cast<size_t>(cell)])
            cells.push_back(cell);
        std::ranges::reverse(cells);

        std::vector<RTSWaypoint> points;
        points.reserve(cells.size() + 1);
        points.push_back({startX, startY});
        for (int cell : cells)
            points.push_back(CellCenter(cell));
        if (reached)
            points.back() = destination;

        // Keep a waypoint only where the straight (widened) segment from the previous kept point is obstructed.
        // Consecutive grid points are always walkable, so each leg advances at least one point.
        std::vector<RTSWaypoint> route;
        size_t anchor = 0;
        while (anchor + 1 < points.size())
        {
            size_t reach = anchor + 1;
            while (reach + 1 < points.size() && IsSegmentClear(points[anchor].x, points[anchor].y, points[reach + 1].x,
                                                               points[reach + 1].y, PLANNING_MARGIN))
            {
                ++reach;
            }
            route.push_back(points[reach]);
            anchor = reach;
        }
        return route;
    }

} // namespace RTS
