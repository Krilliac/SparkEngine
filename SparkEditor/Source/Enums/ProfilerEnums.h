/**
 * @file ProfilerEnums.h
 * @brief Enumerations for the performance profiler system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains all enumerations related to performance profiling,
 * including sample types, bottleneck types, and optimization priorities.
 */

#pragma once

namespace SparkEditor
{

    /**
 * @brief Profiler sample types
 */
    enum class ProfilerSampleType
    {
        CPU_SAMPLE = 0,       ///< CPU timing sample
        GPU_SAMPLE = 1,       ///< GPU timing sample
        MEMORY_SAMPLE = 2,    ///< Memory usage sample
        NETWORK_SAMPLE = 3,   ///< Network activity sample
        AUDIO_SAMPLE = 4,     ///< Audio processing sample
        PHYSICS_SAMPLE = 5,   ///< Physics simulation sample
        RENDERING_SAMPLE = 6, ///< Rendering sample
        CUSTOM_SAMPLE = 7     ///< Custom user sample
    };

    /**
 * @brief Performance bottleneck types
 */
    enum class BottleneckType
    {
        CPU_BOUND = 0,       ///< CPU bottleneck
        GPU_BOUND = 1,       ///< GPU bottleneck
        MEMORY_BOUND = 2,    ///< Memory bottleneck
        IO_BOUND = 3,        ///< I/O bottleneck
        BANDWIDTH_BOUND = 4, ///< Bandwidth bottleneck
        FILLRATE_BOUND = 5,  ///< Fill rate bottleneck
        VERTEX_BOUND = 6,    ///< Vertex processing bottleneck
        TEXTURE_BOUND = 7    ///< Texture memory bottleneck
    };

    /**
 * @brief Optimization suggestion priorities
 */
    enum class OptimizationPriority
    {
        LOW = 0,     ///< Low priority
        MEDIUM = 1,  ///< Medium priority
        HIGH = 2,    ///< High priority
        CRITICAL = 3 ///< Critical priority
    };

    /**
 * @brief Profiler view modes
 */
    enum class ProfilerViewMode
    {
        OVERVIEW = 0,   ///< Overview of all systems
        DETAILED = 1,   ///< Detailed view of specific system
        TIMELINE = 2,   ///< Timeline view
        HIERARCHY = 3,  ///< Hierarchical call view
        STATISTICS = 4, ///< Statistical view
        COMPARISON = 5  ///< Comparison view
    };

    /**
 * @brief Memory allocation categories
 */
    enum class MemoryCategory
    {
        SYSTEM = 0,    ///< System memory
        GRAPHICS = 1,  ///< Graphics memory
        AUDIO = 2,     ///< Audio memory
        PHYSICS = 3,   ///< Physics memory
        SCRIPTS = 4,   ///< Script memory
        ASSETS = 5,    ///< Asset memory
        TEMPORARY = 6, ///< Temporary allocations
        UNKNOWN = 7    ///< Unknown category
    };

    /**
 * @brief Performance metric types
 */
    enum class MetricType
    {
        FRAME_TIME = 0,     ///< Frame time in milliseconds
        FPS = 1,            ///< Frames per second
        CPU_USAGE = 2,      ///< CPU usage percentage
        GPU_USAGE = 3,      ///< GPU usage percentage
        MEMORY_USAGE = 4,   ///< Memory usage in bytes
        DRAW_CALLS = 5,     ///< Number of draw calls
        TRIANGLES = 6,      ///< Number of triangles
        VERTICES = 7,       ///< Number of vertices
        TEXTURE_MEMORY = 8, ///< Texture memory usage
        BUFFER_MEMORY = 9   ///< Buffer memory usage
    };

    /**
 * @brief Profiler capture modes
 */
    enum class CaptureMode
    {
        CONTINUOUS = 0,   ///< Continuous capture
        SINGLE_FRAME = 1, ///< Single frame capture
        RANGE = 2,        ///< Range capture
        TRIGGERED = 3     ///< Triggered capture
    };

    /**
 * @brief Data export formats
 */
    enum class ExportFormat
    {
        JSON = 0,   ///< JSON format
        CSV = 1,    ///< CSV format
        BINARY = 2, ///< Binary format
        XML = 3     ///< XML format
    };

} // namespace SparkEditor