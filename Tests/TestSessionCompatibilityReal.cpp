/**
 * @file TestSessionCompatibilityReal.cpp
 * @brief NET-100: protocol-version negotiation in Connect/ConnectAccepted over real loopback UDP.
 *
 * Drives the production NetworkManager in both roles against a raw UDP peer that speaks the
 * documented wire format (docs/specs/networking-wire-format.md). The server must reject a
 * missing, older, newer, or malformed handshake with a typed ConnectRejected before any client
 * slot exists; the client must refuse a ConnectAccepted that echoes a different version.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkManager.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef SPARK_PLATFORM_WINDOWS
#include <fcntl.h>
#endif

using namespace Spark::Net;

#ifdef ENABLE_NETWORKING

namespace
{
    constexpr uint32_t WIRE_PACKET_MAGIC = 0x5350524B; // "SPRK" message header magic
    constexpr size_t WIRE_HEADER_SIZE = 23;

    struct WireMessage
    {
        MessageType type = MessageType::UserDefined;
        std::vector<uint8_t> payload;
    };

    struct DecodedRejection
    {
        std::string text;
        ConnectRejectReason reason = ConnectRejectReason::Unspecified;
        uint16_t serverVersion = 0;
    };

    std::vector<uint8_t> BuildWire(MessageType type, const std::vector<uint8_t>& payload, uint32_t sequence = 0)
    {
        NetBuffer buf;
        buf.WriteUint32(WIRE_PACKET_MAGIC);
        buf.WriteUint16(static_cast<uint16_t>(type));
        buf.WriteUint8(static_cast<uint8_t>(ChannelType::Reliable));
        buf.WriteUint32(INVALID_CLIENT);
        buf.WriteUint32(sequence);
        buf.WriteFloat(0.0f);
        buf.WriteUint32(static_cast<uint32_t>(payload.size()));
        if (!payload.empty())
            buf.WriteBytes(payload.data(), payload.size());
        return buf.GetData();
    }

    std::optional<WireMessage> ParseWire(const std::vector<uint8_t>& datagram)
    {
        NetBuffer buf;
        buf.WriteBytes(datagram.data(), datagram.size());
        if (buf.ReadUint32() != WIRE_PACKET_MAGIC)
            return std::nullopt;
        WireMessage message;
        message.type = static_cast<MessageType>(buf.ReadUint16());
        buf.ReadUint8();  // channel
        buf.ReadUint32(); // sender
        buf.ReadUint32(); // sequence
        buf.ReadFloat();  // timestamp
        const uint32_t payloadSize = buf.ReadUint32();
        if (buf.HasError() || datagram.size() != WIRE_HEADER_SIZE + payloadSize)
            return std::nullopt;
        message.payload.assign(datagram.begin() + WIRE_HEADER_SIZE, datagram.end());
        return message;
    }

    DecodedRejection DecodeRejection(const std::vector<uint8_t>& payload)
    {
        NetBuffer buf;
        buf.WriteBytes(payload.data(), payload.size());
        DecodedRejection rejection;
        rejection.text = buf.ReadString();
        rejection.reason = static_cast<ConnectRejectReason>(buf.ReadUint8());
        rejection.serverVersion = buf.ReadUint16();
        EXPECT_FALSE(buf.HasError());
        EXPECT_EQ(buf.RemainingBytes(), static_cast<size_t>(0));
        return rejection;
    }

    /// Raw loopback UDP endpoint standing in for a peer built from another protocol version.
    class RawPeer
    {
      public:
        RawPeer()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            // Hold a Winsock reference of our own: the NetworkManager singleton may not be
            // initialized yet (client-role tests build the fake server first) and its Shutdown()
            // must not pull Winsock out from under this socket. WSAStartup is reference-counted.
            WSADATA winsockData{};
            m_winsockStarted = WSAStartup(MAKEWORD(2, 2), &winsockData) == 0;
            if (!m_winsockStarted)
                return;
#endif
            m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (m_socket == INVALID_SOCKET)
                return;
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            local.sin_port = 0;
            m_ready = bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;
#ifdef SPARK_PLATFORM_WINDOWS
            u_long nonBlocking = 1;
            m_ready = m_ready && ioctlsocket(m_socket, FIONBIO, &nonBlocking) == 0;
#else
            const int flags = fcntl(m_socket, F_GETFL, 0);
            m_ready = m_ready && flags >= 0 && fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
        }

        ~RawPeer()
        {
            if (m_socket != INVALID_SOCKET)
                closesocket(m_socket);
#ifdef SPARK_PLATFORM_WINDOWS
            if (m_winsockStarted)
                WSACleanup();
#endif
        }

        RawPeer(const RawPeer&) = delete;
        RawPeer& operator=(const RawPeer&) = delete;

        bool IsReady() const { return m_ready; }

        uint16_t Port() const
        {
            sockaddr_in local{};
#ifdef SPARK_PLATFORM_WINDOWS
            int length = sizeof(local);
#else
            socklen_t length = sizeof(local);
#endif
            if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&local), &length) != 0)
                return 0;
            return ntohs(local.sin_port);
        }

        void SendTo(uint16_t port, const std::vector<uint8_t>& datagram) const
        {
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            target.sin_port = htons(port);
            SendTo(target, datagram);
        }

        void SendTo(const sockaddr_in& target, const std::vector<uint8_t>& datagram) const
        {
            const int sent =
                sendto(m_socket, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                       reinterpret_cast<const sockaddr*>(&target), sizeof(target));
            EXPECT_EQ(sent, static_cast<int>(datagram.size()));
        }

        /// Pump @p manager until a message of @p type arrives or the deadline passes.
        std::optional<WireMessage> AwaitType(NetworkManager& manager, MessageType type,
                                             std::chrono::milliseconds window = std::chrono::milliseconds(400))
        {
            const auto deadline = std::chrono::steady_clock::now() + window;
            while (std::chrono::steady_clock::now() < deadline)
            {
                manager.Update(0.016f);
                std::array<uint8_t, 2048> buffer{};
                sockaddr_in from{};
#ifdef SPARK_PLATFORM_WINDOWS
                int fromLength = sizeof(from);
#else
                socklen_t fromLength = sizeof(from);
#endif
                const int received =
                    recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLength);
                if (received > 0)
                {
                    m_lastSender = from;
                    auto message = ParseWire(std::vector<uint8_t>(buffer.begin(), buffer.begin() + received));
                    if (message && message->type == type)
                        return message;
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return std::nullopt;
        }

        const sockaddr_in& LastSender() const { return m_lastSender; }

      private:
        SOCKET m_socket = INVALID_SOCKET;
        bool m_ready = false;
        sockaddr_in m_lastSender{};
#ifdef SPARK_PLATFORM_WINDOWS
        bool m_winsockStarted = false;
#endif
    };

    std::vector<uint8_t> ConnectPayload(const std::string& name, uint16_t version)
    {
        NetBuffer buf;
        WriteConnectRequest(buf, name, version);
        return buf.GetData();
    }

    /// Start the singleton as a loopback server with a clean slate.
    bool StartLoopbackServer(NetworkManager& manager, int maxClients)
    {
        manager.Shutdown();
        return manager.Initialize() && manager.StartServer(0, maxClients, NetworkEndpointPolicy::Loopback());
    }

    /// Send a Connect carrying @p payload and return the typed rejection the server answers with.
    std::optional<DecodedRejection> ExpectRejected(NetworkManager& server, const std::vector<uint8_t>& payload)
    {
        RawPeer peer;
        EXPECT_TRUE(peer.IsReady());
        peer.SendTo(server.GetBoundPort(), BuildWire(MessageType::Connect, payload));
        auto rejected = peer.AwaitType(server, MessageType::ConnectRejected);
        EXPECT_TRUE(rejected.has_value());
        if (!rejected)
            return std::nullopt;
        return DecodeRejection(rejected->payload);
    }

    /// Start the singleton as a client of @p peer and return the Connect payload it sent.
    std::optional<WireMessage> StartClientAgainst(NetworkManager& client, RawPeer& peer)
    {
        client.Shutdown();
        EXPECT_TRUE(client.Initialize());
        EXPECT_TRUE(client.Connect("127.0.0.1", peer.Port(), "Negotiator"));
        return peer.AwaitType(client, MessageType::Connect);
    }

    std::vector<uint8_t> AcceptPayload(ClientID id, uint16_t echoedVersion)
    {
        NetBuffer buf;
        buf.WriteUint32(id);
        buf.WriteFloat(0.0f);
        buf.WriteUint16(echoedVersion);
        return buf.GetData();
    }

    void PumpUntilSettled(NetworkManager& manager, ConnectionState target)
    {
        for (int i = 0; i < 50 && manager.GetConnectionState() != target; ++i)
        {
            manager.Update(0.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
} // namespace

TEST(SessionCompatibility_SameVersionAccepted)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 4));

    RawPeer peer;
    ASSERT_TRUE(peer.IsReady());
    peer.SendTo(server.GetBoundPort(),
                BuildWire(MessageType::Connect, ConnectPayload("Current", NETWORK_PROTOCOL_VERSION)));
    auto accepted = peer.AwaitType(server, MessageType::ConnectAccepted);
    ASSERT_TRUE(accepted.has_value());

    NetBuffer buf;
    buf.WriteBytes(accepted->payload.data(), accepted->payload.size());
    const ClientID assigned = buf.ReadUint32();
    buf.ReadFloat();
    const uint16_t echoed = buf.ReadUint16();
    ASSERT_FALSE(buf.HasError());
    EXPECT_EQ(buf.RemainingBytes(), static_cast<size_t>(0));
    EXPECT_NE(assigned, INVALID_CLIENT);
    EXPECT_EQ(echoed, NETWORK_PROTOCOL_VERSION);

    const auto clients = server.GetClients();
    ASSERT_EQ(clients.size(), static_cast<size_t>(1));
    EXPECT_EQ(clients.begin()->second.name, std::string("Current"));

    server.Shutdown();
}

TEST(SessionCompatibility_OlderClientRejected)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 4));

    const auto rejection =
        ExpectRejected(server, ConnectPayload("Old", static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION - 1)));
    ASSERT_TRUE(rejection.has_value());
    EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::ProtocolTooOld));
    EXPECT_EQ(rejection->serverVersion, NETWORK_PROTOCOL_VERSION);
    EXPECT_TRUE(rejection->text.find("older") != std::string::npos);
    EXPECT_TRUE(server.GetClients().empty());

    server.Shutdown();
}

TEST(SessionCompatibility_NewerClientRejected)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 4));

    const auto rejection =
        ExpectRejected(server, ConnectPayload("New", static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION + 1)));
    ASSERT_TRUE(rejection.has_value());
    EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::ProtocolTooNew));
    EXPECT_EQ(rejection->serverVersion, NETWORK_PROTOCOL_VERSION);
    EXPECT_TRUE(server.GetClients().empty());

    server.Shutdown();
}

TEST(SessionCompatibility_MissingVersionRejected)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 4));

    // A pre-negotiation client sent only its length-prefixed name.
    NetBuffer legacy;
    legacy.WriteString("LegacyPlayer");
    const auto rejection = ExpectRejected(server, legacy.GetData());
    ASSERT_TRUE(rejection.has_value());
    EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::ProtocolMissing));
    EXPECT_TRUE(server.GetClients().empty());

    server.Shutdown();
}

TEST(SessionCompatibility_TruncatedHandshakeRejected)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 4));

    // Magic and version with no name field: below the Connect schema minimum, dropped before
    // HandleConnect, so no slot, no pending address, and no reply.
    {
        NetBuffer truncated;
        truncated.WriteUint32(NETWORK_HANDSHAKE_MAGIC);
        truncated.WriteUint16(NETWORK_PROTOCOL_VERSION);
        RawPeer peer;
        ASSERT_TRUE(peer.IsReady());
        peer.SendTo(server.GetBoundPort(), BuildWire(MessageType::Connect, truncated.GetData()));
        EXPECT_FALSE(peer.AwaitType(server, MessageType::ConnectAccepted, std::chrono::milliseconds(150)).has_value());
        EXPECT_TRUE(server.GetClients().empty());
    }

    // Name length prefix claims more bytes than the datagram carries.
    {
        NetBuffer overrun;
        overrun.WriteUint32(NETWORK_HANDSHAKE_MAGIC);
        overrun.WriteUint16(NETWORK_PROTOCOL_VERSION);
        overrun.WriteUint16(64);
        overrun.WriteUint8('A');
        const auto rejection = ExpectRejected(server, overrun.GetData());
        ASSERT_TRUE(rejection.has_value());
        EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::MalformedHandshake));
    }

    // Trailing bytes after the name are not silently ignored.
    {
        auto trailing = ConnectPayload("Trailing", NETWORK_PROTOCOL_VERSION);
        trailing.push_back(0xA5);
        const auto rejection = ExpectRejected(server, trailing);
        ASSERT_TRUE(rejection.has_value());
        EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::MalformedHandshake));
    }
    EXPECT_TRUE(server.GetClients().empty());

    // The rejections left no residue: a well-formed handshake still gets the first free slot.
    RawPeer good;
    ASSERT_TRUE(good.IsReady());
    good.SendTo(server.GetBoundPort(),
                BuildWire(MessageType::Connect, ConnectPayload("Good", NETWORK_PROTOCOL_VERSION)));
    EXPECT_TRUE(good.AwaitType(server, MessageType::ConnectAccepted).has_value());
    EXPECT_EQ(server.GetClients().size(), static_cast<size_t>(1));

    server.Shutdown();
}

TEST(SessionCompatibility_ServerFullRejectionIsTyped)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 1));

    RawPeer first;
    ASSERT_TRUE(first.IsReady());
    first.SendTo(server.GetBoundPort(),
                 BuildWire(MessageType::Connect, ConnectPayload("First", NETWORK_PROTOCOL_VERSION)));
    ASSERT_TRUE(first.AwaitType(server, MessageType::ConnectAccepted).has_value());

    const auto rejection = ExpectRejected(server, ConnectPayload("Second", NETWORK_PROTOCOL_VERSION));
    ASSERT_TRUE(rejection.has_value());
    EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::ServerFull));
    EXPECT_EQ(server.GetClients().size(), static_cast<size_t>(1));

    server.Shutdown();
}

TEST(SessionCompatibility_ClientSendsVersionedHandshake)
{
    auto& client = NetworkManager::GetInstance();
    RawPeer fakeServer;
    ASSERT_TRUE(fakeServer.IsReady());
    const auto connect = StartClientAgainst(client, fakeServer);
    ASSERT_TRUE(connect.has_value());

    NetBuffer buf;
    buf.WriteBytes(connect->payload.data(), connect->payload.size());
    EXPECT_EQ(buf.ReadUint32(), NETWORK_HANDSHAKE_MAGIC);
    EXPECT_EQ(buf.ReadUint16(), NETWORK_PROTOCOL_VERSION);
    EXPECT_EQ(buf.ReadString(), std::string("Negotiator"));
    EXPECT_FALSE(buf.HasError());

    // A matching echo completes the handshake.
    fakeServer.SendTo(fakeServer.LastSender(),
                      BuildWire(MessageType::ConnectAccepted, AcceptPayload(7, NETWORK_PROTOCOL_VERSION), 1));
    PumpUntilSettled(client, ConnectionState::Connected);
    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
    EXPECT_EQ(client.GetLocalClientID(), static_cast<ClientID>(7));

    client.Shutdown();
}

TEST(SessionCompatibility_ClientRefusesMismatchedAcceptEcho)
{
    auto& client = NetworkManager::GetInstance();
    RawPeer fakeServer;
    ASSERT_TRUE(fakeServer.IsReady());
    ASSERT_TRUE(StartClientAgainst(client, fakeServer).has_value());

    fakeServer.SendTo(fakeServer.LastSender(),
                      BuildWire(MessageType::ConnectAccepted,
                                AcceptPayload(7, static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION + 1)), 1));
    PumpUntilSettled(client, ConnectionState::Disconnected);
    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(client.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_EQ(client.GetLocalClientID(), INVALID_CLIENT);
    EXPECT_EQ(static_cast<int>(client.GetLastConnectRejectReason()),
              static_cast<int>(ConnectRejectReason::ProtocolMismatch));
    EXPECT_FALSE(client.GetLastConnectionError().empty());

    client.Shutdown();
}

TEST(SessionCompatibility_ClientRecordsTypedRejection)
{
    auto& client = NetworkManager::GetInstance();
    RawPeer fakeServer;
    ASSERT_TRUE(fakeServer.IsReady());
    ASSERT_TRUE(StartClientAgainst(client, fakeServer).has_value());

    NetBuffer reject;
    reject.WriteString("Client protocol version 1 is older than server version 2");
    reject.WriteUint8(static_cast<uint8_t>(ConnectRejectReason::ProtocolTooOld));
    reject.WriteUint16(static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION + 1));
    fakeServer.SendTo(fakeServer.LastSender(), BuildWire(MessageType::ConnectRejected, reject.GetData(), 1));
    PumpUntilSettled(client, ConnectionState::Disconnected);
    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(client.GetLastConnectRejectReason()),
              static_cast<int>(ConnectRejectReason::ProtocolTooOld));
    EXPECT_EQ(client.GetLastConnectionError(), std::string("Client protocol version 1 is older than server version 2"));

    client.Shutdown();
}

TEST(SessionCompatibility_ClientDropsLegacyAccept)
{
    // A pre-negotiation server admits a versioned client and answers with the old 8-byte
    // ConnectAccepted (no version echo). The client must never treat that as a session.
    auto& client = NetworkManager::GetInstance();
    RawPeer fakeServer;
    ASSERT_TRUE(fakeServer.IsReady());
    ASSERT_TRUE(StartClientAgainst(client, fakeServer).has_value());

    NetBuffer legacyAccept;
    legacyAccept.WriteUint32(7);
    legacyAccept.WriteFloat(0.0f);
    fakeServer.SendTo(fakeServer.LastSender(), BuildWire(MessageType::ConnectAccepted, legacyAccept.GetData(), 1));
    PumpUntilSettled(client, ConnectionState::Connected);
    EXPECT_NE(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
    EXPECT_EQ(client.GetLocalClientID(), INVALID_CLIENT);

    client.Shutdown();
}

#endif // ENABLE_NETWORKING
