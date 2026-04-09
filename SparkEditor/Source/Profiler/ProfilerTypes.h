/**
 * @file ProfilerTypes.h
 * @brief Type definitions, enums, and structs for the performance profiling system
 * @author Spark Engine Team
 * @date 2025
 *
 * Extracted from PerformanceProfiler.h to reduce header size and allow
 * other translation units to use profiler types without pulling in the
 * full PerformanceProfiler class.
 */

#pragma once

#ifdef _WIN32
#include <d3d11.h>
#include <cstdint>
#include <wrl/client.h>
#include <DirectXMath.h>
using namespace DirectX;
#else
#include "Core/Platform.h"
using namespace DirectX;
using Microsoft::WRL::ComPtr;
#endif
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <functional>
#include <cfloat>


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
 * @brief Performance counter data
 */
    struct PerformanceCounter
    {
        std::string name;              ///< Counter name
        ProfilerSampleType type;       ///< Sample type
        float currentValue = 0.0f;     ///< Current value
        float minValue = FLT_MAX;      ///< Minimum recorded value
        float maxValue = -FLT_MAX;     ///< Maximum recorded value
        float averageValue = 0.0f;     ///< Average value
        std::string unit;              ///< Value unit (ms, MB, etc.)
        XMFLOAT4 color = {1, 1, 1, 1}; ///< Display color
        bool isActive = true;          ///< Whether counter is active

        // Historical data
        std::vector<float> history;                       ///< Historical values
        int historySize = 1000;                           ///< Maximum history entries
        std::chrono::steady_clock::time_point lastUpdate; ///< Last update time

        /**
     * @brief Add sample to counter
     * @param value Sample value
     */
        void AddSample(float value);

        /**
     * @brief Clear counter data
     */
        void Clear();

        /**
     * @brief Get smoothed value
     * @param smoothingFactor Smoothing factor (0-1)
     * @return Smoothed value
     */
        float GetSmoothedValue(float smoothingFactor = 0.1f) const;
    };

    /**
 * @brief CPU profiling sample
 */
    struct CPUProfileSample
    {
        std::string name;                                         ///< Sample name
        std::string category;                                     ///< Sample category
        std::chrono::high_resolution_clock::time_point startTime; ///< Sample start time
        std::chrono::high_resolution_clock::time_point endTime;   ///< Sample end time
        float duration = 0.0f;                                    ///< Duration in milliseconds
        int threadID = 0;                                         ///< Thread ID
        int depth = 0;                                            ///< Call stack depth
        CPUProfileSample* parent = nullptr;                       ///< Parent sample
        std::vector<std::unique_ptr<CPUProfileSample>> children;  ///< Child samples

        /**
     * @brief Calculate self time (excluding children)
     * @return Self time in milliseconds
     */
        float GetSelfTime() const;

        /**
     * @brief Calculate total time (including children)
     * @return Total time in milliseconds
     */
        float GetTotalTime() const;
    };

    /**
 * @brief GPU profiling sample
 */
    struct GPUProfileSample
    {
        std::string name;            ///< Sample name
        std::string shaderName;      ///< Shader being executed
        uint64_t startTimestamp = 0; ///< GPU start timestamp
        uint64_t endTimestamp = 0;   ///< GPU end timestamp
        float duration = 0.0f;       ///< Duration in milliseconds
        uint32_t depth = 0;          ///< Scope nesting depth (render pass hierarchy)
        int drawCalls = 0;           ///< Number of draw calls
        int vertices = 0;            ///< Number of vertices processed
        int pixels = 0;              ///< Number of pixels processed
        size_t vramUsage = 0;        ///< VRAM usage in bytes
    };

    /**
 * @brief Memory profiling sample
 */
    struct MemoryProfileSample
    {
        std::string category;                            ///< Memory category
        size_t allocatedBytes = 0;                       ///< Currently allocated bytes
        size_t peakBytes = 0;                            ///< Peak allocated bytes
        int allocationCount = 0;                         ///< Number of allocations
        int deallocationCount = 0;                       ///< Number of deallocations
        size_t totalAllocatedBytes = 0;                  ///< Total bytes ever allocated
        std::chrono::steady_clock::time_point timestamp; ///< Sample timestamp
    };

    /**
 * @brief Frame profiling data
 */
    struct FrameProfileData
    {
        int frameNumber = 0;                             ///< Frame number
        std::chrono::steady_clock::time_point timestamp; ///< Frame timestamp
        float frameTime = 0.0f;                          ///< Total frame time (ms)
        float cpuTime = 0.0f;                            ///< CPU time (ms)
        float gpuTime = 0.0f;                            ///< GPU time (ms)
        float renderTime = 0.0f;                         ///< Rendering time (ms)
        float updateTime = 0.0f;                         ///< Update time (ms)
        float physicsTime = 0.0f;                        ///< Physics time (ms)
        float audioTime = 0.0f;                          ///< Audio time (ms)

        // Rendering statistics
        int drawCalls = 0;            ///< Number of draw calls
        int triangles = 0;            ///< Number of triangles rendered
        int textureBinds = 0;         ///< Number of texture bindings
        int shaderSwitches = 0;       ///< Number of shader switches
        int renderTargetSwitches = 0; ///< Number of render target switches

        // Memory statistics
        size_t systemMemoryUsage = 0; ///< System memory usage
        size_t videoMemoryUsage = 0;  ///< Video memory usage
        size_t audioMemoryUsage = 0;  ///< Audio memory usage
        int activeObjects = 0;        ///< Number of active objects
        int visibleObjects = 0;       ///< Number of visible objects

        // Performance metrics
        float fps = 0.0f;                ///< Frames per second
        float targetFrameTime = 16.67f;  ///< Target frame time (60 FPS)
        bool isPerformanceTarget = true; ///< Whether frame met performance target

        // GPU profiling capability/telemetry
        bool gpuTimestampQueriesSupported = false;  ///< Whether GPU timestamps are supported on this backend
        bool gpuPipelineStatsSupported = false;     ///< Whether pipeline stats are supported on this backend
        std::string gpuProfilerBackend = "Unknown"; ///< Backend label (e.g. D3D11)
        std::string gpuProfilerStatus;              ///< Human-readable status for UI fallback messaging
        uint64_t pipelineIAVertices = 0;            ///< IA vertex count (pipeline statistics)
        uint64_t pipelineIAPrimitives = 0;          ///< IA primitive count
        uint64_t pipelineVSInvocations = 0;         ///< VS invocation count
        uint64_t pipelinePSInvocations = 0;         ///< PS invocation count
        uint64_t pipelineCSInvocations = 0;         ///< CS invocation count
        uint64_t pipelineCInvocations = 0;          ///< Clipper invocation count
        uint64_t pipelineCPrimitives = 0;           ///< Clipper output primitive count

        std::vector<std::unique_ptr<CPUProfileSample>> cpuSamples; ///< CPU profiling samples
        std::vector<GPUProfileSample> gpuSamples;                  ///< GPU profiling samples
        std::vector<MemoryProfileSample> memorySamples;            ///< Memory profiling samples
    };

    /**
 * @brief Performance bottleneck identification
 */
    struct PerformanceBottleneck
    {
        enum Type
        {
            CPU_BOUND = 0,
            GPU_BOUND = 1,
            MEMORY_BOUND = 2,
            IO_BOUND = 3,
            BANDWIDTH_BOUND = 4,
            FILLRATE_BOUND = 5,
            VERTEX_BOUND = 6,
            TEXTURE_BOUND = 7
        } type;

        std::string description;                    ///< Bottleneck description
        std::string recommendation;                 ///< Optimization recommendation
        float severity = 0.0f;                      ///< Severity (0-1)
        float confidence = 0.0f;                    ///< Confidence in detection (0-1)
        std::vector<std::string> affectedSystems;   ///< Systems affected by bottleneck
        std::vector<std::string> optimizationHints; ///< Specific optimization suggestions
    };

    /**
 * @brief Automated optimization suggestion
 */
    struct OptimizationSuggestion
    {
        enum Priority
        {
            LOW = 0,
            MEDIUM = 1,
            HIGH = 2,
            CRITICAL = 3
        } priority;

        std::string title;                      ///< Suggestion title
        std::string description;                ///< Detailed description
        std::string category;                   ///< Optimization category
        float estimatedGain = 0.0f;             ///< Estimated performance gain (%)
        float implementationEffort = 0.0f;      ///< Implementation effort (0-1)
        std::vector<std::string> steps;         ///< Implementation steps
        bool isAutomatable = false;             ///< Whether suggestion can be automated
        std::function<bool()> automateFunction; ///< Automation function
    };

    /**
 * @brief Profiler configuration
 */
    struct ProfilerConfig
    {
        // Sampling settings
        bool enableCPUProfiling = true;    ///< Enable CPU profiling
        bool enableGPUProfiling = true;    ///< Enable GPU profiling
        bool enableMemoryProfiling = true; ///< Enable memory profiling
        bool enableDeepProfiling = false;  ///< Enable deep profiling (slower)
        int maxSamplesPerFrame = 10000;    ///< Maximum samples per frame
        float minSampleDuration = 0.01f;   ///< Minimum sample duration (ms)

        // Data retention
        int maxFrameHistory = 3600;                ///< Maximum frames to keep (60s at 60fps)
        int maxCounterHistory = 1000;              ///< Maximum counter history entries
        bool saveProfilingData = false;            ///< Save profiling data to file
        std::string dataOutputPath = "Profiling/"; ///< Profiling data output path

        // Performance targets
        float targetFrameRate = 60.0f;                   ///< Target frame rate
        float cpuBudget = 12.0f;                         ///< CPU budget per frame (ms)
        float gpuBudget = 14.0f;                         ///< GPU budget per frame (ms)
        size_t memoryBudget = 2ULL * 1024 * 1024 * 1024; ///< Memory budget (2GB)

        // Analysis settings
        bool enableBottleneckDetection = true;     ///< Enable automatic bottleneck detection
        bool enableOptimizationSuggestions = true; ///< Enable optimization suggestions
        float bottleneckThreshold = 0.8f;          ///< Bottleneck detection threshold
        int analysisWindowSize = 300;              ///< Analysis window size (frames)

        // UI settings
        bool showDetailedTimings = true;   ///< Show detailed timing breakdown
        bool showMemoryDetails = true;     ///< Show memory allocation details
        bool showOptimizationPanel = true; ///< Show optimization suggestions
        bool showRealTimeGraphs = true;    ///< Show real-time performance graphs
        bool highlightBottlenecks = true;  ///< Highlight detected bottlenecks
    };

    /**
 * @brief RAII profiling scope helper
 */
    class ProfileScope
    {
      public:
        ProfileScope(const std::string& name, const std::string& category = "General");
        ~ProfileScope();

        void End();

      private:
        std::string m_name;
        std::chrono::high_resolution_clock::time_point m_startTime;
        bool m_ended = false;
    };

/**
 * @brief Profiling macros for easy instrumentation
 */
#define PROFILE_SCOPE(name) ProfileScope _profile_scope(name)
#define PROFILE_SCOPE_CATEGORY(name, category) ProfileScope _profile_scope(name, category)
#define PROFILE_FUNCTION() PROFILE_SCOPE(__FUNCTION__)

} // namespace SparkEditor
