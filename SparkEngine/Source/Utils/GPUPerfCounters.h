/**
 * @file GPUPerfCounters.h
 * @brief Categorized GPU performance counter tracking
 *
 * Tracks categorized GPU counters
 * (draw calls, state changes, texture uploads, barriers, render passes)
 * with frame-based aggregation and delta monitoring.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Spark
{
    namespace Graphics
    {

        /**
         * @brief GPU performance counter categories.
         */
        enum class GPUCounterCategory : uint8_t
        {
            DrawCalls,
            Primitives,
            StateChanges,
            TextureUploads,
            BufferUploads,
            RenderPasses,
            Barriers,
            ShaderSwitches,
            RenderTargetSwitches,
            ComputeDispatches,
            Count
        };

        /**
         * @brief Per-frame GPU performance counter state.
         */
        struct GPUFrameCounters
        {
            std::array<uint64_t, static_cast<size_t>(GPUCounterCategory::Count)> values = {};

            uint64_t& operator[](GPUCounterCategory cat) { return values[static_cast<size_t>(cat)]; }
            uint64_t operator[](GPUCounterCategory cat) const { return values[static_cast<size_t>(cat)]; }

            void Reset() { values.fill(0); }

            GPUFrameCounters Delta(const GPUFrameCounters& previous) const
            {
                GPUFrameCounters delta;
                for (size_t i = 0; i < values.size(); ++i)
                    delta.values[i] = values[i] - previous.values[i];
                return delta;
            }
        };

        /**
         * @brief Timestamped GPU render pass sample for the current frame.
         */
        struct GPUPassTimestamp
        {
            std::string passName;
            uint64_t beginTicks = 0;
            uint64_t endTicks = 0;
            uint64_t frequencyHz = 0;
            float durationMs = 0.0f;
            uint32_t depth = 0;
        };

        /**
         * @brief Snapshot of hardware pipeline statistics for a frame.
         */
        struct GPUPipelineStatsCounters
        {
            uint64_t iaVertices = 0;
            uint64_t iaPrimitives = 0;
            uint64_t vsInvocations = 0;
            uint64_t psInvocations = 0;
            uint64_t csInvocations = 0;
            uint64_t clipInvocations = 0;
            uint64_t clipPrimitives = 0;
        };

        /**
         * @brief Tracks and aggregates GPU performance counters per frame.
         */
        class GPUPerfCounters
        {
          public:
            static GPUPerfCounters& GetInstance()
            {
                static GPUPerfCounters instance;
                return instance;
            }

            void Initialize() { Reset(); }
            void Shutdown() { Reset(); }

            /**
             * @brief Increment a counter by the specified amount.
             */
            void Increment(GPUCounterCategory category, uint64_t amount = 1) { m_current[category] += amount; }

            void RecordPassTimestamp(const std::string& passName, uint64_t beginTicks, uint64_t endTicks,
                                     uint64_t frequencyHz, uint32_t depth = 0)
            {
                GPUPassTimestamp ts;
                ts.passName = passName;
                ts.beginTicks = beginTicks;
                ts.endTicks = endTicks;
                ts.frequencyHz = frequencyHz;
                ts.depth = depth;
                if (endTicks > beginTicks && frequencyHz > 0)
                {
                    ts.durationMs =
                        static_cast<float>(endTicks - beginTicks) / static_cast<float>(frequencyHz) * 1000.0f;
                }
                m_currentPassTimestamps.push_back(std::move(ts));
            }

            void SetPipelineStats(const GPUPipelineStatsCounters& stats) { m_currentPipelineStats = stats; }

            /**
             * @brief Call at the end of each frame to snapshot counters.
             */
            void EndFrame()
            {
                m_lastFrame = m_current;
                m_lastPassTimestamps = m_currentPassTimestamps;
                m_lastPipelineStats = m_currentPipelineStats;

                // Update rolling averages
                for (size_t i = 0; i < m_rollingSum.size(); ++i)
                {
                    m_rollingSum[i] -= m_history[m_historyIndex][static_cast<GPUCounterCategory>(i)];
                    m_rollingSum[i] += m_current[static_cast<GPUCounterCategory>(i)];
                }
                m_history[m_historyIndex] = m_current;
                m_historyIndex = (m_historyIndex + 1) % HISTORY_SIZE;

                m_current.Reset();
                m_currentPassTimestamps.clear();
                m_currentPipelineStats = {};
                m_frameCount++;
            }

            void Reset()
            {
                m_current.Reset();
                m_lastFrame.Reset();
                m_currentPassTimestamps.clear();
                m_lastPassTimestamps.clear();
                m_currentPipelineStats = {};
                m_lastPipelineStats = {};
                for (auto& h : m_history)
                    h.Reset();
                m_rollingSum.fill(0);
                m_historyIndex = 0;
                m_frameCount = 0;
            }

            const GPUFrameCounters& GetLastFrame() const { return m_lastFrame; }
            const GPUFrameCounters& GetCurrentFrame() const { return m_current; }
            const std::vector<GPUPassTimestamp>& GetLastPassTimestamps() const { return m_lastPassTimestamps; }
            const GPUPipelineStatsCounters& GetLastPipelineStats() const { return m_lastPipelineStats; }

            uint64_t GetAverage(GPUCounterCategory category) const
            {
                uint32_t frames = std::min(m_frameCount, HISTORY_SIZE);
                return frames > 0 ? m_rollingSum[static_cast<size_t>(category)] / frames : 0;
            }

            static const char* GetCategoryName(GPUCounterCategory cat)
            {
                switch (cat)
                {
                case GPUCounterCategory::DrawCalls:
                    return "Draw Calls";
                case GPUCounterCategory::Primitives:
                    return "Primitives";
                case GPUCounterCategory::StateChanges:
                    return "State Changes";
                case GPUCounterCategory::TextureUploads:
                    return "Texture Uploads";
                case GPUCounterCategory::BufferUploads:
                    return "Buffer Uploads";
                case GPUCounterCategory::RenderPasses:
                    return "Render Passes";
                case GPUCounterCategory::Barriers:
                    return "Barriers";
                case GPUCounterCategory::ShaderSwitches:
                    return "Shader Switches";
                case GPUCounterCategory::RenderTargetSwitches:
                    return "RT Switches";
                case GPUCounterCategory::ComputeDispatches:
                    return "Compute Dispatches";
                default:
                    return "Unknown";
                }
            }

            std::string Console_GetReport() const
            {
                std::string report = "GPU Performance Counters (last frame):\n";
                for (size_t i = 0; i < static_cast<size_t>(GPUCounterCategory::Count); ++i)
                {
                    auto cat = static_cast<GPUCounterCategory>(i);
                    report += "  " + std::string(GetCategoryName(cat)) + ": " + std::to_string(m_lastFrame[cat]) +
                              " (avg: " + std::to_string(GetAverage(cat)) + ")\n";
                }
                report += "Pipeline Stats: IA Vtx=" + std::to_string(m_lastPipelineStats.iaVertices) +
                          ", IA Prim=" + std::to_string(m_lastPipelineStats.iaPrimitives) +
                          ", VS=" + std::to_string(m_lastPipelineStats.vsInvocations) +
                          ", PS=" + std::to_string(m_lastPipelineStats.psInvocations) + "\n";
                if (!m_lastPassTimestamps.empty())
                {
                    report += "Pass Timeline:\n";
                    for (const auto& pass : m_lastPassTimestamps)
                    {
                        report += "  [" + std::to_string(pass.depth) + "] " + pass.passName + ": " +
                                  std::to_string(pass.durationMs) + " ms\n";
                    }
                }
                return report;
            }

          private:
            GPUPerfCounters() = default;

            static constexpr uint32_t HISTORY_SIZE = 120; // 2 seconds at 60fps

            GPUFrameCounters m_current;
            GPUFrameCounters m_lastFrame;
            std::vector<GPUPassTimestamp> m_currentPassTimestamps;
            std::vector<GPUPassTimestamp> m_lastPassTimestamps;
            GPUPipelineStatsCounters m_currentPipelineStats;
            GPUPipelineStatsCounters m_lastPipelineStats;
            std::array<GPUFrameCounters, HISTORY_SIZE> m_history;
            std::array<uint64_t, static_cast<size_t>(GPUCounterCategory::Count)> m_rollingSum = {};
            uint32_t m_historyIndex = 0;
            uint32_t m_frameCount = 0;
        };

    } // namespace Graphics
} // namespace Spark
