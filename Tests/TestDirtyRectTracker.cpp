/**
 * @file TestDirtyRectTracker.cpp
 * @brief Tests for dirty rectangle tracking for partial texture updates
 */

#include "TestFramework.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{

    struct TestDirtyRect
    {
        uint32_t x, y, width, height;

        bool Overlaps(const TestDirtyRect& other) const
        {
            return x < other.x + other.width && x + width > other.x && y < other.y + other.height &&
                   y + height > other.y;
        }

        bool Contains(const TestDirtyRect& other) const
        {
            return x <= other.x && y <= other.y && x + width >= other.x + other.width &&
                   y + height >= other.y + other.height;
        }

        TestDirtyRect Union(const TestDirtyRect& other) const
        {
            uint32_t minX = std::min(x, other.x);
            uint32_t minY = std::min(y, other.y);
            uint32_t maxX = std::max(x + width, other.x + other.width);
            uint32_t maxY = std::max(y + height, other.y + other.height);
            return {minX, minY, maxX - minX, maxY - minY};
        }
    };

    class TestDirtyRectTracker
    {
      public:
        void AddDirtyRect(const TestDirtyRect& rect)
        {
            if (rect.width == 0 || rect.height == 0)
                return;
            for (auto& existing : m_rects)
            {
                if (existing.Contains(rect))
                    return;
                if (existing.Overlaps(rect))
                {
                    existing = existing.Union(rect);
                    return;
                }
            }
            m_rects.push_back(rect);
        }

        void Clear() { m_rects.clear(); }
        bool IsDirty() const { return !m_rects.empty(); }
        size_t GetRectCount() const { return m_rects.size(); }
        const std::vector<TestDirtyRect>& GetRects() const { return m_rects; }

      private:
        std::vector<TestDirtyRect> m_rects;
    };

} // anonymous namespace

TEST(DirtyRect_AddAndClear)
{
    TestDirtyRectTracker tracker;
    EXPECT_FALSE(tracker.IsDirty());

    tracker.AddDirtyRect({10, 10, 50, 50});
    EXPECT_TRUE(tracker.IsDirty());
    EXPECT_EQ(tracker.GetRectCount(), 1u);

    tracker.Clear();
    EXPECT_FALSE(tracker.IsDirty());
}

TEST(DirtyRect_MergeOverlapping)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 100, 100});
    tracker.AddDirtyRect({50, 50, 100, 100}); // Overlapping
    EXPECT_EQ(tracker.GetRectCount(), 1u);

    auto& merged = tracker.GetRects()[0];
    EXPECT_EQ(merged.x, 0u);
    EXPECT_EQ(merged.y, 0u);
    EXPECT_EQ(merged.width, 150u);
    EXPECT_EQ(merged.height, 150u);
}

TEST(DirtyRect_NonOverlappingSeparate)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 10, 10});
    tracker.AddDirtyRect({100, 100, 10, 10}); // Far away
    EXPECT_EQ(tracker.GetRectCount(), 2u);
}

TEST(DirtyRect_ContainedSkipped)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 100, 100});
    tracker.AddDirtyRect({10, 10, 20, 20}); // Fully contained
    EXPECT_EQ(tracker.GetRectCount(), 1u);
}

TEST(DirtyRect_ZeroSizeIgnored)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({10, 10, 0, 50}); // Zero width
    tracker.AddDirtyRect({10, 10, 50, 0}); // Zero height
    EXPECT_FALSE(tracker.IsDirty());
    EXPECT_EQ(tracker.GetRectCount(), 0u);
}

TEST(DirtyRect_AdjacentNotMerged)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 10, 10});
    tracker.AddDirtyRect({10, 0, 10, 10}); // Adjacent, not overlapping
    EXPECT_EQ(tracker.GetRectCount(), 2u);
}

TEST(DirtyRect_IdenticalRect)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({5, 5, 20, 20});
    tracker.AddDirtyRect({5, 5, 20, 20}); // Identical = contained
    EXPECT_EQ(tracker.GetRectCount(), 1u);
}

TEST(DirtyRect_ClearAndReuse)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 50, 50});
    EXPECT_TRUE(tracker.IsDirty());

    tracker.Clear();
    EXPECT_FALSE(tracker.IsDirty());

    tracker.AddDirtyRect({100, 100, 10, 10});
    EXPECT_EQ(tracker.GetRectCount(), 1u);
    EXPECT_EQ(tracker.GetRects()[0].x, 100u);
}

TEST(DirtyRect_ManyNonOverlapping)
{
    TestDirtyRectTracker tracker;
    for (uint32_t i = 0; i < 10; ++i)
        tracker.AddDirtyRect({i * 100, 0, 10, 10});
    EXPECT_EQ(tracker.GetRectCount(), 10u);
}
