/**
 * @file SpatialGrid.h
 * @brief Cell-based spatial partitioning for efficient entity queries (TC-inspired)
 * @author Spark Engine Team
 * @date 2026
 *
 * Divides the world into a fixed-size grid of cells. Each cell tracks which
 * entities are within its bounds. Provides O(1) cell lookup and efficient
 * "visit nearby" queries that only iterate relevant cells.
 *
 * Inspired by TrinityCore's Grid/Cell system:
 * - Fixed cell size (configurable, default 64 units)
 * - Entities tracked by cell membership
 * - VisitNearby() iterates only cells within range
 * - Grid activation/deactivation by player proximity
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#else
#include "../../Core/Platform.h"
#endif

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

// Forward-declare the global World class (defined in Components.h)
class World;

namespace Spark::World
{

    // ============================================================================
    // CellCoord — integer grid coordinate identifying a cell
    // ============================================================================

    struct CellCoord
    {
        int32_t x = 0;
        int32_t z = 0;

        bool operator==(const CellCoord& other) const { return x == other.x && z == other.z; }
    };

    struct CellCoordHash
    {
        size_t operator()(const CellCoord& c) const
        {
            // Combine x and z with a hash mixing constant (golden ratio bits)
            size_t h = std::hash<int32_t>{}(c.x);
            h ^= std::hash<int32_t>{}(c.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // ============================================================================
    // Cell — contains entities within a spatial region
    // ============================================================================

    enum class CellState : uint8_t
    {
        Active,  ///< Has players nearby — fully simulated
        Idle,    ///< No players nearby — reduced update rate
        Unloaded ///< Not loaded — no entities tracked
    };

    struct Cell
    {
        CellCoord coord;
        CellState state = CellState::Unloaded;
        std::unordered_set<entt::entity> entities;
        float timeSincePlayerNearby = 0.0f;

        bool IsEmpty() const { return entities.empty(); }
        int GetEntityCount() const { return static_cast<int>(entities.size()); }
    };

    // ============================================================================
    // SpatialGrid — grid-based spatial indexing
    // ============================================================================

    using EntityVisitor = std::function<void(entt::entity)>;
    using CellVisitor = std::function<void(const Cell&)>;

    /**
 * @brief Fixed-size grid spatial index for efficient nearby entity queries
 *
 * Entities are assigned to cells based on their XZ position. Queries such as
 * VisitNearby() only inspect cells overlapping the search radius, making
 * spatial lookups proportional to the query area rather than total entity count.
 */
    class SpatialGrid
    {
      public:
        /**
     * @brief Construct a spatial grid
     * @param cellSize Size of each cell in world units (default 64.0)
     * @param unloadDelay Seconds before an idle cell transitions to Unloaded (default 300.0)
     */
        explicit SpatialGrid(float cellSize = 64.0f, float unloadDelay = 300.0f);

        // -- Entity Management --

        /** @brief Add an entity at the given XZ world position */
        void AddEntity(entt::entity entity, float x, float z);

        /** @brief Remove an entity from the grid entirely */
        void RemoveEntity(entt::entity entity);

        /** @brief Update an entity's position — moves it between cells if needed */
        void MoveEntity(entt::entity entity, float newX, float newZ);

        // -- Queries --

        /** @brief Convert a world-space XZ position to the corresponding cell coordinate */
        CellCoord WorldToCell(float x, float z) const;

        /** @brief Get a cell by coordinate, or nullptr if it doesn't exist */
        Cell* GetCell(const CellCoord& coord);
        const Cell* GetCell(const CellCoord& coord) const;

        /**
     * @brief Visit all entities within radius of a point
     *
     * Only iterates cells that overlap the query radius, then checks
     * exact distance for each entity (requires position lookup).
     * Without exact distance checks, visits all entities in overlapping cells.
     *
     * @param centerX  Center X in world units
     * @param centerZ  Center Z in world units
     * @param radius   Search radius in world units
     * @param visitor  Callback invoked for each entity in overlapping cells
     */
        void VisitNearby(float centerX, float centerZ, float radius, const EntityVisitor& visitor) const;

        /**
     * @brief Visit all entities in a cell and its neighbors
     * @param center The center cell coordinate
     * @param range  How many cells outward to visit (1 = 3x3, 2 = 5x5)
     * @param visitor Callback invoked for each entity found
     */
        void VisitCellNeighborhood(const CellCoord& center, int range, const EntityVisitor& visitor) const;

        /**
     * @brief Get all entities in cells overlapping the given radius (convenience)
     * @return Vector of entity IDs found in the overlapping cells
     */
        std::vector<entt::entity> GetEntitiesInRadius(float centerX, float centerZ, float radius) const;

        /** @brief Get the cell an entity is currently in, or nullptr if not tracked */
        const Cell* GetEntityCell(entt::entity entity) const;

        // -- Grid State Management --

        /**
     * @brief Update cell states based on player proximity
     *
     * Cells near any player position become Active. Cells that lose player
     * proximity transition to Idle, then Unloaded after m_unloadDelay seconds.
     *
     * @param playerPositions  List of (x, z) positions of all players
     * @param activationRange  Distance in world units for a cell to stay Active
     * @param deltaTime        Frame delta time in seconds
     */
        void UpdateCellStates(const std::vector<std::pair<float, float>>& playerPositions, float activationRange,
                              float deltaTime);

        /**
     * @brief Sync entity positions from ECS Transform components
     *
     * Iterates all entities with a Transform, calling MoveEntity for each.
     * Call once per frame to keep the grid in sync with ECS state.
     *
     * @param world The ECS World (global ::World class)
     */
        void SyncFromECS(::World& world);

        // -- Stats --

        int GetTotalCellCount() const { return static_cast<int>(m_cells.size()); }
        int GetActiveCellCount() const;
        int GetTrackedEntityCount() const { return static_cast<int>(m_entityCells.size()); }
        float GetCellSize() const { return m_cellSize; }

        /** @brief Remove empty Unloaded cells to free memory */
        void PruneEmptyCells();

        /** @brief Clear all cells and entity tracking */
        void Clear();

      private:
        Cell& GetOrCreateCell(const CellCoord& coord);

        float m_cellSize;
        float m_unloadDelay;

        /// Cell storage keyed by grid coordinate
        std::unordered_map<CellCoord, Cell, CellCoordHash> m_cells;

        /// Reverse lookup: entity -> which cell it belongs to
        std::unordered_map<entt::entity, CellCoord> m_entityCells;
    };

} // namespace Spark::World
