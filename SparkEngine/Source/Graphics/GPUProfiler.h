/**
 * @file GPUProfiler.h
 * @brief GPU timestamp queries and pipeline statistics profiling
 * @author Spark Engine Team
 * @date 2026
 *
 * Captures GPU timing data via timestamp queries, tracks pipeline statistics
 * (vertex/pixel shader invocations, draw calls), and exports profiling data
 * in Chrome tracing JSON format for analysis.
 *
 * ## Usage
 * @code
 *   auto& profiler = Spark::Graphics::GPUProfiler::GetInstance();
 *   profiler.Initialize();
 *   // In render loop:
 *   profiler.BeginFrame();
 *   profiler.BeginEvent("ShadowPass");
 *   // ... render shadows ...
 *   profiler.EndEvent();
 *   profiler.EndFrame();
 *   float gpuMs = profiler.GetLastFrame().totalGPUTimeMs;
 * @endcode
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    /** @brief Single GPU timing event with nesting depth. */
    struct GPUTimestamp
    {
        std::string name;
        float durationMs = 0.0f;
        uint32_t depth = 0;
    };

    /** @brief Pipeline statistics for a frame. */
    struct GPUPipelineStats
    {
        uint64_t inputAssemblyVertices = 0;
        uint64_t inputAssemblyPrimitives = 0;
        uint64_t vertexShaderInvocations = 0;
        uint64_t pixelShaderInvocations = 0;
        uint64_t computeShaderInvocations = 0;
        uint64_t clippingPrimitives = 0;
    };

    /** @brief Complete profiling data for a single frame. */
    struct GPUProfileFrame
    {
        float totalGPUTimeMs = 0.0f;
        std::vector<GPUTimestamp> timestamps;
        GPUPipelineStats pipelineStats;
        uint32_t frameNumber = 0;
    };

    /**
     * @brief GPU profiler with timestamp queries and pipeline stats
     *
     * Provides per-frame GPU timing via D3D11/Vulkan timestamp queries,
     * pipeline statistics, frame history, and export to Chrome tracing JSON.
     */
    class GPUProfiler
    {
      public:
        static GPUProfiler& GetInstance();

        /** @brief Initialize profiler and create query resources. */
        void Initialize();

        /** @brief Release all query resources. */
        void Shutdown();

        /** @brief Start profiling a new frame. */
        void BeginFrame();

        /** @brief End profiling the current frame and collect results. */
        void EndFrame();

        /**
         * @brief Begin a named GPU timing event (nestable)
         * @param name Event name for display
         */
        void BeginEvent(const std::string& name);

        /** @brief End the most recent GPU timing event. */
        void EndEvent();

        /** @brief Get the most recently completed frame's profiling data. */
        const GPUProfileFrame& GetLastFrame() const;

        /**
         * @brief Get recent frame history
         * @param count Number of frames to retrieve
         * @return Vector of frame data (most recent last)
         */
        std::vector<GPUProfileFrame> GetFrameHistory(uint32_t count) const;

        /**
         * @brief Get average GPU time across recent frames
         * @param frameCount Number of frames to average
         * @return Average GPU time in milliseconds
         */
        float GetAverageGPUTime(uint32_t frameCount = 60) const;

        /** @brief Enable or disable profiling (disabled = zero overhead). */
        void SetEnabled(bool enabled);

        /** @brief Check if profiling is enabled. */
        bool IsEnabled() const { return m_enabled; }

        /**
         * @brief Export frame history to Chrome tracing JSON format
         * @return JSON string compatible with chrome://tracing
         */
        std::string ExportToJSON() const;

        /**
         * @brief Export frame history to CSV file
         * @param path Output file path
         */
        void ExportToCSV(const std::string& path) const;

        /** @brief Console status string. */
        std::string Console_GetStatus() const;

      private:
        GPUProfiler() = default;

        static constexpr uint32_t MAX_HISTORY = 120;
        static constexpr uint32_t MAX_EVENTS_PER_FRAME = 64;

        std::array<GPUProfileFrame, MAX_HISTORY> m_frameHistory{};
        uint32_t m_frameIndex = 0;
        uint32_t m_globalFrameCounter = 0;

        struct PendingEvent
        {
            std::string name;
            uint32_t depth;
            float startTimeMs;
        };

        std::vector<PendingEvent> m_eventStack;
        std::vector<GPUTimestamp> m_currentFrameTimestamps;
        float m_frameStartTime = 0.0f;
        uint32_t m_currentDepth = 0;
        bool m_enabled = false;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
