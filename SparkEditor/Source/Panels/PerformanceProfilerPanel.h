/**
 * @file PerformanceProfilerPanel.h
 * @brief Runtime performance profiler with frame timeline and subsystem breakdown
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides real-time performance analysis during play mode: per-frame timing,
 * subsystem cost breakdown, GPU/CPU split, memory tracking, and configurable
 * alerts for performance regressions. Includes a flame-graph-style timeline
 * for identifying frame spikes.
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Editor
{
    class PlayModeManager;
}

namespace SparkEditor
{

    // ========================================================================
    // Profiler Data Structures
    // ========================================================================

    /// @brief Timing sample for a single subsystem in a single frame.
    struct SubsystemTimingSample
    {
        float physicsMs = 0.0f;
        float aiMs = 0.0f;
        float animationMs = 0.0f;
        float audioMs = 0.0f;
        float scriptingMs = 0.0f;
        float renderingMs = 0.0f;
        float particlesMs = 0.0f;
        float otherMs = 0.0f;

        float TotalMs() const
        {
            return physicsMs + aiMs + animationMs + audioMs + scriptingMs + renderingMs + particlesMs + otherMs;
        }
    };

    /// @brief Complete frame timing record.
    struct FrameTimingRecord
    {
        uint64_t frameNumber = 0;
        float totalFrameMs = 0.0f;
        float cpuMs = 0.0f;
        float gpuMs = 0.0f;
        SubsystemTimingSample subsystems;
        size_t memoryUsageBytes = 0;
        int drawCalls = 0;
        int triangleCount = 0;
    };

    /// @brief Performance alert when a threshold is exceeded.
    struct PerformanceAlert
    {
        enum class Severity
        {
            Info,
            Warning,
            Critical
        };

        std::string message;
        Severity severity = Severity::Warning;
        float timestamp = 0.0f;
        float value = 0.0f;
        float threshold = 0.0f;
    };

    /// @brief Configurable thresholds for performance alerts.
    struct PerformanceThresholds
    {
        float targetFrameTimeMs = 16.67f;  ///< 60 FPS target
        float warningFrameTimeMs = 20.0f;  ///< Yellow warning
        float criticalFrameTimeMs = 33.3f; ///< Red critical (below 30 FPS)
        float memoryWarningMB = 512.0f;
        float memoryLimitMB = 1024.0f;
        int drawCallWarning = 2000;
        int drawCallLimit = 5000;
        bool enableAlerts = true;
    };

    // ========================================================================
    // Performance Profiler Panel
    // ========================================================================

    /**
     * @brief Real-time performance profiler panel
     *
     * Features:
     * - Frame time graph with target FPS line
     * - Per-subsystem cost breakdown (stacked bar chart)
     * - CPU vs GPU time split
     * - Memory usage tracking
     * - Draw call and triangle count monitoring
     * - Configurable performance alerts
     * - Frame spike detection and highlighting
     * - Min/max/average statistics
     * - Capture and export profiling sessions
     */
    class PerformanceProfilerPanel : public EditorPanel
    {
      public:
        PerformanceProfilerPanel();
        ~PerformanceProfilerPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /// @brief Connect to the play mode manager for timing data.
        void SetPlayModeManager(Spark::Editor::PlayModeManager* manager) { m_playModeManager = manager; }

        /// @brief Push a frame timing record from the engine.
        void RecordFrame(const FrameTimingRecord& record);

        /// @brief Set performance thresholds.
        void SetThresholds(const PerformanceThresholds& thresholds) { m_thresholds = thresholds; }
        const PerformanceThresholds& GetThresholds() const { return m_thresholds; }

        /// @brief Start/stop a profiling capture session.
        void StartCapture();
        void StopCapture();
        bool IsCapturing() const { return m_isCapturing; }

        /// @brief Clear all collected data.
        void ClearData();

      private:
        void RenderToolbar();
        void RenderFrameTimeGraph();
        void RenderSubsystemBreakdown();
        void RenderCPUGPUSplit();
        void RenderMemorySection();
        void RenderDrawCallSection();
        void RenderStatisticsSummary();
        void RenderAlerts();
        void RenderCaptureControls();
        void CheckThresholds(const FrameTimingRecord& record);

        Spark::Editor::PlayModeManager* m_playModeManager = nullptr;

        // Frame history ring buffer
        static constexpr size_t MAX_FRAME_HISTORY = 300;
        std::array<FrameTimingRecord, MAX_FRAME_HISTORY> m_frameHistory{};
        size_t m_frameHistoryIndex = 0;
        size_t m_frameHistorySamples = 0;

        // Running statistics
        float m_avgFrameTimeMs = 0.0f;
        float m_minFrameTimeMs = 999.0f;
        float m_maxFrameTimeMs = 0.0f;
        float m_onePercentLowMs = 0.0f;
        uint64_t m_totalFrames = 0;

        // Thresholds
        PerformanceThresholds m_thresholds;

        // Alerts
        std::vector<PerformanceAlert> m_alerts;
        static constexpr size_t MAX_ALERTS = 100;

        // Capture
        bool m_isCapturing = false;
        std::vector<FrameTimingRecord> m_capturedFrames;
        std::chrono::steady_clock::time_point m_captureStartTime;

        // UI state
        enum class GraphMode
        {
            FrameTime,
            FPS,
            SubsystemBreakdown,
            Memory
        };
        GraphMode m_graphMode = GraphMode::FrameTime;
        float m_graphTimeRange = 5.0f; ///< Seconds of history to show
        bool m_showGrid = true;
        bool m_showTargetLine = true;
        bool m_pauseGraph = false;
        bool m_showAlertPanel = true;
        int m_selectedFrame = -1; ///< Frame selected for detailed view (-1 = none)
    };

} // namespace SparkEditor
