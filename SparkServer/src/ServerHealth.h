/**
 * @file ServerHealth.h
 * @brief SparkServer operator health snapshot: build identity, tick latency, memory, and publication.
 *
 * The snapshot is the process's externally consumable operator surface. It is
 * written as one compact JSON object per line to stdout and, optionally, to an
 * atomically replaced health file that supervisors (the gateway smoke, the
 * editor's Dedicated Server panel, orchestration scripts) poll.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Spark::Server
{
    /**
     * @brief Source identity of the running server binary.
     *
     * The SparkServer executable fills this from the header that
     * SparkServer/cmake/SparkServerBuildIdentity.cmake regenerates on every
     * build, so the commit tracks the checkout that was compiled rather than
     * the one that was last configured. Only the executable consumes the
     * stamp: libraries and tests do not relink when HEAD moves.
     */
    struct BuildIdentity
    {
        std::string_view version = "unknown";   ///< SPARK_ENGINE_VERSION (MAJOR.MINOR.PATCH).
        std::string_view commit = "unknown";    ///< Full lowercase commit hash, or "unknown" without git metadata.
        std::string_view treeState = "unknown"; ///< "clean", "dirty" (tracked files differ from commit), or "unknown".
    };

    /** @brief Percentile summary of recorded tick work durations, in microseconds. */
    struct TickLatencySummary
    {
        uint64_t samples = 0;
        uint64_t p50Micros = 0;
        uint64_t p95Micros = 0;
        uint64_t p99Micros = 0;
        uint64_t maxMicros = 0;
    };

    /**
     * @brief Bounded fixed-bucket histogram of server tick work durations.
     *
     * Memory is constant regardless of run length, so a long soak cannot grow
     * the health surface. A percentile is reported as the upper bound of the
     * bucket holding that rank, clamped to the observed maximum: it never
     * under-reports, and ranks in the overflow bucket report the exact maximum.
     * The summary covers every tick since the last Reset().
     *
     * Record() is called by the server loop thread only; Summarize() may run on
     * any thread concurrently and always returns an internally consistent view.
     */
    class TickLatencyHistogram
    {
      public:
        /** Inclusive bucket upper bounds; durations above the last bound land in the overflow bucket. */
        static constexpr std::array<uint64_t, 15> BucketUpperBoundsMicros = {
            50, 100, 250, 500, 1000, 2000, 4000, 8000, 16000, 33000, 66000, 100000, 250000, 500000, 1000000};

        /** [loop thread] Add one tick's work duration. Negative durations count as zero. */
        void Record(std::chrono::nanoseconds duration) noexcept;
        /** [any thread] Summarize every sample recorded since the last Reset(). */
        [[nodiscard]] TickLatencySummary Summarize() const noexcept;
        /** [loop thread, while not recording] Discard all samples for a fresh lifecycle. */
        void Reset() noexcept;

      private:
        std::array<std::atomic<uint64_t>, BucketUpperBoundsMicros.size() + 1> m_counts{};
        std::atomic<uint64_t> m_maxMicros{0};
    };

    /** @brief Current process resident set size in bytes, or nullopt when the platform query fails. */
    [[nodiscard]] std::optional<uint64_t> QueryResidentSetBytes() noexcept;

    /** @brief Network message-queue occupancy copied from the last completed server tick. */
    struct NetworkQueueHealth
    {
        uint64_t incomingDepth = 0;
        uint64_t outgoingDepth = 0;
        uint64_t incomingPeak = 0; ///< Largest incoming queue since the network runtime initialized.
        uint64_t outgoingPeak = 0; ///< Largest outgoing queue since the network runtime initialized.
    };

    /**
     * @brief Versioned contract identifier, emitted as the first field of every snapshot.
     *
     * The field set, types and units are documented in
     * wiki/advanced/Server-Operations-Runbook.md and enforced by
     * tools/ops/validate_server_health.py. Any change to the field set bumps the version.
     */
    inline constexpr std::string_view HealthSchema = "spark-server-health/1";

    /** @brief Point-in-time server status published to operators. */
    struct ServerHealth
    {
        bool live = false;
        bool ready = false;
        /** A stop was requested and the loop is refusing new work; supervisors should route away. */
        bool draining = false;
        /** Teardown is in progress. */
        bool stopping = false;
        uint16_t port = 0;
        uint32_t players = 0;
        uint64_t ticks = 0;
        size_t loadedModules = 0;
        std::string gameModule;
        std::string currentMap;
        std::string lastError;
        BuildIdentity build;
        TickLatencySummary tickLatency;
        std::optional<uint64_t> residentSetBytes;
        NetworkQueueHealth netQueues;
    };

    /** @brief Serialize a snapshot as one compact JSON object (no trailing newline). */
    [[nodiscard]] std::string FormatHealthJson(const ServerHealth& health);

    /**
     * @brief Atomically replace @p path with @p json followed by a newline.
     *
     * A snapshot that cannot be staged never destroys the previous one: a
     * readiness watchdog reads a missing health file as a hard failure, which
     * is strictly worse than a stale-but-valid one. The snapshot is staged in an
     * unpredictable, exclusively created sibling file, so a file or link planted
     * next to @p path is never followed (SaveFileDurability::PublishFileAtomically).
     */
    void WriteHealthFile(const std::filesystem::path& path, std::string_view json);
} // namespace Spark::Server
