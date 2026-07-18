/**
 * @file DeltaSnapshotManager.h
 * @brief Per-connection delta snapshot tracking for replicated entity fields
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks which replicated fields have changed since the last acknowledged send
 * to each connection. BuildDeltaPacket compares the current entity state against
 * what was last acknowledged by a given connection, and returns a minimal packet
 * containing only the changed fields. Works alongside ReplicationFields.
 */

#pragma once
#include "../../Core/Platform.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Net
{

    // ============================================================================
    // Field Snapshot
    // ============================================================================

    /**
     * @brief Snapshot of a single replicated field's serialized state.
     *
     * Stores the raw bytes and a precomputed hash for fast equality checks
     * when building delta packets.
     */
    struct FieldSnapshot
    {
        uint8_t fieldIndex = 0;               ///< Index of the replicated field (0-63)
        std::vector<uint8_t> serializedValue; ///< Raw serialized bytes of the field value
        uint64_t valueHash = 0;               ///< FNV-1a hash of serializedValue for fast comparison
    };

    // ============================================================================
    // Connection Snapshot
    // ============================================================================

    /**
     * @brief Per-connection record of what field state has been acknowledged.
     *
     * Each connection maintains its own view of the last-sent entity state
     * so that deltas are computed independently per observer.
     */
    struct ConnectionSnapshot
    {
        uint32_t connectionId = 0;                 ///< Owning connection identifier
        uint32_t lastAckedSequence = 0;            ///< Highest sequence number acknowledged by this connection
        std::vector<FieldSnapshot> lastSentFields; ///< Field states at time of last acknowledged send
    };

    // ============================================================================
    // DeltaSnapshotManager
    // ============================================================================

    /**
     * @brief Singleton that manages per-connection delta snapshots for entity replication.
     *
     * The replication pipeline records the current entity state via RecordEntityState(),
     * then calls BuildDeltaPacket() for each connection to produce a minimal update
     * containing only the fields that differ from what that connection last acknowledged.
     *
     * Thread safety: all public methods are guarded by an internal mutex so that
     * the networking thread can acknowledge sequences while the game thread records
     * state and builds packets.
     */
    class DeltaSnapshotManager
    {
      public:
        /// @brief Get the singleton instance
        static DeltaSnapshotManager& GetInstance()
        {
            static DeltaSnapshotManager instance;
            return instance;
        }

        DeltaSnapshotManager(const DeltaSnapshotManager&) = delete;
        DeltaSnapshotManager& operator=(const DeltaSnapshotManager&) = delete;

        /// Upper bound on unacknowledged deltas kept per connection. A client
        /// that never echoes DeltaAck (old build, hostile, or silently dropping
        /// packets) must not grow server memory without limit: once the window
        /// is exceeded, the oldest pending deltas are dropped. Their fields
        /// simply remain unacked and are resent in later deltas.
        static constexpr size_t kMaxPendingDeltasPerConnection = 256;

        // ----- Connection lifecycle -----

        /// @brief Register a new connection for delta tracking
        /// @param connectionId  Unique identifier for the connection
        void RegisterConnection(uint32_t connectionId);

        /// @brief Remove a connection and free its snapshot history
        /// @param connectionId  Connection to remove
        void UnregisterConnection(uint32_t connectionId);

        // ----- State recording -----

        /// @brief Record the current replicated state of an entity
        /// @param entityId  Entity whose state is being recorded
        /// @param fields    Serialized snapshots of all replicated fields
        void RecordEntityState(uint32_t entityId, const std::vector<FieldSnapshot>& fields);

        // ----- Delta building -----

        /**
         * @brief Build a delta packet for a specific connection and entity.
         *
         * Compares the current entity state (from RecordEntityState) against what
         * was last acknowledged by the given connection. Returns a byte buffer
         * containing only the changed fields.
         *
         * Packet format:
         *   [uint32_t entityId] [uint32_t sequence] [uint8_t fieldCount]
         *   For each changed field:
         *     [uint8_t fieldIndex] [uint16_t dataSize] [uint8_t[] data]
         *
         * @param connectionId  Target connection
         * @param entityId      Entity to build delta for
         * @return Serialized delta packet, or empty vector if nothing changed
         */
        std::vector<uint8_t> BuildDeltaPacket(uint32_t connectionId, uint32_t entityId);

        // ----- Acknowledgement -----

        /// @brief Mark a sequence number as acknowledged by a connection
        ///
        /// Acks are cumulative (latest wins): acknowledging sequence S releases
        /// every pending delta up to and including S. Stale or out-of-order acks
        /// (a sequence at or below the last acknowledged one, or one whose
        /// pending record no longer exists) are ignored — the baseline never
        /// regresses.
        ///
        /// @param connectionId  Connection that sent the ack
        /// @param sequence      Sequence number being acknowledged
        void AcknowledgeSequence(uint32_t connectionId, uint32_t sequence);

        // ----- Queries -----

        /// @brief Get the number of unacknowledged delta packets for a connection
        /// @param connectionId  Connection to query
        /// @return Number of pending (unacked) deltas
        size_t GetPendingDeltaCount(uint32_t connectionId) const;

        /// @brief Get a human-readable status string for console display
        /// @return Formatted status string
        std::string Console_GetStatus() const;

        /// @brief Release all resources and clear all tracked state
        void Shutdown();

      private:
        DeltaSnapshotManager() = default;

        /// @brief Compute FNV-1a hash of a byte buffer
        /// @param data  Pointer to the data
        /// @param size  Number of bytes
        /// @return 64-bit hash
        static uint64_t ComputeHash(const uint8_t* data, size_t size);

        /// Pending delta record: a packet awaiting acknowledgement
        struct PendingDelta
        {
            uint32_t sequence = 0;
            uint32_t entityId = 0;
            std::vector<FieldSnapshot> sentFields; ///< Fields that were included in this send
        };

        /// Per-connection tracking data
        struct ConnectionState
        {
            uint32_t connectionId = 0;
            uint32_t lastAckedSequence = 0;
            uint32_t nextSequence = 1;

            /// entityId -> last-acked field snapshots
            std::unordered_map<uint32_t, std::vector<FieldSnapshot>> ackedEntityFields;

            /// Unacknowledged delta packets, keyed by sequence number
            std::unordered_map<uint32_t, PendingDelta> pendingDeltas;
        };

        mutable std::mutex m_mutex;

        /// connectionId -> connection state
        std::unordered_map<uint32_t, ConnectionState> m_connections;

        /// entityId -> current field snapshots (the "ground truth" recorded this frame)
        std::unordered_map<uint32_t, std::vector<FieldSnapshot>> m_currentEntityStates;
    };

} // namespace Spark::Net
