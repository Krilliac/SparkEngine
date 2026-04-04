// TestTelemetry.cpp - Tests for Spark::TelemetrySystem
#include "TestFramework.h"
#include "Utils/Telemetry.h"

// ============================================================================
// Initialize with default config
// ============================================================================

TEST(Telemetry_Initialize_DefaultConfig)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    telemetry.Initialize(cfg);

    EXPECT_EQ(telemetry.GetQueueSize(), 0u);
    EXPECT_TRUE(telemetry.HasConsent());

    telemetry.Shutdown();
}

TEST(Telemetry_Initialize_DisabledByDefault)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    telemetry.Initialize(cfg);

    EXPECT_FALSE(telemetry.GetConfig().enabled);
    EXPECT_FALSE(telemetry.HasConsent());

    telemetry.Shutdown();
}

// ============================================================================
// RecordEvent increments queue size
// ============================================================================

TEST(Telemetry_RecordEvent_IncrementsQueueSize)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    telemetry.Initialize(cfg);

    telemetry.RecordEvent("test_event");
    EXPECT_EQ(telemetry.GetQueueSize(), 1u);

    telemetry.RecordEvent("test_event_2");
    EXPECT_EQ(telemetry.GetQueueSize(), 2u);

    telemetry.Shutdown();
}

TEST(Telemetry_RecordEvent_WithProperties)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    telemetry.Initialize(cfg);

    std::unordered_map<std::string, std::string> props = {{"level", "5"}, {"score", "100"}};
    telemetry.RecordEvent("level_complete", props);
    EXPECT_EQ(telemetry.GetQueueSize(), 1u);

    telemetry.Shutdown();
}

// ============================================================================
// Events are dropped when consent not given
// ============================================================================

TEST(Telemetry_RecordEvent_NoConsent_DropsEvent)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = false;
    telemetry.Initialize(cfg);

    telemetry.RecordEvent("should_be_dropped");
    EXPECT_EQ(telemetry.GetQueueSize(), 0u);

    telemetry.Shutdown();
}

TEST(Telemetry_RecordEvent_Disabled_DropsEvent)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = false;
    cfg.consentGiven = true;
    telemetry.Initialize(cfg);

    telemetry.RecordEvent("should_be_dropped");
    EXPECT_EQ(telemetry.GetQueueSize(), 0u);

    telemetry.Shutdown();
}

// ============================================================================
// SetConsent(false) clears queue
// ============================================================================

TEST(Telemetry_SetConsent_False_ClearsQueue)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    telemetry.Initialize(cfg);

    telemetry.RecordEvent("event_1");
    telemetry.RecordEvent("event_2");
    telemetry.RecordEvent("event_3");
    EXPECT_EQ(telemetry.GetQueueSize(), 3u);

    telemetry.SetConsent(false);
    EXPECT_EQ(telemetry.GetQueueSize(), 0u);
    EXPECT_FALSE(telemetry.HasConsent());

    telemetry.Shutdown();
}

TEST(Telemetry_SetConsent_True_AllowsRecording)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = false;
    telemetry.Initialize(cfg);

    telemetry.RecordEvent("dropped");
    EXPECT_EQ(telemetry.GetQueueSize(), 0u);

    telemetry.SetConsent(true);
    telemetry.RecordEvent("recorded");
    EXPECT_EQ(telemetry.GetQueueSize(), 1u);

    telemetry.Shutdown();
}

// ============================================================================
// FlushEvents empties queue
// ============================================================================

TEST(Telemetry_FlushEvents_EmptiesQueue_WithBackend)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    cfg.localExportPath = "/tmp/spark_telemetry_test";
    telemetry.Initialize(cfg);

    telemetry.RecordEvent("event_a");
    telemetry.RecordEvent("event_b");
    EXPECT_EQ(telemetry.GetQueueSize(), 2u);

    telemetry.FlushEvents();
    EXPECT_EQ(telemetry.GetQueueSize(), 0u);

    telemetry.Shutdown();
}

TEST(Telemetry_FlushEvents_NoBackend_QueueRetained)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    // No localExportPath and no backend registered
    telemetry.Initialize(cfg);

    telemetry.RecordEvent("event_a");
    EXPECT_EQ(telemetry.GetQueueSize(), 1u);

    // FlushEvents with no backend does nothing
    telemetry.FlushEvents();
    EXPECT_EQ(telemetry.GetQueueSize(), 1u);

    telemetry.Shutdown();
}

// ============================================================================
// GetQueueSize accuracy
// ============================================================================

TEST(Telemetry_GetQueueSize_Accuracy)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    telemetry.Initialize(cfg);

    EXPECT_EQ(telemetry.GetQueueSize(), 0u);

    for (uint32_t i = 0; i < 10; ++i)
    {
        telemetry.RecordEvent("event_" + std::to_string(i));
    }
    EXPECT_EQ(telemetry.GetQueueSize(), 10u);

    telemetry.Shutdown();
}

TEST(Telemetry_GetQueueSize_AfterShutdown_Zero)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    telemetry.Initialize(cfg);

    telemetry.RecordEvent("event");
    telemetry.Shutdown();

    EXPECT_EQ(telemetry.GetQueueSize(), 0u);
}

// ============================================================================
// Console_GetStatus
// ============================================================================

TEST(Telemetry_Console_GetStatus_Initialized)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    telemetry.Initialize(cfg);

    auto status = telemetry.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    EXPECT_STR_CONTAINS(status, "initialized");
    EXPECT_STR_CONTAINS(status, "Consent: yes");

    telemetry.Shutdown();
}

TEST(Telemetry_Console_GetStatus_NotInitialized)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    telemetry.Shutdown();

    auto status = telemetry.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    EXPECT_STR_CONTAINS(status, "not initialized");
}
