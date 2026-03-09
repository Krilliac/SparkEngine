/**
 * @file NetworkManager.cpp
 * @brief Networking implementation — serialization, replication, lag compensation
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

    bool NetworkManager::StartServer(uint16_t port, int maxClients)
    {
        m_role = NetworkRole::Server;
        m_connectionState = ConnectionState::Connected;
        m_maxClients = maxClients;
        m_localClientID = 0; // Server is client 0
        m_serverTime = 0.0f;
        return true;
    }

    bool NetworkManager::Connect(const std::string& address, uint16_t port, const std::string& playerName)
    {
        m_role = NetworkRole::Client;
        m_connectionState = ConnectionState::Connecting;

        // Send connect request
        NetworkMessage connectMsg;
        connectMsg.type = MessageType::Connect;
        connectMsg.channel = ChannelType::Reliable;
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

        NetworkMessage disconnectMsg;
        disconnectMsg.type = MessageType::Disconnect;
        disconnectMsg.channel = ChannelType::Reliable;
        SendMessage(disconnectMsg);

        m_connectionState = ConnectionState::Disconnected;
        m_role = NetworkRole::None;
        m_clients.clear();
        m_replicatedEntities.clear();
        m_lagCompensator.Clear();
    }

    void NetworkManager::Update(float deltaTime)
    {
        if (m_role == NetworkRole::None)
            return;

        m_serverTime += deltaTime;

        ProcessIncoming();
        UpdateReplication(deltaTime);
        UpdateHeartbeat(deltaTime);
        ProcessOutgoing();
    }

    void NetworkManager::SendMessage(const NetworkMessage& msg)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outgoingQueue.push(msg);
        m_stats.packetsSent++;
    }

    void NetworkManager::SendToClient(ClientID client, const NetworkMessage& msg)
    {
        NetworkMessage copy = msg;
        copy.senderID = 0; // From server
        SendMessage(copy);
    }

    void NetworkManager::SendToAll(const NetworkMessage& msg)
    {
        for (const auto& [id, _] : m_clients)
        {
            SendToClient(id, msg);
        }
    }

    void NetworkManager::SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg)
    {
        for (const auto& [id, _] : m_clients)
        {
            if (id != excludeClient)
                SendToClient(id, msg);
        }
    }

    void NetworkManager::RegisterHandler(MessageType type, MessageHandler handler)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_handlers[static_cast<uint16_t>(type)] = std::move(handler);
    }

    uint32_t NetworkManager::RegisterReplicatedEntity(const ReplicatedEntity& entity)
    {
        uint32_t netID = m_nextNetworkID++;
        m_replicatedEntities[netID] = entity;
        m_replicatedEntities[netID].networkID = netID;
        return netID;
    }

    void NetworkManager::UnregisterReplicatedEntity(uint32_t networkID)
    {
        m_replicatedEntities.erase(networkID);
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

    void NetworkManager::SendClientInput(const ClientInputState& input)
    {
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
    }

    void NetworkManager::ProcessIncoming()
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_incomingQueue.empty())
        {
            const auto& msg = m_incomingQueue.front();
            auto handlerIt = m_handlers.find(static_cast<uint16_t>(msg.type));
            if (handlerIt != m_handlers.end())
            {
                handlerIt->second(msg);
            }
            m_incomingQueue.pop();
            m_stats.packetsReceived++;
        }
    }

    void NetworkManager::ProcessOutgoing()
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        // In a real implementation, this would serialize and send via UDP socket
        while (!m_outgoingQueue.empty())
        {
            m_outgoingQueue.pop();
        }
    }

    void NetworkManager::UpdateReplication(float deltaTime)
    {
        if (m_role != NetworkRole::Server)
            return;

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
                buf.WriteUint32(netID);
                buf.WriteVector3(entity.position);
                buf.WriteVector3(entity.rotation);
                buf.WriteVector3(entity.velocity);
                msg.payload = buf.GetData();
                SendToAll(msg);

                entity.needsFullSync = false;
                for (auto& prop : entity.properties)
                    prop.dirty = false;
            }
        }
    }

    void NetworkManager::UpdateHeartbeat(float deltaTime)
    {
        m_heartbeatTimer += deltaTime;
        if (m_heartbeatTimer >= m_heartbeatInterval)
        {
            m_heartbeatTimer = 0.0f;

            NetworkMessage heartbeat;
            heartbeat.type = MessageType::Heartbeat;
            heartbeat.channel = ChannelType::Unreliable;
            SendMessage(heartbeat);
        }
    }

    void NetworkManager::HandleConnect(const NetworkMessage& msg)
    {
        if (m_role != NetworkRole::Server)
            return;

        if (static_cast<int>(m_clients.size()) >= m_maxClients)
        {
            NetworkMessage reject;
            reject.type = MessageType::ConnectRejected;
            reject.channel = ChannelType::Reliable;
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
            // Treat payload bytes as a length-prefixed string written by NetBuffer::WriteString
            NetBuffer buf;
            buf.WriteBytes(msg.payload.data(), msg.payload.size());
            // Reset read position by re-creating — WriteBytes appends, ReadString reads from pos 0
            info.name = buf.ReadString();
        }
        if (info.name.empty())
        {
            info.name = "Player_" + std::to_string(newID);
        }
        m_clients[newID] = info;

        NetworkMessage accept;
        accept.type = MessageType::ConnectAccepted;
        accept.channel = ChannelType::Reliable;
        NetBuffer respBuf;
        respBuf.WriteUint32(newID);
        accept.payload = respBuf.GetData();
        SendToClient(newID, accept);
    }

    void NetworkManager::HandleDisconnect(const NetworkMessage& msg)
    {
        m_clients.erase(msg.senderID);
    }

    std::string NetworkManager::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== Network Status ===\n";
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
        return ss.str();
    }

    std::string NetworkManager::Console_ListClients() const
    {
        std::ostringstream ss;
        ss << "=== Connected Clients (" << m_clients.size() << ") ===\n";
        for (const auto& [id, info] : m_clients)
        {
            ss << "  Client " << id << ": " << info.name << " [Ping: " << info.stats.ping << "ms]\n";
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
        return ss.str();
    }

} // namespace Spark::Net
