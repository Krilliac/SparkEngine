/**
 * @file VirtualTexture.h
 * @brief Virtual texturing with feedback-driven page streaming
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a virtual texturing system that keeps only the needed texture
 * pages resident in a GPU page cache. A feedback buffer (written by shaders)
 * reports which texture pages are visible each frame. The manager loads
 * requested pages and evicts stale ones using LRU.
 *
 * This is a CPU-side page manager. GPU indirection table updates and
 * feedback buffer readback are handled by the RHI backend.
 */

#pragma once
#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // ========================================================================
    // Virtual Texture Page
    // ========================================================================

    /**
     * @brief A single tile in the virtual texture page cache.
     *
     * Each page represents one tile of a specific mip level of a texture.
     * Pages are loaded on demand and evicted when not recently used.
     */
    struct VirtualTexturePage
    {
        uint32_t textureId = 0;     ///< Source texture identifier
        uint32_t mipLevel = 0;      ///< Mip level this page belongs to
        uint32_t tileX = 0;         ///< Tile column within the mip level
        uint32_t tileY = 0;         ///< Tile row within the mip level
        bool resident = false;      ///< True if the page is loaded in the cache
        uint32_t lastUsedFrame = 0; ///< Frame number when this page was last needed
    };

    // ========================================================================
    // Feedback Entry
    // ========================================================================

    /**
     * @brief A single entry from the GPU feedback buffer.
     *
     * Each visible pixel writes its texture ID and required mip level
     * into the feedback buffer. The CPU reads this back and determines
     * which pages need to be loaded.
     */
    struct FeedbackEntry
    {
        uint32_t textureId = 0;   ///< Texture that was sampled
        uint32_t requiredMip = 0; ///< Mip level needed for this pixel
    };

    // ========================================================================
    // VirtualTextureManager
    // ========================================================================

    /**
     * @brief Singleton page cache manager for virtual texturing.
     *
     * Each frame:
     * 1. ProcessFeedback() analyzes the feedback buffer to find needed pages
     * 2. Requested pages are queued for async loading
     * 3. Stale pages are evicted via LRU when the cache is full
     *
     * @threadsafety Main thread only.
     */
    class VirtualTextureManager
    {
      public:
        /// @brief Get the singleton instance
        [[deprecated("Use EngineContext::Get()->GetSystem<VirtualTextureManager>() instead")]]
        static VirtualTextureManager& GetInstance();

        /**
         * @brief Initialize the page cache.
         * @param pageCacheSize Maximum number of resident pages (default 1024)
         * @param tileSize      Pixel size of each tile (default 128)
         * @return true on success
         */
        bool Initialize(uint32_t pageCacheSize = 1024, uint32_t tileSize = 128);

        /**
         * @brief Analyze feedback buffer and schedule page loads/evictions.
         *
         * Marks needed pages as used (updating lastUsedFrame), queues new
         * pages for loading, and evicts stale pages if the cache is full.
         *
         * @param feedback     Feedback entries from the GPU readback
         * @param currentFrame Current frame number for LRU tracking
         */
        void ProcessFeedback(const std::vector<FeedbackEntry>& feedback, uint32_t currentFrame);

        /**
         * @brief Request a specific page to be loaded into the cache.
         * @param textureId Texture identifier
         * @param mipLevel  Mip level
         * @param tileX     Tile column
         * @param tileY     Tile row
         * @return true if the page is already resident or was queued
         */
        bool RequestPage(uint32_t textureId, uint32_t mipLevel, uint32_t tileX, uint32_t tileY);

        /**
         * @brief Evict pages not used within the given frame window.
         * @param currentFrame  Current frame number
         * @param framesToKeep  Number of frames a page stays resident without use
         * @return Number of pages evicted
         */
        uint32_t EvictStalePages(uint32_t currentFrame, uint32_t framesToKeep = 60);

        /// @brief Get the number of currently resident pages
        uint32_t GetResidentPageCount() const;

        /// @brief Get the number of pages pending load
        uint32_t GetPendingPageCount() const;

        /// @brief Get a human-readable status string for console display
        std::string Console_GetStatus() const;

        /// @brief Release all resources and clear the page cache
        void Shutdown();

      private:
        VirtualTextureManager() = default;

        /// @brief Find a resident page matching the given parameters
        /// @return Pointer to the page, or nullptr if not found
        VirtualTexturePage* FindResidentPage(uint32_t textureId, uint32_t mipLevel, uint32_t tileX, uint32_t tileY);

        /// @brief Find a pending page matching the given parameters
        /// @return Pointer to the page, or nullptr if not found
        VirtualTexturePage* FindPendingPage(uint32_t textureId, uint32_t mipLevel, uint32_t tileX, uint32_t tileY);

        std::vector<VirtualTexturePage> m_pageCache;    ///< Resident pages
        std::vector<VirtualTexturePage> m_pendingLoads; ///< Pages queued for loading
        uint32_t m_tileSize = 128;                      ///< Pixels per tile edge
        uint32_t m_maxPages = 1024;                     ///< Maximum resident page count
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
