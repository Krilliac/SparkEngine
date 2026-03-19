/**
 * @file TransientResourcePool.cpp
 * @brief Implementation of age-based transient GPU resource pooling
 * @author Spark Engine Team
 * @date 2026
 */

#include "TransientResourcePool.h"

#include <algorithm>
#include <format>

namespace Spark::Graphics
{

    bool TransientResourcePool::Initialize(uint32_t maxIdleFrames)
    {
        if (m_initialized)
        {
            Shutdown();
        }

        m_maxIdleFrames = maxIdleFrames;
        m_currentFrame = 0;
        m_nextHandle = 1;
        m_initialized = true;

        return true;
    }

    uint64_t TransientResourcePool::AcquireResource(const TransientResourceDesc& desc)
    {
        if (!m_initialized)
        {
            return 0;
        }

        // Try to find an available resource with a matching descriptor
        for (auto& resource : m_resources)
        {
            if (!resource.inUse && resource.desc == desc)
            {
                resource.inUse = true;
                resource.lastUsedFrame = m_currentFrame;
                return resource.handle;
            }
        }

        // No match found — create a new pooled resource
        PooledResource newResource;
        newResource.desc = desc;
        newResource.handle = m_nextHandle++;
        newResource.lastUsedFrame = m_currentFrame;
        newResource.inUse = true;

        m_resources.push_back(std::move(newResource));

        return m_resources.back().handle;
    }

    void TransientResourcePool::ReleaseResource(uint64_t handle)
    {
        if (!m_initialized)
        {
            return;
        }

        for (auto& resource : m_resources)
        {
            if (resource.handle == handle)
            {
                resource.inUse = false;
                resource.lastUsedFrame = m_currentFrame;
                return;
            }
        }
    }

    void TransientResourcePool::BeginFrame(uint32_t frameIndex)
    {
        m_currentFrame = frameIndex;
    }

    void TransientResourcePool::GarbageCollect()
    {
        if (!m_initialized)
        {
            return;
        }

        // Remove resources that have been idle for more than maxIdleFrames
        std::erase_if(m_resources,
                      [this](const PooledResource& resource)
                      {
                          if (resource.inUse)
                          {
                              return false;
                          }
                          return (m_currentFrame - resource.lastUsedFrame) > m_maxIdleFrames;
                      });
    }

    uint32_t TransientResourcePool::GetPooledResourceCount() const
    {
        return static_cast<uint32_t>(m_resources.size());
    }

    uint32_t TransientResourcePool::GetActiveResourceCount() const
    {
        uint32_t count = 0;
        for (const auto& resource : m_resources)
        {
            if (resource.inUse)
            {
                ++count;
            }
        }
        return count;
    }

    size_t TransientResourcePool::GetEstimatedMemoryUsage() const
    {
        size_t totalBytes = 0;
        for (const auto& resource : m_resources)
        {
            totalBytes += EstimateResourceSize(resource.desc);
        }
        return totalBytes;
    }

    std::string TransientResourcePool::Console_GetStatus() const
    {
        if (!m_initialized)
        {
            return "TransientResourcePool: Not initialized\n";
        }

        size_t memoryMB = GetEstimatedMemoryUsage() / (1024 * 1024);

        std::string status = "TransientResourcePool:\n";
        status += std::format("  Current frame: {}\n", m_currentFrame);
        status += std::format("  Max idle frames: {}\n", m_maxIdleFrames);
        status += std::format("  Total pooled: {}\n", GetPooledResourceCount());
        status += std::format("  Active (in use): {}\n", GetActiveResourceCount());
        status += std::format("  Idle: {}\n", GetPooledResourceCount() - GetActiveResourceCount());
        status += std::format("  Estimated memory: {} MB\n", memoryMB);

        return status;
    }

    void TransientResourcePool::Shutdown()
    {
        m_resources.clear();
        m_nextHandle = 1;
        m_currentFrame = 0;
        m_initialized = false;
    }

    size_t TransientResourcePool::EstimateResourceSize(const TransientResourceDesc& desc)
    {
        // Rough estimate: width * height * bytes-per-pixel
        // Assume 4 bytes per pixel as a default; format-specific sizes could be added later
        uint32_t bytesPerPixel = 4;

        // Common DXGI format sizes (simplified)
        switch (desc.format)
        {
        case 2: // DXGI_FORMAT_R32G32B32A32_FLOAT
            bytesPerPixel = 16;
            break;
        case 10: // DXGI_FORMAT_R16G16B16A16_FLOAT
            bytesPerPixel = 8;
            break;
        case 28: // DXGI_FORMAT_R8G8B8A8_UNORM
            bytesPerPixel = 4;
            break;
        case 24: // DXGI_FORMAT_R10G10B10A2_UNORM
            bytesPerPixel = 4;
            break;
        case 41: // DXGI_FORMAT_R32_FLOAT (depth)
            bytesPerPixel = 4;
            break;
        default:
            bytesPerPixel = 4;
            break;
        }

        return static_cast<size_t>(desc.width) * desc.height * bytesPerPixel;
    }

} // namespace Spark::Graphics
