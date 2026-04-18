/**
 * @file TestNetworkManagerEdgeCases.cpp
 * @brief Edge-case and error-path tests for NetworkManager
 *
 * Complements TestNetworkManagerOrchestration.cpp (state machines, handlers) and
 * TestNetworkIntegration.cpp (lifecycle, replication) with boundary conditions,
 * error recovery, and concurrent access scenarios.
 */

#include "TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "Engine/Networking/NetworkManager.h"

using namespace Spark::Net;

static void ResetNM()
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
}

// ============================================================================
// NetBuffer Edge Cases
// ============================================================================

TEST(NetBufferEdge_EmptyBuffer)
{
    NetBuffer buf;
    EXPECT_EQ(buf.GetSize(), static_cast<size_t>(0));
    EXPECT_EQ(buf.RemainingBytes(), static_cast<size_t>(0));
    EXPECT_TRUE(buf.IsValid());
}

TEST(NetBufferEdge_WriteReadRoundTrip)
{
    NetBuffer buf;
    buf.WriteUint8(255);
    buf.WriteUint16(65535);
    buf.WriteUint32(0xDEADBEEF);
    buf.WriteFloat(3.14f);
    buf.WriteString("Hello, Network!");

    // Read back from same buffer (read position starts at 0)
    EXPECT_EQ(buf.ReadUint8(), static_cast<uint8_t>(255));
    EXPECT_EQ(buf.ReadUint16(), static_cast<uint16_t>(65535));
    EXPECT_EQ(buf.ReadUint32(), static_cast<uint32_t>(0xDEADBEEF));
    EXPECT_NEAR(buf.ReadFloat(), 3.14f, 0.001f);
    EXPECT_EQ(buf.ReadString(), std::string("Hello, Network!"));
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBufferEdge_ReadPastEnd)
{
    NetBuffer buf;
    buf.WriteUint8(42);

    buf.ReadUint8(); // consume 1 byte
    EXPECT_EQ(buf.RemainingBytes(), static_cast<size_t>(0));

    // Reading past end should set error flag
    buf.ReadUint32();
    EXPECT_TRUE(buf.HasError());
}

TEST(NetBufferEdge_EmptyString)
{
    NetBuffer buf;
    buf.WriteString("");

    std::string result = buf.ReadString();
    EXPECT_EQ(result, std::string(""));
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBufferEdge_LargePayload)
{
    NetBuffer buf;
    std::vector<uint8_t> payload(4096, 0xAB);
    buf.WriteBytes(payload.data(), payload.size());

    EXPECT_GE(buf.GetSize(), payload.size());

    std::vector<uint8_t> readBack(4096);
    buf.ReadBytes(readBack.data(), readBack.size());

    EXPECT_FALSE(buf.HasError());
    EXPECT_EQ(readBack[0], static_cast<uint8_t>(0xAB));
    EXPECT_EQ(readBack[4095], static_cast<uint8_t>(0xAB));
}

TEST(NetBufferEdge_Vector3RoundTrip)
{
    NetBuffer buf;
    XMFLOAT3 vec = {1.0f, 2.0f, 3.0f};
    buf.WriteVector3(vec);

    XMFLOAT3 result = buf.ReadVector3();

    EXPECT_NEAR(result.x, 1.0f, 0.001f);
    EXPECT_NEAR(result.y, 2.0f, 0.001f);
    EXPECT_NEAR(result.z, 3.0f, 0.001f);
}

TEST(NetBufferEdge_ResetClearsAll)
{
    NetBuffer buf;
    buf.WriteUint32(12345);
    EXPECT_GT(buf.GetSize(), static_cast<size_t>(0));

    buf.Reset();
    EXPECT_EQ(buf.GetSize(), static_cast<size_t>(0));
    EXPECT_EQ(buf.GetReadPosition(), static_cast<size_t>(0));
    EXPECT_FALSE(buf.HasError());
}

TEST(NetBufferEdge_CanReadCheck)
{
    NetBuffer buf;
    buf.WriteUint8(1);

    EXPECT_TRUE(buf.CanRead(1));
    EXPECT_FALSE(buf.CanRead(2));
    EXPECT_FALSE(buf.CanRead(100));
}

TEST(NetBufferEdge_MultipleStrings)
{
    NetBuffer buf;
    buf.WriteString("first");
    buf.WriteString("second");
    buf.WriteString("third");

    EXPECT_EQ(buf.ReadString(), std::string("first"));
    EXPECT_EQ(buf.ReadString(), std::string("second"));
    EXPECT_EQ(buf.ReadString(), std::string("third"));
    EXPECT_FALSE(buf.HasError());
}

// ============================================================================
// LagCompensator Edge Cases
// ============================================================================

TEST(LagCompensator_EmptyRewind)
{
    LagCompensator compensator;
    HistorySnapshot out;

    bool ok = compensator.RewindToTime(1.0f, out);
    EXPECT_FALSE(ok);
}

TEST(LagCompensator_SingleSnapshot)
{
    LagCompensator compensator;
    HistorySnapshot snap;
    snap.timestamp = 1.0f;

    compensator.RecordSnapshot(snap);

    HistorySnapshot out;
    bool ok = compensator.RewindToTime(1.0f, out);
    EXPECT_TRUE(ok);
    EXPECT_NEAR(out.timestamp, 1.0f, 0.01f);
}

TEST(LagCompensator_InterpolateBetweenSnapshots)
{
    LagCompensator compensator;

    HistorySnapshot s1;
    s1.timestamp = 0.0f;
    compensator.RecordSnapshot(s1);

    HistorySnapshot s2;
    s2.timestamp = 1.0f;
    compensator.RecordSnapshot(s2);

    HistorySnapshot out;
    bool ok = compensator.RewindToTime(0.5f, out);
    EXPECT_TRUE(ok);
}

TEST(LagCompensator_ClearRemovesAll)
{
    LagCompensator compensator;

    HistorySnapshot snap;
    snap.timestamp = 1.0f;
    compensator.RecordSnapshot(snap);

    compensator.Clear();

    HistorySnapshot out;
    bool ok = compensator.RewindToTime(1.0f, out);
    EXPECT_FALSE(ok);
}

// ============================================================================
// NetworkManager State Transition Edge Cases
// ============================================================================

TEST(NetworkManager_ConnectWithoutInitialize)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();

    // Connect should auto-initialize
    bool ok = nm.Connect("127.0.0.1", 27015);
    EXPECT_TRUE(nm.IsInitialized());
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Client));

    ResetNM();
}

TEST(NetworkManager_StopServerTwice)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);
    nm.StopServer();
    // Second stop should be harmless
    nm.StopServer();
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    ResetNM();
}

TEST(NetworkManager_DisconnectWithoutConnect)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();
    // Disconnect when not connected should be harmless
    nm.Disconnect();
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    ResetNM();
}

TEST(NetworkManager_StatsAfterInit)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();

    auto stats = nm.GetStats();
    EXPECT_NEAR(stats.ping, 0.0f, 0.01f);
    EXPECT_EQ(stats.packetsSent, static_cast<uint64_t>(0));
    EXPECT_EQ(stats.packetsReceived, static_cast<uint64_t>(0));
    EXPECT_NEAR(stats.packetLoss, 0.0f, 0.01f);

    ResetNM();
}

TEST(NetworkManager_RegisterMultipleHandlers)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();

    int count = 0;
    nm.RegisterHandler(MessageType::ChatMessage, [&count](const NetworkMessage&) { count++; });
    nm.RegisterHandler(MessageType::UserDefined, [&count](const NetworkMessage&) { count += 10; });

    // Both handlers registered without crash
    EXPECT_TRUE(nm.IsInitialized());
    ResetNM();
}

// ============================================================================
// NetworkMessage Struct Tests
// ============================================================================

TEST(NetworkMessage_DefaultValues)
{
    NetworkMessage msg;
    EXPECT_EQ(msg.senderID, INVALID_CLIENT);
    EXPECT_EQ(msg.sequence, static_cast<SequenceNumber>(0));
    EXPECT_TRUE(msg.payload.empty());
}

TEST(NetworkMessage_PayloadCopy)
{
    NetworkMessage msg1;
    msg1.type = MessageType::ChatMessage;
    msg1.payload = {0x01, 0x02, 0x03};

    NetworkMessage msg2 = msg1;
    EXPECT_EQ(msg2.payload.size(), static_cast<size_t>(3));
    EXPECT_EQ(msg2.payload[0], static_cast<uint8_t>(0x01));
    EXPECT_EQ(static_cast<int>(msg2.type), static_cast<int>(MessageType::ChatMessage));
}

TEST(NetworkMessage_ChannelTypes)
{
    // Verify all channel types are distinct
    EXPECT_NE(static_cast<int>(ChannelType::Unreliable), static_cast<int>(ChannelType::Reliable));
    EXPECT_NE(static_cast<int>(ChannelType::Reliable), static_cast<int>(ChannelType::ReliableOrdered));
    EXPECT_NE(static_cast<int>(ChannelType::Unreliable), static_cast<int>(ChannelType::ReliableOrdered));
}

TEST(NetworkMessage_RoleTypes)
{
    EXPECT_NE(static_cast<int>(NetworkRole::None), static_cast<int>(NetworkRole::Server));
    EXPECT_NE(static_cast<int>(NetworkRole::None), static_cast<int>(NetworkRole::Client));
    EXPECT_NE(static_cast<int>(NetworkRole::Server), static_cast<int>(NetworkRole::Client));
}

TEST(NetworkMessage_ConnectionStates)
{
    EXPECT_NE(static_cast<int>(ConnectionState::Disconnected), static_cast<int>(ConnectionState::Connecting));
    EXPECT_NE(static_cast<int>(ConnectionState::Connecting), static_cast<int>(ConnectionState::Connected));
    EXPECT_NE(static_cast<int>(ConnectionState::Connected), static_cast<int>(ConnectionState::Disconnecting));
}
