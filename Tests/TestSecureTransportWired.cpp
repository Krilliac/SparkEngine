/**
 * @file TestSecureTransportWired.cpp
 * @brief NET-100: the v2 handshake and sealed datagrams through the production NetworkManager.
 *
 * Every case drives the NetworkManager singleton over real loopback UDP against a raw peer
 * (Tests/Fixtures/SecureTestPeer.h) that builds, seals, tampers with, replays, reorders and
 * truncates datagrams by hand. The singleton must authenticate the server before it admits a
 * session, deliver only frames its SecureChannel authenticates, and never put a player name,
 * chat text or credential-shaped payload on the wire in plaintext, and a TERRAFRONT SCRAM
 * login through it must leave no password, SaltedPassword or ClientKey in the capture.
 */

#include "TestFramework.h"
#include "Fixtures/NetworkTestSecurity.h"
#include "Fixtures/SecureTestPeer.h"
#include "Engine/Networking/NetworkManager.h"
#include "Account/TFAccountSystem.h"
#include "Account/TFCrypto.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFScramWire.h"
#include "Persistence/TFDatabase.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace Spark::Net;
using namespace SparkTestFixtures;

#ifdef ENABLE_NETWORKING

namespace
{
    bool StartLoopbackServer(NetworkManager& manager, int maxClients = 4)
    {
        manager.Shutdown();
        return manager.Initialize() && manager.StartServer(0, maxClients, NetworkEndpointPolicy::Loopback());
    }

    std::vector<uint8_t> Bytes(const std::string& text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    }

    std::vector<uint8_t> ChatWire(const std::string& text, ClientID sender)
    {
        NetBuffer buf;
        buf.WriteString(text);
        return BuildWire(MessageType::ChatMessage, buf.GetData(), ChannelType::Unreliable, 0, sender);
    }

    /// Counts ChatMessage deliveries on the singleton for the lifetime of the object.
    struct ChatCounter
    {
        explicit ChatCounter(NetworkManager& manager) : m_manager(manager)
        {
            m_manager.RegisterHandler(MessageType::ChatMessage, [this](const NetworkMessage&) { ++count; });
        }
        ~ChatCounter() { m_manager.UnregisterHandler(MessageType::ChatMessage); }
        ChatCounter(const ChatCounter&) = delete;
        ChatCounter& operator=(const ChatCounter&) = delete;

        int count = 0;

      private:
        NetworkManager& m_manager;
    };

    void Pump(NetworkManager& manager, int frames = 20)
    {
        PumpUntil(manager, [] { return false; }, std::chrono::milliseconds(frames * 3));
    }

    /// Start the singleton as a client of @p fake and wait for its ClientHello.
    bool StartClient(NetworkManager& client, SecureRawServer& fake, const std::string& name)
    {
        client.Shutdown();
        return client.Initialize() && client.Connect("127.0.0.1", fake.Port(), name) &&
               fake.AwaitConnect(client).has_value();
    }

    void AwaitDisconnected(NetworkManager& client)
    {
        PumpUntil(
            client, [&] { return client.GetConnectionState() == ConnectionState::Disconnected; },
            std::chrono::milliseconds(300));
    }
} // namespace

// ============================================================================
// Handshake in Connect
// ============================================================================

TEST(SecureTransport_Handshake_WrongPinnedKeyAbandons)
{
    auto& client = NetworkManager::GetInstance();
    NetworkSecurityConfig wrongPin = TestNetworkSecurityConfig();
    auto other = GenerateServerIdentity();
    ASSERT_TRUE(other.has_value());
    wrongPin.trust = ServerTrust::Pin(other->publicKey);
    ScopedNetworkSecurity scope(std::move(wrongPin));

    SecureRawServer impostor; // signs with the test identity, which this client does not trust
    ASSERT_TRUE(StartClient(client, impostor, "Pinned"));
    ASSERT_FALSE(impostor.Accept(9).empty());
    AwaitDisconnected(client);

    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(client.GetLastConnectRejectReason()),
              static_cast<int>(ConnectRejectReason::ServerIdentityMismatch));
    EXPECT_EQ(client.GetLocalClientID(), INVALID_CLIENT);
    // Nothing sealed ever left the client.
    for (const auto& datagram : impostor.Socket().Captured())
        EXPECT_EQ(datagram[0], kFrameHandshake);
    client.Shutdown();
}

TEST(SecureTransport_Handshake_TamperedServerHelloAbandons)
{
    auto& client = NetworkManager::GetInstance();
    SecureRawServer fake;
    ASSERT_TRUE(StartClient(client, fake, "Tamper"));

    auto response = RespondToClientHello(fake.LastClientHello(), TestServerIdentity());
    ASSERT_TRUE(response.has_value());
    auto hello = response->serverHello;
    hello[1 + 32 + 32 + 3] ^= 0x01; // one bit of the server nonce, inside the signed transcript
    const auto frame = HandshakeFrame(BuildWire(MessageType::ConnectAccepted,
                                                SecureRawServer::AcceptPayload(5, NETWORK_PROTOCOL_VERSION, hello),
                                                ChannelType::Unreliable));
    fake.Socket().SendTo(fake.Socket().LastSender(), frame);
    AwaitDisconnected(client);

    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(client.GetLastConnectRejectReason()),
              static_cast<int>(ConnectRejectReason::HandshakeAuthFailed));
    client.Shutdown();
}

TEST(SecureTransport_Handshake_TruncatedClientHelloRejectedBeforeSlot)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));

    SecureRawClient peer;
    auto frame = peer.BeginConnect();
    ASSERT_FALSE(frame.empty());
    // Drop the last ClientHello byte and fix the inner length field so only the hello is short.
    frame.pop_back();
    const uint32_t shortLength = static_cast<uint32_t>(CLIENT_HELLO_SIZE - 1);
    for (size_t i = 0; i < 4; ++i)
        frame[1 + 19 + i] = static_cast<uint8_t>(shortLength >> (8 * i));
    ASSERT_TRUE(peer.Socket().SendTo(server.GetBoundPort(), frame));
    const auto rejected = peer.AwaitType(server, MessageType::ConnectRejected);
    ASSERT_TRUE(rejected.has_value());
    EXPECT_TRUE(server.GetClients().empty());
    EXPECT_FALSE(peer.HasChannel());
    server.Shutdown();
}

TEST(SecureTransport_Handshake_DowngradedSuiteRejected)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));

    // Client -> server: a ClientHello naming suite 0 is refused before any slot or signature.
    {
        SecureRawClient peer;
        auto frame = peer.BeginConnect();
        ASSERT_FALSE(frame.empty());
        frame[1 + kWireHeaderSize + 6] = 0; // suite byte
        ASSERT_TRUE(peer.Socket().SendTo(server.GetBoundPort(), frame));
        const auto rejected = peer.AwaitType(server, MessageType::ConnectRejected);
        ASSERT_TRUE(rejected.has_value());
        NetBuffer buf;
        buf.WriteBytes(rejected->payload.data(), rejected->payload.size());
        buf.ReadString();
        EXPECT_EQ(static_cast<int>(buf.ReadUint8()), static_cast<int>(ConnectRejectReason::UnsupportedSuite));
        EXPECT_TRUE(server.GetClients().empty());
    }
    server.Shutdown();

    // Server -> client: a man in the middle rewriting the ServerHello suite breaks the handshake.
    auto& client = NetworkManager::GetInstance();
    SecureRawServer fake;
    ASSERT_TRUE(StartClient(client, fake, "Downgrade"));
    auto response = RespondToClientHello(fake.LastClientHello(), TestServerIdentity());
    ASSERT_TRUE(response.has_value());
    auto hello = response->serverHello;
    hello[0] = 0;
    fake.Socket().SendTo(fake.Socket().LastSender(),
                         HandshakeFrame(BuildWire(MessageType::ConnectAccepted,
                                                  SecureRawServer::AcceptPayload(5, NETWORK_PROTOCOL_VERSION, hello),
                                                  ChannelType::Unreliable)));
    AwaitDisconnected(client);
    EXPECT_EQ(static_cast<int>(client.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(client.GetLastConnectRejectReason()),
              static_cast<int>(ConnectRejectReason::HandshakeAuthFailed));
    client.Shutdown();
}

TEST(SecureTransport_Handshake_ClientFinishedMissingTimesOut)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    int admissions = 0;
    server.RegisterHandler(MessageType::Connect, [&](const NetworkMessage&) { ++admissions; });

    SecureRawClient peer;
    ASSERT_TRUE(peer.Socket().SendTo(server.GetBoundPort(), peer.BeginConnect()));
    const auto accepted = peer.AwaitType(server, MessageType::ConnectAccepted);
    ASSERT_TRUE(accepted.has_value());
    ASSERT_TRUE(peer.FinishFromAccepted(*accepted, TestServerIdentity().publicKey));
    // No ClientFinished: the slot stays Securing, unannounced, and receives no broadcast.
    Pump(server);
    ASSERT_EQ(server.GetClients().size(), static_cast<size_t>(1));
    EXPECT_EQ(static_cast<int>(server.GetClients().begin()->second.state), static_cast<int>(ConnectionState::Securing));
    EXPECT_EQ(admissions, 0);
    for (const auto& datagram : peer.Socket().Captured())
        EXPECT_EQ(datagram[0], kFrameHandshake);

    server.Update(11.0f); // past the 10 s connection timeout
    EXPECT_TRUE(server.GetClients().empty());
    EXPECT_EQ(admissions, 0);
    server.UnregisterHandler(MessageType::Connect);
    server.Shutdown();
}

TEST(SecureTransport_Handshake_PlayerNameNeverInPlaintextCapture)
{
    const std::string name = "Plaintext-Canary-Name";
    auto& client = NetworkManager::GetInstance();
    SecureRawServer fake;
    ASSERT_TRUE(StartClient(client, fake, name));
    ASSERT_FALSE(fake.Accept(4).empty());
    const auto finished = fake.AwaitType(client, MessageType::ClientFinished);
    ASSERT_TRUE(finished.has_value());
    EXPECT_TRUE(finished->payload == EncodeName(name)); // the channel carries it

    ASSERT_FALSE(fake.Socket().Captured().empty());
    for (const auto& datagram : fake.Socket().Captured())
        EXPECT_FALSE(ContainsBytes(datagram, Bytes(name)));
    client.Shutdown();
}

// ============================================================================
// Sealed datagrams
// ============================================================================

TEST(SecureTransport_Wired_PlaintextGameplayAfterHandshakeDropped)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    ChatCounter chats(server);
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "Plain"));
    const uint32_t droppedBefore = server.GetStats().plaintextFramesDropped;

    const auto wire = ChatWire("plaintext", peer.Id());
    peer.Socket().SendTo(server.GetBoundPort(), wire);                 // unframed
    peer.Socket().SendTo(server.GetBoundPort(), HandshakeFrame(wire)); // wrongly framed as handshake
    Pump(server);
    EXPECT_EQ(chats.count, 0);
    EXPECT_TRUE(server.GetStats().plaintextFramesDropped >= droppedBefore + 2);

    ASSERT_TRUE(peer.SendSealed(wire)); // positive control
    EXPECT_TRUE(PumpUntil(server, [&] { return chats.count == 1; }));
    server.Shutdown();
}

TEST(SecureTransport_Wired_BitFlippedSealedPacketDropped)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    ChatCounter chats(server);
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "Flip"));
    const auto authBefore = server.GetStats().securityDrops[static_cast<size_t>(OpenResult::AuthenticationFailed)];

    for (const size_t offset : {size_t{3}, size_t{20}}) // sequence byte, ciphertext byte
    {
        auto frame = peer.Seal(ChatWire("flip", peer.Id()));
        ASSERT_TRUE(frame.size() > offset);
        frame[offset] ^= 0x40;
        peer.Socket().SendTo(server.GetBoundPort(), frame);
    }
    auto frame = peer.Seal(ChatWire("flip", peer.Id()));
    frame.back() ^= 0x01; // tag
    peer.Socket().SendTo(server.GetBoundPort(), frame);
    Pump(server);
    EXPECT_EQ(chats.count, 0);
    EXPECT_EQ(server.GetStats().securityDrops[static_cast<size_t>(OpenResult::AuthenticationFailed)], authBefore + 3);
    server.Shutdown();
}

TEST(SecureTransport_Wired_ReplayedSealedPacketDropped)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    ChatCounter chats(server);
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "Replay"));

    const auto frame = peer.Seal(ChatWire("once", peer.Id()));
    peer.Socket().SendTo(server.GetBoundPort(), frame);
    ASSERT_TRUE(PumpUntil(server, [&] { return chats.count == 1; }));
    peer.Socket().SendTo(server.GetBoundPort(), frame);
    peer.Socket().SendTo(server.GetBoundPort(), frame);
    Pump(server);
    EXPECT_EQ(chats.count, 1);
    EXPECT_EQ(server.GetStats().securityDrops[static_cast<size_t>(OpenResult::Replayed)], 2u);
    server.Shutdown();
}

TEST(SecureTransport_Wired_ReorderInsideWindowDelivered)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    ChatCounter chats(server);
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "Reorder"));

    const auto a = peer.Seal(ChatWire("a", peer.Id()));
    const auto b = peer.Seal(ChatWire("b", peer.Id()));
    const auto c = peer.Seal(ChatWire("c", peer.Id()));
    for (const auto* frame : {&c, &a, &b})
        peer.Socket().SendTo(server.GetBoundPort(), *frame);
    EXPECT_TRUE(PumpUntil(server, [&] { return chats.count == 3; }));
    EXPECT_EQ(chats.count, 3);
    EXPECT_EQ(server.GetStats().securityDrops[static_cast<size_t>(OpenResult::Replayed)], 0u);
    server.Shutdown();
}

TEST(SecureTransport_Wired_WrongKeyPeerDropped)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    ChatCounter chats(server);
    SecureRawClient alice;
    SecureRawClient mallory;
    ASSERT_TRUE(alice.Connect(server, "Alice"));
    ASSERT_TRUE(mallory.Connect(server, "Mallory"));

    // Mallory seals with her own valid channel but injects from Alice's endpoint.
    alice.Socket().SendTo(server.GetBoundPort(), mallory.Seal(ChatWire("forged", alice.Id())));
    Pump(server);
    EXPECT_EQ(chats.count, 0);
    EXPECT_EQ(server.GetStats().securityDrops[static_cast<size_t>(OpenResult::AuthenticationFailed)], 1u);

    // A sealed frame from an endpoint with no session is not even opened.
    SecureRawClient stranger;
    stranger.Socket().SendTo(server.GetBoundPort(), mallory.Seal(ChatWire("stranger", mallory.Id())));
    Pump(server);
    EXPECT_EQ(chats.count, 0);
    server.Shutdown();
}

TEST(SecureTransport_Wired_TruncatedSealedFrameDropped)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    ChatCounter chats(server);
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "Trunc"));

    auto frame = peer.Seal(ChatWire("truncated", peer.Id()));
    frame.pop_back();
    peer.Socket().SendTo(server.GetBoundPort(), frame);
    const std::vector<uint8_t> stub(frame.begin(), frame.begin() + 6);
    peer.Socket().SendTo(server.GetBoundPort(), stub);
    Pump(server);
    EXPECT_EQ(chats.count, 0);
    const auto stats = server.GetStats();
    EXPECT_EQ(stats.securityDrops[static_cast<size_t>(OpenResult::AuthenticationFailed)], 1u);
    EXPECT_EQ(stats.securityDrops[static_cast<size_t>(OpenResult::Malformed)], 1u);
    server.Shutdown();
}

TEST(SecureTransport_Wired_SensitiveMessageRefusedWithoutChannel)
{
    auto& client = NetworkManager::GetInstance();
    SecureRawServer silent; // never answers the handshake
    ASSERT_TRUE(StartClient(client, silent, "Early"));

    const std::vector<uint8_t> secret(96, 0x5C);
    NetworkMessage credential;
    credential.type = static_cast<MessageType>(1234);
    credential.channel = ChannelType::Reliable;
    credential.payload = secret;
    credential.sensitive = true;
    client.SendMessage(credential);
    Pump(client);

    EXPECT_TRUE(client.GetStats().unsealedSendsRefused >= 1u);
    for (const auto& datagram : silent.Socket().Captured())
        EXPECT_FALSE(ContainsBytes(datagram, secret));
    client.Shutdown();
}

TEST(SecureTransport_Wired_CaptureContainsNoPayloadPlaintext)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "Capture"));

    const std::string chatText = "chat-canary-7f3e";
    NetBuffer chatPayload;
    chatPayload.WriteString(chatText);
    NetworkMessage chat;
    chat.type = MessageType::ChatMessage;
    chat.channel = ChannelType::Reliable;
    chat.payload = chatPayload.GetData();
    server.SendToClient(peer.Id(), chat);

    std::vector<uint8_t> credential(96);
    for (size_t i = 0; i < credential.size(); ++i)
        credential[i] = static_cast<uint8_t>(0xA0 + (i % 17));
    NetworkMessage secret;
    secret.type = static_cast<MessageType>(1234);
    secret.channel = ChannelType::Reliable;
    secret.payload = credential;
    secret.sensitive = true;
    server.SendToClient(peer.Id(), secret);

    bool sawChat = false;
    bool sawSecret = false;
    PumpUntil(server,
              [&]
              {
                  while (auto datagram = peer.Socket().Receive())
                  {
                      if (auto opened = peer.Open(*datagram))
                      {
                          sawChat = sawChat || opened->payload == chat.payload;
                          sawSecret = sawSecret || opened->payload == credential;
                      }
                  }
                  return sawChat && sawSecret;
              });
    EXPECT_TRUE(sawChat);
    EXPECT_TRUE(sawSecret);
    for (const auto& datagram : peer.Socket().Captured())
    {
        EXPECT_FALSE(ContainsBytes(datagram, Bytes(chatText)));
        EXPECT_FALSE(ContainsBytes(datagram, credential));
    }
    server.Shutdown();
}

TEST(SecureTransport_Wired_SendKeyRotatesAfterSessionTime)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "Rotate"));

    // Advance session time past SECURE_ROTATE_AFTER_SECONDS in steps shorter than the
    // connection timeout, keeping the session alive with sealed heartbeats.
    const auto heartbeat = BuildWire(MessageType::Heartbeat, {}, ChannelType::Unreliable, 0, peer.Id());
    int opened = 0;
    for (float elapsed = 0.0f; elapsed < SECURE_ROTATE_AFTER_SECONDS + 20.0f; elapsed += 8.0f)
    {
        peer.SendSealed(heartbeat);
        PumpUntil(server, [] { return false; }, std::chrono::milliseconds(6));
        server.Update(8.0f);
        while (auto datagram = peer.Socket().Receive())
        {
            if (peer.Open(*datagram))
                ++opened;
        }
    }
    Pump(server);
    while (auto datagram = peer.Socket().Receive())
    {
        if (peer.Open(*datagram))
            ++opened;
    }
    ASSERT_EQ(server.GetClients().size(), static_cast<size_t>(1));
    EXPECT_TRUE(server.GetStats().keyRotations >= 1u);
    ASSERT_TRUE(peer.Channel() != nullptr);
    EXPECT_TRUE(peer.Channel()->GetReceiveEpoch() >= 1); // the client followed the rotation
    EXPECT_TRUE(opened > 0);
    server.Shutdown();
}

// ============================================================================
// TERRAFRONT login on the production encrypted path
// ============================================================================

namespace
{
    constexpr MessageType TFType(Terrafront::TFMsg id)
    {
        return static_cast<MessageType>(static_cast<uint16_t>(id));
    }

    template <typename T> std::vector<uint8_t> PodBytes(const T& value)
    {
        std::vector<uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        return bytes;
    }

    template <typename T> std::optional<T> PodFrom(const WireMessage& message)
    {
        if (message.payload.size() != sizeof(T))
            return std::nullopt;
        T value{};
        std::memcpy(&value, message.payload.data(), sizeof(T));
        return value;
    }

    /// Server side of the TERRAFRONT onboarding ids, installed as NetworkManager observers the
    /// way TFServerSim::RegisterNetHandlers does, answering through the same TFScramWire helpers
    /// TFServerSim's HandleLogin / HandleLoginProof / HandleRegister call.
    struct TFLoginServer
    {
        explicit TFLoginServer(NetworkManager& manager) : m_manager(manager)
        {
            std::filesystem::remove(m_path);
            opened = m_db.Open(m_path);
            m_accounts.SetDatabase(&m_db);
            m_manager.RegisterSensitiveHandler(TFType(Terrafront::TFMsg::RegisterRequest),
                                               [this](const NetworkMessage& m) { OnRegister(m); });
            m_manager.RegisterSensitiveHandler(TFType(Terrafront::TFMsg::LoginRequest),
                                               [this](const NetworkMessage& m) { OnLoginStart(m); });
            m_manager.RegisterSensitiveHandler(TFType(Terrafront::TFMsg::LoginProof),
                                               [this](const NetworkMessage& m) { OnLoginProof(m); });
        }
        ~TFLoginServer()
        {
            for (const auto id :
                 {Terrafront::TFMsg::RegisterRequest, Terrafront::TFMsg::LoginRequest, Terrafront::TFMsg::LoginProof})
                m_manager.UnregisterHandler(TFType(id));
            m_db.Close();
            std::filesystem::remove(m_path);
        }
        TFLoginServer(const TFLoginServer&) = delete;
        TFLoginServer& operator=(const TFLoginServer&) = delete;

        template <typename T> void Reply(ClientID client, Terrafront::TFMsg id, const T& body)
        {
            NetworkMessage message;
            message.type = TFType(id);
            message.channel = ChannelType::Reliable;
            message.payload = PodBytes(body);
            m_manager.SendToClient(client, message);
        }

        void OnRegister(const NetworkMessage& m)
        {
            Terrafront::TF_RegisterRequest request{};
            if (m.payload.size() != sizeof(request))
                return;
            std::memcpy(&request, m.payload.data(), sizeof(request));
            const Terrafront::TFAuthResult result = Terrafront::RegisterFromRequest(m_accounts, request);
            Terrafront::TF_AuthReply reply{};
            reply.ok = result.ok ? 1 : 0;
            reply.err = static_cast<uint8_t>(result.err);
            reply.accountId = result.accountId;
            Reply(m.senderID, Terrafront::TFMsg::RegisterReply, reply);
        }

        void OnLoginStart(const NetworkMessage& m)
        {
            Terrafront::TF_LoginStart start{};
            if (m.payload.size() != sizeof(start))
                return;
            std::memcpy(&start, m.payload.data(), sizeof(start));
            Terrafront::TF_LoginChallenge challenge{};
            if (Terrafront::MakeLoginChallenge(
                    m_accounts.BeginLogin(m.senderID, Terrafront::TFWireUsername(start.user)), challenge))
                Reply(m.senderID, Terrafront::TFMsg::LoginChallenge, challenge);
        }

        void OnLoginProof(const NetworkMessage& m)
        {
            Terrafront::TF_LoginProof proof{};
            if (m.payload.size() != sizeof(proof))
                return;
            std::memcpy(&proof, m.payload.data(), sizeof(proof));
            const Terrafront::TFScramLoginResult result =
                Terrafront::CompleteLoginFromProof(m_accounts, m.senderID, proof);
            Terrafront::TF_AuthReply reply{};
            reply.ok = result.auth.ok ? 1 : 0;
            reply.err = static_cast<uint8_t>(result.auth.err);
            reply.accountId = result.auth.accountId;
            if (result.auth.ok)
                std::memcpy(reply.serverSignature, result.serverSignature.data(), sizeof(reply.serverSignature));
            Reply(m.senderID, Terrafront::TFMsg::LoginReply, reply);
        }

        bool opened = false;

      private:
        NetworkManager& m_manager;
        std::string m_path = "Saves/test_net100_tflogin_capture.db";
        Terrafront::TFDatabase m_db;
        Terrafront::TFAccountSystem m_accounts;
    };
} // namespace

TEST(SecureTransport_TFLogin_CaptureHasNoPasswordOrSaltedPassword)
{
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    TFLoginServer tfServer(server);
    ASSERT_TRUE(tfServer.opened);
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "TerrafrontPlayer"));

    const std::string user = "capturepilot";
    const std::string password = "tf-capture-canary-password";
    std::vector<std::vector<uint8_t>> sent; // every datagram the client put on the wire
    const auto sendSealed = [&](Terrafront::TFMsg id, const std::vector<uint8_t>& body)
    {
        const auto frame = peer.Seal(BuildWire(TFType(id), body, ChannelType::Reliable, 0, peer.Id()));
        sent.push_back(frame);
        return peer.Socket().SendTo(server.GetBoundPort(), frame);
    };

    // Registration: a client-derived verifier.
    Terrafront::TFAuthErr error = Terrafront::TFAuthErr::ServerError;
    const auto registration = Terrafront::MakeScramRegistration(user, password, error);
    ASSERT_TRUE(registration.has_value());
    ASSERT_TRUE(sendSealed(Terrafront::TFMsg::RegisterRequest, PodBytes(*registration)));
    const auto registered = peer.AwaitType(server, TFType(Terrafront::TFMsg::RegisterReply));
    ASSERT_TRUE(registered.has_value());
    const auto registerReply = PodFrom<Terrafront::TF_AuthReply>(*registered);
    ASSERT_TRUE(registerReply.has_value());
    EXPECT_EQ(static_cast<int>(registerReply->ok), 1);

    // Login: LoginRequest -> LoginChallenge -> LoginProof -> LoginReply, all sealed.
    Terrafront::TFScramClient scram;
    ASSERT_TRUE(sendSealed(Terrafront::TFMsg::LoginRequest, PodBytes(scram.Start(user, password))));
    const auto challengeMessage = peer.AwaitType(server, TFType(Terrafront::TFMsg::LoginChallenge));
    ASSERT_TRUE(challengeMessage.has_value());
    const auto challenge = PodFrom<Terrafront::TF_LoginChallenge>(*challengeMessage);
    ASSERT_TRUE(challenge.has_value());
    const auto proof = scram.Answer(*challenge, error);
    ASSERT_TRUE(proof.has_value());
    ASSERT_TRUE(sendSealed(Terrafront::TFMsg::LoginProof, PodBytes(*proof)));
    const auto replyMessage = peer.AwaitType(server, TFType(Terrafront::TFMsg::LoginReply));
    ASSERT_TRUE(replyMessage.has_value());
    const auto reply = PodFrom<Terrafront::TF_AuthReply>(*replyMessage);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(static_cast<int>(reply->ok), 1);
    EXPECT_TRUE(reply->accountId != 0);
    EXPECT_TRUE(scram.VerifyServer(*reply)); // the server proved it holds this account's ServerKey

    // Neither the password, nor SaltedPassword (PBKDF2), nor ClientKey appears in any datagram
    // either direction carried; and nothing the transport sent was plaintext after the handshake.
    const std::vector<uint8_t> salt(registration->salt, registration->salt + registration->saltLen);
    const auto salted = Terrafront::Crypto::Pbkdf2HmacSha256(password, salt, registration->iterations, 32);
    const Terrafront::Crypto::ScramKeys keys =
        Terrafront::Crypto::DeriveScramKeys(password, salt, registration->iterations);
    const std::vector<uint8_t> clientKey(keys.clientKey.begin(), keys.clientKey.end());
    std::vector<std::vector<uint8_t>> capture = sent;
    capture.insert(capture.end(), peer.Socket().Captured().begin(), peer.Socket().Captured().end());
    ASSERT_TRUE(capture.size() >= 6u);
    for (const auto& datagram : capture)
    {
        EXPECT_FALSE(ContainsBytes(datagram, Bytes(password)));
        EXPECT_FALSE(ContainsBytes(datagram, salted));
        EXPECT_FALSE(ContainsBytes(datagram, clientKey));
        EXPECT_FALSE(ContainsBytes(datagram, Bytes(user))); // the username is inside the channel too
    }
    server.Shutdown();
}

#endif // ENABLE_NETWORKING
