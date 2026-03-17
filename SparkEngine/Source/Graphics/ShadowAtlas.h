/**
 * @file ShadowAtlas.h
 * @brief Priority-based shadow map atlas for efficient shadow rendering
 * @author Spark Engine Team
 * @date 2025
 *
 * Manages a single large atlas texture subdivided into tiles for shadow maps.
 * Lights are assigned tiles based on priority (distance, size, visibility).
 * Supports directional, point (6-face cubemap), and spot light shadows.
 *
 * ## Usage
 * @code
 *   ShadowAtlas atlas;
 *   atlas.Initialize(4096);
 *   atlas.BeginFrame();
 *   atlas.RequestTile(lightId, priority, 1024); // Request 1024x1024 tile
 *   auto tile = atlas.GetTile(lightId);
 *   // Render shadow map into tile.viewport
 *   atlas.EndFrame();
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    /// @brief A tile allocation within the shadow atlas
    struct ShadowTile
    {
        uint32_t x = 0;             ///< Tile X offset in atlas (pixels)
        uint32_t y = 0;             ///< Tile Y offset in atlas (pixels)
        uint32_t size = 0;          ///< Tile width/height (always square)
        uint32_t lightId = 0;       ///< Owning light ID
        float priority = 0.0f;      ///< Assignment priority (higher = more important)
        uint32_t lastUsedFrame = 0; ///< Frame when last rendered
        bool active = false;        ///< Currently in use this frame
    };

    /// @brief Shadow atlas metrics for profiling
    struct ShadowAtlasMetrics
    {
        uint32_t atlasSize = 0;
        uint32_t totalTiles = 0;
        uint32_t activeTiles = 0;
        uint32_t wastedPixels = 0;
        float utilization = 0.0f;
    };

    /**
     * @brief Priority-based shadow map atlas manager
     *
     * Subdivides a square depth texture into variable-size tiles.
     * Higher-priority lights get larger tiles. Tiles persist across
     * frames for temporal stability (only re-rendered when stale).
     */
    class ShadowAtlas
    {
      public:
        ShadowAtlas() = default;
        ~ShadowAtlas() = default;

        bool Initialize(uint32_t atlasSize = 4096, uint32_t minTileSize = 256)
        {
            m_atlasSize = atlasSize;
            m_minTileSize = minTileSize;
            m_tiles.clear();
            m_tileMap.clear();
            m_frameIndex = 0;

            // Pre-subdivide atlas into a grid of minimum-size tiles
            uint32_t gridSize = atlasSize / minTileSize;
            m_gridCells.resize(gridSize * gridSize, false);
            m_gridSize = gridSize;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_tiles.clear();
            m_tileMap.clear();
            m_gridCells.clear();
            m_initialized = false;
        }

        void BeginFrame()
        {
            m_frameIndex++;
            // Mark all tiles as inactive; RequestTile re-activates them
            for (auto& tile : m_tiles)
                tile.active = false;
        }

        /// @brief Request a shadow tile for a light. Returns true if allocated.
        bool RequestTile(uint32_t lightId, float priority, uint32_t desiredSize)
        {
            if (!m_initialized)
                return false;

            // Clamp to valid tile sizes (power of 2, within bounds)
            desiredSize = std::clamp(desiredSize, m_minTileSize, m_atlasSize);

            auto it = m_tileMap.find(lightId);
            if (it != m_tileMap.end())
            {
                // Re-activate existing tile
                ShadowTile& tile = m_tiles[it->second];
                tile.active = true;
                tile.priority = priority;
                tile.lastUsedFrame = m_frameIndex;
                return true;
            }

            // Allocate new tile from grid
            uint32_t cellsNeeded = desiredSize / m_minTileSize;
            if (cellsNeeded == 0)
                cellsNeeded = 1;

            // Find a free region in the grid
            for (uint32_t gy = 0; gy + cellsNeeded <= m_gridSize; ++gy)
            {
                for (uint32_t gx = 0; gx + cellsNeeded <= m_gridSize; ++gx)
                {
                    if (IsRegionFree(gx, gy, cellsNeeded))
                    {
                        MarkRegion(gx, gy, cellsNeeded, true);

                        ShadowTile tile;
                        tile.x = gx * m_minTileSize;
                        tile.y = gy * m_minTileSize;
                        tile.size = cellsNeeded * m_minTileSize;
                        tile.lightId = lightId;
                        tile.priority = priority;
                        tile.lastUsedFrame = m_frameIndex;
                        tile.active = true;

                        m_tileMap[lightId] = static_cast<uint32_t>(m_tiles.size());
                        m_tiles.push_back(tile);
                        return true;
                    }
                }
            }

            // Evict lowest-priority inactive tile if atlas is full
            return EvictAndAllocate(lightId, priority, cellsNeeded);
        }

        /// @brief Get the tile for a light (nullptr if not allocated)
        const ShadowTile* GetTile(uint32_t lightId) const
        {
            auto it = m_tileMap.find(lightId);
            if (it == m_tileMap.end())
                return nullptr;
            return &m_tiles[it->second];
        }

        void EndFrame()
        {
            // Evict tiles unused for several frames to free space
            constexpr uint32_t kStaleFrames = 10;
            for (size_t i = 0; i < m_tiles.size(); ++i)
            {
                if (!m_tiles[i].active && m_frameIndex - m_tiles[i].lastUsedFrame > kStaleFrames)
                {
                    FreeTile(static_cast<uint32_t>(i));
                }
            }
        }

        uint32_t GetAtlasSize() const { return m_atlasSize; }
        bool IsInitialized() const { return m_initialized; }

        ShadowAtlasMetrics GetMetrics() const
        {
            ShadowAtlasMetrics m;
            m.atlasSize = m_atlasSize;
            m.totalTiles = static_cast<uint32_t>(m_tiles.size());
            uint32_t usedPixels = 0;
            for (const auto& t : m_tiles)
            {
                if (t.active)
                {
                    m.activeTiles++;
                    usedPixels += t.size * t.size;
                }
            }
            uint32_t totalPixels = m_atlasSize * m_atlasSize;
            m.wastedPixels = totalPixels - usedPixels;
            m.utilization = totalPixels > 0 ? static_cast<float>(usedPixels) / totalPixels : 0.0f;
            return m;
        }

        std::string Console_GetStatus() const
        {
            auto m = GetMetrics();
            return "ShadowAtlas: " + std::to_string(m.atlasSize) + "x" + std::to_string(m.atlasSize) +
                   " tiles=" + std::to_string(m.activeTiles) + "/" + std::to_string(m.totalTiles) +
                   " util=" + std::to_string(static_cast<int>(m.utilization * 100)) + "%\n";
        }

      private:
        bool IsRegionFree(uint32_t gx, uint32_t gy, uint32_t cells) const
        {
            for (uint32_t dy = 0; dy < cells; ++dy)
                for (uint32_t dx = 0; dx < cells; ++dx)
                    if (m_gridCells[(gy + dy) * m_gridSize + (gx + dx)])
                        return false;
            return true;
        }

        void MarkRegion(uint32_t gx, uint32_t gy, uint32_t cells, bool used)
        {
            for (uint32_t dy = 0; dy < cells; ++dy)
                for (uint32_t dx = 0; dx < cells; ++dx)
                    m_gridCells[(gy + dy) * m_gridSize + (gx + dx)] = used;
        }

        void FreeTile(uint32_t index)
        {
            auto& tile = m_tiles[index];
            uint32_t gx = tile.x / m_minTileSize;
            uint32_t gy = tile.y / m_minTileSize;
            uint32_t cells = tile.size / m_minTileSize;
            MarkRegion(gx, gy, cells, false);
            m_tileMap.erase(tile.lightId);
            tile = {};
        }

        bool EvictAndAllocate(uint32_t lightId, float priority, uint32_t cellsNeeded)
        {
            // Find lowest-priority inactive tile large enough to evict
            float lowestPri = priority;
            int evictIdx = -1;
            for (size_t i = 0; i < m_tiles.size(); ++i)
            {
                if (!m_tiles[i].active && m_tiles[i].size >= cellsNeeded * m_minTileSize &&
                    m_tiles[i].priority < lowestPri)
                {
                    lowestPri = m_tiles[i].priority;
                    evictIdx = static_cast<int>(i);
                }
            }
            if (evictIdx < 0)
                return false;

            uint32_t gx = m_tiles[evictIdx].x / m_minTileSize;
            uint32_t gy = m_tiles[evictIdx].y / m_minTileSize;
            FreeTile(static_cast<uint32_t>(evictIdx));

            // Reuse the freed region
            MarkRegion(gx, gy, cellsNeeded, true);
            ShadowTile tile;
            tile.x = gx * m_minTileSize;
            tile.y = gy * m_minTileSize;
            tile.size = cellsNeeded * m_minTileSize;
            tile.lightId = lightId;
            tile.priority = priority;
            tile.lastUsedFrame = m_frameIndex;
            tile.active = true;

            m_tileMap[lightId] = static_cast<uint32_t>(evictIdx);
            m_tiles[evictIdx] = tile;
            return true;
        }

        bool m_initialized = false;
        uint32_t m_atlasSize = 4096;
        uint32_t m_minTileSize = 256;
        uint32_t m_gridSize = 0;
        uint32_t m_frameIndex = 0;

        std::vector<ShadowTile> m_tiles;
        std::unordered_map<uint32_t, uint32_t> m_tileMap; ///< lightId -> tile index
        std::vector<bool> m_gridCells;                    ///< Grid occupancy
    };

} // namespace Spark::Graphics
