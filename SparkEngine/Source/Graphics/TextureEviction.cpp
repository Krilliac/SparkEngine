#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file TextureEviction.cpp
 * @brief Cache eviction and VRAM management: LRU scoring, eviction policy,
 *        VRAM budget tracking, per-priority caps, cache cleanup, and the
 *        per-frame Update loop that drives eviction.
 */

#include "TextureSystem.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"
#include <algorithm>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// TEXTURE EVICTION & VRAM MANAGEMENT
// ============================================================================

void TextureSystem::Update(float deltaTime)
{
    UpdateMetrics();

    m_currentFrame++;

    // Priority-based eviction: if over budget, evict lowest-priority LRU textures first
    size_t currentUsage = GetMemoryUsage();
    if (currentUsage > m_memoryBudget)
    {
        size_t overage = currentUsage - m_memoryBudget;
        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                       "Texture memory over budget: %zu bytes used, %zu budget, evicting %zu bytes", currentUsage,
                       m_memoryBudget, overage);
        uint32_t evicted = EvictByPriority(overage);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Evicted %u textures by priority", evicted);

        // If priority eviction was not sufficient, fall back to simple GC
        if (GetMemoryUsage() > m_memoryBudget)
        {
            GarbageCollect();
        }
    }
}

void TextureSystem::TouchTexture(const std::string& name, uint64_t currentFrame, float screenCoverage,
                                 float distanceToCamera)
{
    ASSERT_MSG(!name.empty(), "TextureSystem::TouchTexture — name must not be empty");
    ASSERT_MSG(screenCoverage >= 0.0f && screenCoverage <= 1.0f,
               "TextureSystem::TouchTexture — screenCoverage must be in [0, 1]");
    ASSERT_MSG(distanceToCamera >= 0.0f, "TextureSystem::TouchTexture — distanceToCamera must be non-negative");
    std::lock_guard<std::mutex> lock(m_texturesMutex);

    auto& lru = m_lruData[name];
    lru.lastUsedFrame = currentFrame;
    lru.lastUsedTime = std::chrono::steady_clock::now();
    lru.screenCoverage = screenCoverage;
    lru.distanceToCamera = distanceToCamera;
}

void TextureSystem::PinTexture(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    m_lruData[name].pinned = true;
    m_lruData[name].priority = 5;
}

void TextureSystem::UnpinTexture(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    auto it = m_lruData.find(name);
    if (it != m_lruData.end())
    {
        it->second.pinned = false;
        it->second.priority = 2; // Reset to Normal
    }
}

void TextureSystem::SetTexturePriority(const std::string& name, uint8_t priority)
{
    ASSERT_MSG(!name.empty(), "TextureSystem::SetTexturePriority — name must not be empty");
    ASSERT_MSG(priority <= 5, "TextureSystem::SetTexturePriority — priority must be in [0, 5]");
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    m_lruData[name].priority = priority;
    if (priority >= 5)
    {
        m_lruData[name].pinned = true;
    }
}

uint32_t TextureSystem::EvictByPriority(size_t targetFreeBytes)
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);

    if (m_textures.empty())
    {
        return 0;
    }

    // Build scored candidate list: {name, eviction_score, memory_size}
    struct EvictionCandidate
    {
        std::string name;
        float score;
        size_t memoryBytes;
    };

    std::vector<EvictionCandidate> candidates;
    candidates.reserve(m_textures.size());

    for (const auto& [name, texture] : m_textures)
    {
        // Skip default/internal textures
        if (name.starts_with("__"))
        {
            continue;
        }

        // Skip textures held by external references (use_count > 1 means someone else needs it)
        // But still consider them if they have low priority and are old
        auto lruIt = m_lruData.find(name);
        TextureLRUData lru;
        if (lruIt != m_lruData.end())
        {
            lru = lruIt->second;
        }

        // Skip pinned textures
        if (lru.pinned)
        {
            continue;
        }

        float score = lru.GetEvictionScore(m_currentFrame);
        candidates.push_back({name, score, texture->GetMemoryUsage()});
    }

    // Sort by eviction score ascending — lowest score = evict first
    std::sort(candidates.begin(), candidates.end(),
              [](const EvictionCandidate& a, const EvictionCandidate& b) { return a.score < b.score; });

    size_t freedBytes = 0;
    uint32_t evictedCount = 0;

    for (const auto& candidate : candidates)
    {
        if (targetFreeBytes > 0 && freedBytes >= targetFreeBytes)
        {
            break;
        }

        auto texIt = m_textures.find(candidate.name);
        if (texIt == m_textures.end())
        {
            continue;
        }

        // Only evict textures not held externally (use_count == 1 means only the cache holds it)
        if (texIt->second.use_count() <= 1)
        {
            freedBytes += candidate.memoryBytes;
            m_lruData.erase(candidate.name);
            m_textures.erase(texIt);
            evictedCount++;
        }
    }

    return evictedCount;
}

bool TextureSystem::GetTextureLRUData(const std::string& name, TextureLRUData& outData) const
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);

    auto it = m_lruData.find(name);
    if (it != m_lruData.end())
    {
        outData = it->second;
        return true;
    }
    return false;
}

void TextureSystem::GarbageCollect()
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);

    // Remove textures that are only referenced by the texture system
    size_t collected = 0;
    auto it = m_textures.begin();
    while (it != m_textures.end())
    {
        if (it->second.use_count() == 1)
        {
            it = m_textures.erase(it);
            ++collected;
        }
        else
        {
            ++it;
        }
    }

    if (collected > 0)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Garbage collected %zu texture(s)", collected);
        Spark::SimpleConsole::GetInstance().LogInfo("Garbage collected " + std::to_string(collected) + " texture(s)");
    }
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "TextureSystem.h"
#include "../Utils/Validate.h"
#include "../Utils/LogMacros.h"
#include <algorithm>

// ============================================================================
// TextureSystem — Eviction & VRAM Management (Linux stub)
// ============================================================================

void TextureSystem::Update(float /*deltaTime*/)
{
    UpdateMetrics();

    m_currentFrame++;

    // Priority-based eviction: if over budget, evict lowest-priority LRU textures first
    size_t currentUsage = GetMemoryUsage();
    if (currentUsage > m_memoryBudget)
    {
        size_t overage = currentUsage - m_memoryBudget;
        uint32_t evicted = EvictByPriority(overage);

        // If priority eviction was not sufficient, fall back to simple GC
        if (GetMemoryUsage() > m_memoryBudget)
        {
            GarbageCollect();
        }
    }
}

void TextureSystem::TouchTexture(const std::string& name, uint64_t currentFrame, float screenCoverage,
                                 float distanceToCamera)
{
    ASSERT_MSG(!name.empty(), "TextureSystem::TouchTexture — name must not be empty");
    ASSERT_MSG(screenCoverage >= 0.0f && screenCoverage <= 1.0f,
               "TextureSystem::TouchTexture — screenCoverage must be in [0, 1]");
    ASSERT_MSG(distanceToCamera >= 0.0f, "TextureSystem::TouchTexture — distanceToCamera must be non-negative");
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    auto& lru = m_lruData[name];
    lru.lastUsedFrame = currentFrame;
    lru.lastUsedTime = std::chrono::steady_clock::now();
    lru.screenCoverage = screenCoverage;
    lru.distanceToCamera = distanceToCamera;
}

void TextureSystem::PinTexture(const std::string& name)
{
    ASSERT_MSG(!name.empty(), "TextureSystem::PinTexture — name must not be empty");
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    m_lruData[name].pinned = true;
    m_lruData[name].priority = 5;
}

void TextureSystem::UnpinTexture(const std::string& name)
{
    ASSERT_MSG(!name.empty(), "TextureSystem::UnpinTexture — name must not be empty");
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    auto it = m_lruData.find(name);
    if (it != m_lruData.end())
    {
        it->second.pinned = false;
        it->second.priority = 2;
    }
}

void TextureSystem::SetTexturePriority(const std::string& name, uint8_t priority)
{
    ASSERT_MSG(!name.empty(), "TextureSystem::SetTexturePriority — name must not be empty");
    ASSERT_MSG(priority <= 5, "TextureSystem::SetTexturePriority — priority must be in [0, 5]");
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    m_lruData[name].priority = priority;
    if (priority >= 5)
    {
        m_lruData[name].pinned = true;
    }
}

uint32_t TextureSystem::EvictByPriority(size_t targetFreeBytes)
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);

    if (m_textures.empty())
    {
        return 0;
    }

    struct EvictionCandidate
    {
        std::string name;
        float score;
        size_t memoryBytes;
    };

    std::vector<EvictionCandidate> candidates;
    candidates.reserve(m_textures.size());

    for (const auto& [name, texture] : m_textures)
    {
        if (name.starts_with("__"))
        {
            continue;
        }

        auto lruIt = m_lruData.find(name);
        TextureLRUData lru;
        if (lruIt != m_lruData.end())
        {
            lru = lruIt->second;
        }

        if (lru.pinned)
        {
            continue;
        }

        float score = lru.GetEvictionScore(m_currentFrame);
        candidates.push_back({name, score, texture->GetMemoryUsage()});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const EvictionCandidate& a, const EvictionCandidate& b) { return a.score < b.score; });

    size_t freedBytes = 0;
    uint32_t evictedCount = 0;

    for (const auto& candidate : candidates)
    {
        if (targetFreeBytes > 0 && freedBytes >= targetFreeBytes)
        {
            break;
        }

        auto texIt = m_textures.find(candidate.name);
        if (texIt == m_textures.end())
        {
            continue;
        }

        if (texIt->second.use_count() <= 1)
        {
            freedBytes += candidate.memoryBytes;
            m_lruData.erase(candidate.name);
            m_textures.erase(texIt);
            evictedCount++;
        }
    }

    return evictedCount;
}

bool TextureSystem::GetTextureLRUData(const std::string& name, TextureLRUData& outData) const
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    auto it = m_lruData.find(name);
    if (it != m_lruData.end())
    {
        outData = it->second;
        return true;
    }
    return false;
}

void TextureSystem::GarbageCollect()
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    auto it = m_textures.begin();
    while (it != m_textures.end())
    {
        if (it->second.use_count() == 1)
        {
            it = m_textures.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

#endif // SPARK_PLATFORM_WINDOWS
