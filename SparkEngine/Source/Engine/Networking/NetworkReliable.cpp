/**
 * @file NetworkReliable.cpp
 * @brief Reliable channel logic for NetworkManager
 *
 * Extracted from NetworkManager.cpp — implements ACK handling, duplicate
 * detection, sequence tracking, ordered delivery, and sequence pruning.
 *
 * All reliability state is keyed per peer (PeerState): every client numbers
 * its reliable stream independently from 1, so the server must track dedup,
 * ACK bitfields, ordered delivery, and unacked/retransmit maps per client.
 * A client has a single implicit peer — the server — keyed by SERVER_PEER.
 */

#include "NetworkManager.h"
#include "../../Utils/ContainerUtils.h"
#include "../../Utils/Validate.h"
#include <cstring>

#ifdef SendMessage
#undef SendMessage
#endif

using namespace DirectX;
namespace Spark::Net
{

    // --------------------------------------------------------------------------
    // Peer resolution
    // --------------------------------------------------------------------------

    ClientID NetworkManager::GetIncomingPeerKey(const NetworkMessage& msg) const
    {
        // Server: senderID is trusted — ProcessIncoming stamps it from the
        // address→client table, not from the wire. Client: single server peer.
        return (GetRole() == NetworkRole::Server) ? msg.senderID : SERVER_PEER;
    }

    // --------------------------------------------------------------------------
    // ACK processing
    // --------------------------------------------------------------------------

    void NetworkManager::HandleAck(const NetworkMessage& msg)
    {
        // ACK payload: [4 bytes ackSequence] [4 bytes ackBitfield]
        if (msg.payload.size() < 8)
            return;

        uint32_t ackSeq = 0;
        uint32_t ackBits = 0;
        std::memcpy(&ackSeq, msg.payload.data(), 4);
        std::memcpy(&ackBits, msg.payload.data() + 4, 4);

        // Only the sending peer's outgoing stream is covered by this ACK.
        // Erasing from a shared map would let client A's ACK delete messages
        // still in flight to client B — silent permanent loss.
        PeerState& peer = GetPeerState(GetIncomingPeerKey(msg));

        // Compute RTT from the ACKed sequence's original send time (only if not retransmitted)
        auto origIt = peer.reliableOriginalSendTime.find(ackSeq);
        auto retryIt = peer.retransmitCounts.find(ackSeq);
        if (origIt != peer.reliableOriginalSendTime.end())
        {
            // Only use first-send samples for RTT (Karn's algorithm: skip retransmitted)
            if (retryIt == peer.retransmitCounts.end() || retryIt->second == 0)
            {
                float sampleRTT = m_serverTime - origIt->second;
                if (sampleRTT > 0.0f && sampleRTT < m_connectionTimeout)
                    UpdateRTTEstimate(sampleRTT);
            }
        }

        // Remove the acknowledged sequence from this peer's unack map
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "ACK received: seq=%u, bitfield=0x%08X", ackSeq, ackBits);
        peer.unacknowledgedMessages.erase(ackSeq);
        peer.reliableOriginalSendTime.erase(ackSeq);
        peer.retransmitCounts.erase(ackSeq);

        // Process bitfield: bit N means (ackSeq - 1 - N) is also acknowledged
        for (uint32_t bit = 0; bit < 32; ++bit)
        {
            if (ackBits & (1u << bit))
            {
                SequenceNumber ackedSeq = ackSeq - 1 - bit;
                if (ackedSeq > 0)
                {
                    peer.unacknowledgedMessages.erase(ackedSeq);
                    peer.reliableOriginalSendTime.erase(ackedSeq);
                    peer.retransmitCounts.erase(ackedSeq);
                }
            }
        }
    }

    void NetworkManager::SendAckPacket()
    {
        // One ACK per peer, covering only that peer's incoming stream. A merged
        // broadcast ACK would make client B erase unacked messages the server
        // never received just because client A's identically-numbered message
        // arrived.
        const bool isServer = (GetRole() == NetworkRole::Server);
        for (auto& [peerKey, peer] : m_peers)
        {
            if (peer.remoteSequenceHighest == 0)
                continue; // Nothing to acknowledge for this peer

            // Unknown senders (senderID 0 on the server) have no address to
            // ACK back to; their retransmissions age out on their side.
            if (isServer && peerKey == INVALID_CLIENT)
                continue;

            NetworkMessage ack;
            ack.type = MessageType::Ack;
            ack.channel = ChannelType::Unreliable; // ACKs are unreliable (redundancy handles loss)
            ack.senderID = m_localClientID;
            ack.payload.resize(8);
            std::memcpy(ack.payload.data(), &peer.remoteSequenceHighest, 4);
            std::memcpy(ack.payload.data() + 4, &peer.ackBitfield, 4);

            if (isServer)
                SendToClient(peerKey, ack);
            else
                SendMessage(ack);
        }
    }

    // --------------------------------------------------------------------------
    // Duplicate detection
    // --------------------------------------------------------------------------

    bool NetworkManager::IsDuplicateSequence(const PeerState& peer, SequenceNumber seq) const
    {
        bool dup = Spark::ContainerUtils::Contains(peer.receivedSequences, seq);
        if (dup)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Duplicate sequence %u detected — suppressing", seq);
        }
        return dup;
    }

    void NetworkManager::RecordReceivedSequence(PeerState& peer, SequenceNumber seq)
    {
        peer.receivedSequences[seq] = m_serverTime;

        if (seq > peer.remoteSequenceHighest)
        {
            if (peer.remoteSequenceHighest == 0)
            {
                // First sequence ever received — no previous sequences to set in bitfield
                peer.ackBitfield = 0;
            }
            else
            {
                // Shift bitfield: the old highest becomes bit 0, etc.
                uint32_t shift = seq - peer.remoteSequenceHighest;
                if (shift < 32)
                    peer.ackBitfield = (peer.ackBitfield << shift) | (1u << (shift - 1));
                else
                    peer.ackBitfield = 0; // Gap too large, reset bitfield
            }

            peer.remoteSequenceHighest = seq;
        }
        else if (seq < peer.remoteSequenceHighest)
        {
            // Set the corresponding bit for this older sequence
            uint32_t offset = peer.remoteSequenceHighest - seq - 1;
            if (offset < 32)
                peer.ackBitfield |= (1u << offset);
        }
    }

    // --------------------------------------------------------------------------
    // Ordered delivery
    // --------------------------------------------------------------------------

    bool NetworkManager::PopNextOrderedMessage(ClientID peerKey, NetworkMessage& outMessage)
    {
        // Re-resolve on every call: the previously dispatched handler may have
        // mutated m_peers (for example, a Disconnect handler erasing this peer).
        auto peerIt = m_peers.find(peerKey);
        if (peerIt == m_peers.end())
            return false;

        PeerState& peer = peerIt->second;
        auto it = peer.orderedBuffer.find(peer.expectedOrderedSequence);
        if (it == peer.orderedBuffer.end())
            return false;

        outMessage = std::move(it->second);
        peer.orderedBuffer.erase(it);
        peer.expectedOrderedSequence++;
        return true;
    }

    void NetworkManager::PruneReceivedSequences()
    {
        for (auto& [peerKey, peer] : m_peers)
        {
            auto it = peer.receivedSequences.begin();
            while (it != peer.receivedSequences.end())
            {
                if (m_serverTime - it->second > RECEIVED_SEQ_EXPIRY)
                    it = peer.receivedSequences.erase(it);
                else
                    ++it;
            }
        }
    }

} // namespace Spark::Net
