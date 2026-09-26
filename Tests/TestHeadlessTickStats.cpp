/**
 * @file TestHeadlessTickStats.cpp
 * @brief PERF-100: bounded tick work-time histogram used by the headless hosts.
 *
 * Drives the production Spark::HeadlessTickStats (Core/HeadlessTickStats.h)
 * that RunHeadlessLinux/RunHeadlessWindows feed every tick. Pins the bucket
 * layout, the nearest-rank percentile contract (never under-reports, never
 * exceeds the observed max), saturation of out-of-range samples, and the
 * /proc/self/status VmHWM parser the Linux host uses for its own peak RSS.
 */

#include "TestFramework.h"

#include "Core/HeadlessTickStats.h"

#include <cstdint>
#include <string_view>

using Spark::HeadlessTickStats;

TEST(HeadlessTickStats_EmptyReportsZero)
{
    HeadlessTickStats stats;
    EXPECT_EQ(stats.Count(), 0u);
    EXPECT_EQ(stats.MaxUs(), 0u);
    EXPECT_EQ(stats.PercentileUs(50.0), 0u);
    EXPECT_EQ(stats.PercentileUs(99.0), 0u);
}

TEST(HeadlessTickStats_ExactRangeNearestRank)
{
    HeadlessTickStats stats;
    for (uint64_t value = 1; value <= 100; ++value)
        stats.Record(value);

    EXPECT_EQ(stats.Count(), 100u);
    EXPECT_EQ(stats.MaxUs(), 100u);
    // Nearest rank on 1..100: p50 is the 50th sample, p99 the 99th.
    EXPECT_EQ(stats.PercentileUs(50.0), 50u);
    EXPECT_EQ(stats.PercentileUs(99.0), 99u);
    EXPECT_EQ(stats.PercentileUs(100.0), 100u);
    EXPECT_EQ(stats.PercentileUs(0.0), 1u);
}

TEST(HeadlessTickStats_BucketBoundsCoverEveryValue)
{
    // Every value maps to a bucket whose upper bound is >= the value and whose
    // predecessor's upper bound is < the value (buckets are contiguous).
    for (uint64_t value = 0; value < 300000; value += (value < 4096 ? 1 : 97))
    {
        const size_t index = HeadlessTickStats::BucketIndex(value);
        ASSERT_TRUE(index < HeadlessTickStats::BUCKET_COUNT);
        EXPECT_TRUE(HeadlessTickStats::BucketUpperBound(index) >= value);
        if (index > 0)
            EXPECT_TRUE(HeadlessTickStats::BucketUpperBound(index - 1) < value);
    }
    // Log-linear buckets are narrower than 1/64 of their lower bound.
    EXPECT_EQ(HeadlessTickStats::BucketUpperBound(HeadlessTickStats::BucketIndex(1000)), 1007u);
}

TEST(HeadlessTickStats_PercentileNeverUnderReportsOrExceedsMax)
{
    HeadlessTickStats stats;
    for (int i = 0; i < 98; ++i)
        stats.Record(250);
    stats.Record(5000);
    stats.Record(12345);

    const uint64_t p50 = stats.PercentileUs(50.0);
    const uint64_t p99 = stats.PercentileUs(99.0);
    EXPECT_TRUE(p50 >= 250u && p50 < 254u);
    EXPECT_TRUE(p99 >= 5000u && p99 < 5000u + 5000u / 64u + 1u);
    EXPECT_EQ(stats.PercentileUs(100.0), 12345u);
    EXPECT_EQ(stats.MaxUs(), 12345u);
    EXPECT_TRUE(p50 <= p99 && p99 <= stats.MaxUs());
}

TEST(HeadlessTickStats_SaturatesOutOfRangeSamples)
{
    HeadlessTickStats stats;
    stats.Record(UINT64_MAX);
    EXPECT_EQ(stats.Count(), 1u);
    EXPECT_EQ(stats.MaxUs(), HeadlessTickStats::MAX_TRACKED_US);
    EXPECT_EQ(stats.PercentileUs(99.0), HeadlessTickStats::MAX_TRACKED_US);
}

TEST(HeadlessTickStats_ParseVmHwmKibExtractsPeak)
{
    constexpr std::string_view status = "Name:\tSparkEngine\nVmPeak:\t  900000 kB\nVmHWM:\t   28672 kB\n"
                                        "VmRSS:\t   20480 kB\n";
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib(status), 28672u);
    // Final line without a trailing newline.
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib("VmHWM:\t12 kB"), 12u);
    static_assert(HeadlessTickStats::ParseVmHwmKib("VmHWM: 7 kB\n") == 7u);
}

TEST(HeadlessTickStats_ParseVmHwmKibRejectsMissingOrMalformed)
{
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib(""), 0u);
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib("VmRSS:\t20480 kB\n"), 0u);
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib("VmHWM:\t kB\n"), 0u);
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib("VmHWM:\t28672 MB\n"), 0u);
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib("VmHWM:\t28672\n"), 0u);
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib("VmHWM:\t-1 kB\n"), 0u);
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib("VmHWM:\t12345678901234567 kB\n"), 0u);
    // A key merely containing the name is not the VmHWM line.
    EXPECT_EQ(HeadlessTickStats::ParseVmHwmKib("XVmHWM:\t5 kB\n"), 0u);
}
