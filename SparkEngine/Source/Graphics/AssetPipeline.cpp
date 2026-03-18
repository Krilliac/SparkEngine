#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file AssetPipeline.cpp
 * @brief Core asset pipeline — lifecycle, loading, caching, streaming
 *
 * Asset type implementations (MeshAsset, TextureAsset, AudioAsset) are in AssetTypes.cpp.
 * Console integration methods are in AssetPipelineConsoleOps.cpp.
 */

#include "AssetPipeline.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// ============================================================================
// ASSET PIPELINE IMPLEMENTATION
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
    // Stop loading threads
    m_shouldStop = true;
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
    return m_assets.find(path) != m_assets.end();
}

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
    m_shouldStop = false;

    // Start new threads
    for (int i = 0; i < count; ++i)
    {
        m_loadingThreads.emplace_back(&AssetPipeline::LoadingThreadFunction, this);
    }
}

std::vector<std::string> AssetPipeline::ScanDirectory(const std::string& directory, AssetType type)
{
    std::vector<std::string> assets;

    try
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        {
            if (entry.is_regular_file())
            {
                AssetType detectedType = DetectAssetType(entry.path().string());
                if (type == AssetType::Unknown || detectedType == type)
                {
                    assets.push_back(entry.path().string());
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        Spark::SimpleConsole::GetInstance().LogError("Error scanning directory: " + directory + " - " + e.what());
    }

    return assets;
}

AssetType AssetPipeline::DetectAssetType(const std::string& path) const
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return DetectAssetTypeFromExtension(ext);
}

AssetMetadata AssetPipeline::GetAssetMetadata(const std::string& path) const
{
    AssetMetadata metadata;
    metadata.filePath = path;
    metadata.name = std::filesystem::path(path).stem().string();
    metadata.type = DetectAssetType(path);

    if (std::filesystem::exists(path))
    {
        metadata.fileSize = std::filesystem::file_size(path);
        metadata.lastModified = GetFileTimestamp(path);
        metadata.checksum = CalculateChecksum(path);
    }

    return metadata;
}

void AssetPipeline::RefreshAssetMetadata(const std::string& path)
{
    // Implementation would refresh metadata from file
}

void AssetPipeline::CheckForChangedAssets()
{
    // Implementation would check file timestamps and reload changed assets
}

AssetPipeline::AssetMetrics AssetPipeline::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

// Console integration methods

// Private helper methods
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

AssetType AssetPipeline::DetectAssetTypeFromExtension(const std::string& extension) const
{
    if (extension == ".obj" || extension == ".fbx" || extension == ".dae" || extension == ".gltf" ||
        extension == ".glb")
    {
        return AssetType::Mesh;
    }
    else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" ||
             extension == ".dds")
    {
        return AssetType::Texture;
    }
    else if (extension == ".wav" || extension == ".mp3" || extension == ".ogg")
    {
        return AssetType::Audio;
    }
    else if (extension == ".hlsl" || extension == ".fx")
    {
        return AssetType::Shader;
    }
    else if (extension == ".ttf" || extension == ".otf")
    {
        return AssetType::Font;
    }

    return AssetType::Unknown;
}

std::string AssetPipeline::CalculateChecksum(const std::string& filePath) const
{
    // Simple checksum implementation - in production would use MD5/SHA
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return "";

    size_t hash = 0;
    char buffer[1024];
    while (file.read(buffer, sizeof(buffer)))
    {
        for (std::streamsize i = 0; i < file.gcount(); ++i)
        {
            hash = hash * 31 + buffer[i];
        }
    }

    return std::to_string(hash);
}

uint64_t AssetPipeline::GetFileTimestamp(const std::string& filePath) const
{
    try
    {
        auto time = std::filesystem::last_write_time(filePath);
        return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    }
    catch (...)
    {
        return 0;
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

std::shared_ptr<MeshAsset> AssetPipeline::LoadMeshFromFile(const std::string& path)
{
    auto meshAsset = std::make_shared<MeshAsset>(path);
    HRESULT hr = meshAsset->Load(m_device);

    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load mesh: " + path);
        return nullptr;
    }

    return meshAsset;
}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTextureFromFile(const std::string& path)
{
    auto textureAsset = std::make_shared<TextureAsset>(path);
    HRESULT hr = textureAsset->Load(m_device);

    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load texture: " + path);
        return nullptr;
    }

    return textureAsset;
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudioFromFile(const std::string& path)
{
    auto audioAsset = std::make_shared<AudioAsset>(path);
    HRESULT hr = audioAsset->Load(m_device);

    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load audio: " + path);
        return nullptr;
    }

    return audioAsset;
}

// Utility functions
std::string AssetTypeToString(AssetType type)
{
    switch (type)
    {
    case AssetType::Mesh:
        return "Mesh";
    case AssetType::Texture:
        return "Texture";
    case AssetType::Material:
        return "Material";
    case AssetType::Audio:
        return "Audio";
    case AssetType::Animation:
        return "Animation";
    case AssetType::Prefab:
        return "Prefab";
    case AssetType::Scene:
        return "Scene";
    case AssetType::Shader:
        return "Shader";
    case AssetType::Font:
        return "Font";
    default:
        return "Unknown";
    }
}

AssetType StringToAssetType(const std::string& str)
{
    if (str == "Mesh")
        return AssetType::Mesh;
    if (str == "Texture")
        return AssetType::Texture;
    if (str == "Material")
        return AssetType::Material;
    if (str == "Audio")
        return AssetType::Audio;
    if (str == "Animation")
        return AssetType::Animation;
    if (str == "Prefab")
        return AssetType::Prefab;
    if (str == "Scene")
        return AssetType::Scene;
    if (str == "Shader")
        return AssetType::Shader;
    if (str == "Font")
        return AssetType::Font;
    return AssetType::Unknown;
}

std::string StreamingStateToString(StreamingState state)
{
    switch (state)
    {
    case StreamingState::Unloaded:
        return "Unloaded";
    case StreamingState::Loading:
        return "Loading";
    case StreamingState::Loaded:
        return "Loaded";
    case StreamingState::Failed:
        return "Failed";
    case StreamingState::Evicted:
        return "Evicted";
    default:
        return "Unknown";
    }
}

std::string LoadingPriorityToString(LoadingPriority priority)
{
    switch (priority)
    {
    case LoadingPriority::Low:
        return "Low";
    case LoadingPriority::Normal:
        return "Normal";
    case LoadingPriority::High:
        return "High";
    case LoadingPriority::Critical:
        return "Critical";
    default:
        return "Unknown";
    }
}

#endif // inner SPARK_PLATFORM_WINDOWS
#else  // !SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <cstring>
#include <cfloat>
#include <unordered_map>
#include <tiny_obj_loader.h>

// ============================================================================
// AssetPipeline (Linux)
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

std::vector<std::string> AssetPipeline::ScanDirectory(const std::string& directory, AssetType type)
{
    std::vector<std::string> results;
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory))
    {
        return results;
    }

    try
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        {
            if (entry.is_regular_file())
            {
                std::string filePath = entry.path().string();
                AssetType detectedType = DetectAssetType(filePath);
                if (type == AssetType::Unknown || detectedType == type)
                {
                    results.push_back(filePath);
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error&)
    {
        // Permission denied or other filesystem error - return what we have
    }

    std::sort(results.begin(), results.end());
    return results;
}

AssetType AssetPipeline::DetectAssetType(const std::string& path) const
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return DetectAssetTypeFromExtension(ext);
}

AssetMetadata AssetPipeline::GetAssetMetadata(const std::string& path) const
{
    // First check if the asset is loaded and has metadata
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        auto it = m_assets.find(path);
        if (it != m_assets.end() && it->second)
        {
            return it->second->GetMetadata();
        }
    }

    // Build metadata from filesystem
    AssetMetadata metadata;
    metadata.filePath = path;
    metadata.name = std::filesystem::path(path).stem().string();
    metadata.type = DetectAssetType(path);
    metadata.state = StreamingState::Unloaded;
    metadata.priority = LoadingPriority::Normal;

    if (std::filesystem::exists(path))
    {
        metadata.fileSize = std::filesystem::file_size(path);
        metadata.lastModified = GetFileTimestamp(path);
        metadata.checksum = CalculateChecksum(path);
    }
    else
    {
        metadata.fileSize = 0;
        metadata.lastModified = 0;
    }
    metadata.memorySize = 0;

    return metadata;
}

void AssetPipeline::RefreshAssetMetadata(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assets.find(path);
    if (it == m_assets.end() || !it->second)
        return;

    // Re-check the file on disk and update timestamp
    if (std::filesystem::exists(path))
    {
        m_fileTimestamps[path] = GetFileTimestamp(path);
    }
}

void AssetPipeline::CheckForChangedAssets()
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    for (auto& pair : m_fileTimestamps)
    {
        const std::string& path = pair.first;
        uint64_t storedTimestamp = pair.second;
        uint64_t currentTimestamp = GetFileTimestamp(path);

        if (currentTimestamp != 0 && currentTimestamp != storedTimestamp)
        {
            // File has changed - reload the asset
            auto assetIt = m_assets.find(path);
            if (assetIt != m_assets.end() && assetIt->second)
            {
                assetIt->second->Unload();
                assetIt->second->Load(m_device);
                pair.second = currentTimestamp;
            }
        }
    }
}

AssetPipeline::AssetMetrics AssetPipeline::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}


// ============================================================================
// Private helpers
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

AssetType AssetPipeline::DetectAssetTypeFromExtension(const std::string& extension) const
{
    if (extension == ".obj" || extension == ".fbx" || extension == ".gltf" || extension == ".glb")
        return AssetType::Mesh;
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
        extension == ".dds" || extension == ".tga" || extension == ".hdr")
        return AssetType::Texture;
    if (extension == ".wav" || extension == ".ogg" || extension == ".mp3")
        return AssetType::Audio;
    if (extension == ".hlsl" || extension == ".glsl" || extension == ".cso")
        return AssetType::Shader;
    if (extension == ".mat")
        return AssetType::Material;
    if (extension == ".anim")
        return AssetType::Animation;
    if (extension == ".prefab")
        return AssetType::Prefab;
    if (extension == ".scene")
        return AssetType::Scene;
    if (extension == ".ttf" || extension == ".otf")
        return AssetType::Font;
    return AssetType::Unknown;
}

std::string AssetPipeline::CalculateChecksum(const std::string& filePath) const
{
    // Simple additive checksum for Linux (no GPU work, CPU-side only)
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return "";

    uint32_t checksum = 0;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)))
    {
        for (std::streamsize i = 0; i < file.gcount(); ++i)
        {
            checksum = checksum * 31 + static_cast<unsigned char>(buffer[i]);
        }
    }
    // Process remaining bytes
    for (std::streamsize i = 0; i < file.gcount(); ++i)
    {
        checksum = checksum * 31 + static_cast<unsigned char>(buffer[i]);
    }

    std::stringstream ss;
    ss << std::hex << checksum;
    return ss.str();
}

uint64_t AssetPipeline::GetFileTimestamp(const std::string& filePath) const
{
    struct stat fileStat;
    if (stat(filePath.c_str(), &fileStat) != 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(fileStat.st_mtime);
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

std::shared_ptr<MeshAsset> AssetPipeline::LoadMeshFromFile(const std::string& path)
{
    auto mesh = std::make_shared<MeshAsset>(path);
    if (mesh->Load(m_device) == S_OK)
    {
        return mesh;
    }
    return nullptr;
}

std::shared_ptr<TextureAsset> AssetPipeline::LoadTextureFromFile(const std::string& path)
{
    auto texture = std::make_shared<TextureAsset>(path);
    if (texture->Load(m_device) == S_OK)
    {
        return texture;
    }
    return nullptr;
}

std::shared_ptr<AudioAsset> AssetPipeline::LoadAudioFromFile(const std::string& path)
{
    auto audio = std::make_shared<AudioAsset>(path);
    if (audio->Load(m_device) == S_OK)
    {
        return audio;
    }
    return nullptr;
}

HRESULT AssetPipeline::LoadOBJ(const std::string& path, MeshAssetData& meshData)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str());

    if (!ret)
    {
        fprintf(stderr, "[AssetPipeline] OBJ load failed: %s %s\n", path.c_str(), err.c_str());
        return E_FAIL;
    }

    meshData.vertices.clear();
    meshData.indices.clear();
    meshData.submeshes.clear();

    XMFLOAT3 bboxMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 bboxMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    std::unordered_map<size_t, uint32_t> uniqueVertices;

    for (const auto& shape : shapes)
    {
        meshData.submeshes.push_back(static_cast<uint32_t>(meshData.indices.size()));

        for (const auto& index : shape.mesh.indices)
        {
            MeshAssetData::Vertex vertex{};

            if (index.vertex_index >= 0)
            {
                vertex.position = {attrib.vertices[3 * index.vertex_index + 0],
                                   attrib.vertices[3 * index.vertex_index + 1],
                                   attrib.vertices[3 * index.vertex_index + 2]};

                // Update bounding box
                bboxMin.x = std::min(bboxMin.x, vertex.position.x);
                bboxMin.y = std::min(bboxMin.y, vertex.position.y);
                bboxMin.z = std::min(bboxMin.z, vertex.position.z);
                bboxMax.x = std::max(bboxMax.x, vertex.position.x);
                bboxMax.y = std::max(bboxMax.y, vertex.position.y);
                bboxMax.z = std::max(bboxMax.z, vertex.position.z);
            }

            if (index.normal_index >= 0 && !attrib.normals.empty())
            {
                vertex.normal = {attrib.normals[3 * index.normal_index + 0], attrib.normals[3 * index.normal_index + 1],
                                 attrib.normals[3 * index.normal_index + 2]};
            }

            if (index.texcoord_index >= 0 && !attrib.texcoords.empty())
            {
                vertex.texCoord0 = {attrib.texcoords[2 * index.texcoord_index + 0],
                                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};
            }

            vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};

            size_t h = std::hash<int>()(index.vertex_index);
            h ^= std::hash<int>()(index.normal_index) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(index.texcoord_index) + 0x9e3779b9 + (h << 6) + (h >> 2);

            auto it = uniqueVertices.find(h);
            if (it == uniqueVertices.end())
            {
                uint32_t newIndex = static_cast<uint32_t>(meshData.vertices.size());
                uniqueVertices[h] = newIndex;
                meshData.vertices.push_back(vertex);
                meshData.indices.push_back(newIndex);
            }
            else
            {
                meshData.indices.push_back(it->second);
            }
        }
    }

    meshData.boundingBoxMin = bboxMin;
    meshData.boundingBoxMax = bboxMax;

    // Calculate bounding sphere
    meshData.boundingSphereCenter = {(bboxMin.x + bboxMax.x) * 0.5f, (bboxMin.y + bboxMax.y) * 0.5f,
                                     (bboxMin.z + bboxMax.z) * 0.5f};
    float dx = bboxMax.x - bboxMin.x;
    float dy = bboxMax.y - bboxMin.y;
    float dz = bboxMax.z - bboxMin.z;
    meshData.boundingSphereRadius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;

    fprintf(stderr, "[AssetPipeline] Loaded OBJ: %s (%zu verts, %zu tris)\n", path.c_str(), meshData.vertices.size(),
            meshData.indices.size() / 3);
    return S_OK;
}

HRESULT AssetPipeline::LoadFBX(const std::string& path, MeshAssetData& /*meshData*/)
{
    // FBX loading not implemented on Linux - requires proprietary SDK
    (void)path;
    return E_NOTIMPL;
}

HRESULT AssetPipeline::LoadGLTF(const std::string& path, MeshAssetData& /*meshData*/)
{
    // glTF loading not implemented on Linux - requires external library
    (void)path;
    return E_NOTIMPL;
}

// ============================================================================
// Utility functions
// ============================================================================

std::string AssetTypeToString(AssetType type)
{
    switch (type)
    {
    case AssetType::Mesh:
        return "Mesh";
    case AssetType::Texture:
        return "Texture";
    case AssetType::Material:
        return "Material";
    case AssetType::Audio:
        return "Audio";
    case AssetType::Animation:
        return "Animation";
    case AssetType::Prefab:
        return "Prefab";
    case AssetType::Scene:
        return "Scene";
    case AssetType::Shader:
        return "Shader";
    case AssetType::Font:
        return "Font";
    default:
        return "Unknown";
    }
}

AssetType StringToAssetType(const std::string& str)
{
    if (str == "Mesh")
        return AssetType::Mesh;
    if (str == "Texture")
        return AssetType::Texture;
    if (str == "Material")
        return AssetType::Material;
    if (str == "Audio")
        return AssetType::Audio;
    if (str == "Animation")
        return AssetType::Animation;
    if (str == "Prefab")
        return AssetType::Prefab;
    if (str == "Scene")
        return AssetType::Scene;
    if (str == "Shader")
        return AssetType::Shader;
    if (str == "Font")
        return AssetType::Font;
    return AssetType::Unknown;
}

std::string StreamingStateToString(StreamingState state)
{
    switch (state)
    {
    case StreamingState::Unloaded:
        return "Unloaded";
    case StreamingState::Loading:
        return "Loading";
    case StreamingState::Loaded:
        return "Loaded";
    case StreamingState::Failed:
        return "Failed";
    case StreamingState::Evicted:
        return "Evicted";
    default:
        return "Unknown";
    }
}

std::string LoadingPriorityToString(LoadingPriority priority)
{
    switch (priority)
    {
    case LoadingPriority::Low:
        return "Low";
    case LoadingPriority::Normal:
        return "Normal";
    case LoadingPriority::High:
        return "High";
    case LoadingPriority::Critical:
        return "Critical";
    default:
        return "Unknown";
    }
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// RENDERING HELPERS
// ============================================================================

void AssetPipeline::BindMesh(const std::string& meshPath)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    auto it = m_assets.find(meshPath);
    if (it == m_assets.end() || !it->second || !it->second->IsLoaded())
    {
        return;
    }

    auto* meshAsset = dynamic_cast<MeshAsset*>(it->second.get());
    if (!meshAsset)
    {
        return;
    }

    ID3D11Buffer* vertexBuffer = meshAsset->GetVertexBuffer();
    ID3D11Buffer* indexBuffer = meshAsset->GetIndexBuffer();
    if (!vertexBuffer || !indexBuffer)
    {
        return;
    }

    constexpr UINT stride = sizeof(MeshAssetData::Vertex);
    constexpr UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    m_context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
#else
    (void)meshPath;
#endif
}

void AssetPipeline::BindMaterial(const std::string& materialPath)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    auto it = m_assets.find(materialPath);
    if (it == m_assets.end() || !it->second || !it->second->IsLoaded())
    {
        return;
    }

    auto* textureAsset = dynamic_cast<TextureAsset*>(it->second.get());
    if (!textureAsset)
    {
        return;
    }

    ID3D11ShaderResourceView* srv = textureAsset->GetSRV();
    if (srv)
    {
        m_context->PSSetShaderResources(0, 1, &srv);
    }
#else
    (void)materialPath;
#endif
}

void AssetPipeline::DrawBoundMesh()
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    for (const auto& [path, asset] : m_assets)
    {
        if (!asset || asset->GetType() != AssetType::Mesh || !asset->IsLoaded())
        {
            continue;
        }

        auto* meshAsset = dynamic_cast<MeshAsset*>(asset.get());
        if (meshAsset && meshAsset->GetIndexCount() > 0)
        {
            m_context->DrawIndexed(meshAsset->GetIndexCount(), 0, 0);
            return;
        }
    }
#endif
}
