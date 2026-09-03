#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file TextureStreaming.cpp
 * @brief Async texture streaming: streaming queue, priority management, background
 *        loading thread, stream-in/stream-out operations, and metrics updates.
 */

#include "TextureSystem.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include <algorithm>
#include <filesystem>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// TEXTURE STREAMING
// ============================================================================

void TextureSystem::LoadTextureAsync(const std::string& filePath,
                                     std::function<void(std::shared_ptr<Texture>)> callback, const TextureDesc& desc)
{
    // Check if already loaded
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        auto it = m_textures.find(filePath);
        if (it != m_textures.end())
        {
            if (callback)
            {
                callback(it->second);
            }
            return;
        }
    }

    // Queue for streaming
    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "TextureStreaming: queuing async load '%s'", filePath.c_str());
    StreamingRequest request;
    request.filePath = filePath;
    request.desc = AdjustDescForQuality(desc);
    request.callback = callback;
    request.priority = 0;
    request.urgent = false;

    {
        std::lock_guard<std::mutex> lock(m_streamingMutex);
        m_streamingQueue.push(request);
    }

    m_streamingCondition.notify_one();
}

void TextureSystem::SetStreamingThreadCount(int count)
{
    // Stop existing threads. Publish the stop flag under the waiter's mutex: a worker that has already
    // evaluated its wait predicate but not yet blocked would otherwise miss this
    // notification and the join below would hang.
    {
        std::lock_guard<std::mutex> lock(m_streamingMutex);
        m_shouldStop = true;
    }
    m_streamingCondition.notify_all();

    for (auto& thread : m_streamingThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    m_streamingThreads.clear();
    m_shouldStop = false;

    // Start new threads
    for (int i = 0; i < count; ++i)
    {
        m_streamingThreads.emplace_back(&TextureSystem::StreamingThreadFunction, this);
    }
}

void TextureSystem::StreamingThreadFunction()
{
    while (!m_shouldStop)
    {
        StreamingRequest request;
        bool hasRequest = false;

        // Get next request
        {
            std::unique_lock<std::mutex> lock(m_streamingMutex);
            m_streamingCondition.wait(lock, [this] { return !m_streamingQueue.empty() || m_shouldStop; });

            if (m_shouldStop)
                break;

            if (!m_streamingQueue.empty())
            {
                request = m_streamingQueue.front();
                m_streamingQueue.pop();
                hasRequest = true;
            }
        }

        if (hasRequest)
        {
            // Load texture
            auto texture = LoadTextureFromFile(request.filePath, request.desc);

            if (texture)
            {
                // Add to cache
                {
                    std::lock_guard<std::mutex> lock(m_texturesMutex);
                    m_textures[request.filePath] = texture;
                }

                // Call callback
                if (request.callback)
                {
                    request.callback(texture);
                }

                SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "TextureStreaming: loaded '%s'",
                                request.filePath.c_str());
                std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
                m_metrics.loadedTextures++;
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics, "TextureStreaming: failed to load '%s'",
                               request.filePath.c_str());
                // Call callback with null on failure
                if (request.callback)
                {
                    request.callback(nullptr);
                }
            }
        }
    }
}

void TextureSystem::UpdateMetrics()
{
    // Compute memory usage outside the metrics lock to avoid ABBA deadlock:
    // GetMemoryUsage() acquires m_texturesMutex, so we must not hold
    // m_metricsMutex while calling it (other paths lock textures then metrics).
    size_t memUsage = GetMemoryUsage();

    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.totalMemoryUsage = memUsage;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "TextureSystem.h"
#include "../Utils/Validate.h"

// ============================================================================
// TextureSystem — Streaming (Linux stub)
// ============================================================================

void TextureSystem::LoadTextureAsync(const std::string& filePath,
                                     std::function<void(std::shared_ptr<Texture>)> callback, const TextureDesc& desc)
{
    // On Linux, just load synchronously and invoke callback
    auto texture = LoadTexture(filePath, desc);
    if (callback)
    {
        callback(texture);
    }
}

void TextureSystem::SetStreamingThreadCount(int /*count*/)
{
    // No streaming threads on Linux
}

void TextureSystem::StreamingThreadFunction()
{
    // No-op on Linux
}

void TextureSystem::UpdateMetrics()
{
    size_t memUsage = GetMemoryUsage();

    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.totalMemoryUsage = memUsage;
}

#endif // SPARK_PLATFORM_WINDOWS
