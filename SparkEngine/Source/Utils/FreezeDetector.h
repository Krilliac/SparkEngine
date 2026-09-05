/**
 * @file FreezeDetector.h
 * @brief Watchdog thread that detects main-loop freezes and attempts recovery
 * @author Spark Engine Team
 * @date 2026
 *
 * Monitors the main thread heartbeat from a dedicated watchdog thread. If the
 * main loop stops sending heartbeats for longer than the configured threshold,
 * the detector escalates through recovery stages:
 *
 * 1. **Warning** — logs that the main thread appears stalled.
 * 2. **Recovery attempt** — sets an atomic flag the main loop can check to
 *    skip expensive work (e.g. drop the current frame).
 * 3. **Crash dump** — if the freeze persists, captures a minidump via
 *    CrashHandler and optionally terminates the process.
 *
 * Usage:
 * @code
 *   Spark::FreezeDetector::GetInstance().Start();
 *   // In main loop:
 *   Spark::FreezeDetector::GetInstance().Heartbeat();
 *   // At shutdown:
 *   Spark::FreezeDetector::GetInstance().Stop();
 * @endcode
 *
 * @note Active in Debug, Development and Release builds. Compiled to no-ops when
 * SPARK_BUILD_SHIPPING is defined (the MinSizeRel configuration).
 * @see CrashHandler.h, ThreadDebugger.h
 */

#pragma once

#include "../Core/Platform.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace Spark
{

    /**
     * @brief Configuration for the freeze detection watchdog
     */
    struct FreezeDetectorConfig
    {
        /// Seconds without a heartbeat before logging a warning
        float warningThresholdSec = 5.0f;

        /// Seconds without a heartbeat before attempting recovery
        float recoveryThresholdSec = 8.0f;

        /// Seconds without a heartbeat before generating a crash dump
        float crashThresholdSec = 15.0f;

        /// Maximum number of recovery attempts before escalating to crash dump
        uint32_t maxRecoveryAttempts = 3;

        /// How often the watchdog thread checks the heartbeat (milliseconds)
        uint32_t pollIntervalMs = 500;

        /// Whether to generate a minidump on unrecoverable freeze
        bool generateDumpOnFreeze = true;

        /// Whether to terminate the process after generating a dump
        bool terminateOnFreeze = true;

        /// Whether the detector is enabled at all
        bool enabled = true;
    };

    /**
     * @brief Freeze state reported by the watchdog
     */
    enum class FreezeState : uint8_t
    {
        Normal,  ///< Main loop is responsive
        Warning, ///< Heartbeat missed — possible stall
        Frozen,  ///< Recovery attempted — main loop should skip work
        Critical ///< Unrecoverable — dump generated or pending
    };

    /**
     * @brief Snapshot of freeze detector status for diagnostics
     */
    struct FreezeDetectorStatus
    {
        FreezeState state = FreezeState::Normal;
        float timeSinceLastHeartbeatSec = 0.0f;
        uint64_t totalHeartbeats = 0;
        uint32_t totalWarnings = 0;
        uint32_t totalRecoveryAttempts = 0;
        uint32_t totalFreezes = 0;
        bool watchdogRunning = false;
    };

    /**
     * @brief Singleton watchdog that monitors main-loop liveness
     *
     * Thread safety: Heartbeat() is lock-free (atomic store). The watchdog
     * thread only reads the heartbeat timestamp. Configuration must be set
     * before Start().
     */
    class FreezeDetector
    {
      public:
        static FreezeDetector& GetInstance();

        /// Configure thresholds (must be called before Start)
        void Configure(const FreezeDetectorConfig& config);

        /// Get the current configuration
        const FreezeDetectorConfig& GetConfig() const { return m_config; }

        /// Start the watchdog thread
        void Start();

        /// Stop the watchdog thread (blocks until joined)
        void Stop();

        /// @brief Called every frame from the main loop to signal liveness.
        /// This is lock-free and extremely cheap (single atomic store).
        void Heartbeat();

        /// @brief Check if the watchdog has requested the main loop to skip work.
        /// The main loop can poll this to cooperatively recover from a freeze.
        [[nodiscard]] bool IsRecoveryRequested() const;

        /// @brief Acknowledge recovery — resets the recovery flag.
        /// Call this after the main loop has skipped expensive work.
        void AcknowledgeRecovery();

        /// @brief Get the current freeze state (lock-free read)
        [[nodiscard]] FreezeState GetState() const;

        /// @brief Get a full status snapshot for diagnostics
        [[nodiscard]] FreezeDetectorStatus GetStatus() const;

        /// @brief Check if the watchdog thread is running
        [[nodiscard]] bool IsRunning() const { return m_running.load(std::memory_order_relaxed); }

        /// Register console commands (watchdog.status, watchdog.config, etc.)
        void RegisterConsoleCommands();

      private:
        FreezeDetector() = default;
        ~FreezeDetector();
        FreezeDetector(const FreezeDetector&) = delete;
        FreezeDetector& operator=(const FreezeDetector&) = delete;

        /// Watchdog thread entry point
        void WatchdogLoop();

        /// Escalation helpers
        void OnWarning(float elapsed);
        void OnRecoveryAttempt(float elapsed);
        void OnCriticalFreeze(float elapsed);

        FreezeDetectorConfig m_config;

        // Heartbeat timestamp — written by main thread, read by watchdog
        std::atomic<std::chrono::steady_clock::time_point> m_lastHeartbeat{std::chrono::steady_clock::time_point{}};

        // Watchdog thread state
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stopRequested{false};
        std::thread m_watchdogThread;

        // Freeze state — written by watchdog, read by main thread
        std::atomic<FreezeState> m_state{FreezeState::Normal};
        std::atomic<bool> m_recoveryRequested{false};

        // Statistics
        std::atomic<uint64_t> m_totalHeartbeats{0};
        std::atomic<uint32_t> m_totalWarnings{0};
        std::atomic<uint32_t> m_totalRecoveryAttempts{0};
        std::atomic<uint32_t> m_totalFreezes{0};
        std::atomic<uint32_t> m_currentRecoveryAttempts{0};
    };

} // namespace Spark

// ============================================================================
// Convenience macros — active in Debug + Release, no-op in Shipping
// ============================================================================

#if !defined(SPARK_BUILD_SHIPPING)

/// @brief Signal main-loop liveness to the freeze detector
#define SPARK_HEARTBEAT() Spark::FreezeDetector::GetInstance().Heartbeat()

/// @brief Check if the freeze detector wants us to skip expensive work
#define SPARK_FREEZE_RECOVERY_REQUESTED() Spark::FreezeDetector::GetInstance().IsRecoveryRequested()

/// @brief Acknowledge that recovery action was taken
#define SPARK_FREEZE_RECOVERY_ACK() Spark::FreezeDetector::GetInstance().AcknowledgeRecovery()

#else

#define SPARK_HEARTBEAT() ((void)0)
#define SPARK_FREEZE_RECOVERY_REQUESTED() (false)
#define SPARK_FREEZE_RECOVERY_ACK() ((void)0)

#endif
