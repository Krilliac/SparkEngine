/**
 * @file TestDirtyRectTrackerReal.cpp
 * @brief Real-class tests for Spark::Graphics::DirtyRectTracker
 */

#include "TestFramework.h"
#include "Graphics/DirtyRectTracker.h"

TEST(DirtyRectTrackerReal_InitiallyClean)
{
    Spark::Graphics::DirtyRectTracker tracker;
    EXPECT_FALSE(tracker.IsDirty());
    EXPECT_EQ(tracker.GetRectCount(), size_t(0));
    EXPECT_EQ(tracker.GetTotalDirtyArea(), uint64_t(0));
}

TEST(DirtyRectTrackerReal_AddSingleRect)
{
    Spark::Graphics::DirtyRectTracker tracker;
    tracker.AddDirtyRect({10, 20, 100, 50});
    EXPECT_TRUE(tracker.IsDirty());
    EXPECT_EQ(tracker.GetRectCount(), size_t(1));
    EXPECT_EQ(tracker.GetTotalDirtyArea(), uint64_t(5000));
}

TEST(DirtyRectTrackerReal_ZeroSizeIgnored)
{
    Spark::Graphics::DirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 0, 0});
    EXPECT_FALSE(tracker.IsDirty());
    tracker.AddDirtyRect({5, 5, 100, 0});
    EXPECT_FALSE(tracker.IsDirty());
}

TEST(DirtyRectTrackerReal_ClearResets)
{
    Spark::Graphics::DirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 64, 64});
    EXPECT_TRUE(tracker.IsDirty());
    tracker.Clear();
    EXPECT_FALSE(tracker.IsDirty());
    EXPECT_EQ(tracker.GetRectCount(), size_t(0));
    EXPECT_EQ(tracker.GetTotalDirtyArea(), uint64_t(0));
}

TEST(DirtyRectTrackerReal_OverlappingRectsMerge)
{
    Spark::Graphics::DirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 50, 50});
    tracker.AddDirtyRect({25, 25, 50, 50});
    // Overlapping => merged into single rect
    EXPECT_EQ(tracker.GetRectCount(), size_t(1));
}

TEST(DirtyRectTrackerReal_NonOverlappingSeparate)
{
    Spark::Graphics::DirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 10, 10});
    tracker.AddDirtyRect({100, 100, 10, 10});
    EXPECT_EQ(tracker.GetRectCount(), size_t(2));
}

TEST(DirtyRectTrackerReal_ContainedRectSkipped)
{
    Spark::Graphics::DirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 100, 100});
    tracker.AddDirtyRect({10, 10, 20, 20}); // fully contained
    EXPECT_EQ(tracker.GetRectCount(), size_t(1));
}

TEST(DirtyRectTrackerReal_InvalidateAll)
{
    Spark::Graphics::DirtyRectTracker tracker;
    tracker.AddDirtyRect({10, 10, 5, 5});
    tracker.InvalidateAll(1024, 768);
    EXPECT_EQ(tracker.GetRectCount(), size_t(1));
    EXPECT_EQ(tracker.GetTotalDirtyArea(), uint64_t(1024 * 768));
}

TEST(DirtyRectTrackerReal_ShouldFullUpdate)
{
    Spark::Graphics::DirtyRectTracker tracker;
    // Dirty area > half of total => should full update
    tracker.InvalidateAll(100, 100);
    EXPECT_TRUE(tracker.ShouldFullUpdate(100, 100));

    tracker.Clear();
    tracker.AddDirtyRect({0, 0, 1, 1});
    EXPECT_FALSE(tracker.ShouldFullUpdate(1000, 1000));
}

TEST(DirtyRectTrackerReal_DirtyRectOverlaps)
{
    Spark::Graphics::DirtyRect a{0, 0, 10, 10};
    Spark::Graphics::DirtyRect b{5, 5, 10, 10};
    Spark::Graphics::DirtyRect c{20, 20, 5, 5};
    EXPECT_TRUE(a.Overlaps(b));
    EXPECT_TRUE(b.Overlaps(a));
    EXPECT_FALSE(a.Overlaps(c));
}

TEST(DirtyRectTrackerReal_DirtyRectUnion)
{
    Spark::Graphics::DirtyRect a{10, 20, 30, 40};
    Spark::Graphics::DirtyRect b{5, 25, 50, 10};
    auto u = a.Union(b);
    EXPECT_EQ(u.x, uint32_t(5));
    EXPECT_EQ(u.y, uint32_t(20));
    EXPECT_EQ(u.x + u.width, uint32_t(55));
    EXPECT_EQ(u.y + u.height, uint32_t(60));
}

TEST(DirtyRectTrackerReal_CoalesceOnOverflow)
{
    Spark::Graphics::DirtyRectTracker tracker;
    // Add more than MAX_RECTS non-overlapping rects to trigger coalesce
    for (uint32_t i = 0; i <= Spark::Graphics::DirtyRectTracker::MAX_RECTS; ++i)
    {
        tracker.AddDirtyRect({i * 100, 0, 5, 5});
    }
    // After coalesce, should be merged into 1
    EXPECT_LE(tracker.GetRectCount(), size_t(Spark::Graphics::DirtyRectTracker::MAX_RECTS));
}
