/**
 * @file Profiler.h
 * @brief Frame profiling and performance analysis system
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides per-system timing, frame time graphs, GPU timing queries,
 * and memory usage tracking. Renders as an ImGui overlay.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif // SPARK_PLATFORM_WINDOWS
using Microsoft::WRL::ComPtr;
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <chrono>

/**
 * @brief Profiling category
 */
enum class ProfileCategory
{
    Frame,
    Render,
    Physics,
    Audio,
    GameLogic,
    Input,
    Particles,
    UI,
    Custom,
    Count
};

/**
 * @brief Single timing sample
 */
struct ProfileSample
{
    std::string name;
    ProfileCategory category = ProfileCategory::Custom;
    double startTimeMs = 0.0;
    double durationMs = 0.0;
    int depth = 0;
};

/**
 * @brief Frame timing history for graphs
 */
struct FrameTimingHistory
{
    static constexpr int HISTORY_SIZE = 300; // ~5 seconds at 60fps
    std::array<float, HISTORY_SIZE> frameTimes = {};
    int writeIndex = 0;
    float minTime = 0.0f;
    float maxTime = 0.0f;
    float avgTime = 0.0f;

    void Push(float timeMs)
    {
        frameTimes[writeIndex] = timeMs;
        writeIndex = (writeIndex + 1) % HISTORY_SIZE;

        // Update stats
        minTime = 1000.0f;
        maxTime = 0.0f;
        float sum = 0.0f;
        int count = 0;
        for (float t : frameTimes)
        {
            if (t > 0.0f)
            {
                if (t < minTime) minTime = t;
                if (t > maxTime) maxTime = t;
                sum += t;
                count++;
            }
        }
        avgTime = (count > 0) ? sum / count : 0.0f;
    }
};

#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @brief GPU timing query wrapper
 */
struct GPUTimerQuery
{
    ComPtr<ID3D11Query> beginQuery;
    ComPtr<ID3D11Query> endQuery;
    ComPtr<ID3D11Query> disjointQuery;
    bool pending = false;
    double resultMs = 0.0;
};
#endif // SPARK_PLATFORM_WINDOWS

/**
 * @brief Scoped CPU timer - automatically records start/end
 */
class ScopedProfileTimer
{
public:
    ScopedProfileTimer(const std::string& name, ProfileCategory category = ProfileCategory::Custom);
    ~ScopedProfileTimer();

private:
    std::string m_name;
    ProfileCategory m_category;
    std::chrono::high_resolution_clock::time_point m_start;
};

/**
 * @brief Main profiler system
 */
class Profiler
{
public:
    static Profiler& GetInstance()
    {
        static Profiler instance;
        return instance;
    }

#ifdef SPARK_PLATFORM_WINDOWS
    /**
     * @brief Initialize with D3D11 device for GPU queries
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
#endif // SPARK_PLATFORM_WINDOWS

    /**
     * @brief Shutdown and release resources
     */
    void Shutdown();

    // ============================================================================
    // CPU Timing
    // ============================================================================

    /**
     * @brief Begin a named timing section
     */
    void BeginSection(const std::string& name, ProfileCategory category = ProfileCategory::Custom);

    /**
     * @brief End a named timing section
     */
    void EndSection(const std::string& name);

    /**
     * @brief Mark the beginning of a new frame
     */
    void BeginFrame();

    /**
     * @brief Mark the end of the current frame
     */
    void EndFrame();

#ifdef SPARK_PLATFORM_WINDOWS
    // ============================================================================
    // GPU Timing
    // ============================================================================

    /**
     * @brief Begin a GPU timing query
     */
    void BeginGPUTimer(const std::string& name);

    /**
     * @brief End a GPU timing query
     */
    void EndGPUTimer(const std::string& name);

    /**
     * @brief Resolve pending GPU queries (call at end of frame)
     */
    void ResolveGPUQueries();
#endif // SPARK_PLATFORM_WINDOWS

    // ============================================================================
    // Memory Tracking
    // ============================================================================

    /**
     * @brief Record a memory allocation
     */
    void RecordAllocation(const std::string& category, size_t bytes);

    /**
     * @brief Record a memory deallocation
     */
    void RecordDeallocation(const std::string& category, size_t bytes);

    // ============================================================================
    // Query Results
    // ============================================================================

    float GetFrameTimeMs() const { return m_frameHistory.avgTime; }
    float GetFPS() const { return (m_frameHistory.avgTime > 0.0f) ? 1000.0f / m_frameHistory.avgTime : 0.0f; }
    double GetSectionTimeMs(const std::string& name) const;
    double GetGPUTimeMs(const std::string& name) const;
    const FrameTimingHistory& GetFrameHistory() const { return m_frameHistory; }

    // Per-category totals
    double GetCategoryTimeMs(ProfileCategory category) const;

    // ============================================================================
    // Display Control
    // ============================================================================

    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    void Toggle() { m_enabled = !m_enabled; }

    bool IsOverlayVisible() const { return m_overlayVisible; }
    void SetOverlayVisible(bool visible) { m_overlayVisible = visible; }
    void ToggleOverlay() { m_overlayVisible = !m_overlayVisible; }

    // ============================================================================
    // Console Integration
    // ============================================================================

    std::string Console_GetReport() const;
    std::string Console_GetGPUReport() const;
    std::string Console_GetMemoryReport() const;
    void Console_ExportCSV(const std::string& filepath) const;

private:
    Profiler() = default;
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    bool m_enabled = false;
    bool m_overlayVisible = false;

    // CPU timing
    std::vector<ProfileSample> m_currentFrameSamples;
    std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> m_activeSections;
    std::unordered_map<std::string, double> m_sectionResults;

    // Per-category timing
    std::array<double, static_cast<size_t>(ProfileCategory::Count)> m_categoryTotals = {};

    // Frame timing history
    FrameTimingHistory m_frameHistory;
    std::chrono::high_resolution_clock::time_point m_frameStart;

#ifdef SPARK_PLATFORM_WINDOWS
    // GPU timing
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    std::unordered_map<std::string, GPUTimerQuery> m_gpuTimers;
#endif // SPARK_PLATFORM_WINDOWS

    // Memory tracking
    struct MemoryCategory {
        size_t currentBytes = 0;
        size_t peakBytes = 0;
        size_t totalAllocations = 0;
    };
    std::unordered_map<std::string, MemoryCategory> m_memoryCategories;

    int m_frameCount = 0;
};

// Convenience macros for profiling
#ifdef PROFILING_ENABLED
    #define PROFILE_SCOPE(name)       ScopedProfileTimer _profile_##__LINE__(name)
    #define PROFILE_SCOPE_CAT(name, cat) ScopedProfileTimer _profile_##__LINE__(name, cat)
    #define PROFILE_BEGIN(name)       Profiler::GetInstance().BeginSection(name)
    #define PROFILE_END(name)         Profiler::GetInstance().EndSection(name)
    #ifdef SPARK_PLATFORM_WINDOWS
        #define PROFILE_GPU_BEGIN(name)   Profiler::GetInstance().BeginGPUTimer(name)
        #define PROFILE_GPU_END(name)     Profiler::GetInstance().EndGPUTimer(name)
    #else
        #define PROFILE_GPU_BEGIN(name)
        #define PROFILE_GPU_END(name)
    #endif
#else
    #define PROFILE_SCOPE(name)
    #define PROFILE_SCOPE_CAT(name, cat)
    #define PROFILE_BEGIN(name)
    #define PROFILE_END(name)
    #define PROFILE_GPU_BEGIN(name)
    #define PROFILE_GPU_END(name)
#endif
