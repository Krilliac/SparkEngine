/**
 * @file ReplicationFields.cpp
 * @brief Implementation of ReplicatedFieldSet dirty tracking and visibility masks
 */

#include "ReplicationFields.h"

#include "../../Utils/LogMacros.h"
#include <cstring>

namespace Spark::Net
{

    // ============================================================================
    // ReplicatedFieldSet
    // ============================================================================

    void ReplicatedFieldSet::RegisterField(uint8_t index, FieldVisibility visibility)
    {
        if (index >= MAX_REPLICATED_FIELDS)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "RegisterField: index %u exceeds MAX_REPLICATED_FIELDS (%u)",
                           index, MAX_REPLICATED_FIELDS);
            return;
        }

        const auto visibilityIndex = static_cast<size_t>(visibility);
        if (visibilityIndex >= static_cast<size_t>(FieldVisibility::Count))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "RegisterField: visibility value %zu is out of range [0, %zu)",
                           visibilityIndex, static_cast<size_t>(FieldVisibility::Count));
            return;
        }

        const uint64_t bit = 1ULL << index;
        m_visibilityMasks[visibilityIndex] |= bit;

        if (index >= m_fieldCount)
        {
            m_fieldCount = index + 1;
        }

        // New fields start dirty so their initial value is sent
        m_dirtyBits |= bit;
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Registered replicated field %u (visibility: %d, total: %u)",
                        index, static_cast<int>(visibility), m_fieldCount);
    }

    void ReplicatedFieldSet::MarkDirty(uint8_t fieldIndex)
    {
        if (fieldIndex < MAX_REPLICATED_FIELDS)
        {
            m_dirtyBits |= (1ULL << fieldIndex);
        }
    }

    void ReplicatedFieldSet::ClearAllDirty()
    {
        m_dirtyBits = 0;
    }

    bool ReplicatedFieldSet::IsAnyDirty() const
    {
        return m_dirtyBits != 0;
    }

    bool ReplicatedFieldSet::IsFieldDirty(uint8_t fieldIndex) const
    {
        if (fieldIndex >= MAX_REPLICATED_FIELDS)
        {
            return false;
        }
        return (m_dirtyBits >> fieldIndex) & 1;
    }

    uint64_t ReplicatedFieldSet::GetDirtyMask() const
    {
        return m_dirtyBits;
    }

    uint64_t ReplicatedFieldSet::GetDirtyMaskForVisibility(FieldVisibility visibility) const
    {
        return m_dirtyBits & GetVisibilityMask(visibility);
    }

    uint64_t ReplicatedFieldSet::GetVisibilityMask(FieldVisibility visibility) const
    {
        const auto idx = static_cast<size_t>(visibility);
        if (idx >= static_cast<size_t>(FieldVisibility::Count))
        {
            return 0;
        }
        return m_visibilityMasks[idx];
    }

    void ReplicatedFieldSet::WriteDirtyMask(std::vector<uint8_t>& buffer) const
    {
        const auto offset = buffer.size();
        buffer.resize(offset + sizeof(uint64_t));
        std::memcpy(buffer.data() + offset, &m_dirtyBits, sizeof(uint64_t));
    }

    bool ReplicatedFieldSet::ReadDirtyMask(const std::vector<uint8_t>& buffer, size_t readPos)
    {
        // Subtraction form: `readPos + kDirtyMaskBytes` is an unchecked size_t
        // addition, and the operand comes from a decode cursor.
        if (buffer.size() < kDirtyMaskBytes || readPos > buffer.size() - kDirtyMaskBytes)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "ReadDirtyMask: buffer too small (readPos: %zu, bufSize: %zu)",
                           readPos, buffer.size());
            // The mask is deliberately left untouched; the caller must abandon the packet.
            return false;
        }
        std::memcpy(&m_dirtyBits, buffer.data() + readPos, kDirtyMaskBytes);
        return true;
    }

    uint8_t ReplicatedFieldSet::GetFieldCount() const
    {
        return m_fieldCount;
    }

} // namespace Spark::Net
