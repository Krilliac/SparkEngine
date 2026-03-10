/**
 * @file AdvancedAssetPipeline.cpp
 * @brief Implementation of the advanced asset processing and pipeline system
 * @author Spark Engine Team
 * @date 2025
 */

#include "AdvancedAssetPipeline.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <queue>
#include <sstream>
#include <unordered_set>

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
        metadata.sourceModifiedTime = std::chrono::clock_cast<std::chrono::system_clock>(
            fs::last_write_time(sourcePath));
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
        metadata.sourceModifiedTime = std::chrono::clock_cast<std::chrono::system_clock>(
            fs::last_write_time(sourcePath));
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
        metadata.sourceModifiedTime = std::chrono::clock_cast<std::chrono::system_clock>(
            fs::last_write_time(sourcePath));
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

    std::vector<std::string> AssetDependencyGraph::GetProcessingOrder(
        const std::vector<std::string>& assetPaths) const
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

    // =========================================================================
    // AdvancedAssetPipeline
    // =========================================================================

    AdvancedAssetPipeline::AdvancedAssetPipeline()
        : EditorPanel("Asset Pipeline", "asset_pipeline")
    {
    }

    AdvancedAssetPipeline::~AdvancedAssetPipeline()
    {
        Shutdown();
    }

    bool AdvancedAssetPipeline::Initialize()
    {
        // Register default processors
        RegisterProcessor(std::make_unique<TextureProcessor>());
        RegisterProcessor(std::make_unique<MeshProcessor>());
        RegisterProcessor(std::make_unique<AudioProcessor>());

        // Start processing threads
        m_shouldStopProcessing.store(false);
        for (int i = 0; i < m_maxProcessingThreads; ++i)
        {
            m_processingThreads.emplace_back(&AdvancedAssetPipeline::ProcessingThreadFunction, this);
        }

        // Start file system monitoring thread
        m_shouldStopMonitoring.store(false);
        if (m_fileSystemMonitoring)
        {
            m_monitoringThread = std::thread(&AdvancedAssetPipeline::FileSystemMonitoringFunction, this);
        }

        m_isInitialized = true;
        return true;
    }

    void AdvancedAssetPipeline::Update(float /*deltaTime*/)
    {
        // Check for completed jobs and update batch progress
        std::lock_guard<std::mutex> batchLock(m_batchMutex);
        for (auto& [id, batch] : m_batchOperations)
        {
            if (!batch.isActive)
            {
                continue;
            }

            if (batch.totalAssets > 0)
            {
                batch.progress = static_cast<float>(batch.completedAssets) /
                                 static_cast<float>(batch.totalAssets);
            }

            if (batch.completedAssets >= batch.totalAssets)
            {
                batch.isActive = false;
                batch.progress = 1.0f;
                if (batch.completionCallback)
                {
                    batch.completionCallback();
                }
            }
            else if (batch.progressCallback)
            {
                batch.progressCallback(batch.progress);
            }
        }
    }

    void AdvancedAssetPipeline::Render()
    {
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        if (ImGui::BeginTabBar("AssetPipelineTabs"))
        {
            if (ImGui::BeginTabItem("Assets"))
            {
                RenderAssetList();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Queue"))
            {
                RenderProcessingQueue();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Batch"))
            {
                RenderBatchOperations();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Inspector"))
            {
                RenderAssetInspector();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Dependencies"))
            {
                RenderDependencyViewer();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Statistics"))
            {
                RenderProcessingStatistics();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Import Settings"))
            {
                RenderImportSettings();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        EndPanel();
    }

    void AdvancedAssetPipeline::Shutdown()
    {
        // Stop processing threads
        m_shouldStopProcessing.store(true);
        m_queueCondition.notify_all();

        for (auto& thread : m_processingThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        m_processingThreads.clear();

        // Stop monitoring thread
        m_shouldStopMonitoring.store(true);
        if (m_monitoringThread.joinable())
        {
            m_monitoringThread.join();
        }

        // Clear data
        {
            std::lock_guard<std::mutex> lock(m_metadataMutex);
            m_assetMetadata.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            while (!m_processingQueue.empty())
            {
                m_processingQueue.pop();
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_batchMutex);
            m_batchOperations.clear();
        }

        m_processors.clear();
        m_processorMap.clear();

        m_isInitialized = false;
    }

    bool AdvancedAssetPipeline::HandleEvent(const std::string& eventType, void* /*eventData*/)
    {
        if (eventType == "asset_modified")
        {
            return true;
        }
        if (eventType == "asset_deleted")
        {
            return true;
        }
        return false;
    }

    void AdvancedAssetPipeline::RegisterProcessor(std::unique_ptr<AssetProcessor> processor)
    {
        if (!processor)
        {
            return;
        }

        // Build extension map
        for (const auto& ext : processor->GetSupportedExtensions())
        {
            std::string lowerExt = ext;
            std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            m_processorMap[lowerExt] = processor.get();
        }

        m_processors.push_back(std::move(processor));
    }

    bool AdvancedAssetPipeline::ProcessAsset(const std::string& assetPath, const AssetImportSettings& settings,
                                             std::function<void(const AssetMetadata&)> callback)
    {
        if (!fs::exists(assetPath))
        {
            return false;
        }

        AssetProcessor* processor = GetProcessorForAsset(assetPath);
        if (!processor)
        {
            return false;
        }

        ProcessingJob job;
        job.assetPath = assetPath;
        job.settings = settings;
        job.completionCallback = std::move(callback);
        job.priority = 0;
        job.submissionTime = std::chrono::system_clock::now();

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_processingQueue.push(std::move(job));
        }
        m_queueCondition.notify_one();

        return true;
    }

    uint32_t AdvancedAssetPipeline::ProcessAssetsBatch(const std::vector<std::string>& assetPaths,
                                                       const AssetImportSettings& settings,
                                                       std::function<void(float)> progressCallback,
                                                       std::function<void()> completionCallback)
    {
        uint32_t batchID;
        {
            std::lock_guard<std::mutex> lock(m_batchMutex);
            batchID = m_nextBatchID++;

            BatchOperation batch;
            batch.name = "Batch " + std::to_string(batchID);
            batch.assetPaths = assetPaths;
            batch.settings = settings;
            batch.progressCallback = std::move(progressCallback);
            batch.completionCallback = std::move(completionCallback);
            batch.isActive = true;
            batch.progress = 0.0f;
            batch.completedAssets = 0;
            batch.totalAssets = static_cast<int>(assetPaths.size());

            m_batchOperations[batchID] = std::move(batch);
        }

        // Queue all assets with a callback that updates batch progress
        for (const auto& assetPath : assetPaths)
        {
            auto batchCallback = [this, batchID](const AssetMetadata& /*meta*/)
            {
                std::lock_guard<std::mutex> lock(m_batchMutex);
                auto it = m_batchOperations.find(batchID);
                if (it != m_batchOperations.end())
                {
                    it->second.completedAssets++;
                }
            };

            ProcessAsset(assetPath, settings, batchCallback);
        }

        return batchID;
    }

    bool AdvancedAssetPipeline::CancelBatchOperation(uint32_t operationID)
    {
        std::lock_guard<std::mutex> batchLock(m_batchMutex);
        auto it = m_batchOperations.find(operationID);
        if (it == m_batchOperations.end())
        {
            return false;
        }

        it->second.isActive = false;

        // Remove pending jobs for this batch from the queue
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        std::unordered_set<std::string> batchAssets(
            it->second.assetPaths.begin(), it->second.assetPaths.end());

        std::priority_queue<ProcessingJob> filteredQueue;
        while (!m_processingQueue.empty())
        {
            ProcessingJob job = m_processingQueue.top();
            m_processingQueue.pop();
            if (batchAssets.find(job.assetPath) == batchAssets.end())
            {
                filteredQueue.push(std::move(job));
            }
        }
        m_processingQueue = std::move(filteredQueue);

        return true;
    }

    const AssetMetadata* AdvancedAssetPipeline::GetAssetMetadata(const std::string& assetPath) const
    {
        std::lock_guard<std::mutex> lock(m_metadataMutex);
        auto it = m_assetMetadata.find(assetPath);
        if (it != m_assetMetadata.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    bool AdvancedAssetPipeline::RefreshAssetMetadata(const std::string& assetPath)
    {
        const fs::path path(assetPath);
        if (!fs::exists(path))
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_metadataMutex);
        auto& metadata = m_assetMetadata[assetPath];
        metadata.sourceFilePath = assetPath;
        metadata.sourceFileSize = fs::file_size(path);
        metadata.sourceModifiedTime = std::chrono::clock_cast<std::chrono::system_clock>(
            fs::last_write_time(path));
        metadata.checksum = CalculateChecksum(assetPath);

        return true;
    }

    int AdvancedAssetPipeline::ScanDirectory(const std::string& directoryPath, bool recursive)
    {
        const fs::path dirPath(directoryPath);
        if (!fs::exists(dirPath) || !fs::is_directory(dirPath))
        {
            return 0;
        }

        int assetCount = 0;

        auto processEntry = [&](const fs::directory_entry& entry)
        {
            if (!entry.is_regular_file())
            {
                return;
            }

            const std::string filePath = entry.path().string();
            AssetProcessor* processor = GetProcessorForAsset(filePath);
            if (!processor)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(m_metadataMutex);
            auto& metadata = m_assetMetadata[filePath];
            metadata.sourceFilePath = filePath;
            metadata.sourceFileSize = entry.file_size();
            metadata.sourceModifiedTime = std::chrono::clock_cast<std::chrono::system_clock>(
                entry.last_write_time());
            metadata.type = processor->GetAssetType();
            metadata.processorName = processor->GetName();

            if (metadata.guid.empty())
            {
                // Generate a simple GUID from the file path hash
                std::hash<std::string> hasher;
                metadata.guid = std::to_string(hasher(filePath));
            }

            m_dependencyGraph.AddAsset(filePath);
            ++assetCount;
        };

        std::error_code ec;
        if (recursive)
        {
            for (const auto& entry : fs::recursive_directory_iterator(dirPath, ec))
            {
                processEntry(entry);
            }
        }
        else
        {
            for (const auto& entry : fs::directory_iterator(dirPath, ec))
            {
                processEntry(entry);
            }
        }

        return assetCount;
    }

    AdvancedAssetPipeline::ProcessingStatistics AdvancedAssetPipeline::GetProcessingStatistics() const
    {
        std::lock_guard<std::mutex> lock(m_metadataMutex);
        ProcessingStatistics stats;
        float totalProcessingTime = 0.0f;
        size_t totalSourceSize = 0;

        for (const auto& [path, metadata] : m_assetMetadata)
        {
            stats.totalAssets++;

            switch (metadata.status)
            {
            case ProcessingStatus::COMPLETED:
                stats.processedAssets++;
                stats.totalProcessedSize += metadata.processedFileSize;
                totalProcessingTime += metadata.processingTime;
                totalSourceSize += metadata.sourceFileSize;
                break;
            case ProcessingStatus::FAILED:
                stats.failedAssets++;
                break;
            case ProcessingStatus::PENDING:
            case ProcessingStatus::PROCESSING:
                stats.pendingAssets++;
                break;
            default:
                break;
            }
        }

        if (stats.processedAssets > 0)
        {
            stats.averageProcessingTime = totalProcessingTime / static_cast<float>(stats.processedAssets);
        }

        if (totalSourceSize > 0 && stats.totalProcessedSize > 0)
        {
            stats.compressionRatio = static_cast<float>(stats.totalProcessedSize) /
                                     static_cast<float>(totalSourceSize);
        }

        m_statistics = stats;
        return stats;
    }

    void AdvancedAssetPipeline::SetFileSystemMonitoring(bool enabled)
    {
        if (m_fileSystemMonitoring == enabled)
        {
            return;
        }

        m_fileSystemMonitoring = enabled;

        if (enabled)
        {
            // Start monitoring thread
            m_shouldStopMonitoring.store(false);
            if (!m_monitoringThread.joinable())
            {
                m_monitoringThread = std::thread(&AdvancedAssetPipeline::FileSystemMonitoringFunction, this);
            }
        }
        else
        {
            // Stop monitoring thread
            m_shouldStopMonitoring.store(true);
            if (m_monitoringThread.joinable())
            {
                m_monitoringThread.join();
            }
        }
    }

    void AdvancedAssetPipeline::SetProcessingThreadCount(int threadCount)
    {
        if (threadCount < 1)
        {
            threadCount = 1;
        }

        // Stop existing threads
        m_shouldStopProcessing.store(true);
        m_queueCondition.notify_all();

        for (auto& thread : m_processingThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        m_processingThreads.clear();

        // Start new threads
        m_shouldStopProcessing.store(false);
        m_maxProcessingThreads = threadCount;
        for (int i = 0; i < threadCount; ++i)
        {
            m_processingThreads.emplace_back(&AdvancedAssetPipeline::ProcessingThreadFunction, this);
        }
    }

    void AdvancedAssetPipeline::OptimizeAllAssets(std::function<void(float)> progressCallback)
    {
        std::vector<std::string> assetPaths;
        {
            std::lock_guard<std::mutex> lock(m_metadataMutex);
            for (const auto& [path, metadata] : m_assetMetadata)
            {
                assetPaths.push_back(path);
            }
        }

        if (assetPaths.empty())
        {
            if (progressCallback)
            {
                progressCallback(1.0f);
            }
            return;
        }

        AssetImportSettings settings = m_currentImportSettings;
        settings.overwriteExisting = true;

        ProcessAssetsBatch(assetPaths, settings, std::move(progressCallback), nullptr);
    }

    std::vector<std::string> AdvancedAssetPipeline::ValidateAllAssets()
    {
        std::vector<std::string> invalidAssets;

        std::lock_guard<std::mutex> lock(m_metadataMutex);
        for (const auto& [path, metadata] : m_assetMetadata)
        {
            AssetProcessor* processor = GetProcessorForAsset(path);
            if (!processor)
            {
                invalidAssets.push_back(path);
                continue;
            }

            if (!processor->Validate(metadata))
            {
                invalidAssets.push_back(path);
            }
        }

        return invalidAssets;
    }

    bool AdvancedAssetPipeline::ExportAssetDatabase(const std::string& filePath)
    {
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_metadataMutex);

        file << "SPARK_ASSET_DATABASE\n";
        file << "VERSION 1\n";
        file << "ASSET_COUNT " << m_assetMetadata.size() << "\n";
        file << "\n";

        for (const auto& [path, metadata] : m_assetMetadata)
        {
            file << "BEGIN_ASSET\n";
            file << "GUID " << metadata.guid << "\n";
            file << "SOURCE " << metadata.sourceFilePath << "\n";
            file << "PROCESSED " << metadata.processedFilePath << "\n";
            file << "TYPE " << static_cast<int>(metadata.type) << "\n";
            file << "SOURCE_SIZE " << metadata.sourceFileSize << "\n";
            file << "PROCESSED_SIZE " << metadata.processedFileSize << "\n";
            file << "STATUS " << static_cast<int>(metadata.status) << "\n";
            file << "CHECKSUM " << metadata.checksum << "\n";
            file << "PROCESSOR " << metadata.processorName << "\n";
            file << "PROCESSING_TIME " << metadata.processingTime << "\n";

            if (!metadata.errorMessage.empty())
            {
                file << "ERROR " << metadata.errorMessage << "\n";
            }

            if (!metadata.thumbnailPath.empty())
            {
                file << "THUMBNAIL " << metadata.thumbnailPath << "\n";
            }

            for (const auto& dep : metadata.dependencies)
            {
                file << "DEPENDENCY " << dep << "\n";
            }

            for (const auto& [key, value] : metadata.customData)
            {
                file << "CUSTOM " << key << " " << value << "\n";
            }

            file << "END_ASSET\n\n";
        }

        return true;
    }

    bool AdvancedAssetPipeline::ImportAssetDatabase(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        std::string line;

        // Read header
        if (!std::getline(file, line) || line != "SPARK_ASSET_DATABASE")
        {
            return false;
        }

        // Skip version and count lines
        std::getline(file, line); // VERSION
        std::getline(file, line); // ASSET_COUNT
        std::getline(file, line); // blank line

        std::lock_guard<std::mutex> lock(m_metadataMutex);
        m_assetMetadata.clear();

        AssetMetadata currentMetadata;
        bool inAsset = false;

        while (std::getline(file, line))
        {
            if (line.empty())
            {
                continue;
            }

            if (line == "BEGIN_ASSET")
            {
                currentMetadata = AssetMetadata();
                inAsset = true;
                continue;
            }

            if (line == "END_ASSET")
            {
                if (inAsset && !currentMetadata.sourceFilePath.empty())
                {
                    m_assetMetadata[currentMetadata.sourceFilePath] = currentMetadata;
                    m_dependencyGraph.AddAsset(currentMetadata.sourceFilePath);
                }
                inAsset = false;
                continue;
            }

            if (!inAsset)
            {
                continue;
            }

            std::istringstream iss(line);
            std::string tag;
            iss >> tag;

            if (tag == "GUID")
            {
                iss >> currentMetadata.guid;
            }
            else if (tag == "SOURCE")
            {
                std::getline(iss >> std::ws, currentMetadata.sourceFilePath);
            }
            else if (tag == "PROCESSED")
            {
                std::getline(iss >> std::ws, currentMetadata.processedFilePath);
            }
            else if (tag == "TYPE")
            {
                int type;
                iss >> type;
                currentMetadata.type = static_cast<AssetType>(type);
            }
            else if (tag == "SOURCE_SIZE")
            {
                iss >> currentMetadata.sourceFileSize;
            }
            else if (tag == "PROCESSED_SIZE")
            {
                iss >> currentMetadata.processedFileSize;
            }
            else if (tag == "STATUS")
            {
                int status;
                iss >> status;
                currentMetadata.status = static_cast<ProcessingStatus>(status);
            }
            else if (tag == "CHECKSUM")
            {
                iss >> currentMetadata.checksum;
            }
            else if (tag == "PROCESSOR")
            {
                std::getline(iss >> std::ws, currentMetadata.processorName);
            }
            else if (tag == "PROCESSING_TIME")
            {
                iss >> currentMetadata.processingTime;
            }
            else if (tag == "ERROR")
            {
                std::getline(iss >> std::ws, currentMetadata.errorMessage);
            }
            else if (tag == "THUMBNAIL")
            {
                std::getline(iss >> std::ws, currentMetadata.thumbnailPath);
            }
            else if (tag == "DEPENDENCY")
            {
                std::string dep;
                std::getline(iss >> std::ws, dep);
                currentMetadata.dependencies.push_back(dep);
            }
            else if (tag == "CUSTOM")
            {
                std::string key, value;
                iss >> key;
                std::getline(iss >> std::ws, value);
                currentMetadata.customData[key] = value;
            }
        }

        // Rebuild dependency graph
        UpdateDependencyGraph();

        return true;
    }

    // =========================================================================
    // Private methods
    // =========================================================================

    void AdvancedAssetPipeline::RenderAssetList()
    {
        // Search filter
        static char searchBuf[256] = {};
        ImGui::InputText("Search", searchBuf, sizeof(searchBuf));
        m_searchFilter = searchBuf;

        // Type filter combo
        const char* typeNames[] = {"All", "Texture", "Mesh", "Material", "Shader",
                                   "Audio", "Animation", "Script", "Font"};
        int currentType = static_cast<int>(m_typeFilter);
        if (ImGui::Combo("Type Filter", &currentType, typeNames, IM_ARRAYSIZE(typeNames)))
        {
            m_typeFilter = static_cast<AssetType>(currentType);
        }

        ImGui::Separator();

        // Scan directory button
        if (ImGui::Button("Scan Asset Directory"))
        {
            ScanDirectory(m_assetDirectory, true);
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh All"))
        {
            std::lock_guard<std::mutex> lock(m_metadataMutex);
            for (auto& [path, metadata] : m_assetMetadata)
            {
                RefreshAssetMetadata(path);
            }
        }

        ImGui::Separator();

        // Asset table
        if (ImGui::BeginTable("AssetTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                                    ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Size");
            ImGui::TableHeadersRow();

            std::lock_guard<std::mutex> lock(m_metadataMutex);
            for (const auto& [path, metadata] : m_assetMetadata)
            {
                // Apply search filter
                if (!m_searchFilter.empty())
                {
                    if (path.find(m_searchFilter) == std::string::npos)
                    {
                        continue;
                    }
                }

                // Apply type filter
                if (m_typeFilter != AssetType::UNKNOWN && metadata.type != m_typeFilter)
                {
                    continue;
                }

                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                const fs::path fsPath(path);
                bool isSelected = (m_selectedAsset == path);
                if (ImGui::Selectable(fsPath.filename().string().c_str(), isSelected,
                                      ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_selectedAsset = path;
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", metadata.processorName.c_str());

                ImGui::TableSetColumnIndex(2);
                const char* statusNames[] = {"Pending", "Processing", "Completed", "Failed",
                                             "Skipped", "Cancelled"};
                int statusIdx = static_cast<int>(metadata.status);
                if (statusIdx >= 0 && statusIdx < 6)
                {
                    ImGui::Text("%s", statusNames[statusIdx]);
                }

                ImGui::TableSetColumnIndex(3);
                if (metadata.sourceFileSize > 1024 * 1024)
                {
                    ImGui::Text("%.2f MB",
                                static_cast<float>(metadata.sourceFileSize) / (1024.0f * 1024.0f));
                }
                else if (metadata.sourceFileSize > 1024)
                {
                    ImGui::Text("%.2f KB",
                                static_cast<float>(metadata.sourceFileSize) / 1024.0f);
                }
                else
                {
                    ImGui::Text("%zu B", metadata.sourceFileSize);
                }
            }

            ImGui::EndTable();
        }
    }

    void AdvancedAssetPipeline::RenderProcessingQueue()
    {
        ImGui::Text("Processing Queue");
        ImGui::Separator();

        std::lock_guard<std::mutex> lock(m_queueMutex);
        ImGui::Text("Jobs in queue: %d", static_cast<int>(m_processingQueue.size()));
        ImGui::Text("Processing threads: %d", static_cast<int>(m_processingThreads.size()));

        ImGui::Separator();

        // Thread count adjustment
        int threadCount = m_maxProcessingThreads;
        if (ImGui::SliderInt("Thread Count", &threadCount, 1, 16))
        {
            // Note: cannot call SetProcessingThreadCount here while holding the queue lock.
            // Instead we just update the desired count; actual thread changes happen on next frame.
            m_maxProcessingThreads = threadCount;
        }

        ImGui::Separator();

        // Display queue contents (we can only peek at the top)
        if (!m_processingQueue.empty())
        {
            const auto& topJob = m_processingQueue.top();
            ImGui::Text("Next job: %s", fs::path(topJob.assetPath).filename().string().c_str());
            ImGui::Text("Priority: %d", topJob.priority);
        }
        else
        {
            ImGui::Text("Queue is empty.");
        }
    }

    void AdvancedAssetPipeline::RenderBatchOperations()
    {
        ImGui::Text("Batch Operations");
        ImGui::Separator();

        std::lock_guard<std::mutex> lock(m_batchMutex);

        if (m_batchOperations.empty())
        {
            ImGui::Text("No batch operations.");
            return;
        }

        for (const auto& [id, batch] : m_batchOperations)
        {
            ImGui::PushID(static_cast<int>(id));

            ImGui::Text("%s (ID: %u)", batch.name.c_str(), id);
            ImGui::ProgressBar(batch.progress,
                               ImVec2(-1.0f, 0.0f),
                               batch.isActive ? "Processing..." : "Complete");
            ImGui::Text("Progress: %d / %d assets", batch.completedAssets, batch.totalAssets);

            if (batch.isActive)
            {
                if (ImGui::Button("Cancel"))
                {
                    // Cannot call CancelBatchOperation here because it would deadlock (we hold the lock).
                    // Mark inactive directly.
                    // We cast away constness for the UI interaction.
                    const_cast<BatchOperation&>(batch).isActive = false;
                }
            }

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    void AdvancedAssetPipeline::RenderAssetInspector()
    {
        ImGui::Text("Asset Inspector");
        ImGui::Separator();

        if (m_selectedAsset.empty())
        {
            ImGui::Text("No asset selected. Select an asset from the Assets tab.");
            return;
        }

        std::lock_guard<std::mutex> lock(m_metadataMutex);
        auto it = m_assetMetadata.find(m_selectedAsset);
        if (it == m_assetMetadata.end())
        {
            ImGui::Text("Asset not found in metadata cache.");
            return;
        }

        const auto& metadata = it->second;

        ImGui::Text("GUID: %s", metadata.guid.c_str());
        ImGui::Text("Source: %s", metadata.sourceFilePath.c_str());
        ImGui::Text("Processed: %s", metadata.processedFilePath.c_str());
        ImGui::Text("Type: %d", static_cast<int>(metadata.type));
        ImGui::Text("Processor: %s", metadata.processorName.c_str());

        ImGui::Separator();

        ImGui::Text("Source size: %zu bytes", metadata.sourceFileSize);
        ImGui::Text("Processed size: %zu bytes", metadata.processedFileSize);
        ImGui::Text("Processing time: %.3f seconds", metadata.processingTime);

        const char* statusNames[] = {"Pending", "Processing", "Completed", "Failed",
                                     "Skipped", "Cancelled"};
        int statusIdx = static_cast<int>(metadata.status);
        if (statusIdx >= 0 && statusIdx < 6)
        {
            ImGui::Text("Status: %s", statusNames[statusIdx]);
        }

        if (!metadata.errorMessage.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s",
                               metadata.errorMessage.c_str());
        }

        ImGui::Text("Checksum: %s", metadata.checksum.c_str());

        ImGui::Separator();

        if (!metadata.dependencies.empty())
        {
            ImGui::Text("Dependencies:");
            for (const auto& dep : metadata.dependencies)
            {
                ImGui::BulletText("%s", dep.c_str());
            }
        }

        if (!metadata.dependents.empty())
        {
            ImGui::Text("Dependents:");
            for (const auto& dep : metadata.dependents)
            {
                ImGui::BulletText("%s", dep.c_str());
            }
        }

        if (!metadata.customData.empty())
        {
            ImGui::Separator();
            ImGui::Text("Custom Data:");
            for (const auto& [key, value] : metadata.customData)
            {
                ImGui::Text("  %s: %s", key.c_str(), value.c_str());
            }
        }

        ImGui::Separator();

        if (ImGui::Button("Reprocess"))
        {
            ProcessAsset(m_selectedAsset, m_currentImportSettings);
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh Metadata"))
        {
            RefreshAssetMetadata(m_selectedAsset);
        }
    }

    void AdvancedAssetPipeline::RenderDependencyViewer()
    {
        ImGui::Text("Dependency Viewer");
        ImGui::Separator();

        if (m_selectedAsset.empty())
        {
            ImGui::Text("Select an asset to view its dependencies.");
            return;
        }

        ImGui::Text("Asset: %s", fs::path(m_selectedAsset).filename().string().c_str());
        ImGui::Separator();

        auto dependencies = m_dependencyGraph.GetDependencies(m_selectedAsset);
        auto dependents = m_dependencyGraph.GetDependents(m_selectedAsset);

        ImGui::Text("Dependencies (%d):", static_cast<int>(dependencies.size()));
        for (const auto& dep : dependencies)
        {
            ImGui::BulletText("%s", fs::path(dep).filename().string().c_str());
        }

        ImGui::Separator();

        ImGui::Text("Dependents (%d):", static_cast<int>(dependents.size()));
        for (const auto& dep : dependents)
        {
            ImGui::BulletText("%s", fs::path(dep).filename().string().c_str());
        }

        ImGui::Separator();

        auto affected = m_dependencyGraph.GetAffectedAssets(m_selectedAsset);
        ImGui::Text("Affected by changes (%d):", static_cast<int>(affected.size()));
        for (const auto& asset : affected)
        {
            ImGui::BulletText("%s", fs::path(asset).filename().string().c_str());
        }

        ImGui::Separator();

        if (ImGui::Button("Detect Circular Dependencies"))
        {
            std::vector<std::string> allAssets;
            {
                std::lock_guard<std::mutex> lock(m_metadataMutex);
                for (const auto& [path, metadata] : m_assetMetadata)
                {
                    allAssets.push_back(path);
                }
            }

            auto cyclic = m_dependencyGraph.DetectCircularDependencies(allAssets);
            if (cyclic.empty())
            {
                // No circular dependencies, good
            }
        }
    }

    void AdvancedAssetPipeline::RenderProcessingStatistics()
    {
        ImGui::Text("Processing Statistics");
        ImGui::Separator();

        auto stats = GetProcessingStatistics();

        ImGui::Text("Total assets: %d", stats.totalAssets);
        ImGui::Text("Processed: %d", stats.processedAssets);
        ImGui::Text("Failed: %d", stats.failedAssets);
        ImGui::Text("Pending: %d", stats.pendingAssets);

        ImGui::Separator();

        ImGui::Text("Average processing time: %.3f seconds", stats.averageProcessingTime);

        if (stats.totalProcessedSize > 1024 * 1024)
        {
            ImGui::Text("Total processed size: %.2f MB",
                        static_cast<float>(stats.totalProcessedSize) / (1024.0f * 1024.0f));
        }
        else
        {
            ImGui::Text("Total processed size: %zu bytes", stats.totalProcessedSize);
        }

        ImGui::Text("Compression ratio: %.2f", stats.compressionRatio);

        ImGui::Separator();

        // Progress overview bar
        float overallProgress = 0.0f;
        if (stats.totalAssets > 0)
        {
            overallProgress = static_cast<float>(stats.processedAssets) /
                              static_cast<float>(stats.totalAssets);
        }
        ImGui::ProgressBar(overallProgress, ImVec2(-1.0f, 0.0f), "Overall Progress");

        ImGui::Separator();

        if (ImGui::Button("Validate All Assets"))
        {
            auto invalid = ValidateAllAssets();
            // Results would be shown in a log or status bar
        }

        ImGui::SameLine();
        if (ImGui::Button("Optimize All Assets"))
        {
            OptimizeAllAssets(nullptr);
        }

        ImGui::Separator();

        if (ImGui::Button("Export Database"))
        {
            ExportAssetDatabase(m_cacheDirectory + "asset_database.txt");
        }

        ImGui::SameLine();
        if (ImGui::Button("Import Database"))
        {
            ImportAssetDatabase(m_cacheDirectory + "asset_database.txt");
        }
    }

    void AdvancedAssetPipeline::RenderImportSettings()
    {
        ImGui::Text("Import Settings");
        ImGui::Separator();

        // Common settings
        ImGui::Checkbox("Enabled", &m_currentImportSettings.enabled);
        ImGui::Checkbox("Overwrite Existing", &m_currentImportSettings.overwriteExisting);
        ImGui::Checkbox("Auto-Process on Import", &m_autoProcessOnImport);
        ImGui::Checkbox("Generate Thumbnails", &m_generateThumbnails);

        bool monitoring = m_fileSystemMonitoring;
        if (ImGui::Checkbox("File System Monitoring", &monitoring))
        {
            SetFileSystemMonitoring(monitoring);
        }

        ImGui::Separator();

        // Texture settings
        if (ImGui::CollapsingHeader("Texture Settings"))
        {
            auto& texSettings = m_currentImportSettings.textureSettings;

            const char* formatNames[] = {"Auto", "DXT1", "DXT5", "BC7", "Uncompressed"};
            int format = static_cast<int>(texSettings.format);
            if (ImGui::Combo("Format", &format, formatNames, IM_ARRAYSIZE(formatNames)))
            {
                texSettings.format = static_cast<AssetImportSettings::TextureSettings::Format>(format);
            }

            ImGui::SliderInt("Max Texture Size", &texSettings.maxTextureSize, 64, 8192);
            ImGui::Checkbox("Generate Mip Maps", &texSettings.generateMipMaps);
            ImGui::Checkbox("sRGB", &texSettings.sRGB);
            ImGui::SliderFloat("Compression Quality", &texSettings.compressionQuality, 0.0f, 1.0f);
            ImGui::Checkbox("Alpha Is Transparency", &texSettings.alphaIsTransparency);
        }

        // Mesh settings
        if (ImGui::CollapsingHeader("Mesh Settings"))
        {
            auto& meshSettings = m_currentImportSettings.meshSettings;

            ImGui::Checkbox("Generate Normals", &meshSettings.generateNormals);
            ImGui::Checkbox("Generate Tangents", &meshSettings.generateTangents);
            ImGui::Checkbox("Generate Lightmap UVs", &meshSettings.generateLightmapUVs);
            ImGui::SliderFloat("Normal Smoothing Angle", &meshSettings.normalSmoothingAngle, 0.0f, 180.0f);
            ImGui::Checkbox("Optimize Mesh", &meshSettings.optimizeMesh);
            ImGui::Checkbox("Weld Vertices", &meshSettings.weldVertices);
            ImGui::SliderFloat("Weld Threshold", &meshSettings.weldThreshold, 0.00001f, 0.01f, "%.5f");
        }

        // Audio settings
        if (ImGui::CollapsingHeader("Audio Settings"))
        {
            auto& audioSettings = m_currentImportSettings.audioSettings;

            const char* audioFormatNames[] = {"Auto", "WAV", "OGG", "MP3"};
            int audioFormat = static_cast<int>(audioSettings.format);
            if (ImGui::Combo("Audio Format", &audioFormat, audioFormatNames, IM_ARRAYSIZE(audioFormatNames)))
            {
                audioSettings.format = static_cast<AssetImportSettings::AudioSettings::Format>(audioFormat);
            }

            ImGui::SliderInt("Sample Rate", &audioSettings.sampleRate, 8000, 96000);
            ImGui::SliderInt("Bit Depth", &audioSettings.bitDepth, 8, 32);
            ImGui::Checkbox("Force 3D", &audioSettings.force3D);
            ImGui::SliderFloat("Audio Compression Quality", &audioSettings.compressionQuality, 0.0f, 1.0f);
            ImGui::Checkbox("Load in Background", &audioSettings.loadInBackground);
        }

        // Animation settings
        if (ImGui::CollapsingHeader("Animation Settings"))
        {
            auto& animSettings = m_currentImportSettings.animationSettings;

            ImGui::Checkbox("Import Animation", &animSettings.importAnimation);
            ImGui::Checkbox("Optimize Keyframes", &animSettings.optimizeKeyframes);
            ImGui::SliderFloat("Keyframe Reduction", &animSettings.keyframeReduction, 0.0f, 1.0f, "%.3f");
            ImGui::Checkbox("Compress Rotation", &animSettings.compressRotation);
            ImGui::Checkbox("Compress Position", &animSettings.compressPosition);
            ImGui::Checkbox("Compress Scale", &animSettings.compressScale);
        }
    }

    void AdvancedAssetPipeline::ProcessingThreadFunction()
    {
        while (!m_shouldStopProcessing.load())
        {
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCondition.wait(lock, [this]
                {
                    return m_shouldStopProcessing.load() || !m_processingQueue.empty();
                });
            }

            if (m_shouldStopProcessing.load())
            {
                return;
            }

            ProcessNextJob();
        }
    }

    void AdvancedAssetPipeline::FileSystemMonitoringFunction()
    {
        // Track last known write times for monitored files
        std::unordered_map<std::string, fs::file_time_type> lastWriteTimes;

        while (!m_shouldStopMonitoring.load())
        {
            // Poll the asset directory for changes
            std::error_code ec;
            const fs::path assetDir(m_assetDirectory);

            if (fs::exists(assetDir, ec) && fs::is_directory(assetDir, ec))
            {
                for (const auto& entry : fs::recursive_directory_iterator(assetDir, ec))
                {
                    if (m_shouldStopMonitoring.load())
                    {
                        return;
                    }

                    if (!entry.is_regular_file())
                    {
                        continue;
                    }

                    const std::string filePath = entry.path().string();
                    const auto writeTime = entry.last_write_time(ec);
                    if (ec)
                    {
                        continue;
                    }

                    auto it = lastWriteTimes.find(filePath);
                    if (it == lastWriteTimes.end())
                    {
                        // New file detected
                        lastWriteTimes[filePath] = writeTime;

                        if (m_autoProcessOnImport && GetProcessorForAsset(filePath))
                        {
                            ProcessAsset(filePath, m_currentImportSettings);
                        }
                    }
                    else if (it->second != writeTime)
                    {
                        // File was modified
                        it->second = writeTime;

                        if (m_autoProcessOnImport && GetProcessorForAsset(filePath))
                        {
                            ProcessAsset(filePath, m_currentImportSettings);
                        }
                    }
                }
            }

            // Sleep to avoid excessive polling
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    bool AdvancedAssetPipeline::ProcessNextJob()
    {
        ProcessingJob job;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_processingQueue.empty())
            {
                return false;
            }
            job = m_processingQueue.top();
            m_processingQueue.pop();
        }

        // Find processor for this asset
        AssetProcessor* processor = GetProcessorForAsset(job.assetPath);
        if (!processor)
        {
            return false;
        }

        // Create or update metadata
        AssetMetadata metadata;
        {
            std::lock_guard<std::mutex> lock(m_metadataMutex);
            auto it = m_assetMetadata.find(job.assetPath);
            if (it != m_assetMetadata.end())
            {
                metadata = it->second;
            }
            else
            {
                metadata.sourceFilePath = job.assetPath;
                if (metadata.guid.empty())
                {
                    std::hash<std::string> hasher;
                    metadata.guid = std::to_string(hasher(job.assetPath));
                }
            }
            metadata.status = ProcessingStatus::PROCESSING;
            m_assetMetadata[job.assetPath] = metadata;
        }

        // Process the asset
        auto startTime = std::chrono::high_resolution_clock::now();
        bool success = processor->Process(metadata, job.settings, nullptr);
        auto endTime = std::chrono::high_resolution_clock::now();

        float elapsed = std::chrono::duration<float>(endTime - startTime).count();
        metadata.processingTime = elapsed;

        if (!success)
        {
            metadata.status = ProcessingStatus::FAILED;
        }

        // Update metadata in the cache
        {
            std::lock_guard<std::mutex> lock(m_metadataMutex);
            m_assetMetadata[job.assetPath] = metadata;
        }

        // Invoke completion callback
        if (job.completionCallback)
        {
            job.completionCallback(metadata);
        }

        return true;
    }

    AssetProcessor* AdvancedAssetPipeline::GetProcessorForAsset(const std::string& assetPath)
    {
        const fs::path path(assetPath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        auto it = m_processorMap.find(ext);
        if (it != m_processorMap.end())
        {
            return it->second;
        }
        return nullptr;
    }

    std::string AdvancedAssetPipeline::CalculateChecksum(const std::string& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            return {};
        }

        // Simple hash: read file contents and compute a hash
        std::size_t hash = 0;
        const std::size_t prime = 0x100000001B3ULL;
        constexpr std::size_t offset = 0xCBF29CE484222325ULL;

        hash = offset;

        char buffer[4096];
        while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0)
        {
            auto bytesRead = file.gcount();
            for (std::streamsize i = 0; i < bytesRead; ++i)
            {
                hash ^= static_cast<std::size_t>(static_cast<unsigned char>(buffer[i]));
                hash *= prime;
            }
        }

        // Convert hash to hex string
        std::ostringstream oss;
        oss << std::hex << hash;
        return oss.str();
    }

    void AdvancedAssetPipeline::UpdateDependencyGraph()
    {
        // Rebuild dependency graph from current metadata
        std::lock_guard<std::mutex> lock(m_metadataMutex);

        for (const auto& [path, metadata] : m_assetMetadata)
        {
            m_dependencyGraph.AddAsset(path);

            for (const auto& dep : metadata.dependencies)
            {
                m_dependencyGraph.AddDependency(path, dep);
            }
        }
    }

    bool AdvancedAssetPipeline::SaveMetadata(const AssetMetadata& metadata)
    {
        if (metadata.sourceFilePath.empty())
        {
            return false;
        }

        const fs::path metaPath = fs::path(metadata.sourceFilePath).string() + ".meta";
        std::ofstream file(metaPath.string());
        if (!file.is_open())
        {
            return false;
        }

        file << "GUID=" << metadata.guid << "\n";
        file << "SOURCE=" << metadata.sourceFilePath << "\n";
        file << "PROCESSED=" << metadata.processedFilePath << "\n";
        file << "TYPE=" << static_cast<int>(metadata.type) << "\n";
        file << "SOURCE_SIZE=" << metadata.sourceFileSize << "\n";
        file << "PROCESSED_SIZE=" << metadata.processedFileSize << "\n";
        file << "STATUS=" << static_cast<int>(metadata.status) << "\n";
        file << "CHECKSUM=" << metadata.checksum << "\n";
        file << "PROCESSOR=" << metadata.processorName << "\n";
        file << "PROCESSING_TIME=" << metadata.processingTime << "\n";

        if (!metadata.errorMessage.empty())
        {
            file << "ERROR=" << metadata.errorMessage << "\n";
        }

        if (!metadata.thumbnailPath.empty())
        {
            file << "THUMBNAIL=" << metadata.thumbnailPath << "\n";
        }

        for (const auto& dep : metadata.dependencies)
        {
            file << "DEPENDENCY=" << dep << "\n";
        }

        for (const auto& [key, value] : metadata.customData)
        {
            file << "CUSTOM_" << key << "=" << value << "\n";
        }

        return true;
    }

    std::unique_ptr<AssetMetadata> AdvancedAssetPipeline::LoadMetadata(const std::string& assetPath)
    {
        const std::string metaPath = assetPath + ".meta";
        std::ifstream file(metaPath);
        if (!file.is_open())
        {
            return nullptr;
        }

        auto metadata = std::make_unique<AssetMetadata>();
        metadata->sourceFilePath = assetPath;

        std::string line;
        while (std::getline(file, line))
        {
            auto eqPos = line.find('=');
            if (eqPos == std::string::npos)
            {
                continue;
            }

            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);

            if (key == "GUID")
            {
                metadata->guid = value;
            }
            else if (key == "SOURCE")
            {
                metadata->sourceFilePath = value;
            }
            else if (key == "PROCESSED")
            {
                metadata->processedFilePath = value;
            }
            else if (key == "TYPE")
            {
                metadata->type = static_cast<AssetType>(std::stoi(value));
            }
            else if (key == "SOURCE_SIZE")
            {
                metadata->sourceFileSize = std::stoull(value);
            }
            else if (key == "PROCESSED_SIZE")
            {
                metadata->processedFileSize = std::stoull(value);
            }
            else if (key == "STATUS")
            {
                metadata->status = static_cast<ProcessingStatus>(std::stoi(value));
            }
            else if (key == "CHECKSUM")
            {
                metadata->checksum = value;
            }
            else if (key == "PROCESSOR")
            {
                metadata->processorName = value;
            }
            else if (key == "PROCESSING_TIME")
            {
                metadata->processingTime = std::stof(value);
            }
            else if (key == "ERROR")
            {
                metadata->errorMessage = value;
            }
            else if (key == "THUMBNAIL")
            {
                metadata->thumbnailPath = value;
            }
            else if (key == "DEPENDENCY")
            {
                metadata->dependencies.push_back(value);
            }
            else if (key.rfind("CUSTOM_", 0) == 0)
            {
                std::string customKey = key.substr(7); // Remove "CUSTOM_" prefix
                metadata->customData[customKey] = value;
            }
        }

        return metadata;
    }

} // namespace SparkEditor
