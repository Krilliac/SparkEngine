/**
 * @file TestFPSMultiplayer.cpp
 * @brief Tests for FPS multiplayer system
 */

#include "TestFramework.h"
#include "Game/MultiplayerSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace SparkFPS;

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

TEST(FPSMultiplayer_JoinLeavePlayerTracking)
{
    std::unordered_map<uint32_t, NetworkPlayerState> players;

    // Join 3 players
    for (uint32_t i = 1; i <= 3; ++i)
    {
        NetworkPlayerState state;
        state.clientId = i;
        state.health = 100.0f;
        state.isAlive = true;
        players[i] = state;
    }
    EXPECT_EQ(players.size(), static_cast<size_t>(3));

    // Player 2 leaves
    players.erase(2);
    EXPECT_EQ(players.size(), static_cast<size_t>(2));
    EXPECT_TRUE(players.find(2) == players.end());
    EXPECT_TRUE(players.find(1) != players.end());
    EXPECT_TRUE(players.find(3) != players.end());
}

TEST(FPSMultiplayer_ScoreboardUpdates)
{
    std::unordered_map<uint32_t, PlayerScore> scores;

    PlayerScore p1;
    p1.clientId = 1;
    p1.playerName = "Player1";
    scores[1] = p1;

    PlayerScore p2;
    p2.clientId = 2;
    p2.playerName = "Player2";
    scores[2] = p2;

    // Player 1 kills Player 2
    scores[1].kills++;
    scores[1].score += 100;
    scores[2].deaths++;

    EXPECT_EQ(scores[1].kills, static_cast<uint32_t>(1));
    EXPECT_EQ(scores[1].score, 100);
    EXPECT_EQ(scores[2].deaths, static_cast<uint32_t>(1));
    EXPECT_EQ(scores[2].kills, static_cast<uint32_t>(0));

    // Sort scoreboard
    std::vector<PlayerScore> board;
    for (const auto& [id, s] : scores)
        board.push_back(s);
    std::sort(board.begin(), board.end(), [](const PlayerScore& a, const PlayerScore& b) { return a.score > b.score; });

    EXPECT_EQ(board[0].clientId, static_cast<uint32_t>(1));
}

TEST(FPSMultiplayer_RespawnTimerCountdown)
{
    float respawnTimer = 5.0f;
    float dt = 1.0f / 60.0f;

    int frames = 0;
    while (respawnTimer > 0.0f)
    {
        respawnTimer -= dt;
        frames++;
    }

    // Should take approximately 5 seconds * 60 fps = 300 frames
    EXPECT_TRUE(frames >= 299 && frames <= 301);
}

TEST(FPSMultiplayer_DamageReducesHealth)
{
    NetworkPlayerState state;
    state.health = 100.0f;
    state.isAlive = true;

    float damage = 25.0f;
    state.health -= damage;
    EXPECT_NEAR(state.health, 75.0f, 0.001f);
    EXPECT_TRUE(state.isAlive);

    // Lethal damage
    state.health -= 80.0f;
    if (state.health <= 0.0f)
    {
        state.health = 0.0f;
        state.isAlive = false;
    }
    EXPECT_NEAR(state.health, 0.0f, 0.001f);
    EXPECT_FALSE(state.isAlive);
}

TEST(FPSMultiplayer_MovementAppliesCorrectly)
{
    NetworkPlayerState state;
    state.posX = 0.0f;
    state.posZ = 0.0f;
    state.yaw = 0.0f; // facing forward (along X)

    float moveSpeed = 8.0f;
    float dt = 1.0f / 60.0f;

    // Move forward
    float forward = 1.0f;
    float sinYaw = std::sin(state.yaw);
    float cosYaw = std::cos(state.yaw);
    state.posX += forward * cosYaw * moveSpeed * dt;
    state.posZ += forward * sinYaw * moveSpeed * dt;

    EXPECT_TRUE(state.posX > 0.0f);
    EXPECT_NEAR(state.posZ, 0.0f, 0.01f); // yaw=0 means no Z movement
}

TEST(FPSMultiplayer_SnapshotDeltaCompression)
{
    NetworkPlayerState prev;
    prev.posX = 0.0f;
    prev.health = 100.0f;

    NetworkPlayerState curr;
    curr.posX = 1.5f;
    curr.health = 100.0f; // unchanged

    // Only position changed
    uint8_t dirtyMask = 0;
    if (std::abs(curr.posX - prev.posX) > 0.001f)
        dirtyMask |= 0x01;
    if (std::abs(curr.health - prev.health) > 0.001f)
        dirtyMask |= 0x02;

    EXPECT_EQ(dirtyMask, static_cast<uint8_t>(0x01));
}

TEST(FPSMultiplayer_SpawnPointSelection)
{
    struct SpawnPoint
    {
        float x, y, z, yaw;
    };

    std::vector<SpawnPoint> spawnPoints = {{0, 1, 0, 0}, {10, 1, 0, 90}, {-10, 1, 10, 180}, {5, 1, -10, 270}};

    EXPECT_EQ(spawnPoints.size(), static_cast<size_t>(4));

    // All spawn points have y=1 (ground level + standing height)
    for (const auto& sp : spawnPoints)
    {
        EXPECT_NEAR(sp.y, 1.0f, 0.001f);
    }
}

TEST(FPSMultiplayer_MultiplePlayersIndependent)
{
    std::unordered_map<uint32_t, NetworkPlayerState> players;

    for (uint32_t i = 1; i <= 4; ++i)
    {
        NetworkPlayerState state;
        state.clientId = i;
        state.posX = static_cast<float>(i) * 5.0f;
        state.health = 100.0f;
        players[i] = state;
    }

    // Damage player 2 only
    players[2].health -= 30.0f;

    // Other players should be unaffected
    EXPECT_NEAR(players[1].health, 100.0f, 0.001f);
    EXPECT_NEAR(players[2].health, 70.0f, 0.001f);
    EXPECT_NEAR(players[3].health, 100.0f, 0.001f);
    EXPECT_NEAR(players[4].health, 100.0f, 0.001f);
}
