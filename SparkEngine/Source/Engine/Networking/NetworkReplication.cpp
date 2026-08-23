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
#include <cstring>

#ifdef SendMessage
#undef SendMessage
#endif

using namespace DirectX;
namespace Spark::Net
{

    namespace
    {
        // Serializes an entity's state without touching m_replicatedEntities.
        // Callers that hold m_replicationMutex use this directly; locking wrappers
        // must not call SerializeEntityState while holding the mutex (non-recursive).
        void SerializeEntityStateUnlocked(uint32_t networkID, const ReplicatedEntity& entity, NetBuffer& outBuffer)
        {
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
    } // namespace

    // --------------------------------------------------------------------------
    // Entity Replication
    // --------------------------------------------------------------------------

    uint32_t NetworkManager::RegisterReplicatedEntity(const ReplicatedEntity& entity)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        uint32_t netID = m_nextNetworkID.fetch_add(1, std::memory_order_relaxed);

        // Read role first (respects lock order: m_stateMutex before m_replicationMutex)
        NetworkRole role = GetRole();

        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            m_replicatedEntities[netID] = entity;
            m_replicatedEntities[netID].networkID = netID;
            m_replicatedEntities[netID].needsFullSync = true;
            ++m_replicationMutationEpoch;
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
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        // Read role before replication lock (respects lock order)
        NetworkRole role = GetRole();

        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            auto it = m_replicatedEntities.find(networkID);
            if (it == m_replicatedEntities.end())
                return;

            SPARK_LOG_INFO(Spark::LogCategory::Network, "Entity unregistered: netID=%u", networkID);
            m_replicatedEntities.erase(it);
            ++m_replicationMutationEpoch;
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
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        std::lock_guard<std::mutex> lock(m_replicationMutex);
        auto it = m_replicatedEntities.find(networkID);
        if (it == m_replicatedEntities.end())
            return;
        for (auto& prop : it->second.properties)
        {
            if (prop.name == propertyName)
            {
                prop.dirty = true;
                ++m_replicationMutationEpoch;
                break;
            }
        }
    }

    ReplicatedEntity* NetworkManager::GetReplicatedEntity(uint32_t networkID)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        std::lock_guard<std::mutex> lock(m_replicationMutex);
        auto it = m_replicatedEntities.find(networkID);
        return (it != m_replicatedEntities.end()) ? &it->second : nullptr;
    }

    void NetworkManager::SendFullEntitySync(ClientID targetClient)
    {
        std::unique_lock<std::recursive_mutex> apiLock(m_apiMutex);
        const uint64_t lifecycleEpoch = m_lifecycleEpoch;
        if (GetRole() != NetworkRole::Server)
            return;

        // A rejected/pending endpoint is never entitled to an entity walk. This
        // guard also keeps accidental callers from turning rejection traffic
        // into O(connects * replicated-entities) CPU amplification.
        {
            std::lock_guard<std::mutex> clientsLock(m_clientsMutex);
            if (!m_clients.contains(targetClient))
                return;
        }

        ++m_stats.fullEntitySyncs;

        auto& scopeFilter = ConnectionScopeFilter::GetInstance();
        std::vector<ReplicatedEntity> entities;
        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            entities.reserve(m_replicatedEntities.size());
            for (const auto& [netID, entity] : m_replicatedEntities)
                entities.push_back(entity);
        }
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Sending full entity sync to client %u (%zu entities)",
                        targetClient, entities.size());
        for (const auto& entity : entities)
        {
            const uint32_t netID = entity.networkID;
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
            apiLock.unlock();
            try
            {
                // Property serializers are application callbacks. The entity is
                // a value snapshot, so no manager lock is needed while they run.
                SerializeEntityStateUnlocked(netID, entity, stateBuf);
            }
            catch (...)
            {
                apiLock.lock();
                throw;
            }
            apiLock.lock();
            if (m_lifecycleEpoch != lifecycleEpoch)
                return;
            stateMsg.payload = stateBuf.GetData();
            SendToClient(targetClient, stateMsg);
        }
    }

    void NetworkManager::SerializeEntityState(uint32_t networkID, NetBuffer& outBuffer) const
    {
        std::unique_lock<std::recursive_mutex> apiLock(m_apiMutex);
        ReplicatedEntity entity;
        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            auto it = m_replicatedEntities.find(networkID);
            if (it == m_replicatedEntities.end())
                return;
            entity = it->second;
        }

        apiLock.unlock();
        SerializeEntityStateUnlocked(networkID, entity, outBuffer);
    }

    void NetworkManager::DeserializeEntityState(NetBuffer& inBuffer)
    {
        std::unique_lock<std::recursive_mutex> apiLock(m_apiMutex);
        const uint64_t lifecycleEpoch = m_lifecycleEpoch;
        uint32_t networkID = inBuffer.ReadUint32();
        if (inBuffer.HasError())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "DeserializeEntityState: malformed packet (no networkID)");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            auto it = m_replicatedEntities.find(networkID);
            if (it == m_replicatedEntities.end())
            {
                // Entity not known locally -- create a placeholder
                SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Creating placeholder for unknown entity netID=%u",
                                networkID);
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
                ++m_replicationMutationEpoch;
                return;
            }

            auto& entity = it->second;
            entity.position = inBuffer.ReadVector3();
            entity.rotation = inBuffer.ReadVector3();
            entity.velocity = inBuffer.ReadVector3();
            entity.lastUpdateTime = m_serverTime;
            ++m_replicationMutationEpoch;
        }

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

            {
                std::function<void(NetBuffer&)> deserialize;
                {
                    std::lock_guard<std::mutex> lock(m_replicationMutex);
                    auto entityIt = m_replicatedEntities.find(networkID);
                    if (entityIt != m_replicatedEntities.end())
                    {
                        for (const auto& prop : entityIt->second.properties)
                        {
                            if (prop.name == propName && prop.deserialize)
                            {
                                deserialize = prop.deserialize;
                                break;
                            }
                        }
                    }
                }
                if (deserialize)
                {
                    apiLock.unlock();
                    try
                    {
                        deserialize(inBuffer);
                    }
                    catch (...)
                    {
                        apiLock.lock();
                        throw;
                    }
                    apiLock.lock();
                    if (m_lifecycleEpoch != lifecycleEpoch)
                        return;
                }
            }
        }
    }

    // --------------------------------------------------------------------------
    // UpdateReplication
    // --------------------------------------------------------------------------

    bool NetworkManager::UpdateReplication(float deltaTime, std::unique_lock<std::recursive_mutex>& apiLock,
                                           uint64_t lifecycleEpoch)
    {
        if (GetRole() != NetworkRole::Server)
            return true;

        m_replicationTimer += deltaTime;
        if (m_replicationTimer < m_replicationInterval)
            return true;
        m_replicationTimer = 0.0f;

        auto& deltaManager = DeltaSnapshotManager::GetInstance();
        auto& scopeFilter = ConnectionScopeFilter::GetInstance();

        // Work from value snapshots so application serializers can run with no
        // NetworkManager/container lock held. A callback is then free to wait
        // for another thread using any public networking API.
        std::vector<ReplicatedEntity> entities;
        {
            std::lock_guard<std::mutex> replicationLock(m_replicationMutex);
            entities.reserve(m_replicatedEntities.size());
            for (const auto& [netID, entity] : m_replicatedEntities)
                entities.push_back(entity);
        }

        auto serializeUnlocked =
            [&apiLock, this, lifecycleEpoch](const std::function<void(NetBuffer&)>& serializer, NetBuffer& buffer)
        {
            apiLock.unlock();
            try
            {
                serializer(buffer);
            }
            catch (...)
            {
                apiLock.lock();
                throw;
            }
            apiLock.lock();
            return m_lifecycleEpoch == lifecycleEpoch;
        };

        for (const auto& entity : entities)
        {
            const uint32_t netID = entity.networkID;
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
                const uint64_t mutationEpochBeforeCallbacks = m_replicationMutationEpoch;
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
                        if (!serializeUnlocked(prop.serialize, fieldBuf))
                            return false;
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

                NetBuffer serializedState;
                apiLock.unlock();
                try
                {
                    SerializeEntityStateUnlocked(netID, entity, serializedState);
                }
                catch (...)
                {
                    apiLock.lock();
                    throw;
                }
                apiLock.lock();
                if (m_lifecycleEpoch != lifecycleEpoch)
                    return false;
                std::vector<uint8_t> payload = serializedState.GetData();

                if (entity.needsFullSync)
                {
                    // Full sync: previously broadcast to all clients. Now filter
                    // by ConnectionScopeFilter so out-of-scope clients don't see
                    // entities they can't observe.
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
                    // Delta sync: with needsFullSync false, SerializeEntityState emits only
                    // dirty properties — a property-level delta in the same wire format the
                    // client's EntityStateUpdate handler (DeserializeEntityState) parses.
                    // BuildDeltaPacket's field-indexed format has no receive-side parser and
                    // must not go on the wire; it serves as the per-connection sequence and
                    // baseline tracker for the delta-ack loop.
                    for (ClientID clientId : connectedClients)
                    {
                        if (!scopeFilter.IsEntityInScope(clientId, entity.position, entity.areaId, entity.teamMask,
                                                         entity.visibilityMask))
                        {
                            continue;
                        }

                        // Assign this connection's next delta sequence and record the pending
                        // baseline that the client's DeltaAck echo will confirm. The record
                        // format is [uint32 entityId][uint32 sequence][fieldCount...] — read
                        // the sequence back from offset 4. An empty record means nothing
                        // changed against this connection's acked baseline; still send
                        // (position/rotation travel in the payload) but with sequence 0 so
                        // the client does not echo an ack for untracked state.
                        uint32_t deltaSequence = 0;
                        const std::vector<uint8_t> deltaRecord = deltaManager.BuildDeltaPacket(clientId, netID);
                        if (deltaRecord.size() >= 2 * sizeof(uint32_t))
                        {
                            std::memcpy(&deltaSequence, deltaRecord.data() + sizeof(uint32_t), sizeof(deltaSequence));
                        }

                        NetworkMessage msg;
                        msg.type = MessageType::EntityStateUpdate;
                        msg.channel = ChannelType::Unreliable;
                        // Unreliable messages never get a reliable-channel sequence, so the
                        // header field is free to carry the delta sequence space.
                        msg.sequence = deltaSequence;
                        msg.payload = payload;
                        SendToClient(clientId, msg);
                    }
                }

                // A callback may have asked another thread to mutate replication
                // state while the API lock was released. Never erase that newer
                // dirty signal; an extra resend is safer than a lost update.
                if (m_replicationMutationEpoch == mutationEpochBeforeCallbacks)
                {
                    std::lock_guard<std::mutex> replicationLock(m_replicationMutex);
                    auto live = m_replicatedEntities.find(netID);
                    if (live != m_replicatedEntities.end())
                    {
                        live->second.needsFullSync = false;
                        for (auto& prop : live->second.properties)
                            prop.dirty = false;
                    }
                }
            }
        }
        return true;
    }

    // --------------------------------------------------------------------------
    // Per-connection interest management
    // --------------------------------------------------------------------------

    void NetworkManager::SetClientScope(ClientID client, const XMFLOAT3& position, float radius, uint32_t areaId,
                                        uint32_t teamMask, uint32_t visibilityMask)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
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
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        ConnectionScopeFilter::GetInstance().RemoveConnection(static_cast<uint32_t>(client));
    }

} // namespace Spark::Net
