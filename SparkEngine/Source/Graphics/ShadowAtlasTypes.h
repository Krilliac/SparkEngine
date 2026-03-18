/**
 * @file ShadowAtlasTypes.h
 * @brief Type definitions for the shadow atlas system
 * @author Spark Engine Team
 * @date 2025
 *
 * Standalone structs used by ShadowAtlas and its consumers.
 * Extracted from ShadowAtlas.h to allow lightweight inclusion
 * without pulling in the full atlas manager.
 */

#pragma once

#include <cstdint>

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

} // namespace Spark::Graphics
