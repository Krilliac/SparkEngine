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
