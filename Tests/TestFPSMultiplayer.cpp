/**
 * @file TestFPSMultiplayer.cpp
 * @brief FPS multiplayer wire-format tests and production-class FPSMultiplayerSystem tests
 *
 * FPSMultiplayer_* covers the NetworkPlayerState/PlayerInput wire encoding.
 * FPSMultiplayerProduction_* drives the real SparkFPS::FPSMultiplayerSystem (compiled into
 * SparkTests from GameModules/SparkGameFPS/Source/Game/MultiplayerSystem.cpp) against the
 * real NetworkManager singleton: a loopback server on an ephemeral port for the server-side
 * tests, and a client that completes the handshake with a loopback peer for reconciliation. Game
 * rules are driven through FPSMultiplayerSystemTestAccess; FPSMultiplayerProduction_NetworkPath*
 * instead exchange real datagrams with a raw loopback peer, so every message crosses
 * NetworkManager's socket, packet validator and handler dispatch.
 */

#include "TestFramework.h"
#include "Game/MultiplayerSystem.h"
#include "Engine/Networking/NetworkManager.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
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

        static uint32_t LastAppliedSequence(const FPSMultiplayerSystem& system, uint32_t clientId)
        {
            const auto it = system.m_lastInputByPlayer.find(clientId);
            return it != system.m_lastInputByPlayer.end() ? it->second.sequenceNumber : 0;
        }

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

    /// Pump @p system one frame at a time until @p done holds. Returns false after 2 s.
    template <typename Condition> bool PumpUntil(FPSMultiplayerSystem& system, Condition done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            system.Update(kFrame);
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    /// One datagram as a loopback peer received it.
    struct WireMessage
    {
        Spark::Net::MessageType type = Spark::Net::MessageType::UserDefined;
        uint32_t sequence = 0;
        std::vector<uint8_t> payload;
    };

    /// Loopback UDP peer that speaks the documented wire format
    /// (docs/specs/networking-wire-format.md): it stands in for an FPS server facing a real
    /// client, or for a remote client facing a real server. It holds its own Winsock reference
    /// on Windows so the NetworkManager::Shutdown() in FPSSessionGuard never pulls Winsock
    /// from under it.
    class LoopbackPeer
    {
      public:
        LoopbackPeer()
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

        ~LoopbackPeer()
        {
            if (m_socket != INVALID_SOCKET)
                closesocket(m_socket);
#ifdef SPARK_PLATFORM_WINDOWS
            if (m_winsockStarted)
                WSACleanup();
#endif
        }

        LoopbackPeer(const LoopbackPeer&) = delete;
        LoopbackPeer& operator=(const LoopbackPeer&) = delete;

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

        /// Address of the client accepted by AcceptClient, or the server set by SetRemotePort.
        void SetRemotePort(uint16_t port)
        {
            m_remote = {};
            m_remote.sin_family = AF_INET;
            m_remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            m_remote.sin_port = htons(port);
        }

        bool Send(Spark::Net::MessageType type, Spark::Net::ChannelType channel, uint32_t sequence,
                  const std::vector<uint8_t>& body) const
        {
            Spark::Net::NetBuffer wire;
            wire.WriteUint32(kWirePacketMagic);
            wire.WriteUint16(static_cast<uint16_t>(type));
            wire.WriteUint8(static_cast<uint8_t>(channel));
            wire.WriteUint32(Spark::Net::INVALID_CLIENT); // receivers never trust the wire sender
            wire.WriteUint32(sequence);
            wire.WriteFloat(0.0f); // timestamp
            wire.WriteUint32(static_cast<uint32_t>(body.size()));
            if (!body.empty())
                wire.WriteBytes(body.data(), body.size());
            const auto& datagram = wire.GetData();
            const int sent =
                sendto(m_socket, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                       reinterpret_cast<const sockaddr*>(&m_remote), sizeof(m_remote));
            return sent == static_cast<int>(datagram.size());
        }

        bool Send(SparkFPS::FPSMessageType type, const std::vector<uint8_t>& body) const
        {
            return Send(static_cast<Spark::Net::MessageType>(type), Spark::Net::ChannelType::Unreliable, 0, body);
        }

        /// Read one pending datagram; false when none is waiting or it is not a Spark packet.
        bool Receive(WireMessage& out, sockaddr_in* outSender = nullptr) const
        {
            std::array<uint8_t, 65536> buffer{};
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
            out.type = static_cast<Spark::Net::MessageType>(wire.ReadUint16());
            wire.ReadUint8();  // channel
            wire.ReadUint32(); // sender
            out.sequence = wire.ReadUint32();
            wire.ReadFloat(); // timestamp
            const uint32_t length = wire.ReadUint32();
            if (wire.HasError() || magic != kWirePacketMagic || length != wire.RemainingBytes())
                return false;
            out.payload.resize(length);
            if (length > 0)
                wire.ReadBytes(out.payload.data(), length);
            if (outSender)
                *outSender = from;
            if (wire.HasError())
                return false;
            if (out.type == static_cast<Spark::Net::MessageType>(SparkFPS::FPSMessageType::PlayerInput))
                ++m_inputsReceived;
            return true;
        }

        /// FPS PlayerInput datagrams Receive has read so far.
        size_t InputsReceived() const { return m_inputsReceived; }

        /// Pump @p client until its Connect arrives, accept it as @p assignedId, then pump
        /// until the client has adopted that id. Returns false on timeout.
        bool AcceptClient(FPSMultiplayerSystem& client, uint32_t assignedId)
        {
            WireMessage connect;
            sockaddr_in clientAddress{};
            const bool gotConnect = PumpUntil(
                client,
                [&] { return Receive(connect, &clientAddress) && connect.type == Spark::Net::MessageType::Connect; });
            if (!gotConnect)
                return false;
            m_remote = clientAddress;

            Spark::Net::NetBuffer payload;
            payload.WriteUint32(assignedId);
            payload.WriteFloat(0.0f); // server time
            payload.WriteUint16(Spark::Net::NETWORK_PROTOCOL_VERSION);
            if (!Send(Spark::Net::MessageType::ConnectAccepted, Spark::Net::ChannelType::Reliable, 1,
                      payload.GetData()))
                return false;

            return PumpUntil(client, [&] { return client.GetLocalClientId() == assignedId; });
        }

      private:
        static constexpr uint32_t kWirePacketMagic = 0x5350524B; // "SPRK"

        SOCKET m_socket = INVALID_SOCKET;
        sockaddr_in m_remote{};
        mutable size_t m_inputsReceived = 0;
        bool m_ready = false;
#ifdef SPARK_PLATFORM_WINDOWS
        bool m_winsockStarted = false;
#endif
    };

    /// A StateSnapshot payload in the documented layout (FPSMessageType::StateSnapshot).
    std::vector<uint8_t> BuildSnapshotBatch(uint32_t batch,
                                            const std::vector<std::pair<NetworkPlayerState, PlayerScore>>& players,
                                            uint16_t declaredCount)
    {
        std::vector<uint8_t> bytes;
        Detail::WriteU32(bytes, batch);
        bytes.push_back(static_cast<uint8_t>(declaredCount));
        bytes.push_back(static_cast<uint8_t>(declaredCount >> 8));
        for (const auto& [state, score] : players)
        {
            NetworkPlayerState stamped = state;
            stamped.sequenceNumber = batch;
            const auto record = stamped.Serialize();
            bytes.insert(bytes.end(), record.begin(), record.end());
            Detail::WriteU32(bytes, score.kills);
            Detail::WriteU32(bytes, score.deaths);
            Detail::WriteU32(bytes, score.assists);
            Detail::WriteU32(bytes, static_cast<uint32_t>(score.score));
        }
        return bytes;
    }

    std::vector<uint8_t> BuildSnapshotBatch(uint32_t batch,
                                            const std::vector<std::pair<NetworkPlayerState, PlayerScore>>& players)
    {
        return BuildSnapshotBatch(batch, players, static_cast<uint16_t>(players.size()));
    }

    /// One player's record from a StateSnapshot payload, or nullopt when absent or malformed.
    std::optional<NetworkPlayerState> FindInSnapshot(const std::vector<uint8_t>& payload, uint32_t clientId,
                                                     uint32_t* outKills = nullptr)
    {
        constexpr size_t kHeader = 6;
        constexpr size_t kRecord = NetworkPlayerState::SerializedSize + 16;
        if (payload.size() < kHeader)
            return std::nullopt;
        const size_t count = static_cast<size_t>(payload[4]) | (static_cast<size_t>(payload[5]) << 8);
        if (payload.size() != kHeader + count * kRecord)
            return std::nullopt;
        for (size_t index = 0; index < count; ++index)
        {
            const uint8_t* record = payload.data() + kHeader + index * kRecord;
            const auto state = NetworkPlayerState::Deserialize(record, NetworkPlayerState::SerializedSize);
            if (state.clientId != clientId)
                continue;
            if (outKills)
            {
                size_t offset = NetworkPlayerState::SerializedSize;
                *outKills = Detail::ReadU32(record, offset);
            }
            return state;
        }
        return std::nullopt;
    }
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

TEST(FPSMultiplayerProduction_ApplyClientInputRejectsHostileInput)
{
    // Hostile values are rejected at ApplyClientInput itself, so they cannot reach the
    // authoritative state through any input path: the direct handler seam here and the
    // listen-server host below, not only the wire decoder.
    FPSSessionGuard guard;
    auto& system = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(system));
    JoinAt(system, 7, 0.0f, 1.0f, 0.0f, 0.0f);
    const NetworkPlayerState* state = system.GetPlayerState(7);
    ASSERT_TRUE(state != nullptr);

    PlayerInput step;
    step.forward = 1.0f;
    step.sequenceNumber = 1;
    Access::DeliverInput(system, 7, step);
    const float settledX = state->posX;
    ASSERT_TRUE(std::abs(settledX - 8.0f * kFrame) < 1e-5f);

    // Every non-finite field is rejected whole, fire included: nothing moves, turns or
    // spawns, and the sequence is not consumed.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const std::array<std::array<float, 4>, 5> poisoned{{
        {nan, 0.0f, 0.0f, 0.0f},
        {1.0f, -inf, 0.0f, 0.0f},
        {1.0f, 0.0f, nan, 0.0f},
        {1.0f, 0.0f, inf, 0.0f},
        {1.0f, 0.0f, 0.0f, nan},
    }};
    for (const auto& fields : poisoned)
    {
        PlayerInput hostile;
        hostile.forward = fields[0];
        hostile.strafe = fields[1];
        hostile.yaw = fields[2];
        hostile.pitch = fields[3];
        hostile.fire = true;
        hostile.sequenceNumber = 2;
        Access::DeliverInput(system, 7, hostile);
    }
    EXPECT_NEAR(state->posX, settledX, 1e-6f);
    EXPECT_NEAR(state->posZ, 0.0f, 1e-6f);
    EXPECT_TRUE(std::isfinite(state->yaw) && std::isfinite(state->pitch));
    EXPECT_EQ(Access::ActiveProjectiles(system), static_cast<size_t>(0));
    EXPECT_EQ(Access::LastAppliedSequence(system, 7), static_cast<uint32_t>(1));

    // A replayed sequence and a sequence of 0 change nothing.
    PlayerInput replay = step;
    Access::DeliverInput(system, 7, replay);
    PlayerInput zeroSequence = step;
    zeroSequence.sequenceNumber = 0;
    Access::DeliverInput(system, 7, zeroSequence);
    EXPECT_NEAR(state->posX, settledX, 1e-6f);
    EXPECT_EQ(Access::LastAppliedSequence(system, 7), static_cast<uint32_t>(1));

    // Oversized axes are clamped to one full-speed step, and an out-of-range look is
    // bounded: pitch stops at straight up, yaw wraps into [-pi, pi] as the same heading.
    PlayerInput boosted;
    boosted.forward = 1000.0f;
    boosted.strafe = -1000.0f;
    boosted.yaw = 1.0f + 2.0f * 3.14159265f * 1000.0f;
    boosted.pitch = 50.0f;
    boosted.sequenceNumber = 2;
    Access::DeliverInput(system, 7, boosted);
    EXPECT_NEAR(state->posX, settledX + 8.0f * kFrame, 1e-5f);
    EXPECT_NEAR(state->posZ, 8.0f * kFrame, 1e-5f); // strafe -1 at yaw 0 moves along +Z
    EXPECT_NEAR(state->pitch, 0.5f * 3.14159265f, 1e-5f);
    EXPECT_TRUE(std::abs(state->yaw) <= 3.14159265f + 1e-6f);
    EXPECT_NEAR(std::cos(state->yaw), std::cos(1.0f), 2e-3f);
    EXPECT_NEAR(std::sin(state->yaw), std::sin(1.0f), 2e-3f);

    // An honest atan2 yaw at the boundary is applied exactly: the wrap never perturbs it.
    PlayerInput boundary;
    boundary.yaw = -3.14159265f;
    boundary.sequenceNumber = 3;
    Access::DeliverInput(system, 7, boundary);
    EXPECT_EQ(state->yaw, -3.14159265f);

    // The listen-server host's own input goes through the same gate.
    const NetworkPlayerState* host = system.GetPlayerState(kHostId);
    ASSERT_TRUE(host != nullptr);
    const float hostX = host->posX;
    const float hostZ = host->posZ;
    PlayerInput hostPoisoned;
    hostPoisoned.forward = 1.0f;
    hostPoisoned.yaw = nan;
    hostPoisoned.fire = true;
    system.SendInput(hostPoisoned);
    EXPECT_NEAR(host->posX, hostX, 1e-6f);
    EXPECT_NEAR(host->posZ, hostZ, 1e-6f);
    EXPECT_TRUE(std::isfinite(host->yaw));
    EXPECT_EQ(Access::ActiveProjectiles(system), static_cast<size_t>(0));
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
    LoopbackPeer fakeServer; // declared before the guard: its Winsock reference outlives Shutdown()
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

TEST(FPSMultiplayerProduction_NetworkPathServerAdmitsInputAndBroadcastsSnapshots)
{
    LoopbackPeer remote; // declared before the guard: its Winsock reference outlives Shutdown()
    FPSSessionGuard guard;
    ASSERT_TRUE(remote.IsReady());

    auto& server = FPSMultiplayerSystem::GetInstance();
    ASSERT_TRUE(StartServer(server));
    Access::SetSpawnPoints(server, {{0.0f, 1.0f, 0.0f, 0.0f}});
    remote.SetRemotePort(Spark::Net::NetworkManager::GetInstance().GetBoundPort());

    // Admission through NetworkManager's handshake spawns the player and its score row.
    Spark::Net::NetBuffer connect;
    Spark::Net::WriteConnectRequest(connect, "RawPeer");
    ASSERT_TRUE(remote.Send(Spark::Net::MessageType::Connect, Spark::Net::ChannelType::Reliable, 1, connect.GetData()));
    ASSERT_TRUE(PumpUntil(server, [&] { return server.GetAllPlayerStates().size() == 2; }));
    uint32_t remoteId = Spark::Net::INVALID_CLIENT;
    for (const auto& [id, state] : server.GetAllPlayerStates())
    {
        if (id != kHostId)
            remoteId = id;
    }
    ASSERT_TRUE(remoteId != Spark::Net::INVALID_CLIENT);
    ASSERT_TRUE(FindScore(server.GetScoreboard(), remoteId) != nullptr);
    const NetworkPlayerState* state = server.GetPlayerState(remoteId);
    ASSERT_TRUE(state != nullptr);

    // A PlayerInput message moves the sender's player, identified by endpoint, not by payload.
    PlayerInput forward;
    forward.forward = 1.0f;
    forward.sequenceNumber = 1;
    ASSERT_TRUE(remote.Send(FPSMessageType::PlayerInput, forward.Serialize()));
    ASSERT_TRUE(PumpUntil(server, [&] { return state->posX > 0.0f; }));
    EXPECT_NEAR(state->posX, 8.0f * kFrame, 1e-5f);

    // The 20 Hz snapshot reaches the peer over UDP and acknowledges input 1.
    bool acknowledged = false;
    ASSERT_TRUE(PumpUntil(server,
                          [&]
                          {
                              WireMessage message;
                              while (remote.Receive(message))
                              {
                                  if (message.type !=
                                      static_cast<Spark::Net::MessageType>(FPSMessageType::StateSnapshot))
                                      continue;
                                  const auto own = FindInSnapshot(message.payload, remoteId);
                                  const auto host = FindInSnapshot(message.payload, kHostId);
                                  acknowledged = own && host && own->acknowledgedInputSequence == 1 &&
                                                 std::abs(own->posX - 8.0f * kFrame) < 1e-5f;
                                  if (acknowledged)
                                      return true;
                              }
                              return false;
                          }));
    EXPECT_TRUE(acknowledged);

    // Hostile inputs change nothing: a replayed sequence, a non-finite axis, a truncated
    // payload, and a sequence of 0.
    const float settledX = state->posX;
    PlayerInput replay = forward;
    PlayerInput nonFinite = forward;
    nonFinite.forward = std::numeric_limits<float>::quiet_NaN();
    nonFinite.sequenceNumber = 2;
    PlayerInput zeroSequence = forward;
    zeroSequence.sequenceNumber = 0;
    std::vector<uint8_t> truncated = forward.Serialize();
    truncated.pop_back();
    ASSERT_TRUE(remote.Send(FPSMessageType::PlayerInput, replay.Serialize()));
    ASSERT_TRUE(remote.Send(FPSMessageType::PlayerInput, nonFinite.Serialize()));
    ASSERT_TRUE(remote.Send(FPSMessageType::PlayerInput, zeroSequence.Serialize()));
    ASSERT_TRUE(remote.Send(FPSMessageType::PlayerInput, truncated));
    for (int frame = 0; frame < 10; ++frame)
        server.Update(kFrame);
    EXPECT_NEAR(state->posX, settledX, 1e-6f);

    // An out-of-range axis is clamped to one full-speed step.
    PlayerInput boosted;
    boosted.forward = 1000.0f;
    boosted.sequenceNumber = 3;
    ASSERT_TRUE(remote.Send(FPSMessageType::PlayerInput, boosted.Serialize()));
    ASSERT_TRUE(PumpUntil(server, [&] { return state->posX > settledX; }));
    EXPECT_NEAR(state->posX, settledX + 8.0f * kFrame, 1e-5f);

    // A flood of inputs cannot outrun the server clock: the 0.25 s budget admits at most
    // 15 steps of the 100 sent in one burst.
    for (int frame = 0; frame < 30; ++frame)
        server.Update(kFrame); // refill the budget to its cap
    const float beforeFlood = state->posX;
    for (uint32_t sequence = 10; sequence < 110; ++sequence)
    {
        PlayerInput flood = forward;
        flood.sequenceNumber = sequence;
        ASSERT_TRUE(remote.Send(FPSMessageType::PlayerInput, flood.Serialize()));
    }
    for (int frame = 0; frame < 5; ++frame)
        server.Update(kFrame);
    const float floodSteps = (state->posX - beforeFlood) / (8.0f * kFrame);
    EXPECT_GE(floodSteps, 1.0f - 1e-3f);
    EXPECT_LE(floodSteps, 15.0f + 1e-3f);

    // Held fire at 60 Hz for one second spawns at most the server's 600 RPM: one projectile
    // every 6 applied inputs, 10 in all, however many fire inputs arrive.
    for (int frame = 0; frame < 30; ++frame)
        server.Update(kFrame);
    ASSERT_EQ(Access::ActiveProjectiles(server), static_cast<size_t>(0));
    constexpr uint32_t kFirstFireSequence = 200;
    constexpr uint32_t kFireInputs = 60;
    for (uint32_t index = 0; index < kFireInputs; ++index)
    {
        PlayerInput held;
        held.fire = true;
        held.yaw = 0.0f; // +X, away from the host at the origin
        held.sequenceNumber = kFirstFireSequence + index;
        ASSERT_TRUE(remote.Send(FPSMessageType::PlayerInput, held.Serialize()));
        server.Update(kFrame);
    }
    ASSERT_TRUE(PumpUntil(
        server, [&] { return Access::LastAppliedSequence(server, remoteId) == kFirstFireSequence + kFireInputs - 1; }));
    EXPECT_EQ(Access::ActiveProjectiles(server), static_cast<size_t>(10));

    // Disconnect through NetworkManager removes the player and its score row.
    ASSERT_TRUE(remote.Send(Spark::Net::MessageType::Disconnect, Spark::Net::ChannelType::Reliable, 2, {}));
    ASSERT_TRUE(PumpUntil(server, [&] { return server.GetPlayerState(remoteId) == nullptr; }));
    EXPECT_TRUE(FindScore(server.GetScoreboard(), remoteId) == nullptr);
    EXPECT_TRUE(server.GetPlayerState(kHostId) != nullptr);
}

TEST(FPSMultiplayerProduction_NetworkPathClientAppliesSnapshotBatches)
{
    LoopbackPeer fakeServer; // declared before the guard: its Winsock reference outlives Shutdown()
    FPSSessionGuard guard;
    ASSERT_TRUE(fakeServer.IsReady());

    auto& client = FPSMultiplayerSystem::GetInstance();
    Spark::Net::NetworkManager::GetInstance().Shutdown();
    client.Initialize(false);
    ASSERT_TRUE(client.Connect("127.0.0.1", fakeServer.Port()));

    // No input leaves the client before the handshake assigns its id: nothing is recorded,
    // and no PlayerInput datagram reaches the server by the time the handshake completes.
    PlayerInput early;
    early.forward = 1.0f;
    client.SendInput(early);
    EXPECT_EQ(Access::LastSentInput(client).sequenceNumber, static_cast<uint32_t>(0));

    constexpr uint32_t kLocalId = 7;
    constexpr uint32_t kRemoteId = 9;
    ASSERT_TRUE(fakeServer.AcceptClient(client, kLocalId));
    {
        WireMessage pending;
        while (fakeServer.Receive(pending))
        {
        }
    }
    EXPECT_EQ(fakeServer.InputsReceived(), static_cast<size_t>(0));

    // Local input travels as an FPS PlayerInput message carrying the prediction sequence.
    PlayerInput step;
    step.forward = 1.0f;
    client.SendInput(step);
    bool inputSeen = false;
    ASSERT_TRUE(PumpUntil(
        client,
        [&]
        {
            WireMessage message;
            while (fakeServer.Receive(message))
            {
                if (message.type == static_cast<Spark::Net::MessageType>(FPSMessageType::PlayerInput) &&
                    message.payload.size() == PlayerInput::SerializedSize)
                {
                    const auto sent = PlayerInput::Deserialize(message.payload.data(), message.payload.size());
                    inputSeen = sent.sequenceNumber == 1 && sent.forward == 1.0f;
                }
            }
            return inputSeen;
        }));

    NetworkPlayerState local;
    local.clientId = kLocalId;
    local.posX = 3.0f;
    local.posY = 1.0f;
    local.posZ = -2.0f;
    local.acknowledgedInputSequence = 1;
    NetworkPlayerState remote;
    remote.clientId = kRemoteId;
    remote.posX = 10.0f;
    remote.posY = 1.0f;
    PlayerScore remoteScore;
    remoteScore.kills = 2;
    remoteScore.score = 200;
    PlayerScore localScore;
    localScore.deaths = 1;

    // An accepted batch places both players and replaces the scoreboard.
    ASSERT_TRUE(fakeServer.Send(FPSMessageType::StateSnapshot,
                                BuildSnapshotBatch(1, {{local, localScore}, {remote, remoteScore}})));
    ASSERT_TRUE(PumpUntil(client, [&] { return client.GetPlayerState(kRemoteId) != nullptr; }));
    ASSERT_TRUE(PumpUntil(client, [&] { return std::abs(client.GetPlayerState(kLocalId)->posX - 3.0f) < 1e-4f; }));
    EXPECT_NEAR(client.GetPlayerState(kLocalId)->posZ, -2.0f, 1e-4f);
    EXPECT_NEAR(client.GetPlayerState(kRemoteId)->posX, 10.0f, 1e-4f);
    ASSERT_TRUE(FindScore(client.GetScoreboard(), kRemoteId) != nullptr);
    EXPECT_EQ(FindScore(client.GetScoreboard(), kRemoteId)->kills, static_cast<uint32_t>(2));
    EXPECT_EQ(FindScore(client.GetScoreboard(), kRemoteId)->score, 200);
    EXPECT_EQ(FindScore(client.GetScoreboard(), kLocalId)->deaths, static_cast<uint32_t>(1));

    // Malformed, non-finite and stale batches are dropped whole.
    const uint32_t packetsBeforeHostile = Spark::Net::NetworkManager::GetInstance().GetStats().packetsReceived;
    PlayerScore forged = remoteScore;
    forged.kills = 50;
    NetworkPlayerState poisoned = remote;
    poisoned.posX = std::numeric_limits<float>::infinity();
    ASSERT_TRUE(fakeServer.Send(FPSMessageType::StateSnapshot,
                                BuildSnapshotBatch(2, {{local, localScore}, {remote, forged}}, 3)));
    ASSERT_TRUE(fakeServer.Send(FPSMessageType::StateSnapshot,
                                BuildSnapshotBatch(3, {{local, localScore}, {poisoned, forged}})));
    ASSERT_TRUE(fakeServer.Send(FPSMessageType::StateSnapshot, {0x01, 0x02}));
    std::vector<uint8_t> oversized = BuildSnapshotBatch(4, {}, static_cast<uint16_t>(kMaxPlayers + 1));
    oversized.resize(oversized.size() + (kMaxPlayers + 1) * (NetworkPlayerState::SerializedSize + 16));
    ASSERT_TRUE(fakeServer.Send(FPSMessageType::StateSnapshot, oversized));
    // All four reach the handler before these checks: NetworkManager counts each dispatched datagram.
    ASSERT_TRUE(PumpUntil(
        client, [&]
        { return Spark::Net::NetworkManager::GetInstance().GetStats().packetsReceived >= packetsBeforeHostile + 4; }));
    EXPECT_EQ(FindScore(client.GetScoreboard(), kRemoteId)->kills, static_cast<uint32_t>(2));
    EXPECT_NEAR(client.GetPlayerState(kRemoteId)->posX, 10.0f, 1e-3f);

    // A newer batch accepts; replaying an older batch number afterwards does nothing.
    PlayerScore updated = remoteScore;
    updated.kills = 3;
    ASSERT_TRUE(fakeServer.Send(FPSMessageType::StateSnapshot,
                                BuildSnapshotBatch(5, {{local, localScore}, {remote, updated}})));
    ASSERT_TRUE(PumpUntil(client, [&] { return FindScore(client.GetScoreboard(), kRemoteId)->kills == 3; }));
    EXPECT_TRUE(std::isfinite(client.GetPlayerState(kRemoteId)->posX));
    EXPECT_NEAR(client.GetPlayerState(kRemoteId)->posX, 10.0f, 1e-3f);
    ASSERT_TRUE(
        fakeServer.Send(FPSMessageType::StateSnapshot, BuildSnapshotBatch(4, {{local, localScore}, {remote, forged}})));
    for (int frame = 0; frame < 10; ++frame)
        client.Update(kFrame);
    EXPECT_EQ(FindScore(client.GetScoreboard(), kRemoteId)->kills, static_cast<uint32_t>(3));

    // A remote player missing from an accepted batch has left the session.
    ASSERT_TRUE(fakeServer.Send(FPSMessageType::StateSnapshot, BuildSnapshotBatch(6, {{local, localScore}})));
    ASSERT_TRUE(PumpUntil(client, [&] { return client.GetPlayerState(kRemoteId) == nullptr; }));
    EXPECT_TRUE(FindScore(client.GetScoreboard(), kRemoteId) == nullptr);
    for (int frame = 0; frame < 5; ++frame)
        client.Update(kFrame);
    EXPECT_TRUE(client.GetPlayerState(kRemoteId) == nullptr);
    EXPECT_TRUE(client.GetPlayerState(kLocalId) != nullptr);

    // The server closing the session ends it on the client.
    ASSERT_TRUE(fakeServer.Send(Spark::Net::MessageType::Disconnect, Spark::Net::ChannelType::Reliable, 2, {}));
    ASSERT_TRUE(PumpUntil(client, [&] { return !client.IsActive(); }));
    EXPECT_TRUE(Spark::Net::NetworkManager::GetInstance().GetRole() == Spark::Net::NetworkRole::None);
}
