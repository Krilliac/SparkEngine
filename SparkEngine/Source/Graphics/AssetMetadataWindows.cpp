/**
 * @file AssetMetadataWindows.cpp
 * @brief Windows/D3D11 implementation — split from AssetMetadata.cpp
 */
#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @file AssetMetadata.cpp
 * @brief Asset metadata, type detection, path resolution, directory scanning
 *
 * Contains asset type detection from file extensions, metadata retrieval,
 * hot-reload timestamp tracking, checksum calculation, and enum-to-string
 * utility functions.
 * Split from AssetPipeline.cpp for maintainability.
 */

#include "AssetPipeline.h"
#include "../Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include <fstream>
#include <filesystem>
#include <algorithm>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// ASSET TYPE DETECTION (Windows)
// ============================================================================

AssetType AssetPipeline::DetectAssetType(const std::string& path) const
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return DetectAssetTypeFromExtension(ext);
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

// ============================================================================
// DIRECTORY SCANNING (Windows)
// ============================================================================

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
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Error scanning directory '%s': %s", directory.c_str(), e.what());
        Spark::SimpleConsole::GetInstance().LogError("Error scanning directory: " + directory + " - " + e.what());
    }

    SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "ScanDirectory '%s': found %zu assets", directory.c_str(),
                    assets.size());
    return assets;
}

// ============================================================================
// METADATA RETRIEVAL (Windows)
// ============================================================================

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
        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "GetAssetMetadata '%s': type=%s size=%zu", path.c_str(),
                        AssetTypeToString(metadata.type).c_str(), static_cast<size_t>(metadata.fileSize));
    }
    else
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GetAssetMetadata: file not found '%s'", path.c_str());
    }

    return metadata;
}

void AssetPipeline::RefreshAssetMetadata(const std::string& path)
{
    // Implementation would refresh metadata from file
}

// ============================================================================
// HOT-RELOAD SUPPORT (Windows)
// ============================================================================

void AssetPipeline::CheckForChangedAssets()
{
    // Implementation would check file timestamps and reload changed assets
}

// ============================================================================
// FILE UTILITIES (Windows)
// ============================================================================

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

// ============================================================================
// UTILITY FUNCTIONS (Windows)
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

#endif // inner SPARK_PLATFORM_WINDOWS

#endif // SPARK_PLATFORM_WINDOWS
