/**
 * @file PersistentMaterialCB.h
 * @brief Persistent GPU material constant buffers (SRP Batcher pattern)
 * @author Spark Engine Team
 * @date 2025
 *
 * Keeps material constant buffers resident in GPU memory rather than
 * re-uploading every frame. Only updates when material properties actually
 * change. Inspired by Unity's SRP Batcher which reduces render state
 * changes by sorting objects by shader variant and keeping CBs persistent.
 *
 * @see MaterialSystem.h, DrawSortKey.h
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Material CB Slot
    // =========================================================================

    struct MaterialCBSlot
    {
        uint32_t materialId = 0;
        uint64_t dataHash = 0;         ///< Hash of last-uploaded data
        uint32_t bufferOffset = 0;     ///< Offset in the mega-buffer
        uint32_t bufferSize = 0;       ///< Size of this material's CB data
        uint32_t lastUpdatedFrame = 0; ///< Frame when data was last changed
        bool needsUpload = true;       ///< Needs GPU upload this frame
    };

    // =========================================================================
    // Persistent Material CB Manager
    // =========================================================================

    /**
     * @brief Manages persistent GPU constant buffers for materials
     *
     * Instead of creating per-material CBs, allocates a large "mega-buffer"
     * and sub-allocates regions for each material. Materials only re-upload
     * when their properties change (detected via data hash).
     *
     * Draw sorting groups objects by shader variant → material CB offset,
     * minimizing GPU state changes (SRP Batcher pattern).
     */
    class PersistentMaterialCBManager
    {
      public:
        PersistentMaterialCBManager() = default;
        ~PersistentMaterialCBManager() = default;

        /**
         * @brief Initialize the mega-buffer
         * @param maxMaterials  Maximum number of materials
         * @param cbSizePerMat  Constant buffer size per material (bytes, 16-byte aligned)
         */
        void Initialize(uint32_t maxMaterials = 4096, uint32_t cbSizePerMat = 256)
        {
            m_maxMaterials = maxMaterials;
            m_cbSizePerMat = (cbSizePerMat + 15) & ~15u; // 16-byte align
            m_megaBufferSize = m_maxMaterials * m_cbSizePerMat;
            m_cpuShadow.resize(m_megaBufferSize, 0);
            m_slots.clear();
            m_nextOffset = 0;
            m_frameIndex = 0;
            m_dirtyCount = 0;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_cpuShadow.clear();
            m_slots.clear();
            m_initialized = false;
        }

        /**
         * @brief Register a material and allocate CB space
         * @return Buffer offset for this material, or UINT32_MAX on failure
         */
        uint32_t RegisterMaterial(uint32_t materialId)
        {
            if (!m_initialized)
                return UINT32_MAX;

            auto it = m_slots.find(materialId);
            if (it != m_slots.end())
                return it->second.bufferOffset;

            if (m_nextOffset + m_cbSizePerMat > m_megaBufferSize)
                return UINT32_MAX;

            MaterialCBSlot slot;
            slot.materialId = materialId;
            slot.bufferOffset = m_nextOffset;
            slot.bufferSize = m_cbSizePerMat;
            slot.needsUpload = true;

            m_slots[materialId] = slot;
            m_nextOffset += m_cbSizePerMat;
            return slot.bufferOffset;
        }

        /**
         * @brief Update material data (CPU-side shadow buffer)
         *
         * Only marks for GPU upload if data actually changed (hash comparison).
         */
        bool UpdateMaterial(uint32_t materialId, const void* data, uint32_t dataSize)
        {
            auto it = m_slots.find(materialId);
            if (it == m_slots.end())
                return false;

            auto& slot = it->second;
            uint32_t copySize = std::min(dataSize, slot.bufferSize);

            // Hash the new data
            uint64_t newHash = HashData(static_cast<const uint8_t*>(data), copySize);
            if (newHash == slot.dataHash)
                return false; // No change

            // Copy to CPU shadow buffer
            memcpy(m_cpuShadow.data() + slot.bufferOffset, data, copySize);
            slot.dataHash = newHash;
            slot.needsUpload = true;
            slot.lastUpdatedFrame = m_frameIndex;
            m_dirtyCount++;
            return true;
        }

        /**
         * @brief Get materials that need GPU upload this frame
         */
        std::vector<MaterialCBSlot*> GetDirtySlots()
        {
            std::vector<MaterialCBSlot*> dirty;
            for (auto& [id, slot] : m_slots)
            {
                if (slot.needsUpload)
                    dirty.push_back(&slot);
            }
            return dirty;
        }

        /**
         * @brief Mark all dirty slots as uploaded (call after GPU upload)
         */
        void ClearDirtyFlags()
        {
            for (auto& [id, slot] : m_slots)
                slot.needsUpload = false;
            m_dirtyCount = 0;
        }

        void BeginFrame() { m_frameIndex++; }

        /** @brief Get CPU shadow buffer pointer at a material's offset */
        const uint8_t* GetMaterialData(uint32_t materialId) const
        {
            auto it = m_slots.find(materialId);
            if (it == m_slots.end())
                return nullptr;
            return m_cpuShadow.data() + it->second.bufferOffset;
        }

        uint32_t GetBufferOffset(uint32_t materialId) const
        {
            auto it = m_slots.find(materialId);
            return it != m_slots.end() ? it->second.bufferOffset : UINT32_MAX;
        }

        // --- Metrics ---
        uint32_t GetRegisteredCount() const { return static_cast<uint32_t>(m_slots.size()); }
        uint32_t GetDirtyCount() const { return m_dirtyCount; }
        uint32_t GetMegaBufferSize() const { return m_megaBufferSize; }
        uint32_t GetUsedSize() const { return m_nextOffset; }
        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            return "PersistentMaterialCB: " + std::to_string(m_slots.size()) + " materials, " +
                   std::to_string(m_nextOffset / 1024) + "/" + std::to_string(m_megaBufferSize / 1024) + "KB used, " +
                   std::to_string(m_dirtyCount) + " dirty\n";
        }

      private:
        static uint64_t HashData(const uint8_t* data, uint32_t size)
        {
            uint64_t hash = 14695981039346656037ULL;
            for (uint32_t i = 0; i < size; ++i)
            {
                hash ^= data[i];
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        std::vector<uint8_t> m_cpuShadow;
        std::unordered_map<uint32_t, MaterialCBSlot> m_slots;
        uint32_t m_maxMaterials = 0;
        uint32_t m_cbSizePerMat = 256;
        uint32_t m_megaBufferSize = 0;
        uint32_t m_nextOffset = 0;
        uint32_t m_frameIndex = 0;
        uint32_t m_dirtyCount = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
