/**
 * @file NetworkDebugPanel.h
 * @brief Editor panel for real-time network debugging and diagnostics
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides live bandwidth/latency graphs, packet inspection, artificial
 * lag/loss simulation sliders, and entity replication monitoring.
 */

#pragma once

#include "../Core/EditorPanel.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <numeric>
#include <string>
#include <vector>

namespace SparkEditor
{

    /** @brief A single network statistics snapshot */
    struct NetworkSnapshot
    {
        float timestamp = 0.0f;       ///< Seconds since monitoring started
        float bytesSentPerSec = 0.0f; ///< Outgoing bandwidth (bytes/sec)
        float bytesRecvPerSec = 0.0f; ///< Incoming bandwidth (bytes/sec)
        float latencyMs = 0.0f;       ///< Round-trip latency (ms)
        uint32_t packetsSent = 0;     ///< Packets sent this interval
        uint32_t packetsRecv = 0;     ///< Packets received this interval
        uint32_t packetsDropped = 0;  ///< Packets dropped this interval
    };

    /** @brief A logged network packet for inspection */
    struct PacketLogEntry
    {
        float timestamp = 0.0f; ///< When the packet was sent/received
        std::string direction;  ///< "SEND" or "RECV"
        std::string packetType; ///< Packet type name
        uint32_t sizeBytes = 0; ///< Packet size
        std::string summary;    ///< Human-readable summary
    };

    /** @brief Artificial network condition settings for testing */
    struct NetworkSimulationSettings
    {
        float artificialLatencyMs = 0.0f; ///< Additional latency to inject (ms)
        float packetLossPercent = 0.0f;   ///< Percentage of packets to drop (0-100)
        float jitterMs = 0.0f;            ///< Latency variance (ms)
        float bandwidthLimitKBps = 0.0f;  ///< Bandwidth cap (KB/s, 0 = unlimited)
        bool enabled = false;             ///< Whether simulation is active
    };

    /** @brief Entity replication stats for the replication viewer */
    struct ReplicationInfo
    {
        uint32_t entityId = 0;          ///< Entity ID
        std::string entityName;         ///< Entity display name
        uint32_t dirtyProperties = 0;   ///< Number of dirty properties this frame
        uint32_t snapshotSizeBytes = 0; ///< Size of last snapshot delta
        float updateFrequency = 0.0f;   ///< Updates per second
        float priority = 0.0f;          ///< Replication priority
    };

    /**
     * @brief Editor panel for real-time network debugging
     *
     * Displays live bandwidth and latency graphs, a searchable packet log,
     * artificial network condition sliders, and entity replication stats.
     */
    class NetworkDebugPanel : public EditorPanel
    {
      public:
        NetworkDebugPanel() : EditorPanel("Network Debug", "NetworkDebug") {}

        bool Initialize() override
        {
            m_snapshots.clear();
            m_packetLog.clear();
            m_replicationInfo.clear();
            m_simulationSettings = {};
            m_monitoringStartTime = std::chrono::steady_clock::now();
            m_sampleInterval = 0.5f;
            m_timeSinceLastSample = 0.0f;
            m_graphWindowSeconds = 60.0f;
            m_maxPacketLogEntries = 500;
            m_filterPacketType = "";
            m_showSendPackets = true;
            m_showRecvPackets = true;
            m_accumulatedBytesSent = 0;
            m_accumulatedBytesRecv = 0;
            m_accumulatedPacketsSent = 0;
            m_accumulatedPacketsRecv = 0;
            m_accumulatedPacketsDropped = 0;
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "NetworkDebugPanel initialized");
            return true;
        }

        void Update(float deltaTime) override
        {
            m_timeSinceLastSample += deltaTime;
            m_totalTime += deltaTime;

            if (m_timeSinceLastSample >= m_sampleInterval)
            {
                TakeSnapshot();
                m_timeSinceLastSample = 0.0f;
            }

            // Prune old snapshots outside the graph window
            float cutoff = m_totalTime - m_graphWindowSeconds;
            while (!m_snapshots.empty() && m_snapshots.front().timestamp < cutoff)
                m_snapshots.pop_front();
        }

        void Render() override
        {
            RenderBandwidthGraph();
            RenderLatencyGraph();
            RenderSimulationControls();
            RenderPacketLog();
            RenderReplicationViewer();
            RenderConnectionSummary();
        }

        void Shutdown() override
        {
            m_snapshots.clear();
            m_packetLog.clear();
        }

        std::string GetTypeName() const override { return "NetworkDebugPanel"; }

        // --- Public API for external systems to feed data ---

        /** @brief Record bytes sent (called by NetworkManager each frame) */
        void RecordBytesSent(uint32_t bytes) { m_accumulatedBytesSent += bytes; }

        /** @brief Record bytes received */
        void RecordBytesRecv(uint32_t bytes) { m_accumulatedBytesRecv += bytes; }

        /** @brief Record a packet sent/received for the log */
        void LogPacket(const std::string& direction, const std::string& type, uint32_t sizeBytes,
                       const std::string& summary = "")
        {
            PacketLogEntry entry;
            entry.timestamp = m_totalTime;
            entry.direction = direction;
            entry.packetType = type;
            entry.sizeBytes = sizeBytes;
            entry.summary = summary;
            m_packetLog.push_back(std::move(entry));

            if (m_packetLog.size() > m_maxPacketLogEntries)
                m_packetLog.pop_front();

            if (direction == "SEND")
                m_accumulatedPacketsSent++;
            else
                m_accumulatedPacketsRecv++;
        }

        /** @brief Record a dropped packet */
        void RecordPacketDrop() { m_accumulatedPacketsDropped++; }

        /** @brief Update latency measurement */
        void SetCurrentLatency(float latencyMs) { m_currentLatencyMs = latencyMs; }

        /** @brief Update replication info for an entity */
        void UpdateReplicationInfo(const ReplicationInfo& info)
        {
            for (auto& existing : m_replicationInfo)
            {
                if (existing.entityId == info.entityId)
                {
                    existing = info;
                    return;
                }
            }
            m_replicationInfo.push_back(info);
        }

        /** @brief Get the current simulation settings */
        const NetworkSimulationSettings& GetSimulationSettings() const { return m_simulationSettings; }

        /** @brief Set simulation settings (from external systems) */
        void SetSimulationSettings(const NetworkSimulationSettings& settings) { m_simulationSettings = settings; }

        /** @brief Get the current average bandwidth (bytes/sec out) */
        float GetAverageBandwidthOut() const
        {
            if (m_snapshots.empty())
                return 0.0f;
            float total = 0.0f;
            for (const auto& s : m_snapshots)
                total += s.bytesSentPerSec;
            return total / static_cast<float>(m_snapshots.size());
        }

        /** @brief Get the current average latency (ms) */
        float GetAverageLatency() const
        {
            if (m_snapshots.empty())
                return 0.0f;
            float total = 0.0f;
            for (const auto& s : m_snapshots)
                total += s.latencyMs;
            return total / static_cast<float>(m_snapshots.size());
        }

        /** @brief Get peak latency in the current window */
        float GetPeakLatency() const
        {
            float peak = 0.0f;
            for (const auto& s : m_snapshots)
                peak = std::max(peak, s.latencyMs);
            return peak;
        }

        /** @brief Get number of snapshots in the graph window */
        size_t GetSnapshotCount() const { return m_snapshots.size(); }

        /** @brief Get number of logged packets */
        size_t GetPacketLogSize() const { return m_packetLog.size(); }

        /** @brief Get number of tracked replicated entities */
        size_t GetReplicatedEntityCount() const { return m_replicationInfo.size(); }

      private:
        void TakeSnapshot()
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "NetworkDebugPanel: TakeSnapshot at t=%.2f", m_totalTime);
            float interval = std::max(m_sampleInterval, 0.001f);
            NetworkSnapshot snapshot;
            snapshot.timestamp = m_totalTime;
            snapshot.bytesSentPerSec = static_cast<float>(m_accumulatedBytesSent) / interval;
            snapshot.bytesRecvPerSec = static_cast<float>(m_accumulatedBytesRecv) / interval;
            snapshot.latencyMs = m_currentLatencyMs;
            snapshot.packetsSent = m_accumulatedPacketsSent;
            snapshot.packetsRecv = m_accumulatedPacketsRecv;
            snapshot.packetsDropped = m_accumulatedPacketsDropped;

            m_snapshots.push_back(snapshot);

            // Reset accumulators
            m_accumulatedBytesSent = 0;
            m_accumulatedBytesRecv = 0;
            m_accumulatedPacketsSent = 0;
            m_accumulatedPacketsRecv = 0;
            m_accumulatedPacketsDropped = 0;
        }

        void RenderBandwidthGraph()
        {
            // ImGui plot: bytesSentPerSec and bytesRecvPerSec over time
            // Would use ImGui::PlotLines in real implementation
            (void)m_graphWindowSeconds;
        }

        void RenderLatencyGraph()
        {
            // ImGui plot: latencyMs over time with min/avg/max/p99 labels
        }

        void RenderSimulationControls()
        {
            // ImGui sliders for:
            // - Artificial latency (0-500ms)
            // - Packet loss (0-100%)
            // - Jitter (0-100ms)
            // - Bandwidth limit (0-unlimited KB/s)
            // - Enable/disable toggle
            (void)m_simulationSettings;
        }

        void RenderPacketLog()
        {
            // Scrollable table with columns: Time, Direction, Type, Size, Summary
            // Filter by packet type, direction
            (void)m_filterPacketType;
            (void)m_showSendPackets;
            (void)m_showRecvPackets;
        }

        void RenderReplicationViewer()
        {
            // Table: EntityID, Name, DirtyProps, SnapshotSize, UpdateHz, Priority
            // Sorted by snapshot size (biggest bandwidth consumers first)
            auto sorted = m_replicationInfo;
            std::sort(sorted.begin(), sorted.end(), [](const ReplicationInfo& a, const ReplicationInfo& b)
                      { return a.snapshotSizeBytes > b.snapshotSizeBytes; });
        }

        void RenderConnectionSummary()
        {
            // Overall stats: total bytes sent/recv, uptime, packet loss rate
        }

        // State
        std::deque<NetworkSnapshot> m_snapshots;
        std::deque<PacketLogEntry> m_packetLog;
        std::vector<ReplicationInfo> m_replicationInfo;
        NetworkSimulationSettings m_simulationSettings;

        std::chrono::steady_clock::time_point m_monitoringStartTime;
        float m_sampleInterval = 0.5f;
        float m_timeSinceLastSample = 0.0f;
        float m_totalTime = 0.0f;
        float m_graphWindowSeconds = 60.0f;
        size_t m_maxPacketLogEntries = 500;
        float m_currentLatencyMs = 0.0f;

        // Accumulators (reset each sample interval)
        uint32_t m_accumulatedBytesSent = 0;
        uint32_t m_accumulatedBytesRecv = 0;
        uint32_t m_accumulatedPacketsSent = 0;
        uint32_t m_accumulatedPacketsRecv = 0;
        uint32_t m_accumulatedPacketsDropped = 0;

        // Filters
        std::string m_filterPacketType;
        bool m_showSendPackets = true;
        bool m_showRecvPackets = true;
    };

} // namespace SparkEditor
