/**
 * @file ShadowAtlas.cpp
 * @brief Implementation of priority-based shadow map atlas tile allocation
 *
 * Uses a grid-based shelf allocator to pack variable-size shadow map tiles
 * into a single atlas texture. Supports priority-based eviction and
 * frame-based staleness tracking.
 */

#include "ShadowAtlas.h"

namespace Spark::Graphics
{

    bool ShadowAtlas::Initialize(uint32_t atlasSize, uint32_t minTileSize)
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

    void ShadowAtlas::Shutdown()
    {
        m_tiles.clear();
        m_tileMap.clear();
        m_gridCells.clear();
        m_initialized = false;
    }

    void ShadowAtlas::BeginFrame()
    {
        m_frameIndex++;
        // Mark all tiles as inactive; RequestTile re-activates them
        for (auto& tile : m_tiles)
        {
            tile.active = false;
        }
    }

    bool ShadowAtlas::RequestTile(uint32_t lightId, float priority, uint32_t desiredSize)
    {
        if (!m_initialized)
        {
            return false;
        }

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
        {
            cellsNeeded = 1;
        }

        // Find a free region in the grid (shelf-based scan: row by row, left to right)
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

    void ShadowAtlas::ReleaseTile(uint32_t lightId)
    {
        auto it = m_tileMap.find(lightId);
        if (it == m_tileMap.end())
        {
            return;
        }
        FreeTile(it->second);
    }

    const ShadowTile* ShadowAtlas::GetTile(uint32_t lightId) const
    {
        auto it = m_tileMap.find(lightId);
        if (it == m_tileMap.end())
        {
            return nullptr;
        }
        return &m_tiles[it->second];
    }

    void ShadowAtlas::EndFrame()
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

    ShadowAtlasMetrics ShadowAtlas::GetMetrics() const
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

    std::string ShadowAtlas::Console_GetStatus() const
    {
        auto m = GetMetrics();
        return "ShadowAtlas: " + std::to_string(m.atlasSize) + "x" + std::to_string(m.atlasSize) +
               " tiles=" + std::to_string(m.activeTiles) + "/" + std::to_string(m.totalTiles) +
               " util=" + std::to_string(static_cast<int>(m.utilization * 100)) + "%\n";
    }

    // =========================================================================
    // Private helpers
    // =========================================================================

    bool ShadowAtlas::IsRegionFree(uint32_t gx, uint32_t gy, uint32_t cells) const
    {
        for (uint32_t dy = 0; dy < cells; ++dy)
        {
            for (uint32_t dx = 0; dx < cells; ++dx)
            {
                if (m_gridCells[(gy + dy) * m_gridSize + (gx + dx)])
                {
                    return false;
                }
            }
        }
        return true;
    }

    void ShadowAtlas::MarkRegion(uint32_t gx, uint32_t gy, uint32_t cells, bool used)
    {
        for (uint32_t dy = 0; dy < cells; ++dy)
        {
            for (uint32_t dx = 0; dx < cells; ++dx)
            {
                m_gridCells[(gy + dy) * m_gridSize + (gx + dx)] = used;
            }
        }
    }

    void ShadowAtlas::FreeTile(uint32_t index)
    {
        auto& tile = m_tiles[index];
        uint32_t gx = tile.x / m_minTileSize;
        uint32_t gy = tile.y / m_minTileSize;
        uint32_t cells = tile.size / m_minTileSize;
        MarkRegion(gx, gy, cells, false);
        m_tileMap.erase(tile.lightId);
        tile = {};
    }

    bool ShadowAtlas::EvictAndAllocate(uint32_t lightId, float priority, uint32_t cellsNeeded)
    {
        // Find lowest-priority inactive tile large enough to evict
        float lowestPri = priority;
        int evictIdx = -1;
        for (size_t i = 0; i < m_tiles.size(); ++i)
        {
            if (!m_tiles[i].active && m_tiles[i].size >= cellsNeeded * m_minTileSize && m_tiles[i].priority < lowestPri)
            {
                lowestPri = m_tiles[i].priority;
                evictIdx = static_cast<int>(i);
            }
        }
        if (evictIdx < 0)
        {
            return false;
        }

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

} // namespace Spark::Graphics
