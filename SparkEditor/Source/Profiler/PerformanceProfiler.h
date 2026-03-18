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
#include "ProfilerTypes.h"
#ifdef _WIN32
#include <d3d11.h>
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
#include <unordered_map>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <functional>


namespace SparkEditor
{

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