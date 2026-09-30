/**
 * @file TestMMOAuthHardening.cpp
 * @brief MMO authentication hardening: equal-work login, per-peer admission, registration
 *        limits, and game-thread chat delivery.
 *
 * MMOAuth_*    : account system and admission-budget policy (compiled in every configuration).
 * MMOAuthNet_* : the real MMOSessionGate and MMOChatSystem behind the NetworkManager singleton
 *                over secured loopback (networking + the ImGui-gated MMO sources).
 *
 * Timing is asserted structurally (how many PBKDF2 verifications run, with which parameters),
 * never by wall-clock measurement. Admission budgets run on the gate's server tick time, which
 * the tests drive explicitly.
 */

#include "TestFramework.h"

#include "../GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.h"
#include "../GameModules/SparkGameMMO/Source/Session/MMOAuthAdmission.h"

#if defined(ENABLE_NETWORKING) && defined(SPARK_TEST_HAS_IMGUI)
#include "../GameModules/SparkGameMMO/Source/Character/MMOCharacterSystem.h"
#include "../GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.h"
#include "../GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.h"
#include "../GameModules/SparkGameMMO/Source/Session/MMOSessionGate.h"
#include "Engine/Networking/NetworkManager.h"
#include "Fixtures/SecureTestPeer.h"
#include "Spark/IEngineContext.h"

#include <chrono>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <thread>

// Windows.h defines SendMessage; keep the alias away from MMOChatSystem calls.
#ifdef SendMessage
#undef SendMessage
#endif
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    /// Records each password verification Login asks for. The recorded hash fixes the PBKDF2
    /// work Spark::PasswordHash::Verify performs (scheme, iteration count, salt and key length).
    struct VerifierProbe
    {
        int calls = 0;
        std::string lastHash;
    };
    VerifierProbe g_verifierProbe;

    bool RecordingVerifier(std::string_view /*password*/, std::string_view encodedHash)
    {
        ++g_verifierProbe.calls;
        g_verifierProbe.lastHash = std::string(encodedHash);
        return false;
    }

    std::vector<std::string> SplitHash(const std::string& encoded)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true)
        {
            const size_t separator = encoded.find('$', start);
            parts.push_back(encoded.substr(start, separator == std::string::npos ? separator : separator - start));
            if (separator == std::string::npos)
            {
                return parts;
            }
            start = separator + 1;
        }
    }

    bool IsHex(const std::string& text)
    {
        return !text.empty() && text.find_first_not_of("0123456789abcdef") == std::string::npos;
    }

    /// True when @p candidate makes Verify run exactly the key derivation @p reference does.
    bool SameDerivationWork(const std::string& reference, const std::string& candidate)
    {
        const auto ref = SplitHash(reference);
        const auto got = SplitHash(candidate);
        return ref.size() == 4 && got.size() == 4 && got[0] == ref[0] && got[1] == ref[1] &&
               got[2].size() == ref[2].size() && got[3].size() == ref[3].size() && IsHex(got[2]) && IsHex(got[3]);
    }
} // namespace

// SEC follow-up: Login used to return before any key derivation when the username was unknown
// (or the account was locked, suspended or banned), so response time revealed which usernames
// exist. Every path must now run exactly one verification with the production PBKDF2 parameters.
TEST(MMOAuth_UnknownUserPerformsSamePbkdf2WorkAsKnownUser)
{
    MMO::MMOAccountSystem accounts;
    ASSERT_TRUE(accounts.Initialize(nullptr));
    const MMO::AuthResult registered = accounts.Register("timing_known", "correct-horse-battery");
    ASSERT_TRUE(registered.success);
    const auto account = accounts.GetAccount(registered.accountId);
    ASSERT_TRUE(account.has_value());
    const std::string realHash = account->passwordHash;
    ASSERT_EQ(SplitHash(realHash).size(), static_cast<size_t>(4));

    accounts.SetPasswordVerifier(&RecordingVerifier);

    g_verifierProbe = {};
    const MMO::AuthResult wrongPassword = accounts.Login("timing_known", "wrong-password");
    EXPECT_FALSE(wrongPassword.success);
    EXPECT_EQ(g_verifierProbe.calls, 1);
    EXPECT_EQ(g_verifierProbe.lastHash, realHash);

    g_verifierProbe = {};
    const MMO::AuthResult unknown = accounts.Login("timing_nobody", "wrong-password");
    EXPECT_FALSE(unknown.success);
    EXPECT_EQ(unknown.errorMessage, wrongPassword.errorMessage);
    EXPECT_EQ(g_verifierProbe.calls, 1);
    EXPECT_TRUE(SameDerivationWork(realHash, g_verifierProbe.lastHash));
    EXPECT_NE(g_verifierProbe.lastHash, realHash);

    // A restricted account must not answer faster than an active one either.
    for (const auto status : {MMO::AccountStatus::Locked, MMO::AccountStatus::Suspended, MMO::AccountStatus::Banned})
    {
        ASSERT_TRUE(accounts.SetAccountStatus(registered.accountId, status, "test"));
        g_verifierProbe = {};
        const MMO::AuthResult restricted = accounts.Login("timing_known", "wrong-password");
        EXPECT_FALSE(restricted.success);
        EXPECT_EQ(g_verifierProbe.calls, 1);
        EXPECT_EQ(g_verifierProbe.lastHash, realHash);
    }
    // Attempts against a restricted account are refused before they count as failures.
    const auto after = accounts.GetAccount(registered.accountId);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->failedLoginAttempts, 1);

    accounts.SetPasswordVerifier(nullptr);
    accounts.Shutdown();
}

// One peer's credential attempts must never hold a different peer at RateLimited.
TEST(MMOAuth_AdmissionBudgetIsPerPeer)
{
    using Budget = MMO::AuthAdmissionBudget;
    Budget budget;
    Budget::Peer alice;
    Budget::Peer bob;

    EXPECT_TRUE(budget.TryAdmit(alice, Budget::Operation::Login));
    EXPECT_FALSE(budget.TryAdmit(alice, Budget::Operation::Login)); // alice's own cooldown
    EXPECT_TRUE(budget.TryAdmit(bob, Budget::Operation::Login));    // bob is unaffected by alice

    // Half the cooldown is not enough; the full cooldown restores alice.
    budget.Advance(Budget::PeerCooldown * 0.5f);
    Budget::AdvancePeer(alice, Budget::PeerCooldown * 0.5f);
    EXPECT_FALSE(budget.TryAdmit(alice, Budget::Operation::Login));
    budget.Advance(Budget::PeerCooldown * 0.5f);
    Budget::AdvancePeer(alice, Budget::PeerCooldown * 0.5f);
    EXPECT_TRUE(budget.TryAdmit(alice, Budget::Operation::Login));
}

// Brute-force bound: however many peers try, total KDF admissions stay within the global bucket.
TEST(MMOAuth_AdmissionBudgetBoundsAggregateRate)
{
    using Budget = MMO::AuthAdmissionBudget;
    Budget budget;
    std::array<Budget::Peer, 64> peers{};

    size_t burst = 0;
    for (auto& peer : peers)
    {
        burst += budget.TryAdmit(peer, Budget::Operation::Login) ? 1u : 0u;
    }
    EXPECT_EQ(burst, static_cast<size_t>(Budget::GlobalBurst));

    // Ten seconds of every peer retrying every 0.1 s tick.
    constexpr float kTick = 0.1f;
    constexpr int kTicks = 100;
    size_t admitted = 0;
    std::array<size_t, 64> perPeer{};
    for (int tick = 0; tick < kTicks; ++tick)
    {
        budget.Advance(kTick);
        for (size_t index = 0; index < peers.size(); ++index)
        {
            Budget::AdvancePeer(peers[index], kTick);
            if (budget.TryAdmit(peers[index], Budget::Operation::Login))
            {
                ++admitted;
                ++perPeer[index];
            }
        }
    }
    const float seconds = kTick * static_cast<float>(kTicks);
    EXPECT_LE(admitted, static_cast<size_t>(Budget::GlobalRate * seconds + 0.5f));
    EXPECT_GE(admitted, static_cast<size_t>(Budget::GlobalRate * seconds - 1.5f));
    for (const size_t count : perPeer)
    {
        EXPECT_LE(count, static_cast<size_t>(seconds / Budget::PeerCooldown + 0.5f));
    }
}

// Registration: one account per connection, and a global window across reconnecting peers.
TEST(MMOAuth_RegistrationBudgetPerPeerAndWindow)
{
    using Budget = MMO::AuthAdmissionBudget;
    Budget budget;
    std::array<Budget::Peer, 10> peers{};

    // Two ticks of four admissions: eight registrations exhaust the window.
    for (size_t index = 0; index < 4; ++index)
    {
        EXPECT_TRUE(budget.TryAdmit(peers[index], Budget::Operation::Register));
    }
    budget.Advance(1.0f);
    for (size_t index = 4; index < 8; ++index)
    {
        EXPECT_TRUE(budget.TryAdmit(peers[index], Budget::Operation::Register));
    }
    budget.Advance(1.0f);
    EXPECT_FALSE(budget.TryAdmit(peers[8], Budget::Operation::Register));
    // A refused registration charges nothing: the same peer can still log in.
    EXPECT_TRUE(budget.TryAdmit(peers[8], Budget::Operation::Login));

    // The window refills at RegistrationsPerWindow per RegistrationWindow seconds.
    const float refillOne = Budget::RegistrationWindow / Budget::RegistrationsPerWindow;
    budget.Advance(refillOne);
    Budget::AdvancePeer(peers[9], refillOne);
    EXPECT_TRUE(budget.TryAdmit(peers[9], Budget::Operation::Register));

    // A peer that already registered cannot register again, even with its cooldown elapsed.
    budget.Advance(Budget::RegistrationWindow);
    Budget::AdvancePeer(peers[0], Budget::RegistrationWindow);
    EXPECT_FALSE(budget.TryAdmit(peers[0], Budget::Operation::Register));
    EXPECT_TRUE(budget.TryAdmit(peers[0], Budget::Operation::Login));
}

#if defined(ENABLE_NETWORKING) && defined(SPARK_TEST_HAS_IMGUI)

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

        /// Pump transport and gate (@p gateDelta server seconds per tick) until every client
        /// has the reply for its most recent request.
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

// SEC follow-up: the gate's single global 0.25 s cooldown let one client hold every other client
// at RateLimited. Two peers authenticating in the same server tick must both be served, while a
// peer that retries immediately is still throttled.
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
    // A negligible gate delta keeps both requests inside one admission interval.
    ASSERT_TRUE(server.AwaitReplies(network, {&alice, &bob}, 0.0001f, statuses));
    EXPECT_EQ(statuses[0], Wire::Status::Rejected);
    EXPECT_EQ(statuses[1], Wire::Status::Rejected);

    ASSERT_TRUE(alice.Send(network, Wire::Operation::Login, "nobody_alice"));
    ASSERT_TRUE(server.AwaitReplies(network, {&alice}, 0.0001f, statuses));
    EXPECT_EQ(statuses[0], Wire::Status::RateLimited);

    server.Stop(network);
}

// SEC follow-up: wire registration was bounded only by the 1024-account cap, so one connection
// could create accounts continuously. A connection may now register one account.
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
    EXPECT_EQ(statuses[0], Wire::Status::Ok);

    // Well past every cooldown (gate time advances at most 0.1 s per Update).
    for (int tick = 0; tick < 50; ++tick)
    {
        network.Update(0.016f);
        server.gate.Update(0.1f);
    }
    ASSERT_TRUE(client.Send(network, Wire::Operation::Register, "reg_second"));
    ASSERT_TRUE(server.AwaitReplies(network, {&client}, 0.0001f, statuses));
    EXPECT_EQ(statuses[0], Wire::Status::RateLimited);
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

// SEC follow-up: the chat handler runs on whichever thread pumps NetworkManager (the
// DedicatedServer tick thread), but it appended to the history the game thread reads and
// writes. Network delivery must only queue; the game thread's Update records and relays.
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
    // The probe follows the chat datagram, so once it is dispatched the chat handler has run.
    ASSERT_TRUE(speaker.SendSealed(
        SparkTestFixtures::BuildWire(kProbeType, std::vector<uint8_t>{0x01}, ChannelType::Reliable, 2, speaker.Id())));
    ASSERT_TRUE(SparkTestFixtures::PumpUntil(network, [&] { return probes > 0; }, kWindow));
    for (int tick = 0; tick < 5; ++tick)
    {
        network.Update(0.016f);
    }
    // Network-thread work alone must not have touched game-thread state.
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
