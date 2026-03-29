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
         * @brief Tracks and aggregates GPU performance counters per frame.
         */
        class GPUPerfCounters
        {
          public:
            [[deprecated("Use EngineContext::Get()->GetSystem<GPUPerfCounters>() instead")]]
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

            /**
             * @brief Call at the end of each frame to snapshot counters.
             */
            void EndFrame()
            {
                m_lastFrame = m_current;

                // Update rolling averages
                for (size_t i = 0; i < m_rollingSum.size(); ++i)
                {
                    m_rollingSum[i] -= m_history[m_historyIndex][static_cast<GPUCounterCategory>(i)];
                    m_rollingSum[i] += m_current[static_cast<GPUCounterCategory>(i)];
                }
                m_history[m_historyIndex] = m_current;
                m_historyIndex = (m_historyIndex + 1) % HISTORY_SIZE;

                m_current.Reset();
                m_frameCount++;
            }

            void Reset()
            {
                m_current.Reset();
                m_lastFrame.Reset();
                for (auto& h : m_history)
                    h.Reset();
                m_rollingSum.fill(0);
                m_historyIndex = 0;
                m_frameCount = 0;
            }

            const GPUFrameCounters& GetLastFrame() const { return m_lastFrame; }
            const GPUFrameCounters& GetCurrentFrame() const { return m_current; }

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
                return report;
            }

          private:
            GPUPerfCounters() = default;

            static constexpr uint32_t HISTORY_SIZE = 120; // 2 seconds at 60fps

            GPUFrameCounters m_current;
            GPUFrameCounters m_lastFrame;
            std::array<GPUFrameCounters, HISTORY_SIZE> m_history;
            std::array<uint64_t, static_cast<size_t>(GPUCounterCategory::Count)> m_rollingSum = {};
            uint32_t m_historyIndex = 0;
            uint32_t m_frameCount = 0;
        };

    } // namespace Graphics
} // namespace Spark
