/**
 * @file ClipmapTerrain.h
 * @brief Clipmap-based terrain LOD system for efficient large-world rendering
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a geometry clipmap approach where concentric grid rings of
 * increasing cell size surround the camera. Inner levels have fine detail,
 * outer levels have coarse detail. Each level snaps to its cell grid to
 * avoid per-frame full rebuilds — only edge rows/columns update as the
 * camera moves.
 *
 * Complements the existing TerrainRenderer by providing LOD-aware mesh
 * generation. TerrainRenderer can use ClipmapTerrain::GenerateMesh() to
 * build per-level meshes instead of a single full-resolution grid.
 */

#pragma once
#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // ========================================================================
    // Clipmap Level
    // ========================================================================

    /**
     * @brief Description of a single clipmap LOD ring.
     *
     * Each level is a square grid of cells centered on the camera position,
     * snapped to the level's cell size. Level 0 is the finest detail;
     * each subsequent level doubles the cell size.
     */
    struct ClipmapLevel
    {
        uint32_t level = 0;       ///< LOD level index (0 = finest)
        float cellSize = 1.0f;    ///< World-space size of one grid cell
        uint32_t gridSize = 64;   ///< Number of grid cells per side
        XMFLOAT3 center{0, 0, 0}; ///< Center position (snapped to cell grid)
        bool needsUpdate = true;  ///< True when the level needs mesh regeneration
    };

    // ========================================================================
    // ClipmapTerrain
    // ========================================================================

    /**
     * @brief Singleton clipmap terrain LOD manager.
     *
     * Manages a stack of concentric grid levels around the camera. As the
     * camera moves, levels are recentered (snapped to their grid) and marked
     * for mesh regeneration. The GenerateMesh() method produces vertex/index
     * data for a given level that the renderer can upload to GPU buffers.
     *
     * @threadsafety Main thread only.
     */
    class ClipmapTerrain
    {
      public:
        /// @brief Get the singleton instance
        static ClipmapTerrain& GetInstance();

        /**
         * @brief Initialize the clipmap level stack.
         * @param numLevels    Number of LOD levels (default 4)
         * @param gridSize     Grid cells per side per level (default 64)
         * @param baseCellSize World-space cell size for level 0 (default 1.0)
         * @return true on success
         */
        bool Initialize(uint32_t numLevels = 4, uint32_t gridSize = 64, float baseCellSize = 1.0f);

        /**
         * @brief Update clipmap centers based on camera position.
         *
         * Snaps each level's center to its cell grid. If the center moved,
         * the level is marked for mesh regeneration.
         *
         * @param cameraPosition Current camera world position
         */
        void Update(const XMFLOAT3& cameraPosition);

        /**
         * @brief Generate mesh data for a specific clipmap level.
         *
         * Produces a flat grid of vertices with Y heights sampled from the
         * heightmap. If no heightmap is set, Y is zero everywhere.
         *
         * @param level    LOD level index
         * @param vertices Output vertex positions (x, y, z interleaved)
         * @param indices  Output triangle indices
         */
        void GenerateMesh(uint32_t level, std::vector<float>& vertices, std::vector<uint32_t>& indices) const;

        /// @brief Get the number of clipmap levels
        uint32_t GetLevelCount() const;

        /// @brief Get a specific clipmap level (bounds-checked)
        const ClipmapLevel& GetLevel(uint32_t level) const;

        /**
         * @brief Sample terrain height at a world position.
         *
         * Bilinearly interpolates the heightmap. Returns 0 if no heightmap
         * is loaded or the position is outside the heightmap bounds.
         *
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return Terrain height at the given position
         */
        float SampleHeight(float worldX, float worldZ) const;

        /**
         * @brief Set the heightmap data for terrain height sampling.
         * @param heights  Row-major heightmap values
         * @param width    Heightmap width in samples
         * @param height   Heightmap height in samples
         */
        void SetHeightmapData(const std::vector<float>& heights, uint32_t width, uint32_t height);

        /// @brief Get a human-readable status string for console display
        std::string Console_GetStatus() const;

        /// @brief Release all resources
        void Shutdown();

      private:
        ClipmapTerrain() = default;

        std::vector<ClipmapLevel> m_levels;
        std::vector<float> m_heightmap;
        uint32_t m_heightmapWidth = 0;
        uint32_t m_heightmapHeight = 0;
        bool m_initialized = false;

        /// @brief Snap a world coordinate to a grid cell boundary
        static float SnapToGrid(float value, float cellSize);
    };

} // namespace Spark::Graphics
