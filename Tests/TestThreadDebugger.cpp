// TestThreadDebugger.cpp - Extended tests for ThreadDebugger
// Covers edge cases, disabled mode, null args, reports, macros, and
// multi-thread scenarios not covered by TestDebugUtilities.cpp

#include "TestFramework.h"
#include "Utils/ThreadDebugger.h"
#include <thread>

static void ResetDebugger()
{
    auto& dbg = Spark::ThreadDebugger::GetInstance();
    dbg.Reset();
    dbg.SetEnabled(true);
}

// =============================================================================
// Thread lifecycle edge cases
// =============================================================================

TEST(ThreadDbg_MultipleThreadsActiveCount)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordThreadStart(1, "A");
    dbg.RecordThreadStart(2, "B");
    dbg.RecordThreadStart(3, "C");

    EXPECT_EQ(dbg.GetActiveThreadCount(), (uint32_t)3);

    dbg.RecordThreadEnd(2);
    EXPECT_EQ(dbg.GetActiveThreadCount(), (uint32_t)2);

    auto all = dbg.GetAllThreads();
    EXPECT_EQ(all.size(), (size_t)3);
}

TEST(ThreadDbg_NullNameHandled)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordThreadStart(50, nullptr, nullptr, 0, nullptr);

    auto threads = dbg.GetAllThreads();
    EXPECT_EQ(threads.size(), (size_t)1);
    EXPECT_EQ(threads[0].name, std::string(""));
    EXPECT_EQ(threads[0].creationFile, std::string(""));
    EXPECT_EQ(threads[0].creationFunction, std::string(""));
}

TEST(ThreadDbg_SetNameUnknownThread)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    // No crash on unknown thread ID
    dbg.SetThreadName(999, "Ghost");
    EXPECT_EQ(dbg.GetAllThreads().size(), (size_t)0);
}

TEST(ThreadDbg_EndUnknownThread)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    // No crash on unknown thread ID
    dbg.RecordThreadEnd(777);
    EXPECT_EQ(dbg.GetAllThreads().size(), (size_t)0);
}

TEST(ThreadDbg_CreationSiteInfo)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordThreadStart(100, "Worker", "main.cpp", 42, "LaunchWorker");

    auto threads = dbg.GetAllThreads();
    EXPECT_EQ(threads[0].creationFile, std::string("main.cpp"));
    EXPECT_EQ(threads[0].creationLine, 42);
    EXPECT_EQ(threads[0].creationFunction, std::string("LaunchWorker"));
}

// =============================================================================
// Mutex contention edge cases
// =============================================================================

TEST(ThreadDbg_ContentionStatsTopN)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    for (int i = 0; i < 5; ++i)
    {
        std::string name = "Mutex" + std::to_string(i);
        dbg.RecordMutexWaitBegin(name.c_str());
        dbg.RecordMutexWaitEnd(name.c_str());
    }

    auto top2 = dbg.GetContentionStats(2);
    EXPECT_EQ(top2.size(), (size_t)2);
}

TEST(ThreadDbg_TotalContentionEvents)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordMutexWaitBegin("A");
    dbg.RecordMutexWaitEnd("A");
    dbg.RecordMutexWaitBegin("B");
    dbg.RecordMutexWaitEnd("B");
    dbg.RecordMutexWaitBegin("A");
    dbg.RecordMutexWaitEnd("A");

    EXPECT_EQ(dbg.GetTotalContentionEvents(), (uint64_t)3);
}

TEST(ThreadDbg_WaitEndWithoutBeginIgnored)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordMutexWaitEnd("NeverStarted");
    EXPECT_EQ(dbg.GetTotalContentionEvents(), (uint64_t)0);
}

TEST(ThreadDbg_NullMutexNameIgnored)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordMutexWaitBegin(nullptr);
    dbg.RecordMutexWaitEnd(nullptr);
    dbg.RecordMutexAcquire(nullptr, 1);
    dbg.RecordMutexRelease(nullptr, 1);

    EXPECT_EQ(dbg.GetTotalContentionEvents(), (uint64_t)0);
}

TEST(ThreadDbg_PeakWaitTimeTracked)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordMutexWaitBegin("PeakTest");
    dbg.RecordMutexWaitEnd("PeakTest");

    auto stats = dbg.GetContentionStats();
    EXPECT_EQ(stats.size(), (size_t)1);
    EXPECT_TRUE(stats[0].peakWaitTimeMs >= 0.0f);
    EXPECT_TRUE(stats[0].avgWaitTimeMs >= 0.0f);
}

// =============================================================================
// Lock-order tracking
// =============================================================================

TEST(ThreadDbg_NoFalsePositiveSameOrder)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    // Both threads acquire in same order — no warning
    dbg.RecordMutexAcquire("X", 1);
    dbg.RecordMutexAcquire("Y", 1);
    dbg.RecordMutexRelease("Y", 1);
    dbg.RecordMutexRelease("X", 1);

    dbg.RecordMutexAcquire("X", 2);
    dbg.RecordMutexAcquire("Y", 2);
    dbg.RecordMutexRelease("Y", 2);
    dbg.RecordMutexRelease("X", 2);

    EXPECT_EQ(dbg.GetDeadlockWarnings().size(), (size_t)0);
}

TEST(ThreadDbg_SingleMutexNoWarning)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordMutexAcquire("OnlyOne", 1);
    dbg.RecordMutexRelease("OnlyOne", 1);

    EXPECT_EQ(dbg.GetDeadlockWarnings().size(), (size_t)0);
}

TEST(ThreadDbg_ReleaseUnheldMutex)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    // No crash
    dbg.RecordMutexRelease("NotHeld", 42);
    EXPECT_EQ(dbg.GetDeadlockWarnings().size(), (size_t)0);
}

TEST(ThreadDbg_ThreadEndCleansHeldMutexes)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordThreadStart(1, "T1");
    dbg.RecordMutexAcquire("L1", 1);
    dbg.RecordMutexAcquire("L2", 1);

    // End thread without releasing — should clean up held mutexes
    dbg.RecordThreadEnd(1);
    EXPECT_EQ(dbg.GetActiveThreadCount(), (uint32_t)0);
}

// =============================================================================
// Enabled/disabled
// =============================================================================

TEST(ThreadDbg_DisabledSkipsAll)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.SetEnabled(false);
    EXPECT_FALSE(dbg.IsEnabled());

    dbg.RecordThreadStart(1, "Ignored");
    dbg.RecordMutexWaitBegin("Ignored");
    dbg.RecordMutexWaitEnd("Ignored");
    dbg.RecordMutexAcquire("Ignored", 1);
    dbg.RecordMutexRelease("Ignored", 1);
    dbg.RecordThreadEnd(1);

    EXPECT_EQ(dbg.GetAllThreads().size(), (size_t)0);
    EXPECT_EQ(dbg.GetTotalContentionEvents(), (uint64_t)0);

    dbg.SetEnabled(true);
}

TEST(ThreadDbg_DisabledSetThreadNameIgnored)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordThreadStart(1, "Before");
    dbg.SetEnabled(false);
    dbg.SetThreadName(1, "ShouldNotChange");
    dbg.SetEnabled(true);

    auto threads = dbg.GetAllThreads();
    EXPECT_EQ(threads[0].name, std::string("Before"));
}

// =============================================================================
// Reports (smoke tests — verify they don't crash)
// =============================================================================

TEST(ThreadDbg_PrintThreadReportNoCrash)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordThreadStart(1, "Main", "engine.cpp", 10, "Start");
    dbg.RecordThreadStart(2, "Worker");
    dbg.RecordThreadEnd(2);

    dbg.PrintThreadReport();
    EXPECT_TRUE(true);
}

TEST(ThreadDbg_PrintContentionReportNoCrash)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordMutexWaitBegin("TestLock");
    dbg.RecordMutexWaitEnd("TestLock");

    dbg.PrintContentionReport();
    EXPECT_TRUE(true);
}

TEST(ThreadDbg_PrintSummaryNoCrash)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.RecordThreadStart(1, "Main");
    dbg.RecordMutexWaitBegin("Lock");
    dbg.RecordMutexWaitEnd("Lock");

    dbg.PrintSummary();
    EXPECT_TRUE(true);
}

TEST(ThreadDbg_PrintEmptyReportsNoCrash)
{
    ResetDebugger();
    auto& dbg = Spark::ThreadDebugger::GetInstance();

    dbg.PrintThreadReport();
    dbg.PrintContentionReport();
    dbg.PrintSummary();
    EXPECT_TRUE(true);
}

// =============================================================================
// GetCurrentThreadId consistency
// =============================================================================

TEST(ThreadDbg_CurrentThreadIdConsistent)
{
    uint64_t id1 = Spark::ThreadDebugger::GetCurrentThreadId();
    uint64_t id2 = Spark::ThreadDebugger::GetCurrentThreadId();
    EXPECT_EQ(id1, id2);
    EXPECT_TRUE(id1 != 0);
}

// =============================================================================
// Macros
// =============================================================================

TEST(ThreadDbg_MacrosCompile)
{
    ResetDebugger();

    uint64_t tid = Spark::ThreadDebugger::GetCurrentThreadId();
    SPARK_TRACK_THREAD_START(tid, "MacroThread");
    SPARK_TRACK_MUTEX_WAIT("MacroMutex");
    SPARK_TRACK_MUTEX_ACQUIRED("MacroMutex");
    SPARK_TRACK_MUTEX_ACQUIRE("MacroMutex", tid);
    SPARK_TRACK_MUTEX_RELEASE("MacroMutex", tid);
    SPARK_PRINT_THREAD_REPORT();
    SPARK_TRACK_THREAD_END(tid);

    EXPECT_TRUE(true);
}
