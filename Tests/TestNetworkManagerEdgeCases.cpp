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
#include "Engine/Networking/NetworkBindPolicy.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>

using namespace Spark::Net;

TEST(NetworkBindPolicy_RequiresCanonicalLoopbackOrExactRfc1918SubnetHost)
{
    const NetworkEndpointPolicy defaultPolicy = ResolveNetworkEndpointPolicy(nullptr);
    EXPECT_TRUE(defaultPolicy.IsValid());
    EXPECT_EQ(defaultPolicy.BindAddress(), uint32_t{0x7F000001u});
    EXPECT_EQ(static_cast<int>(defaultPolicy.PeerScope()), static_cast<int>(NetworkPeerScope::LoopbackOnly));
    EXPECT_FALSE(NetworkEndpointPolicy::Loopback(0u).IsValid());
    EXPECT_FALSE(NetworkEndpointPolicy::PrivateLan(0u, 24u).IsValid());
    EXPECT_FALSE(NetworkEndpointPolicy::PrivateLan(0x64400001u, 24u).IsValid());

    for (const std::string_view allowed :
         {"local", "loopback", "localhost", "127.8.9.10", "10.1.2.3/8", "172.16.4.5/12",
          "172.31.254.1/12", "192.168.7.8/24", "192.168.7.9/30", "192.168.1.0/23",
          "192.168.0.255/23"})
        EXPECT_TRUE(ResolveNetworkEndpointPolicy(allowed).IsValid());

    for (const std::string_view rejected : {"",
                                            "all",
                                            "any",
                                            "public",
                                            "0.0.0.0",
                                            "test",
                                            "multicast",
                                            "broadcast",
                                            "cgnat",
                                            "8.8.8.8",
                                            "169.254.1.1",
                                            "192.0.2.1",
                                            "198.51.100.2",
                                            "203.0.113.3",
                                            "100.64.0.1",
                                            "224.0.0.1",
                                             "255.255.255.255",
                                             "::ffff:127.0.0.1",
                                             "0x7f.0.0.1",
                                             "127.1",
                                             "0127.0.0.1",
                                             "127.000.0.1",
                                             "010.1.2.3/8",
                                             "192.168.001.9/24",
                                             "127.0.0.0",
                                             "127.255.255.255",
                                             "10.1.2.3",
                                             "10.1.2.3/7",
                                             "10.1.2.3/031",
                                             "10.1.2.3/31",
                                             "10.1.2.3/32",
                                             "172.16.4.5/11",
                                             "192.168.7.8/15",
                                             "192.168.1.0/24",
                                             "192.168.1.255/24",
                                             "192.168.1.127/25",
                                             "192.168.1.128/25",
                                             "typo"})
        EXPECT_FALSE(ResolveNetworkEndpointPolicy(rejected).IsValid());

    EXPECT_EQ(static_cast<int>(ResolveNetworkEndpointPolicy("192.168.1.20").Error()),
              static_cast<int>(NetworkEndpointPolicyError::MissingPrefix));
    EXPECT_EQ(static_cast<int>(ResolveNetworkEndpointPolicy("192.168.1.0/24").Error()),
              static_cast<int>(NetworkEndpointPolicyError::NetworkAddress));
    EXPECT_EQ(static_cast<int>(ResolveNetworkEndpointPolicy("192.168.1.255/24").Error()),
              static_cast<int>(NetworkEndpointPolicyError::BroadcastAddress));

    const NetworkEndpointPolicy privatePolicy = ResolveNetworkEndpointPolicy("192.168.50.10/24");
    EXPECT_EQ(static_cast<int>(privatePolicy.PeerScope()), static_cast<int>(NetworkPeerScope::PrivateLan));
    EXPECT_EQ(privatePolicy.SubnetPrefixLength(), static_cast<uint8_t>(24));
    EXPECT_EQ(privatePolicy.NetworkAddress(), uint32_t{0xC0A83200u});
    EXPECT_EQ(privatePolicy.BroadcastAddress(), uint32_t{0xC0A832FFu});
    EXPECT_TRUE(privatePolicy.AllowsPeerAddress(0xC0A83214u));
    EXPECT_FALSE(privatePolicy.AllowsPeerAddress(0xC0A83200u));
    EXPECT_FALSE(privatePolicy.AllowsPeerAddress(0xC0A832FFu));
    EXPECT_FALSE(privatePolicy.AllowsPeerAddress(0xC0A83314u));
    EXPECT_FALSE(privatePolicy.AllowsPeerAddress(0x0A010203u));
    EXPECT_FALSE(privatePolicy.AllowsPeerAddress(0x7F000001u));
    EXPECT_FALSE(privatePolicy.AllowsPeerAddress(0xC0000201u));

    EXPECT_TRUE(IsIPv4LoopbackAddress("127.0.0.1"));
    EXPECT_TRUE(IsIPv4LoopbackAddress("127.255.8.9"));
    EXPECT_FALSE(IsIPv4LoopbackAddress("126.255.255.255"));
    EXPECT_FALSE(IsIPv4LoopbackAddress("128.0.0.1"));
    EXPECT_FALSE(IsIPv4LoopbackAddress("127.0.0"));
    EXPECT_FALSE(IsIPv4LoopbackAddress("127.0.0.256"));
    EXPECT_FALSE(IsIPv4LoopbackAddress("127.invalid.0.1"));
    EXPECT_FALSE(IsIPv4LoopbackAddress("127.000.0.1"));
}

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

TEST(NetBufferEdge_SecureResetClearsAll)
{
    NetBuffer buf;
    const std::vector<uint8_t> secret{'s', 'e', 'c', 'r', 'e', 't'};
    buf.WriteBytes(secret.data(), secret.size());
    (void)buf.ReadUint32();
    (void)buf.ReadUint32(); // force the error state before resetting
    EXPECT_TRUE(buf.HasError());

    buf.SecureReset();
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

TEST(NetworkManager_LoopbackPolicyRejectsRemoteClientDestination)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();

    EXPECT_FALSE(nm.Connect("192.0.2.10", 27015));
    EXPECT_FALSE(nm.IsInitialized());
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_FALSE(nm.IsClientLoopback(1));
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
    EXPECT_FALSE(msg.sensitive);
    EXPECT_FALSE(msg.localOnly);
    EXPECT_EQ(msg.ownerLifecycleEpoch, static_cast<uint64_t>(0));
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

TEST(NetworkMessage_SensitiveOwnershipSurvivesCopiesAndCanBeRevoked)
{
    static_assert(std::is_nothrow_move_constructible_v<NetworkMessage>);
    static_assert(std::is_nothrow_move_assignable_v<NetworkMessage>);

    NetworkMessage original;
    original.type = MessageType::UserDefined;
    original.channel = ChannelType::Reliable;
    original.payload = {'s', 'e', 'c', 'r', 'e', 't'};
    original.sensitive = true;
    original.localOnly = true;
    original.ownerLifecycleEpoch = 42;

    NetworkMessage copied = original;
    EXPECT_TRUE(copied.sensitive);
    EXPECT_TRUE(copied.localOnly);
    EXPECT_EQ(copied.ownerLifecycleEpoch, static_cast<uint64_t>(42));
    EXPECT_TRUE(copied.payload == original.payload);

    NetworkMessage assigned;
    assigned = copied;
    EXPECT_TRUE(assigned.sensitive);
    EXPECT_TRUE(assigned.localOnly);
    EXPECT_EQ(assigned.ownerLifecycleEpoch, static_cast<uint64_t>(42));
    EXPECT_TRUE(assigned.payload == original.payload);

    NetworkMessage moved = std::move(assigned);
    EXPECT_TRUE(moved.sensitive);
    EXPECT_TRUE(moved.localOnly);
    EXPECT_EQ(moved.ownerLifecycleEpoch, static_cast<uint64_t>(42));
    EXPECT_TRUE(moved.payload == original.payload);
    EXPECT_FALSE(assigned.sensitive);
    EXPECT_FALSE(assigned.localOnly);
    EXPECT_EQ(assigned.ownerLifecycleEpoch, static_cast<uint64_t>(0));

    NetworkMessage moveAssigned;
    moveAssigned.payload = {'o', 'l', 'd'};
    moveAssigned.sensitive = true;
    moveAssigned = std::move(moved);
    EXPECT_TRUE(moveAssigned.sensitive);
    EXPECT_TRUE(moveAssigned.payload == original.payload);
    EXPECT_FALSE(moved.sensitive);

    NetworkMessage plain;
    plain.payload = {'p', 'l', 'a', 'i', 'n'};
    moveAssigned = plain;
    EXPECT_FALSE(moveAssigned.sensitive);
    EXPECT_TRUE(moveAssigned.payload == plain.payload);

    copied.ClearSensitivePayload();
    EXPECT_FALSE(copied.sensitive);
    EXPECT_TRUE(copied.payload.empty());
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

TEST(NetworkManager_ConcurrentSnapshotsSendAndStopAreSerialized)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    ASSERT_TRUE(nm.Initialize());
    ASSERT_TRUE(nm.StartServer(0, 4));
    EXPECT_TRUE(nm.GetBoundPort() != 0);

    std::atomic<bool> start{false};
    std::thread updater(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 250; ++i)
                nm.Update(0.001f);
        });
    std::thread reader(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 250; ++i)
            {
                auto stats = nm.GetStats();
                auto clients = nm.GetClients();
                auto inputs = nm.GetPendingInputs();
                (void)stats;
                (void)clients;
                (void)inputs;
            }
        });
    std::thread sender(
        [&]
        {
            NetworkMessage message;
            message.type = MessageType::Heartbeat;
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 250; ++i)
                nm.SendMessage(message);
        });

    start.store(true, std::memory_order_release);
    nm.StopServer();
    updater.join();
    reader.join();
    sender.join();

    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_TRUE(nm.GetClients().empty());
    EXPECT_EQ(nm.GetBoundPort(), static_cast<uint16_t>(0));
    nm.Shutdown();
}

TEST(NetworkManager_EphemeralPortChurnIsReleaseSafe)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    for (int cycle = 0; cycle < 12; ++cycle)
    {
        ASSERT_TRUE(nm.StartServer(0, 2));
        EXPECT_TRUE(nm.GetBoundPort() != 0);
        nm.StopServer();
        EXPECT_EQ(nm.GetBoundPort(), static_cast<uint16_t>(0));
    }
    nm.Shutdown();
}

TEST(NetworkManager_ReplicationCallbackMayWaitForCrossThreadApiCall)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    ASSERT_TRUE(nm.StartServer(0, 2));

    std::atomic<bool> launched{false};
    std::atomic<bool> workerFinished{false};
    std::atomic<bool> workerFinishedDuringCallback{false};
    std::thread worker;

    ReplicatedEntity entity;
    entity.ownerID = 0;
    entity.entityType = "CallbackLockRegression";
    ReplicatedProperty property;
    property.name = "value";
    property.type = ReplicatedProperty::Type::Int;
    property.dirty = true;
    property.serialize = [&](NetBuffer& buffer)
    {
        if (!launched.exchange(true, std::memory_order_acq_rel))
        {
            worker = std::thread(
                [&]
                {
                    (void)nm.GetStats();
                    workerFinished.store(true, std::memory_order_release);
                });

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (!workerFinished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            workerFinishedDuringCallback.store(workerFinished.load(std::memory_order_acquire),
                                               std::memory_order_release);
        }
        buffer.WriteUint32(42);
    };
    entity.properties.push_back(std::move(property));
    nm.RegisterReplicatedEntity(entity);

    nm.Update(0.1f);
    if (worker.joinable())
        worker.join();

    EXPECT_TRUE(launched.load(std::memory_order_acquire));
    EXPECT_TRUE(workerFinished.load(std::memory_order_acquire));
    // Before the callback-lock fix this is deterministically false: Update
    // holds m_apiMutex, so the worker cannot complete GetStats until the
    // callback returns.
    EXPECT_TRUE(workerFinishedDuringCallback.load(std::memory_order_acquire));

    nm.Shutdown();
}
