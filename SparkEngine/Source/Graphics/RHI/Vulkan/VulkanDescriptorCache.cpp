/**
 * @file VulkanDescriptorCache.cpp
 * @brief Descriptor set layout caching and batched writes implementation
 */

#ifdef SPARK_VULKAN_SUPPORT

#include "VulkanDescriptorCache.h"

#include "../../../Utils/LogMacros.h"

namespace Spark::RHI::Vulkan
{

    void VulkanDescriptorCache::Initialize(VkDevice device, uint32_t framesInFlight)
    {
        m_device = device;
        m_framePools.resize(framesInFlight);
        m_pendingWrites.reserve(WRITE_FLUSH_THRESHOLD);

        // Create per-frame pools
        for (auto& pool : m_framePools)
        {
            if (!CreateFramePool(pool, 1024))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "VulkanDescriptorCache: failed to create frame pool");
                return;
            }
        }

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "VulkanDescriptorCache: initialized (%u frame pools)",
                       framesInFlight);
    }

    void VulkanDescriptorCache::Shutdown()
    {
        if (!m_initialized)
            return;

        FlushWrites();

        // Destroy frame pools
        for (auto& pool : m_framePools)
        {
            if (pool.pool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(m_device, pool.pool, nullptr);
                pool.pool = VK_NULL_HANDLE;
            }
        }

        // Destroy cached layouts
        {
            std::unique_lock lock(m_layoutMutex);
            for (uint32_t i = 0; i < m_layoutCount; ++i)
            {
                if (m_layouts[i].layout != VK_NULL_HANDLE)
                {
                    vkDestroyDescriptorSetLayout(m_device, m_layouts[i].layout, nullptr);
                    m_layouts[i].layout = VK_NULL_HANDLE;
                }
            }
            m_layoutCount = 0;
        }

        m_initialized = false;
    }

    VkDescriptorSetLayout VulkanDescriptorCache::GetOrCreateLayout(const VkDescriptorSetLayoutCreateInfo& createInfo)
    {
        uint64_t hash = HashLayoutCreateInfo(createInfo);

        // Fast path: read-only lookup
        {
            std::shared_lock lock(m_layoutMutex);
            for (uint32_t i = 0; i < m_layoutCount; ++i)
            {
                if (m_layouts[i].hash == hash)
                {
                    m_layouts[i].useCount++;
                    return m_layouts[i].layout;
                }
            }
        }

        // Slow path: create new layout
        std::unique_lock lock(m_layoutMutex);

        // Double-check after acquiring write lock
        for (uint32_t i = 0; i < m_layoutCount; ++i)
        {
            if (m_layouts[i].hash == hash)
            {
                m_layouts[i].useCount++;
                return m_layouts[i].layout;
            }
        }

        if (m_layoutCount >= MAX_CACHED_LAYOUTS)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "VulkanDescriptorCache: layout cache full (%u/%u)",
                           m_layoutCount, MAX_CACHED_LAYOUTS);
            // Evict least-used layout
            uint32_t minIdx = 0;
            uint32_t minUse = m_layouts[0].useCount;
            for (uint32_t i = 1; i < m_layoutCount; ++i)
            {
                if (m_layouts[i].useCount < minUse)
                {
                    minIdx = i;
                    minUse = m_layouts[i].useCount;
                }
            }
            vkDestroyDescriptorSetLayout(m_device, m_layouts[minIdx].layout, nullptr);
            m_layouts[minIdx] = m_layouts[m_layoutCount - 1];
            m_layoutCount--;
        }

        VkDescriptorSetLayout newLayout = VK_NULL_HANDLE;
        VkResult result = vkCreateDescriptorSetLayout(m_device, &createInfo, nullptr, &newLayout);
        if (result != VK_SUCCESS)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "VulkanDescriptorCache: vkCreateDescriptorSetLayout failed (%d)", result);
            return VK_NULL_HANDLE;
        }

        m_layouts[m_layoutCount] = {newLayout, hash, 1};
        m_layoutCount++;

        return newLayout;
    }

    uint32_t VulkanDescriptorCache::GetCachedLayoutCount() const
    {
        std::shared_lock lock(m_layoutMutex);
        return m_layoutCount;
    }

    void VulkanDescriptorCache::BeginFrame(uint32_t frameIndex)
    {
        m_currentFrameIndex = frameIndex % static_cast<uint32_t>(m_framePools.size());
        auto& pool = m_framePools[m_currentFrameIndex];
        if (pool.pool != VK_NULL_HANDLE)
        {
            vkResetDescriptorPool(m_device, pool.pool, 0);
            pool.allocatedSets = 0;
        }
    }

    VkDescriptorSet VulkanDescriptorCache::AllocateSet(VkDescriptorSetLayout layout)
    {
        auto& pool = m_framePools[m_currentFrameIndex];
        if (pool.allocatedSets >= pool.maxSets)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "VulkanDescriptorCache: frame pool exhausted (%u/%u)",
                           pool.allocatedSets, pool.maxSets);
            return VK_NULL_HANDLE;
        }

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = pool.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult result = vkAllocateDescriptorSets(m_device, &allocInfo, &set);
        if (result == VK_SUCCESS)
        {
            pool.allocatedSets++;
        }
        return set;
    }

    void VulkanDescriptorCache::QueueWrite(const VkWriteDescriptorSet& write)
    {
        m_pendingWrites.push_back(write);
        if (m_pendingWrites.size() >= WRITE_FLUSH_THRESHOLD)
        {
            FlushWrites();
        }
    }

    void VulkanDescriptorCache::FlushWrites()
    {
        if (m_pendingWrites.empty())
            return;

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(m_pendingWrites.size()), m_pendingWrites.data(), 0,
                               nullptr);
        m_pendingWrites.clear();
    }

    std::string VulkanDescriptorCache::Console_GetReport() const
    {
        std::string report = "VulkanDescriptorCache:\n";
        report +=
            "  Cached layouts: " + std::to_string(m_layoutCount) + "/" + std::to_string(MAX_CACHED_LAYOUTS) + "\n";
        for (size_t i = 0; i < m_framePools.size(); ++i)
        {
            report += "  Frame pool " + std::to_string(i) + ": " + std::to_string(m_framePools[i].allocatedSets) + "/" +
                      std::to_string(m_framePools[i].maxSets) + " sets\n";
        }
        report += "  Pending writes: " + std::to_string(m_pendingWrites.size()) + "\n";
        return report;
    }

    uint64_t VulkanDescriptorCache::HashLayoutCreateInfo(const VkDescriptorSetLayoutCreateInfo& info)
    {
        uint64_t hash = 14695981039346656037ULL;
        constexpr uint64_t prime = 1099511628211ULL;

        hash ^= info.flags;
        hash *= prime;
        hash ^= info.bindingCount;
        hash *= prime;

        for (uint32_t i = 0; i < info.bindingCount; ++i)
        {
            const auto& b = info.pBindings[i];
            hash ^= b.binding;
            hash *= prime;
            hash ^= static_cast<uint64_t>(b.descriptorType);
            hash *= prime;
            hash ^= b.descriptorCount;
            hash *= prime;
            hash ^= b.stageFlags;
            hash *= prime;
        }

        return hash;
    }

    bool VulkanDescriptorCache::CreateFramePool(FrameDescriptorPool& pool, uint32_t maxSets)
    {
        // Pool sizes for common descriptor types
        std::array<VkDescriptorPoolSize, 4> poolSizes = {{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets * 4},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxSets * 8},
            {VK_DESCRIPTOR_TYPE_SAMPLER, maxSets * 4},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxSets * 2},
        }};

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = maxSets;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        VkResult result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &pool.pool);
        pool.maxSets = maxSets;
        pool.allocatedSets = 0;
        return result == VK_SUCCESS;
    }

} // namespace Spark::RHI::Vulkan

#endif // SPARK_VULKAN_SUPPORT
