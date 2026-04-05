/**
 * @file FreezeDetector.cpp
 * @brief Watchdog thread implementation for main-loop freeze detection and recovery
 */

#include "Utils/FreezeDetector.h"
#include "../Core/Platform.h"
#include "Utils/CrashHandler.h"
#include "Utils/DeadlockDetector.h"
#include "Utils/SparkConsole.h"
#include "Utils/StackTrace.h"

#include <cstdio>
#include <format>

namespace Spark
{

    FreezeDetector& FreezeDetector::GetInstance()
    {
        static FreezeDetector instance;
        return instance;
    }

    FreezeDetector::~FreezeDetector()
    {
        Stop();
    }

    void FreezeDetector::Configure(const FreezeDetectorConfig& config)
    {
        m_config = config;
    }

    void FreezeDetector::Start()
    {
        if (!m_config.enabled)
            return;

        if (m_running.load(std::memory_order_relaxed))
            return; // Already running

        m_stopRequested.store(false, std::memory_order_relaxed);
        m_lastHeartbeat.store(std::chrono::steady_clock::now(), std::memory_order_release);
        m_state.store(FreezeState::Normal, std::memory_order_relaxed);
        m_recoveryRequested.store(false, std::memory_order_relaxed);
        m_currentRecoveryAttempts.store(0, std::memory_order_relaxed);

        m_watchdogThread = std::thread([this] { WatchdogLoop(); });
        m_running.store(true, std::memory_order_release);

        SimpleConsole::GetInstance().LogInfo(
            std::format("FreezeDetector watchdog started (warn={:.1f}s, recover={:.1f}s, crash={:.1f}s)",
                        m_config.warningThresholdSec, m_config.recoveryThresholdSec, m_config.crashThresholdSec));
    }

    void FreezeDetector::Stop()
    {
        if (!m_running.load(std::memory_order_relaxed))
            return;

        m_stopRequested.store(true, std::memory_order_release);
        if (m_watchdogThread.joinable())
            m_watchdogThread.join();

        m_running.store(false, std::memory_order_release);
    }

    void FreezeDetector::Heartbeat()
    {
        m_lastHeartbeat.store(std::chrono::steady_clock::now(), std::memory_order_release);
        m_totalHeartbeats.fetch_add(1, std::memory_order_relaxed);

        // If we were in a warning/frozen state, reset to normal
        auto current = m_state.load(std::memory_order_relaxed);
        if (current != FreezeState::Normal)
        {
            m_state.store(FreezeState::Normal, std::memory_order_release);
            m_currentRecoveryAttempts.store(0, std::memory_order_relaxed);
        }
    }

    bool FreezeDetector::IsRecoveryRequested() const
    {
        return m_recoveryRequested.load(std::memory_order_acquire);
    }

    void FreezeDetector::AcknowledgeRecovery()
    {
        m_recoveryRequested.store(false, std::memory_order_release);
    }

    FreezeState FreezeDetector::GetState() const
    {
        return m_state.load(std::memory_order_acquire);
    }

    FreezeDetectorStatus FreezeDetector::GetStatus() const
    {
        FreezeDetectorStatus status;
        status.state = m_state.load(std::memory_order_acquire);
        status.totalHeartbeats = m_totalHeartbeats.load(std::memory_order_relaxed);
        status.totalWarnings = m_totalWarnings.load(std::memory_order_relaxed);
        status.totalRecoveryAttempts = m_totalRecoveryAttempts.load(std::memory_order_relaxed);
        status.totalFreezes = m_totalFreezes.load(std::memory_order_relaxed);
        status.watchdogRunning = m_running.load(std::memory_order_relaxed);

        auto lastHb = m_lastHeartbeat.load(std::memory_order_acquire);
        auto now = std::chrono::steady_clock::now();
        status.timeSinceLastHeartbeatSec = std::chrono::duration<float>(now - lastHb).count();

        return status;
    }

    // ========================================================================
    // Watchdog thread
    // ========================================================================

    void FreezeDetector::WatchdogLoop()
    {
        const auto pollInterval = std::chrono::milliseconds(m_config.pollIntervalMs);

        while (!m_stopRequested.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(pollInterval);

            if (m_stopRequested.load(std::memory_order_acquire))
                break;

            auto lastHb = m_lastHeartbeat.load(std::memory_order_acquire);
            auto now = std::chrono::steady_clock::now();
            float elapsedSec = std::chrono::duration<float>(now - lastHb).count();

            auto currentState = m_state.load(std::memory_order_acquire);

            // Check if we've exhausted recovery attempts and should escalate to critical
            uint32_t attempts = m_currentRecoveryAttempts.load(std::memory_order_relaxed);
            if (elapsedSec >= m_config.crashThresholdSec && currentState != FreezeState::Critical &&
                attempts >= m_config.maxRecoveryAttempts)
            {
                OnCriticalFreeze(elapsedSec);
                continue;
            }

            if (elapsedSec >= m_config.recoveryThresholdSec && currentState != FreezeState::Critical)
            {
                // Increment recovery attempts each poll cycle while frozen
                if (currentState < FreezeState::Frozen)
                    OnRecoveryAttempt(elapsedSec);
                else if (attempts < m_config.maxRecoveryAttempts)
                    OnRecoveryAttempt(elapsedSec);
            }
            else if (elapsedSec >= m_config.warningThresholdSec && currentState < FreezeState::Warning)
            {
                OnWarning(elapsedSec);
            }
        }
    }

    void FreezeDetector::OnWarning(float elapsed)
    {
        m_state.store(FreezeState::Warning, std::memory_order_release);
        m_totalWarnings.fetch_add(1, std::memory_order_relaxed);

        std::fprintf(stderr, "[FreezeDetector] WARNING: Main loop unresponsive for %.1f seconds\n", elapsed);

        // Check if the freeze is caused by a deadlock
        auto deadlock = DeadlockDetector::GetInstance().CheckForDeadlock();
        if (deadlock)
        {
            std::fprintf(stderr, "[FreezeDetector] %s", deadlock->description.c_str());
        }
    }

    void FreezeDetector::OnRecoveryAttempt(float elapsed)
    {
        uint32_t attempt = m_currentRecoveryAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
        m_totalRecoveryAttempts.fetch_add(1, std::memory_order_relaxed);
        m_state.store(FreezeState::Frozen, std::memory_order_release);
        m_recoveryRequested.store(true, std::memory_order_release);

        std::fprintf(stderr,
                     "[FreezeDetector] FREEZE: Main loop frozen for %.1f seconds — "
                     "recovery attempt %u/%u (signaling main thread to skip frame)\n",
                     elapsed, attempt, m_config.maxRecoveryAttempts);
    }

    void FreezeDetector::OnCriticalFreeze(float elapsed)
    {
        m_state.store(FreezeState::Critical, std::memory_order_release);
        m_totalFreezes.fetch_add(1, std::memory_order_relaxed);

        std::fprintf(stderr,
                     "[FreezeDetector] CRITICAL: Main loop frozen for %.1f seconds — "
                     "all recovery attempts exhausted\n",
                     elapsed);

        // Capture stack trace of the watchdog thread (main thread stack requires
        // platform-specific suspension — the minidump captures all threads)
        auto trace = StackTrace::Capture(2);
        std::fprintf(stderr, "[FreezeDetector] Watchdog stack:\n%s\n", trace.ToString().c_str());

        if (m_config.generateDumpOnFreeze)
        {
            std::fprintf(stderr, "[FreezeDetector] Generating crash dump...\n");
            std::string msg = std::format("Freeze detected: main loop unresponsive for {:.1f}s", elapsed);
            TriggerCrashHandler(msg.c_str());
        }

        if (m_config.terminateOnFreeze)
        {
            std::fprintf(stderr, "[FreezeDetector] Terminating process due to unrecoverable freeze.\n");
            std::fflush(stderr);
            // Use _exit to avoid atexit handlers that might deadlock
            std::_Exit(1);
        }
    }

    // ========================================================================
    // Console commands
    // ========================================================================

    void FreezeDetector::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "watchdog.status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto status = FreezeDetector::GetInstance().GetStatus();
                const char* stateStr = "Unknown";
                switch (status.state)
                {
                case FreezeState::Normal:
                    stateStr = "Normal";
                    break;
                case FreezeState::Warning:
                    stateStr = "Warning";
                    break;
                case FreezeState::Frozen:
                    stateStr = "Frozen";
                    break;
                case FreezeState::Critical:
                    stateStr = "Critical";
                    break;
                }

                return std::format("Watchdog: {} | State: {} | Last heartbeat: {:.2f}s ago\n"
                                   "Heartbeats: {} | Warnings: {} | Recovery attempts: {} | Freezes: {}",
                                   status.watchdogRunning ? "RUNNING" : "STOPPED", stateStr,
                                   status.timeSinceLastHeartbeatSec, status.totalHeartbeats, status.totalWarnings,
                                   status.totalRecoveryAttempts, status.totalFreezes);
            },
            "Show freeze detector status", "Watchdog");

        console.RegisterCommand(
            "watchdog.config",
            [](const std::vector<std::string>&) -> std::string
            {
                const auto& cfg = FreezeDetector::GetInstance().GetConfig();
                return std::format("Enabled: {} | Warning: {:.1f}s | Recovery: {:.1f}s | Crash: {:.1f}s\n"
                                   "Max recovery attempts: {} | Poll interval: {}ms\n"
                                   "Generate dump: {} | Terminate: {}",
                                   cfg.enabled ? "yes" : "no", cfg.warningThresholdSec, cfg.recoveryThresholdSec,
                                   cfg.crashThresholdSec, cfg.maxRecoveryAttempts, cfg.pollIntervalMs,
                                   cfg.generateDumpOnFreeze ? "yes" : "no", cfg.terminateOnFreeze ? "yes" : "no");
            },
            "Show freeze detector configuration", "Watchdog");

        console.RegisterCommand(
            "watchdog.stop",
            [](const std::vector<std::string>&) -> std::string
            {
                FreezeDetector::GetInstance().Stop();
                return "Freeze detector watchdog stopped.";
            },
            "Stop the freeze detector watchdog", "Watchdog");

        console.RegisterCommand(
            "watchdog.start",
            [](const std::vector<std::string>&) -> std::string
            {
                FreezeDetector::GetInstance().Start();
                return "Freeze detector watchdog started.";
            },
            "Start the freeze detector watchdog", "Watchdog");
    }

} // namespace Spark
