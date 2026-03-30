/**
 * @file AdvancedAssetPipelineUI.cpp
 * @brief UI rendering methods for the AdvancedAssetPipeline
 *
 * Split from AdvancedAssetPipeline.cpp for maintainability.
 */

#include "AdvancedAssetPipeline.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace SparkEditor
{

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
        const char* typeNames[] = {"All",   "Texture",   "Mesh",   "Material", "Shader",
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
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "User triggered asset directory scan");
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
        if (ImGui::BeginTable("AssetTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
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
                    if (!path.contains(m_searchFilter))
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
                const char* statusNames[] = {"Pending", "Processing", "Completed", "Failed", "Skipped", "Cancelled"};
                int statusIdx = static_cast<int>(metadata.status);
                if (statusIdx >= 0 && statusIdx < 6)
                {
                    ImGui::Text("%s", statusNames[statusIdx]);
                }

                ImGui::TableSetColumnIndex(3);
                if (metadata.sourceFileSize > 1024 * 1024)
                {
                    ImGui::Text("%.2f MB", static_cast<float>(metadata.sourceFileSize) / (1024.0f * 1024.0f));
                }
                else if (metadata.sourceFileSize > 1024)
                {
                    ImGui::Text("%.2f KB", static_cast<float>(metadata.sourceFileSize) / 1024.0f);
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
            ImGui::ProgressBar(batch.progress, ImVec2(-1.0f, 0.0f), batch.isActive ? "Processing..." : "Complete");
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

        const char* statusNames[] = {"Pending", "Processing", "Completed", "Failed", "Skipped", "Cancelled"};
        int statusIdx = static_cast<int>(metadata.status);
        if (statusIdx >= 0 && statusIdx < 6)
        {
            ImGui::Text("Status: %s", statusNames[statusIdx]);
        }

        if (!metadata.errorMessage.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", metadata.errorMessage.c_str());
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
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Reprocessing asset: %s", m_selectedAsset.c_str());
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
            overallProgress = static_cast<float>(stats.processedAssets) / static_cast<float>(stats.totalAssets);
        }
        ImGui::ProgressBar(overallProgress, ImVec2(-1.0f, 0.0f), "Overall Progress");

        ImGui::Separator();

        if (ImGui::Button("Validate All Assets"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Validating all assets");
            auto invalid = ValidateAllAssets();
            // Results would be shown in a log or status bar
        }

        ImGui::SameLine();
        if (ImGui::Button("Optimize All Assets"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Optimizing all assets");
            OptimizeAllAssets(nullptr);
        }

        ImGui::Separator();

        if (ImGui::Button("Export Database"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Exporting asset database");
            ExportAssetDatabase(m_cacheDirectory + "asset_database.txt");
        }

        ImGui::SameLine();
        if (ImGui::Button("Import Database"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Importing asset database");
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
                m_queueCondition.wait(lock,
                                      [this] { return m_shouldStopProcessing.load() || !m_processingQueue.empty(); });
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
