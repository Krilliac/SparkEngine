/**
 * @file TestGPUPerfCounters.cpp
 * @brief Tests for categorized GPU performance counter tracking
 */

#include "TestFramework.h"
#include <array>
#include <cstdint>

namespace
{

    enum class TestGPUCounter : uint8_t
    {
        DrawCalls,
        Primitives,
        StateChanges,
        TextureUploads,
        Barriers,
        Count
    };

    class TestGPUCounters
    {
      public:
        void Increment(TestGPUCounter cat, uint64_t amount = 1) { m_current[static_cast<size_t>(cat)] += amount; }

        void EndFrame()
        {
            m_lastFrame = m_current;
            m_current.fill(0);
            m_frameCount++;
        }

        void Reset()
        {
            m_current.fill(0);
            m_lastFrame.fill(0);
            m_frameCount = 0;
        }

        uint64_t GetLastFrame(TestGPUCounter cat) const { return m_lastFrame[static_cast<size_t>(cat)]; }
        uint64_t GetCurrent(TestGPUCounter cat) const { return m_current[static_cast<size_t>(cat)]; }
        uint32_t GetFrameCount() const { return m_frameCount; }

      private:
        std::array<uint64_t, static_cast<size_t>(TestGPUCounter::Count)> m_current = {};
        std::array<uint64_t, static_cast<size_t>(TestGPUCounter::Count)> m_lastFrame = {};
        uint32_t m_frameCount = 0;
    };

} // anonymous namespace

TEST(GPUPerfCounters_IncrementAndFrame)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::DrawCalls, 150);
    counters.Increment(TestGPUCounter::Primitives, 50000);
    counters.Increment(TestGPUCounter::StateChanges, 30);

    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::DrawCalls), 150u);
    counters.EndFrame();

    EXPECT_EQ(counters.GetLastFrame(TestGPUCounter::DrawCalls), 150u);
    EXPECT_EQ(counters.GetLastFrame(TestGPUCounter::Primitives), 50000u);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::DrawCalls), 0u); // Reset
    EXPECT_EQ(counters.GetFrameCount(), 1u);
}

TEST(GPUPerfCounters_Reset)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::Barriers, 10);
    counters.EndFrame();
    counters.Reset();
    EXPECT_EQ(counters.GetLastFrame(TestGPUCounter::Barriers), 0u);
    EXPECT_EQ(counters.GetFrameCount(), 0u);
}

TEST(GPUPerfCounters_MultipleFrames)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::DrawCalls, 100);
    counters.EndFrame();
    counters.Increment(TestGPUCounter::DrawCalls, 200);
    counters.EndFrame();

    // LastFrame should reflect the most recent completed frame
    EXPECT_EQ(counters.GetLastFrame(TestGPUCounter::DrawCalls), 200u);
    EXPECT_EQ(counters.GetFrameCount(), 2u);
}

TEST(GPUPerfCounters_AllCategories)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::DrawCalls, 50);
    counters.Increment(TestGPUCounter::Primitives, 10000);
    counters.Increment(TestGPUCounter::StateChanges, 25);
    counters.Increment(TestGPUCounter::TextureUploads, 8);
    counters.Increment(TestGPUCounter::Barriers, 12);

    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::DrawCalls), 50u);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::Primitives), 10000u);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::StateChanges), 25u);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::TextureUploads), 8u);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::Barriers), 12u);
}

TEST(GPUPerfCounters_IncrementMultipleTimes)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::DrawCalls, 10);
    counters.Increment(TestGPUCounter::DrawCalls, 20);
    counters.Increment(TestGPUCounter::DrawCalls, 30);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::DrawCalls), 60u);
}

TEST(GPUPerfCounters_CurrentResetAfterEndFrame)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::Primitives, 5000);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::Primitives), 5000u);

    counters.EndFrame();
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::Primitives), 0u);
    EXPECT_EQ(counters.GetLastFrame(TestGPUCounter::Primitives), 5000u);
}

TEST(GPUPerfCounters_InitiallyZero)
{
    TestGPUCounters counters;
    for (int i = 0; i < static_cast<int>(TestGPUCounter::Count); ++i)
    {
        EXPECT_EQ(counters.GetCurrent(static_cast<TestGPUCounter>(i)), 0u);
        EXPECT_EQ(counters.GetLastFrame(static_cast<TestGPUCounter>(i)), 0u);
    }
    EXPECT_EQ(counters.GetFrameCount(), 0u);
}

TEST(GPUPerfCounters_LargeValues)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::Primitives, 1'000'000'000ULL);
    counters.Increment(TestGPUCounter::Primitives, 1'000'000'000ULL);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::Primitives), 2'000'000'000ULL);
}

TEST(GPUPerfCounters_ResetMidFrame)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::DrawCalls, 50);
    counters.Reset();
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::DrawCalls), 0u);
    EXPECT_EQ(counters.GetFrameCount(), 0u);
}
