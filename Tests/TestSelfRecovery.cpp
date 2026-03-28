// TestSelfRecovery.cpp - Tests for self-recovery from potentially fatal occurrences
//
// Tests cover:
// 1. FaultIsolation auto-recovery with exponential backoff
// 2. Memory pressure callback system
// 3. Network auto-reconnect state machine

#include "TestFramework.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

// ============================================================================
// Standalone reimplementation of fault isolation with auto-recovery
// (mirrors Spark::SubsystemFaultIsolator from FaultIsolation.h)
// ============================================================================

namespace
{

    struct TestFaultRecord
    {
        uint32_t faultCount = 0;
        uint32_t maxRetries = 3;
        bool enabled = true;
        std::string lastError;

        // Auto-recovery fields
        bool autoRecoveryEnabled = true;
        float cooldownSeconds = 5.0f;
        float nextRetryTime = 0.0f;
        uint32_t recoveryAttempts = 0;
        uint32_t maxRecoveryAttempts = 3;
    };

    static constexpr float MAX_BACKOFF = 60.0f;

    class TestRecoveryIsolator
    {
      public:
        void ReportFault(const std::string& name, const std::string& error, float currentTime)
        {
            auto& record = m_records[name];
            record.faultCount++;
            record.lastError = error;

            if (record.faultCount >= record.maxRetries)
            {
                record.enabled = false;

                // Calculate next retry time with exponential backoff
                float backoff =
                    record.cooldownSeconds * static_cast<float>(1u << std::min(record.recoveryAttempts, 5u));
                backoff = std::min(backoff, MAX_BACKOFF);
                record.nextRetryTime = currentTime + backoff;
            }
        }

        void Update(float engineTime)
        {
            for (auto& [name, record] : m_records)
            {
                if (record.enabled || !record.autoRecoveryEnabled)
                    continue;

                if (record.maxRecoveryAttempts > 0 && record.recoveryAttempts >= record.maxRecoveryAttempts)
                    continue;

                if (engineTime < record.nextRetryTime)
                    continue;

                // Auto-recover
                record.recoveryAttempts++;
                record.faultCount = 0;
                record.enabled = true;
            }
        }

        bool IsEnabled(const std::string& name) const
        {
            auto it = m_records.find(name);
            return it == m_records.end() || it->second.enabled;
        }

        void SetAutoRecovery(const std::string& name, bool enabled) { m_records[name].autoRecoveryEnabled = enabled; }

        void SetRecoveryCooldown(const std::string& name, float seconds)
        {
            m_records[name].cooldownSeconds = std::max(seconds, 1.0f);
        }

        void SetMaxRecoveryAttempts(const std::string& name, uint32_t max)
        {
            m_records[name].maxRecoveryAttempts = max;
        }

        const TestFaultRecord* GetRecord(const std::string& name) const
        {
            auto it = m_records.find(name);
            return it != m_records.end() ? &it->second : nullptr;
        }

      private:
        std::unordered_map<std::string, TestFaultRecord> m_records;
    };

    // ========================================================================
    // Standalone memory pressure callback system
    // ========================================================================

    enum class TestHealthStatus
    {
        Healthy,
        Warning,
        Critical
    };

    class TestPressureMonitor
    {
      public:
        using PressureCallback = std::function<void(TestHealthStatus)>;

        void RegisterCallback(const std::string& name, PressureCallback cb) { m_callbacks[name] = std::move(cb); }

        void UnregisterCallback(const std::string& name) { m_callbacks.erase(name); }

        void SetHealth(TestHealthStatus status, float currentTime)
        {
            if (status != TestHealthStatus::Healthy && status != m_previousStatus)
            {
                if (currentTime - m_lastCallbackTime >= m_cooldown)
                {
                    m_lastCallbackTime = currentTime;
                    for (const auto& [name, cb] : m_callbacks)
                        cb(status);
                }
            }
            m_previousStatus = status;
        }

        void SetCooldown(float seconds) { m_cooldown = seconds; }

      private:
        std::unordered_map<std::string, PressureCallback> m_callbacks;
        TestHealthStatus m_previousStatus = TestHealthStatus::Healthy;
        float m_lastCallbackTime = -100.0f;
        float m_cooldown = 10.0f;
    };

    // ========================================================================
    // Standalone network auto-reconnect state machine
    // ========================================================================

    struct TestReconnectConfig
    {
        bool enabled = false;
        float baseDelay = 2.0f;
        float maxDelay = 30.0f;
        uint32_t maxAttempts = 5;
    };

    class TestReconnector
    {
      public:
        void SetConfig(const TestReconnectConfig& cfg)
        {
            m_config = cfg;
            m_attempts = 0;
            m_nextRetryTime = 0.0f;
        }

        void OnConnected()
        {
            m_connected = true;
            m_wasConnected = true;
            m_attempts = 0;
        }

        void OnDisconnected() { m_connected = false; }

        // Returns true if a reconnect attempt was made
        bool Update(float currentTime)
        {
            if (m_connected || !m_config.enabled || !m_wasConnected)
                return false;

            if (m_config.maxAttempts > 0 && m_attempts >= m_config.maxAttempts)
            {
                if (!m_gaveUp)
                {
                    m_gaveUp = true;
                    if (m_failedCallback)
                        m_failedCallback();
                }
                return false;
            }

            if (currentTime < m_nextRetryTime)
                return false;

            m_attempts++;
            float backoff = m_config.baseDelay * static_cast<float>(1u << std::min(m_attempts - 1, 5u));
            backoff = std::min(backoff, m_config.maxDelay);
            m_nextRetryTime = currentTime + backoff;

            return true; // Reconnect attempted
        }

        void SetFailedCallback(std::function<void()> cb) { m_failedCallback = std::move(cb); }

        bool IsConnected() const { return m_connected; }
        uint32_t GetAttempts() const { return m_attempts; }
        bool HasGivenUp() const { return m_gaveUp; }

      private:
        TestReconnectConfig m_config;
        uint32_t m_attempts = 0;
        float m_nextRetryTime = 0.0f;
        bool m_connected = false;
        bool m_wasConnected = false;
        bool m_gaveUp = false;
        std::function<void()> m_failedCallback;
    };

} // anonymous namespace

// ============================================================================
// Tests: FaultIsolation auto-recovery
// ============================================================================

TEST(SelfRecovery_AutoRecoveryAfterCooldown)
{
    TestRecoveryIsolator isolator;
    float time = 0.0f;

    // Fault subsystem until disabled (default maxRetries=3)
    for (int i = 0; i < 3; i++)
        isolator.ReportFault("Weather", "crash", time);
    EXPECT_FALSE(isolator.IsEnabled("Weather"));

    // Before cooldown: still disabled
    time = 4.0f;
    isolator.Update(time);
    EXPECT_FALSE(isolator.IsEnabled("Weather"));

    // After cooldown (5s): auto-recovered
    time = 6.0f;
    isolator.Update(time);
    EXPECT_TRUE(isolator.IsEnabled("Weather"));

    auto* record = isolator.GetRecord("Weather");
    EXPECT_EQ(record->recoveryAttempts, 1u);
    EXPECT_EQ(record->faultCount, 0u);
}

TEST(SelfRecovery_ExponentialBackoff)
{
    TestRecoveryIsolator isolator;
    float time = 0.0f;

    // First fault cycle
    for (int i = 0; i < 3; i++)
        isolator.ReportFault("AI", "crash", time);
    EXPECT_FALSE(isolator.IsEnabled("AI"));

    // Recover at 5s (base cooldown)
    time = 6.0f;
    isolator.Update(time);
    EXPECT_TRUE(isolator.IsEnabled("AI"));

    // Second fault cycle (at time 6)
    for (int i = 0; i < 3; i++)
        isolator.ReportFault("AI", "crash again", time);
    EXPECT_FALSE(isolator.IsEnabled("AI"));

    // Should NOT recover at 11s (6 + 5) because backoff doubles to 10s
    time = 11.0f;
    isolator.Update(time);
    EXPECT_FALSE(isolator.IsEnabled("AI"));

    // Should recover at 16s (6 + 10)
    time = 17.0f;
    isolator.Update(time);
    EXPECT_TRUE(isolator.IsEnabled("AI"));

    auto* record = isolator.GetRecord("AI");
    EXPECT_EQ(record->recoveryAttempts, 2u);
}

TEST(SelfRecovery_MaxRecoveryAttemptsExhausted)
{
    TestRecoveryIsolator isolator;
    isolator.SetMaxRecoveryAttempts("Buggy", 2);
    float time = 0.0f;

    // First fault cycle + recovery
    for (int i = 0; i < 3; i++)
        isolator.ReportFault("Buggy", "error", time);
    time = 6.0f;
    isolator.Update(time);
    EXPECT_TRUE(isolator.IsEnabled("Buggy"));

    // Second fault cycle + recovery
    for (int i = 0; i < 3; i++)
        isolator.ReportFault("Buggy", "error", time);
    time = 20.0f;
    isolator.Update(time);
    EXPECT_TRUE(isolator.IsEnabled("Buggy"));

    // Third fault cycle — should NOT recover (max 2 attempts)
    for (int i = 0; i < 3; i++)
        isolator.ReportFault("Buggy", "error", time);
    time = 100.0f;
    isolator.Update(time);
    EXPECT_FALSE(isolator.IsEnabled("Buggy"));
}

TEST(SelfRecovery_AutoRecoveryCanBeDisabled)
{
    TestRecoveryIsolator isolator;
    isolator.SetAutoRecovery("Critical", false);
    float time = 0.0f;

    for (int i = 0; i < 3; i++)
        isolator.ReportFault("Critical", "error", time);
    EXPECT_FALSE(isolator.IsEnabled("Critical"));

    // Even after a long time, should stay disabled
    time = 1000.0f;
    isolator.Update(time);
    EXPECT_FALSE(isolator.IsEnabled("Critical"));
}

TEST(SelfRecovery_UnlimitedRecoveryAttempts)
{
    TestRecoveryIsolator isolator;
    isolator.SetMaxRecoveryAttempts("Resilient", 0); // unlimited
    float time = 0.0f;

    // Run 10 fault/recovery cycles
    for (int cycle = 0; cycle < 10; cycle++)
    {
        for (int i = 0; i < 3; i++)
            isolator.ReportFault("Resilient", "error", time);
        EXPECT_FALSE(isolator.IsEnabled("Resilient"));

        time += 100.0f; // Always past any backoff
        isolator.Update(time);
        EXPECT_TRUE(isolator.IsEnabled("Resilient"));
    }

    auto* record = isolator.GetRecord("Resilient");
    EXPECT_EQ(record->recoveryAttempts, 10u);
}

TEST(SelfRecovery_CustomCooldown)
{
    TestRecoveryIsolator isolator;
    isolator.SetRecoveryCooldown("Fast", 2.0f);
    float time = 0.0f;

    for (int i = 0; i < 3; i++)
        isolator.ReportFault("Fast", "error", time);
    EXPECT_FALSE(isolator.IsEnabled("Fast"));

    // Should recover after 2s, not 5s
    time = 3.0f;
    isolator.Update(time);
    EXPECT_TRUE(isolator.IsEnabled("Fast"));
}

// ============================================================================
// Tests: Memory pressure callbacks
// ============================================================================

TEST(SelfRecovery_PressureCallbackFiresOnCritical)
{
    TestPressureMonitor monitor;
    int callCount = 0;
    TestHealthStatus lastStatus = TestHealthStatus::Healthy;

    monitor.RegisterCallback("test",
                             [&](TestHealthStatus status)
                             {
                                 callCount++;
                                 lastStatus = status;
                             });

    monitor.SetHealth(TestHealthStatus::Critical, 0.0f);
    EXPECT_EQ(callCount, 1);
    EXPECT_TRUE(lastStatus == TestHealthStatus::Critical);
}

TEST(SelfRecovery_PressureCallbackCooldown)
{
    TestPressureMonitor monitor;
    monitor.SetCooldown(10.0f);
    int callCount = 0;

    monitor.RegisterCallback("test", [&](TestHealthStatus) { callCount++; });

    // First transition: fires
    monitor.SetHealth(TestHealthStatus::Critical, 0.0f);
    EXPECT_EQ(callCount, 1);

    // Back to healthy
    monitor.SetHealth(TestHealthStatus::Healthy, 1.0f);

    // Another Critical within cooldown: should NOT fire
    monitor.SetHealth(TestHealthStatus::Critical, 5.0f);
    EXPECT_EQ(callCount, 1);

    // Back to healthy and another Critical after cooldown: fires
    monitor.SetHealth(TestHealthStatus::Healthy, 11.0f);
    monitor.SetHealth(TestHealthStatus::Critical, 12.0f);
    EXPECT_EQ(callCount, 2);
}

TEST(SelfRecovery_PressureCallbackUnregister)
{
    TestPressureMonitor monitor;
    int callCount = 0;

    monitor.RegisterCallback("test", [&](TestHealthStatus) { callCount++; });
    monitor.UnregisterCallback("test");

    monitor.SetHealth(TestHealthStatus::Critical, 0.0f);
    EXPECT_EQ(callCount, 0);
}

TEST(SelfRecovery_PressureCallbackMultipleSubscribers)
{
    TestPressureMonitor monitor;
    int countA = 0, countB = 0;

    monitor.RegisterCallback("A", [&](TestHealthStatus) { countA++; });
    monitor.RegisterCallback("B", [&](TestHealthStatus) { countB++; });

    monitor.SetHealth(TestHealthStatus::Warning, 0.0f);
    EXPECT_EQ(countA, 1);
    EXPECT_EQ(countB, 1);
}

TEST(SelfRecovery_PressureCallbackNotFiredForHealthy)
{
    TestPressureMonitor monitor;
    int callCount = 0;

    monitor.RegisterCallback("test", [&](TestHealthStatus) { callCount++; });

    monitor.SetHealth(TestHealthStatus::Healthy, 0.0f);
    EXPECT_EQ(callCount, 0);
}

// ============================================================================
// Tests: Network auto-reconnect
// ============================================================================

TEST(SelfRecovery_NetworkReconnectWithBackoff)
{
    TestReconnector reconnector;
    reconnector.SetConfig({true, 2.0f, 30.0f, 5});
    reconnector.OnConnected();
    reconnector.OnDisconnected();

    // First attempt immediately
    EXPECT_TRUE(reconnector.Update(0.0f));
    EXPECT_EQ(reconnector.GetAttempts(), 1u);

    // Second attempt should wait 2s (base delay)
    EXPECT_FALSE(reconnector.Update(1.0f));
    EXPECT_TRUE(reconnector.Update(3.0f));
    EXPECT_EQ(reconnector.GetAttempts(), 2u);

    // Third attempt waits 4s (2 * 2^1)
    EXPECT_FALSE(reconnector.Update(5.0f));
    EXPECT_TRUE(reconnector.Update(8.0f));
    EXPECT_EQ(reconnector.GetAttempts(), 3u);
}

TEST(SelfRecovery_NetworkReconnectMaxAttempts)
{
    TestReconnector reconnector;
    reconnector.SetConfig({true, 1.0f, 30.0f, 3});
    reconnector.OnConnected();
    reconnector.OnDisconnected();

    bool failedCallbackFired = false;
    reconnector.SetFailedCallback([&]() { failedCallbackFired = true; });

    float time = 0.0f;
    for (int i = 0; i < 3; i++)
    {
        reconnector.Update(time);
        time += 100.0f;
    }
    EXPECT_EQ(reconnector.GetAttempts(), 3u);

    // Next update should give up
    reconnector.Update(time);
    EXPECT_TRUE(reconnector.HasGivenUp());
    EXPECT_TRUE(failedCallbackFired);

    // No more attempts
    EXPECT_FALSE(reconnector.Update(time + 100.0f));
}

TEST(SelfRecovery_NetworkReconnectDisabled)
{
    TestReconnector reconnector;
    reconnector.SetConfig({false, 2.0f, 30.0f, 5});
    reconnector.OnConnected();
    reconnector.OnDisconnected();

    EXPECT_FALSE(reconnector.Update(0.0f));
    EXPECT_EQ(reconnector.GetAttempts(), 0u);
}

TEST(SelfRecovery_NetworkReconnectResetsOnSuccess)
{
    TestReconnector reconnector;
    reconnector.SetConfig({true, 1.0f, 30.0f, 5});
    reconnector.OnConnected();
    reconnector.OnDisconnected();

    // Make 2 attempts
    float time = 0.0f;
    reconnector.Update(time);
    time += 10.0f;
    reconnector.Update(time);
    EXPECT_EQ(reconnector.GetAttempts(), 2u);

    // Simulate successful reconnection
    reconnector.OnConnected();
    EXPECT_EQ(reconnector.GetAttempts(), 0u);
    EXPECT_TRUE(reconnector.IsConnected());
}

TEST(SelfRecovery_NetworkReconnectRequiresPriorConnection)
{
    TestReconnector reconnector;
    reconnector.SetConfig({true, 1.0f, 30.0f, 5});
    // Never called OnConnected — was never connected

    EXPECT_FALSE(reconnector.Update(0.0f));
    EXPECT_EQ(reconnector.GetAttempts(), 0u);
}
