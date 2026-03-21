/**
 * @file PerformanceProfiler.cpp
 * @brief Implementation of the Performance Profiler panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "../Profiler/PerformanceProfiler.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <fstream>

using namespace DirectX;
namespace SparkEditor
{

    // Global profiler instance
    PerformanceProfiler* g_profiler = nullptr;

    // ============================================================================
    // PerformanceCounter Implementation
    // ============================================================================

    void PerformanceCounter::AddSample(float value)
    {
        currentValue = value;
        if (value < minValue)
            minValue = value;
        if (value > maxValue)
            maxValue = value;

        history.push_back(value);
        if (static_cast<int>(history.size()) > historySize)
        {
            history.erase(history.begin());
        }

        // Recalculate average
        float sum = 0.0f;
        for (float v : history)
            sum += v;
        averageValue = history.empty() ? 0.0f : sum / static_cast<float>(history.size());

        lastUpdate = std::chrono::steady_clock::now();
    }

    void PerformanceCounter::Clear()
    {
        currentValue = 0.0f;
        minValue = FLT_MAX;
        maxValue = -FLT_MAX;
        averageValue = 0.0f;
        history.clear();
    }

    float PerformanceCounter::GetSmoothedValue(float smoothingFactor) const
    {
        if (history.size() < 2)
            return currentValue;
        float smoothed = history[0];
        for (size_t i = 1; i < history.size(); ++i)
        {
            smoothed = smoothingFactor * history[i] + (1.0f - smoothingFactor) * smoothed;
        }
        return smoothed;
    }

    // ============================================================================
    // CPUProfileSample Implementation
    // ============================================================================

    float CPUProfileSample::GetSelfTime() const
    {
        float childTime = 0.0f;
        for (const auto& child : children)
        {
            childTime += child->GetTotalTime();
        }
        return duration - childTime;
    }

    float CPUProfileSample::GetTotalTime() const
    {
        return duration;
    }

    // ============================================================================
    // ProfileScope Implementation
    // ============================================================================

    ProfileScope::ProfileScope(const std::string& name, const std::string& /*category*/)
        : m_name(name), m_startTime(std::chrono::high_resolution_clock::now()), m_ended(false)
    {
    }

    ProfileScope::~ProfileScope()
    {
        if (!m_ended)
            End();
    }

    void ProfileScope::End()
    {
        m_ended = true;
    }

    // ============================================================================
    // PerformanceProfiler Implementation
    // ============================================================================

    PerformanceProfiler::PerformanceProfiler() : EditorPanel("Performance Profiler", "performance_profiler_panel") {}

    PerformanceProfiler::~PerformanceProfiler() {}

    bool PerformanceProfiler::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Performance Profiler panel\n";
        m_isProfiling = true;
        return true;
    }

    void PerformanceProfiler::Update(float deltaTime)
    {
        if (!m_isProfiling)
            return;

        // Update frame data
        UpdateFrameData();

        m_currentFrameNumber++;
    }

    void PerformanceProfiler::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Toolbar
            if (m_isProfiling)
            {
                if (ImGui::Button("Stop Profiling"))
                {
                    StopProfiling();
                }
            }
            else
            {
                if (ImGui::Button("Start Profiling"))
                {
                    StartProfiling();
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Clear Data"))
            {
                ClearProfilingData();
            }

            ImGui::Separator();

            // Tab bar for different views
            if (ImGui::BeginTabBar("ProfilerTabs"))
            {
                if (m_showOverview && ImGui::BeginTabItem("Overview"))
                {
                    RenderOverviewPanel();
                    ImGui::EndTabItem();
                }
                if (m_showCPUProfiler && ImGui::BeginTabItem("CPU"))
                {
                    RenderCPUProfilerPanel();
                    ImGui::EndTabItem();
                }
                if (m_showGPUProfiler && ImGui::BeginTabItem("GPU"))
                {
                    RenderGPUProfilerPanel();
                    ImGui::EndTabItem();
                }
                if (m_showMemoryProfiler && ImGui::BeginTabItem("Memory"))
                {
                    RenderMemoryProfilerPanel();
                    ImGui::EndTabItem();
                }
                if (m_showCounters && ImGui::BeginTabItem("Counters"))
                {
                    RenderPerformanceCountersPanel();
                    ImGui::EndTabItem();
                }
                if (m_showOptimization && ImGui::BeginTabItem("Optimization"))
                {
                    RenderOptimizationPanel();
                    ImGui::EndTabItem();
                }
                if (m_showConfiguration && ImGui::BeginTabItem("Configuration"))
                {
                    RenderConfigurationPanel();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void PerformanceProfiler::Shutdown()
    {
        std::cout << "Shutting down Performance Profiler panel\n";
        StopProfiling();
    }

    bool PerformanceProfiler::HandleEvent(const std::string& /*eventType*/, void* /*eventData*/)
    {
        return false;
    }

    void PerformanceProfiler::StartProfiling()
    {
        m_isProfiling = true;
        std::cout << "Started profiling session\n";
    }

    void PerformanceProfiler::StopProfiling()
    {
        m_isProfiling = false;
        std::cout << "Stopped profiling session\n";
    }

    uint32_t PerformanceProfiler::BeginCPUSample(const std::string& name, const std::string& category)
    {
        if (!m_isProfiling)
            return 0;

        std::lock_guard<std::mutex> lock(m_cpuSampleMutex);

        auto sample = std::make_unique<CPUProfileSample>();
        sample->name = name;
        sample->category = category;
        sample->startTime = std::chrono::high_resolution_clock::now();

        uint32_t id = m_nextCPUSampleID++;
        m_cpuSampleMap[id] = sample.get();
        m_activeCPUSamples.push_back(std::move(sample));

        return id;
    }

    void PerformanceProfiler::EndCPUSample(uint32_t sampleID)
    {
        if (!m_isProfiling)
            return;

        std::lock_guard<std::mutex> lock(m_cpuSampleMutex);

        auto it = m_cpuSampleMap.find(sampleID);
        if (it != m_cpuSampleMap.end())
        {
            auto* sample = it->second;
            sample->endTime = std::chrono::high_resolution_clock::now();
            sample->duration = std::chrono::duration<float, std::milli>(sample->endTime - sample->startTime).count();
            m_cpuSampleMap.erase(it);
        }
    }

    void PerformanceProfiler::SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        m_device = device;
        m_context = context;
    }

    void PerformanceProfiler::BeginGPUSample(const std::string& name, const std::string& shaderName)
    {
        if (!m_isProfiling)
            return;

        GPUProfileSample sample;
        sample.name = name;
        sample.shaderName = shaderName;
        m_activeGPUSamples[name] = sample;

        // Issue a D3D11 timestamp query if device is available
        if (m_device && m_context)
        {
            // Start the per-frame disjoint query if not already active
            if (!m_disjointActive)
            {
                D3D11_QUERY_DESC disjointDesc = {};
                disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                HRESULT hr = m_device->CreateQuery(&disjointDesc, m_disjointQuery.ReleaseAndGetAddressOf());
                if (SUCCEEDED(hr))
                {
                    m_context->Begin(m_disjointQuery.Get());
                    m_disjointActive = true;
                }
            }

            GPUQueryPair queryPair;
            queryPair.name = name;
            queryPair.shaderName = shaderName;

            D3D11_QUERY_DESC tsDesc = {};
            tsDesc.Query = D3D11_QUERY_TIMESTAMP;

            HRESULT hr = m_device->CreateQuery(&tsDesc, queryPair.beginQuery.GetAddressOf());
            if (SUCCEEDED(hr))
            {
                hr = m_device->CreateQuery(&tsDesc, queryPair.endQuery.GetAddressOf());
            }

            if (SUCCEEDED(hr))
            {
                m_context->End(queryPair.beginQuery.Get()); // Timestamp queries use End(), not Begin()
                queryPair.begun = true;
                m_pendingGPUQueries[name] = std::move(queryPair);
            }
        }
    }

    void PerformanceProfiler::EndGPUSample(const std::string& name)
    {
        if (!m_isProfiling)
            return;

        // Issue the end timestamp query
        if (m_context)
        {
            auto it = m_pendingGPUQueries.find(name);
            if (it != m_pendingGPUQueries.end() && it->second.begun)
            {
                m_context->End(it->second.endQuery.Get());
                it->second.ended = true;
            }
        }

        m_activeGPUSamples.erase(name);
    }

    void PerformanceProfiler::RecordMemoryAllocation(const std::string& category, size_t bytes, void* pointer)
    {
        std::lock_guard<std::mutex> lock(m_memoryMutex);

        if (pointer)
        {
            m_memoryAllocations[pointer] = {category, bytes};
        }
        m_memoryCategories[category].allocatedBytes += bytes;
        m_memoryCategories[category].allocationCount++;
        m_memoryCategories[category].totalAllocatedBytes += bytes;
    }

    void PerformanceProfiler::RecordMemoryDeallocation(void* pointer)
    {
        std::lock_guard<std::mutex> lock(m_memoryMutex);

        auto it = m_memoryAllocations.find(pointer);
        if (it != m_memoryAllocations.end())
        {
            auto& [category, bytes] = it->second;
            m_memoryCategories[category].allocatedBytes -= bytes;
            m_memoryCategories[category].deallocationCount++;
            m_memoryAllocations.erase(it);
        }
    }

    uint32_t PerformanceProfiler::AddPerformanceCounter(const std::string& name, ProfilerSampleType type,
                                                        const std::string& unit)
    {
        PerformanceCounter counter;
        counter.name = name;
        counter.type = type;
        counter.unit = unit;
        m_performanceCounters.push_back(counter);
        return m_nextCounterID++;
    }

    void PerformanceProfiler::UpdatePerformanceCounter(uint32_t counterID, float value)
    {
        if (counterID > 0 && counterID <= m_performanceCounters.size())
        {
            m_performanceCounters[counterID - 1].AddSample(value);
        }
    }

    const FrameProfileData* PerformanceProfiler::GetCurrentFrame() const
    {
        return m_currentFrame.get();
    }

    const FrameProfileData* PerformanceProfiler::GetFrame(int frameIndex) const
    {
        if (frameIndex < 0 || frameIndex >= static_cast<int>(m_frameHistory.size()))
        {
            return nullptr;
        }
        return m_frameHistory[m_frameHistory.size() - 1 - frameIndex].get();
    }

    bool PerformanceProfiler::ApplyOptimization(int suggestionIndex)
    {
        if (suggestionIndex < 0 || suggestionIndex >= static_cast<int>(m_optimizationSuggestions.size()))
        {
            return false;
        }
        auto& suggestion = m_optimizationSuggestions[suggestionIndex];
        if (suggestion.isAutomatable && suggestion.automateFunction)
        {
            return suggestion.automateFunction();
        }
        return false;
    }

    bool PerformanceProfiler::ExportProfilingData(const std::string& filePath, const std::string& format)
    {
        try
        {
            std::ofstream file(filePath);
            if (!file.is_open())
                return false;

            if (format == "json")
            {
                file << "{\n  \"profiling_data\": {\n";
                file << "    \"frame_count\": " << m_frameHistory.size() << ",\n";
                file << "    \"frames\": [\n";
                for (size_t i = 0; i < m_frameHistory.size(); ++i)
                {
                    const auto& frame = m_frameHistory[i];
                    file << "      {\n";
                    file << "        \"frameNumber\": " << frame->frameNumber << ",\n";
                    file << "        \"frameTime\": " << std::fixed << std::setprecision(3) << frame->frameTime
                         << ",\n";
                    file << "        \"fps\": " << frame->fps << ",\n";
                    file << "        \"cpuTime\": " << frame->cpuTime << ",\n";
                    file << "        \"gpuTime\": " << frame->gpuTime << ",\n";
                    file << "        \"drawCalls\": " << frame->drawCalls << ",\n";
                    file << "        \"triangles\": " << frame->triangles << ",\n";
                    file << "        \"systemMemoryUsage\": " << frame->systemMemoryUsage << ",\n";
                    file << "        \"videoMemoryUsage\": " << frame->videoMemoryUsage << "\n";
                    file << "      }" << (i + 1 < m_frameHistory.size() ? "," : "") << "\n";
                }
                file << "    ]\n  }\n}\n";
            }
            else if (format == "csv")
            {
                file << "frameNumber,frameTime,fps,cpuTime,gpuTime,drawCalls,triangles,systemMemory,videoMemory\n";
                for (const auto& frame : m_frameHistory)
                {
                    file << frame->frameNumber << "," << std::fixed << std::setprecision(3) << frame->frameTime << ","
                         << frame->fps << "," << frame->cpuTime << "," << frame->gpuTime << "," << frame->drawCalls
                         << "," << frame->triangles << "," << frame->systemMemoryUsage << "," << frame->videoMemoryUsage
                         << "\n";
                }
            }
            else
            {
                file << "Spark Engine Profiling Data Export\n";
                file << "================================\n";
                file << "Total Frames: " << m_frameHistory.size() << "\n\n";
                for (const auto& frame : m_frameHistory)
                {
                    file << "Frame: " << frame->frameNumber << "  Time: " << std::fixed << std::setprecision(2)
                         << frame->frameTime << "ms" << "  FPS: " << frame->fps << "  CPU: " << frame->cpuTime << "ms"
                         << "  GPU: " << frame->gpuTime << "ms" << "  Draw Calls: " << frame->drawCalls << "\n";
                }
            }

            file.close();
            std::cout << "Exported profiling data to: " << filePath << " (format: " << format << ")\n";
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool PerformanceProfiler::ImportProfilingData(const std::string& filePath)
    {
        try
        {
            std::ifstream file(filePath);
            if (!file.is_open())
                return false;

            ClearProfilingData();
            std::string line;
            std::unique_ptr<FrameProfileData> frame;

            while (std::getline(file, line))
            {
                // Detect frame boundaries
                if (line.contains("\"frameNumber\"") || line.contains("Frame:"))
                {
                    if (frame)
                    {
                        m_frameHistory.push_back(std::move(frame));
                    }
                    frame = std::make_unique<FrameProfileData>();
                }
                if (!frame)
                {
                    frame = std::make_unique<FrameProfileData>();
                }

                // Parse key-value pairs from JSON or text format
                auto extractFloat = [&](const std::string& key) -> float
                {
                    auto pos = line.find(key);
                    if (pos == std::string::npos)
                        return -1.0f;
                    pos += key.length();
                    while (pos < line.size() && !std::isdigit(line[pos]) && line[pos] != '-')
                        pos++;
                    if (pos >= line.size())
                        return -1.0f;
                    try
                    {
                        return std::stof(line.substr(pos));
                    }
                    catch (...)
                    {
                        return -1.0f;
                    }
                };

                float val;
                if ((val = extractFloat("frameTime")) >= 0)
                    frame->frameTime = val;
                if ((val = extractFloat("fps")) >= 0)
                    frame->fps = val;
                if ((val = extractFloat("cpuTime")) >= 0)
                    frame->cpuTime = val;
                if ((val = extractFloat("gpuTime")) >= 0)
                    frame->gpuTime = val;
                if ((val = extractFloat("drawCalls")) >= 0)
                    frame->drawCalls = static_cast<int>(val);
                if ((val = extractFloat("triangles")) >= 0)
                    frame->triangles = static_cast<int>(val);
            }

            if (frame)
            {
                m_frameHistory.push_back(std::move(frame));
            }

            file.close();
            std::cout << "Imported profiling data from: " << filePath << " (" << m_frameHistory.size() << " frames)\n";
            return !m_frameHistory.empty();
        }
        catch (...)
        {
            return false;
        }
    }

    void PerformanceProfiler::ClearProfilingData()
    {
        m_frameHistory.clear();
        m_currentFrame.reset();
        m_activeCPUSamples.clear();
        m_cpuSampleMap.clear();
        m_activeGPUSamples.clear();
        m_performanceCounters.clear();
        m_detectedBottlenecks.clear();
        m_optimizationSuggestions.clear();
        m_snapshots.clear();
        std::cout << "Cleared all profiling data\n";
    }

    void PerformanceProfiler::SetConfiguration(const ProfilerConfig& config)
    {
        m_config = config;
    }

    uint32_t PerformanceProfiler::TakeSnapshot(const std::string& name)
    {
        PerformanceSnapshot snapshot;
        snapshot.name = name;
        snapshot.timestamp = std::chrono::steady_clock::now();
        if (m_currentFrame)
        {
            snapshot.frameData.frameNumber = m_currentFrame->frameNumber;
            snapshot.frameData.frameTime = m_currentFrame->frameTime;
            snapshot.frameData.fps = m_currentFrame->fps;
            snapshot.frameData.cpuTime = m_currentFrame->cpuTime;
            snapshot.frameData.gpuTime = m_currentFrame->gpuTime;
            snapshot.frameData.drawCalls = m_currentFrame->drawCalls;
        }
        snapshot.counters = m_performanceCounters;
        m_snapshots.push_back(std::move(snapshot));
        return m_nextSnapshotID++;
    }

    std::string PerformanceProfiler::CompareSnapshots(uint32_t snapshot1, uint32_t snapshot2)
    {
        if (snapshot1 == 0 || snapshot2 == 0 || snapshot1 > m_snapshots.size() || snapshot2 > m_snapshots.size())
        {
            return "Invalid snapshot IDs";
        }

        const auto& s1 = m_snapshots[snapshot1 - 1];
        const auto& s2 = m_snapshots[snapshot2 - 1];

        std::stringstream ss;
        ss << "Comparing: " << s1.name << " vs " << s2.name << "\n";
        ss << "Frame time: " << s1.frameData.frameTime << " vs " << s2.frameData.frameTime << " ms\n";
        return ss.str();
    }

    std::string PerformanceProfiler::GetTrendAnalysis(const std::string& metric, float timespan)
    {
        if (m_frameHistory.empty())
        {
            return "No profiling data available for trend analysis.";
        }

        // Collect metric values over the timespan
        std::vector<float> values;
        auto now = std::chrono::steady_clock::now();

        for (const auto& frame : m_frameHistory)
        {
            float age = std::chrono::duration<float>(now - frame->timestamp).count();
            if (age > timespan)
                continue;

            float val = 0.0f;
            if (metric == "frameTime")
                val = frame->frameTime;
            else if (metric == "fps")
                val = frame->fps;
            else if (metric == "cpuTime")
                val = frame->cpuTime;
            else if (metric == "gpuTime")
                val = frame->gpuTime;
            else if (metric == "drawCalls")
                val = static_cast<float>(frame->drawCalls);
            else if (metric == "triangles")
                val = static_cast<float>(frame->triangles);
            else
                val = frame->frameTime; // default to frame time

            values.push_back(val);
        }

        if (values.empty())
        {
            return "No data points found for metric '" + metric + "' in the specified timespan.";
        }

        // Calculate statistics
        float sum = 0.0f, minVal = FLT_MAX, maxVal = -FLT_MAX;
        for (float v : values)
        {
            sum += v;
            if (v < minVal)
                minVal = v;
            if (v > maxVal)
                maxVal = v;
        }
        float avg = sum / static_cast<float>(values.size());

        // Calculate standard deviation
        float varianceSum = 0.0f;
        for (float v : values)
        {
            float diff = v - avg;
            varianceSum += diff * diff;
        }
        float stddev = std::sqrt(varianceSum / static_cast<float>(values.size()));

        // Determine trend by comparing first half vs second half averages
        size_t mid = values.size() / 2;
        float firstHalfAvg = 0.0f, secondHalfAvg = 0.0f;
        for (size_t i = 0; i < mid; ++i)
            firstHalfAvg += values[i];
        for (size_t i = mid; i < values.size(); ++i)
            secondHalfAvg += values[i];
        if (mid > 0)
            firstHalfAvg /= static_cast<float>(mid);
        if (values.size() - mid > 0)
            secondHalfAvg /= static_cast<float>(values.size() - mid);

        float changePercent = (firstHalfAvg != 0.0f) ? ((secondHalfAvg - firstHalfAvg) / firstHalfAvg) * 100.0f : 0.0f;

        std::string trend;
        if (changePercent > 5.0f)
            trend = "DEGRADING";
        else if (changePercent < -5.0f)
            trend = "IMPROVING";
        else
            trend = "STABLE";

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "Trend Analysis for '" << metric << "' (last " << timespan << "s):\n";
        ss << "  Samples: " << values.size() << "\n";
        ss << "  Average: " << avg << "\n";
        ss << "  Min: " << minVal << "  Max: " << maxVal << "\n";
        ss << "  Std Dev: " << stddev << "\n";
        ss << "  Trend: " << trend << " (" << std::showpos << changePercent << "%)\n";

        return ss.str();
    }

    // ============================================================================
    // Private Methods
    // ============================================================================

    void PerformanceProfiler::RenderOverviewPanel()
    {
        ImGui::Text("Performance Overview");
        ImGui::Separator();

        if (m_currentFrame)
        {
            ImGui::Text("Frame: %d", m_currentFrame->frameNumber);
            ImGui::Text("Frame Time: %.2f ms", m_currentFrame->frameTime);
            ImGui::Text("FPS: %.1f", m_currentFrame->fps);
            ImGui::Text("Draw Calls: %d", m_currentFrame->drawCalls);
        }
        else
        {
            ImGui::Text("No profiling data available");
        }
    }

    void PerformanceProfiler::RenderCPUProfilerPanel()
    {
        ImGui::Text("CPU Profiler");
        ImGui::Separator();

        if (m_currentFrame && !m_currentFrame->cpuSamples.empty())
        {
            for (const auto& sample : m_currentFrame->cpuSamples)
            {
                RenderCPUSampleHierarchy(sample.get(), 0);
            }
        }
        else
        {
            ImGui::Text("No CPU samples available");
        }
    }

    void PerformanceProfiler::RenderGPUProfilerPanel()
    {
        ImGui::Text("GPU Profiler");
        ImGui::Separator();

        if (m_currentFrame && !m_currentFrame->gpuSamples.empty())
        {
            for (const auto& sample : m_currentFrame->gpuSamples)
            {
                ImGui::Text("%s: %.2f ms (%d draw calls)", sample.name.c_str(), sample.duration, sample.drawCalls);
            }
        }
        else
        {
            ImGui::Text("No GPU samples available");
        }
    }

    void PerformanceProfiler::RenderMemoryProfilerPanel()
    {
        ImGui::Text("Memory Profiler");
        ImGui::Separator();

        std::lock_guard<std::mutex> lock(m_memoryMutex);
        for (const auto& [category, sample] : m_memoryCategories)
        {
            float mb = static_cast<float>(sample.allocatedBytes) / (1024.0f * 1024.0f);
            ImGui::Text("%s: %.2f MB (%d allocs)", category.c_str(), mb, sample.allocationCount);
        }

        if (m_memoryCategories.empty())
        {
            ImGui::Text("No memory data available");
        }
    }

    void PerformanceProfiler::RenderPerformanceCountersPanel()
    {
        ImGui::Text("Performance Counters");
        ImGui::Separator();

        if (ImGui::BeginTable("CountersTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("Average");
            ImGui::TableSetupColumn("Min/Max");
            ImGui::TableSetupColumn("Unit");
            ImGui::TableHeadersRow();

            for (const auto& counter : m_performanceCounters)
            {
                if (!counter.isActive)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", counter.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.2f", counter.currentValue);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f", counter.averageValue);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f / %.2f", counter.minValue, counter.maxValue);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", counter.unit.c_str());
            }

            ImGui::EndTable();
        }
    }

    void PerformanceProfiler::RenderOptimizationPanel()
    {
        ImGui::Text("Optimization Suggestions");
        ImGui::Separator();

        if (m_optimizationSuggestions.empty())
        {
            ImGui::Text("No optimization suggestions");
            return;
        }

        for (int i = 0; i < static_cast<int>(m_optimizationSuggestions.size()); ++i)
        {
            const auto& suggestion = m_optimizationSuggestions[i];
            ImGui::Text("[%s] %s",
                        suggestion.priority == OptimizationSuggestion::CRITICAL ? "CRITICAL"
                        : suggestion.priority == OptimizationSuggestion::HIGH   ? "HIGH"
                        : suggestion.priority == OptimizationSuggestion::MEDIUM ? "MEDIUM"
                                                                                : "LOW",
                        suggestion.title.c_str());
            ImGui::Text("  %s", suggestion.description.c_str());
            if (suggestion.isAutomatable && ImGui::Button(("Apply##" + std::to_string(i)).c_str()))
            {
                ApplyOptimization(i);
            }
            ImGui::Separator();
        }
    }

    void PerformanceProfiler::RenderConfigurationPanel()
    {
        ImGui::Text("Profiler Configuration");
        ImGui::Separator();

        ImGui::Checkbox("CPU Profiling", &m_config.enableCPUProfiling);
        ImGui::Checkbox("GPU Profiling", &m_config.enableGPUProfiling);
        ImGui::Checkbox("Memory Profiling", &m_config.enableMemoryProfiling);
        ImGui::Checkbox("Deep Profiling", &m_config.enableDeepProfiling);
        ImGui::SliderFloat("Target FPS", &m_config.targetFrameRate, 30.0f, 144.0f);
    }

    void PerformanceProfiler::UpdateFrameData()
    {
        auto frame = std::make_unique<FrameProfileData>();
        frame->frameNumber = m_currentFrameNumber;
        frame->timestamp = std::chrono::steady_clock::now();

        m_currentFrame = std::move(frame);

        // Keep history within limits
        if (static_cast<int>(m_frameHistory.size()) >= m_config.maxFrameHistory)
        {
            m_frameHistory.erase(m_frameHistory.begin());
        }
        auto historyFrame = std::make_unique<FrameProfileData>();
        historyFrame->frameNumber = m_currentFrame->frameNumber;
        historyFrame->timestamp = m_currentFrame->timestamp;
        historyFrame->frameTime = m_currentFrame->frameTime;
        historyFrame->fps = m_currentFrame->fps;
        m_frameHistory.push_back(std::move(historyFrame));
    }

    void PerformanceProfiler::AnalyzePerformance()
    {
        DetectCPUBottlenecks();
        DetectGPUBottlenecks();
        DetectMemoryBottlenecks();
        GenerateOptimizationSuggestions();
        m_lastAnalysisTime = std::chrono::steady_clock::now();
    }

    void PerformanceProfiler::GenerateOptimizationSuggestions()
    {
        m_optimizationSuggestions.clear();

        if (!m_currentFrame)
            return;

        float targetFrameTime = 1000.0f / m_config.targetFrameRate;

        // Check frame time budget
        if (m_currentFrame->frameTime > targetFrameTime * 1.5f)
        {
            OptimizationSuggestion suggestion;
            suggestion.priority = OptimizationSuggestion::CRITICAL;
            suggestion.title = "Frame time significantly exceeds budget";
            suggestion.description =
                "Current frame time (" + std::to_string(static_cast<int>(m_currentFrame->frameTime)) +
                "ms) is over 150% of target (" + std::to_string(static_cast<int>(targetFrameTime)) + "ms).";
            suggestion.category = "Performance";
            suggestion.estimatedGain =
                ((m_currentFrame->frameTime - targetFrameTime) / m_currentFrame->frameTime) * 100.0f;
            suggestion.steps = {"Profile CPU and GPU to identify bottleneck", "Reduce scene complexity",
                                "Optimize hot paths"};
            m_optimizationSuggestions.push_back(suggestion);
        }
        else if (m_currentFrame->frameTime > targetFrameTime)
        {
            OptimizationSuggestion suggestion;
            suggestion.priority = OptimizationSuggestion::HIGH;
            suggestion.title = "Frame time exceeds budget";
            suggestion.description = "Current frame time exceeds target by " +
                                     std::to_string(static_cast<int>(m_currentFrame->frameTime - targetFrameTime)) +
                                     "ms.";
            suggestion.category = "Performance";
            suggestion.estimatedGain =
                ((m_currentFrame->frameTime - targetFrameTime) / m_currentFrame->frameTime) * 100.0f;
            suggestion.steps = {"Identify most expensive operations", "Consider LOD adjustments", "Batch draw calls"};
            m_optimizationSuggestions.push_back(suggestion);
        }

        // Check draw calls
        if (m_currentFrame->drawCalls > 2000)
        {
            OptimizationSuggestion suggestion;
            suggestion.priority =
                m_currentFrame->drawCalls > 5000 ? OptimizationSuggestion::HIGH : OptimizationSuggestion::MEDIUM;
            suggestion.title = "High draw call count";
            suggestion.description = "Draw call count (" + std::to_string(m_currentFrame->drawCalls) +
                                     ") is high. Consider batching or instancing.";
            suggestion.category = "Rendering";
            suggestion.estimatedGain = 15.0f;
            suggestion.steps = {"Enable draw call batching", "Use GPU instancing for repeated objects",
                                "Merge static meshes"};
            m_optimizationSuggestions.push_back(suggestion);
        }

        // Check memory usage
        size_t totalMem = m_currentFrame->systemMemoryUsage + m_currentFrame->videoMemoryUsage;
        if (totalMem > m_config.memoryBudget * 0.9)
        {
            OptimizationSuggestion suggestion;
            suggestion.priority =
                totalMem > m_config.memoryBudget ? OptimizationSuggestion::CRITICAL : OptimizationSuggestion::HIGH;
            suggestion.title = "Memory usage approaching budget";
            float memMB = static_cast<float>(totalMem) / (1024.0f * 1024.0f);
            float budgetMB = static_cast<float>(m_config.memoryBudget) / (1024.0f * 1024.0f);
            suggestion.description = "Using " + std::to_string(static_cast<int>(memMB)) + "MB of " +
                                     std::to_string(static_cast<int>(budgetMB)) + "MB budget.";
            suggestion.category = "Memory";
            suggestion.estimatedGain = 5.0f;
            suggestion.steps = {"Review texture resolutions", "Unload unused assets", "Use streaming"};
            m_optimizationSuggestions.push_back(suggestion);
        }

        // Add bottleneck-driven suggestions
        for (const auto& bottleneck : m_detectedBottlenecks)
        {
            if (bottleneck.severity > m_config.bottleneckThreshold)
            {
                OptimizationSuggestion suggestion;
                suggestion.priority =
                    bottleneck.severity > 0.9f ? OptimizationSuggestion::CRITICAL : OptimizationSuggestion::HIGH;
                suggestion.title = bottleneck.description;
                suggestion.description = bottleneck.recommendation;
                suggestion.category = "Bottleneck";
                suggestion.estimatedGain = bottleneck.severity * 20.0f;
                suggestion.steps = bottleneck.optimizationHints;
                m_optimizationSuggestions.push_back(suggestion);
            }
        }
    }

    void PerformanceProfiler::DetectCPUBottlenecks()
    {
        if (!m_currentFrame)
            return;

        // Check if CPU-bound: cpuTime significantly higher than gpuTime
        if (m_currentFrame->cpuTime > m_config.cpuBudget)
        {
            PerformanceBottleneck bottleneck;
            bottleneck.type = PerformanceBottleneck::CPU_BOUND;
            bottleneck.description = "CPU bound - CPU time exceeds budget";
            bottleneck.recommendation = "Optimize game logic, reduce physics complexity, or offload work to GPU.";
            bottleneck.severity = std::min(1.0f, m_currentFrame->cpuTime / (m_config.cpuBudget * 2.0f));
            bottleneck.confidence = 0.8f;
            bottleneck.affectedSystems = {"Game Logic", "Physics", "AI"};
            bottleneck.optimizationHints = {"Profile CPU samples to find hot functions",
                                            "Consider multithreading for expensive operations",
                                            "Reduce update frequency for non-critical systems"};

            // Look for expensive CPU samples
            if (!m_currentFrame->cpuSamples.empty())
            {
                for (const auto& sample : m_currentFrame->cpuSamples)
                {
                    if (sample->duration > m_config.cpuBudget * 0.5f)
                    {
                        bottleneck.optimizationHints.push_back("Hot function: " + sample->name + " (" +
                                                               std::to_string(static_cast<int>(sample->duration)) +
                                                               "ms)");
                    }
                }
            }

            m_detectedBottlenecks.push_back(bottleneck);
        }
    }

    void PerformanceProfiler::DetectGPUBottlenecks()
    {
        if (!m_currentFrame)
            return;

        if (m_currentFrame->gpuTime > m_config.gpuBudget)
        {
            PerformanceBottleneck bottleneck;
            bottleneck.type = PerformanceBottleneck::GPU_BOUND;
            bottleneck.description = "GPU bound - GPU time exceeds budget";
            bottleneck.recommendation = "Reduce rendering complexity, optimize shaders, or lower resolution.";
            bottleneck.severity = std::min(1.0f, m_currentFrame->gpuTime / (m_config.gpuBudget * 2.0f));
            bottleneck.confidence = 0.8f;
            bottleneck.affectedSystems = {"Rendering", "Post-Processing", "Shadows"};
            bottleneck.optimizationHints = {"Reduce shadow map resolution", "Use simpler shaders for distant objects",
                                            "Implement occlusion culling", "Reduce post-processing passes"};
            m_detectedBottlenecks.push_back(bottleneck);
        }

        // Check for excessive draw calls causing overhead
        if (m_currentFrame->drawCalls > 3000 && m_currentFrame->gpuTime > m_config.gpuBudget * 0.7f)
        {
            PerformanceBottleneck bottleneck;
            bottleneck.type = PerformanceBottleneck::VERTEX_BOUND;
            bottleneck.description = "Excessive draw call overhead";
            bottleneck.recommendation = "Batch draw calls, use instancing, merge static geometry.";
            bottleneck.severity = std::min(1.0f, static_cast<float>(m_currentFrame->drawCalls) / 5000.0f);
            bottleneck.confidence = 0.7f;
            bottleneck.affectedSystems = {"Rendering Pipeline"};
            bottleneck.optimizationHints = {"Enable hardware instancing", "Merge static meshes",
                                            "Use indirect draw calls"};
            m_detectedBottlenecks.push_back(bottleneck);
        }
    }

    void PerformanceProfiler::DetectMemoryBottlenecks()
    {
        std::lock_guard<std::mutex> lock(m_memoryMutex);

        size_t totalAllocated = 0;
        int totalAllocations = 0;
        for (const auto& [category, sample] : m_memoryCategories)
        {
            totalAllocated += sample.allocatedBytes;
            totalAllocations += sample.allocationCount;
        }

        if (totalAllocated > m_config.memoryBudget * 0.85)
        {
            PerformanceBottleneck bottleneck;
            bottleneck.type = PerformanceBottleneck::MEMORY_BOUND;
            bottleneck.description = "Memory usage approaching budget limit";
            float usageMB = static_cast<float>(totalAllocated) / (1024.0f * 1024.0f);
            float budgetMB = static_cast<float>(m_config.memoryBudget) / (1024.0f * 1024.0f);
            bottleneck.recommendation = "Current usage: " + std::to_string(static_cast<int>(usageMB)) + "MB / " +
                                        std::to_string(static_cast<int>(budgetMB)) + "MB budget.";
            bottleneck.severity =
                std::min(1.0f, static_cast<float>(totalAllocated) / static_cast<float>(m_config.memoryBudget));
            bottleneck.confidence = 0.9f;
            bottleneck.affectedSystems = {"Memory Management"};

            // Identify largest categories
            for (const auto& [category, sample] : m_memoryCategories)
            {
                float categoryMB = static_cast<float>(sample.allocatedBytes) / (1024.0f * 1024.0f);
                if (categoryMB > 100.0f)
                {
                    bottleneck.optimizationHints.push_back(category + ": " +
                                                           std::to_string(static_cast<int>(categoryMB)) + "MB");
                }
            }

            m_detectedBottlenecks.push_back(bottleneck);
        }

        // Check for excessive allocation frequency (potential GC pressure / fragmentation)
        if (totalAllocations > 10000)
        {
            PerformanceBottleneck bottleneck;
            bottleneck.type = PerformanceBottleneck::MEMORY_BOUND;
            bottleneck.description = "High allocation frequency detected";
            bottleneck.recommendation = "Use object pools or arena allocators to reduce allocation overhead.";
            bottleneck.severity = std::min(1.0f, static_cast<float>(totalAllocations) / 50000.0f);
            bottleneck.confidence = 0.6f;
            bottleneck.affectedSystems = {"Memory Management", "CPU"};
            bottleneck.optimizationHints = {"Implement object pooling", "Use stack allocators for temporary data",
                                            "Reduce per-frame allocations"};
            m_detectedBottlenecks.push_back(bottleneck);
        }
    }

    void PerformanceProfiler::ProcessGPUQueries()
    {
        if (!m_currentFrame)
            return;

        // Collect results from D3D11 timestamp queries
        if (m_context && m_disjointActive && m_disjointQuery)
        {
            // End the disjoint query for this frame
            m_context->End(m_disjointQuery.Get());
            m_disjointActive = false;

            // Retrieve the disjoint query data (contains GPU frequency)
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData = {};
            HRESULT hr = m_context->GetData(m_disjointQuery.Get(), &disjointData, sizeof(disjointData), 0);

            if (SUCCEEDED(hr) && !disjointData.Disjoint && disjointData.Frequency > 0)
            {
                // Collect timestamp results for all completed query pairs
                for (auto it = m_pendingGPUQueries.begin(); it != m_pendingGPUQueries.end();)
                {
                    auto& [name, queryPair] = *it;
                    if (!queryPair.begun || !queryPair.ended)
                    {
                        ++it;
                        continue;
                    }

                    UINT64 beginTimestamp = 0;
                    UINT64 endTimestamp = 0;

                    HRESULT hrBegin =
                        m_context->GetData(queryPair.beginQuery.Get(), &beginTimestamp, sizeof(UINT64), 0);
                    HRESULT hrEnd = m_context->GetData(queryPair.endQuery.Get(), &endTimestamp, sizeof(UINT64), 0);

                    if (SUCCEEDED(hrBegin) && SUCCEEDED(hrEnd) && endTimestamp > beginTimestamp)
                    {
                        float durationMs = static_cast<float>(endTimestamp - beginTimestamp) /
                                           static_cast<float>(disjointData.Frequency) * 1000.0f;

                        GPUProfileSample sample;
                        sample.name = queryPair.name;
                        sample.shaderName = queryPair.shaderName;
                        sample.startTimestamp = beginTimestamp;
                        sample.endTimestamp = endTimestamp;
                        sample.duration = durationMs;

                        m_currentFrame->gpuSamples.push_back(sample);
                    }

                    it = m_pendingGPUQueries.erase(it);
                }
            }
            else
            {
                // Disjoint or failed — discard all pending queries
                m_pendingGPUQueries.clear();
            }
        }

        // Fallback: process any samples with manually-set timestamps (non-D3D11 path)
        for (auto& [name, sample] : m_activeGPUSamples)
        {
            if (sample.endTimestamp > sample.startTimestamp)
            {
                sample.duration =
                    static_cast<float>(sample.endTimestamp - sample.startTimestamp) / 1000000.0f; // ns to ms
                if (sample.duration > 0.0f)
                {
                    m_currentFrame->gpuSamples.push_back(sample);
                }
            }
        }

        // Calculate total GPU time from all collected samples
        float totalGpuTime = 0.0f;
        for (const auto& sample : m_currentFrame->gpuSamples)
        {
            totalGpuTime += sample.duration;
        }
        m_currentFrame->gpuTime = totalGpuTime;
    }

    void PerformanceProfiler::UpdateMemoryTracking()
    {
        std::lock_guard<std::mutex> lock(m_memoryMutex);

        if (!m_currentFrame)
            return;

        // Update frame memory statistics from categories
        size_t totalSystem = 0;
        size_t totalVideo = 0;

        for (auto& [category, sample] : m_memoryCategories)
        {
            sample.timestamp = std::chrono::steady_clock::now();

            // Track peak usage
            if (sample.allocatedBytes > sample.peakBytes)
            {
                sample.peakBytes = sample.allocatedBytes;
            }

            // Categorize memory into system vs video
            if (category == "Textures" || category == "Shaders" || category == "RenderTargets" || category == "GPU")
            {
                totalVideo += sample.allocatedBytes;
            }
            else
            {
                totalSystem += sample.allocatedBytes;
            }

            // Add to frame memory samples
            m_currentFrame->memorySamples.push_back(sample);
        }

        m_currentFrame->systemMemoryUsage = totalSystem;
        m_currentFrame->videoMemoryUsage = totalVideo;
        m_currentFrame->activeObjects = static_cast<int>(m_memoryAllocations.size());
    }

    void PerformanceProfiler::CalculateStatistics()
    {
        if (m_frameHistory.empty())
            return;

        // Calculate aggregate statistics over recent history
        int windowSize = std::min(static_cast<int>(m_frameHistory.size()), m_config.analysisWindowSize);
        int startIdx = static_cast<int>(m_frameHistory.size()) - windowSize;

        float sumFrameTime = 0.0f, sumCpuTime = 0.0f, sumGpuTime = 0.0f;
        float minFrameTime = FLT_MAX, maxFrameTime = 0.0f;
        int sumDrawCalls = 0, sumTriangles = 0;
        std::vector<float> frameTimes;

        for (int i = startIdx; i < static_cast<int>(m_frameHistory.size()); ++i)
        {
            const auto& frame = m_frameHistory[i];
            sumFrameTime += frame->frameTime;
            sumCpuTime += frame->cpuTime;
            sumGpuTime += frame->gpuTime;
            sumDrawCalls += frame->drawCalls;
            sumTriangles += frame->triangles;
            if (frame->frameTime < minFrameTime)
                minFrameTime = frame->frameTime;
            if (frame->frameTime > maxFrameTime)
                maxFrameTime = frame->frameTime;
            frameTimes.push_back(frame->frameTime);
        }

        float avgFrameTime = sumFrameTime / static_cast<float>(windowSize);
        float avgFps = (avgFrameTime > 0.0f) ? 1000.0f / avgFrameTime : 0.0f;

        // Calculate percentiles (sort frame times)
        std::sort(frameTimes.begin(), frameTimes.end());
        float p50 = frameTimes.empty() ? 0.0f : frameTimes[frameTimes.size() / 2];
        float p95 = frameTimes.empty() ? 0.0f : frameTimes[static_cast<size_t>(frameTimes.size() * 0.95f)];
        float p99 = frameTimes.empty() ? 0.0f : frameTimes[static_cast<size_t>(frameTimes.size() * 0.99f)];

        // Update the current frame with calculated statistics
        if (m_currentFrame)
        {
            m_currentFrame->fps = avgFps;
            m_currentFrame->frameTime = avgFrameTime;
            m_currentFrame->isPerformanceTarget = (avgFrameTime <= m_currentFrame->targetFrameTime);
        }

        // Clear and regenerate bottlenecks
        m_detectedBottlenecks.clear();
    }

    void PerformanceProfiler::RenderCPUSampleHierarchy(const CPUProfileSample* sample, int depth)
    {
        if (!sample)
            return;

        std::string indent(depth * 2, ' ');
        ImGui::Text("%s%s: %.3f ms (self: %.3f ms)", indent.c_str(), sample->name.c_str(), sample->GetTotalTime(),
                    sample->GetSelfTime());

        for (const auto& child : sample->children)
        {
            RenderCPUSampleHierarchy(child.get(), depth + 1);
        }
    }

    void PerformanceProfiler::RenderPerformanceGraph(const PerformanceCounter& counter, const XMFLOAT2& size)
    {
        if (counter.history.empty())
            return;

        std::vector<float> data(counter.history.begin(), counter.history.end());
        ImGui::PlotLines(("##" + counter.name).c_str(), data.data(), static_cast<int>(data.size()), 0, nullptr,
                         counter.minValue, counter.maxValue, ImVec2(size.x, size.y));
    }

} // namespace SparkEditor
