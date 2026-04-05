// TestMemoryIntegrity.cpp - Tests for memory integrity and branch protection

#include "TestFramework.h"
#include "Engine/Security/MemoryIntegrity.h"
#include <cstring>
#include <vector>

using namespace Spark::Security;

// =============================================================================
// Helpers
// =============================================================================

/// Resettable test buffer for region scanning
static uint8_t g_testBuffer[256];

static void ResetTestState()
{
    auto& sys = MemoryIntegritySystem::GetInstance();
    sys.Shutdown();

    // Fill buffer with known pattern
    for (size_t i = 0; i < sizeof(g_testBuffer); ++i)
        g_testBuffer[i] = static_cast<uint8_t>(i & 0xFF);

    IntegrityConfig config;
    config.autoDiscoverRegions = false; // Don't scan real code pages in tests
    config.enabled = true;
    config.scanIntervalSec = 999.0f; // We'll scan manually
    sys.Configure(config);
    sys.Initialize();
}

// =============================================================================
// Code Region Registration and Scanning
// =============================================================================

TEST(MemoryIntegrity_RegisterAndScanClean)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    sys.RegisterCodeRegion("test_buffer", g_testBuffer, sizeof(g_testBuffer));
    bool clean = sys.ScanAllRegions();
    EXPECT_TRUE(clean);
    EXPECT_EQ(sys.GetViolations().size(), size_t{0});

    sys.Shutdown();
}

TEST(MemoryIntegrity_DetectModifiedRegion)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    sys.RegisterCodeRegion("test_buffer", g_testBuffer, sizeof(g_testBuffer));

    // Verify initially clean
    EXPECT_TRUE(sys.ScanAllRegions());

    // Tamper with the buffer (simulates NOP patching)
    g_testBuffer[10] = 0x90; // NOP
    g_testBuffer[11] = 0x90;
    g_testBuffer[12] = 0x90;

    // Scan should detect the modification
    bool clean = sys.ScanAllRegions();
    EXPECT_FALSE(clean);
    EXPECT_EQ(sys.GetViolations().size(), size_t{1});
    EXPECT_TRUE(sys.GetViolations()[0].type == ViolationType::CodeModified);
    EXPECT_TRUE(sys.GetViolations()[0].severity == ViolationSeverity::Critical);

    sys.Shutdown();
}

TEST(MemoryIntegrity_RegisterFunction)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    // Register a function (uses same underlying mechanism as RegisterCodeRegion)
    sys.RegisterFunction("test_func", g_testBuffer, 64);
    EXPECT_TRUE(sys.ScanAllRegions());

    auto metrics = sys.GetMetrics();
    EXPECT_EQ(metrics.regionsMonitored, uint32_t{1});

    sys.Shutdown();
}

TEST(MemoryIntegrity_NullRegionIgnored)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    sys.RegisterCodeRegion("null_region", nullptr, 0);
    sys.RegisterCodeRegion("zero_size", g_testBuffer, 0);

    auto metrics = sys.GetMetrics();
    EXPECT_EQ(metrics.regionsMonitored, uint32_t{0});

    sys.Shutdown();
}

// =============================================================================
// Batch Scanning
// =============================================================================

TEST(MemoryIntegrity_BatchScanning)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    // Register multiple regions from the same buffer at different offsets
    sys.RegisterCodeRegion("region_0", g_testBuffer, 64);
    sys.RegisterCodeRegion("region_1", g_testBuffer + 64, 64);
    sys.RegisterCodeRegion("region_2", g_testBuffer + 128, 64);
    sys.RegisterCodeRegion("region_3", g_testBuffer + 192, 64);

    // Batch of 2 should only scan first 2 regions
    EXPECT_TRUE(sys.ScanRegionBatch(2));

    auto metrics = sys.GetMetrics();
    EXPECT_EQ(metrics.regionsMonitored, uint32_t{4});
    EXPECT_EQ(metrics.scanCount, uint32_t{1});

    // Tamper with region_3
    g_testBuffer[200] = 0x90;

    // Batch of 2 starting from cursor=2 should hit region_2 and region_3
    bool clean = sys.ScanRegionBatch(2);
    EXPECT_FALSE(clean);
    EXPECT_EQ(sys.GetViolations().size(), size_t{1});

    sys.Shutdown();
}

// =============================================================================
// Branch Guards — Normal Execution
// =============================================================================

TEST(MemoryIntegrity_BranchGuardNormal)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    constexpr uint64_t branchId = Spark::FNV1a64("test_branch");

    sys.RegisterBranchGuard(branchId, "test_branch");
    sys.RecordBranchExecution(branchId, 1); // Path 1 taken
    sys.VerifyBranchExecuted(branchId);

    // No violations — branch was executed
    EXPECT_EQ(sys.GetViolations().size(), size_t{0});
    EXPECT_FALSE(sys.WasBranchBypassed(branchId));

    sys.Shutdown();
}

TEST(MemoryIntegrity_BranchGuardBypassed)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    constexpr uint64_t branchId = Spark::FNV1a64("bypassed_branch");

    sys.RegisterBranchGuard(branchId, "bypassed_branch");
    // Do NOT call RecordBranchExecution — simulates NOP'd branch
    sys.VerifyBranchExecuted(branchId);

    // Should detect the bypass
    EXPECT_EQ(sys.GetViolations().size(), size_t{1});
    EXPECT_TRUE(sys.GetViolations()[0].type == ViolationType::BranchBypassed);
    EXPECT_TRUE(sys.WasBranchBypassed(branchId));

    sys.Shutdown();
}

TEST(MemoryIntegrity_BranchGuardRepeatedBypassEscalates)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    constexpr uint64_t branchId = Spark::FNV1a64("escalation_branch");

    sys.RegisterBranchGuard(branchId, "escalation_branch");

    // Bypass 3 times — severity should escalate to Critical
    sys.VerifyBranchExecuted(branchId); // bypass 1 = Warning
    sys.VerifyBranchExecuted(branchId); // bypass 2 = Warning
    sys.VerifyBranchExecuted(branchId); // bypass 3 = Critical

    EXPECT_EQ(sys.GetViolations().size(), size_t{3});
    EXPECT_TRUE(sys.GetViolations()[0].severity == ViolationSeverity::Warning);
    EXPECT_TRUE(sys.GetViolations()[2].severity == ViolationSeverity::Critical);

    sys.Shutdown();
}

TEST(MemoryIntegrity_BranchGuardUnknownId)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    // Verify an ID that was never registered or executed
    sys.VerifyBranchExecuted(0xDEADBEEF);

    EXPECT_EQ(sys.GetViolations().size(), size_t{1});
    EXPECT_TRUE(sys.GetViolations()[0].type == ViolationType::BranchBypassed);
    EXPECT_TRUE(sys.GetViolations()[0].severity == ViolationSeverity::Critical);

    sys.Shutdown();
}

TEST(MemoryIntegrity_BranchGuardResetsPerCycle)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    constexpr uint64_t branchId = Spark::FNV1a64("cycle_branch");

    sys.RegisterBranchGuard(branchId, "cycle_branch");

    // Cycle 1: execute then verify — clean
    sys.RecordBranchExecution(branchId, 1);
    sys.VerifyBranchExecuted(branchId);
    EXPECT_EQ(sys.GetViolations().size(), size_t{0});

    // Cycle 2: execute then verify — still clean
    sys.RecordBranchExecution(branchId, 1);
    sys.VerifyBranchExecuted(branchId);
    EXPECT_EQ(sys.GetViolations().size(), size_t{0});

    // Cycle 3: skip execution — bypass detected
    sys.VerifyBranchExecuted(branchId);
    EXPECT_EQ(sys.GetViolations().size(), size_t{1});

    sys.Shutdown();
}

// =============================================================================
// Violation Callback
// =============================================================================

TEST(MemoryIntegrity_ViolationCallback)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    int callbackCount = 0;
    ViolationType lastType{};

    sys.SetViolationCallback(
        [&](const Violation& v)
        {
            ++callbackCount;
            lastType = v.type;
        });

    sys.RegisterCodeRegion("cb_test", g_testBuffer, sizeof(g_testBuffer));

    // Tamper and scan
    g_testBuffer[0] = 0xFF;
    sys.ScanAllRegions();

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(lastType == ViolationType::CodeModified);

    sys.Shutdown();
}

// =============================================================================
// Metrics Tracking
// =============================================================================

TEST(MemoryIntegrity_MetricsTracking)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    sys.RegisterCodeRegion("metrics_r1", g_testBuffer, 64);
    sys.RegisterCodeRegion("metrics_r2", g_testBuffer + 64, 64);

    constexpr uint64_t branchId = Spark::FNV1a64("metrics_branch");
    sys.RegisterBranchGuard(branchId, "metrics_branch");

    auto m = sys.GetMetrics();
    EXPECT_EQ(m.regionsMonitored, uint32_t{2});
    EXPECT_EQ(m.branchGuardsRegistered, uint32_t{1});
    EXPECT_EQ(m.scanCount, uint32_t{0});

    sys.ScanAllRegions();
    m = sys.GetMetrics();
    EXPECT_EQ(m.scanCount, uint32_t{1});

    sys.Shutdown();
}

// =============================================================================
// Checkpoint Macros (integration test)
// =============================================================================

TEST(MemoryIntegrity_CheckpointMacros)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    // Simulate what the macros do
    constexpr uint64_t cpId = Spark::FNV1a64("test_checkpoint");
    sys.RegisterBranchGuard(cpId, "test_checkpoint");
    sys.RecordBranchExecution(cpId, 1);
    sys.VerifyBranchExecuted(cpId);

    EXPECT_EQ(sys.GetViolations().size(), size_t{0});

    sys.Shutdown();
}

TEST(MemoryIntegrity_AutoRegisterOnRecord)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    // Record without explicit registration — should auto-register
    constexpr uint64_t branchId = Spark::FNV1a64("auto_reg");
    sys.RecordBranchExecution(branchId, 1);
    sys.VerifyBranchExecuted(branchId);

    EXPECT_EQ(sys.GetViolations().size(), size_t{0});

    sys.Shutdown();
}

// =============================================================================
// Status String
// =============================================================================

TEST(MemoryIntegrity_StatusString)
{
    ResetTestState();
    auto& sys = MemoryIntegritySystem::GetInstance();

    sys.RegisterCodeRegion("status_test", g_testBuffer, 64);
    std::string status = sys.GetStatusString();

    EXPECT_TRUE(status.find("Memory Integrity") != std::string::npos);
    EXPECT_TRUE(status.find("Regions monitored: 1") != std::string::npos);

    sys.Shutdown();
}

// =============================================================================
// Update-Driven Scanning
// =============================================================================

TEST(MemoryIntegrity_UpdateTriggersPeriodicScan)
{
    auto& sys = MemoryIntegritySystem::GetInstance();
    sys.Shutdown();

    // Reset buffer
    for (size_t i = 0; i < sizeof(g_testBuffer); ++i)
        g_testBuffer[i] = static_cast<uint8_t>(i & 0xFF);

    IntegrityConfig config;
    config.autoDiscoverRegions = false;
    config.enabled = true;
    config.scanIntervalSec = 1.0f;
    config.batchSize = 4;
    sys.Configure(config);
    sys.Initialize();

    sys.RegisterCodeRegion("update_test", g_testBuffer, sizeof(g_testBuffer));

    // Not enough time elapsed — no scan
    sys.Update(0.5f);
    EXPECT_EQ(sys.GetMetrics().scanCount, uint32_t{0});

    // Now enough time has passed
    sys.Update(0.6f);
    EXPECT_EQ(sys.GetMetrics().scanCount, uint32_t{1});

    sys.Shutdown();
}
