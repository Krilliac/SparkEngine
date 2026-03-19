/**
 * @file NetworkReplication.cpp
 * @brief Entity replication methods for NetworkManager
 *
 * Extracted from NetworkManager.cpp — implements entity registration,
 * property dirty-tracking, serialization/deserialization, and replication updates.
 */

#include "NetworkManager.h"
#include "../../Utils/Assert.h"
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
        m_replicatedEntities[netID] = entity;
        m_replicatedEntities[netID].networkID = netID;
        m_replicatedEntities[netID].needsFullSync = true;

        // Notify clients about the new entity (server only)
        NetworkRole role;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            role = m_role;
        }
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

} // namespace Spark::Net
