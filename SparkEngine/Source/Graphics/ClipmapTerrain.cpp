/**
 * @file ClipmapTerrain.cpp
 * @brief Implementation of clipmap-based terrain LOD system
 */

#include "ClipmapTerrain.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Spark::Graphics
{

    // ========================================================================
    // Singleton
    // ========================================================================

    ClipmapTerrain& ClipmapTerrain::GetInstance()
    {
        static ClipmapTerrain instance;
        return instance;
    }

    // ========================================================================
    // Initialize
    // ========================================================================

    bool ClipmapTerrain::Initialize(uint32_t numLevels, uint32_t gridSize, float baseCellSize)
    {
        if (m_initialized)
        {
            return true;
        }

        if (numLevels == 0 || gridSize < 2 || baseCellSize <= 0.0f)
        {
            return false;
        }

        m_levels.clear();
        m_levels.reserve(numLevels);

        for (uint32_t i = 0; i < numLevels; ++i)
        {
            ClipmapLevel level;
            level.level = i;
            level.cellSize = baseCellSize * static_cast<float>(1u << i);
            level.gridSize = gridSize;
            level.center = {0.0f, 0.0f, 0.0f};
            level.needsUpdate = true;
            m_levels.push_back(level);
        }

        m_initialized = true;
        return true;
    }

    // ========================================================================
    // Update
    // ========================================================================

    void ClipmapTerrain::Update(const XMFLOAT3& cameraPosition)
    {
        if (!m_initialized)
        {
            return;
        }

        for (auto& level : m_levels)
        {
            // Snap camera position to this level's grid
            float snappedX = SnapToGrid(cameraPosition.x, level.cellSize);
            float snappedZ = SnapToGrid(cameraPosition.z, level.cellSize);

            // Check if the center has moved (needs mesh update)
            if (snappedX != level.center.x || snappedZ != level.center.z)
            {
                level.center.x = snappedX;
                level.center.y = cameraPosition.y;
                level.center.z = snappedZ;
                level.needsUpdate = true;
            }
        }
    }

    // ========================================================================
    // GenerateMesh
    // ========================================================================

    void ClipmapTerrain::GenerateMesh(uint32_t levelIndex, std::vector<float>& vertices,
                                      std::vector<uint32_t>& indices) const
    {
        if (!m_initialized || levelIndex >= m_levels.size())
        {
            return;
        }

        const auto& level = m_levels[levelIndex];
        const uint32_t gridSize = level.gridSize;
        const float cellSize = level.cellSize;
        const float halfExtent = (gridSize * cellSize) * 0.5f;

        // Generate grid vertices: 3 floats per vertex (x, y, z)
        uint32_t vertCount = (gridSize + 1) * (gridSize + 1);
        vertices.clear();
        vertices.reserve(static_cast<size_t>(vertCount) * 3);

        for (uint32_t z = 0; z <= gridSize; ++z)
        {
            for (uint32_t x = 0; x <= gridSize; ++x)
            {
                float worldX = level.center.x - halfExtent + x * cellSize;
                float worldZ = level.center.z - halfExtent + z * cellSize;
                float worldY = SampleHeight(worldX, worldZ);

                vertices.push_back(worldX);
                vertices.push_back(worldY);
                vertices.push_back(worldZ);
            }
        }

        // Generate triangle indices (two triangles per grid cell)
        uint32_t quadCount = gridSize * gridSize;
        indices.clear();
        indices.reserve(static_cast<size_t>(quadCount) * 6);

        uint32_t stride = gridSize + 1;
        for (uint32_t z = 0; z < gridSize; ++z)
        {
            for (uint32_t x = 0; x < gridSize; ++x)
            {
                uint32_t topLeft = z * stride + x;
                uint32_t topRight = topLeft + 1;
                uint32_t bottomLeft = (z + 1) * stride + x;
                uint32_t bottomRight = bottomLeft + 1;

                // Triangle 1
                indices.push_back(topLeft);
                indices.push_back(bottomLeft);
                indices.push_back(topRight);

                // Triangle 2
                indices.push_back(topRight);
                indices.push_back(bottomLeft);
                indices.push_back(bottomRight);
            }
        }
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    uint32_t ClipmapTerrain::GetLevelCount() const
    {
        return static_cast<uint32_t>(m_levels.size());
    }

    const ClipmapLevel& ClipmapTerrain::GetLevel(uint32_t level) const
    {
        static const ClipmapLevel s_empty;
        if (level < m_levels.size())
        {
            return m_levels[level];
        }
        return s_empty;
    }

    // ========================================================================
    // SampleHeight
    // ========================================================================

    float ClipmapTerrain::SampleHeight(float worldX, float worldZ) const
    {
        if (m_heightmap.empty() || m_heightmapWidth == 0 || m_heightmapHeight == 0)
        {
            return 0.0f;
        }

        // Map world coordinates to heightmap UV space.
        // Assume heightmap covers [-halfW, +halfW] x [-halfH, +halfH] with 1 unit per texel.
        float halfW = static_cast<float>(m_heightmapWidth) * 0.5f;
        float halfH = static_cast<float>(m_heightmapHeight) * 0.5f;

        float u = (worldX + halfW) / static_cast<float>(m_heightmapWidth);
        float v = (worldZ + halfH) / static_cast<float>(m_heightmapHeight);

        // Clamp to [0, 1]
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);

        // Convert to texel coordinates
        float texX = u * (m_heightmapWidth - 1);
        float texZ = v * (m_heightmapHeight - 1);

        // Bilinear interpolation
        int x0 = static_cast<int>(texX);
        int z0 = static_cast<int>(texZ);
        int x1 = std::min(x0 + 1, static_cast<int>(m_heightmapWidth - 1));
        int z1 = std::min(z0 + 1, static_cast<int>(m_heightmapHeight - 1));

        float fracX = texX - static_cast<float>(x0);
        float fracZ = texZ - static_cast<float>(z0);

        float h00 = m_heightmap[static_cast<size_t>(z0) * m_heightmapWidth + x0];
        float h10 = m_heightmap[static_cast<size_t>(z0) * m_heightmapWidth + x1];
        float h01 = m_heightmap[static_cast<size_t>(z1) * m_heightmapWidth + x0];
        float h11 = m_heightmap[static_cast<size_t>(z1) * m_heightmapWidth + x1];

        float h0 = h00 + fracX * (h10 - h00);
        float h1 = h01 + fracX * (h11 - h01);

        return h0 + fracZ * (h1 - h0);
    }

    // ========================================================================
    // SetHeightmapData
    // ========================================================================

    void ClipmapTerrain::SetHeightmapData(const std::vector<float>& heights, uint32_t width, uint32_t height)
    {
        m_heightmap = heights;
        m_heightmapWidth = width;
        m_heightmapHeight = height;

        // Mark all levels as needing update since heightmap changed
        for (auto& level : m_levels)
        {
            level.needsUpdate = true;
        }
    }

    // ========================================================================
    // Console Status
    // ========================================================================

    std::string ClipmapTerrain::Console_GetStatus() const
    {
        std::string status;
        status += std::format("ClipmapTerrain: {}\n", m_initialized ? "initialized" : "not initialized");

        if (m_initialized)
        {
            status += std::format("  Levels: {}\n", m_levels.size());
            for (const auto& level : m_levels)
            {
                status += std::format("  Level {}: cellSize={:.1f}, grid={}x{}, center=({:.1f}, {:.1f}), update={}\n",
                                      level.level, level.cellSize, level.gridSize, level.gridSize, level.center.x,
                                      level.center.z, level.needsUpdate ? "yes" : "no");
            }
            status += std::format("  Heightmap: {}x{} ({} samples)\n", m_heightmapWidth, m_heightmapHeight,
                                  m_heightmap.size());
        }

        return status;
    }

    // ========================================================================
    // Shutdown
    // ========================================================================

    void ClipmapTerrain::Shutdown()
    {
        m_levels.clear();
        m_heightmap.clear();
        m_heightmapWidth = 0;
        m_heightmapHeight = 0;
        m_initialized = false;
    }

    // ========================================================================
    // SnapToGrid
    // ========================================================================

    float ClipmapTerrain::SnapToGrid(float value, float cellSize)
    {
        return std::floor(value / cellSize) * cellSize;
    }

} // namespace Spark::Graphics
