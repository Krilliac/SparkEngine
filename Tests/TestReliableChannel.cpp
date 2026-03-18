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
