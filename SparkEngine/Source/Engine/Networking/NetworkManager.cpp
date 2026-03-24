/**
 * @file NetworkManager.cpp
 * @brief Core NetworkManager — singleton, update loop, socket helpers, lag compensation
 *
 * Connection lifecycle methods are in NetworkConnection.cpp.
 * Entity replication methods are in NetworkReplication.cpp.
 * Reliable channel logic is in NetworkReliable.cpp.
 * NetBuffer serialization is in NetworkBuffer.cpp.
 */

#include "NetworkManager.h"
#include "../../Utils/Assert.h"
#include "../../Utils/Validate.h"
#include <sstream>
#include <cstring>
#include <algorithm>
#include <thread>

// Windows headers may redefine SendMessage after our includes.
// Undefine it so NetworkManager::SendMessage compiles correctly.
#ifdef SendMessage
#undef SendMessage
#endif

using namespace DirectX;
namespace Spark::Net
{

    // ============================================================================
    // LagCompensator
    // ============================================================================

    void LagCompensator::RecordSnapshot(const HistorySnapshot& snapshot)
    {
        m_history.push_back(snapshot);

        // Prune old snapshots
        float cutoff = snapshot.timestamp - m_maxHistoryDuration;
        m_history.erase(std::remove_if(m_history.begin(), m_history.end(),
                                       [cutoff](const HistorySnapshot& s) { return s.timestamp < cutoff; }),
                        m_history.end());
    }

    bool LagCompensator::RewindToTime(float targetTime, HistorySnapshot& outSnapshot) const
    {
        if (m_history.empty())
            return false;

        // Find the two snapshots bracketing the target time
        const HistorySnapshot* before = nullptr;
        const HistorySnapshot* after = nullptr;

        for (size_t i = 0; i < m_history.size(); ++i)
        {
            if (m_history[i].timestamp <= targetTime)
            {
                before = &m_history[i];
            }
            if (m_history[i].timestamp >= targetTime && !after)
            {
                after = &m_history[i];
            }
        }

        if (!before && !after)
            return false;

        // If we have both, interpolate; otherwise use whichever we have
        if (before && after && before != after)
        {
            float timeDelta = after->timestamp - before->timestamp;
            float t = (timeDelta > 0.0f) ? (targetTime - before->timestamp) / timeDelta : 0.0f;
            t = (std::max)(0.0f, (std::min)(1.0f, t));

            outSnapshot.timestamp = targetTime;
            outSnapshot.entities.clear();

            for (const auto& entityBefore : before->entities)
            {
                for (const auto& entityAfter : after->entities)
                {
                    if (entityBefore.networkID == entityAfter.networkID)
                    {
                        HistorySnapshot::EntityState interpolated;
                        interpolated.networkID = entityBefore.networkID;

                        XMVECTOR pb = XMLoadFloat3(&entityBefore.position);
                        XMVECTOR pa = XMLoadFloat3(&entityAfter.position);
                        XMStoreFloat3(&interpolated.position, XMVectorLerp(pb, pa, t));

                        XMVECTOR rb = XMLoadFloat3(&entityBefore.rotation);
                        XMVECTOR ra = XMLoadFloat3(&entityAfter.rotation);
                        XMStoreFloat3(&interpolated.rotation, XMVectorLerp(rb, ra, t));

                        XMVECTOR bminB = XMLoadFloat3(&entityBefore.boundsMin);
                        XMVECTOR bminA = XMLoadFloat3(&entityAfter.boundsMin);
                        XMStoreFloat3(&interpolated.boundsMin, XMVectorLerp(bminB, bminA, t));

                        XMVECTOR bmaxB = XMLoadFloat3(&entityBefore.boundsMax);
                        XMVECTOR bmaxA = XMLoadFloat3(&entityAfter.boundsMax);
                        XMStoreFloat3(&interpolated.boundsMax, XMVectorLerp(bmaxB, bmaxA, t));

                        outSnapshot.entities.push_back(interpolated);
                        break;
                    }
                }
            }
            return true;
        }
        else if (before)
        {
            outSnapshot = *before;
            return true;
        }
        else if (after)
        {
            outSnapshot = *after;
            return true;
        }

        return false;
    }

    // ============================================================================
    // NetworkManager — Singleton & Lifecycle
    // ============================================================================

    NetworkManager& NetworkManager::GetInstance()
    {
        static NetworkManager instance;
        return instance;
    }

    NetworkManager::~NetworkManager()
    {
        Shutdown();
    }

    // --------------------------------------------------------------------------
    // Socket helpers (ENABLE_NETWORKING only)
    // --------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool NetworkManager::CreateSocket(uint16_t port)
    {
        // Retry socket creation up to 3 times for transient OS-level failures
        constexpr int kMaxSocketRetries = 3;
        for (int attempt = 0; attempt < kMaxSocketRetries; ++attempt)
        {
            m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (m_socket != INVALID_SOCKET)
                break;

            SPARK_LOG_WARN(Spark::LogCategory::Network, "Socket creation attempt %d/%d failed — retrying", attempt + 1,
                           kMaxSocketRetries);
            std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
        }
        if (m_socket == INVALID_SOCKET)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to create UDP socket after %d attempts",
                            kMaxSocketRetries);
            return false;
        }

        // Set non-blocking mode
#ifdef SPARK_PLATFORM_WINDOWS
        u_long nonBlocking = 1;
        if (ioctlsocket(m_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to set socket to non-blocking mode");
            CloseSocket();
            return false;
        }
#else
        int flags = fcntl(m_socket, F_GETFL, 0);
        if (flags == -1 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to set socket to non-blocking mode");
            CloseSocket();
            return false;
        }
#endif // SPARK_PLATFORM_WINDOWS

        // Set socket buffer sizes for game traffic
        int sendBufSize = 65536;
        int recvBufSize = 65536;
        setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBufSize), sizeof(sendBufSize));
        setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBufSize), sizeof(recvBufSize));

        // Bind to the specified port (0 = OS-assigned ephemeral port for clients)
        // If the requested port is in use, try up to 5 consecutive ports
        constexpr int kMaxPortRetries = 5;
        uint16_t bindPort = port;
        bool bound = false;

        for (int attempt = 0; attempt <= kMaxPortRetries; ++attempt)
        {
            sockaddr_in localAddr{};
            localAddr.sin_family = AF_INET;
            localAddr.sin_addr.s_addr = INADDR_ANY;
            localAddr.sin_port = htons(bindPort);

            if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) != SOCKET_ERROR)
            {
                bound = true;
                if (attempt > 0)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Network,
                                   "Port %u was unavailable — bound to fallback port %u instead", port, bindPort);
                }
                break;
            }

            // Port 0 means OS-assigned — no point retrying with port 1
            if (port == 0)
                break;

            ++bindPort;
        }

        if (!bound)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to bind socket to port %u (tried %d ports)", port,
                            kMaxPortRetries + 1);
            CloseSocket();
            return false;
        }

        return true;
    }

    void NetworkManager::CloseSocket()
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

    std::vector<uint8_t> NetworkManager::SerializeMessage(const NetworkMessage& msg) const
    {
        // Wire format (little-endian):
        //   [4] magic 0x5350524B ("SPRK")
        //   [2] message type
        //   [1] channel type
        //   [4] sender ID
        //   [4] sequence number
        //   [4] timestamp (float bits)
        //   [4] payload length
        //   [N] payload bytes

        NetBuffer buf;
        buf.WriteUint32(0x5350524B); // Magic
        buf.WriteUint16(static_cast<uint16_t>(msg.type));
        buf.WriteUint8(static_cast<uint8_t>(msg.channel));
        buf.WriteUint32(msg.senderID);
        buf.WriteUint32(msg.sequence);
        buf.WriteFloat(msg.timestamp);
        buf.WriteUint32(static_cast<uint32_t>(msg.payload.size()));
        if (!msg.payload.empty())
        {
            buf.WriteBytes(msg.payload.data(), msg.payload.size());
        }
        return std::vector<uint8_t>(buf.GetData().begin(), buf.GetData().end());
    }

    bool NetworkManager::DeserializeMessage(const uint8_t* data, size_t length, NetworkMessage& outMsg) const
    {
        // Minimum header: magic(4) + type(2) + channel(1) + sender(4) + seq(4) + timestamp(4) + payloadLen(4) = 23
        if (length < 23)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Packet too small (%zu bytes, need 23 minimum)", length);
            return false;
        }

        NetBuffer buf;
        buf.WriteBytes(data, length);

        uint32_t magic = buf.ReadUint32();
        if (magic != 0x5350524B)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Invalid packet magic 0x%08X (expected 0x5350524B)", magic);
            return false;
        }

        outMsg.type = static_cast<MessageType>(buf.ReadUint16());
        outMsg.channel = static_cast<ChannelType>(buf.ReadUint8());
        outMsg.senderID = buf.ReadUint32();
        outMsg.sequence = buf.ReadUint32();
        outMsg.timestamp = buf.ReadFloat();
        uint32_t payloadLen = buf.ReadUint32();

        if (buf.GetReadPosition() + payloadLen > length)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Payload length %u exceeds remaining packet data", payloadLen);
            return false;
        }

        // Reject unreasonably large payloads to prevent memory exhaustion attacks
        constexpr uint32_t kMaxPayloadSize = 64 * 1024; // 64 KB
        if (payloadLen > kMaxPayloadSize)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Payload length %u exceeds max allowed (%u)", payloadLen,
                           kMaxPayloadSize);
            return false;
        }

        outMsg.payload.resize(payloadLen);
        if (payloadLen > 0)
        {
            buf.ReadBytes(outMsg.payload.data(), payloadLen);
        }

        return true;
    }

    bool NetworkManager::SendRawTo(const std::vector<uint8_t>& data, const sockaddr_in& addr)
    {
        if (m_socket == INVALID_SOCKET || data.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "SendRawTo failed: %s",
                           m_socket == INVALID_SOCKET ? "socket not open" : "empty data");
            return false;
        }

        int sent = sendto(m_socket, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                          reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

        if (sent == SOCKET_ERROR)
        {
            m_stats.packetsDropped++;
            return false;
        }

        m_bytesSentSinceSample += static_cast<uint64_t>(sent);
        m_stats.bytesSent += static_cast<uint64_t>(sent);
        return true;
    }

    int NetworkManager::ReceiveRaw(std::vector<uint8_t>& outData, sockaddr_in& outSender)
    {
        if (m_socket == INVALID_SOCKET)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "ReceiveRaw called with invalid socket");
            return -1;
        }

        static constexpr int MAX_PACKET_SIZE = 4096;
        outData.resize(MAX_PACKET_SIZE);

        socklen_t senderLen = sizeof(outSender);
        int received = recvfrom(m_socket, reinterpret_cast<char*>(outData.data()), MAX_PACKET_SIZE, 0,
                                reinterpret_cast<sockaddr*>(&outSender), &senderLen);

        if (received <= 0)
        {
            outData.clear();
            return received;
        }

        outData.resize(static_cast<size_t>(received));
        m_bytesReceivedSinceSample += static_cast<uint64_t>(received);
        m_stats.bytesReceived += static_cast<uint64_t>(received);
        return received;
    }

#endif // ENABLE_NETWORKING

    // --------------------------------------------------------------------------
    // Update
    // --------------------------------------------------------------------------

    void NetworkManager::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (m_role == NetworkRole::None)
            return;

        SPARK_WARN_IF(Spark::LogCategory::Network, deltaTime < 0.0f, "Negative deltaTime in NetworkManager::Update");
        m_serverTime += deltaTime;

        // Receive from socket and enqueue
        ProcessIncoming();

        // Dispatch queued messages to handlers
        {
            std::queue<NetworkMessage> toDispatch;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                std::swap(toDispatch, m_incomingQueue);
            }

            while (!toDispatch.empty())
            {
                const auto& msg = toDispatch.front();

                // Handle ACK messages internally (not dispatched to user handlers)
                if (msg.type == MessageType::Ack)
                {
                    HandleAck(msg);
                    m_stats.packetsReceived++;
                    toDispatch.pop();
                    continue;
                }

                // Duplicate detection for reliable messages
                if (msg.channel != ChannelType::Unreliable && msg.sequence > 0)
                {
                    if (IsDuplicateSequence(msg.sequence))
                    {
                        // Already received — send ACK again but don't dispatch
                        RecordReceivedSequence(msg.sequence);
                        m_stats.packetsReceived++;
                        toDispatch.pop();
                        continue;
                    }
                    RecordReceivedSequence(msg.sequence);
                }

                // Ordered delivery: buffer out-of-order ReliableOrdered messages
                if (msg.channel == ChannelType::ReliableOrdered && msg.sequence > 0)
                {
                    if (msg.sequence != m_expectedOrderedSequence)
                    {
                        // Buffer for later delivery
                        m_orderedBuffer[msg.sequence] = msg;
                        m_stats.packetsReceived++;
                        toDispatch.pop();
                        continue;
                    }
                    // This is the expected sequence — deliver it, then flush buffer
                    m_expectedOrderedSequence++;
                }

                MessageHandler handler;
                {
                    std::lock_guard<std::mutex> lock(m_handlerMutex);
                    auto it = m_handlers.find(static_cast<uint16_t>(msg.type));
                    if (it != m_handlers.end())
                    {
                        handler = it->second;
                    }
                }
                if (handler)
                {
                    handler(msg);
                }
                else
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Network, "Unknown message type %u — dropping",
                                   static_cast<unsigned>(msg.type));
                }

                // Check channel before popping — msg reference is invalidated by pop()
                bool wasReliableOrdered = (msg.channel == ChannelType::ReliableOrdered);
                m_stats.packetsReceived++;
                toDispatch.pop();

                // After delivering an ordered message, flush any buffered successors
                if (wasReliableOrdered)
                    FlushOrderedBuffer();
            }
        }

        // Send periodic ACK packets
        m_ackSendTimer += deltaTime;
        if (m_ackSendTimer >= ACK_SEND_INTERVAL)
        {
            m_ackSendTimer = 0.0f;
            SendAckPacket();
            PruneReceivedSequences();
        }

        UpdateReplication(deltaTime);
        UpdateHeartbeat(deltaTime);

        // Check for timed-out clients (server) or server timeout (client)
        CheckConnectionTimeouts();

        // Update bandwidth stats every second
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastBandwidthSample).count();
        if (elapsed >= 1000)
        {
            float seconds = static_cast<float>(elapsed) / 1000.0f;
            m_stats.bandwidthUp = static_cast<float>(m_bytesSentSinceSample) / (1024.0f * seconds);
            m_stats.bandwidthDown = static_cast<float>(m_bytesReceivedSinceSample) / (1024.0f * seconds);
            m_bytesSentSinceSample = 0;
            m_bytesReceivedSinceSample = 0;
            m_lastBandwidthSample = now;
        }

        ProcessOutgoing();
    }

    // --------------------------------------------------------------------------
    // Client Input
    // --------------------------------------------------------------------------

    void NetworkManager::SendClientInput(const ClientInputState& input)
    {
        if (m_role != NetworkRole::Client)
            return;

        ClientInputState timestamped = input;
        timestamped.inputSequence = m_inputSequence++;
        timestamped.timestamp = m_serverTime;

        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_inputHistory.push_back(timestamped);

            // Cap history size
            if (m_inputHistory.size() > 120)
                m_inputHistory.erase(m_inputHistory.begin());
        }

        NetworkMessage msg;
        msg.type = MessageType::ClientInput;
        msg.channel = ChannelType::Unreliable;
        msg.senderID = m_localClientID;
        NetBuffer buf;
        buf.WriteUint32(timestamped.inputSequence);
        buf.WriteFloat(timestamped.moveForward);
        buf.WriteFloat(timestamped.moveRight);
        buf.WriteFloat(timestamped.lookYaw);
        buf.WriteFloat(timestamped.lookPitch);
        buf.WriteUint8(static_cast<uint8_t>((timestamped.jump ? 1 : 0) | (timestamped.fire ? 2 : 0) |
                                            (timestamped.reload ? 4 : 0) | (timestamped.sprint ? 8 : 0) |
                                            (timestamped.crouch ? 16 : 0)));
        buf.WriteFloat(timestamped.deltaTime);
        msg.payload = buf.GetData();
        SendMessage(msg);
    }

    // --------------------------------------------------------------------------
    // RTT Estimation (Jacobson/Karels)
    // --------------------------------------------------------------------------

    void NetworkManager::UpdateRTTEstimate(float sampleRTT)
    {
        if (!m_rttInitialized)
        {
            m_smoothedRTT = sampleRTT;
            m_rttVariance = sampleRTT / 2.0f;
            m_rttInitialized = true;
        }
        else
        {
            // RFC 6298: RTTVAR = (1 - beta) * RTTVAR + beta * |SRTT - R|
            //           SRTT  = (1 - alpha) * SRTT  + alpha * R
            constexpr float alpha = 0.125f;
            constexpr float beta = 0.25f;
            float diff = std::abs(m_smoothedRTT - sampleRTT);
            m_rttVariance = (1.0f - beta) * m_rttVariance + beta * diff;
            m_smoothedRTT = (1.0f - alpha) * m_smoothedRTT + alpha * sampleRTT;
        }

        // Update stats
        m_stats.ping = m_smoothedRTT * 1000.0f;
        m_stats.jitter = m_rttVariance * 1000.0f;
    }

    float NetworkManager::GetRetransmitTimeout() const
    {
        if (!m_rttInitialized)
            return m_reliableRetransmitInterval;

        // RTO = SRTT + max(G, 4 * RTTVAR), clamped to [0.2s, 8s]
        float rto = m_smoothedRTT + 4.0f * m_rttVariance;
        return (std::max)(0.2f, (std::min)(rto, 8.0f));
    }

} // namespace Spark::Net
