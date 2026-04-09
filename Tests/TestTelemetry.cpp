// TestTelemetry.cpp - Tests for Spark::TelemetrySystem
#include "TestFramework.h"
#include "Utils/Telemetry.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    class CountingTelemetryBackend final : public Spark::ITelemetryBackend
    {
      public:
        bool Send(const std::vector<Spark::TelemetryEvent>& events) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_batches++;
            m_totalEvents += events.size();

            uint64_t previous = 0;
            for (const auto& event : events)
            {
                if (previous != 0)
                    m_nonDecreasingOrder = m_nonDecreasingOrder && event.sequence >= previous;
                previous = event.sequence;
            }

            return true;
        }

        std::string_view GetBackendName() const override { return "Counting"; }

        size_t GetTotalEvents() const { return m_totalEvents; }
        size_t GetBatchCount() const { return m_batches; }
        bool IsOrderNonDecreasing() const { return m_nonDecreasingOrder; }

      private:
        mutable std::mutex m_mutex;
        size_t m_totalEvents = 0;
        size_t m_batches = 0;
        bool m_nonDecreasingOrder = true;
    };
} // namespace

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

TEST(Telemetry_ConcurrentRecordAndFlush_DeterministicCountAndOrder)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    Spark::TelemetryConfig cfg;
    cfg.enabled = true;
    cfg.consentGiven = true;
    cfg.batchSize = 64;
    cfg.maxQueueSize = 5000;
    telemetry.Initialize(cfg);

    auto backend = std::make_unique<CountingTelemetryBackend>();
    auto* backendPtr = backend.get();
    telemetry.RegisterBackend(std::move(backend));

    constexpr int kThreadCount = 6;
    constexpr int kEventsPerThread = 300;
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i)
    {
        workers.emplace_back(
            [&, threadIndex = i]()
            {
                for (int j = 0; j < kEventsPerThread; ++j)
                {
                    std::unordered_map<std::string, std::string> props{{"producer", std::to_string(threadIndex)},
                                                                       {"index", std::to_string(j)}};
                    telemetry.RecordEvent("concurrent_event", props);
                }
            });
    }

    for (auto& worker : workers)
        worker.join();

    EXPECT_EQ(telemetry.GetQueueSize(), static_cast<uint32_t>(kThreadCount * kEventsPerThread));

    telemetry.FlushEvents();

    EXPECT_EQ(telemetry.GetQueueSize(), 0u);
    EXPECT_EQ(backendPtr->GetTotalEvents(), static_cast<size_t>(kThreadCount * kEventsPerThread));
    EXPECT_TRUE(backendPtr->GetBatchCount() > 0);
    EXPECT_TRUE(backendPtr->IsOrderNonDecreasing());

    telemetry.Shutdown();
}
