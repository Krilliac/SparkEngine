/**
 * @file TestMMOAuthNetwork.cpp
 * @brief Behavioral MMO session-gate registration and chat-thread tests.
 *
 * These tests intentionally use only the pre-existing wire, session-gate, and chat APIs. They
 * can therefore be run against the pre-hardening baseline to demonstrate behavioral RED results.
 */

#include "TestFramework.h"

#if defined(ENABLE_NETWORKING) && defined(SPARK_TEST_HAS_IMGUI)

#include "../GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.h"
#include "../GameModules/SparkGameMMO/Source/Character/MMOCharacterSystem.h"
#include "../GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.h"
#include "../GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.h"
#include "../GameModules/SparkGameMMO/Source/Session/MMOSessionGate.h"
#include "Engine/Networking/NetworkManager.h"
#include "Fixtures/SecureTestPeer.h"
#include "Spark/IEngineContext.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Windows.h defines SendMessage; keep the alias away from MMOChatSystem calls.
#ifdef SendMessage
#undef SendMessage
#endif

namespace
{
    using Spark::Net::ChannelType;
    using Spark::Net::MessageType;
    using Spark::Net::NetworkManager;
    using SparkTestFixtures::SecureRawClient;
    namespace Wire = MMO::SessionGateWire;

    constexpr auto kWindow = std::chrono::milliseconds(20000);

    class AuthNetContext final : public Spark::IEngineContext
    {
      public:
        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        Spark::NetworkManager* GetNetwork() override { return &NetworkManager::GetInstance(); }
        const Spark::NetworkManager* GetNetwork() const override { return &NetworkManager::GetInstance(); }
        bool IsHeadless() const override { return true; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }
    };

    bool StartLoopbackServer(NetworkManager& manager)
    {
        manager.Shutdown();
        return manager.Initialize() && manager.StartServer(0, 8, Spark::Net::NetworkEndpointPolicy::Loopback());
    }

    /// A raw secured client that speaks the session-gate wire protocol.
    struct GateClient
    {
        SecureRawClient raw;
        uint32_t sequence = 1;
        uint32_t requestId = 0;

        bool Send(NetworkManager& server, Wire::Operation operation, std::string_view user)
        {
            Wire::Packet packet{};
            packet.operation = operation;
            packet.requestId = ++requestId;
            std::memcpy(packet.username.data(), user.data(), (std::min)(user.size(), packet.username.size() - 1));
            constexpr std::string_view kPassword = "mmo-auth-test-password";
            std::memcpy(packet.password.data(), kPassword.data(), kPassword.size());
            const std::vector<uint8_t> payload = Wire::Encode(packet);
            return !payload.empty() &&
                   raw.SendSealed(SparkTestFixtures::BuildWire(MMO::MMOSessionGate::RequestType, payload,
                                                               ChannelType::Reliable, sequence++, raw.Id()),
                                  server.GetBoundPort());
        }

        /// The reply to requestId, if it has arrived.
        std::optional<Wire::Packet> Poll(uint32_t id)
        {
            while (auto datagram = raw.Socket().Receive())
            {
                const auto message = raw.Open(*datagram);
                Wire::Packet reply{};
                if (message && message->type == MMO::MMOSessionGate::ReplyType &&
                    Wire::Decode(message->payload, reply) && reply.response && reply.requestId == id &&
                    reply.operation != Wire::Operation::State)
                {
                    m_replies.push_back(reply);
                }
            }
            for (const auto& reply : m_replies)
            {
                if (reply.requestId == id)
                {
                    return reply;
                }
            }
            return std::nullopt;
        }

      private:
        std::vector<Wire::Packet> m_replies;
    };

    /// Real server-side session gate with the services the MMO module binds to it.
    struct GateServer
    {
        AuthNetContext context;
        MMO::MMOAccountSystem accounts;
        MMO::MMOCharacterSystem characters;
        MMO::MMOPlayerSystem players;
        MMO::MMOSessionGate gate;

        bool Start(NetworkManager& network)
        {
            return StartLoopbackServer(network) && accounts.Initialize(&context) && characters.Initialize(&context) &&
                   players.Initialize(&context) && gate.Initialize(network, accounts, characters, players);
        }

        void Stop(NetworkManager& network)
        {
            gate.Shutdown();
            players.Shutdown();
            characters.Shutdown();
            accounts.Shutdown();
            network.Shutdown();
        }

        /// Pump transport and gate until every client has the reply for its most recent request.
        bool AwaitReplies(NetworkManager& network, std::initializer_list<GateClient*> clients, float gateDelta,
                          std::vector<Wire::Status>& statuses)
        {
            const auto deadline = std::chrono::steady_clock::now() + kWindow;
            while (std::chrono::steady_clock::now() < deadline)
            {
                network.Update(0.016f);
                gate.Update(gateDelta);
                statuses.clear();
                for (GateClient* client : clients)
                {
                    if (const auto reply = client->Poll(client->requestId))
                    {
                        statuses.push_back(reply->status);
                    }
                }
                if (statuses.size() == clients.size())
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        }
    };
} // namespace

// SEC follow-up: one peer must not throttle another peer's session-gate request.
TEST(MMOAuthNet_OnePeerCannotThrottleAnother)
{
    auto& network = NetworkManager::GetInstance();
    GateServer server;
    ASSERT_TRUE(server.Start(network));
    GateClient alice;
    GateClient bob;
    ASSERT_TRUE(alice.raw.Connect(network, "AuthAlice"));
    ASSERT_TRUE(bob.raw.Connect(network, "AuthBob"));

    ASSERT_TRUE(alice.Send(network, Wire::Operation::Login, "nobody_alice"));
    ASSERT_TRUE(bob.Send(network, Wire::Operation::Login, "nobody_bob"));
    std::vector<Wire::Status> statuses;
    ASSERT_TRUE(server.AwaitReplies(network, {&alice, &bob}, 0.0001f, statuses));
    EXPECT_EQ(static_cast<int>(statuses[0]), static_cast<int>(Wire::Status::Rejected));
    EXPECT_EQ(static_cast<int>(statuses[1]), static_cast<int>(Wire::Status::Rejected));

    ASSERT_TRUE(alice.Send(network, Wire::Operation::Login, "nobody_alice"));
    ASSERT_TRUE(server.AwaitReplies(network, {&alice}, 0.0001f, statuses));
    EXPECT_EQ(static_cast<int>(statuses[0]), static_cast<int>(Wire::Status::RateLimited));

    server.Stop(network);
}

// SEC follow-up: wire registration must be bounded per peer, beyond the account-count cap.
TEST(MMOAuthNet_RegistrationIsLimitedPerPeer)
{
    auto& network = NetworkManager::GetInstance();
    GateServer server;
    ASSERT_TRUE(server.Start(network));
    GateClient client;
    ASSERT_TRUE(client.raw.Connect(network, "AuthRegistrar"));

    ASSERT_TRUE(client.Send(network, Wire::Operation::Register, "reg_first"));
    std::vector<Wire::Status> statuses;
    ASSERT_TRUE(server.AwaitReplies(network, {&client}, 0.0001f, statuses));
    EXPECT_EQ(static_cast<int>(statuses[0]), static_cast<int>(Wire::Status::Ok));

    for (int tick = 0; tick < 50; ++tick)
    {
        network.Update(0.016f);
        server.gate.Update(0.1f);
    }
    ASSERT_TRUE(client.Send(network, Wire::Operation::Register, "reg_second"));
    ASSERT_TRUE(server.AwaitReplies(network, {&client}, 0.0001f, statuses));
    EXPECT_EQ(static_cast<int>(statuses[0]), static_cast<int>(Wire::Status::RateLimited));
    EXPECT_EQ(server.accounts.GetAccountCount(), static_cast<size_t>(1));
    EXPECT_FALSE(server.accounts.FindAccount("reg_second").has_value());

    server.Stop(network);
}

namespace
{
    constexpr auto kChatType = static_cast<MessageType>(static_cast<uint16_t>(MessageType::UserDefined) + 1u);
    constexpr auto kProbeType = static_cast<MessageType>(static_cast<uint16_t>(MessageType::UserDefined) + 7u);

    bool HistoryContains(const MMO::MMOChatSystem& chat, const std::string& text)
    {
        for (const auto& entry : chat.GetHistory())
        {
            if (entry.text == text)
            {
                return true;
            }
        }
        return false;
    }
} // namespace

// SEC follow-up: network delivery queues chat work; Update records and relays it on the game thread.
TEST(MMOAuthNet_ChatHandlerDefersToGameThread)
{
    auto& network = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(network));
    AuthNetContext context;
    MMO::MMOChatSystem chat;
    ASSERT_TRUE(chat.Initialize(&context));

    int probes = 0;
    network.GetPacketValidator().RegisterSchema(kProbeType, {.minPayloadSize = 1,
                                                             .maxPayloadSize = 16,
                                                             .requiresAuth = true,
                                                             .allowedFromClient = true,
                                                             .allowedFromServer = false});
    network.RegisterHandler(kProbeType, [&probes](const Spark::Net::NetworkMessage&) { ++probes; });

    SecureRawClient speaker;
    SecureRawClient listener;
    ASSERT_TRUE(speaker.Connect(network, "Speaker"));
    ASSERT_TRUE(listener.Connect(network, "Listener"));

    const std::string text = "deferred hello";
    const auto payload = MMO::MMOChatSystem::EncodeWirePayload(MMO::ChatChannel::Area, "Speaker", text);
    ASSERT_TRUE(
        speaker.SendSealed(SparkTestFixtures::BuildWire(kChatType, payload, ChannelType::Reliable, 1, speaker.Id())));
    ASSERT_TRUE(speaker.SendSealed(
        SparkTestFixtures::BuildWire(kProbeType, std::vector<uint8_t>{0x01}, ChannelType::Reliable, 2, speaker.Id())));
    ASSERT_TRUE(SparkTestFixtures::PumpUntil(network, [&] { return probes > 0; }, kWindow));
    for (int tick = 0; tick < 5; ++tick)
    {
        network.Update(0.016f);
    }
    EXPECT_FALSE(HistoryContains(chat, text));

    chat.Update(0.016f);
    EXPECT_TRUE(HistoryContains(chat, text));
    bool relayed = false;
    const auto deadline = std::chrono::steady_clock::now() + kWindow;
    while (!relayed && std::chrono::steady_clock::now() < deadline)
    {
        network.Update(0.016f);
        while (auto datagram = listener.Socket().Receive())
        {
            const auto message = listener.Open(*datagram);
            if (message && message->type == kChatType)
            {
                const auto decoded = MMO::MMOChatSystem::DecodeWirePayload(message->payload);
                relayed = decoded && decoded->text == text &&
                          decoded->senderName == "Speaker#" + std::to_string(speaker.Id());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(relayed);

    network.UnregisterHandler(kProbeType);
    chat.Shutdown();
    network.Shutdown();
}

#endif // ENABLE_NETWORKING && SPARK_TEST_HAS_IMGUI
