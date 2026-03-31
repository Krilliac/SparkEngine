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
using Microsoft::WRL::ComPtr;
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <string_view>
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
 *
 * Uses string_view into caller-owned storage (typically string literals from
 * PROFILE_SCOPE macros) to avoid per-sample heap allocations on the hot path.
 */
struct ProfileSample
{
    std::string_view name;
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
        // Subtract old value from running sum, add new value — O(1) instead of O(N).
        float oldVal = frameTimes[writeIndex];
        frameTimes[writeIndex] = timeMs;
        writeIndex = (writeIndex + 1) % HISTORY_SIZE;

        // Maintain running sum for O(1) average
        if (oldVal > 0.0f)
        {
            m_runningSum -= oldVal;
            // Min/max may need full recalc if we removed the extremum
            m_dirtyMinMax = m_dirtyMinMax || (oldVal <= minTime) || (oldVal >= maxTime);
        }
        else
        {
            m_sampleCount++;
        }
        m_runningSum += timeMs;
        avgTime = (m_sampleCount > 0) ? m_runningSum / static_cast<float>(m_sampleCount) : 0.0f;

        // Fast-path min/max update
        if (m_sampleCount == 1)
        {
            // First sample — initialize both extremes
            minTime = timeMs;
            maxTime = timeMs;
            m_dirtyMinMax = false;
        }
        else if (!m_dirtyMinMax)
        {
            if (timeMs < minTime)
                minTime = timeMs;
            if (timeMs > maxTime)
                maxTime = timeMs;
        }
        else
        {
            // Full recalc only when we evicted an extremum (rare: ~1/HISTORY_SIZE pushes)
            minTime = 1000.0f;
            maxTime = 0.0f;
            for (float t : frameTimes)
            {
                if (t > 0.0f)
                {
                    if (t < minTime)
                        minTime = t;
                    if (t > maxTime)
                        maxTime = t;
                }
            }
            m_dirtyMinMax = false;
        }
    }

    // Internal state for O(1) running average (public for aggregate init compatibility)
    float m_runningSum = 0.0f;
    int m_sampleCount = 0;
    bool m_dirtyMinMax = false;
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
    ScopedProfileTimer(std::string_view name, ProfileCategory category = ProfileCategory::Custom);
    ~ScopedProfileTimer();

  private:
    std::string_view m_name; ///< Non-owning view — caller must ensure lifetime (typically a string literal)
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
     * @param name Section name (must be a string literal or stable pointer — stored as string_view)
     */
    void BeginSection(std::string_view name, ProfileCategory category = ProfileCategory::Custom);

    /**
     * @brief End a named timing section
     * @param name Must match a previous BeginSection call
     */
    void EndSection(std::string_view name);

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
    void BeginGPUTimer(std::string_view name);

    /**
     * @brief End a GPU timing query
     */
    void EndGPUTimer(std::string_view name);

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
    void RecordAllocation(std::string_view category, size_t bytes);

    /**
     * @brief Record a memory deallocation
     */
    void RecordDeallocation(std::string_view category, size_t bytes);

    // ============================================================================
    // Query Results
    // ============================================================================

    float GetFrameTimeMs() const { return m_frameHistory.avgTime; }
    float GetFPS() const { return (m_frameHistory.avgTime > 0.0f) ? 1000.0f / m_frameHistory.avgTime : 0.0f; }
    double GetSectionTimeMs(std::string_view name) const;
    double GetGPUTimeMs(std::string_view name) const;
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

    // CPU timing — use string_view keys (pointing to stable caller storage, typically
    // string literals from PROFILE_SCOPE macros) to avoid per-frame heap allocations.
    // Samples are pre-reserved to avoid mid-frame reallocation.
    std::vector<ProfileSample> m_currentFrameSamples;
    std::unordered_map<std::string_view, std::chrono::high_resolution_clock::time_point> m_activeSections;
    std::unordered_map<std::string_view, double> m_sectionResults;
    static constexpr size_t kExpectedSectionsPerFrame = 64; ///< Pre-reserve hint

    // Per-category timing
    std::array<double, static_cast<size_t>(ProfileCategory::Count)> m_categoryTotals = {};

    // Frame timing history
    FrameTimingHistory m_frameHistory;
    std::chrono::high_resolution_clock::time_point m_frameStart;

#ifdef SPARK_PLATFORM_WINDOWS
    // GPU timing — string keys are kept as std::string here because GPU timer
    // names may come from dynamic sources, and timer creation is a cold path.
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    std::unordered_map<std::string, GPUTimerQuery, std::hash<std::string>, std::equal_to<>> m_gpuTimers;
#endif // SPARK_PLATFORM_WINDOWS

    // Memory tracking
    struct MemoryCategory
    {
        size_t currentBytes = 0;     ///< Currently allocated bytes in this category.
        size_t peakBytes = 0;        ///< High-water mark of allocated bytes.
        size_t totalAllocations = 0; ///< Lifetime count of allocations (for leak detection).
    };
    std::unordered_map<std::string, MemoryCategory, std::hash<std::string>, std::equal_to<>> m_memoryCategories;

    int m_frameCount = 0;
};

// Convenience macros for profiling
#ifdef PROFILING_ENABLED
#define PROFILE_SCOPE(name) ScopedProfileTimer _profile_##__LINE__(name)
#define PROFILE_SCOPE_CAT(name, cat) ScopedProfileTimer _profile_##__LINE__(name, cat)
#define PROFILE_BEGIN(name) Profiler::GetInstance().BeginSection(name)
#define PROFILE_END(name) Profiler::GetInstance().EndSection(name)
#ifdef SPARK_PLATFORM_WINDOWS
#define PROFILE_GPU_BEGIN(name) Profiler::GetInstance().BeginGPUTimer(name)
#define PROFILE_GPU_END(name) Profiler::GetInstance().EndGPUTimer(name)
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
