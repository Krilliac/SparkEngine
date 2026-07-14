/**
 * @file TerrainComponents.h
 * @brief ECS components for terrain entities
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides TerrainComponent for heightmap-based terrain entities in the scene.
 * Terrain entities represent large landscape surfaces with heightmap data,
 * multi-layer texture splatmaps, and LOD settings.
 */

#pragma once

#include "../../../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>

// =============================================================================
// TerrainComponent
// =============================================================================

/**
 * @struct TerrainComponent
 * @brief Attaches terrain data to an entity, enabling heightmap-based landscape rendering.
 *
 * The terrain system uses this component to identify entities that represent
 * terrain surfaces. The heightmap, texture layers, and LOD parameters are
 * stored here and consumed by the rendering and physics systems.
 */
struct TerrainComponent
{
    std::string terrainAssetPath;               ///< Path to .sparkterrain asset file.
    int heightmapResolution = 513;              ///< Heightmap width/height in samples.
    float terrainSize = 1000.0f;                ///< Terrain extent in world units (square).
    float heightScale = 1.0f;                   ///< Vertical scale multiplier.
    float minHeight = 0.0f;                     ///< Minimum terrain height.
    float maxHeight = 100.0f;                   ///< Maximum terrain height.
    std::vector<float> heightmap;               ///< Height values (resolution * resolution).
    std::vector<std::string> textureLayerPaths; ///< Diffuse texture paths per layer.
    std::vector<uint8_t> splatmap;              ///< RGBA blend weights per splatmap texel.
    int splatmapResolution = 512;               ///< Splatmap width/height.
    int lodLevels = 4;                          ///< Number of LOD levels.
    float lodBias = 1.0f;                       ///< LOD distance bias multiplier.
    bool generateCollider = true;               ///< Generate physics collision mesh.
    std::string physicsMaterial;                ///< Physics material asset path.
    bool castShadows = true;                    ///< Whether terrain casts shadows.
    bool receiveShadows = true;                 ///< Whether terrain receives shadows.
    bool visible = true;                        ///< Runtime visibility toggle.

    // Runtime state (not serialized)
    uint32_t selectedLOD = 0; ///< LOD level selected by TerrainSystem.
    bool dirty = true;        ///< Mesh needs rebuild (heightmap changed).
};
