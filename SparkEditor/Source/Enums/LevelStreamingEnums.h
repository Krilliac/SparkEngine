/**
 * @file LevelStreamingEnums.h
 * @brief Enumerations for the level streaming system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains all enumerations related to level streaming,
 * including tile states, loading priorities, and streaming methods.
 */

#pragma once

namespace SparkEditor {

/**
 * @brief Tile loading states
 */
enum class TileState {
    UNLOADED = 0,           ///< Tile is not loaded
    LOADING = 1,            ///< Tile is currently loading
    LOADED = 2,             ///< Tile is fully loaded and active
    UNLOADING = 3,          ///< Tile is being unloaded
    ERROR = 4,              ///< Error occurred during loading
    CACHED = 5              ///< Tile is cached but not active
};

/**
 * @brief Loading priority levels
 */
enum class LoadingPriority {
    LOW = 0,                ///< Low priority (background loading)
    NORMAL = 1,             ///< Normal priority
    HIGH = 2,               ///< High priority (visible soon)
    IMMEDIATE = 3           ///< Immediate priority (blocking load)
};

/**
 * @brief Streaming methods
 */
enum class StreamingMethod {
    DISTANCE_BASED = 0,     ///< Stream based on distance from player
    TRIGGER_BASED = 1,      ///< Stream when entering trigger volumes
    MANUAL = 2,             ///< Manual streaming control
    PRIORITY_BASED = 3,     ///< Stream based on priority and memory budget
    PREDICTIVE = 4          ///< Predictive streaming based on player movement
};

/**
 * @brief Level of detail for tiles
 */
enum class TileLOD {
    LOD_0 = 0,              ///< Highest detail level
    LOD_1 = 1,              ///< High detail level
    LOD_2 = 2,              ///< Medium detail level
    LOD_3 = 3,              ///< Low detail level
    LOD_4 = 4               ///< Lowest detail level
};

/**
 * @brief Tile types
 */
enum class TileType {
    TERRAIN = 0,            ///< Terrain tile
    GEOMETRY = 1,           ///< Static geometry tile
    VEGETATION = 2,         ///< Vegetation tile
    WATER = 3,              ///< Water tile
    BUILDINGS = 4,          ///< Building tile
    PROPS = 5,              ///< Props and details tile
    LIGHTING = 6,           ///< Lighting information tile
    COLLISION = 7,          ///< Collision data tile
    CUSTOM = 8              ///< Custom tile type
};

/**
 * @brief Streaming trigger types
 */
enum class TriggerType {
    SPHERE = 0,             ///< Spherical trigger volume
    BOX = 1,                ///< Box trigger volume
    PLANE = 2,              ///< Planar trigger
    CUSTOM = 3              ///< Custom trigger shape
};

/**
 * @brief World composition modes
 */
enum class CompositionMode {
    TILES = 0,              ///< Tile-based composition
    SEAMLESS = 1,           ///< Seamless world
    LAYERED = 2,            ///< Layered composition
    HYBRID = 3              ///< Hybrid composition
};

/**
 * @brief Memory management strategies
 */
enum class MemoryStrategy {
    LAZY = 0,               ///< Load when needed, unload when far
    AGGRESSIVE = 1,         ///< Preload aggressively, keep loaded
    BALANCED = 2,           ///< Balance between memory and performance
    CUSTOM = 3              ///< Custom memory management
};

/**
 * @brief Streaming quality settings
 */
enum class StreamingQuality {
    LOW = 0,                ///< Low quality (performance focused)
    MEDIUM = 1,             ///< Medium quality
    HIGH = 2,               ///< High quality
    ULTRA = 3               ///< Ultra quality (quality focused)
};

/**
 * @brief Tile boundary handling
 */
enum class BoundaryHandling {
    HARD_BOUNDARY = 0,      ///< Hard boundaries (pop-in/out)
    SOFT_BOUNDARY = 1,      ///< Soft boundaries (fade in/out)
    SEAMLESS = 2,           ///< Seamless boundaries
    OVERLAP = 3             ///< Overlapping boundaries
};

/**
 * @brief Background loading states
 */
enum class BackgroundLoadState {
    IDLE = 0,               ///< No background loading
    PREPARING = 1,          ///< Preparing to load
    LOADING_MESH = 2,       ///< Loading mesh data
    LOADING_TEXTURES = 3,   ///< Loading texture data
    LOADING_AUDIO = 4,      ///< Loading audio data
    PROCESSING = 5,         ///< Processing loaded data
    FINALIZING = 6          ///< Finalizing load
};

} // namespace SparkEditor