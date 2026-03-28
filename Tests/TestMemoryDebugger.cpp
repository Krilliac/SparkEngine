// TestMemoryDebugger.cpp - Tests for memory leak detection, allocation tracking, and corruption detection

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/MemoryDebugger.h"

// =============================================================================
// Basic Allocation Tracking
// =============================================================================

TEST(MemDbg_RecordAllocAndFree)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int dummy = 0;
    md.RecordAlloc(&dummy, 1024, "General", __FILE__, __LINE__, __FUNCTION__);

    EXPECT_EQ(md.GetCurrentBytes(), static_cast<size_t>(1024));
    EXPECT_EQ(md.GetActiveAllocationCount(), static_cast<size_t>(1));
    EXPECT_EQ(md.GetTotalAllocationCount(), static_cast<uint64_t>(1));

    bool freed = md.RecordFree(&dummy);
    EXPECT_TRUE(freed);
    EXPECT_EQ(md.GetCurrentBytes(), static_cast<size_t>(0));
    EXPECT_EQ(md.GetActiveAllocationCount(), static_cast<size_t>(0));
}

TEST(MemDbg_MultipleAllocations)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int a = 0, b = 0, c = 0;
    md.RecordAlloc(&a, 100, "Textures");
    md.RecordAlloc(&b, 200, "Audio");
    md.RecordAlloc(&c, 300, "Textures");

    EXPECT_EQ(md.GetCurrentBytes(), static_cast<size_t>(600));
    EXPECT_EQ(md.GetActiveAllocationCount(), static_cast<size_t>(3));

    md.RecordFree(&b);
    EXPECT_EQ(md.GetCurrentBytes(), static_cast<size_t>(400));
    EXPECT_EQ(md.GetActiveAllocationCount(), static_cast<size_t>(2));

    md.RecordFree(&a);
    md.RecordFree(&c);
    EXPECT_EQ(md.GetCurrentBytes(), static_cast<size_t>(0));
}

// =============================================================================
// Double-Free Detection
// =============================================================================

TEST(MemDbg_DoubleFreeDetection)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int dummy = 0;
    md.RecordAlloc(&dummy, 512, "General");

    bool first = md.RecordFree(&dummy);
    EXPECT_TRUE(first);
    EXPECT_EQ(md.GetDoubleFreeCount(), static_cast<uint64_t>(0));

    // Second free of same pointer should be detected
    bool second = md.RecordFree(&dummy);
    EXPECT_FALSE(second);
    EXPECT_EQ(md.GetDoubleFreeCount(), static_cast<uint64_t>(1));
}

TEST(MemDbg_FreeUntrackedPointer)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int dummy = 0;
    bool result = md.RecordFree(&dummy);
    EXPECT_FALSE(result);
    EXPECT_EQ(md.GetDoubleFreeCount(), static_cast<uint64_t>(1));
}

// =============================================================================
// Category Statistics
// =============================================================================

TEST(MemDbg_CategoryStats)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int a = 0, b = 0, c = 0, d = 0;
    md.RecordAlloc(&a, 1000, "Textures");
    md.RecordAlloc(&b, 2000, "Textures");
    md.RecordAlloc(&c, 500, "Audio");
    md.RecordAlloc(&d, 300, "Audio");

    auto stats = md.GetCategoryStats();
    EXPECT_EQ(stats.size(), static_cast<size_t>(2));

    // Stats are sorted by currentBytes descending
    bool foundTextures = false;
    bool foundAudio = false;
    for (const auto& s : stats)
    {
        if (s.name == "Textures")
        {
            foundTextures = true;
            EXPECT_EQ(s.currentBytes, static_cast<size_t>(3000));
            EXPECT_EQ(s.totalAllocations, static_cast<uint64_t>(2));
            EXPECT_EQ(s.totalBytesAllocated, static_cast<size_t>(3000));
        }
        if (s.name == "Audio")
        {
            foundAudio = true;
            EXPECT_EQ(s.currentBytes, static_cast<size_t>(800));
            EXPECT_EQ(s.totalAllocations, static_cast<uint64_t>(2));
        }
    }
    EXPECT_TRUE(foundTextures);
    EXPECT_TRUE(foundAudio);
}

TEST(MemDbg_CategoryDeallocUpdates)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int a = 0, b = 0;
    md.RecordAlloc(&a, 1000, "Meshes");
    md.RecordAlloc(&b, 500, "Meshes");
    md.RecordFree(&a);

    auto stats = md.GetCategoryStats();
    EXPECT_EQ(stats.size(), static_cast<size_t>(1));
    EXPECT_EQ(stats[0].currentBytes, static_cast<size_t>(500));
    EXPECT_EQ(stats[0].totalAllocations, static_cast<uint64_t>(2));
    EXPECT_EQ(stats[0].totalDeallocations, static_cast<uint64_t>(1));
}

// =============================================================================
// Peak Memory Tracking
// =============================================================================

TEST(MemDbg_PeakTracking)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int a = 0, b = 0, c = 0;
    md.RecordAlloc(&a, 1000, "General");
    md.RecordAlloc(&b, 2000, "General");
    // Peak should be 3000
    md.RecordFree(&a);
    // Current is 2000, peak should still be 3000
    md.RecordAlloc(&c, 500, "General");
    // Current is 2500, peak should still be 3000

    EXPECT_EQ(md.GetCurrentBytes(), static_cast<size_t>(2500));
    EXPECT_EQ(md.GetPeakBytes(), static_cast<size_t>(3000));
}

TEST(MemDbg_CategoryPeakTracking)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int a = 0, b = 0;
    md.RecordAlloc(&a, 2000, "GPU");
    md.RecordAlloc(&b, 1000, "GPU");
    md.RecordFree(&a);

    auto stats = md.GetCategoryStats();
    EXPECT_EQ(stats.size(), static_cast<size_t>(1));
    EXPECT_EQ(stats[0].peakBytes, static_cast<size_t>(3000));
    EXPECT_EQ(stats[0].currentBytes, static_cast<size_t>(1000));
}

// =============================================================================
// Leak Detection
// =============================================================================

TEST(MemDbg_LeakDetection)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int a = 0, b = 0, c = 0;
    md.RecordAlloc(&a, 100, "Leak1", "test.cpp", 10, "TestFunc");
    md.RecordAlloc(&b, 200, "Leak2", "test.cpp", 20, "TestFunc");
    md.RecordAlloc(&c, 300, "Leak3", "test.cpp", 30, "TestFunc");
    md.RecordFree(&b); // Free one, leave two as leaks

    auto leaks = md.GetLeaks();
    EXPECT_EQ(leaks.size(), static_cast<size_t>(2));

    // Leaks sorted by allocation index (oldest first)
    EXPECT_EQ(leaks[0].size, static_cast<size_t>(100));
    EXPECT_EQ(leaks[1].size, static_cast<size_t>(300));

    // Clean up
    md.RecordFree(&a);
    md.RecordFree(&c);
}

TEST(MemDbg_NoLeaksReturnsEmpty)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int a = 0;
    md.RecordAlloc(&a, 100, "General");
    md.RecordFree(&a);

    auto leaks = md.GetLeaks();
    EXPECT_EQ(leaks.size(), static_cast<size_t>(0));
}

// =============================================================================
// Hot Spot Analysis
// =============================================================================

TEST(MemDbg_HotSpotAnalysis)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    // Allocate from different call sites
    int ptrs[10];
    for (int i = 0; i < 5; ++i)
        md.RecordAlloc(&ptrs[i], 100, "General", "loader.cpp", 42, "LoadAsset");

    for (int i = 5; i < 7; ++i)
        md.RecordAlloc(&ptrs[i], 200, "General", "renderer.cpp", 99, "CreateBuffer");

    auto hotspots = md.GetHotSpots(10);
    EXPECT_GE(hotspots.size(), static_cast<size_t>(2));

    // First hotspot should be loader.cpp with 5 allocations
    EXPECT_EQ(hotspots[0].count, static_cast<uint64_t>(5));
    EXPECT_EQ(hotspots[0].totalBytes, static_cast<size_t>(500));

    // Second hotspot: renderer.cpp with 2 allocations
    EXPECT_EQ(hotspots[1].count, static_cast<uint64_t>(2));
    EXPECT_EQ(hotspots[1].totalBytes, static_cast<size_t>(400));
    EXPECT_EQ(hotspots[1].largestAllocation, static_cast<size_t>(200));

    // Cleanup
    for (int i = 0; i < 7; ++i)
        md.RecordFree(&ptrs[i]);
}

TEST(MemDbg_HotSpotTopN)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int ptrs[6];
    md.RecordAlloc(&ptrs[0], 100, "General", "a.cpp", 1, "FuncA");
    md.RecordAlloc(&ptrs[1], 100, "General", "b.cpp", 1, "FuncB");
    md.RecordAlloc(&ptrs[2], 100, "General", "c.cpp", 1, "FuncC");
    md.RecordAlloc(&ptrs[3], 100, "General", "d.cpp", 1, "FuncD");
    md.RecordAlloc(&ptrs[4], 100, "General", "e.cpp", 1, "FuncE");
    md.RecordAlloc(&ptrs[5], 100, "General", "f.cpp", 1, "FuncF");

    // Request only top 3
    auto hotspots = md.GetHotSpots(3);
    EXPECT_LE(hotspots.size(), static_cast<size_t>(3));

    for (int i = 0; i < 6; ++i)
        md.RecordFree(&ptrs[i]);
}

// =============================================================================
// Disabled State
// =============================================================================

TEST(MemDbg_DisabledNoTracking)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(false);

    int dummy = 0;
    md.RecordAlloc(&dummy, 1024, "General");

    EXPECT_EQ(md.GetCurrentBytes(), static_cast<size_t>(0));
    EXPECT_EQ(md.GetActiveAllocationCount(), static_cast<size_t>(0));
    EXPECT_EQ(md.GetTotalAllocationCount(), static_cast<uint64_t>(0));

    md.SetEnabled(true);
}

// =============================================================================
// Reset
// =============================================================================

TEST(MemDbg_Reset)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.SetEnabled(true);

    int a = 0;
    md.RecordAlloc(&a, 1024, "General");
    md.RecordFree(&a);
    md.RecordFree(&a); // double-free

    EXPECT_GT(md.GetTotalAllocationCount(), static_cast<uint64_t>(0));
    EXPECT_GT(md.GetDoubleFreeCount(), static_cast<uint64_t>(0));

    md.Reset();

    EXPECT_EQ(md.GetCurrentBytes(), static_cast<size_t>(0));
    EXPECT_EQ(md.GetPeakBytes(), static_cast<size_t>(0));
    EXPECT_EQ(md.GetTotalAllocationCount(), static_cast<uint64_t>(0));
    EXPECT_EQ(md.GetActiveAllocationCount(), static_cast<size_t>(0));
    EXPECT_EQ(md.GetDoubleFreeCount(), static_cast<uint64_t>(0));
}

// =============================================================================
// Null Pointer Handling
// =============================================================================

TEST(MemDbg_NullPointerIgnored)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    md.RecordAlloc(nullptr, 1024, "General");
    EXPECT_EQ(md.GetActiveAllocationCount(), static_cast<size_t>(0));

    bool result = md.RecordFree(nullptr);
    EXPECT_TRUE(result); // null free is silently accepted
    EXPECT_EQ(md.GetDoubleFreeCount(), static_cast<uint64_t>(0));
}

// =============================================================================
// Print Leak Report
// =============================================================================

TEST(MemDbg_PrintLeakReportReturnsCount)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    int a = 0, b = 0;
    md.RecordAlloc(&a, 100, "General");
    md.RecordAlloc(&b, 200, "General");

    size_t leakCount = md.PrintLeakReport();
    EXPECT_EQ(leakCount, static_cast<size_t>(2));

    md.RecordFree(&a);
    md.RecordFree(&b);

    leakCount = md.PrintLeakReport();
    EXPECT_EQ(leakCount, static_cast<size_t>(0));
}
