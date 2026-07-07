/**
 * @file Profiler.cpp
 * @brief Frame profiling and performance analysis implementation
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements hierarchical CPU profiling with scoped timers, per-frame
 * statistics, GPU timing queries (Windows/DX11), and memory tracking.
 */

#include "Profiler.h"
#include "Validate.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>

// ============================================================================
// ScopedProfileTimer
// ============================================================================

ScopedProfileTimer::ScopedProfileTimer(std::string_view name, ProfileCategory category)
    : m_name(name), m_category(category), m_start(std::chrono::high_resolution_clock::now())
{
    Profiler::GetInstance().BeginSection(m_name, m_category);
}

ScopedProfileTimer::~ScopedProfileTimer()
{
    Profiler::GetInstance().EndSection(m_name);
}

// ============================================================================
// Profiler - Initialization / Shutdown
// ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS
HRESULT Profiler::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    SPARK_WARN_IF_NULL(Spark::LogCategory::Core, device);
    SPARK_WARN_IF_NULL(Spark::LogCategory::Core, context);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device = device;
    m_context = context;
    m_enabled = true;
    m_frameCount = 0;
    m_categoryTotals.fill(0.0);
    return S_OK;
}
#endif

void Profiler::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Profiler shutting down");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = false;
    m_currentFrameSamples.clear();
    m_activeSections.clear();
    m_sectionResults.clear();
    m_categoryTotals.fill(0.0);
#ifdef SPARK_PLATFORM_WINDOWS
    m_gpuTimers.clear();
    m_device = nullptr;
    m_context = nullptr;
#endif
    m_memoryCategories.clear();
}

// ============================================================================
// CPU Timing
// ============================================================================

void Profiler::BeginSection(std::string_view name, ProfileCategory category)
{
    if (!m_enabled)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::high_resolution_clock::now();
    // Push onto the per-name stack so a nested BeginSection with the same name does
    // not overwrite the outer scope's start time.
    m_activeSections[name].push_back(now);

    ProfileSample sample;
    sample.name = name;
    sample.category = category;
    sample.depth = 0;
    sample.startTimeMs = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
    m_currentFrameSamples.push_back(sample);
}

void Profiler::EndSection(std::string_view name)
{
    if (!m_enabled)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_activeSections.find(name);
    if (it == m_activeSections.end() || it->second.empty())
    {
        return;
    }

    // Pop the innermost active start time for this name (LIFO).
    auto start = it->second.back();
    it->second.pop_back();
    if (it->second.empty())
    {
        m_activeSections.erase(it);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double durationMs = std::chrono::duration<double, std::milli>(end - start).count();

    m_sectionResults[name] = durationMs;

    // Reverse-search samples: the most recent unfinished sample with this name
    // is the innermost open scope — pairs correctly with LIFO nesting.
    for (auto rit = m_currentFrameSamples.rbegin(); rit != m_currentFrameSamples.rend(); ++rit)
    {
        if (rit->name == name && rit->durationMs == 0.0)
        {
            rit->durationMs = durationMs;

            auto catIdx = static_cast<size_t>(rit->category);
            if (catIdx < m_categoryTotals.size())
            {
                m_categoryTotals[catIdx] += durationMs;
            }
            break;
        }
    }
}

void Profiler::BeginFrame()
{
    if (!m_enabled)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    m_frameStart = std::chrono::high_resolution_clock::now();

    // Clear but retain capacity — avoids per-frame heap allocations.
    m_currentFrameSamples.clear();
    if (m_currentFrameSamples.capacity() < kExpectedSectionsPerFrame)
    {
        m_currentFrameSamples.reserve(kExpectedSectionsPerFrame);
    }
    m_categoryTotals.fill(0.0);
    m_sectionResults.clear();
}

void Profiler::EndFrame()
{
    if (!m_enabled)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto end = std::chrono::high_resolution_clock::now();
    float frameMs = std::chrono::duration<float, std::milli>(end - m_frameStart).count();
    m_frameHistory.Push(frameMs);
    m_frameCount++;
}

// ============================================================================
// GPU Timing
// ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

void Profiler::BeginGPUTimer(std::string_view name)
{
    if (!m_enabled || !m_device || !m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto& timer = m_gpuTimers[std::string(name)];

    if (!timer.beginQuery)
    {
        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_TIMESTAMP;
        HRESULT hr1 = m_device->CreateQuery(&desc, timer.beginQuery.GetAddressOf());
        HRESULT hr2 = m_device->CreateQuery(&desc, timer.endQuery.GetAddressOf());

        D3D11_QUERY_DESC disjointDesc = {};
        disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        HRESULT hr3 = m_device->CreateQuery(&disjointDesc, timer.disjointQuery.GetAddressOf());

        if (FAILED(hr1) || FAILED(hr2) || FAILED(hr3))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Profiler: failed to create GPU timestamp queries");
            timer.beginQuery.Reset();
            timer.endQuery.Reset();
            timer.disjointQuery.Reset();
            return;
        }
    }

    m_context->Begin(timer.disjointQuery.Get());
    m_context->End(timer.beginQuery.Get());
    timer.pending = true;
}

void Profiler::EndGPUTimer(std::string_view name)
{
    if (!m_enabled || !m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_gpuTimers.find(std::string(name));
    if (it == m_gpuTimers.end())
    {
        return;
    }

    m_context->End(it->second.endQuery.Get());
    m_context->End(it->second.disjointQuery.Get());
}

void Profiler::ResolveGPUQueries()
{
    if (!m_enabled || !m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [name, timer] : m_gpuTimers)
    {
        if (!timer.pending)
        {
            continue;
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData;
        if (m_context->GetData(timer.disjointQuery.Get(), &disjointData, sizeof(disjointData),
                               D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            continue;
        }

        if (disjointData.Disjoint)
        {
            timer.pending = false;
            continue;
        }

        UINT64 beginTime = 0, endTime = 0;
        if (m_context->GetData(timer.beginQuery.Get(), &beginTime, sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) !=
            S_OK)
        {
            continue;
        }
        if (m_context->GetData(timer.endQuery.Get(), &endTime, sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            continue;
        }

        timer.resultMs =
            static_cast<double>(endTime - beginTime) / static_cast<double>(disjointData.Frequency) * 1000.0;
        timer.pending = false;
    }
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Memory Tracking
// ============================================================================

void Profiler::RecordAllocation(std::string_view category, size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& cat = m_memoryCategories[std::string(category)];
    cat.currentBytes += bytes;
    cat.totalAllocations++;
    if (cat.currentBytes > cat.peakBytes)
    {
        cat.peakBytes = cat.currentBytes;
    }
}

void Profiler::RecordDeallocation(std::string_view category, size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& cat = m_memoryCategories[std::string(category)];
    if (bytes <= cat.currentBytes)
    {
        cat.currentBytes -= bytes;
    }
    else
    {
        cat.currentBytes = 0;
    }
}

// ============================================================================
// Query Results
// ============================================================================

double Profiler::GetSectionTimeMs(std::string_view name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sectionResults.find(name);
    if (it != m_sectionResults.end())
    {
        return it->second;
    }
    return 0.0;
}

// Intentional: name used only inside #ifdef SPARK_PLATFORM_WINDOWS
double Profiler::GetGPUTimeMs([[maybe_unused]] std::string_view name) const
{
#ifdef SPARK_PLATFORM_WINDOWS
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_gpuTimers.find(std::string(name));
    if (it != m_gpuTimers.end())
    {
        return it->second.resultMs;
    }
#endif
    return 0.0;
}

double Profiler::GetCategoryTimeMs(ProfileCategory category) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto idx = static_cast<size_t>(category);
    if (idx < m_categoryTotals.size())
    {
        return m_categoryTotals[idx];
    }
    return 0.0;
}

// ============================================================================
// Console Integration
// ============================================================================

std::string Profiler::Console_GetReport() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ostringstream ss;
    ss << "=== CPU Profiler Report ===\n";
    ss << std::fixed << std::setprecision(2);
    ss << "Frame: " << m_frameHistory.avgTime << " ms ("
       << (m_frameHistory.avgTime > 0.0f ? 1000.0f / m_frameHistory.avgTime : 0.0f) << " FPS)\n";
    ss << "Min: " << m_frameHistory.minTime << " ms, Max: " << m_frameHistory.maxTime << " ms\n\n";

    ss << "--- Section Timings ---\n";
    for (const auto& [name, timeMs] : m_sectionResults)
    {
        ss << "  " << name << ": " << timeMs << " ms\n";
    }

    ss << "\n--- Category Totals ---\n";
    const char* catNames[] = {"Frame", "Render", "Physics", "Audio", "GameLogic", "Input", "Particles", "UI", "Custom"};
    for (size_t i = 0; i < m_categoryTotals.size(); ++i)
    {
        if (m_categoryTotals[i] > 0.001)
        {
            ss << "  " << catNames[i] << ": " << m_categoryTotals[i] << " ms\n";
        }
    }

    return ss.str();
}

std::string Profiler::Console_GetGPUReport() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ostringstream ss;
    ss << "=== GPU Profiler Report ===\n";
    ss << std::fixed << std::setprecision(2);

#ifdef SPARK_PLATFORM_WINDOWS
    for (const auto& [name, timer] : m_gpuTimers)
    {
        ss << "  " << name << ": " << timer.resultMs << " ms\n";
    }
#else
    ss << "  GPU timing not available on this platform.\n";
#endif

    return ss.str();
}

std::string Profiler::Console_GetMemoryReport() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ostringstream ss;
    ss << "=== Memory Report ===\n";
    for (const auto& [name, cat] : m_memoryCategories)
    {
        ss << "  " << name << ": " << (cat.currentBytes / 1024) << " KB current, " << (cat.peakBytes / 1024)
           << " KB peak, " << cat.totalAllocations << " allocations\n";
    }
    return ss.str();
}

void Profiler::Console_ExportCSV(const std::string& filepath) const
{
    std::ofstream file(filepath);
    if (!file.is_open())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    file << "Section,TimeMs\n";
    for (const auto& [name, timeMs] : m_sectionResults)
    {
        file << name << "," << timeMs << "\n";
    }

    file << "\nCategory,TimeMs\n";
    const char* catNames[] = {"Frame", "Render", "Physics", "Audio", "GameLogic", "Input", "Particles", "UI", "Custom"};
    for (size_t i = 0; i < m_categoryTotals.size(); ++i)
    {
        file << catNames[i] << "," << m_categoryTotals[i] << "\n";
    }

    file << "\nMemoryCategory,CurrentKB,PeakKB,Allocations\n";
    for (const auto& [name, cat] : m_memoryCategories)
    {
        file << name << "," << (cat.currentBytes / 1024) << "," << (cat.peakBytes / 1024) << "," << cat.totalAllocations
             << "\n";
    }
}
