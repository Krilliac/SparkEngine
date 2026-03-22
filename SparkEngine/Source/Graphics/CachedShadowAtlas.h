/**
 * @file CachedShadowAtlas.h
 * @brief Cached shadow atlas that avoids re-rendering static shadow maps
 * @author Spark Engine Team
 * @date 2025
 *
 * Extends the existing ShadowAtlas with shadow caching: static lights that
 * haven't moved or changed only render their shadow map once and reuse it
 * across frames. Only dynamic or invalidated shadows are re-rendered.
 *
 * Two atlases are maintained:
 * - Dynamic atlas: re-rendered every frame (moving lights, moving casters)
 * - Cached atlas: rendered once, invalidated on change
 *
 * @see ShadowAtlas.h, LightingSystem.h
 */

#pragma once

#include "ShadowAtlas.h"

#include <bitset>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Shadow Cache Entry
    // =========================================================================

    /**
     * @brief Tracks the cached state of a shadow map tile
     */
    struct ShadowCacheEntry
    {
        uint32_t lightId = 0;
        uint64_t lightStateHash = 0;       ///< Hash of light transform + params
        uint64_t casterSceneHash = 0;      ///< Hash of shadow casters in light's frustum
        uint32_t renderedFrame = 0;        ///< Frame when shadow was last rendered
        bool isStatic = false;             ///< Light is flagged as static
        bool needsUpdate = true;           ///< Shadow must be re-rendered this frame
        uint32_t updateCooldownFrames = 0; ///< Frames since last update request
    };

    // =========================================================================
    // Shadow Update Request
    // =========================================================================

    /**
     * @brief Request to update a light's shadow map in the cached atlas
     */
    struct ShadowUpdateRequest
    {
        uint32_t lightId = 0;
        float priority = 0.0f;
        bool forceUpdate = false; ///< Bypass caching (e.g., first frame)
        bool isStatic = false;

        // Light state for change detection
        float posX = 0, posY = 0, posZ = 0;
        float dirX = 0, dirY = 0, dirZ = 0;
        float range = 0;
        float spotAngle = 0;
    };

    // =========================================================================
    // Cached Shadow Atlas
    // =========================================================================

    /**
     * @brief Shadow atlas with caching for static lights
     *
     * Manages two separate atlas regions:
     * 1. Cached region: holds shadow maps for static/stable lights
     * 2. Dynamic region: holds shadow maps re-rendered every frame
     *
     * Change detection uses hashes of light state and scene geometry
     * to determine when cached shadows need re-rendering.
     */
    class CachedShadowAtlas
    {
      public:
        CachedShadowAtlas() = default;
        ~CachedShadowAtlas() = default;

        /**
         * @brief Initialize with separate dynamic and cached atlas sizes
         * @param dynamicAtlasSize  Size of the dynamic shadow atlas
         * @param cachedAtlasSize   Size of the cached shadow atlas
         * @param minTileSize       Minimum tile size in both atlases
         */
        bool Initialize(uint32_t dynamicAtlasSize = 2048, uint32_t cachedAtlasSize = 4096, uint32_t minTileSize = 256)
        {
            if (!m_dynamicAtlas.Initialize(dynamicAtlasSize, minTileSize))
                return false;
            if (!m_cachedAtlas.Initialize(cachedAtlasSize, minTileSize))
                return false;

            m_cacheEntries.clear();
            m_frameIndex = 0;
            m_cachedRendersAvoided = 0;
            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_dynamicAtlas.Shutdown();
            m_cachedAtlas.Shutdown();
            m_cacheEntries.clear();
            m_initialized = false;
        }

        /**
         * @brief Begin a new frame — evaluate which shadows need updating
         */
        void BeginFrame()
        {
            m_frameIndex++;
            m_dynamicAtlas.BeginFrame();
            m_cachedAtlas.BeginFrame();
            m_shadowsToRender.clear();
            m_cachedRendersAvoided = 0;
        }

        /**
         * @brief Request shadow rendering for a light
         *
         * Determines whether the light's shadow needs re-rendering or can be
         * served from cache.
         */
        bool RequestShadow(const ShadowUpdateRequest& request)
        {
            if (!m_initialized)
                return false;

            uint64_t stateHash = ComputeLightStateHash(request);

            auto it = m_cacheEntries.find(request.lightId);
            if (it != m_cacheEntries.end())
            {
                auto& entry = it->second;

                // Check if the cached shadow is still valid
                bool stateChanged = (entry.lightStateHash != stateHash);
                bool forceRender = request.forceUpdate;

                if (!stateChanged && !forceRender && entry.isStatic && !entry.needsUpdate)
                {
                    // Cache hit — reuse existing shadow map
                    if (entry.isStatic)
                    {
                        m_cachedAtlas.RequestTile(request.lightId, request.priority,
                                                  GetDesiredTileSize(request.priority));
                    }
                    else
                    {
                        m_dynamicAtlas.RequestTile(request.lightId, request.priority,
                                                   GetDesiredTileSize(request.priority));
                    }
                    m_cachedRendersAvoided++;
                    return true;
                }

                // Cache miss — need to re-render
                entry.lightStateHash = stateHash;
                entry.needsUpdate = true;
                entry.isStatic = request.isStatic;
            }
            else
            {
                // New light — create cache entry
                ShadowCacheEntry entry;
                entry.lightId = request.lightId;
                entry.lightStateHash = stateHash;
                entry.isStatic = request.isStatic;
                entry.needsUpdate = true;
                m_cacheEntries[request.lightId] = entry;
            }

            // Allocate tile and mark for rendering
            ShadowAtlas& atlas = request.isStatic ? m_cachedAtlas : m_dynamicAtlas;
            uint32_t tileSize = GetDesiredTileSize(request.priority);

            if (!atlas.RequestTile(request.lightId, request.priority, tileSize))
                return false;

            m_shadowsToRender.push_back(request.lightId);
            return true;
        }

        /**
         * @brief Get the list of light IDs that need shadow rendering this frame
         *
         * After calling RequestShadow for all lights, iterate this list to
         * render only the shadows that actually need updating.
         */
        const std::vector<uint32_t>& GetShadowsToRender() const { return m_shadowsToRender; }

        /**
         * @brief Mark a light's shadow as successfully rendered
         */
        void MarkRendered(uint32_t lightId)
        {
            auto it = m_cacheEntries.find(lightId);
            if (it != m_cacheEntries.end())
            {
                it->second.renderedFrame = m_frameIndex;
                it->second.needsUpdate = false;
            }
        }

        /**
         * @brief Invalidate a cached shadow (e.g., scene geometry changed near light)
         */
        void InvalidateShadow(uint32_t lightId)
        {
            auto it = m_cacheEntries.find(lightId);
            if (it != m_cacheEntries.end())
                it->second.needsUpdate = true;
        }

        /** @brief Invalidate all cached shadows (e.g., after scene reload) */
        void InvalidateAll()
        {
            for (auto& [id, entry] : m_cacheEntries)
                entry.needsUpdate = true;
        }

        void EndFrame()
        {
            m_dynamicAtlas.EndFrame();
            m_cachedAtlas.EndFrame();
        }

        // --- Accessors ---

        const ShadowAtlas& GetDynamicAtlas() const { return m_dynamicAtlas; }
        const ShadowAtlas& GetCachedAtlas() const { return m_cachedAtlas; }

        /** @brief Get the tile for a light from whichever atlas holds it */
        const ShadowTile* GetTile(uint32_t lightId) const
        {
            const ShadowTile* tile = m_cachedAtlas.GetTile(lightId);
            if (tile)
                return tile;
            return m_dynamicAtlas.GetTile(lightId);
        }

        /** @brief How many shadow renders were avoided this frame via caching */
        uint32_t GetCachedRendersAvoided() const { return m_cachedRendersAvoided; }

        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            auto dm = m_dynamicAtlas.GetMetrics();
            auto cm = m_cachedAtlas.GetMetrics();
            return "CachedShadowAtlas:\n"
                   "  Dynamic: " +
                   std::to_string(dm.activeTiles) + " tiles, " +
                   std::to_string(static_cast<int>(dm.utilization * 100)) +
                   "% util\n"
                   "  Cached:  " +
                   std::to_string(cm.activeTiles) + " tiles, " +
                   std::to_string(static_cast<int>(cm.utilization * 100)) +
                   "% util\n"
                   "  Renders this frame: " +
                   std::to_string(m_shadowsToRender.size()) +
                   "\n"
                   "  Cached hits: " +
                   std::to_string(m_cachedRendersAvoided) + "\n";
        }

      private:
        /**
         * @brief Compute a hash of the light's state for change detection
         */
        static uint64_t ComputeLightStateHash(const ShadowUpdateRequest& req)
        {
            // FNV-1a hash of light transform and parameters
            uint64_t hash = 14695981039346656037ULL;
            auto hashFloat = [&](float f)
            {
                uint32_t bits;
                memcpy(&bits, &f, sizeof(bits));
                hash ^= bits;
                hash *= 1099511628211ULL;
            };

            hashFloat(req.posX);
            hashFloat(req.posY);
            hashFloat(req.posZ);
            hashFloat(req.dirX);
            hashFloat(req.dirY);
            hashFloat(req.dirZ);
            hashFloat(req.range);
            hashFloat(req.spotAngle);

            return hash;
        }

        /**
         * @brief Determine tile size based on priority
         */
        uint32_t GetDesiredTileSize(float priority) const
        {
            if (priority > 0.8f)
                return 1024;
            if (priority > 0.4f)
                return 512;
            return 256;
        }

        ShadowAtlas m_dynamicAtlas;
        ShadowAtlas m_cachedAtlas;
        std::unordered_map<uint32_t, ShadowCacheEntry> m_cacheEntries;
        std::vector<uint32_t> m_shadowsToRender;
        uint32_t m_frameIndex = 0;
        uint32_t m_cachedRendersAvoided = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
