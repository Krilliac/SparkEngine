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

namespace SparkEditor {

PerformanceProfiler::PerformanceProfiler() 
    : EditorPanel("Performance Profiler", "performance_profiler_panel") {
}

PerformanceProfiler::~PerformanceProfiler() {
    // Destructor
}

bool PerformanceProfiler::Initialize() {
    std::cout << "Initializing Performance Profiler panel\n";
    
    // Initialize built-in performance counters
    InitializeBuiltInCounters();
    
    // Start profiling by default
    m_isProfilerRunning = true;
    
    return true;
}

void PerformanceProfiler::Update(float deltaTime) {
    if (!m_isProfilerRunning) return;
    
    auto now = std::chrono::steady_clock::now();
    
    // Update frame time counter
    auto frameTimeCounter = GetCounter("Frame Time");
    if (frameTimeCounter) {
        frameTimeCounter->AddSample(deltaTime * 1000.0f); // Convert to ms
    }
    
    // Update FPS counter
    auto fpsCounter = GetCounter("FPS");
    if (fpsCounter && deltaTime > 0.0f) {
        fpsCounter->AddSample(1.0f / deltaTime);
    }
    
    // Simulate some performance data
    UpdateSimulatedCounters(deltaTime);
    
    // Update profiling session
    if (m_currentSession.isActive) {
        m_currentSession.currentTime = std::chrono::duration<float>(now - m_currentSession.startTime).count();
        
        if (m_currentSession.duration > 0.0f && m_currentSession.currentTime >= m_currentSession.duration) {
            StopProfiling();
        }
    }
}

void PerformanceProfiler::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        // Profiler toolbar
        RenderProfilerToolbar();
        
        ImGui::Separator();
        
        // Tab bar for different views
        if (ImGui::BeginTabBar("ProfilerTabs")) {
            
            if (ImGui::BeginTabItem("Real-Time")) {
                RenderRealTimeView();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("Counters")) {
                RenderCountersView();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("CPU Profiling")) {
                RenderCPUProfilingView();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("Memory")) {
                RenderMemoryView();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("GPU")) {
                RenderGPUView();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("Analysis")) {
                RenderAnalysisView();
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

bool PerformanceProfiler::HandleEvent(const std::string& eventType, void* eventData) {
    return false;
}

void PerformanceProfiler::StartProfiling(float duration) {
    m_currentSession.isActive = true;
    m_currentSession.startTime = std::chrono::steady_clock::now();
    m_currentSession.duration = duration;
    m_currentSession.currentTime = 0.0f;
    m_currentSession.sampleCount = 0;
    
    // Clear CPU profiling data
    m_cpuSamples.clear();
    
    m_isProfilerRunning = true;
    std::cout << "Started profiling session";
    if (duration > 0.0f) {
        std::cout << " for " << duration << " seconds";
    }
    std::cout << "\n";
}

void PerformanceProfiler::StopProfiling() {
    m_currentSession.isActive = false;
    m_isProfilerRunning = false;
    
    if (m_currentSession.sampleCount > 0) {
        std::cout << "Stopped profiling session. Collected " << m_currentSession.sampleCount << " samples\n";
        GenerateProfilingReport();
    }
}

void PerformanceProfiler::AddCounter(const PerformanceCounter& counter) {
    m_counters[counter.name] = counter;
}

void PerformanceProfiler::RemoveCounter(const std::string& name) {
    m_counters.erase(name);
}

PerformanceCounter* PerformanceProfiler::GetCounter(const std::string& name) {
    auto it = m_counters.find(name);
    return (it != m_counters.end()) ? &it->second : nullptr;
}

void PerformanceProfiler::BeginCPUSample(const std::string& name, const std::string& category) {
    if (!m_isProfilerRunning) return;
    
    CPUProfileSample sample;
    sample.name = name;
    sample.category = category;
    sample.startTime = std::chrono::high_resolution_clock::now();
    sample.isActive = true;
    
    m_activeCPUSamples[name] = sample;
}

void PerformanceProfiler::EndCPUSample(const std::string& name) {
    if (!m_isProfilerRunning) return;
    
    auto it = m_activeCPUSamples.find(name);
    if (it != m_activeCPUSamples.end()) {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<float, std::milli>(endTime - it->second.startTime).count();
        
        it->second.endTime = endTime;
        it->second.duration = duration;
        it->second.isActive = false;
        
        // Add to completed samples
        m_cpuSamples.push_back(it->second);
        
        // Remove from active samples
        m_activeCPUSamples.erase(it);
        
        m_currentSession.sampleCount++;
        
        // Update CPU counter
        auto cpuCounter = GetCounter("CPU Sample: " + name);
        if (cpuCounter) {
            cpuCounter->AddSample(duration);
        }
    }
}

void PerformanceProfiler::AddMemorySample(const MemoryProfileSample& sample) {
    if (!m_isProfilerRunning) return;
    
    m_memorySamples.push_back(sample);
    
    // Update memory counters
    auto totalMemCounter = GetCounter("Total Memory");
    if (totalMemCounter) {
        totalMemCounter->AddSample(static_cast<float>(sample.totalMemory / 1024 / 1024)); // MB
    }
    
    auto usedMemCounter = GetCounter("Used Memory");
    if (usedMemCounter) {
        usedMemCounter->AddSample(static_cast<float>(sample.usedMemory / 1024 / 1024)); // MB
    }
}

void PerformanceProfiler::AddGPUSample(const GPUProfileSample& sample) {
    if (!m_isProfilerRunning) return;
    
    m_gpuSamples.push_back(sample);
    
    // Update GPU counters
    auto gpuTimeCounter = GetCounter("GPU Frame Time");
    if (gpuTimeCounter) {
        gpuTimeCounter->AddSample(sample.frameTime);
    }
    
    auto drawCallsCounter = GetCounter("Draw Calls");
    if (drawCallsCounter) {
        drawCallsCounter->AddSample(static_cast<float>(sample.drawCalls));
    }
}

ProfilingReport PerformanceProfiler::GenerateReport() const {
    ProfilingReport report;
    report.sessionDuration = m_currentSession.currentTime;
    report.totalSamples = m_currentSession.sampleCount;
    report.timestamp = std::chrono::system_clock::now();
    
    // Generate summary statistics for each counter
    for (const auto& [name, counter] : m_counters) {
        if (!counter.history.empty()) {
            CounterSummary summary;
            summary.counterName = name;
            summary.sampleCount = counter.history.size();
            summary.averageValue = counter.averageValue;
            summary.minValue = counter.minValue;
            summary.maxValue = counter.maxValue;
            summary.currentValue = counter.currentValue;
            
            report.counterSummaries[name] = summary;
        }
    }
    
    return report;
}

bool PerformanceProfiler::ExportReport(const std::string& filePath, ReportFormat format) const {
    auto report = GenerateReport();
    
    try {
        std::ofstream file(filePath);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for export: " << filePath << "\n";
            return false;
        }
        
        switch (format) {
            case ReportFormat::TEXT:
                ExportTextReport(file, report);
                break;
            case ReportFormat::CSV:
                ExportCSVReport(file, report);
                break;
            case ReportFormat::JSON:
                ExportJSONReport(file, report);
                break;
        }
        
        file.close();
        std::cout << "Exported profiling report to: " << filePath << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error exporting report: " << e.what() << "\n";
        return false;
    }
}

void PerformanceProfiler::ClearData() {
    // Clear all collected data
    for (auto& [name, counter] : m_counters) {
        counter.Clear();
    }
    
    m_cpuSamples.clear();
    m_memorySamples.clear();
    m_gpuSamples.clear();
    m_activeCPUSamples.clear();
    
    m_currentSession.sampleCount = 0;
    
    std::cout << "Cleared all profiling data\n";
}

// Private implementation methods

void PerformanceProfiler::InitializeBuiltInCounters() {
    // Frame time counter
    PerformanceCounter frameTime;
    frameTime.name = "Frame Time";
    frameTime.type = ProfilerSampleType::CPU_SAMPLE;
    frameTime.unit = "ms";
    frameTime.color = XMFLOAT4(1.0f, 0.5f, 0.0f, 1.0f); // Orange
    AddCounter(frameTime);
    
    // FPS counter
    PerformanceCounter fps;
    fps.name = "FPS";
    fps.type = ProfilerSampleType::CPU_SAMPLE;
    fps.unit = "fps";
    fps.color = XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f); // Green
    AddCounter(fps);
    
    // Memory counters
    PerformanceCounter totalMem;
    totalMem.name = "Total Memory";
    totalMem.type = ProfilerSampleType::MEMORY_SAMPLE;
    totalMem.unit = "MB";
    totalMem.color = XMFLOAT4(0.0f, 0.5f, 1.0f, 1.0f); // Blue
    AddCounter(totalMem);
    
    PerformanceCounter usedMem;
    usedMem.name = "Used Memory";
    usedMem.type = ProfilerSampleType::MEMORY_SAMPLE;
    usedMem.unit = "MB";
    usedMem.color = XMFLOAT4(1.0f, 0.0f, 0.5f, 1.0f); // Pink
    AddCounter(usedMem);
    
    // GPU counters
    PerformanceCounter gpuTime;
    gpuTime.name = "GPU Frame Time";
    gpuTime.type = ProfilerSampleType::GPU_SAMPLE;
    gpuTime.unit = "ms";
    gpuTime.color = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow
    AddCounter(gpuTime);
    
    PerformanceCounter drawCalls;
    drawCalls.name = "Draw Calls";
    drawCalls.type = ProfilerSampleType::RENDERING_SAMPLE;
    drawCalls.unit = "calls";
    drawCalls.color = XMFLOAT4(0.5f, 0.0f, 1.0f, 1.0f); // Purple
    AddCounter(drawCalls);
}

void PerformanceProfiler::UpdateSimulatedCounters(float deltaTime) {
    // Simulate some realistic performance data for demonstration
    static float timeAccumulator = 0.0f;
    timeAccumulator += deltaTime;
    
    // Memory usage simulation
    static size_t baseMemory = 256 * 1024 * 1024; // 256 MB base
    size_t currentMemory = baseMemory + static_cast<size_t>(50 * 1024 * 1024 * sin(timeAccumulator * 0.5f));
    
    MemoryProfileSample memSample;
    memSample.timestamp = std::chrono::steady_clock::now();
    memSample.totalMemory = 1024 * 1024 * 1024; // 1 GB total
    memSample.usedMemory = currentMemory;
    memSample.availableMemory = memSample.totalMemory - memSample.usedMemory;
    
    AddMemorySample(memSample);
    
    // GPU simulation
    GPUProfileSample gpuSample;
    gpuSample.timestamp = std::chrono::steady_clock::now();
    gpuSample.frameTime = 16.67f + 2.0f * sin(timeAccumulator * 2.0f); // ~60fps with variation
    gpuSample.drawCalls = 150 + static_cast<int>(50 * cos(timeAccumulator * 1.5f));
    gpuSample.triangles = gpuSample.drawCalls * 500; // Estimate
    gpuSample.memoryUsage = 512 * 1024 * 1024; // 512 MB VRAM
    
    AddGPUSample(gpuSample);
}

void PerformanceProfiler::RenderProfilerToolbar() {
    if (m_isProfilerRunning) {
        if (ImGui::Button("Stop Profiling")) {
            StopProfiling();
        }
    } else {
        if (ImGui::Button("Start Profiling")) {
            StartProfiling(0.0f); // Indefinite duration
        }
        ImGui::SameLine();
        if (ImGui::Button("Profile 30s")) {
            StartProfiling(30.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Profile 60s")) {
            StartProfiling(60.0f);
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Clear Data")) {
        ClearData();
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Export Report")) {
        ExportReport("profiling_report.txt", ReportFormat::TEXT);
    }
    
    // Show session status
    if (m_currentSession.isActive) {
        ImGui::SameLine();
        ImGui::Text("| Running: %.1fs", m_currentSession.currentTime);
        if (m_currentSession.duration > 0.0f) {
            ImGui::SameLine();
            ImGui::Text("/ %.1fs", m_currentSession.duration);
        }
        ImGui::SameLine();
        ImGui::Text("| Samples: %d", m_currentSession.sampleCount);
    }
}

void PerformanceProfiler::RenderRealTimeView() {
    // Show real-time graphs of key performance metrics
    const float plotHeight = 100.0f;
    
    for (auto& [name, counter] : m_counters) {
        if (!counter.isActive || counter.history.empty()) continue;
        
        ImVec4 color = ImVec4(counter.color.x, counter.color.y, counter.color.z, counter.color.w);
        
        // Plot the counter history
        ImGui::Text("%s: %.2f %s", name.c_str(), counter.currentValue, counter.unit.c_str());
        
        // Convert history to float array for ImGui
        std::vector<float> plotData(counter.history.begin(), counter.history.end());
        
        ImGui::PlotLines(("##" + name).c_str(), plotData.data(), static_cast<int>(plotData.size()),
                        0, nullptr, counter.minValue, counter.maxValue, ImVec2(0, plotHeight));
        
        // Show statistics
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("Avg: %.2f", counter.averageValue);
        ImGui::Text("Min: %.2f", counter.minValue);
        ImGui::Text("Max: %.2f", counter.maxValue);
        ImGui::EndGroup();
        
        ImGui::Separator();
    }
}

void PerformanceProfiler::RenderCountersView() {
    // Show detailed counter information in table format
    if (ImGui::BeginTable("CountersTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Average");
        ImGui::TableSetupColumn("Min");
        ImGui::TableSetupColumn("Max");
        ImGui::TableSetupColumn("Unit");
        ImGui::TableSetupColumn("Samples");
        ImGui::TableHeadersRow();
        
        for (const auto& [name, counter] : m_counters) {
            if (!counter.isActive) continue;
            
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", name.c_str());
            
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.2f", counter.currentValue);
            
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f", counter.averageValue);
            
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.2f", counter.minValue);
            
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", counter.maxValue);
            
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%s", counter.unit.c_str());
            
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%zu", counter.history.size());
        }
        
        ImGui::EndTable();
    }
}

void PerformanceProfiler::RenderCPUProfilingView() {
    ImGui::Text("CPU Profiling Samples: %zu", m_cpuSamples.size());
    
    if (ImGui::BeginTable("CPUSamplesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("Duration (ms)");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        
        // Show recent samples
        for (auto it = m_cpuSamples.rbegin(); it != m_cpuSamples.rend() && it < m_cpuSamples.rbegin() + 100; ++it) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", it->name.c_str());
            
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", it->category.c_str());
            
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", it->duration);
            
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("Completed");
        }
        
        ImGui::EndTable();
    }
}

void PerformanceProfiler::RenderMemoryView() {
    if (m_memorySamples.empty()) {
        ImGui::Text("No memory samples available");
        return;
    }
    
    const auto& latestSample = m_memorySamples.back();
    
    ImGui::Text("Memory Usage Overview");
    ImGui::Separator();
    
    float usedMB = static_cast<float>(latestSample.usedMemory) / (1024 * 1024);
    float totalMB = static_cast<float>(latestSample.totalMemory) / (1024 * 1024);
    float usageRatio = usedMB / totalMB;
    
    ImGui::Text("Used: %.1f MB / %.1f MB (%.1f%%)", usedMB, totalMB, usageRatio * 100.0f);
    ImGui::ProgressBar(usageRatio, ImVec2(0.0f, 0.0f));
    
    // Memory history graph
    if (m_memorySamples.size() > 1) {
        std::vector<float> memoryHistory;
        for (const auto& sample : m_memorySamples) {
            memoryHistory.push_back(static_cast<float>(sample.usedMemory) / (1024 * 1024));
        }
        
        ImGui::PlotLines("Memory Usage (MB)", memoryHistory.data(), static_cast<int>(memoryHistory.size()),
                        0, nullptr, 0.0f, totalMB, ImVec2(0, 150));
    }
}

void PerformanceProfiler::RenderGPUView() {
    if (m_gpuSamples.empty()) {
        ImGui::Text("No GPU samples available");
        return;
    }
    
    const auto& latestSample = m_gpuSamples.back();
    
    ImGui::Text("GPU Performance Overview");
    ImGui::Separator();
    
    ImGui::Text("Frame Time: %.2f ms", latestSample.frameTime);
    ImGui::Text("Draw Calls: %d", latestSample.drawCalls);
    ImGui::Text("Triangles: %d", latestSample.triangles);
    ImGui::Text("VRAM Usage: %.1f MB", latestSample.memoryUsage / (1024.0f * 1024.0f));
    
    // GPU frame time history
    if (m_gpuSamples.size() > 1) {
        std::vector<float> frameTimeHistory;
        for (const auto& sample : m_gpuSamples) {
            frameTimeHistory.push_back(sample.frameTime);
        }
        
        ImGui::PlotLines("GPU Frame Time (ms)", frameTimeHistory.data(), static_cast<int>(frameTimeHistory.size()),
                        0, nullptr, 0.0f, 33.33f, ImVec2(0, 100)); // 0-33ms range (30-inf FPS)
    }
}

void PerformanceProfiler::RenderAnalysisView() {
    ImGui::Text("Performance Analysis");
    ImGui::Separator();
    
    auto report = GenerateReport();
    
    ImGui::Text("Session Duration: %.1f seconds", report.sessionDuration);
    ImGui::Text("Total Samples: %d", report.totalSamples);
    
    ImGui::Separator();
    ImGui::Text("Performance Summary:");
    
    for (const auto& [name, summary] : report.counterSummaries) {
        ImGui::Text("%s:", name.c_str());
        ImGui::Indent();
        ImGui::Text("Current: %.2f", summary.currentValue);
        ImGui::Text("Average: %.2f", summary.averageValue);
        ImGui::Text("Range: %.2f - %.2f", summary.minValue, summary.maxValue);
        ImGui::Text("Samples: %d", summary.sampleCount);
        ImGui::Unindent();
        ImGui::Separator();
    }
}

void PerformanceProfiler::GenerateProfilingReport() const {
    auto report = GenerateReport();
    
    std::cout << "\n=== Performance Profiling Report ===\n";
    std::cout << "Session Duration: " << report.sessionDuration << " seconds\n";
    std::cout << "Total Samples: " << report.totalSamples << "\n";
    std::cout << "\nCounter Summary:\n";
    
    for (const auto& [name, summary] : report.counterSummaries) {
        std::cout << "  " << name << ": ";
        std::cout << "Current=" << summary.currentValue;
        std::cout << ", Avg=" << summary.averageValue;
        std::cout << ", Range=" << summary.minValue << "-" << summary.maxValue;
        std::cout << " (" << summary.sampleCount << " samples)\n";
    }
    
    std::cout << "====================================\n\n";
}

void PerformanceProfiler::ExportTextReport(std::ofstream& file, const ProfilingReport& report) const {
    file << "Performance Profiling Report\n";
    file << "============================\n\n";
    file << "Session Duration: " << report.sessionDuration << " seconds\n";
    file << "Total Samples: " << report.totalSamples << "\n";
    file << "Generated: " << std::chrono::duration_cast<std::chrono::seconds>(
        report.timestamp.time_since_epoch()).count() << "\n\n";
    
    file << "Counter Summary:\n";
    for (const auto& [name, summary] : report.counterSummaries) {
        file << "  " << name << ":\n";
        file << "    Current: " << summary.currentValue << "\n";
        file << "    Average: " << summary.averageValue << "\n";
        file << "    Minimum: " << summary.minValue << "\n";
        file << "    Maximum: " << summary.maxValue << "\n";
        file << "    Samples: " << summary.sampleCount << "\n\n";
    }
}

void PerformanceProfiler::ExportCSVReport(std::ofstream& file, const ProfilingReport& report) const {
    file << "Counter,Current,Average,Minimum,Maximum,Samples\n";
    for (const auto& [name, summary] : report.counterSummaries) {
        file << name << "," << summary.currentValue << "," << summary.averageValue << ","
             << summary.minValue << "," << summary.maxValue << "," << summary.sampleCount << "\n";
    }
}

void PerformanceProfiler::ExportJSONReport(std::ofstream& file, const ProfilingReport& report) const {
    file << "{\n";
    file << "  \"sessionDuration\": " << report.sessionDuration << ",\n";
    file << "  \"totalSamples\": " << report.totalSamples << ",\n";
    file << "  \"timestamp\": " << std::chrono::duration_cast<std::chrono::seconds>(
        report.timestamp.time_since_epoch()).count() << ",\n";
    file << "  \"counters\": {\n";
    
    bool first = true;
    for (const auto& [name, summary] : report.counterSummaries) {
        if (!first) file << ",\n";
        file << "    \"" << name << "\": {\n";
        file << "      \"current\": " << summary.currentValue << ",\n";
        file << "      \"average\": " << summary.averageValue << ",\n";
        file << "      \"minimum\": " << summary.minValue << ",\n";
        file << "      \"maximum\": " << summary.maxValue << ",\n";
        file << "      \"samples\": " << summary.sampleCount << "\n";
        file << "    }";
        first = false;
    }
    
    file << "\n  }\n";
    file << "}\n";
}

} // namespace SparkEditor