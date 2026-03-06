/**
 * @file PerformanceProfiler.cpp
 * @brief Implementation of the Performance Profiler panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "../Profiler/PerformanceProfiler.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <fstream>

namespace SparkEditor {

// Global profiler instance
PerformanceProfiler* g_profiler = nullptr;

// ============================================================================
// PerformanceCounter Implementation
// ============================================================================

void PerformanceCounter::AddSample(float value) {
    currentValue = value;
    if (value < minValue) minValue = value;
    if (value > maxValue) maxValue = value;

    history.push_back(value);
    if (static_cast<int>(history.size()) > historySize) {
        history.erase(history.begin());
    }

    // Recalculate average
    float sum = 0.0f;
    for (float v : history) sum += v;
    averageValue = history.empty() ? 0.0f : sum / static_cast<float>(history.size());

    lastUpdate = std::chrono::steady_clock::now();
}

void PerformanceCounter::Clear() {
    currentValue = 0.0f;
    minValue = FLT_MAX;
    maxValue = -FLT_MAX;
    averageValue = 0.0f;
    history.clear();
}

float PerformanceCounter::GetSmoothedValue(float smoothingFactor) const {
    if (history.size() < 2) return currentValue;
    float smoothed = history[0];
    for (size_t i = 1; i < history.size(); ++i) {
        smoothed = smoothingFactor * history[i] + (1.0f - smoothingFactor) * smoothed;
    }
    return smoothed;
}

// ============================================================================
// CPUProfileSample Implementation
// ============================================================================

float CPUProfileSample::GetSelfTime() const {
    float childTime = 0.0f;
    for (const auto& child : children) {
        childTime += child->GetTotalTime();
    }
    return duration - childTime;
}

float CPUProfileSample::GetTotalTime() const {
    return duration;
}

// ============================================================================
// ProfileScope Implementation
// ============================================================================

ProfileScope::ProfileScope(const std::string& name, const std::string& /*category*/)
    : m_name(name), m_startTime(std::chrono::high_resolution_clock::now()), m_ended(false) {
}

ProfileScope::~ProfileScope() {
    if (!m_ended) End();
}

void ProfileScope::End() {
    m_ended = true;
}

// ============================================================================
// PerformanceProfiler Implementation
// ============================================================================

PerformanceProfiler::PerformanceProfiler()
    : EditorPanel("Performance Profiler", "performance_profiler_panel") {
}

PerformanceProfiler::~PerformanceProfiler() {
}

bool PerformanceProfiler::Initialize() {
    std::cout << "Initializing Performance Profiler panel\n";
    m_isProfiling = true;
    return true;
}

void PerformanceProfiler::Update(float deltaTime) {
    if (!m_isProfiling) return;

    // Update frame data
    UpdateFrameData();

    m_currentFrameNumber++;
}

void PerformanceProfiler::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        // Toolbar
        if (m_isProfiling) {
            if (ImGui::Button("Stop Profiling")) {
                StopProfiling();
            }
        } else {
            if (ImGui::Button("Start Profiling")) {
                StartProfiling();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear Data")) {
            ClearProfilingData();
        }

        ImGui::Separator();

        // Tab bar for different views
        if (ImGui::BeginTabBar("ProfilerTabs")) {
            if (m_showOverview && ImGui::BeginTabItem("Overview")) {
                RenderOverviewPanel();
                ImGui::EndTabItem();
            }
            if (m_showCPUProfiler && ImGui::BeginTabItem("CPU")) {
                RenderCPUProfilerPanel();
                ImGui::EndTabItem();
            }
            if (m_showGPUProfiler && ImGui::BeginTabItem("GPU")) {
                RenderGPUProfilerPanel();
                ImGui::EndTabItem();
            }
            if (m_showMemoryProfiler && ImGui::BeginTabItem("Memory")) {
                RenderMemoryProfilerPanel();
                ImGui::EndTabItem();
            }
            if (m_showCounters && ImGui::BeginTabItem("Counters")) {
                RenderPerformanceCountersPanel();
                ImGui::EndTabItem();
            }
            if (m_showOptimization && ImGui::BeginTabItem("Optimization")) {
                RenderOptimizationPanel();
                ImGui::EndTabItem();
            }
            if (m_showConfiguration && ImGui::BeginTabItem("Configuration")) {
                RenderConfigurationPanel();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    EndPanel();
}

void PerformanceProfiler::Shutdown() {
    std::cout << "Shutting down Performance Profiler panel\n";
    StopProfiling();
}

bool PerformanceProfiler::HandleEvent(const std::string& /*eventType*/, void* /*eventData*/) {
    return false;
}

void PerformanceProfiler::StartProfiling() {
    m_isProfiling = true;
    std::cout << "Started profiling session\n";
}

void PerformanceProfiler::StopProfiling() {
    m_isProfiling = false;
    std::cout << "Stopped profiling session\n";
}

uint32_t PerformanceProfiler::BeginCPUSample(const std::string& name, const std::string& category) {
    if (!m_isProfiling) return 0;

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

void PerformanceProfiler::EndCPUSample(uint32_t sampleID) {
    if (!m_isProfiling) return;

    std::lock_guard<std::mutex> lock(m_cpuSampleMutex);

    auto it = m_cpuSampleMap.find(sampleID);
    if (it != m_cpuSampleMap.end()) {
        auto* sample = it->second;
        sample->endTime = std::chrono::high_resolution_clock::now();
        sample->duration = std::chrono::duration<float, std::milli>(
            sample->endTime - sample->startTime).count();
        m_cpuSampleMap.erase(it);
    }
}

void PerformanceProfiler::BeginGPUSample(const std::string& name, const std::string& shaderName) {
    if (!m_isProfiling) return;

    GPUProfileSample sample;
    sample.name = name;
    sample.shaderName = shaderName;
    m_activeGPUSamples[name] = sample;
}

void PerformanceProfiler::EndGPUSample(const std::string& name) {
    if (!m_isProfiling) return;
    m_activeGPUSamples.erase(name);
}

void PerformanceProfiler::RecordMemoryAllocation(const std::string& category, size_t bytes, void* pointer) {
    std::lock_guard<std::mutex> lock(m_memoryMutex);

    if (pointer) {
        m_memoryAllocations[pointer] = {category, bytes};
    }
    m_memoryCategories[category].allocatedBytes += bytes;
    m_memoryCategories[category].allocationCount++;
    m_memoryCategories[category].totalAllocatedBytes += bytes;
}

void PerformanceProfiler::RecordMemoryDeallocation(void* pointer) {
    std::lock_guard<std::mutex> lock(m_memoryMutex);

    auto it = m_memoryAllocations.find(pointer);
    if (it != m_memoryAllocations.end()) {
        auto& [category, bytes] = it->second;
        m_memoryCategories[category].allocatedBytes -= bytes;
        m_memoryCategories[category].deallocationCount++;
        m_memoryAllocations.erase(it);
    }
}

uint32_t PerformanceProfiler::AddPerformanceCounter(const std::string& name, ProfilerSampleType type, const std::string& unit) {
    PerformanceCounter counter;
    counter.name = name;
    counter.type = type;
    counter.unit = unit;
    m_performanceCounters.push_back(counter);
    return m_nextCounterID++;
}

void PerformanceProfiler::UpdatePerformanceCounter(uint32_t counterID, float value) {
    if (counterID > 0 && counterID <= m_performanceCounters.size()) {
        m_performanceCounters[counterID - 1].AddSample(value);
    }
}

const FrameProfileData* PerformanceProfiler::GetCurrentFrame() const {
    return m_currentFrame.get();
}

const FrameProfileData* PerformanceProfiler::GetFrame(int frameIndex) const {
    if (frameIndex < 0 || frameIndex >= static_cast<int>(m_frameHistory.size())) {
        return nullptr;
    }
    return m_frameHistory[m_frameHistory.size() - 1 - frameIndex].get();
}

bool PerformanceProfiler::ApplyOptimization(int suggestionIndex) {
    if (suggestionIndex < 0 || suggestionIndex >= static_cast<int>(m_optimizationSuggestions.size())) {
        return false;
    }
    auto& suggestion = m_optimizationSuggestions[suggestionIndex];
    if (suggestion.isAutomatable && suggestion.automateFunction) {
        return suggestion.automateFunction();
    }
    return false;
}

bool PerformanceProfiler::ExportProfilingData(const std::string& filePath, const std::string& format) {
    try {
        std::ofstream file(filePath);
        if (!file.is_open()) return false;

        if (format == "json") {
            file << "{ \"frames\": " << m_frameHistory.size() << " }\n";
        } else {
            file << "Profiling Data Export\n";
            file << "Frames: " << m_frameHistory.size() << "\n";
        }

        file.close();
        std::cout << "Exported profiling data to: " << filePath << "\n";
        return true;
    } catch (...) {
        return false;
    }
}

bool PerformanceProfiler::ImportProfilingData(const std::string& /*filePath*/) {
    // Stub
    return false;
}

void PerformanceProfiler::ClearProfilingData() {
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

void PerformanceProfiler::SetConfiguration(const ProfilerConfig& config) {
    m_config = config;
}

uint32_t PerformanceProfiler::TakeSnapshot(const std::string& name) {
    PerformanceSnapshot snapshot;
    snapshot.name = name;
    snapshot.timestamp = std::chrono::steady_clock::now();
    if (m_currentFrame) {
        snapshot.frameData = *m_currentFrame;
    }
    snapshot.counters = m_performanceCounters;
    m_snapshots.push_back(std::move(snapshot));
    return m_nextSnapshotID++;
}

std::string PerformanceProfiler::CompareSnapshots(uint32_t snapshot1, uint32_t snapshot2) {
    if (snapshot1 == 0 || snapshot2 == 0 ||
        snapshot1 > m_snapshots.size() || snapshot2 > m_snapshots.size()) {
        return "Invalid snapshot IDs";
    }

    const auto& s1 = m_snapshots[snapshot1 - 1];
    const auto& s2 = m_snapshots[snapshot2 - 1];

    std::stringstream ss;
    ss << "Comparing: " << s1.name << " vs " << s2.name << "\n";
    ss << "Frame time: " << s1.frameData.frameTime << " vs " << s2.frameData.frameTime << " ms\n";
    return ss.str();
}

std::string PerformanceProfiler::GetTrendAnalysis(const std::string& /*metric*/, float /*timespan*/) {
    return "Trend analysis not yet implemented";
}

// ============================================================================
// Private Methods
// ============================================================================

void PerformanceProfiler::RenderOverviewPanel() {
    ImGui::Text("Performance Overview");
    ImGui::Separator();

    if (m_currentFrame) {
        ImGui::Text("Frame: %d", m_currentFrame->frameNumber);
        ImGui::Text("Frame Time: %.2f ms", m_currentFrame->frameTime);
        ImGui::Text("FPS: %.1f", m_currentFrame->fps);
        ImGui::Text("Draw Calls: %d", m_currentFrame->drawCalls);
    } else {
        ImGui::Text("No profiling data available");
    }
}

void PerformanceProfiler::RenderCPUProfilerPanel() {
    ImGui::Text("CPU Profiler");
    ImGui::Separator();

    if (m_currentFrame && !m_currentFrame->cpuSamples.empty()) {
        for (const auto& sample : m_currentFrame->cpuSamples) {
            RenderCPUSampleHierarchy(sample.get(), 0);
        }
    } else {
        ImGui::Text("No CPU samples available");
    }
}

void PerformanceProfiler::RenderGPUProfilerPanel() {
    ImGui::Text("GPU Profiler");
    ImGui::Separator();

    if (m_currentFrame && !m_currentFrame->gpuSamples.empty()) {
        for (const auto& sample : m_currentFrame->gpuSamples) {
            ImGui::Text("%s: %.2f ms (%d draw calls)",
                sample.name.c_str(), sample.duration, sample.drawCalls);
        }
    } else {
        ImGui::Text("No GPU samples available");
    }
}

void PerformanceProfiler::RenderMemoryProfilerPanel() {
    ImGui::Text("Memory Profiler");
    ImGui::Separator();

    std::lock_guard<std::mutex> lock(m_memoryMutex);
    for (const auto& [category, sample] : m_memoryCategories) {
        float mb = static_cast<float>(sample.allocatedBytes) / (1024.0f * 1024.0f);
        ImGui::Text("%s: %.2f MB (%d allocs)", category.c_str(), mb, sample.allocationCount);
    }

    if (m_memoryCategories.empty()) {
        ImGui::Text("No memory data available");
    }
}

void PerformanceProfiler::RenderPerformanceCountersPanel() {
    ImGui::Text("Performance Counters");
    ImGui::Separator();

    if (ImGui::BeginTable("CountersTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Average");
        ImGui::TableSetupColumn("Min/Max");
        ImGui::TableSetupColumn("Unit");
        ImGui::TableHeadersRow();

        for (const auto& counter : m_performanceCounters) {
            if (!counter.isActive) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", counter.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", counter.currentValue);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", counter.averageValue);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f / %.2f", counter.minValue, counter.maxValue);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%s", counter.unit.c_str());
        }

        ImGui::EndTable();
    }
}

void PerformanceProfiler::RenderOptimizationPanel() {
    ImGui::Text("Optimization Suggestions");
    ImGui::Separator();

    if (m_optimizationSuggestions.empty()) {
        ImGui::Text("No optimization suggestions");
        return;
    }

    for (int i = 0; i < static_cast<int>(m_optimizationSuggestions.size()); ++i) {
        const auto& suggestion = m_optimizationSuggestions[i];
        ImGui::Text("[%s] %s",
            suggestion.priority == OptimizationSuggestion::CRITICAL ? "CRITICAL" :
            suggestion.priority == OptimizationSuggestion::HIGH ? "HIGH" :
            suggestion.priority == OptimizationSuggestion::MEDIUM ? "MEDIUM" : "LOW",
            suggestion.title.c_str());
        ImGui::Text("  %s", suggestion.description.c_str());
        if (suggestion.isAutomatable && ImGui::Button(("Apply##" + std::to_string(i)).c_str())) {
            ApplyOptimization(i);
        }
        ImGui::Separator();
    }
}

void PerformanceProfiler::RenderConfigurationPanel() {
    ImGui::Text("Profiler Configuration");
    ImGui::Separator();

    ImGui::Checkbox("CPU Profiling", &m_config.enableCPUProfiling);
    ImGui::Checkbox("GPU Profiling", &m_config.enableGPUProfiling);
    ImGui::Checkbox("Memory Profiling", &m_config.enableMemoryProfiling);
    ImGui::Checkbox("Deep Profiling", &m_config.enableDeepProfiling);
    ImGui::SliderFloat("Target FPS", &m_config.targetFrameRate, 30.0f, 144.0f);
}

void PerformanceProfiler::UpdateFrameData() {
    auto frame = std::make_unique<FrameProfileData>();
    frame->frameNumber = m_currentFrameNumber;
    frame->timestamp = std::chrono::steady_clock::now();

    m_currentFrame = std::move(frame);

    // Keep history within limits
    if (static_cast<int>(m_frameHistory.size()) >= m_config.maxFrameHistory) {
        m_frameHistory.erase(m_frameHistory.begin());
    }
    m_frameHistory.push_back(std::make_unique<FrameProfileData>(*m_currentFrame));
}

void PerformanceProfiler::AnalyzePerformance() {
    DetectCPUBottlenecks();
    DetectGPUBottlenecks();
    DetectMemoryBottlenecks();
    GenerateOptimizationSuggestions();
    m_lastAnalysisTime = std::chrono::steady_clock::now();
}

void PerformanceProfiler::GenerateOptimizationSuggestions() {
    // Stub - would analyze profiling data and generate suggestions
}

void PerformanceProfiler::DetectCPUBottlenecks() {
    // Stub
}

void PerformanceProfiler::DetectGPUBottlenecks() {
    // Stub
}

void PerformanceProfiler::DetectMemoryBottlenecks() {
    // Stub
}

void PerformanceProfiler::ProcessGPUQueries() {
    // Stub
}

void PerformanceProfiler::UpdateMemoryTracking() {
    // Stub
}

void PerformanceProfiler::CalculateStatistics() {
    // Stub
}

void PerformanceProfiler::RenderCPUSampleHierarchy(const CPUProfileSample* sample, int depth) {
    if (!sample) return;

    std::string indent(depth * 2, ' ');
    ImGui::Text("%s%s: %.3f ms (self: %.3f ms)",
        indent.c_str(), sample->name.c_str(), sample->GetTotalTime(), sample->GetSelfTime());

    for (const auto& child : sample->children) {
        RenderCPUSampleHierarchy(child.get(), depth + 1);
    }
}

void PerformanceProfiler::RenderPerformanceGraph(const PerformanceCounter& counter, const XMFLOAT2& size) {
    if (counter.history.empty()) return;

    std::vector<float> data(counter.history.begin(), counter.history.end());
    ImGui::PlotLines(("##" + counter.name).c_str(), data.data(), static_cast<int>(data.size()),
                    0, nullptr, counter.minValue, counter.maxValue, ImVec2(size.x, size.y));
}

} // namespace SparkEditor
