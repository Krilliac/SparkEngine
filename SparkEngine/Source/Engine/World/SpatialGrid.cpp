/**
 * @file SpatialGrid.cpp
 * @brief Implementation of cell-based spatial partitioning
 */

#include "SpatialGrid.h"
#include "../ECS/Components.h"

#include <algorithm>
#include <cmath>

namespace Spark::World
{

    // ============================================================================
    // Construction
    // ============================================================================

    SpatialGrid::SpatialGrid(float cellSize, float unloadDelay)
        : m_cellSize(cellSize > 0.0f ? cellSize : 64.0f), m_unloadDelay(unloadDelay > 0.0f ? unloadDelay : 300.0f)
    {
    }

    // ============================================================================
    // Entity Management
    // ============================================================================

    void SpatialGrid::AddEntity(entt::entity entity, float x, float z)
    {
        // Remove first if already tracked (idempotent add)
        RemoveEntity(entity);

        CellCoord coord = WorldToCell(x, z);
        Cell& cell = GetOrCreateCell(coord);
        cell.entities.insert(entity);
        m_entityCells[entity] = coord;
    }

    void SpatialGrid::RemoveEntity(entt::entity entity)
    {
        auto it = m_entityCells.find(entity);
        if (it == m_entityCells.end())
        {
            return;
        }

        CellCoord coord = it->second;
        m_entityCells.erase(it);

        auto cellIt = m_cells.find(coord);
        if (cellIt != m_cells.end())
        {
            cellIt->second.entities.erase(entity);
        }
    }

    void SpatialGrid::MoveEntity(entt::entity entity, float newX, float newZ)
    {
        CellCoord newCoord = WorldToCell(newX, newZ);

        auto it = m_entityCells.find(entity);
        if (it == m_entityCells.end())
        {
            // Entity not tracked yet — add it
            AddEntity(entity, newX, newZ);
            return;
        }

        // Same cell — no work needed
        if (it->second == newCoord)
        {
            return;
        }

        // Remove from old cell
        CellCoord oldCoord = it->second;
        auto oldCellIt = m_cells.find(oldCoord);
        if (oldCellIt != m_cells.end())
        {
            oldCellIt->second.entities.erase(entity);
        }

        // Add to new cell
        Cell& newCell = GetOrCreateCell(newCoord);
        newCell.entities.insert(entity);
        it->second = newCoord;
    }

    // ============================================================================
    // Queries
    // ============================================================================

    CellCoord SpatialGrid::WorldToCell(float x, float z) const
    {
        return CellCoord{static_cast<int32_t>(std::floor(x / m_cellSize)),
                         static_cast<int32_t>(std::floor(z / m_cellSize))};
    }

    Cell* SpatialGrid::GetCell(const CellCoord& coord)
    {
        auto it = m_cells.find(coord);
        return (it != m_cells.end()) ? &it->second : nullptr;
    }

    const Cell* SpatialGrid::GetCell(const CellCoord& coord) const
    {
        auto it = m_cells.find(coord);
        return (it != m_cells.end()) ? &it->second : nullptr;
    }

    void SpatialGrid::VisitNearby(float centerX, float centerZ, float radius, const EntityVisitor& visitor) const
    {
        // Compute the range of cells that the query circle overlaps
        CellCoord minCell = WorldToCell(centerX - radius, centerZ - radius);
        CellCoord maxCell = WorldToCell(centerX + radius, centerZ + radius);

        for (int32_t cx = minCell.x; cx <= maxCell.x; ++cx)
        {
            for (int32_t cz = minCell.z; cz <= maxCell.z; ++cz)
            {
                const Cell* cell = GetCell(CellCoord{cx, cz});
                if (cell == nullptr)
                {
                    continue;
                }

                for (entt::entity entity : cell->entities)
                {
                    visitor(entity);
                }
            }
        }
    }

    void SpatialGrid::VisitCellNeighborhood(const CellCoord& center, int range, const EntityVisitor& visitor) const
    {
        for (int32_t dx = -range; dx <= range; ++dx)
        {
            for (int32_t dz = -range; dz <= range; ++dz)
            {
                const Cell* cell = GetCell(CellCoord{center.x + dx, center.z + dz});
                if (cell == nullptr)
                {
                    continue;
                }

                for (entt::entity entity : cell->entities)
                {
                    visitor(entity);
                }
            }
        }
    }

    std::vector<entt::entity> SpatialGrid::GetEntitiesInRadius(float centerX, float centerZ, float radius) const
    {
        std::vector<entt::entity> result;
        VisitNearby(centerX, centerZ, radius, [&result](entt::entity entity) { result.push_back(entity); });
        return result;
    }

    const Cell* SpatialGrid::GetEntityCell(entt::entity entity) const
    {
        auto it = m_entityCells.find(entity);
        if (it == m_entityCells.end())
        {
            return nullptr;
        }
        return GetCell(it->second);
    }

    // ============================================================================
    // Grid State Management
    // ============================================================================

    void SpatialGrid::UpdateCellStates(const std::vector<std::pair<float, float>>& playerPositions,
                                       float activationRange, float deltaTime)
    {
        // Convert player positions to cell coordinates with the activation range
        // expressed in cell units for faster distance checks
        float cellActivationRange = activationRange / m_cellSize;
        float cellActivationRangeSq = cellActivationRange * cellActivationRange;

        // Precompute player cell coordinates
        std::vector<CellCoord> playerCells;
        playerCells.reserve(playerPositions.size());
        for (const auto& [px, pz] : playerPositions)
        {
            playerCells.push_back(WorldToCell(px, pz));
        }

        for (auto& [coord, cell] : m_cells)
        {
            // Check if any player is within activation range (in cell-space)
            bool nearPlayer = false;
            for (const auto& pc : playerCells)
            {
                float dx = static_cast<float>(coord.x - pc.x);
                float dz = static_cast<float>(coord.z - pc.z);
                if (dx * dx + dz * dz <= cellActivationRangeSq)
                {
                    nearPlayer = true;
                    break;
                }
            }

            if (nearPlayer)
            {
                cell.state = CellState::Active;
                cell.timeSincePlayerNearby = 0.0f;
            }
            else
            {
                cell.timeSincePlayerNearby += deltaTime;

                if (cell.timeSincePlayerNearby >= m_unloadDelay)
                {
                    cell.state = CellState::Unloaded;
                }
                else if (cell.state == CellState::Active)
                {
                    // Transition from Active to Idle when players leave
                    cell.state = CellState::Idle;
                }
            }
        }
    }

    void SpatialGrid::SyncFromECS(::World& world)
    {
        auto view = world.GetEntitiesWith<Transform>();
        for (auto entity : view)
        {
            const auto* transform = world.GetComponent<Transform>(entity);
            if (transform != nullptr)
            {
                MoveEntity(entity, transform->position.x, transform->position.z);
            }
        }
    }

    // ============================================================================
    // Stats & Maintenance
    // ============================================================================

    int SpatialGrid::GetActiveCellCount() const
    {
        int count = 0;
        for (const auto& [coord, cell] : m_cells)
        {
            if (cell.state == CellState::Active)
            {
                ++count;
            }
        }
        return count;
    }

    void SpatialGrid::PruneEmptyCells()
    {
        std::erase_if(m_cells,
                      [](const auto& pair)
                      {
                          const Cell& cell = pair.second;
                          return cell.state == CellState::Unloaded && cell.IsEmpty();
                      });
    }

    void SpatialGrid::Clear()
    {
        m_cells.clear();
        m_entityCells.clear();
    }

    // ============================================================================
    // Private helpers
    // ============================================================================

    Cell& SpatialGrid::GetOrCreateCell(const CellCoord& coord)
    {
        auto [it, inserted] = m_cells.try_emplace(coord);
        if (inserted)
        {
            it->second.coord = coord;
            it->second.state = CellState::Active;
        }
        return it->second;
    }

} // namespace Spark::World
