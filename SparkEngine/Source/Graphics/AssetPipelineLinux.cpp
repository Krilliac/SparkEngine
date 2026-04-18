/**
 * @file AssetPipelineLinux.cpp
 * @brief Linux implementation — split from AssetPipeline.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


#include "AssetPipeline.h"
#include "GraphicsEngineRHI.h"
#include "RHI/RHIResources.h"
#include "../Utils/LogMacros.h"
#include "../Utils/Validate.h"
#include <filesystem>
#include <cstring>

// ============================================================================
// ASSET PIPELINE LIFECYCLE (Linux)
// ============================================================================

AssetPipeline::AssetPipeline() : m_device(nullptr), m_context(nullptr)
{
    m_cache = std::make_unique<AssetCache>(512);
    memset(&m_metrics, 0, sizeof(m_metrics));
}

AssetPipeline::~AssetPipeline()
{
    Shutdown();
}

HRESULT AssetPipeline::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device = device;
    m_context = context;
    m_shouldStop = false;
    memset(&m_metrics, 0, sizeof(m_metrics));

    if (!m_cache)
    {
        m_cache = std::make_unique<AssetCache>(512);
    }

    return S_OK;
}

void AssetPipeline::Shutdown()
{
    m_shouldStop = true;
    m_queueCondition.notify_all();

    for (auto& thread : m_loadingThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    m_loadingThreads.clear();

    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        for (auto& pair : m_assets)
        {
            if (pair.second)
                pair.second->Unload();
        }
        m_assets.clear();
    }

    if (m_cache)
        m_cache->Clear();

    m_fileTimestamps.clear();
    m_device = nullptr;
    m_context = nullptr;
}

void AssetPipeline::Update(float /*deltaTime*/)
{
    // Process any queued async load requests
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_loadQueue.empty())
        {
            AssetLoadRequest request = std::move(m_loadQueue.front());
            m_loadQueue.pop();

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

    UpdateMetrics();

    if (m_hotReloadingEnabled)
    {
        CheckForChangedAssets();
    }
}

// ============================================================================
// SYNCHRONOUS LOADING (Linux)
// ============================================================================

std::shared_ptr<Asset> AssetPipeline::LoadAsset(const std::string& path, AssetType type)
{
    // Check if already loaded
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        auto it = m_assets.find(path);
        if (it != m_assets.end())
            return it->second;
    }

    // Check cache
    if (m_cache)
    {
        auto cached = m_cache->GetAsset(path);
        if (cached)
        {
            std::lock_guard<std::mutex> lock(m_assetsMutex);
            m_assets[path] = cached;
            return cached;
        }
    }

    if (type == AssetType::Unknown)
    {
        type = DetectAssetType(path);
    }

    std::shared_ptr<Asset> asset;
    switch (type)
    {
    case AssetType::Mesh:
        asset = LoadMesh(path);
        break;
    case AssetType::Texture:
        asset = LoadTexture(path);
        break;
    case AssetType::Audio:
        asset = LoadAudio(path);
        break;
    default:
        break;
    }

    if (asset)
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        m_assets[path] = asset;
        if (m_cache)
            m_cache->AddAsset(asset);
        // Track file timestamp for hot reloading
        m_fileTimestamps[path] = GetFileTimestamp(path);
    }

    return asset;
}

std::shared_ptr<MeshAsset> AssetPipeline::LoadMesh(const std::string& path)
{
    auto mesh = std::make_shared<MeshAsset>(path);
    mesh->Load(m_device);
    // Linux/macOS render path consumes the RHI buffer handles, not the
    // D3D11 ComPtr members. Populate them here so `BindMesh` / `DrawBoundMesh`
    // (and `GraphicsEngine::ProcessDrawList`) can actually issue draws.
    BuildRHIBuffersForMesh(*mesh);
    return mesh;
}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTexture(const std::string& path)
{
    auto texture = std::make_shared<TextureAsset>(path);
    texture->Load(m_device);
    return texture;
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudio(const std::string& path)
{
    auto audio = std::make_shared<AudioAsset>(path);
    audio->Load(m_device);
    return audio;
}

// ============================================================================
// ASYNCHRONOUS LOADING (Linux)
// ============================================================================

void AssetPipeline::LoadAssetAsync(const AssetLoadRequest& request)
{
    if (m_backgroundStreaming && !m_loadingThreads.empty())
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_loadQueue.push(request);
        m_queueCondition.notify_one();
    }
    else
    {
        // Fall back to synchronous loading
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
    request.onError = [](const std::string&) {};
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
    request.onError = [](const std::string&) {};
    LoadAssetAsync(request);
}

// ============================================================================
// ASSET MANAGEMENT (Linux)
// ============================================================================

void AssetPipeline::UnloadAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assets.find(path);
    if (it != m_assets.end())
    {
        if (it->second)
            it->second->Unload();
        m_assets.erase(it);
    }
    if (m_cache)
        m_cache->RemoveAsset(path);
    m_fileTimestamps.erase(path);
}

void AssetPipeline::UnloadAllAssets()
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    for (auto& pair : m_assets)
    {
        if (pair.second)
            pair.second->Unload();
    }
    m_assets.clear();
    if (m_cache)
        m_cache->Clear();
    m_fileTimestamps.clear();
}

std::shared_ptr<Asset> AssetPipeline::GetAsset(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assets.find(path);
    if (it != m_assets.end())
        return it->second;

    // Try cache as fallback
    if (m_cache)
    {
        auto cached = m_cache->GetAsset(path);
        if (cached)
        {
            m_assets[path] = cached;
            return cached;
        }
    }
    return nullptr;
}

bool AssetPipeline::IsAssetLoaded(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assets.find(path);
    return it != m_assets.end() && it->second && it->second->IsLoaded();
}

// ============================================================================
// CACHE MANAGEMENT (Linux)
// ============================================================================

void AssetPipeline::SetCacheSize(size_t maxMemoryMB)
{
    if (m_cache)
        m_cache->SetMaxMemory(maxMemoryMB);
}

void AssetPipeline::EvictUnusedAssets()
{
    if (m_cache)
        m_cache->EvictLRU();
}

void AssetPipeline::PreloadAssets(const std::vector<std::string>& paths)
{
    for (const auto& path : paths)
    {
        LoadAsset(path);
    }
}

// ============================================================================
// STREAMING CONTROL (Linux)
// ============================================================================

void AssetPipeline::EnableBackgroundStreaming(bool enabled)
{
    m_backgroundStreaming = enabled;
}

void AssetPipeline::SetStreamingThreadCount(int count)
{
    // Stop existing threads
    m_shouldStop = true;
    m_queueCondition.notify_all();
    for (auto& thread : m_loadingThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    m_loadingThreads.clear();

    // Start new threads if requested
    if (count > 0 && m_backgroundStreaming)
    {
        m_shouldStop = false;
        for (int i = 0; i < count; ++i)
        {
            m_loadingThreads.emplace_back(&AssetPipeline::LoadingThreadFunction, this);
        }
    }
}

// ============================================================================
// METRICS (Linux)
// ============================================================================

AssetPipeline::AssetMetrics AssetPipeline::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

// ============================================================================
// PRIVATE HELPERS (Linux)
// ============================================================================

void AssetPipeline::LoadingThreadFunction()
{
    while (!m_shouldStop)
    {
        AssetLoadRequest request;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCondition.wait(lock, [this] { return m_shouldStop || !m_loadQueue.empty(); });

            if (m_shouldStop && m_loadQueue.empty())
                return;
            if (m_loadQueue.empty())
                continue;

            request = std::move(m_loadQueue.front());
            m_loadQueue.pop();
        }

        auto asset = LoadAsset(request.assetPath, request.expectedType);
        if (asset && request.onLoaded)
        {
            request.onLoaded(asset);
        }
        else if (!asset && request.onError)
        {
            request.onError("Failed to load asset: " + request.assetPath);
            std::lock_guard<std::mutex> lock(m_metricsMutex);
            m_metrics.failedLoads++;
        }
    }
}

void AssetPipeline::UpdateMetrics()
{
    // Collect data that requires m_assetsMutex first (consistent lock ordering:
    // assets -> metrics, matching LoadAsset and other call-sites).
    uint32_t totalAssets = 0;
    uint32_t loadedAssets = 0;
    size_t memoryUsage = 0;
    {
        std::lock_guard<std::mutex> assetsLock(m_assetsMutex);
        totalAssets = static_cast<uint32_t>(m_assets.size());
        for (const auto& pair : m_assets)
        {
            if (pair.second && pair.second->IsLoaded())
            {
                loadedAssets++;
                memoryUsage += pair.second->GetMemoryUsage();
            }
        }
    }

    uint32_t pendingRequests = 0;
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        pendingRequests = static_cast<uint32_t>(m_loadQueue.size());
    }

    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.totalAssets = totalAssets;
    m_metrics.loadedAssets = loadedAssets;
    m_metrics.memoryUsage = memoryUsage;

    if (m_metrics.memoryUsage > m_metrics.maxMemoryUsage)
    {
        m_metrics.maxMemoryUsage = m_metrics.memoryUsage;
    }

    m_metrics.pendingRequests = pendingRequests;

    if (m_cache)
    {
        m_metrics.cacheHitRatio = m_cache->GetHitRatio();
    }
    m_metrics.backgroundLoading = m_backgroundStreaming;
    m_metrics.streamingThreads = static_cast<uint32_t>(m_loadingThreads.size());
}

// ============================================================================
// RHI BUFFER UPLOAD (Linux / macOS)
// ============================================================================
// Moves the freshly-loaded CPU-side vertex / index data from `MeshAsset` onto
// the RHI device. This is what lets the non-Windows `ProcessDrawList` /
// `BindMesh` / `DrawBoundMesh` path actually issue draws — the MeshAsset's
// D3D11 ComPtr buffers are stubs on Linux, so without this step the render
// path has nothing to bind.
//
// The bridge singleton is shared across all GraphicsEngine TUs
// (`LinuxRHIState::bridge`); we reach for it directly instead of threading
// a pointer through the async load queue, which would force AssetPipeline
// to know about GraphicsEngine's internals.

void AssetPipeline::BuildRHIBuffersForMesh(MeshAsset& mesh)
{
    auto& rhi = Spark::Graphics::Detail::GetRHI();
    if (!rhi.initialized)
    {
        // Pre-graphics init (e.g. tests loading assets directly) — keep the
        // mesh data on the CPU side and let a later `BuildRHIBuffersForMesh`
        // retry fill the buffers when the bridge is up. Not a warning because
        // headless tooling legitimately uses this path.
        return;
    }

    const MeshAssetData& meshData = mesh.GetMeshData();
    if (meshData.vertices.empty() || meshData.indices.empty())
    {
        // Nothing to upload — a format loader may have returned success on an
        // empty stub asset. Leave the RHI buffers as nullptr; the render path
        // guards on `GetRHIVertexBuffer() != nullptr` before dispatching.
        return;
    }

    const uint64_t vbSize = static_cast<uint64_t>(meshData.vertices.size() * sizeof(MeshAssetData::Vertex));
    const uint32_t vbStride = static_cast<uint32_t>(sizeof(MeshAssetData::Vertex));
    auto vb = rhi.bridge.CreateVertexBuffer(meshData.vertices.data(), vbSize, vbStride);

    const uint64_t ibSize = static_cast<uint64_t>(meshData.indices.size() * sizeof(uint32_t));
    constexpr uint32_t ibStride = sizeof(uint32_t);
    auto ib = rhi.bridge.CreateIndexBuffer(meshData.indices.data(), ibSize, ibStride);

    if (!vb || !ib)
    {
        // CreateVertexBuffer / CreateIndexBuffer only return null on OOM or
        // backend failure. Warn once per mesh so CI logs surface the issue
        // but the asset stays usable (the draw path's null guard keeps the
        // frame rendering, minus this mesh).
        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                       "AssetPipeline::BuildRHIBuffersForMesh: RHI buffer creation failed for '%s' "
                       "(vb=%p, ib=%p)",
                       mesh.GetPath().c_str(), static_cast<void*>(vb.get()), static_cast<void*>(ib.get()));
        return;
    }

    mesh.SetRHIBuffers(std::move(vb), std::move(ib));
}


#endif // !SPARK_PLATFORM_WINDOWS
