/**
 * @file VulkanDescriptorCache.h
 * @brief Descriptor set layout caching and batched descriptor writes
 *
 * Two-tier caching inspired by RPCS3's descriptor management:
 * 1. Layout cache: deduplicates VkDescriptorSetLayout creation (up to 64)
 * 2. Batched writes: accumulates VkWriteDescriptorSet, flushes in bulk
 *
 * Also manages per-frame descriptor pools that reset each frame instead
 * of individual free operations (much faster for transient descriptors).
 *
 * @see VulkanDevice.h
 */

#pragma once

#ifdef SPARK_VULKAN_SUPPORT

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace Spark::RHI::Vulkan
{

    /**
     * @brief Cached descriptor set layout with usage tracking.
     */
    struct CachedLayout
    {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        uint64_t hash = 0;
        uint32_t useCount = 0;
    };

    /**
     * @brief Per-frame descriptor pool that resets entirely each frame.
     */
    struct FrameDescriptorPool
    {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        uint32_t allocatedSets = 0;
        uint32_t maxSets = 0;
    };

    /**
     * @brief Descriptor layout cache + batched write manager.
     *
     * Thread-safe: layout lookups use shared_mutex (concurrent reads),
     * descriptor writes are per-command-list (single-threaded).
     */
    class VulkanDescriptorCache
    {
      public:
        VulkanDescriptorCache() = default;
        ~VulkanDescriptorCache() = default;

        /// @brief Initialize with Vulkan device.
        void Initialize(VkDevice device, uint32_t framesInFlight = 2);

        /// @brief Destroy all cached layouts and pools.
        void Shutdown();

        // ====================================================================
        // Layout cache (thread-safe, shared_mutex for concurrent reads)
        // ====================================================================

        /// @brief Get or create a descriptor set layout.
        /// @return Cached layout handle (never VK_NULL_HANDLE on success).
        VkDescriptorSetLayout GetOrCreateLayout(const VkDescriptorSetLayoutCreateInfo& createInfo);

        /// @brief Number of cached layouts.
        uint32_t GetCachedLayoutCount() const;

        // ====================================================================
        // Per-frame descriptor pool management
        // ====================================================================

        /// @brief Reset the current frame's descriptor pool. Call at frame start.
        void BeginFrame(uint32_t frameIndex);

        /// @brief Allocate a descriptor set from the current frame's pool.
        VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);

        // ====================================================================
        // Batched descriptor writes
        // ====================================================================

        /// @brief Queue a descriptor write (deferred until FlushWrites).
        void QueueWrite(const VkWriteDescriptorSet& write);

        /// @brief Flush all queued writes in a single vkUpdateDescriptorSets call.
        void FlushWrites();

        /// @brief Number of pending writes.
        uint32_t GetPendingWriteCount() const { return static_cast<uint32_t>(m_pendingWrites.size()); }

        /// @brief Get diagnostic report.
        std::string Console_GetReport() const;

      private:
        static uint64_t HashLayoutCreateInfo(const VkDescriptorSetLayoutCreateInfo& info);
        bool CreateFramePool(FrameDescriptorPool& pool, uint32_t maxSets);

        VkDevice m_device = VK_NULL_HANDLE;

        // Layout cache (max 64 unique layouts)
        static constexpr uint32_t MAX_CACHED_LAYOUTS = 64;
        mutable std::shared_mutex m_layoutMutex;
        std::array<CachedLayout, MAX_CACHED_LAYOUTS> m_layouts = {};
        uint32_t m_layoutCount = 0;

        // Per-frame pools
        std::vector<FrameDescriptorPool> m_framePools;
        uint32_t m_currentFrameIndex = 0;

        // Batched writes (flushed periodically or before draw)
        static constexpr uint32_t WRITE_FLUSH_THRESHOLD = 4096;
        std::vector<VkWriteDescriptorSet> m_pendingWrites;

        bool m_initialized = false;
    };

} // namespace Spark::RHI::Vulkan

#endif // SPARK_VULKAN_SUPPORT
