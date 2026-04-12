/**
 * @file TestCacheDebuggerPhaseFF.cpp
 * @brief Phase FF Theme 3D tests for Spark::CacheDebugger
 */

#include "TestFramework.h"
#include "Utils/CacheDebugger.h"

namespace
{
    void ResetCacheDebugger()
    {
        Spark::CacheDebugger::GetInstance().Reset();
        Spark::CacheDebugger::GetInstance().SetEnabled(true);
    }
} // namespace

TEST(CacheDebuggerPhaseFF_SingletonStable)
{
    auto& a = Spark::CacheDebugger::GetInstance();
    auto& b = Spark::CacheDebugger::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(CacheDebuggerPhaseFF_EnabledByDefault)
{
    ResetCacheDebugger();
    auto& cd = Spark::CacheDebugger::GetInstance();
    EXPECT_TRUE(cd.IsEnabled());
}

TEST(CacheDebuggerPhaseFF_SetEnabledToggle)
{
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.SetEnabled(false);
    EXPECT_FALSE(cd.IsEnabled());
    cd.SetEnabled(true);
    EXPECT_TRUE(cd.IsEnabled());
}

TEST(CacheDebuggerPhaseFF_RegisterCache)
{
    ResetCacheDebugger();
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.RegisterCache("PhaseFF_Cache", 100, 1024 * 1024);
    // Registering again is a no-op / update; should not crash.
    cd.RegisterCache("PhaseFF_Cache", 200, 2 * 1024 * 1024);
    EXPECT_TRUE(cd.IsEnabled());
}

TEST(CacheDebuggerPhaseFF_RecordHitsAndMisses)
{
    ResetCacheDebugger();
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.RegisterCache("PhaseFF_HitMiss", 0, 0);

    for (int i = 0; i < 8; ++i)
        cd.RecordHit("PhaseFF_HitMiss");
    for (int i = 0; i < 2; ++i)
        cd.RecordMiss("PhaseFF_HitMiss");

    auto stats = cd.GetCacheStats("PhaseFF_HitMiss");
    EXPECT_EQ(stats.hits, static_cast<uint64_t>(8));
    EXPECT_EQ(stats.misses, static_cast<uint64_t>(2));
    EXPECT_NEAR(stats.hitRatePercent, 80.0f, 0.5f);
}

TEST(CacheDebuggerPhaseFF_RecordInsertionAndEviction)
{
    ResetCacheDebugger();
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.RegisterCache("PhaseFF_InsEv", 0, 0);

    cd.RecordInsertion("PhaseFF_InsEv", nullptr, 1024);
    cd.RecordInsertion("PhaseFF_InsEv", nullptr, 512);
    cd.RecordEviction("PhaseFF_InsEv", nullptr, 1024);

    auto stats = cd.GetCacheStats("PhaseFF_InsEv");
    EXPECT_EQ(stats.insertions, static_cast<uint64_t>(2));
    EXPECT_EQ(stats.evictions, static_cast<uint64_t>(1));
}

TEST(CacheDebuggerPhaseFF_GetCacheStatsUnknownReturnsEmpty)
{
    ResetCacheDebugger();
    auto& cd = Spark::CacheDebugger::GetInstance();
    // Unknown caches return a default-constructed CacheStats (empty
    // cacheName, zero hits/misses).
    auto stats = cd.GetCacheStats("NonexistentPhaseFF");
    EXPECT_EQ(stats.hits, static_cast<uint64_t>(0));
    EXPECT_EQ(stats.misses, static_cast<uint64_t>(0));
    EXPECT_TRUE(stats.cacheName.empty());
}

TEST(CacheDebuggerPhaseFF_ResetClearsAll)
{
    ResetCacheDebugger();
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.RegisterCache("PhaseFF_ToClear", 0, 0);
    cd.RecordHit("PhaseFF_ToClear");
    cd.RecordHit("PhaseFF_ToClear");

    cd.Reset();
    auto stats = cd.GetCacheStats("PhaseFF_ToClear");
    // Reset wipes the map entirely; GetCacheStats now returns an
    // empty default-constructed struct.
    EXPECT_EQ(stats.hits, static_cast<uint64_t>(0));
    EXPECT_TRUE(stats.cacheName.empty());
}

TEST(CacheDebuggerPhaseFF_RecordWhenDisabledIsNoOp)
{
    ResetCacheDebugger();
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.RegisterCache("PhaseFF_DisabledRec", 0, 0);
    cd.RecordHit("PhaseFF_DisabledRec"); // One baseline hit.
    cd.SetEnabled(false);
    cd.RecordHit("PhaseFF_DisabledRec");
    cd.RecordMiss("PhaseFF_DisabledRec");
    cd.SetEnabled(true);

    auto stats = cd.GetCacheStats("PhaseFF_DisabledRec");
    // Only the baseline hit should have been recorded.
    EXPECT_EQ(stats.hits, static_cast<uint64_t>(1));
    EXPECT_EQ(stats.misses, static_cast<uint64_t>(0));
}
