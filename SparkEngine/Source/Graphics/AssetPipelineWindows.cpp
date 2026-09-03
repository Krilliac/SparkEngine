/**
 * @file AssetPipelineWindows.cpp
 * @brief Windows/D3D11 implementation — split from AssetPipeline.cpp
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @file AssetPipeline.cpp
 * @brief Core asset pipeline — lifecycle, loading orchestration, caching, streaming
 *
 * Asset type implementations (MeshAsset, TextureAsset, AudioAsset) are in AssetTypes.cpp.
 * Model file parsing and rendering helpers are in ModelLoading.cpp.
 * Metadata, type detection, and utility functions are in AssetMetadata.cpp.
 * Console integration methods are in AssetPipelineConsoleOps.cpp.
 */

#include "AssetPipeline.h"
#include "Utils/Assert.h"
#include "../Utils/ContainerUtils.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include <iostream>
#include <filesystem>

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// ============================================================================
// ASSET PIPELINE LIFECYCLE (Windows)
// ============================================================================

AssetPipeline::AssetPipeline() : m_device(nullptr), m_context(nullptr) {}

AssetPipeline::~AssetPipeline()
{
    Shutdown();
}

HRESULT AssetPipeline::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);

    m_device = device;
    m_context = context;

    // Initialize cache
    m_cache = std::make_unique<AssetCache>(512); // 512MB default

    // Initialize metrics
    memset(&m_metrics, 0, sizeof(m_metrics));

    // Start loading threads
    SetStreamingThreadCount(2);

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "AssetPipeline initialized successfully");
    return S_OK;
}

void AssetPipeline::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "AssetPipeline shutting down");
    // Stop loading threads. Publish the stop flag under the waiter's mutex: a worker that has already
    // evaluated its wait predicate but not yet blocked would otherwise miss this
    // notification and the join below would hang.
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_shouldStop = true;
    }
    m_queueCondition.notify_all();

    for (auto& thread : m_loadingThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    // Clear assets
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_assets.clear();
    }

    m_cache.reset();
    m_device = nullptr;
    m_context = nullptr;

    Spark::SimpleConsole::GetInstance().LogInfo("AssetPipeline shutdown complete");
}

void AssetPipeline::Update(float deltaTime)
{
    UpdateMetrics();

    if (m_hotReloadingEnabled)
    {
        CheckForChangedAssets();
    }
}

// ============================================================================
// SYNCHRONOUS LOADING (Windows)
// ============================================================================

std::shared_ptr<Asset> AssetPipeline::LoadAsset(const std::string& path, AssetType type)
{
    // Check cache first
    auto cachedAsset = m_cache->GetAsset(path);
    if (cachedAsset)
    {
        return cachedAsset;
    }

    // Check if already loaded
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        auto it = m_assets.find(path);
        if (it != m_assets.end())
        {
            return it->second;
        }
    }

    // Detect type if unknown
    if (type == AssetType::Unknown)
    {
        type = DetectAssetType(path);
    }

    // Load based on type
    std::shared_ptr<Asset> asset;
    switch (type)
    {
    case AssetType::Mesh:
        asset = LoadMeshFromFile(path);
        break;
    case AssetType::Texture:
        asset = LoadTextureFromFile(path);
        break;
    case AssetType::Audio:
        asset = LoadAudioFromFile(path);
        break;
    default:
        Spark::SimpleConsole::GetInstance().LogError("Unsupported asset type for: " + path);
        return nullptr;
    }

    if (asset)
    {
        // Add to cache and assets
        m_cache->AddAsset(asset);

        {
            std::lock_guard<std::mutex> lock(m_assetsMutex);
            m_assets[path] = asset;
        }

        std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
        m_metrics.loadedAssets++;
    }

    return asset;
}

std::shared_ptr<MeshAsset> AssetPipeline::LoadMesh(const std::string& path)
{
    auto asset = LoadAsset(path, AssetType::Mesh);
    return std::dynamic_pointer_cast<MeshAsset>(asset);
}

// Windows loads mesh GPU buffers through the D3D11 device inside
// `MeshAsset::Load` directly (see AssetTypesWindows.cpp), so the RHI buffer
// upload helper is a no-op here. Keeping the symbol defined on every
// platform means ModelLoading / AssetPipelineLinux / etc. can call it
// without conditional compilation at the call site.
void AssetPipeline::BuildRHIBuffersForMesh(MeshAsset& /*mesh*/) {}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTexture(const std::string& path)
{
    auto asset = LoadAsset(path, AssetType::Texture);
    return std::dynamic_pointer_cast<TextureAsset>(asset);
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudio(const std::string& path)
{
    auto asset = LoadAsset(path, AssetType::Audio);
    return std::dynamic_pointer_cast<AudioAsset>(asset);
}

// ============================================================================
// ASYNCHRONOUS LOADING (Windows)
// ============================================================================

void AssetPipeline::LoadAssetAsync(const AssetLoadRequest& request)
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_loadQueue.push(request);
    }

    m_queueCondition.notify_one();
}

void AssetPipeline::LoadMeshAsync(const std::string& path, std::function<void(std::shared_ptr<MeshAsset>)> callback)
{
    AssetLoadRequest request;
    request.assetPath = path;
    request.expectedType = AssetType::Mesh;
    request.priority = LoadingPriority::Normal;
    request.onLoaded = [callback](std::shared_ptr<void> asset)
    {
        if (callback)
        {
            callback(std::static_pointer_cast<MeshAsset>(asset));
        }
    };

    LoadAssetAsync(request);
}

void AssetPipeline::LoadTextureAsync(const std::string& path,
                                     std::function<void(std::shared_ptr<TextureAsset>)> callback)
{
    AssetLoadRequest request;
    request.assetPath = path;
    request.expectedType = AssetType::Texture;
    request.priority = LoadingPriority::Normal;
    request.onLoaded = [callback](std::shared_ptr<void> asset)
    {
        if (callback)
        {
            callback(std::static_pointer_cast<TextureAsset>(asset));
        }
    };

    LoadAssetAsync(request);
}

// ============================================================================
// ASSET MANAGEMENT (Windows)
// ============================================================================

void AssetPipeline::UnloadAsset(const std::string& path)
{
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_assets.erase(path);
    }

    m_cache->RemoveAsset(path);

    std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
    m_metrics.loadedAssets--;
}

void AssetPipeline::UnloadAllAssets()
{
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_assets.clear();
    }

    m_cache->Clear();

    std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
    m_metrics.loadedAssets = 0;
}

std::shared_ptr<Asset> AssetPipeline::GetAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assets.find(path);
    return (it != m_assets.end()) ? it->second : nullptr;
}

bool AssetPipeline::IsAssetLoaded(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    return Spark::ContainerUtils::Contains(m_assets, path);
}

// ============================================================================
// CACHE MANAGEMENT (Windows)
// ============================================================================

void AssetPipeline::SetCacheSize(size_t maxMemoryMB)
{
    if (m_cache)
    {
        m_cache->SetMaxMemory(maxMemoryMB);
    }
}

void AssetPipeline::EvictUnusedAssets()
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);

    size_t evicted = 0;
    auto it = m_assets.begin();
    while (it != m_assets.end())
    {
        if (it->second.use_count() == 1)
        {
            it = m_assets.erase(it);
            ++evicted;
        }
        else
        {
            ++it;
        }
    }

    if (evicted > 0)
    {
        Spark::SimpleConsole::GetInstance().LogInfo("Evicted " + std::to_string(evicted) + " unused asset(s)");
    }
}

void AssetPipeline::PreloadAssets(const std::vector<std::string>& paths)
{
    for (const auto& path : paths)
    {
        LoadAsset(path);
    }
}

// ============================================================================
// STREAMING CONTROL (Windows)
// ============================================================================

void AssetPipeline::EnableBackgroundStreaming(bool enabled)
{
    m_backgroundStreaming = enabled;
}

void AssetPipeline::SetStreamingThreadCount(int count)
{
    // Stop existing threads. Publish the stop flag under the waiter's mutex: a worker that has already
    // evaluated its wait predicate but not yet blocked would otherwise miss this
    // notification and the join below would hang.
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_shouldStop = true;
    }
    m_queueCondition.notify_all();

    for (auto& thread : m_loadingThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    m_loadingThreads.clear();
    m_shouldStop = false;

    // Start new threads
    for (int i = 0; i < count; ++i)
    {
        m_loadingThreads.emplace_back(&AssetPipeline::LoadingThreadFunction, this);
    }
}

// ============================================================================
// METRICS (Windows)
// ============================================================================

AssetPipeline::AssetMetrics AssetPipeline::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

// ============================================================================
// PRIVATE HELPERS (Windows)
// ============================================================================

void AssetPipeline::LoadingThreadFunction()
{
    while (!m_shouldStop)
    {
        AssetLoadRequest request;
        bool hasRequest = false;

        // Get next request
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCondition.wait(lock, [this] { return !m_loadQueue.empty() || m_shouldStop; });

            if (m_shouldStop)
                break;

            if (!m_loadQueue.empty())
            {
                request = m_loadQueue.front();
                m_loadQueue.pop();
                hasRequest = true;
            }
        }

        if (hasRequest)
        {
            // Load asset
            auto asset = LoadAsset(request.assetPath, request.expectedType);

            if (asset && request.onLoaded)
            {
                request.onLoaded(asset);
            }
            else if (!asset && request.onError)
            {
                request.onError("Failed to load asset: " + request.assetPath);
            }
        }
    }
}

void AssetPipeline::UpdateMetrics()
{
    // Collect data that requires m_assetsMutex first (consistent lock ordering:
    // assets -> metrics, matching LoadAsset and other call-sites).
    uint32_t totalAssets = 0;
    {
        std::lock_guard<std::mutex> assetsLock(m_assetsMutex);
        totalAssets = static_cast<uint32_t>(m_assets.size());
    }

    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.totalAssets = totalAssets;

    // Update other metrics
    m_metrics.streamingThreads = static_cast<uint32_t>(m_loadingThreads.size());
    m_metrics.backgroundLoading = m_backgroundStreaming;

    if (m_cache)
    {
        m_metrics.cacheHitRatio = m_cache->GetHitRatio();
        m_metrics.memoryUsage = m_cache->GetCurrentMemory();
    }
}

#endif // inner SPARK_PLATFORM_WINDOWS

#endif // SPARK_PLATFORM_WINDOWS
