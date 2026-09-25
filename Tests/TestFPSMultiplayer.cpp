/**
 * @file TestFPSMultiplayer.cpp
 * @brief FPS multiplayer wire-format tests and production-class FPSMultiplayerSystem tests
 *
 * FPSMultiplayer_* covers the NetworkPlayerState/PlayerInput wire encoding.
 * FPSMultiplayerProduction_* drives the real SparkFPS::FPSMultiplayerSystem (compiled into
 * SparkTests from GameModules/SparkGameFPS/Source/Game/MultiplayerSystem.cpp) against the
 * real NetworkManager singleton: a loopback server on an ephemeral port for the server-side
 * tests, and a client that completes the handshake with a loopback peer for reconciliation. Message
 * handlers are private and not yet registered with NetworkManager, so the tests invoke them
 * through FPSMultiplayerSystemTestAccess exactly as the future dispatch will.
 */

#include "TestFramework.h"
#include "Game/MultiplayerSystem.h"
#include "Engine/Networking/NetworkManager.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#ifndef SPARK_PLATFORM_WINDOWS
#include <fcntl.h>
#endif

using namespace SparkFPS;

namespace SparkFPS
{
    struct FPSMultiplayerSystemTestAccess
    {
        static void SetSpawnPoints(FPSMultiplayerSystem& system, std::vector<SpawnPoint> points)
        {
            system.m_spawnPoints = std::move(points);
        }

        static void Join(FPSMultiplayerSystem& system, uint32_t clientId) { system.OnPlayerJoined(clientId); }

        static void Leave(FPSMultiplayerSystem& system, uint32_t clientId) { system.OnPlayerLeft(clientId); }

        static void DeliverInput(FPSMultiplayerSystem& system, uint32_t clientId, const PlayerInput& input)
        {
            system.OnPlayerInputReceived(clientId, input);
        }

        static void DeliverDamageReport(FPSMultiplayerSystem& system, uint32_t attackerId, uint32_t victimId,
                                        float damage)
        {
            system.OnPlayerDamaged(attackerId, victimId, damage);
        }

        static void DeliverSnapshot(FPSMultiplayerSystem& system, const NetworkPlayerState& snapshot)
        {
            system.OnStateSnapshotReceived(snapshot);
        }

        static size_t ActiveProjectiles(const FPSMultiplayerSystem& system) { return system.m_projectiles.size(); }

        static PlayerInput LastSentInput(const FPSMultiplayerSystem& system)
        {
            const auto it = system.m_lastInputByPlayer.find(system.m_localClientId);
            return it != system.m_lastInputByPlayer.end() ? it->second : PlayerInput{};
        }
    };
} // namespace SparkFPS

TEST(FPSMultiplayer_PlayerStateSerializationRoundtrip)
{
    NetworkPlayerState original;
    original.clientId = 7;
    original.posX = 15.5f;
    original.posY = 2.0f;
    original.posZ = -8.3f;
    original.velX = 4.0f;
    original.velY = -1.0f;
    original.velZ = 2.5f;
    original.yaw = 1.57f;
    original.pitch = -0.3f;
    original.health = 65.0f;
    original.actionFlags = ActionJump | ActionFire;
    original.acknowledgedInputSequence = 38;
    original.currentWeapon = 2;
    original.isAlive = true;
    original.isCrouching = true;
    original.sequenceNumber = 42;

    auto data = original.Serialize();
    auto restored = NetworkPlayerState::Deserialize(data.data(), data.size());

    EXPECT_EQ(data.size(), NetworkPlayerState::SerializedSize);
    EXPECT_EQ(restored.clientId, static_cast<uint32_t>(7));
    EXPECT_NEAR(restored.posX, 15.5f, 0.001f);
    EXPECT_NEAR(restored.posY, 2.0f, 0.001f);
    EXPECT_NEAR(restored.posZ, -8.3f, 0.001f);
    EXPECT_NEAR(restored.velX, 4.0f, 0.001f);
    EXPECT_NEAR(restored.velY, -1.0f, 0.001f);
    EXPECT_NEAR(restored.velZ, 2.5f, 0.001f);
    EXPECT_NEAR(restored.yaw, 1.57f, 0.001f);
    EXPECT_NEAR(restored.health, 65.0f, 0.001f);
    EXPECT_EQ(restored.actionFlags, static_cast<uint32_t>(ActionJump | ActionFire));
    EXPECT_EQ(restored.acknowledgedInputSequence, static_cast<uint32_t>(38));
    EXPECT_EQ(restored.currentWeapon, static_cast<uint8_t>(2));
    EXPECT_TRUE(restored.isAlive);
    EXPECT_TRUE(restored.isCrouching);
    EXPECT_EQ(restored.sequenceNumber, static_cast<uint32_t>(42));
}

TEST(FPSMultiplayer_PlayerInputSerializationRoundtrip)
{
    PlayerInput original;
    original.forward = 1.0f;
    original.strafe = -0.5f;
    original.yaw = 3.14f;
    original.pitch = 0.2f;
    original.jump = true;
    original.fire = true;
    original.reload = false;
    original.crouch = true;
    original.sequenceNumber = 100;

    auto data = original.Serialize();
    auto restored = PlayerInput::Deserialize(data.data(), data.size());

    EXPECT_EQ(data.size(), PlayerInput::SerializedSize);
    EXPECT_NEAR(restored.forward, 1.0f, 0.001f);
    EXPECT_NEAR(restored.strafe, -0.5f, 0.001f);
    EXPECT_TRUE(restored.jump);
    EXPECT_TRUE(restored.fire);
    EXPECT_FALSE(restored.reload);
    EXPECT_TRUE(restored.crouch);
    EXPECT_EQ(restored.sequenceNumber, static_cast<uint32_t>(100));
}

TEST(FPSMultiplayer_WireEncodingIsStableAndTruncatedPayloadDefaults)
{
    NetworkPlayerState state;
    state.clientId = 0x12345678u;
    state.health = 25.0f;
    const auto stateBytes = state.Serialize();

    EXPECT_EQ(stateBytes.size(), static_cast<size_t>(55));
    EXPECT_EQ(stateBytes[0], static_cast<uint8_t>(0x78));
    EXPECT_EQ(stateBytes[1], static_cast<uint8_t>(0x56));
    EXPECT_EQ(stateBytes[2], static_cast<uint8_t>(0x34));
    EXPECT_EQ(stateBytes[3], static_cast<uint8_t>(0x12));

    const auto truncatedState = NetworkPlayerState::Deserialize(stateBytes.data(), stateBytes.size() - 1);
    EXPECT_EQ(truncatedState.clientId, static_cast<uint32_t>(0));
    EXPECT_NEAR(truncatedState.health, 100.0f, 0.001f);

    PlayerInput input;
    input.forward = 1.0f;
    input.jump = true;
    const auto inputBytes = input.Serialize();
    EXPECT_EQ(inputBytes.size(), static_cast<size_t>(24));

    const auto truncatedInput = PlayerInput::Deserialize(inputBytes.data(), inputBytes.size() - 1);
    EXPECT_NEAR(truncatedInput.forward, 0.0f, 0.001f);
    EXPECT_FALSE(truncatedInput.jump);
}

namespace
{
    using Access = FPSMultiplayerSystemTestAccess;
    constexpr float kFrame = 1.0f / 60.0f;
    constexpr float kSnapshotStep = 0.06f; // one 20 Hz snapshot per Update
    constexpr uint32_t kHostId = 0;

    /// Shuts the singleton system and NetworkManager down at scope exit so a failed
    /// assertion never leaks a bound server or client socket into later tests.
    struct FPSSessionGuard
    {
        FPSSessionGuard() = default;
        FPSSessionGuard(const FPSSessionGuard&) = delete;
        FPSSessionGuard& operator=(const FPSSessionGuard&) = delete;
        ~FPSSessionGuard()
        {
            auto& system = FPSMultiplayerSystem::GetInstance();
            system.Shutdown();
            Access::SetSpawnPoints(system, {});
            Spark::Net::NetworkManager::GetInstance().Shutdown();
        }
    };

    /// Starts a real loopback server; the host (client 0) spawns far from the arena.
    bool StartServer(FPSMultiplayerSystem& system)
    {
        Spark::Net::NetworkManager::GetInstance().Shutdown();
        system.Initialize(true);
        Access::SetSpawnPoints(system, {{0.0f, 1.0f, -50.0f, 0.0f}});
        return system.StartServer(0, 8);
    }

    void JoinAt(FPSMultiplayerSystem& system, uint32_t clientId, float x, float y, float z, float yaw)
    {
        Access::SetSpawnPoints(system, {{x, y, z, yaw}});
        Access::Join(system, clientId);
    }

    float HealthOf(const FPSMultiplayerSystem& system, uint32_t clientId)
    {
        const NetworkPlayerState* state = system.GetPlayerState(clientId);
        return state ? state->health : -1.0f;
    }

    const PlayerScore* FindScore(const std::vector<PlayerScore>& board, uint32_t clientId)
    {
        for (const auto& score : board)
        {
            if (score.clientId == clientId)
                return &score;
        }
        return nullptr;
    }

    /// Loopback UDP peer standing in for an FPS server: it answers the client's Connect with
    /// a ConnectAccepted in the documented wire format (docs/specs/networking-wire-format.md)
    /// and sends nothing else. It holds its own Winsock reference on Windows so the
    /// NetworkManager::Shutdown() in FPSSessionGuard never pulls Winsock from under it.
    class HandshakePeer
    {
      public:
        HandshakePeer()
        {
#ifdef SPARK_PLATFORM_WINDOWS
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

        ~HandshakePeer()
        {
            if (m_socket != INVALID_SOCKET)
                closesocket(m_socket);
#ifdef SPARK_PLATFORM_WINDOWS
            if (m_winsockStarted)
                WSACleanup();
#endif
        }

        HandshakePeer(const HandshakePeer&) = delete;
        HandshakePeer& operator=(const HandshakePeer&) = delete;

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

        /// Pump @p client until its Connect arrives, accept it as @p assignedId, then pump
        /// until the client has adopted that id. Returns false on timeout.
        bool AcceptClient(FPSMultiplayerSystem& client, uint32_t assignedId)
        {
            sockaddr_in clientAddress{};
            if (!PumpUntil(client, [&] { return ReceiveConnect(clientAddress); }))
                return false;

            Spark::Net::NetBuffer payload;
            payload.WriteUint32(assignedId);
            payload.WriteFloat(0.0f); // server time
            payload.WriteUint16(Spark::Net::NETWORK_PROTOCOL_VERSION);
            const auto& body = payload.GetData();

            Spark::Net::NetBuffer wire;
            wire.WriteUint32(kWirePacketMagic);
            wire.WriteUint16(static_cast<uint16_t>(Spark::Net::MessageType::ConnectAccepted));
            wire.WriteUint8(static_cast<uint8_t>(Spark::Net::ChannelType::Reliable));
            wire.WriteUint32(Spark::Net::INVALID_CLIENT); // sender: the server
            wire.WriteUint32(1);                          // sequence
            wire.WriteFloat(0.0f);                        // timestamp
            wire.WriteUint32(static_cast<uint32_t>(body.size()));
            wire.WriteBytes(body.data(), body.size());
            const auto& datagram = wire.GetData();
            const int sent =
                sendto(m_socket, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                       reinterpret_cast<const sockaddr*>(&clientAddress), sizeof(clientAddress));
            if (sent != static_cast<int>(datagram.size()))
                return false;

            return PumpUntil(client, [&] { return client.GetLocalClientId() == assignedId; });
        }

      private:
        static constexpr uint32_t kWirePacketMagic = 0x5350524B; // "SPRK"

        template <typename Condition> static bool PumpUntil(FPSMultiplayerSystem& client, Condition done)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline)
            {
                client.Update(kFrame);
                if (done())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }

        bool ReceiveConnect(sockaddr_in& outSender) const
        {
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
            if (received <= 0)
                return false;

            Spark::Net::NetBuffer wire;
            wire.WriteBytes(buffer.data(), static_cast<size_t>(received));
            const uint32_t magic = wire.ReadUint32();
            const auto type = static_cast<Spark::Net::MessageType>(wire.ReadUint16());
            if (wire.HasError() || magic != kWirePacketMagic || type != Spark::Net::MessageType::Connect)
                return false;
            outSender = from;
            return true;
        }

        SOCKET m_socket = INVALID_SOCKET;
        bool m_ready = false;
#ifdef SPARK_PLATFORM_WINDOWS
        bool m_winsockStarted = false;
#endif
    };
} // namespace

TEST(FPSMultiplayerProduction_ServerAppliesInputAndAcknowledgesSequence)
{
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));
    EXPECT_TRUE(system.IsServer());
    EXPECT_TRUE(system.IsActive());
    ASSERT_TRUE(system.GetPlayerState(kHostId) != nullptr);

    JoinAt(system, 7, 0.0f, 1.0f, 0.0f, 0.0f);

    PlayerInput forward;
    forward.forward = 1.0f;
    forward.sequenceNumber = 1;
    Access::DeliverInput(system, 7, forward);

    const NetworkPlayerState* state = system.GetPlayerState(7);
    ASSERT_TRUE(state != nullptr);
    EXPECT_NEAR(state->posX, 8.0f * kFrame, 1e-5f);
    EXPECT_NEAR(state->posZ, 0.0f, 1e-5f);
    EXPECT_NEAR(state->velX, 8.0f, 1e-5f);

    // Strafing right at yaw 0 moves along -Z; the new yaw applies to the next input.
    PlayerInput strafe;
    strafe.strafe = 1.0f;
    strafe.yaw = 1.0f;
    strafe.crouch = true;
    strafe.sequenceNumber = 2;
    Access::DeliverInput(system, 7, strafe);
    EXPECT_NEAR(state->posX, 8.0f * kFrame, 1e-5f);
    EXPECT_NEAR(state->posZ, -8.0f * kFrame, 1e-5f);
    EXPECT_NEAR(state->yaw, 1.0f, 1e-6f);
    EXPECT_TRUE(state->isCrouching);
    EXPECT_EQ(state->actionFlags, static_cast<uint32_t>(ActionCrouch));

    // The next authoritative snapshot acknowledges the last applied input.
    system.Update(kSnapshotStep);
    EXPECT_EQ(state->acknowledgedInputSequence, static_cast<uint32_t>(2));
    EXPECT_GE(state->sequenceNumber, static_cast<uint32_t>(1));

    // Input for a player the server does not know is ignored, fire included.
    PlayerInput stray;
    stray.forward = 1.0f;
    stray.fire = true;
    Access::DeliverInput(system, 99, stray);
    EXPECT_TRUE(system.GetPlayerState(99) == nullptr);
    EXPECT_EQ(Access::ActiveProjectiles(system), static_cast<size_t>(0));

    // Fire input from a known player spawns one server projectile.
    PlayerInput fire;
    fire.fire = true;
    fire.yaw = 1.0f;
    fire.sequenceNumber = 3;
    Access::DeliverInput(system, 7, fire);
    EXPECT_EQ(Access::ActiveProjectiles(system), static_cast<size_t>(1));
    EXPECT_EQ(state->actionFlags, static_cast<uint32_t>(ActionFire));
}

TEST(FPSMultiplayerProduction_ProjectileHitIsValidatedAndDamagesVictim)
{
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));

    JoinAt(system, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    JoinAt(system, 2, 10.0f, 1.0f, 0.0f, 0.0f);
    JoinAt(system, 3, 0.0f, 1.0f, 10.0f, 0.0f); // bystander, off the line of fire

    PlayerInput fire;
    fire.fire = true;
    fire.yaw = 0.0f; // +X, straight at player 2
    fire.sequenceNumber = 1;
    Access::DeliverInput(system, 1, fire);
    ASSERT_EQ(Access::ActiveProjectiles(system), static_cast<size_t>(1));

    // 500 u/s covers 8.3 m per frame, far more than the 0.8 m hitbox: the swept test
    // must still register the hit instead of tunnelling through the victim.
    for (int frame = 0; frame < 30 && Access::ActiveProjectiles(system) > 0; ++frame)
        system.Update(kFrame);

    EXPECT_EQ(Access::ActiveProjectiles(system), static_cast<size_t>(0));
    EXPECT_NEAR(HealthOf(system, 2), 75.0f, 1e-4f);
    EXPECT_NEAR(HealthOf(system, 1), 100.0f, 1e-4f);
    EXPECT_NEAR(HealthOf(system, 3), 100.0f, 1e-4f);
    EXPECT_NEAR(HealthOf(system, kHostId), 100.0f, 1e-4f);
}

TEST(FPSMultiplayerProduction_DamageReportsFailClosed)
{
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));

    JoinAt(system, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    JoinAt(system, 2, 10.0f, 1.0f, 0.0f, 0.0f);

    // No lag-compensation history has been recorded yet: nothing to validate against.
    Access::DeliverDamageReport(system, 1, 2, 25.0f);
    EXPECT_NEAR(HealthOf(system, 2), 100.0f, 1e-4f);

    system.Update(kFrame);

    // Hostile amounts, self-damage and unknown attackers never touch health.
    Access::DeliverDamageReport(system, 1, 2, -50.0f);
    Access::DeliverDamageReport(system, 1, 2, 0.0f);
    Access::DeliverDamageReport(system, 1, 2, std::numeric_limits<float>::quiet_NaN());
    Access::DeliverDamageReport(system, 1, 2, std::numeric_limits<float>::infinity());
    Access::DeliverDamageReport(system, 2, 2, 25.0f);
    Access::DeliverDamageReport(system, 42, 2, 25.0f);
    EXPECT_NEAR(HealthOf(system, 2), 100.0f, 1e-4f);
    EXPECT_TRUE(FindScore(system.GetScoreboard(), 42) == nullptr);

    // A player standing in the line of fire blocks the shot.
    JoinAt(system, 3, 5.0f, 1.0f, 0.0f, 0.0f);
    system.Update(kFrame);
    Access::DeliverDamageReport(system, 1, 2, 25.0f);
    EXPECT_NEAR(HealthOf(system, 2), 100.0f, 1e-4f);
    EXPECT_NEAR(HealthOf(system, 3), 100.0f, 1e-4f);

    // With the line of fire clear the same report is validated and applied.
    Access::Leave(system, 3);
    system.Update(kFrame);
    Access::DeliverDamageReport(system, 1, 2, 25.0f);
    EXPECT_NEAR(HealthOf(system, 2), 75.0f, 1e-4f);

    // The amount is server-owned: an oversized report deals one weapon hit, not a kill.
    Access::DeliverDamageReport(system, 1, 2, 1.0e9f);
    EXPECT_NEAR(HealthOf(system, 2), 50.0f, 1e-4f);

    // Player 2 faces +X, away from player 1: a report without aim is rejected.
    Access::DeliverDamageReport(system, 2, 1, 25.0f);
    EXPECT_NEAR(HealthOf(system, 1), 100.0f, 1e-4f);

    // Turned toward player 1 the same report lands; four hits kill player 1.
    PlayerInput turn;
    turn.yaw = 3.14159265f;
    turn.sequenceNumber = 1;
    Access::DeliverInput(system, 2, turn);
    system.Update(kFrame);
    for (int hit = 0; hit < 4; ++hit)
        Access::DeliverDamageReport(system, 2, 1, 25.0f);
    ASSERT_FALSE(system.GetPlayerState(1)->isAlive);

    // A dead attacker's report is rejected even with line of fire and aim.
    system.Update(kFrame);
    Access::DeliverDamageReport(system, 1, 2, 25.0f);
    EXPECT_NEAR(HealthOf(system, 2), 50.0f, 1e-4f);
}

TEST(FPSMultiplayerProduction_ElevatedDiagonalShotClearsShooterHitbox)
{
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));

    // 45 degrees of yaw and ~35 degrees of elevation: a fixed muzzle offset along the ray
    // would still be inside the shooter's own box, which would then occlude the shot.
    JoinAt(system, 1, 0.0f, 1.0f, 0.0f, 0.7853982f);
    JoinAt(system, 2, 3.0f, 4.0f, 3.0f, 0.0f);
    system.Update(kFrame);

    Access::DeliverDamageReport(system, 1, 2, 25.0f);
    EXPECT_NEAR(HealthOf(system, 2), 75.0f, 1e-4f);
    EXPECT_NEAR(HealthOf(system, 1), 100.0f, 1e-4f);
}

TEST(FPSMultiplayerProduction_ServerRestartKeepsHitValidation)
{
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));

    // A first session long enough that its server clock runs well past the 1 s rewind window.
    JoinAt(system, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    for (int frame = 0; frame < 180; ++frame)
        system.Update(kFrame);

    // Restart without NetworkManager::Shutdown: the new session's hits must validate
    // against its own history, not be rejected against the old session's timestamps.
    system.StopServer();
    Access::SetSpawnPoints(system, {{0.0f, 1.0f, -50.0f, 0.0f}});
    ASSERT_TRUE(system.StartServer(0, 8));
    JoinAt(system, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    JoinAt(system, 2, 10.0f, 1.0f, 0.0f, 0.0f);
    system.Update(kFrame);

    Access::DeliverDamageReport(system, 1, 2, 25.0f);
    EXPECT_NEAR(HealthOf(system, 2), 75.0f, 1e-4f);
}

TEST(FPSMultiplayerProduction_KillScoresAndRespawnsAfterTimer)
{
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));

    JoinAt(system, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    JoinAt(system, 2, 10.0f, 1.0f, 0.0f, 0.0f);
    Access::SetSpawnPoints(system, {{20.0f, 1.0f, 20.0f, 1.5f}});
    system.Update(kFrame);

    // Each validated report deals one 25-point weapon hit; the fourth kills.
    for (int hit = 0; hit < 3; ++hit)
        Access::DeliverDamageReport(system, 1, 2, 25.0f);
    EXPECT_NEAR(HealthOf(system, 2), 25.0f, 1e-4f);
    EXPECT_TRUE(system.GetPlayerState(2)->isAlive);
    Access::DeliverDamageReport(system, 1, 2, 25.0f);

    const NetworkPlayerState* victim = system.GetPlayerState(2);
    ASSERT_TRUE(victim != nullptr);
    EXPECT_FALSE(victim->isAlive);
    EXPECT_NEAR(victim->health, 0.0f, 1e-6f);

    const auto board = system.GetScoreboard();
    const PlayerScore* killer = FindScore(board, 1);
    const PlayerScore* victimScore = FindScore(board, 2);
    ASSERT_TRUE(killer != nullptr);
    ASSERT_TRUE(victimScore != nullptr);
    EXPECT_EQ(killer->kills, static_cast<uint32_t>(1));
    EXPECT_EQ(killer->score, 100);
    EXPECT_EQ(killer->deaths, static_cast<uint32_t>(0));
    EXPECT_EQ(victimScore->deaths, static_cast<uint32_t>(1));
    EXPECT_EQ(victimScore->kills, static_cast<uint32_t>(0));
    EXPECT_EQ(board.front().clientId, static_cast<uint32_t>(1));

    // A dead player cannot move, and further damage neither revives nor re-kills.
    PlayerInput move;
    move.forward = 1.0f;
    move.sequenceNumber = 1;
    Access::DeliverInput(system, 2, move);
    EXPECT_NEAR(victim->posX, 10.0f, 1e-6f);
    Access::DeliverDamageReport(system, 1, 2, 60.0f);
    EXPECT_EQ(FindScore(system.GetScoreboard(), 2)->deaths, static_cast<uint32_t>(1));

    // The 5 s respawn timer runs on the server tick: still dead at 4.9 s...
    for (int frame = 0; frame < 294; ++frame)
        system.Update(kFrame);
    EXPECT_FALSE(victim->isAlive);

    // ...and back at the configured spawn point with full health by 5.1 s.
    for (int frame = 0; frame < 12; ++frame)
        system.Update(kFrame);
    EXPECT_TRUE(victim->isAlive);
    EXPECT_NEAR(victim->health, 100.0f, 1e-6f);
    EXPECT_NEAR(victim->posX, 20.0f, 1e-6f);
    EXPECT_NEAR(victim->posZ, 20.0f, 1e-6f);
    EXPECT_NEAR(victim->yaw, 1.5f, 1e-6f);
    EXPECT_EQ(FindScore(system.GetScoreboard(), 2)->deaths, static_cast<uint32_t>(1));
}

TEST(FPSMultiplayerProduction_ScoreboardOrderIsStable)
{
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));

    constexpr float kFacePlusZ = 1.5707963f;
    JoinAt(system, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    JoinAt(system, 2, 10.0f, 1.0f, 0.0f, kFacePlusZ);
    JoinAt(system, 3, 0.0f, 1.0f, 10.0f, -kFacePlusZ);
    JoinAt(system, 4, 10.0f, 1.0f, 10.0f, 0.0f);
    system.Update(kFrame);

    for (int hit = 0; hit < 4; ++hit)
    {
        Access::DeliverDamageReport(system, 3, 1, 25.0f); // 3 kills 1 along -Z
        Access::DeliverDamageReport(system, 2, 4, 25.0f); // 2 kills 4 along +Z
    }

    // score desc, then kills desc, then deaths asc, then client id asc.
    const std::vector<uint32_t> expected{2, 3, kHostId, 1, 4};
    for (int pass = 0; pass < 3; ++pass)
    {
        const auto board = system.GetScoreboard();
        ASSERT_EQ(board.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(board[i].clientId, expected[i]);
    }
}

TEST(FPSMultiplayerProduction_PlayerLeaveRemovesStateScoreAndTimer)
{
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));

    JoinAt(system, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    JoinAt(system, 2, 10.0f, 1.0f, 0.0f, 0.0f);
    system.Update(kFrame);
    for (int hit = 0; hit < 4; ++hit)
        Access::DeliverDamageReport(system, 1, 2, 25.0f);
    ASSERT_FALSE(system.GetPlayerState(2)->isAlive);

    Access::Leave(system, 2);
    EXPECT_TRUE(system.GetPlayerState(2) == nullptr);
    EXPECT_TRUE(FindScore(system.GetScoreboard(), 2) == nullptr);
    EXPECT_EQ(system.GetAllPlayerStates().size(), static_cast<size_t>(2));

    // The cleared respawn timer must not resurrect the departed player.
    for (int frame = 0; frame < 400; ++frame)
        system.Update(kFrame);
    EXPECT_TRUE(system.GetPlayerState(2) == nullptr);
    EXPECT_TRUE(system.GetPlayerState(1) != nullptr);
}

TEST(FPSMultiplayerProduction_ClientReconcilesToServerAuthority)
{
    // Phase 1: the real server simulates three inputs and produces snapshots.
    std::vector<PlayerInput> inputs(3);
    inputs[0].forward = 1.0f;
    inputs[0].yaw = 0.5f;
    inputs[1].forward = 1.0f;
    inputs[1].strafe = 0.5f;
    inputs[1].yaw = 0.7f;
    inputs[2].forward = -1.0f;
    inputs[2].yaw = 0.7f;

    NetworkPlayerState joinSnapshot;
    NetworkPlayerState ackTwoSnapshot;
    NetworkPlayerState ackThreeSnapshot;
    {
        FPSSessionGuard guard;
        auto& server = FPSMultiplayerSystem::GetInstance();
        ASSERT_TRUE(StartServer(server));
        JoinAt(server, 7, 3.0f, 1.0f, -2.0f, 0.5f);

        server.Update(kSnapshotStep);
        joinSnapshot = *server.GetPlayerState(7);

        for (uint32_t i = 0; i < 2; ++i)
        {
            PlayerInput wire = inputs[i];
            wire.sequenceNumber = i + 1;
            Access::DeliverInput(server, 7, wire);
        }
        server.Update(kSnapshotStep);
        ackTwoSnapshot = *server.GetPlayerState(7);

        PlayerInput third = inputs[2];
        third.sequenceNumber = 3;
        Access::DeliverInput(server, 7, third);
        server.Update(kSnapshotStep);
        ackThreeSnapshot = *server.GetPlayerState(7);
    }
    ASSERT_EQ(joinSnapshot.acknowledgedInputSequence, static_cast<uint32_t>(0));
    ASSERT_EQ(ackTwoSnapshot.acknowledgedInputSequence, static_cast<uint32_t>(2));
    ASSERT_EQ(ackThreeSnapshot.acknowledgedInputSequence, static_cast<uint32_t>(3));
    ASSERT_TRUE(joinSnapshot.sequenceNumber < ackTwoSnapshot.sequenceNumber);
    ASSERT_TRUE(ackTwoSnapshot.sequenceNumber < ackThreeSnapshot.sequenceNumber);

    // Phase 2: a real client connects to a loopback peer that speaks the handshake and
    // assigns a client id, then receives those snapshots.
    HandshakePeer fakeServer; // declared before the guard: its Winsock reference outlives Shutdown()
    FPSSessionGuard guard;
    ASSERT_TRUE(fakeServer.IsReady());

    auto& client = FPSMultiplayerSystem::GetInstance();
    Spark::Net::NetworkManager::GetInstance().Shutdown();
    client.Initialize(false);
    ASSERT_TRUE(client.Connect("127.0.0.1", fakeServer.Port()));
    EXPECT_FALSE(client.IsServer());
    EXPECT_EQ(client.GetLocalClientId(), Spark::Net::INVALID_CLIENT);

    // Before the handshake assigns an id, a snapshot is never taken as local authority,
    // not even one carrying the host's id 0 (which equals INVALID_CLIENT).
    NetworkPlayerState hostSnapshot = joinSnapshot;
    hostSnapshot.clientId = kHostId;
    Access::DeliverSnapshot(client, hostSnapshot);
    client.Update(kFrame);
    EXPECT_TRUE(client.GetPlayerState(kHostId) == nullptr);
    EXPECT_TRUE(client.GetAllPlayerStates().empty());

    constexpr uint32_t kAssignedId = 7;
    ASSERT_TRUE(fakeServer.AcceptClient(client, kAssignedId));
    const uint32_t localId = client.GetLocalClientId();
    ASSERT_EQ(localId, kAssignedId);
    joinSnapshot.clientId = localId;
    ackTwoSnapshot.clientId = localId;
    ackThreeSnapshot.clientId = localId;

    Access::DeliverSnapshot(client, joinSnapshot);
    client.Update(kFrame);
    const NetworkPlayerState* local = client.GetPlayerState(localId);
    ASSERT_TRUE(local != nullptr);
    EXPECT_NEAR(local->posX, 3.0f, 1e-5f);
    EXPECT_NEAR(local->posZ, -2.0f, 1e-5f);
    const uint32_t correctionsAfterSpawn = client.GetDebugMetrics().correctionCount;

    // Client prediction runs the same movement as the server: after the same three
    // inputs it stands exactly where the server put the player.
    for (const auto& input : inputs)
        client.SendInput(input);
    EXPECT_EQ(Access::LastSentInput(client).sequenceNumber, static_cast<uint32_t>(3));
    EXPECT_NEAR(local->posX, ackThreeSnapshot.posX, 1e-4f);
    EXPECT_NEAR(local->posZ, ackThreeSnapshot.posZ, 1e-4f);

    // A snapshot acknowledging input 2 replays input 3 and lands on the same spot: no
    // correction. Repeated frames without a new snapshot must not replay again.
    Access::DeliverSnapshot(client, ackTwoSnapshot);
    for (int frame = 0; frame < 5; ++frame)
        client.Update(kFrame);
    EXPECT_NEAR(local->posX, ackThreeSnapshot.posX, 1e-4f);
    EXPECT_NEAR(local->posZ, ackThreeSnapshot.posZ, 1e-4f);
    EXPECT_EQ(client.GetDebugMetrics().correctionCount, correctionsAfterSpawn);

    // A stale snapshot is ignored.
    Access::DeliverSnapshot(client, joinSnapshot);
    client.Update(kFrame);
    EXPECT_NEAR(local->posX, ackThreeSnapshot.posX, 1e-4f);

    // A diverging authoritative snapshot wins: position, health and life come from
    // the server and the correction is counted.
    NetworkPlayerState corrected = ackThreeSnapshot;
    corrected.sequenceNumber = ackThreeSnapshot.sequenceNumber + 1;
    corrected.posX += 2.0f;
    corrected.health = 40.0f;
    Access::DeliverSnapshot(client, corrected);
    client.Update(kFrame);
    EXPECT_NEAR(local->posX, corrected.posX, 1e-4f);
    EXPECT_NEAR(local->posZ, corrected.posZ, 1e-4f);
    EXPECT_NEAR(local->health, 40.0f, 1e-6f);
    EXPECT_EQ(client.GetDebugMetrics().correctionCount, correctionsAfterSpawn + 1);
}
