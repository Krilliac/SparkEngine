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

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
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
         */
        void Initialize(const TelemetryConfig& config)
        {
            m_config = config;
            m_eventQueue.clear();
            m_timeSinceFlush = 0.0f;
            m_totalEventsRecorded = 0;

            // Generate a simple session ID from the current time
            auto now = std::chrono::system_clock::now().time_since_epoch();
            m_sessionId =
                "session_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

            // Auto-create a local file backend if a path is configured and no backend is registered
            if (!m_config.localExportPath.empty() && !m_backend)
            {
                m_backend = std::make_unique<LocalFileTelemetryBackend>(m_config.localExportPath);
            }

            m_initialized = true;
        }

        /** @brief Flush remaining events and shut down. */
        void Shutdown() override
        {
            if (m_initialized)
            {
                FlushEvents();
            }
            m_eventQueue.clear();
            m_backend.reset();
            m_initialized = false;
        }

        // --- Recording ---

        /**
         * @brief Record a telemetry event.
         * @param name       Event name (e.g. "player_death").
         * @param properties Optional key-value metadata.
         *
         * Silently drops the event if consent is not given or the queue is full.
         */
        void RecordEvent(std::string_view name,
                         const std::optional<std::unordered_map<std::string, std::string>>& properties = std::nullopt)
        {
            if (!CanRecord())
                return;

            if (m_eventQueue.size() >= m_config.maxQueueSize)
                return;

            TelemetryEvent evt;
            evt.name = std::string(name);
            evt.timestamp = GetCurrentTimestampMs();
            evt.sessionId = m_sessionId;
            if (properties.has_value())
                evt.properties = properties.value();

            m_eventQueue.push_back(std::move(evt));
            m_totalEventsRecorded++;
        }

        /**
         * @brief Record a timed event with a duration measurement.
         * @param name       Event name.
         * @param durationMs Duration of the measured operation in milliseconds.
         * @param properties Optional additional metadata.
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
         */
        void FlushEvents()
        {
            if (!m_backend || m_eventQueue.empty())
                return;

            // Send in batches
            size_t offset = 0;
            while (offset < m_eventQueue.size())
            {
                size_t batchEnd = offset + m_config.batchSize;
                if (batchEnd > m_eventQueue.size())
                    batchEnd = m_eventQueue.size();

                std::vector<TelemetryEvent> batch(m_eventQueue.begin() + static_cast<ptrdiff_t>(offset),
                                                  m_eventQueue.begin() + static_cast<ptrdiff_t>(batchEnd));

                m_backend->Send(batch);
                offset = batchEnd;
            }

            m_eventQueue.clear();
            m_timeSinceFlush = 0.0f;
        }

        /**
         * @brief Per-frame update. Auto-flushes when the interval elapses.
         * @param dt Delta time in seconds.
         */
        void Update(float dt) override
        {
            if (!m_initialized || !m_config.enabled)
                return;

            m_timeSinceFlush += dt;
            if (m_timeSinceFlush >= m_config.flushIntervalSeconds && !m_eventQueue.empty())
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
         */
        void SetConsent(bool consent)
        {
            m_config.consentGiven = consent;
            if (!consent)
            {
                m_eventQueue.clear();
            }
        }

        /** @brief Check if the user has given consent. */
        bool HasConsent() const { return m_config.consentGiven; }

        /** @brief Check if telemetry service has been initialized. */
        bool IsInitialized() const override { return m_initialized; }

        // --- Queries ---

        /** @brief Get the current configuration (read-only). */
        const TelemetryConfig& GetConfig() const { return m_config; }

        /** @brief Number of events currently queued. */
        uint32_t GetQueueSize() const { return static_cast<uint32_t>(m_eventQueue.size()); }

        // --- Backend ---

        /**
         * @brief Register a custom telemetry backend.
         * @param backend The backend to use (replaces any existing backend).
         */
        void RegisterBackend(std::unique_ptr<ITelemetryBackend> backend) { m_backend = std::move(backend); }

        // --- Console ---

        /** @brief Get telemetry system status (console integration). */
        std::string Console_GetStatus() const
        {
            std::string status = "TelemetrySystem: ";
            status += m_initialized ? "initialized" : "not initialized";
            status += " | Enabled: ";
            status += m_config.enabled ? "yes" : "no";
            status += " | Consent: ";
            status += m_config.consentGiven ? "yes" : "no";
            status += " | Queued: " + std::to_string(m_eventQueue.size());
            status += "/" + std::to_string(m_config.maxQueueSize);
            status += " | Total recorded: " + std::to_string(m_totalEventsRecorded);
            status += " | Backend: ";
            status += m_backend ? std::string(m_backend->GetBackendName()) : "none";
            status += " | Session: " + m_sessionId;
            return status;
        }

      private:
        TelemetrySystem() = default;
        ~TelemetrySystem() = default;

        /** @brief Check if recording is permitted. */
        bool CanRecord() const { return m_initialized && m_config.enabled && m_config.consentGiven; }

        /** @brief Get current time in epoch milliseconds. */
        static uint64_t GetCurrentTimestampMs()
        {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        }

        bool m_initialized = false;
        TelemetryConfig m_config;
        std::vector<TelemetryEvent> m_eventQueue;
        std::unique_ptr<ITelemetryBackend> m_backend;
        std::string m_sessionId;
        float m_timeSinceFlush = 0.0f;
        uint64_t m_totalEventsRecorded = 0;
    };

} // namespace Spark
