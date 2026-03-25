/**
 * @file TestNetworkStress.cpp
 * @brief Stress tests and security-focused pentesting for the networking layer
 *
 * Uses real UDP sockets on localhost to exercise the NetworkManager under
 * adversarial conditions: connection flooding, malformed packets, buffer
 * overflow attempts, oversized payloads, rapid connect/disconnect cycling,
 * invalid message types, string length manipulation, spoofed sender IDs,
 * and concurrent client stress.
 *
 * Requires ENABLE_NETWORKING to be defined (CMake: ENABLE_NETWORKING=ON).
 */

#include "TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "../SparkEngine/Source/Engine/Networking/NetworkManager.h"

#ifdef ENABLE_NETWORKING

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace Spark::Net;

// ============================================================================
// Helpers
// ============================================================================

/// Raw UDP socket wrapper for sending crafted packets to the server
class RawUDPSender
{
  public:
    bool Open()
    {
        m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET)
            return false;

            // Non-blocking
#ifdef SPARK_PLATFORM_WINDOWS
        u_long nonBlocking = 1;
        ioctlsocket(m_socket, FIONBIO, &nonBlocking);
#else
        int flags = fcntl(m_socket, F_GETFL, 0);
        fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
#endif

        // Bind to ephemeral port
        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = INADDR_ANY;
        localAddr.sin_port = 0;
        if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR)
        {
            Close();
            return false;
        }
        return true;
    }

    void Close()
    {
        if (m_socket != INVALID_SOCKET)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            closesocket(m_socket);
#else
            close(m_socket);
#endif
            m_socket = INVALID_SOCKET;
        }
    }

    bool SendTo(const void* data, size_t size, uint16_t port)
    {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);

        int sent = sendto(m_socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                          reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        return sent > 0;
    }

    int Receive(void* buf, size_t bufSize)
    {
        sockaddr_in sender{};
        socklen_t senderLen = sizeof(sender);
        return recvfrom(m_socket, reinterpret_cast<char*>(buf), static_cast<int>(bufSize), 0,
                        reinterpret_cast<sockaddr*>(&sender), &senderLen);
    }

    ~RawUDPSender() { Close(); }

  private:
    SOCKET m_socket = INVALID_SOCKET;
};

/// Build a valid SPRK packet from raw components
static std::vector<uint8_t> BuildPacket(MessageType type, ChannelType channel, uint32_t senderID, uint32_t sequence,
                                        float timestamp, const std::vector<uint8_t>& payload)
{
    NetBuffer buf;
    buf.WriteUint32(0x5350524B); // "SPRK" magic
    buf.WriteUint16(static_cast<uint16_t>(type));
    buf.WriteUint8(static_cast<uint8_t>(channel));
    buf.WriteUint32(senderID);
    buf.WriteUint32(sequence);
    buf.WriteFloat(timestamp);
    buf.WriteUint32(static_cast<uint32_t>(payload.size()));
    if (!payload.empty())
    {
        buf.WriteBytes(payload.data(), payload.size());
    }
    return std::vector<uint8_t>(buf.GetData().begin(), buf.GetData().end());
}

/// Build a connect request packet with a player name
static std::vector<uint8_t> BuildConnectPacket(const std::string& playerName)
{
    NetBuffer nameBuf;
    nameBuf.WriteString(playerName);
    return BuildPacket(MessageType::Connect, ChannelType::Reliable, 0, 0, 0.0f,
                       std::vector<uint8_t>(nameBuf.GetData().begin(), nameBuf.GetData().end()));
}

static constexpr uint16_t TEST_PORT_BASE = 29000;
static std::atomic<uint16_t> s_nextPort{TEST_PORT_BASE};

/// Get a unique port for each test to avoid collisions
static uint16_t GetUniquePort()
{
    return s_nextPort.fetch_add(1);
}

/// Start server, run callback, then shut down. Handles cleanup on failure.
static void WithServer(uint16_t port, int maxClients, std::function<void(NetworkManager&)> callback)
{
    auto& nm = NetworkManager::GetInstance();
    nm.Shutdown();
    nm.Initialize();
    bool started = nm.StartServer(port, maxClients);
    if (!started)
    {
        nm.Shutdown();
        return;
    }
    callback(nm);
    nm.StopServer();
    nm.Shutdown();
}

// ============================================================================
// 1. Connection Flooding — DoS Resistance
// ============================================================================

TEST(NetworkStress_ConnectionFlood)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 4,
               [port](NetworkManager& nm)
               {
                   // Flood with 50 connection requests from separate sockets
                   constexpr int kFloodCount = 50;
                   std::vector<RawUDPSender> senders(kFloodCount);

                   int sentCount = 0;
                   for (int i = 0; i < kFloodCount; ++i)
                   {
                       if (!senders[i].Open())
                           continue;
                       auto pkt = BuildConnectPacket("Flood_" + std::to_string(i));
                       if (senders[i].SendTo(pkt.data(), pkt.size(), port))
                           sentCount++;
                   }

                   // Process several frames to handle all the packets
                   for (int frame = 0; frame < 20; ++frame)
                   {
                       nm.Update(0.016f);
                   }

                   // Server should cap at maxClients (4) and not crash
                   auto clientCount = static_cast<int>(nm.GetClients().size());
                   EXPECT_LE(clientCount, 4);
                   EXPECT_GT(sentCount, 0);

                   // Server should still be functional
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
                   EXPECT_EQ(static_cast<int>(nm.GetConnectionState()), static_cast<int>(ConnectionState::Connected));
               });
}

// ============================================================================
// 2. Malformed Packet Fuzzing
// ============================================================================

TEST(NetworkStress_MalformedPackets)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   std::mt19937 rng(42);

                   // Send a variety of malformed packets
                   for (int i = 0; i < 100; ++i)
                   {
                       // Random garbage of random length (1-200 bytes)
                       std::uniform_int_distribution<size_t> sizeDist(1, 200);
                       size_t len = sizeDist(rng);
                       std::vector<uint8_t> garbage(len);
                       for (auto& b : garbage)
                           b = static_cast<uint8_t>(rng() & 0xFF);

                       sender.SendTo(garbage.data(), garbage.size(), port);
                   }

                   // Process frames — server must not crash
                   for (int frame = 0; frame < 10; ++frame)
                   {
                       nm.Update(0.016f);
                   }

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 3. Invalid Magic Number
// ============================================================================

TEST(NetworkStress_InvalidMagic)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Build packets with wrong magic numbers
                   uint32_t badMagics[] = {0x00000000, 0xDEADBEEF, 0xFFFFFFFF, 0x5350524C /* "SPRL" */};

                   for (uint32_t magic : badMagics)
                   {
                       NetBuffer buf;
                       buf.WriteUint32(magic);
                       buf.WriteUint16(static_cast<uint16_t>(MessageType::Connect));
                       buf.WriteUint8(0);
                       buf.WriteUint32(0);
                       buf.WriteUint32(0);
                       buf.WriteFloat(0.0f);
                       buf.WriteUint32(0);
                       auto data = buf.GetData();
                       sender.SendTo(data.data(), data.size(), port);
                   }

                   for (int frame = 0; frame < 5; ++frame)
                       nm.Update(0.016f);

                   // No clients should have been accepted
                   EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 0);
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 4. Truncated Packets (header incomplete)
// ============================================================================

TEST(NetworkStress_TruncatedPackets)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Build a valid packet then send progressive truncations
                   auto fullPacket = BuildConnectPacket("TruncTest");

                   // Send truncated versions: 1 byte, 4 bytes, 10 bytes, 22 bytes (just under min 23)
                   size_t truncations[] = {1, 4, 10, 15, 22};
                   for (size_t len : truncations)
                   {
                       if (len <= fullPacket.size())
                       {
                           sender.SendTo(fullPacket.data(), len, port);
                       }
                   }

                   for (int frame = 0; frame < 5; ++frame)
                       nm.Update(0.016f);

                   // None should result in a connected client
                   EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 0);
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 5. Payload Length Mismatch — claims more data than present
// ============================================================================

TEST(NetworkStress_PayloadLengthOverflow)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Craft a packet that claims a huge payload length but has minimal data
                   NetBuffer buf;
                   buf.WriteUint32(0x5350524B); // SPRK
                   buf.WriteUint16(static_cast<uint16_t>(MessageType::Connect));
                   buf.WriteUint8(0);
                   buf.WriteUint32(0);
                   buf.WriteUint32(0);
                   buf.WriteFloat(0.0f);
                   buf.WriteUint32(0xFFFFFFFF); // Claim 4GB payload
                   // But only send the header
                   auto data = buf.GetData();
                   sender.SendTo(data.data(), data.size(), port);

                   // Also try exactly 64KB+1 (just over the max payload limit)
                   buf.Reset();
                   buf.WriteUint32(0x5350524B);
                   buf.WriteUint16(static_cast<uint16_t>(MessageType::ChatMessage));
                   buf.WriteUint8(0);
                   buf.WriteUint32(1);
                   buf.WriteUint32(1);
                   buf.WriteFloat(0.0f);
                   buf.WriteUint32(65537); // 64KB + 1
                   auto data2 = buf.GetData();
                   sender.SendTo(data2.data(), data2.size(), port);

                   for (int frame = 0; frame < 5; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetClients().size()), 0);
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 6. Oversized UDP Datagram
// ============================================================================

TEST(NetworkStress_OversizedDatagram)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Send a packet larger than the 4096-byte receive buffer
                   std::vector<uint8_t> big(8192, 0xAA);
                   // Put a valid magic at the start so it at least tries to parse
                   big[0] = 0x4B;
                   big[1] = 0x52;
                   big[2] = 0x50;
                   big[3] = 0x53;

                   sender.SendTo(big.data(), big.size(), port);

                   for (int frame = 0; frame < 5; ++frame)
                       nm.Update(0.016f);

                   // Server must survive
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 7. Rapid Connect/Disconnect Cycling
// ============================================================================

TEST(NetworkStress_RapidConnectDisconnect)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   // Rapidly connect and disconnect from many sockets
                   for (int cycle = 0; cycle < 20; ++cycle)
                   {
                       RawUDPSender sender;
                       if (!sender.Open())
                           continue;

                       // Connect
                       auto connectPkt = BuildConnectPacket("Cycler_" + std::to_string(cycle));
                       sender.SendTo(connectPkt.data(), connectPkt.size(), port);

                       nm.Update(0.016f);

                       // Immediately disconnect
                       auto disconnectPkt = BuildPacket(MessageType::Disconnect, ChannelType::Reliable, 0, 0, 0.0f, {});
                       sender.SendTo(disconnectPkt.data(), disconnectPkt.size(), port);

                       nm.Update(0.016f);
                   }

                   // A few more frames to settle
                   for (int frame = 0; frame < 10; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 8. Invalid Message Types
// ============================================================================

TEST(NetworkStress_InvalidMessageTypes)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Send packets with invalid/out-of-range message type values
                   uint16_t badTypes[] = {0, 999, 1001, 5000, 0xFFFF, 50000};
                   for (uint16_t t : badTypes)
                   {
                       NetBuffer buf;
                       buf.WriteUint32(0x5350524B);
                       buf.WriteUint16(t);
                       buf.WriteUint8(0);
                       buf.WriteUint32(0);
                       buf.WriteUint32(0);
                       buf.WriteFloat(0.0f);
                       buf.WriteUint32(0);
                       auto data = buf.GetData();
                       sender.SendTo(data.data(), data.size(), port);
                   }

                   for (int frame = 0; frame < 5; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 9. Invalid Channel Types
// ============================================================================

TEST(NetworkStress_InvalidChannelTypes)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Channel byte values beyond the valid enum range
                   uint8_t badChannels[] = {3, 10, 128, 255};
                   for (uint8_t ch : badChannels)
                   {
                       NetBuffer buf;
                       buf.WriteUint32(0x5350524B);
                       buf.WriteUint16(static_cast<uint16_t>(MessageType::Heartbeat));
                       buf.WriteUint8(ch);
                       buf.WriteUint32(1);
                       buf.WriteUint32(0);
                       buf.WriteFloat(0.0f);
                       buf.WriteUint32(0);
                       auto data = buf.GetData();
                       sender.SendTo(data.data(), data.size(), port);
                   }

                   for (int frame = 0; frame < 5; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 10. Spoofed Sender IDs
// ============================================================================

TEST(NetworkStress_SpoofedSenderIDs)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Send heartbeats claiming to be various fake client IDs
                   uint32_t spoofIDs[] = {0, 1, 100, 0xFFFFFFFF, 999999};
                   for (uint32_t id : spoofIDs)
                   {
                       auto pkt = BuildPacket(MessageType::Heartbeat, ChannelType::Unreliable, id, 0, 0.0f, {});
                       sender.SendTo(pkt.data(), pkt.size(), port);
                   }

                   // Send entity state updates from non-existent clients
                   for (uint32_t id : spoofIDs)
                   {
                       NetBuffer payload;
                       payload.WriteUint32(42);    // Fake entity network ID
                       payload.WriteFloat(100.0f); // x
                       payload.WriteFloat(200.0f); // y
                       payload.WriteFloat(300.0f); // z
                       auto pkt = BuildPacket(MessageType::EntityStateUpdate, ChannelType::Reliable, id, 1, 0.0f,
                                              std::vector<uint8_t>(payload.GetData().begin(), payload.GetData().end()));
                       sender.SendTo(pkt.data(), pkt.size(), port);
                   }

                   for (int frame = 0; frame < 10; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 11. Sequence Number Manipulation
// ============================================================================

TEST(NetworkStress_SequenceManipulation)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // First, legitimately connect
                   auto connectPkt = BuildConnectPacket("SeqTest");
                   sender.SendTo(connectPkt.data(), connectPkt.size(), port);
                   for (int f = 0; f < 3; ++f)
                       nm.Update(0.016f);

                   // Now send packets with extreme sequence numbers
                   uint32_t badSeqs[] = {0, 0xFFFFFFFF, 0x80000000, 1, 0xFFFFFFFE};
                   for (uint32_t seq : badSeqs)
                   {
                       auto pkt = BuildPacket(MessageType::Heartbeat, ChannelType::Reliable, 1, seq, 0.0f, {});
                       sender.SendTo(pkt.data(), pkt.size(), port);
                   }

                   // Duplicate sequence numbers
                   for (int i = 0; i < 10; ++i)
                   {
                       auto pkt = BuildPacket(MessageType::Heartbeat, ChannelType::Reliable, 1, 42, 0.0f, {});
                       sender.SendTo(pkt.data(), pkt.size(), port);
                   }

                   for (int frame = 0; frame < 10; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 12. Timestamp Manipulation
// ============================================================================

TEST(NetworkStress_TimestampManipulation)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Send packets with adversarial timestamp values
                   float badTimestamps[] = {-1.0f,
                                            -1e30f,
                                            std::numeric_limits<float>::infinity(),
                                            -std::numeric_limits<float>::infinity(),
                                            std::numeric_limits<float>::quiet_NaN(),
                                            std::numeric_limits<float>::max(),
                                            std::numeric_limits<float>::lowest(),
                                            0.0f,
                                            1e20f};

                   for (float ts : badTimestamps)
                   {
                       auto pkt = BuildPacket(MessageType::Heartbeat, ChannelType::Unreliable, 0, 0, ts, {});
                       sender.SendTo(pkt.data(), pkt.size(), port);
                   }

                   for (int frame = 0; frame < 5; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 13. Zero-Length and Single-Byte Packets
// ============================================================================

TEST(NetworkStress_MinimalPackets)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Single byte
                   uint8_t one = 0x00;
                   sender.SendTo(&one, 1, port);

                   // All zeros of various lengths
                   for (size_t len = 1; len <= 24; ++len)
                   {
                       std::vector<uint8_t> zeros(len, 0);
                       sender.SendTo(zeros.data(), zeros.size(), port);
                   }

                   // All 0xFF
                   for (size_t len = 1; len <= 24; ++len)
                   {
                       std::vector<uint8_t> ones(len, 0xFF);
                       sender.SendTo(ones.data(), ones.size(), port);
                   }

                   for (int frame = 0; frame < 5; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 14. Concurrent Multi-Client Stress
// ============================================================================

TEST(NetworkStress_ConcurrentClients)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   constexpr int kClientCount = 16;
                   std::atomic<int> connectedCount{0};
                   std::atomic<bool> stopFlag{false};

                   // Spawn threads that each connect and send traffic
                   std::vector<std::thread> threads;
                   threads.reserve(kClientCount);

                   for (int i = 0; i < kClientCount; ++i)
                   {
                       threads.emplace_back(
                           [port, i, &connectedCount, &stopFlag]
                           {
                               RawUDPSender sender;
                               if (!sender.Open())
                                   return;

                               // Connect
                               auto connectPkt = BuildConnectPacket("Client_" + std::to_string(i));
                               if (sender.SendTo(connectPkt.data(), connectPkt.size(), port))
                                   connectedCount.fetch_add(1);

                               // Simulate rapid game traffic until told to stop
                               auto heartbeat =
                                   BuildPacket(MessageType::Heartbeat, ChannelType::Unreliable, 0, 0, 0.0f, {});
                               for (int pkt = 0; pkt < 50 && !stopFlag.load(); ++pkt)
                               {
                                   sender.SendTo(heartbeat.data(), heartbeat.size(), port);
                                   std::this_thread::sleep_for(std::chrono::microseconds(100));
                               }
                           });
                   }

                   // Process server frames while clients are sending
                   for (int frame = 0; frame < 60; ++frame)
                   {
                       nm.Update(0.016f);
                       std::this_thread::sleep_for(std::chrono::milliseconds(8));
                   }

                   // Signal threads to stop and wait for them to finish
                   stopFlag.store(true);
                   for (auto& t : threads)
                       t.join();

                   // Process remaining packets after all threads are done
                   for (int frame = 0; frame < 20; ++frame)
                       nm.Update(0.016f);

                   EXPECT_GT(connectedCount.load(), 0);
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 15. Packet Replay Attack
// ============================================================================

TEST(NetworkStress_ReplayAttack)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Capture a valid connect packet
                   auto connectPkt = BuildConnectPacket("ReplayVictim");
                   sender.SendTo(connectPkt.data(), connectPkt.size(), port);

                   for (int f = 0; f < 3; ++f)
                       nm.Update(0.016f);

                   auto clientsBefore = nm.GetClients().size();

                   // Replay the exact same packet many times
                   for (int i = 0; i < 50; ++i)
                   {
                       sender.SendTo(connectPkt.data(), connectPkt.size(), port);
                   }

                   for (int f = 0; f < 10; ++f)
                       nm.Update(0.016f);

                   // Replayed connects from the same address are now detected
                   // and ignored — only one client should exist
                   auto clientsAfter = nm.GetClients().size();
                   EXPECT_EQ(static_cast<int>(clientsAfter), static_cast<int>(clientsBefore));
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 16. Interleaved Valid and Invalid Traffic
// ============================================================================

TEST(NetworkStress_InterleavedTraffic)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender validSender;
                   RawUDPSender attackSender;
                   EXPECT_TRUE(validSender.Open());
                   EXPECT_TRUE(attackSender.Open());

                   std::mt19937 rng(123);

                   // Interleave valid connections with garbage
                   for (int round = 0; round < 20; ++round)
                   {
                       // Valid connect
                       auto connectPkt = BuildConnectPacket("Valid_" + std::to_string(round));
                       validSender.SendTo(connectPkt.data(), connectPkt.size(), port);

                       // Random garbage
                       std::vector<uint8_t> garbage(64);
                       for (auto& b : garbage)
                           b = static_cast<uint8_t>(rng() & 0xFF);
                       attackSender.SendTo(garbage.data(), garbage.size(), port);

                       nm.Update(0.016f);
                   }

                   for (int frame = 0; frame < 10; ++frame)
                       nm.Update(0.016f);

                   // Valid clients should have connected; garbage should be rejected
                   EXPECT_GT(static_cast<int>(nm.GetClients().size()), 0);
                   EXPECT_LE(static_cast<int>(nm.GetClients().size()), 32);
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 17. NetBuffer Serialization Boundary Tests
// ============================================================================

TEST(NetworkStress_NetBufferBoundaries)
{
    // Test NetBuffer read past end
    {
        NetBuffer buf;
        buf.WriteUint8(42);

        // Read the one byte
        EXPECT_EQ(buf.ReadUint8(), static_cast<uint8_t>(42));
        EXPECT_FALSE(buf.HasError());

        // Read past end — should set error flag, not crash
        buf.ReadUint32();
        EXPECT_TRUE(buf.HasError());
    }

    // Test empty buffer reads
    {
        NetBuffer buf;
        EXPECT_FALSE(buf.HasError());

        buf.ReadUint8();
        EXPECT_TRUE(buf.HasError());
    }

    // Test CanRead
    {
        NetBuffer buf;
        buf.WriteUint32(1234);
        EXPECT_TRUE(buf.CanRead(4));
        EXPECT_FALSE(buf.CanRead(5));
    }

    // Test writing and reading maximum-size string
    {
        NetBuffer buf;
        // A large but not insane string
        std::string largeStr(1000, 'X');
        buf.WriteString(largeStr);
        auto readBack = buf.ReadString();
        EXPECT_EQ(readBack.size(), largeStr.size());
        EXPECT_FALSE(buf.HasError());
    }

    // Test empty string round-trip
    {
        NetBuffer buf;
        buf.WriteString("");
        auto readBack = buf.ReadString();
        EXPECT_EQ(readBack.size(), static_cast<size_t>(0));
        EXPECT_FALSE(buf.HasError());
    }
}

// ============================================================================
// 18. Server Survives High-Rate Traffic Burst
// ============================================================================

TEST(NetworkStress_HighRateBurst)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // First connect legitimately
                   auto connectPkt = BuildConnectPacket("BurstClient");
                   sender.SendTo(connectPkt.data(), connectPkt.size(), port);
                   for (int f = 0; f < 3; ++f)
                       nm.Update(0.016f);

                   // Blast 1000 heartbeats as fast as possible
                   auto heartbeat = BuildPacket(MessageType::Heartbeat, ChannelType::Unreliable, 1, 0, 0.0f, {});
                   for (int i = 0; i < 1000; ++i)
                   {
                       sender.SendTo(heartbeat.data(), heartbeat.size(), port);
                   }

                   // Process — the 256-per-frame cap in ProcessIncoming should prevent stalls
                   for (int frame = 0; frame < 20; ++frame)
                   {
                       nm.Update(0.016f);
                   }

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
                   EXPECT_GT(nm.GetStats().packetsReceived + nm.GetStats().bytesReceived, static_cast<uint64_t>(0));
               });
}

// ============================================================================
// 19. Mixed Message Type Storm
// ============================================================================

TEST(NetworkStress_MixedMessageStorm)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Connect first
                   auto connectPkt = BuildConnectPacket("StormClient");
                   sender.SendTo(connectPkt.data(), connectPkt.size(), port);
                   for (int f = 0; f < 3; ++f)
                       nm.Update(0.016f);

                   // Send every valid message type
                   MessageType allTypes[] = {
                       MessageType::Connect,     MessageType::ConnectAccepted, MessageType::ConnectRejected,
                       MessageType::Disconnect,  MessageType::Heartbeat,       MessageType::Ack,
                       MessageType::EntitySpawn, MessageType::EntityDestroy,   MessageType::EntityStateUpdate,
                       MessageType::EntityRPC,   MessageType::ClientInput,     MessageType::InputAck,
                       MessageType::ChatMessage, MessageType::GameStateSync,   MessageType::MatchStart,
                       MessageType::MatchEnd,    MessageType::PlayerRespawn,   MessageType::ScoreUpdate};

                   for (auto type : allTypes)
                   {
                       // With empty payload
                       auto pkt = BuildPacket(type, ChannelType::Unreliable, 1, 0, 0.0f, {});
                       sender.SendTo(pkt.data(), pkt.size(), port);

                       // With small payload
                       std::vector<uint8_t> smallPayload = {1, 2, 3, 4};
                       auto pkt2 = BuildPacket(type, ChannelType::Reliable, 1, 0, 0.0f, smallPayload);
                       sender.SendTo(pkt2.data(), pkt2.size(), port);
                   }

                   for (int frame = 0; frame < 10; ++frame)
                       nm.Update(0.016f);

                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

// ============================================================================
// 20. Server Bandwidth Stats Under Load
// ============================================================================

TEST(NetworkStress_BandwidthTracking)
{
    uint16_t port = GetUniquePort();

    WithServer(port, 32,
               [port](NetworkManager& nm)
               {
                   RawUDPSender sender;
                   EXPECT_TRUE(sender.Open());

                   // Connect
                   auto connectPkt = BuildConnectPacket("BWClient");
                   sender.SendTo(connectPkt.data(), connectPkt.size(), port);
                   for (int f = 0; f < 3; ++f)
                       nm.Update(0.016f);

                   auto statsBefore = nm.GetStats();

                   // Send 100 packets
                   for (int i = 0; i < 100; ++i)
                   {
                       auto heartbeat = BuildPacket(MessageType::Heartbeat, ChannelType::Unreliable, 1, 0, 0.0f, {});
                       sender.SendTo(heartbeat.data(), heartbeat.size(), port);
                   }

                   for (int frame = 0; frame < 10; ++frame)
                       nm.Update(0.016f);

                   auto statsAfter = nm.GetStats();

                   // bytesReceived should have increased
                   EXPECT_GT(statsAfter.bytesReceived, statsBefore.bytesReceived);
                   EXPECT_EQ(static_cast<int>(nm.GetRole()), static_cast<int>(NetworkRole::Server));
               });
}

#else // !ENABLE_NETWORKING

// When networking is compiled out, provide a single test that documents this
TEST(NetworkStress_Skipped)
{
    // Networking stress tests require ENABLE_NETWORKING to be defined.
    // The CMake option ENABLE_NETWORKING=ON (default) provides this.
    EXPECT_TRUE(true);
}

#endif // ENABLE_NETWORKING
