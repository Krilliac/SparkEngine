// TestSpatialGrid.cpp - Tests for cell-based spatial partitioning
// Standalone reimplementation to avoid platform-specific header dependencies

#include "TestFramework.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================================
// Standalone reimplementation of Spark::World::SpatialGrid
// ============================================================================

namespace
{

    struct CellCoord
    {
        int x = 0;
        int z = 0;

        bool operator==(const CellCoord& other) const { return x == other.x && z == other.z; }
    };

    struct CellCoordHash
    {
        size_t operator()(const CellCoord& c) const
        {
            size_t h1 = std::hash<int>{}(c.x);
            size_t h2 = std::hash<int>{}(c.z);
            return h1 ^ (h2 << 16);
        }
    };

    enum class CellState
    {
        Active,
        Idle,
        Unloaded
    };

    using EntityId = uint32_t;

    struct Cell
    {
        CellCoord coord;
        CellState state = CellState::Unloaded;
        std::unordered_set<EntityId> entities;
    };

    class SpatialGrid
    {
      public:
        explicit SpatialGrid(float cellSize) : m_cellSize(cellSize) {}

        CellCoord WorldToCell(float worldX, float worldZ) const
        {
            return {static_cast<int>(std::floor(worldX / m_cellSize)),
                    static_cast<int>(std::floor(worldZ / m_cellSize))};
        }

        void AddEntity(EntityId id, float worldX, float worldZ)
        {
            CellCoord coord = WorldToCell(worldX, worldZ);
            auto& cell = GetOrCreateCell(coord);
            cell.entities.insert(id);
            m_entityPositions[id] = {worldX, worldZ};
            m_entityCells[id] = coord;
            m_totalEntityCount++;
        }

        void RemoveEntity(EntityId id)
        {
            auto it = m_entityCells.find(id);
            if (it == m_entityCells.end())
                return;

            CellCoord coord = it->second;
            auto cellIt = m_cells.find(coord);
            if (cellIt != m_cells.end())
            {
                cellIt->second.entities.erase(id);
            }
            m_entityCells.erase(it);
            m_entityPositions.erase(id);
            m_totalEntityCount--;
        }

        void MoveEntity(EntityId id, float newX, float newZ)
        {
            auto it = m_entityCells.find(id);
            if (it == m_entityCells.end())
                return;

            CellCoord oldCoord = it->second;
            CellCoord newCoord = WorldToCell(newX, newZ);

            if (!(oldCoord == newCoord))
            {
                auto cellIt = m_cells.find(oldCoord);
                if (cellIt != m_cells.end())
                {
                    cellIt->second.entities.erase(id);
                }

                auto& newCell = GetOrCreateCell(newCoord);
                newCell.entities.insert(id);
                m_entityCells[id] = newCoord;
            }

            m_entityPositions[id] = {newX, newZ};
        }

        std::vector<EntityId> GetEntitiesInRadius(float centerX, float centerZ, float radius) const
        {
            std::vector<EntityId> result;
            float radiusSq = radius * radius;

            int minCellX = static_cast<int>(std::floor((centerX - radius) / m_cellSize));
            int maxCellX = static_cast<int>(std::floor((centerX + radius) / m_cellSize));
            int minCellZ = static_cast<int>(std::floor((centerZ - radius) / m_cellSize));
            int maxCellZ = static_cast<int>(std::floor((centerZ + radius) / m_cellSize));

            for (int cx = minCellX; cx <= maxCellX; ++cx)
            {
                for (int cz = minCellZ; cz <= maxCellZ; ++cz)
                {
                    auto it = m_cells.find({cx, cz});
                    if (it == m_cells.end())
                        continue;

                    for (EntityId id : it->second.entities)
                    {
                        auto posIt = m_entityPositions.find(id);
                        if (posIt == m_entityPositions.end())
                            continue;

                        float dx = posIt->second.first - centerX;
                        float dz = posIt->second.second - centerZ;
                        if (dx * dx + dz * dz <= radiusSq)
                        {
                            result.push_back(id);
                        }
                    }
                }
            }

            return result;
        }

        void SetCellState(const CellCoord& coord, CellState state)
        {
            auto& cell = GetOrCreateCell(coord);
            cell.state = state;
        }

        CellState GetCellState(const CellCoord& coord) const
        {
            auto it = m_cells.find(coord);
            if (it == m_cells.end())
                return CellState::Unloaded;
            return it->second.state;
        }

        size_t PruneEmptyCells()
        {
            size_t pruned = 0;
            for (auto it = m_cells.begin(); it != m_cells.end();)
            {
                if (it->second.entities.empty() && it->second.state == CellState::Unloaded)
                {
                    it = m_cells.erase(it);
                    pruned++;
                }
                else
                {
                    ++it;
                }
            }
            return pruned;
        }

        size_t GetTotalEntityCount() const { return m_totalEntityCount; }

        size_t GetCellCount() const { return m_cells.size(); }

        /// Visit all cells in a neighborhood (including center)
        std::vector<CellCoord> GetNeighborhood(const CellCoord& center, int range) const
        {
            std::vector<CellCoord> neighbors;
            for (int dx = -range; dx <= range; ++dx)
            {
                for (int dz = -range; dz <= range; ++dz)
                {
                    neighbors.push_back({center.x + dx, center.z + dz});
                }
            }
            return neighbors;
        }

      private:
        Cell& GetOrCreateCell(const CellCoord& coord)
        {
            auto it = m_cells.find(coord);
            if (it == m_cells.end())
            {
                Cell cell;
                cell.coord = coord;
                auto [inserted, _] = m_cells.emplace(coord, std::move(cell));
                return inserted->second;
            }
            return it->second;
        }

        float m_cellSize;
        std::unordered_map<CellCoord, Cell, CellCoordHash> m_cells;
        std::unordered_map<EntityId, std::pair<float, float>> m_entityPositions;
        std::unordered_map<EntityId, CellCoord> m_entityCells;
        size_t m_totalEntityCount = 0;
    };

} // namespace

// ============================================================================
// CellCoord from world position
// ============================================================================

TEST(SpatialGrid_CellCoordFromPosition)
{
    SpatialGrid grid(64.0f);

    auto c1 = grid.WorldToCell(0.0f, 0.0f);
    EXPECT_EQ(c1.x, 0);
    EXPECT_EQ(c1.z, 0);

    auto c2 = grid.WorldToCell(63.9f, 63.9f);
    EXPECT_EQ(c2.x, 0);
    EXPECT_EQ(c2.z, 0);

    auto c3 = grid.WorldToCell(64.0f, 128.0f);
    EXPECT_EQ(c3.x, 1);
    EXPECT_EQ(c3.z, 2);

    // Negative positions should map to negative cells
    auto c4 = grid.WorldToCell(-1.0f, -1.0f);
    EXPECT_EQ(c4.x, -1);
    EXPECT_EQ(c4.z, -1);

    auto c5 = grid.WorldToCell(-64.0f, -128.0f);
    EXPECT_EQ(c5.x, -1);
    EXPECT_EQ(c5.z, -2);
}

// ============================================================================
// Add / Remove / Move entity
// ============================================================================

TEST(SpatialGrid_AddEntity)
{
    SpatialGrid grid(64.0f);

    grid.AddEntity(1, 10.0f, 20.0f);
    grid.AddEntity(2, 70.0f, 80.0f);

    EXPECT_EQ(grid.GetTotalEntityCount(), 2u);
    EXPECT_EQ(grid.GetCellCount(), 2u);
}

TEST(SpatialGrid_AddMultipleToSameCell)
{
    SpatialGrid grid(64.0f);

    grid.AddEntity(1, 10.0f, 10.0f);
    grid.AddEntity(2, 20.0f, 20.0f);
    grid.AddEntity(3, 30.0f, 30.0f);

    // All within cell (0,0)
    EXPECT_EQ(grid.GetTotalEntityCount(), 3u);
    EXPECT_EQ(grid.GetCellCount(), 1u);
}

TEST(SpatialGrid_RemoveEntity)
{
    SpatialGrid grid(64.0f);

    grid.AddEntity(1, 10.0f, 10.0f);
    grid.AddEntity(2, 20.0f, 20.0f);

    grid.RemoveEntity(1);
    EXPECT_EQ(grid.GetTotalEntityCount(), 1u);

    // Removing nonexistent entity should be safe
    grid.RemoveEntity(999);
    EXPECT_EQ(grid.GetTotalEntityCount(), 1u);
}

TEST(SpatialGrid_MoveEntity_SameCell)
{
    SpatialGrid grid(64.0f);

    grid.AddEntity(1, 10.0f, 10.0f);
    grid.MoveEntity(1, 20.0f, 20.0f);

    // Still in cell (0,0)
    EXPECT_EQ(grid.GetCellCount(), 1u);
    EXPECT_EQ(grid.GetTotalEntityCount(), 1u);
}

TEST(SpatialGrid_MoveEntity_DifferentCell)
{
    SpatialGrid grid(64.0f);

    grid.AddEntity(1, 10.0f, 10.0f);
    grid.MoveEntity(1, 100.0f, 100.0f);

    // Now in cell (1,1), old cell (0,0) still exists but is empty
    EXPECT_EQ(grid.GetCellCount(), 2u);

    auto nearby = grid.GetEntitiesInRadius(100.0f, 100.0f, 10.0f);
    EXPECT_EQ(nearby.size(), 1u);

    auto farAway = grid.GetEntitiesInRadius(10.0f, 10.0f, 5.0f);
    EXPECT_EQ(farAway.size(), 0u);
}

// ============================================================================
// GetEntitiesInRadius
// ============================================================================

TEST(SpatialGrid_GetEntitiesInRadius)
{
    SpatialGrid grid(64.0f);

    grid.AddEntity(1, 0.0f, 0.0f);
    grid.AddEntity(2, 10.0f, 0.0f);
    grid.AddEntity(3, 100.0f, 0.0f);
    grid.AddEntity(4, 200.0f, 200.0f);

    auto result = grid.GetEntitiesInRadius(0.0f, 0.0f, 50.0f);
    EXPECT_EQ(result.size(), 2u);

    // Entity 3 is at 100 — just within radius 101
    auto result2 = grid.GetEntitiesInRadius(0.0f, 0.0f, 101.0f);
    EXPECT_EQ(result2.size(), 3u);
}

TEST(SpatialGrid_GetEntitiesInRadius_Empty)
{
    SpatialGrid grid(64.0f);

    auto result = grid.GetEntitiesInRadius(0.0f, 0.0f, 100.0f);
    EXPECT_EQ(result.size(), 0u);
}

// ============================================================================
// Cell states
// ============================================================================

TEST(SpatialGrid_CellState_DefaultUnloaded)
{
    SpatialGrid grid(64.0f);

    CellCoord coord{0, 0};
    EXPECT_TRUE(grid.GetCellState(coord) == CellState::Unloaded);
}

TEST(SpatialGrid_CellState_SetAndGet)
{
    SpatialGrid grid(64.0f);

    CellCoord coord{1, 2};
    grid.SetCellState(coord, CellState::Active);
    EXPECT_TRUE(grid.GetCellState(coord) == CellState::Active);

    grid.SetCellState(coord, CellState::Idle);
    EXPECT_TRUE(grid.GetCellState(coord) == CellState::Idle);
}

// ============================================================================
// PruneEmptyCells
// ============================================================================

TEST(SpatialGrid_PruneEmptyCells)
{
    SpatialGrid grid(64.0f);

    grid.AddEntity(1, 10.0f, 10.0f);
    grid.AddEntity(2, 100.0f, 100.0f);
    grid.RemoveEntity(1);

    // Cell (0,0) is now empty and Unloaded — should be pruned
    // Cell (1,1) still has entity 2 — should not be pruned
    size_t pruned = grid.PruneEmptyCells();
    EXPECT_EQ(pruned, 1u);
    EXPECT_EQ(grid.GetCellCount(), 1u);
}

TEST(SpatialGrid_PruneEmptyCells_KeepsActiveCells)
{
    SpatialGrid grid(64.0f);

    CellCoord coord{5, 5};
    grid.SetCellState(coord, CellState::Active);

    // Cell is empty but Active — should not be pruned
    size_t pruned = grid.PruneEmptyCells();
    EXPECT_EQ(pruned, 0u);
    EXPECT_EQ(grid.GetCellCount(), 1u);
}

// ============================================================================
// Neighborhood visiting
// ============================================================================

TEST(SpatialGrid_Neighborhood_Range0)
{
    SpatialGrid grid(64.0f);

    auto neighbors = grid.GetNeighborhood({3, 4}, 0);
    EXPECT_EQ(neighbors.size(), 1u);
    EXPECT_TRUE(neighbors[0] == (CellCoord{3, 4}));
}

TEST(SpatialGrid_Neighborhood_Range1)
{
    SpatialGrid grid(64.0f);

    auto neighbors = grid.GetNeighborhood({0, 0}, 1);
    // 3x3 = 9 cells
    EXPECT_EQ(neighbors.size(), 9u);
}

TEST(SpatialGrid_Neighborhood_Range2)
{
    SpatialGrid grid(64.0f);

    auto neighbors = grid.GetNeighborhood({0, 0}, 2);
    // 5x5 = 25 cells
    EXPECT_EQ(neighbors.size(), 25u);
}

// ============================================================================
// Entity count tracking
// ============================================================================

TEST(SpatialGrid_EntityCountTracking)
{
    SpatialGrid grid(64.0f);

    EXPECT_EQ(grid.GetTotalEntityCount(), 0u);

    grid.AddEntity(1, 0.0f, 0.0f);
    EXPECT_EQ(grid.GetTotalEntityCount(), 1u);

    grid.AddEntity(2, 10.0f, 10.0f);
    EXPECT_EQ(grid.GetTotalEntityCount(), 2u);

    grid.RemoveEntity(1);
    EXPECT_EQ(grid.GetTotalEntityCount(), 1u);

    grid.RemoveEntity(2);
    EXPECT_EQ(grid.GetTotalEntityCount(), 0u);
}
