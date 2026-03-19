/**
 * @file PipelineStateCache.cpp
 * @brief Immutable flyweight PSO cache implementation
 */

#include "PipelineStateCache.h"

namespace Spark::Graphics
{

    bool PipelineStateCache::Initialize()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.clear();
        return true;
    }

    std::shared_ptr<PipelineStateDesc> PipelineStateCache::GetOrCreate(const PipelineStateDesc& desc)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_cache.find(desc);
        if (it != m_cache.end())
        {
            return it->second;
        }

        auto pso = std::make_shared<PipelineStateDesc>(desc);
        m_cache[desc] = pso;
        return pso;
    }

    void PipelineStateCache::Invalidate(uint64_t shaderHash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_cache.begin();
        while (it != m_cache.end())
        {
            if (it->first.shaderHash == shaderHash)
            {
                it = m_cache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    uint32_t PipelineStateCache::GetCachedStateCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<uint32_t>(m_cache.size());
    }

    void PipelineStateCache::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.clear();
    }

    std::string PipelineStateCache::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string status = "PipelineStateCache:\n";
        status += "  Cached states: " + std::to_string(m_cache.size()) + "\n";
        return status;
    }

    void PipelineStateCache::Shutdown()
    {
        Clear();
    }

} // namespace Spark::Graphics
