// TestTelemetrySpool.cpp - Durable telemetry delivery and hostile spool coverage
#include "TestFramework.h"
#include "Utils/Telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kSpoolArtifact = "spark-telemetry.spool";
    constexpr const char* kSpoolStaging = "spark-telemetry.spool.tmp";

    class TempTelemetrySpool final
    {
      public:
        explicit TempTelemetrySpool(const char* tag)
        {
            static std::atomic<uint64_t> sequence{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            // macOS exposes its temporary directory through /var, which is a
            // system symlink to /private/var. Resolve that trusted platform
            // alias before constructing fixtures so the tests still exercise
            // only the explicit hostile symlinks created below.
            const fs::path tempRoot = fs::canonical(fs::temp_directory_path());
            m_root = tempRoot / (std::string("spark-telemetry-") + tag + "-" +
                                 std::to_string(++sequence) + "-" + std::to_string(stamp));
            fs::create_directories(m_root);
        }

        ~TempTelemetrySpool()
        {
            std::error_code error;
            fs::remove_all(m_root, error);
        }

        const fs::path& Root() const { return m_root; }
        fs::path Artifact() const { return m_root / kSpoolArtifact; }
        fs::path Staging() const { return m_root / kSpoolStaging; }

        void WriteArtifact(const std::string& bytes) const
        {
            std::ofstream output(Artifact(), std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

      private:
        fs::path m_root;
    };

    struct BackendState final
    {
        void Capture(const std::vector<Spark::TelemetryEvent>& events)
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++calls;
            attemptedEvents.insert(attemptedEvents.end(), events.begin(), events.end());
        }

        size_t CallCount() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return calls;
        }

        std::vector<Spark::TelemetryEvent> Events() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return attemptedEvents;
        }

        mutable std::mutex mutex;
        size_t calls = 0;
        std::vector<Spark::TelemetryEvent> attemptedEvents;
    };

    class FixedResultTelemetryBackend final : public Spark::ITelemetryBackend
    {
      public:
        FixedResultTelemetryBackend(Spark::TelemetryDeliveryResult result, std::shared_ptr<BackendState> state)
            : m_result(result), m_state(std::move(state))
        {
        }

        Spark::TelemetryDeliveryResult Send(const std::vector<Spark::TelemetryEvent>& events) override
        {
            m_state->Capture(events);
            return m_result;
        }

        std::string_view GetBackendName() const override { return "FixedResult"; }

      private:
        Spark::TelemetryDeliveryResult m_result;
        std::shared_ptr<BackendState> m_state;
    };

    class TelemetryReset final
    {
      public:
        explicit TelemetryReset(Spark::TelemetrySystem& telemetry) : m_telemetry(telemetry) {}

        ~TelemetryReset()
        {
            if (m_telemetry.IsInitialized())
            {
                m_telemetry.SetConsent(false);
                m_telemetry.Shutdown();
            }
        }

      private:
        Spark::TelemetrySystem& m_telemetry;
    };

    class WorkingDirectoryGuard final
    {
      public:
        explicit WorkingDirectoryGuard(const fs::path& replacement) : m_previous(fs::current_path())
        {
            fs::current_path(replacement);
        }
        ~WorkingDirectoryGuard()
        {
            std::error_code ignored;
            fs::current_path(m_previous, ignored);
        }

      private:
        fs::path m_previous;
    };

    Spark::TelemetryConfig MakeSpoolConfig(const fs::path& directory)
    {
        Spark::TelemetryConfig config;
        config.enabled = true;
        config.consentGiven = true;
        config.batchSize = 16;
        config.maxQueueSize = 64;
        config.spoolDirectory = directory.string();
        config.maxSpoolBytes = 1024 * 1024;
        config.maxSpoolEvents = 64;
        config.retryIntervalSeconds = 0.01f;
        return config;
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    uint32_t ReadSpoolEventCount(const fs::path& path)
    {
        const std::string bytes = ReadFile(path);
        EXPECT_TRUE(bytes.size() >= 16u);
        if (bytes.size() < 16u)
            return 0;

        uint32_t count = 0;
        for (unsigned index = 0; index < 4; ++index)
        {
            count |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[12 + index])) << (index * 8);
        }
        return count;
    }

    void ExerciseRejectedArtifact(const Spark::TelemetryConfig& config, const std::shared_ptr<BackendState>& state)
    {
        auto& telemetry = Spark::TelemetrySystem::GetInstance();
        telemetry.Initialize(config);
        telemetry.RegisterBackend(
            std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, state));

        const auto restored = telemetry.GetDeliveryStats();
        EXPECT_EQ(restored.queuedEvents, 0u);
        EXPECT_EQ(restored.spooledEvents, 0u);
        EXPECT_EQ(restored.deliveredEvents, 0u);
        EXPECT_EQ(restored.rejectedEvents, 0u);
        EXPECT_EQ(restored.backendDeliveryAttempts, 0u);
        EXPECT_EQ(restored.retryableFailedEvents, 0u);
        EXPECT_EQ(restored.spoolIoFailures, 0u);
        EXPECT_GT(restored.spoolRejectedOperations, 0u);
        EXPECT_EQ(restored.droppedEvents, 0u);

        telemetry.FlushEvents();
        EXPECT_EQ(state->CallCount(), 0u);
        telemetry.Shutdown();
    }
} // namespace

TEST(Telemetry_SpoolRecovery)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    TelemetryReset reset(telemetry);
    TempTelemetrySpool spool("recovery");
    const auto config = MakeSpoolConfig(spool.Root());

    telemetry.Initialize(config);
    auto retryState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::RetryableFailure, retryState));
    telemetry.RecordEvent("recovery.first");
    telemetry.RecordEvent("recovery.second");
    telemetry.FlushEvents();

    const auto failed = telemetry.GetDeliveryStats();
    EXPECT_EQ(failed.queuedEvents, 2u);
    EXPECT_EQ(failed.spooledEvents, 2u);
    EXPECT_EQ(failed.deliveredEvents, 0u);
    EXPECT_EQ(failed.rejectedEvents, 0u);
    EXPECT_EQ(failed.backendDeliveryAttempts, retryState->CallCount());
    EXPECT_EQ(failed.retryableFailedEvents, 2u);
    EXPECT_EQ(failed.spoolIoFailures, 0u);
    EXPECT_EQ(failed.spoolRejectedOperations, 0u);
    EXPECT_EQ(failed.droppedEvents, 0u);
    EXPECT_TRUE(fs::is_regular_file(spool.Artifact()));
    EXPECT_GT(fs::file_size(spool.Artifact()), 0u);

    telemetry.Shutdown();
    EXPECT_TRUE(fs::is_regular_file(spool.Artifact()));
    const auto shutdownCarryover = telemetry.GetDeliveryStats();
    EXPECT_EQ(shutdownCarryover.queuedEvents, 2u);
    EXPECT_EQ(shutdownCarryover.queuedBytes, failed.queuedBytes);
    EXPECT_EQ(shutdownCarryover.carryoverEvents, 2u);
    EXPECT_EQ(shutdownCarryover.carryoverBytes, failed.queuedBytes);

    // Simulate loss of the durable image between sessions. The unique bounded
    // carryover must be recommitted during Initialize, before any explicit flush.
    ASSERT_TRUE(fs::remove(spool.Artifact()));
    EXPECT_FALSE(fs::exists(spool.Artifact()));

    telemetry.Initialize(config);
    const auto restored = telemetry.GetDeliveryStats();
    EXPECT_EQ(restored.queuedEvents, 2u);
    EXPECT_EQ(restored.carryoverEvents, 0u);
    EXPECT_EQ(restored.carryoverBytes, 0u);
    EXPECT_EQ(restored.spooledEvents, 2u);
    EXPECT_EQ(restored.deliveredEvents, 0u);
    EXPECT_EQ(restored.backendDeliveryAttempts, 0u);
    EXPECT_EQ(restored.retryableFailedEvents, 0u);
    EXPECT_EQ(restored.spoolIoFailures, 0u);
    EXPECT_EQ(restored.spoolRejectedOperations, 0u);
    EXPECT_TRUE(fs::is_regular_file(spool.Artifact()));
    EXPECT_EQ(ReadSpoolEventCount(spool.Artifact()), 2u);

    auto deliveredState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, deliveredState));
    telemetry.FlushEvents();

    const auto deliveredEvents = deliveredState->Events();
    EXPECT_EQ(deliveredState->CallCount(), 1u);
    ASSERT_EQ(deliveredEvents.size(), 2u);
    EXPECT_TRUE(std::is_sorted(deliveredEvents.begin(), deliveredEvents.end(),
                               [](const auto& lhs, const auto& rhs) { return lhs.sequence < rhs.sequence; }));
    EXPECT_EQ(deliveredEvents[0].name, std::string("recovery.first"));
    EXPECT_EQ(deliveredEvents[1].name, std::string("recovery.second"));
    EXPECT_LT(deliveredEvents[0].sequence, deliveredEvents[1].sequence);

    const auto succeeded = telemetry.GetDeliveryStats();
    EXPECT_EQ(succeeded.queuedEvents, 0u);
    EXPECT_EQ(succeeded.spooledEvents, 0u);
    EXPECT_EQ(succeeded.deliveredEvents, deliveredEvents.size());
    EXPECT_EQ(succeeded.rejectedEvents, 0u);
    EXPECT_EQ(succeeded.backendDeliveryAttempts, 1u);
    EXPECT_EQ(succeeded.retryableFailedEvents, 0u);
    EXPECT_EQ(succeeded.spoolIoFailures, 0u);
    EXPECT_EQ(succeeded.spoolRejectedOperations, 0u);
    EXPECT_EQ(succeeded.droppedEvents, 0u);
    EXPECT_FALSE(fs::exists(spool.Artifact()));
}

TEST(Telemetry_SpoolRecovery_RejectedIsTerminal)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    TelemetryReset reset(telemetry);
    TempTelemetrySpool spool("rejected");
    auto config = MakeSpoolConfig(spool.Root());
    config.batchSize = 2;

    telemetry.Initialize(config);
    auto rejectedState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Rejected, rejectedState));
    telemetry.RecordEvent("rejected.first");
    telemetry.RecordEvent("rejected.second");
    telemetry.RecordEvent("rejected.third");
    telemetry.FlushEvents();

    const auto rejected = telemetry.GetDeliveryStats();
    const auto rejectedEvents = rejectedState->Events();
    EXPECT_EQ(rejectedState->CallCount(), 2u);
    ASSERT_EQ(rejectedEvents.size(), 3u);
    EXPECT_EQ(rejected.queuedEvents, 0u);
    EXPECT_EQ(rejected.spooledEvents, 0u);
    EXPECT_EQ(rejected.deliveredEvents, 0u);
    EXPECT_EQ(rejected.rejectedEvents, rejectedEvents.size());
    EXPECT_EQ(rejected.backendDeliveryAttempts, rejectedState->CallCount());
    EXPECT_EQ(rejected.retryableFailedEvents, 0u);
    EXPECT_EQ(rejected.spoolIoFailures, 0u);
    EXPECT_EQ(rejected.spoolRejectedOperations, 0u);
    EXPECT_EQ(rejected.droppedEvents, 0u);
    EXPECT_FALSE(fs::exists(spool.Artifact()));

    telemetry.Shutdown();
    telemetry.Initialize(config);
    auto replayState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, replayState));
    telemetry.FlushEvents();
    EXPECT_EQ(replayState->CallCount(), 0u);
    const auto restored = telemetry.GetDeliveryStats();
    EXPECT_EQ(restored.queuedEvents, 0u);
    EXPECT_EQ(restored.spooledEvents, 0u);
    EXPECT_EQ(restored.backendDeliveryAttempts, 0u);
    EXPECT_EQ(restored.retryableFailedEvents, 0u);
}

TEST(Telemetry_SpoolRecovery_UpdateRetriesAtBoundary)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    TelemetryReset reset(telemetry);
    TempTelemetrySpool spool("retry-boundary");
    auto config = MakeSpoolConfig(spool.Root());
    config.flushIntervalSeconds = 100.0f;
    config.retryIntervalSeconds = 1.0f;

    telemetry.Initialize(config);
    auto retryState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::RetryableFailure, retryState));
    telemetry.RecordEvent("retry.boundary");
    telemetry.FlushEvents();
    EXPECT_EQ(retryState->CallCount(), 1u);

    auto deliveredState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, deliveredState));
    telemetry.Update(0.5f);
    EXPECT_EQ(deliveredState->CallCount(), 0u);
    EXPECT_EQ(telemetry.GetDeliveryStats().queuedEvents, 1u);

    telemetry.Update(0.5f);
    EXPECT_EQ(deliveredState->CallCount(), 1u);
    const auto delivered = telemetry.GetDeliveryStats();
    EXPECT_EQ(delivered.queuedEvents, 0u);
    EXPECT_EQ(delivered.spooledEvents, 0u);
    EXPECT_EQ(delivered.deliveredEvents, 1u);
    EXPECT_EQ(delivered.rejectedEvents, 0u);
    EXPECT_EQ(delivered.backendDeliveryAttempts, 2u);
    EXPECT_EQ(delivered.retryableFailedEvents, 1u);
    EXPECT_EQ(delivered.spoolIoFailures, 0u);
    EXPECT_EQ(delivered.spoolRejectedOperations, 0u);
    EXPECT_EQ(delivered.droppedEvents, 0u);
    EXPECT_FALSE(fs::exists(spool.Artifact()));
}

TEST(Telemetry_SpoolRecovery_CapDropAccounting)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    TelemetryReset reset(telemetry);
    TempTelemetrySpool eventCapSpool("event-cap");
    auto eventCapConfig = MakeSpoolConfig(eventCapSpool.Root());
    eventCapConfig.maxQueueBytes = 1024 * 1024;
    eventCapConfig.maxSpoolEvents = 2;

    telemetry.Initialize(eventCapConfig);
    auto eventCapState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::RetryableFailure, eventCapState));
    telemetry.RecordEvent("cap.first");
    telemetry.RecordEvent("cap.second");
    telemetry.RecordEvent("cap.dropped");

    const auto queuedAtEventCap = telemetry.GetDeliveryStats();
    EXPECT_EQ(queuedAtEventCap.queuedEvents, 3u);
    EXPECT_GT(queuedAtEventCap.queuedBytes, 0u);
    EXPECT_EQ(queuedAtEventCap.spooledEvents, 0u);
    EXPECT_EQ(queuedAtEventCap.droppedEvents, 0u);
    EXPECT_EQ(eventCapState->CallCount(), 0u);
    telemetry.FlushEvents();

    const auto eventCapStats = telemetry.GetDeliveryStats();
    EXPECT_EQ(eventCapStats.queuedEvents, 2u);
    EXPECT_GT(eventCapStats.queuedBytes, 0u);
    EXPECT_LT(eventCapStats.queuedBytes, queuedAtEventCap.queuedBytes);
    EXPECT_EQ(eventCapStats.spooledEvents, 2u);
    EXPECT_EQ(eventCapStats.droppedEvents, 1u);
    EXPECT_EQ(eventCapStats.backendDeliveryAttempts, eventCapState->CallCount());
    EXPECT_EQ(eventCapStats.retryableFailedEvents, 2u);
    EXPECT_EQ(eventCapStats.spoolIoFailures, 0u);
    EXPECT_EQ(eventCapStats.spoolRejectedOperations, 0u);
    telemetry.SetConsent(false);
    const auto eventCapRevoked = telemetry.GetDeliveryStats();
    EXPECT_EQ(eventCapRevoked.queuedEvents, 0u);
    EXPECT_EQ(eventCapRevoked.queuedBytes, 0u);
    EXPECT_EQ(eventCapRevoked.spooledEvents, 0u);
    telemetry.Shutdown();

    TempTelemetrySpool queueEventCapSpool("queue-event-cap");
    auto queueEventCapConfig = MakeSpoolConfig(queueEventCapSpool.Root());
    queueEventCapConfig.maxQueueSize = 2;
    queueEventCapConfig.maxQueueBytes = 1024 * 1024;
    telemetry.Initialize(queueEventCapConfig);
    auto queueEventCapState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, queueEventCapState));
    telemetry.RecordEvent("queue.event.first");
    telemetry.RecordEvent("queue.event.second");
    telemetry.RecordEvent("queue.event.dropped");

    const auto queueEventCapBeforeFlush = telemetry.GetDeliveryStats();
    EXPECT_EQ(queueEventCapBeforeFlush.queuedEvents, 2u);
    EXPECT_GT(queueEventCapBeforeFlush.queuedBytes, 0u);
    EXPECT_EQ(queueEventCapBeforeFlush.spooledEvents, 0u);
    EXPECT_EQ(queueEventCapBeforeFlush.droppedEvents, 1u);
    EXPECT_EQ(queueEventCapState->CallCount(), 0u);
    telemetry.FlushEvents();

    const auto deliveredQueueEventCap = queueEventCapState->Events();
    EXPECT_EQ(queueEventCapState->CallCount(), 1u);
    ASSERT_EQ(deliveredQueueEventCap.size(), 2u);
    EXPECT_EQ(deliveredQueueEventCap[0].name, std::string("queue.event.first"));
    EXPECT_EQ(deliveredQueueEventCap[1].name, std::string("queue.event.second"));
    const auto queueEventCapAfterFlush = telemetry.GetDeliveryStats();
    EXPECT_EQ(queueEventCapAfterFlush.queuedEvents, 0u);
    EXPECT_EQ(queueEventCapAfterFlush.queuedBytes, 0u);
    EXPECT_EQ(queueEventCapAfterFlush.spooledEvents, 0u);
    EXPECT_EQ(queueEventCapAfterFlush.deliveredEvents, 2u);
    EXPECT_EQ(queueEventCapAfterFlush.droppedEvents, 1u);
    EXPECT_FALSE(fs::exists(queueEventCapSpool.Artifact()));
    telemetry.SetConsent(false);
    telemetry.Shutdown();

    TempTelemetrySpool queueByteCapSpool("queue-byte-cap");
    auto queueByteCapConfig = MakeSpoolConfig(queueByteCapSpool.Root());
    queueByteCapConfig.maxQueueBytes = 1;
    telemetry.Initialize(queueByteCapConfig);
    auto queueByteCapState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, queueByteCapState));
    const std::string tooLargeEventName(64, 'x');
    telemetry.RecordEvent(tooLargeEventName);

    const auto queueByteCapBeforeFlush = telemetry.GetDeliveryStats();
    EXPECT_EQ(queueByteCapBeforeFlush.queuedEvents, 0u);
    EXPECT_EQ(queueByteCapBeforeFlush.queuedBytes, 0u);
    EXPECT_EQ(queueByteCapBeforeFlush.spooledEvents, 0u);
    EXPECT_EQ(queueByteCapBeforeFlush.droppedEvents, 1u);
    EXPECT_EQ(queueByteCapState->CallCount(), 0u);
    EXPECT_FALSE(fs::exists(queueByteCapSpool.Artifact()));
    telemetry.FlushEvents();

    const auto queueByteCapAfterFlush = telemetry.GetDeliveryStats();
    EXPECT_EQ(queueByteCapAfterFlush.queuedEvents, 0u);
    EXPECT_EQ(queueByteCapAfterFlush.queuedBytes, 0u);
    EXPECT_EQ(queueByteCapAfterFlush.spooledEvents, 0u);
    EXPECT_EQ(queueByteCapAfterFlush.droppedEvents, 1u);
    EXPECT_EQ(queueByteCapState->CallCount(), 0u);
    EXPECT_FALSE(fs::exists(queueByteCapSpool.Artifact()));
    telemetry.SetConsent(false);
    telemetry.Shutdown();

    TempTelemetrySpool byteCapSpool("byte-cap");
    auto byteCapConfig = MakeSpoolConfig(byteCapSpool.Root());
    byteCapConfig.maxSpoolBytes = 16;
    telemetry.Initialize(byteCapConfig);
    auto byteCapState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::RetryableFailure, byteCapState));
    telemetry.RecordEvent("cannot-fit");
    EXPECT_GT(telemetry.GetDeliveryStats().queuedBytes, 0u);
    telemetry.FlushEvents();

    const auto byteCapStats = telemetry.GetDeliveryStats();
    EXPECT_EQ(byteCapStats.queuedEvents, 0u);
    EXPECT_EQ(byteCapStats.queuedBytes, 0u);
    EXPECT_EQ(byteCapStats.spooledEvents, 0u);
    EXPECT_EQ(byteCapStats.droppedEvents, 1u);
    EXPECT_EQ(byteCapStats.backendDeliveryAttempts, 0u);
    EXPECT_EQ(byteCapStats.retryableFailedEvents, 0u);
    EXPECT_EQ(byteCapStats.spoolIoFailures, 0u);
    EXPECT_EQ(byteCapStats.spoolRejectedOperations, 0u);
    EXPECT_EQ(byteCapState->CallCount(), 0u);
    telemetry.SetConsent(false);
    telemetry.Shutdown();

    TempTelemetrySpool eventMergeSpool("merged-event-bound");
    auto generousEventMergeConfig = MakeSpoolConfig(eventMergeSpool.Root());
    generousEventMergeConfig.maxQueueSize = 8;
    generousEventMergeConfig.maxQueueBytes = 1024 * 1024;
    generousEventMergeConfig.maxSpoolEvents = 8;
    telemetry.Initialize(generousEventMergeConfig);
    auto eventMergeRetryState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(std::make_unique<FixedResultTelemetryBackend>(
        Spark::TelemetryDeliveryResult::RetryableFailure, eventMergeRetryState));

    telemetry.RecordEvent("merge.event.oldest");
    telemetry.RecordEvent("merge.event.second");
    telemetry.RecordEvent("merge.event.dropped");
    telemetry.FlushEvents();
    const auto persistedEventCandidates = telemetry.GetDeliveryStats();
    EXPECT_EQ(persistedEventCandidates.queuedEvents, 3u);
    EXPECT_GT(persistedEventCandidates.queuedBytes, 0u);
    EXPECT_EQ(persistedEventCandidates.spooledEvents, 3u);
    EXPECT_EQ(persistedEventCandidates.droppedEvents, 0u);
    EXPECT_EQ(persistedEventCandidates.backendDeliveryAttempts, eventMergeRetryState->CallCount());
    EXPECT_EQ(persistedEventCandidates.retryableFailedEvents, 3u);
    EXPECT_EQ(persistedEventCandidates.spoolIoFailures, 0u);
    EXPECT_EQ(persistedEventCandidates.spoolRejectedOperations, 0u);
    telemetry.Shutdown();
    EXPECT_TRUE(fs::is_regular_file(eventMergeSpool.Artifact()));

    auto tighterEventMergeConfig = MakeSpoolConfig(eventMergeSpool.Root());
    tighterEventMergeConfig.maxQueueSize = 2;
    tighterEventMergeConfig.maxQueueBytes = 1024 * 1024;
    tighterEventMergeConfig.maxSpoolEvents = 3;
    telemetry.Initialize(tighterEventMergeConfig);

    const auto boundedEventMerge = telemetry.GetDeliveryStats();
    EXPECT_EQ(boundedEventMerge.queuedEvents, 2u);
    EXPECT_GT(boundedEventMerge.queuedBytes, 0u);
    EXPECT_EQ(boundedEventMerge.spooledEvents, 2u);
    EXPECT_EQ(boundedEventMerge.droppedEvents, 1u);
    EXPECT_TRUE(fs::is_regular_file(eventMergeSpool.Artifact()));
    EXPECT_EQ(ReadSpoolEventCount(eventMergeSpool.Artifact()), 2u);
    const std::string boundedEventArtifact = ReadFile(eventMergeSpool.Artifact());
    EXPECT_TRUE(boundedEventArtifact.find("merge.event.oldest") != std::string::npos);
    EXPECT_TRUE(boundedEventArtifact.find("merge.event.second") != std::string::npos);
    EXPECT_TRUE(boundedEventArtifact.find("merge.event.dropped") == std::string::npos);

    // The tightened restore must durably publish the bounded image before any
    // explicit flush or delivery. A shutdown/reinitialize cycle cannot revive
    // the event that was removed from that image.
    telemetry.Shutdown();
    EXPECT_TRUE(fs::is_regular_file(eventMergeSpool.Artifact()));
    EXPECT_EQ(ReadSpoolEventCount(eventMergeSpool.Artifact()), 2u);
    telemetry.Initialize(tighterEventMergeConfig);
    const auto reloadedEventMerge = telemetry.GetDeliveryStats();
    EXPECT_EQ(reloadedEventMerge.queuedEvents, 2u);
    EXPECT_GT(reloadedEventMerge.queuedBytes, 0u);
    EXPECT_EQ(reloadedEventMerge.spooledEvents, 2u);
    EXPECT_EQ(reloadedEventMerge.droppedEvents, 0u);
    EXPECT_EQ(ReadSpoolEventCount(eventMergeSpool.Artifact()), 2u);

    auto eventMergeDeliveredState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered,
                                                                            eventMergeDeliveredState));
    telemetry.FlushEvents();
    const auto deliveredEventMerge = eventMergeDeliveredState->Events();
    EXPECT_EQ(eventMergeDeliveredState->CallCount(), 1u);
    ASSERT_EQ(deliveredEventMerge.size(), 2u);
    EXPECT_EQ(deliveredEventMerge[0].name, std::string("merge.event.oldest"));
    EXPECT_EQ(deliveredEventMerge[1].name, std::string("merge.event.second"));
    EXPECT_LT(deliveredEventMerge[0].sequence, deliveredEventMerge[1].sequence);
    const auto completedEventMerge = telemetry.GetDeliveryStats();
    EXPECT_EQ(completedEventMerge.queuedEvents, 0u);
    EXPECT_EQ(completedEventMerge.queuedBytes, 0u);
    EXPECT_EQ(completedEventMerge.spooledEvents, 0u);
    EXPECT_EQ(completedEventMerge.deliveredEvents, 2u);
    EXPECT_EQ(completedEventMerge.droppedEvents, 0u);
    EXPECT_FALSE(fs::exists(eventMergeSpool.Artifact()));
    telemetry.SetConsent(false);
    telemetry.Shutdown();

    TempTelemetrySpool byteMergeSpool("merged-byte-bound");
    auto generousByteMergeConfig = MakeSpoolConfig(byteMergeSpool.Root());
    generousByteMergeConfig.maxQueueSize = 8;
    generousByteMergeConfig.maxQueueBytes = 1024 * 1024;
    generousByteMergeConfig.maxSpoolEvents = 8;
    telemetry.Initialize(generousByteMergeConfig);
    auto byteMergeRetryState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(std::make_unique<FixedResultTelemetryBackend>(
        Spark::TelemetryDeliveryResult::RetryableFailure, byteMergeRetryState));

    telemetry.RecordEvent("merge.byte.oldest");
    const auto oldestOnly = telemetry.GetDeliveryStats();
    EXPECT_EQ(oldestOnly.queuedEvents, 1u);
    EXPECT_GT(oldestOnly.queuedBytes, 0u);
    telemetry.RecordEvent("merge.byte.second");
    const auto retainedPair = telemetry.GetDeliveryStats();
    EXPECT_EQ(retainedPair.queuedEvents, 2u);
    EXPECT_GT(retainedPair.queuedBytes, oldestOnly.queuedBytes);
    telemetry.RecordEvent("merge.byte.dropped");
    const auto allMergedCandidates = telemetry.GetDeliveryStats();
    EXPECT_EQ(allMergedCandidates.queuedEvents, 3u);
    EXPECT_GT(allMergedCandidates.queuedBytes, retainedPair.queuedBytes);

    telemetry.FlushEvents();
    const auto persistedCandidates = telemetry.GetDeliveryStats();
    EXPECT_EQ(persistedCandidates.queuedEvents, 3u);
    EXPECT_EQ(persistedCandidates.queuedBytes, allMergedCandidates.queuedBytes);
    EXPECT_EQ(persistedCandidates.spooledEvents, 3u);
    EXPECT_EQ(persistedCandidates.droppedEvents, 0u);
    EXPECT_EQ(persistedCandidates.backendDeliveryAttempts, byteMergeRetryState->CallCount());
    EXPECT_EQ(persistedCandidates.retryableFailedEvents, 3u);
    EXPECT_EQ(persistedCandidates.spoolIoFailures, 0u);
    EXPECT_EQ(persistedCandidates.spoolRejectedOperations, 0u);
    telemetry.Shutdown();
    EXPECT_TRUE(fs::is_regular_file(byteMergeSpool.Artifact()));

    auto tighterConfig = MakeSpoolConfig(byteMergeSpool.Root());
    tighterConfig.maxQueueSize = 3;
    tighterConfig.maxQueueBytes = retainedPair.queuedBytes;
    tighterConfig.maxSpoolEvents = 3;
    telemetry.Initialize(tighterConfig);

    const auto boundedMerge = telemetry.GetDeliveryStats();
    EXPECT_EQ(boundedMerge.queuedEvents, 2u);
    EXPECT_EQ(boundedMerge.queuedBytes, retainedPair.queuedBytes);
    EXPECT_EQ(boundedMerge.droppedEvents, 1u);

    auto mergeDeliveredState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, mergeDeliveredState));
    telemetry.FlushEvents();
    const auto deliveredMergedEvents = mergeDeliveredState->Events();
    EXPECT_EQ(mergeDeliveredState->CallCount(), 1u);
    ASSERT_EQ(deliveredMergedEvents.size(), 2u);
    EXPECT_EQ(deliveredMergedEvents[0].name, std::string("merge.byte.oldest"));
    EXPECT_EQ(deliveredMergedEvents[1].name, std::string("merge.byte.second"));
    EXPECT_LT(deliveredMergedEvents[0].sequence, deliveredMergedEvents[1].sequence);

    const auto deliveredMerge = telemetry.GetDeliveryStats();
    EXPECT_EQ(deliveredMerge.queuedEvents, 0u);
    EXPECT_EQ(deliveredMerge.queuedBytes, 0u);
    EXPECT_EQ(deliveredMerge.spooledEvents, 0u);
    EXPECT_EQ(deliveredMerge.deliveredEvents, 2u);
    EXPECT_EQ(deliveredMerge.droppedEvents, 1u);
    EXPECT_FALSE(fs::exists(byteMergeSpool.Artifact()));
    telemetry.SetConsent(false);
    telemetry.Shutdown();

    TempTelemetrySpool blockedCarryoverSpool("blocked-carryover-bound");
    auto generousBlockedConfig = MakeSpoolConfig(blockedCarryoverSpool.Root());
    generousBlockedConfig.maxQueueSize = 8;
    generousBlockedConfig.maxQueueBytes = 1024 * 1024;
    generousBlockedConfig.maxSpoolEvents = 8;
    telemetry.Initialize(generousBlockedConfig);
    auto blockedRetryState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(std::make_unique<FixedResultTelemetryBackend>(
        Spark::TelemetryDeliveryResult::RetryableFailure, blockedRetryState));

    telemetry.RecordEvent("blocked.carryover.oldest");
    telemetry.RecordEvent("blocked.carryover.second");
    const uint64_t blockedRetainedBytes = telemetry.GetDeliveryStats().queuedBytes;
    EXPECT_GT(blockedRetainedBytes, 0u);
    telemetry.RecordEvent("blocked.carryover.dropped");
    const uint64_t blockedAllBytes = telemetry.GetDeliveryStats().queuedBytes;
    EXPECT_GT(blockedAllBytes, blockedRetainedBytes);
    telemetry.FlushEvents();
    const auto blockedPersisted = telemetry.GetDeliveryStats();
    EXPECT_EQ(blockedPersisted.queuedEvents, 3u);
    EXPECT_EQ(blockedPersisted.spooledEvents, 3u);
    EXPECT_EQ(blockedPersisted.backendDeliveryAttempts, 1u);
    EXPECT_EQ(blockedPersisted.retryableFailedEvents, 3u);
    EXPECT_EQ(blockedPersisted.droppedEvents, 0u);
    ASSERT_TRUE(fs::is_regular_file(blockedCarryoverSpool.Artifact()));
    const std::string validBlockedArtifact = ReadFile(blockedCarryoverSpool.Artifact());
    EXPECT_EQ(ReadSpoolEventCount(blockedCarryoverSpool.Artifact()), 3u);
    telemetry.Shutdown();
    const auto generousBlockedShutdown = telemetry.GetDeliveryStats();
    EXPECT_EQ(generousBlockedShutdown.queuedEvents, 3u);
    EXPECT_EQ(generousBlockedShutdown.queuedBytes, blockedAllBytes);
    EXPECT_EQ(generousBlockedShutdown.carryoverEvents, 3u);
    EXPECT_EQ(generousBlockedShutdown.carryoverBytes, blockedAllBytes);

    const std::string rejectedArtifact = "not-a-valid-telemetry-spool";
    blockedCarryoverSpool.WriteArtifact(rejectedArtifact);
    auto tightBlockedConfig = MakeSpoolConfig(blockedCarryoverSpool.Root());
    tightBlockedConfig.maxQueueSize = 2;
    tightBlockedConfig.maxQueueBytes = blockedRetainedBytes;
    tightBlockedConfig.maxSpoolEvents = 8;

    telemetry.Initialize(tightBlockedConfig);
    const auto blocked = telemetry.GetDeliveryStats();
    EXPECT_EQ(blocked.queuedEvents, 2u);
    EXPECT_EQ(blocked.queuedBytes, blockedRetainedBytes);
    EXPECT_EQ(blocked.carryoverEvents, 2u);
    EXPECT_EQ(blocked.carryoverBytes, blockedRetainedBytes);
    EXPECT_EQ(blocked.spooledEvents, 0u);
    EXPECT_EQ(blocked.backendDeliveryAttempts, 0u);
    EXPECT_EQ(blocked.retryableFailedEvents, 0u);
    EXPECT_EQ(blocked.spoolIoFailures, 0u);
    EXPECT_GT(blocked.spoolRejectedOperations, 0u);
    EXPECT_EQ(blocked.droppedEvents, 1u);

    telemetry.RecordEvent("blocked.carryover.must-not-accumulate");
    const auto afterBlockedRecord = telemetry.GetDeliveryStats();
    EXPECT_EQ(afterBlockedRecord.queuedEvents, 2u);
    EXPECT_EQ(afterBlockedRecord.queuedBytes, blockedRetainedBytes);
    EXPECT_EQ(afterBlockedRecord.carryoverEvents, 2u);
    EXPECT_EQ(afterBlockedRecord.carryoverBytes, blockedRetainedBytes);
    EXPECT_EQ(afterBlockedRecord.spooledEvents, 0u);
    EXPECT_EQ(afterBlockedRecord.backendDeliveryAttempts, 0u);
    EXPECT_EQ(afterBlockedRecord.retryableFailedEvents, 0u);
    EXPECT_EQ(afterBlockedRecord.droppedEvents, 1u);
    EXPECT_EQ(ReadFile(blockedCarryoverSpool.Artifact()), rejectedArtifact);

    // Repair the hostile artifact in the same session. The maintenance retry
    // must rewrite its stale three-event image to the already-counted bounded
    // carryover without counting the same logical drop a second time.
    blockedCarryoverSpool.WriteArtifact(validBlockedArtifact);
    telemetry.Update(tightBlockedConfig.retryIntervalSeconds);
    const auto repaired = telemetry.GetDeliveryStats();
    EXPECT_EQ(repaired.queuedEvents, 2u);
    EXPECT_EQ(repaired.queuedBytes, blockedRetainedBytes);
    EXPECT_EQ(repaired.carryoverEvents, 0u);
    EXPECT_EQ(repaired.carryoverBytes, 0u);
    EXPECT_EQ(repaired.spooledEvents, 2u);
    EXPECT_EQ(repaired.backendDeliveryAttempts, 0u);
    EXPECT_EQ(repaired.retryableFailedEvents, 0u);
    EXPECT_EQ(repaired.spoolIoFailures, 0u);
    EXPECT_GT(repaired.spoolRejectedOperations, 0u);
    EXPECT_EQ(repaired.droppedEvents, 1u);
    EXPECT_EQ(ReadSpoolEventCount(blockedCarryoverSpool.Artifact()), 2u);
    const std::string repairedArtifact = ReadFile(blockedCarryoverSpool.Artifact());
    EXPECT_TRUE(repairedArtifact.find("blocked.carryover.oldest") != std::string::npos);
    EXPECT_TRUE(repairedArtifact.find("blocked.carryover.second") != std::string::npos);
    EXPECT_TRUE(repairedArtifact.find("blocked.carryover.dropped") == std::string::npos);
    EXPECT_TRUE(repairedArtifact.find("blocked.carryover.must-not-accumulate") == std::string::npos);

    // Re-enter the blocked state once more with an already-bounded carryover.
    // Admission remains closed, and a second repair introduces no new drop.
    telemetry.Shutdown();
    blockedCarryoverSpool.WriteArtifact(rejectedArtifact);
    telemetry.Initialize(tightBlockedConfig);
    const auto reblocked = telemetry.GetDeliveryStats();
    EXPECT_EQ(reblocked.queuedEvents, 2u);
    EXPECT_EQ(reblocked.queuedBytes, blockedRetainedBytes);
    EXPECT_EQ(reblocked.carryoverEvents, 2u);
    EXPECT_EQ(reblocked.carryoverBytes, blockedRetainedBytes);
    EXPECT_EQ(reblocked.spooledEvents, 0u);
    EXPECT_EQ(reblocked.backendDeliveryAttempts, 0u);
    EXPECT_EQ(reblocked.retryableFailedEvents, 0u);
    EXPECT_GT(reblocked.spoolRejectedOperations, 0u);
    EXPECT_EQ(reblocked.droppedEvents, 0u);

    telemetry.RecordEvent("blocked.carryover.reinit-must-not-accumulate");
    const auto afterReblockedRecord = telemetry.GetDeliveryStats();
    EXPECT_EQ(afterReblockedRecord.queuedEvents, 2u);
    EXPECT_EQ(afterReblockedRecord.queuedBytes, blockedRetainedBytes);
    EXPECT_EQ(afterReblockedRecord.carryoverEvents, 2u);
    EXPECT_EQ(afterReblockedRecord.carryoverBytes, blockedRetainedBytes);
    EXPECT_EQ(afterReblockedRecord.backendDeliveryAttempts, 0u);
    EXPECT_EQ(afterReblockedRecord.droppedEvents, 0u);
    EXPECT_EQ(ReadFile(blockedCarryoverSpool.Artifact()), rejectedArtifact);

    blockedCarryoverSpool.WriteArtifact(repairedArtifact);
    telemetry.Update(tightBlockedConfig.retryIntervalSeconds);
    const auto repairedReloaded = telemetry.GetDeliveryStats();
    EXPECT_EQ(repairedReloaded.queuedEvents, 2u);
    EXPECT_EQ(repairedReloaded.queuedBytes, blockedRetainedBytes);
    EXPECT_EQ(repairedReloaded.carryoverEvents, 0u);
    EXPECT_EQ(repairedReloaded.carryoverBytes, 0u);
    EXPECT_EQ(repairedReloaded.spooledEvents, 2u);
    EXPECT_GT(repairedReloaded.spoolRejectedOperations, 0u);
    EXPECT_EQ(repairedReloaded.droppedEvents, 0u);
    EXPECT_EQ(ReadSpoolEventCount(blockedCarryoverSpool.Artifact()), 2u);

    auto blockedDeliveredState = std::make_shared<BackendState>();
    telemetry.RegisterBackend(std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered,
                                                                            blockedDeliveredState));
    telemetry.FlushEvents();
    const auto blockedDeliveredEvents = blockedDeliveredState->Events();
    EXPECT_EQ(blockedDeliveredState->CallCount(), 1u);
    ASSERT_EQ(blockedDeliveredEvents.size(), 2u);
    EXPECT_EQ(blockedDeliveredEvents[0].name, std::string("blocked.carryover.oldest"));
    EXPECT_EQ(blockedDeliveredEvents[1].name, std::string("blocked.carryover.second"));
    const auto blockedDelivered = telemetry.GetDeliveryStats();
    EXPECT_EQ(blockedDelivered.queuedEvents, 0u);
    EXPECT_EQ(blockedDelivered.queuedBytes, 0u);
    EXPECT_EQ(blockedDelivered.carryoverEvents, 0u);
    EXPECT_EQ(blockedDelivered.carryoverBytes, 0u);
    EXPECT_EQ(blockedDelivered.spooledEvents, 0u);
    EXPECT_EQ(blockedDelivered.deliveredEvents, 2u);
    EXPECT_EQ(blockedDelivered.backendDeliveryAttempts, 1u);
    EXPECT_EQ(blockedDelivered.retryableFailedEvents, 0u);
    EXPECT_EQ(blockedDelivered.droppedEvents, 0u);
    EXPECT_FALSE(fs::exists(blockedCarryoverSpool.Artifact()));
}

TEST(Telemetry_SpoolRecovery_ConsentRevocation)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    TelemetryReset reset(telemetry);
    TempTelemetrySpool spool("consent");
    const fs::path sentinel = spool.Root() / "caller-owned.txt";
    {
        std::ofstream output(sentinel, std::ios::binary);
        output << "preserve-me";
    }

    telemetry.Initialize(MakeSpoolConfig(spool.Root()));
    auto state = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::RetryableFailure, state));
    telemetry.RecordEvent("consent.pending");
    telemetry.FlushEvents();
    EXPECT_TRUE(fs::is_regular_file(spool.Artifact()));
    const size_t callsBeforeRevocation = state->CallCount();

    telemetry.SetConsent(false);
    telemetry.FlushEvents();
    const auto stats = telemetry.GetDeliveryStats();
    EXPECT_EQ(stats.queuedEvents, 0u);
    EXPECT_EQ(stats.spooledEvents, 0u);
    EXPECT_EQ(state->CallCount(), callsBeforeRevocation);
    EXPECT_FALSE(fs::exists(spool.Artifact()));
    EXPECT_FALSE(fs::exists(spool.Staging()));
    EXPECT_TRUE(fs::is_directory(spool.Root()));
    EXPECT_EQ(ReadFile(sentinel), std::string("preserve-me"));
}

TEST(Telemetry_SpoolRecovery_HostileArtifactRejection)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    TelemetryReset reset(telemetry);

    TempTelemetrySpool malformed("malformed");
    const std::string malformedBytes = "not-a-telemetry-spool";
    malformed.WriteArtifact(malformedBytes);
    auto state = std::make_shared<BackendState>();
    ExerciseRejectedArtifact(MakeSpoolConfig(malformed.Root()), state);
    EXPECT_EQ(ReadFile(malformed.Artifact()), malformedBytes);

    TempTelemetrySpool oversized("oversized");
    const std::string oversizedBytes(64, 'x');
    oversized.WriteArtifact(oversizedBytes);
    auto oversizedConfig = MakeSpoolConfig(oversized.Root());
    oversizedConfig.maxSpoolBytes = 32;
    state = std::make_shared<BackendState>();
    ExerciseRejectedArtifact(oversizedConfig, state);
    EXPECT_EQ(ReadFile(oversized.Artifact()), oversizedBytes);

    TempTelemetrySpool nonRegular("nonregular");
    fs::create_directory(nonRegular.Artifact());
    {
        std::ofstream output(nonRegular.Artifact() / "caller-owned.txt");
        output << "preserve-me";
    }
    state = std::make_shared<BackendState>();
    ExerciseRejectedArtifact(MakeSpoolConfig(nonRegular.Root()), state);
    EXPECT_TRUE(fs::is_directory(fs::symlink_status(nonRegular.Artifact())));
    EXPECT_TRUE(fs::exists(nonRegular.Artifact() / "caller-owned.txt"));
}

TEST(Telemetry_SpoolRecovery_SymlinkRejection)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    TelemetryReset reset(telemetry);
    TempTelemetrySpool symlinked("symlink-artifact");
    const fs::path symlinkTarget = symlinked.Root() / "caller-owned-target.txt";
    {
        std::ofstream output(symlinkTarget);
        output << "preserve-me";
    }
    std::error_code symlinkError;
    fs::create_symlink(symlinkTarget, symlinked.Artifact(), symlinkError);
    if (symlinkError)
    {
#ifdef _WIN32
        SKIP_TEST("File symlink fixtures are unsupported: " + symlinkError.message());
#else
        ASSERT_TRUE(!symlinkError);
#endif
    }

    auto state = std::make_shared<BackendState>();
    ExerciseRejectedArtifact(MakeSpoolConfig(symlinked.Root()), state);
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(symlinked.Artifact())));
    EXPECT_EQ(ReadFile(symlinkTarget), std::string("preserve-me"));

    TempTelemetrySpool redirected("symlink-directory");
    const fs::path externalDirectory = redirected.Root() / "caller-owned-directory";
    const fs::path linkedDirectory = redirected.Root() / "spool-link";
    fs::create_directory(externalDirectory);
    {
        std::ofstream output(externalDirectory / "caller-owned.txt");
        output << "preserve-me";
    }
    symlinkError.clear();
    fs::create_directory_symlink(externalDirectory, linkedDirectory, symlinkError);
    if (symlinkError)
    {
#ifdef _WIN32
        SKIP_TEST("Directory symlink fixtures are unsupported: " + symlinkError.message());
#else
        ASSERT_TRUE(!symlinkError);
#endif
    }

    telemetry.Initialize(MakeSpoolConfig(linkedDirectory));
    state = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, state));
    telemetry.RecordEvent("symlink.directory.rejected");
    telemetry.FlushEvents();
    const auto directSymlinkStats = telemetry.GetDeliveryStats();
    EXPECT_EQ(state->CallCount(), 0u);
    EXPECT_EQ(directSymlinkStats.queuedEvents, 0u);
    EXPECT_EQ(directSymlinkStats.queuedBytes, 0u);
    EXPECT_EQ(directSymlinkStats.carryoverEvents, 0u);
    EXPECT_EQ(directSymlinkStats.carryoverBytes, 0u);
    EXPECT_EQ(directSymlinkStats.spooledEvents, 0u);
    EXPECT_EQ(directSymlinkStats.backendDeliveryAttempts, 0u);
    EXPECT_EQ(directSymlinkStats.retryableFailedEvents, 0u);
    EXPECT_EQ(directSymlinkStats.droppedEvents, 0u);
    EXPECT_EQ(directSymlinkStats.spoolIoFailures, 0u);
    EXPECT_GT(directSymlinkStats.spoolRejectedOperations, 0u);
    telemetry.SetConsent(false);
    telemetry.Shutdown();
    EXPECT_FALSE(fs::exists(externalDirectory / kSpoolArtifact));
    EXPECT_FALSE(fs::exists(externalDirectory / kSpoolStaging));
    EXPECT_EQ(ReadFile(externalDirectory / "caller-owned.txt"), std::string("preserve-me"));

    const fs::path linkedParent = redirected.Root() / "parent-link";
    const fs::path finalLeaf = linkedParent / "new-spool";
    symlinkError.clear();
    fs::create_directory_symlink(externalDirectory, linkedParent, symlinkError);
    if (symlinkError)
    {
#ifdef _WIN32
        SKIP_TEST("Parent symlink fixtures are unsupported: " + symlinkError.message());
#else
        ASSERT_TRUE(!symlinkError);
#endif
    }

    {
        WorkingDirectoryGuard cwd(redirected.Root());
        telemetry.Initialize(MakeSpoolConfig(fs::path("parent-link") / "new-spool"));
    }
    state = std::make_shared<BackendState>();
    telemetry.RegisterBackend(
        std::make_unique<FixedResultTelemetryBackend>(Spark::TelemetryDeliveryResult::Delivered, state));
    telemetry.RecordEvent("symlink.parent.rejected");
    telemetry.FlushEvents();
    const auto parentSymlinkStats = telemetry.GetDeliveryStats();
    EXPECT_EQ(state->CallCount(), 0u);
    EXPECT_EQ(parentSymlinkStats.queuedEvents, 0u);
    EXPECT_EQ(parentSymlinkStats.queuedBytes, 0u);
    EXPECT_EQ(parentSymlinkStats.carryoverEvents, 0u);
    EXPECT_EQ(parentSymlinkStats.carryoverBytes, 0u);
    EXPECT_EQ(parentSymlinkStats.spooledEvents, 0u);
    EXPECT_EQ(parentSymlinkStats.backendDeliveryAttempts, 0u);
    EXPECT_EQ(parentSymlinkStats.retryableFailedEvents, 0u);
    EXPECT_EQ(parentSymlinkStats.droppedEvents, 0u);
    EXPECT_EQ(parentSymlinkStats.spoolIoFailures, 0u);
    EXPECT_GT(parentSymlinkStats.spoolRejectedOperations, 0u);
    telemetry.SetConsent(false);
    telemetry.Shutdown();
    EXPECT_FALSE(fs::exists(externalDirectory / "new-spool"));
    EXPECT_EQ(ReadFile(externalDirectory / "caller-owned.txt"), std::string("preserve-me"));

    // A missing leaf below a live symlink must not be treated as safely gone.
    // The unresolved consent-revocation cleanup remains a privacy fence for the
    // next session until the hostile path itself disappears.
    Spark::TelemetryConfig unspooled;
    unspooled.enabled = true;
    unspooled.consentGiven = true;
    telemetry.Initialize(unspooled);
    telemetry.RecordEvent("symlink.parent.cleanup-fence");
    EXPECT_EQ(telemetry.GetDeliveryStats().queuedEvents, 0u);
    telemetry.SetConsent(false);
    telemetry.Shutdown();
}

TEST(Telemetry_SpoolRecovery_DisappearedRejectedArtifactCleanupDoesNotPoisonNextSession)
{
    auto& telemetry = Spark::TelemetrySystem::GetInstance();
    TelemetryReset reset(telemetry);
    fs::path rejectedDirectory;

    {
        TempTelemetrySpool redirected("disappeared-rejected-cleanup");
        rejectedDirectory = redirected.Root() / "missing-parent" / "new-spool";

        EXPECT_TRUE(Spark::TelemetryDetail::TelemetrySpool::InspectDeferredCleanupDirectory("relative-spool") ==
                    Spark::TelemetryDetail::TelemetrySpoolResult::Rejected);
        EXPECT_TRUE(Spark::TelemetryDetail::TelemetrySpool::InspectDeferredCleanupDirectory(redirected.Root().string()) ==
                    Spark::TelemetryDetail::TelemetrySpoolResult::Success);
        EXPECT_TRUE(Spark::TelemetryDetail::TelemetrySpool::InspectDeferredCleanupDirectory(rejectedDirectory.string()) ==
                    Spark::TelemetryDetail::TelemetrySpoolResult::NotFound);
        const fs::path regularAncestor = redirected.Root() / "caller-owned-file";
        {
            std::ofstream output(regularAncestor);
            output << "preserve-me";
        }
        EXPECT_TRUE(Spark::TelemetryDetail::TelemetrySpool::InspectDeferredCleanupDirectory(
                        (regularAncestor / "child").string()) == Spark::TelemetryDetail::TelemetrySpoolResult::Rejected);

        telemetry.Initialize(MakeSpoolConfig(rejectedDirectory));
        EXPECT_GT(telemetry.GetDeliveryStats().spoolRejectedOperations, 0u);
        telemetry.SetConsent(false);
        telemetry.Shutdown();
    }
    EXPECT_FALSE(fs::exists(rejectedDirectory));

    Spark::TelemetryConfig unspooled;
    unspooled.enabled = true;
    unspooled.consentGiven = true;
    telemetry.Initialize(unspooled);
    telemetry.RecordEvent("unspooled.after-disappeared-cleanup");

    EXPECT_EQ(telemetry.GetQueueSize(), 1u);
}
