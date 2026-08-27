/**
 * @file TestNetworkManagerIntegration.cpp
 * @brief Integration tests for NetworkManager lifecycle and message handling
 *
 * Tests the core NetworkManager orchestration: Initialize → Update → Shutdown.
 * When ENABLE_NETWORKING is defined, tests use real loopback sockets.
 * When it's not defined, tests verify the stub doesn't crash.
 */

#include "TestFramework.h"
#include "Engine/Networking/InstabilitySimulator.h"
#include "Engine/Networking/NetworkManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef SPARK_PLATFORM_WINDOWS
#include <fcntl.h>
#endif

using namespace Spark::Net;

namespace Spark::Net
{
    struct NetworkManagerClientIdTestAccess
    {
        static void SeedNextClientID(NetworkManager& manager, ClientID candidate)
        {
            std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
            std::lock_guard<std::mutex> clientsLock(manager.m_clientsMutex);
            manager.m_nextClientID = candidate;
        }

        static uint64_t DroppedIncomingMessages(const NetworkManager& manager)
        {
            return manager.m_droppedIncomingMessages.load(std::memory_order_relaxed);
        }

        static size_t PendingOutgoingMessages(const NetworkManager& manager)
        {
            std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
            std::lock_guard<std::mutex> queueLock(manager.m_queueMutex);
            return manager.m_outgoingQueue.size();
        }

        static size_t PendingIncomingMessages(const NetworkManager& manager)
        {
            std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
            std::lock_guard<std::mutex> queueLock(manager.m_queueMutex);
            return manager.m_incomingQueue.size();
        }

        static size_t PendingClientReliableMessages(const NetworkManager& manager)
        {
            std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
            const auto peer = manager.m_peers.find(NetworkManager::SERVER_PEER);
            return peer == manager.m_peers.end() ? 0 : peer->second.unacknowledgedMessages.size();
        }

        static size_t PendingClientSensitiveReliableMessages(const NetworkManager& manager)
        {
            std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
            const auto peer = manager.m_peers.find(NetworkManager::SERVER_PEER);
            if (peer == manager.m_peers.end())
                return 0;
            return static_cast<size_t>(std::count_if(peer->second.unacknowledgedMessages.begin(),
                                                     peer->second.unacknowledgedMessages.end(),
                                                     [](const auto& entry) { return entry.second.sensitive; }));
        }

        static void ReceiveAvailableIncomingForTest(NetworkManager& manager)
        {
            std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
            (void)manager.ProcessIncoming();
        }

        static bool DeserializeMessageForTest(const NetworkManager& manager, const std::vector<uint8_t>& data,
                                              NetworkMessage& message)
        {
            return manager.DeserializeMessage(data.data(), data.size(), message);
        }
    };
} // namespace Spark::Net

// ============================================================================
// Lifecycle Tests
// ============================================================================

TEST(NetworkClientIdPolicy_ReservedBoundaryNeverBecomesGenerated)
{
    EXPECT_FALSE(IsReservedClientID(1));
    EXPECT_FALSE(IsReservedClientID(LAST_GENERATED_CLIENT_ID));
    EXPECT_TRUE(IsReservedClientID(INVALID_CLIENT));
    EXPECT_TRUE(IsReservedClientID(FIRST_RESERVED_CLIENT_ID));
    EXPECT_TRUE(IsReservedClientID(0xFFFFFF01u));
    EXPECT_TRUE(IsReservedClientID(UINT32_MAX));
    EXPECT_EQ(NormalizeGeneratedClientID(INVALID_CLIENT), ClientID{1});
    EXPECT_EQ(NormalizeGeneratedClientID(0xFFFFFF01u), ClientID{1});
    EXPECT_EQ(AdvanceGeneratedClientID(LAST_GENERATED_CLIENT_ID), ClientID{1});
}

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

namespace
{
    class ScopedNetworkBindMode
    {
      public:
        explicit ScopedNetworkBindMode(const char* value)
        {
            if (const char* previous = std::getenv("SPARK_NETWORK_BIND_MODE"))
                m_previous = previous;
#ifdef SPARK_PLATFORM_WINDOWS
            _putenv_s("SPARK_NETWORK_BIND_MODE", value);
#else
            setenv("SPARK_NETWORK_BIND_MODE", value, 1);
#endif
        }

        ~ScopedNetworkBindMode()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            _putenv_s("SPARK_NETWORK_BIND_MODE", m_previous ? m_previous->c_str() : "");
#else
            if (m_previous)
                setenv("SPARK_NETWORK_BIND_MODE", m_previous->c_str(), 1);
            else
                unsetenv("SPARK_NETWORK_BIND_MODE");
#endif
        }

      private:
        std::optional<std::string> m_previous;
    };

    bool BindLoopbackEphemeral(SOCKET socket)
    {
        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = 0;
        localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return bind(socket, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) == 0;
    }

    bool BindAnyEphemeral(SOCKET socket)
    {
        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = 0;
        localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        return bind(socket, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) == 0;
    }

    uint16_t BoundPort(SOCKET socket)
    {
        sockaddr_in localAddr{};
#ifdef SPARK_PLATFORM_WINDOWS
        int localAddrLength = sizeof(localAddr);
#else
        socklen_t localAddrLength = sizeof(localAddr);
#endif
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&localAddr), &localAddrLength) != 0)
            return 0;
        return ntohs(localAddr.sin_port);
    }

    bool SetNonBlocking(SOCKET socket)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        u_long enabled = 1;
        return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
        const int flags = fcntl(socket, F_GETFL, 0);
        return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    std::vector<std::vector<uint8_t>> ReceiveDatagrams(SOCKET socket, std::chrono::milliseconds duration)
    {
        std::vector<std::vector<uint8_t>> datagrams;
        const auto deadline = std::chrono::steady_clock::now() + duration;
        do
        {
            std::array<uint8_t, 8192> buffer{};
            const int received = recvfrom(socket, reinterpret_cast<char*>(buffer.data()),
                                          static_cast<int>(buffer.size()), 0, nullptr, nullptr);
            if (received > 0)
            {
                datagrams.emplace_back(buffer.begin(), buffer.begin() + received);
                continue;
            }
#ifdef SPARK_PLATFORM_WINDOWS
            EXPECT_TRUE(received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK);
#else
            EXPECT_TRUE(received == SOCKET_ERROR && (errno == EWOULDBLOCK || errno == EAGAIN));
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        return datagrams;
    }

    bool ContainsBytes(const std::vector<uint8_t>& haystack, const std::vector<uint8_t>& needle)
    {
        return !needle.empty() &&
               std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
    }

    std::vector<uint8_t> BuildWireMessage(MessageType type, ChannelType channel, ClientID sender,
                                          const std::vector<uint8_t>& payload = {})
    {
        std::vector<uint8_t> packet;
        auto put32 = [&](uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
                packet.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
        };
        auto put16 = [&](uint16_t value)
        {
            packet.push_back(static_cast<uint8_t>(value & 0xffu));
            packet.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
        };
        put32(0x5350524B);
        put16(static_cast<uint16_t>(type));
        packet.push_back(static_cast<uint8_t>(channel));
        put32(sender);
        put32(0); // sequence
        put32(0); // timestamp bits
        put32(static_cast<uint32_t>(payload.size()));
        packet.insert(packet.end(), payload.begin(), payload.end());
        return packet;
    }
} // namespace

TEST(NetworkManager_GeneratedClientIdsWrapBeforeReservedSentinels)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    ASSERT_TRUE(nm.Initialize());
    ASSERT_TRUE(nm.StartServer(0, 2, NetworkBindScope::LoopbackOnly));
    NetworkManagerClientIdTestAccess::SeedNextClientID(nm, LAST_GENERATED_CLIENT_ID);

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(nm.GetBoundPort());
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto connect = BuildWireMessage(MessageType::Connect, ChannelType::Reliable, INVALID_CLIENT);

    SOCKET first = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(first != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(first));
    ASSERT_EQ(sendto(first, reinterpret_cast<const char*>(connect.data()), static_cast<int>(connect.size()), 0,
                     reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)),
              static_cast<int>(connect.size()));
    for (int i = 0; i < 50 && nm.GetClients().size() < 1; ++i)
    {
        nm.Update(0.01f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(nm.GetClients().size(), static_cast<size_t>(1));
    EXPECT_TRUE(nm.GetClients().contains(LAST_GENERATED_CLIENT_ID));

    SOCKET second = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(second != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(second));
    ASSERT_EQ(sendto(second, reinterpret_cast<const char*>(connect.data()), static_cast<int>(connect.size()), 0,
                     reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)),
              static_cast<int>(connect.size()));
    for (int i = 0; i < 50 && nm.GetClients().size() < 2; ++i)
    {
        nm.Update(0.01f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_EQ(nm.GetClients().size(), static_cast<size_t>(2));
    EXPECT_TRUE(nm.GetClients().contains(ClientID{1}));
    EXPECT_FALSE(nm.GetClients().contains(0xFFFFFF01u));
    for (const auto& [id, client] : nm.GetClients())
    {
        (void)client;
        EXPECT_FALSE(IsReservedClientID(id));
    }

    closesocket(second);
    closesocket(first);
    nm.StopServer();
    nm.Shutdown();
}

TEST(NetworkManager_MessageCallbackMayWaitForCrossThreadStopServer)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    ASSERT_TRUE(nm.Initialize());
    ASSERT_TRUE(nm.StartServer(0, 4));
    const uint16_t port = nm.GetBoundPort();
    ASSERT_TRUE(port != 0);

    SOCKET rawSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(rawSock != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(rawSock));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    auto sendPacket = [&](MessageType type)
    {
        std::vector<uint8_t> packet;
        auto put32 = [&](uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
                packet.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
        };
        auto put16 = [&](uint16_t value)
        {
            packet.push_back(static_cast<uint8_t>(value & 0xFF));
            packet.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        };
        put32(0x5350524B);
        put16(static_cast<uint16_t>(type));
        packet.push_back(static_cast<uint8_t>(ChannelType::Unreliable));
        put32(INVALID_CLIENT);
        put32(0);
        put32(0);
        put32(0);
        return sendto(rawSock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                      reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    };

    ASSERT_TRUE(sendPacket(MessageType::Connect) > 0);
    for (int i = 0; i < 50 && nm.GetClients().empty(); ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_FALSE(nm.GetClients().empty());
    const ClientID admittedClient = nm.GetClients().begin()->first;
    EXPECT_TRUE(nm.IsClientLoopback(admittedClient));
    EXPECT_FALSE(nm.IsClientLoopback(INVALID_CLIENT));

    std::atomic<bool> callbackCalled{false};
    std::atomic<bool> workerFinished{false};
    std::atomic<bool> workerFinishedDuringCallback{false};
    std::thread worker;
    nm.RegisterHandler(MessageType::UserDefined,
                       [&](const NetworkMessage&)
                       {
                           callbackCalled.store(true, std::memory_order_release);
                           worker = std::thread(
                               [&]
                               {
                                   nm.StopServer();
                                   workerFinished.store(true, std::memory_order_release);
                               });

                           const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                           while (!workerFinished.load(std::memory_order_acquire) &&
                                  std::chrono::steady_clock::now() < deadline)
                               std::this_thread::yield();
                           workerFinishedDuringCallback.store(workerFinished.load(std::memory_order_acquire),
                                                              std::memory_order_release);
                       });

    ASSERT_TRUE(sendPacket(MessageType::UserDefined) > 0);
    for (int i = 0; i < 50 && !callbackCalled.load(std::memory_order_acquire); ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (worker.joinable())
        worker.join();

    EXPECT_TRUE(callbackCalled.load(std::memory_order_acquire));
    EXPECT_TRUE(workerFinished.load(std::memory_order_acquire));
    // Under the old Update lock contract, the worker's StopServer could not
    // finish until this callback returned. Update must also detect the
    // lifecycle epoch change and avoid continuing against the closed socket.
    EXPECT_TRUE(workerFinishedDuringCallback.load(std::memory_order_acquire));
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));

    closesocket(rawSock);
    nm.Shutdown();
}

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
    ASSERT_TRUE(BindLoopbackEphemeral(rawSock));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    // Hand-build the wire format documented in NetworkManager::SerializeMessage:
    //   [4] magic 0x5350524B  [2] type  [1] channel  [4] senderID
    //   [4] sequence  [4] timestamp  [4] payloadLen  [N] payload
    std::vector<uint8_t> packet;
    auto put32 = [&](uint32_t v)
    {
        packet.push_back(static_cast<uint8_t>(v & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto put16 = [&](uint16_t v)
    {
        packet.push_back(static_cast<uint8_t>(v & 0xFF));
        packet.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };

    put32(0x5350524B);                                             // magic "SPRK"
    put16(static_cast<uint16_t>(MessageType::ClientInput));        // requiresAuth = true
    packet.push_back(static_cast<uint8_t>(ChannelType::Reliable)); // channel
    put32(999);                                                    // FORGED senderID -- never Connect'd
    put32(1);                                                      // sequence
    put32(0);                                                      // timestamp bits (0.0f)
    put32(8);                                                      // payload length (meets minPayloadSize=8)
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

TEST(NetworkManager_ProcessIncoming_RejectsInvalidChannelBeforeConnect)
{
    auto& nm = NetworkManager::GetInstance();
    EXPECT_TRUE(nm.Initialize());

    const uint16_t port = 39200;
    EXPECT_TRUE(nm.StartServer(port, 8));
    nm.GetPacketValidator().ResetStatistics();

    SOCKET rawSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    EXPECT_TRUE(rawSock != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(rawSock));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    std::vector<uint8_t> packet;
    auto put32 = [&](uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            packet.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    };
    auto put16 = [&](uint16_t value)
    {
        packet.push_back(static_cast<uint8_t>(value & 0xFF));
        packet.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    };

    put32(0x5350524B);
    put16(static_cast<uint16_t>(MessageType::Connect));
    packet.push_back(255); // not a defined ChannelType
    put32(INVALID_CLIENT);
    put32(0);
    put32(0);
    put32(0);

    const int sent = sendto(rawSock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                            reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    EXPECT_EQ(sent, static_cast<int>(packet.size()));
    closesocket(rawSock);

    for (int i = 0; i < 10; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(nm.GetClients().empty());
    EXPECT_EQ(nm.GetPacketValidator().GetStatistics().totalValidated, 0u);

    nm.StopServer();
    nm.Shutdown();
}

TEST(NetworkManager_ClientAcceptsOnlyConfiguredServerEndpoint)
{
    auto& nm = NetworkManager::GetInstance();
    EXPECT_TRUE(nm.Initialize());

    const uint16_t port = 39201;
    SOCKET serverSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET spoofSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    EXPECT_TRUE(serverSock != INVALID_SOCKET);
    EXPECT_TRUE(spoofSock != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(spoofSock));

    sockaddr_in listenAddr{};
    listenAddr.sin_family = AF_INET;
    listenAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &listenAddr.sin_addr);
    EXPECT_EQ(bind(serverSock, reinterpret_cast<sockaddr*>(&listenAddr), sizeof(listenAddr)), 0);

#ifdef SPARK_PLATFORM_WINDOWS
    u_long nonBlocking = 1;
    EXPECT_EQ(ioctlsocket(serverSock, FIONBIO, &nonBlocking), 0);
#else
    const int flags = fcntl(serverSock, F_GETFL, 0);
    EXPECT_TRUE(flags >= 0);
    EXPECT_EQ(fcntl(serverSock, F_SETFL, flags | O_NONBLOCK), 0);
#endif
    EXPECT_TRUE(nm.Connect("127.0.0.1", port, "EndpointTest"));

    sockaddr_in clientAddr{};
#ifdef SPARK_PLATFORM_WINDOWS
    int clientAddrLength = sizeof(clientAddr);
#else
    socklen_t clientAddrLength = sizeof(clientAddr);
#endif
    std::vector<uint8_t> receiveBuffer(1024);
    int received = SOCKET_ERROR;
    for (int i = 0; i < 20 && received == SOCKET_ERROR; ++i)
    {
        nm.Update(0.016f);
        received =
            recvfrom(serverSock, reinterpret_cast<char*>(receiveBuffer.data()), static_cast<int>(receiveBuffer.size()),
                     0, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLength);
        if (received == SOCKET_ERROR)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(received > 0);

    std::vector<uint8_t> accepted;
    auto put32 = [&](uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            accepted.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    };
    auto put16 = [&](uint16_t value)
    {
        accepted.push_back(static_cast<uint8_t>(value & 0xFF));
        accepted.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    };
    put32(0x5350524B);
    put16(static_cast<uint16_t>(MessageType::ConnectAccepted));
    accepted.push_back(static_cast<uint8_t>(ChannelType::Reliable));
    put32(INVALID_CLIENT);
    put32(1);
    put32(0);
    put32(4);
    put32(42);

    EXPECT_EQ(sendto(spoofSock, reinterpret_cast<const char*>(accepted.data()), static_cast<int>(accepted.size()), 0,
                     reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr)),
              static_cast<int>(accepted.size()));
    for (int i = 0; i < 5; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Connecting));
    EXPECT_EQ(nm.GetLocalClientID(), INVALID_CLIENT);

    EXPECT_EQ(sendto(serverSock, reinterpret_cast<const char*>(accepted.data()), static_cast<int>(accepted.size()), 0,
                     reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr)),
              static_cast<int>(accepted.size()));
    for (int i = 0; i < 10 && nm.GetConnectionState() != ConnectionState::Connected; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
    EXPECT_EQ(nm.GetLocalClientID(), 42u);

    nm.Disconnect();
    closesocket(spoofSock);
    closesocket(serverSock);
    nm.Shutdown();
}

TEST(NetworkManager_RejectedConnectDoesNotTriggerEntitySync)
{
    auto& nm = NetworkManager::GetInstance();
    EXPECT_TRUE(nm.Initialize());

    const uint16_t port = 39202;
    EXPECT_TRUE(nm.StartServer(port, 1));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    std::vector<uint8_t> connectPacket;
    auto put32 = [&](uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            connectPacket.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    };
    auto put16 = [&](uint16_t value)
    {
        connectPacket.push_back(static_cast<uint8_t>(value & 0xFF));
        connectPacket.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    };
    put32(0x5350524B);
    put16(static_cast<uint16_t>(MessageType::Connect));
    connectPacket.push_back(static_cast<uint8_t>(ChannelType::Reliable));
    put32(INVALID_CLIENT);
    put32(0);
    put32(0);
    put32(0);

    auto sendConnect = [&]()
    {
        SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        EXPECT_TRUE(client != INVALID_SOCKET);
        EXPECT_TRUE(BindLoopbackEphemeral(client));
        const int sent =
            sendto(client, reinterpret_cast<const char*>(connectPacket.data()), static_cast<int>(connectPacket.size()),
                   0, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
        EXPECT_EQ(sent, static_cast<int>(connectPacket.size()));
        return client;
    };

    SOCKET admitted = sendConnect();
    for (int i = 0; i < 20 && nm.GetClients().empty(); ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(nm.GetClients().size(), 1u);
    EXPECT_EQ(nm.GetStats().fullEntitySyncs, 1u);

    SOCKET rejected = sendConnect();
    for (int i = 0; i < 10; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(nm.GetClients().size(), 1u);
    EXPECT_EQ(nm.GetStats().fullEntitySyncs, 1u);

    closesocket(rejected);
    closesocket(admitted);
    nm.StopServer();
    nm.Shutdown();
}

TEST(NetworkManager_RequestedPortConflictFailsWithoutAdjacentFallback)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    ASSERT_TRUE(nm.Initialize());

    SOCKET reservation = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(reservation != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(reservation));

    sockaddr_in reservedAddress{};
#ifdef SPARK_PLATFORM_WINDOWS
    int reservedAddressLength = sizeof(reservedAddress);
#else
    socklen_t reservedAddressLength = sizeof(reservedAddress);
#endif
    ASSERT_EQ(getsockname(reservation, reinterpret_cast<sockaddr*>(&reservedAddress), &reservedAddressLength), 0);
    const uint16_t reservedPort = ntohs(reservedAddress.sin_port);
    ASSERT_TRUE(reservedPort != 0);

    EXPECT_FALSE(nm.StartServer(reservedPort, 4));
    EXPECT_EQ(nm.GetBoundPort(), static_cast<uint16_t>(0));
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));

    closesocket(reservation);
    nm.Shutdown();
}

TEST(NetworkManager_ProtocolHandlersAndApplicationObserversBothRunExactlyOnce)
{
    auto& nm = NetworkManager::GetInstance();
    ASSERT_TRUE(nm.Initialize());
    ASSERT_TRUE(nm.StartServer(0, 1));

    int connectCallbacks = 0;
    int disconnectCallbacks = 0;
    size_t clientsSeenByDisconnect = 99;
    nm.RegisterHandler(MessageType::Connect,
                       [&](const NetworkMessage& msg)
                       {
                           ++connectCallbacks;
                           EXPECT_TRUE(msg.senderID != INVALID_CLIENT);
                       });
    nm.RegisterHandler(MessageType::Disconnect,
                       [&](const NetworkMessage&)
                       {
                           ++disconnectCallbacks;
                           clientsSeenByDisconnect = nm.GetClients().size();
                       });

    SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(client != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(client));
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(nm.GetBoundPort());
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    const auto connect = BuildWireMessage(MessageType::Connect, ChannelType::Reliable, INVALID_CLIENT);
    auto sendPacket = [&](const std::vector<uint8_t>& packet)
    {
        return sendto(client, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                      reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr));
    };

    EXPECT_EQ(sendPacket(connect), static_cast<int>(connect.size()));
    for (int i = 0; i < 20 && connectCallbacks == 0; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(connectCallbacks, 1);
    EXPECT_EQ(nm.GetClients().size(), 1u);
    EXPECT_EQ(nm.GetStats().fullEntitySyncs, 1u);

    const auto disconnect = BuildWireMessage(MessageType::Disconnect, ChannelType::Reliable, INVALID_CLIENT);
    EXPECT_EQ(sendPacket(disconnect), static_cast<int>(disconnect.size()));
    for (int i = 0; i < 20 && disconnectCallbacks == 0; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(disconnectCallbacks, 1);
    EXPECT_EQ(clientsSeenByDisconnect, 0u);
    EXPECT_TRUE(nm.GetClients().empty());

    // The released capacity is usable immediately; reconnect admission and its
    // observer each occur once, with a fresh full sync.
    EXPECT_EQ(sendPacket(connect), static_cast<int>(connect.size()));
    for (int i = 0; i < 20 && connectCallbacks < 2; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(connectCallbacks, 2);
    EXPECT_EQ(nm.GetClients().size(), 1u);
    EXPECT_EQ(nm.GetStats().fullEntitySyncs, 2u);

    closesocket(client);
    nm.StopServer();
    nm.Shutdown();
}

TEST(NetworkManager_PolicyKickDropsGameplayQueuedBehindConnectInSameReceivePump)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    ASSERT_TRUE(nm.Initialize());
    ASSERT_TRUE(nm.StartServer(0, 1, NetworkBindScope::LoopbackOnly));

    int connectCallbacks = 0;
    int gameplayCallbacks = 0;
    uint32_t gameplayEntity = 0;
    nm.RegisterHandler(MessageType::Connect,
                       [&](const NetworkMessage& message)
                       {
                           ++connectCallbacks;
                           nm.KickClient(message.senderID, "policy denied");
                       });
    nm.RegisterHandler(MessageType::UserDefined,
                       [&](const NetworkMessage& message)
                       {
                           ++gameplayCallbacks;
                           ReplicatedEntity entity{};
                           entity.ownerID = message.senderID;
                           entity.entityType = "UnauthorizedGameplayEffect";
                           gameplayEntity = nm.RegisterReplicatedEntity(entity);
                       });

    SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(client != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(client));
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(nm.GetBoundPort());
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto sendPacket = [&](const std::vector<uint8_t>& packet)
    {
        return sendto(client, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                      reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress));
    };

    const auto connect = BuildWireMessage(MessageType::Connect, ChannelType::Reliable, INVALID_CLIENT);
    const auto gameplay = BuildWireMessage(MessageType::UserDefined, ChannelType::Unreliable, INVALID_CLIENT,
                                           std::vector<uint8_t>{'g', 'a', 'm', 'e', 'p', 'l', 'a', 'y'});
    const uint64_t droppedBefore = NetworkManagerClientIdTestAccess::DroppedIncomingMessages(nm);
    ASSERT_EQ(sendPacket(connect), static_cast<int>(connect.size()));
    ASSERT_EQ(sendPacket(gameplay), static_cast<int>(gameplay.size()));

    for (int i = 0; i < 50 && connectCallbacks == 0; ++i)
    {
        nm.Update(0.01f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    nm.Update(0.01f);

    EXPECT_EQ(connectCallbacks, 1);
    EXPECT_TRUE(nm.GetClients().empty());
    EXPECT_EQ(gameplayCallbacks, 0);
    EXPECT_EQ(gameplayEntity, static_cast<uint32_t>(0));
    EXPECT_EQ(nm.GetStats().fullEntitySyncs, 0u);
    EXPECT_EQ(NetworkManagerClientIdTestAccess::DroppedIncomingMessages(nm), droppedBefore + 1);

    closesocket(client);
    nm.StopServer();
    nm.Shutdown();
}

TEST(NetworkManager_ConcurrentLoopbackQueriesExerciseAdmittedAddressMapDuringStop)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    ASSERT_TRUE(nm.Initialize());
    ASSERT_TRUE(nm.StartServer(0, 1, NetworkBindScope::LoopbackOnly));

    SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(client != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(client));
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(nm.GetBoundPort());
    serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto connect = BuildWireMessage(MessageType::Connect, ChannelType::Reliable, INVALID_CLIENT);
    ASSERT_EQ(sendto(client, reinterpret_cast<const char*>(connect.data()), static_cast<int>(connect.size()), 0,
                     reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)),
              static_cast<int>(connect.size()));
    for (int i = 0; i < 50 && nm.GetClients().empty(); ++i)
    {
        nm.Update(0.01f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_FALSE(nm.GetClients().empty());
    const ClientID admittedClient = nm.GetClients().begin()->first;
    ASSERT_TRUE(nm.IsClientLoopback(admittedClient));

    std::atomic<bool> start{false};
    std::atomic<bool> keepRunning{true};
    std::atomic<uint32_t> realMapQueries{0};
    std::thread reader(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (keepRunning.load(std::memory_order_acquire))
            {
                if (nm.IsClientLoopback(admittedClient))
                    realMapQueries.fetch_add(1, std::memory_order_relaxed);
            }
        });

    start.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (realMapQueries.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool exercisedRealMap = realMapQueries.load(std::memory_order_relaxed) > 0;
    nm.StopServer();
    keepRunning.store(false, std::memory_order_release);
    reader.join();

    EXPECT_TRUE(exercisedRealMap);
    EXPECT_FALSE(nm.IsClientLoopback(admittedClient));
    closesocket(client);
    nm.Shutdown();
}

TEST(NetworkManager_BoundPortRejectsReuseAttacker)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    ASSERT_TRUE(nm.Initialize());
    ASSERT_TRUE(nm.StartServer(0, 4, NetworkBindScope::LoopbackOnly));
    const uint16_t boundPort = nm.GetBoundPort();
    ASSERT_TRUE(boundPort != 0);

    SOCKET attacker = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(attacker != INVALID_SOCKET);
    const int reuse = 1;
    ASSERT_EQ(setsockopt(attacker, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)), 0);

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(boundPort);
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(bind(attacker, reinterpret_cast<const sockaddr*>(&target), sizeof(target)), SOCKET_ERROR);

    closesocket(attacker);
    nm.StopServer();
    nm.Shutdown();
}

TEST(NetworkManager_LocalOnlyReliableMessageNeverReachesRemoteTransport)
{
    const ScopedNetworkBindMode bindMode("all");
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    ASSERT_TRUE(nm.Initialize());

    // TEST-NET-1 is numeric and non-loopback, so this boundary check does not
    // depend on hostname resolution or a configured external IPv4 interface.
    ASSERT_TRUE(nm.Connect("192.0.2.1", 9, "LocalOnlyBoundary"));
    const size_t queuedBefore = NetworkManagerClientIdTestAccess::PendingOutgoingMessages(nm);
    const auto droppedBefore = nm.GetStats().packetsDropped;
    ASSERT_EQ(queuedBefore, static_cast<size_t>(1)); // The ordinary Connect is eligible for remote transport.

    NetworkMessage credential;
    credential.type = MessageType::UserDefined;
    credential.channel = ChannelType::Reliable;
    credential.payload = {'s', 'e', 'c', 'r', 'e', 't'};
    credential.sensitive = true;
    credential.localOnly = true;

    nm.SendMessage(credential);
    EXPECT_EQ(NetworkManagerClientIdTestAccess::PendingOutgoingMessages(nm), queuedBefore);
    EXPECT_EQ(NetworkManagerClientIdTestAccess::PendingClientSensitiveReliableMessages(nm), static_cast<size_t>(0));
    EXPECT_EQ(nm.GetStats().packetsDropped, droppedBefore + 1);

    credential.ClearSensitivePayload();
    nm.Disconnect();
    nm.Shutdown();
}

TEST(NetworkManager_DisconnectDiscardsOwnedDelayedCredentialAndRetainsUnownedTrafficAcrossReconnect)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    InstabilitySimulator::GetInstance().Shutdown();
    ASSERT_TRUE(nm.Initialize());

    SOCKET loopbackReceiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(loopbackReceiver != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(loopbackReceiver));
    ASSERT_TRUE(SetNonBlocking(loopbackReceiver));
    const uint16_t loopbackReceiverPort = BoundPort(loopbackReceiver);
    ASSERT_TRUE(loopbackReceiverPort != 0);

    {
        const ScopedNetworkBindMode loopbackMode("loopback");
        ASSERT_TRUE(nm.Connect("127.0.0.1", loopbackReceiverPort, "DelayedLocalOnly"));
        nm.Update(0.01f); // Flush the ordinary Connect before enabling delay.
        ASSERT_FALSE(ReceiveDatagrams(loopbackReceiver, std::chrono::milliseconds(100)).empty());

        InstabilitySettings settings;
        settings.enabled = true;
        settings.latencyMs = 1000.0f;
        InstabilitySimulator::GetInstance().SetSettings(settings);

        NetworkMessage credential;
        credential.type = MessageType::UserDefined;
        credential.channel = ChannelType::Reliable;
        credential.payload = {'d', 'e', 'l', 'a', 'y', 'e', 'd', '-', 's', 'e', 'c', 'r', 'e', 't'};
        credential.sensitive = true;
        credential.localOnly = true;
        nm.SendMessage(credential);
        nm.Update(0.01f); // Serialize into the instability queue for loopback.
        const std::vector<uint8_t> retainedTraffic{'u', 'n', 'o', 'w', 'n', 'e', 'd', '-',
                                                   't', 'r', 'a', 'f', 'f', 'i', 'c'};
        InstabilitySimulator::GetInstance().QueuePacket(retainedTraffic, 1000.0f);
        ASSERT_EQ(InstabilitySimulator::GetInstance().GetQueuedPacketCount(), static_cast<size_t>(2));
        credential.ClearSensitivePayload();
        nm.Disconnect();
        EXPECT_EQ(InstabilitySimulator::GetInstance().GetQueuedPacketCount(), static_cast<size_t>(1));
        EXPECT_TRUE(InstabilitySimulator::GetInstance().GetSettings().enabled);

        const auto terminalPackets = ReceiveDatagrams(loopbackReceiver, std::chrono::milliseconds(100));
        const bool receivedDisconnect =
            std::any_of(terminalPackets.begin(), terminalPackets.end(),
                        [&](const auto& data)
                        {
                            NetworkMessage message;
                            return NetworkManagerClientIdTestAccess::DeserializeMessageForTest(nm, data, message) &&
                                   message.type == MessageType::Disconnect;
                        });
        EXPECT_TRUE(receivedDisconnect);
    }

    SOCKET reconnectReceiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(reconnectReceiver != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(reconnectReceiver));
    ASSERT_TRUE(SetNonBlocking(reconnectReceiver));
    const uint16_t reconnectReceiverPort = BoundPort(reconnectReceiver);
    ASSERT_TRUE(reconnectReceiverPort != 0);
    ASSERT_NE(reconnectReceiverPort, loopbackReceiverPort);
    closesocket(loopbackReceiver);

    InstabilitySettings noAdditionalDelay;
    noAdditionalDelay.enabled = true;
    InstabilitySimulator::GetInstance().SetSettings(noAdditionalDelay);

    {
        const ScopedNetworkBindMode reconnectMode("loopback");
        ASSERT_TRUE(nm.Connect("127.0.0.1", reconnectReceiverPort, "ChangedDestination"));
        nm.Update(0.01f);
        ASSERT_FALSE(ReceiveDatagrams(reconnectReceiver, std::chrono::milliseconds(100)).empty());
        nm.Update(2.0f);
    }

    const auto afterReconnect = ReceiveDatagrams(reconnectReceiver, std::chrono::milliseconds(150));
    const std::vector<uint8_t> retainedTraffic{'u', 'n', 'o', 'w', 'n', 'e', 'd', '-',
                                               't', 'r', 'a', 'f', 'f', 'i', 'c'};
    const std::vector<uint8_t> discardedSecret{'d', 'e', 'l', 'a', 'y', 'e', 'd', '-', 's', 'e', 'c', 'r', 'e', 't'};
    EXPECT_TRUE(std::any_of(afterReconnect.begin(), afterReconnect.end(),
                            [&](const auto& datagram) { return datagram == retainedTraffic; }));
    EXPECT_FALSE(std::any_of(afterReconnect.begin(), afterReconnect.end(),
                             [&](const auto& datagram) { return ContainsBytes(datagram, discardedSecret); }));
    EXPECT_EQ(InstabilitySimulator::GetInstance().GetQueuedPacketCount(), static_cast<size_t>(0));
    InstabilitySimulator::GetInstance().Shutdown();
    nm.Disconnect();
    closesocket(reconnectReceiver);
    nm.Shutdown();
}

TEST(InstabilitySimulator_DiscardThroughLifecycleIsBounded)
{
    auto& simulator = InstabilitySimulator::GetInstance();
    simulator.Shutdown();
    InstabilitySettings settings;
    settings.enabled = true;
    settings.latencyMs = 250.0f;
    simulator.SetSettings(settings);

    simulator.QueuePacket({'g', 'e', 'n', 'e', 'r', 'i', 'c'}, 1000.0f);
    simulator.QueuePacket({'o', 'l', 'd', '-', 's', 'e', 'c', 'r', 'e', 't'}, 1001.0f, true, 1, 7);
    simulator.QueuePacket({'f', 'u', 't', 'u', 'r', 'e'}, 1002.0f, true, 2, 9);
    ASSERT_EQ(simulator.GetQueuedPacketCount(), static_cast<size_t>(3));

    EXPECT_EQ(simulator.DiscardPacketsThroughLifecycle(7), static_cast<size_t>(1));
    EXPECT_EQ(simulator.GetQueuedPacketCount(), static_cast<size_t>(2));
    EXPECT_TRUE(simulator.GetSettings().enabled);
    EXPECT_EQ(simulator.GetSettings().latencyMs, 250.0f);

    auto remaining = simulator.GetReadyPackets(1002.0f);
    ASSERT_EQ(remaining.size(), static_cast<size_t>(2));
    EXPECT_EQ(remaining[0].lifecycleEpoch, static_cast<uint64_t>(0));
    EXPECT_EQ(remaining[1].lifecycleEpoch, static_cast<uint64_t>(9));
    simulator.Shutdown();
}

TEST(InstabilitySimulator_DisableSecurelyDropsDelayedPacketsBeforeReenable)
{
    auto& simulator = InstabilitySimulator::GetInstance();
    simulator.Shutdown();
    static_assert(std::is_same_v<decltype(simulator.GetSettings()), InstabilitySettings>);

    InstabilitySettings delayed;
    delayed.enabled = true;
    delayed.latencyMs = 1000.0f;
    simulator.SetSettings(delayed);
    simulator.QueuePacket({'s', 't', 'a', 'l', 'e', '-', 's', 'e', 'c', 'r', 'e', 't'}, 1000.0f, true, 7, 9);
    ASSERT_EQ(simulator.GetQueuedPacketCount(), static_cast<size_t>(1));

    InstabilitySettings disabled;
    simulator.SetSettings(disabled);
    EXPECT_EQ(simulator.GetQueuedPacketCount(), static_cast<size_t>(0));
    simulator.QueuePacket({'q', 'u', 'e', 'u', 'e', 'd', '-', 'w', 'h', 'i', 'l', 'e', '-', 'o', 'f', 'f'}, 1000.0f);
    EXPECT_EQ(simulator.GetQueuedPacketCount(), static_cast<size_t>(0));

    InstabilitySettings enabled;
    enabled.enabled = true;
    simulator.SetSettings(enabled);
    EXPECT_TRUE(simulator.GetReadyPackets(2000.0f).empty());
    simulator.Shutdown();
}

TEST(NetworkManager_ClearApplicationHandlersPreservesConnectRejectedTransition)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    ASSERT_TRUE(nm.Initialize());

    SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(server != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(server));

    sockaddr_in serverAddr{};
#ifdef SPARK_PLATFORM_WINDOWS
    int serverAddrLength = sizeof(serverAddr);
#else
    socklen_t serverAddrLength = sizeof(serverAddr);
#endif
    ASSERT_TRUE(getsockname(server, reinterpret_cast<sockaddr*>(&serverAddr), &serverAddrLength) == 0);

    int applicationCallbacks = 0;
    nm.RegisterHandler(MessageType::ConnectRejected, [&](const NetworkMessage&) { ++applicationCallbacks; });
    nm.ClearHandlers();
    ASSERT_TRUE(nm.Connect("127.0.0.1", ntohs(serverAddr.sin_port), "RejectedPlayer"));

    sockaddr_in clientAddr{};
#ifdef SPARK_PLATFORM_WINDOWS
    int clientAddrLength = sizeof(clientAddr);
#else
    socklen_t clientAddrLength = sizeof(clientAddr);
#endif
    std::vector<uint8_t> request(2048);
    int received = -1;
    for (int i = 0; i < 20 && received <= 0; ++i)
    {
        nm.Update(0.016f);
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(server, &readSet);
        timeval timeout{0, 10'000};
#ifdef SPARK_PLATFORM_WINDOWS
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = select(server + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready > 0)
            received = recvfrom(server, reinterpret_cast<char*>(request.data()), static_cast<int>(request.size()), 0,
                                reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLength);
    }
    ASSERT_TRUE(received > 0);

    NetBuffer reason;
    reason.WriteString("Server maintenance");
    const auto rejection = BuildWireMessage(MessageType::ConnectRejected, ChannelType::Reliable, 0, reason.GetData());
    EXPECT_EQ(sendto(server, reinterpret_cast<const char*>(rejection.data()), static_cast<int>(rejection.size()), 0,
                     reinterpret_cast<const sockaddr*>(&clientAddr), clientAddrLength),
              static_cast<int>(rejection.size()));

    for (int i = 0; i < 20 && nm.GetConnectionState() != ConnectionState::Disconnected; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::None));
    EXPECT_EQ(nm.GetLocalClientID(), INVALID_CLIENT);
    EXPECT_EQ(nm.GetLastConnectionError(), "Server maintenance");
    EXPECT_EQ(applicationCallbacks, 0);

    closesocket(server);
    nm.Shutdown();
}

TEST(NetworkManager_ConnectRejectedSecurelyDrainsQueuedAndDelayedLifecycleTraffic)
{
    auto& nm = NetworkManager::GetInstance();
    auto& simulator = InstabilitySimulator::GetInstance();
    nm.Shutdown();
    simulator.Shutdown();
    ASSERT_TRUE(nm.Initialize());

    SOCKET rejectingServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(rejectingServer != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(rejectingServer));

    sockaddr_in rejectingServerAddr{};
#ifdef SPARK_PLATFORM_WINDOWS
    int rejectingServerAddrLength = sizeof(rejectingServerAddr);
#else
    socklen_t rejectingServerAddrLength = sizeof(rejectingServerAddr);
#endif
    ASSERT_EQ(
        getsockname(rejectingServer, reinterpret_cast<sockaddr*>(&rejectingServerAddr), &rejectingServerAddrLength), 0);
    ASSERT_TRUE(nm.Connect("127.0.0.1", ntohs(rejectingServerAddr.sin_port), "RejectedSecrets"));

    sockaddr_in clientAddr{};
#ifdef SPARK_PLATFORM_WINDOWS
    int clientAddrLength = sizeof(clientAddr);
#else
    socklen_t clientAddrLength = sizeof(clientAddr);
#endif
    std::vector<uint8_t> request(2048);
    int received = -1;
    for (int i = 0; i < 20 && received <= 0; ++i)
    {
        nm.Update(0.016f);
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(rejectingServer, &readSet);
        timeval timeout{0, 10'000};
#ifdef SPARK_PLATFORM_WINDOWS
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = select(rejectingServer + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready > 0)
            received =
                recvfrom(rejectingServer, reinterpret_cast<char*>(request.data()), static_cast<int>(request.size()), 0,
                         reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLength);
    }
    ASSERT_TRUE(received > 0);

    InstabilitySettings delayed;
    delayed.enabled = true;
    delayed.latencyMs = 1000.0f;
    simulator.SetSettings(delayed);

    const std::vector<uint8_t> delayedSecretBytes{'d', 'e', 'l', 'a', 'y', 'e', 'd', '-', 'r', 'e', 'j', 'e',
                                                  'c', 't', 'e', 'd', '-', 's', 'e', 'c', 'r', 'e', 't'};
    NetworkMessage delayedSecret;
    delayedSecret.type = MessageType::UserDefined;
    delayedSecret.channel = ChannelType::Reliable;
    delayedSecret.payload = delayedSecretBytes;
    delayedSecret.sensitive = true;
    nm.SendMessage(delayedSecret);
    delayedSecret.ClearSensitivePayload();
    nm.Update(0.01f);
    ASSERT_EQ(simulator.GetQueuedPacketCount(), static_cast<size_t>(1));

    const std::vector<uint8_t> queuedSecretBytes{'q', 'u', 'e', 'u', 'e', 'd', '-', 'r', 'e', 'j', 'e',
                                                 'c', 't', 'e', 'd', '-', 's', 'e', 'c', 'r', 'e', 't'};
    NetworkMessage queuedSecret;
    queuedSecret.type = MessageType::UserDefined;
    queuedSecret.channel = ChannelType::Reliable;
    queuedSecret.payload = queuedSecretBytes;
    queuedSecret.sensitive = true;
    nm.SendMessage(queuedSecret);
    queuedSecret.ClearSensitivePayload();
    ASSERT_EQ(NetworkManagerClientIdTestAccess::PendingOutgoingMessages(nm), static_cast<size_t>(1));
    ASSERT_TRUE(NetworkManagerClientIdTestAccess::PendingClientReliableMessages(nm) >= static_cast<size_t>(1));
    ASSERT_EQ(NetworkManagerClientIdTestAccess::PendingClientSensitiveReliableMessages(nm), static_cast<size_t>(1));

    NetBuffer reason;
    reason.WriteString("Rejected with queued secrets");
    const auto rejection = BuildWireMessage(MessageType::ConnectRejected, ChannelType::Reliable, 0, reason.GetData());
    ASSERT_EQ(sendto(rejectingServer, reinterpret_cast<const char*>(rejection.data()),
                     static_cast<int>(rejection.size()), 0, reinterpret_cast<const sockaddr*>(&clientAddr),
                     clientAddrLength),
              static_cast<int>(rejection.size()));

    for (int i = 0; i < 20 && nm.GetConnectionState() != ConnectionState::Disconnected; ++i)
    {
        nm.Update(0.016f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(NetworkManagerClientIdTestAccess::PendingOutgoingMessages(nm), static_cast<size_t>(0));
    EXPECT_EQ(NetworkManagerClientIdTestAccess::PendingIncomingMessages(nm), static_cast<size_t>(0));
    EXPECT_EQ(NetworkManagerClientIdTestAccess::PendingClientReliableMessages(nm), static_cast<size_t>(0));
    EXPECT_EQ(NetworkManagerClientIdTestAccess::PendingClientSensitiveReliableMessages(nm), static_cast<size_t>(0));
    EXPECT_EQ(simulator.GetQueuedPacketCount(), static_cast<size_t>(0));
    closesocket(rejectingServer);

    InstabilitySettings immediate;
    immediate.enabled = true;
    simulator.SetSettings(immediate);
    SOCKET reconnectServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(reconnectServer != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(reconnectServer));
    ASSERT_TRUE(SetNonBlocking(reconnectServer));
    ASSERT_TRUE(nm.Connect("127.0.0.1", BoundPort(reconnectServer), "CleanReconnect"));
    nm.Update(2.0f);

    const auto afterReconnect = ReceiveDatagrams(reconnectServer, std::chrono::milliseconds(150));
    ASSERT_FALSE(afterReconnect.empty());
    EXPECT_FALSE(std::any_of(afterReconnect.begin(), afterReconnect.end(),
                             [&](const auto& datagram) { return ContainsBytes(datagram, delayedSecretBytes); }));
    EXPECT_FALSE(std::any_of(afterReconnect.begin(), afterReconnect.end(),
                             [&](const auto& datagram) { return ContainsBytes(datagram, queuedSecretBytes); }));

    nm.Disconnect();
    closesocket(reconnectServer);
    simulator.Shutdown();
    nm.Shutdown();
}

TEST(NetworkManager_ConnectRejectedStopsSameBatchRejectedLifecycleDispatch)
{
    auto& nm = NetworkManager::GetInstance();
    auto& simulator = InstabilitySimulator::GetInstance();
    nm.Shutdown();
    simulator.Shutdown();
    ASSERT_TRUE(nm.Initialize());

    SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(server != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(server));

    sockaddr_in serverAddr{};
#ifdef SPARK_PLATFORM_WINDOWS
    int serverAddrLength = sizeof(serverAddr);
#else
    socklen_t serverAddrLength = sizeof(serverAddr);
#endif
    ASSERT_EQ(getsockname(server, reinterpret_cast<sockaddr*>(&serverAddr), &serverAddrLength), 0);

    int rejectionCallbacks = 0;
    nm.RegisterSensitiveHandler(MessageType::ConnectRejected, [&](const NetworkMessage&) { ++rejectionCallbacks; });
    ASSERT_TRUE(nm.Connect("127.0.0.1", ntohs(serverAddr.sin_port), "SameBatchReject"));

    sockaddr_in clientAddr{};
#ifdef SPARK_PLATFORM_WINDOWS
    int clientAddrLength = sizeof(clientAddr);
#else
    socklen_t clientAddrLength = sizeof(clientAddr);
#endif
    std::vector<uint8_t> request(2048);
    int received = -1;
    for (int i = 0; i < 20 && received <= 0; ++i)
    {
        nm.Update(0.016f);
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(server, &readSet);
        timeval timeout{0, 10'000};
#ifdef SPARK_PLATFORM_WINDOWS
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = select(server + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready > 0)
            received = recvfrom(server, reinterpret_cast<char*>(request.data()), static_cast<int>(request.size()), 0,
                                reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLength);
    }
    ASSERT_TRUE(received > 0);

    NetBuffer acceptedReason;
    acceptedReason.WriteString("First rejection wins");
    const auto acceptedRejection =
        BuildWireMessage(MessageType::ConnectRejected, ChannelType::Reliable, 0, acceptedReason.GetData());
    NetBuffer staleReason;
    staleReason.WriteString("Stale rejection must not dispatch");
    const auto staleRejection =
        BuildWireMessage(MessageType::ConnectRejected, ChannelType::Reliable, 0, staleReason.GetData());
    ASSERT_EQ(sendto(server, reinterpret_cast<const char*>(acceptedRejection.data()),
                     static_cast<int>(acceptedRejection.size()), 0, reinterpret_cast<const sockaddr*>(&clientAddr),
                     clientAddrLength),
              static_cast<int>(acceptedRejection.size()));
    for (int i = 0; i < 20 && NetworkManagerClientIdTestAccess::PendingIncomingMessages(nm) < 1; ++i)
    {
        NetworkManagerClientIdTestAccess::ReceiveAvailableIncomingForTest(nm);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(NetworkManagerClientIdTestAccess::PendingIncomingMessages(nm), static_cast<size_t>(1));

    ASSERT_EQ(sendto(server, reinterpret_cast<const char*>(staleRejection.data()),
                     static_cast<int>(staleRejection.size()), 0, reinterpret_cast<const sockaddr*>(&clientAddr),
                     clientAddrLength),
              static_cast<int>(staleRejection.size()));
    for (int i = 0; i < 20 && NetworkManagerClientIdTestAccess::PendingIncomingMessages(nm) < 2; ++i)
    {
        NetworkManagerClientIdTestAccess::ReceiveAvailableIncomingForTest(nm);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(NetworkManagerClientIdTestAccess::PendingIncomingMessages(nm), static_cast<size_t>(2));

    nm.Update(0.016f);
    EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Disconnected));
    EXPECT_EQ(nm.GetLastConnectionError(), "First rejection wins");
    EXPECT_EQ(rejectionCallbacks, 1);
    EXPECT_EQ(NetworkManagerClientIdTestAccess::PendingIncomingMessages(nm), static_cast<size_t>(0));

    closesocket(server);
    SOCKET reconnectServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_TRUE(reconnectServer != INVALID_SOCKET);
    ASSERT_TRUE(BindLoopbackEphemeral(reconnectServer));
    ASSERT_TRUE(nm.Connect("127.0.0.1", BoundPort(reconnectServer), "SameBatchReconnect"));
    nm.Update(0.016f);
    EXPECT_EQ(rejectionCallbacks, 1);

    nm.Disconnect();
    closesocket(reconnectServer);
    simulator.Shutdown();
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
