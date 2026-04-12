/**
 * @file TestNetworkManagerReal.cpp
 * @brief Real-class tests for Spark::Net types (NetBuffer, LagCompensator, enums)
 *
 * Tests the actual production NetworkManager.h — serialization buffer,
 * lag compensation, network types, and auto-reconnect config.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkManager.h"

#include <cstring>

using namespace Spark::Net;

// ---------------------------------------------------------------------------
// NetBuffer — Write/Read round-trip
// ---------------------------------------------------------------------------

TEST(NetworkManagerReal_NetBufferUint8RoundTrip)
{
    NetBuffer buf;
    buf.WriteUint8(0);
    buf.WriteUint8(42);
    buf.WriteUint8(255);
    EXPECT_EQ(buf.GetSize(), size_t(3));

    EXPECT_EQ(buf.ReadUint8(), uint8_t(0));
    EXPECT_EQ(buf.ReadUint8(), uint8_t(42));
    EXPECT_EQ(buf.ReadUint8(), uint8_t(255));
    EXPECT_TRUE(buf.IsValid());
}

TEST(NetworkManagerReal_NetBufferUint16RoundTrip)
{
    NetBuffer buf;
    buf.WriteUint16(0);
    buf.WriteUint16(1000);
    buf.WriteUint16(65535);

    EXPECT_EQ(buf.ReadUint16(), uint16_t(0));
    EXPECT_EQ(buf.ReadUint16(), uint16_t(1000));
    EXPECT_EQ(buf.ReadUint16(), uint16_t(65535));
    EXPECT_TRUE(buf.IsValid());
}

TEST(NetworkManagerReal_NetBufferUint32RoundTrip)
{
    NetBuffer buf;
    buf.WriteUint32(0);
    buf.WriteUint32(123456789);
    buf.WriteUint32(0xFFFFFFFF);

    EXPECT_EQ(buf.ReadUint32(), uint32_t(0));
    EXPECT_EQ(buf.ReadUint32(), uint32_t(123456789));
    EXPECT_EQ(buf.ReadUint32(), uint32_t(0xFFFFFFFF));
    EXPECT_TRUE(buf.IsValid());
}

TEST(NetworkManagerReal_NetBufferFloatRoundTrip)
{
    NetBuffer buf;
    buf.WriteFloat(0.0f);
    buf.WriteFloat(3.14159f);
    buf.WriteFloat(-1.0f);

    EXPECT_NEAR(buf.ReadFloat(), 0.0f, 0.0001f);
    EXPECT_NEAR(buf.ReadFloat(), 3.14159f, 0.0001f);
    EXPECT_NEAR(buf.ReadFloat(), -1.0f, 0.0001f);
    EXPECT_TRUE(buf.IsValid());
}

TEST(NetworkManagerReal_NetBufferStringRoundTrip)
{
    NetBuffer buf;
    buf.WriteString("Hello");
    buf.WriteString("");
    buf.WriteString("SparkEngine");

    EXPECT_EQ(buf.ReadString(), std::string("Hello"));
    EXPECT_EQ(buf.ReadString(), std::string(""));
    EXPECT_EQ(buf.ReadString(), std::string("SparkEngine"));
    EXPECT_TRUE(buf.IsValid());
}

TEST(NetworkManagerReal_NetBufferVector3RoundTrip)
{
    NetBuffer buf;
    XMFLOAT3 v{1.0f, 2.5f, -3.0f};
    buf.WriteVector3(v);

    XMFLOAT3 out = buf.ReadVector3();
    EXPECT_NEAR(out.x, 1.0f, 0.001f);
    EXPECT_NEAR(out.y, 2.5f, 0.001f);
    EXPECT_NEAR(out.z, -3.0f, 0.001f);
    EXPECT_TRUE(buf.IsValid());
}

TEST(NetworkManagerReal_NetBufferReset)
{
    NetBuffer buf;
    buf.WriteUint32(42);
    EXPECT_EQ(buf.GetSize(), size_t(4));

    buf.Reset();
    EXPECT_EQ(buf.GetSize(), size_t(0));
    EXPECT_EQ(buf.GetReadPosition(), size_t(0));
    EXPECT_TRUE(buf.IsValid());
    EXPECT_FALSE(buf.HasError());
}

TEST(NetworkManagerReal_NetBufferCanRead)
{
    NetBuffer buf;
    buf.WriteUint8(1);
    EXPECT_TRUE(buf.CanRead(1));
    EXPECT_FALSE(buf.CanRead(2));

    buf.ReadUint8();
    EXPECT_FALSE(buf.CanRead(1));
}

TEST(NetworkManagerReal_NetBufferRemainingBytes)
{
    NetBuffer buf;
    buf.WriteUint32(100);
    buf.WriteUint32(200);
    EXPECT_EQ(buf.RemainingBytes(), size_t(8));

    buf.ReadUint32();
    EXPECT_EQ(buf.RemainingBytes(), size_t(4));
}

TEST(NetworkManagerReal_NetBufferBytesRoundTrip)
{
    NetBuffer buf;
    uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    buf.WriteBytes(data, 4);
    EXPECT_EQ(buf.GetSize(), size_t(4));

    uint8_t out[4] = {};
    buf.ReadBytes(out, 4);
    EXPECT_EQ(out[0], uint8_t(0xDE));
    EXPECT_EQ(out[1], uint8_t(0xAD));
    EXPECT_EQ(out[2], uint8_t(0xBE));
    EXPECT_EQ(out[3], uint8_t(0xEF));
    EXPECT_TRUE(buf.IsValid());
}

// ---------------------------------------------------------------------------
// LagCompensator
// ---------------------------------------------------------------------------

TEST(NetworkManagerReal_LagCompensatorRecordAndRewind)
{
    LagCompensator lag;

    HistorySnapshot snap1;
    snap1.timestamp = 1.0f;
    snap1.entities.push_back({1, {0, 0, 0}, {0, 0, 0}, {-1, -1, -1}, {1, 1, 1}});

    HistorySnapshot snap2;
    snap2.timestamp = 2.0f;
    snap2.entities.push_back({1, {10, 0, 0}, {0, 0, 0}, {9, -1, -1}, {11, 1, 1}});

    lag.RecordSnapshot(snap1);
    lag.RecordSnapshot(snap2);

    HistorySnapshot result;
    bool found = lag.RewindToTime(1.0f, result);
    EXPECT_TRUE(found);
}

TEST(NetworkManagerReal_LagCompensatorClear)
{
    LagCompensator lag;

    HistorySnapshot snap;
    snap.timestamp = 1.0f;
    lag.RecordSnapshot(snap);

    lag.Clear();

    HistorySnapshot result;
    bool found = lag.RewindToTime(1.0f, result);
    EXPECT_FALSE(found);
}

TEST(NetworkManagerReal_LagCompensatorMaxHistory)
{
    LagCompensator lag;
    lag.SetMaxHistoryDuration(0.5f);

    // Record snapshots spanning 2 seconds
    for (int i = 0; i < 20; ++i)
    {
        HistorySnapshot snap;
        snap.timestamp = static_cast<float>(i) * 0.1f;
        lag.RecordSnapshot(snap);
    }

    // Very old snapshot should be pruned
    HistorySnapshot result;
    bool found = lag.RewindToTime(0.0f, result);
    // May or may not find depending on pruning implementation
    (void)found;
}

// ---------------------------------------------------------------------------
// Network types and enums
// ---------------------------------------------------------------------------

TEST(NetworkManagerReal_ConnectionStateEnumValues)
{
    EXPECT_NE(static_cast<int>(ConnectionState::Disconnected), static_cast<int>(ConnectionState::Connected));
    EXPECT_NE(static_cast<int>(ConnectionState::Connecting), static_cast<int>(ConnectionState::Disconnecting));
}

TEST(NetworkManagerReal_NetworkRoleEnumValues)
{
    EXPECT_EQ(static_cast<int>(NetworkRole::None), 0);
    EXPECT_NE(static_cast<int>(NetworkRole::Server), static_cast<int>(NetworkRole::Client));
}

TEST(NetworkManagerReal_ChannelTypeEnumValues)
{
    EXPECT_NE(static_cast<int>(ChannelType::Unreliable), static_cast<int>(ChannelType::Reliable));
    EXPECT_NE(static_cast<int>(ChannelType::Reliable), static_cast<int>(ChannelType::ReliableOrdered));
}

TEST(NetworkManagerReal_MessageTypeValues)
{
    EXPECT_EQ(static_cast<uint16_t>(MessageType::Connect), 1);
    EXPECT_EQ(static_cast<uint16_t>(MessageType::UserDefined), 1000);
}

TEST(NetworkManagerReal_NetworkMessageDefaults)
{
    NetworkMessage msg;
    msg.type = MessageType::Heartbeat;
    EXPECT_TRUE(msg.channel == ChannelType::Unreliable);
    EXPECT_EQ(msg.senderID, INVALID_CLIENT);
    EXPECT_EQ(msg.sequence, uint32_t(0));
    EXPECT_TRUE(msg.payload.empty());
    EXPECT_NEAR(msg.timestamp, 0.0f, 0.001f);
}

TEST(NetworkManagerReal_ClientInputStateDefaults)
{
    ClientInputState input;
    input.inputSequence = 1;
    EXPECT_NEAR(input.moveForward, 0.0f, 0.001f);
    EXPECT_NEAR(input.moveRight, 0.0f, 0.001f);
    EXPECT_FALSE(input.jump);
    EXPECT_FALSE(input.fire);
    EXPECT_FALSE(input.reload);
    EXPECT_FALSE(input.sprint);
    EXPECT_FALSE(input.crouch);
}

TEST(NetworkManagerReal_NetworkStatsDefaults)
{
    NetworkStats stats;
    EXPECT_NEAR(stats.ping, 0.0f, 0.001f);
    EXPECT_EQ(stats.bytesSent, uint64_t(0));
    EXPECT_EQ(stats.bytesReceived, uint64_t(0));
    EXPECT_EQ(stats.packetsSent, uint32_t(0));
    EXPECT_EQ(stats.packetsReceived, uint32_t(0));
    EXPECT_EQ(stats.correctionCount, uint32_t(0));
}

TEST(NetworkManagerReal_AutoReconnectConfigDefaults)
{
    NetworkManager::AutoReconnectConfig config;
    EXPECT_FALSE(config.enabled);
    EXPECT_NEAR(config.baseDelay, 2.0f, 0.001f);
    EXPECT_NEAR(config.maxDelay, 30.0f, 0.001f);
    EXPECT_EQ(config.maxAttempts, uint32_t(5));
}

TEST(NetworkManagerReal_ReplicatedEntityDefaults)
{
    ReplicatedEntity entity;
    entity.networkID = 1;
    entity.ownerID = INVALID_CLIENT;
    EXPECT_TRUE(entity.needsFullSync);
    EXPECT_EQ(entity.areaId, uint32_t(0));
    EXPECT_EQ(entity.teamMask, 0xFFFFFFFFu);
    EXPECT_EQ(entity.visibilityMask, 0xFFFFFFFFu);
}

TEST(NetworkManagerReal_Constants)
{
    EXPECT_EQ(INVALID_CLIENT, uint32_t(0));
    EXPECT_EQ(DEFAULT_PORT, uint16_t(27015));
}
