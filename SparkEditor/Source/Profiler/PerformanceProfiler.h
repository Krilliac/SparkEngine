/**
 * @file PerformanceProfiler.h
 * @brief Performance profiling and optimization system for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a comprehensive performance profiling system similar to
 * Unity Profiler and Unreal Insights, providing real-time analysis of CPU, GPU,
 * memory usage, and automated optimization recommendations.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
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

    /**
 * @brief Performance profiling and optimization system
 * 
 * Provides comprehensive performance analysis including:
 * - Real-time CPU and GPU profiling with call stacks
 * - Memory allocation tracking and leak detection
 * - Automated bottleneck identification and analysis
 * - Performance optimization suggestions
 * - Historical performance data and trending
 * - Integration with rendering and game systems
 * - Export capabilities for external analysis
 * - Real-time performance budgets and alerts
 * 
 * Inspired by Unity Profiler, Unreal Insights, and Intel VTune.
 */
    class PerformanceProfiler : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        PerformanceProfiler();

        /**
     * @brief Destructor
     */
        ~PerformanceProfiler() override;

        /**
     * @brief Initialize the performance profiler
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update performance profiler
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render performance profiler UI
     */
        void Render() override;

        /**
     * @brief Shutdown the performance profiler
     */
        void Shutdown() override;

        /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Start profiling session
     */
        void StartProfiling();

        /**
     * @brief Stop profiling session
     */
        void StopProfiling();

        /**
     * @brief Check if profiling is active
     * @return true if profiling is active
     */
        bool IsProfiling() const { return m_isProfiling; }

        /**
     * @brief Begin CPU profiling sample
     * @param name Sample name
     * @param category Sample category
     * @return Sample ID for ending
     */
        uint32_t BeginCPUSample(const std::string& name, const std::string& category = "General");

        /**
     * @brief End CPU profiling sample
     * @param sampleID Sample ID from BeginCPUSample
     */
        void EndCPUSample(uint32_t sampleID);

        /**
     * @brief Begin GPU profiling sample
     * @param name Sample name
     * @param shaderName Shader name (optional)
     */
        void BeginGPUSample(const std::string& name, const std::string& shaderName = "");

        /**
     * @brief End GPU profiling sample
     * @param name Sample name (must match BeginGPUSample)
     */
        void EndGPUSample(const std::string& name);

        /**
     * @brief Record memory allocation
     * @param category Memory category
     * @param bytes Number of bytes allocated
     * @param pointer Allocated pointer (for tracking)
     */
        void RecordMemoryAllocation(const std::string& category, size_t bytes, void* pointer = nullptr);

        /**
     * @brief Record memory deallocation
     * @param pointer Deallocated pointer
     */
        void RecordMemoryDeallocation(void* pointer);

        /**
     * @brief Add custom performance counter
     * @param name Counter name
     * @param type Counter type
     * @param unit Value unit
     * @return Counter ID
     */
        uint32_t AddPerformanceCounter(const std::string& name, ProfilerSampleType type, const std::string& unit);

        /**
     * @brief Update performance counter value
     * @param counterID Counter ID
     * @param value New value
     */
        void UpdatePerformanceCounter(uint32_t counterID, float value);

        /**
     * @brief Get current frame data
     * @return Pointer to current frame data
     */
        const FrameProfileData* GetCurrentFrame() const;

        /**
     * @brief Get frame data by index
     * @param frameIndex Frame index (0 = most recent)
     * @return Pointer to frame data, or nullptr if not available
     */
        const FrameProfileData* GetFrame(int frameIndex) const;

        /**
     * @brief Get detected bottlenecks
     * @return Vector of detected bottlenecks
     */
        const std::vector<PerformanceBottleneck>& GetBottlenecks() const { return m_detectedBottlenecks; }

        /**
     * @brief Get optimization suggestions
     * @return Vector of optimization suggestions
     */
        const std::vector<OptimizationSuggestion>& GetOptimizationSuggestions() const
        {
            return m_optimizationSuggestions;
        }

        /**
     * @brief Apply optimization suggestion
     * @param suggestionIndex Index of suggestion to apply
     * @return true if optimization was applied successfully
     */
        bool ApplyOptimization(int suggestionIndex);

        /**
     * @brief Export profiling data
     * @param filePath Output file path
     * @param format Export format ("csv", "json", "binary")
     * @return true if export succeeded
     */
        bool ExportProfilingData(const std::string& filePath, const std::string& format = "json");

        /**
     * @brief Import profiling data
     * @param filePath Input file path
     * @return true if import succeeded
     */
        bool ImportProfilingData(const std::string& filePath);

        /**
     * @brief Clear all profiling data
     */
        void ClearProfilingData();

        /**
     * @brief Set profiler configuration
     * @param config New configuration
     */
        void SetConfiguration(const ProfilerConfig& config);

        /**
     * @brief Get profiler configuration
     * @return Reference to current configuration
     */
        const ProfilerConfig& GetConfiguration() const { return m_config; }

        /**
     * @brief Take performance snapshot
     * @param name Snapshot name
     * @return Snapshot ID
     */
        uint32_t TakeSnapshot(const std::string& name);

        /**
     * @brief Compare two performance snapshots
     * @param snapshot1 First snapshot ID
     * @param snapshot2 Second snapshot ID
     * @return Comparison results
     */
        std::string CompareSnapshots(uint32_t snapshot1, uint32_t snapshot2);

        /**
     * @brief Get performance trend analysis
     * @param metric Metric to analyze
     * @param timespan Timespan to analyze (seconds)
     * @return Trend analysis results
     */
        std::string GetTrendAnalysis(const std::string& metric, float timespan = 60.0f);

      private:
        /**
     * @brief Render overview panel
     */
        void RenderOverviewPanel();

        /**
     * @brief Render CPU profiler panel
     */
        void RenderCPUProfilerPanel();

        /**
     * @brief Render GPU profiler panel
     */
        void RenderGPUProfilerPanel();

        /**
     * @brief Render memory profiler panel
     */
        void RenderMemoryProfilerPanel();

        /**
     * @brief Render performance counters panel
     */
        void RenderPerformanceCountersPanel();

        /**
     * @brief Render optimization panel
     */
        void RenderOptimizationPanel();

        /**
     * @brief Render configuration panel
     */
        void RenderConfigurationPanel();

        /**
     * @brief Update frame profiling data
     */
        void UpdateFrameData();

        /**
     * @brief Analyze performance for bottlenecks
     */
        void AnalyzePerformance();

        /**
     * @brief Generate optimization suggestions
     */
        void GenerateOptimizationSuggestions();

        /**
     * @brief Detect CPU bottlenecks
     */
        void DetectCPUBottlenecks();

        /**
     * @brief Detect GPU bottlenecks
     */
        void DetectGPUBottlenecks();

        /**
     * @brief Detect memory bottlenecks
     */
        void DetectMemoryBottlenecks();

        /**
     * @brief Process GPU timing queries
     */
        void ProcessGPUQueries();

        /**
     * @brief Update memory tracking
     */
        void UpdateMemoryTracking();

        /**
     * @brief Calculate performance statistics
     */
        void CalculateStatistics();

        /**
     * @brief Render CPU sample hierarchy
     * @param sample CPU sample to render
     * @param depth Hierarchy depth
     */
        void RenderCPUSampleHierarchy(const CPUProfileSample* sample, int depth = 0);

        /**
     * @brief Render performance graph
     * @param counter Performance counter to graph
     * @param size Graph size
     */
        void RenderPerformanceGraph(const PerformanceCounter& counter, const XMFLOAT2& size);

      private:
        // Profiling state
        bool m_isProfiling = false; ///< Whether profiling is active
        ProfilerConfig m_config;    ///< Profiler configuration

        // Frame data
        std::vector<std::unique_ptr<FrameProfileData>> m_frameHistory; ///< Historical frame data
        std::unique_ptr<FrameProfileData> m_currentFrame;              ///< Current frame data
        int m_currentFrameNumber = 0;                                  ///< Current frame number

        // CPU profiling
        std::vector<std::unique_ptr<CPUProfileSample>> m_activeCPUSamples; ///< Currently active CPU samples
        std::unordered_map<uint32_t, CPUProfileSample*> m_cpuSampleMap;    ///< Sample ID to sample map
        uint32_t m_nextCPUSampleID = 1;                                    ///< Next CPU sample ID
        std::mutex m_cpuSampleMutex;                                       ///< CPU sample mutex

        // GPU profiling
        std::unordered_map<std::string, GPUProfileSample> m_activeGPUSamples; ///< Active GPU samples
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;                        ///< DirectX device
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;                ///< DirectX context
        std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> m_gpuQueries;        ///< GPU timing queries

        // Memory profiling
        std::unordered_map<void*, std::pair<std::string, size_t>> m_memoryAllocations; ///< Active allocations
        std::unordered_map<std::string, MemoryProfileSample> m_memoryCategories;       ///< Memory by category
        std::mutex m_memoryMutex;                                                      ///< Memory tracking mutex

        // Performance counters
        std::vector<PerformanceCounter> m_performanceCounters; ///< Custom performance counters
        uint32_t m_nextCounterID = 1;                          ///< Next counter ID

        // Analysis results
        std::vector<PerformanceBottleneck> m_detectedBottlenecks;      ///< Detected bottlenecks
        std::vector<OptimizationSuggestion> m_optimizationSuggestions; ///< Optimization suggestions
        std::chrono::steady_clock::time_point m_lastAnalysisTime;      ///< Last analysis time

        // UI state
        bool m_showOverview = true;       ///< Show overview panel
        bool m_showCPUProfiler = true;    ///< Show CPU profiler panel
        bool m_showGPUProfiler = true;    ///< Show GPU profiler panel
        bool m_showMemoryProfiler = true; ///< Show memory profiler panel
        bool m_showCounters = false;      ///< Show performance counters panel
        bool m_showOptimization = true;   ///< Show optimization panel
        bool m_showConfiguration = false; ///< Show configuration panel

        // Visualization settings
        float m_timelineZoom = 1.0f;   ///< Timeline zoom level
        float m_timelineOffset = 0.0f; ///< Timeline offset
        int m_selectedFrame = 0;       ///< Selected frame for detailed view
        std::string m_selectedSample;  ///< Selected sample name

        // Performance snapshots
        struct PerformanceSnapshot
        {
            std::string name;
            std::chrono::steady_clock::time_point timestamp;
            FrameProfileData frameData;
            std::vector<PerformanceCounter> counters;
        };
        std::vector<PerformanceSnapshot> m_snapshots; ///< Performance snapshots
        uint32_t m_nextSnapshotID = 1;                ///< Next snapshot ID
    };

    // Global profiler instance for easy access
    extern PerformanceProfiler* g_profiler;

} // namespace SparkEditor