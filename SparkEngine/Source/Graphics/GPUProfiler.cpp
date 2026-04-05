/**
 * @file GPUProfiler.cpp
 * @brief GPU profiler implementation
 */

#include "GPUProfiler.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace Spark::Graphics
{

    GPUProfiler& GPUProfiler::GetInstance()
    {
        static GPUProfiler instance;
        return instance;
    }

    void GPUProfiler::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GPUProfiler::Initialize");
        m_frameIndex = 0;
        m_globalFrameCounter = 0;
        m_currentDepth = 0;
        m_enabled = true;
        m_initialized = true;
        m_eventStack.clear();
        m_currentFrameTimestamps.clear();

        for (auto& frame : m_frameHistory)
            frame = {};
    }

    void GPUProfiler::Shutdown()
    {
        m_enabled = false;
        m_initialized = false;
        m_eventStack.clear();
        m_currentFrameTimestamps.clear();
    }

    void GPUProfiler::BeginFrame()
    {
        if (!m_enabled)
            return;

        m_currentFrameTimestamps.clear();
        m_eventStack.clear();
        m_currentDepth = 0;

        auto now = std::chrono::high_resolution_clock::now();
        m_frameStartTime =
            static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()) /
            1000.0f;
    }

    void GPUProfiler::EndFrame()
    {
        if (!m_enabled)
            return;

        auto now = std::chrono::high_resolution_clock::now();
        float frameEndTime =
            static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()) /
            1000.0f;

        auto& frame = m_frameHistory[m_frameIndex % MAX_HISTORY];
        frame.totalGPUTimeMs = frameEndTime - m_frameStartTime;
        frame.timestamps = m_currentFrameTimestamps;
        frame.frameNumber = m_globalFrameCounter;

        m_frameIndex = (m_frameIndex + 1) % MAX_HISTORY;
        m_globalFrameCounter++;
    }

    void GPUProfiler::BeginEvent(const std::string& name)
    {
        if (!m_enabled)
            return;

        auto now = std::chrono::high_resolution_clock::now();
        float currentTime =
            static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()) /
            1000.0f;

        PendingEvent ev;
        ev.name = name;
        ev.depth = m_currentDepth;
        ev.startTimeMs = currentTime;
        m_eventStack.push_back(ev);

        m_currentDepth++;
    }

    void GPUProfiler::EndEvent()
    {
        if (!m_enabled || m_eventStack.empty())
            return;

        auto now = std::chrono::high_resolution_clock::now();
        float currentTime =
            static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()) /
            1000.0f;

        auto& pending = m_eventStack.back();

        GPUTimestamp ts;
        ts.name = pending.name;
        ts.depth = pending.depth;
        ts.durationMs = currentTime - pending.startTimeMs;
        m_currentFrameTimestamps.push_back(ts);

        m_eventStack.pop_back();
        if (m_currentDepth > 0)
            m_currentDepth--;
    }

    const GPUProfileFrame& GPUProfiler::GetLastFrame() const
    {
        uint32_t idx = (m_frameIndex + MAX_HISTORY - 1) % MAX_HISTORY;
        return m_frameHistory[idx];
    }

    std::vector<GPUProfileFrame> GPUProfiler::GetFrameHistory(uint32_t count) const
    {
        count = std::min(count, MAX_HISTORY);
        count = std::min(count, m_globalFrameCounter);
        std::vector<GPUProfileFrame> result;
        result.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t idx = (m_frameIndex + MAX_HISTORY - count + i) % MAX_HISTORY;
            result.push_back(m_frameHistory[idx]);
        }
        return result;
    }

    float GPUProfiler::GetAverageGPUTime(uint32_t frameCount) const
    {
        frameCount = std::min(frameCount, MAX_HISTORY);
        frameCount = std::min(frameCount, m_globalFrameCounter);
        if (frameCount == 0)
            return 0.0f;

        float total = 0.0f;
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            uint32_t idx = (m_frameIndex + MAX_HISTORY - frameCount + i) % MAX_HISTORY;
            total += m_frameHistory[idx].totalGPUTimeMs;
        }
        return total / static_cast<float>(frameCount);
    }

    void GPUProfiler::SetEnabled(bool enabled)
    {
        m_enabled = enabled;
    }

    std::string GPUProfiler::ExportToJSON() const
    {
        std::ostringstream ss;
        ss << "{\"traceEvents\":[";

        bool first = true;
        auto history = GetFrameHistory(std::min(m_globalFrameCounter, MAX_HISTORY));

        for (const auto& frame : history)
        {
            float baseTime = static_cast<float>(frame.frameNumber) * 16666.0f; // ~60fps in microseconds

            for (const auto& ts : frame.timestamps)
            {
                if (!first)
                    ss << ",";
                first = false;

                ss << "{\"name\":\"" << ts.name << "\"," << "\"cat\":\"GPU\"," << "\"ph\":\"X\","
                   << "\"ts\":" << static_cast<int64_t>(baseTime) << ","
                   << "\"dur\":" << static_cast<int64_t>(ts.durationMs * 1000.0f) << "," << "\"pid\":1,"
                   << "\"tid\":" << ts.depth << "}";
            }
        }

        ss << "]}";
        return ss.str();
    }

    void GPUProfiler::ExportToCSV(const std::string& path) const
    {
        std::ofstream file(path);
        if (!file.is_open())
            return;

        file << "Frame,Event,Depth,DurationMs\n";

        auto history = GetFrameHistory(std::min(m_globalFrameCounter, MAX_HISTORY));
        for (const auto& frame : history)
        {
            for (const auto& ts : frame.timestamps)
            {
                file << frame.frameNumber << "," << ts.name << "," << ts.depth << "," << ts.durationMs << "\n";
            }
        }
    }

    std::string GPUProfiler::Console_GetStatus() const
    {
        if (!m_enabled)
            return "GPUProfiler: Disabled";

        auto avg = GetAverageGPUTime(60);
        return "GPUProfiler: " + std::to_string(m_globalFrameCounter) + " frames, avg " +
               std::to_string(avg).substr(0, 5) + "ms";
    }

} // namespace Spark::Graphics
