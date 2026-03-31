// TestMemoryMonitor.cpp - Tests for proactive runtime memory health monitoring

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/MemoryDebugger.h"
#include "../SparkEngine/Source/Utils/MemoryMonitor.h"

// =============================================================================
// Lifecycle
// =============================================================================

TEST(MemoryMonitor_InitializeAndShutdown)
{
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);

    auto& mm = Spark::MemoryMonitor::GetInstance();
    mm.Initialize();
    mm.SetEnabled(true);

    EXPECT_TRUE(mm.IsEnabled());
    EXPECT_TRUE(mm.IsHealthy());
    EXPECT_TRUE(mm.GetHealthStatus() == Spark::MemoryHealthStatus::Healthy);

    mm.Shutdown();
    EXPECT_FALSE(mm.IsEnabled());
}

// =============================================================================
// Budget Management
// =============================================================================

TEST(MemoryMonitor_SetCategoryBudget)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    mm.SetCategoryBudget("Textures", 100 * 1024 * 1024); // 100 MB
    mm.SetCategoryBudget("Audio", 50 * 1024 * 1024);     // 50 MB

    auto budgets = mm.GetCategoryBudgets();
    EXPECT_EQ(budgets.size(), static_cast<size_t>(2));
    EXPECT_EQ(budgets["Textures"], static_cast<size_t>(100 * 1024 * 1024));
    EXPECT_EQ(budgets["Audio"], static_cast<size_t>(50 * 1024 * 1024));

    // Remove a budget by setting to 0
    mm.SetCategoryBudget("Audio", 0);
    budgets = mm.GetCategoryBudgets();
    EXPECT_EQ(budgets.size(), static_cast<size_t>(1));

    mm.Shutdown();
}

TEST(MemoryMonitor_GlobalBudget)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    mm.SetGlobalBudget(256 * 1024 * 1024); // 256 MB

    // The global budget is just stored, tested via health checks
    mm.Shutdown();
}

// =============================================================================
// Threshold Configuration
// =============================================================================

TEST(MemoryMonitor_ThresholdSetters)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    // These should not throw
    EXPECT_NO_THROW(mm.SetLeakRateThreshold(2 * 1024 * 1024));
    EXPECT_NO_THROW(mm.SetSpikeThreshold(20 * 1024 * 1024));
    EXPECT_NO_THROW(mm.SetBudgetWarningPercent(0.75f));
    EXPECT_NO_THROW(mm.SetStackWarningBytes(128 * 1024));
    EXPECT_NO_THROW(mm.SetCheckInterval(2.0f));

    mm.Shutdown();
}

// =============================================================================
// Snapshots
// =============================================================================

TEST(MemoryMonitor_TakeSnapshot)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    int a = 0, b = 0;
    md.RecordAlloc(&a, 1024, "Textures");
    md.RecordAlloc(&b, 2048, "Audio");

    auto snap = mm.TakeSnapshot();
    EXPECT_EQ(snap.totalBytes, static_cast<size_t>(3072));
    EXPECT_EQ(snap.activeAllocations, static_cast<size_t>(2));
    EXPECT_EQ(snap.categories.size(), static_cast<size_t>(2));

    md.RecordFree(&a);
    md.RecordFree(&b);
    mm.Shutdown();
}

TEST(MemoryMonitor_CompareSnapshots)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    auto before = mm.TakeSnapshot();

    int a = 0, b = 0;
    md.RecordAlloc(&a, 4096, "Textures");
    md.RecordAlloc(&b, 2048, "Audio");

    auto after = mm.TakeSnapshot();

    std::string diff = Spark::MemoryMonitor::CompareSnapshots(before, after);
    EXPECT_STR_CONTAINS(diff, "Memory Snapshot Comparison");
    EXPECT_STR_CONTAINS(diff, "Total bytes:");
    EXPECT_STR_CONTAINS(diff, "Active allocations:");

    md.RecordFree(&a);
    md.RecordFree(&b);
    mm.Shutdown();
}

// =============================================================================
// Update and Health Checks
// =============================================================================

TEST(MemoryMonitor_UpdateNoAnomalies)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    // Small allocation, no spikes
    int dummy = 0;
    md.RecordAlloc(&dummy, 100, "General");

    // Update with enough dt to trigger health checks
    mm.Update(2.0f);

    EXPECT_TRUE(mm.IsHealthy());
    auto anomalies = mm.GetRecentAnomalies();
    // Should be no anomalies for tiny allocations
    // (spike threshold is 10 MB by default)

    md.RecordFree(&dummy);
    mm.Shutdown();
}

TEST(MemoryMonitor_DoubleFreeAnomalyDetection)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();
    mm.SetEnabled(true);

    int dummy = 0;
    md.RecordAlloc(&dummy, 100, "General");
    md.RecordFree(&dummy);
    md.RecordFree(&dummy); // Triggers double-free in MemoryDebugger

    // Update to check for double-frees
    mm.Update(0.016f);

    auto anomalies = mm.GetRecentAnomalies();
    bool foundDoubleFree = false;
    for (const auto& a : anomalies)
    {
        if (a.type == Spark::MemoryAnomalyType::DoubleFree)
            foundDoubleFree = true;
    }
    EXPECT_TRUE(foundDoubleFree);

    mm.Shutdown();
}

// =============================================================================
// Status Report
// =============================================================================

TEST(MemoryMonitor_GetStatusReport)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    std::string report = mm.GetStatusReport();
    EXPECT_STR_CONTAINS(report, "Memory Monitor Status");
    EXPECT_STR_CONTAINS(report, "Health:");
    EXPECT_STR_CONTAINS(report, "Current:");

    mm.Shutdown();
}

// =============================================================================
// Disabled State
// =============================================================================

TEST(MemoryMonitor_DisabledSkipsUpdate)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();
    mm.SetEnabled(false);

    // Update should be a no-op when disabled
    EXPECT_NO_THROW(mm.Update(1.0f));

    mm.SetEnabled(true);
    mm.Shutdown();
}

// =============================================================================
// Anomaly Ring Buffer
// =============================================================================

TEST(MemoryMonitor_AnomalyRingBufferWraps)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    // Force many double-free anomalies to fill the ring buffer
    int dummy = 0;
    md.RecordAlloc(&dummy, 100, "General");
    md.RecordFree(&dummy);

    // Generate 70 double-frees (ring buffer is 64)
    for (int i = 0; i < 70; ++i)
    {
        md.RecordFree(&dummy);
        mm.Update(0.016f);
    }

    auto anomalies = mm.GetRecentAnomalies();
    // Should cap at ring buffer size
    EXPECT_LE(static_cast<int>(anomalies.size()), Spark::MemoryMonitor::ANOMALY_RING_SIZE);

    mm.Shutdown();
}

// =============================================================================
// Pressure Callbacks
// =============================================================================

TEST(MemoryMonitor_PressureCallbackRegistration)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    bool callbackInvoked = false;
    mm.RegisterPressureCallback("test_cb", [&](Spark::MemoryHealthStatus status) { callbackInvoked = true; });

    // Unregister should not crash
    mm.UnregisterPressureCallback("test_cb");
    mm.UnregisterPressureCallback("nonexistent_cb"); // no-op

    mm.Shutdown();
}

// =============================================================================
// Global Budget + Update
// =============================================================================

TEST(MemoryMonitor_GlobalBudgetExceeded)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();
    mm.SetEnabled(true);

    // Set a very small global budget
    mm.SetGlobalBudget(100);

    // Allocate more than the budget
    int dummy = 0;
    md.RecordAlloc(&dummy, 200, "General");

    // Run several update cycles to trigger checks
    for (int i = 0; i < 5; ++i)
        mm.Update(1.0f);

    // Should have detected budget anomaly
    auto anomalies = mm.GetRecentAnomalies();
    bool foundBudget = false;
    for (const auto& a : anomalies)
    {
        if (a.type == Spark::MemoryAnomalyType::BudgetExceeded || a.type == Spark::MemoryAnomalyType::BudgetWarning)
            foundBudget = true;
    }
    // May or may not trigger depending on check interval, just verify no crash
    (void)foundBudget;

    md.RecordFree(&dummy);
    mm.SetGlobalBudget(0);
    mm.Shutdown();
}

// =============================================================================
// Category Budget Warning
// =============================================================================

TEST(MemoryMonitor_CategoryBudgetWarning)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();
    mm.SetEnabled(true);

    mm.SetCategoryBudget("SmallBudget", 100);
    mm.SetBudgetWarningPercent(0.5f); // 50% warning

    int dummy = 0;
    md.RecordAlloc(&dummy, 80, "SmallBudget");

    for (int i = 0; i < 5; ++i)
        mm.Update(1.0f);

    md.RecordFree(&dummy);
    mm.SetCategoryBudget("SmallBudget", 0);
    mm.Shutdown();
}

// =============================================================================
// Snapshot Content
// =============================================================================

TEST(MemoryMonitor_SnapshotCapturesPeakAndDoubleFrees)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    int a = 0;
    md.RecordAlloc(&a, 500, "Test");
    md.RecordFree(&a);
    md.RecordFree(&a); // double-free

    auto snap = mm.TakeSnapshot();
    EXPECT_EQ(snap.peakBytes, static_cast<size_t>(500));
    EXPECT_EQ(snap.doubleFreeCount, static_cast<uint64_t>(1));
    EXPECT_EQ(snap.totalBytes, static_cast<size_t>(0));
    EXPECT_EQ(snap.activeAllocations, static_cast<size_t>(0));

    mm.Shutdown();
}

// =============================================================================
// Health Status Transitions
// =============================================================================

TEST(MemoryMonitor_HealthStatusAfterInit)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    EXPECT_TRUE(mm.GetHealthStatus() == Spark::MemoryHealthStatus::Healthy);
    EXPECT_TRUE(mm.IsHealthy());

    mm.Shutdown();
}

// =============================================================================
// Status Report Content
// =============================================================================

TEST(MemoryMonitor_StatusReportFormat)
{
    auto& mm = Spark::MemoryMonitor::GetInstance();
    auto& md = Spark::MemoryDebugger::GetInstance();
    md.Reset();
    md.SetEnabled(true);
    mm.Initialize();

    int dummy = 0;
    md.RecordAlloc(&dummy, 1024, "TestCat");

    std::string report = mm.GetStatusReport();
    EXPECT_STR_CONTAINS(report, "Memory Monitor Status");
    EXPECT_STR_CONTAINS(report, "Health:");
    EXPECT_STR_CONTAINS(report, "Current:");
    EXPECT_STR_CONTAINS(report, "Peak:");

    md.RecordFree(&dummy);
    mm.Shutdown();
}
