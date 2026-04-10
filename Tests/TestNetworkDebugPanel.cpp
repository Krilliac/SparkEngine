// TestNetworkDebugPanel.cpp - Tests for SparkEditor::NetworkDebugPanel
// Standalone test that exercises the panel's data collection logic
#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// ============================================================================
// Standalone reimplementation for testing (avoids ImGui/EditorPanel dep)
// ============================================================================

namespace
{

    struct NetworkSnapshot
    {
        float timestamp = 0.0f;
        float bytesSentPerSec = 0.0f;
        float bytesRecvPerSec = 0.0f;
        float latencyMs = 0.0f;
        uint32_t packetsSent = 0;
        uint32_t packetsRecv = 0;
        uint32_t packetsDropped = 0;
    };

    struct PacketLogEntry
    {
        float timestamp = 0.0f;
        std::string direction;
        std::string packetType;
        uint32_t sizeBytes = 0;
    };

    class NetworkDebugCollector
    {
      public:
        void Initialize()
        {
            m_snapshots.clear();
            m_packetLog.clear();
            m_sampleInterval = 0.5f;
            m_timeSinceLastSample = 0.0f;
            m_totalTime = 0.0f;
            m_graphWindowSeconds = 60.0f;
            m_maxLogEntries = 500;
            m_currentLatencyMs = 0.0f;
            m_accBytesSent = 0;
            m_accBytesRecv = 0;
            m_accPktSent = 0;
            m_accPktRecv = 0;
            m_accPktDropped = 0;
        }

        void RecordBytesSent(uint32_t bytes) { m_accBytesSent += bytes; }
        void RecordBytesRecv(uint32_t bytes) { m_accBytesRecv += bytes; }
        void SetCurrentLatency(float ms) { m_currentLatencyMs = ms; }
        void RecordPacketDrop() { m_accPktDropped++; }

        void LogPacket(const std::string& dir, const std::string& type, uint32_t size)
        {
            m_packetLog.push_back({m_totalTime, dir, type, size});
            if (m_packetLog.size() > m_maxLogEntries)
                m_packetLog.pop_front();
            if (dir == "SEND")
                m_accPktSent++;
            else
                m_accPktRecv++;
        }

        void Update(float dt)
        {
            m_timeSinceLastSample += dt;
            m_totalTime += dt;

            if (m_timeSinceLastSample >= m_sampleInterval)
            {
                float interval = std::max(m_sampleInterval, 0.001f);
                NetworkSnapshot snap;
                snap.timestamp = m_totalTime;
                snap.bytesSentPerSec = static_cast<float>(m_accBytesSent) / interval;
                snap.bytesRecvPerSec = static_cast<float>(m_accBytesRecv) / interval;
                snap.latencyMs = m_currentLatencyMs;
                snap.packetsSent = m_accPktSent;
                snap.packetsRecv = m_accPktRecv;
                snap.packetsDropped = m_accPktDropped;
                m_snapshots.push_back(snap);

                m_accBytesSent = 0;
                m_accBytesRecv = 0;
                m_accPktSent = 0;
                m_accPktRecv = 0;
                m_accPktDropped = 0;
                m_timeSinceLastSample = 0.0f;

                float cutoff = m_totalTime - m_graphWindowSeconds;
                while (!m_snapshots.empty() && m_snapshots.front().timestamp < cutoff)
                    m_snapshots.pop_front();
            }
        }

        float GetAverageLatency() const
        {
            if (m_snapshots.empty())
                return 0.0f;
            float total = 0.0f;
            for (const auto& s : m_snapshots)
                total += s.latencyMs;
            return total / static_cast<float>(m_snapshots.size());
        }

        float GetPeakLatency() const
        {
            float peak = 0.0f;
            for (const auto& s : m_snapshots)
                peak = std::max(peak, s.latencyMs);
            return peak;
        }

        float GetAverageBandwidthOut() const
        {
            if (m_snapshots.empty())
                return 0.0f;
            float total = 0.0f;
            for (const auto& s : m_snapshots)
                total += s.bytesSentPerSec;
            return total / static_cast<float>(m_snapshots.size());
        }

        size_t GetSnapshotCount() const { return m_snapshots.size(); }
        size_t GetPacketLogSize() const { return m_packetLog.size(); }

      private:
        std::deque<NetworkSnapshot> m_snapshots;
        std::deque<PacketLogEntry> m_packetLog;
        float m_sampleInterval = 0.5f;
        float m_timeSinceLastSample = 0.0f;
        float m_totalTime = 0.0f;
        float m_graphWindowSeconds = 60.0f;
        size_t m_maxLogEntries = 500;
        float m_currentLatencyMs = 0.0f;
        uint32_t m_accBytesSent = 0;
        uint32_t m_accBytesRecv = 0;
        uint32_t m_accPktSent = 0;
        uint32_t m_accPktRecv = 0;
        uint32_t m_accPktDropped = 0;
    };

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST(NetworkDebug_Initialize)
{
    NetworkDebugCollector collector;
    collector.Initialize();
    EXPECT_EQ(collector.GetSnapshotCount(), static_cast<size_t>(0));
    EXPECT_EQ(collector.GetPacketLogSize(), static_cast<size_t>(0));
}

TEST(NetworkDebug_RecordBandwidth)
{
    NetworkDebugCollector collector;
    collector.Initialize();

    collector.RecordBytesSent(1000);
    collector.RecordBytesRecv(500);
    collector.Update(0.5f); // Triggers snapshot

    EXPECT_EQ(collector.GetSnapshotCount(), static_cast<size_t>(1));
    EXPECT_TRUE(collector.GetAverageBandwidthOut() > 0.0f);
}

TEST(NetworkDebug_RecordLatency)
{
    NetworkDebugCollector collector;
    collector.Initialize();

    collector.SetCurrentLatency(50.0f);
    collector.Update(0.5f);

    EXPECT_TRUE(std::abs(collector.GetAverageLatency() - 50.0f) < 0.01f);
}

TEST(NetworkDebug_PeakLatency)
{
    NetworkDebugCollector collector;
    collector.Initialize();

    collector.SetCurrentLatency(30.0f);
    collector.Update(0.5f);
    collector.SetCurrentLatency(80.0f);
    collector.Update(0.5f);
    collector.SetCurrentLatency(40.0f);
    collector.Update(0.5f);

    EXPECT_TRUE(std::abs(collector.GetPeakLatency() - 80.0f) < 0.01f);
}

TEST(NetworkDebug_PacketLog)
{
    NetworkDebugCollector collector;
    collector.Initialize();

    collector.LogPacket("SEND", "EntityUpdate", 128);
    collector.LogPacket("RECV", "Ack", 16);
    collector.LogPacket("SEND", "InputCmd", 64);

    EXPECT_EQ(collector.GetPacketLogSize(), static_cast<size_t>(3));
}

TEST(NetworkDebug_PacketLogBounded)
{
    NetworkDebugCollector collector;
    collector.Initialize();

    // Log more than max entries (500)
    for (int i = 0; i < 600; ++i)
        collector.LogPacket("SEND", "Ping", 8);

    EXPECT_TRUE(collector.GetPacketLogSize() <= 500);
}

TEST(NetworkDebug_SnapshotWindow)
{
    NetworkDebugCollector collector;
    collector.Initialize();

    // Generate 130 snapshots (65 seconds at 0.5s interval)
    // Window is 60 seconds, so older ones should be pruned
    for (int i = 0; i < 130; ++i)
    {
        collector.RecordBytesSent(100);
        collector.Update(0.5f);
    }

    // Should have ~120 snapshots (60s window / 0.5s interval)
    EXPECT_TRUE(collector.GetSnapshotCount() <= 121);
    EXPECT_TRUE(collector.GetSnapshotCount() >= 119);
}

TEST(NetworkDebug_NoSnapshotBeforeInterval)
{
    NetworkDebugCollector collector;
    collector.Initialize();

    collector.RecordBytesSent(100);
    collector.Update(0.1f); // Less than 0.5s interval

    EXPECT_EQ(collector.GetSnapshotCount(), static_cast<size_t>(0));
}

TEST(NetworkDebug_MultipleSnapshots)
{
    NetworkDebugCollector collector;
    collector.Initialize();

    collector.RecordBytesSent(1000);
    collector.SetCurrentLatency(20.0f);
    collector.Update(0.5f);

    collector.RecordBytesSent(2000);
    collector.SetCurrentLatency(40.0f);
    collector.Update(0.5f);

    EXPECT_EQ(collector.GetSnapshotCount(), static_cast<size_t>(2));
    float avgLatency = collector.GetAverageLatency();
    EXPECT_TRUE(std::abs(avgLatency - 30.0f) < 0.01f);
}

// ============================================================================
// Real-class PollEngineNetwork tests
// Gated by SPARK_TEST_HAS_IMGUI, which is defined by Tests/CMakeLists.txt
// exactly when the `imgui` target exists — the same condition under which
// SparkEditor/Source/Core/EditorPanel.cpp is added to SparkTests. Using
// __has_include(<imgui.h>) here would compile the test in configurations
// where imgui.h is on the include path (exported transitively by the
// engine target) but EditorPanel.cpp is not linked, producing undefined
// symbols for the NetworkDebugPanel base class constructor at link time.
// ============================================================================
#ifdef SPARK_TEST_HAS_IMGUI
#include "Panels/NetworkDebugPanel.h"
#include "Engine/Networking/NetworkManager.h"

TEST(NetworkDebugPanelReal_PollEngineNetworkDelta)
{
    // Seed the NetworkManager's stats counters with known values BEFORE
    // initializing the panel so the baseline is captured.
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    auto& stats = const_cast<Spark::Net::NetworkStats&>(nm.GetStats());
    const uint64_t seedSent = stats.bytesSent;
    const uint64_t seedRecv = stats.bytesReceived;
    const uint32_t seedDropped = stats.packetsDropped;

    SparkEditor::NetworkDebugPanel panel;
    EXPECT_TRUE(panel.Initialize());

    // First Update() with no new bytes — delta is zero, no snapshot yet.
    panel.Update(0.1f);
    EXPECT_EQ(panel.GetSnapshotCount(), static_cast<size_t>(0));

    // Push 2048 bytes into the engine stats and tick the panel by a full
    // sample interval (0.5s) — PollEngineNetwork should observe the delta,
    // RecordBytesSent should accumulate it, and TakeSnapshot should emit.
    stats.bytesSent = seedSent + 2048;
    stats.bytesReceived = seedRecv + 1024;
    stats.ping = 42.0f;
    panel.Update(0.5f);

    EXPECT_EQ(panel.GetSnapshotCount(), static_cast<size_t>(1));
    EXPECT_TRUE(std::abs(panel.GetAverageLatency() - 42.0f) < 0.01f);
    // Bandwidth = 2048 bytes over 0.5s interval = 4096 B/s
    EXPECT_TRUE(panel.GetAverageBandwidthOut() > 4000.0f);
    EXPECT_TRUE(panel.GetAverageBandwidthOut() < 4200.0f);

    // Restore stats so subsequent tests see a clean slate.
    stats.bytesSent = seedSent;
    stats.bytesReceived = seedRecv;
    stats.packetsDropped = seedDropped;
    stats.ping = 0.0f;
}

TEST(NetworkDebugPanelReal_PollEngineNetworkHandlesReset)
{
    // If NetworkManager's cumulative counters go BACKWARDS (e.g. after a
    // reconnect resets the stats), PollEngineNetwork must clamp the delta
    // to zero rather than wrapping a uint64 subtraction.
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    auto& stats = const_cast<Spark::Net::NetworkStats&>(nm.GetStats());
    const uint64_t seedSent = stats.bytesSent;
    const uint64_t seedRecv = stats.bytesReceived;

    // Seed baseline at something high.
    stats.bytesSent = 1'000'000;
    stats.bytesReceived = 500'000;

    SparkEditor::NetworkDebugPanel panel;
    EXPECT_TRUE(panel.Initialize());

    // Simulate a reset: counters go to zero.
    stats.bytesSent = 0;
    stats.bytesReceived = 0;

    // First poll after reset should clamp the delta and not crash /
    // emit a huge bogus snapshot value.
    panel.Update(0.5f);
    EXPECT_EQ(panel.GetSnapshotCount(), static_cast<size_t>(1));
    EXPECT_TRUE(panel.GetAverageBandwidthOut() < 1.0f);

    // Restore.
    stats.bytesSent = seedSent;
    stats.bytesReceived = seedRecv;
}
#endif // SPARK_TEST_HAS_IMGUI
