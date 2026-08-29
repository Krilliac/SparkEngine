/**
 * @file Telemetry.h
 * @brief Runtime telemetry and analytics with privacy-first design
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Provides opt-in event recording for gameplay analytics, performance metrics,
 * and crash diagnostics. Events are batched and flushed periodically to a
 * configurable backend (local file or HTTP endpoint).
 *
 * Privacy guarantees:
 * - All recording is gated on explicit user consent
 * - Revoking consent immediately clears the event queue
 * - No events are recorded or transmitted without consent
 *
 * @code
 *   auto& telemetry = Spark::TelemetrySystem::GetInstance();
 *   Spark::TelemetryConfig cfg;
 *   cfg.enabled = true;
 *   cfg.consentGiven = true;
 *   cfg.localExportPath = "telemetry/";
 *   telemetry.Initialize(cfg);
 *   telemetry.RecordEvent("level_start", {{"level", "3"}, {"difficulty", "hard"}});
 *   telemetry.Shutdown();
 * @endcode
 */

#pragma once

#include "Spark/ServiceInterfaces.h"
#include "Utils/Assert.h"
#include "Utils/TelemetrySpool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Spark
{

    // ============================================================================
    // Telemetry Event
    // ============================================================================

    /**
     * @brief A single telemetry data point.
     */
    struct TelemetryEvent
    {
        std::string name;                                        ///< Event name (e.g. "level_complete")
        uint64_t timestamp = 0;                                  ///< Epoch milliseconds
        std::unordered_map<std::string, std::string> properties; ///< Key-value metadata
        std::string sessionId;                                   ///< Session identifier
        uint64_t sequence = 0;                                   ///< Monotonic enqueue order for deterministic flush
    };

    // ============================================================================
    // Telemetry Config
    // ============================================================================

    /**
     * @brief Configuration for the telemetry system.
     */
    struct TelemetryConfig
    {
        bool enabled = false;                              ///< Master enable switch
        bool consentGiven = false;                         ///< User has opted in
        std::string localExportPath;                       ///< Directory for local JSON export
        std::string httpEndpoint;                          ///< Remote endpoint URL (future use)
        uint32_t batchSize = 50;                           ///< Events per flush batch
        float flushIntervalSeconds = 30.0f;                ///< Auto-flush interval
        uint32_t maxQueueSize = 10000;                     ///< Maximum queued events before dropping
        uint64_t maxQueueBytes = 4ull * 1024ull * 1024ull; ///< Maximum conservative in-memory queue budget
        std::string spoolDirectory; ///< Durable retry spool directory (empty disables persistence)
        uint64_t maxSpoolBytes = 4ull * 1024ull * 1024ull; ///< Maximum committed spool artifact size
        uint32_t maxSpoolEvents = 10000;                   ///< Maximum number of durably retained events
        float retryIntervalSeconds = 5.0f;                 ///< Retry cadence after a deferred or failed delivery
    };

    /** @brief Result of one backend delivery attempt. */
    enum class TelemetryDeliveryResult : uint8_t
    {
        Delivered,
        RetryableFailure,
        Rejected
    };

    /** @brief Thread-safe snapshot of current and cumulative delivery accounting. */
    struct TelemetryDeliveryStats
    {
        uint64_t queuedEvents = 0;
        uint64_t queuedBytes = 0;
        uint64_t carryoverEvents = 0;
        uint64_t carryoverBytes = 0;
        uint64_t spooledEvents = 0;
        uint64_t backendDeliveryAttempts = 0;
        uint64_t deliveredEvents = 0;
        uint64_t rejectedEvents = 0;
        uint64_t retryableFailedEvents = 0;
        uint64_t spoolIoFailures = 0;
        uint64_t spoolRejectedOperations = 0;
        uint64_t droppedEvents = 0;
    };

    // ============================================================================
    // Backend Interface
    // ============================================================================

    /**
     * @brief Abstract interface for telemetry output destinations.
     */
    class ITelemetryBackend
    {
      public:
        virtual ~ITelemetryBackend() = default;

        /**
         * @brief Send a batch of events to the backend.
         * @param events The events to transmit/persist.
         * @return Terminal delivery, retryable failure, or permanent rejection.
         */
        virtual TelemetryDeliveryResult Send(const std::vector<TelemetryEvent>& events) = 0;

        /** @brief Human-readable backend name. */
        virtual std::string_view GetBackendName() const = 0;
    };

    // ============================================================================
    // Local File Backend
    // ============================================================================

    /**
     * @brief Writes telemetry events to JSON files on disk.
     *
     * Each flush produces a timestamped JSON file in the configured directory.
     */
    class LocalFileTelemetryBackend final : public ITelemetryBackend
    {
      public:
        /**
         * @param exportPath Directory to write JSON files into (must exist or be creatable).
         */
        explicit LocalFileTelemetryBackend(std::string exportPath) : m_exportPath(std::move(exportPath)) {}

        TelemetryDeliveryResult Send(const std::vector<TelemetryEvent>& events) override
        {
            if (events.empty())
                return TelemetryDeliveryResult::Delivered;
            if (m_exportPath.empty())
                return TelemetryDeliveryResult::RetryableFailure;

            // Build filename from epoch time
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

            const std::string fileName = "telemetry_" + std::to_string(ms) + "_" +
                                         std::to_string(events.front().sequence) + "_" +
                                         std::to_string(events.back().sequence) + ".json";
            const std::filesystem::path filePath = std::filesystem::path(m_exportPath) / fileName;

            std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
                return TelemetryDeliveryResult::RetryableFailure;

            file << "[\n";
            for (size_t i = 0; i < events.size(); ++i)
            {
                const auto& evt = events[i];
                file << "  {\n";
                file << "    \"name\": \"" << EscapeJson(evt.name) << "\",\n";
                file << "    \"timestamp\": " << evt.timestamp << ",\n";
                file << "    \"sequence\": " << evt.sequence << ",\n";
                file << "    \"sessionId\": \"" << EscapeJson(evt.sessionId) << "\",\n";
                file << "    \"properties\": {";

                bool firstProp = true;
                for (const auto& [key, value] : evt.properties)
                {
                    if (!firstProp)
                        file << ",";
                    firstProp = false;
                    file << "\n      \"" << EscapeJson(key) << "\": \"" << EscapeJson(value) << "\"";
                }

                file << "\n    }\n";
                file << "  }";
                if (i + 1 < events.size())
                    file << ",";
                file << "\n";
            }
            file << "]\n";

            file.flush();
            const bool flushed = file.good();
            file.close();
            if (!flushed || file.fail())
                return TelemetryDeliveryResult::RetryableFailure;

            ++m_filesWritten;
            return TelemetryDeliveryResult::Delivered;
        }

        std::string_view GetBackendName() const override { return "LocalFile"; }

        /** @brief Number of files successfully written since creation. */
        uint32_t GetFilesWritten() const { return m_filesWritten; }

      private:
        /** @brief Minimal JSON string escaping. */
        static std::string EscapeJson(const std::string& input)
        {
            std::string result;
            result.reserve(input.size());
            constexpr char hex[] = "0123456789abcdef";
            for (const unsigned char ch : input)
            {
                switch (ch)
                {
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                case '\b':
                    result += "\\b";
                    break;
                case '\f':
                    result += "\\f";
                    break;
                default:
                    if (ch < 0x20)
                    {
                        result += "\\u00";
                        result += hex[(ch >> 4) & 0x0f];
                        result += hex[ch & 0x0f];
                    }
                    else
                    {
                        result += static_cast<char>(ch);
                    }
                    break;
                }
            }
            return result;
        }

        std::string m_exportPath;
        uint32_t m_filesWritten = 0;
    };

    // ============================================================================
    // Telemetry System
    // ============================================================================

    /**
     * @class TelemetrySystem
     * @brief Singleton managing event recording, batching, and backend dispatch.
     */
    class TelemetrySystem : public ITelemetryService
    {
      public:
        /** @brief Get the singleton instance. */
        static TelemetrySystem& GetInstance()
        {
            static TelemetrySystem instance;
            return instance;
        }

        TelemetrySystem(const TelemetrySystem&) = delete;
        TelemetrySystem& operator=(const TelemetrySystem&) = delete;

        // --- Lifecycle ---

        /**
         * @brief Initialize the telemetry system.
         * @param config Configuration (enable, consent, paths, batching).
         * @note Threading: game-thread-only.
         */
        void Initialize(const TelemetryConfig& config)
        {
            m_acceptingRecords.store(false, std::memory_order_release);
            m_recordingGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_gameThreadId = std::this_thread::get_id();
            if (m_initialized.load(std::memory_order_acquire))
            {
                if (!config.consentGiven)
                    SetConsent(false);
                Shutdown();
            }
            m_shuttingDown = false;

            if (!config.consentGiven && m_spoolRequired && !m_config.spoolDirectory.empty())
            {
                m_spoolRestorePending = false;
                m_spoolCleanupPending = true;
            }
            PreserveActiveCleanupForReinitialize_GameThread();
            m_config = config;
            if (m_config.batchSize == 0)
                m_config.batchSize = 1;
            if (m_config.flushIntervalSeconds <= 0.0f)
                m_config.flushIntervalSeconds = 30.0f;
            if (m_config.retryIntervalSeconds <= 0.0f)
                m_config.retryIntervalSeconds = 5.0f;

            {
                std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                m_eventQueue.clear();
                m_eventQueueBytes = 0;
                m_queuedEventCount.store(0, std::memory_order_relaxed);
                m_queuedByteCount.store(0, std::memory_order_relaxed);
            }
            m_timeSinceFlush = 0.0f;
            m_retryPending = false;
            m_spoolConfigurePending = false;
            m_spoolRestorePending = false;
            m_spoolCleanupPending = false;
            m_spooledEventCount.store(0, std::memory_order_relaxed);
            m_backendDeliveryAttemptCount.store(0, std::memory_order_relaxed);
            m_deliveredEventCount.store(0, std::memory_order_relaxed);
            m_rejectedEventCount.store(0, std::memory_order_relaxed);
            m_retryableFailedEventCount.store(0, std::memory_order_relaxed);
            m_spoolIoFailureCount.store(0, std::memory_order_relaxed);
            m_spoolRejectedOperationCount.store(0, std::memory_order_relaxed);
            m_droppedEventCount.store(0, std::memory_order_relaxed);
            m_preaccountedCarryoverDropSequences.clear();
            m_totalEventsRecorded.store(0, std::memory_order_relaxed);
            m_nextEventSequence.store(1, std::memory_order_relaxed);
            m_enabled.store(m_config.enabled, std::memory_order_relaxed);
            m_consentGiven.store(m_config.consentGiven, std::memory_order_relaxed);
            m_maxQueueSize.store(m_config.maxQueueSize, std::memory_order_relaxed);
            m_maxQueueBytes.store(m_config.maxQueueBytes, std::memory_order_relaxed);
            auto carryoverConstraint = ConstrainQueueCandidates(m_shutdownCarryover);
            AdvanceSequencePast(carryoverConstraint.maximumValidSequence);
            m_carryoverEventCount.store(static_cast<uint64_t>(m_shutdownCarryover.size()), std::memory_order_relaxed);
            m_carryoverByteCount.store(carryoverConstraint.retainedBytes, std::memory_order_relaxed);
            m_preaccountedCarryoverDropSequences = std::move(carryoverConstraint.droppedSequences);
            SortUniqueSequences(m_preaccountedCarryoverDropSequences);
            m_droppedEventCount.fetch_add(static_cast<uint64_t>(m_preaccountedCarryoverDropSequences.size()),
                                          std::memory_order_relaxed);
            m_spoolRequired = !m_config.spoolDirectory.empty();
            TryConfigureActiveSpool_GameThread();

            // Generate a simple session ID from the current time
            auto now = std::chrono::system_clock::now().time_since_epoch();
            const std::string sessionId =
                "session_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
            {
                std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                m_sessionId = sessionId;
            }

            if (!m_config.consentGiven)
            {
                const uint64_t discarded = static_cast<uint64_t>(m_shutdownCarryover.size());
                m_shutdownCarryover.clear();
                m_carryoverEventCount.store(0, std::memory_order_relaxed);
                m_carryoverByteCount.store(0, std::memory_order_relaxed);
                m_preaccountedCarryoverDropSequences.clear();
                m_droppedEventCount.fetch_add(discarded, std::memory_order_relaxed);
                m_spoolCleanupPending = m_spoolRequired;
                m_spoolRestorePending = false;
            }
            else
            {
                m_spoolRestorePending = m_spoolRequired;
            }

            RetryDeferredSpoolPurges_GameThread();
            TryClearActiveSpool_GameThread();
            if (m_config.enabled && m_config.consentGiven)
                RestorePendingEvents_GameThread();
            if (!m_spoolRequired && m_config.consentGiven)
            {
                if (!m_shutdownCarryover.empty())
                {
                    std::vector<TelemetryEvent> carryover;
                    carryover.swap(m_shutdownCarryover);
                    m_carryoverEventCount.store(0, std::memory_order_relaxed);
                    m_carryoverByteCount.store(0, std::memory_order_relaxed);
                    MergeQueuedEvents_GameThread(std::move(carryover));
                    m_retryPending = m_queuedEventCount.load(std::memory_order_relaxed) != 0;
                }
                m_preaccountedCarryoverDropSequences.clear();
            }

            // Auto-create a local file backend if a path is configured and no backend is registered
            if (!m_config.localExportPath.empty() && !m_backend)
            {
                m_backend = std::make_unique<LocalFileTelemetryBackend>(m_config.localExportPath);
            }

            m_initialized.store(true, std::memory_order_release);
            RefreshAcceptingRecords_GameThread();
        }

        /**
         * @brief Flush remaining events and shut down.
         * @note Threading: game-thread-only.
         */
        void Shutdown() override
        {
            AssertGameThread("Shutdown");
            m_shuttingDown = true;
            m_acceptingRecords.store(false, std::memory_order_release);
            m_recordingGeneration.fetch_add(1, std::memory_order_acq_rel);
            if (m_initialized.load(std::memory_order_acquire))
                FlushEvents();

            {
                std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                if (m_spoolRequired)
                {
                    m_shutdownCarryover.insert(m_shutdownCarryover.end(), std::make_move_iterator(m_eventQueue.begin()),
                                               std::make_move_iterator(m_eventQueue.end()));
                    const auto constrained = ConstrainQueueCandidates(m_shutdownCarryover);
                    AdvanceSequencePast(constrained.maximumValidSequence);
                    m_droppedEventCount.fetch_add(constrained.droppedEvents, std::memory_order_relaxed);
                    m_carryoverEventCount.store(static_cast<uint64_t>(m_shutdownCarryover.size()),
                                                std::memory_order_relaxed);
                    m_carryoverByteCount.store(constrained.retainedBytes, std::memory_order_relaxed);
                }
                else
                {
                    const uint64_t discarded = SaturatingAdd(static_cast<uint64_t>(m_eventQueue.size()),
                                                             static_cast<uint64_t>(m_shutdownCarryover.size()));
                    m_shutdownCarryover.clear();
                    m_carryoverEventCount.store(0, std::memory_order_relaxed);
                    m_carryoverByteCount.store(0, std::memory_order_relaxed);
                    m_droppedEventCount.fetch_add(discarded, std::memory_order_relaxed);
                }
                m_eventQueue.clear();
                m_eventQueueBytes = 0;
                m_queuedEventCount.store(0, std::memory_order_relaxed);
                m_queuedByteCount.store(0, std::memory_order_relaxed);
            }
            m_backend.reset();
            m_initialized.store(false, std::memory_order_release);
        }

        // --- Recording ---

        /**
         * @brief Record a telemetry event.
         * @param name       Event name (e.g. "player_death").
         * @param properties Optional key-value metadata.
         *
         * Silently drops the event if consent is not given or the queue is full.
         * @note Threading: thread-safe (multi-producer).
         */
        void RecordEvent(std::string_view name,
                         const std::optional<std::unordered_map<std::string, std::string>>& properties = std::nullopt)
        {
            const uint64_t recordingGeneration = m_recordingGeneration.load(std::memory_order_acquire);
            if (!CanRecord())
                return;

            TelemetryEvent evt;
            evt.name = std::string(name);
            evt.timestamp = GetCurrentTimestampMs();
            evt.sequence = AllocateEventSequence();
            if (evt.sequence == 0)
            {
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (properties.has_value())
                evt.properties = properties.value();

            EnqueueEventThreadSafe(std::move(evt), recordingGeneration);
        }

        /**
         * @brief Record a timed event with a duration measurement.
         * @param name       Event name.
         * @param durationMs Duration of the measured operation in milliseconds.
         * @param properties Optional additional metadata.
         * @note Threading: thread-safe (multi-producer).
         */
        void RecordTimedEvent(
            std::string_view name, float durationMs,
            const std::optional<std::unordered_map<std::string, std::string>>& properties = std::nullopt)
        {
            if (!CanRecord())
                return;

            std::unordered_map<std::string, std::string> merged;
            if (properties.has_value())
                merged = properties.value();
            merged["duration_ms"] = std::to_string(durationMs);

            RecordEvent(name, merged);
        }

        /**
         * @brief Flush all queued events to the registered backend.
         *
         * Sends events in batches of config.batchSize.
         * @note Threading: game-thread-only.
         */
        void FlushEvents()
        {
            AssertGameThread("FlushEvents");
            if (!m_initialized.load(std::memory_order_acquire))
                return;

            m_timeSinceFlush = 0.0f;
            if (!m_consentGiven.load(std::memory_order_relaxed))
            {
                RetryOwnedSpoolCleanup_GameThread();
                return;
            }

            RunSpoolMaintenance_GameThread();
            if (HasSpoolMaintenancePending_GameThread())
                return;
            if (!m_enabled.load(std::memory_order_relaxed))
                return;

            std::vector<TelemetryEvent> pendingEvents;
            ExtractQueuedEventsForFlush_GameThread(pendingEvents);
            if (pendingEvents.empty())
            {
                RetryOwnedSpoolCleanup_GameThread();
                return;
            }

            NormalizeBySequence(pendingEvents);
            uint64_t constrainedDrops = 0;
            if (m_spool.IsConfigured())
            {
                std::vector<TelemetryEvent> durableSnapshot = pendingEvents;
                constrainedDrops = m_spool.Constrain(durableSnapshot);
                if (!PersistPendingEvents_GameThread(durableSnapshot))
                {
                    // Keep the complete pre-constraint set in memory until the
                    // atomically bounded durable snapshot has actually committed.
                    MergeQueuedEvents_GameThread(std::move(pendingEvents));
                    m_retryPending = true;
                    return;
                }
                pendingEvents = std::move(durableSnapshot);
            }
            else if (!PersistPendingEvents_GameThread(pendingEvents))
            {
                MergeQueuedEvents_GameThread(std::move(pendingEvents));
                m_retryPending = true;
                return;
            }

            // Publish capacity loss only after the corresponding retained durable
            // snapshot has committed. Before that point all candidates remain live.
            m_droppedEventCount.fetch_add(constrainedDrops, std::memory_order_relaxed);
            if (pendingEvents.empty())
            {
                return;
            }

            if (!m_backend)
            {
                MergeQueuedEvents_GameThread(std::move(pendingEvents));
                m_retryPending = true;
                return;
            }

            while (!pendingEvents.empty())
            {
                const size_t batchCount = (std::min)(pendingEvents.size(), static_cast<size_t>(m_config.batchSize));
                std::vector<TelemetryEvent> batch(pendingEvents.begin(),
                                                  pendingEvents.begin() + static_cast<ptrdiff_t>(batchCount));

                TelemetryDeliveryResult result = TelemetryDeliveryResult::RetryableFailure;
                m_backendDeliveryAttemptCount.fetch_add(1, std::memory_order_relaxed);
                try
                {
                    result = m_backend->Send(batch);
                }
                catch (...)
                {
                    result = TelemetryDeliveryResult::RetryableFailure;
                }

                if (result == TelemetryDeliveryResult::RetryableFailure)
                {
                    m_retryableFailedEventCount.fetch_add(static_cast<uint64_t>(batchCount), std::memory_order_relaxed);
                    MergeQueuedEvents_GameThread(std::move(pendingEvents));
                    m_retryPending = true;
                    return;
                }

                if (result == TelemetryDeliveryResult::Delivered)
                    m_deliveredEventCount.fetch_add(batchCount, std::memory_order_relaxed);
                else
                    m_rejectedEventCount.fetch_add(batchCount, std::memory_order_relaxed);

                pendingEvents.erase(pendingEvents.begin(), pendingEvents.begin() + static_cast<ptrdiff_t>(batchCount));

                // Advance the durable cursor only after a terminal backend result.
                // If this replace fails, the older artifact remains valid; replay
                // may duplicate a delivered event, which is why sequence is the
                // backend deduplication key and delivery is explicitly at-least-once.
                if (!PersistPendingEvents_GameThread(pendingEvents))
                {
                    if (!pendingEvents.empty())
                        MergeQueuedEvents_GameThread(std::move(pendingEvents));
                    else
                        m_spoolCleanupPending = true;
                    m_retryPending = true;
                    RefreshAcceptingRecords_GameThread();
                    return;
                }
            }

            m_retryPending = false;
        }

        /**
         * @brief Per-frame update. Auto-flushes when the interval elapses.
         * @param dt Delta time in seconds.
         * @note Threading: game-thread-only.
         */
        void Update(float dt) override
        {
            AssertGameThread("Update");
            if (!m_initialized.load(std::memory_order_acquire))
                return;

            m_timeSinceFlush += (std::max)(dt, 0.0f);
            if (HasSpoolMaintenancePending_GameThread() && m_timeSinceFlush >= m_config.retryIntervalSeconds)
            {
                FlushEvents();
                return;
            }

            if (!m_enabled.load(std::memory_order_relaxed) || !m_consentGiven.load(std::memory_order_relaxed))
                return;

            const float interval = m_retryPending ? m_config.retryIntervalSeconds : m_config.flushIntervalSeconds;
            if (m_timeSinceFlush >= interval && m_queuedEventCount.load(std::memory_order_relaxed) > 0)
            {
                FlushEvents();
            }
        }

        // --- Consent ---

        /**
         * @brief Set user consent for telemetry collection.
         * @param consent true to allow recording, false to revoke.
         *
         * Revoking consent immediately clears all queued events.
         * @note Threading: game-thread-only.
         */
        void SetConsent(bool consent)
        {
            AssertGameThread("SetConsent");
            const bool previouslyConsented = m_consentGiven.load(std::memory_order_relaxed);
            m_acceptingRecords.store(false, std::memory_order_release);
            m_recordingGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_config.consentGiven = consent;
            m_consentGiven.store(consent, std::memory_order_relaxed);
            if (!consent)
            {
                uint64_t discarded = 0;
                {
                    std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                    discarded = static_cast<uint64_t>(m_eventQueue.size());
                    m_eventQueue.clear();
                    m_eventQueueBytes = 0;
                    m_queuedEventCount.store(0, std::memory_order_relaxed);
                    m_queuedByteCount.store(0, std::memory_order_relaxed);
                }
                discarded += static_cast<uint64_t>(m_shutdownCarryover.size());
                m_shutdownCarryover.clear();
                m_carryoverEventCount.store(0, std::memory_order_relaxed);
                m_carryoverByteCount.store(0, std::memory_order_relaxed);
                m_preaccountedCarryoverDropSequences.clear();
                m_droppedEventCount.fetch_add(discarded, std::memory_order_relaxed);

                m_spoolRestorePending = false;
                m_spoolCleanupPending = m_spoolRequired;
                m_retryPending = m_spoolCleanupPending || !m_deferredSpoolPurges.empty();
            }
            else if (!previouslyConsented && !m_spoolCleanupPending)
            {
                m_spoolRestorePending = m_spoolRequired;
            }

            RunSpoolMaintenance_GameThread();
            RefreshAcceptingRecords_GameThread();
        }

        /**
         * @brief Check if the user has given consent.
         * @note Threading: thread-safe.
         */
        bool HasConsent() const { return m_consentGiven.load(std::memory_order_relaxed); }

        /**
         * @brief Check if telemetry service has been initialized.
         * @note Threading: thread-safe.
         */
        bool IsInitialized() const override { return m_initialized.load(std::memory_order_acquire); }

        // --- Queries ---

        /**
         * @brief Get the current configuration (read-only snapshot).
         * @note Threading: game-thread-only.
         */
        const TelemetryConfig& GetConfig() const
        {
            AssertGameThread("GetConfig");
            return m_config;
        }

        /**
         * @brief Number of events currently queued.
         * @note Threading: thread-safe.
         */
        uint32_t GetQueueSize() const
        {
            const uint64_t pendingEvents = SaturatingAdd(m_queuedEventCount.load(std::memory_order_relaxed),
                                                         m_carryoverEventCount.load(std::memory_order_relaxed));
            return static_cast<uint32_t>(
                (std::min)(pendingEvents, static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
        }

        /** @brief Thread-safe, independently sampled delivery gauges and counters. */
        TelemetryDeliveryStats GetDeliveryStats() const
        {
            TelemetryDeliveryStats stats;
            stats.carryoverEvents = m_carryoverEventCount.load(std::memory_order_relaxed);
            stats.carryoverBytes = m_carryoverByteCount.load(std::memory_order_relaxed);
            stats.queuedEvents =
                SaturatingAdd(m_queuedEventCount.load(std::memory_order_relaxed), stats.carryoverEvents);
            stats.queuedBytes = SaturatingAdd(m_queuedByteCount.load(std::memory_order_relaxed), stats.carryoverBytes);
            stats.spooledEvents = m_spooledEventCount.load(std::memory_order_relaxed);
            stats.backendDeliveryAttempts = m_backendDeliveryAttemptCount.load(std::memory_order_relaxed);
            stats.deliveredEvents = m_deliveredEventCount.load(std::memory_order_relaxed);
            stats.rejectedEvents = m_rejectedEventCount.load(std::memory_order_relaxed);
            stats.retryableFailedEvents = m_retryableFailedEventCount.load(std::memory_order_relaxed);
            stats.spoolIoFailures = m_spoolIoFailureCount.load(std::memory_order_relaxed);
            stats.spoolRejectedOperations = m_spoolRejectedOperationCount.load(std::memory_order_relaxed);
            stats.droppedEvents = m_droppedEventCount.load(std::memory_order_relaxed);
            return stats;
        }

        // --- Backend ---

        /**
         * @brief Register a custom telemetry backend.
         * @param backend The backend to use (replaces any existing backend).
         * @note Threading: game-thread-only.
         */
        void RegisterBackend(std::unique_ptr<ITelemetryBackend> backend)
        {
            AssertGameThread("RegisterBackend");
            m_backend = std::move(backend);
        }

        // --- Console ---

        /**
         * @brief Get telemetry system status (console integration).
         * @note Threading: thread-safe.
         */
        std::string Console_GetStatus() const
        {
            std::string status = "TelemetrySystem: ";
            status += m_initialized.load(std::memory_order_acquire) ? "initialized" : "not initialized";
            status += " | Enabled: ";
            status += m_enabled.load(std::memory_order_relaxed) ? "yes" : "no";
            status += " | Consent: ";
            status += m_consentGiven.load(std::memory_order_relaxed) ? "yes" : "no";
            const uint64_t carryoverEvents = m_carryoverEventCount.load(std::memory_order_relaxed);
            const uint64_t carryoverBytes = m_carryoverByteCount.load(std::memory_order_relaxed);
            const uint64_t pendingEvents =
                SaturatingAdd(m_queuedEventCount.load(std::memory_order_relaxed), carryoverEvents);
            const uint64_t pendingBytes =
                SaturatingAdd(m_queuedByteCount.load(std::memory_order_relaxed), carryoverBytes);
            status += " | Queued: " + std::to_string(pendingEvents);
            status += "/" + std::to_string(m_config.maxQueueSize);
            status += " | Queued bytes: " + std::to_string(pendingBytes);
            status += "/" + std::to_string(m_config.maxQueueBytes);
            status += " | Carryover: " + std::to_string(carryoverEvents) + " events/" + std::to_string(carryoverBytes) +
                      " bytes";
            status += " | Total recorded: " + std::to_string(m_totalEventsRecorded.load(std::memory_order_relaxed));
            status += " | Backend: ";
            status += m_backend ? std::string(m_backend->GetBackendName()) : "none";
            status += " | Session: " + m_sessionId;
            return status;
        }

      private:
        struct PendingSpoolPurge
        {
            TelemetryDetail::TelemetrySpool spool;
            std::string directory;
            uint64_t maxBytes = 0;
            uint32_t maxEvents = 0;
        };

        struct QueueConstraintResult
        {
            uint64_t retainedBytes = 0;
            uint64_t droppedEvents = 0;
            uint64_t maximumValidSequence = 0;
            std::vector<uint64_t> droppedSequences;
        };

        TelemetrySystem() = default;
        ~TelemetrySystem() = default;

        void AssertGameThread(const char* methodName) const
        {
            ASSERT_MSG(std::this_thread::get_id() == m_gameThreadId, "TelemetrySystem::%s must run on game thread",
                       methodName);
        }

        /** @brief Check if recording is permitted. */
        bool CanRecord() const
        {
            return m_acceptingRecords.load(std::memory_order_acquire) &&
                   m_initialized.load(std::memory_order_acquire) && m_enabled.load(std::memory_order_relaxed) &&
                   m_consentGiven.load(std::memory_order_relaxed);
        }

        // Stable conservative accounting: the fixed charges cover the event/container object and one
        // unordered-map node plus allocator/bucket slack per property. String payload bytes are charged exactly.
        static constexpr uint64_t kQueueEventOverheadBytes = 256;
        static constexpr uint64_t kQueuePropertyOverheadBytes = 128;
        static constexpr size_t kMaxQueuePropertiesPerEvent = 256;
        static constexpr size_t kMaxQueueStringBytes = 1024 * 1024;

        static bool CheckedAddQueueBytes(uint64_t& total, uint64_t amount)
        {
            if (amount > (std::numeric_limits<uint64_t>::max)() - total)
                return false;
            total += amount;
            return true;
        }

        static uint64_t SaturatingAdd(uint64_t lhs, uint64_t rhs)
        {
            const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
            return rhs > maximum - lhs ? maximum : lhs + rhs;
        }

        static uint64_t CountSortedSequenceIntersection(const std::vector<uint64_t>& lhs,
                                                        const std::vector<uint64_t>& rhs)
        {
            auto left = lhs.begin();
            auto right = rhs.begin();
            uint64_t intersection = 0;
            while (left != lhs.end() && right != rhs.end())
            {
                if (*left < *right)
                    ++left;
                else if (*right < *left)
                    ++right;
                else
                {
                    ++intersection;
                    ++left;
                    ++right;
                }
            }
            return intersection;
        }

        static void SortUniqueSequences(std::vector<uint64_t>& sequences)
        {
            std::sort(sequences.begin(), sequences.end());
            sequences.erase(std::unique(sequences.begin(), sequences.end()), sequences.end());
        }

        static bool FitsQueueByteBudget(uint64_t currentBytes, uint64_t eventBytes, uint64_t maxBytes)
        {
            return currentBytes <= maxBytes && eventBytes <= maxBytes - currentBytes;
        }

        static bool TryCalculateQueuedEventBytes(const TelemetryEvent& event, uint64_t& bytes)
        {
            if (event.sequence == 0 || event.sequence == (std::numeric_limits<uint64_t>::max)() ||
                event.name.size() > kMaxQueueStringBytes || event.sessionId.size() > kMaxQueueStringBytes ||
                event.properties.size() > kMaxQueuePropertiesPerEvent)
            {
                return false;
            }

            bytes = kQueueEventOverheadBytes;
            if (!CheckedAddQueueBytes(bytes, static_cast<uint64_t>(event.name.size())) ||
                !CheckedAddQueueBytes(bytes, static_cast<uint64_t>(event.sessionId.size())))
            {
                return false;
            }

            for (const auto& [key, value] : event.properties)
            {
                if (key.size() > kMaxQueueStringBytes || value.size() > kMaxQueueStringBytes ||
                    !CheckedAddQueueBytes(bytes, kQueuePropertyOverheadBytes) ||
                    !CheckedAddQueueBytes(bytes, static_cast<uint64_t>(key.size())) ||
                    !CheckedAddQueueBytes(bytes, static_cast<uint64_t>(value.size())))
                {
                    return false;
                }
            }
            return true;
        }

        QueueConstraintResult ConstrainQueueCandidates(std::vector<TelemetryEvent>& events) const
        {
            NormalizeBySequence(events);

            const size_t maxQueueSize = static_cast<size_t>(m_maxQueueSize.load(std::memory_order_relaxed));
            const uint64_t maxQueueBytes = m_maxQueueBytes.load(std::memory_order_relaxed);
            QueueConstraintResult result;
            size_t retainedEvents = 0;
            for (size_t index = 0; index < events.size(); ++index)
            {
                uint64_t eventBytes = 0;
                if (!TryCalculateQueuedEventBytes(events[index], eventBytes))
                {
                    ++result.droppedEvents;
                    result.droppedSequences.push_back(events[index].sequence);
                    continue;
                }

                result.maximumValidSequence = events[index].sequence;
                if (retainedEvents >= maxQueueSize ||
                    !FitsQueueByteBudget(result.retainedBytes, eventBytes, maxQueueBytes))
                {
                    ++result.droppedEvents;
                    result.droppedSequences.push_back(events[index].sequence);
                    continue;
                }

                if (retainedEvents != index)
                    events[retainedEvents] = std::move(events[index]);
                ++retainedEvents;
                result.retainedBytes += eventBytes;
            }
            events.erase(events.begin() + static_cast<ptrdiff_t>(retainedEvents), events.end());
            return result;
        }

        /** @brief Thread-safe writer path for producers calling RecordEvent. */
        void EnqueueEventThreadSafe(TelemetryEvent&& evt, uint64_t recordingGeneration)
        {
            std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
            if (!CanRecord() || recordingGeneration != m_recordingGeneration.load(std::memory_order_acquire))
                return;
            evt.sessionId = m_sessionId;
            uint64_t eventBytes = 0;
            const uint64_t maxQueueBytes = m_maxQueueBytes.load(std::memory_order_relaxed);
            if (!TryCalculateQueuedEventBytes(evt, eventBytes) ||
                m_eventQueue.size() >= static_cast<size_t>(m_maxQueueSize.load(std::memory_order_relaxed)) ||
                !FitsQueueByteBudget(m_eventQueueBytes, eventBytes, maxQueueBytes))
            {
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            m_eventQueue.push_back(std::move(evt));
            m_eventQueueBytes += eventBytes;
            m_queuedEventCount.store(static_cast<uint64_t>(m_eventQueue.size()), std::memory_order_relaxed);
            m_queuedByteCount.store(m_eventQueueBytes, std::memory_order_relaxed);
            m_totalEventsRecorded.fetch_add(1, std::memory_order_relaxed);
        }

        /** @brief Game-thread read/flush path; swaps queued events into @p outEvents. */
        void ExtractQueuedEventsForFlush_GameThread(std::vector<TelemetryEvent>& outEvents)
        {
            AssertGameThread("ExtractQueuedEventsForFlush_GameThread");
            std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
            outEvents.clear();
            outEvents.swap(m_eventQueue);
            m_eventQueueBytes = 0;
            m_queuedEventCount.store(0, std::memory_order_relaxed);
            m_queuedByteCount.store(0, std::memory_order_relaxed);
        }

        static void NormalizeBySequence(std::vector<TelemetryEvent>& events)
        {
            std::stable_sort(events.begin(), events.end(), [](const TelemetryEvent& lhs, const TelemetryEvent& rhs)
                             { return lhs.sequence < rhs.sequence; });
            events.erase(std::unique(events.begin(), events.end(),
                                     [](const TelemetryEvent& lhs, const TelemetryEvent& rhs)
                                     { return lhs.sequence == rhs.sequence; }),
                         events.end());
        }

        uint64_t AllocateEventSequence()
        {
            uint64_t current = m_nextEventSequence.load(std::memory_order_relaxed);
            while (current != 0 && current != (std::numeric_limits<uint64_t>::max)())
            {
                if (m_nextEventSequence.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                                              std::memory_order_relaxed))
                {
                    return current;
                }
            }
            return 0;
        }

        void AdvanceSequencePast(uint64_t maximum)
        {
            if (maximum == 0 || maximum == (std::numeric_limits<uint64_t>::max)())
                return;
            const uint64_t desired = maximum + 1;
            uint64_t current = m_nextEventSequence.load(std::memory_order_relaxed);
            while (current < desired && !m_nextEventSequence.compare_exchange_weak(
                                            current, desired, std::memory_order_relaxed, std::memory_order_relaxed))
            {
            }
        }

        void MergeQueuedEvents_GameThread(std::vector<TelemetryEvent>&& events)
        {
            AssertGameThread("MergeQueuedEvents_GameThread");
            if (events.empty())
                return;

            std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
            m_eventQueue.insert(m_eventQueue.end(), std::make_move_iterator(events.begin()),
                                std::make_move_iterator(events.end()));
            const auto constrained = ConstrainQueueCandidates(m_eventQueue);
            AdvanceSequencePast(constrained.maximumValidSequence);
            m_eventQueueBytes = constrained.retainedBytes;
            m_queuedEventCount.store(static_cast<uint64_t>(m_eventQueue.size()), std::memory_order_relaxed);
            m_queuedByteCount.store(m_eventQueueBytes, std::memory_order_relaxed);
            m_droppedEventCount.fetch_add(constrained.droppedEvents, std::memory_order_relaxed);
        }

        [[nodiscard]] bool HasPendingSpoolPurge_GameThread() const
        {
            return m_spoolCleanupPending || !m_deferredSpoolPurges.empty();
        }

        [[nodiscard]] bool HasSpoolMaintenancePending_GameThread() const
        {
            return m_spoolConfigurePending || m_spoolRestorePending || HasPendingSpoolPurge_GameThread();
        }

        void RefreshAcceptingRecords_GameThread()
        {
            const bool accepting = m_initialized.load(std::memory_order_acquire) &&
                                   m_enabled.load(std::memory_order_relaxed) &&
                                   m_consentGiven.load(std::memory_order_relaxed) && !m_shuttingDown &&
                                   !HasSpoolMaintenancePending_GameThread();
            m_acceptingRecords.store(accepting, std::memory_order_release);
        }

        void PreserveActiveCleanupForReinitialize_GameThread()
        {
            if (!m_spoolCleanupPending || m_config.spoolDirectory.empty())
                return;

            PendingSpoolPurge pending;
            pending.spool = m_spool;
            pending.directory = m_config.spoolDirectory;
            pending.maxBytes = m_config.maxSpoolBytes;
            pending.maxEvents = m_config.maxSpoolEvents;
            m_deferredSpoolPurges.push_back(std::move(pending));
        }

        void AccountSpoolFailure_GameThread(TelemetryDetail::TelemetrySpoolResult result)
        {
            AssertGameThread("AccountSpoolFailure_GameThread");
            if (result == TelemetryDetail::TelemetrySpoolResult::Rejected)
            {
                m_spoolRejectedOperationCount.fetch_add(1, std::memory_order_relaxed);
            }
            else if (result != TelemetryDetail::TelemetrySpoolResult::Success &&
                     result != TelemetryDetail::TelemetrySpoolResult::NotFound)
            {
                // Disabled is an unexpected health failure whenever a required
                // configured spool operation reaches this helper.
                m_spoolIoFailureCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        bool TryConfigureActiveSpool_GameThread()
        {
            AssertGameThread("TryConfigureActiveSpool_GameThread");
            if (!m_spoolRequired)
            {
                m_spool.Configure({}, 0, 0);
                m_spoolConfigurePending = false;
                return true;
            }

            const auto result =
                m_spool.Configure(m_config.spoolDirectory, m_config.maxSpoolBytes, m_config.maxSpoolEvents);
            if (result == TelemetryDetail::TelemetrySpoolResult::Success)
            {
                m_spoolConfigurePending = false;
                return true;
            }

            m_spoolConfigurePending = true;
            m_retryPending = true;
            AccountSpoolFailure_GameThread(result);
            return false;
        }

        void RetryDeferredSpoolPurges_GameThread()
        {
            AssertGameThread("RetryDeferredSpoolPurges_GameThread");
            auto purge = m_deferredSpoolPurges.begin();
            while (purge != m_deferredSpoolPurges.end())
            {
                if (!purge->spool.IsConfigured())
                {
                    const uint64_t purgeBytes = (std::max)(purge->maxBytes, static_cast<uint64_t>(16));
                    const uint32_t purgeEvents =
                        (std::min)((std::max)(purge->maxEvents, 1u), static_cast<uint32_t>(100000));
                    const auto configureResult = purge->spool.Configure(purge->directory, purgeBytes, purgeEvents);
                    if (configureResult != TelemetryDetail::TelemetrySpoolResult::Success)
                    {
                        AccountSpoolFailure_GameThread(configureResult);
                        ++purge;
                        continue;
                    }
                }

                const auto clearResult = purge->spool.Clear();
                if (clearResult == TelemetryDetail::TelemetrySpoolResult::Success)
                    purge = m_deferredSpoolPurges.erase(purge);
                else
                {
                    AccountSpoolFailure_GameThread(clearResult);
                    ++purge;
                }
            }
            if (!m_deferredSpoolPurges.empty())
                m_retryPending = true;
        }

        void TryClearActiveSpool_GameThread()
        {
            AssertGameThread("TryClearActiveSpool_GameThread");
            if (!m_spoolCleanupPending || m_spoolConfigurePending || !m_spool.IsConfigured())
                return;

            const auto clearResult = m_spool.Clear();
            if (clearResult == TelemetryDetail::TelemetrySpoolResult::Success)
            {
                m_spoolCleanupPending = false;
                m_spoolRestorePending = false;
                m_spooledEventCount.store(0, std::memory_order_relaxed);
            }
            else
            {
                m_retryPending = true;
                AccountSpoolFailure_GameThread(clearResult);
            }
        }

        void RestorePendingEvents_GameThread()
        {
            AssertGameThread("RestorePendingEvents_GameThread");

            if (!m_spoolRestorePending)
                return;
            if (m_spoolConfigurePending || !m_spool.IsConfigured() || HasPendingSpoolPurge_GameThread())
            {
                m_retryPending = true;
                return;
            }

            std::vector<TelemetryEvent> restoredEvents;
            const auto restoreResult = m_spool.Restore(restoredEvents);
            std::vector<uint64_t> restoredDurableSequences;
            uint64_t restoredArtifactEvents = 0;
            if (restoreResult == TelemetryDetail::TelemetrySpoolResult::Success)
            {
                restoredArtifactEvents = static_cast<uint64_t>(restoredEvents.size());
                restoredDurableSequences.reserve(restoredEvents.size());
                for (const auto& event : restoredEvents)
                    restoredDurableSequences.push_back(event.sequence);
                m_spooledEventCount.store(restoredArtifactEvents, std::memory_order_relaxed);
            }
            else if (restoreResult == TelemetryDetail::TelemetrySpoolResult::NotFound)
            {
                m_spooledEventCount.store(0, std::memory_order_relaxed);
            }
            else
            {
                // Rejected and I/O-failed artifacts remain unread and must never
                // be replaced by a newer snapshot. Retry until the caller fixes
                // or explicitly purges the fixed artifact.
                AccountSpoolFailure_GameThread(restoreResult);
                m_retryPending = true;
                return;
            }

            // Build the prospective state without consuming any live source. A
            // failed bounded rewrite must leave the queue, carryover, and artifact
            // exactly where they were so a later maintenance pass can retry.
            std::vector<TelemetryEvent> restoreCandidates;
            {
                std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                restoreCandidates = m_eventQueue;
            }
            restoreCandidates.insert(restoreCandidates.end(), m_shutdownCarryover.begin(), m_shutdownCarryover.end());
            restoreCandidates.insert(restoreCandidates.end(), std::make_move_iterator(restoredEvents.begin()),
                                     std::make_move_iterator(restoredEvents.end()));

            auto queueConstraint = ConstrainQueueCandidates(restoreCandidates);
            std::vector<uint64_t> droppedSequences = std::move(queueConstraint.droppedSequences);
            uint64_t retainedBytes = queueConstraint.retainedBytes;
            if (m_spool.IsConfigured())
            {
                const uint64_t spoolDrops = m_spool.Constrain(restoreCandidates, &droppedSequences);
                if (spoolDrops != 0)
                {
                    auto finalQueueConstraint = ConstrainQueueCandidates(restoreCandidates);
                    retainedBytes = finalQueueConstraint.retainedBytes;
                    droppedSequences.insert(droppedSequences.end(),
                                            std::make_move_iterator(finalQueueConstraint.droppedSequences.begin()),
                                            std::make_move_iterator(finalQueueConstraint.droppedSequences.end()));
                }
            }
            SortUniqueSequences(droppedSequences);
            const uint64_t droppedEvents = static_cast<uint64_t>(droppedSequences.size());

            const bool retainedMatchesDurable =
                restoredDurableSequences.size() == restoreCandidates.size() &&
                std::equal(restoredDurableSequences.begin(), restoredDurableSequences.end(), restoreCandidates.begin(),
                           [](uint64_t sequence, const TelemetryEvent& event) { return sequence == event.sequence; });
            if (droppedEvents != 0 || !retainedMatchesDurable)
            {
                const auto storeResult = m_spool.Store(restoreCandidates);
                if (storeResult != TelemetryDetail::TelemetrySpoolResult::Success)
                {
                    AccountSpoolFailure_GameThread(storeResult);
                    m_retryPending = true;
                    return;
                }
                restoredArtifactEvents = static_cast<uint64_t>(restoreCandidates.size());
            }

            // The bounded artifact is committed (or no rewrite was necessary), so
            // the corresponding in-memory state and drop count may now be published.
            {
                std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                m_eventQueue = std::move(restoreCandidates);
                m_eventQueueBytes = retainedBytes;
                m_queuedEventCount.store(static_cast<uint64_t>(m_eventQueue.size()), std::memory_order_relaxed);
                m_queuedByteCount.store(m_eventQueueBytes, std::memory_order_relaxed);
            }
            m_shutdownCarryover.clear();
            m_carryoverEventCount.store(0, std::memory_order_relaxed);
            m_carryoverByteCount.store(0, std::memory_order_relaxed);
            AdvanceSequencePast(queueConstraint.maximumValidSequence);
            const uint64_t alreadyAccounted =
                CountSortedSequenceIntersection(m_preaccountedCarryoverDropSequences, droppedSequences);
            m_droppedEventCount.fetch_add(droppedEvents - alreadyAccounted, std::memory_order_relaxed);
            m_preaccountedCarryoverDropSequences.clear();
            m_spooledEventCount.store(restoredArtifactEvents, std::memory_order_relaxed);
            m_spoolRestorePending = false;
            if (m_queuedEventCount.load(std::memory_order_relaxed) != 0)
                m_retryPending = true;
        }

        void RunSpoolMaintenance_GameThread()
        {
            AssertGameThread("RunSpoolMaintenance_GameThread");
            RetryDeferredSpoolPurges_GameThread();

            if (m_spoolConfigurePending)
                TryConfigureActiveSpool_GameThread();

            TryClearActiveSpool_GameThread();
            RestorePendingEvents_GameThread();
            if (HasSpoolMaintenancePending_GameThread())
                m_retryPending = true;
            RefreshAcceptingRecords_GameThread();
        }

        bool PersistPendingEvents_GameThread(const std::vector<TelemetryEvent>& events)
        {
            AssertGameThread("PersistPendingEvents_GameThread");
            if (!m_spoolRequired)
            {
                m_spooledEventCount.store(0, std::memory_order_relaxed);
                m_spoolCleanupPending = false;
                return true;
            }

            if (m_spoolConfigurePending || m_spoolRestorePending || HasPendingSpoolPurge_GameThread() ||
                !m_spool.IsConfigured())
            {
                return false;
            }

            const auto result = m_spool.Store(events);
            if (result != TelemetryDetail::TelemetrySpoolResult::Success)
            {
                AccountSpoolFailure_GameThread(result);
                return false;
            }

            m_spooledEventCount.store(static_cast<uint64_t>(events.size()), std::memory_order_relaxed);
            m_spoolCleanupPending = false;
            return true;
        }

        void RetryOwnedSpoolCleanup_GameThread()
        {
            AssertGameThread("RetryOwnedSpoolCleanup_GameThread");
            RunSpoolMaintenance_GameThread();
        }

        /** @brief Get current time in epoch milliseconds. */
        static uint64_t GetCurrentTimestampMs()
        {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        }

        std::thread::id m_gameThreadId = std::this_thread::get_id();
        std::atomic<bool> m_initialized{false};
        std::atomic<bool> m_acceptingRecords{false};
        std::atomic<uint64_t> m_recordingGeneration{0};
        std::atomic<bool> m_enabled{false};
        std::atomic<bool> m_consentGiven{false};
        std::atomic<uint32_t> m_maxQueueSize{0};
        std::atomic<uint64_t> m_maxQueueBytes{0};
        TelemetryConfig m_config;
        mutable std::mutex m_eventQueueMutex;
        std::vector<TelemetryEvent> m_eventQueue;
        uint64_t m_eventQueueBytes = 0; ///< Guarded by m_eventQueueMutex.
        std::vector<TelemetryEvent> m_shutdownCarryover;
        std::vector<uint64_t> m_preaccountedCarryoverDropSequences;
        std::unique_ptr<ITelemetryBackend> m_backend;
        TelemetryDetail::TelemetrySpool m_spool;
        std::vector<PendingSpoolPurge> m_deferredSpoolPurges;
        std::string m_sessionId;
        std::atomic<uint64_t> m_queuedEventCount{0};
        std::atomic<uint64_t> m_queuedByteCount{0};
        std::atomic<uint64_t> m_carryoverEventCount{0};
        std::atomic<uint64_t> m_carryoverByteCount{0};
        std::atomic<uint64_t> m_spooledEventCount{0};
        std::atomic<uint64_t> m_backendDeliveryAttemptCount{0};
        std::atomic<uint64_t> m_deliveredEventCount{0};
        std::atomic<uint64_t> m_rejectedEventCount{0};
        std::atomic<uint64_t> m_retryableFailedEventCount{0};
        std::atomic<uint64_t> m_spoolIoFailureCount{0};
        std::atomic<uint64_t> m_spoolRejectedOperationCount{0};
        std::atomic<uint64_t> m_droppedEventCount{0};
        std::atomic<uint64_t> m_nextEventSequence{1};
        float m_timeSinceFlush = 0.0f;
        bool m_spoolRequired = false;
        bool m_shuttingDown = false;
        bool m_spoolConfigurePending = false;
        bool m_spoolRestorePending = false;
        bool m_retryPending = false;
        bool m_spoolCleanupPending = false;
        std::atomic<uint64_t> m_totalEventsRecorded{0};
    };

} // namespace Spark
