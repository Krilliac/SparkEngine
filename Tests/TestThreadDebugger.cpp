// TestThreadDebugger.cpp - Tests for thread lifecycle tracking, mutex contention, and deadlock detection

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/ThreadDebugger.h"
#include <thread>

// =============================================================================
// Enable / Disable
// =============================================================================

TEST(ThreadDbg_EnableDisable)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();

    td.SetEnabled(true);
    EXPECT_TRUE(td.IsEnabled());

    td.SetEnabled(false);
    EXPECT_FALSE(td.IsEnabled());

    td.SetEnabled(true);
}

// =============================================================================
// Thread Lifecycle
// =============================================================================

TEST(ThreadDbg_RecordThreadStartAndEnd)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    td.RecordThreadStart(100, "Worker", "test.cpp", 10, "TestFunc");

    auto active = td.GetActiveThreads();
    EXPECT_EQ(static_cast<int>(active.size()), 1);
    EXPECT_EQ(active[0].threadId, static_cast<uint64_t>(100));
    EXPECT_EQ(active[0].name, std::string("Worker"));
    EXPECT_TRUE(active[0].alive);
    EXPECT_EQ(active[0].creationFile, std::string("test.cpp"));
    EXPECT_EQ(active[0].creationLine, 10);
    EXPECT_EQ(active[0].creationFunction, std::string("TestFunc"));

    td.RecordThreadEnd(100);

    active = td.GetActiveThreads();
    EXPECT_EQ(static_cast<int>(active.size()), 0);

    auto all = td.GetAllThreads();
    EXPECT_EQ(static_cast<int>(all.size()), 1);
    EXPECT_FALSE(all[0].alive);
}

TEST(ThreadDbg_MultipleThreads)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    td.RecordThreadStart(1, "Thread1");
    td.RecordThreadStart(2, "Thread2");
    td.RecordThreadStart(3, "Thread3");

    EXPECT_EQ(td.GetActiveThreadCount(), 3u);

    td.RecordThreadEnd(2);
    EXPECT_EQ(td.GetActiveThreadCount(), 2u);

    auto all = td.GetAllThreads();
    EXPECT_EQ(static_cast<int>(all.size()), 3);
}

TEST(ThreadDbg_SetThreadName)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    td.RecordThreadStart(42, "OldName");

    auto threads = td.GetAllThreads();
    EXPECT_EQ(threads[0].name, std::string("OldName"));

    td.SetThreadName(42, "NewName");

    threads = td.GetAllThreads();
    EXPECT_EQ(threads[0].name, std::string("NewName"));
}

TEST(ThreadDbg_SetThreadNameNonexistentNoOp)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // Should not crash
    td.SetThreadName(999, "Ghost");
    EXPECT_EQ(td.GetActiveThreadCount(), 0u);
}

TEST(ThreadDbg_RecordThreadEndCleansHeldMutexes)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    td.RecordThreadStart(10, "Worker");
    td.RecordMutexAcquire("MutexA", 10);
    td.RecordThreadEnd(10);

    // Ending a thread should clean up its held mutexes
    // Verify no crash and thread is marked dead
    auto all = td.GetAllThreads();
    EXPECT_EQ(static_cast<int>(all.size()), 1);
    EXPECT_FALSE(all[0].alive);
}

// =============================================================================
// Disabled State
// =============================================================================

TEST(ThreadDbg_DisabledNoTracking)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(false);

    td.RecordThreadStart(50, "Ghost");
    EXPECT_EQ(td.GetActiveThreadCount(), 0u);
    EXPECT_EQ(static_cast<int>(td.GetAllThreads().size()), 0);

    // Re-enable for other tests
    td.SetEnabled(true);
}

TEST(ThreadDbg_DisabledMutexOpsNoOp)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(false);

    td.RecordMutexWaitBegin("TestMutex");
    td.RecordMutexWaitEnd("TestMutex");
    td.RecordMutexAcquire("TestMutex", 1);
    td.RecordMutexRelease("TestMutex", 1);

    EXPECT_EQ(td.GetTotalContentionEvents(), static_cast<uint64_t>(0));

    td.SetEnabled(true);
}

// =============================================================================
// Null Pointer Handling
// =============================================================================

TEST(ThreadDbg_NullNameHandling)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // nullptr name for thread start should not crash
    td.RecordThreadStart(77, nullptr);
    auto threads = td.GetAllThreads();
    EXPECT_EQ(static_cast<int>(threads.size()), 1);
    EXPECT_EQ(threads[0].name, std::string(""));

    // nullptr for SetThreadName should be a no-op
    td.SetThreadName(77, nullptr);
    threads = td.GetAllThreads();
    EXPECT_EQ(threads[0].name, std::string(""));

    // nullptr mutex names should be no-ops
    td.RecordMutexWaitBegin(nullptr);
    td.RecordMutexWaitEnd(nullptr);
    td.RecordMutexAcquire(nullptr, 77);
    td.RecordMutexRelease(nullptr, 77);
    EXPECT_EQ(td.GetTotalContentionEvents(), static_cast<uint64_t>(0));
}

// =============================================================================
// Mutex Contention
// =============================================================================

TEST(ThreadDbg_MutexContentionTracking)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    td.RecordMutexWaitBegin("RenderLock", "graphics.cpp", 50);
    // Small delay to ensure measurable wait time
    volatile int x = 0;
    for (int i = 0; i < 10000; ++i)
        x += i;
    td.RecordMutexWaitEnd("RenderLock");

    EXPECT_EQ(td.GetTotalContentionEvents(), static_cast<uint64_t>(1));

    auto stats = td.GetContentionStats();
    EXPECT_EQ(static_cast<int>(stats.size()), 1);
    EXPECT_EQ(stats[0].mutexName, std::string("RenderLock"));
    EXPECT_EQ(stats[0].totalWaits, static_cast<uint64_t>(1));
    EXPECT_GE(stats[0].totalWaitTimeMs, 0.0f);
    EXPECT_GE(stats[0].peakWaitTimeMs, 0.0f);
    EXPECT_GE(stats[0].avgWaitTimeMs, 0.0f);
}

TEST(ThreadDbg_MultipleContentionEvents)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    for (int i = 0; i < 5; ++i)
    {
        td.RecordMutexWaitBegin("FrequentMutex");
        td.RecordMutexWaitEnd("FrequentMutex");
    }

    EXPECT_EQ(td.GetTotalContentionEvents(), static_cast<uint64_t>(5));

    auto stats = td.GetContentionStats();
    EXPECT_EQ(static_cast<int>(stats.size()), 1);
    EXPECT_EQ(stats[0].totalWaits, static_cast<uint64_t>(5));
}

TEST(ThreadDbg_ContentionStatsTopN)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // Create contention on multiple mutexes
    for (int m = 0; m < 5; ++m)
    {
        std::string name = "Mutex" + std::to_string(m);
        for (int i = 0; i < m + 1; ++i)
        {
            td.RecordMutexWaitBegin(name.c_str());
            td.RecordMutexWaitEnd(name.c_str());
        }
    }

    // Request top 3 only
    auto stats = td.GetContentionStats(3);
    EXPECT_LE(static_cast<int>(stats.size()), 3);
}

TEST(ThreadDbg_MutexWaitEndWithoutBegin)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // Ending a wait that was never started should not crash
    td.RecordMutexWaitEnd("OrphanMutex");
    EXPECT_EQ(td.GetTotalContentionEvents(), static_cast<uint64_t>(0));
}

// =============================================================================
// Mutex Acquire / Release (Lock-Order Tracking)
// =============================================================================

TEST(ThreadDbg_MutexAcquireAndRelease)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    uint64_t tid = Spark::ThreadDebugger::GetCurrentThreadId();
    td.RecordMutexAcquire("LockA", tid);
    td.RecordMutexRelease("LockA", tid);

    // No deadlock warnings for single lock
    auto warnings = td.GetDeadlockWarnings();
    EXPECT_EQ(static_cast<int>(warnings.size()), 0);
}

TEST(ThreadDbg_LockOrderInversionDetection)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // Thread 1: Lock A, then Lock B (establishes A->B order)
    td.RecordMutexAcquire("LockA", 1);
    td.RecordMutexAcquire("LockB", 1);
    td.RecordMutexRelease("LockB", 1);
    td.RecordMutexRelease("LockA", 1);

    // Thread 2: Lock B, then Lock A (reverse order = inversion warning)
    td.RecordMutexAcquire("LockB", 2);
    td.RecordMutexAcquire("LockA", 2); // Should trigger warning
    td.RecordMutexRelease("LockA", 2);
    td.RecordMutexRelease("LockB", 2);

    auto warnings = td.GetDeadlockWarnings();
    EXPECT_GE(static_cast<int>(warnings.size()), 1);

    if (!warnings.empty())
    {
        EXPECT_STR_CONTAINS(warnings[0].description, "Lock-order inversion");
    }
}

TEST(ThreadDbg_NoWarningForConsistentOrder)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // Both threads lock in same order: A->B
    td.RecordMutexAcquire("LockA", 1);
    td.RecordMutexAcquire("LockB", 1);
    td.RecordMutexRelease("LockB", 1);
    td.RecordMutexRelease("LockA", 1);

    td.RecordMutexAcquire("LockA", 2);
    td.RecordMutexAcquire("LockB", 2);
    td.RecordMutexRelease("LockB", 2);
    td.RecordMutexRelease("LockA", 2);

    auto warnings = td.GetDeadlockWarnings();
    EXPECT_EQ(static_cast<int>(warnings.size()), 0);
}

TEST(ThreadDbg_ReleaseNonHeldMutex)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    // Releasing a mutex not acquired should not crash
    td.RecordMutexRelease("Phantom", 99);
    EXPECT_EQ(static_cast<int>(td.GetDeadlockWarnings().size()), 0);
}

// =============================================================================
// GetCurrentThreadId
// =============================================================================

TEST(ThreadDbg_GetCurrentThreadId)
{
    uint64_t id1 = Spark::ThreadDebugger::GetCurrentThreadId();
    uint64_t id2 = Spark::ThreadDebugger::GetCurrentThreadId();
    EXPECT_EQ(id1, id2); // Same thread should return same ID

    // ID should be non-zero
    EXPECT_NE(id1, static_cast<uint64_t>(0));
}

TEST(ThreadDbg_DifferentThreadsDifferentIds)
{
    uint64_t mainId = Spark::ThreadDebugger::GetCurrentThreadId();
    uint64_t otherId = 0;

    std::thread t([&]() { otherId = Spark::ThreadDebugger::GetCurrentThreadId(); });
    t.join();

    EXPECT_NE(mainId, otherId);
}

// =============================================================================
// Reports (should not crash)
// =============================================================================

TEST(ThreadDbg_PrintReportsDoNotCrash)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.Reset();
    td.SetEnabled(true);

    td.RecordThreadStart(1, "TestThread", "test.cpp", 1, "main");
    td.RecordMutexWaitBegin("TestMutex");
    td.RecordMutexWaitEnd("TestMutex");

    EXPECT_NO_THROW(td.PrintThreadReport());
    EXPECT_NO_THROW(td.PrintContentionReport());
    EXPECT_NO_THROW(td.PrintSummary());
}

// =============================================================================
// Reset
// =============================================================================

TEST(ThreadDbg_Reset)
{
    auto& td = Spark::ThreadDebugger::GetInstance();
    td.SetEnabled(true);

    td.RecordThreadStart(1, "T1");
    td.RecordMutexWaitBegin("M1");
    td.RecordMutexWaitEnd("M1");
    td.RecordMutexAcquire("M1", 1);

    td.Reset();

    EXPECT_EQ(td.GetActiveThreadCount(), 0u);
    EXPECT_EQ(static_cast<int>(td.GetAllThreads().size()), 0);
    EXPECT_EQ(td.GetTotalContentionEvents(), static_cast<uint64_t>(0));
    EXPECT_EQ(static_cast<int>(td.GetDeadlockWarnings().size()), 0);
    EXPECT_EQ(static_cast<int>(td.GetContentionStats().size()), 0);
}
