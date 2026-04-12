/**
 * @file AssetMetadataLinux.cpp
 * @brief Linux implementation — split from AssetMetadata.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


#include "AssetPipeline.h"
#include "Utils/LogMacros.h"
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <cstring>

// ============================================================================
// ASSET TYPE DETECTION (Linux)
// ============================================================================

AssetType AssetPipeline::DetectAssetType(const std::string& path) const
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return DetectAssetTypeFromExtension(ext);
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

// ============================================================================
// DIRECTORY SCANNING (Linux)
// ============================================================================

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

    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "ScanDirectory '%s': found %zu assets", directory.c_str(),
                    results.size());
    std::sort(results.begin(), results.end());
    return results;
}

// ============================================================================
// METADATA RETRIEVAL (Linux)
// ============================================================================

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

// ============================================================================
// HOT-RELOAD SUPPORT (Linux)
// ============================================================================

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
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Hot-reload: asset changed '%s', reloading", path.c_str());
                assetIt->second->Unload();
                assetIt->second->Load(m_device);
                pair.second = currentTimestamp;
            }
        }
    }
}

// ============================================================================
// FILE UTILITIES (Linux)
// ============================================================================

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

// ============================================================================
// UTILITY FUNCTIONS (Linux)
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


#endif // !SPARK_PLATFORM_WINDOWS
