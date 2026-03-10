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
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>

// ============================================================================
// ScopedProfileTimer
// ============================================================================

ScopedProfileTimer::ScopedProfileTimer(const std::string& name, ProfileCategory category)
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

void Profiler::BeginSection(const std::string& name, ProfileCategory category)
{
    if (!m_enabled)
    {
        return;
    }

    m_activeSections[name] = std::chrono::high_resolution_clock::now();

    ProfileSample sample;
    sample.name = name;
    sample.category = category;
    sample.depth = 0; // Depth tracking could be enhanced with a stack
    sample.startTimeMs = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
    m_currentFrameSamples.push_back(sample);
}

void Profiler::EndSection(const std::string& name)
{
    if (!m_enabled)
    {
        return;
    }

    auto it = m_activeSections.find(name);
    if (it == m_activeSections.end())
    {
        return;
    }

    auto end = std::chrono::high_resolution_clock::now();
    double durationMs = std::chrono::duration<double, std::milli>(end - it->second).count();

    m_sectionResults[name] = durationMs;
    m_activeSections.erase(it);

    // Update the corresponding sample's duration
    for (auto& sample : m_currentFrameSamples)
    {
        if (sample.name == name && sample.durationMs == 0.0)
        {
            sample.durationMs = durationMs;

            // Add to category total
            auto catIdx = static_cast<size_t>(sample.category);
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

    m_frameStart = std::chrono::high_resolution_clock::now();
    m_currentFrameSamples.clear();
    m_categoryTotals.fill(0.0);
    m_sectionResults.clear();
}

void Profiler::EndFrame()
{
    if (!m_enabled)
    {
        return;
    }

    auto end = std::chrono::high_resolution_clock::now();
    float frameMs = std::chrono::duration<float, std::milli>(end - m_frameStart).count();
    m_frameHistory.Push(frameMs);
    m_frameCount++;
}

// ============================================================================
// GPU Timing
// ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

void Profiler::BeginGPUTimer(const std::string& name)
{
    if (!m_enabled || !m_device || !m_context)
    {
        return;
    }

    auto& timer = m_gpuTimers[name];

    if (!timer.beginQuery)
    {
        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_TIMESTAMP;
        m_device->CreateQuery(&desc, timer.beginQuery.GetAddressOf());
        m_device->CreateQuery(&desc, timer.endQuery.GetAddressOf());

        D3D11_QUERY_DESC disjointDesc = {};
        disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        m_device->CreateQuery(&disjointDesc, timer.disjointQuery.GetAddressOf());
    }

    m_context->Begin(timer.disjointQuery.Get());
    m_context->End(timer.beginQuery.Get());
    timer.pending = true;
}

void Profiler::EndGPUTimer(const std::string& name)
{
    if (!m_enabled || !m_context)
    {
        return;
    }

    auto it = m_gpuTimers.find(name);
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

    for (auto& [name, timer] : m_gpuTimers)
    {
        if (!timer.pending)
        {
            continue;
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData;
        if (m_context->GetData(timer.disjointQuery.Get(), &disjointData,
                               sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            continue;
        }

        if (disjointData.Disjoint)
        {
            timer.pending = false;
            continue;
        }

        UINT64 beginTime = 0, endTime = 0;
        if (m_context->GetData(timer.beginQuery.Get(), &beginTime,
                               sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            continue;
        }
        if (m_context->GetData(timer.endQuery.Get(), &endTime,
                               sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            continue;
        }

        timer.resultMs = static_cast<double>(endTime - beginTime) /
                         static_cast<double>(disjointData.Frequency) * 1000.0;
        timer.pending = false;
    }
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Memory Tracking
// ============================================================================

void Profiler::RecordAllocation(const std::string& category, size_t bytes)
{
    auto& cat = m_memoryCategories[category];
    cat.currentBytes += bytes;
    cat.totalAllocations++;
    if (cat.currentBytes > cat.peakBytes)
    {
        cat.peakBytes = cat.currentBytes;
    }
}

void Profiler::RecordDeallocation(const std::string& category, size_t bytes)
{
    auto& cat = m_memoryCategories[category];
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

double Profiler::GetSectionTimeMs(const std::string& name) const
{
    auto it = m_sectionResults.find(name);
    if (it != m_sectionResults.end())
    {
        return it->second;
    }
    return 0.0;
}

double Profiler::GetGPUTimeMs([[maybe_unused]] const std::string& name) const
{
#ifdef SPARK_PLATFORM_WINDOWS
    auto it = m_gpuTimers.find(name);
    if (it != m_gpuTimers.end())
    {
        return it->second.resultMs;
    }
#endif
    return 0.0;
}

double Profiler::GetCategoryTimeMs(ProfileCategory category) const
{
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
    const char* catNames[] = {"Frame", "Render", "Physics", "Audio",
                              "GameLogic", "Input", "Particles", "UI", "Custom"};
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
    std::ostringstream ss;
    ss << "=== Memory Report ===\n";
    for (const auto& [name, cat] : m_memoryCategories)
    {
        ss << "  " << name << ": "
           << (cat.currentBytes / 1024) << " KB current, "
           << (cat.peakBytes / 1024) << " KB peak, "
           << cat.totalAllocations << " allocations\n";
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

    file << "Section,TimeMs\n";
    for (const auto& [name, timeMs] : m_sectionResults)
    {
        file << name << "," << timeMs << "\n";
    }

    file << "\nCategory,TimeMs\n";
    const char* catNames[] = {"Frame", "Render", "Physics", "Audio",
                              "GameLogic", "Input", "Particles", "UI", "Custom"};
    for (size_t i = 0; i < m_categoryTotals.size(); ++i)
    {
        file << catNames[i] << "," << m_categoryTotals[i] << "\n";
    }

    file << "\nMemoryCategory,CurrentKB,PeakKB,Allocations\n";
    for (const auto& [name, cat] : m_memoryCategories)
    {
        file << name << ","
             << (cat.currentBytes / 1024) << ","
             << (cat.peakBytes / 1024) << ","
             << cat.totalAllocations << "\n";
    }
}
