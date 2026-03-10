/**
 * @file GPUTimestampQuery.h
 * @brief Per-pass GPU timing with double-buffered timestamp queries
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides precise per-pass GPU timing using D3D11 timestamp queries with
 * double-buffering to avoid GPU stalls. Results from frame N-2 are read
 * while frame N is being recorded. Includes rolling history for averaging.
 *
 * ## Usage
 * @code
 *   GPUTimestampQuery gpuTimer;
 *   gpuTimer.Initialize(device, 32);
 *
 *   // Each frame:
 *   gpuTimer.BeginFrame(context);
 *
 *   {
 *       ScopedTimestamp scope(gpuTimer, context, "ShadowPass");
 *       // ... render shadows ...
 *   }
 *
 *   gpuTimer.EndFrame(context);
 *
 *   // Read results (from 2 frames ago):
 *   float shadowMs = gpuTimer.GetPassTimeMs("ShadowPass");
 * @endcode
 *
 * @see GraphicsEngine, GPUDebugMarkers, Profiler
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <algorithm>
#include <numeric>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    /**
 * @brief Per-pass GPU timing system with double-buffered queries
 *
 * Allocates pools of timestamp query pairs and disjoint queries, using
 * double-buffering so that results from frame N-2 can be read without
 * stalling the GPU pipeline.
 */
    class GPUTimestampQuery
    {
      public:
        static constexpr uint32_t kFrameLatency = 2;  ///< Frames of latency before results are available
        static constexpr uint32_t kHistorySize = 120; ///< Rolling history frames per pass

        GPUTimestampQuery() = default;
        ~GPUTimestampQuery() { Shutdown(); }

        // Non-copyable, non-movable
        GPUTimestampQuery(const GPUTimestampQuery&) = delete;
        GPUTimestampQuery& operator=(const GPUTimestampQuery&) = delete;
        GPUTimestampQuery(GPUTimestampQuery&&) = delete;
        GPUTimestampQuery& operator=(GPUTimestampQuery&&) = delete;

        /**
     * @brief Initialize query pools
     * @param device D3D11 device
     * @param maxTimers Maximum simultaneous timer pairs per frame
     * @return true on success
     */
        bool Initialize(ID3D11Device* device, uint32_t maxTimers = 32)
        {
            if (!device || maxTimers == 0)
            {
                return false;
            }

            m_maxTimers = maxTimers;

            // Create query pools for each buffered frame
            for (uint32_t frame = 0; frame < kFrameLatency; ++frame)
            {
                auto& frameData = m_frames[frame];

                // Disjoint query (one per frame, for frequency + validity)
                D3D11_QUERY_DESC disjointDesc = {};
                disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                HRESULT hr = device->CreateQuery(&disjointDesc, frameData.disjointQuery.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    Shutdown();
                    return false;
                }

                // Timestamp query pairs
                frameData.beginQueries.resize(maxTimers);
                frameData.endQueries.resize(maxTimers);

                D3D11_QUERY_DESC tsDesc = {};
                tsDesc.Query = D3D11_QUERY_TIMESTAMP;

                for (uint32_t i = 0; i < maxTimers; ++i)
                {
                    hr = device->CreateQuery(&tsDesc, frameData.beginQueries[i].ReleaseAndGetAddressOf());
                    if (FAILED(hr))
                    {
                        Shutdown();
                        return false;
                    }

                    hr = device->CreateQuery(&tsDesc, frameData.endQueries[i].ReleaseAndGetAddressOf());
                    if (FAILED(hr))
                    {
                        Shutdown();
                        return false;
                    }
                }

                frameData.timerNames.resize(maxTimers);
                frameData.activeCount = 0;
            }

            m_initialized = true;
            m_frameIndex = 0;
            return true;
        }

        /**
     * @brief Release all query objects
     */
        void Shutdown()
        {
            for (auto& frame : m_frames)
            {
                frame.disjointQuery.Reset();
                frame.beginQueries.clear();
                frame.endQueries.clear();
                frame.timerNames.clear();
                frame.activeCount = 0;
            }

            m_passTimes.clear();
            m_passHistory.clear();
            m_initialized = false;
            m_frameIndex = 0;
        }

        /**
     * @brief Begin a new frame — start disjoint query and collect previous results
     * @param context D3D11 device context
     */
        void BeginFrame(ID3D11DeviceContext* context)
        {
            if (!m_initialized || !context)
            {
                return;
            }

            // Collect results from the oldest buffered frame (N-2)
            uint32_t readFrame = (m_frameIndex + 1) % kFrameLatency;
            if (m_frameIndex >= kFrameLatency)
            {
                CollectResults(context, readFrame);
            }

            // Start current frame's disjoint query
            uint32_t writeFrame = m_frameIndex % kFrameLatency;
            auto& frameData = m_frames[writeFrame];
            frameData.activeCount = 0;

            context->Begin(frameData.disjointQuery.Get());
        }

        /**
     * @brief End the current frame — end disjoint query
     * @param context D3D11 device context
     */
        void EndFrame(ID3D11DeviceContext* context)
        {
            if (!m_initialized || !context)
            {
                return;
            }

            uint32_t writeFrame = m_frameIndex % kFrameLatency;
            context->End(m_frames[writeFrame].disjointQuery.Get());

            m_frameIndex++;
        }

        /**
     * @brief Begin a timestamp for a named pass
     * @param context D3D11 device context
     * @param name Pass name (e.g., "ShadowPass", "GBuffer", "Lighting")
     * @return Timer ID for use with EndTimestamp, or UINT32_MAX on failure
     */
        uint32_t BeginTimestamp(ID3D11DeviceContext* context, const char* name)
        {
            if (!m_initialized || !context || !name)
            {
                return UINT32_MAX;
            }

            uint32_t writeFrame = m_frameIndex % kFrameLatency;
            auto& frameData = m_frames[writeFrame];

            if (frameData.activeCount >= m_maxTimers)
            {
                return UINT32_MAX; // Pool exhausted
            }

            uint32_t timerID = frameData.activeCount;
            frameData.timerNames[timerID] = name;
            context->End(frameData.beginQueries[timerID].Get());

            frameData.activeCount++;
            return timerID;
        }

        /**
     * @brief End a timestamp for a previously started timer
     * @param context D3D11 device context
     * @param timerID Timer ID returned by BeginTimestamp
     */
        void EndTimestamp(ID3D11DeviceContext* context, uint32_t timerID)
        {
            if (!m_initialized || !context || timerID == UINT32_MAX)
            {
                return;
            }

            uint32_t writeFrame = m_frameIndex % kFrameLatency;
            auto& frameData = m_frames[writeFrame];

            if (timerID >= frameData.activeCount)
            {
                return;
            }

            context->End(frameData.endQueries[timerID].Get());
        }

        /**
     * @brief Get the most recent completed timing for a pass
     * @param name Pass name
     * @return Time in milliseconds, or 0.0f if not available
     */
        float GetPassTimeMs(const char* name) const
        {
            auto it = m_passTimes.find(name);
            return (it != m_passTimes.end()) ? it->second : 0.0f;
        }

        /**
     * @brief Get the rolling average timing for a pass
     * @param name Pass name
     * @param frameCount Number of frames to average over (clamped to history size)
     * @return Average time in milliseconds
     */
        float GetAveragePassTimeMs(const char* name, uint32_t frameCount = 60) const
        {
            auto it = m_passHistory.find(name);
            if (it == m_passHistory.end())
            {
                return 0.0f;
            }

            const auto& history = it->second;
            uint32_t count = std::min(frameCount, static_cast<uint32_t>(history.count));
            if (count == 0)
            {
                return 0.0f;
            }

            float sum = 0.0f;
            uint32_t readIdx = history.writeIndex;
            for (uint32_t i = 0; i < count; ++i)
            {
                readIdx = (readIdx == 0) ? kHistorySize - 1 : readIdx - 1;
                sum += history.samples[readIdx];
            }

            return sum / static_cast<float>(count);
        }

        /**
     * @brief Get all most-recent pass timings
     * @return Map of pass name to time in milliseconds
     */
        const std::unordered_map<std::string, float>& GetAllPassTimes() const { return m_passTimes; }

        /**
     * @brief Get the total GPU time across all measured passes
     * @return Sum of all pass times in milliseconds
     */
        float GetTotalGPUTimeMs() const
        {
            float total = 0.0f;
            for (const auto& [name, time] : m_passTimes)
            {
                total += time;
            }
            return total;
        }

        /** @brief Check if initialized */
        bool IsInitialized() const { return m_initialized; }

        /** @brief Get number of active timers in the current frame */
        uint32_t GetActiveTimerCount() const
        {
            if (!m_initialized)
            {
                return 0;
            }
            uint32_t writeFrame = m_frameIndex % kFrameLatency;
            return m_frames[writeFrame].activeCount;
        }

        /**
     * @brief Get a console-friendly status string with all pass timings
     */
        std::wstring Console_GetStatus() const
        {
            if (!m_initialized)
            {
                return L"[GPUTimestampQuery] Not initialized";
            }

            std::wstring result = L"[GPUTimestampQuery] Pass timings:\n";

            if (m_passTimes.empty())
            {
                result += L"  (no results yet — waiting for frame latency)\n";
                return result;
            }

            float total = 0.0f;
            for (const auto& [name, time] : m_passTimes)
            {
                wchar_t line[128];
                float avg = GetAveragePassTimeMs(name.c_str(), 60);
                swprintf(line, 128, L"  %-24S  %6.3f ms  (avg: %6.3f ms)\n", name.c_str(), time, avg);
                result += line;
                total += time;
            }

            wchar_t totalLine[64];
            swprintf(totalLine, 64, L"  %-24s  %6.3f ms\n", L"TOTAL", total);
            result += totalLine;

            return result;
        }

      private:
        /**
     * @brief Rolling history buffer for a single pass
     */
        struct PassHistory
        {
            std::array<float, kHistorySize> samples = {};
            uint32_t writeIndex = 0;
            uint32_t count = 0;

            void Push(float value)
            {
                samples[writeIndex] = value;
                writeIndex = (writeIndex + 1) % kHistorySize;
                if (count < kHistorySize)
                {
                    count++;
                }
            }
        };

        /**
     * @brief Per-frame query data
     */
        struct FrameData
        {
            ComPtr<ID3D11Query> disjointQuery;
            std::vector<ComPtr<ID3D11Query>> beginQueries;
            std::vector<ComPtr<ID3D11Query>> endQueries;
            std::vector<std::string> timerNames;
            uint32_t activeCount = 0;
        };

        /**
     * @brief Collect results from a completed frame
     */
        void CollectResults(ID3D11DeviceContext* context, uint32_t frameSlot)
        {
            auto& frameData = m_frames[frameSlot];

            if (frameData.activeCount == 0)
            {
                return;
            }

            // Get disjoint query data (contains frequency and validity)
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData = {};
            HRESULT hr = context->GetData(frameData.disjointQuery.Get(), &disjointData, sizeof(disjointData),
                                          D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE)
            {
                return; // Results not ready yet
            }

            if (FAILED(hr) || disjointData.Disjoint)
            {
                return; // Invalid results (GPU frequency changed mid-frame)
            }

            double ticksToMs = 1000.0 / static_cast<double>(disjointData.Frequency);

            // Collect each timer pair
            for (uint32_t i = 0; i < frameData.activeCount; ++i)
            {
                UINT64 beginTicks = 0;
                UINT64 endTicks = 0;

                hr = context->GetData(frameData.beginQueries[i].Get(), &beginTicks, sizeof(UINT64),
                                      D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr != S_OK)
                {
                    continue;
                }

                hr = context->GetData(frameData.endQueries[i].Get(), &endTicks, sizeof(UINT64),
                                      D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr != S_OK)
                {
                    continue;
                }

                float timeMs = static_cast<float>(static_cast<double>(endTicks - beginTicks) * ticksToMs);

                const auto& name = frameData.timerNames[i];
                m_passTimes[name] = timeMs;
                m_passHistory[name].Push(timeMs);
            }
        }

        std::array<FrameData, kFrameLatency> m_frames;
        std::unordered_map<std::string, float> m_passTimes;         ///< Most recent results
        std::unordered_map<std::string, PassHistory> m_passHistory; ///< Rolling history
        uint32_t m_maxTimers = 0;
        uint32_t m_frameIndex = 0;
        bool m_initialized = false;
    };

    /**
 * @brief RAII scoped timestamp — begins on construction, ends on destruction
 */
    class ScopedTimestamp
    {
      public:
        ScopedTimestamp(GPUTimestampQuery& query, ID3D11DeviceContext* context, const char* name)
            : m_query(query), m_context(context)
        {
            m_timerID = m_query.BeginTimestamp(m_context, name);
        }

        ~ScopedTimestamp() { m_query.EndTimestamp(m_context, m_timerID); }

        // Non-copyable, non-movable
        ScopedTimestamp(const ScopedTimestamp&) = delete;
        ScopedTimestamp& operator=(const ScopedTimestamp&) = delete;
        ScopedTimestamp(ScopedTimestamp&&) = delete;
        ScopedTimestamp& operator=(ScopedTimestamp&&) = delete;

      private:
        GPUTimestampQuery& m_query;
        ID3D11DeviceContext* m_context;
        uint32_t m_timerID;
    };

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
