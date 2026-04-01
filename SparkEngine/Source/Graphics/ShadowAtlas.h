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
#include "RHI/RHI.h"

#include <cstdint>
#include <memory>
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

        /// @brief Initialize the atlas with a given size and minimum tile size
        bool Initialize(uint32_t atlasSize = 4096, uint32_t minTileSize = 256);

        /// @brief Release all resources
        void Shutdown();

        /// @brief Begin a new frame, marking all tiles inactive
        void BeginFrame();

        /// @brief Request a shadow tile for a light. Returns true if allocated.
        bool RequestTile(uint32_t lightId, float priority, uint32_t desiredSize);

        /// @brief Explicitly release a tile for a light
        void ReleaseTile(uint32_t lightId);

        /// @brief Get the tile for a light (nullptr if not allocated)
        const ShadowTile* GetTile(uint32_t lightId) const;

        /// @brief End frame: evict stale tiles unused for several frames
        void EndFrame();

        /// @brief Get the atlas texture size in pixels
        uint32_t GetAtlasSize() const { return m_atlasSize; }

        /// @brief Check if the atlas has been initialized
        bool IsInitialized() const { return m_initialized; }

        /// @brief Get atlas utilization metrics
        ShadowAtlasMetrics GetMetrics() const;

        /// @brief Get a human-readable status string for the console
        std::string Console_GetStatus() const;

        /// @brief Create the GPU depth texture atlas (R32_FLOAT, atlasSize x atlasSize)
        /// @param device RHI device used to create the texture
        /// @return True if the GPU texture was created successfully
        bool CreateGPUResources(Spark::RHI::IRHIDevice* device);

        /// @brief Set the viewport to the tile region for a given light's shadow pass
        /// @param cmdList RHI command list to record the viewport change on
        /// @param lightId Light whose tile viewport should be bound
        void BindForShadowPass(Spark::RHI::IRHICommandList* cmdList, uint32_t lightId);

        /// @brief Get the depth atlas texture for sampling in lighting shaders
        /// @return Pointer to the atlas texture, or nullptr if not created
        Spark::RHI::IRHITexture* GetAtlasTexture() const;

      private:
        bool IsRegionFree(uint32_t gx, uint32_t gy, uint32_t cells) const;
        void MarkRegion(uint32_t gx, uint32_t gy, uint32_t cells, bool used);
        void FreeTile(uint32_t index);
        bool EvictAndAllocate(uint32_t lightId, float priority, uint32_t cellsNeeded);

        bool m_initialized = false;
        uint32_t m_atlasSize = 4096;
        uint32_t m_minTileSize = 256;
        uint32_t m_gridSize = 0;
        uint32_t m_frameIndex = 0;

        std::vector<ShadowTile> m_tiles;
        std::unordered_map<uint32_t, uint32_t> m_tileMap; ///< lightId -> tile index
        std::vector<bool> m_gridCells;                    ///< Grid occupancy

        std::unique_ptr<Spark::RHI::IRHITexture> m_gpuAtlasTexture; ///< GPU depth atlas texture
    };

} // namespace Spark::Graphics
