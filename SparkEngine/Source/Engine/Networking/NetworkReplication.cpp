/**
 * @file NetworkReplication.cpp
 * @brief Entity replication methods for NetworkManager
 *
 * Extracted from NetworkManager.cpp — implements entity registration,
 * property dirty-tracking, serialization/deserialization, and replication updates.
 */

#include "NetworkManager.h"
#include "ConnectionScopeFilter.h"
#include "DeltaSnapshotManager.h"
#include "../../Utils/Assert.h"
#include "../../Utils/Validate.h"
#include <algorithm>

#ifdef SendMessage
#undef SendMessage
#endif

using namespace DirectX;
namespace Spark::Net
{

    // --------------------------------------------------------------------------
    // Entity Replication
    // --------------------------------------------------------------------------

    uint32_t NetworkManager::RegisterReplicatedEntity(const ReplicatedEntity& entity)
    {
        uint32_t netID = m_nextNetworkID.fetch_add(1, std::memory_order_relaxed);

        // Read role first (respects lock order: m_stateMutex before m_replicationMutex)
        NetworkRole role = GetRole();

        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            m_replicatedEntities[netID] = entity;
            m_replicatedEntities[netID].networkID = netID;
            m_replicatedEntities[netID].needsFullSync = true;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Entity registered: netID=%u type='%s' owner=%u", netID,
                       entity.entityType.c_str(), entity.ownerID);

        // Notify clients about the new entity (server only)
        if (role == NetworkRole::Server)
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
        // Read role before replication lock (respects lock order)
        NetworkRole role = GetRole();

        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            auto it = m_replicatedEntities.find(networkID);
            if (it == m_replicatedEntities.end())
                return;

            SPARK_LOG_INFO(Spark::LogCategory::Network, "Entity unregistered: netID=%u", networkID);
            m_replicatedEntities.erase(it);
        }

        // Notify clients after releasing m_replicationMutex to avoid
        // holding it during network I/O (SendToAll acquires m_queueMutex).
        if (role == NetworkRole::Server)
        {
            NetworkMessage msg;
            msg.type = MessageType::EntityDestroy;
            msg.channel = ChannelType::Reliable;

            NetBuffer buf;
            buf.WriteUint32(networkID);
            msg.payload = buf.GetData();
            SendToAll(msg);
        }
    }

    void NetworkManager::MarkPropertyDirty(uint32_t networkID, const std::string& propertyName)
    {
        std::lock_guard<std::mutex> lock(m_replicationMutex);
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
        std::lock_guard<std::mutex> lock(m_replicationMutex);
        auto it = m_replicatedEntities.find(networkID);
        return (it != m_replicatedEntities.end()) ? &it->second : nullptr;
    }

    void NetworkManager::SendFullEntitySync(ClientID targetClient)
    {
        if (GetRole() != NetworkRole::Server)
            return;

        auto& scopeFilter = ConnectionScopeFilter::GetInstance();

        std::lock_guard<std::mutex> lock(m_replicationMutex);
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Sending full entity sync to client %u (%zu entities)",
                        targetClient, m_replicatedEntities.size());
        for (const auto& [netID, entity] : m_replicatedEntities)
        {
            // Per-connection interest filter: skip entities outside the client's scope.
            // If no scope is set for this client, IsEntityInScope returns true (see-all default).
            if (!scopeFilter.IsEntityInScope(targetClient, entity.position, entity.areaId, entity.teamMask,
                                             entity.visibilityMask))
            {
                continue;
            }

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
        std::lock_guard<std::mutex> lock(m_replicationMutex);
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
        if (inBuffer.HasError())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "DeserializeEntityState: malformed packet (no networkID)");
            return;
        }

        std::lock_guard<std::mutex> lock(m_replicationMutex);
        auto it = m_replicatedEntities.find(networkID);
        if (it == m_replicatedEntities.end())
        {
            // Entity not known locally -- create a placeholder
            SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Creating placeholder for unknown entity netID=%u", networkID);
            ReplicatedEntity placeholder;
            placeholder.networkID = networkID;
            placeholder.position = inBuffer.ReadVector3();
            placeholder.rotation = inBuffer.ReadVector3();
            placeholder.velocity = inBuffer.ReadVector3();
            // Skip remaining property data
            uint16_t propCount = inBuffer.ReadUint16();
            if (inBuffer.HasError())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "DeserializeEntityState: truncated packet for placeholder netID=%u", networkID);
                return;
            }
            for (uint16_t i = 0; i < propCount && !inBuffer.HasError(); ++i)
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
        if (inBuffer.HasError())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "DeserializeEntityState: truncated packet for netID=%u",
                           networkID);
            return;
        }
        for (uint16_t i = 0; i < propCount && !inBuffer.HasError(); ++i)
        {
            std::string propName = inBuffer.ReadString();
            uint8_t propType = inBuffer.ReadUint8();
            (void)propType;

            if (inBuffer.HasError())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "DeserializeEntityState: buffer error reading property %u/%u for netID=%u", i, propCount,
                               networkID);
                break;
            }

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
    // UpdateReplication
    // --------------------------------------------------------------------------

    void NetworkManager::UpdateReplication(float deltaTime)
    {
        if (GetRole() != NetworkRole::Server)
            return;

        m_replicationTimer += deltaTime;
        if (m_replicationTimer < m_replicationInterval)
            return;
        m_replicationTimer = 0.0f;

        auto& deltaManager = DeltaSnapshotManager::GetInstance();
        auto& scopeFilter = ConnectionScopeFilter::GetInstance();

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
                // Record the current entity state for delta snapshot tracking.
                // This builds FieldSnapshot entries from the entity's serialized properties.
                std::vector<FieldSnapshot> fieldSnapshots;
                fieldSnapshots.reserve(entity.properties.size());

                for (size_t i = 0; i < entity.properties.size(); ++i)
                {
                    const auto& prop = entity.properties[i];
                    if (!prop.dirty && !entity.needsFullSync)
                        continue;

                    FieldSnapshot fs;
                    fs.fieldIndex = static_cast<uint8_t>(i & 0xFF);
                    if (prop.serialize)
                    {
                        NetBuffer fieldBuf;
                        prop.serialize(fieldBuf);
                        fs.serializedValue = fieldBuf.GetData();
                    }
                    fieldSnapshots.push_back(std::move(fs));
                }

                deltaManager.RecordEntityState(netID, fieldSnapshots);

                // Snapshot client IDs once — both code paths need them so we can
                // apply the per-connection interest filter instead of broadcasting.
                std::vector<ClientID> connectedClients;
                {
                    std::lock_guard<std::mutex> clientLock(m_clientsMutex);
                    connectedClients.reserve(m_clients.size());
                    for (const auto& [cid, cinfo] : m_clients)
                        connectedClients.push_back(cid);
                }

                if (entity.needsFullSync)
                {
                    // Full sync: previously broadcast to all clients. Now filter
                    // by ConnectionScopeFilter so out-of-scope clients don't see
                    // entities they can't observe.
                    NetBuffer buf;
                    SerializeEntityState(netID, buf);
                    std::vector<uint8_t> payload = buf.GetData();

                    for (ClientID clientId : connectedClients)
                    {
                        if (!scopeFilter.IsEntityInScope(clientId, entity.position, entity.areaId, entity.teamMask,
                                                         entity.visibilityMask))
                        {
                            continue;
                        }
                        NetworkMessage msg;
                        msg.type = MessageType::EntityStateUpdate;
                        msg.channel = ChannelType::Reliable;
                        msg.payload = payload;
                        SendToClient(clientId, msg);
                    }
                }
                else
                {
                    // Delta sync: build per-connection delta packets, filtered by scope.
                    for (ClientID clientId : connectedClients)
                    {
                        if (!scopeFilter.IsEntityInScope(clientId, entity.position, entity.areaId, entity.teamMask,
                                                         entity.visibilityMask))
                        {
                            continue;
                        }
                        auto deltaPacket = deltaManager.BuildDeltaPacket(clientId, netID);
                        if (!deltaPacket.empty())
                        {
                            NetworkMessage msg;
                            msg.type = MessageType::EntityStateUpdate;
                            msg.channel = ChannelType::Unreliable;
                            msg.payload = std::move(deltaPacket);
                            SendToClient(clientId, msg);
                        }
                    }
                }

                entity.needsFullSync = false;
                for (auto& prop : entity.properties)
                    prop.dirty = false;
            }
        }
    }

    // --------------------------------------------------------------------------
    // Per-connection interest management
    // --------------------------------------------------------------------------

    void NetworkManager::SetClientScope(ClientID client, const XMFLOAT3& position, float radius, uint32_t areaId,
                                        uint32_t teamMask, uint32_t visibilityMask)
    {
        ConnectionScope scope;
        scope.areaId = areaId;
        scope.position = position;
        scope.radius = radius;
        scope.teamMask = teamMask;
        scope.visibilityMask = visibilityMask;
        ConnectionScopeFilter::GetInstance().SetScope(static_cast<uint32_t>(client), scope);
    }

    void NetworkManager::ClearClientScope(ClientID client)
    {
        ConnectionScopeFilter::GetInstance().RemoveConnection(static_cast<uint32_t>(client));
    }

} // namespace Spark::Net
