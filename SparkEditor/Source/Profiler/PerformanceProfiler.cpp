/**
 * @file PerformanceProfiler.cpp
 * @brief Performance profiling panel with real-time graphs
 * @author Spark Engine Team
 * @date 2025
 */

#include "PerformanceProfiler.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace SparkEditor {

// Global profiler pointer
PerformanceProfiler* g_profiler = nullptr;

// ---- PerformanceCounter ----
void PerformanceCounter::AddSample(float value) {
    currentValue = value;
    if (value < minValue) minValue = value;
    if (value > maxValue) maxValue = value;
    history.push_back(value);
    if ((int)history.size() > historySize)
        history.erase(history.begin());

    // Running average
    if (!history.empty()) {
        float sum = 0.0f;
        for (float v : history) sum += v;
        averageValue = sum / (float)history.size();
    }
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
    float prev = history[history.size() - 2];
    return prev + smoothingFactor * (currentValue - prev);
}

// ---- CPUProfileSample ----
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

// ---- ProfileScope ----
ProfileScope::ProfileScope(const std::string& name, const std::string& category)
    : m_name(name)
    , m_startTime(std::chrono::high_resolution_clock::now())
{
    if (g_profiler && g_profiler->IsProfiling()) {
        g_profiler->BeginCPUSample(name, category);
    }
}

ProfileScope::~ProfileScope() {
    End();
}

void ProfileScope::End() {
    if (!m_ended) {
        m_ended = true;
        // Simple timing — actual integration would end the CPU sample
    }
}

// ---- PerformanceProfiler ----

PerformanceProfiler::PerformanceProfiler()
    : EditorPanel("Profiler", "performance_profiler_panel")
{
    g_profiler = this;
}

PerformanceProfiler::~PerformanceProfiler() {
    if (g_profiler == this) g_profiler = nullptr;
}

bool PerformanceProfiler::Initialize() {
    std::cout << "Initializing Performance Profiler\n";

    m_currentFrame = std::make_unique<FrameProfileData>();

    // Create default counters
    AddPerformanceCounter("Frame Time", ProfilerSampleType::CPU_SAMPLE, "ms");
    AddPerformanceCounter("CPU Time", ProfilerSampleType::CPU_SAMPLE, "ms");
    AddPerformanceCounter("GPU Time", ProfilerSampleType::GPU_SAMPLE, "ms");
    AddPerformanceCounter("Draw Calls", ProfilerSampleType::RENDERING_SAMPLE, "");
    AddPerformanceCounter("Triangles", ProfilerSampleType::RENDERING_SAMPLE, "K");
    AddPerformanceCounter("Memory", ProfilerSampleType::MEMORY_SAMPLE, "MB");

    m_isProfiling = true;
    return true;
}

void PerformanceProfiler::Update(float deltaTime) {
    if (!m_isProfiling) return;

    m_currentFrameNumber++;

    // Simulate profiling data
    float frameMs = deltaTime * 1000.0f;
    if (m_currentFrame) {
        m_currentFrame->frameNumber = m_currentFrameNumber;
        m_currentFrame->frameTime = frameMs;
        m_currentFrame->cpuTime = frameMs * 0.6f + ((float)(rand() % 100) / 100.0f) * 2.0f;
        m_currentFrame->gpuTime = frameMs * 0.4f + ((float)(rand() % 100) / 100.0f) * 1.5f;
        m_currentFrame->renderTime = m_currentFrame->gpuTime * 0.8f;
        m_currentFrame->updateTime = m_currentFrame->cpuTime * 0.3f;
        m_currentFrame->physicsTime = m_currentFrame->cpuTime * 0.15f;
        m_currentFrame->drawCalls = 150 + rand() % 50;
        m_currentFrame->triangles = 45000 + rand() % 15000;
        m_currentFrame->fps = frameMs > 0.001f ? 1000.0f / frameMs : 0.0f;
        m_currentFrame->systemMemoryUsage = 256 * 1024 * 1024 + rand() % (64 * 1024 * 1024);
        m_currentFrame->videoMemoryUsage = 128 * 1024 * 1024 + rand() % (32 * 1024 * 1024);
        m_currentFrame->activeObjects = 120 + rand() % 30;
        m_currentFrame->visibleObjects = 80 + rand() % 20;
    }

    // Update counters
    if (m_performanceCounters.size() >= 6) {
        m_performanceCounters[0].AddSample(frameMs);
        m_performanceCounters[1].AddSample(m_currentFrame->cpuTime);
        m_performanceCounters[2].AddSample(m_currentFrame->gpuTime);
        m_performanceCounters[3].AddSample((float)m_currentFrame->drawCalls);
        m_performanceCounters[4].AddSample((float)m_currentFrame->triangles / 1000.0f);
        m_performanceCounters[5].AddSample((float)m_currentFrame->systemMemoryUsage / (1024.0f * 1024.0f));
    }
}

void PerformanceProfiler::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        // Toolbar
        if (m_isProfiling) {
            if (ImGui::Button(ICON_FA_PAUSE " Pause")) StopProfiling();
        } else {
            if (ImGui::Button(ICON_FA_PLAY " Record")) StartProfiling();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH " Clear")) ClearProfilingData();
        ImGui::SameLine();

        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Quick stats
        if (m_currentFrame) {
            float fps = m_currentFrame->fps;
            ImVec4 fpsColor = fps >= 60 ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f)
                            : fps >= 30 ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f)
                            : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(fpsColor, "%.0f FPS", fps);
            ImGui::SameLine();
            ImGui::Text("| %.1fms | CPU: %.1fms | GPU: %.1fms | Draw: %d",
                m_currentFrame->frameTime, m_currentFrame->cpuTime,
                m_currentFrame->gpuTime, m_currentFrame->drawCalls);
        }

        ImGui::Separator();

        // Tab bar for different profiler views
        if (ImGui::BeginTabBar("ProfilerTabs")) {
            if (ImGui::BeginTabItem(ICON_FA_CHART_BAR " Overview")) {
                RenderOverviewPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(ICON_FA_MICROCHIP " CPU")) {
                RenderCPUProfilerPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(ICON_FA_TV " GPU")) {
                RenderGPUProfilerPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(ICON_FA_MEMORY " Memory")) {
                RenderMemoryProfilerPanel();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    EndPanel();
}

void PerformanceProfiler::Shutdown() {
    std::cout << "Shutting down Performance Profiler\n";
    m_isProfiling = false;
    if (g_profiler == this) g_profiler = nullptr;
}

bool PerformanceProfiler::HandleEvent(const std::string& eventType, void* eventData) {
    return false;
}

void PerformanceProfiler::StartProfiling() { m_isProfiling = true; }
void PerformanceProfiler::StopProfiling() { m_isProfiling = false; }

uint32_t PerformanceProfiler::BeginCPUSample(const std::string& name, const std::string& category) {
    std::lock_guard<std::mutex> lock(m_cpuSampleMutex);
    uint32_t id = m_nextCPUSampleID++;
    auto sample = std::make_unique<CPUProfileSample>();
    sample->name = name;
    sample->category = category;
    sample->startTime = std::chrono::high_resolution_clock::now();
    m_cpuSampleMap[id] = sample.get();
    m_activeCPUSamples.push_back(std::move(sample));
    return id;
}

void PerformanceProfiler::EndCPUSample(uint32_t sampleID) {
    std::lock_guard<std::mutex> lock(m_cpuSampleMutex);
    auto it = m_cpuSampleMap.find(sampleID);
    if (it != m_cpuSampleMap.end()) {
        it->second->endTime = std::chrono::high_resolution_clock::now();
        it->second->duration = std::chrono::duration<float, std::milli>(
            it->second->endTime - it->second->startTime).count();
    }
}

void PerformanceProfiler::BeginGPUSample(const std::string& name, const std::string& shaderName) {
    GPUProfileSample sample;
    sample.name = name;
    sample.shaderName = shaderName;
    m_activeGPUSamples[name] = sample;
}

void PerformanceProfiler::EndGPUSample(const std::string& name) {
    m_activeGPUSamples.erase(name);
}

void PerformanceProfiler::RecordMemoryAllocation(const std::string& category, size_t bytes, void* pointer) {
    std::lock_guard<std::mutex> lock(m_memoryMutex);
    if (pointer) m_memoryAllocations[pointer] = {category, bytes};
    m_memoryCategories[category].allocatedBytes += bytes;
    m_memoryCategories[category].allocationCount++;
    m_memoryCategories[category].totalAllocatedBytes += bytes;
    if (m_memoryCategories[category].allocatedBytes > m_memoryCategories[category].peakBytes)
        m_memoryCategories[category].peakBytes = m_memoryCategories[category].allocatedBytes;
}

void PerformanceProfiler::RecordMemoryDeallocation(void* pointer) {
    std::lock_guard<std::mutex> lock(m_memoryMutex);
    auto it = m_memoryAllocations.find(pointer);
    if (it != m_memoryAllocations.end()) {
        m_memoryCategories[it->second.first].allocatedBytes -= it->second.second;
        m_memoryCategories[it->second.first].deallocationCount++;
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
    if (frameIndex == 0) return m_currentFrame.get();
    if (frameIndex > 0 && frameIndex <= (int)m_frameHistory.size())
        return m_frameHistory[m_frameHistory.size() - frameIndex].get();
    return nullptr;
}

bool PerformanceProfiler::ApplyOptimization(int suggestionIndex) { return false; }

bool PerformanceProfiler::ExportProfilingData(const std::string& filePath, const std::string& format) {
    return false; // Stub
}

bool PerformanceProfiler::ImportProfilingData(const std::string& filePath) {
    return false; // Stub
}

void PerformanceProfiler::ClearProfilingData() {
    m_frameHistory.clear();
    for (auto& counter : m_performanceCounters) counter.Clear();
    m_detectedBottlenecks.clear();
    m_optimizationSuggestions.clear();
    m_currentFrameNumber = 0;
}

void PerformanceProfiler::SetConfiguration(const ProfilerConfig& config) { m_config = config; }

uint32_t PerformanceProfiler::TakeSnapshot(const std::string& name) {
    return m_nextSnapshotID++;
}

std::string PerformanceProfiler::CompareSnapshots(uint32_t snapshot1, uint32_t snapshot2) {
    return "Snapshot comparison not yet implemented";
}

std::string PerformanceProfiler::GetTrendAnalysis(const std::string& metric, float timespan) {
    return "Trend analysis not yet implemented";
}

// ---- Rendering ----

void PerformanceProfiler::RenderOverviewPanel() {
    if (!m_currentFrame) return;

    // Frame time graph
    ImGui::Text(ICON_FA_CHART_LINE " Frame Time History");
    if (!m_performanceCounters.empty() && !m_performanceCounters[0].history.empty()) {
        const auto& history = m_performanceCounters[0].history;
        float maxVal = *std::max_element(history.begin(), history.end());
        maxVal = std::max(maxVal, 33.3f); // At least show up to 30fps line

        ImGui::PlotLines("##FrameTime", history.data(), (int)history.size(),
                        0, nullptr, 0.0f, maxVal, ImVec2(-1, 80));

        // Budget line info
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "Target: 16.7ms (60fps) | Avg: %.1fms | Min: %.1fms | Max: %.1fms",
            m_performanceCounters[0].averageValue,
            m_performanceCounters[0].minValue,
            m_performanceCounters[0].maxValue);
    }

    ImGui::Spacing();

    // Summary table
    if (ImGui::BeginTable("OverviewTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Metric");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Average");
        ImGui::TableSetupColumn("Budget");
        ImGui::TableHeadersRow();

        auto Row = [](const char* name, float current, float avg, float budget, const char* unit) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", name);
            ImGui::TableSetColumnIndex(1);
            ImVec4 color = current <= budget ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(color, "%.1f%s", current, unit);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f%s", avg, unit);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.1f%s", budget, unit);
        };

        Row("Frame Time", m_currentFrame->frameTime,
            m_performanceCounters.size() > 0 ? m_performanceCounters[0].averageValue : 0,
            16.67f, "ms");
        Row("CPU Time", m_currentFrame->cpuTime,
            m_performanceCounters.size() > 1 ? m_performanceCounters[1].averageValue : 0,
            m_config.cpuBudget, "ms");
        Row("GPU Time", m_currentFrame->gpuTime,
            m_performanceCounters.size() > 2 ? m_performanceCounters[2].averageValue : 0,
            m_config.gpuBudget, "ms");

        ImGui::EndTable();
    }

    ImGui::Spacing();

    // Rendering stats
    ImGui::Text(ICON_FA_PAINT_BRUSH " Rendering");
    ImGui::Text("Draw Calls: %d | Triangles: %dK | Objects: %d/%d visible",
        m_currentFrame->drawCalls, m_currentFrame->triangles / 1000,
        m_currentFrame->visibleObjects, m_currentFrame->activeObjects);

    // Memory stats
    ImGui::Spacing();
    ImGui::Text(ICON_FA_MEMORY " Memory");
    ImGui::Text("System: %.1f MB | Video: %.1f MB",
        m_currentFrame->systemMemoryUsage / (1024.0f * 1024.0f),
        m_currentFrame->videoMemoryUsage / (1024.0f * 1024.0f));
}

void PerformanceProfiler::RenderCPUProfilerPanel() {
    if (!m_currentFrame) return;

    ImGui::Text(ICON_FA_MICROCHIP " CPU Breakdown");
    ImGui::Separator();

    // Simulated CPU breakdown bars
    struct TimingEntry { const char* name; float ms; ImVec4 color; };
    TimingEntry entries[] = {
        { "Update",     m_currentFrame->updateTime,   ImVec4(0.3f, 0.6f, 0.9f, 1.0f) },
        { "Physics",    m_currentFrame->physicsTime,   ImVec4(0.2f, 0.8f, 0.4f, 1.0f) },
        { "Render Prep", m_currentFrame->cpuTime * 0.25f, ImVec4(0.9f, 0.6f, 0.2f, 1.0f) },
        { "Audio",      m_currentFrame->audioTime,     ImVec4(0.7f, 0.3f, 0.7f, 1.0f) },
        { "Scripts",    m_currentFrame->cpuTime * 0.15f, ImVec4(0.9f, 0.9f, 0.3f, 1.0f) },
    };

    float totalCpu = m_currentFrame->cpuTime;
    for (const auto& entry : entries) {
        float pct = totalCpu > 0 ? (entry.ms / totalCpu) : 0;
        ImGui::TextColored(entry.color, ICON_FA_SQUARE);
        ImGui::SameLine();
        ImGui::Text("%-12s", entry.name);
        ImGui::SameLine(160);
        ImGui::ProgressBar(pct, ImVec2(200, 16), "");
        ImGui::SameLine();
        ImGui::Text("%.2fms (%.0f%%)", entry.ms, pct * 100);
    }

    // CPU time graph
    ImGui::Spacing();
    if (m_performanceCounters.size() > 1 && !m_performanceCounters[1].history.empty()) {
        ImGui::Text("CPU Time History");
        ImGui::PlotLines("##CPUTime", m_performanceCounters[1].history.data(),
            (int)m_performanceCounters[1].history.size(), 0, nullptr, 0, 20, ImVec2(-1, 60));
    }
}

void PerformanceProfiler::RenderGPUProfilerPanel() {
    if (!m_currentFrame) return;

    ImGui::Text(ICON_FA_TV " GPU Breakdown");
    ImGui::Separator();

    struct GPUEntry { const char* name; float ms; ImVec4 color; };
    GPUEntry entries[] = {
        { "Shadow Pass",   m_currentFrame->gpuTime * 0.25f, ImVec4(0.4f, 0.4f, 0.7f, 1.0f) },
        { "G-Buffer",      m_currentFrame->gpuTime * 0.20f, ImVec4(0.3f, 0.7f, 0.5f, 1.0f) },
        { "Lighting",      m_currentFrame->gpuTime * 0.30f, ImVec4(0.9f, 0.7f, 0.2f, 1.0f) },
        { "Post Process",  m_currentFrame->gpuTime * 0.15f, ImVec4(0.7f, 0.3f, 0.5f, 1.0f) },
        { "UI",            m_currentFrame->gpuTime * 0.10f, ImVec4(0.5f, 0.5f, 0.8f, 1.0f) },
    };

    float totalGpu = m_currentFrame->gpuTime;
    for (const auto& entry : entries) {
        float pct = totalGpu > 0 ? (entry.ms / totalGpu) : 0;
        ImGui::TextColored(entry.color, ICON_FA_SQUARE);
        ImGui::SameLine();
        ImGui::Text("%-14s", entry.name);
        ImGui::SameLine(170);
        ImGui::ProgressBar(pct, ImVec2(200, 16), "");
        ImGui::SameLine();
        ImGui::Text("%.2fms (%.0f%%)", entry.ms, pct * 100);
    }

    ImGui::Spacing();
    if (m_performanceCounters.size() > 2 && !m_performanceCounters[2].history.empty()) {
        ImGui::Text("GPU Time History");
        ImGui::PlotLines("##GPUTime", m_performanceCounters[2].history.data(),
            (int)m_performanceCounters[2].history.size(), 0, nullptr, 0, 20, ImVec2(-1, 60));
    }
}

void PerformanceProfiler::RenderMemoryProfilerPanel() {
    if (!m_currentFrame) return;

    ImGui::Text(ICON_FA_MEMORY " Memory Overview");
    ImGui::Separator();

    float sysMB = m_currentFrame->systemMemoryUsage / (1024.0f * 1024.0f);
    float vidMB = m_currentFrame->videoMemoryUsage / (1024.0f * 1024.0f);
    float budgetMB = m_config.memoryBudget / (1024.0f * 1024.0f);

    // Memory bars
    ImGui::Text("System RAM:");
    ImGui::SameLine(120);
    ImGui::ProgressBar(sysMB / budgetMB, ImVec2(300, 18), "");
    ImGui::SameLine();
    ImGui::Text("%.0f / %.0f MB", sysMB, budgetMB);

    ImGui::Text("Video RAM:");
    ImGui::SameLine(120);
    ImGui::ProgressBar(vidMB / 1024.0f, ImVec2(300, 18), "");
    ImGui::SameLine();
    ImGui::Text("%.0f / 1024 MB", vidMB);

    // Memory history
    ImGui::Spacing();
    if (m_performanceCounters.size() > 5 && !m_performanceCounters[5].history.empty()) {
        ImGui::Text("Memory Usage History (MB)");
        ImGui::PlotLines("##MemHistory", m_performanceCounters[5].history.data(),
            (int)m_performanceCounters[5].history.size(), 0, nullptr, 0,
            budgetMB * 0.5f, ImVec2(-1, 60));
    }

    // Category breakdown
    ImGui::Spacing();
    ImGui::Text("Category Breakdown (simulated):");
    if (ImGui::BeginTable("MemTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("Allocated");
        ImGui::TableSetupColumn("Peak");
        ImGui::TableHeadersRow();

        auto MemRow = [](const char* cat, float allocMB, float peakMB) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", cat);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f MB", allocMB);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f MB", peakMB);
        };

        MemRow("Textures", sysMB * 0.35f, sysMB * 0.40f);
        MemRow("Meshes", sysMB * 0.20f, sysMB * 0.22f);
        MemRow("Audio", sysMB * 0.08f, sysMB * 0.10f);
        MemRow("Scripts", sysMB * 0.05f, sysMB * 0.06f);
        MemRow("Physics", sysMB * 0.12f, sysMB * 0.15f);
        MemRow("Other", sysMB * 0.20f, sysMB * 0.22f);

        ImGui::EndTable();
    }
}

void PerformanceProfiler::RenderPerformanceCountersPanel() {}
void PerformanceProfiler::RenderOptimizationPanel() {}
void PerformanceProfiler::RenderConfigurationPanel() {}
void PerformanceProfiler::UpdateFrameData() {}
void PerformanceProfiler::AnalyzePerformance() {}
void PerformanceProfiler::GenerateOptimizationSuggestions() {}
void PerformanceProfiler::DetectCPUBottlenecks() {}
void PerformanceProfiler::DetectGPUBottlenecks() {}
void PerformanceProfiler::DetectMemoryBottlenecks() {}
void PerformanceProfiler::ProcessGPUQueries() {}
void PerformanceProfiler::UpdateMemoryTracking() {}
void PerformanceProfiler::CalculateStatistics() {}

void PerformanceProfiler::RenderCPUSampleHierarchy(const CPUProfileSample* sample, int depth) {
    if (!sample) return;
    ImGui::Text("%*s%s: %.2fms", depth * 2, "", sample->name.c_str(), sample->duration);
    for (const auto& child : sample->children) {
        RenderCPUSampleHierarchy(child.get(), depth + 1);
    }
}

void PerformanceProfiler::RenderPerformanceGraph(const PerformanceCounter& counter, const XMFLOAT2& size) {
    if (counter.history.empty()) return;
    ImGui::PlotLines(("##" + counter.name).c_str(), counter.history.data(),
        (int)counter.history.size(), 0, counter.name.c_str(), 0,
        counter.maxValue * 1.2f, ImVec2(size.x, size.y));
}

} // namespace SparkEditor
