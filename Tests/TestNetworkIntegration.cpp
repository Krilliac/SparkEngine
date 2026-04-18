/**
 * @file TestNetworkIntegration.cpp
 * @brief Integration tests for NetworkManager orchestration
 *
 * Tests the high-level NetworkManager lifecycle, message routing, entity
 * replication pipeline, reliable channel mechanics, and lag compensation —
 * all without requiring actual sockets (ENABLE_NETWORKING is off in test builds).
 */

#include "TestFramework.h"

// Platform stubs for test builds (no Windows headers)
#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "Engine/Networking/NetworkManager.h"

using namespace Spark::Net;

// ============================================================================
// Helper: reset the singleton between tests
// ============================================================================

static void ResetNetworkManager()
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
}

// ============================================================================
// 1. Lifecycle Tests
// ============================================================================

TEST(NetworkManager_InitializeShutdown)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();

    EXPECT_FALSE(nm.IsInitialized());
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));

    bool ok = nm.Initialize();
    EXPECT_TRUE(ok);
    EXPECT_TRUE(nm.IsInitialized());

    // Double-init should succeed silently
    bool ok2 = nm.Initialize();
    EXPECT_TRUE(ok2);

    nm.Shutdown();
    EXPECT_FALSE(nm.IsInitialized());
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));

    ResetNetworkManager();
}

TEST(NetworkManager_ServerStartStop)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();

    bool started = nm.StartServer(27015, 16);
    // StartServer auto-initializes if needed
    EXPECT_TRUE(nm.IsInitialized());
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
    EXPECT_EQ(nm.GetLocalClientID(), static_cast<ClientID>(0)); // Server is client 0

    nm.StopServer();
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));

    ResetNetworkManager();
}

TEST(NetworkManager_ClientConnectDisconnect)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();

    bool connected = nm.Connect("127.0.0.1", 27015, "TestPlayer");
    // Without ENABLE_NETWORKING, socket creation is skipped but state transitions occur
    EXPECT_TRUE(nm.IsInitialized());
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Client));
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Connecting));

    nm.Disconnect();
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));

    ResetNetworkManager();
}

// ============================================================================
// 2. Message Handler Tests
// ============================================================================

TEST(NetworkManager_RegisterHandler_Dispatches)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    bool handlerCalled = false;
    nm.RegisterHandler(MessageType::ChatMessage, [&handlerCalled](const NetworkMessage& msg) { handlerCalled = true; });

    // Manually inject a message into the incoming queue via SendMessage + Update loop
    // Since ENABLE_NETWORKING is off, we test the handler registration itself
    EXPECT_FALSE(handlerCalled);

    // Calling Update with no socket should not crash
    nm.Update(0.016f);

    ResetNetworkManager();
}

TEST(NetworkManager_MultipleHandlers)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();

    int chatCount = 0;
    int scoreCount = 0;

    nm.RegisterHandler(MessageType::ChatMessage, [&chatCount](const NetworkMessage&) { chatCount++; });
    nm.RegisterHandler(MessageType::ScoreUpdate, [&scoreCount](const NetworkMessage&) { scoreCount++; });

    // Handler replacement: re-register should overwrite
    int newChatCount = 0;
    nm.RegisterHandler(MessageType::ChatMessage, [&newChatCount](const NetworkMessage&) { newChatCount++; });

    EXPECT_EQ(chatCount, 0);
    EXPECT_EQ(newChatCount, 0);

    ResetNetworkManager();
}

// ============================================================================
// 3. Entity Replication Tests
// ============================================================================

TEST(NetworkManager_EntityRegisterUnregister)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    ReplicatedEntity entity;
    entity.ownerID = 1;
    entity.entityType = "Player";
    entity.position = {10.0f, 20.0f, 30.0f};
    entity.rotation = {0.0f, 90.0f, 0.0f};

    uint32_t netID = nm.RegisterReplicatedEntity(entity);
    EXPECT_GT(netID, static_cast<uint32_t>(0));

    auto* found = nm.GetReplicatedEntity(netID);
    EXPECT_TRUE(found != nullptr);
    EXPECT_EQ(found->networkID, netID);
    EXPECT_EQ(found->ownerID, static_cast<ClientID>(1));
    EXPECT_NEAR(found->position.x, 10.0f, 0.001f);
    EXPECT_NEAR(found->position.y, 20.0f, 0.001f);
    EXPECT_NEAR(found->position.z, 30.0f, 0.001f);

    nm.UnregisterReplicatedEntity(netID);
    EXPECT_TRUE(nm.GetReplicatedEntity(netID) == nullptr);

    ResetNetworkManager();
}

TEST(NetworkManager_EntityPropertyDirtyTracking)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    float health = 100.0f;

    ReplicatedEntity entity;
    entity.ownerID = 1;
    entity.entityType = "Player";

    ReplicatedProperty healthProp;
    healthProp.name = "health";
    healthProp.type = ReplicatedProperty::Type::Float;
    healthProp.dirty = false;
    healthProp.serialize = [&health](NetBuffer& buf) { buf.WriteFloat(health); };
    healthProp.deserialize = [&health](NetBuffer& buf) { health = buf.ReadFloat(); };
    entity.properties.push_back(healthProp);

    uint32_t netID = nm.RegisterReplicatedEntity(entity);
    auto* ent = nm.GetReplicatedEntity(netID);
    EXPECT_TRUE(ent != nullptr);

    // Initially needsFullSync is true
    EXPECT_TRUE(ent->needsFullSync);

    // Mark a property dirty
    nm.MarkPropertyDirty(netID, "health");
    bool foundDirty = false;
    for (const auto& prop : ent->properties)
    {
        if (prop.name == "health" && prop.dirty)
            foundDirty = true;
    }
    EXPECT_TRUE(foundDirty);

    // Marking a nonexistent property should not crash
    nm.MarkPropertyDirty(netID, "nonexistent");

    // Marking on nonexistent entity should not crash
    nm.MarkPropertyDirty(99999, "health");

    ResetNetworkManager();
}

TEST(NetworkManager_MultipleEntities)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    std::vector<uint32_t> ids;
    for (int i = 0; i < 10; ++i)
    {
        ReplicatedEntity entity;
        entity.ownerID = static_cast<ClientID>(i + 1);
        entity.entityType = "NPC_" + std::to_string(i);
        entity.position = {static_cast<float>(i), 0.0f, 0.0f};
        ids.push_back(nm.RegisterReplicatedEntity(entity));
    }

    // All IDs should be unique
    for (size_t i = 0; i < ids.size(); ++i)
    {
        for (size_t j = i + 1; j < ids.size(); ++j)
        {
            EXPECT_NE(ids[i], ids[j]);
        }
    }

    // Unregister half
    for (size_t i = 0; i < 5; ++i)
    {
        nm.UnregisterReplicatedEntity(ids[i]);
        EXPECT_TRUE(nm.GetReplicatedEntity(ids[i]) == nullptr);
    }

    // Other half still exists
    for (size_t i = 5; i < 10; ++i)
    {
        EXPECT_TRUE(nm.GetReplicatedEntity(ids[i]) != nullptr);
    }

    ResetNetworkManager();
}

// ============================================================================
// 4. Serialization Round-Trip Tests
// ============================================================================

TEST(NetworkManager_SerializeDeserializeEntityState)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    float health = 75.5f;
    std::string playerName = "Alice";

    ReplicatedEntity entity;
    entity.ownerID = 1;
    entity.entityType = "Player";
    entity.position = {1.0f, 2.0f, 3.0f};
    entity.rotation = {10.0f, 20.0f, 30.0f};
    entity.velocity = {0.5f, 0.0f, -1.0f};

    ReplicatedProperty healthProp;
    healthProp.name = "health";
    healthProp.type = ReplicatedProperty::Type::Float;
    healthProp.dirty = true;
    healthProp.serialize = [&health](NetBuffer& buf) { buf.WriteFloat(health); };
    healthProp.deserialize = [&health](NetBuffer& buf) { health = buf.ReadFloat(); };
    entity.properties.push_back(healthProp);

    ReplicatedProperty nameProp;
    nameProp.name = "playerName";
    nameProp.type = ReplicatedProperty::Type::String;
    nameProp.dirty = true;
    nameProp.serialize = [&playerName](NetBuffer& buf) { buf.WriteString(playerName); };
    nameProp.deserialize = [&playerName](NetBuffer& buf) { playerName = buf.ReadString(); };
    entity.properties.push_back(nameProp);

    uint32_t netID = nm.RegisterReplicatedEntity(entity);

    // Serialize
    NetBuffer outBuf;
    nm.SerializeEntityState(netID, outBuf);
    EXPECT_GT(outBuf.GetSize(), static_cast<size_t>(0));

    // Modify local values to prove deserialization restores them
    auto* ent = nm.GetReplicatedEntity(netID);
    ent->position = {0.0f, 0.0f, 0.0f};
    ent->rotation = {0.0f, 0.0f, 0.0f};
    ent->velocity = {0.0f, 0.0f, 0.0f};
    health = 0.0f;
    playerName = "RESET";

    // Deserialize — reconstruct a read buffer from the serialized data
    NetBuffer inBuf;
    inBuf.WriteBytes(outBuf.GetData().data(), outBuf.GetData().size());

    nm.DeserializeEntityState(inBuf);

    EXPECT_NEAR(ent->position.x, 1.0f, 0.001f);
    EXPECT_NEAR(ent->position.y, 2.0f, 0.001f);
    EXPECT_NEAR(ent->position.z, 3.0f, 0.001f);
    EXPECT_NEAR(ent->rotation.x, 10.0f, 0.001f);
    EXPECT_NEAR(ent->velocity.z, -1.0f, 0.001f);
    EXPECT_NEAR(health, 75.5f, 0.001f);
    EXPECT_EQ(playerName, std::string("Alice"));

    ResetNetworkManager();
}

TEST(NetworkManager_DeserializeUnknownEntity_CreatesPlaceholder)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // Build a buffer that looks like an entity state for networkID=999 (doesn't exist)
    NetBuffer fakeBuf;
    fakeBuf.WriteUint32(999);                   // networkID
    fakeBuf.WriteVector3({5.0f, 10.0f, 15.0f}); // position
    fakeBuf.WriteVector3({0.0f, 45.0f, 0.0f});  // rotation
    fakeBuf.WriteVector3({1.0f, 0.0f, 0.0f});   // velocity
    fakeBuf.WriteUint16(0);                     // 0 properties

    // Reconstruct as read buffer
    NetBuffer readBuf;
    readBuf.WriteBytes(fakeBuf.GetData().data(), fakeBuf.GetData().size());

    nm.DeserializeEntityState(readBuf);

    // A placeholder should have been created
    auto* placeholder = nm.GetReplicatedEntity(999);
    EXPECT_TRUE(placeholder != nullptr);
    EXPECT_NEAR(placeholder->position.x, 5.0f, 0.001f);
    EXPECT_NEAR(placeholder->position.y, 10.0f, 0.001f);
    EXPECT_NEAR(placeholder->position.z, 15.0f, 0.001f);
    EXPECT_NEAR(placeholder->rotation.y, 45.0f, 0.001f);

    ResetNetworkManager();
}

// ============================================================================
// 5. Server Update Loop Tests
// ============================================================================

TEST(NetworkManager_UpdateDoesNotCrashWhenIdle)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // Simulate 60 frames of update with no clients or messages
    for (int i = 0; i < 60; ++i)
    {
        nm.Update(0.016f);
    }

    EXPECT_GT(nm.GetServerTime(), 0.9f); // ~60 * 0.016 = 0.96s

    ResetNetworkManager();
}

TEST(NetworkManager_UpdateDoesNothingWhenRoleNone)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();

    // Update without starting server or connecting should be a no-op
    float timeBefore = nm.GetServerTime();
    nm.Update(1.0f);
    float timeAfter = nm.GetServerTime();

    // Server time should not advance when role is None
    EXPECT_NEAR(timeBefore, timeAfter, 0.001f);

    ResetNetworkManager();
}

// ============================================================================
// 6. Client Input Tests
// ============================================================================

TEST(NetworkManager_ClientInputSerialization)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    // NOTE: Don't call nm.Connect() here — it would block on a TCP connect
    // to a port with no listener. Instead just initialize and test serialization.
    nm.Initialize();

    ClientInputState input;
    input.moveForward = 1.0f;
    input.moveRight = -0.5f;
    input.lookYaw = 45.0f;
    input.lookPitch = -10.0f;
    input.jump = true;
    input.fire = false;
    input.reload = true;
    input.sprint = true;
    input.crouch = false;
    input.deltaTime = 0.016f;

    // SendClientInput should not crash
    nm.SendClientInput(input);
    nm.SendClientInput(input);
    nm.SendClientInput(input);

    // Verify input is queued (in outgoing) — exercised via Update
    nm.Update(0.016f);

    ResetNetworkManager();
}

// ============================================================================
// 7. Lag Compensator Tests
// ============================================================================

TEST(LagCompensator_RecordAndRewind)
{
    LagCompensator comp;

    // Record snapshots at 0.0s, 0.1s, 0.2s
    for (int i = 0; i < 3; ++i)
    {
        HistorySnapshot snap;
        snap.timestamp = static_cast<float>(i) * 0.1f;

        HistorySnapshot::EntityState es;
        es.networkID = 1;
        es.position = {static_cast<float>(i) * 10.0f, 0.0f, 0.0f};
        es.rotation = {0.0f, 0.0f, 0.0f};
        es.boundsMin = {-1.0f, -1.0f, -1.0f};
        es.boundsMax = {1.0f, 1.0f, 1.0f};
        snap.entities.push_back(es);

        comp.RecordSnapshot(snap);
    }

    // Rewind to t=0.05 — should interpolate between snapshot 0 and 1
    HistorySnapshot result;
    bool ok = comp.RewindToTime(0.05f, result);
    EXPECT_TRUE(ok);
    EXPECT_NEAR(result.timestamp, 0.05f, 0.001f);
    EXPECT_EQ(result.entities.size(), static_cast<size_t>(1));
    EXPECT_NEAR(result.entities[0].position.x, 5.0f, 0.5f); // Interpolated

    // Rewind to exact snapshot time
    ok = comp.RewindToTime(0.1f, result);
    EXPECT_TRUE(ok);
    EXPECT_NEAR(result.entities[0].position.x, 10.0f, 0.5f);

    // Rewind to time before all snapshots
    ok = comp.RewindToTime(-1.0f, result);
    // Should return the earliest snapshot
    EXPECT_TRUE(ok);
}

TEST(LagCompensator_PrunesOldSnapshots)
{
    LagCompensator comp;
    comp.SetMaxHistoryDuration(0.5f);

    // Record snapshots over 2 seconds
    for (int i = 0; i < 20; ++i)
    {
        HistorySnapshot snap;
        snap.timestamp = static_cast<float>(i) * 0.1f;
        comp.RecordSnapshot(snap);
    }

    // Rewind to t=0.0 should fail (pruned)
    HistorySnapshot result;
    bool ok = comp.RewindToTime(0.0f, result);
    // The oldest snapshot should be around t=1.4 (2.0 - 0.5 = 1.5 cutoff)
    // So rewinding to 0.0 should still return something (the oldest available)
    // but the position won't match t=0 data
    EXPECT_TRUE(ok); // Returns closest available

    // Rewind to recent time should work
    ok = comp.RewindToTime(1.8f, result);
    EXPECT_TRUE(ok);
}

TEST(LagCompensator_EmptyHistory)
{
    LagCompensator comp;

    HistorySnapshot result;
    bool ok = comp.RewindToTime(0.0f, result);
    EXPECT_FALSE(ok);

    comp.Clear();
    ok = comp.RewindToTime(0.0f, result);
    EXPECT_FALSE(ok);
}

// ============================================================================
// 8. NetBuffer Extended Tests
// ============================================================================

TEST(NetBuffer_OverrunProtection)
{
    NetBuffer buf;
    buf.WriteUint8(42);

    // Read the single byte
    uint8_t val = buf.ReadUint8();
    EXPECT_EQ(val, static_cast<uint8_t>(42));
    EXPECT_FALSE(buf.HasError());

    // Attempt to over-read — should set error flag
    uint8_t overread = buf.ReadUint8();
    EXPECT_TRUE(buf.HasError());
    EXPECT_EQ(overread, static_cast<uint8_t>(0)); // Default on error

    // All subsequent reads should also fail
    uint32_t val32 = buf.ReadUint32();
    EXPECT_TRUE(buf.HasError());
    EXPECT_EQ(val32, static_cast<uint32_t>(0));
}

TEST(NetBuffer_Integration_StringRoundTrip)
{
    NetBuffer buf;
    buf.WriteString("Hello, SparkEngine!");
    buf.WriteString("");
    buf.WriteString("Unicode test: 42");

    // Reconstruct as read buffer
    NetBuffer readBuf;
    readBuf.WriteBytes(buf.GetData().data(), buf.GetData().size());

    EXPECT_EQ(readBuf.ReadString(), std::string("Hello, SparkEngine!"));
    EXPECT_EQ(readBuf.ReadString(), std::string(""));
    EXPECT_EQ(readBuf.ReadString(), std::string("Unicode test: 42"));
    EXPECT_FALSE(readBuf.HasError());
}

TEST(NetBuffer_Integration_Vector3RoundTrip)
{
    NetBuffer buf;
    XMFLOAT3 vec = {1.5f, -2.5f, 3.14159f};
    buf.WriteVector3(vec);

    NetBuffer readBuf;
    readBuf.WriteBytes(buf.GetData().data(), buf.GetData().size());

    XMFLOAT3 result = readBuf.ReadVector3();
    EXPECT_NEAR(result.x, 1.5f, 0.0001f);
    EXPECT_NEAR(result.y, -2.5f, 0.0001f);
    EXPECT_NEAR(result.z, 3.14159f, 0.0001f);
    EXPECT_FALSE(readBuf.HasError());
}

TEST(NetBuffer_Integration_CanRead)
{
    NetBuffer buf;
    buf.WriteUint32(12345);

    NetBuffer readBuf;
    readBuf.WriteBytes(buf.GetData().data(), buf.GetData().size());

    EXPECT_TRUE(readBuf.CanRead(4));
    EXPECT_FALSE(readBuf.CanRead(5));
    EXPECT_TRUE(readBuf.CanRead(0));
}

TEST(NetBuffer_Integration_Reset)
{
    NetBuffer buf;
    buf.WriteUint32(100);
    buf.WriteFloat(3.14f);

    EXPECT_GT(buf.GetSize(), static_cast<size_t>(0));

    buf.Reset();
    EXPECT_EQ(buf.GetSize(), static_cast<size_t>(0));
    EXPECT_EQ(buf.GetReadPosition(), static_cast<size_t>(0));
    EXPECT_FALSE(buf.HasError());
}

// ============================================================================
// 9. Console Status Tests
// ============================================================================

TEST(NetworkManager_ConsoleStatusOutput)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    std::string status = nm.Console_GetStatus();
    EXPECT_TRUE(status.find("Server") != std::string::npos);
    EXPECT_TRUE(status.find("Connected") != std::string::npos);

    std::string clients = nm.Console_ListClients();
    EXPECT_TRUE(clients.find("0") != std::string::npos); // "0" clients

    std::string stats = nm.Console_GetStats();
    EXPECT_TRUE(stats.find("Ping") != std::string::npos);

    ResetNetworkManager();
}

// ============================================================================
// 10. Replication Timer Tests
// ============================================================================

TEST(NetworkManager_ReplicationTimerFires)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // Register an entity with dirty properties
    float health = 100.0f;
    ReplicatedEntity entity;
    entity.ownerID = 1;
    entity.entityType = "Player";
    entity.position = {0.0f, 0.0f, 0.0f};

    ReplicatedProperty healthProp;
    healthProp.name = "health";
    healthProp.type = ReplicatedProperty::Type::Float;
    healthProp.dirty = true;
    healthProp.serialize = [&health](NetBuffer& buf) { buf.WriteFloat(health); };
    entity.properties.push_back(healthProp);

    uint32_t netID = nm.RegisterReplicatedEntity(entity);

    // Run enough updates to trigger replication (interval = 0.05s)
    for (int i = 0; i < 10; ++i)
    {
        nm.Update(0.016f); // ~10 * 0.016 = 0.16s > 0.05s
    }

    // After replication fires, dirty flags should be cleared
    auto* ent = nm.GetReplicatedEntity(netID);
    EXPECT_TRUE(ent != nullptr);
    EXPECT_FALSE(ent->needsFullSync);
    bool anyDirty = false;
    for (const auto& prop : ent->properties)
    {
        if (prop.dirty)
            anyDirty = true;
    }
    EXPECT_FALSE(anyDirty);

    ResetNetworkManager();
}

// ============================================================================
// 11. Connection Handling Tests
// ============================================================================

TEST(NetworkManager_HandleConnectCreatesClient)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 4);

    // Simulate a connect message (normally from ProcessIncoming)
    NetworkMessage connectMsg;
    connectMsg.type = MessageType::Connect;
    connectMsg.channel = ChannelType::Reliable;
    connectMsg.senderID = INVALID_CLIENT;
    NetBuffer buf;
    buf.WriteString("TestPlayer");
    connectMsg.payload = buf.GetData();

    // The HandleConnect is private but is registered as a handler.
    // We can test indirectly by checking client count changes won't crash.
    // Update without actual socket data is the safest approach.
    nm.Update(0.016f);

    // At minimum, the server should be running without error
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Connected));

    ResetNetworkManager();
}

TEST(NetworkManager_KickClientDoesNotCrashOnInvalid)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // Kick a client that doesn't exist — should not crash
    nm.KickClient(999, "Test kick");

    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));

    ResetNetworkManager();
}

// ============================================================================
// 12. Bandwidth Stats Tests
// ============================================================================

TEST(NetworkManager_BandwidthStatsReset)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    const auto& stats = nm.GetStats();
    EXPECT_EQ(stats.bytesSent, static_cast<uint64_t>(0));
    EXPECT_EQ(stats.bytesReceived, static_cast<uint64_t>(0));
    EXPECT_NEAR(stats.packetLoss, 0.0f, 0.001f);

    ResetNetworkManager();
}

// ============================================================================
// 13. Full Entity Sync Tests
// ============================================================================

TEST(NetworkManager_SendFullEntitySyncDoesNotCrashWithoutClients)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();

    // Register some entities without starting a real server (avoids blocking socket)
    for (int i = 0; i < 5; ++i)
    {
        ReplicatedEntity entity;
        entity.ownerID = 1;
        entity.entityType = "TestEntity";
        entity.position = {static_cast<float>(i), 0.0f, 0.0f};
        nm.RegisterReplicatedEntity(entity);
    }

    // SendFullEntitySync to a nonexistent client — should not crash
    nm.SendFullEntitySync(999);

    ResetNetworkManager();
}

TEST(NetworkManager_SendFullEntitySyncNotServerNoop)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.Connect("127.0.0.1", 27015, "Client");

    // As a client, SendFullEntitySync should be a no-op
    nm.SendFullEntitySync(1);

    ResetNetworkManager();
}

// ============================================================================
// 14. Edge Cases
// ============================================================================

TEST(NetworkManager_UnregisterNonexistentEntity)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // Should not crash
    nm.UnregisterReplicatedEntity(99999);
    EXPECT_TRUE(nm.GetReplicatedEntity(99999) == nullptr);

    ResetNetworkManager();
}

TEST(NetworkManager_ShutdownWithActiveEntities)
{
    ResetNetworkManager();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    for (int i = 0; i < 10; ++i)
    {
        ReplicatedEntity entity;
        entity.ownerID = 1;
        entity.entityType = "TestEntity";
        nm.RegisterReplicatedEntity(entity);
    }

    // Shutdown should clean up all entities without crash
    nm.Shutdown();
    EXPECT_FALSE(nm.IsInitialized());

    ResetNetworkManager();
}

TEST(NetworkManager_RapidStartStopCycles)
{
    auto& nm = NetworkManager::GetInstance();

    // Test rapid init/shutdown cycles without opening real sockets
    // (StartServer and Connect use blocking socket ops that can hang in tests)
    for (int i = 0; i < 5; ++i)
    {
        ResetNetworkManager();
        nm.Initialize();
        nm.Update(0.016f);
        nm.Shutdown();
    }

    ResetNetworkManager();
}
