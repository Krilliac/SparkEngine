/**
 * @file PerformanceProfilerPanel.cpp
 * @brief Runtime performance profiler with frame timeline and subsystem breakdown
 * @author Spark Engine Team
 * @date 2025
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <sstream>
#include <iomanip>

#include <imgui.h>

#include "PerformanceProfilerPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

namespace SparkEditor
{

    // ========================================================================
    // Color constants
    // ========================================================================

    static constexpr ImU32 COLOR_GREEN = IM_COL32(100, 220, 100, 255);
    static constexpr ImU32 COLOR_YELLOW = IM_COL32(220, 200, 60, 255);
    static constexpr ImU32 COLOR_RED = IM_COL32(220, 60, 60, 255);
    static constexpr ImU32 COLOR_TARGET_LINE = IM_COL32(100, 180, 255, 180);
    static constexpr ImU32 COLOR_GRID = IM_COL32(255, 255, 255, 30);
    static constexpr ImU32 COLOR_GRAPH_BG = IM_COL32(30, 30, 30, 200);
    static constexpr ImU32 COLOR_SELECTED_FRAME = IM_COL32(255, 255, 255, 100);

    // Subsystem colors
    static constexpr ImU32 COLOR_PHYSICS = IM_COL32(65, 135, 245, 255);
    static constexpr ImU32 COLOR_AI = IM_COL32(245, 165, 35, 255);
    static constexpr ImU32 COLOR_ANIMATION = IM_COL32(155, 89, 182, 255);
    static constexpr ImU32 COLOR_AUDIO = IM_COL32(46, 204, 113, 255);
    static constexpr ImU32 COLOR_SCRIPTING = IM_COL32(231, 76, 60, 255);
    static constexpr ImU32 COLOR_RENDERING = IM_COL32(52, 152, 219, 255);
    static constexpr ImU32 COLOR_PARTICLES = IM_COL32(241, 196, 15, 255);
    static constexpr ImU32 COLOR_OTHER = IM_COL32(149, 165, 166, 255);

    // Alert severity colors
    static constexpr ImU32 COLOR_ALERT_INFO = IM_COL32(100, 180, 255, 255);
    static constexpr ImU32 COLOR_ALERT_WARNING = IM_COL32(255, 200, 60, 255);
    static constexpr ImU32 COLOR_ALERT_CRITICAL = IM_COL32(255, 60, 60, 255);

    // ========================================================================
    // Helper functions
    // ========================================================================

    static ImU32 GetFrameTimeColor(float frameTimeMs, const PerformanceThresholds& thresholds)
    {
        if (frameTimeMs <= thresholds.targetFrameTimeMs)
        {
            return COLOR_GREEN;
        }
        else if (frameTimeMs <= thresholds.warningFrameTimeMs)
        {
            // Lerp green -> yellow
            float t = (frameTimeMs - thresholds.targetFrameTimeMs) /
                      (thresholds.warningFrameTimeMs - thresholds.targetFrameTimeMs);
            t = std::clamp(t, 0.0f, 1.0f);
            int r = static_cast<int>(100 + t * 120);
            int g = static_cast<int>(220 - t * 20);
            int b = static_cast<int>(100 - t * 40);
            return IM_COL32(r, g, b, 255);
        }
        else if (frameTimeMs <= thresholds.criticalFrameTimeMs)
        {
            // Lerp yellow -> red
            float t = (frameTimeMs - thresholds.warningFrameTimeMs) /
                      (thresholds.criticalFrameTimeMs - thresholds.warningFrameTimeMs);
            t = std::clamp(t, 0.0f, 1.0f);
            int r = static_cast<int>(220);
            int g = static_cast<int>(200 - t * 140);
            int b = static_cast<int>(60);
            return IM_COL32(r, g, b, 255);
        }
        return COLOR_RED;
    }

    static const char* GetSeverityLabel(PerformanceAlert::Severity severity)
    {
        switch (severity)
        {
        case PerformanceAlert::Severity::Info:
            return "INFO";
        case PerformanceAlert::Severity::Warning:
            return "WARN";
        case PerformanceAlert::Severity::Critical:
            return "CRIT";
        }
        return "???";
    }

    static ImU32 GetSeverityColor(PerformanceAlert::Severity severity)
    {
        switch (severity)
        {
        case PerformanceAlert::Severity::Info:
            return COLOR_ALERT_INFO;
        case PerformanceAlert::Severity::Warning:
            return COLOR_ALERT_WARNING;
        case PerformanceAlert::Severity::Critical:
            return COLOR_ALERT_CRITICAL;
        }
        return COLOR_ALERT_INFO;
    }

    static std::string FormatBytes(size_t bytes)
    {
        if (bytes < 1024)
        {
            return std::to_string(bytes) + " B";
        }
        else if (bytes < 1024 * 1024)
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0);
            return oss.str() + " KB";
        }
        else if (bytes < 1024ULL * 1024 * 1024)
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / (1024.0 * 1024.0));
            return oss.str() + " MB";
        }
        else
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
            return oss.str() + " GB";
        }
    }

    // ========================================================================
    // Constructor / Lifecycle
    // ========================================================================

    PerformanceProfilerPanel::PerformanceProfilerPanel()
        : EditorPanel("Performance Profiler", "performance_profiler_panel")
    {
        m_icon = ICON_FA_CHART_BAR;
        m_frameHistory.fill(FrameTimingRecord{});
    }

    bool PerformanceProfilerPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Performance Profiler panel\n";
        ClearData();
        m_isInitialized = true;
        return true;
    }

    void PerformanceProfilerPanel::Update(float deltaTime)
    {
        (void)deltaTime;
        // Frame data is pushed externally via RecordFrame(). Nothing to poll here.
    }

    void PerformanceProfilerPanel::Render()
    {
        if (!IsVisible())
        {
            return;
        }

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            switch (m_graphMode)
            {
            case GraphMode::FrameTime:
            case GraphMode::FPS:
                RenderFrameTimeGraph();
                break;
            case GraphMode::SubsystemBreakdown:
                RenderSubsystemBreakdown();
                break;
            case GraphMode::Memory:
                RenderMemorySection();
                break;
            }

            ImGui::Separator();
            RenderCPUGPUSplit();

            ImGui::Separator();
            RenderDrawCallSection();

            ImGui::Separator();
            RenderStatisticsSummary();

            ImGui::Separator();
            RenderCaptureControls();

            if (m_showAlertPanel)
            {
                ImGui::Separator();
                RenderAlerts();
            }
        }
        EndPanel();
    }

    void PerformanceProfilerPanel::Shutdown()
    {
        std::cout << "Shutting down Performance Profiler panel\n";
        StopCapture();
        ClearData();
        m_isInitialized = false;
    }

    bool PerformanceProfilerPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        if (eventType == "frame_timing")
        {
            if (eventData != nullptr)
            {
                const auto* record = static_cast<const FrameTimingRecord*>(eventData);
                RecordFrame(*record);
                return true;
            }
        }
        else if (eventType == "clear_profiler")
        {
            ClearData();
            return true;
        }
        else if (eventType == "start_capture")
        {
            StartCapture();
            return true;
        }
        else if (eventType == "stop_capture")
        {
            StopCapture();
            return true;
        }
        return false;
    }

    // ========================================================================
    // Data Management
    // ========================================================================

    void PerformanceProfilerPanel::RecordFrame(const FrameTimingRecord& record)
    {
        if (m_pauseGraph)
        {
            return;
        }

        // Push to ring buffer
        m_frameHistory[m_frameHistoryIndex] = record;
        m_frameHistoryIndex = (m_frameHistoryIndex + 1) % MAX_FRAME_HISTORY;
        if (m_frameHistorySamples < MAX_FRAME_HISTORY)
        {
            ++m_frameHistorySamples;
        }
        ++m_totalFrames;

        // Update running statistics
        m_minFrameTimeMs = std::min(m_minFrameTimeMs, record.totalFrameMs);
        m_maxFrameTimeMs = std::max(m_maxFrameTimeMs, record.totalFrameMs);

        // Compute average from ring buffer
        float sum = 0.0f;
        size_t count = m_frameHistorySamples;
        for (size_t i = 0; i < count; ++i)
        {
            sum += m_frameHistory[i].totalFrameMs;
        }
        m_avgFrameTimeMs = (count > 0) ? (sum / static_cast<float>(count)) : 0.0f;

        // Compute 1% low (sort a copy, take bottom 1%)
        if (count >= 10)
        {
            std::vector<float> sorted;
            sorted.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                sorted.push_back(m_frameHistory[i].totalFrameMs);
            }
            std::sort(sorted.begin(), sorted.end(), std::greater<float>());

            size_t onePercentCount = std::max(static_cast<size_t>(1), count / 100);
            float worstSum = 0.0f;
            for (size_t i = 0; i < onePercentCount; ++i)
            {
                worstSum += sorted[i];
            }
            m_onePercentLowMs = worstSum / static_cast<float>(onePercentCount);
        }

        // Capture session
        if (m_isCapturing)
        {
            m_capturedFrames.push_back(record);
        }

        // Check thresholds for alerts
        CheckThresholds(record);
    }

    void PerformanceProfilerPanel::StartCapture()
    {
        if (m_isCapturing)
        {
            return;
        }
        m_isCapturing = true;
        m_capturedFrames.clear();
        m_captureStartTime = std::chrono::steady_clock::now();
        std::cout << "[Profiler] Capture started\n";
    }

    void PerformanceProfilerPanel::StopCapture()
    {
        if (!m_isCapturing)
        {
            return;
        }
        m_isCapturing = false;
        auto elapsed = std::chrono::steady_clock::now() - m_captureStartTime;
        auto elapsedSec = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;
        std::cout << "[Profiler] Capture stopped: " << m_capturedFrames.size() << " frames over " << std::fixed
                  << std::setprecision(1) << elapsedSec << "s\n";
    }

    void PerformanceProfilerPanel::ClearData()
    {
        m_frameHistory.fill(FrameTimingRecord{});
        m_frameHistoryIndex = 0;
        m_frameHistorySamples = 0;
        m_avgFrameTimeMs = 0.0f;
        m_minFrameTimeMs = 999.0f;
        m_maxFrameTimeMs = 0.0f;
        m_onePercentLowMs = 0.0f;
        m_totalFrames = 0;
        m_alerts.clear();
        m_selectedFrame = -1;
    }

    // ========================================================================
    // Threshold Checking
    // ========================================================================

    void PerformanceProfilerPanel::CheckThresholds(const FrameTimingRecord& record)
    {
        if (!m_thresholds.enableAlerts)
        {
            return;
        }

        float elapsedSec = static_cast<float>(m_totalFrames) * m_avgFrameTimeMs / 1000.0f;

        // Frame time checks
        if (record.totalFrameMs > m_thresholds.criticalFrameTimeMs)
        {
            PerformanceAlert alert;
            alert.severity = PerformanceAlert::Severity::Critical;
            alert.timestamp = elapsedSec;
            alert.value = record.totalFrameMs;
            alert.threshold = m_thresholds.criticalFrameTimeMs;

            std::ostringstream oss;
            oss << "Frame spike: " << std::fixed << std::setprecision(1) << record.totalFrameMs
                << "ms (limit: " << m_thresholds.criticalFrameTimeMs << "ms)";
            alert.message = oss.str();
            m_alerts.push_back(alert);
        }
        else if (record.totalFrameMs > m_thresholds.warningFrameTimeMs)
        {
            PerformanceAlert alert;
            alert.severity = PerformanceAlert::Severity::Warning;
            alert.timestamp = elapsedSec;
            alert.value = record.totalFrameMs;
            alert.threshold = m_thresholds.warningFrameTimeMs;

            std::ostringstream oss;
            oss << "Frame time warning: " << std::fixed << std::setprecision(1) << record.totalFrameMs
                << "ms (target: " << m_thresholds.warningFrameTimeMs << "ms)";
            alert.message = oss.str();
            m_alerts.push_back(alert);
        }

        // Memory checks
        float memoryMB = static_cast<float>(record.memoryUsageBytes) / (1024.0f * 1024.0f);
        if (memoryMB > m_thresholds.memoryLimitMB)
        {
            PerformanceAlert alert;
            alert.severity = PerformanceAlert::Severity::Critical;
            alert.timestamp = elapsedSec;
            alert.value = memoryMB;
            alert.threshold = m_thresholds.memoryLimitMB;

            std::ostringstream oss;
            oss << "Memory limit exceeded: " << std::fixed << std::setprecision(0) << memoryMB
                << " MB (limit: " << m_thresholds.memoryLimitMB << " MB)";
            alert.message = oss.str();
            m_alerts.push_back(alert);
        }
        else if (memoryMB > m_thresholds.memoryWarningMB)
        {
            PerformanceAlert alert;
            alert.severity = PerformanceAlert::Severity::Warning;
            alert.timestamp = elapsedSec;
            alert.value = memoryMB;
            alert.threshold = m_thresholds.memoryWarningMB;

            std::ostringstream oss;
            oss << "Memory warning: " << std::fixed << std::setprecision(0) << memoryMB
                << " MB (warning: " << m_thresholds.memoryWarningMB << " MB)";
            alert.message = oss.str();
            m_alerts.push_back(alert);
        }

        // Draw call checks
        if (record.drawCalls > m_thresholds.drawCallLimit)
        {
            PerformanceAlert alert;
            alert.severity = PerformanceAlert::Severity::Critical;
            alert.timestamp = elapsedSec;
            alert.value = static_cast<float>(record.drawCalls);
            alert.threshold = static_cast<float>(m_thresholds.drawCallLimit);

            std::ostringstream oss;
            oss << "Draw call limit exceeded: " << record.drawCalls << " (limit: " << m_thresholds.drawCallLimit << ")";
            alert.message = oss.str();
            m_alerts.push_back(alert);
        }
        else if (record.drawCalls > m_thresholds.drawCallWarning)
        {
            PerformanceAlert alert;
            alert.severity = PerformanceAlert::Severity::Warning;
            alert.timestamp = elapsedSec;
            alert.value = static_cast<float>(record.drawCalls);
            alert.threshold = static_cast<float>(m_thresholds.drawCallWarning);

            std::ostringstream oss;
            oss << "Draw call warning: " << record.drawCalls << " (warning: " << m_thresholds.drawCallWarning << ")";
            alert.message = oss.str();
            m_alerts.push_back(alert);
        }

        // Trim alert list
        while (m_alerts.size() > MAX_ALERTS)
        {
            m_alerts.erase(m_alerts.begin());
        }
    }

    // ========================================================================
    // UI Rendering
    // ========================================================================

    void PerformanceProfilerPanel::RenderToolbar()
    {
        // Graph mode selector
        ImGui::Text(ICON_FA_CHART_BAR " Mode:");
        ImGui::SameLine();

        if (ImGui::RadioButton("Frame Time", m_graphMode == GraphMode::FrameTime))
        {
            m_graphMode = GraphMode::FrameTime;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("FPS", m_graphMode == GraphMode::FPS))
        {
            m_graphMode = GraphMode::FPS;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Subsystems", m_graphMode == GraphMode::SubsystemBreakdown))
        {
            m_graphMode = GraphMode::SubsystemBreakdown;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Memory", m_graphMode == GraphMode::Memory))
        {
            m_graphMode = GraphMode::Memory;
        }

        // Controls row
        if (m_pauseGraph)
        {
            if (ImGui::Button(ICON_FA_PLAY " Resume"))
            {
                m_pauseGraph = false;
            }
        }
        else
        {
            if (ImGui::Button(ICON_FA_PAUSE " Pause"))
            {
                m_pauseGraph = true;
            }
        }
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_TRASH " Clear"))
        {
            ClearData();
        }
        ImGui::SameLine();

        ImGui::Checkbox("Grid", &m_showGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Target", &m_showTargetLine);
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_EXCLAMATION " Alerts", &m_showAlertPanel);

        // Threshold settings (collapsible)
        if (ImGui::TreeNode(ICON_FA_COG " Thresholds"))
        {
            ImGui::SliderFloat("Target FPS", &m_thresholds.targetFrameTimeMs, 8.0f, 33.3f, "%.1f ms");
            ImGui::SliderFloat("Warning", &m_thresholds.warningFrameTimeMs, 10.0f, 50.0f, "%.1f ms");
            ImGui::SliderFloat("Critical", &m_thresholds.criticalFrameTimeMs, 16.0f, 100.0f, "%.1f ms");
            ImGui::SliderFloat("Mem Warning (MB)", &m_thresholds.memoryWarningMB, 64.0f, 2048.0f, "%.0f MB");
            ImGui::SliderFloat("Mem Limit (MB)", &m_thresholds.memoryLimitMB, 128.0f, 4096.0f, "%.0f MB");
            ImGui::SliderInt("Draw Call Warn", &m_thresholds.drawCallWarning, 100, 10000);
            ImGui::SliderInt("Draw Call Limit", &m_thresholds.drawCallLimit, 500, 20000);
            ImGui::Checkbox("Enable Alerts", &m_thresholds.enableAlerts);
            ImGui::TreePop();
        }
    }

    void PerformanceProfilerPanel::RenderFrameTimeGraph()
    {
        const float graphHeight = 150.0f;
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, graphHeight);

        if (canvasSize.x < 10.0f || canvasSize.y < 10.0f)
        {
            return;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                COLOR_GRAPH_BG);

        // Determine Y-axis scale
        bool isFPSMode = (m_graphMode == GraphMode::FPS);
        float maxValue = 0.0f;
        size_t sampleCount = m_frameHistorySamples;

        for (size_t i = 0; i < sampleCount; ++i)
        {
            float val = m_frameHistory[i].totalFrameMs;
            if (isFPSMode && val > 0.001f)
            {
                val = 1000.0f / val;
            }
            maxValue = std::max(maxValue, val);
        }

        // Pad the scale
        if (isFPSMode)
        {
            maxValue = std::max(maxValue, 120.0f);
        }
        else
        {
            maxValue = std::max(maxValue, m_thresholds.criticalFrameTimeMs * 1.5f);
        }

        // Grid lines
        if (m_showGrid)
        {
            int gridLines = 4;
            for (int i = 1; i <= gridLines; ++i)
            {
                float fraction = static_cast<float>(i) / static_cast<float>(gridLines + 1);
                float y = canvasPos.y + canvasSize.y * (1.0f - fraction);
                drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y), COLOR_GRID);

                float gridValue = maxValue * fraction;
                std::ostringstream label;
                label << std::fixed << std::setprecision(1) << gridValue;
                if (isFPSMode)
                {
                    label << " fps";
                }
                else
                {
                    label << " ms";
                }
                drawList->AddText(ImVec2(canvasPos.x + 4.0f, y - 12.0f), IM_COL32(200, 200, 200, 150),
                                  label.str().c_str());
            }
        }

        // Target FPS line
        if (m_showTargetLine)
        {
            float targetValue = isFPSMode ? (1000.0f / m_thresholds.targetFrameTimeMs) : m_thresholds.targetFrameTimeMs;
            float targetY = canvasPos.y + canvasSize.y * (1.0f - targetValue / maxValue);
            targetY = std::clamp(targetY, canvasPos.y, canvasPos.y + canvasSize.y);

            drawList->AddLine(ImVec2(canvasPos.x, targetY), ImVec2(canvasPos.x + canvasSize.x, targetY),
                              COLOR_TARGET_LINE, 1.5f);

            std::ostringstream targetLabel;
            targetLabel << std::fixed << std::setprecision(1) << targetValue;
            if (isFPSMode)
            {
                targetLabel << " fps target";
            }
            else
            {
                targetLabel << " ms target";
            }
            drawList->AddText(ImVec2(canvasPos.x + canvasSize.x - 120.0f, targetY - 14.0f), COLOR_TARGET_LINE,
                              targetLabel.str().c_str());
        }

        // Plot frame data as a smooth line graph
        if (sampleCount >= 2)
        {
            float barWidth = canvasSize.x / static_cast<float>(MAX_FRAME_HISTORY);

            for (size_t i = 0; i < sampleCount; ++i)
            {
                // Read from ring buffer in order (oldest first)
                size_t bufferIdx;
                if (sampleCount < MAX_FRAME_HISTORY)
                {
                    bufferIdx = i;
                }
                else
                {
                    bufferIdx = (m_frameHistoryIndex + i) % MAX_FRAME_HISTORY;
                }

                float val = m_frameHistory[bufferIdx].totalFrameMs;
                if (isFPSMode && val > 0.001f)
                {
                    val = 1000.0f / val;
                }

                float fraction = std::clamp(val / maxValue, 0.0f, 1.0f);
                float x = canvasPos.x + static_cast<float>(i) * barWidth;
                float y = canvasPos.y + canvasSize.y * (1.0f - fraction);

                // Connect with previous point via line
                if (i > 0)
                {
                    size_t prevIdx;
                    if (sampleCount < MAX_FRAME_HISTORY)
                    {
                        prevIdx = i - 1;
                    }
                    else
                    {
                        prevIdx = (m_frameHistoryIndex + i - 1) % MAX_FRAME_HISTORY;
                    }

                    float prevVal = m_frameHistory[prevIdx].totalFrameMs;
                    if (isFPSMode && prevVal > 0.001f)
                    {
                        prevVal = 1000.0f / prevVal;
                    }

                    float prevFraction = std::clamp(prevVal / maxValue, 0.0f, 1.0f);
                    float prevX = canvasPos.x + static_cast<float>(i - 1) * barWidth;
                    float prevY = canvasPos.y + canvasSize.y * (1.0f - prevFraction);

                    ImU32 lineColor = GetFrameTimeColor(m_frameHistory[bufferIdx].totalFrameMs, m_thresholds);
                    drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), lineColor, 1.5f);
                }
            }

            // Selected frame highlight
            if (m_selectedFrame >= 0 && static_cast<size_t>(m_selectedFrame) < sampleCount)
            {
                float x = canvasPos.x + static_cast<float>(m_selectedFrame) * barWidth;
                drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y), COLOR_SELECTED_FRAME,
                                  1.0f);
            }
        }

        // Border
        drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                          IM_COL32(80, 80, 80, 255));

        // Reserve space and handle mouse interaction
        ImGui::Dummy(canvasSize);
        if (ImGui::IsItemHovered())
        {
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            float relX = mousePos.x - canvasPos.x;
            float barWidth = canvasSize.x / static_cast<float>(MAX_FRAME_HISTORY);
            int hoverIdx = static_cast<int>(relX / barWidth);
            hoverIdx = std::clamp(hoverIdx, 0, static_cast<int>(sampleCount) - 1);

            if (ImGui::IsMouseClicked(0))
            {
                m_selectedFrame = hoverIdx;
            }

            // Tooltip
            size_t bufferIdx;
            if (sampleCount < MAX_FRAME_HISTORY)
            {
                bufferIdx = static_cast<size_t>(hoverIdx);
            }
            else
            {
                bufferIdx = (m_frameHistoryIndex + static_cast<size_t>(hoverIdx)) % MAX_FRAME_HISTORY;
            }
            const auto& frame = m_frameHistory[bufferIdx];

            ImGui::BeginTooltip();
            ImGui::Text("Frame #%llu", static_cast<unsigned long long>(frame.frameNumber));
            ImGui::Text("Total: %.2f ms (%.0f FPS)", frame.totalFrameMs,
                        frame.totalFrameMs > 0.001f ? 1000.0f / frame.totalFrameMs : 0.0f);
            ImGui::Text("CPU: %.2f ms  GPU: %.2f ms", frame.cpuMs, frame.gpuMs);
            ImGui::Text("Draw Calls: %d  Tris: %d", frame.drawCalls, frame.triangleCount);
            ImGui::EndTooltip();
        }

        // Current frame info text
        if (sampleCount > 0)
        {
            size_t latestIdx = (m_frameHistoryIndex + MAX_FRAME_HISTORY - 1) % MAX_FRAME_HISTORY;
            const auto& latest = m_frameHistory[latestIdx];
            float fps = latest.totalFrameMs > 0.001f ? 1000.0f / latest.totalFrameMs : 0.0f;

            ImU32 textColor = GetFrameTimeColor(latest.totalFrameMs, m_thresholds);
            ImGui::PushStyleColor(ImGuiCol_Text, textColor);
            ImGui::Text("%.1f FPS  (%.2f ms)", fps, latest.totalFrameMs);
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::TextDisabled("| Samples: %zu / %zu", sampleCount, MAX_FRAME_HISTORY);
        }
    }

    void PerformanceProfilerPanel::RenderSubsystemBreakdown()
    {
        ImGui::Text(ICON_FA_CHART_BAR " Subsystem Breakdown");

        size_t sampleCount = m_frameHistorySamples;
        if (sampleCount == 0)
        {
            ImGui::TextDisabled("No data collected yet.");
            return;
        }

        const float graphHeight = 120.0f;
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, graphHeight);

        if (canvasSize.x < 10.0f)
        {
            return;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                COLOR_GRAPH_BG);

        // Find max total subsystem time for scaling
        float maxSubsystemTotal = 0.0f;
        for (size_t i = 0; i < sampleCount; ++i)
        {
            float total = m_frameHistory[i].subsystems.TotalMs();
            maxSubsystemTotal = std::max(maxSubsystemTotal, total);
        }
        maxSubsystemTotal = std::max(maxSubsystemTotal, 1.0f);

        float barWidth = canvasSize.x / static_cast<float>(MAX_FRAME_HISTORY);

        struct SubsystemInfo
        {
            float SubsystemTimingSample::*field;
            ImU32 color;
        };

        // Draw stacked bars for each frame, ordered bottom-to-top
        const SubsystemInfo subsystems[] = {
            {&SubsystemTimingSample::renderingMs, COLOR_RENDERING},
            {&SubsystemTimingSample::physicsMs, COLOR_PHYSICS},
            {&SubsystemTimingSample::aiMs, COLOR_AI},
            {&SubsystemTimingSample::animationMs, COLOR_ANIMATION},
            {&SubsystemTimingSample::audioMs, COLOR_AUDIO},
            {&SubsystemTimingSample::scriptingMs, COLOR_SCRIPTING},
            {&SubsystemTimingSample::particlesMs, COLOR_PARTICLES},
            {&SubsystemTimingSample::otherMs, COLOR_OTHER},
        };

        for (size_t i = 0; i < sampleCount; ++i)
        {
            size_t bufferIdx;
            if (sampleCount < MAX_FRAME_HISTORY)
            {
                bufferIdx = i;
            }
            else
            {
                bufferIdx = (m_frameHistoryIndex + i) % MAX_FRAME_HISTORY;
            }

            const auto& sample = m_frameHistory[bufferIdx].subsystems;
            float x = canvasPos.x + static_cast<float>(i) * barWidth;
            float currentY = canvasPos.y + canvasSize.y;

            for (const auto& sys : subsystems)
            {
                float ms = sample.*(sys.field);
                float height = (ms / maxSubsystemTotal) * canvasSize.y;
                if (height < 0.5f)
                {
                    continue;
                }

                float topY = currentY - height;
                drawList->AddRectFilled(ImVec2(x, topY), ImVec2(x + barWidth, currentY), sys.color);
                currentY = topY;
            }
        }

        drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                          IM_COL32(80, 80, 80, 255));
        ImGui::Dummy(canvasSize);

        // Legend
        ImGui::Spacing();
        struct LegendEntry
        {
            const char* name;
            ImU32 color;
        };
        const LegendEntry legend[] = {
            {"Rendering", COLOR_RENDERING}, {"Physics", COLOR_PHYSICS}, {"AI", COLOR_AI},
            {"Animation", COLOR_ANIMATION}, {"Audio", COLOR_AUDIO},     {"Scripting", COLOR_SCRIPTING},
            {"Particles", COLOR_PARTICLES}, {"Other", COLOR_OTHER},
        };

        for (size_t i = 0; i < 8; ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine();
            }
            ImVec2 pos = ImGui::GetCursorScreenPos();
            drawList->AddRectFilled(pos, ImVec2(pos.x + 10.0f, pos.y + 10.0f), legend[i].color);
            ImGui::Dummy(ImVec2(12.0f, 10.0f));
            ImGui::SameLine();
            ImGui::Text("%s", legend[i].name);
        }

        // Show averages for latest frame
        if (sampleCount > 0)
        {
            size_t latestIdx = (m_frameHistoryIndex + MAX_FRAME_HISTORY - 1) % MAX_FRAME_HISTORY;
            const auto& sub = m_frameHistory[latestIdx].subsystems;

            ImGui::Spacing();
            ImGui::Columns(4, "subsystem_cols", false);
            ImGui::Text("Physics: %.2f ms", sub.physicsMs);
            ImGui::NextColumn();
            ImGui::Text("AI: %.2f ms", sub.aiMs);
            ImGui::NextColumn();
            ImGui::Text("Animation: %.2f ms", sub.animationMs);
            ImGui::NextColumn();
            ImGui::Text("Audio: %.2f ms", sub.audioMs);
            ImGui::NextColumn();
            ImGui::Text("Scripting: %.2f ms", sub.scriptingMs);
            ImGui::NextColumn();
            ImGui::Text("Rendering: %.2f ms", sub.renderingMs);
            ImGui::NextColumn();
            ImGui::Text("Particles: %.2f ms", sub.particlesMs);
            ImGui::NextColumn();
            ImGui::Text("Other: %.2f ms", sub.otherMs);
            ImGui::Columns(1);
        }
    }

    void PerformanceProfilerPanel::RenderCPUGPUSplit()
    {
        ImGui::Text(ICON_FA_BOLT " CPU / GPU Split");

        size_t sampleCount = m_frameHistorySamples;
        if (sampleCount == 0)
        {
            ImGui::TextDisabled("No data collected yet.");
            return;
        }

        // Use latest frame data
        size_t latestIdx = (m_frameHistoryIndex + MAX_FRAME_HISTORY - 1) % MAX_FRAME_HISTORY;
        const auto& latest = m_frameHistory[latestIdx];

        float total = latest.cpuMs + latest.gpuMs;
        if (total < 0.001f)
        {
            ImGui::TextDisabled("No CPU/GPU timing data.");
            return;
        }

        float cpuFraction = latest.cpuMs / total;
        float gpuFraction = latest.gpuMs / total;

        // Draw horizontal stacked bar
        float barHeight = 24.0f;
        float barWidth = ImGui::GetContentRegionAvail().x;
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        float cpuWidth = barWidth * cpuFraction;
        float gpuWidth = barWidth * gpuFraction;

        // CPU bar (blue)
        drawList->AddRectFilled(barPos, ImVec2(barPos.x + cpuWidth, barPos.y + barHeight), IM_COL32(52, 152, 219, 220));
        // GPU bar (green)
        drawList->AddRectFilled(ImVec2(barPos.x + cpuWidth, barPos.y),
                                ImVec2(barPos.x + cpuWidth + gpuWidth, barPos.y + barHeight),
                                IM_COL32(46, 204, 113, 220));

        // Labels on bars
        if (cpuWidth > 60.0f)
        {
            std::ostringstream cpuLabel;
            cpuLabel << "CPU " << std::fixed << std::setprecision(1) << latest.cpuMs << "ms";
            drawList->AddText(ImVec2(barPos.x + 4.0f, barPos.y + 4.0f), IM_COL32(255, 255, 255, 255),
                              cpuLabel.str().c_str());
        }
        if (gpuWidth > 60.0f)
        {
            std::ostringstream gpuLabel;
            gpuLabel << "GPU " << std::fixed << std::setprecision(1) << latest.gpuMs << "ms";
            drawList->AddText(ImVec2(barPos.x + cpuWidth + 4.0f, barPos.y + 4.0f), IM_COL32(255, 255, 255, 255),
                              gpuLabel.str().c_str());
        }

        drawList->AddRect(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight), IM_COL32(80, 80, 80, 255));

        ImGui::Dummy(ImVec2(barWidth, barHeight));

        // Bound indicator
        const char* boundLabel = (latest.cpuMs > latest.gpuMs) ? "CPU Bound" : "GPU Bound";
        ImU32 boundColor = (latest.cpuMs > latest.gpuMs) ? IM_COL32(52, 152, 219, 255) : IM_COL32(46, 204, 113, 255);
        ImGui::PushStyleColor(ImGuiCol_Text, boundColor);
        ImGui::Text("%s (%.1f%% / %.1f%%)", boundLabel, cpuFraction * 100.0f, gpuFraction * 100.0f);
        ImGui::PopStyleColor();
    }

    void PerformanceProfilerPanel::RenderMemorySection()
    {
        ImGui::Text(ICON_FA_SERVER " Memory Usage");

        size_t sampleCount = m_frameHistorySamples;
        if (sampleCount == 0)
        {
            ImGui::TextDisabled("No data collected yet.");
            return;
        }

        size_t latestIdx = (m_frameHistoryIndex + MAX_FRAME_HISTORY - 1) % MAX_FRAME_HISTORY;
        const auto& latest = m_frameHistory[latestIdx];

        float memoryMB = static_cast<float>(latest.memoryUsageBytes) / (1024.0f * 1024.0f);
        float limitMB = m_thresholds.memoryLimitMB;

        // Progress bar style display
        float fraction = std::clamp(memoryMB / limitMB, 0.0f, 1.0f);

        ImU32 barColor;
        if (memoryMB < m_thresholds.memoryWarningMB)
        {
            barColor = COLOR_GREEN;
        }
        else if (memoryMB < m_thresholds.memoryLimitMB)
        {
            barColor = COLOR_YELLOW;
        }
        else
        {
            barColor = COLOR_RED;
        }

        float barHeight = 20.0f;
        float barWidth = ImGui::GetContentRegionAvail().x;
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight), COLOR_GRAPH_BG);
        // Filled portion
        drawList->AddRectFilled(barPos, ImVec2(barPos.x + barWidth * fraction, barPos.y + barHeight), barColor);
        // Warning threshold marker
        float warningX = barPos.x + barWidth * (m_thresholds.memoryWarningMB / limitMB);
        drawList->AddLine(ImVec2(warningX, barPos.y), ImVec2(warningX, barPos.y + barHeight), COLOR_YELLOW, 2.0f);
        // Border
        drawList->AddRect(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight), IM_COL32(80, 80, 80, 255));

        // Label
        std::ostringstream memLabel;
        memLabel << FormatBytes(latest.memoryUsageBytes) << " / " << std::fixed << std::setprecision(0) << limitMB
                 << " MB";
        drawList->AddText(ImVec2(barPos.x + 4.0f, barPos.y + 3.0f), IM_COL32(255, 255, 255, 255),
                          memLabel.str().c_str());

        ImGui::Dummy(ImVec2(barWidth, barHeight));

        // Memory timeline graph
        const float graphHeight = 80.0f;
        ImVec2 graphPos = ImGui::GetCursorScreenPos();
        ImVec2 graphSize = ImVec2(ImGui::GetContentRegionAvail().x, graphHeight);

        drawList->AddRectFilled(graphPos, ImVec2(graphPos.x + graphSize.x, graphPos.y + graphSize.y), COLOR_GRAPH_BG);

        // Find max memory for scaling
        size_t maxMem = 0;
        for (size_t i = 0; i < sampleCount; ++i)
        {
            maxMem = std::max(maxMem, m_frameHistory[i].memoryUsageBytes);
        }
        float maxMemMB = std::max(static_cast<float>(maxMem) / (1024.0f * 1024.0f), limitMB);

        float pointWidth = graphSize.x / static_cast<float>(MAX_FRAME_HISTORY);

        for (size_t i = 1; i < sampleCount; ++i)
        {
            size_t prevBufIdx;
            size_t curBufIdx;
            if (sampleCount < MAX_FRAME_HISTORY)
            {
                prevBufIdx = i - 1;
                curBufIdx = i;
            }
            else
            {
                prevBufIdx = (m_frameHistoryIndex + i - 1) % MAX_FRAME_HISTORY;
                curBufIdx = (m_frameHistoryIndex + i) % MAX_FRAME_HISTORY;
            }

            float prevMB = static_cast<float>(m_frameHistory[prevBufIdx].memoryUsageBytes) / (1024.0f * 1024.0f);
            float curMB = static_cast<float>(m_frameHistory[curBufIdx].memoryUsageBytes) / (1024.0f * 1024.0f);

            float prevFrac = std::clamp(prevMB / maxMemMB, 0.0f, 1.0f);
            float curFrac = std::clamp(curMB / maxMemMB, 0.0f, 1.0f);

            float x0 = graphPos.x + static_cast<float>(i - 1) * pointWidth;
            float y0 = graphPos.y + graphSize.y * (1.0f - prevFrac);
            float x1 = graphPos.x + static_cast<float>(i) * pointWidth;
            float y1 = graphPos.y + graphSize.y * (1.0f - curFrac);

            ImU32 color = (curMB > m_thresholds.memoryWarningMB) ? COLOR_YELLOW : COLOR_GREEN;
            if (curMB > m_thresholds.memoryLimitMB)
            {
                color = COLOR_RED;
            }
            drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), color, 1.5f);
        }

        drawList->AddRect(graphPos, ImVec2(graphPos.x + graphSize.x, graphPos.y + graphSize.y),
                          IM_COL32(80, 80, 80, 255));
        ImGui::Dummy(graphSize);
    }

    void PerformanceProfilerPanel::RenderDrawCallSection()
    {
        ImGui::Text(ICON_FA_CUBE " Draw Calls & Triangles");

        size_t sampleCount = m_frameHistorySamples;
        if (sampleCount == 0)
        {
            ImGui::TextDisabled("No data collected yet.");
            return;
        }

        size_t latestIdx = (m_frameHistoryIndex + MAX_FRAME_HISTORY - 1) % MAX_FRAME_HISTORY;
        const auto& latest = m_frameHistory[latestIdx];

        // Draw calls bar
        float dcFraction = std::clamp(
            static_cast<float>(latest.drawCalls) / static_cast<float>(m_thresholds.drawCallLimit), 0.0f, 1.0f);
        ImU32 dcColor;
        if (latest.drawCalls < m_thresholds.drawCallWarning)
        {
            dcColor = COLOR_GREEN;
        }
        else if (latest.drawCalls < m_thresholds.drawCallLimit)
        {
            dcColor = COLOR_YELLOW;
        }
        else
        {
            dcColor = COLOR_RED;
        }

        ImGui::Text("Draw Calls: %d / %d", latest.drawCalls, m_thresholds.drawCallLimit);

        float barHeight = 14.0f;
        float barWidth = ImGui::GetContentRegionAvail().x;
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight), COLOR_GRAPH_BG);
        drawList->AddRectFilled(barPos, ImVec2(barPos.x + barWidth * dcFraction, barPos.y + barHeight), dcColor);

        // Warning threshold marker
        float warnX = barPos.x + barWidth * (static_cast<float>(m_thresholds.drawCallWarning) /
                                             static_cast<float>(m_thresholds.drawCallLimit));
        drawList->AddLine(ImVec2(warnX, barPos.y), ImVec2(warnX, barPos.y + barHeight), COLOR_YELLOW, 1.5f);

        drawList->AddRect(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight), IM_COL32(80, 80, 80, 255));
        ImGui::Dummy(ImVec2(barWidth, barHeight));

        // Triangle count
        ImGui::Text("Triangles: %s",
                    latest.triangleCount > 1000000 ? (std::to_string(latest.triangleCount / 1000000) + "." +
                                                      std::to_string((latest.triangleCount / 100000) % 10) + "M")
                                                         .c_str()
                    : latest.triangleCount > 1000 ? (std::to_string(latest.triangleCount / 1000) + "." +
                                                     std::to_string((latest.triangleCount / 100) % 10) + "K")
                                                        .c_str()
                                                  : std::to_string(latest.triangleCount).c_str());
    }

    void PerformanceProfilerPanel::RenderStatisticsSummary()
    {
        ImGui::Text(ICON_FA_INFO_CIRCLE " Statistics");

        if (m_totalFrames == 0)
        {
            ImGui::TextDisabled("No data collected yet.");
            return;
        }

        ImGui::Columns(2, "stats_cols", false);

        // Frame time statistics
        ImGui::Text("Avg Frame Time:");
        ImGui::NextColumn();
        ImGui::Text("%.2f ms (%.0f FPS)", m_avgFrameTimeMs,
                    m_avgFrameTimeMs > 0.001f ? 1000.0f / m_avgFrameTimeMs : 0.0f);
        ImGui::NextColumn();

        ImGui::Text("Min Frame Time:");
        ImGui::NextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, COLOR_GREEN);
        ImGui::Text("%.2f ms (%.0f FPS)", m_minFrameTimeMs,
                    m_minFrameTimeMs > 0.001f ? 1000.0f / m_minFrameTimeMs : 0.0f);
        ImGui::PopStyleColor();
        ImGui::NextColumn();

        ImGui::Text("Max Frame Time:");
        ImGui::NextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, GetFrameTimeColor(m_maxFrameTimeMs, m_thresholds));
        ImGui::Text("%.2f ms (%.0f FPS)", m_maxFrameTimeMs,
                    m_maxFrameTimeMs > 0.001f ? 1000.0f / m_maxFrameTimeMs : 0.0f);
        ImGui::PopStyleColor();
        ImGui::NextColumn();

        ImGui::Text("1%% Low:");
        ImGui::NextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, GetFrameTimeColor(m_onePercentLowMs, m_thresholds));
        ImGui::Text("%.2f ms (%.0f FPS)", m_onePercentLowMs,
                    m_onePercentLowMs > 0.001f ? 1000.0f / m_onePercentLowMs : 0.0f);
        ImGui::PopStyleColor();
        ImGui::NextColumn();

        ImGui::Text("Total Frames:");
        ImGui::NextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(m_totalFrames));
        ImGui::NextColumn();

        ImGui::Text("Buffer Usage:");
        ImGui::NextColumn();
        ImGui::Text("%zu / %zu", m_frameHistorySamples, MAX_FRAME_HISTORY);
        ImGui::NextColumn();

        ImGui::Columns(1);
    }

    void PerformanceProfilerPanel::RenderAlerts()
    {
        ImGui::Text(ICON_FA_EXCLAMATION " Alerts (%zu)", m_alerts.size());

        if (m_alerts.empty())
        {
            ImGui::TextDisabled("No alerts.");
            return;
        }

        // Scrollable alert list
        float alertHeight = std::min(150.0f, static_cast<float>(m_alerts.size()) * 22.0f + 4.0f);
        ImGui::BeginChild("AlertList", ImVec2(0, alertHeight), true);

        // Show newest alerts first
        for (auto it = m_alerts.rbegin(); it != m_alerts.rend(); ++it)
        {
            const auto& alert = *it;
            ImU32 sevColor = GetSeverityColor(alert.severity);
            const char* sevLabel = GetSeverityLabel(alert.severity);

            ImGui::PushStyleColor(ImGuiCol_Text, sevColor);
            ImGui::Text("[%s]", sevLabel);
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::TextWrapped("%s", alert.message.c_str());
        }

        ImGui::EndChild();

        if (ImGui::Button("Clear Alerts"))
        {
            m_alerts.clear();
        }
    }

    void PerformanceProfilerPanel::RenderCaptureControls()
    {
        ImGui::Text(ICON_FA_FILM " Capture");
        ImGui::SameLine();

        if (m_isCapturing)
        {
            // Show pulsing capture indicator
            float pulse = (std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f) + 1.0f) * 0.5f;
            ImU32 recordColor = IM_COL32(255, static_cast<int>(60 * pulse), static_cast<int>(60 * pulse), 255);
            ImGui::PushStyleColor(ImGuiCol_Text, recordColor);
            ImGui::Text(ICON_FA_CIRCLE " REC");
            ImGui::PopStyleColor();
            ImGui::SameLine();

            auto elapsed = std::chrono::steady_clock::now() - m_captureStartTime;
            auto elapsedSec = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0f;
            ImGui::Text("%.1fs | %zu frames", elapsedSec, m_capturedFrames.size());
            ImGui::SameLine();

            if (ImGui::Button(ICON_FA_STOP " Stop"))
            {
                StopCapture();
            }
        }
        else
        {
            if (ImGui::Button(ICON_FA_PLAY " Start Capture"))
            {
                StartCapture();
            }

            if (!m_capturedFrames.empty())
            {
                ImGui::SameLine();
                ImGui::Text("Last capture: %zu frames", m_capturedFrames.size());
                ImGui::SameLine();

                if (ImGui::Button(ICON_FA_DOWNLOAD " Export CSV"))
                {
                    // Build CSV to console for now (file dialog would need platform layer)
                    std::ostringstream csv;
                    csv << "Frame,TotalMs,CpuMs,GpuMs,PhysicsMs,AiMs,AnimationMs,AudioMs,"
                        << "ScriptingMs,RenderingMs,ParticlesMs,OtherMs,MemoryBytes,DrawCalls,Triangles\n";

                    for (const auto& frame : m_capturedFrames)
                    {
                        csv << frame.frameNumber << "," << std::fixed << std::setprecision(3) << frame.totalFrameMs
                            << "," << frame.cpuMs << "," << frame.gpuMs << "," << frame.subsystems.physicsMs << ","
                            << frame.subsystems.aiMs << "," << frame.subsystems.animationMs << ","
                            << frame.subsystems.audioMs << "," << frame.subsystems.scriptingMs << ","
                            << frame.subsystems.renderingMs << "," << frame.subsystems.particlesMs << ","
                            << frame.subsystems.otherMs << "," << frame.memoryUsageBytes << "," << frame.drawCalls
                            << "," << frame.triangleCount << "\n";
                    }

                    ImGui::SetClipboardText(csv.str().c_str());
                    std::cout << "[Profiler] Capture CSV copied to clipboard (" << m_capturedFrames.size()
                              << " frames)\n";
                }

                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_TRASH " Discard"))
                {
                    m_capturedFrames.clear();
                }
            }
        }
    }

} // namespace SparkEditor
