#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file AssetPipelineConsoleOps.cpp
 * @brief Console integration methods for AssetPipeline
 *
 * All Console_* methods for runtime asset management via console commands.
 * Split from AssetPipeline.cpp for maintainability.
 */

#include "AssetPipeline.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"
#include <sstream>
#include <filesystem>

#ifdef SPARK_PLATFORM_WINDOWS

AssetPipeline::AssetMetrics AssetPipeline::Console_GetMetrics() const
{
    return GetMetrics();
}

std::string AssetPipeline::Console_ListAssets() const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    std::stringstream ss;

    ss << "=== Loaded Assets (" << m_assets.size() << ") ===\n";
    for (const auto& pair : m_assets)
    {
        const auto& asset = pair.second;
        ss << pair.first << " - " << AssetTypeToString(asset->GetType());
        ss << " (" << (asset->GetMemoryUsage() / 1024) << " KB)\n";
    }

    return ss.str();
}

std::string AssetPipeline::Console_GetAssetInfo(const std::string& path) const
{
    std::shared_ptr<Asset> asset;
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        auto it = m_assets.find(path);
        asset = (it != m_assets.end()) ? it->second : nullptr;
    }

    if (!asset)
    {
        return "Asset not found: " + path;
    }

    std::stringstream ss;
    ss << "=== Asset Info: " << path << " ===\n";
    ss << "Type: " << AssetTypeToString(asset->GetType()) << "\n";
    ss << "Memory Usage: " << (asset->GetMemoryUsage() / 1024) << " KB\n";
    ss << "Loaded: " << (asset->IsLoaded() ? "Yes" : "No") << "\n";

    return ss.str();
}

bool AssetPipeline::Console_LoadAsset(const std::string& path)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Console loading asset: %s", path.c_str());
    auto asset = LoadAsset(path);
    return asset != nullptr;
}

bool AssetPipeline::Console_UnloadAsset(const std::string& path)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Console unloading asset: %s", path.c_str());
    UnloadAsset(path);
    return true;
}

void AssetPipeline::Console_SetCacheSize(size_t maxMemoryMB)
{
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Asset cache size set to %zu MB", maxMemoryMB);
    SetCacheSize(maxMemoryMB);
    Spark::SimpleConsole::GetInstance().LogSuccess("Asset cache size set to: " + std::to_string(maxMemoryMB) + " MB");
}

void AssetPipeline::Console_ForceGC()
{
    EvictUnusedAssets();
    Spark::SimpleConsole::GetInstance().LogSuccess("Asset garbage collection completed");
}

void AssetPipeline::Console_EnableStreaming(bool enabled)
{
    EnableBackgroundStreaming(enabled);
    Spark::SimpleConsole::GetInstance().LogSuccess("Background streaming " +
                                                   std::string(enabled ? "enabled" : "disabled"));
}

void AssetPipeline::Console_SetStreamingThreads(int count)
{
    SetStreamingThreadCount(count);
    Spark::SimpleConsole::GetInstance().LogSuccess("Streaming thread count set to: " + std::to_string(count));
}

int AssetPipeline::Console_ScanDirectory(const std::string& directory)
{
    auto assets = ScanDirectory(directory);
    Spark::SimpleConsole::GetInstance().LogSuccess("Found " + std::to_string(assets.size()) +
                                                   " assets in: " + directory);
    return static_cast<int>(assets.size());
}

void AssetPipeline::Console_EnableHotReload(bool enabled)
{
    EnableHotReloading(enabled);
    Spark::SimpleConsole::GetInstance().LogSuccess("Hot reloading " + std::string(enabled ? "enabled" : "disabled"));
}

int AssetPipeline::Console_PreloadDirectory(const std::string& directory)
{
    auto assets = ScanDirectory(directory);
    PreloadAssets(assets);
    Spark::SimpleConsole::GetInstance().LogSuccess("Preloaded " + std::to_string(assets.size()) +
                                                   " assets from: " + directory);
    return static_cast<int>(assets.size());
}

int AssetPipeline::Console_ReloadAllAssets()
{
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        count = static_cast<int>(m_assets.size());
    }

    UnloadAllAssets();
    Spark::SimpleConsole::GetInstance().LogSuccess("Marked " + std::to_string(count) + " assets for reload");
    return count;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "../Utils/LogMacros.h"
#include <sstream>
#include <filesystem>

AssetPipeline::AssetMetrics AssetPipeline::Console_GetMetrics() const
{
    return GetMetrics();
}

std::string AssetPipeline::Console_ListAssets() const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    std::stringstream ss;
    ss << "=== Loaded Assets (" << m_assets.size() << ") ===\n";
    if (m_assets.empty())
    {
        ss << "  (none)\n";
    }
    else
    {
        for (const auto& pair : m_assets)
        {
            ss << "  " << pair.first;
            if (pair.second)
            {
                ss << " [" << AssetTypeToString(pair.second->GetType()) << "]";
                ss << " " << (pair.second->IsLoaded() ? "loaded" : "unloaded");
                ss << " (" << (pair.second->GetMemoryUsage() / 1024) << " KB)";
            }
            ss << "\n";
        }
    }
    ss << "--- Cache: hit ratio " << (m_cache ? m_cache->GetHitRatio() * 100.0f : 0.0f) << "% ("
       << (m_cache ? m_cache->GetCacheHits() : 0u) << " hits / " << (m_cache ? m_cache->GetCacheMisses() : 0u)
       << " misses)\n";
    return ss.str();
}

std::string AssetPipeline::Console_GetAssetInfo(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    auto it = m_assets.find(path);
    if (it == m_assets.end() || !it->second)
    {
        return "Asset not found: " + path;
    }

    const auto& asset = it->second;
    const auto& meta = asset->GetMetadata();
    std::stringstream ss;
    ss << "=== Asset: " << path << " ===\n";
    ss << "  Name:       " << meta.name << "\n";
    ss << "  Type:       " << AssetTypeToString(asset->GetType()) << "\n";
    ss << "  Loaded:     " << (asset->IsLoaded() ? "Yes" : "No") << "\n";
    ss << "  Memory:     " << (asset->GetMemoryUsage() / 1024) << " KB\n";
    ss << "  File Size:  " << (meta.fileSize / 1024) << " KB\n";
    ss << "  State:      " << StreamingStateToString(meta.state) << "\n";
    if (!meta.checksum.empty())
    {
        ss << "  Checksum:   " << meta.checksum << "\n";
    }
    if (!meta.dependencies.empty())
    {
        ss << "  Dependencies: " << meta.dependencies.size() << "\n";
    }
    return ss.str();
}

bool AssetPipeline::Console_LoadAsset(const std::string& path)
{
    auto asset = LoadAsset(path);
    return asset != nullptr;
}

bool AssetPipeline::Console_UnloadAsset(const std::string& path)
{
    if (!IsAssetLoaded(path))
        return false;
    UnloadAsset(path);
    return true;
}

void AssetPipeline::Console_SetCacheSize(size_t maxMemoryMB)
{
    SetCacheSize(maxMemoryMB);
}

void AssetPipeline::Console_ForceGC()
{
    EvictUnusedAssets();
    UpdateMetrics();
}

void AssetPipeline::Console_EnableStreaming(bool enabled)
{
    EnableBackgroundStreaming(enabled);
}

void AssetPipeline::Console_SetStreamingThreads(int count)
{
    SetStreamingThreadCount(count);
}

int AssetPipeline::Console_ScanDirectory(const std::string& directory)
{
    auto results = ScanDirectory(directory);
    return static_cast<int>(results.size());
}

void AssetPipeline::Console_EnableHotReload(bool enabled)
{
    EnableHotReloading(enabled);
}

int AssetPipeline::Console_PreloadDirectory(const std::string& directory)
{
    auto paths = ScanDirectory(directory);
    PreloadAssets(paths);
    return static_cast<int>(paths.size());
}

int AssetPipeline::Console_ReloadAllAssets()
{
    std::lock_guard<std::mutex> lock(m_assetsMutex);
    int count = 0;
    for (auto& pair : m_assets)
    {
        if (pair.second)
        {
            pair.second->Unload();
            pair.second->Load(m_device);
            count++;
        }
    }
    return count;
}

#endif // SPARK_PLATFORM_WINDOWS
