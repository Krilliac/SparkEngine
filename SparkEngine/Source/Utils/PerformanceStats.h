/**
 * @file PerformanceStats.h
 * @brief Real-time performance monitoring and statistics collection
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a lightweight performance statistics collector that tracks
 * FPS, frame times, memory usage, and per-system timing breakdowns.
 * Designed to complement the existing Profiler.h with a simpler,
 * always-on stats interface.
 *
 * ## Usage
 * @code
 *   Spark::PerformanceStats stats;
 *
 *   // Each frame
 *   stats.BeginFrame();
 *   // ... do work ...
 *   stats.RecordSystemTime("Physics", physicsMs);
 *   stats.RecordSystemTime("Render", renderMs);
 *   stats.EndFrame();
 *
 *   // Query
 *   float fps = stats.GetAverageFPS();
 *   float frameTime = stats.GetAverageFrameTime();
 *   auto& history = stats.GetFrameTimeHistory();
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cstdint>
#include <sstream>

#include "RingBuffer.h"

namespace Spark
{

    // =============================================================================
    // Frame Time Ring Buffer (backed by generic RingBuffer)
    // =============================================================================

    /**
 * @class FrameTimeRingBuffer
 * @brief Fixed-size ring buffer storing recent frame times for graphing
 *
 * Wraps RingBuffer<float, 300> with convenience aggregation methods
 * (min, max, average) and oldest-first Get() access.
 */
    class FrameTimeRingBuffer
    {
      public:
        static constexpr size_t CAPACITY = 300; ///< ~5 seconds at 60fps

        void Push(float frameTimeMs) { m_buffer.Push(frameTimeMs); }

        /** @brief Get a value by index (0 = oldest) */
        float Get(int index) const
        {
            if (index < 0 || index >= static_cast<int>(m_buffer.Size()))
                return 0.0f;
            // RingBuffer [0] = newest, so reverse for oldest-first access
            return m_buffer[m_buffer.Size() - 1 - index];
        }

        int GetCount() const { return static_cast<int>(m_buffer.Size()); }

        float GetMin() const
        {
            if (m_buffer.IsEmpty())
                return 0.0f;
            float minVal = m_buffer[0];
            for (float v : m_buffer)
                minVal = std::min(minVal, v);
            return minVal;
        }

        float GetMax() const
        {
            if (m_buffer.IsEmpty())
                return 0.0f;
            float maxVal = m_buffer[0];
            for (float v : m_buffer)
                maxVal = std::max(maxVal, v);
            return maxVal;
        }

        float GetAverage() const
        {
            if (m_buffer.IsEmpty())
                return 0.0f;
            float sum = 0.0f;
            for (float v : m_buffer)
                sum += v;
            return sum / m_buffer.Size();
        }

        void Clear() { m_buffer.Clear(); }

        /** @brief Access the underlying ring buffer for iteration */
        const RingBuffer<float, CAPACITY>& GetBuffer() const { return m_buffer; }

      private:
        RingBuffer<float, CAPACITY> m_buffer;
    };

    // =============================================================================
    // System Timing Entry
    // =============================================================================

    /**
 * @brief Timing data for a single engine system
 */
    struct SystemTiming
    {
        std::string name;
        float lastTimeMs = 0.0f;    ///< Last frame's time in ms
        float averageTimeMs = 0.0f; ///< Running average
        float peakTimeMs = 0.0f;    ///< Peak time in current window
        int sampleCount = 0;
    };

    // =============================================================================
    // Performance Stats
    // =============================================================================

    /**
 * @class PerformanceStats
 * @brief Collects and reports runtime performance metrics
 *
 * Call BeginFrame() at the start of each frame and EndFrame() at the end.
 * Between those calls, use RecordSystemTime() to track individual system costs.
 */
    class PerformanceStats
    {
      public:
        PerformanceStats() { m_frameStart = Clock::now(); }

        // ---- Frame lifecycle ----

        /**
     * @brief Mark the start of a new frame
     */
        void BeginFrame() { m_frameStart = Clock::now(); }

        /**
     * @brief Mark the end of the current frame and record statistics
     */
        void EndFrame()
        {
            auto now = Clock::now();
            float frameMs = std::chrono::duration<float, std::milli>(now - m_frameStart).count();

            m_frameTimeHistory.Push(frameMs);
            m_totalFrames++;
            m_totalFrameTimeMs += frameMs;

            // Update FPS counters
            m_fpsAccumulator += frameMs;
            m_fpsFrameCount++;

            if (m_fpsAccumulator >= 1000.0f)
            {
                m_currentFPS = m_fpsFrameCount * 1000.0f / m_fpsAccumulator;
                m_fpsAccumulator = 0.0f;
                m_fpsFrameCount = 0;

                // Track min/max FPS
                if (m_currentFPS > m_maxFPS)
                    m_maxFPS = m_currentFPS;
                if (m_currentFPS < m_minFPS || m_minFPS == 0.0f)
                    m_minFPS = m_currentFPS;
            }

            // Update per-system averages
            for (auto& [name, timing] : m_systemTimings)
            {
                if (timing.sampleCount > 0)
                {
                    timing.averageTimeMs = (timing.averageTimeMs * 0.9f) + (timing.lastTimeMs * 0.1f);
                }
            }
        }

        // ---- System timing ----

        /**
     * @brief Record the time spent in a named system this frame
     *
     * @param systemName  Name of the system (e.g. "Physics", "Render", "AI")
     * @param timeMs      Time in milliseconds
     */
        void RecordSystemTime(const std::string& systemName, float timeMs)
        {
            auto& timing = m_systemTimings[systemName];
            timing.name = systemName;
            timing.lastTimeMs = timeMs;
            timing.sampleCount++;
            if (timeMs > timing.peakTimeMs)
            {
                timing.peakTimeMs = timeMs;
            }
        }

        // ---- Counters ----

        /** @brief Set the current draw call count (from renderer) */
        void SetDrawCallCount(int count) { m_drawCallCount = count; }

        /** @brief Set the current entity count (from ECS world) */
        void SetEntityCount(int count) { m_entityCount = count; }

        /** @brief Set the current physics body count */
        void SetPhysicsBodyCount(int count) { m_physicsBodyCount = count; }

        /** @brief Set memory usage in bytes */
        void SetMemoryUsage(uint64_t bytes) { m_memoryUsageBytes = bytes; }

        /** @brief Set triangle count rendered this frame */
        void SetTriangleCount(int count) { m_triangleCount = count; }

        // ---- Queries ----

        /** @brief Get the current FPS (updated every second) */
        float GetCurrentFPS() const { return m_currentFPS; }

        /** @brief Get the average FPS over the entire session */
        float GetAverageFPS() const
        {
            if (m_totalFrameTimeMs <= 0.0f)
                return 0.0f;
            return m_totalFrames * 1000.0f / m_totalFrameTimeMs;
        }

        /** @brief Get the minimum recorded FPS */
        float GetMinFPS() const { return m_minFPS; }

        /** @brief Get the maximum recorded FPS */
        float GetMaxFPS() const { return m_maxFPS; }

        /** @brief Get the average frame time in milliseconds */
        float GetAverageFrameTime() const { return m_frameTimeHistory.GetAverage(); }

        /** @brief Get the frame time history ring buffer for graphing */
        const FrameTimeRingBuffer& GetFrameTimeHistory() const { return m_frameTimeHistory; }

        /** @brief Get all system timings */
        const std::unordered_map<std::string, SystemTiming>& GetSystemTimings() const { return m_systemTimings; }

        int GetDrawCallCount() const { return m_drawCallCount; }
        int GetEntityCount() const { return m_entityCount; }
        int GetPhysicsBodyCount() const { return m_physicsBodyCount; }
        uint64_t GetMemoryUsageBytes() const { return m_memoryUsageBytes; }
        int GetTriangleCount() const { return m_triangleCount; }
        uint64_t GetTotalFrames() const { return m_totalFrames; }

        /** @brief Get a formatted summary string */
        std::string GetSummary() const
        {
            std::ostringstream ss;
            ss << "FPS: " << static_cast<int>(m_currentFPS) << " (min:" << static_cast<int>(m_minFPS)
               << " max:" << static_cast<int>(m_maxFPS) << ") Frame: " << GetAverageFrameTime() << "ms"
               << " Draw calls: " << m_drawCallCount << " Entities: " << m_entityCount << " Tris: " << m_triangleCount;

            if (m_memoryUsageBytes > 0)
            {
                ss << " Mem: " << (m_memoryUsageBytes / (1024 * 1024)) << "MB";
            }

            return ss.str();
        }

        /**
     * @brief Reset all statistics
     */
        void Reset()
        {
            m_frameTimeHistory.Clear();
            m_systemTimings.clear();
            m_currentFPS = 0.0f;
            m_minFPS = 0.0f;
            m_maxFPS = 0.0f;
            m_totalFrames = 0;
            m_totalFrameTimeMs = 0.0f;
            m_fpsAccumulator = 0.0f;
            m_fpsFrameCount = 0;
            m_drawCallCount = 0;
            m_entityCount = 0;
            m_physicsBodyCount = 0;
            m_memoryUsageBytes = 0;
            m_triangleCount = 0;
        }

      private:
        using Clock = std::chrono::high_resolution_clock;
        using TimePoint = std::chrono::time_point<Clock>;

        TimePoint m_frameStart;

        // Frame time tracking
        FrameTimeRingBuffer m_frameTimeHistory;
        uint64_t m_totalFrames = 0;
        float m_totalFrameTimeMs = 0.0f;

        // FPS tracking
        float m_currentFPS = 0.0f;
        float m_minFPS = 0.0f;
        float m_maxFPS = 0.0f;
        float m_fpsAccumulator = 0.0f;
        int m_fpsFrameCount = 0;

        // Per-system timing
        std::unordered_map<std::string, SystemTiming> m_systemTimings;

        // Counters (set externally by engine systems)
        int m_drawCallCount = 0;
        int m_entityCount = 0;
        int m_physicsBodyCount = 0;
        uint64_t m_memoryUsageBytes = 0;
        int m_triangleCount = 0;
    };

} // namespace Spark
