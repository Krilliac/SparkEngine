/**
 * @file AssetProcessors.cpp
 * @brief Asset processor implementations (Texture, Mesh, Audio, DependencyGraph)
 *
 * Split from AdvancedAssetPipeline.cpp for maintainability.
 */

#include "AdvancedAssetPipeline.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace SparkEditor
{

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
        if (progressCallback)
        {
            progressCallback(0.0f);
        }

        const fs::path sourcePath(metadata.sourceFilePath);
        if (!fs::exists(sourcePath))
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Source file does not exist: " + metadata.sourceFilePath;
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
        if (settings.textureSettings.format != AssetImportSettings::TextureSettings::UNCOMPRESSED)
        {
            CompressTexture(sourcePath.string(), outputPath.string(), settings.textureSettings);
        }

        if (progressCallback)
        {
            progressCallback(0.7f);
        }

        // Generate mip maps if requested
        if (settings.textureSettings.generateMipMaps)
        {
            GenerateMipMaps(outputPath.string());
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
        metadata.sourceModifiedTime =
            std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(sourcePath));
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
        // Thumbnail generation requires GPU access which is not available in the editor pipeline.
        // Return true to indicate no error; thumbnails are generated at runtime.
        return true;
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
        if (progressCallback)
        {
            progressCallback(0.0f);
        }

        const fs::path sourcePath(metadata.sourceFilePath);
        if (!fs::exists(sourcePath))
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Source file does not exist: " + metadata.sourceFilePath;
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
            OptimizeMesh(outputPath.string(), settings.meshSettings);
        }

        if (progressCallback)
        {
            progressCallback(0.5f);
        }

        if (settings.meshSettings.generateNormals)
        {
            GenerateNormals(outputPath.string(), settings.meshSettings.normalSmoothingAngle);
        }

        if (progressCallback)
        {
            progressCallback(0.6f);
        }

        if (settings.meshSettings.generateTangents)
        {
            GenerateTangents(outputPath.string());
        }

        if (progressCallback)
        {
            progressCallback(0.7f);
        }

        if (settings.meshSettings.generateLightmapUVs)
        {
            GenerateLightmapUVs(outputPath.string());
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
        metadata.sourceModifiedTime =
            std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(sourcePath));
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
        return true;
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
        if (progressCallback)
        {
            progressCallback(0.0f);
        }

        const fs::path sourcePath(metadata.sourceFilePath);
        if (!fs::exists(sourcePath))
        {
            metadata.status = ProcessingStatus::FAILED;
            metadata.errorMessage = "Source file does not exist: " + metadata.sourceFilePath;
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
        ConvertAudio(sourcePath.string(), outputPath.string(), settings.audioSettings);

        if (progressCallback)
        {
            progressCallback(0.7f);
        }

        // Analyze audio and extract metadata
        AnalyzeAudio(outputPath.string(), metadata);

        if (progressCallback)
        {
            progressCallback(0.9f);
        }

        // Update metadata
        metadata.processedFilePath = outputPath.string();
        metadata.sourceFileSize = fs::file_size(sourcePath);
        metadata.processedFileSize = fs::file_size(outputPath);
        metadata.processedTime = std::chrono::system_clock::now();
        metadata.sourceModifiedTime =
            std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(sourcePath));
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
        return true;
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
        if (m_dependencies.find(assetPath) == m_dependencies.end())
        {
            m_dependencies[assetPath] = {};
        }
        if (m_dependents.find(assetPath) == m_dependents.end())
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
        if (std::find(deps.begin(), deps.end(), dependency) == deps.end())
        {
            deps.push_back(dependency);
        }

        auto& revDeps = m_dependents[dependency];
        if (std::find(revDeps.begin(), revDeps.end(), dependent) == revDeps.end())
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
                if (std::find(result.begin(), result.end(), asset) == result.end())
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
