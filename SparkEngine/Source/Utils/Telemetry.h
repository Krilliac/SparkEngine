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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
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
        bool enabled = false;               ///< Master enable switch
        bool consentGiven = false;          ///< User has opted in
        std::string localExportPath;        ///< Directory for local JSON export
        std::string httpEndpoint;           ///< Remote endpoint URL (future use)
        uint32_t batchSize = 50;            ///< Events per flush batch
        float flushIntervalSeconds = 30.0f; ///< Auto-flush interval
        uint32_t maxQueueSize = 10000;      ///< Maximum queued events before dropping
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
         * @return true if the batch was accepted.
         */
        virtual bool Send(const std::vector<TelemetryEvent>& events) = 0;

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

        bool Send(const std::vector<TelemetryEvent>& events) override
        {
            if (events.empty() || m_exportPath.empty())
                return false;

            // Build filename from epoch time
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

            std::string separator = m_exportPath.back() == '/' ? "" : "/";
            std::string filePath = m_exportPath + separator + "telemetry_" + std::to_string(ms) + ".json";

            std::ofstream file(filePath);
            if (!file.is_open())
                return false;

            file << "[\n";
            for (size_t i = 0; i < events.size(); ++i)
            {
                const auto& evt = events[i];
                file << "  {\n";
                file << "    \"name\": \"" << EscapeJson(evt.name) << "\",\n";
                file << "    \"timestamp\": " << evt.timestamp << ",\n";
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

            m_filesWritten++;
            return file.good();
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
            for (char ch : input)
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
                default:
                    result += ch;
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
            m_gameThreadId = std::this_thread::get_id();
            m_config = config;
            {
                std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                m_eventQueue.clear();
            }
            m_timeSinceFlush = 0.0f;
            m_queuedEventCount.store(0, std::memory_order_relaxed);
            m_totalEventsRecorded.store(0, std::memory_order_relaxed);
            m_nextEventSequence.store(1, std::memory_order_relaxed);
            m_enabled.store(m_config.enabled, std::memory_order_relaxed);
            m_consentGiven.store(m_config.consentGiven, std::memory_order_relaxed);
            m_maxQueueSize.store(m_config.maxQueueSize, std::memory_order_relaxed);

            // Generate a simple session ID from the current time
            auto now = std::chrono::system_clock::now().time_since_epoch();
            m_sessionId =
                "session_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

            // Auto-create a local file backend if a path is configured and no backend is registered
            if (!m_config.localExportPath.empty() && !m_backend)
            {
                m_backend = std::make_unique<LocalFileTelemetryBackend>(m_config.localExportPath);
            }

            m_initialized.store(true, std::memory_order_release);
        }

        /**
         * @brief Flush remaining events and shut down.
         * @note Threading: game-thread-only.
         */
        void Shutdown() override
        {
            AssertGameThread("Shutdown");
            if (m_initialized.load(std::memory_order_acquire))
            {
                FlushEvents();
            }
            {
                std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                m_eventQueue.clear();
            }
            m_queuedEventCount.store(0, std::memory_order_relaxed);
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
            if (!CanRecord())
                return;

            TelemetryEvent evt;
            evt.name = std::string(name);
            evt.timestamp = GetCurrentTimestampMs();
            evt.sessionId = m_sessionId;
            evt.sequence = m_nextEventSequence.fetch_add(1, std::memory_order_relaxed);
            if (properties.has_value())
                evt.properties = properties.value();

            EnqueueEventThreadSafe(std::move(evt));
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
            if (!m_backend)
                return;

            std::vector<TelemetryEvent> pendingEvents;
            ExtractQueuedEventsForFlush_GameThread(pendingEvents);
            if (pendingEvents.empty())
                return;

            std::sort(pendingEvents.begin(), pendingEvents.end(),
                      [](const TelemetryEvent& lhs, const TelemetryEvent& rhs) { return lhs.sequence < rhs.sequence; });

            // Send in batches
            size_t offset = 0;
            while (offset < pendingEvents.size())
            {
                size_t batchEnd = offset + m_config.batchSize;
                if (batchEnd > pendingEvents.size())
                    batchEnd = pendingEvents.size();

                std::vector<TelemetryEvent> batch(pendingEvents.begin() + static_cast<ptrdiff_t>(offset),
                                                  pendingEvents.begin() + static_cast<ptrdiff_t>(batchEnd));

                m_backend->Send(batch);
                offset = batchEnd;
            }

            m_timeSinceFlush = 0.0f;
        }

        /**
         * @brief Per-frame update. Auto-flushes when the interval elapses.
         * @param dt Delta time in seconds.
         * @note Threading: game-thread-only.
         */
        void Update(float dt) override
        {
            AssertGameThread("Update");
            if (!m_initialized.load(std::memory_order_acquire) || !m_enabled.load(std::memory_order_relaxed))
                return;

            m_timeSinceFlush += dt;
            if (m_timeSinceFlush >= m_config.flushIntervalSeconds &&
                m_queuedEventCount.load(std::memory_order_relaxed) > 0)
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
            m_config.consentGiven = consent;
            m_consentGiven.store(consent, std::memory_order_relaxed);
            if (!consent)
            {
                std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
                m_eventQueue.clear();
                m_queuedEventCount.store(0, std::memory_order_relaxed);
            }
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
        uint32_t GetQueueSize() const { return m_queuedEventCount.load(std::memory_order_relaxed); }

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
            status += " | Queued: " + std::to_string(m_queuedEventCount.load(std::memory_order_relaxed));
            status += "/" + std::to_string(m_config.maxQueueSize);
            status += " | Total recorded: " + std::to_string(m_totalEventsRecorded.load(std::memory_order_relaxed));
            status += " | Backend: ";
            status += m_backend ? std::string(m_backend->GetBackendName()) : "none";
            status += " | Session: " + m_sessionId;
            return status;
        }

      private:
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
            return m_initialized.load(std::memory_order_acquire) && m_enabled.load(std::memory_order_relaxed) &&
                   m_consentGiven.load(std::memory_order_relaxed);
        }

        /** @brief Thread-safe writer path for producers calling RecordEvent. */
        void EnqueueEventThreadSafe(TelemetryEvent&& evt)
        {
            std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
            if (m_eventQueue.size() >= static_cast<size_t>(m_maxQueueSize.load(std::memory_order_relaxed)))
                return;

            m_eventQueue.push_back(std::move(evt));
            m_queuedEventCount.store(static_cast<uint32_t>(m_eventQueue.size()), std::memory_order_relaxed);
            m_totalEventsRecorded.fetch_add(1, std::memory_order_relaxed);
        }

        /** @brief Game-thread read/flush path; swaps queued events into @p outEvents. */
        void ExtractQueuedEventsForFlush_GameThread(std::vector<TelemetryEvent>& outEvents)
        {
            AssertGameThread("ExtractQueuedEventsForFlush_GameThread");
            std::lock_guard<std::mutex> queueLock(m_eventQueueMutex);
            outEvents.clear();
            outEvents.swap(m_eventQueue);
            m_queuedEventCount.store(0, std::memory_order_relaxed);
        }

        /** @brief Get current time in epoch milliseconds. */
        static uint64_t GetCurrentTimestampMs()
        {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        }

        std::thread::id m_gameThreadId = std::this_thread::get_id();
        std::atomic<bool> m_initialized{false};
        std::atomic<bool> m_enabled{false};
        std::atomic<bool> m_consentGiven{false};
        std::atomic<uint32_t> m_maxQueueSize{0};
        TelemetryConfig m_config;
        mutable std::mutex m_eventQueueMutex;
        std::vector<TelemetryEvent> m_eventQueue;
        std::unique_ptr<ITelemetryBackend> m_backend;
        std::string m_sessionId;
        std::atomic<uint32_t> m_queuedEventCount{0};
        std::atomic<uint64_t> m_nextEventSequence{1};
        float m_timeSinceFlush = 0.0f;
        std::atomic<uint64_t> m_totalEventsRecorded{0};
    };

} // namespace Spark
