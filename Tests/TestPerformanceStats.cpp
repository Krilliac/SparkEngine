// TestPerformanceStats.cpp - Tests for performance statistics collection
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <thread>

namespace TestPerf
{

    class FrameTimeRingBuffer
    {
      public:
        static constexpr int CAPACITY = 300;

        void Push(float v)
        {
            m_buf[m_write] = v;
            m_write = (m_write + 1) % CAPACITY;
            if (m_count < CAPACITY)
                m_count++;
        }

        float Get(int index) const
        {
            if (index < 0 || index >= m_count)
                return 0.0f;
            int actual = (m_write - m_count + index + CAPACITY) % CAPACITY;
            return m_buf[actual];
        }

        int GetCount() const { return m_count; }

        float GetMin() const
        {
            if (m_count == 0)
                return 0.0f;
            float v = m_buf[(m_write - m_count + CAPACITY) % CAPACITY];
            for (int i = 1; i < m_count; ++i)
            {
                int idx = (m_write - m_count + i + CAPACITY) % CAPACITY;
                v = std::min(v, m_buf[idx]);
            }
            return v;
        }

        float GetMax() const
        {
            if (m_count == 0)
                return 0.0f;
            float v = m_buf[(m_write - m_count + CAPACITY) % CAPACITY];
            for (int i = 1; i < m_count; ++i)
            {
                int idx = (m_write - m_count + i + CAPACITY) % CAPACITY;
                v = std::max(v, m_buf[idx]);
            }
            return v;
        }

        float GetAverage() const
        {
            if (m_count == 0)
                return 0.0f;
            float sum = 0.0f;
            for (int i = 0; i < m_count; ++i)
            {
                int idx = (m_write - m_count + i + CAPACITY) % CAPACITY;
                sum += m_buf[idx];
            }
            return sum / m_count;
        }

        void Clear()
        {
            m_count = 0;
            m_write = 0;
            std::memset(m_buf, 0, sizeof(m_buf));
        }

      private:
        float m_buf[CAPACITY] = {};
        int m_write = 0;
        int m_count = 0;
    };

    struct SystemTiming
    {
        std::string name;
        float lastTimeMs = 0.0f;
        float peakTimeMs = 0.0f;
        int sampleCount = 0;
    };

    class PerformanceStats
    {
      public:
        void BeginFrame() { m_start = Clock::now(); }

        void EndFrame()
        {
            auto now = Clock::now();
            float ms = std::chrono::duration<float, std::milli>(now - m_start).count();
            m_history.Push(ms);
            m_totalFrames++;
        }

        void RecordSystemTime(const std::string& name, float timeMs)
        {
            auto& t = m_systems[name];
            t.name = name;
            t.lastTimeMs = timeMs;
            t.sampleCount++;
            if (timeMs > t.peakTimeMs)
                t.peakTimeMs = timeMs;
        }

        void SetDrawCallCount(int c) { m_drawCalls = c; }
        void SetEntityCount(int c) { m_entities = c; }
        void SetMemoryUsage(uint64_t b) { m_memory = b; }

        const FrameTimeRingBuffer& GetHistory() const { return m_history; }
        int GetDrawCallCount() const { return m_drawCalls; }
        int GetEntityCount() const { return m_entities; }
        uint64_t GetMemoryUsage() const { return m_memory; }
        uint64_t GetTotalFrames() const { return m_totalFrames; }
        const std::unordered_map<std::string, SystemTiming>& GetSystems() const { return m_systems; }

        void Reset()
        {
            m_history.Clear();
            m_systems.clear();
            m_totalFrames = 0;
            m_drawCalls = 0;
            m_entities = 0;
            m_memory = 0;
        }

      private:
        using Clock = std::chrono::high_resolution_clock;
        std::chrono::time_point<Clock> m_start;
        FrameTimeRingBuffer m_history;
        std::unordered_map<std::string, SystemTiming> m_systems;
        uint64_t m_totalFrames = 0;
        int m_drawCalls = 0;
        int m_entities = 0;
        uint64_t m_memory = 0;
    };

} // namespace TestPerf

// =============================================================================
// Tests
// =============================================================================

TEST(PerfStats_RingBufferPush)
{
    TestPerf::FrameTimeRingBuffer buf;
    EXPECT_EQ(buf.GetCount(), 0);

    buf.Push(16.6f);
    buf.Push(16.7f);
    buf.Push(16.5f);
    EXPECT_EQ(buf.GetCount(), 3);
    EXPECT_NEAR(buf.Get(0), 16.6f, 0.01f);
    EXPECT_NEAR(buf.Get(1), 16.7f, 0.01f);
    EXPECT_NEAR(buf.Get(2), 16.5f, 0.01f);
}

TEST(PerfStats_RingBufferWrap)
{
    TestPerf::FrameTimeRingBuffer buf;
    // Fill past capacity
    for (int i = 0; i < 310; ++i)
    {
        buf.Push(static_cast<float>(i));
    }
    EXPECT_EQ(buf.GetCount(), TestPerf::FrameTimeRingBuffer::CAPACITY);
    // Oldest should be 10 (310 - 300)
    EXPECT_NEAR(buf.Get(0), 10.0f, 0.01f);
    // Newest should be 309
    EXPECT_NEAR(buf.Get(299), 309.0f, 0.01f);
}

TEST(PerfStats_RingBufferStats)
{
    TestPerf::FrameTimeRingBuffer buf;
    buf.Push(10.0f);
    buf.Push(20.0f);
    buf.Push(30.0f);

    EXPECT_NEAR(buf.GetMin(), 10.0f, 0.01f);
    EXPECT_NEAR(buf.GetMax(), 30.0f, 0.01f);
    EXPECT_NEAR(buf.GetAverage(), 20.0f, 0.01f);
}

TEST(PerfStats_RingBufferClear)
{
    TestPerf::FrameTimeRingBuffer buf;
    buf.Push(1.0f);
    buf.Push(2.0f);
    buf.Clear();
    EXPECT_EQ(buf.GetCount(), 0);
    EXPECT_NEAR(buf.GetAverage(), 0.0f, 0.001f);
}

TEST(PerfStats_RecordFrames)
{
    TestPerf::PerformanceStats stats;
    EXPECT_EQ(stats.GetTotalFrames(), (uint64_t)0);

    stats.BeginFrame();
    stats.EndFrame();
    stats.BeginFrame();
    stats.EndFrame();

    EXPECT_EQ(stats.GetTotalFrames(), (uint64_t)2);
    EXPECT_EQ(stats.GetHistory().GetCount(), 2);
}

TEST(PerfStats_SystemTimings)
{
    TestPerf::PerformanceStats stats;

    stats.RecordSystemTime("Physics", 2.5f);
    stats.RecordSystemTime("Render", 8.0f);
    stats.RecordSystemTime("AI", 1.2f);

    auto& systems = stats.GetSystems();
    EXPECT_EQ((int)systems.size(), 3);
    EXPECT_NEAR(systems.at("Physics").lastTimeMs, 2.5f, 0.01f);
    EXPECT_NEAR(systems.at("Render").lastTimeMs, 8.0f, 0.01f);
    EXPECT_NEAR(systems.at("AI").lastTimeMs, 1.2f, 0.01f);
}

TEST(PerfStats_PeakTracking)
{
    TestPerf::PerformanceStats stats;

    stats.RecordSystemTime("Physics", 2.0f);
    stats.RecordSystemTime("Physics", 5.0f);
    stats.RecordSystemTime("Physics", 3.0f);

    auto& systems = stats.GetSystems();
    EXPECT_NEAR(systems.at("Physics").peakTimeMs, 5.0f, 0.01f);
    EXPECT_NEAR(systems.at("Physics").lastTimeMs, 3.0f, 0.01f);
    EXPECT_EQ(systems.at("Physics").sampleCount, 3);
}

TEST(PerfStats_Counters)
{
    TestPerf::PerformanceStats stats;

    stats.SetDrawCallCount(150);
    stats.SetEntityCount(500);
    stats.SetMemoryUsage(1024 * 1024 * 256); // 256MB

    EXPECT_EQ(stats.GetDrawCallCount(), 150);
    EXPECT_EQ(stats.GetEntityCount(), 500);
    EXPECT_EQ(stats.GetMemoryUsage(), (uint64_t)(1024 * 1024 * 256));
}

TEST(PerfStats_Reset)
{
    TestPerf::PerformanceStats stats;

    stats.BeginFrame();
    stats.EndFrame();
    stats.RecordSystemTime("Physics", 2.0f);
    stats.SetDrawCallCount(100);

    stats.Reset();
    EXPECT_EQ(stats.GetTotalFrames(), (uint64_t)0);
    EXPECT_EQ(stats.GetDrawCallCount(), 0);
    EXPECT_EQ(stats.GetHistory().GetCount(), 0);
    EXPECT_TRUE(stats.GetSystems().empty());
}

TEST(PerfStats_RingBufferOutOfBounds)
{
    TestPerf::FrameTimeRingBuffer buf;
    buf.Push(1.0f);

    EXPECT_NEAR(buf.Get(-1), 0.0f, 0.001f);  // Negative index
    EXPECT_NEAR(buf.Get(100), 0.0f, 0.001f); // Beyond count
}
