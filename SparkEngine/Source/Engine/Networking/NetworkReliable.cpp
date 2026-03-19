/**
 * @file NetworkReliable.cpp
 * @brief Reliable channel logic for NetworkManager
 *
 * Extracted from NetworkManager.cpp — implements ACK handling, duplicate
 * detection, sequence tracking, ordered delivery, and sequence pruning.
 */

#include "NetworkManager.h"
#include <cstring>

#ifdef SendMessage
#undef SendMessage
#endif

using namespace DirectX;
namespace Spark::Net
{

    // --------------------------------------------------------------------------
    // ACK processing
    // --------------------------------------------------------------------------

    void NetworkManager::HandleAck(const NetworkMessage& msg)
    {
        // ACK payload: [4 bytes ackSequence] [4 bytes ackBitfield]
        if (msg.payload.size() < 8)
            return;

        NetBuffer buf;
        // Reconstruct buffer from payload for reading
        // (payload is already deserialized bytes)
        uint32_t ackSeq = 0;
        uint32_t ackBits = 0;
        std::memcpy(&ackSeq, msg.payload.data(), 4);
        std::memcpy(&ackBits, msg.payload.data() + 4, 4);

        // Compute RTT from the ACKed sequence's original send time (only if not retransmitted)
        auto origIt = m_reliableOriginalSendTime.find(ackSeq);
        auto retryIt = m_retransmitCounts.find(ackSeq);
        if (origIt != m_reliableOriginalSendTime.end())
        {
            // Only use first-send samples for RTT (Karn's algorithm: skip retransmitted)
            if (retryIt == m_retransmitCounts.end() || retryIt->second == 0)
            {
                float sampleRTT = m_serverTime - origIt->second;
                if (sampleRTT > 0.0f && sampleRTT < m_connectionTimeout)
                    UpdateRTTEstimate(sampleRTT);
            }
        }

        // Remove the acknowledged sequence from unack map
        m_unacknowledgedMessages.erase(ackSeq);
        m_reliableOriginalSendTime.erase(ackSeq);
        m_retransmitCounts.erase(ackSeq);

        // Process bitfield: bit N means (ackSeq - 1 - N) is also acknowledged
        for (uint32_t bit = 0; bit < 32; ++bit)
        {
            if (ackBits & (1u << bit))
            {
                SequenceNumber ackedSeq = ackSeq - 1 - bit;
                if (ackedSeq > 0)
                {
                    m_unacknowledgedMessages.erase(ackedSeq);
                    m_reliableOriginalSendTime.erase(ackedSeq);
                    m_retransmitCounts.erase(ackedSeq);
                }
            }
        }
    }

    void NetworkManager::SendAckPacket()
    {
        if (m_remoteSequenceHighest == 0)
            return; // Nothing to acknowledge

        NetworkMessage ack;
        ack.type = MessageType::Ack;
        ack.channel = ChannelType::Unreliable; // ACKs are unreliable (redundancy handles loss)
        ack.senderID = m_localClientID;
        ack.payload.resize(8);
        std::memcpy(ack.payload.data(), &m_remoteSequenceHighest, 4);
        std::memcpy(ack.payload.data() + 4, &m_ackBitfield, 4);

        SendMessage(ack);
    }

    // --------------------------------------------------------------------------
    // Duplicate detection
    // --------------------------------------------------------------------------

    bool NetworkManager::IsDuplicateSequence(SequenceNumber seq) const
    {
        return m_receivedSequences.find(seq) != m_receivedSequences.end();
    }

    void NetworkManager::RecordReceivedSequence(SequenceNumber seq)
    {
        m_receivedSequences[seq] = m_serverTime;

        if (seq > m_remoteSequenceHighest)
        {
            if (m_remoteSequenceHighest == 0)
            {
                // First sequence ever received — no previous sequences to set in bitfield
                m_ackBitfield = 0;
            }
            else
            {
                // Shift bitfield: the old highest becomes bit 0, etc.
                uint32_t shift = seq - m_remoteSequenceHighest;
                if (shift < 32)
                    m_ackBitfield = (m_ackBitfield << shift) | (1u << (shift - 1));
                else
                    m_ackBitfield = 0; // Gap too large, reset bitfield
            }

            m_remoteSequenceHighest = seq;
        }
        else if (seq < m_remoteSequenceHighest)
        {
            // Set the corresponding bit for this older sequence
            uint32_t offset = m_remoteSequenceHighest - seq - 1;
            if (offset < 32)
                m_ackBitfield |= (1u << offset);
        }
    }

    // --------------------------------------------------------------------------
    // Ordered delivery
    // --------------------------------------------------------------------------

    void NetworkManager::FlushOrderedBuffer()
    {
        while (true)
        {
            auto it = m_orderedBuffer.find(m_expectedOrderedSequence);
            if (it == m_orderedBuffer.end())
                break;

            // Dispatch this now-in-order message
            MessageHandler handler;
            {
                std::lock_guard<std::mutex> lock(m_handlerMutex);
                auto handlerIt = m_handlers.find(static_cast<uint16_t>(it->second.type));
                if (handlerIt != m_handlers.end())
                    handler = handlerIt->second;
            }
            if (handler)
                handler(it->second);

            m_orderedBuffer.erase(it);
            m_expectedOrderedSequence++;
        }
    }

    void NetworkManager::PruneReceivedSequences()
    {
        auto it = m_receivedSequences.begin();
        while (it != m_receivedSequences.end())
        {
            if (m_serverTime - it->second > RECEIVED_SEQ_EXPIRY)
                it = m_receivedSequences.erase(it);
            else
                ++it;
        }
    }

} // namespace Spark::Net
