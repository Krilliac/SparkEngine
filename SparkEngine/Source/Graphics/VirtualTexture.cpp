/**
 * @file VirtualTexture.cpp
 * @brief Implementation of virtual texture page cache management
 */

#include "VirtualTexture.h"
#include "../Utils/LogMacros.h"

#include <algorithm>
#include <format>

namespace Spark::Graphics
{

    // ========================================================================
    // Singleton
    // ========================================================================

    VirtualTextureManager& VirtualTextureManager::GetInstance()
    {
        static VirtualTextureManager instance;
        return instance;
    }

    // ========================================================================
    // Initialize
    // ========================================================================

    bool VirtualTextureManager::Initialize(uint32_t pageCacheSize, uint32_t tileSize)
    {
        if (m_initialized)
        {
            return true;
        }

        if (pageCacheSize == 0 || tileSize == 0)
        {
            return false;
        }

        m_maxPages = pageCacheSize;
        m_tileSize = tileSize;
        m_pageCache.clear();
        m_pageCache.reserve(pageCacheSize);
        m_pendingLoads.clear();
        m_initialized = true;

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "VirtualTextureManager initialized (cache=%u pages, tile=%ux%u)",
                       pageCacheSize, tileSize, tileSize);

        return true;
    }

    // ========================================================================
    // ProcessFeedback
    // ========================================================================

    void VirtualTextureManager::ProcessFeedback(const std::vector<FeedbackEntry>& feedback, uint32_t currentFrame)
    {
        if (!m_initialized)
        {
            return;
        }

        // Deduplicate feedback entries by building a set of unique (textureId, mip) pairs.
        // For each unique entry, check if the page is resident; if not, request it.
        // We use a simple linear scan since feedback buffers are typically small after
        // downsampling on the GPU.
        struct PageKey
        {
            uint32_t textureId;
            uint32_t mipLevel;

            bool operator==(const PageKey& other) const
            {
                return textureId == other.textureId && mipLevel == other.mipLevel;
            }
        };

        std::vector<PageKey> uniqueRequests;
        uniqueRequests.reserve(feedback.size());

        for (const auto& entry : feedback)
        {
            PageKey key{entry.textureId, entry.requiredMip};

            bool alreadySeen = false;
            for (const auto& existing : uniqueRequests)
            {
                if (existing == key)
                {
                    alreadySeen = true;
                    break;
                }
            }

            if (!alreadySeen)
            {
                uniqueRequests.push_back(key);
            }
        }

        // Process each unique request
        for (const auto& key : uniqueRequests)
        {
            // Check if already resident
            auto* resident = FindResidentPage(key.textureId, key.mipLevel, 0, 0);
            if (resident)
            {
                resident->lastUsedFrame = currentFrame;
                continue;
            }

            // Not resident — request it (tile 0,0 as a representative page).
            // A full implementation would compute exact tiles from screen UVs.
            RequestPage(key.textureId, key.mipLevel, 0, 0);
        }

        // Promote pending loads to resident (simulated instant load for now).
        // A production system would async-load from disk and promote on completion.
        for (auto& pending : m_pendingLoads)
        {
            pending.resident = true;
            pending.lastUsedFrame = currentFrame;
            m_pageCache.push_back(std::move(pending));
        }
        m_pendingLoads.clear();

        // Evict if over capacity
        if (m_pageCache.size() > m_maxPages)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Graphics,
                            "Virtual texture cache over capacity (%zu/%u), evicting stale pages", m_pageCache.size(),
                            m_maxPages);
            EvictStalePages(currentFrame, 60);
        }
    }

    // ========================================================================
    // RequestPage
    // ========================================================================

    bool VirtualTextureManager::RequestPage(uint32_t textureId, uint32_t mipLevel, uint32_t tileX, uint32_t tileY)
    {
        if (!m_initialized)
        {
            return false;
        }

        // Already resident?
        if (FindResidentPage(textureId, mipLevel, tileX, tileY))
        {
            return true;
        }

        // Already pending?
        if (FindPendingPage(textureId, mipLevel, tileX, tileY))
        {
            return true;
        }

        // Queue for loading
        VirtualTexturePage page;
        page.textureId = textureId;
        page.mipLevel = mipLevel;
        page.tileX = tileX;
        page.tileY = tileY;
        page.resident = false;
        page.lastUsedFrame = 0;
        m_pendingLoads.push_back(std::move(page));

        return true;
    }

    // ========================================================================
    // EvictStalePages
    // ========================================================================

    uint32_t VirtualTextureManager::EvictStalePages(uint32_t currentFrame, uint32_t framesToKeep)
    {
        if (!m_initialized || m_pageCache.empty())
        {
            return 0;
        }

        uint32_t evictThreshold = (currentFrame > framesToKeep) ? (currentFrame - framesToKeep) : 0;

        // Remove pages not used within the retention window
        auto eraseBegin =
            std::remove_if(m_pageCache.begin(), m_pageCache.end(), [evictThreshold](const VirtualTexturePage& page)
                           { return page.lastUsedFrame < evictThreshold; });

        uint32_t evicted = static_cast<uint32_t>(std::distance(eraseBegin, m_pageCache.end()));
        m_pageCache.erase(eraseBegin, m_pageCache.end());

        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Evicted %u stale virtual texture pages", evicted);

        // If still over capacity, evict least-recently-used pages
        if (m_pageCache.size() > m_maxPages)
        {
            // Sort by lastUsedFrame ascending (oldest first)
            std::sort(m_pageCache.begin(), m_pageCache.end(),
                      [](const VirtualTexturePage& a, const VirtualTexturePage& b)
                      { return a.lastUsedFrame < b.lastUsedFrame; });

            uint32_t excess = static_cast<uint32_t>(m_pageCache.size() - m_maxPages);
            m_pageCache.erase(m_pageCache.begin(), m_pageCache.begin() + excess);
            evicted += excess;
        }

        return evicted;
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    uint32_t VirtualTextureManager::GetResidentPageCount() const
    {
        return static_cast<uint32_t>(m_pageCache.size());
    }

    uint32_t VirtualTextureManager::GetPendingPageCount() const
    {
        return static_cast<uint32_t>(m_pendingLoads.size());
    }

    // ========================================================================
    // Console Status
    // ========================================================================

    std::string VirtualTextureManager::Console_GetStatus() const
    {
        std::string status;
        status += std::format("VirtualTextureManager: {}\n", m_initialized ? "initialized" : "not initialized");

        if (m_initialized)
        {
            status += std::format("  Page cache: {}/{} resident\n", m_pageCache.size(), m_maxPages);
            status += std::format("  Pending loads: {}\n", m_pendingLoads.size());
            status += std::format("  Tile size: {}x{}\n", m_tileSize, m_tileSize);
        }

        return status;
    }

    // ========================================================================
    // Shutdown
    // ========================================================================

    void VirtualTextureManager::Shutdown()
    {
        m_pageCache.clear();
        m_pendingLoads.clear();
        m_tileSize = 128;
        m_maxPages = 1024;
        m_initialized = false;
    }

    // ========================================================================
    // FindResidentPage / FindPendingPage
    // ========================================================================

    VirtualTexturePage* VirtualTextureManager::FindResidentPage(uint32_t textureId, uint32_t mipLevel, uint32_t tileX,
                                                                uint32_t tileY)
    {
        for (auto& page : m_pageCache)
        {
            if (page.textureId == textureId && page.mipLevel == mipLevel && page.tileX == tileX && page.tileY == tileY)
            {
                return &page;
            }
        }
        return nullptr;
    }

    VirtualTexturePage* VirtualTextureManager::FindPendingPage(uint32_t textureId, uint32_t mipLevel, uint32_t tileX,
                                                               uint32_t tileY)
    {
        for (auto& page : m_pendingLoads)
        {
            if (page.textureId == textureId && page.mipLevel == mipLevel && page.tileX == tileX && page.tileY == tileY)
            {
                return &page;
            }
        }
        return nullptr;
    }

} // namespace Spark::Graphics
