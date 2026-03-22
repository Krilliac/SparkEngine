/**
 * @file ReflectionProbeCache.h
 * @brief Cached reflection probe cubemaps with LRU eviction
 * @author Spark Engine Team
 * @date 2025
 *
 * Caches rendered reflection probe cubemaps to avoid re-rendering every frame.
 * Static probes render once and cache until invalidated. Dynamic probes render
 * on a staggered schedule (one face per frame or one probe per N frames).
 *
 * Features: parallax correction, priority-based rendering budget, LRU eviction.
 *
 * @see LightProbeSystem.h, HybridRT/ProbeSystem.h
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Reflection Probe
    // =========================================================================

    struct ReflectionProbe
    {
        uint32_t probeId = 0;
        float posX = 0, posY = 0, posZ = 0;          ///< Probe world position
        float boxMinX = 0, boxMinY = 0, boxMinZ = 0; ///< Influence box min (for parallax correction)
        float boxMaxX = 0, boxMaxY = 0, boxMaxZ = 0; ///< Influence box max
        float blendDistance = 1.0f;                  ///< Fade distance at box edges
        float importance = 1.0f;                     ///< Priority weight for rendering budget
        int priority = 0;                            ///< Higher = rendered first
        bool isStatic = true;                        ///< Static probes render once
        uint32_t resolution = 256;                   ///< Cubemap face resolution

        // State
        bool needsRender = true;
        uint32_t lastRenderedFrame = 0;
        int nextFaceToRender = 0; ///< For staggered rendering (0-5 for cubemap faces)
    };

    // =========================================================================
    // Probe Cache Entry
    // =========================================================================

    struct ProbeCacheEntry
    {
        uint32_t probeId = 0;
        uint32_t resolution = 0;
        uint64_t sceneHash = 0; ///< Hash of scene state when rendered
        uint32_t renderedFrame = 0;
        uint32_t lastAccessedFrame = 0;
        bool valid = false;

        // Cache slot in the texture array/atlas
        uint32_t cacheSlot = UINT32_MAX;
    };

    // =========================================================================
    // Reflection Probe Cache
    // =========================================================================

    /**
     * @brief Manages cached reflection probe cubemaps
     *
     * Maintains a fixed-size cache (texture array or atlas) of cubemap renders.
     * Static probes are rendered once and cached indefinitely.
     * Dynamic probes re-render on a budget (N probes per frame).
     * LRU eviction when cache is full.
     */
    class ReflectionProbeCache
    {
      public:
        ReflectionProbeCache() = default;
        ~ReflectionProbeCache() = default;

        /**
         * @brief Initialize the probe cache
         * @param maxCachedProbes Maximum number of probe cubemaps to cache
         * @param renderBudget   Max probe renders per frame (0 = unlimited)
         */
        void Initialize(uint32_t maxCachedProbes = 64, uint32_t renderBudget = 4)
        {
            m_maxSlots = maxCachedProbes;
            m_renderBudget = renderBudget;
            m_cacheSlotUsed.resize(maxCachedProbes, false);
            m_entries.clear();
            m_probes.clear();
            m_frameIndex = 0;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_entries.clear();
            m_probes.clear();
            m_cacheSlotUsed.clear();
            m_initialized = false;
        }

        /** @brief Register a reflection probe */
        void RegisterProbe(const ReflectionProbe& probe) { m_probes[probe.probeId] = probe; }

        /** @brief Remove a probe and free its cache slot */
        void UnregisterProbe(uint32_t probeId)
        {
            auto cit = m_entries.find(probeId);
            if (cit != m_entries.end())
            {
                FreeCacheSlot(cit->second.cacheSlot);
                m_entries.erase(cit);
            }
            m_probes.erase(probeId);
        }

        /**
         * @brief Update probes for the current frame
         *
         * Determines which probes need rendering, respects the per-frame budget,
         * and returns the list of probe IDs to render.
         */
        std::vector<uint32_t> Update(float cameraX, float cameraY, float cameraZ)
        {
            m_frameIndex++;
            std::vector<uint32_t> toRender;

            // Collect probes that need rendering, sorted by priority and distance
            struct RenderCandidate
            {
                uint32_t probeId;
                float score; // Higher = should render sooner
            };
            std::vector<RenderCandidate> candidates;

            for (auto& [id, probe] : m_probes)
            {
                // Access the cache entry
                auto cit = m_entries.find(id);
                bool cached = (cit != m_entries.end() && cit->second.valid);

                // Static probes only render once
                if (probe.isStatic && cached)
                {
                    cit->second.lastAccessedFrame = m_frameIndex;
                    continue;
                }

                // Dynamic probes or uncached static probes need rendering
                if (!cached || probe.needsRender)
                {
                    float dx = probe.posX - cameraX;
                    float dy = probe.posY - cameraY;
                    float dz = probe.posZ - cameraZ;
                    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    float distScore = 1.0f / (1.0f + dist * 0.1f);

                    candidates.push_back({id, probe.importance * distScore + probe.priority});
                }
            }

            // Sort by score (highest first)
            std::sort(candidates.begin(), candidates.end(),
                      [](const RenderCandidate& a, const RenderCandidate& b) { return a.score > b.score; });

            // Respect render budget
            uint32_t budget = m_renderBudget > 0 ? m_renderBudget : static_cast<uint32_t>(candidates.size());
            for (size_t i = 0; i < candidates.size() && i < budget; ++i)
            {
                uint32_t pid = candidates[i].probeId;

                // Ensure cache slot
                auto& entry = m_entries[pid];
                if (entry.cacheSlot == UINT32_MAX)
                {
                    uint32_t slot = AllocateCacheSlot();
                    if (slot == UINT32_MAX)
                    {
                        // Evict LRU
                        slot = EvictLRU();
                        if (slot == UINT32_MAX)
                            continue;
                    }
                    entry.cacheSlot = slot;
                }

                entry.probeId = pid;
                entry.renderedFrame = m_frameIndex;
                entry.lastAccessedFrame = m_frameIndex;
                entry.valid = true;

                m_probes[pid].needsRender = false;
                m_probes[pid].lastRenderedFrame = m_frameIndex;

                toRender.push_back(pid);
            }

            return toRender;
        }

        /** @brief Invalidate a probe's cache (e.g., scene changed near it) */
        void InvalidateProbe(uint32_t probeId)
        {
            auto cit = m_entries.find(probeId);
            if (cit != m_entries.end())
                cit->second.valid = false;

            auto pit = m_probes.find(probeId);
            if (pit != m_probes.end())
                pit->second.needsRender = true;
        }

        /** @brief Invalidate all probes */
        void InvalidateAll()
        {
            for (auto& [id, entry] : m_entries)
                entry.valid = false;
            for (auto& [id, probe] : m_probes)
                probe.needsRender = true;
        }

        /** @brief Get cache slot for a probe (UINT32_MAX if not cached) */
        uint32_t GetCacheSlot(uint32_t probeId) const
        {
            auto it = m_entries.find(probeId);
            if (it != m_entries.end() && it->second.valid)
                return it->second.cacheSlot;
            return UINT32_MAX;
        }

        const ReflectionProbe* GetProbe(uint32_t probeId) const
        {
            auto it = m_probes.find(probeId);
            return it != m_probes.end() ? &it->second : nullptr;
        }

        // --- Metrics ---
        uint32_t GetProbeCount() const { return static_cast<uint32_t>(m_probes.size()); }
        uint32_t GetCachedCount() const
        {
            uint32_t count = 0;
            for (const auto& [id, entry] : m_entries)
                if (entry.valid)
                    count++;
            return count;
        }
        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            return "ReflectionProbeCache: " + std::to_string(m_probes.size()) + " probes, " +
                   std::to_string(GetCachedCount()) + " cached, budget=" + std::to_string(m_renderBudget) + "/frame\n";
        }

      private:
        uint32_t AllocateCacheSlot()
        {
            for (uint32_t i = 0; i < m_maxSlots; ++i)
            {
                if (!m_cacheSlotUsed[i])
                {
                    m_cacheSlotUsed[i] = true;
                    return i;
                }
            }
            return UINT32_MAX;
        }

        void FreeCacheSlot(uint32_t slot)
        {
            if (slot < m_maxSlots)
                m_cacheSlotUsed[slot] = false;
        }

        uint32_t EvictLRU()
        {
            uint32_t oldestFrame = UINT32_MAX;
            uint32_t evictId = UINT32_MAX;

            for (const auto& [id, entry] : m_entries)
            {
                if (entry.valid && entry.lastAccessedFrame < oldestFrame)
                {
                    oldestFrame = entry.lastAccessedFrame;
                    evictId = id;
                }
            }

            if (evictId != UINT32_MAX)
            {
                uint32_t slot = m_entries[evictId].cacheSlot;
                m_entries[evictId].valid = false;
                m_entries[evictId].cacheSlot = UINT32_MAX;
                FreeCacheSlot(slot);
                return AllocateCacheSlot();
            }

            return UINT32_MAX;
        }

        std::unordered_map<uint32_t, ReflectionProbe> m_probes;
        std::unordered_map<uint32_t, ProbeCacheEntry> m_entries;
        std::vector<bool> m_cacheSlotUsed;
        uint32_t m_maxSlots = 64;
        uint32_t m_renderBudget = 4;
        uint32_t m_frameIndex = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
