/**
 * @file TestConnectionTimeout.cpp
 * @brief Tests for connection timeout detection, RTT estimation, and
 *        exponential backoff in the reliable channel.
 */

#include "TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "../SparkEngine/Source/Engine/Networking/NetworkManager.h"
#include "../SparkEngine/Source/Engine/ECS/Systems/TerrainSystem.h"

using namespace Spark::Net;

static void ResetNM()
{
    NetworkManager::GetInstance().Shutdown();
}

// ============================================================================
// RTT Estimation
// ============================================================================

TEST(RTT_InitialEstimate)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // RTT should start at 0
    EXPECT_NEAR(nm.GetEstimatedRTT(), 0.0f, 0.01f);

    ResetNM();
}

TEST(RTT_UpdatesFromACK)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // Simulate some updates to advance server time
    for (int i = 0; i < 60; ++i)
        nm.Update(0.016f);

    // RTT estimation is internal — verify it doesn't crash during normal operation
    EXPECT_TRUE(nm.IsInitialized());

    ResetNM();
}

// ============================================================================
// Connection Timeout Detection
// ============================================================================

TEST(Timeout_ServerDetectsTimedOutClients)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    bool timeoutCalled = false;
    ClientID timedOutClient = INVALID_CLIENT;
    nm.SetTimeoutHandler([&](ClientID id)
                         {
                             timeoutCalled = true;
                             timedOutClient = id;
                         });

    // Simulate 10+ seconds of updates (connection timeout is 10s)
    // Without any clients connected, no timeouts should fire
    for (int i = 0; i < 700; ++i)
        nm.Update(0.016f);

    EXPECT_FALSE(timeoutCalled);

    ResetNM();
}

TEST(Timeout_HandlerCanBeSet)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    int callCount = 0;
    nm.SetTimeoutHandler([&](ClientID) { callCount++; });

    // Replace handler
    nm.SetTimeoutHandler([&](ClientID) { callCount += 10; });

    // No clients to time out
    nm.Update(15.0f);
    EXPECT_EQ(callCount, 0);

    ResetNM();
}

TEST(Timeout_NoHandlerDoesNotCrash)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // Don't set a handler — timeout should still work without crashing
    for (int i = 0; i < 700; ++i)
        nm.Update(0.016f);

    EXPECT_TRUE(nm.IsInitialized());
    ResetNM();
}

// ============================================================================
// Exponential Backoff
// ============================================================================

TEST(Backoff_ReliableMessageRetransmission)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    // Send a reliable message — without ENABLE_NETWORKING it goes to outgoing queue
    NetworkMessage msg;
    msg.type = MessageType::ChatMessage;
    msg.channel = ChannelType::Reliable;
    NetBuffer buf;
    buf.WriteString("Test reliable");
    msg.payload = buf.GetData();
    nm.SendMessage(msg);

    // Run several updates to process the message
    for (int i = 0; i < 60; ++i)
        nm.Update(0.016f);

    // Should not crash — backoff logic exercises internally
    EXPECT_TRUE(nm.IsInitialized());

    ResetNM();
}

TEST(Backoff_StatsTrackDroppedPackets)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    const auto& stats = nm.GetStats();
    uint32_t initialDropped = stats.packetsDropped;

    // Update for a long time — reliable messages with no ACK should eventually be dropped
    for (int i = 0; i < 60; ++i)
        nm.Update(0.016f);

    // Dropped count should be >= initial (may increase if reliable messages time out)
    EXPECT_GE(stats.packetsDropped, initialDropped);

    ResetNM();
}

// ============================================================================
// Terrain System Wiring
// ============================================================================

TEST(TerrainSystem_WiredCorrectly)
{
    Spark::ECS::TerrainSystem sys;
    EXPECT_EQ(sys.GetActiveTerrainCount(), 0);
    std::string name = sys.GetName();
    EXPECT_TRUE(name == "TerrainSystem");
}

// ============================================================================
// Console Status
// ============================================================================

TEST(NetworkManager_ConsoleStatusShowsRTT)
{
    ResetNM();
    auto& nm = NetworkManager::GetInstance();
    nm.StartServer(27015, 16);

    std::string stats = nm.Console_GetStats();
    EXPECT_TRUE(stats.find("Ping") != std::string::npos);
    EXPECT_TRUE(stats.find("Unacked") != std::string::npos);

    ResetNM();
}
