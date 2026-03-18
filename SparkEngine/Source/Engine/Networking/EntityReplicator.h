/**
 * @file EntityReplicator.h
 * @brief Manages entity state replication with dirty tracking and visibility (TC-inspired)
 *
 * EntityReplicator sits between the ECS world and the network layer. It tracks
 * which entities have dirty replicated fields, builds delta packets for ongoing
 * updates and full-state packets for newly observed entities, and applies
 * incoming state updates to local entities.
 *
 * Designed to replace the string-based MarkPropertyDirty approach with typed,
 * bitmask-driven dirty tracking inspired by TrinityCore's UpdateField system.
 */

#pragma once

#include "ReplicationFields.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Spark::Net
{

    // Forward declaration
    class NetBuffer;

    /// Unique network identifier for an entity (matches NetworkIdentity::networkID)
    using NetworkEntityID = uint32_t;

    // ============================================================================
    // Serialization callbacks
    // ============================================================================

    /**
     * @brief Callback to serialize a single field into a byte buffer.
     * @param fieldIndex  The field being serialized
     * @param buffer      Destination byte buffer (append to end)
     */
    using FieldSerializer = std::function<void(uint8_t fieldIndex, std::vector<uint8_t>& buffer)>;

    /**
     * @brief Callback to deserialize a single field from a byte buffer.
     * @param fieldIndex  The field being deserialized
     * @param buffer      Source byte buffer
     * @param readPos     Current read offset (updated by callback)
     * @return Number of bytes consumed, or 0 on error
     */
    using FieldDeserializer =
        std::function<size_t(uint8_t fieldIndex, const std::vector<uint8_t>& buffer, size_t readPos)>;

    // ============================================================================
    // ReplicatedEntityEntry
    // ============================================================================

    /**
     * @brief Internal bookkeeping for a single replicated entity.
     *
     * Holds the field set (dirty bits + visibility), plus user-provided
     * callbacks for serializing and deserializing individual fields.
     */
    struct ReplicatedEntityEntry
    {
        NetworkEntityID entityID = 0;
        ReplicatedFieldSet fieldSet;
        FieldSerializer serializer;
        FieldDeserializer deserializer;
    };

    // ============================================================================
    // EntityReplicator
    // ============================================================================

    /**
     * @brief Central manager for entity state replication with dirty tracking.
     *
     * Typical server-side flow each tick:
     *   1. Game logic modifies ReplicatedField<T> values (dirty bits set automatically)
     *   2. Call GatherDirtyEntities() to find what needs sending
     *   3. For each dirty entity, call WriteUpdatePacket() with the observer's visibility
     *   4. Send the resulting buffers over the network
     *   5. Call FlushDirty() to reset all dirty bits
     *
     * Client-side flow:
     *   1. Receive a packet from the server
     *   2. Call ProcessIncoming() to apply the state to local entities
     */
    class EntityReplicator
    {
      public:
        EntityReplicator() = default;

        // -- Entity registration -------------------------------------------------

        /**
         * @brief Register an entity for replication.
         * @param entityID    Network ID (must be unique)
         * @param fieldSet    Configured field set with registered fields
         * @param serializer  Callback to serialize individual fields
         * @param deserializer Callback to deserialize individual fields
         */
        void RegisterEntity(NetworkEntityID entityID, const ReplicatedFieldSet& fieldSet, FieldSerializer serializer,
                            FieldDeserializer deserializer);

        /// @brief Remove an entity from replication tracking
        void UnregisterEntity(NetworkEntityID entityID);

        /// @brief Check if an entity is registered
        bool IsEntityRegistered(NetworkEntityID entityID) const;

        /// @brief Get mutable access to an entity's field set (for marking dirty)
        ReplicatedFieldSet* GetFieldSet(NetworkEntityID entityID);

        // -- Packet construction -------------------------------------------------

        /**
         * @brief Write full state for an entity (used when it first enters an observer's view).
         *
         * Format: [entityID:u32] [fieldCount:u8] [allFieldsMask:u64] [field0..fieldN data]
         *
         * @param entityID  Entity to serialize
         * @param buffer    Destination byte buffer
         * @return true if entity was found and serialized
         */
        bool WriteCreatePacket(NetworkEntityID entityID, std::vector<uint8_t>& buffer) const;

        /**
         * @brief Write delta update containing only dirty fields visible to the observer.
         *
         * Format: [entityID:u32] [dirtyMask:u64] [dirty field data...]
         *
         * @param entityID          Entity to serialize
         * @param buffer            Destination byte buffer
         * @param observerVisibility The highest visibility level the observer qualifies for
         * @return true if entity was found and had dirty visible fields
         */
        bool WriteUpdatePacket(NetworkEntityID entityID, std::vector<uint8_t>& buffer,
                               FieldVisibility observerVisibility) const;

        // -- Incoming state application ------------------------------------------

        /**
         * @brief Apply an incoming state update to local entities.
         *
         * Reads [entityID:u32] [dirtyMask:u64] [field data...] from the buffer
         * and invokes the entity's deserializer for each flagged field.
         *
         * @param buffer   Source byte buffer
         * @param readPos  Read offset into buffer (updated on return)
         * @return true if the update was applied successfully
         */
        bool ProcessIncoming(const std::vector<uint8_t>& buffer, size_t& readPos);

        // -- Dirty state management ----------------------------------------------

        /// @brief Return IDs of all entities that have at least one dirty field
        std::vector<NetworkEntityID> GatherDirtyEntities() const;

        /// @brief Clear dirty bits for all registered entities (call after network send)
        void FlushDirty();

        /// @brief Clear dirty bits for a single entity
        void FlushDirty(NetworkEntityID entityID);

        /// @brief Number of currently registered entities
        size_t GetEntityCount() const;

      private:
        std::unordered_map<NetworkEntityID, ReplicatedEntityEntry> m_entities;
    };

} // namespace Spark::Net
