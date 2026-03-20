/**
 * @file ConstantBufferDiff.h
 * @brief Content-diff constant buffer uploads to skip redundant GPU updates
 *
 * Caches the last-uploaded constant buffer contents and memcmp's before
 * UpdateSubresource() to skip
 * uploads when data hasn't changed.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{
    namespace Graphics
    {

        /**
         * @brief Cached constant buffer slot with content comparison.
         */
        struct CBSlotCache
        {
            std::vector<uint8_t> lastData;
            uint64_t updateCount = 0;
            uint64_t skipCount = 0;

            /**
             * @brief Compare new data against cached. Returns true if different.
             */
            bool NeedsUpdate(const void* data, size_t size) const
            {
                if (lastData.size() != size)
                    return true;
                return std::memcmp(lastData.data(), data, size) != 0;
            }

            /**
             * @brief Cache the new data after a successful upload.
             */
            void CacheData(const void* data, size_t size)
            {
                lastData.resize(size);
                std::memcpy(lastData.data(), data, size);
                updateCount++;
            }

            float GetSkipRate() const
            {
                uint64_t total = updateCount + skipCount;
                return total > 0 ? static_cast<float>(skipCount) / static_cast<float>(total) : 0.0f;
            }
        };

        /**
         * @brief Manager for content-diff constant buffer uploads.
         *
         * Tracks per-slot CB content and skips redundant uploads.
         * Provides metrics for tuning constant buffer update frequency.
         */
        class ConstantBufferDiffManager
        {
          public:
            static ConstantBufferDiffManager& GetInstance()
            {
                static ConstantBufferDiffManager instance;
                return instance;
            }

            void Initialize() { m_slots.clear(); }
            void Shutdown() { m_slots.clear(); }

            /**
             * @brief Check if a CB update is needed and cache if so.
             *
             * @param slotKey Unique key identifying the CB slot (e.g. "VS_PerObject")
             * @param data    Pointer to the new constant buffer data
             * @param size    Size in bytes
             * @return true if the data differs and an upload is needed
             */
            bool ShouldUpdate(const std::string& slotKey, const void* data, size_t size)
            {
                auto& slot = m_slots[slotKey];
                if (slot.NeedsUpdate(data, size))
                {
                    slot.CacheData(data, size);
                    return true;
                }
                slot.skipCount++;
                m_totalSkipped++;
                return false;
            }

            /**
             * @brief Reset all cached slots at frame start if desired.
             */
            void BeginFrame() { m_frameSkipped = 0; }

            struct Metrics
            {
                size_t trackedSlots = 0;
                uint64_t totalSkipped = 0;
                float averageSkipRate = 0.0f;
            };

            Metrics GetMetrics() const
            {
                Metrics m;
                m.trackedSlots = m_slots.size();
                m.totalSkipped = m_totalSkipped;
                float totalRate = 0.0f;
                for (const auto& [key, slot] : m_slots)
                    totalRate += slot.GetSkipRate();
                m.averageSkipRate = m.trackedSlots > 0 ? totalRate / static_cast<float>(m.trackedSlots) : 0.0f;
                return m;
            }

            std::string Console_GetReport() const
            {
                auto m = GetMetrics();
                return "CB Diff: " + std::to_string(m.trackedSlots) + " slots, " + std::to_string(m.totalSkipped) +
                       " skipped, avg skip rate " + std::to_string(static_cast<int>(m.averageSkipRate * 100.0f)) + "%";
            }

          private:
            ConstantBufferDiffManager() = default;

            std::unordered_map<std::string, CBSlotCache> m_slots;
            uint64_t m_totalSkipped = 0;
            uint64_t m_frameSkipped = 0;
        };

    } // namespace Graphics
} // namespace Spark
