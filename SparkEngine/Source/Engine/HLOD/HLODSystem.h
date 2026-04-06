/**
 * @file HLODSystem.h
 * @brief Hierarchical LOD and World Partition system
 * @see SeamlessAreaManager.h, WorldOriginSystem.h
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils/LogMacros.h"

namespace Spark::HLOD
{

    /// @brief Minimal 3D vector used within the HLOD/WorldPartition system
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
        float LengthSquared() const { return x * x + y * y + z * z; }
        float Length() const { return std::sqrt(LengthSquared()); }
    };

    /// @brief Unique entity identifier (matches ECS convention)
    using EntityID = uint32_t;

    /// @brief Hierarchical LOD levels from full detail to billboard imposters
    enum class HLODLevel : uint8_t
    {
        LOD0 = 0, ///< Full detail — original source meshes
        LOD1 = 1, ///< Merged clusters — combined geometry, reduced draw calls
        LOD2 = 2, ///< Simplified proxy — decimated mesh
        LOD3 = 3, ///< Imposter/billboard — flat card with baked textures
        Count
    };

    static constexpr uint32_t HLOD_LEVEL_COUNT = static_cast<uint32_t>(HLODLevel::Count);

    /// @brief A group of spatially-close entities merged into a single LOD unit
    struct HLODCluster
    {
        uint32_t clusterId = 0;
        Vec3 boundsMin;                                 ///< AABB minimum corner (world space)
        Vec3 boundsMax;                                 ///< AABB maximum corner (world space)
        Vec3 center;                                    ///< Cluster centroid
        float radius = 0.0f;                            ///< Bounding sphere radius from center
        std::vector<EntityID> sourceEntities;           ///< Original entities in this cluster
        HLODLevel lodLevel = HLODLevel::LOD0;           ///< Current active LOD level
        uint32_t triangleCounts[HLOD_LEVEL_COUNT] = {}; ///< Per-LOD triangle counts
        float screenSizeThreshold = 0.05f;              ///< Screen fraction for LOD switching
    };

    /// @brief Simplified mesh data representing a cluster at a specific LOD
    struct HLODProxy
    {
        uint32_t proxyId = 0;
        uint32_t sourceClusterId = 0; ///< Cluster this proxy represents
        HLODLevel lodLevel = HLODLevel::LOD1;
        uint32_t vertexCount = 0;     ///< Simplified mesh vertex count
        uint32_t indexCount = 0;      ///< Simplified mesh index count
        uint32_t imposterAtlasId = 0; ///< Texture atlas for LOD3 billboards (0 = none)
    };

    /// @brief Configuration for the HLOD build process
    struct HLODBuildSettings
    {
        float clusterSize = 128.0f;
        uint32_t lod1TargetTriangles = 5000;
        uint32_t lod2TargetTriangles = 500;
        uint32_t imposterResolution = 256;
        uint32_t imposterDirections = 8;
        float screenSizeThresholdLOD1 = 0.10f;
        float screenSizeThresholdLOD2 = 0.03f;
        float screenSizeThresholdLOD3 = 0.01f;
    };

    /// @brief Build-time tool for generating HLOD clusters and proxy meshes
    class HLODBuilder
    {
      public:
        /// @brief Spatial information for a source entity
        struct EntityEntry
        {
            EntityID entityId = 0;
            Vec3 position;
            float boundingRadius = 1.0f;
            uint32_t triangleCount = 0;
        };

        /// @brief Build clusters from entities using grid-based spatial grouping
        void BuildClusters(const std::vector<EntityEntry>& entities, const HLODBuildSettings& settings)
        {
            m_clusters.clear();
            m_proxies.clear();
            m_settings = settings;

            std::unordered_map<uint64_t, std::vector<const EntityEntry*>> grid;
            for (const auto& entity : entities)
            {
                auto cellX = static_cast<int32_t>(std::floor(entity.position.x / settings.clusterSize));
                auto cellZ = static_cast<int32_t>(std::floor(entity.position.z / settings.clusterSize));
                uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cellX)) << 32) |
                               static_cast<uint64_t>(static_cast<uint32_t>(cellZ));
                grid[key].push_back(&entity);
            }

            uint32_t nextClusterId = 1;
            for (const auto& [key, cellEntities] : grid)
            {
                HLODCluster cluster;
                cluster.clusterId = nextClusterId++;

                Vec3 bMin = cellEntities[0]->position;
                Vec3 bMax = cellEntities[0]->position;
                uint32_t totalTris = 0;

                for (const auto* e : cellEntities)
                {
                    cluster.sourceEntities.push_back(e->entityId);
                    bMin.x = std::min(bMin.x, e->position.x - e->boundingRadius);
                    bMin.y = std::min(bMin.y, e->position.y - e->boundingRadius);
                    bMin.z = std::min(bMin.z, e->position.z - e->boundingRadius);
                    bMax.x = std::max(bMax.x, e->position.x + e->boundingRadius);
                    bMax.y = std::max(bMax.y, e->position.y + e->boundingRadius);
                    bMax.z = std::max(bMax.z, e->position.z + e->boundingRadius);
                    totalTris += e->triangleCount;
                }

                cluster.boundsMin = bMin;
                cluster.boundsMax = bMax;
                cluster.center = (bMin + bMax) * 0.5f;
                cluster.radius = (bMax - bMin).Length() * 0.5f;
                cluster.triangleCounts[0] = totalTris;
                cluster.triangleCounts[1] = std::min(totalTris, settings.lod1TargetTriangles);
                cluster.triangleCounts[2] = std::min(totalTris, settings.lod2TargetTriangles);
                cluster.triangleCounts[3] = 2; // Billboard quad
                cluster.screenSizeThreshold = settings.screenSizeThresholdLOD1;

                m_clusters.push_back(std::move(cluster));
            }
        }

        /// @brief Generate a simplified proxy mesh for a cluster at targetTriCount
        HLODProxy GenerateProxy(const HLODCluster& cluster, uint32_t targetTriCount) const
        {
            HLODProxy proxy;
            proxy.proxyId = cluster.clusterId * 10 + 1;
            proxy.sourceClusterId = cluster.clusterId;
            proxy.lodLevel = (targetTriCount <= m_settings.lod2TargetTriangles) ? HLODLevel::LOD2 : HLODLevel::LOD1;
            proxy.vertexCount = targetTriCount;
            proxy.indexCount = targetTriCount * 3;
            return proxy;
        }

        /// @brief Generate an imposter billboard proxy from multiple capture directions
        HLODProxy GenerateImposter(const HLODCluster& cluster) const
        {
            HLODProxy proxy;
            proxy.proxyId = cluster.clusterId * 10 + 3;
            proxy.sourceClusterId = cluster.clusterId;
            proxy.lodLevel = HLODLevel::LOD3;
            proxy.vertexCount = 4;
            proxy.indexCount = 6;
            proxy.imposterAtlasId = cluster.clusterId;
            return proxy;
        }

        /// @brief Get all built clusters
        const std::vector<HLODCluster>& GetClusters() const { return m_clusters; }

        /// @brief Get all generated proxy meshes
        const std::vector<HLODProxy>& GetProxies() const { return m_proxies; }

      private:
        std::vector<HLODCluster> m_clusters;
        std::vector<HLODProxy> m_proxies;
        HLODBuildSettings m_settings;
    };

    /// @brief Load state for a world partition cell
    enum class CellLoadState : uint8_t
    {
        Unloaded, ///< Not in memory
        Loading,  ///< Async load in progress
        Loaded,   ///< Fully loaded and active
        Unloading ///< Async unload in progress
    };

    /// @brief Axis-aligned world bounds for the partition grid
    struct WorldBounds
    {
        Vec3 min;
        Vec3 max;
    };

    /// @brief A single cell in the world partition grid
    struct WorldPartitionCell
    {
        int32_t cellX = 0;
        int32_t cellZ = 0;
        Vec3 boundsMin;
        Vec3 boundsMax;
        std::vector<EntityID> entities;
        CellLoadState loadState = CellLoadState::Unloaded;
        float priority = 0.0f; ///< Distance-based priority (lower = closer)
    };

    /// @brief Uniform grid that partitions the world into streamable cells
    class WorldPartitionGrid
    {
      public:
        /// @brief Create the grid over the given world bounds with given cell size
        void Initialize(const WorldBounds& bounds, float cellSize)
        {
            if (cellSize <= 0.0f)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Scene,
                                "WorldPartitionGrid::Initialize: cellSize must be > 0 (got {}), defaulting to 256",
                                cellSize);
                cellSize = 256.0f;
            }
            Vec3 worldExtent = bounds.max - bounds.min;
            if (worldExtent.x <= 0.0f || worldExtent.y <= 0.0f || worldExtent.z <= 0.0f)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Scene,
                               "WorldPartitionGrid::Initialize: degenerate world bounds (extent {},{},{})",
                               worldExtent.x, worldExtent.y, worldExtent.z);
            }
            m_bounds = bounds;
            m_cellSize = cellSize;
            m_cells.clear();
            m_entityToCell.clear();
        }

        /// @brief Place an entity into the cell containing the given position
        void AssignEntity(EntityID entityId, const Vec3& position)
        {
            auto [cx, cz] = PositionToCell(position);
            uint64_t key = CellKey(cx, cz);

            auto it = m_cells.find(key);
            if (it == m_cells.end())
            {
                WorldPartitionCell cell;
                cell.cellX = cx;
                cell.cellZ = cz;
                cell.boundsMin = {m_bounds.min.x + cx * m_cellSize, m_bounds.min.y, m_bounds.min.z + cz * m_cellSize};
                cell.boundsMax = {cell.boundsMin.x + m_cellSize, m_bounds.max.y, cell.boundsMin.z + m_cellSize};
                it = m_cells.emplace(key, std::move(cell)).first;
            }

            it->second.entities.push_back(entityId);
            m_entityToCell[entityId] = key;
        }

        /// @brief Remove an entity from its current cell
        void RemoveEntity(EntityID entityId)
        {
            auto it = m_entityToCell.find(entityId);
            if (it == m_entityToCell.end())
            {
                return;
            }

            uint64_t key = it->second;
            m_entityToCell.erase(it);

            auto cellIt = m_cells.find(key);
            if (cellIt != m_cells.end())
            {
                auto& ents = cellIt->second.entities;
                ents.erase(std::remove(ents.begin(), ents.end(), entityId), ents.end());
            }
        }

        /// @brief Update streaming: load nearby cells, unload distant ones
        void UpdateStreaming(const Vec3& viewerPosition, float loadRadius, float unloadRadius)
        {
            m_loadedCells.clear();

            for (auto& [key, cell] : m_cells)
            {
                Vec3 cellCenter = (cell.boundsMin + cell.boundsMax) * 0.5f;
                Vec3 diff = cellCenter - viewerPosition;
                float distXZ = std::sqrt(diff.x * diff.x + diff.z * diff.z);
                cell.priority = distXZ;

                if (distXZ <= loadRadius && cell.loadState == CellLoadState::Unloaded)
                {
                    cell.loadState = CellLoadState::Loaded;
                    SPARK_LOG_DEBUG(Spark::LogCategory::Scene, "WorldPartitionGrid: loaded cell ({},{})", cell.cellX,
                                    cell.cellZ);
                }
                else if (distXZ > unloadRadius && cell.loadState == CellLoadState::Loaded)
                {
                    cell.loadState = CellLoadState::Unloaded;
                    SPARK_LOG_DEBUG(Spark::LogCategory::Scene, "WorldPartitionGrid: unloaded cell ({},{})", cell.cellX,
                                    cell.cellZ);
                }

                if (cell.loadState == CellLoadState::Loaded)
                {
                    m_loadedCells.push_back(&cell);
                }
            }
        }

        /// @brief Get all currently loaded cells
        const std::vector<const WorldPartitionCell*>& GetLoadedCells() const { return m_loadedCells; }

        /// @brief Get the cell at a world-space position (or nullptr if none exists)
        const WorldPartitionCell* GetCellAt(const Vec3& position) const
        {
            auto [cx, cz] = PositionToCell(position);
            auto it = m_cells.find(CellKey(cx, cz));
            return (it != m_cells.end()) ? &it->second : nullptr;
        }

        /// @brief Change the grid cell size (clears all cells)
        void SetCellSize(float cellSize)
        {
            m_cellSize = cellSize;
            m_cells.clear();
            m_entityToCell.clear();
            m_loadedCells.clear();
        }

        /// @brief Get the current cell size
        float GetCellSize() const { return m_cellSize; }

      private:
        std::pair<int32_t, int32_t> PositionToCell(const Vec3& pos) const
        {
            int32_t cx = static_cast<int32_t>(std::floor((pos.x - m_bounds.min.x) / m_cellSize));
            int32_t cz = static_cast<int32_t>(std::floor((pos.z - m_bounds.min.z) / m_cellSize));
            return {cx, cz};
        }

        static uint64_t CellKey(int32_t cx, int32_t cz)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                   static_cast<uint64_t>(static_cast<uint32_t>(cz));
        }

        WorldBounds m_bounds{};
        float m_cellSize = 256.0f;
        std::unordered_map<uint64_t, WorldPartitionCell> m_cells;
        std::unordered_map<EntityID, uint64_t> m_entityToCell;
        std::vector<const WorldPartitionCell*> m_loadedCells;
    };

    /// @brief Singleton managing HLOD generation and world-partition streaming
    class HLODSystem
    {
      public:
        static HLODSystem& GetInstance()
        {
            static HLODSystem instance;
            return instance;
        }

        /// @brief Initialize the HLOD system with default settings
        void Initialize()
        {
            m_loadRadius = 1000.0f;
            m_unloadRadius = 1500.0f;
            m_viewDistance = 2000.0f;
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Scene, "HLODSystem initialized");
        }

        /// @brief Per-frame update — advances streaming and LOD selection
        void Update(const Vec3& viewerPosition)
        {
            if (!m_initialized)
            {
                return;
            }
            m_lastViewerPosition = viewerPosition;
            m_grid.UpdateStreaming(viewerPosition, m_loadRadius, m_unloadRadius);
        }

        /// @brief Shut down and release all HLOD and partition data
        void Shutdown()
        {
            m_clusters.clear();
            m_grid = WorldPartitionGrid{};
            m_initialized = false;
        }

        /// @brief Trigger HLOD cluster generation from the builder's entity data
        void BuildHLOD(const HLODBuildSettings& settings)
        {
            m_buildSettings = settings;
            m_clusters = m_builder.GetClusters();
            SPARK_LOG_INFO(Spark::LogCategory::Scene, "HLODSystem: built HLOD with {} clusters", m_clusters.size());
        }

        /// @brief Get clusters visible from a position within a view distance
        std::vector<const HLODCluster*> GetVisibleClusters(const Vec3& viewerPos, float viewDistance) const
        {
            std::vector<const HLODCluster*> result;
            float viewDistSq = viewDistance * viewDistance;

            for (const auto& cluster : m_clusters)
            {
                Vec3 diff = cluster.center - viewerPos;
                if (diff.LengthSquared() <= viewDistSq)
                {
                    result.push_back(&cluster);
                }
            }
            return result;
        }

        /// @brief Get the HLOD builder for populating entities and triggering builds
        HLODBuilder& GetBuilder() { return m_builder; }
        const HLODBuilder& GetBuilder() const { return m_builder; }

        /// @brief Get the world partition grid for cell management
        WorldPartitionGrid& GetGrid() { return m_grid; }
        const WorldPartitionGrid& GetGrid() const { return m_grid; }

        void SetLoadRadius(float radius) { m_loadRadius = radius; }
        void SetUnloadRadius(float radius) { m_unloadRadius = radius; }
        void SetViewDistance(float distance) { m_viewDistance = distance; }
        float GetLoadRadius() const { return m_loadRadius; }
        float GetUnloadRadius() const { return m_unloadRadius; }
        float GetViewDistance() const { return m_viewDistance; }

        /// @brief Console status string for debugging
        std::string Console_GetStatus() const
        {
            std::string status = "=== HLODSystem Status ===\n";
            status += "Initialized: " + std::string(m_initialized ? "yes" : "no") + "\n";
            status += "Clusters: " + std::to_string(m_clusters.size()) + "\n";
            status += "Grid cell size: " + std::to_string(m_grid.GetCellSize()) + "\n";
            status += "Loaded cells: " + std::to_string(m_grid.GetLoadedCells().size()) + "\n";
            status += "Load radius: " + std::to_string(m_loadRadius) + "\n";
            status += "Unload radius: " + std::to_string(m_unloadRadius) + "\n";
            status += "View distance: " + std::to_string(m_viewDistance) + "\n";
            return status;
        }

      private:
        HLODSystem() = default;

        HLODBuilder m_builder;
        WorldPartitionGrid m_grid;
        std::vector<HLODCluster> m_clusters;
        HLODBuildSettings m_buildSettings;
        Vec3 m_lastViewerPosition;
        float m_loadRadius = 1000.0f;
        float m_unloadRadius = 1500.0f;
        float m_viewDistance = 2000.0f;
        bool m_initialized = false;
    };

} // namespace Spark::HLOD
