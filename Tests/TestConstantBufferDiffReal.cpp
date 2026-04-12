/**
 * @file TestConstantBufferDiffReal.cpp
 * @brief Real-class tests for Spark::Graphics::ConstantBufferDiffManager
 */

#include "TestFramework.h"
#include "Graphics/ConstantBufferDiff.h"

namespace
{
    void ResetCBDiff()
    {
        auto& mgr = Spark::Graphics::ConstantBufferDiffManager::GetInstance();
        mgr.Shutdown();
        mgr.Initialize();
    }
} // namespace

TEST(ConstantBufferDiffReal_SingletonStable)
{
    auto& a = Spark::Graphics::ConstantBufferDiffManager::GetInstance();
    auto& b = Spark::Graphics::ConstantBufferDiffManager::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(ConstantBufferDiffReal_FirstUpdateAlwaysDirty)
{
    ResetCBDiff();
    auto& mgr = Spark::Graphics::ConstantBufferDiffManager::GetInstance();

    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    bool needed = mgr.ShouldUpdate("VS_PerObject", data, sizeof(data));
    EXPECT_TRUE(needed);
}

TEST(ConstantBufferDiffReal_SameDataSkips)
{
    ResetCBDiff();
    auto& mgr = Spark::Graphics::ConstantBufferDiffManager::GetInstance();

    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    mgr.ShouldUpdate("VS_PerObject", data, sizeof(data));
    bool needed = mgr.ShouldUpdate("VS_PerObject", data, sizeof(data));
    EXPECT_FALSE(needed);
}

TEST(ConstantBufferDiffReal_DifferentDataDirty)
{
    ResetCBDiff();
    auto& mgr = Spark::Graphics::ConstantBufferDiffManager::GetInstance();

    float data1[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float data2[4] = {1.0f, 2.0f, 3.0f, 5.0f};
    mgr.ShouldUpdate("VS_PerObject", data1, sizeof(data1));
    bool needed = mgr.ShouldUpdate("VS_PerObject", data2, sizeof(data2));
    EXPECT_TRUE(needed);
}

TEST(ConstantBufferDiffReal_MetricsTracking)
{
    ResetCBDiff();
    auto& mgr = Spark::Graphics::ConstantBufferDiffManager::GetInstance();

    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    mgr.ShouldUpdate("SlotA", data, sizeof(data));
    mgr.ShouldUpdate("SlotA", data, sizeof(data)); // skip
    mgr.ShouldUpdate("SlotA", data, sizeof(data)); // skip

    auto metrics = mgr.GetMetrics();
    EXPECT_EQ(metrics.trackedSlots, size_t(1));
    // totalSkipped is cumulative across the singleton lifetime, so use GE
    EXPECT_GE(metrics.totalSkipped, uint64_t(2));
    EXPECT_GT(metrics.averageSkipRate, 0.0f);
}

TEST(ConstantBufferDiffReal_MultipleSlots)
{
    ResetCBDiff();
    auto& mgr = Spark::Graphics::ConstantBufferDiffManager::GetInstance();

    float dataA[2] = {1.0f, 2.0f};
    float dataB[2] = {3.0f, 4.0f};
    mgr.ShouldUpdate("SlotA", dataA, sizeof(dataA));
    mgr.ShouldUpdate("SlotB", dataB, sizeof(dataB));

    auto metrics = mgr.GetMetrics();
    EXPECT_EQ(metrics.trackedSlots, size_t(2));
}

TEST(ConstantBufferDiffReal_CBSlotCacheSkipRate)
{
    Spark::Graphics::CBSlotCache slot;
    EXPECT_NEAR(slot.GetSkipRate(), 0.0f, 0.001f);

    uint8_t data = 42;
    slot.CacheData(&data, 1);
    slot.skipCount = 3;
    // updateCount=1, skipCount=3 => skipRate = 3/4 = 0.75
    EXPECT_NEAR(slot.GetSkipRate(), 0.75f, 0.01f);
}

TEST(ConstantBufferDiffReal_ConsoleReport)
{
    ResetCBDiff();
    auto& mgr = Spark::Graphics::ConstantBufferDiffManager::GetInstance();
    std::string report = mgr.Console_GetReport();
    EXPECT_STR_CONTAINS(report, "CB Diff");
}

TEST(ConstantBufferDiffReal_SizeChangeIsDirty)
{
    ResetCBDiff();
    auto& mgr = Spark::Graphics::ConstantBufferDiffManager::GetInstance();

    float small[2] = {1.0f, 2.0f};
    float big[4] = {1.0f, 2.0f, 0.0f, 0.0f};
    mgr.ShouldUpdate("Slot", small, sizeof(small));
    bool needed = mgr.ShouldUpdate("Slot", big, sizeof(big));
    EXPECT_TRUE(needed);
}
