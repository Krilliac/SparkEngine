/**
 * @file TestSessionCompatibilityReal.cpp
 * @brief NET-100: protocol-version negotiation in the v2 handshake over real loopback UDP.
 *
 * Drives the production NetworkManager in both roles against a raw UDP peer that speaks the
 * documented wire format (docs/specs/networking-wire-format.md): framed datagrams, a ClientHello
 * in Connect and a signed ServerHello in ConnectAccepted. The server must reject a missing,
 * older, newer, or malformed handshake with a typed ConnectRejected before any client slot
 * exists, and must answer an unframed pre-v2 Connect in a form that client can still parse; the
 * client must refuse a ConnectAccepted that echoes a different version.
 */

#include "TestFramework.h"
#include "Fixtures/SecureTestPeer.h"
#include "Engine/Networking/NetworkManager.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace Spark::Net;

#ifdef ENABLE_NETWORKING

using namespace SparkTestFixtures;

namespace
{
    struct DecodedRejection
    {
        std::string text;
        ConnectRejectReason reason = ConnectRejectReason::Unspecified;
        uint16_t serverVersion = 0;
    };

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

    /// A ClientHello advertising @p version, produced by the shipped ClientHandshake.
    std::vector<uint8_t> HelloWithVersion(uint16_t version)
    {
        ClientHandshake handshake;
        auto hello = handshake.Begin(version);
        EXPECT_TRUE(hello.has_value());
        return hello ? std::vector<uint8_t>(hello->begin(), hello->end()) : std::vector<uint8_t>{};
    }

    /// Start the singleton as a loopback server with a clean slate.
    bool StartLoopbackServer(NetworkManager& manager, int maxClients)
    {
        manager.Shutdown();
        return manager.Initialize() && manager.StartServer(0, maxClients, NetworkEndpointPolicy::Loopback());
    }

    /// Send a framed Connect carrying @p payload and return the typed rejection the server answers with.
    std::optional<DecodedRejection> ExpectRejected(NetworkManager& server, const std::vector<uint8_t>& payload)
    {
        SecureRawClient peer;
        EXPECT_TRUE(peer.Socket().IsReady());
        peer.Socket().SendTo(server.GetBoundPort(), HandshakeFrame(BuildWire(MessageType::Connect, payload)));
        auto rejected = peer.AwaitType(server, MessageType::ConnectRejected);
        EXPECT_TRUE(rejected.has_value());
        if (!rejected)
            return std::nullopt;
        return DecodeRejection(rejected->payload);
    }

    /// Start the singleton as a client of @p peer and return the ClientHello it sent.
    std::optional<std::vector<uint8_t>> StartClientAgainst(NetworkManager& client, SecureRawServer& peer)
    {
        client.Shutdown();
        EXPECT_TRUE(client.Initialize());
        EXPECT_TRUE(client.Connect("127.0.0.1", peer.Port(), "Negotiator"));
        return peer.AwaitConnect(client);
    }

    void PumpUntilSettled(NetworkManager& manager, ConnectionState target)
    {
        PumpUntil(manager, [&] { return manager.GetConnectionState() == target; }, std::chrono::milliseconds(150));
    }
} // namespace

TEST(SessionCompatibility_SameVersionAccepted)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 4));

    SecureRawClient peer;
    ASSERT_TRUE(peer.Socket().IsReady());
    peer.Socket().SendTo(server.GetBoundPort(), peer.BeginConnect());
    auto accepted = peer.AwaitType(server, MessageType::ConnectAccepted);
    ASSERT_TRUE(accepted.has_value());

    NetBuffer buf;
    buf.WriteBytes(accepted->payload.data(), accepted->payload.size());
    const ClientID assigned = buf.ReadUint32();
    buf.ReadFloat();
    const uint16_t echoed = buf.ReadUint16();
    ASSERT_FALSE(buf.HasError());
    EXPECT_EQ(buf.RemainingBytes(), SERVER_HELLO_SIZE);
    EXPECT_NE(assigned, INVALID_CLIENT);
    EXPECT_EQ(echoed, NETWORK_PROTOCOL_VERSION);

    // The slot exists but is not admitted until the sealed ClientFinished arrives.
    auto clients = server.GetClientSlots();
    ASSERT_EQ(clients.size(), static_cast<size_t>(1));
    EXPECT_EQ(static_cast<int>(clients.begin()->second.state), static_cast<int>(ConnectionState::Securing));
    EXPECT_TRUE(server.GetClients().empty()); // not a player until admitted

    ASSERT_TRUE(peer.FinishFromAccepted(*accepted, SparkTestFixtures::TestServerIdentity().publicKey));
    ASSERT_TRUE(peer.SendSealed(
        BuildWire(MessageType::ClientFinished, EncodeName("Current"), ChannelType::Reliable, 0, assigned),
        server.GetBoundPort()));
    ASSERT_TRUE(PumpUntil(server,
                          [&]
                          {
                              const auto current = server.GetClients();
                              return current.size() == 1 && current.begin()->second.state == ConnectionState::Connected;
                          }));
    EXPECT_EQ(server.GetClients().begin()->second.name, std::string("Current"));

    server.Shutdown();
}

TEST(SessionCompatibility_OlderClientRejected)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 4));

    const auto rejection =
        ExpectRejected(server, HelloWithVersion(static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION - 1)));
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
        ExpectRejected(server, HelloWithVersion(static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION + 1)));
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

    // Magic and version only: below the Connect schema minimum, dropped before HandleConnect,
    // so no slot, no pending address, and no reply.
    {
        NetBuffer truncated;
        truncated.WriteUint32(NETWORK_HANDSHAKE_MAGIC);
        truncated.WriteUint16(NETWORK_PROTOCOL_VERSION);
        SecureRawClient peer;
        ASSERT_TRUE(peer.Socket().IsReady());
        peer.Socket().SendTo(server.GetBoundPort(),
                             HandshakeFrame(BuildWire(MessageType::Connect, truncated.GetData())));
        EXPECT_FALSE(peer.AwaitType(server, MessageType::ConnectAccepted, std::chrono::milliseconds(150)).has_value());
        EXPECT_TRUE(server.GetClients().empty());
    }

    // A ClientHello missing its last byte.
    {
        auto shortHello = HelloWithVersion(NETWORK_PROTOCOL_VERSION);
        shortHello.pop_back();
        const auto rejection = ExpectRejected(server, shortHello);
        ASSERT_TRUE(rejection.has_value());
        EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::MalformedHandshake));
    }

    // Trailing bytes after the ClientHello are not silently ignored.
    {
        auto trailing = HelloWithVersion(NETWORK_PROTOCOL_VERSION);
        trailing.push_back(0xA5);
        const auto rejection = ExpectRejected(server, trailing);
        ASSERT_TRUE(rejection.has_value());
        EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::MalformedHandshake));
    }
    EXPECT_TRUE(server.GetClients().empty());

    // The rejections left no residue: a well-formed handshake still gets the first free slot.
    SecureRawClient good;
    ASSERT_TRUE(good.Connect(server, "Good"));
    EXPECT_EQ(server.GetClients().size(), static_cast<size_t>(1));

    server.Shutdown();
}

TEST(SessionCompatibility_ServerFullRejectionIsTyped)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 1));

    SecureRawClient first;
    ASSERT_TRUE(first.Connect(server, "First"));

    const auto rejection = ExpectRejected(server, HelloWithVersion(NETWORK_PROTOCOL_VERSION));
    ASSERT_TRUE(rejection.has_value());
    EXPECT_EQ(static_cast<int>(rejection->reason), static_cast<int>(ConnectRejectReason::ServerFull));
    EXPECT_EQ(server.GetClients().size(), static_cast<size_t>(1));

    server.Shutdown();
}

TEST(SessionCompatibility_ClientSendsVersionedHandshake)
{
    auto& client = NetworkManager::GetInstance();
    SecureRawServer fakeServer;
    ASSERT_TRUE(fakeServer.Socket().IsReady());
    const auto hello = StartClientAgainst(client, fakeServer);
    ASSERT_TRUE(hello.has_value());

    // The Connect payload is exactly a ClientHello: magic, version, suite, key, nonce -- no name.
    ASSERT_EQ(hello->size(), CLIENT_HELLO_SIZE);
    NetBuffer buf;
    buf.WriteBytes(hello->data(), hello->size());
    EXPECT_EQ(buf.ReadUint32(), NETWORK_HANDSHAKE_MAGIC);
    EXPECT_EQ(buf.ReadUint16(), NETWORK_PROTOCOL_VERSION);
    EXPECT_EQ(buf.ReadUint8(), HANDSHAKE_SUITE_X25519_ED25519_CHACHAPOLY_HKDF_SHA256);

    // A signed, matching echo completes the handshake; the name follows sealed.
    ASSERT_FALSE(fakeServer.Accept(7).empty());
    PumpUntilSettled(client, ConnectionState::Connected);
    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
    EXPECT_EQ(client.GetLocalClientID(), static_cast<ClientID>(7));
    const auto finished = fakeServer.AwaitType(client, MessageType::ClientFinished);
    ASSERT_TRUE(finished.has_value());
    EXPECT_TRUE(finished->payload == EncodeName("Negotiator"));

    client.Shutdown();
}

TEST(SessionCompatibility_ClientRefusesMismatchedAcceptEcho)
{
    auto& client = NetworkManager::GetInstance();
    SecureRawServer fakeServer;
    ASSERT_TRUE(fakeServer.Socket().IsReady());
    ASSERT_TRUE(StartClientAgainst(client, fakeServer).has_value());

    ASSERT_FALSE(fakeServer.Accept(7, nullptr, static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION + 1)).empty());
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
    SecureRawServer fakeServer;
    ASSERT_TRUE(fakeServer.Socket().IsReady());
    ASSERT_TRUE(StartClientAgainst(client, fakeServer).has_value());

    NetBuffer reject;
    reject.WriteString("Client protocol version 2 is older than server version 3");
    reject.WriteUint8(static_cast<uint8_t>(ConnectRejectReason::ProtocolTooOld));
    reject.WriteUint16(static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION + 1));
    fakeServer.Socket().SendTo(fakeServer.Socket().LastSender(),
                               HandshakeFrame(BuildWire(MessageType::ConnectRejected, reject.GetData())));
    PumpUntilSettled(client, ConnectionState::Disconnected);
    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(client.GetLastConnectRejectReason()),
              static_cast<int>(ConnectRejectReason::ProtocolTooOld));
    EXPECT_EQ(client.GetLastConnectionError(), std::string("Client protocol version 2 is older than server version 3"));

    client.Shutdown();
}

TEST(SessionCompatibility_ClientDropsLegacyAccept)
{
    // A pre-v2 server answers with an unframed ConnectAccepted, or with the old 10-byte echo
    // and no ServerHello. Neither can authenticate a server, so neither becomes a session.
    auto& client = NetworkManager::GetInstance();
    SecureRawServer fakeServer;
    ASSERT_TRUE(fakeServer.Socket().IsReady());
    ASSERT_TRUE(StartClientAgainst(client, fakeServer).has_value());

    NetBuffer legacyAccept;
    legacyAccept.WriteUint32(7);
    legacyAccept.WriteFloat(0.0f);
    legacyAccept.WriteUint16(NETWORK_PROTOCOL_VERSION);
    const auto wire = BuildWire(MessageType::ConnectAccepted, legacyAccept.GetData(), ChannelType::Reliable, 1);
    fakeServer.Socket().SendTo(fakeServer.Socket().LastSender(), wire);                 // unframed
    fakeServer.Socket().SendTo(fakeServer.Socket().LastSender(), HandshakeFrame(wire)); // framed, no hello
    PumpUntilSettled(client, ConnectionState::Connected);
    EXPECT_NE(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
    EXPECT_EQ(client.GetLocalClientID(), INVALID_CLIENT);

    // The handshake was not consumed: the real, signed answer still completes it.
    ASSERT_FALSE(fakeServer.Accept(7).empty());
    PumpUntilSettled(client, ConnectionState::Connected);
    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Connected));

    client.Shutdown();
}

TEST(SessionCompatibility_LegacyUnframedConnectGetsTypedRejection)
{
    // A version-1 client sends an unframed "SPRK" Connect and can only parse an unframed reply.
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server, 4));

    NetBuffer v1Connect;
    v1Connect.WriteUint32(NETWORK_HANDSHAKE_MAGIC);
    v1Connect.WriteUint16(1);
    v1Connect.WriteString("OldClient");
    LoopbackSocket legacyPeer;
    ASSERT_TRUE(legacyPeer.IsReady());
    legacyPeer.SendTo(server.GetBoundPort(), BuildWire(MessageType::Connect, v1Connect.GetData()));

    std::optional<WireMessage> reply;
    PumpUntil(server,
              [&]
              {
                  while (auto datagram = legacyPeer.Receive())
                  {
                      reply = ParseWire(*datagram); // unframed: parses from byte 0
                  }
                  return reply.has_value();
              });
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(static_cast<int>(reply->type), static_cast<int>(MessageType::ConnectRejected));
    const auto rejection = DecodeRejection(reply->payload);
    EXPECT_EQ(static_cast<int>(rejection.reason), static_cast<int>(ConnectRejectReason::ProtocolTooOld));
    EXPECT_EQ(rejection.serverVersion, NETWORK_PROTOCOL_VERSION);
    EXPECT_TRUE(server.GetClients().empty());
    EXPECT_TRUE(server.GetStats().plaintextFramesDropped >= 1u);

    server.Shutdown();
}

#endif // ENABLE_NETWORKING
