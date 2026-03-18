/**
 * @file ReplicationFields.cpp
 * @brief Implementation of ReplicatedFieldSet dirty tracking and visibility masks
 */

#include "ReplicationFields.h"

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
            return;
        }

        const uint64_t bit = 1ULL << index;
        m_visibilityMasks[static_cast<size_t>(visibility)] |= bit;

        if (index >= m_fieldCount)
        {
            m_fieldCount = index + 1;
        }

        // New fields start dirty so their initial value is sent
        m_dirtyBits |= bit;
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

    size_t ReplicatedFieldSet::ReadDirtyMask(const std::vector<uint8_t>& buffer, size_t readPos)
    {
        if (readPos + sizeof(uint64_t) > buffer.size())
        {
            return 0;
        }
        std::memcpy(&m_dirtyBits, buffer.data() + readPos, sizeof(uint64_t));
        return sizeof(uint64_t);
    }

    uint8_t ReplicatedFieldSet::GetFieldCount() const
    {
        return m_fieldCount;
    }

} // namespace Spark::Net
