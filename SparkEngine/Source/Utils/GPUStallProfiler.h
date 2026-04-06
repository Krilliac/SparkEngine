/**
 * @file GPUStallProfiler.h
 * @brief CPU-GPU timeline analysis and bottleneck detection
 *
 * Tracks CPU submission and GPU execution timelines to classify each frame
 * as CPU-bound, GPU-bound, balanced, or stalled (pipeline bubble). Inspired
 * by RPCS3's GPU pipeline stall tracking and correlation system.
 *
 * @see Profiler.h, GPUPerfCounters.h, HitchDetector.h
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace Spark
{

    /// @brief Frame bottleneck classification.
    enum class FrameBottleneck : uint8_t
    {
        Unknown,  ///< Not enough data yet
        CPUBound, ///< GPU idle, waiting for CPU to submit work
        GPUBound, ///< CPU idle, waiting for GPU to finish
        Balanced, ///< Both busy with minimal gaps
        Bubble,   ///< Neither busy (I/O wait, OS scheduler, etc.)
    };

    /// @brief Per-frame CPU-GPU timeline data.
    struct FrameTimeline
    {
        double cpuFrameMs = 0.0; ///< CPU frame time (submission start to end)
        double gpuFrameMs = 0.0; ///< GPU frame time (execution start to end)
        double presentMs = 0.0;  ///< Time spent in Present/fence wait
        double cpuIdleMs = 0.0;  ///< CPU idle time (waiting for GPU)
        double gpuIdleMs = 0.0;  ///< GPU idle time (waiting for CPU submission)
        FrameBottleneck bottleneck = FrameBottleneck::Unknown;
    };

    /**
     * @brief Analyzes CPU-GPU overlap to identify frame bottlenecks.
     *
     * Call BeginCPUWork()/EndCPUWork() around the main-thread frame logic,
     * and feed GPU timing from the Profiler's GPU timer queries.
     */
    class GPUStallProfiler
    {
      public:
        static GPUStallProfiler& GetInstance()
        {
            static GPUStallProfiler instance;
            return instance;
        }

        void Initialize()
        {
            m_historyIndex = 0;
            m_frameCount = 0;
        }

        void Shutdown() { m_frameCount = 0; }

        /// @brief Call at the start of CPU frame work.
        void BeginCPUWork() { m_cpuStart = std::chrono::high_resolution_clock::now(); }

        /// @brief Call at the end of CPU frame work (before Present).
        void EndCPUWork() { m_cpuEnd = std::chrono::high_resolution_clock::now(); }

        /// @brief Call after Present completes to record the present stall.
        void RecordPresentTime(double presentMs) { m_currentPresentMs = presentMs; }

        /// @brief Feed GPU frame time from Profiler GPU queries.
        void RecordGPUFrameTime(double gpuMs) { m_currentGpuMs = gpuMs; }

        /**
         * @brief Finalize the frame analysis. Call at end of frame after all timings recorded.
         */
        void EndFrame()
        {
            FrameTimeline timeline;

            auto cpuDuration = std::chrono::duration<double, std::milli>(m_cpuEnd - m_cpuStart);
            timeline.cpuFrameMs = cpuDuration.count();
            timeline.gpuFrameMs = m_currentGpuMs;
            timeline.presentMs = m_currentPresentMs;

            // Classify bottleneck
            double totalFrameMs = std::max(timeline.cpuFrameMs, timeline.gpuFrameMs);
            if (totalFrameMs < 0.01)
            {
                timeline.bottleneck = FrameBottleneck::Unknown;
            }
            else
            {
                double cpuUtil = timeline.cpuFrameMs / totalFrameMs;
                double gpuUtil = timeline.gpuFrameMs / totalFrameMs;

                // GPU idle = frame time - GPU busy time (CPU was slower)
                timeline.gpuIdleMs = std::max(0.0, timeline.cpuFrameMs - timeline.gpuFrameMs);
                // CPU idle = present wait time (GPU was slower)
                timeline.cpuIdleMs = timeline.presentMs;

                constexpr double kBoundThreshold = 0.7;
                constexpr double kBalancedLow = 0.4;

                if (cpuUtil > kBoundThreshold && gpuUtil < kBalancedLow)
                    timeline.bottleneck = FrameBottleneck::CPUBound;
                else if (gpuUtil > kBoundThreshold && cpuUtil < kBalancedLow)
                    timeline.bottleneck = FrameBottleneck::GPUBound;
                else if (cpuUtil < kBalancedLow && gpuUtil < kBalancedLow)
                    timeline.bottleneck = FrameBottleneck::Bubble;
                else
                    timeline.bottleneck = FrameBottleneck::Balanced;
            }

            m_history[m_historyIndex] = timeline;
            m_historyIndex = (m_historyIndex + 1) % HISTORY_SIZE;
            m_frameCount++;

            // Reset for next frame
            m_currentGpuMs = 0.0;
            m_currentPresentMs = 0.0;
        }

        /// @brief Get the most recent frame's timeline.
        const FrameTimeline& GetLastFrame() const
        {
            uint32_t idx = (m_historyIndex + HISTORY_SIZE - 1) % HISTORY_SIZE;
            return m_history[idx];
        }

        /// @brief Get the current bottleneck classification.
        FrameBottleneck GetCurrentBottleneck() const { return GetLastFrame().bottleneck; }

        /// @brief Get bottleneck distribution over recent history (percentage per type).
        struct BottleneckDistribution
        {
            float cpuBoundPct = 0.0f;
            float gpuBoundPct = 0.0f;
            float balancedPct = 0.0f;
            float bubblePct = 0.0f;
        };

        BottleneckDistribution GetDistribution() const
        {
            BottleneckDistribution dist;
            uint32_t frames = std::min(m_frameCount, HISTORY_SIZE);
            if (frames == 0)
                return dist;

            uint32_t cpuCount = 0;
            uint32_t gpuCount = 0;
            uint32_t balCount = 0;
            uint32_t bubCount = 0;

            for (uint32_t i = 0; i < frames; ++i)
            {
                switch (m_history[i].bottleneck)
                {
                case FrameBottleneck::CPUBound:
                    cpuCount++;
                    break;
                case FrameBottleneck::GPUBound:
                    gpuCount++;
                    break;
                case FrameBottleneck::Balanced:
                    balCount++;
                    break;
                case FrameBottleneck::Bubble:
                    bubCount++;
                    break;
                default:
                    break;
                }
            }

            float inv = 100.0f / static_cast<float>(frames);
            dist.cpuBoundPct = static_cast<float>(cpuCount) * inv;
            dist.gpuBoundPct = static_cast<float>(gpuCount) * inv;
            dist.balancedPct = static_cast<float>(balCount) * inv;
            dist.bubblePct = static_cast<float>(bubCount) * inv;
            return dist;
        }

        static const char* GetBottleneckName(FrameBottleneck b)
        {
            switch (b)
            {
            case FrameBottleneck::CPUBound:
                return "CPU-bound";
            case FrameBottleneck::GPUBound:
                return "GPU-bound";
            case FrameBottleneck::Balanced:
                return "Balanced";
            case FrameBottleneck::Bubble:
                return "Bubble";
            default:
                return "Unknown";
            }
        }

        std::string Console_GetReport() const
        {
            const auto& last = GetLastFrame();
            auto dist = GetDistribution();
            std::string report = "GPU Stall Profiler:\n";
            report += "  Current: " + std::string(GetBottleneckName(last.bottleneck)) + "\n";
            report += "  CPU: " + std::to_string(last.cpuFrameMs) + "ms";
            report += "  GPU: " + std::to_string(last.gpuFrameMs) + "ms";
            report += "  Present: " + std::to_string(last.presentMs) + "ms\n";
            report += "  Distribution (last " + std::to_string(std::min(m_frameCount, HISTORY_SIZE)) + " frames):\n";
            report += "    CPU-bound: " + std::to_string(dist.cpuBoundPct) + "%\n";
            report += "    GPU-bound: " + std::to_string(dist.gpuBoundPct) + "%\n";
            report += "    Balanced:  " + std::to_string(dist.balancedPct) + "%\n";
            report += "    Bubble:    " + std::to_string(dist.bubblePct) + "%\n";
            return report;
        }

      private:
        GPUStallProfiler() = default;

        static constexpr uint32_t HISTORY_SIZE = 300;

        std::array<FrameTimeline, HISTORY_SIZE> m_history = {};
        uint32_t m_historyIndex = 0;
        uint32_t m_frameCount = 0;

        std::chrono::high_resolution_clock::time_point m_cpuStart;
        std::chrono::high_resolution_clock::time_point m_cpuEnd;
        double m_currentGpuMs = 0.0;
        double m_currentPresentMs = 0.0;
    };

} // namespace Spark
