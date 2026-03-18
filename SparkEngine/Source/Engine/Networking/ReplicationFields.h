/**
 * @file ReplicationFields.h
 * @brief Typed replicated field system with automatic dirty tracking (TrinityCore-inspired)
 *
 * Replaces string-based manual MarkPropertyDirty with typed UpdateField-style
 * templates. Each ReplicatedField<T> automatically sets its dirty bit when the
 * value changes, enabling delta-only network updates.
 *
 * Usage:
 *   ReplicatedField<float> m_health{0, FieldVisibility::Public};
 *   m_health.Set(75.0f);          // automatically marks dirty
 *   if (m_health.IsDirty()) { ... }
 */

#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>
#include <cstring>

namespace Spark::Net
{

    // Forward declaration — defined in NetworkManager.h
    class NetBuffer;

    // ============================================================================
    // Field Visibility
    // ============================================================================

    /// Determines which observers receive updates for a replicated field.
    enum class FieldVisibility : uint8_t
    {
        Public,    ///< All connected clients
        Private,   ///< Owner client only
        Party,     ///< Owner's party members
        Spectator, ///< Spectators only
        Count
    };

    // ============================================================================
    // ReplicatedField<T>
    // ============================================================================

    /**
     * @brief Wraps a value with automatic dirty tracking and visibility metadata.
     *
     * When Set() is called with a different value, the dirty bit is raised.
     * The field index (0-63) maps to a bit position in ReplicatedFieldSet's
     * dirty bitmask, enabling efficient batch serialization of only changed fields.
     *
     * @tparam T Value type — must be equality-comparable and trivially copyable
     *           for network serialization (float, int32_t, uint32_t, bool, etc.)
     */
    template <typename T> class ReplicatedField
    {
      public:
        ReplicatedField(uint8_t fieldIndex, FieldVisibility visibility = FieldVisibility::Public)
            : m_fieldIndex(fieldIndex), m_visibility(visibility)
        {
        }

        /// @brief Read the current value
        const T& Get() const { return m_value; }

        /// @brief Set a new value; marks dirty only if the value actually changed
        void Set(const T& value)
        {
            if (m_value != value)
            {
                m_value = value;
                m_dirty = true;
            }
        }

        /// @brief Implicit read-only conversion for convenience
        operator const T&() const { return m_value; }

        bool IsDirty() const { return m_dirty; }

        void ClearDirty() { m_dirty = false; }

        uint8_t GetFieldIndex() const { return m_fieldIndex; }

        FieldVisibility GetVisibility() const { return m_visibility; }

        /// @brief Write the raw value bytes into a byte buffer
        void Serialize(std::vector<uint8_t>& buffer) const
        {
            static_assert(std::is_trivially_copyable_v<T>, "ReplicatedField<T> requires trivially copyable T");
            const auto offset = buffer.size();
            buffer.resize(offset + sizeof(T));
            std::memcpy(buffer.data() + offset, &m_value, sizeof(T));
        }

        /// @brief Read raw value bytes from a byte buffer at the given position
        /// @return Number of bytes consumed
        size_t Deserialize(const std::vector<uint8_t>& buffer, size_t readPos)
        {
            static_assert(std::is_trivially_copyable_v<T>, "ReplicatedField<T> requires trivially copyable T");
            if (readPos + sizeof(T) > buffer.size())
            {
                return 0;
            }
            std::memcpy(&m_value, buffer.data() + readPos, sizeof(T));
            return sizeof(T);
        }

      private:
        T m_value{};
        uint8_t m_fieldIndex;
        FieldVisibility m_visibility;
        bool m_dirty = true; ///< Dirty on creation — send initial state to all observers
    };

    // ============================================================================
    // ReplicatedFieldSet
    // ============================================================================

    /// Maximum number of replicated fields per entity (one bit per field in a uint64_t).
    constexpr uint8_t MAX_REPLICATED_FIELDS = 64;

    /**
     * @brief Manages dirty bitmask and visibility masks for a set of replicated fields.
     *
     * Each entity that participates in replication owns one ReplicatedFieldSet.
     * The set tracks which field indices are dirty and which visibility level
     * each field belongs to, enabling the EntityReplicator to efficiently
     * gather and serialize only the changed fields visible to a given observer.
     */
    class ReplicatedFieldSet
    {
      public:
        ReplicatedFieldSet() = default;

        /// @brief Register a field index with its visibility level
        void RegisterField(uint8_t index, FieldVisibility visibility);

        /// @brief Mark a single field as dirty by index
        void MarkDirty(uint8_t fieldIndex);

        /// @brief Clear all dirty bits (call after network send)
        void ClearAllDirty();

        /// @brief Check if any field is dirty
        bool IsAnyDirty() const;

        /// @brief Check if a specific field is dirty
        bool IsFieldDirty(uint8_t fieldIndex) const;

        /// @brief Get the raw dirty bitmask
        uint64_t GetDirtyMask() const;

        /// @brief Get dirty mask filtered by visibility level.
        ///        Returns only bits for fields visible at the given level.
        uint64_t GetDirtyMaskForVisibility(FieldVisibility visibility) const;

        /// @brief Get the visibility mask for a given level
        uint64_t GetVisibilityMask(FieldVisibility visibility) const;

        /// @brief Write the dirty bitmask into a byte buffer
        void WriteDirtyMask(std::vector<uint8_t>& buffer) const;

        /// @brief Read a dirty bitmask from a byte buffer
        /// @return Number of bytes consumed (8) or 0 on failure
        size_t ReadDirtyMask(const std::vector<uint8_t>& buffer, size_t readPos);

        /// @brief Get the number of registered fields
        uint8_t GetFieldCount() const;

      private:
        uint64_t m_dirtyBits = 0;

        /// One bitmask per visibility level — bit N is set if field N has that visibility
        uint64_t m_visibilityMasks[static_cast<size_t>(FieldVisibility::Count)] = {};

        uint8_t m_fieldCount = 0;
    };

} // namespace Spark::Net
