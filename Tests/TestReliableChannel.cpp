// TestReliableChannel.cpp - Tests for reliable networking: ACK, duplicate detection, ordered delivery
// Standalone implementations for CI testing (no networking/socket dependency)

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
    EXPECT_TRUE(tracker.pending.empty());
    EXPECT_EQ(tracker.dropped.size(), 1u);
    EXPECT_EQ(tracker.dropped[0], 42u);
}
