/**
 * @file TelemetrySpool.h
 * @brief Internal durable spool for consented telemetry delivery.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace Spark
{
    struct TelemetryEvent;

    namespace TelemetryDetail
    {
        enum class TelemetrySpoolResult : uint8_t
        {
            Success,
            Disabled,
            NotFound,
            Rejected,
            IoFailure
        };

        /**
         * @brief Game-thread-only persistence for pending telemetry events.
         *
         * The spool owns exactly two fixed filenames inside a caller-selected
         * directory: the committed artifact and its atomic staging file. It
         * never removes the directory or any other entry.
         */
        class TelemetrySpool final
        {
          public:
            /** Configure the owned directory and hard byte/event bounds. */
            TelemetrySpoolResult Configure(std::string_view directory, uint64_t maxBytes, uint32_t maxEvents);

            /** Whether a trusted spool directory was configured. */
            [[nodiscard]] bool IsConfigured() const { return !m_directory.empty(); }

            /**
             * Classify a deferred cleanup path without following symlinks or
             * reparse points. NotFound is returned only when every existing
             * ancestor is a real directory and the next component is absent.
             */
            [[nodiscard]] static TelemetrySpoolResult InspectDeferredCleanupDirectory(std::string_view directory);

            /**
             * Keep the oldest representable events within the configured
             * format and capacity limits.
             * @return Number of events removed from @p events.
             */
            uint64_t Constrain(std::vector<TelemetryEvent>& events,
                               std::vector<uint64_t>* droppedSequences = nullptr) const;

            /** Restore a valid committed artifact without modifying it. */
            TelemetrySpoolResult Restore(std::vector<TelemetryEvent>& events) const;

            /** Atomically replace the committed artifact with @p events. */
            TelemetrySpoolResult Store(const std::vector<TelemetryEvent>& events);

            /** Remove only the fixed committed/staging artifacts. */
            TelemetrySpoolResult Clear();

            static constexpr std::string_view kArtifactName = "spark-telemetry.spool";
            static constexpr std::string_view kStagingName = "spark-telemetry.spool.tmp";

          private:
            [[nodiscard]] bool ValidateDirectory() const;

            std::filesystem::path m_directory;
            std::filesystem::path m_artifactPath;
            std::filesystem::path m_stagingPath;
            uint64_t m_maxBytes = 0;
            uint32_t m_maxEvents = 0;
        };
    } // namespace TelemetryDetail
} // namespace Spark
