/** @file TestGraphicsBenchmarkStats.cpp */
#include "TestFramework.h"
#include "Graphics/GraphicsBenchmarkStats.h"

#include <cmath>
#include <limits>

using Spark::Graphics::GraphicsBenchmarkStats;

TEST(GraphicsBenchmarkStats_PercentilesAreInterpolated)
{
    GraphicsBenchmarkStats stats;
    stats.Add(1.0);
    stats.Add(2.0);
    stats.Add(3.0);
    stats.Add(4.0);
    stats.Add(5.0);

    EXPECT_EQ(stats.Count(), size_t(5));
    EXPECT_NEAR(stats.Mean(), 3.0, 0.000001);
    EXPECT_NEAR(stats.Percentile(0.50), 3.0, 0.000001);
    EXPECT_NEAR(stats.Percentile(0.95), 4.8, 0.000001);
    EXPECT_NEAR(stats.Percentile(0.99), 4.96, 0.000001);
}

TEST(GraphicsBenchmarkStats_RejectsInvalidAndBoundsCapacity)
{
    GraphicsBenchmarkStats stats(2);
    stats.Add(-1.0);
    stats.Add(std::numeric_limits<double>::quiet_NaN());
    stats.Add(2.0);
    stats.Add(4.0);
    stats.Add(8.0);

    EXPECT_EQ(stats.Count(), size_t(2));
    EXPECT_TRUE(stats.IsTruncated());
    EXPECT_EQ(stats.Capacity(), size_t(2));
    EXPECT_NEAR(stats.Min(), 2.0, 0.000001);
    EXPECT_NEAR(stats.Max(), 4.0, 0.000001);
}

TEST(GraphicsBenchmarkStats_EmptyReportsNaN)
{
    GraphicsBenchmarkStats stats;
    EXPECT_TRUE(std::isnan(stats.Mean()));
    EXPECT_TRUE(std::isnan(stats.Percentile(0.5)));
}
