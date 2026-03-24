// TestNetworkInterpolation.cpp - Tests for network entity interpolation buffer
// Standalone implementations for CI testing (no DirectXMath dependency)

#include "TestFramework.h"
#include "TestCommonMath.h"
#include <cmath>
#include <algorithm>
#include <array>

namespace TestNetInterp
{

    using TestMath::Vec3;

    Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
    {
        return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
    }

    struct Snapshot
    {
        Vec3 position;
        float timestamp = 0.0f;
    };

    // ============================================================================
    // Simulated NetworkInterpolationBuffer
    // ============================================================================

    class TestInterpolationBuffer
    {
      public:
        static constexpr size_t MAX_SNAPSHOTS = 32;

        void AddSnapshot(const Snapshot& snapshot)
        {
            m_snapshots[m_writeIndex] = snapshot;
            m_writeIndex = (m_writeIndex + 1) % MAX_SNAPSHOTS;
            if (m_count < MAX_SNAPSHOTS)
            {
                m_count++;
            }
        }

        Snapshot Sample(float currentTime) const
        {
            if (m_count == 0)
            {
                return Snapshot{};
            }
            if (m_count == 1)
            {
                return GetSnapshot(0);
            }

            float renderTime = currentTime - m_interpolationDelay;

            const auto& oldest = GetSnapshot(0);
            const auto& newest = GetSnapshot(m_count - 1);

            if (renderTime <= oldest.timestamp)
            {
                return oldest;
            }
            if (renderTime >= newest.timestamp)
            {
                return newest;
            }

            for (size_t i = 0; i < m_count - 1; ++i)
            {
                const auto& a = GetSnapshot(i);
                const auto& b = GetSnapshot(i + 1);

                if (renderTime >= a.timestamp && renderTime <= b.timestamp)
                {
                    float range = b.timestamp - a.timestamp;
                    float t = (range > 0.0001f) ? (renderTime - a.timestamp) / range : 0.0f;

                    Snapshot result;
                    result.timestamp = renderTime;
                    result.position = Lerp(a.position, b.position, t);
                    return result;
                }
            }

            return newest;
        }

        void SetInterpolationDelay(float seconds) { m_interpolationDelay = std::clamp(seconds, 0.0f, 1.0f); }
        float GetInterpolationDelay() const { return m_interpolationDelay; }
        void Clear()
        {
            m_count = 0;
            m_writeIndex = 0;
        }
        size_t GetSnapshotCount() const { return m_count; }

      private:
        std::array<Snapshot, MAX_SNAPSHOTS> m_snapshots{};
        size_t m_count = 0;
        size_t m_writeIndex = 0;
        float m_interpolationDelay = 0.1f;

        const Snapshot& GetSnapshot(size_t logicalIndex) const
        {
            size_t startIndex = (m_count == MAX_SNAPSHOTS) ? m_writeIndex : 0;
            size_t actualIndex = (startIndex + logicalIndex) % MAX_SNAPSHOTS;
            return m_snapshots[actualIndex];
        }
    };

} // namespace TestNetInterp

// ============================================================================
// Tests
// ============================================================================

TEST(NetInterp_EmptyBuffer_ReturnsDefault)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    auto result = buffer.Sample(1.0f);
    EXPECT_NEAR(result.position.x, 0.0f, 0.001f);
    EXPECT_NEAR(result.position.y, 0.0f, 0.001f);
    EXPECT_NEAR(result.position.z, 0.0f, 0.001f);
}

TEST(NetInterp_SingleSnapshot_ReturnsThatSnapshot)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.AddSnapshot({{5, 10, 15}, 1.0f});

    auto result = buffer.Sample(1.5f);
    EXPECT_NEAR(result.position.x, 5.0f, 0.001f);
    EXPECT_NEAR(result.position.y, 10.0f, 0.001f);
    EXPECT_NEAR(result.position.z, 15.0f, 0.001f);
}

TEST(NetInterp_TwoSnapshots_InterpolatesMidpoint)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.SetInterpolationDelay(0.0f); // No delay for testing

    buffer.AddSnapshot({{0, 0, 0}, 0.0f});
    buffer.AddSnapshot({{10, 20, 30}, 1.0f});

    auto result = buffer.Sample(0.5f);
    EXPECT_NEAR(result.position.x, 5.0f, 0.001f);
    EXPECT_NEAR(result.position.y, 10.0f, 0.001f);
    EXPECT_NEAR(result.position.z, 15.0f, 0.001f);
}

TEST(NetInterp_TwoSnapshots_InterpolatesQuarter)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.SetInterpolationDelay(0.0f);

    buffer.AddSnapshot({{0, 0, 0}, 0.0f});
    buffer.AddSnapshot({{100, 0, 0}, 1.0f});

    auto result = buffer.Sample(0.25f);
    EXPECT_NEAR(result.position.x, 25.0f, 0.001f);
}

TEST(NetInterp_BeforeFirstSnapshot_ReturnsOldest)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.SetInterpolationDelay(0.0f);

    buffer.AddSnapshot({{5, 0, 0}, 1.0f});
    buffer.AddSnapshot({{10, 0, 0}, 2.0f});

    auto result = buffer.Sample(0.5f);
    EXPECT_NEAR(result.position.x, 5.0f, 0.001f);
}

TEST(NetInterp_AfterLastSnapshot_ReturnsNewest)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.SetInterpolationDelay(0.0f);

    buffer.AddSnapshot({{5, 0, 0}, 1.0f});
    buffer.AddSnapshot({{10, 0, 0}, 2.0f});

    auto result = buffer.Sample(5.0f);
    EXPECT_NEAR(result.position.x, 10.0f, 0.001f);
}

TEST(NetInterp_InterpolationDelay_OffsetsRenderTime)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.SetInterpolationDelay(0.5f);

    buffer.AddSnapshot({{0, 0, 0}, 0.0f});
    buffer.AddSnapshot({{10, 0, 0}, 1.0f});

    // currentTime=1.0, delay=0.5 -> renderTime=0.5 -> midpoint
    auto result = buffer.Sample(1.0f);
    EXPECT_NEAR(result.position.x, 5.0f, 0.001f);
}

TEST(NetInterp_CircularBuffer_WrapsCorrectly)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.SetInterpolationDelay(0.0f);

    // Fill buffer beyond capacity (32 max)
    for (int i = 0; i < 40; ++i)
    {
        buffer.AddSnapshot({{static_cast<float>(i), 0, 0}, static_cast<float>(i)});
    }

    EXPECT_EQ(buffer.GetSnapshotCount(), static_cast<size_t>(32));

    // Oldest should now be snapshot 8 (40 - 32)
    auto resultOldest = buffer.Sample(8.0f);
    EXPECT_NEAR(resultOldest.position.x, 8.0f, 0.001f);

    // Newest should be snapshot 39
    auto resultNewest = buffer.Sample(50.0f);
    EXPECT_NEAR(resultNewest.position.x, 39.0f, 0.001f);
}

TEST(NetInterp_MultipleSnapshots_InterpolatesBetweenCorrectPair)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.SetInterpolationDelay(0.0f);

    buffer.AddSnapshot({{0, 0, 0}, 0.0f});
    buffer.AddSnapshot({{10, 0, 0}, 1.0f});
    buffer.AddSnapshot({{30, 0, 0}, 2.0f});

    // Sample at 1.5 should interpolate between snapshots at t=1 (10) and t=2 (30)
    auto result = buffer.Sample(1.5f);
    EXPECT_NEAR(result.position.x, 20.0f, 0.001f);
}

TEST(NetInterp_Clear_ResetsBuffer)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.AddSnapshot({{5, 0, 0}, 1.0f});
    EXPECT_EQ(buffer.GetSnapshotCount(), static_cast<size_t>(1));

    buffer.Clear();
    EXPECT_EQ(buffer.GetSnapshotCount(), static_cast<size_t>(0));

    auto result = buffer.Sample(1.0f);
    EXPECT_NEAR(result.position.x, 0.0f, 0.001f);
}

TEST(NetInterp_SetInterpolationDelay_Clamps)
{
    TestNetInterp::TestInterpolationBuffer buffer;

    buffer.SetInterpolationDelay(-1.0f);
    EXPECT_NEAR(buffer.GetInterpolationDelay(), 0.0f, 0.001f);

    buffer.SetInterpolationDelay(5.0f);
    EXPECT_NEAR(buffer.GetInterpolationDelay(), 1.0f, 0.001f);

    buffer.SetInterpolationDelay(0.2f);
    EXPECT_NEAR(buffer.GetInterpolationDelay(), 0.2f, 0.001f);
}

TEST(NetInterp_SameTimestamp_ReturnsFirst)
{
    TestNetInterp::TestInterpolationBuffer buffer;
    buffer.SetInterpolationDelay(0.0f);

    buffer.AddSnapshot({{5, 0, 0}, 1.0f});
    buffer.AddSnapshot({{10, 0, 0}, 1.0f});

    auto result = buffer.Sample(1.0f);
    // With zero range, t=0, should return first snapshot's position
    EXPECT_NEAR(result.position.x, 5.0f, 0.001f);
}
