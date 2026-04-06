/**
 * @file vk_mem_alloc.h
 * @brief Minimal VMA (Vulkan Memory Allocator) stub for SparkEngine build compatibility.
 *
 * This provides the subset of AMD's Vulkan Memory Allocator API used by
 * SparkEngine. Replace with the full VMA library for production use.
 *
 * The real library is available at: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
 * License: MIT
 *
 * ## Production replacement
 * Download vk_mem_alloc.h from the VMA repo and place it here. No other
 * changes needed — the engine uses the standard VMA API.
 */

#pragma once

// Only compile when Vulkan is available
#ifdef SPARK_VULKAN_SUPPORT

#include <vulkan/vulkan.h>

#include <cstdint>

// ============================================================================
// VMA Type Definitions
// ============================================================================

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

typedef enum VmaMemoryUsage
{
    VMA_MEMORY_USAGE_UNKNOWN = 0,
    VMA_MEMORY_USAGE_GPU_ONLY = 1,
    VMA_MEMORY_USAGE_CPU_ONLY = 2,
    VMA_MEMORY_USAGE_CPU_TO_GPU = 3,
    VMA_MEMORY_USAGE_GPU_TO_CPU = 4,
    VMA_MEMORY_USAGE_CPU_COPY = 5,
    VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED = 6,
    VMA_MEMORY_USAGE_AUTO = 7,
    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE = 8,
    VMA_MEMORY_USAGE_AUTO_PREFER_HOST = 9,
} VmaMemoryUsage;

typedef enum VmaAllocationCreateFlagBits
{
    VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT = 0x00000001,
    VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT = 0x00000002,
    VMA_ALLOCATION_CREATE_MAPPED_BIT = 0x00000004,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT = 0x00000040,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT = 0x00000080,
} VmaAllocationCreateFlagBits;
typedef uint32_t VmaAllocationCreateFlags;

typedef struct VmaAllocatorCreateInfo
{
    VmaAllocationCreateFlags flags;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkDeviceSize preferredLargeHeapBlockSize;
    const VkAllocationCallbacks* pAllocationCallbacks;
    void* pDeviceMemoryCallbacks;
    const VkDeviceSize* pHeapSizeLimit;
    void* pVulkanFunctions;
    VkInstance instance;
    uint32_t vulkanApiVersion;
    const void* pTypeExternalMemoryHandleTypes;
} VmaAllocatorCreateInfo;

typedef struct VmaAllocationCreateInfo
{
    VmaAllocationCreateFlags flags;
    VmaMemoryUsage usage;
    VkMemoryPropertyFlags requiredFlags;
    VkMemoryPropertyFlags preferredFlags;
    uint32_t memoryTypeBits;
    VkDeviceSize pool;
    void* pUserData;
    float priority;
} VmaAllocationCreateInfo;

typedef struct VmaAllocationInfo
{
    uint32_t memoryType;
    VkDeviceMemory deviceMemory;
    VkDeviceSize offset;
    VkDeviceSize size;
    void* pMappedData;
    void* pUserData;
    const char* pName;
} VmaAllocationInfo;

typedef struct VmaTotalStatistics
{
    struct
    {
        uint32_t blockCount;
        uint32_t allocationCount;
        VkDeviceSize usedBytes;
        VkDeviceSize unusedBytes;
    } total;
} VmaTotalStatistics;

// ============================================================================
// VMA API Stubs
// ============================================================================

// Stub allocator state
struct VmaAllocator_T
{
    VkDevice device;
    VkPhysicalDevice physicalDevice;
};

struct VmaAllocation_T
{
    VkDeviceMemory memory;
    VkDeviceSize offset;
    VkDeviceSize size;
    void* mappedData;
};

inline VkResult vmaCreateAllocator(const VmaAllocatorCreateInfo* pCreateInfo, VmaAllocator* pAllocator)
{
    auto* alloc = new VmaAllocator_T();
    alloc->device = pCreateInfo->device;
    alloc->physicalDevice = pCreateInfo->physicalDevice;
    *pAllocator = alloc;
    return VK_SUCCESS;
}

inline void vmaDestroyAllocator(VmaAllocator allocator)
{
    delete allocator;
}

inline VkResult vmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* pBufferCreateInfo,
                                const VmaAllocationCreateInfo* pAllocationCreateInfo, VkBuffer* pBuffer,
                                VmaAllocation* pAllocation, VmaAllocationInfo* pAllocationInfo)
{
    VkResult result = vkCreateBuffer(allocator->device, pBufferCreateInfo, nullptr, pBuffer);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(allocator->device, *pBuffer, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;

    // Find suitable memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(allocator->physicalDevice, &memProps);
    VkMemoryPropertyFlags desiredProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (pAllocationCreateInfo->usage == VMA_MEMORY_USAGE_CPU_TO_GPU ||
        pAllocationCreateInfo->usage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST)
        desiredProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((memReq.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & desiredProps) == desiredProps)
        {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }

    auto* allocationObj = new VmaAllocation_T();
    result = vkAllocateMemory(allocator->device, &allocInfo, nullptr, &allocationObj->memory);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(allocator->device, *pBuffer, nullptr);
        delete allocationObj;
        return result;
    }

    allocationObj->offset = 0;
    allocationObj->size = memReq.size;
    allocationObj->mappedData = nullptr;
    vkBindBufferMemory(allocator->device, *pBuffer, allocationObj->memory, 0);

    *pAllocation = allocationObj;
    if (pAllocationInfo)
    {
        pAllocationInfo->deviceMemory = allocationObj->memory;
        pAllocationInfo->offset = 0;
        pAllocationInfo->size = memReq.size;
        pAllocationInfo->pMappedData = nullptr;
    }

    return VK_SUCCESS;
}

inline void vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
{
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(allocator->device, buffer, nullptr);
    if (allocation)
    {
        vkFreeMemory(allocator->device, allocation->memory, nullptr);
        delete allocation;
    }
}

inline VkResult vmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo* pImageCreateInfo,
                               const VmaAllocationCreateInfo* pAllocationCreateInfo, VkImage* pImage,
                               VmaAllocation* pAllocation, VmaAllocationInfo* pAllocationInfo)
{
    VkResult result = vkCreateImage(allocator->device, pImageCreateInfo, nullptr, pImage);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(allocator->device, *pImage, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(allocator->physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }

    auto* allocationObj = new VmaAllocation_T();
    result = vkAllocateMemory(allocator->device, &allocInfo, nullptr, &allocationObj->memory);
    if (result != VK_SUCCESS)
    {
        vkDestroyImage(allocator->device, *pImage, nullptr);
        delete allocationObj;
        return result;
    }

    allocationObj->offset = 0;
    allocationObj->size = memReq.size;
    allocationObj->mappedData = nullptr;
    vkBindImageMemory(allocator->device, *pImage, allocationObj->memory, 0);

    *pAllocation = allocationObj;
    if (pAllocationInfo)
    {
        pAllocationInfo->deviceMemory = allocationObj->memory;
        pAllocationInfo->offset = 0;
        pAllocationInfo->size = memReq.size;
        pAllocationInfo->pMappedData = nullptr;
    }

    return VK_SUCCESS;
}

inline void vmaDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation)
{
    if (image != VK_NULL_HANDLE)
        vkDestroyImage(allocator->device, image, nullptr);
    if (allocation)
    {
        vkFreeMemory(allocator->device, allocation->memory, nullptr);
        delete allocation;
    }
}

inline void vmaCalculateStatistics(VmaAllocator /*allocator*/, VmaTotalStatistics* pStats)
{
    pStats->total.blockCount = 0;
    pStats->total.allocationCount = 0;
    pStats->total.usedBytes = 0;
    pStats->total.unusedBytes = 0;
}

inline VkResult vmaMapMemory(VmaAllocator allocator, VmaAllocation allocation, void** ppData)
{
    return vkMapMemory(allocator->device, allocation->memory, allocation->offset, allocation->size, 0, ppData);
}

inline void vmaUnmapMemory(VmaAllocator allocator, VmaAllocation allocation)
{
    vkUnmapMemory(allocator->device, allocation->memory);
}

#endif // SPARK_VULKAN_SUPPORT
