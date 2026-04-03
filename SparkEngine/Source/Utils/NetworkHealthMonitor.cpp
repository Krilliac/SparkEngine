/**
 * @file NetworkHealthMonitor.cpp
 * @brief Network health monitoring — RTT/loss tracking and health state classification
 */

#include "../Core/Platform.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Spark
{

    NetworkHealthMonitor& NetworkHealthMonitor::GetInstance()
    {
        static NetworkHealthMonitor instance;
        return instance;
    }

    void NetworkHealthMonitor::Initialize()
    {
        if (m_initialized)
            return;

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

    void NetworkHealthMonitor::Shutdown()
    {
        m_initialized = false;
    }

    void NetworkHealthMonitor::Configure(const NetworkHealthConfig& config)
    {
        m_config = config;
    }

    void NetworkHealthMonitor::Update(float dt)
    {
        if (!m_config.enabled || !m_initialized)
            return;

        m_timeSinceLastPacket += dt;
        m_windowElapsed += dt;

        // Refresh loss rate each measurement window
        if (m_windowElapsed >= m_config.measurementWindowSec)
        {
            if (m_windowPacketsSent > 0)
                m_currentLossRate = static_cast<float>(m_windowPacketsLost) / static_cast<float>(m_windowPacketsSent);
            else
                m_currentLossRate = 0.0f;

            m_windowPacketsSent = 0;
            m_windowPacketsLost = 0;
            m_windowElapsed = 0.0f;
        }

        // Check for disconnect timeout
        if (m_timeSinceLastPacket >= m_config.disconnectTimeoutSec && !m_wasDisconnected)
        {
            m_wasDisconnected = true;
            m_disconnectCount++;
        }

        m_state = EvaluateHealthState();
    }

    void NetworkHealthMonitor::RecordRTT(float ms)
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

    void NetworkHealthMonitor::RecordPacketSent()
    {
        if (!m_initialized)
            return;
        m_packetsSent++;
        m_windowPacketsSent++;
    }

    void NetworkHealthMonitor::RecordPacketReceived()
    {
        if (!m_initialized)
            return;
        m_packetsReceived++;

        // Reset disconnect timer on receiving any packet
        if (m_wasDisconnected)
        {
            m_wasDisconnected = false;
            m_reconnectCount++;
        }
        m_timeSinceLastPacket = 0.0f;
    }

    void NetworkHealthMonitor::RecordPacketLost()
    {
        if (!m_initialized)
            return;
        m_packetsLost++;
        m_windowPacketsLost++;
    }

    void NetworkHealthMonitor::OnDisconnect()
    {
        if (!m_initialized)
            return;
        if (!m_wasDisconnected)
        {
            m_wasDisconnected = true;
            m_disconnectCount++;
        }
    }

    void NetworkHealthMonitor::OnReconnect()
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

    NetworkHealthState NetworkHealthMonitor::EvaluateHealthState() const
    {
        // Disconnected takes priority
        if (m_wasDisconnected)
            return NetworkHealthState::Disconnected;

        // Compute average RTT from ring buffer
        float avgRtt = 0.0f;
        if (m_rttCount > 0)
        {
            float sum = 0.0f;
            for (uint32_t i = 0; i < m_rttCount; i++)
                sum += m_rttHistory[i];
            avgRtt = sum / static_cast<float>(m_rttCount);
        }

        // Critical packet loss forces Poor
        if (m_currentLossRate >= m_config.packetLossCritical)
            return NetworkHealthState::Poor;

        // RTT-based classification
        NetworkHealthState rttState = NetworkHealthState::Excellent;
        if (avgRtt >= m_config.poorRttMs)
            rttState = NetworkHealthState::Poor;
        else if (avgRtt >= m_config.degradedRttMs)
            rttState = NetworkHealthState::Degraded;
        else if (avgRtt >= m_config.goodRttMs)
            rttState = NetworkHealthState::Good;

        // Packet loss can degrade state by one level
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

    NetworkHealthStatus NetworkHealthMonitor::GetStatus() const
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

        // Compute average RTT
        if (m_rttCount > 0)
        {
            float sum = 0.0f;
            for (uint32_t i = 0; i < m_rttCount; i++)
                sum += m_rttHistory[i];
            status.avgRttMs = sum / static_cast<float>(m_rttCount);

            // Jitter = average absolute deviation from mean
            float devSum = 0.0f;
            for (uint32_t i = 0; i < m_rttCount; i++)
                devSum += std::abs(m_rttHistory[i] - status.avgRttMs);
            status.jitterMs = devSum / static_cast<float>(m_rttCount);
        }

        return status;
    }

    // ========================================================================
    // Console commands
    // ========================================================================

    static const char* HealthStateToString(NetworkHealthState s)
    {
        switch (s)
        {
        case NetworkHealthState::Excellent:
            return "Excellent";
        case NetworkHealthState::Good:
            return "Good";
        case NetworkHealthState::Degraded:
            return "Degraded";
        case NetworkHealthState::Poor:
            return "Poor";
        case NetworkHealthState::Disconnected:
            return "Disconnected";
        }
        return "Unknown";
    }

    void NetworkHealthMonitor::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "nethealth.status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto status = NetworkHealthMonitor::GetInstance().GetStatus();
                return std::format("State: {} | RTT avg: {:.1f}ms peak: {:.1f}ms jitter: {:.1f}ms\n"
                                   "Loss: {:.1f}% | Sent: {} Recv: {} Lost: {}\n"
                                   "Disconnects: {} | Reconnects: {} | Last packet: {:.1f}s ago",
                                   HealthStateToString(status.state), status.avgRttMs, status.peakRttMs,
                                   status.jitterMs, status.packetLossRate * 100.0f, status.packetsSent,
                                   status.packetsReceived, status.packetsLost, status.disconnectCount,
                                   status.reconnectCount, status.timeSinceLastPacketSec);
            },
            "Show network health status", "NetworkHealth");

        console.RegisterCommand(
            "nethealth.config",
            [](const std::vector<std::string>&) -> std::string
            {
                const auto& cfg = NetworkHealthMonitor::GetInstance().GetConfig();
                return std::format(
                    "Enabled: {} | RTT thresholds: Excellent <{:.0f}ms, Good <{:.0f}ms, Degraded <{:.0f}ms, "
                    "Poor <{:.0f}ms\n"
                    "Loss warning: {:.1f}% | Loss critical: {:.1f}% | Disconnect timeout: {:.1f}s | Window: {:.1f}s",
                    cfg.enabled ? "yes" : "no", cfg.excellentRttMs, cfg.goodRttMs, cfg.degradedRttMs, cfg.poorRttMs,
                    cfg.packetLossWarning * 100.0f, cfg.packetLossCritical * 100.0f, cfg.disconnectTimeoutSec,
                    cfg.measurementWindowSec);
            },
            "Show network health configuration", "NetworkHealth");
    }

} // namespace Spark
