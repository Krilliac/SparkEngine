// TestReliableChannel.cpp - Tests for reliable networking: ACK, duplicate detection, ordered delivery
// First half: standalone mirror implementations of the channel logic (no socket dependency).
// Second half (SPARK_TEST_HAS_NETWORKING): loopback regression tests that drive the real
// NetworkManager server with raw UDP clients to prove reliability state is keyed per peer.

#include "TestFramework.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace TestReliableChannel
{

    using SequenceNumber = uint32_t;

    struct AckState
    {
        SequenceNumber remoteHighest = 0;
        uint32_t ackBitfield = 0;
        std::unordered_map<SequenceNumber, float> receivedSequences;

        void RecordReceived(SequenceNumber seq, float time)
        {
            receivedSequences[seq] = time;

            if (seq > remoteHighest)
            {
                if (remoteHighest == 0)
                {
                    ackBitfield = 0;
                }
                else
                {
                    uint32_t shift = seq - remoteHighest;
                    if (shift < 32)
                        ackBitfield = (ackBitfield << shift) | (1u << (shift - 1));
                    else
                        ackBitfield = 0;
                }
                remoteHighest = seq;
            }
            else if (seq < remoteHighest)
            {
                uint32_t offset = remoteHighest - seq - 1;
                if (offset < 32)
                    ackBitfield |= (1u << offset);
            }
        }

        bool IsDuplicate(SequenceNumber seq) const { return receivedSequences.find(seq) != receivedSequences.end(); }
    };

    struct UnackedTracker
    {
        std::unordered_map<SequenceNumber, bool> unacked;

        void Track(SequenceNumber seq) { unacked[seq] = true; }

        void ProcessAck(SequenceNumber ackSeq, uint32_t ackBits)
        {
            unacked.erase(ackSeq);
            for (uint32_t bit = 0; bit < 32; ++bit)
            {
                if (ackBits & (1u << bit))
                {
                    SequenceNumber ackedSeq = ackSeq - 1 - bit;
                    if (ackedSeq > 0)
                        unacked.erase(ackedSeq);
                }
            }
        }
    };

    struct OrderedBuffer
    {
        SequenceNumber expectedNext = 1;
        std::unordered_map<SequenceNumber, int> buffered;
        std::vector<int> delivered;

        bool Receive(SequenceNumber seq, int payload)
        {
            if (seq == expectedNext)
            {
                delivered.push_back(payload);
                expectedNext++;
                while (buffered.count(expectedNext))
                {
                    delivered.push_back(buffered[expectedNext]);
                    buffered.erase(expectedNext);
                    expectedNext++;
                }
                return true;
            }
            else
            {
                buffered[seq] = payload;
                return false;
            }
        }
    };

    struct RetransmitTracker
    {
        struct PendingMessage
        {
            SequenceNumber seq = 0;
            float sendTime = 0.0f;
            int retryCount = 0;
            float lastSendTime = 0.0f;
        };

        float baseInterval = 0.5f;
        int maxRetries = 10;
        std::unordered_map<SequenceNumber, PendingMessage> pending;
        std::vector<SequenceNumber> dropped;

        void Track(SequenceNumber seq, float time) { pending[seq] = {seq, time, 0, time}; }

        void Acknowledge(SequenceNumber seq) { pending.erase(seq); }

        float GetBackoffInterval(int retryCount) const
        {
            int exponent = std::min(retryCount, 3);
            return baseInterval * static_cast<float>(1 << exponent);
        }

        std::vector<SequenceNumber> GetRetransmissions(float currentTime)
        {
            std::vector<SequenceNumber> retransmits;
            for (auto& [seq, msg] : pending)
            {
                float elapsed = currentTime - msg.lastSendTime;
                float interval = GetBackoffInterval(msg.retryCount);
                if (elapsed >= interval)
                {
                    msg.retryCount++;
                    if (msg.retryCount > maxRetries)
                    {
                        dropped.push_back(seq);
                        continue;
                    }
                    msg.lastSendTime = currentTime;
                    retransmits.push_back(seq);
                }
            }
            for (SequenceNumber seq : dropped)
                pending.erase(seq);
            return retransmits;
        }
    };

} // namespace TestReliableChannel

using namespace TestReliableChannel;

TEST(AckBitfield_TracksHighestSequence)
{
    AckState state;
    state.RecordReceived(1, 0.0f);
    EXPECT_EQ(state.remoteHighest, 1u);
    EXPECT_EQ(state.ackBitfield, 0u);

    state.RecordReceived(2, 0.1f);
    EXPECT_EQ(state.remoteHighest, 2u);
    EXPECT_EQ(state.ackBitfield & 1u, 1u);

    state.RecordReceived(3, 0.2f);
    EXPECT_EQ(state.remoteHighest, 3u);
    EXPECT_EQ(state.ackBitfield & 0x3u, 0x3u);
}

TEST(AckBitfield_HandlesOutOfOrder)
{
    AckState state;
    state.RecordReceived(1, 0.0f);
    state.RecordReceived(3, 0.1f);
    EXPECT_EQ(state.remoteHighest, 3u);

    state.RecordReceived(2, 0.15f);
    EXPECT_EQ(state.remoteHighest, 3u);
    EXPECT_EQ(state.ackBitfield & 0x3u, 0x3u);
}

TEST(AckBitfield_HandlesLargeGaps)
{
    AckState state;
    state.RecordReceived(1, 0.0f);
    state.RecordReceived(50, 1.0f);
    EXPECT_EQ(state.remoteHighest, 50u);
    EXPECT_EQ(state.ackBitfield, 0u);
}

TEST(DuplicateDetection_IdentifiesRepeats)
{
    AckState state;
    state.RecordReceived(5, 1.0f);
    EXPECT_TRUE(state.IsDuplicate(5));
    EXPECT_FALSE(state.IsDuplicate(6));

    state.RecordReceived(10, 2.0f);
    EXPECT_TRUE(state.IsDuplicate(5));
    EXPECT_TRUE(state.IsDuplicate(10));
    EXPECT_FALSE(state.IsDuplicate(7));
}

TEST(UnackedTracker_RemovesAcknowledged)
{
    UnackedTracker tracker;
    tracker.Track(1);
    tracker.Track(2);
    tracker.Track(3);

    EXPECT_EQ(tracker.unacked.size(), 3u);

    uint32_t bits = (1u << 0) | (1u << 1);
    tracker.ProcessAck(3, bits);

    EXPECT_TRUE(tracker.unacked.empty());
}

TEST(UnackedTracker_PartialAck)
{
    UnackedTracker tracker;
    tracker.Track(1);
    tracker.Track(2);
    tracker.Track(3);
    tracker.Track(4);

    tracker.ProcessAck(4, 1u);

    EXPECT_EQ(tracker.unacked.size(), 2u);
    EXPECT_EQ(tracker.unacked.count(1), 1u);
    EXPECT_EQ(tracker.unacked.count(2), 1u);
}

TEST(OrderedDelivery_InOrder)
{
    OrderedBuffer buf;
    EXPECT_TRUE(buf.Receive(1, 100));
    EXPECT_TRUE(buf.Receive(2, 200));
    EXPECT_TRUE(buf.Receive(3, 300));

    EXPECT_EQ(buf.delivered.size(), 3u);
    EXPECT_EQ(buf.delivered[0], 100);
    EXPECT_EQ(buf.delivered[1], 200);
    EXPECT_EQ(buf.delivered[2], 300);
}

TEST(OrderedDelivery_OutOfOrderFlush)
{
    OrderedBuffer buf;
    EXPECT_FALSE(buf.Receive(3, 300));
    EXPECT_FALSE(buf.Receive(2, 200));
    EXPECT_TRUE(buf.delivered.empty());

    EXPECT_TRUE(buf.Receive(1, 100));
    EXPECT_EQ(buf.delivered.size(), 3u);
    EXPECT_EQ(buf.delivered[0], 100);
    EXPECT_EQ(buf.delivered[1], 200);
    EXPECT_EQ(buf.delivered[2], 300);
}

TEST(OrderedDelivery_PartialGapFill)
{
    OrderedBuffer buf;
    buf.Receive(1, 10);
    buf.Receive(4, 40);
    buf.Receive(5, 50);

    EXPECT_EQ(buf.delivered.size(), 1u);
    EXPECT_EQ(buf.buffered.size(), 2u);

    buf.Receive(2, 20);
    EXPECT_EQ(buf.delivered.size(), 2u);
    EXPECT_EQ(buf.delivered[1], 20);

    buf.Receive(3, 30);
    EXPECT_EQ(buf.delivered.size(), 5u);
    EXPECT_EQ(buf.delivered[2], 30);
    EXPECT_EQ(buf.delivered[3], 40);
    EXPECT_EQ(buf.delivered[4], 50);
}

// =============================================================================
// New tests: ACK bitmask encoding, retransmission, backoff, duplicate rejection
// =============================================================================

TEST(ReliableChannel_ACKBitmaskEncoding)
{
    AckState state;
    // Receive sequences 1, 2, 4, 5 (gap at 3)
    state.RecordReceived(1, 0.0f);
    state.RecordReceived(2, 0.1f);
    state.RecordReceived(4, 0.2f);
    state.RecordReceived(5, 0.3f);

    EXPECT_EQ(state.remoteHighest, 5u);
    // Bit 0 = seq 4 (received), bit 1 = seq 3 (NOT received), bit 2 = seq 2, bit 3 = seq 1
    // Bitfield should have bits 0, 2, 3 set = 0b1101 = 13
    EXPECT_TRUE((state.ackBitfield & (1u << 0)) != 0);  // seq 4
    EXPECT_FALSE((state.ackBitfield & (1u << 1)) != 0); // seq 3 missing
    EXPECT_TRUE((state.ackBitfield & (1u << 2)) != 0);  // seq 2
    EXPECT_TRUE((state.ackBitfield & (1u << 3)) != 0);  // seq 1
}

TEST(ReliableChannel_RetransmitAfterTimeout)
{
    RetransmitTracker tracker;
    tracker.baseInterval = 0.5f;

    // Track a message at time 0
    tracker.Track(1, 0.0f);
    EXPECT_EQ(tracker.pending.size(), 1u);

    // Before timeout — no retransmission
    auto retransmits = tracker.GetRetransmissions(0.3f);
    EXPECT_TRUE(retransmits.empty());

    // After timeout — should retransmit
    retransmits = tracker.GetRetransmissions(0.6f);
    EXPECT_EQ(retransmits.size(), 1u);
    EXPECT_EQ(retransmits[0], 1u);

    // Acknowledge — removed from pending
    tracker.Acknowledge(1);
    EXPECT_TRUE(tracker.pending.empty());
}

TEST(ReliableChannel_ExponentialBackoff)
{
    RetransmitTracker tracker;
    tracker.baseInterval = 1.0f;

    // Verify exponential backoff intervals
    EXPECT_NEAR(tracker.GetBackoffInterval(0), 1.0f, 0.001f); // 1 * 2^0 = 1
    EXPECT_NEAR(tracker.GetBackoffInterval(1), 2.0f, 0.001f); // 1 * 2^1 = 2
    EXPECT_NEAR(tracker.GetBackoffInterval(2), 4.0f, 0.001f); // 1 * 2^2 = 4
    EXPECT_NEAR(tracker.GetBackoffInterval(3), 8.0f, 0.001f); // 1 * 2^3 = 8
    // Capped at 2^3
    EXPECT_NEAR(tracker.GetBackoffInterval(4), 8.0f, 0.001f);
    EXPECT_NEAR(tracker.GetBackoffInterval(10), 8.0f, 0.001f);
}

TEST(ReliableChannel_DuplicateRejection)
{
    AckState state;
    state.RecordReceived(5, 1.0f);
    EXPECT_TRUE(state.IsDuplicate(5));
    EXPECT_FALSE(state.IsDuplicate(6));

    // Receiving same sequence again should be detected
    state.RecordReceived(5, 2.0f);
    EXPECT_TRUE(state.IsDuplicate(5));

    // Highest should not change
    EXPECT_EQ(state.remoteHighest, 5u);
}

TEST(ReliableChannel_OutOfOrderDelivery)
{
    OrderedBuffer buf;

    // Send packets 1, 3, 2 — should deliver all in order once gap is filled
    buf.Receive(1, 100);
    EXPECT_EQ(buf.delivered.size(), 1u);

    buf.Receive(3, 300);
    EXPECT_EQ(buf.delivered.size(), 1u); // 3 is buffered, waiting for 2

    buf.Receive(2, 200);
    EXPECT_EQ(buf.delivered.size(), 3u); // 2 fills the gap, flushes 2 and 3

    EXPECT_EQ(buf.delivered[0], 100);
    EXPECT_EQ(buf.delivered[1], 200);
    EXPECT_EQ(buf.delivered[2], 300);
}

TEST(ReliableChannel_MaxRetriesExceeded)
{
    RetransmitTracker tracker;
    tracker.baseInterval = 0.1f;
    tracker.maxRetries = 3;

    tracker.Track(42, 0.0f);

    // Simulate retransmissions until max retries exceeded
    float time = 0.0f;
    int retransmitCount = 0;
    for (int i = 0; i < 10; ++i)
    {
        time += 10.0f; // Jump far enough to trigger retransmission
        auto retransmits = tracker.GetRetransmissions(time);
        retransmitCount += static_cast<int>(retransmits.size());
        if (tracker.pending.empty())
            break;
    }

    // Message should have been dropped after maxRetries
    EXPECT_EQ(retransmitCount, tracker.maxRetries);
    EXPECT_TRUE(tracker.pending.empty());
    EXPECT_EQ(tracker.dropped.size(), 1u);
    EXPECT_EQ(tracker.dropped[0], 42u);
}

// =============================================================================
// Per-peer reliability regression tests (real NetworkManager over UDP loopback)
//
// Every client numbers its reliable stream independently from 1. These tests
// drive the production server with two raw UDP clients to prove that the
// receive-side dedup/ordered state and the send-side unacked/ACK state are
// keyed per peer — previously they were server-wide, so client B's seq N was
// dropped as a duplicate of client A's seq N and a merged ACK broadcast made
// B erase messages the server never received (silent permanent loss).
// =============================================================================

#if defined(SPARK_TEST_HAS_NETWORKING) && defined(ENABLE_NETWORKING)

#include "Engine/Networking/NetworkManager.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"
#include <chrono>
#include <thread>

namespace TestReliablePerPeer
{
    namespace Net = Spark::Net;

    constexpr uint32_t kWireMagic = 0x5350524B; // "SPRK"
    static_assert(Net::NETWORK_WIRE_HEADER_SIZE == 23, "version-1 wire header must remain byte-compatible");

    /// Raw UDP endpoint speaking the engine wire format — lets one test process
    /// simulate multiple independent clients against the singleton server.
    class RawUdpClient
    {
      public:
        ~RawUdpClient() { Close(); }

        bool Open(uint16_t serverPort)
        {
            m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (m_socket == INVALID_SOCKET)
                return false;

            sockaddr_in localAddr{};
            localAddr.sin_family = AF_INET;
            localAddr.sin_port = 0;
            localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) != 0)
            {
                closesocket(m_socket);
                m_socket = INVALID_SOCKET;
                return false;
            }

#ifdef SPARK_PLATFORM_WINDOWS
            u_long nonBlocking = 1;
            ioctlsocket(m_socket, FIONBIO, &nonBlocking);
#else
            int flags = fcntl(m_socket, F_GETFL, 0);
            fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
#endif
            std::memset(&m_serverAddr, 0, sizeof(m_serverAddr));
            m_serverAddr.sin_family = AF_INET;
            m_serverAddr.sin_port = htons(serverPort);
            return inet_pton(AF_INET, "127.0.0.1", &m_serverAddr.sin_addr) == 1;
        }

        void Close()
        {
            if (m_socket != INVALID_SOCKET)
            {
                closesocket(m_socket);
                m_socket = INVALID_SOCKET;
            }
        }

        bool Send(Net::MessageType type, Net::ChannelType channel, Net::SequenceNumber sequence,
                  const std::vector<uint8_t>& payload, bool sensitive = false)
        {
            return SendRawChannel(type, static_cast<uint8_t>(channel), sequence, payload, sensitive);
        }

        bool SendRawChannel(Net::MessageType type, uint8_t wireChannel, Net::SequenceNumber sequence,
                            const std::vector<uint8_t>& payload, bool sensitive = false)
        {
            Net::NetBuffer buf;
            const auto clearSensitiveWire = Spark::MakeScopeExit(
                [&buf, sensitive]
                {
                    if (sensitive)
                        buf.SecureReset();
                });
            buf.WriteUint32(kWireMagic);
            buf.WriteUint16(static_cast<uint16_t>(type));
            // `sensitive` controls only sender-local buffer erasure. It must
            // never alter this version-1 wire byte.
            buf.WriteUint8(wireChannel);
            buf.WriteUint32(m_clientID);
            buf.WriteUint32(sequence);
            buf.WriteFloat(0.0f); // timestamp
            buf.WriteUint32(static_cast<uint32_t>(payload.size()));
            if (!payload.empty())
                buf.WriteBytes(payload.data(), payload.size());

            const auto& data = buf.GetData();
            const int sent =
                ::sendto(m_socket, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                         reinterpret_cast<const sockaddr*>(&m_serverAddr), sizeof(m_serverAddr));
            return sent == static_cast<int>(data.size());
        }

        /// Non-blocking receive of one engine-format message. False = nothing waiting.
        bool TryReceive(Net::NetworkMessage& outMsg)
        {
            outMsg.ClearSensitivePayload();
            std::vector<uint8_t> raw(Net::MAX_UDP_WIRE_DATAGRAM_SIZE);
            const auto clearRaw = Spark::MakeScopeExit([&raw] { Spark::SecureClear(raw); });
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int received = ::recvfrom(m_socket, reinterpret_cast<char*>(raw.data()), static_cast<int>(raw.size()), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (received < static_cast<int>(Net::NETWORK_WIRE_HEADER_SIZE))
                return false;
            m_lastWireSize = static_cast<size_t>(received);

            Net::NetBuffer buf;
            const auto clearWireCopy = Spark::MakeScopeExit([&buf] { buf.SecureReset(); });
            buf.WriteBytes(raw.data(), static_cast<size_t>(received));
            if (buf.ReadUint32() != kWireMagic)
                return false;
            outMsg.type = static_cast<Net::MessageType>(buf.ReadUint16());
            const uint8_t rawChannel = buf.ReadUint8();
            m_lastWireChannel = rawChannel;
            if (rawChannel > static_cast<uint8_t>(Net::ChannelType::ReliableOrdered))
                return false;
            outMsg.channel = static_cast<Net::ChannelType>(rawChannel);
            // This raw peer has no locally registered sensitive message types.
            outMsg.sensitive = false;
            outMsg.senderID = buf.ReadUint32();
            outMsg.sequence = buf.ReadUint32();
            outMsg.timestamp = buf.ReadFloat();
            uint32_t payloadLen = buf.ReadUint32();
            if (!Net::IsNetworkPayloadSizeValid(payloadLen) ||
                payloadLen > static_cast<size_t>(received) - Net::NETWORK_WIRE_HEADER_SIZE)
                return false;
            outMsg.payload.resize(payloadLen);
            if (payloadLen > 0)
                buf.ReadBytes(outMsg.payload.data(), payloadLen);
            return buf.IsValid();
        }

        /// Handshake: send Connect and pump the server until ConnectAccepted arrives.
        bool Connect(Net::NetworkManager& server, const std::string& name)
        {
            Net::NetBuffer payload;
            payload.WriteString(name);
            for (int attempt = 0; attempt < 50; ++attempt)
            {
                Send(Net::MessageType::Connect, Net::ChannelType::Reliable, 0, payload.GetData());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                server.Update(0.01f);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));

                Net::NetworkMessage msg;
                while (TryReceive(msg))
                {
                    if (msg.type == Net::MessageType::ConnectAccepted && msg.payload.size() >= 4)
                    {
                        Net::NetBuffer acceptBuf;
                        acceptBuf.WriteBytes(msg.payload.data(), msg.payload.size());
                        m_clientID = acceptBuf.ReadUint32();
                        return true;
                    }
                }
            }
            return false;
        }

        /// Drain the socket; count messages of the given type and record the last sequence seen.
        int DrainCount(Net::MessageType type, Net::SequenceNumber& outLastSequence)
        {
            int count = 0;
            Net::NetworkMessage msg;
            while (TryReceive(msg))
            {
                if (msg.type == type)
                {
                    ++count;
                    outLastSequence = msg.sequence;
                }
            }
            return count;
        }

        Net::ClientID GetClientID() const { return m_clientID; }
        uint8_t GetLastWireChannel() const { return m_lastWireChannel; }
        size_t GetLastWireSize() const { return m_lastWireSize; }

      private:
        SOCKET m_socket = INVALID_SOCKET;
        sockaddr_in m_serverAddr{};
        Net::ClientID m_clientID = 0;
        uint8_t m_lastWireChannel = 0xFF;
        size_t m_lastWireSize = 0;
    };

    /// Pump the server with real-time gaps so loopback datagrams land.
    inline void PumpServer(Net::NetworkManager& server, int frames, float deltaTime = 0.01f)
    {
        for (int i = 0; i < frames; ++i)
        {
            server.Update(deltaTime);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
} // namespace TestReliablePerPeer

using namespace TestReliablePerPeer;

TEST(ReliablePerPeer_TwoClientsSameSequences_BothDispatched)
{
    auto& nm = Net::NetworkManager::GetInstance();
    nm.Shutdown();
    EXPECT_TRUE(nm.StartServer(28451, 8));

    std::unordered_map<Net::ClientID, int> receivedBySender;
    nm.RegisterHandler(Net::MessageType::ChatMessage,
                       [&receivedBySender](const Net::NetworkMessage& msg) { receivedBySender[msg.senderID]++; });

    RawUdpClient clientA;
    RawUdpClient clientB;
    EXPECT_TRUE(clientA.Open(28451));
    EXPECT_TRUE(clientB.Open(28451));
    EXPECT_TRUE(clientA.Connect(nm, "ClientA"));
    EXPECT_TRUE(clientB.Connect(nm, "ClientB"));
    EXPECT_NE(clientA.GetClientID(), clientB.GetClientID());

    // Both clients number their reliable streams identically from 1 — the
    // exact collision that server-wide receive state silently dropped.
    const std::vector<uint8_t> chat{'h', 'i'};
    for (Net::SequenceNumber seq = 1; seq <= 3; ++seq)
    {
        clientA.Send(Net::MessageType::ChatMessage, Net::ChannelType::Reliable, seq, chat);
        clientB.Send(Net::MessageType::ChatMessage, Net::ChannelType::Reliable, seq, chat);
    }
    PumpServer(nm, 20);

    EXPECT_EQ(receivedBySender[clientA.GetClientID()], 3);
    EXPECT_EQ(receivedBySender[clientB.GetClientID()], 3);

    nm.Shutdown();
}

TEST(ReliablePerPeer_OrderedChannelIndependentPerPeer)
{
    auto& nm = Net::NetworkManager::GetInstance();
    nm.Shutdown();
    EXPECT_TRUE(nm.StartServer(28452, 8));

    std::unordered_map<Net::ClientID, std::vector<uint8_t>> deliveredBySender;
    nm.RegisterHandler(Net::MessageType::UserDefined, [&deliveredBySender](const Net::NetworkMessage& msg)
                       { deliveredBySender[msg.senderID].push_back(msg.payload.empty() ? 0 : msg.payload[0]); });

    RawUdpClient clientA;
    RawUdpClient clientB;
    EXPECT_TRUE(clientA.Open(28452));
    EXPECT_TRUE(clientB.Open(28452));
    EXPECT_TRUE(clientA.Connect(nm, "ClientA"));
    EXPECT_TRUE(clientB.Connect(nm, "ClientB"));

    // A sends its ordered stream in order; B sends the same sequence numbers
    // out of order. Each peer's reorder buffer and next-expected counter must
    // be independent: B's gap must not stall A, and B's stream must be
    // delivered in order once its seq 1 arrives.
    clientA.Send(Net::MessageType::UserDefined, Net::ChannelType::ReliableOrdered, 1, {1});
    clientA.Send(Net::MessageType::UserDefined, Net::ChannelType::ReliableOrdered, 2, {2});
    clientB.Send(Net::MessageType::UserDefined, Net::ChannelType::ReliableOrdered, 2, {12});
    PumpServer(nm, 10);
    clientB.Send(Net::MessageType::UserDefined, Net::ChannelType::ReliableOrdered, 1, {11});
    PumpServer(nm, 20);

    const auto& fromA = deliveredBySender[clientA.GetClientID()];
    const auto& fromB = deliveredBySender[clientB.GetClientID()];
    EXPECT_EQ(fromA.size(), 2u);
    EXPECT_EQ(fromB.size(), 2u);
    if (fromA.size() == 2)
    {
        EXPECT_EQ(fromA[0], 1);
        EXPECT_EQ(fromA[1], 2);
    }
    if (fromB.size() == 2)
    {
        EXPECT_EQ(fromB[0], 11);
        EXPECT_EQ(fromB[1], 12);
    }

    nm.Shutdown();
}

TEST(ReliablePerPeer_AckFromOnePeerDoesNotClearAnothers)
{
    auto& nm = Net::NetworkManager::GetInstance();
    nm.Shutdown();
    EXPECT_TRUE(nm.StartServer(28453, 8));

    RawUdpClient clientA;
    RawUdpClient clientB;
    EXPECT_TRUE(clientA.Open(28453));
    EXPECT_TRUE(clientB.Open(28453));
    EXPECT_TRUE(clientA.Connect(nm, "ClientA"));
    EXPECT_TRUE(clientB.Connect(nm, "ClientB"));

    // Server reliable broadcast: each peer gets its own sequence stream
    // (seq 1 = ConnectAccepted, seq 2 = this chat message, per peer).
    Net::NetworkMessage chat;
    chat.type = Net::MessageType::ChatMessage;
    chat.channel = Net::ChannelType::Reliable;
    chat.payload = {'y', 'o'};
    nm.SendMessage(chat);
    PumpServer(nm, 6);

    Net::SequenceNumber seqToA = 0;
    Net::SequenceNumber seqToB = 0;
    EXPECT_GE(clientA.DrainCount(Net::MessageType::ChatMessage, seqToA), 1);
    EXPECT_GE(clientB.DrainCount(Net::MessageType::ChatMessage, seqToB), 1);
    EXPECT_EQ(seqToA, 2u);
    EXPECT_EQ(seqToB, 2u);

    // A acknowledges its whole stream: ackSeq=2 with bitfield bit 0 = seq 1.
    // This must clear ONLY A's unacked map — B never acked anything.
    std::vector<uint8_t> ackPayload(8);
    const uint32_t ackSeq = seqToA;
    const uint32_t ackBits = 0x1u;
    std::memcpy(ackPayload.data(), &ackSeq, 4);
    std::memcpy(ackPayload.data() + 4, &ackBits, 4);
    clientA.Send(Net::MessageType::Ack, Net::ChannelType::Unreliable, 0, ackPayload);
    PumpServer(nm, 6);

    // Discard anything already in flight, then advance simulated time past the
    // retransmit backoff (0.5 s base) while staying under the 10 s timeout.
    clientA.DrainCount(Net::MessageType::ChatMessage, seqToA);
    clientB.DrainCount(Net::MessageType::ChatMessage, seqToB);
    PumpServer(nm, 4, 0.4f);

    Net::SequenceNumber ignored = 0;
    const int retransmitsToA = clientA.DrainCount(Net::MessageType::ChatMessage, ignored);
    const int retransmitsToB = clientB.DrainCount(Net::MessageType::ChatMessage, ignored);
    EXPECT_EQ(retransmitsToA, 0); // A acked — its stream is settled
    EXPECT_GE(retransmitsToB, 1); // B's copy must still be tracked and retransmitted

    nm.Shutdown();
}

TEST(NetworkWire_MaxPayload_LoopbackSendPreservesWholeDatagram)
{
    auto& nm = Net::NetworkManager::GetInstance();
    nm.Shutdown();
    EXPECT_TRUE(nm.StartServer(28454, 2));

    RawUdpClient client;
    EXPECT_TRUE(client.Open(28454));
    EXPECT_TRUE(client.Connect(nm, "MaxPayloadClient"));

    Net::NetworkMessage outbound;
    outbound.type = Net::MessageType::UserDefined;
    outbound.channel = Net::ChannelType::Unreliable;
    outbound.payload.resize(Net::MAX_NETWORK_MESSAGE_PAYLOAD_SIZE);
    for (size_t i = 0; i < outbound.payload.size(); ++i)
        outbound.payload[i] = static_cast<uint8_t>(i % 251);

    nm.SendToClient(client.GetClientID(), outbound);

    Net::NetworkMessage received;
    bool found = false;
    for (int attempt = 0; attempt < 100 && !found; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Net::NetworkMessage candidate;
        while (client.TryReceive(candidate))
        {
            if (candidate.type == Net::MessageType::UserDefined)
            {
                received = candidate;
                found = true;
                break;
            }
        }
    }

    EXPECT_TRUE(found);
    EXPECT_EQ(received.payload.size(), Net::MAX_NETWORK_MESSAGE_PAYLOAD_SIZE);
    EXPECT_TRUE(received.payload == outbound.payload);
    nm.Shutdown();
}

TEST(NetworkWire_MaxPlusOneReliableRejectedBeforeSequenceAllocation)
{
    auto& nm = Net::NetworkManager::GetInstance();
    nm.Shutdown();
    EXPECT_TRUE(nm.StartServer(28455, 2));

    RawUdpClient client;
    EXPECT_TRUE(client.Open(28455));
    EXPECT_TRUE(client.Connect(nm, "OversizeClient"));

    const uint64_t droppedBefore = nm.GetStats().packetsDropped;

    Net::NetworkMessage oversize;
    oversize.type = Net::MessageType::UserDefined;
    oversize.channel = Net::ChannelType::Reliable;
    oversize.payload.resize(Net::MAX_NETWORK_MESSAGE_PAYLOAD_SIZE + 1, 0xEE);
    nm.SendToClient(client.GetClientID(), oversize);

    Net::NetworkMessage valid;
    valid.type = Net::MessageType::UserDefined;
    valid.channel = Net::ChannelType::Reliable;
    valid.payload = {0x2A};
    nm.SendToClient(client.GetClientID(), valid);

    Net::NetworkMessage received;
    bool found = false;
    for (int attempt = 0; attempt < 100 && !found; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Net::NetworkMessage candidate;
        while (client.TryReceive(candidate))
        {
            if (candidate.type == Net::MessageType::UserDefined)
            {
                received = candidate;
                found = true;
                break;
            }
        }
    }

    EXPECT_TRUE(found);
    EXPECT_EQ(received.sequence, 2u); // ConnectAccepted consumed sequence 1.
    EXPECT_TRUE(received.payload == valid.payload);
    EXPECT_EQ(nm.GetStats().packetsDropped, droppedBefore + 1);
    nm.Shutdown();
}

TEST(NetworkWire_5KiBReliableLoopbackReceiveIsNotTruncated)
{
    auto& nm = Net::NetworkManager::GetInstance();
    nm.Shutdown();
    EXPECT_TRUE(nm.StartServer(28456, 2));

    RawUdpClient client;
    EXPECT_TRUE(client.Open(28456));
    EXPECT_TRUE(client.Connect(nm, "FiveKiBClient"));

    std::vector<uint8_t> expected(5 * 1024);
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<uint8_t>((i * 7) % 253);

    std::vector<uint8_t> received;
    nm.RegisterHandler(Net::MessageType::UserDefined,
                       [&received](const Net::NetworkMessage& msg) { received = msg.payload; });

    EXPECT_TRUE(client.Send(Net::MessageType::UserDefined, Net::ChannelType::Reliable, 1, expected));
    PumpServer(nm, 20);

    EXPECT_EQ(received.size(), expected.size());
    EXPECT_TRUE(received == expected);
    nm.Shutdown();
}

TEST(NetworkWire_SensitiveOwnershipIsLocalAndHighChannelBitsAreRejected)
{
    auto& nm = Net::NetworkManager::GetInstance();
    nm.Shutdown();
    EXPECT_TRUE(nm.StartServer(28457, 2));

    RawUdpClient client;
    EXPECT_TRUE(client.Open(28457));
    EXPECT_TRUE(client.Connect(nm, "SensitiveOwnershipClient"));

    bool serverSawSensitive = false;
    int serverDispatchCount = 0;
    std::vector<uint8_t> serverPayload;
    nm.RegisterSensitiveHandler(
        Net::MessageType::UserDefined,
        [&serverSawSensitive, &serverDispatchCount, &serverPayload](const Net::NetworkMessage& msg)
        {
            ++serverDispatchCount;
            serverSawSensitive = msg.sensitive;
            serverPayload = msg.payload;
        });

    std::vector<uint8_t> request{'p', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't'};
    // Version 1 reserves every channel value above 2. In particular, the
    // formerly proposed 0x80 sensitivity bit must remain invalid so old and
    // new peers enforce the same 23-byte format.
    EXPECT_TRUE(client.SendRawChannel(Net::MessageType::UserDefined, 0x81, 1, request, true));
    PumpServer(nm, 10);
    EXPECT_EQ(serverDispatchCount, 0);

    // The sender clears its local serialization buffer, while the receiver
    // derives sensitive ownership exclusively from its registered type.
    EXPECT_TRUE(client.Send(Net::MessageType::UserDefined, Net::ChannelType::Reliable, 1, request, true));
    PumpServer(nm, 20);
    EXPECT_EQ(serverDispatchCount, 1);
    EXPECT_TRUE(serverSawSensitive);
    EXPECT_TRUE(serverPayload == request);

    // Classification follows the currently registered local handler. Replacing
    // it with a normal handler must not leave a stale sensitive-type entry.
    bool replacementSawSensitive = true;
    nm.RegisterHandler(Net::MessageType::UserDefined, [&replacementSawSensitive](const Net::NetworkMessage& msg)
                       { replacementSawSensitive = msg.sensitive; });
    EXPECT_TRUE(client.Send(Net::MessageType::UserDefined, Net::ChannelType::Reliable, 2, request, true));
    PumpServer(nm, 20);
    EXPECT_FALSE(replacementSawSensitive);

    Net::NetworkMessage reply;
    reply.type = Net::MessageType::UserDefined;
    reply.channel = Net::ChannelType::Reliable;
    reply.payload = {'r', 'e', 'p', 'l', 'y'};
    reply.sensitive = true;
    nm.SendToClient(client.GetClientID(), reply);

    Net::NetworkMessage received;
    bool found = false;
    for (int attempt = 0; attempt < 100 && !found; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Net::NetworkMessage candidate;
        while (client.TryReceive(candidate))
        {
            if (candidate.type == Net::MessageType::UserDefined)
            {
                received = candidate;
                found = true;
                break;
            }
        }
    }

    EXPECT_TRUE(found);
    EXPECT_EQ(client.GetLastWireChannel(), static_cast<uint8_t>(Net::ChannelType::Reliable));
    EXPECT_EQ(client.GetLastWireSize(), Net::NETWORK_WIRE_HEADER_SIZE + reply.payload.size());
    EXPECT_FALSE(received.sensitive);
    EXPECT_TRUE(received.payload == reply.payload);
    Spark::SecureClear(request);
    Spark::SecureClear(serverPayload);
    nm.Shutdown();
}

#endif // SPARK_TEST_HAS_NETWORKING && ENABLE_NETWORKING
