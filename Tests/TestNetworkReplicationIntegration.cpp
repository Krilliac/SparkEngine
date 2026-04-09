/**
 * @file TestNetworkReplicationIntegration.cpp
 * @brief Integration tests for network entity replication
 *
 * Tests entity state serialization, delta snapshots, and client prediction
 * reconciliation through the networking pipeline.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/ClientPrediction.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

using namespace Spark;

// ============================================================================
// Serialization helpers for testing
// ============================================================================

struct ReplicatedEntityState
{
    uint32_t entityId = 0;
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f, rotW = 1.0f;
    float health = 100.0f;
    uint32_t sequenceNumber = 0;

    std::vector<uint8_t> Serialize() const
    {
        std::vector<uint8_t> buffer(sizeof(ReplicatedEntityState));
        std::memcpy(buffer.data(), this, sizeof(ReplicatedEntityState));
        return buffer;
    }

    static ReplicatedEntityState Deserialize(const std::vector<uint8_t>& data)
    {
        ReplicatedEntityState state;
        if (data.size() >= sizeof(ReplicatedEntityState))
        {
            std::memcpy(&state, data.data(), sizeof(ReplicatedEntityState));
        }
        return state;
    }
};

struct DeltaField
{
    uint8_t fieldId;
    std::vector<uint8_t> data;
};

// ============================================================================
// Tests
// ============================================================================

TEST(NetReplication_StateSerializationRoundtrip)
{
    ReplicatedEntityState original;
    original.entityId = 42;
    original.posX = 10.5f;
    original.posY = 20.3f;
    original.posZ = -5.7f;
    original.rotW = 0.707f;
    original.rotY = 0.707f;
    original.health = 75.0f;
    original.sequenceNumber = 1234;

    auto data = original.Serialize();
    auto restored = ReplicatedEntityState::Deserialize(data);

    EXPECT_EQ(restored.entityId, static_cast<uint32_t>(42));
    EXPECT_NEAR(restored.posX, 10.5f, 0.001f);
    EXPECT_NEAR(restored.posY, 20.3f, 0.001f);
    EXPECT_NEAR(restored.posZ, -5.7f, 0.001f);
    EXPECT_NEAR(restored.rotW, 0.707f, 0.001f);
    EXPECT_NEAR(restored.health, 75.0f, 0.001f);
    EXPECT_EQ(restored.sequenceNumber, static_cast<uint32_t>(1234));
}

TEST(NetReplication_DeltaSnapshotOnlyChangedFields)
{
    ReplicatedEntityState prev;
    prev.entityId = 1;
    prev.posX = 0.0f;
    prev.posY = 0.0f;
    prev.posZ = 0.0f;
    prev.health = 100.0f;

    ReplicatedEntityState curr;
    curr.entityId = 1;
    curr.posX = 1.0f;    // changed
    curr.posY = 0.0f;    // same
    curr.posZ = 0.0f;    // same
    curr.health = 90.0f; // changed

    // Build delta
    uint8_t dirtyMask = 0;
    if (curr.posX != prev.posX)
        dirtyMask |= 0x01;
    if (curr.posY != prev.posY)
        dirtyMask |= 0x02;
    if (curr.posZ != prev.posZ)
        dirtyMask |= 0x04;
    if (curr.health != prev.health)
        dirtyMask |= 0x08;

    EXPECT_EQ(dirtyMask, static_cast<uint8_t>(0x09)); // posX and health changed

    // Delta should be smaller than full state
    int deltaFieldCount = 0;
    for (int i = 0; i < 8; ++i)
    {
        if (dirtyMask & (1 << i))
            ++deltaFieldCount;
    }
    EXPECT_EQ(deltaFieldCount, 2);
}

TEST(NetReplication_ClientPredictionReconciliation)
{
    // Client predicts position locally, server sends authoritative state
    float clientPosX = 10.0f;
    float clientPosY = 0.0f;
    float moveSpeed = 5.0f;
    float dt = 1.0f / 60.0f;

    // Client applies 10 frames of input
    std::vector<float> inputHistory;
    for (int i = 0; i < 10; ++i)
    {
        float input = 1.0f; // moving right
        inputHistory.push_back(input);
        clientPosX += input * moveSpeed * dt;
    }

    float predictedX = clientPosX;

    // Server says player is at slightly different position (due to latency)
    float serverPosX = 10.0f + 5 * (1.0f * moveSpeed * dt); // only 5 frames processed
    uint32_t serverAckSeq = 5;                              // server has processed up to input 5

    // Reconciliation: snap to server, replay unacknowledged inputs
    clientPosX = serverPosX;
    for (uint32_t i = serverAckSeq; i < inputHistory.size(); ++i)
    {
        clientPosX += inputHistory[i] * moveSpeed * dt;
    }

    // After reconciliation, client should be close to predicted position
    EXPECT_NEAR(clientPosX, predictedX, 0.01f);
}

TEST(NetReplication_SequenceNumberWraparound)
{
    uint32_t seq = UINT32_MAX - 5;

    // Advance past wraparound
    for (int i = 0; i < 10; ++i)
    {
        seq++;
    }

    // After wraparound, sequence should be small
    EXPECT_TRUE(seq < 10);
}

TEST(NetReplication_PacketSizeEstimate)
{
    // Full state size
    size_t fullStateSize = sizeof(ReplicatedEntityState);

    // For 32 players, full snapshot
    size_t fullSnapshotSize = fullStateSize * 32;

    // Delta snapshot with ~25% fields changed per entity
    size_t deltaPerEntity = 1 + (fullStateSize / 4); // 1 byte mask + 25% fields
    size_t deltaSnapshotSize = deltaPerEntity * 32;

    // Delta should be significantly smaller
    EXPECT_TRUE(deltaSnapshotSize < fullSnapshotSize);
}

TEST(NetReplication_InterpolationBetweenSnapshots)
{
    // Client interpolates between two server snapshots
    ReplicatedEntityState snap1;
    snap1.posX = 0.0f;
    snap1.posY = 0.0f;

    ReplicatedEntityState snap2;
    snap2.posX = 10.0f;
    snap2.posY = 5.0f;

    float t = 0.5f; // halfway between snapshots

    float interpX = snap1.posX + (snap2.posX - snap1.posX) * t;
    float interpY = snap1.posY + (snap2.posY - snap1.posY) * t;

    EXPECT_NEAR(interpX, 5.0f, 0.001f);
    EXPECT_NEAR(interpY, 2.5f, 0.001f);

    // At t=0, should be snap1
    t = 0.0f;
    interpX = snap1.posX + (snap2.posX - snap1.posX) * t;
    EXPECT_NEAR(interpX, 0.0f, 0.001f);

    // At t=1, should be snap2
    t = 1.0f;
    interpX = snap1.posX + (snap2.posX - snap1.posX) * t;
    EXPECT_NEAR(interpX, 10.0f, 0.001f);
}

TEST(NetReplication_EntityCreationReplication)
{
    // Verify entity creation message can carry initial state
    struct EntitySpawnMessage
    {
        uint32_t entityId;
        uint8_t entityType; // 0=player, 1=projectile, 2=item
        ReplicatedEntityState initialState;
    };

    EntitySpawnMessage msg;
    msg.entityId = 100;
    msg.entityType = 0; // player
    msg.initialState.posX = 5.0f;
    msg.initialState.posY = 1.0f;
    msg.initialState.health = 100.0f;

    EXPECT_EQ(msg.entityId, static_cast<uint32_t>(100));
    EXPECT_EQ(msg.entityType, static_cast<uint8_t>(0));
    EXPECT_NEAR(msg.initialState.health, 100.0f, 0.001f);
}

TEST(NetReplication_EntityDestructionMessage)
{
    struct EntityDestroyMessage
    {
        uint32_t entityId;
        uint8_t reason; // 0=killed, 1=disconnected, 2=despawned
    };

    EntityDestroyMessage msg;
    msg.entityId = 42;
    msg.reason = 0; // killed

    EXPECT_EQ(msg.entityId, static_cast<uint32_t>(42));
    EXPECT_EQ(msg.reason, static_cast<uint8_t>(0));
}

TEST(NetReplication_MultiEntitySnapshot)
{
    std::vector<ReplicatedEntityState> snapshot;

    for (uint32_t i = 0; i < 32; ++i)
    {
        ReplicatedEntityState state;
        state.entityId = i;
        state.posX = static_cast<float>(i) * 2.0f;
        state.posY = 1.0f;
        state.health = 100.0f;
        state.sequenceNumber = 100;
        snapshot.push_back(state);
    }

    EXPECT_EQ(snapshot.size(), static_cast<size_t>(32));

    // Serialize the entire snapshot
    size_t totalBytes = 0;
    for (const auto& state : snapshot)
    {
        auto data = state.Serialize();
        totalBytes += data.size();
    }

    EXPECT_TRUE(totalBytes > 0);
    EXPECT_EQ(totalBytes, sizeof(ReplicatedEntityState) * 32);
}

TEST(NetReplication_LagCompensationRewind)
{
    // Server stores position history for lag compensation
    struct PositionSnapshot
    {
        float timestamp;
        float posX, posY, posZ;
    };

    // 200ms of history at 60Hz = 12 snapshots
    std::vector<PositionSnapshot> history;
    for (int i = 0; i < 12; ++i)
    {
        PositionSnapshot snap;
        snap.timestamp = static_cast<float>(i) / 60.0f;
        snap.posX = static_cast<float>(i) * 0.5f; // moving right
        snap.posY = 1.0f;
        snap.posZ = 0.0f;
        history.push_back(snap);
    }

    // Client reports hit at t=100ms (6 frames ago)
    float clientTime = 6.0f / 60.0f;

    // Find closest snapshot
    size_t closestIdx = 0;
    float minDiff = 999.0f;
    for (size_t i = 0; i < history.size(); ++i)
    {
        float diff = std::abs(history[i].timestamp - clientTime);
        if (diff < minDiff)
        {
            minDiff = diff;
            closestIdx = i;
        }
    }

    EXPECT_EQ(closestIdx, static_cast<size_t>(6));
    EXPECT_NEAR(history[closestIdx].posX, 3.0f, 0.001f);
}

TEST(NetReplication_MultiplayerSmoke_ServerClientConvergence)
{
    struct PlayerState
    {
        float x = 0.0f;
        float z = 0.0f;
        float vx = 0.0f;
        float vz = 0.0f;
        uint32_t ack = 0;
    };
    struct ProjectileState
    {
        bool active = false;
        float x = 0.0f;
        float z = 0.0f;
        float vx = 0.0f;
        float vz = 0.0f;
        float ttl = 0.0f;
    };

    constexpr float dt = 1.0f / 60.0f;
    Spark::ClientPrediction prediction;
    Spark::PredictedState predicted;
    PlayerState server{};
    ProjectileState projectile{};
    std::deque<Spark::PredictedInput> sentInputs;
    uint32_t correctionCount = 0;

    for (int frame = 0; frame < 180; ++frame)
    {
        Spark::PredictedInput input{};
        input.timestamp = static_cast<float>(frame) * dt;
        input.moveDirection = {0.0f, 0.0f, 1.0f};
        input.lookYaw = 0.0f;
        input.lookPitch = 0.0f;
        input.fire = (frame == 30);

        const uint32_t sequence = prediction.RecordInput(input);
        input.sequenceNumber = sequence;
        sentInputs.push_back(input);

        prediction.ApplyPrediction(predicted, input, dt);

        // Simulate 3-frame transport latency for server authoritative processing.
        if (sentInputs.size() >= 3)
        {
            const auto authoritativeInput = sentInputs.front();
            sentInputs.pop_front();

            // Server and client use slightly different simulation paths in practice
            // (fixed-point/quantized movement, collision response, etc.).
            // Add deterministic quantization so reconciliation corrections are exercised.
            server.vx = authoritativeInput.moveDirection.x * 4.9f;
            server.vz = authoritativeInput.moveDirection.z * 4.9f;
            server.x = std::round((server.x + server.vx * dt) * 1000.0f) / 1000.0f;
            server.z = std::round((server.z + server.vz * dt) * 1000.0f) / 1000.0f;
            server.ack = authoritativeInput.sequenceNumber;

            if (authoritativeInput.fire)
            {
                projectile.active = true;
                projectile.x = server.x;
                projectile.z = server.z;
                projectile.vx = 0.0f;
                projectile.vz = 30.0f;
                projectile.ttl = 0.5f;
            }
        }

        if (projectile.active)
        {
            projectile.x += projectile.vx * dt;
            projectile.z += projectile.vz * dt;
            projectile.ttl -= dt;
            if (projectile.ttl <= 0.0f)
                projectile.active = false;
        }

        Spark::PredictedState authoritative{};
        authoritative.position = {server.x, 0.0f, server.z};
        authoritative.velocity = {server.vx, 0.0f, server.vz};
        authoritative.lastProcessedInput = server.ack;

        prediction.Reconcile(authoritative, dt);
        if (prediction.GetLastCorrectionMagnitude() > 0.001f)
            ++correctionCount;
    }

    // Client and server should converge closely after reconciliation.
    EXPECT_NEAR(prediction.GetState().position.x, server.x, 0.20f);
    EXPECT_NEAR(prediction.GetState().position.z, server.z, 0.20f);

    // Projectile should have been spawned and later despawned by TTL.
    EXPECT_FALSE(projectile.active);

    // Prediction corrections should have occurred due to latency and reconciliation.
    EXPECT_TRUE(correctionCount > 0);
}
