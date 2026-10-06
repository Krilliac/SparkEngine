#include "TestFramework.h"
#include "Graphics/VRAMBudgetMonitor.h"

TEST(VRAMTelemetry_LiveQueryWinsWhenNonZero)
{
    const auto reading = SelectVRAMUsage(128 * 1024 * 1024, true, 32 * 1024 * 1024);
    EXPECT_EQ(reading.bytes, static_cast<size_t>(128 * 1024 * 1024));
    EXPECT_TRUE(reading.fromLiveQuery);
    EXPECT_FALSE(reading.liveQueryReportedZero);
}

TEST(VRAMTelemetry_UsesExistingEstimateWhenLiveQueryReportsZero)
{
    const auto reading = SelectVRAMUsage(0, true, 24 * 1024 * 1024);
    EXPECT_EQ(reading.bytes, static_cast<size_t>(24 * 1024 * 1024));
    EXPECT_FALSE(reading.fromLiveQuery);
    EXPECT_TRUE(reading.liveQueryReportedZero);
}

TEST(VRAMTelemetry_UsesEstimateWhenLiveQueryUnavailable)
{
    const auto reading = SelectVRAMUsage(0, false, 16 * 1024 * 1024);
    EXPECT_EQ(reading.bytes, static_cast<size_t>(16 * 1024 * 1024));
    EXPECT_FALSE(reading.fromLiveQuery);
    EXPECT_FALSE(reading.liveQueryReportedZero);
}

TEST(VRAMTelemetry_PreservesGenuineZeroWhenNoEstimateExists)
{
    const auto reading = SelectVRAMUsage(0, true, 0);
    EXPECT_EQ(reading.bytes, static_cast<size_t>(0));
    EXPECT_TRUE(reading.fromLiveQuery);
    EXPECT_FALSE(reading.liveQueryReportedZero);
}

TEST(VRAMTelemetry_FailedLiveQueryUsesEstimateAndIsNotLive)
{
    // IsCurrentUsageValid() is the gate that maps a failed DXGI query to the
    // `liveAvailable == false` input used by SelectVRAMUsage().
    const auto reading = SelectVRAMUsage(0, false, 8 * 1024 * 1024);
    EXPECT_EQ(reading.bytes, static_cast<size_t>(8 * 1024 * 1024));
    EXPECT_FALSE(reading.fromLiveQuery);
    EXPECT_FALSE(reading.liveQueryReportedZero);
}
