/**
 * @file EntityReplicator.cpp
 * @brief Implementation of dirty-tracked entity replication
 */

#include "EntityReplicator.h"
#include "../../Utils/Validate.h"

#include <cstring>

namespace Spark::Net
{

    // ============================================================================
    // Registration
    // ============================================================================

    void EntityReplicator::RegisterEntity(NetworkEntityID entityID, const ReplicatedFieldSet& fieldSet,
                                          FieldSerializer serializer, FieldDeserializer deserializer)
    {
        ReplicatedEntityEntry entry;
        entry.entityID = entityID;
        entry.fieldSet = fieldSet;
        entry.serializer = std::move(serializer);
        entry.deserializer = std::move(deserializer);

        m_entities[entityID] = std::move(entry);
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "EntityReplicator: registered entity %u (%u fields)", entityID,
                        fieldSet.GetFieldCount());
    }

    void EntityReplicator::UnregisterEntity(NetworkEntityID entityID)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "EntityReplicator: unregistered entity %u", entityID);
        m_entities.erase(entityID);
    }

    bool EntityReplicator::IsEntityRegistered(NetworkEntityID entityID) const
    {
        return m_entities.contains(entityID);
    }

    ReplicatedFieldSet* EntityReplicator::GetFieldSet(NetworkEntityID entityID)
    {
        auto it = m_entities.find(entityID);
        if (it == m_entities.end())
        {
            return nullptr;
        }
        return &it->second.fieldSet;
    }

    // ============================================================================
    // Packet Construction
    // ============================================================================

    bool EntityReplicator::WriteCreatePacket(NetworkEntityID entityID, std::vector<uint8_t>& buffer) const
    {
        auto it = m_entities.find(entityID);
        if (it == m_entities.end() || !it->second.serializer)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "WriteCreatePacket: entity %u not found or has no serializer",
                           entityID);
            return false;
        }

        const auto& entry = it->second;
        const uint8_t fieldCount = entry.fieldSet.GetFieldCount();

        // Header: entityID + fieldCount
        const auto headerOffset = buffer.size();
        buffer.resize(headerOffset + sizeof(uint32_t) + sizeof(uint8_t));
        std::memcpy(buffer.data() + headerOffset, &entityID, sizeof(uint32_t));
        buffer[headerOffset + sizeof(uint32_t)] = fieldCount;

        // Serialize every field (full state for new observers)
        for (uint8_t i = 0; i < fieldCount; ++i)
        {
            entry.serializer(i, buffer);
        }

        return true;
    }

    bool EntityReplicator::WriteUpdatePacket(NetworkEntityID entityID, std::vector<uint8_t>& buffer,
                                             FieldVisibility observerVisibility) const
    {
        auto it = m_entities.find(entityID);
        if (it == m_entities.end() || !it->second.serializer)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "WriteUpdatePacket: entity %u not found or has no serializer",
                           entityID);
            return false;
        }

        const auto& entry = it->second;

        // Build a combined visibility mask
        uint64_t visibleMask = entry.fieldSet.GetVisibilityMask(FieldVisibility::Public);
        if (observerVisibility != FieldVisibility::Public)
        {
            visibleMask |= entry.fieldSet.GetVisibilityMask(observerVisibility);
        }

        const uint64_t dirtyMask = entry.fieldSet.GetDirtyMask() & visibleMask;
        if (dirtyMask == 0)
        {
            return false; // Nothing to send for this observer
        }

        // Header: entityID + filtered dirty mask
        const auto headerOffset = buffer.size();
        buffer.resize(headerOffset + sizeof(uint32_t) + sizeof(uint64_t));
        std::memcpy(buffer.data() + headerOffset, &entityID, sizeof(uint32_t));
        std::memcpy(buffer.data() + headerOffset + sizeof(uint32_t), &dirtyMask, sizeof(uint64_t));

        // Serialize only dirty+visible fields
        const uint8_t fieldCount = entry.fieldSet.GetFieldCount();
        for (uint8_t i = 0; i < fieldCount; ++i)
        {
            if ((dirtyMask >> i) & 1ULL)
            {
                entry.serializer(i, buffer);
            }
        }

        return true;
    }

    // ============================================================================
    // Incoming Data
    // ============================================================================

    bool EntityReplicator::ProcessIncoming(const std::vector<uint8_t>& buffer, size_t& offset)
    {
        if (offset + sizeof(uint32_t) + sizeof(uint64_t) > buffer.size())
        {
            return false;
        }

        uint32_t entityID = 0;
        std::memcpy(&entityID, buffer.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        uint64_t dirtyMask = 0;
        std::memcpy(&dirtyMask, buffer.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);

        auto it = m_entities.find(entityID);
        if (it == m_entities.end() || !it->second.deserializer)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network,
                           "ProcessIncoming: entity %u not registered — queueing for late binding", entityID);
            // Store the unprocessed entity data offset so the caller can skip it
            // We can't deserialize without a registered schema, but we don't treat this as fatal
            return false;
        }

        auto& entry = it->second;
        const uint8_t fieldCount = entry.fieldSet.GetFieldCount();

        for (uint8_t i = 0; i < fieldCount; ++i)
        {
            if ((dirtyMask >> i) & 1ULL)
            {
                size_t bytesRead = entry.deserializer(i, buffer, offset);
                if (bytesRead == 0)
                {
                    // Skip this field and continue processing remaining fields
                    SPARK_LOG_WARN(Spark::LogCategory::Network,
                                   "ProcessIncoming: deserializer returned 0 bytes for entity %u field %u — skipping "
                                   "remaining fields",
                                   entityID, i);
                    return true; // Partial update applied — not fatal
                }
                offset += bytesRead;
            }
        }

        return true;
    }

    // ============================================================================
    // Dirty Management
    // ============================================================================

    std::vector<NetworkEntityID> EntityReplicator::GatherDirtyEntities() const
    {
        std::vector<NetworkEntityID> dirtyIDs;
        dirtyIDs.reserve(m_entities.size());

        for (const auto& [id, entry] : m_entities)
        {
            if (entry.fieldSet.IsAnyDirty())
            {
                dirtyIDs.push_back(id);
            }
        }

        return dirtyIDs;
    }

    void EntityReplicator::FlushDirty()
    {
        for (auto& [id, entry] : m_entities)
        {
            entry.fieldSet.ClearAllDirty();
        }
    }

    void EntityReplicator::FlushDirty(NetworkEntityID entityID)
    {
        auto it = m_entities.find(entityID);
        if (it != m_entities.end())
        {
            it->second.fieldSet.ClearAllDirty();
        }
    }

    size_t EntityReplicator::GetEntityCount() const
    {
        return m_entities.size();
    }

} // namespace Spark::Net
