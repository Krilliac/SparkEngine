/**
 * @file NetworkHealthMonitor.h
 * @brief Monitors network connection quality with health state classification
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks round-trip time, packet loss, jitter, and disconnect events over a
 * sliding measurement window. Classifies connection health into discrete
 * states (Excellent through Disconnected) based on configurable thresholds.
 *
 * Usage:
 * @code
 *   auto& monitor = Spark::NetworkHealthMonitor::GetInstance();
 *   monitor.Initialize();
 *   // From networking code:
 *   monitor.RecordRTT(45.0f);
 *   monitor.RecordPacketSent();
 *   monitor.RecordPacketReceived();
 *   // Each frame:
 *   monitor.Update(dt);
 * @endcode
 *
 * @note Active in Debug and Release builds. Compiled to no-ops in SPARK_SHIPPING.
 * @see FreezeDetector.h, HitchDetector.h
 */

#pragma once

#include "../Core/Platform.h"

#include <array>
#include <cstdint>

namespace Spark
{

    /**
     * @brief Network connection health classification
     */
    enum class NetworkHealthState : uint8_t
    {
        Excellent,   ///< Low RTT, no loss
        Good,        ///< Acceptable RTT, minimal loss
        Degraded,    ///< Elevated RTT or noticeable loss
        Poor,        ///< High RTT or significant loss
        Disconnected ///< No packets received within timeout
    };

    /**
     * @brief Configuration for network health monitoring
     */
    struct NetworkHealthConfig
    {
        /// RTT thresholds (ms) for health state classification
        float excellentRttMs = 50.0f;
        float goodRttMs = 100.0f;
        float degradedRttMs = 200.0f;
        float poorRttMs = 500.0f;

        /// Packet loss rate thresholds
        float packetLossWarning = 0.02f;  ///< 2% — degrades state
        float packetLossCritical = 0.10f; ///< 10% — forces Poor

        /// Seconds without any packet before declaring Disconnected
        float disconnectTimeoutSec = 10.0f;

        /// Sliding window for averaging metrics (seconds)
        float measurementWindowSec = 5.0f;

        /// Whether the monitor is enabled
        bool enabled = true;
    };

    /**
     * @brief Snapshot of network health status for diagnostics
     */
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

    /**
     * @brief Singleton monitor for network connection quality
     *
     * Feed metrics from networking code via RecordRTT/RecordPacket* methods.
     * Call Update(dt) every frame to advance timers and evaluate health state.
     */
    class NetworkHealthMonitor
    {
      public:
        static NetworkHealthMonitor& GetInstance();

        void Initialize();
        void Update(float dt);
        void Shutdown();

        void Configure(const NetworkHealthConfig& config);
        [[nodiscard]] const NetworkHealthConfig& GetConfig() const { return m_config; }

        [[nodiscard]] NetworkHealthStatus GetStatus() const;

        /// Record a round-trip time measurement (milliseconds)
        void RecordRTT(float ms);

        /// Record a packet sent (increments sent counter)
        void RecordPacketSent();

        /// Record a packet received (increments received counter, resets timeout)
        void RecordPacketReceived();

        /// Record a lost packet (increments lost counter)
        void RecordPacketLost();

        /// Notify of a disconnect event
        void OnDisconnect();

        /// Notify of a reconnect event
        void OnReconnect();

        void RegisterConsoleCommands();

      private:
        NetworkHealthMonitor() = default;
        ~NetworkHealthMonitor() = default;
        NetworkHealthMonitor(const NetworkHealthMonitor&) = delete;
        NetworkHealthMonitor& operator=(const NetworkHealthMonitor&) = delete;

        [[nodiscard]] NetworkHealthState EvaluateHealthState() const;

        NetworkHealthConfig m_config;

        // RTT ring buffer
        static constexpr uint32_t RTT_HISTORY_SIZE = 256;
        std::array<float, RTT_HISTORY_SIZE> m_rttHistory{};
        uint32_t m_rttIndex = 0;
        uint32_t m_rttCount = 0;
        float m_peakRttMs = 0.0f;

        // Packet counters
        uint64_t m_packetsSent = 0;
        uint64_t m_packetsReceived = 0;
        uint64_t m_packetsLost = 0;

        // Window-based loss tracking
        uint32_t m_windowPacketsSent = 0;
        uint32_t m_windowPacketsLost = 0;
        float m_windowElapsed = 0.0f;
        float m_currentLossRate = 0.0f;

        // Disconnect tracking
        float m_timeSinceLastPacket = 0.0f;
        bool m_wasDisconnected = false;
        uint32_t m_disconnectCount = 0;
        uint32_t m_reconnectCount = 0;

        NetworkHealthState m_state = NetworkHealthState::Disconnected;
        bool m_initialized = false;
    };

} // namespace Spark

#if !defined(SPARK_SHIPPING)
#define SPARK_NET_HEALTH_UPDATE(dt) Spark::NetworkHealthMonitor::GetInstance().Update(dt)
#else
#define SPARK_NET_HEALTH_UPDATE(dt) ((void)0)
#endif
