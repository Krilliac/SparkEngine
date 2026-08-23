/**
 * @file AssetProcessors.cpp
 * @brief Asset processor implementations (Texture, Mesh, Audio, DependencyGraph)
 *
 * Split from AdvancedAssetPipeline.cpp for maintainability.
 */

#include "AdvancedAssetPipeline.h"
#include "Graphics/LODGenerator.h"
#include "Utils/ContainerUtils.h"
#include "Utils/LogMacros.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace SparkEditor
{
    namespace
    {
        struct LODProfileDefinition
        {
            std::vector<float> triangleTargets;
            std::vector<float> errorThresholds;
            std::vector<float> screenThresholds;
        };

        const char* ToString(LODTargetPlatform platform)
        {
            switch (platform)
            {
            case LODTargetPlatform::Desktop:
                return "Desktop";
            case LODTargetPlatform::Mobile:
                return "Mobile";
            case LODTargetPlatform::Console:
                return "Console";
            }
            return "Desktop";
        }

        const char* ToString(LODQualityTier quality)
        {
            switch (quality)
            {
            case LODQualityTier::Low:
                return "Low";
            case LODQualityTier::Medium:
                return "Medium";
            case LODQualityTier::High:
                return "High";
            case LODQualityTier::Ultra:
                return "Ultra";
            }
            return "High";
        }

        std::chrono::system_clock::time_point ToSystemClock(fs::file_time_type fileTime)
        {
            const auto fileNow = fs::file_time_type::clock::now();
            const auto systemNow = std::chrono::system_clock::now();
            return std::chrono::time_point_cast<std::chrono::system_clock::duration>(fileTime - fileNow + systemNow);
        }

        LODProfileDefinition GetLODProfile(LODTargetPlatform platform, LODQualityTier quality)
        {
            // Triangle targets are ratios versus LOD0.
            if (platform == LODTargetPlatform::Mobile)
            {
                switch (quality)
                {
                case LODQualityTier::Low:
                    return {{1.0f, 0.45f, 0.22f, 0.10f}, {0.0f, 0.015f, 0.03f, 0.06f}, {1.0f, 0.7f, 0.4f, 0.2f}};
                case LODQualityTier::Medium:
                    return {{1.0f, 0.55f, 0.30f, 0.15f}, {0.0f, 0.012f, 0.025f, 0.05f}, {1.0f, 0.75f, 0.5f, 0.25f}};
                case LODQualityTier::High:
                case LODQualityTier::Ultra:
                    return {{1.0f, 0.65f, 0.38f, 0.20f}, {0.0f, 0.010f, 0.020f, 0.04f}, {1.0f, 0.8f, 0.55f, 0.3f}};
                }
            }
            else if (platform == LODTargetPlatform::Console)
            {
                switch (quality)
                {
                case LODQualityTier::Low:
                    return {{1.0f, 0.55f, 0.30f, 0.16f}, {0.0f, 0.012f, 0.024f, 0.048f}, {1.0f, 0.75f, 0.48f, 0.24f}};
                case LODQualityTier::Medium:
                    return {{1.0f, 0.62f, 0.36f, 0.20f}, {0.0f, 0.010f, 0.020f, 0.040f}, {1.0f, 0.8f, 0.55f, 0.3f}};
                case LODQualityTier::High:
                    return {{1.0f, 0.70f, 0.45f, 0.25f}, {0.0f, 0.009f, 0.018f, 0.035f}, {1.0f, 0.82f, 0.6f, 0.35f}};
                case LODQualityTier::Ultra:
                    return {{1.0f, 0.78f, 0.52f, 0.30f}, {0.0f, 0.007f, 0.015f, 0.03f}, {1.0f, 0.86f, 0.65f, 0.4f}};
                }
            }

            // Desktop defaults
            switch (quality)
            {
            case LODQualityTier::Low:
                return {{1.0f, 0.60f, 0.35f, 0.20f}, {0.0f, 0.012f, 0.024f, 0.05f}, {1.0f, 0.78f, 0.52f, 0.28f}};
            case LODQualityTier::Medium:
                return {{1.0f, 0.68f, 0.42f, 0.25f}, {0.0f, 0.010f, 0.020f, 0.04f}, {1.0f, 0.82f, 0.58f, 0.34f}};
            case LODQualityTier::High:
                return {{1.0f, 0.75f, 0.50f, 0.30f}, {0.0f, 0.008f, 0.016f, 0.032f}, {1.0f, 0.85f, 0.62f, 0.38f}};
            case LODQualityTier::Ultra:
                return {{1.0f, 0.82f, 0.58f, 0.36f}, {0.0f, 0.006f, 0.013f, 0.026f}, {1.0f, 0.88f, 0.68f, 0.42f}};
            }

            return {{1.0f, 0.75f, 0.50f, 0.30f}, {0.0f, 0.008f, 0.016f, 0.032f}, {1.0f, 0.85f, 0.62f, 0.38f}};
        }

        bool LoadOBJMeshForLOD(const std::string& filePath, std::vector<float>& positions,
                               std::vector<uint32_t>& indices)
        {
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                return false;
            }

            positions.clear();
            indices.clear();

            std::vector<std::array<float, 3>> rawPositions;
            std::string line;
            while (std::getline(file, line))
            {
                std::istringstream iss(line);
                std::string prefix;
                iss >> prefix;

                if (prefix == "v")
                {
                    std::array<float, 3> p{};
                    iss >> p[0] >> p[1] >> p[2];
                    rawPositions.push_back(p);
                }
                else if (prefix == "f")
                {
                    std::vector<uint32_t> face;
                    std::string token;
                    while (iss >> token)
                    {
                        std::istringstream tokenStream(token);
                        std::string indexToken;
                        std::getline(tokenStream, indexToken, '/');
                        if (indexToken.empty())
                        {
                            continue;
                        }

                        int idx = std::stoi(indexToken);
                        if (idx > 0)
                        {
                            face.push_back(static_cast<uint32_t>(idx - 1));
                        }
                    }

                    for (size_t i = 2; i < face.size(); ++i)
                    {
                        indices.push_back(face[0]);
                        indices.push_back(face[i - 1]);
                        indices.push_back(face[i]);
                    }
                }
            }

            positions.reserve(rawPositions.size() * 3);
            for (const auto& p : rawPositions)
            {
                positions.push_back(p[0]);
                positions.push_back(p[1]);
                positions.push_back(p[2]);
            }

            return !positions.empty() && indices.size() >= 3;
        }

        std::string ComputeFileChecksum(const std::string& filePath)
        {
            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open())
            {
                return {};
            }

            constexpr std::size_t prime = 0x100000001B3ULL;
            constexpr std::size_t offset = 0xCBF29CE484222325ULL;
            std::size_t hash = offset;

            char buffer[4096];
            while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0)
            {
                const std::streamsize bytesRead = file.gcount();
                for (std::streamsize i = 0; i < bytesRead; ++i)
                {
                    hash ^= static_cast<std::size_t>(static_cast<unsigned char>(buffer[i]));
                    hash *= prime;
                }
            }

            std::ostringstream oss;
            oss << std::hex << hash;
            return oss.str();
        }
    } // namespace

    // =========================================================================
    // AssetProcessor
    // =========================================================================

    bool AssetProcessor::CanProcess(const std::string& filePath) const
    {
        const auto extensions = GetSupportedExtensions();
        const fs::path path(filePath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        for (const auto& supported : extensions)
        {
            std::string supportedLower = supported;
            std::transform(supportedLower.begin(), supportedLower.end(), supportedLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == supportedLower)
            {
                return true;
            }
        }
        return false;
    }

    // =========================================================================
    // TextureProcessor
    // =========================================================================

    std::vector<std::string> TextureProcessor::GetSupportedExtensions() const
    {
        return {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".hdr"};
    }

    bool TextureProcessor::Process(AssetMetadata& metadata, const AssetImportSettings& settings,
                                   std::function<void(float)> progressCallback)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Processing texture: %s", metadata.sourceFilePath.c_str());
        if (progressCallback)
        {
            progressCallback(0.0f);
        }

        const fs::path sourcePath(metadata.sourceFilePath);
        if (!fs::exists(sourcePath))
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Source file does not exist: " + metadata.sourceFilePath;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Texture source not found: %s",
                            metadata.sourceFilePath.c_str());
            return false;
        }

        if (settings.textureSettings.format != AssetImportSettings::TextureSettings::AUTO &&
            settings.textureSettings.format != AssetImportSettings::TextureSettings::UNCOMPRESSED)
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Requested offline texture compression is not supported";
            return false;
        }
        if (settings.textureSettings.generateMipMaps)
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Requested offline mip-map generation is not supported";
            return false;
        }

        if (progressCallback)
        {
            progressCallback(0.2f);
        }

        // Determine output path
        fs::path outputPath;
        if (!settings.outputDirectory.empty())
        {
            outputPath = fs::path(settings.outputDirectory) / sourcePath.filename();
        }
        else
        {
            outputPath = sourcePath;
        }

        // Copy source to processed location
        if (outputPath != sourcePath)
        {
            std::error_code ec;
            fs::create_directories(outputPath.parent_path(), ec);
            fs::copy_file(sourcePath, outputPath, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Failed to copy file: " + ec.message();
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.5f);
        }

        // Compress texture if needed
        if (settings.textureSettings.format != AssetImportSettings::TextureSettings::UNCOMPRESSED &&
            settings.textureSettings.format != AssetImportSettings::TextureSettings::AUTO)
        {
            if (!CompressTexture(sourcePath.string(), outputPath.string(), settings.textureSettings))
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Texture compression failed";
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.7f);
        }

        // Generate mip maps if requested
        if (settings.textureSettings.generateMipMaps)
        {
            if (!GenerateMipMaps(outputPath.string()))
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Mip-map generation failed";
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.9f);
        }

        // Update metadata
        metadata.processedFilePath = outputPath.string();
        metadata.sourceFileSize = fs::file_size(sourcePath);
        metadata.processedFileSize = fs::file_size(outputPath);
        metadata.checksum = ComputeFileChecksum(outputPath.string());
        metadata.processedTime = std::chrono::system_clock::now();
        metadata.sourceModifiedTime = ToSystemClock(fs::last_write_time(sourcePath));
        metadata.status = ProcessingStatus::COMPLETED;
        metadata.processorName = GetName();
        metadata.type = AssetType::TEXTURE;

        if (progressCallback)
        {
            progressCallback(1.0f);
        }

        return true;
    }

    bool TextureProcessor::GenerateThumbnail(const AssetMetadata& /*metadata*/, int /*thumbnailSize*/)
    {
        // Thumbnail generation requires GPU access which is unavailable here.
        return false;
    }

    bool TextureProcessor::Validate(const AssetMetadata& metadata)
    {
        const fs::path sourcePath(metadata.sourceFilePath);
        if (!fs::exists(sourcePath))
        {
            return false;
        }

        const auto fileSize = fs::file_size(sourcePath);
        if (fileSize == 0)
        {
            return false;
        }

        return true;
    }

    bool TextureProcessor::CompressTexture(const std::string& inputPath, const std::string& outputPath,
                                           const AssetImportSettings::TextureSettings& /*settings*/)
    {
        // Real GPU-based compression is not available in the editor pipeline.
        // Copy file as a pass-through operation.
        if (inputPath == outputPath)
        {
            return true;
        }

        std::error_code ec;
        fs::copy_file(inputPath, outputPath, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    bool TextureProcessor::GenerateMipMaps(const std::string& /*texturePath*/)
    {
        // Mip map generation is performed at runtime by the graphics engine.
        return true;
    }

    // =========================================================================
    // MeshProcessor
    // =========================================================================

    std::vector<std::string> MeshProcessor::GetSupportedExtensions() const
    {
        return {".obj", ".fbx", ".gltf", ".glb", ".dae"};
    }

    bool MeshProcessor::Process(AssetMetadata& metadata, const AssetImportSettings& settings,
                                std::function<void(float)> progressCallback)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Processing mesh: %s", metadata.sourceFilePath.c_str());
        if (progressCallback)
        {
            progressCallback(0.0f);
        }

        const fs::path sourcePath(metadata.sourceFilePath);
        if (!fs::exists(sourcePath))
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Source file does not exist: " + metadata.sourceFilePath;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Mesh source not found: %s", metadata.sourceFilePath.c_str());
            return false;
        }

        if (settings.meshSettings.optimizeMesh || settings.meshSettings.weldVertices ||
            settings.meshSettings.generateNormals || settings.meshSettings.generateTangents ||
            settings.meshSettings.generateLightmapUVs)
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Requested offline mesh transforms are not supported";
            return false;
        }

        if (progressCallback)
        {
            progressCallback(0.1f);
        }

        // Determine output path
        fs::path outputPath;
        if (!settings.outputDirectory.empty())
        {
            outputPath = fs::path(settings.outputDirectory) / sourcePath.filename();
        }
        else
        {
            outputPath = sourcePath;
        }

        // Copy source to processed location
        if (outputPath != sourcePath)
        {
            std::error_code ec;
            fs::create_directories(outputPath.parent_path(), ec);
            fs::copy_file(sourcePath, outputPath, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Failed to copy file: " + ec.message();
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.3f);
        }

        // Apply mesh processing steps
        if (settings.meshSettings.optimizeMesh)
        {
            if (!OptimizeMesh(outputPath.string(), settings.meshSettings))
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Mesh optimization failed";
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.5f);
        }

        if (settings.meshSettings.generateNormals)
        {
            if (!GenerateNormals(outputPath.string(), settings.meshSettings.normalSmoothingAngle))
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Normal generation failed";
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.6f);
        }

        if (settings.meshSettings.generateTangents)
        {
            if (!GenerateTangents(outputPath.string()))
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Tangent generation failed";
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.7f);
        }

        if (settings.meshSettings.generateLightmapUVs)
        {
            if (!GenerateLightmapUVs(outputPath.string()))
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Lightmap UV generation failed";
                return false;
            }
        }

        if (settings.meshSettings.autoGenerateLODs)
        {
            if (!GenerateAutoLODs(metadata, settings.meshSettings))
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Automatic LOD generation failed or is unsupported for this format";
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.9f);
        }

        // Update metadata
        metadata.processedFilePath = outputPath.string();
        metadata.sourceFileSize = fs::file_size(sourcePath);
        metadata.processedFileSize = fs::file_size(outputPath);
        metadata.processedTime = std::chrono::system_clock::now();
        metadata.sourceModifiedTime = ToSystemClock(fs::last_write_time(sourcePath));
        metadata.status = ProcessingStatus::COMPLETED;
        metadata.processorName = GetName();
        metadata.type = AssetType::MESH;

        if (progressCallback)
        {
            progressCallback(1.0f);
        }

        return true;
    }

    bool MeshProcessor::GenerateThumbnail(const AssetMetadata& /*metadata*/, int /*thumbnailSize*/)
    {
        // Mesh thumbnail generation requires a 3D renderer, handled at runtime.
        return false;
    }

    bool MeshProcessor::Validate(const AssetMetadata& metadata)
    {
        return fs::exists(metadata.sourceFilePath);
    }

    bool MeshProcessor::OptimizeMesh(const std::string& /*meshPath*/,
                                     const AssetImportSettings::MeshSettings& /*settings*/)
    {
        // Mesh optimization (vertex reordering, cache optimization) is handled by the runtime.
        return true;
    }

    bool MeshProcessor::GenerateNormals(const std::string& /*meshPath*/, float /*smoothingAngle*/)
    {
        return true;
    }

    bool MeshProcessor::GenerateTangents(const std::string& /*meshPath*/)
    {
        return true;
    }

    bool MeshProcessor::GenerateLightmapUVs(const std::string& /*meshPath*/)
    {
        return true;
    }

    bool MeshProcessor::GenerateAutoLODs(AssetMetadata& metadata, const AssetImportSettings::MeshSettings& settings)
    {
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        if (!LoadOBJMeshForLOD(metadata.processedFilePath.empty() ? metadata.sourceFilePath
                                                                  : metadata.processedFilePath,
                               positions, indices))
        {
            metadata.customData["lod.status"] = "skipped";
            metadata.customData["lod.reason"] = "format_not_supported_for_offline_lod";
            return false;
        }

        const LODProfileDefinition profile = GetLODProfile(settings.lodPlatform, settings.lodQuality);
        Spark::Graphics::LODGenerationOptions options{};
        options.lodCount = static_cast<uint32_t>(profile.triangleTargets.size());
        options.maxError = profile.errorThresholds.size() > 1 ? profile.errorThresholds[1] : 0.01f;
        options.screenSizeThresholds = profile.screenThresholds;
        options.reductionPerLevel = profile.triangleTargets.size() > 1 ? profile.triangleTargets[1] : 0.75f;

        auto result = Spark::Graphics::LODGenerator::GetInstance().Generate(
            positions.data(), static_cast<uint32_t>(positions.size() / 3), indices.data(),
            static_cast<uint32_t>(indices.size()), options);
        if (result.levels.empty())
        {
            metadata.customData["lod.status"] = "failed";
            metadata.customData["lod.reason"] = "generation_failed";
            return false;
        }

        std::ostringstream triCounts;
        std::ostringstream errors;
        for (size_t i = 0; i < result.levels.size(); ++i)
        {
            if (i > 0)
            {
                triCounts << ",";
                errors << ",";
            }
            triCounts << result.levels[i].triangleCount;
            errors << result.levels[i].geometricError;
        }

        std::ostringstream profileSeed;
        profileSeed << ToString(settings.lodPlatform) << "|" << ToString(settings.lodQuality) << "|"
                    << metadata.checksum << "|lod_meta_v2";

        metadata.customData["lod.status"] = "generated";
        metadata.customData["lod.meta_version"] = "2";
        metadata.customData["lod.profile_platform"] = ToString(settings.lodPlatform);
        metadata.customData["lod.profile_quality"] = ToString(settings.lodQuality);
        metadata.customData["lod.level_count"] = std::to_string(result.levels.size());
        metadata.customData["lod.level0_triangles"] = std::to_string(result.levels.front().triangleCount);
        metadata.customData["lod.triangle_counts"] = triCounts.str();
        metadata.customData["lod.geometric_errors"] = errors.str();
        metadata.customData["lod.total_reduction"] = std::to_string(result.totalReduction);
        metadata.customData["lod.rebuild_fingerprint"] = std::to_string(std::hash<std::string>{}(profileSeed.str()));
        return true;
    }

    // =========================================================================
    // AudioProcessor
    // =========================================================================

    std::vector<std::string> AudioProcessor::GetSupportedExtensions() const
    {
        return {".wav", ".ogg", ".mp3", ".flac"};
    }

    bool AudioProcessor::Process(AssetMetadata& metadata, const AssetImportSettings& settings,
                                 std::function<void(float)> progressCallback)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Processing audio: %s", metadata.sourceFilePath.c_str());
        if (progressCallback)
        {
            progressCallback(0.0f);
        }

        const fs::path sourcePath(metadata.sourceFilePath);
        if (!fs::exists(sourcePath))
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Source file does not exist: " + metadata.sourceFilePath;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Audio source not found: %s", metadata.sourceFilePath.c_str());
            return false;
        }

        std::string requestedExtension;
        switch (settings.audioSettings.format)
        {
        case AssetImportSettings::AudioSettings::WAV:
            requestedExtension = ".wav";
            break;
        case AssetImportSettings::AudioSettings::OGG:
            requestedExtension = ".ogg";
            break;
        case AssetImportSettings::AudioSettings::MP3:
            requestedExtension = ".mp3";
            break;
        case AssetImportSettings::AudioSettings::AUTO:
            break;
        }
        std::string sourceExtension = sourcePath.extension().string();
        std::transform(sourceExtension.begin(), sourceExtension.end(), sourceExtension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!requestedExtension.empty() && requestedExtension != sourceExtension)
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Requested offline audio format conversion is not supported";
            return false;
        }

        if (progressCallback)
        {
            progressCallback(0.2f);
        }

        // Determine output path
        fs::path outputPath;
        if (!settings.outputDirectory.empty())
        {
            outputPath = fs::path(settings.outputDirectory) / sourcePath.filename();
        }
        else
        {
            outputPath = sourcePath;
        }

        // Copy source to processed location
        if (outputPath != sourcePath)
        {
            std::error_code ec;
            fs::create_directories(outputPath.parent_path(), ec);
            fs::copy_file(sourcePath, outputPath, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                metadata.status = ProcessingStatus::FAILED;
                metadata.errorMessage = "Failed to copy file: " + ec.message();
                return false;
            }
        }

        if (progressCallback)
        {
            progressCallback(0.5f);
        }

        // Convert audio format if needed
        if (!ConvertAudio(sourcePath.string(), outputPath.string(), settings.audioSettings))
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Audio conversion failed";
            return false;
        }

        if (progressCallback)
        {
            progressCallback(0.7f);
        }

        // Analyze audio and extract metadata
        if (!AnalyzeAudio(outputPath.string(), metadata))
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Audio analysis failed";
            return false;
        }

        if (progressCallback)
        {
            progressCallback(0.9f);
        }

        // Update metadata
        metadata.processedFilePath = outputPath.string();
        metadata.sourceFileSize = fs::file_size(sourcePath);
        metadata.processedFileSize = fs::file_size(outputPath);
        metadata.processedTime = std::chrono::system_clock::now();
        metadata.sourceModifiedTime = ToSystemClock(fs::last_write_time(sourcePath));
        metadata.status = ProcessingStatus::COMPLETED;
        metadata.processorName = GetName();
        metadata.type = AssetType::AUDIO;

        if (progressCallback)
        {
            progressCallback(1.0f);
        }

        return true;
    }

    bool AudioProcessor::GenerateThumbnail(const AssetMetadata& /*metadata*/, int /*thumbnailSize*/)
    {
        // Audio waveform thumbnail generation is not supported in the pipeline.
        return false;
    }

    bool AudioProcessor::Validate(const AssetMetadata& metadata)
    {
        return fs::exists(metadata.sourceFilePath);
    }

    bool AudioProcessor::ConvertAudio(const std::string& inputPath, const std::string& outputPath,
                                      const AssetImportSettings::AudioSettings& /*settings*/)
    {
        // Full audio format conversion requires a codec library. Copy as pass-through.
        if (inputPath == outputPath)
        {
            return true;
        }

        std::error_code ec;
        fs::copy_file(inputPath, outputPath, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    bool AudioProcessor::AnalyzeAudio(const std::string& audioPath, AssetMetadata& metadata)
    {
        const fs::path path(audioPath);
        if (!fs::exists(path))
        {
            return false;
        }

        // Extract basic metadata from file properties
        metadata.customData["audioFileSize"] = std::to_string(fs::file_size(path));
        metadata.customData["audioFormat"] = path.extension().string();
        metadata.customData["audioFileName"] = path.filename().string();

        return true;
    }

    // =========================================================================
    // AssetDependencyGraph
    // =========================================================================

    void AssetDependencyGraph::AddAsset(const std::string& assetPath)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Adding asset to dependency graph: %s", assetPath.c_str());
        if (!Spark::ContainerUtils::Contains(m_dependencies, assetPath))
        {
            m_dependencies[assetPath] = {};
        }
        if (!Spark::ContainerUtils::Contains(m_dependents, assetPath))
        {
            m_dependents[assetPath] = {};
        }
    }

    void AssetDependencyGraph::RemoveAsset(const std::string& assetPath)
    {
        m_dependencies.erase(assetPath);
        m_dependents.erase(assetPath);

        // Remove this asset from all dependents lists
        for (auto& [asset, deps] : m_dependents)
        {
            deps.erase(std::remove(deps.begin(), deps.end(), assetPath), deps.end());
        }

        // Remove this asset from all dependencies lists
        for (auto& [asset, deps] : m_dependencies)
        {
            deps.erase(std::remove(deps.begin(), deps.end(), assetPath), deps.end());
        }
    }

    void AssetDependencyGraph::AddDependency(const std::string& dependent, const std::string& dependency)
    {
        auto& deps = m_dependencies[dependent];
        if (!Spark::ContainerUtils::Contains(deps, dependency))
        {
            deps.push_back(dependency);
        }

        auto& revDeps = m_dependents[dependency];
        if (!Spark::ContainerUtils::Contains(revDeps, dependent))
        {
            revDeps.push_back(dependent);
        }
    }

    void AssetDependencyGraph::RemoveDependency(const std::string& dependent, const std::string& dependency)
    {
        auto depsIt = m_dependencies.find(dependent);
        if (depsIt != m_dependencies.end())
        {
            auto& deps = depsIt->second;
            deps.erase(std::remove(deps.begin(), deps.end(), dependency), deps.end());
        }

        auto revIt = m_dependents.find(dependency);
        if (revIt != m_dependents.end())
        {
            auto& revDeps = revIt->second;
            revDeps.erase(std::remove(revDeps.begin(), revDeps.end(), dependent), revDeps.end());
        }
    }

    std::vector<std::string> AssetDependencyGraph::GetDependencies(const std::string& assetPath) const
    {
        auto it = m_dependencies.find(assetPath);
        if (it != m_dependencies.end())
        {
            return it->second;
        }
        return {};
    }

    std::vector<std::string> AssetDependencyGraph::GetDependents(const std::string& assetPath) const
    {
        auto it = m_dependents.find(assetPath);
        if (it != m_dependents.end())
        {
            return it->second;
        }
        return {};
    }

    std::vector<std::string> AssetDependencyGraph::GetProcessingOrder(const std::vector<std::string>& assetPaths) const
    {
        // Kahn's algorithm for topological sort
        std::unordered_set<std::string> assetSet(assetPaths.begin(), assetPaths.end());

        // Build in-degree map limited to the provided assets
        std::unordered_map<std::string, int> inDegree;
        std::unordered_map<std::string, std::vector<std::string>> localDeps;

        for (const auto& asset : assetPaths)
        {
            inDegree[asset] = 0;
            localDeps[asset] = {};
        }

        for (const auto& asset : assetPaths)
        {
            auto it = m_dependencies.find(asset);
            if (it != m_dependencies.end())
            {
                for (const auto& dep : it->second)
                {
                    if (assetSet.count(dep) > 0)
                    {
                        localDeps[asset].push_back(dep);
                        inDegree[asset]++;
                    }
                }
            }
        }

        // Start with nodes that have zero in-degree (no dependencies)
        std::queue<std::string> ready;
        for (const auto& asset : assetPaths)
        {
            if (inDegree[asset] == 0)
            {
                ready.push(asset);
            }
        }

        std::vector<std::string> result;
        result.reserve(assetPaths.size());

        while (!ready.empty())
        {
            std::string current = ready.front();
            ready.pop();
            result.push_back(current);

            // Find assets that depend on current
            auto depIt = m_dependents.find(current);
            if (depIt != m_dependents.end())
            {
                for (const auto& dependent : depIt->second)
                {
                    if (assetSet.count(dependent) > 0)
                    {
                        inDegree[dependent]--;
                        if (inDegree[dependent] == 0)
                        {
                            ready.push(dependent);
                        }
                    }
                }
            }
        }

        // If not all assets were processed, there is a cycle; add remaining in original order
        if (result.size() < assetPaths.size())
        {
            for (const auto& asset : assetPaths)
            {
                if (!Spark::ContainerUtils::Contains(result, asset))
                {
                    result.push_back(asset);
                }
            }
        }

        return result;
    }

    std::vector<std::string> AssetDependencyGraph::DetectCircularDependencies(
        const std::vector<std::string>& assetPaths) const
    {
        // DFS coloring: 0 = white (unvisited), 1 = gray (in progress), 2 = black (done)
        std::unordered_map<std::string, int> color;
        std::vector<std::string> cycleNodes;

        for (const auto& asset : assetPaths)
        {
            color[asset] = 0;
        }

        std::function<bool(const std::string&)> dfs = [&](const std::string& node) -> bool
        {
            color[node] = 1; // gray

            auto it = m_dependencies.find(node);
            if (it != m_dependencies.end())
            {
                for (const auto& dep : it->second)
                {
                    auto colorIt = color.find(dep);
                    if (colorIt == color.end())
                    {
                        continue; // Not in our set
                    }

                    if (colorIt->second == 1)
                    {
                        // Back edge found - cycle detected
                        cycleNodes.push_back(dep);
                        cycleNodes.push_back(node);
                        return true;
                    }

                    if (colorIt->second == 0)
                    {
                        if (dfs(dep))
                        {
                            cycleNodes.push_back(node);
                            return true;
                        }
                    }
                }
            }

            color[node] = 2; // black
            return false;
        };

        for (const auto& asset : assetPaths)
        {
            if (color[asset] == 0)
            {
                dfs(asset);
            }
        }

        // Remove duplicates
        std::sort(cycleNodes.begin(), cycleNodes.end());
        cycleNodes.erase(std::unique(cycleNodes.begin(), cycleNodes.end()), cycleNodes.end());

        return cycleNodes;
    }

    std::vector<std::string> AssetDependencyGraph::GetAffectedAssets(const std::string& assetPath) const
    {
        // BFS through the dependents graph
        std::vector<std::string> affected;
        std::unordered_set<std::string> visited;
        std::queue<std::string> toVisit;

        toVisit.push(assetPath);
        visited.insert(assetPath);

        while (!toVisit.empty())
        {
            std::string current = toVisit.front();
            toVisit.pop();

            auto it = m_dependents.find(current);
            if (it != m_dependents.end())
            {
                for (const auto& dependent : it->second)
                {
                    if (visited.insert(dependent).second)
                    {
                        affected.push_back(dependent);
                        toVisit.push(dependent);
                    }
                }
            }
        }

        return affected;
    }


} // namespace SparkEditor
