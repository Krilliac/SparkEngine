/**
 * @file TestFreezeDetector.cpp
 * @brief Tests for the FreezeDetector watchdog system
 */

#include "TestFramework.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Inline minimal FreezeDetector reproduction for unit testing
// (avoids pulling in full engine headers — mirrors the real implementation)
// ============================================================================

namespace TestFreeze
{

    enum class FreezeState : uint8_t
    {
        Normal,
        Warning,
        Frozen,
        Critical
    };

    struct FreezeDetectorConfig
    {
        float warningThresholdSec = 0.2f;  // Short for testing
        float recoveryThresholdSec = 0.4f; // Short for testing
        float crashThresholdSec = 1.0f;    // Short for testing
        uint32_t maxRecoveryAttempts = 3;
        uint32_t pollIntervalMs = 50;
        bool generateDumpOnFreeze = false; // Don't dump in tests
        bool terminateOnFreeze = false;    // Don't terminate in tests
        bool enabled = true;
    };

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

    class FreezeDetector
    {
      public:
        void Configure(const FreezeDetectorConfig& config) { m_config = config; }
        const FreezeDetectorConfig& GetConfig() const { return m_config; }

        void Start()
        {
            if (!m_config.enabled || m_running.load(std::memory_order_relaxed))
                return;
            m_stopRequested.store(false, std::memory_order_relaxed);
            m_lastHeartbeat.store(std::chrono::steady_clock::now(), std::memory_order_release);
            m_state.store(FreezeState::Normal, std::memory_order_relaxed);
            m_recoveryRequested.store(false, std::memory_order_relaxed);
            m_currentRecoveryAttempts.store(0, std::memory_order_relaxed);
            m_watchdogThread = std::thread([this] { WatchdogLoop(); });
            m_running.store(true, std::memory_order_release);
        }

        void Stop()
        {
            if (!m_running.load(std::memory_order_relaxed))
                return;
            m_stopRequested.store(true, std::memory_order_release);
            if (m_watchdogThread.joinable())
                m_watchdogThread.join();
            m_running.store(false, std::memory_order_release);
        }

        ~FreezeDetector() { Stop(); }

        void Heartbeat()
        {
            m_lastHeartbeat.store(std::chrono::steady_clock::now(), std::memory_order_release);
            m_totalHeartbeats.fetch_add(1, std::memory_order_relaxed);
            auto current = m_state.load(std::memory_order_relaxed);
            if (current != FreezeState::Normal)
            {
                m_state.store(FreezeState::Normal, std::memory_order_release);
                m_currentRecoveryAttempts.store(0, std::memory_order_relaxed);
            }
        }

        bool IsRecoveryRequested() const { return m_recoveryRequested.load(std::memory_order_acquire); }

        void AcknowledgeRecovery() { m_recoveryRequested.store(false, std::memory_order_release); }

        FreezeState GetState() const { return m_state.load(std::memory_order_acquire); }

        FreezeDetectorStatus GetStatus() const
        {
            FreezeDetectorStatus status;
            status.state = m_state.load(std::memory_order_acquire);
            status.totalHeartbeats = m_totalHeartbeats.load(std::memory_order_relaxed);
            status.totalWarnings = m_totalWarnings.load(std::memory_order_relaxed);
            status.totalRecoveryAttempts = m_totalRecoveryAttempts.load(std::memory_order_relaxed);
            status.totalFreezes = m_totalFreezes.load(std::memory_order_relaxed);
            status.watchdogRunning = m_running.load(std::memory_order_relaxed);
            auto lastHb = m_lastHeartbeat.load(std::memory_order_acquire);
            status.timeSinceLastHeartbeatSec =
                std::chrono::duration<float>(std::chrono::steady_clock::now() - lastHb).count();
            return status;
        }

        bool IsRunning() const { return m_running.load(std::memory_order_relaxed); }

      private:
        void WatchdogLoop()
        {
            const auto pollInterval = std::chrono::milliseconds(m_config.pollIntervalMs);
            while (!m_stopRequested.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(pollInterval);
                if (m_stopRequested.load(std::memory_order_acquire))
                    break;

                auto lastHb = m_lastHeartbeat.load(std::memory_order_acquire);
                float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - lastHb).count();
                auto currentState = m_state.load(std::memory_order_acquire);

                uint32_t attempts = m_currentRecoveryAttempts.load(std::memory_order_relaxed);
                if (elapsed >= m_config.crashThresholdSec && currentState != FreezeState::Critical &&
                    attempts >= m_config.maxRecoveryAttempts)
                {
                    m_state.store(FreezeState::Critical, std::memory_order_release);
                    m_totalFreezes.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (elapsed >= m_config.recoveryThresholdSec && currentState != FreezeState::Critical)
                {
                    if (currentState < FreezeState::Frozen || attempts < m_config.maxRecoveryAttempts)
                    {
                        m_currentRecoveryAttempts.fetch_add(1, std::memory_order_relaxed);
                        m_totalRecoveryAttempts.fetch_add(1, std::memory_order_relaxed);
                        m_state.store(FreezeState::Frozen, std::memory_order_release);
                        m_recoveryRequested.store(true, std::memory_order_release);
                    }
                }
                else if (elapsed >= m_config.warningThresholdSec && currentState < FreezeState::Warning)
                {
                    m_state.store(FreezeState::Warning, std::memory_order_release);
                    m_totalWarnings.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        FreezeDetectorConfig m_config;
        std::atomic<std::chrono::steady_clock::time_point> m_lastHeartbeat{std::chrono::steady_clock::time_point{}};
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stopRequested{false};
        std::thread m_watchdogThread;
        std::atomic<FreezeState> m_state{FreezeState::Normal};
        std::atomic<bool> m_recoveryRequested{false};
        std::atomic<uint64_t> m_totalHeartbeats{0};
        std::atomic<uint32_t> m_totalWarnings{0};
        std::atomic<uint32_t> m_totalRecoveryAttempts{0};
        std::atomic<uint32_t> m_totalFreezes{0};
        std::atomic<uint32_t> m_currentRecoveryAttempts{0};
    };

} // namespace TestFreeze

// ============================================================================
// Tests
// ============================================================================

TEST(FreezeDetector_InitialState)
{
    TestFreeze::FreezeDetector fd;
    EXPECT_FALSE(fd.IsRunning());
    EXPECT_EQ(static_cast<int>(fd.GetState()), static_cast<int>(TestFreeze::FreezeState::Normal));
    EXPECT_FALSE(fd.IsRecoveryRequested());
}

TEST(FreezeDetector_StartStop)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.pollIntervalMs = 20;
    fd.Configure(cfg);

    fd.Start();
    EXPECT_TRUE(fd.IsRunning());

    fd.Stop();
    EXPECT_FALSE(fd.IsRunning());
}

TEST(FreezeDetector_HeartbeatKeepsNormal)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.warningThresholdSec = 0.3f;
    cfg.recoveryThresholdSec = 0.6f;
    cfg.crashThresholdSec = 1.0f;
    cfg.pollIntervalMs = 20;
    fd.Configure(cfg);

    fd.Start();

    // Send heartbeats for 500ms — should stay Normal
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() < 0.5f)
    {
        fd.Heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(static_cast<int>(fd.GetState()), static_cast<int>(TestFreeze::FreezeState::Normal));
    EXPECT_TRUE(fd.GetStatus().totalHeartbeats > 0);
    EXPECT_EQ(fd.GetStatus().totalWarnings, 0u);

    fd.Stop();
}

TEST(FreezeDetector_WarningOnMissedHeartbeat)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.warningThresholdSec = 0.1f;
    cfg.recoveryThresholdSec = 5.0f; // High — don't escalate
    cfg.crashThresholdSec = 10.0f;
    cfg.pollIntervalMs = 20;
    fd.Configure(cfg);

    fd.Start();
    // Don't send heartbeats — wait for warning
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    auto state = fd.GetState();
    EXPECT_TRUE(state == TestFreeze::FreezeState::Warning || state == TestFreeze::FreezeState::Frozen);
    EXPECT_TRUE(fd.GetStatus().totalWarnings > 0);

    fd.Stop();
}

TEST(FreezeDetector_RecoveryRequestedOnFreeze)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.warningThresholdSec = 0.05f;
    cfg.recoveryThresholdSec = 0.15f;
    cfg.crashThresholdSec = 10.0f;
    cfg.pollIntervalMs = 20;
    fd.Configure(cfg);

    fd.Start();
    // Don't send heartbeats — wait for recovery request
    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    EXPECT_TRUE(fd.IsRecoveryRequested());
    EXPECT_TRUE(fd.GetStatus().totalRecoveryAttempts > 0);

    // Acknowledge recovery
    fd.AcknowledgeRecovery();
    EXPECT_FALSE(fd.IsRecoveryRequested());

    fd.Stop();
}

TEST(FreezeDetector_HeartbeatResetsState)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.warningThresholdSec = 0.05f;
    cfg.recoveryThresholdSec = 5.0f;
    cfg.crashThresholdSec = 10.0f;
    cfg.pollIntervalMs = 20;
    fd.Configure(cfg);

    fd.Start();
    // Wait for the watchdog to observe the missed heartbeat. A fixed sleep is
    // flaky on loaded CI hosts because the watchdog thread may not be scheduled
    // before the assertion even when the threshold has elapsed.
    const auto warningDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fd.GetState() < TestFreeze::FreezeState::Warning && std::chrono::steady_clock::now() < warningDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(fd.GetState() >= TestFreeze::FreezeState::Warning);

    // Send heartbeat to recover
    fd.Heartbeat();
    EXPECT_EQ(static_cast<int>(fd.GetState()), static_cast<int>(TestFreeze::FreezeState::Normal));

    fd.Stop();
}

TEST(FreezeDetector_DisabledDoesNotStart)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.enabled = false;
    fd.Configure(cfg);

    fd.Start();
    EXPECT_FALSE(fd.IsRunning());
}

TEST(FreezeDetector_CriticalAfterMaxRecoveryAttempts)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.warningThresholdSec = 0.05f;
    cfg.recoveryThresholdSec = 0.1f;
    cfg.crashThresholdSec = 0.5f;
    cfg.maxRecoveryAttempts = 2;
    cfg.pollIntervalMs = 20;
    cfg.generateDumpOnFreeze = false;
    cfg.terminateOnFreeze = false;
    fd.Configure(cfg);

    fd.Start();
    // Don't heartbeat — let it escalate to critical
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    EXPECT_EQ(static_cast<int>(fd.GetState()), static_cast<int>(TestFreeze::FreezeState::Critical));
    EXPECT_TRUE(fd.GetStatus().totalFreezes > 0);

    fd.Stop();
}

TEST(FreezeDetector_StatusSnapshot)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.pollIntervalMs = 20;
    fd.Configure(cfg);

    fd.Start();
    fd.Heartbeat();
    fd.Heartbeat();
    fd.Heartbeat();

    auto status = fd.GetStatus();
    EXPECT_TRUE(status.watchdogRunning);
    EXPECT_EQ(status.totalHeartbeats, 3u);
    EXPECT_EQ(static_cast<int>(status.state), static_cast<int>(TestFreeze::FreezeState::Normal));

    fd.Stop();

    status = fd.GetStatus();
    EXPECT_FALSE(status.watchdogRunning);
}

TEST(FreezeDetector_MultipleStartStopCycles)
{
    TestFreeze::FreezeDetector fd;
    TestFreeze::FreezeDetectorConfig cfg;
    cfg.pollIntervalMs = 20;
    fd.Configure(cfg);

    for (int i = 0; i < 3; ++i)
    {
        fd.Start();
        EXPECT_TRUE(fd.IsRunning());
        fd.Heartbeat();
        fd.Stop();
        EXPECT_FALSE(fd.IsRunning());
    }
}
