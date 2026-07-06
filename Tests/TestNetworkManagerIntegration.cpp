/**
 * @file TestNetworkManagerIntegration.cpp
 * @brief Integration tests for NetworkManager lifecycle and message handling
 *
 * Tests the core NetworkManager orchestration: Initialize → Update → Shutdown.
 * When ENABLE_NETWORKING is defined, tests use real loopback sockets.
 * When it's not defined, tests verify the stub doesn't crash.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkManager.h"

#include <chrono>
#include <thread>
#include <vector>

using namespace Spark::Net;

// ============================================================================
// Lifecycle Tests
// ============================================================================

TEST(NetworkManager_Initialize_Succeeds)
{
    auto& nm = NetworkManager::GetInstance();
    bool ok = nm.Initialize();
#ifdef ENABLE_NETWORKING
    EXPECT_TRUE(ok);
    EXPECT_TRUE(nm.IsInitialized());
#else
    // Stub always returns false
    EXPECT_FALSE(ok);
#endif
    nm.Shutdown();
}

TEST(NetworkManager_ShutdownWithoutInit_DoesNotCrash)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown(); // Safe on uninitialized manager
}

TEST(NetworkManager_DoubleInit_DoesNotCrash)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();
    nm.Initialize(); // Double init should be safe
    nm.Shutdown();
}

TEST(NetworkManager_DoubleShutdown_DoesNotCrash)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();
    nm.Shutdown();
    nm.Shutdown(); // Double shutdown should be safe
}

// ============================================================================
// State Query Tests
// ============================================================================

TEST(NetworkManager_InitialRole_IsNone)
{
    auto& nm = NetworkManager::GetInstance();
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
}

TEST(NetworkManager_InitialConnectionState_IsDisconnected)
{
    auto& nm = NetworkManager::GetInstance();
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
}

// ============================================================================
// Update Tests
// ============================================================================

TEST(NetworkManager_UpdateWithoutInit_DoesNotCrash)
{
    auto& nm = NetworkManager::GetInstance();
    // Should be safe to call Update without Initialize
    nm.Update(0.016f);
}

TEST(NetworkManager_UpdateAfterInit_DoesNotCrash)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();

    for (int i = 0; i < 10; ++i)
        nm.Update(0.016f);

    nm.Shutdown();
}

// ============================================================================
// Server Tests (require ENABLE_NETWORKING)
// ============================================================================

#ifdef ENABLE_NETWORKING

TEST(NetworkManager_StartServer_Succeeds)
{
    auto& nm = NetworkManager::GetInstance();
    EXPECT_TRUE(nm.Initialize());

    // Use a high port to avoid conflicts
    bool serverOk = nm.StartServer(39100, 8);
    EXPECT_TRUE(serverOk);
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));

    nm.StopServer();
    nm.Shutdown();
}

TEST(NetworkManager_StopServer_ResetsRole)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();
    nm.StartServer(39101, 4);
    nm.StopServer();
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    nm.Shutdown();
}

TEST(NetworkManager_ServerUpdate_ProcessesWithoutClients)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();
    nm.StartServer(39102, 4);

    // Run a few update ticks with no clients connected
    for (int i = 0; i < 5; ++i)
        nm.Update(0.016f);

    // Server should be running without errors
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));

    nm.StopServer();
    nm.Shutdown();
}

TEST(NetworkManager_ServerDisconnect_CleansUpGracefully)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();
    nm.StartServer(39103, 4);
    nm.Disconnect();
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    nm.Shutdown();
}

// ============================================================================
// Auth gateway regression test (net-auth lane)
//
// ProcessIncoming's isAuthenticated flag used to be derived from the
// wire-supplied msg.senderID. On the server, that field comes straight off
// the just-deserialized packet -- fully attacker-controlled -- so a client
// could stamp any non-zero senderID and bypass every requiresAuth schema
// (ClientInput, ChatMessage, EntityRPC, ...) without ever completing the
// Connect handshake. This test sends a raw, hand-built ClientInput datagram
// (requiresAuth=true) with a forged senderID from a socket that never
// performed Connect, so the address is absent from the server's
// m_clientAddresses table. The fixed code authenticates from that
// server-trusted table instead of the wire value, so the forged packet must
// be rejected as PacketViolation::Unauthenticated.
// ============================================================================

TEST(NetworkManager_ProcessIncoming_RejectsForgedSenderIDFromUnknownAddress)
{
    auto& nm = NetworkManager::GetInstance();
    EXPECT_TRUE(nm.Initialize());

    const uint16_t port = 39199; // dedicated port, distinct from other tests in this file
    EXPECT_TRUE(nm.StartServer(port, 8));

    nm.GetPacketValidator().ResetStatistics();

    SOCKET rawSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    EXPECT_TRUE(rawSock != INVALID_SOCKET);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    // Hand-build the wire format documented in NetworkManager::SerializeMessage:
    //   [4] magic 0x5350524B  [2] type  [1] channel  [4] senderID
    //   [4] sequence  [4] timestamp  [4] payloadLen  [N] payload
    std::vector<uint8_t> packet;
    auto put32 = [&](uint32_t v) {
        packet.push_back(static_cast<uint8_t>(v & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto put16 = [&](uint16_t v) {
        packet.push_back(static_cast<uint8_t>(v & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };

    put32(0x5350524B);                                          // magic "SPRK"
    put16(static_cast<uint16_t>(MessageType::ClientInput));      // requiresAuth = true
    packet.push_back(static_cast<uint8_t>(ChannelType::Reliable)); // channel
    put32(999);                                                 // FORGED senderID -- never Connect'd
    put32(1);                                                   // sequence
    put32(0);                                                   // timestamp bits (0.0f)
    put32(8);                                                   // payload length (meets minPayloadSize=8)
    for (int i = 0; i < 8; ++i)
        packet.push_back(0);

    int sent = sendto(rawSock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                      reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    EXPECT_EQ(sent, static_cast<int>(packet.size()));

    closesocket(rawSock);

    // Give the server a few ticks to receive and process the datagram.
    for (int i = 0; i < 20 && nm.GetPacketValidator().GetStatistics().totalValidated == 0; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto stats = nm.GetPacketValidator().GetStatistics();
    EXPECT_TRUE(stats.totalValidated >= 1);
    // The forged sender must be rejected as unauthenticated -- NOT accepted via a
    // spoofed senderID (that would be the pre-fix auth-bypass behavior).
    EXPECT_TRUE(stats.rejectedUnauthenticated >= 1);

    nm.StopServer();
    nm.Shutdown();
}

#endif // ENABLE_NETWORKING

// ============================================================================
// Console Integration Tests
// ============================================================================

#ifdef ENABLE_NETWORKING

TEST(NetworkManager_ConsoleGetStatus_ReturnsNonEmpty)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();
    auto status = nm.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
    nm.Shutdown();
}

TEST(NetworkManager_ConsoleGetStats_ReturnsNonEmpty)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Initialize();
    auto stats = nm.Console_GetStats();
    EXPECT_TRUE(!stats.empty());
    nm.Shutdown();
}

#endif // ENABLE_NETWORKING
