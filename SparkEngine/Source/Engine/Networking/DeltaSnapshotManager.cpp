/**
 * @file DeltaSnapshotManager.cpp
 * @brief Implementation of per-connection delta snapshot tracking
 */

#include "DeltaSnapshotManager.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace Spark::Net
{

    // ========================================================================
    // FNV-1a Hash
    // ========================================================================

    uint64_t DeltaSnapshotManager::ComputeHash(const uint8_t* data, size_t size)
    {
        constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
        constexpr uint64_t FNV_PRIME = 1099511628211ULL;

        uint64_t hash = FNV_OFFSET_BASIS;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint64_t>(data[i]);
            hash *= FNV_PRIME;
        }
        return hash;
    }

    // ========================================================================
    // Connection Lifecycle
    // ========================================================================

    void DeltaSnapshotManager::RegisterConnection(uint32_t connectionId)
    {
        std::lock_guard lock(m_mutex);

        if (m_connections.contains(connectionId))
        {
            return;
        }

        ConnectionState state;
        state.connectionId = connectionId;
        m_connections[connectionId] = std::move(state);
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "DeltaSnapshotManager: registered connection %u", connectionId);
    }

    void DeltaSnapshotManager::UnregisterConnection(uint32_t connectionId)
    {
        std::lock_guard lock(m_mutex);
        m_connections.erase(connectionId);
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "DeltaSnapshotManager: unregistered connection %u", connectionId);
    }

    // ========================================================================
    // State Recording
    // ========================================================================

    void DeltaSnapshotManager::RecordEntityState(uint32_t entityId, const std::vector<FieldSnapshot>& fields)
    {
        std::lock_guard lock(m_mutex);

        // Store a copy with hashes precomputed
        auto& stored = m_currentEntityStates[entityId];
        stored.clear();
        stored.reserve(fields.size());

        for (const auto& field : fields)
        {
            FieldSnapshot snapshot;
            snapshot.fieldIndex = field.fieldIndex;
            snapshot.serializedValue = field.serializedValue;

            // Compute hash if not already provided
            if (field.valueHash != 0)
            {
                snapshot.valueHash = field.valueHash;
            }
            else if (!field.serializedValue.empty())
            {
                snapshot.valueHash = ComputeHash(field.serializedValue.data(), field.serializedValue.size());
            }

            stored.push_back(std::move(snapshot));
        }
    }

    // ========================================================================
    // Delta Building
    // ========================================================================

    /// @brief Helper to write a value into a byte buffer in little-endian order
    template <typename T> static void WriteToBuffer(std::vector<uint8_t>& buffer, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto offset = buffer.size();
        buffer.resize(offset + sizeof(T));
        std::memcpy(buffer.data() + offset, &value, sizeof(T));
    }

    std::vector<uint8_t> DeltaSnapshotManager::BuildDeltaPacket(uint32_t connectionId, uint32_t entityId)
    {
        std::lock_guard lock(m_mutex);

        // Find the connection
        auto connIt = m_connections.find(connectionId);
        if (connIt == m_connections.end())
        {
            return {};
        }

        // Find the current entity state
        auto entityIt = m_currentEntityStates.find(entityId);
        if (entityIt == m_currentEntityStates.end())
        {
            return {};
        }

        auto& conn = connIt->second;
        const auto& currentFields = entityIt->second;

        // Get the last-acked fields for this entity on this connection
        const std::vector<FieldSnapshot>* ackedFields = nullptr;
        auto ackedIt = conn.ackedEntityFields.find(entityId);
        if (ackedIt != conn.ackedEntityFields.end())
        {
            ackedFields = &ackedIt->second;
        }

        // Compare each current field against the acked state
        std::vector<const FieldSnapshot*> changedFields;
        changedFields.reserve(currentFields.size());

        for (const auto& current : currentFields)
        {
            bool changed = true;

            if (ackedFields != nullptr)
            {
                // Look for this field index in the acked set
                for (const auto& acked : *ackedFields)
                {
                    if (acked.fieldIndex == current.fieldIndex)
                    {
                        // Compare by hash for speed
                        if (acked.valueHash == current.valueHash)
                        {
                            changed = false;
                        }
                        break;
                    }
                }
            }

            if (changed)
            {
                changedFields.push_back(&current);
            }
        }

        // Nothing changed — no packet needed
        if (changedFields.empty())
        {
            return {};
        }

        // Assign a sequence number
        uint32_t sequence = conn.nextSequence++;

        // Build the packet
        std::vector<uint8_t> packet;
        packet.reserve(64 + changedFields.size() * 16);

        WriteToBuffer(packet, entityId);
        WriteToBuffer(packet, sequence);

        auto fieldCount = static_cast<uint8_t>(changedFields.size());
        WriteToBuffer(packet, fieldCount);

        for (const auto* field : changedFields)
        {
            WriteToBuffer(packet, field->fieldIndex);

            auto dataSize = static_cast<uint16_t>(field->serializedValue.size());
            WriteToBuffer(packet, dataSize);

            if (!field->serializedValue.empty())
            {
                packet.insert(packet.end(), field->serializedValue.begin(), field->serializedValue.end());
            }
        }

        // Record as pending for acknowledgement tracking
        PendingDelta pending;
        pending.sequence = sequence;
        pending.entityId = entityId;
        pending.sentFields.reserve(changedFields.size());

        for (const auto* field : changedFields)
        {
            pending.sentFields.push_back(*field);
        }

        conn.pendingDeltas[sequence] = std::move(pending);

        // Bound per-connection memory: a silent client that never acks must not
        // grow pendingDeltas without limit. Sequences are monotonic from 1, so
        // dropping everything older than the window keeps at most
        // kMaxPendingDeltasPerConnection entries alive.
        if (conn.pendingDeltas.size() > kMaxPendingDeltasPerConnection)
        {
            const uint32_t oldestKept = sequence - static_cast<uint32_t>(kMaxPendingDeltasPerConnection);
            std::erase_if(conn.pendingDeltas, [oldestKept](const auto& pair) { return pair.first <= oldestKept; });
        }

        return packet;
    }

    // ========================================================================
    // Acknowledgement
    // ========================================================================

    void DeltaSnapshotManager::AcknowledgeSequence(uint32_t connectionId, uint32_t sequence)
    {
        std::lock_guard lock(m_mutex);

        auto connIt = m_connections.find(connectionId);
        if (connIt == m_connections.end())
        {
            return;
        }

        auto& conn = connIt->second;

        // Find the pending delta for this sequence
        auto pendingIt = conn.pendingDeltas.find(sequence);
        if (pendingIt == conn.pendingDeltas.end())
        {
            return;
        }

        const auto& pending = pendingIt->second;
        uint32_t entityId = pending.entityId;

        // Update the last-acked sequence
        if (sequence > conn.lastAckedSequence)
        {
            conn.lastAckedSequence = sequence;
        }

        // Merge the acknowledged fields into the connection's acked entity state.
        // For each field in the pending delta, update (or insert) the acked snapshot.
        auto& ackedFields = conn.ackedEntityFields[entityId];

        for (const auto& sentField : pending.sentFields)
        {
            bool found = false;
            for (auto& acked : ackedFields)
            {
                if (acked.fieldIndex == sentField.fieldIndex)
                {
                    acked.serializedValue = sentField.serializedValue;
                    acked.valueHash = sentField.valueHash;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                ackedFields.push_back(sentField);
            }
        }

        // Remove all pending deltas up to and including this sequence
        // (cumulative ack — if seq 5 is acked, seqs 1-4 are implicitly acked too)
        std::erase_if(conn.pendingDeltas, [sequence](const auto& pair) { return pair.first <= sequence; });
    }

    // ========================================================================
    // Queries
    // ========================================================================

    size_t DeltaSnapshotManager::GetPendingDeltaCount(uint32_t connectionId) const
    {
        std::lock_guard lock(m_mutex);

        auto it = m_connections.find(connectionId);
        if (it == m_connections.end())
        {
            return 0;
        }
        return it->second.pendingDeltas.size();
    }

    std::string DeltaSnapshotManager::Console_GetStatus() const
    {
        std::lock_guard lock(m_mutex);

        std::string status;
        status += std::format("DeltaSnapshotManager: {} connections, {} entities tracked\n", m_connections.size(),
                              m_currentEntityStates.size());

        for (const auto& [connId, conn] : m_connections)
        {
            status += std::format("  Connection {}: lastAcked={}, pending={}, entities={}\n", connId,
                                  conn.lastAckedSequence, conn.pendingDeltas.size(), conn.ackedEntityFields.size());
        }

        return status;
    }

    // ========================================================================
    // Shutdown
    // ========================================================================

    void DeltaSnapshotManager::Shutdown()
    {
        std::lock_guard lock(m_mutex);
        m_connections.clear();
        m_currentEntityStates.clear();
    }

} // namespace Spark::Net
