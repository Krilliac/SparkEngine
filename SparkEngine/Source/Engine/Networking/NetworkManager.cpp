/**
 * @file NetworkManager.cpp
 * @brief Networking implementation -- serialization, replication, lag compensation
 *
 * All socket-level code is guarded by ENABLE_NETWORKING.  When the flag is
 * off the engine links against the header-only NetworkManagerStub instead.
 */

#include "NetworkManager.h"
#include <sstream>
#include <cstring>
#include <algorithm>

using namespace DirectX;
namespace Spark::Net
{

    // ============================================================================
    // NetBuffer
    // ============================================================================

    void NetBuffer::WriteUint8(uint8_t val)
    {
        m_data.push_back(val);
    }

    void NetBuffer::WriteUint16(uint16_t val)
    {
        m_data.push_back(static_cast<uint8_t>(val & 0xFF));
        m_data.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    }

    void NetBuffer::WriteUint32(uint32_t val)
    {
        for (int i = 0; i < 4; ++i)
            m_data.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }

    void NetBuffer::WriteFloat(float val)
    {
        uint32_t bits;
        std::memcpy(&bits, &val, sizeof(float));
        WriteUint32(bits);
    }

    void NetBuffer::WriteString(const std::string& val)
    {
        WriteUint16(static_cast<uint16_t>(val.size()));
        for (char c : val)
            m_data.push_back(static_cast<uint8_t>(c));
    }

    void NetBuffer::WriteVector3(const XMFLOAT3& val)
    {
        WriteFloat(val.x);
        WriteFloat(val.y);
        WriteFloat(val.z);
    }

    void NetBuffer::WriteBytes(const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        m_data.insert(m_data.end(), bytes, bytes + size);
    }

    uint8_t NetBuffer::ReadUint8()
    {
        return (m_readPos < m_data.size()) ? m_data[m_readPos++] : 0;
    }

    uint16_t NetBuffer::ReadUint16()
    {
        if (m_readPos + 2 > m_data.size())
            return 0;
        uint16_t val = 0;
        for (int i = 0; i < 2; ++i)
            val |= static_cast<uint16_t>(m_data[m_readPos++]) << (i * 8);
        return val;
    }

    uint32_t NetBuffer::ReadUint32()
    {
        if (m_readPos + 4 > m_data.size())
            return 0;
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i)
            val |= static_cast<uint32_t>(m_data[m_readPos++]) << (i * 8);
        return val;
    }

    float NetBuffer::ReadFloat()
    {
        uint32_t bits = ReadUint32();
        float val;
        std::memcpy(&val, &bits, sizeof(float));
        return val;
    }

    std::string NetBuffer::ReadString()
    {
        uint16_t len = ReadUint16();
        std::string result(len, '\0');
        for (uint16_t i = 0; i < len && m_readPos < m_data.size(); ++i)
            result[i] = static_cast<char>(m_data[m_readPos++]);
        return result;
    }

    XMFLOAT3 NetBuffer::ReadVector3()
    {
        float x = ReadFloat(), y = ReadFloat(), z = ReadFloat();
        return {x, y, z};
    }

    void NetBuffer::ReadBytes(void* data, size_t size)
    {
        auto* bytes = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < size && m_readPos < m_data.size(); ++i)
            bytes[i] = m_data[m_readPos++];
    }

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
    // NetworkManager
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
    // Initialize / Shutdown
    // --------------------------------------------------------------------------

    bool NetworkManager::Initialize()
    {
        if (m_initialized)
            return true;

#ifdef ENABLE_NETWORKING
#ifdef SPARK_PLATFORM_WINDOWS
        WSADATA wsaData{};
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0)
            return false;
#endif // SPARK_PLATFORM_WINDOWS
#endif // ENABLE_NETWORKING

        m_initialized = true;
        m_lastBandwidthSample = std::chrono::steady_clock::now();
        m_bytesSentSinceSample = 0;
        m_bytesReceivedSinceSample = 0;
        m_stats = {};

        // Register built-in message handlers
        RegisterHandler(MessageType::Connect, [this](const NetworkMessage& msg) { HandleConnect(msg); });
        RegisterHandler(MessageType::Disconnect, [this](const NetworkMessage& msg) { HandleDisconnect(msg); });
        RegisterHandler(MessageType::ConnectAccepted,
                        [this](const NetworkMessage& msg)
                        {
                            if (m_role == NetworkRole::Client)
                            {
                                NetBuffer buf;
                                buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                m_localClientID = buf.ReadUint32();
                                m_connectionState = ConnectionState::Connected;
                            }
                        });
        RegisterHandler(MessageType::Heartbeat,
                        [this](const NetworkMessage& msg)
                        {
                            if (m_role == NetworkRole::Server)
                            {
                                auto it = m_clients.find(msg.senderID);
                                if (it != m_clients.end())
                                {
                                    it->second.lastHeartbeatTime = m_serverTime;
                                }
                            }
                        });
        RegisterHandler(MessageType::EntityStateUpdate,
                        [this](const NetworkMessage& msg)
                        {
                            if (m_role == NetworkRole::Client)
                            {
                                NetBuffer buf;
                                buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                DeserializeEntityState(buf);
                            }
                        });
        RegisterHandler(MessageType::ClientInput,
                        [this](const NetworkMessage& msg)
                        {
                            if (m_role == NetworkRole::Server)
                            {
                                NetBuffer buf;
                                buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                ClientInputState input;
                                input.inputSequence = buf.ReadUint32();
                                input.moveForward = buf.ReadFloat();
                                input.moveRight = buf.ReadFloat();
                                input.lookYaw = buf.ReadFloat();
                                input.lookPitch = buf.ReadFloat();
                                uint8_t flags = buf.ReadUint8();
                                input.jump = (flags & 1) != 0;
                                input.fire = (flags & 2) != 0;
                                input.reload = (flags & 4) != 0;
                                input.sprint = (flags & 8) != 0;
                                input.crouch = (flags & 16) != 0;
                                input.deltaTime = buf.ReadFloat();
                                input.timestamp = m_serverTime;
                                m_pendingInputs.push_back(input);
                            }
                        });

        return true;
    }

    void NetworkManager::Shutdown()
    {
        if (!m_initialized)
            return;

        if (m_connectionState != ConnectionState::Disconnected)
        {
            Disconnect();
        }

#ifdef ENABLE_NETWORKING
        CloseSocket();

#ifdef SPARK_PLATFORM_WINDOWS
        WSACleanup();
#endif // SPARK_PLATFORM_WINDOWS

        m_clientAddresses.clear();
#endif // ENABLE_NETWORKING

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            while (!m_outgoingQueue.empty())
                m_outgoingQueue.pop();
            while (!m_incomingQueue.empty())
                m_incomingQueue.pop();
        }

        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            m_handlers.clear();
        }

        m_clients.clear();
        m_replicatedEntities.clear();
        m_pendingInputs.clear();
        m_inputHistory.clear();
        m_unacknowledgedMessages.clear();
        m_lagCompensator.Clear();

        m_role = NetworkRole::None;
        m_connectionState = ConnectionState::Disconnected;
        m_localClientID = INVALID_CLIENT;
        m_serverTime = 0.0f;
        m_nextClientID = 1;
        m_nextNetworkID = 1;
        m_nextOutgoingSequence = 1;
        m_inputSequence = 0;
        m_heartbeatTimer = 0.0f;
        m_replicationTimer = 0.0f;
        m_stats = {};
        m_initialized = false;
    }

    // --------------------------------------------------------------------------
    // Socket helpers (ENABLE_NETWORKING only)
    // --------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool NetworkManager::CreateSocket(uint16_t port)
    {
        m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET)
            return false;

            // Set non-blocking mode
#ifdef SPARK_PLATFORM_WINDOWS
        u_long nonBlocking = 1;
        if (ioctlsocket(m_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
        {
            CloseSocket();
            return false;
        }
#else
        int flags = fcntl(m_socket, F_GETFL, 0);
        if (flags == -1 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) == -1)
        {
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
        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = INADDR_ANY;
        localAddr.sin_port = htons(port);

        if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR)
        {
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
            return false;

        NetBuffer buf;
        buf.WriteBytes(data, length);

        uint32_t magic = buf.ReadUint32();
        if (magic != 0x5350524B)
            return false;

        outMsg.type = static_cast<MessageType>(buf.ReadUint16());
        outMsg.channel = static_cast<ChannelType>(buf.ReadUint8());
        outMsg.senderID = buf.ReadUint32();
        outMsg.sequence = buf.ReadUint32();
        outMsg.timestamp = buf.ReadFloat();
        uint32_t payloadLen = buf.ReadUint32();

        if (buf.GetReadPosition() + payloadLen > length)
            return false;

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
            return false;

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
            return -1;

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
    // StartServer / StopServer
    // --------------------------------------------------------------------------

    bool NetworkManager::StartServer(uint16_t port, int maxClients)
    {
        if (!m_initialized)
        {
            if (!Initialize())
                return false;
        }

#ifdef ENABLE_NETWORKING
        if (!CreateSocket(port))
            return false;
#endif // ENABLE_NETWORKING

        m_role = NetworkRole::Server;
        m_connectionState = ConnectionState::Connected;
        m_maxClients = maxClients;
        m_localClientID = 0; // Server is client 0
        m_serverTime = 0.0f;
        m_heartbeatTimer = 0.0f;
        m_replicationTimer = 0.0f;
        return true;
    }

    void NetworkManager::StopServer()
    {
        if (m_role != NetworkRole::Server)
            return;

        // Notify all connected clients
        NetworkMessage disconnectMsg;
        disconnectMsg.type = MessageType::Disconnect;
        disconnectMsg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString("Server shutting down");
        disconnectMsg.payload = buf.GetData();

        for (const auto& [id, info] : m_clients)
        {
            SendToClient(id, disconnectMsg);
        }

        // Flush the outgoing queue so disconnect messages are sent
        ProcessOutgoing();

#ifdef ENABLE_NETWORKING
        CloseSocket();
        m_clientAddresses.clear();
#endif // ENABLE_NETWORKING

        m_clients.clear();
        m_replicatedEntities.clear();
        m_lagCompensator.Clear();
        m_pendingInputs.clear();
        m_unacknowledgedMessages.clear();

        m_role = NetworkRole::None;
        m_connectionState = ConnectionState::Disconnected;
        m_localClientID = INVALID_CLIENT;
    }

    // --------------------------------------------------------------------------
    // Connect / Disconnect
    // --------------------------------------------------------------------------

    bool NetworkManager::Connect(const std::string& address, uint16_t port, const std::string& playerName)
    {
        if (!m_initialized)
        {
            if (!Initialize())
                return false;
        }

#ifdef ENABLE_NETWORKING
        // Create a client socket on an ephemeral port (0)
        if (!CreateSocket(0))
            return false;

        // Resolve the server address
        std::memset(&m_serverAddress, 0, sizeof(m_serverAddress));
        m_serverAddress.sin_family = AF_INET;
        m_serverAddress.sin_port = htons(port);

        if (inet_pton(AF_INET, address.c_str(), &m_serverAddress.sin_addr) != 1)
        {
            CloseSocket();
            return false;
        }
#endif // ENABLE_NETWORKING

        m_role = NetworkRole::Client;
        m_connectionState = ConnectionState::Connecting;

        // Send connect request
        NetworkMessage connectMsg;
        connectMsg.type = MessageType::Connect;
        connectMsg.channel = ChannelType::Reliable;
        connectMsg.senderID = INVALID_CLIENT;
        connectMsg.timestamp = 0.0f;
        NetBuffer buf;
        buf.WriteString(playerName);
        connectMsg.payload = buf.GetData();
        SendMessage(connectMsg);

        return true;
    }

    void NetworkManager::Disconnect()
    {
        if (m_connectionState == ConnectionState::Disconnected)
            return;

        m_connectionState = ConnectionState::Disconnecting;

        NetworkMessage disconnectMsg;
        disconnectMsg.type = MessageType::Disconnect;
        disconnectMsg.channel = ChannelType::Reliable;
        disconnectMsg.senderID = m_localClientID;
        SendMessage(disconnectMsg);

        // Flush so the disconnect message actually goes out
        ProcessOutgoing();

#ifdef ENABLE_NETWORKING
        CloseSocket();
        m_clientAddresses.clear();
#endif // ENABLE_NETWORKING

        m_connectionState = ConnectionState::Disconnected;
        m_role = NetworkRole::None;
        m_localClientID = INVALID_CLIENT;
        m_clients.clear();
        m_replicatedEntities.clear();
        m_lagCompensator.Clear();
        m_pendingInputs.clear();
        m_inputHistory.clear();
        m_unacknowledgedMessages.clear();
    }

    // --------------------------------------------------------------------------
    // Update
    // --------------------------------------------------------------------------

    void NetworkManager::Update(float deltaTime)
    {
        if (m_role == NetworkRole::None)
            return;

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
                m_stats.packetsReceived++;
                toDispatch.pop();
            }
        }

        UpdateReplication(deltaTime);
        UpdateHeartbeat(deltaTime);

        // Check for timed-out clients (server)
        if (m_role == NetworkRole::Server)
        {
            std::vector<ClientID> timedOut;
            for (const auto& [id, info] : m_clients)
            {
                if (m_serverTime - info.lastHeartbeatTime > m_connectionTimeout)
                {
                    timedOut.push_back(id);
                }
            }
            for (ClientID id : timedOut)
            {
                m_clients.erase(id);
#ifdef ENABLE_NETWORKING
                m_clientAddresses.erase(id);
#endif
            }
        }

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
    // Send helpers
    // --------------------------------------------------------------------------

    void NetworkManager::SendMessage(const NetworkMessage& msg)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        NetworkMessage queued = msg;
        queued.timestamp = m_serverTime;

        // Assign sequence number for reliable messages
        if (queued.channel != ChannelType::Unreliable)
        {
            queued.sequence = m_nextOutgoingSequence++;
        }

        m_outgoingQueue.push(queued);
        m_stats.packetsSent++;
    }

    void NetworkManager::SendToClient(ClientID client, const NetworkMessage& msg)
    {
        if (m_role != NetworkRole::Server)
            return;

        NetworkMessage copy = msg;
        copy.senderID = 0; // From server

#ifdef ENABLE_NETWORKING
        auto addrIt = m_clientAddresses.find(client);
        if (addrIt == m_clientAddresses.end())
            return;

        auto serialized = SerializeMessage(copy);
        SendRawTo(serialized, addrIt->second);
        m_stats.packetsSent++;

        // Track reliable messages for retransmission
        if (copy.channel != ChannelType::Unreliable)
        {
            m_unacknowledgedMessages[copy.sequence] = copy;
        }
#else
        // Without networking, just enqueue for local testing
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outgoingQueue.push(copy);
        m_stats.packetsSent++;
#endif // ENABLE_NETWORKING
    }

    void NetworkManager::SendToAll(const NetworkMessage& msg)
    {
        for (const auto& [id, info] : m_clients)
        {
            SendToClient(id, msg);
        }
    }

    void NetworkManager::SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg)
    {
        for (const auto& [id, info] : m_clients)
        {
            if (id != excludeClient)
                SendToClient(id, msg);
        }
    }

    void NetworkManager::BroadcastMessage(const NetworkMessage& msg)
    {
        SendToAll(msg);
    }

    void NetworkManager::RegisterHandler(MessageType type, MessageHandler handler)
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        m_handlers[static_cast<uint16_t>(type)] = std::move(handler);
    }

    // --------------------------------------------------------------------------
    // Entity Replication
    // --------------------------------------------------------------------------

    uint32_t NetworkManager::RegisterReplicatedEntity(const ReplicatedEntity& entity)
    {
        uint32_t netID = m_nextNetworkID++;
        m_replicatedEntities[netID] = entity;
        m_replicatedEntities[netID].networkID = netID;
        m_replicatedEntities[netID].needsFullSync = true;

        // Notify clients about the new entity (server only)
        if (m_role == NetworkRole::Server)
        {
            NetworkMessage msg;
            msg.type = MessageType::EntitySpawn;
            msg.channel = ChannelType::Reliable;

            NetBuffer buf;
            buf.WriteUint32(netID);
            buf.WriteUint32(entity.ownerID);
            buf.WriteString(entity.entityType);
            buf.WriteVector3(entity.position);
            buf.WriteVector3(entity.rotation);
            msg.payload = buf.GetData();
            SendToAll(msg);
        }

        return netID;
    }

    void NetworkManager::UnregisterReplicatedEntity(uint32_t networkID)
    {
        auto it = m_replicatedEntities.find(networkID);
        if (it == m_replicatedEntities.end())
            return;

        // Notify clients about entity destruction (server only)
        if (m_role == NetworkRole::Server)
        {
            NetworkMessage msg;
            msg.type = MessageType::EntityDestroy;
            msg.channel = ChannelType::Reliable;

            NetBuffer buf;
            buf.WriteUint32(networkID);
            msg.payload = buf.GetData();
            SendToAll(msg);
        }

        m_replicatedEntities.erase(it);
    }

    void NetworkManager::MarkPropertyDirty(uint32_t networkID, const std::string& propertyName)
    {
        auto it = m_replicatedEntities.find(networkID);
        if (it == m_replicatedEntities.end())
            return;
        for (auto& prop : it->second.properties)
        {
            if (prop.name == propertyName)
            {
                prop.dirty = true;
                break;
            }
        }
    }

    ReplicatedEntity* NetworkManager::GetReplicatedEntity(uint32_t networkID)
    {
        auto it = m_replicatedEntities.find(networkID);
        return (it != m_replicatedEntities.end()) ? &it->second : nullptr;
    }

    void NetworkManager::SendFullEntitySync(ClientID targetClient)
    {
        if (m_role != NetworkRole::Server)
            return;

        for (const auto& [netID, entity] : m_replicatedEntities)
        {
            // Send spawn message
            NetworkMessage spawnMsg;
            spawnMsg.type = MessageType::EntitySpawn;
            spawnMsg.channel = ChannelType::Reliable;

            NetBuffer spawnBuf;
            spawnBuf.WriteUint32(netID);
            spawnBuf.WriteUint32(entity.ownerID);
            spawnBuf.WriteString(entity.entityType);
            spawnBuf.WriteVector3(entity.position);
            spawnBuf.WriteVector3(entity.rotation);
            spawnMsg.payload = spawnBuf.GetData();
            SendToClient(targetClient, spawnMsg);

            // Send state update with properties
            NetworkMessage stateMsg;
            stateMsg.type = MessageType::EntityStateUpdate;
            stateMsg.channel = ChannelType::Reliable;

            NetBuffer stateBuf;
            SerializeEntityState(netID, stateBuf);
            stateMsg.payload = stateBuf.GetData();
            SendToClient(targetClient, stateMsg);
        }
    }

    void NetworkManager::SerializeEntityState(uint32_t networkID, NetBuffer& outBuffer) const
    {
        auto it = m_replicatedEntities.find(networkID);
        if (it == m_replicatedEntities.end())
            return;

        const auto& entity = it->second;

        outBuffer.WriteUint32(networkID);
        outBuffer.WriteVector3(entity.position);
        outBuffer.WriteVector3(entity.rotation);
        outBuffer.WriteVector3(entity.velocity);

        // Serialize replicated properties
        uint16_t propCount = 0;
        for (const auto& prop : entity.properties)
        {
            if (prop.dirty || entity.needsFullSync)
                propCount++;
        }
        outBuffer.WriteUint16(propCount);

        for (const auto& prop : entity.properties)
        {
            if (prop.dirty || entity.needsFullSync)
            {
                outBuffer.WriteString(prop.name);
                outBuffer.WriteUint8(static_cast<uint8_t>(prop.type));
                if (prop.serialize)
                {
                    prop.serialize(outBuffer);
                }
            }
        }
    }

    void NetworkManager::DeserializeEntityState(NetBuffer& inBuffer)
    {
        uint32_t networkID = inBuffer.ReadUint32();
        auto it = m_replicatedEntities.find(networkID);
        if (it == m_replicatedEntities.end())
        {
            // Entity not known locally -- create a placeholder
            ReplicatedEntity placeholder;
            placeholder.networkID = networkID;
            placeholder.position = inBuffer.ReadVector3();
            placeholder.rotation = inBuffer.ReadVector3();
            placeholder.velocity = inBuffer.ReadVector3();
            // Skip remaining property data
            uint16_t propCount = inBuffer.ReadUint16();
            for (uint16_t i = 0; i < propCount; ++i)
            {
                inBuffer.ReadString(); // name
                inBuffer.ReadUint8();  // type
                // Cannot deserialize without a handler -- skip
            }
            placeholder.lastUpdateTime = m_serverTime;
            m_replicatedEntities[networkID] = placeholder;
            return;
        }

        auto& entity = it->second;
        entity.position = inBuffer.ReadVector3();
        entity.rotation = inBuffer.ReadVector3();
        entity.velocity = inBuffer.ReadVector3();
        entity.lastUpdateTime = m_serverTime;

        uint16_t propCount = inBuffer.ReadUint16();
        for (uint16_t i = 0; i < propCount; ++i)
        {
            std::string propName = inBuffer.ReadString();
            uint8_t propType = inBuffer.ReadUint8();
            (void)propType;

            // Find matching property and deserialize
            for (auto& prop : entity.properties)
            {
                if (prop.name == propName && prop.deserialize)
                {
                    prop.deserialize(inBuffer);
                    break;
                }
            }
        }
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

        m_inputHistory.push_back(timestamped);

        // Cap history size
        if (m_inputHistory.size() > 120)
            m_inputHistory.erase(m_inputHistory.begin());

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
    // KickClient
    // --------------------------------------------------------------------------

    void NetworkManager::KickClient(ClientID client, const std::string& reason)
    {
        auto it = m_clients.find(client);
        if (it == m_clients.end())
            return;

        NetworkMessage msg;
        msg.type = MessageType::Disconnect;
        msg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString(reason);
        msg.payload = buf.GetData();
        SendToClient(client, msg);

        m_clients.erase(it);
#ifdef ENABLE_NETWORKING
        m_clientAddresses.erase(client);
#endif
    }

    // --------------------------------------------------------------------------
    // ProcessIncoming -- read from socket, parse, enqueue
    // --------------------------------------------------------------------------

    void NetworkManager::ProcessIncoming()
    {
#ifdef ENABLE_NETWORKING
        if (m_socket == INVALID_SOCKET)
            return;

        // Read all available datagrams
        for (int i = 0; i < 256; ++i) // Cap per-frame reads
        {
            std::vector<uint8_t> rawData;
            sockaddr_in senderAddr{};
            int received = ReceiveRaw(rawData, senderAddr);
            if (received <= 0)
                break;

            NetworkMessage msg;
            if (!DeserializeMessage(rawData.data(), rawData.size(), msg))
                continue;

            // On the server, map sender address to a client ID
            if (m_role == NetworkRole::Server)
            {
                // Check if this is a new connection
                if (msg.type == MessageType::Connect)
                {
                    // HandleConnect will assign an ID; we need to map the address
                    // temporarily set senderID to 0 so HandleConnect creates it
                    msg.senderID = INVALID_CLIENT;

                    // Process the connect directly to get the new client ID
                    HandleConnect(msg);

                    // The last assigned client gets this address
                    ClientID newID = m_nextClientID - 1;
                    m_clientAddresses[newID] = senderAddr;

                    // Send the full entity state to the new client
                    SendFullEntitySync(newID);
                    continue;
                }

                // Look up sender by address
                ClientID foundID = INVALID_CLIENT;
                for (const auto& [id, addr] : m_clientAddresses)
                {
                    if (addr.sin_addr.s_addr == senderAddr.sin_addr.s_addr && addr.sin_port == senderAddr.sin_port)
                    {
                        foundID = id;
                        break;
                    }
                }
                msg.senderID = foundID;
            }

            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_incomingQueue.push(msg);
        }
#else
        // Without networking, just dispatch whatever is in the queue already
        // (for local/testing scenarios)
#endif // ENABLE_NETWORKING
    }

    // --------------------------------------------------------------------------
    // ProcessOutgoing -- serialize and send queued messages
    // --------------------------------------------------------------------------

    void NetworkManager::ProcessOutgoing()
    {
        std::queue<NetworkMessage> toSend;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::swap(toSend, m_outgoingQueue);
        }

#ifdef ENABLE_NETWORKING
        while (!toSend.empty())
        {
            const auto& msg = toSend.front();
            auto serialized = SerializeMessage(msg);

            if (m_role == NetworkRole::Client)
            {
                // Client sends everything to the server
                SendRawTo(serialized, m_serverAddress);
            }
            else if (m_role == NetworkRole::Server)
            {
                // Server messages queued via SendMessage (not SendToClient)
                // are broadcast to all clients
                for (const auto& [id, addr] : m_clientAddresses)
                {
                    SendRawTo(serialized, addr);
                }
            }

            // Track reliable messages for retransmission
            if (msg.channel != ChannelType::Unreliable && msg.sequence > 0)
            {
                m_unacknowledgedMessages[msg.sequence] = msg;
            }

            toSend.pop();
        }

        // Retransmit unacknowledged reliable messages
        std::vector<SequenceNumber> toRetransmit;
        for (auto& [seq, unacked] : m_unacknowledgedMessages)
        {
            float age = m_serverTime - unacked.timestamp;
            if (age > m_reliableRetransmitInterval)
            {
                toRetransmit.push_back(seq);
            }
        }

        for (SequenceNumber seq : toRetransmit)
        {
            auto it = m_unacknowledgedMessages.find(seq);
            if (it == m_unacknowledgedMessages.end())
                continue;

            auto& retransmitMsg = it->second;
            retransmitMsg.timestamp = m_serverTime; // Reset timer
            auto serialized = SerializeMessage(retransmitMsg);

            if (m_role == NetworkRole::Client)
            {
                SendRawTo(serialized, m_serverAddress);
            }
            else if (m_role == NetworkRole::Server)
            {
                for (const auto& [id, addr] : m_clientAddresses)
                {
                    SendRawTo(serialized, addr);
                }
            }

            // Drop after too many retries (prevent infinite retransmit)
            float totalAge = m_serverTime - retransmitMsg.timestamp;
            if (totalAge > m_connectionTimeout)
            {
                m_unacknowledgedMessages.erase(it);
                m_stats.packetsDropped++;
            }
        }
#else
        // Without networking, just discard
        while (!toSend.empty())
        {
            toSend.pop();
        }
#endif // ENABLE_NETWORKING
    }

    // --------------------------------------------------------------------------
    // UpdateReplication
    // --------------------------------------------------------------------------

    void NetworkManager::UpdateReplication(float deltaTime)
    {
        if (m_role != NetworkRole::Server)
            return;

        m_replicationTimer += deltaTime;
        if (m_replicationTimer < m_replicationInterval)
            return;
        m_replicationTimer = 0.0f;

        for (auto& [netID, entity] : m_replicatedEntities)
        {
            bool hasDirty = entity.needsFullSync;
            if (!hasDirty)
            {
                for (const auto& prop : entity.properties)
                {
                    if (prop.dirty)
                    {
                        hasDirty = true;
                        break;
                    }
                }
            }

            if (hasDirty)
            {
                NetworkMessage msg;
                msg.type = MessageType::EntityStateUpdate;
                msg.channel = entity.needsFullSync ? ChannelType::Reliable : ChannelType::Unreliable;

                NetBuffer buf;
                SerializeEntityState(netID, buf);
                msg.payload = buf.GetData();
                SendToAll(msg);

                entity.needsFullSync = false;
                for (auto& prop : entity.properties)
                    prop.dirty = false;
            }
        }
    }

    // --------------------------------------------------------------------------
    // UpdateHeartbeat
    // --------------------------------------------------------------------------

    void NetworkManager::UpdateHeartbeat(float deltaTime)
    {
        m_heartbeatTimer += deltaTime;
        if (m_heartbeatTimer >= m_heartbeatInterval)
        {
            m_heartbeatTimer = 0.0f;

            NetworkMessage heartbeat;
            heartbeat.type = MessageType::Heartbeat;
            heartbeat.channel = ChannelType::Unreliable;
            heartbeat.senderID = m_localClientID;
            heartbeat.timestamp = m_serverTime;
            SendMessage(heartbeat);
        }
    }

    // --------------------------------------------------------------------------
    // HandleConnect / HandleDisconnect
    // --------------------------------------------------------------------------

    void NetworkManager::HandleConnect(const NetworkMessage& msg)
    {
        if (m_role != NetworkRole::Server)
            return;

        if (static_cast<int>(m_clients.size()) >= m_maxClients)
        {
            NetworkMessage reject;
            reject.type = MessageType::ConnectRejected;
            reject.channel = ChannelType::Reliable;
            NetBuffer rejectBuf;
            rejectBuf.WriteString("Server full");
            reject.payload = rejectBuf.GetData();
            SendMessage(reject);
            return;
        }

        ClientID newID = m_nextClientID++;
        ClientInfo info;
        info.id = newID;
        info.state = ConnectionState::Connected;
        info.lastHeartbeatTime = m_serverTime;

        // Parse player name from connection request payload
        if (!msg.payload.empty())
        {
            NetBuffer buf;
            buf.WriteBytes(msg.payload.data(), msg.payload.size());
            info.name = buf.ReadString();
        }
        if (info.name.empty())
        {
            info.name = "Player_" + std::to_string(newID);
        }
        m_clients[newID] = info;

        // Send acceptance with assigned client ID
        NetworkMessage accept;
        accept.type = MessageType::ConnectAccepted;
        accept.channel = ChannelType::Reliable;
        NetBuffer respBuf;
        respBuf.WriteUint32(newID);
        respBuf.WriteFloat(m_serverTime);
        accept.payload = respBuf.GetData();
        SendToClient(newID, accept);
    }

    void NetworkManager::HandleDisconnect(const NetworkMessage& msg)
    {
        ClientID clientID = msg.senderID;
        m_clients.erase(clientID);

#ifdef ENABLE_NETWORKING
        m_clientAddresses.erase(clientID);
#endif

        // Remove entities owned by this client
        std::vector<uint32_t> ownedEntities;
        for (const auto& [netID, entity] : m_replicatedEntities)
        {
            if (entity.ownerID == clientID)
            {
                ownedEntities.push_back(netID);
            }
        }
        for (uint32_t netID : ownedEntities)
        {
            UnregisterReplicatedEntity(netID);
        }
    }

    // --------------------------------------------------------------------------
    // Console commands
    // --------------------------------------------------------------------------

    std::string NetworkManager::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== Network Status ===\n";
        ss << "Initialized: " << (m_initialized ? "Yes" : "No") << "\n";
        ss << "Role: ";
        switch (m_role)
        {
        case NetworkRole::None:
            ss << "None";
            break;
        case NetworkRole::Server:
            ss << "Server";
            break;
        case NetworkRole::Client:
            ss << "Client";
            break;
        }
        ss << "\nState: ";
        switch (m_connectionState)
        {
        case ConnectionState::Disconnected:
            ss << "Disconnected";
            break;
        case ConnectionState::Connecting:
            ss << "Connecting";
            break;
        case ConnectionState::Connected:
            ss << "Connected";
            break;
        case ConnectionState::Disconnecting:
            ss << "Disconnecting";
            break;
        }
        ss << "\nServer Time: " << m_serverTime << "s\n";
        ss << "Replicated Entities: " << m_replicatedEntities.size() << "\n";
        if (m_role == NetworkRole::Server)
            ss << "Connected Clients: " << m_clients.size() << "/" << m_maxClients << "\n";
        if (m_role == NetworkRole::Client)
            ss << "Local Client ID: " << m_localClientID << "\n";
        return ss.str();
    }

    std::string NetworkManager::Console_ListClients() const
    {
        std::ostringstream ss;
        ss << "=== Connected Clients (" << m_clients.size() << ") ===\n";
        for (const auto& [id, info] : m_clients)
        {
            ss << "  Client " << id << ": " << info.name << " [Ping: " << info.stats.ping << "ms]"
               << " [Last heartbeat: " << info.lastHeartbeatTime << "s]\n";
        }
        return ss.str();
    }

    std::string NetworkManager::Console_GetStats() const
    {
        std::ostringstream ss;
        ss << "=== Network Stats ===\n";
        ss << "Ping: " << m_stats.ping << "ms (jitter: " << m_stats.jitter << "ms)\n";
        ss << "Packet Loss: " << (m_stats.packetLoss * 100.0f) << "%\n";
        ss << "Bandwidth: Up " << m_stats.bandwidthUp << " KB/s, Down " << m_stats.bandwidthDown << " KB/s\n";
        ss << "Packets: Sent " << m_stats.packetsSent << ", Received " << m_stats.packetsReceived << ", Dropped "
           << m_stats.packetsDropped << "\n";
        ss << "Bytes: Sent " << m_stats.bytesSent << ", Received " << m_stats.bytesReceived << "\n";
        ss << "Unacked reliable messages: " << m_unacknowledgedMessages.size() << "\n";
        return ss.str();
    }

} // namespace Spark::Net
