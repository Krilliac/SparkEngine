/**
 * @file TestNetworkHealthMonitor.cpp
 * @brief Tests for the NetworkHealthMonitor connection quality tracker
 */

#include "TestFramework.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Inline minimal NetworkHealthMonitor reproduction for unit testing
// ============================================================================

namespace TestNetHealth
{

    enum class NetworkHealthState : uint8_t
    {
        Excellent,
        Good,
        Degraded,
        Poor,
        Disconnected
    };

    struct NetworkHealthConfig
    {
        float excellentRttMs = 50.0f;
        float goodRttMs = 100.0f;
        float degradedRttMs = 200.0f;
        float poorRttMs = 500.0f;
        float packetLossWarning = 0.02f;
        float packetLossCritical = 0.10f;
        float disconnectTimeoutSec = 10.0f;
        float measurementWindowSec = 5.0f;
        bool enabled = true;
    };

    struct NetworkHealthStatus
    {
        NetworkHealthState state = NetworkHealthState::Disconnected;
        float avgRttMs = 0.0f;
        float peakRttMs = 0.0f;
        float jitterMs = 0.0f;
        float packetLossRate = 0.0f;
        uint64_t packetsSent = 0;
        uint64_t packetsReceived = 0;
        uint64_t packetsLost = 0;
        uint32_t disconnectCount = 0;
        uint32_t reconnectCount = 0;
        float timeSinceLastPacketSec = 0.0f;
    };

    class NetworkHealthMonitor
    {
      public:
        void Initialize()
        {
            m_rttHistory.fill(0.0f);
            m_rttIndex = 0;
            m_rttCount = 0;
            m_peakRttMs = 0.0f;
            m_packetsSent = 0;
            m_packetsReceived = 0;
            m_packetsLost = 0;
            m_windowPacketsSent = 0;
            m_windowPacketsLost = 0;
            m_windowElapsed = 0.0f;
            m_currentLossRate = 0.0f;
            m_timeSinceLastPacket = 0.0f;
            m_wasDisconnected = false;
            m_disconnectCount = 0;
            m_reconnectCount = 0;
            m_state = NetworkHealthState::Disconnected;
            m_initialized = true;
        }

        void Shutdown() { m_initialized = false; }

        void Configure(const NetworkHealthConfig& config) { m_config = config; }

        void Update(float dt)
        {
            if (!m_config.enabled || !m_initialized)
                return;
            m_timeSinceLastPacket += dt;
            m_windowElapsed += dt;
            if (m_windowElapsed >= m_config.measurementWindowSec)
            {
                if (m_windowPacketsSent > 0)
                    m_currentLossRate =
                        static_cast<float>(m_windowPacketsLost) / static_cast<float>(m_windowPacketsSent);
                else
                    m_currentLossRate = 0.0f;
                m_windowPacketsSent = 0;
                m_windowPacketsLost = 0;
                m_windowElapsed = 0.0f;
            }
            if (m_timeSinceLastPacket >= m_config.disconnectTimeoutSec && !m_wasDisconnected)
            {
                m_wasDisconnected = true;
                m_disconnectCount++;
            }
            m_state = EvaluateHealthState();
        }

        void RecordRTT(float ms)
        {
            if (!m_initialized)
                return;
            m_rttHistory[m_rttIndex % RTT_HISTORY_SIZE] = ms;
            m_rttIndex++;
            if (m_rttCount < RTT_HISTORY_SIZE)
                m_rttCount++;
            if (ms > m_peakRttMs)
                m_peakRttMs = ms;
        }

        void RecordPacketSent()
        {
            if (!m_initialized)
                return;
            m_packetsSent++;
            m_windowPacketsSent++;
        }

        void RecordPacketReceived()
        {
            if (!m_initialized)
                return;
            m_packetsReceived++;
            if (m_wasDisconnected)
            {
                m_wasDisconnected = false;
                m_reconnectCount++;
            }
            m_timeSinceLastPacket = 0.0f;
        }

        void RecordPacketLost()
        {
            if (!m_initialized)
                return;
            m_packetsLost++;
            m_windowPacketsLost++;
        }

        void OnDisconnect()
        {
            if (!m_initialized)
                return;
            if (!m_wasDisconnected)
            {
                m_wasDisconnected = true;
                m_disconnectCount++;
            }
        }

        void OnReconnect()
        {
            if (!m_initialized)
                return;
            if (m_wasDisconnected)
            {
                m_wasDisconnected = false;
                m_reconnectCount++;
                m_timeSinceLastPacket = 0.0f;
            }
        }

        NetworkHealthStatus GetStatus() const
        {
            NetworkHealthStatus status;
            status.state = m_state;
            status.peakRttMs = m_peakRttMs;
            status.packetsSent = m_packetsSent;
            status.packetsReceived = m_packetsReceived;
            status.packetsLost = m_packetsLost;
            status.packetLossRate = m_currentLossRate;
            status.disconnectCount = m_disconnectCount;
            status.reconnectCount = m_reconnectCount;
            status.timeSinceLastPacketSec = m_timeSinceLastPacket;
            if (m_rttCount > 0)
            {
                float sum = 0.0f;
                for (uint32_t i = 0; i < m_rttCount; i++)
                    sum += m_rttHistory[i];
                status.avgRttMs = sum / static_cast<float>(m_rttCount);
                float devSum = 0.0f;
                for (uint32_t i = 0; i < m_rttCount; i++)
                    devSum += std::abs(m_rttHistory[i] - status.avgRttMs);
                status.jitterMs = devSum / static_cast<float>(m_rttCount);
            }
            return status;
        }

      private:
        NetworkHealthState EvaluateHealthState() const
        {
            if (m_wasDisconnected)
                return NetworkHealthState::Disconnected;
            float avgRtt = 0.0f;
            if (m_rttCount > 0)
            {
                float sum = 0.0f;
                for (uint32_t i = 0; i < m_rttCount; i++)
                    sum += m_rttHistory[i];
                avgRtt = sum / static_cast<float>(m_rttCount);
            }
            if (m_currentLossRate >= m_config.packetLossCritical)
                return NetworkHealthState::Poor;
            NetworkHealthState rttState = NetworkHealthState::Excellent;
            if (avgRtt >= m_config.poorRttMs)
                rttState = NetworkHealthState::Poor;
            else if (avgRtt >= m_config.degradedRttMs)
                rttState = NetworkHealthState::Degraded;
            else if (avgRtt >= m_config.goodRttMs)
                rttState = NetworkHealthState::Good;
            if (m_currentLossRate >= m_config.packetLossWarning)
            {
                if (rttState == NetworkHealthState::Excellent)
                    return NetworkHealthState::Good;
                if (rttState == NetworkHealthState::Good)
                    return NetworkHealthState::Degraded;
                if (rttState == NetworkHealthState::Degraded)
                    return NetworkHealthState::Poor;
            }
            return rttState;
        }

        NetworkHealthConfig m_config;
        static constexpr uint32_t RTT_HISTORY_SIZE = 256;
        std::array<float, RTT_HISTORY_SIZE> m_rttHistory{};
        uint32_t m_rttIndex = 0;
        uint32_t m_rttCount = 0;
        float m_peakRttMs = 0.0f;
        uint64_t m_packetsSent = 0;
        uint64_t m_packetsReceived = 0;
        uint64_t m_packetsLost = 0;
        uint32_t m_windowPacketsSent = 0;
        uint32_t m_windowPacketsLost = 0;
        float m_windowElapsed = 0.0f;
        float m_currentLossRate = 0.0f;
        float m_timeSinceLastPacket = 0.0f;
        bool m_wasDisconnected = false;
        uint32_t m_disconnectCount = 0;
        uint32_t m_reconnectCount = 0;
        NetworkHealthState m_state = NetworkHealthState::Disconnected;
        bool m_initialized = false;
    };

} // namespace TestNetHealth

// ============================================================================
// Tests
// ============================================================================

TEST(NetHealth_InitialState)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    monitor.Initialize();
    auto status = monitor.GetStatus();
    EXPECT_TRUE(status.state == TestNetHealth::NetworkHealthState::Disconnected);
    EXPECT_TRUE(status.packetsSent == 0);
    EXPECT_TRUE(status.avgRttMs == 0.0f);
}

TEST(NetHealth_ExcellentOnLowRTT)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    monitor.Initialize();

    // Receive a packet to clear disconnected state
    monitor.RecordPacketReceived();

    // Record low RTT samples
    for (int i = 0; i < 10; i++)
        monitor.RecordRTT(30.0f);

    monitor.Update(0.016f);
    EXPECT_TRUE(monitor.GetStatus().state == TestNetHealth::NetworkHealthState::Excellent);
}

TEST(NetHealth_DegradedOnHighRTT)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    monitor.Initialize();
    monitor.RecordPacketReceived();

    for (int i = 0; i < 10; i++)
        monitor.RecordRTT(150.0f); // Between Good and Degraded

    monitor.Update(0.016f);
    EXPECT_TRUE(monitor.GetStatus().state == TestNetHealth::NetworkHealthState::Good);

    // Now push into degraded range
    for (int i = 0; i < 50; i++)
        monitor.RecordRTT(250.0f);

    monitor.Update(0.016f);
    EXPECT_TRUE(monitor.GetStatus().state == TestNetHealth::NetworkHealthState::Degraded);
}

TEST(NetHealth_DisconnectOnTimeout)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    TestNetHealth::NetworkHealthConfig cfg;
    cfg.disconnectTimeoutSec = 2.0f;
    monitor.Configure(cfg);
    monitor.Initialize();
    monitor.RecordPacketReceived(); // Start connected

    monitor.Update(0.016f);
    EXPECT_TRUE(monitor.GetStatus().state != TestNetHealth::NetworkHealthState::Disconnected);

    // Advance past timeout
    for (int i = 0; i < 150; i++)
        monitor.Update(0.016f); // ~2.4 seconds

    auto status = monitor.GetStatus();
    EXPECT_TRUE(status.state == TestNetHealth::NetworkHealthState::Disconnected);
    EXPECT_TRUE(status.disconnectCount >= 1);
}

TEST(NetHealth_ReconnectAfterDisconnect)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    TestNetHealth::NetworkHealthConfig cfg;
    cfg.disconnectTimeoutSec = 1.0f;
    monitor.Configure(cfg);
    monitor.Initialize();
    monitor.RecordPacketReceived();

    // Disconnect
    for (int i = 0; i < 70; i++)
        monitor.Update(0.016f);
    EXPECT_TRUE(monitor.GetStatus().disconnectCount >= 1);

    // Reconnect
    monitor.RecordPacketReceived();
    monitor.Update(0.016f);

    auto status = monitor.GetStatus();
    EXPECT_TRUE(status.reconnectCount >= 1);
    EXPECT_TRUE(status.state != TestNetHealth::NetworkHealthState::Disconnected);
}

TEST(NetHealth_PacketLossDegratesState)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    TestNetHealth::NetworkHealthConfig cfg;
    cfg.measurementWindowSec = 1.0f;
    cfg.packetLossWarning = 0.02f;
    monitor.Configure(cfg);
    monitor.Initialize();
    monitor.RecordPacketReceived();

    // Low RTT — would be Excellent without loss
    for (int i = 0; i < 10; i++)
        monitor.RecordRTT(20.0f);

    // 5% packet loss (above warning threshold)
    for (int i = 0; i < 100; i++)
    {
        monitor.RecordPacketSent();
        if (i % 20 == 0) // 5% loss
            monitor.RecordPacketLost();
    }

    // Advance past measurement window to compute loss rate
    monitor.Update(1.1f);

    auto status = monitor.GetStatus();
    // Excellent RTT + warning loss => degraded to Good
    EXPECT_TRUE(status.state == TestNetHealth::NetworkHealthState::Good);
}

TEST(NetHealth_CriticalLossForcesPoor)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    TestNetHealth::NetworkHealthConfig cfg;
    cfg.measurementWindowSec = 1.0f;
    cfg.packetLossCritical = 0.10f;
    monitor.Configure(cfg);
    monitor.Initialize();
    monitor.RecordPacketReceived();

    for (int i = 0; i < 10; i++)
        monitor.RecordRTT(20.0f);

    // 15% loss — above critical
    for (int i = 0; i < 100; i++)
    {
        monitor.RecordPacketSent();
        if (i % 7 == 0)
            monitor.RecordPacketLost();
    }

    monitor.Update(1.1f);
    EXPECT_TRUE(monitor.GetStatus().state == TestNetHealth::NetworkHealthState::Poor);
}

TEST(NetHealth_RTTPeakTracked)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    monitor.Initialize();

    monitor.RecordRTT(30.0f);
    monitor.RecordRTT(200.0f);
    monitor.RecordRTT(50.0f);

    auto status = monitor.GetStatus();
    EXPECT_TRUE(status.peakRttMs >= 200.0f);
}

TEST(NetHealth_JitterComputed)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    monitor.Initialize();

    // Variable RTTs should produce non-zero jitter
    monitor.RecordRTT(20.0f);
    monitor.RecordRTT(80.0f);
    monitor.RecordRTT(20.0f);
    monitor.RecordRTT(80.0f);

    auto status = monitor.GetStatus();
    EXPECT_TRUE(status.jitterMs > 20.0f);
}

TEST(NetHealth_ManualDisconnectReconnect)
{
    TestNetHealth::NetworkHealthMonitor monitor;
    monitor.Initialize();
    monitor.RecordPacketReceived();

    monitor.OnDisconnect();
    monitor.Update(0.016f);
    EXPECT_TRUE(monitor.GetStatus().state == TestNetHealth::NetworkHealthState::Disconnected);
    EXPECT_TRUE(monitor.GetStatus().disconnectCount == 1);

    monitor.OnReconnect();
    for (int i = 0; i < 10; i++)
        monitor.RecordRTT(30.0f);
    monitor.Update(0.016f);
    EXPECT_TRUE(monitor.GetStatus().state != TestNetHealth::NetworkHealthState::Disconnected);
    EXPECT_TRUE(monitor.GetStatus().reconnectCount == 1);
}
