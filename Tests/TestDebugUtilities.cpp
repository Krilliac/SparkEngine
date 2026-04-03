/**
 * @file TestDebugUtilities.cpp
 * @brief Unit tests for CpuDebugger, CacheDebugger, IODebugger, and ThreadDebugger
 *
 * Tests follow the same pattern as TestDebugTools.cpp:
 * GetInstance() -> Reset() -> SetEnabled(true) -> exercise -> assert -> cleanup
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/CpuDebugger.h"
#include "../SparkEngine/Source/Utils/CacheDebugger.h"
#include "../SparkEngine/Source/Utils/IODebugger.h"
#include "../SparkEngine/Source/Utils/ThreadDebugger.h"

#include <chrono>
#include <thread>

// ============================================================================
// CpuDebugger Tests
// ============================================================================

TEST(CpuDebugger_BasicTiming)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.BeginSection("TestSection", "General", __FILE__, __LINE__, __FUNCTION__);
    // Small busy-wait to ensure measurable time
    volatile unsigned int x = 0;
    for (unsigned int i = 0; i < 10000; ++i)
        x += i;
    cd.EndSection("TestSection");

    auto stats = cd.GetSectionStats("TestSection");
    EXPECT_EQ(stats.hitCount, static_cast<uint64_t>(1));
    EXPECT_GE(stats.totalTimeMs, 0.0f);
    EXPECT_EQ(stats.name, std::string("TestSection"));
}

TEST(CpuDebugger_SectionStats)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    // Run section multiple times
    for (int i = 0; i < 10; ++i)
    {
        cd.BeginSection("Repeated", "Test");
        volatile unsigned int x = 0;
        for (unsigned int j = 0; j < 1000; ++j)
            x += j;
        cd.EndSection("Repeated");
    }

    auto stats = cd.GetSectionStats("Repeated");
    EXPECT_EQ(stats.hitCount, static_cast<uint64_t>(10));
    EXPECT_GE(stats.totalTimeMs, 0.0f);
    EXPECT_GE(stats.maxTimeMs, stats.minTimeMs);
    EXPECT_GE(stats.avgTimeMs, 0.0f);
}

TEST(CpuDebugger_HotSpots)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    // Create two sections with different call sites
    cd.BeginSection("FastSection", "General", "fileA.cpp", 10, "funcA");
    cd.EndSection("FastSection");

    cd.BeginSection("SlowSection", "General", "fileB.cpp", 20, "funcB");
    volatile unsigned int x = 0;
    for (unsigned int i = 0; i < 100000; ++i)
        x += i;
    cd.EndSection("SlowSection");

    auto spots = cd.GetHotSpots(10);
    EXPECT_GE(spots.size(), static_cast<size_t>(2));
}

TEST(CpuDebugger_ScopedSection)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    {
        Spark::ScopedCpuSection section("ScopedTest", "General", __FILE__, __LINE__, __FUNCTION__);
        volatile unsigned int x = 0;
        for (unsigned int i = 0; i < 1000; ++i)
            x += i;
    }

    auto stats = cd.GetSectionStats("ScopedTest");
    EXPECT_EQ(stats.hitCount, static_cast<uint64_t>(1));
    EXPECT_GE(stats.totalTimeMs, 0.0f);
}

TEST(CpuDebugger_CategoryFiltering)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.BeginSection("RenderSection", "Rendering");
    cd.EndSection("RenderSection");

    cd.BeginSection("PhysicsSection", "Physics");
    cd.EndSection("PhysicsSection");

    auto all = cd.GetAllStats();
    EXPECT_EQ(all.size(), static_cast<size_t>(2));

    bool foundRendering = false;
    bool foundPhysics = false;
    for (const auto& s : all)
    {
        if (s.category == "Rendering")
            foundRendering = true;
        if (s.category == "Physics")
            foundPhysics = true;
    }
    EXPECT_TRUE(foundRendering);
    EXPECT_TRUE(foundPhysics);
}

TEST(CpuDebugger_P99Calculation)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    // Run 100 samples to populate the ring buffer
    for (int i = 0; i < 100; ++i)
    {
        cd.BeginSection("P99Test", "Test");
        volatile unsigned int x = 0;
        for (unsigned int j = 0; j < 1000; ++j)
            x += j;
        cd.EndSection("P99Test");
    }

    auto stats = cd.GetSectionStats("P99Test");
    EXPECT_EQ(stats.hitCount, static_cast<uint64_t>(100));
    EXPECT_EQ(stats.recentSampleCount, static_cast<uint32_t>(100));
    // P99 should be between min and max (or equal to max for small sample sizes)
    EXPECT_GE(stats.p99TimeMs, stats.minTimeMs);
    EXPECT_LE(stats.p99TimeMs, stats.maxTimeMs);
}

TEST(CpuDebugger_Reset)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.SetEnabled(true);

    cd.BeginSection("WillBeCleared");
    cd.EndSection("WillBeCleared");
    EXPECT_GE(cd.GetSectionCount(), static_cast<size_t>(1));

    cd.Reset();
    EXPECT_EQ(cd.GetSectionCount(), static_cast<size_t>(0));
    EXPECT_EQ(cd.GetTotalTimeMs(), 0.0f);
}

TEST(CpuDebugger_DisabledNoTracking)
{
    auto& cd = Spark::CpuDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(false);

    cd.BeginSection("ShouldNotTrack");
    cd.EndSection("ShouldNotTrack");

    EXPECT_EQ(cd.GetSectionCount(), static_cast<size_t>(0));
    cd.SetEnabled(true);
}

// ============================================================================
// CacheDebugger Tests
// ============================================================================

TEST(CacheDebugger_RegisterCache)
{
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.RegisterCache("TextureCache", 1024, 64 * 1024 * 1024);
    cd.RegisterCache("ShaderCache", 256, 0);

    auto all = cd.GetAllCacheStats();
    EXPECT_EQ(all.size(), static_cast<size_t>(2));

    auto stats = cd.GetCacheStats("TextureCache");
    EXPECT_EQ(stats.cacheName, std::string("TextureCache"));
    EXPECT_EQ(stats.maxEntries, static_cast<size_t>(1024));
}

TEST(CacheDebugger_HitMissTracking)
{
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.RegisterCache("TestCache");

    cd.RecordHit("TestCache");
    cd.RecordHit("TestCache");
    cd.RecordHit("TestCache");
    cd.RecordMiss("TestCache");

    auto stats = cd.GetCacheStats("TestCache");
    EXPECT_EQ(stats.hits, static_cast<uint64_t>(3));
    EXPECT_EQ(stats.misses, static_cast<uint64_t>(1));
    EXPECT_NEAR(stats.hitRatePercent, 75.0f, 0.1f);
}

TEST(CacheDebugger_EvictionTracking)
{
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.RegisterCache("EvictCache");

    cd.RecordInsertion("EvictCache", "key1", 1024);
    cd.RecordInsertion("EvictCache", "key2", 2048);
    EXPECT_EQ(cd.GetCacheStats("EvictCache").currentEntries, static_cast<size_t>(2));
    EXPECT_EQ(cd.GetCacheStats("EvictCache").currentSizeBytes, static_cast<size_t>(3072));

    cd.RecordEviction("EvictCache", "key1", 1024);
    EXPECT_EQ(cd.GetCacheStats("EvictCache").evictions, static_cast<uint64_t>(1));
    EXPECT_EQ(cd.GetCacheStats("EvictCache").currentEntries, static_cast<size_t>(1));
    EXPECT_EQ(cd.GetCacheStats("EvictCache").currentSizeBytes, static_cast<size_t>(2048));
}

TEST(CacheDebugger_InefficientCaches)
{
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.RegisterCache("GoodCache");
    cd.RegisterCache("BadCache");

    // Good cache: 90% hit rate
    for (int i = 0; i < 9; ++i)
        cd.RecordHit("GoodCache");
    cd.RecordMiss("GoodCache");

    // Bad cache: 20% hit rate
    cd.RecordHit("BadCache");
    for (int i = 0; i < 4; ++i)
        cd.RecordMiss("BadCache");

    auto inefficient = cd.GetInefficientCaches(50.0f);
    EXPECT_EQ(inefficient.size(), static_cast<size_t>(1));
    EXPECT_EQ(inefficient[0], std::string("BadCache"));
}

TEST(CacheDebugger_MultipleCaches)
{
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.RegisterCache("CacheA");
    cd.RegisterCache("CacheB");
    cd.RegisterCache("CacheC");

    cd.RecordHit("CacheA");
    cd.RecordMiss("CacheB");
    cd.RecordHit("CacheC");

    EXPECT_EQ(cd.GetCacheCount(), static_cast<size_t>(3));
    EXPECT_EQ(cd.GetCacheStats("CacheA").hits, static_cast<uint64_t>(1));
    EXPECT_EQ(cd.GetCacheStats("CacheB").misses, static_cast<uint64_t>(1));
    EXPECT_EQ(cd.GetCacheStats("CacheC").hits, static_cast<uint64_t>(1));
}

TEST(CacheDebugger_OverallHitRate)
{
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.RegisterCache("CacheX");
    cd.RegisterCache("CacheY");

    // CacheX: 2 hits, 0 misses
    cd.RecordHit("CacheX");
    cd.RecordHit("CacheX");

    // CacheY: 1 hit, 1 miss
    cd.RecordHit("CacheY");
    cd.RecordMiss("CacheY");

    // Overall: 3 hits, 1 miss = 75%
    EXPECT_NEAR(cd.GetOverallHitRate(), 75.0f, 0.1f);
}

TEST(CacheDebugger_Reset)
{
    auto& cd = Spark::CacheDebugger::GetInstance();
    cd.Reset();
    cd.SetEnabled(true);

    cd.RegisterCache("WillBeCleared");
    cd.RecordHit("WillBeCleared");
    EXPECT_EQ(cd.GetCacheCount(), static_cast<size_t>(1));

    cd.Reset();
    EXPECT_EQ(cd.GetCacheCount(), static_cast<size_t>(0));
}

// ============================================================================
// IODebugger Tests
// ============================================================================

TEST(IODebugger_TrackRead)
{
    auto& io = Spark::IODebugger::GetInstance();
    io.Reset();
    io.SetEnabled(true);

    io.RecordRead("data/textures/grass.dds", 4096, 1.5f, __FILE__, __LINE__, __FUNCTION__);

    EXPECT_EQ(io.GetTotalBytesRead(), static_cast<size_t>(4096));
    EXPECT_EQ(io.GetTotalOperationCount(), static_cast<uint64_t>(1));
    EXPECT_NEAR(io.GetTotalIOTimeMs(), 1.5f, 0.01f);
}

TEST(IODebugger_TrackWrite)
{
    auto& io = Spark::IODebugger::GetInstance();
    io.Reset();
    io.SetEnabled(true);

    io.RecordWrite("saves/game.sav", 8192, 2.0f, __FILE__, __LINE__, __FUNCTION__);

    EXPECT_EQ(io.GetTotalBytesWritten(), static_cast<size_t>(8192));
    EXPECT_EQ(io.GetTotalOperationCount(), static_cast<uint64_t>(1));
}

TEST(IODebugger_FileStats)
{
    auto& io = Spark::IODebugger::GetInstance();
    io.Reset();
    io.SetEnabled(true);

    // Multiple operations on the same file
    io.RecordRead("data/level.dat", 1024, 0.5f);
    io.RecordRead("data/level.dat", 2048, 1.0f);
    io.RecordWrite("data/level.dat", 512, 0.3f);

    auto stats = io.GetFileStats();
    EXPECT_EQ(stats.size(), static_cast<size_t>(1));
    EXPECT_EQ(stats[0].readCount, static_cast<uint64_t>(2));
    EXPECT_EQ(stats[0].writeCount, static_cast<uint64_t>(1));
    EXPECT_EQ(stats[0].totalBytesRead, static_cast<size_t>(3072));
    EXPECT_EQ(stats[0].totalBytesWritten, static_cast<size_t>(512));
    EXPECT_NEAR(stats[0].peakReadTimeMs, 1.0f, 0.01f);
}

TEST(IODebugger_SlowestFiles)
{
    auto& io = Spark::IODebugger::GetInstance();
    io.Reset();
    io.SetEnabled(true);

    io.RecordRead("fast.dat", 100, 0.1f);
    io.RecordRead("slow.dat", 100, 10.0f);
    io.RecordRead("medium.dat", 100, 5.0f);

    auto slowest = io.GetSlowestFiles(2);
    EXPECT_EQ(slowest.size(), static_cast<size_t>(2));
    EXPECT_EQ(slowest[0].filePath, std::string("slow.dat"));
    EXPECT_EQ(slowest[1].filePath, std::string("medium.dat"));
}

TEST(IODebugger_HotSpots)
{
    auto& io = Spark::IODebugger::GetInstance();
    io.Reset();
    io.SetEnabled(true);

    // Record from different call sites
    io.RecordRead("a.dat", 100, 0.1f, "loader.cpp", 10, "LoadTexture");
    io.RecordRead("b.dat", 100, 0.1f, "loader.cpp", 10, "LoadTexture");
    io.RecordRead("c.dat", 100, 0.1f, "loader.cpp", 10, "LoadTexture");
    io.RecordRead("d.dat", 100, 0.1f, "audio.cpp", 20, "LoadSound");

    auto spots = io.GetHotSpots(10);
    EXPECT_GE(spots.size(), static_cast<size_t>(2));
    // The loader.cpp site should have more operations
    EXPECT_EQ(spots[0].operationCount, static_cast<uint64_t>(3));
}

TEST(IODebugger_Reset)
{
    auto& io = Spark::IODebugger::GetInstance();
    io.SetEnabled(true);

    io.RecordRead("test.dat", 100, 0.1f);
    EXPECT_GE(io.GetTrackedFileCount(), static_cast<size_t>(1));

    io.Reset();
    EXPECT_EQ(io.GetTrackedFileCount(), static_cast<size_t>(0));
    EXPECT_EQ(io.GetTotalBytesRead(), static_cast<size_t>(0));
    EXPECT_EQ(io.GetTotalBytesWritten(), static_cast<size_t>(0));
    EXPECT_EQ(io.GetTotalOperationCount(), static_cast<uint64_t>(0));
}

// ============================================================================
// ThreadDebugger Tests
// ============================================================================

TEST(ThreadDebugger_TrackThreadLifecycle)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    uint64_t tid = 12345;
    td.RecordThreadStart(tid, "Worker", __FILE__, __LINE__, __FUNCTION__);

    EXPECT_EQ(td.GetActiveThreadCount(), static_cast<uint32_t>(1));

    auto threads = td.GetActiveThreads();
    EXPECT_EQ(threads.size(), static_cast<size_t>(1));
    EXPECT_EQ(threads[0].threadId, tid);
    EXPECT_TRUE(threads[0].alive);

    td.RecordThreadEnd(tid);
    EXPECT_EQ(td.GetActiveThreadCount(), static_cast<uint32_t>(0));

    // Thread should still exist in history but marked as not alive
    auto all = td.GetAllThreads();
    EXPECT_EQ(all.size(), static_cast<size_t>(1));
    EXPECT_FALSE(all[0].alive);
}

TEST(ThreadDebugger_ThreadNaming)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    uint64_t tid = 99;
    td.RecordThreadStart(tid, "OldName");
    td.SetThreadName(tid, "NewName");

    auto threads = td.GetActiveThreads();
    EXPECT_EQ(threads.size(), static_cast<size_t>(1));
    EXPECT_EQ(threads[0].name, std::string("NewName"));

    td.RecordThreadEnd(tid);
}

TEST(ThreadDebugger_ContentionTracking)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // Simulate a mutex wait
    td.RecordMutexWaitBegin("RenderMutex", __FILE__, __LINE__);
    // Small delay to simulate contention
    volatile unsigned int x = 0;
    for (unsigned int i = 0; i < 10000; ++i)
        x += i;
    td.RecordMutexWaitEnd("RenderMutex");

    auto stats = td.GetContentionStats();
    EXPECT_EQ(stats.size(), static_cast<size_t>(1));
    EXPECT_EQ(stats[0].mutexName, std::string("RenderMutex"));
    EXPECT_EQ(stats[0].totalWaits, static_cast<uint64_t>(1));
    EXPECT_GE(stats[0].totalWaitTimeMs, 0.0f);
}

TEST(ThreadDebugger_ContentionHotSpots)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // Multiple waits on different mutexes
    for (int i = 0; i < 5; ++i)
    {
        td.RecordMutexWaitBegin("HotMutex");
        td.RecordMutexWaitEnd("HotMutex");
    }

    td.RecordMutexWaitBegin("ColdMutex");
    td.RecordMutexWaitEnd("ColdMutex");

    auto stats = td.GetContentionStats();
    EXPECT_EQ(stats.size(), static_cast<size_t>(2));
    // HotMutex should have more waits
    EXPECT_EQ(stats[0].totalWaits + stats[1].totalWaits, static_cast<uint64_t>(6));
}

TEST(ThreadDebugger_DeadlockWarning)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    uint64_t threadA = 1;
    uint64_t threadB = 2;

    td.RecordThreadStart(threadA, "ThreadA");
    td.RecordThreadStart(threadB, "ThreadB");

    // Thread A acquires MutexX then MutexY
    td.RecordMutexAcquire("MutexX", threadA);
    td.RecordMutexAcquire("MutexY", threadA);
    td.RecordMutexRelease("MutexY", threadA);
    td.RecordMutexRelease("MutexX", threadA);

    // Thread B acquires MutexY then MutexX (reverse order = potential deadlock)
    td.RecordMutexAcquire("MutexY", threadB);
    td.RecordMutexAcquire("MutexX", threadB); // This should trigger a warning
    td.RecordMutexRelease("MutexX", threadB);
    td.RecordMutexRelease("MutexY", threadB);

    auto warnings = td.GetDeadlockWarnings();
    EXPECT_GE(warnings.size(), static_cast<size_t>(1));

    td.RecordThreadEnd(threadA);
    td.RecordThreadEnd(threadB);
}

TEST(ThreadDebugger_Reset)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.SetEnabled(true);

    td.RecordThreadStart(1, "TestThread");
    td.RecordMutexWaitBegin("TestMutex");
    td.RecordMutexWaitEnd("TestMutex");

    td.Reset();
    EXPECT_EQ(td.GetActiveThreadCount(), static_cast<uint32_t>(0));
    EXPECT_EQ(td.GetTotalContentionEvents(), static_cast<uint64_t>(0));
    EXPECT_EQ(td.GetDeadlockWarnings().size(), static_cast<size_t>(0));
}

TEST(ThreadDebugger_GetCurrentThreadId)
{
    // Verify the utility function returns a non-zero value
    uint64_t tid = Spark::ThreadDebugger::GetCurrentThreadId();
    EXPECT_NE(tid, static_cast<uint64_t>(0));
}
