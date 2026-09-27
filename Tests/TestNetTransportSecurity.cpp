/**
 * @file TestNetTransportSecurity.cpp
 * @brief Regression tests for NetworkManager transport hardening (SEC-net-transport lane)
 *
 * Every test drives the real singleton NetworkManager over loopback UDP with a
 * hand-built raw peer, so each one reaches the server through the same
 * ProcessIncoming -> validation -> dispatch path a remote host would use.
 * Registered as the pinned `NetTransportSecurity` CTest family.
 */

#include "TestFramework.h"
#include "Engine/Networking/DeltaSnapshotManager.h"
#include "Engine/Networking/NetworkManager.h"
#include "Fixtures/NetworkTestSecurity.h"
#include "Fixtures/SecureTestPeer.h"

#ifdef ENABLE_NETWORKING

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef SPARK_PLATFORM_WINDOWS
#include <fcntl.h>
#endif

using namespace Spark::Net;

namespace Spark::Net
{
    struct NetworkManagerTransportSecurityTestAccess
    {
        /// Position the client's outgoing reliable stream (to its server) at @p next.
        static void SeedClientOutgoingSequence(NetworkManager& manager, SequenceNumber next)
        {
            std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
            manager.GetPeerState(NetworkManager::SERVER_PEER).nextOutgoingSequence = next;
        }

        /// Position the server's in-order delivery expectation for @p client at @p next.
        static void SeedExpectedOrderedSequence(NetworkManager& manager, ClientID client, SequenceNumber next)
        {
            std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
            manager.GetPeerState(client).expectedOrderedSequence = next;
        }
    };
} // namespace Spark::Net

namespace
{
    /// Wire format documented in NetworkManager::SerializeMessage:
    /// [4] magic [2] type [1] channel [4] senderID [4] sequence [4] timestamp [4] payloadLen [N] payload
    std::vector<uint8_t> BuildWire(MessageType type, ChannelType channel, uint32_t sequence,
                                   const std::vector<uint8_t>& payload = {})
    {
        std::vector<uint8_t> packet;
        auto put32 = [&](uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
                packet.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
        };
        put32(0x5350524B);
        packet.push_back(static_cast<uint8_t>(static_cast<uint16_t>(type) & 0xFFu));
        packet.push_back(static_cast<uint8_t>((static_cast<uint16_t>(type) >> 8) & 0xFFu));
        packet.push_back(static_cast<uint8_t>(channel));
        put32(INVALID_CLIENT);
        put32(sequence);
        put32(0); // timestamp bits (0.0f)
        put32(static_cast<uint32_t>(payload.size()));
        packet.insert(packet.end(), payload.begin(), payload.end());
        return packet;
    }


    uint16_t WireType(const std::vector<uint8_t>& datagram)
    {
        if (datagram.size() < NETWORK_WIRE_HEADER_SIZE)
            return 0;
        return static_cast<uint16_t>(datagram[4] | (datagram[5] << 8));
    }

    uint32_t WireSequence(const std::vector<uint8_t>& datagram)
    {
        if (datagram.size() < NETWORK_WIRE_HEADER_SIZE)
            return 0;
        return static_cast<uint32_t>(datagram[11]) | (static_cast<uint32_t>(datagram[12]) << 8) |
               (static_cast<uint32_t>(datagram[13]) << 16) | (static_cast<uint32_t>(datagram[14]) << 24);
    }

    /// Non-blocking loopback UDP socket that speaks the raw wire format. NET-100 v2: sends are
    /// framed (handshake types) or sealed with the peer's channel once a handshake installed one,
    /// and Drain/DrainFrom return the inner messages of frames this peer can open.
    class RawPeer
    {
      public:
        explicit RawPeer(uint16_t serverPort)
        {
            m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (m_socket == INVALID_SOCKET)
                return;
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
            {
                Close();
                return;
            }
#ifdef SPARK_PLATFORM_WINDOWS
            u_long enabled = 1;
            ioctlsocket(m_socket, FIONBIO, &enabled);
#else
            const int flags = fcntl(m_socket, F_GETFL, 0);
            fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
#endif
            m_server.sin_family = AF_INET;
            m_server.sin_port = htons(serverPort);
            m_server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }

        ~RawPeer() { Close(); }
        RawPeer(const RawPeer&) = delete;
        RawPeer& operator=(const RawPeer&) = delete;

        [[nodiscard]] bool IsReady() const { return m_socket != INVALID_SOCKET; }

        bool Send(const std::vector<uint8_t>& message) const { return SendTo(m_server, message); }

        bool SendRaw(std::span<const uint8_t> datagram) const { return SendRawTo(m_server, datagram); }

        std::optional<std::vector<uint8_t>> ReceiveRaw() const
        {
            std::array<uint8_t, 8192> buffer{};
            const int received = recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()),
                                          static_cast<int>(buffer.size()), 0, nullptr, nullptr);
            if (received <= 0)
                return std::nullopt;
            return std::vector<uint8_t>(buffer.begin(), buffer.begin() + received);
        }

        /// Client role: the full v2 handshake with @p nm (pumped). Returns the assigned id.
        ClientID Handshake(NetworkManager& nm) const
        {
            auto session = SparkTestFixtures::RawHandshake(
                nm, [this](std::span<const uint8_t> datagram) { return SendRaw(datagram); },
                [this] { return ReceiveRaw(); }, "SecTransport");
            if (!session)
                return INVALID_CLIENT;
            m_channel = std::move(session->channel);
            return session->id;
        }

        /// Client role, split in two for burst tests: send the framed Connect...
        bool BeginHandshake() const
        {
            m_handshake = std::make_unique<ClientHandshake>();
            auto hello = m_handshake->Begin(NETWORK_PROTOCOL_VERSION);
            return hello && Send(SparkTestFixtures::BuildWire(MessageType::Connect, *hello, ChannelType::Reliable));
        }

        /// ...then verify the ConnectAccepted already queued and send the sealed ClientFinished.
        bool FinishHandshake() const
        {
            while (auto datagram = ReceiveRaw())
            {
                auto inner = SparkTestFixtures::OpenFrame(nullptr, *datagram);
                auto message = inner ? SparkTestFixtures::ParseWire(*inner) : std::nullopt;
                if (!message || message->type != MessageType::ConnectAccepted)
                    continue;
                constexpr size_t kPrefix = 4 + 4 + 2;
                if (message->payload.size() != kPrefix + SERVER_HELLO_SIZE)
                    return false;
                auto channel = m_handshake->Finish(std::span(message->payload).subspan(kPrefix),
                                                   SparkTestFixtures::TestServerIdentity().publicKey);
                if (!channel)
                    return false;
                m_channel = std::move(*channel);
                return Send(SparkTestFixtures::BuildWire(MessageType::ClientFinished,
                                                         SparkTestFixtures::EncodeName("SecTransport")));
            }
            return false;
        }

        /// Server role: answer @p clientHello as the pinned test identity; returns the framed accept.
        std::vector<uint8_t> AcceptClientHello(std::span<const uint8_t> clientHello, ClientID assigned) const
        {
            auto response = RespondToClientHello(clientHello, SparkTestFixtures::TestServerIdentity());
            if (!response)
                return {};
            m_channel = std::move(response->channel);
            return SparkTestFixtures::BuildWire(MessageType::ConnectAccepted,
                                                SparkTestFixtures::SecureRawServer::AcceptPayload(
                                                    assigned, NETWORK_PROTOCOL_VERSION, response->serverHello),
                                                ChannelType::Unreliable);
        }

        /// Read every datagram currently queued on the socket.
        std::vector<std::vector<uint8_t>> Drain() const
        {
            std::vector<std::vector<uint8_t>> datagrams;
            for (;;)
            {
                std::array<uint8_t, 8192> buffer{};
                const int received = recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()),
                                              static_cast<int>(buffer.size()), 0, nullptr, nullptr);
                if (received <= 0)
                    break;
                auto inner = SparkTestFixtures::OpenFrame(
                    m_channel.get(), std::span<const uint8_t>(buffer.data(), static_cast<size_t>(received)));
                if (inner)
                    datagrams.push_back(std::move(*inner));
            }
            return datagrams;
        }

        struct Datagram
        {
            std::vector<uint8_t> bytes;
            sockaddr_in from{};
        };

        /// Read every queued datagram together with its source endpoint.
        std::vector<Datagram> DrainFrom() const
        {
            std::vector<Datagram> datagrams;
            for (;;)
            {
                std::array<uint8_t, 8192> buffer{};
                Datagram datagram;
#ifdef SPARK_PLATFORM_WINDOWS
                int fromLength = sizeof(datagram.from);
#else
                socklen_t fromLength = sizeof(datagram.from);
#endif
                const int received =
                    recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                             reinterpret_cast<sockaddr*>(&datagram.from), &fromLength);
                if (received <= 0)
                    break;
                auto inner = SparkTestFixtures::OpenFrame(
                    m_channel.get(), std::span<const uint8_t>(buffer.data(), static_cast<size_t>(received)));
                if (!inner)
                    continue;
                datagram.bytes = std::move(*inner);
                datagrams.push_back(std::move(datagram));
            }
            return datagrams;
        }

        bool SendTo(const sockaddr_in& destination, const std::vector<uint8_t>& message) const
        {
            const auto frame = SparkTestFixtures::FrameForSend(m_channel.get(), message);
            return SendRawTo(destination, frame.empty() ? std::span<const uint8_t>(message) : frame);
        }

        bool SendRawTo(const sockaddr_in& destination, std::span<const uint8_t> datagram) const
        {
            return sendto(m_socket, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()),
                          0, reinterpret_cast<const sockaddr*>(&destination),
                          sizeof(destination)) == static_cast<int>(datagram.size());
        }

        [[nodiscard]] uint16_t LocalPort() const
        {
            sockaddr_in local{};
#ifdef SPARK_PLATFORM_WINDOWS
            int localLength = sizeof(local);
#else
            socklen_t localLength = sizeof(local);
#endif
            if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&local), &localLength) != 0)
                return 0;
            return ntohs(local.sin_port);
        }

        void Close()
        {
            if (m_socket != INVALID_SOCKET)
                closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }

      private:
        SOCKET m_socket = INVALID_SOCKET;
        sockaddr_in m_server{};
        mutable std::unique_ptr<SecureChannel> m_channel;     ///< Opening advances the replay window
        mutable std::unique_ptr<ClientHandshake> m_handshake; ///< BeginHandshake / FinishHandshake
    };

    NetworkManager& FreshManager()
    {
        auto& nm = NetworkManager::GetInstance();
        nm.Shutdown();
        nm.Initialize();
        return nm;
    }

    /// Pump the manager until @p done holds (loopback delivery is asynchronous).
    bool PumpUntil(NetworkManager& nm, const std::function<bool()>& done, int iterations = 100, float dt = 0.016f)
    {
        for (int i = 0; i < iterations; ++i)
        {
            if (done())
                return true;
            nm.Update(dt);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

    /// Admit one raw peer and return the ClientID the server assigned it.
    ClientID AdmitPeer(NetworkManager& nm, const RawPeer& peer)
    {
        return peer.Handshake(nm);
    }

    /// Drive the singleton as a client against @p fakeServer until the handshake completes.
    /// Returns the client's endpoint as the fake server saw it.
    bool ConnectClientToFakeServer(NetworkManager& nm, const RawPeer& fakeServer, sockaddr_in& clientEndpoint)
    {
        if (!nm.Connect("127.0.0.1", fakeServer.LocalPort(), "SessionProbe", NetworkEndpointPolicy::Loopback()))
            return false;
        std::vector<uint8_t> clientHello;
        const bool received = PumpUntil(
            nm,
            [&]
            {
                for (const auto& datagram : fakeServer.DrainFrom())
                {
                    if (WireType(datagram.bytes) == static_cast<uint16_t>(MessageType::Connect))
                    {
                        clientEndpoint = datagram.from;
                        clientHello.assign(datagram.bytes.begin() + NETWORK_WIRE_HEADER_SIZE, datagram.bytes.end());
                    }
                }
                return !clientHello.empty();
            });
        if (!received)
            return false;

        const auto accept = fakeServer.AcceptClientHello(clientHello, 42);
        if (accept.empty() || !fakeServer.SendTo(clientEndpoint, accept))
            return false;
        return PumpUntil(nm, [&] { return nm.GetConnectionState() == ConnectionState::Connected; });
    }

    /// True when the manager still exposes the pre-fix server-side input queue.
    template <typename Manager>
    constexpr bool kRetainsServerInputs = requires(const Manager& manager) { manager.GetPendingInputs(); };
} // namespace

// ============================================================================
// Finding 36: ClientInput was parsed into an unbounded, unattributed server
// queue that nothing drained. The transport no longer retains input; the
// application observer receives each datagram attributed to its sender.
// ============================================================================

TEST(NetTransportSec_ClientInputIsNotRetainedAndReachesObserverAttributed)
{
    EXPECT_FALSE(kRetainsServerInputs<NetworkManager>);

    auto& nm = FreshManager();
    ASSERT_TRUE(nm.StartServer(0, 2, NetworkEndpointPolicy::Loopback()));
    std::vector<ClientID> observedSenders;
    nm.RegisterHandler(MessageType::ClientInput,
                       [&](const NetworkMessage& message) { observedSenders.push_back(message.senderID); });
    {
        RawPeer peer(nm.GetBoundPort());
        ASSERT_TRUE(peer.IsReady());
        const ClientID admitted = AdmitPeer(nm, peer);
        ASSERT_TRUE(admitted != INVALID_CLIENT);

        // 8 bytes: shorter than the 25-byte layout the old handler zero-filled into its queue.
        for (int i = 0; i < 3; ++i)
            EXPECT_TRUE(peer.Send(
                BuildWire(MessageType::ClientInput, ChannelType::Unreliable, 0, std::vector<uint8_t>(8, 0xAB))));
        EXPECT_TRUE(PumpUntil(nm, [&] { return observedSenders.size() >= 3; }));
        EXPECT_EQ(observedSenders.size(), static_cast<size_t>(3));
        for (const ClientID sender : observedSenders)
            EXPECT_EQ(sender, admitted);
    }
    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// Finding 40: only the graceful Disconnect path released a client's
// DeltaSnapshotManager state; heartbeat timeout, kick and server stop leaked
// it into the process-global singleton for the life of the process.
// ============================================================================

TEST(NetTransportSec_TimeoutKickAndStopReleaseDeltaState)
{
    auto& deltas = DeltaSnapshotManager::GetInstance();
    auto& nm = FreshManager();
    ASSERT_TRUE(nm.StartServer(0, 4, NetworkEndpointPolicy::Loopback()));
    RawPeer silent(nm.GetBoundPort());
    RawPeer kicked(nm.GetBoundPort());
    RawPeer stopped(nm.GetBoundPort());
    ASSERT_TRUE(silent.IsReady() && kicked.IsReady() && stopped.IsReady());

    // Heartbeat timeout: the raw peer never heartbeats, so 1 s steps pass the 10 s limit.
    const ClientID silentId = AdmitPeer(nm, silent);
    ASSERT_TRUE(silentId != INVALID_CLIENT);
    EXPECT_TRUE(deltas.HasConnection(silentId));
    EXPECT_TRUE(PumpUntil(nm, [&] { return !nm.GetClients().contains(silentId); }, 20, 1.0f));
    EXPECT_FALSE(deltas.HasConnection(silentId));

    // Server-initiated kick.
    const ClientID kickedId = AdmitPeer(nm, kicked);
    ASSERT_TRUE(kickedId != INVALID_CLIENT);
    EXPECT_TRUE(deltas.HasConnection(kickedId));
    nm.KickClient(kickedId, "test kick");
    EXPECT_FALSE(nm.GetClients().contains(kickedId));
    EXPECT_FALSE(deltas.HasConnection(kickedId));

    // Whole-server stop.
    const ClientID stoppedId = AdmitPeer(nm, stopped);
    ASSERT_TRUE(stoppedId != INVALID_CLIENT);
    EXPECT_TRUE(deltas.HasConnection(stoppedId));
    nm.StopServer();
    EXPECT_FALSE(deltas.HasConnection(stoppedId));

    silent.Close();
    kicked.Close();
    stopped.Close();
    nm.Shutdown();
}

TEST(NetTransportSec_DeltaReRegistrationStartsFromEmptyBaseline)
{
    auto& deltas = DeltaSnapshotManager::GetInstance();
    constexpr uint32_t connectionId = 0x5EC40u;
    constexpr uint32_t entityId = 0x5EC41u;
    deltas.UnregisterConnection(connectionId);
    deltas.RegisterConnection(connectionId);

    FieldSnapshot field;
    field.fieldIndex = 0;
    field.serializedValue = {0x2A};
    deltas.RecordEntityState(entityId, {field});
    EXPECT_FALSE(deltas.BuildDeltaPacket(connectionId, entityId).empty());
    EXPECT_EQ(deltas.GetPendingDeltaCount(connectionId), static_cast<size_t>(1));

    // A reused ID must not inherit the earlier connection's pending deltas or baseline.
    deltas.RegisterConnection(connectionId);
    EXPECT_EQ(deltas.GetPendingDeltaCount(connectionId), static_cast<size_t>(0));
    EXPECT_FALSE(deltas.BuildDeltaPacket(connectionId, entityId).empty());

    deltas.UnregisterConnection(connectionId);
    EXPECT_FALSE(deltas.HasConnection(connectionId));
}

// ============================================================================
// Finding 41: a client never ended its session. A server Disconnect (kick or
// shutdown) ran server-only cleanup and a silent server was never detected,
// so the client stayed Connected with its socket open and auto-reconnect
// could never run.
// ============================================================================

TEST(NetTransportSec_ClientEndsSessionOnServerDisconnect)
{
    auto& nm = FreshManager();
    RawPeer fakeServer(0);
    ASSERT_TRUE(fakeServer.IsReady());
    sockaddr_in clientEndpoint{};
    ASSERT_TRUE(ConnectClientToFakeServer(nm, fakeServer, clientEndpoint));
    EXPECT_TRUE(nm.GetBoundPort() != 0);

    NetBuffer reason;
    reason.WriteString("kicked by test");
    ASSERT_TRUE(fakeServer.SendTo(clientEndpoint,
                                  BuildWire(MessageType::Disconnect, ChannelType::Reliable, 2, reason.GetData())));
    EXPECT_TRUE(PumpUntil(nm, [&] { return nm.GetConnectionState() == ConnectionState::Disconnected; }));

    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_EQ(nm.GetLocalClientID(), INVALID_CLIENT);
    EXPECT_EQ(nm.GetBoundPort(), static_cast<uint16_t>(0)); // socket closed
    EXPECT_TRUE(nm.GetLastConnectionError().find("kicked by test") != std::string::npos);

    fakeServer.Close();
    nm.Shutdown();
}

TEST(NetTransportSec_ClientDetectsSilentServerAndAutoReconnects)
{
    auto& nm = FreshManager();
    RawPeer fakeServer(0);
    ASSERT_TRUE(fakeServer.IsReady());
    sockaddr_in clientEndpoint{};
    ASSERT_TRUE(ConnectClientToFakeServer(nm, fakeServer, clientEndpoint));

    NetworkManager::AutoReconnectConfig reconnect;
    reconnect.enabled = true;
    reconnect.baseDelay = 0.5f;
    reconnect.maxDelay = 1.0f;
    reconnect.maxAttempts = 2;
    nm.SetAutoReconnect(reconnect);

    // The fake server now says nothing; 1 s steps pass the 10 s liveness limit.
    (void)fakeServer.DrainFrom();
    EXPECT_TRUE(PumpUntil(nm, [&] { return nm.GetConnectionState() == ConnectionState::Disconnected; }, 20, 1.0f));
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_TRUE(nm.GetLastConnectionError().find("timed out") != std::string::npos);
    (void)fakeServer.DrainFrom(); // retransmissions from the ended session are not a reconnect

    // The ended session unblocks auto-reconnect: a fresh Connect reaches the server.
    bool reconnectSeen = false;
    EXPECT_TRUE(PumpUntil(
        nm,
        [&]
        {
            for (const auto& datagram : fakeServer.DrainFrom())
            {
                if (WireType(datagram.bytes) == static_cast<uint16_t>(MessageType::Connect))
                    reconnectSeen = true;
            }
            return reconnectSeen;
        },
        40, 0.1f));
    EXPECT_TRUE(reconnectSeen);

    nm.SetAutoReconnect(NetworkManager::AutoReconnectConfig{});
    nm.Disconnect();
    fakeServer.Close();
    nm.Shutdown();
}

// A server-sent Disconnect is authoritative. If it left auto-reconnect armed, a
// server that kicks (or bans) a client from its Connect observer would loop
// forever: every ConnectAccepted resets the attempt counter, so maxAttempts
// never runs out and each cycle costs the server a full admission.
TEST(NetTransportSec_ServerDisconnectDisarmsAutoReconnect)
{
    auto& nm = FreshManager();
    RawPeer fakeServer(0);
    ASSERT_TRUE(fakeServer.IsReady());

    NetworkManager::AutoReconnectConfig reconnect;
    reconnect.enabled = true;
    reconnect.baseDelay = 0.1f;
    reconnect.maxDelay = 0.2f;
    reconnect.maxAttempts = 0; // unlimited: the loop the fix must prevent
    nm.SetAutoReconnect(reconnect);

    sockaddr_in clientEndpoint{};
    ASSERT_TRUE(ConnectClientToFakeServer(nm, fakeServer, clientEndpoint));
    (void)fakeServer.DrainFrom();

    NetBuffer reason;
    reason.WriteString("banned");
    ASSERT_TRUE(fakeServer.SendTo(clientEndpoint,
                                  BuildWire(MessageType::Disconnect, ChannelType::Reliable, 2, reason.GetData())));
    EXPECT_TRUE(PumpUntil(nm, [&] { return nm.GetConnectionState() == ConnectionState::Disconnected; }));
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    (void)fakeServer.DrainFrom(); // traffic from the ended session is not a reconnect

    // Several backoff periods of simulated time: no new Connect may reach the server.
    bool reconnectSeen = false;
    (void)PumpUntil(
        nm,
        [&]
        {
            for (const auto& datagram : fakeServer.DrainFrom())
            {
                if (WireType(datagram.bytes) == static_cast<uint16_t>(MessageType::Connect))
                    reconnectSeen = true;
            }
            return reconnectSeen;
        },
        40, 0.1f);
    EXPECT_FALSE(reconnectSeen);
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));

    nm.SetAutoReconnect(NetworkManager::AutoReconnectConfig{});
    nm.Disconnect();
    fakeServer.Close();
    nm.Shutdown();
}

// ============================================================================
// Finding 38: every admission synchronously ran an O(entity-count) full sync,
// so a burst of unauthenticated connects (or connect/disconnect churn) made one
// frame walk the whole replicated world once per datagram.
// ============================================================================

TEST(NetTransportSec_FullEntitySyncsAreBudgetedPerUpdate)
{
    auto& nm = FreshManager();
    ASSERT_TRUE(nm.StartServer(0, 32, NetworkEndpointPolicy::Loopback()));
    ReplicatedEntity entity;
    entity.entityType = "SyncProbe";
    ASSERT_TRUE(nm.RegisterReplicatedEntity(entity) != 0);

    constexpr size_t kPeers = 10;
    static_assert(kPeers > NetworkManager::kMaxFullSyncsPerUpdate);
    std::vector<std::unique_ptr<RawPeer>> peers;
    for (size_t i = 0; i < kPeers; ++i)
    {
        peers.push_back(std::make_unique<RawPeer>(nm.GetBoundPort()));
        ASSERT_TRUE(peers.back()->IsReady());
        ASSERT_TRUE(peers.back()->BeginHandshake());
    }
    // Every Connect lands and is answered in one Update; then every ClientFinished lands
    // before the next, so that one Update admits the whole burst.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    nm.Update(0.016f);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (const auto& peer : peers)
        ASSERT_TRUE(peer->FinishHandshake());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(nm.GetStats().fullEntitySyncs, 0u);
    nm.Update(0.016f);

    ASSERT_EQ(nm.GetClients().size(), kPeers);
    EXPECT_EQ(nm.GetStats().fullEntitySyncs, static_cast<uint32_t>(NetworkManager::kMaxFullSyncsPerUpdate));

    // The deferred admissions are synced on later Updates; nobody is skipped.
    EXPECT_TRUE(PumpUntil(nm, [&] { return nm.GetStats().fullEntitySyncs >= kPeers; }));
    EXPECT_EQ(nm.GetStats().fullEntitySyncs, static_cast<uint32_t>(kPeers));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (const auto& peer : peers)
    {
        bool spawned = false;
        for (const auto& datagram : peer->Drain())
            spawned = spawned || WireType(datagram) == static_cast<uint16_t>(MessageType::EntitySpawn);
        EXPECT_TRUE(spawned);
    }

    peers.clear();
    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// Finding 39: reliable sequence 0 skips dedup, ACK and ordering. No sender
// emits it except at the uint32 wrap, where the stream then went 0xFFFFFFFF
// -> 0 (an untracked, endlessly retransmitted message) and the receiver's
// ordered expectation went to 0, wedging every later ReliableOrdered message.
// ============================================================================

TEST(NetTransportSec_ReliableSequenceHelpersSkipZeroAcrossWrap)
{
    static_assert(NextReliableSequence(1u) == 2u);
    static_assert(NextReliableSequence(0xFFFFFFFFu) == 1u);
    static_assert(IsSequenceNewer(1u, 0xFFFFFFFFu));
    static_assert(!IsSequenceNewer(0xFFFFFFFFu, 1u));
    static_assert(!IsSequenceNewer(5u, 5u));

    SequenceNumber next = 0xFFFFFFFEu;
    EXPECT_EQ(TakeReliableSequence(next), 0xFFFFFFFEu);
    EXPECT_EQ(TakeReliableSequence(next), 0xFFFFFFFFu);
    EXPECT_EQ(TakeReliableSequence(next), 1u);
    EXPECT_EQ(next, 2u);
}

TEST(NetTransportSec_ClientReliableStreamWrapsToOneNotZero)
{
    auto& nm = FreshManager();
    RawPeer fakeServer(0);
    ASSERT_TRUE(fakeServer.IsReady());
    sockaddr_in clientEndpoint{};
    ASSERT_TRUE(ConnectClientToFakeServer(nm, fakeServer, clientEndpoint));
    (void)fakeServer.DrainFrom();

    NetworkManagerTransportSecurityTestAccess::SeedClientOutgoingSequence(nm, 0xFFFFFFFFu);
    NetworkMessage message;
    message.type = MessageType::UserDefined;
    message.channel = ChannelType::Reliable;
    message.payload = {0x01};
    nm.SendMessage(message);
    nm.SendMessage(message);

    std::vector<uint32_t> sequences;
    EXPECT_TRUE(PumpUntil(nm,
                          [&]
                          {
                              for (const auto& datagram : fakeServer.DrainFrom())
                              {
                                  if (WireType(datagram.bytes) == static_cast<uint16_t>(MessageType::UserDefined))
                                      sequences.push_back(WireSequence(datagram.bytes));
                              }
                              return sequences.size() >= 2;
                          }));
    ASSERT_TRUE(sequences.size() >= 2);
    EXPECT_EQ(sequences[0], 0xFFFFFFFFu);
    EXPECT_EQ(sequences[1], 1u);

    nm.Disconnect();
    fakeServer.Close();
    nm.Shutdown();
}

TEST(NetTransportSec_OrderedDeliveryContinuesAcrossSequenceWrap)
{
    auto& nm = FreshManager();
    ASSERT_TRUE(nm.StartServer(0, 2, NetworkEndpointPolicy::Loopback()));
    std::vector<uint8_t> delivered;
    nm.RegisterHandler(MessageType::UserDefined,
                       [&](const NetworkMessage& message)
                       {
                           if (!message.payload.empty())
                               delivered.push_back(message.payload.front());
                       });
    {
        RawPeer peer(nm.GetBoundPort());
        ASSERT_TRUE(peer.IsReady());
        // The handshake uses reliable sequence 0 (untracked), so it cannot collide with the
        // dedup window below.
        const ClientID admitted = AdmitPeer(nm, peer);
        ASSERT_TRUE(admitted != INVALID_CLIENT);
        NetworkManagerTransportSecurityTestAccess::SeedExpectedOrderedSequence(nm, admitted, 0xFFFFFFFFu);

        ASSERT_TRUE(peer.Send(BuildWire(MessageType::UserDefined, ChannelType::ReliableOrdered, 0xFFFFFFFFu, {0xA1})));
        ASSERT_TRUE(peer.Send(BuildWire(MessageType::UserDefined, ChannelType::ReliableOrdered, 1u, {0xA2})));
        EXPECT_TRUE(PumpUntil(nm, [&] { return delivered.size() >= 2; }));
        ASSERT_EQ(delivered.size(), static_cast<size_t>(2));
        EXPECT_EQ(delivered[0], static_cast<uint8_t>(0xA1));
        EXPECT_EQ(delivered[1], static_cast<uint8_t>(0xA2));
    }
    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// Finding 35 (engine side): a server EntityStateUpdate for an unknown network
// ID created a client placeholder with no cap, and non-finite or truncated
// transforms were written straight into the entity.
// ============================================================================

namespace
{
    NetBuffer EntityStateBuffer(uint32_t networkID, float positionX)
    {
        NetBuffer out;
        out.WriteUint32(networkID);
        out.WriteVector3(DirectX::XMFLOAT3{positionX, 0.0f, 0.0f});
        out.WriteVector3(DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f});
        out.WriteVector3(DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f});
        out.WriteUint16(0);
        NetBuffer in;
        in.WriteBytes(out.GetData().data(), out.GetData().size());
        return in;
    }
} // namespace

TEST(NetTransportSec_ClientPlaceholderEntitiesAreCappedAndFinite)
{
    auto& nm = FreshManager();
    constexpr uint32_t kBase = 0x100000u;
    constexpr uint32_t kCap = static_cast<uint32_t>(NetworkManager::kMaxReplicatedEntities);
    constexpr uint32_t kSent = kCap + 64;
    for (uint32_t i = 0; i < kSent; ++i)
    {
        NetBuffer state = EntityStateBuffer(kBase + i, 1.0f);
        nm.DeserializeEntityState(state);
    }
    EXPECT_TRUE(nm.GetReplicatedEntitySnapshot(kBase).has_value());
    EXPECT_TRUE(nm.GetReplicatedEntitySnapshot(kBase + kCap - 1).has_value());
    EXPECT_FALSE(nm.GetReplicatedEntitySnapshot(kBase + kCap).has_value());
    EXPECT_FALSE(nm.GetReplicatedEntitySnapshot(kBase + kSent - 1).has_value());

    // Known entities still update at the cap.
    NetBuffer moved = EntityStateBuffer(kBase, 5.0f);
    nm.DeserializeEntityState(moved);
    const auto afterMove = nm.GetReplicatedEntitySnapshot(kBase);
    ASSERT_TRUE(afterMove.has_value());
    EXPECT_NEAR(afterMove->position.x, 5.0f, 1e-6f);

    // A non-finite transform never reaches an existing entity.
    NetBuffer poisoned = EntityStateBuffer(kBase, std::numeric_limits<float>::quiet_NaN());
    nm.DeserializeEntityState(poisoned);
    const auto afterPoison = nm.GetReplicatedEntitySnapshot(kBase);
    ASSERT_TRUE(afterPoison.has_value());
    EXPECT_NEAR(afterPoison->position.x, 5.0f, 1e-6f);

    nm.Shutdown();

    // Below the cap, a non-finite transform for an unknown ID creates nothing.
    auto& fresh = FreshManager();
    NetBuffer infinite = EntityStateBuffer(kBase, std::numeric_limits<float>::infinity());
    fresh.DeserializeEntityState(infinite);
    EXPECT_FALSE(fresh.GetReplicatedEntitySnapshot(kBase).has_value());
    fresh.Shutdown();
}

#endif // ENABLE_NETWORKING
