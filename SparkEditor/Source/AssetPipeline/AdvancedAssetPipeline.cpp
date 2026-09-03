/**
 * @file AdvancedAssetPipeline.cpp
 * @brief Implementation of the advanced asset processing and pipeline system
 * @author Spark Engine Team
 * @date 2025
 */

#include "AdvancedAssetPipeline.h"
#include "Utils/ContainerUtils.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <queue>
#include <sstream>

namespace fs = std::filesystem;

namespace SparkEditor
{
    namespace
    {
        std::chrono::system_clock::time_point ToSystemClock(fs::file_time_type fileTime)
        {
            const auto fileNow = fs::file_time_type::clock::now();
            const auto systemNow = std::chrono::system_clock::now();
            return std::chrono::time_point_cast<std::chrono::system_clock::duration>(fileTime - fileNow + systemNow);
        }
    } // namespace

    // =========================================================================
    // AdvancedAssetPipeline
    // =========================================================================

    AdvancedAssetPipeline::AdvancedAssetPipeline() : EditorPanel("Asset Pipeline", "asset_pipeline") {}

    AdvancedAssetPipeline::~AdvancedAssetPipeline()
    {
        Shutdown();
    }

    bool AdvancedAssetPipeline::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (m_isInitialized)
            return true;

        // Register default processors
        RegisterProcessor(std::make_unique<TextureProcessor>());
        RegisterProcessor(std::make_unique<MeshProcessor>());
        RegisterProcessor(std::make_unique<AudioProcessor>());

        // Start processing threads
        m_shouldStopProcessing.store(false);
        m_acceptingJobs.store(true);
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "AdvancedAssetPipeline initialized with %d processing threads",
                       m_maxProcessingThreads);
        return true;
    }

    void AdvancedAssetPipeline::Update(float /*deltaTime*/)
    {
        // Workers update state immediately but preserve the historical editor
        // thread affinity by queueing callbacks for this unlocked UI tick.
        DrainBatchNotifications();
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "AdvancedAssetPipeline shutting down");
        m_acceptingJobs.store(false);
        // Stop processing threads. The flag is published under the queue mutex:
        // a worker that has already evaluated the wait predicate but not yet
        // blocked would otherwise miss this notification and never wake, and
        // the join below would hang.
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_shouldStopProcessing.store(true);
        }
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

        std::vector<ProcessingJob> abandonedJobs;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            while (!m_processingQueue.empty())
            {
                abandonedJobs.push_back(m_processingQueue.top());
                m_processingQueue.pop();
            }
        }

        std::vector<std::function<void()>> cancelledBatchCallbacks;
        {
            std::lock_guard<std::mutex> lock(m_batchMutex);
            for (auto& [id, batch] : m_batchOperations)
            {
                if (!batch.isActive)
                    continue;
                batch.isActive = false;
                batch.status = ProcessingStatus::CANCELLED;
                if (!batch.completionDispatched)
                {
                    batch.completionDispatched = true;
                    if (batch.completionCallback)
                        cancelledBatchCallbacks.push_back(batch.completionCallback);
                }
            }
        }

        // Every admitted job and batch reaches one terminal callback. These
        // invocations are deliberately outside queue/batch locks.
        for (auto& job : abandonedJobs)
        {
            if (!job.completionCallback)
                continue;
            AssetMetadata cancelled;
            cancelled.sourceFilePath = job.assetPath;
            cancelled.status = ProcessingStatus::CANCELLED;
            cancelled.errorMessage = "Asset pipeline shut down before processing";
            try
            {
                job.completionCallback(cancelled);
            }
            catch (...)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Asset cancellation callback threw during shutdown");
            }
        }
        for (auto& callback : cancelledBatchCallbacks)
            QueueBatchNotification(std::move(callback));

        // No future Update is guaranteed after shutdown. Drain after all
        // workers have joined and all terminal notifications are queued.
        DrainBatchNotifications();

        // Clear data
        {
            std::lock_guard<std::mutex> lock(m_metadataMutex);
            m_assetMetadata.clear();
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
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Attempted to register null asset processor");
            return;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Registering asset processor: %s", processor->GetName().c_str());
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
        return EnqueueAsset(assetPath, settings, std::move(callback), 0);
    }

    bool AdvancedAssetPipeline::EnqueueAsset(const std::string& assetPath, const AssetImportSettings& settings,
                                             std::function<void(const AssetMetadata&)> callback, uint32_t batchID)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);

        AssetMetadata rejected;
        rejected.sourceFilePath = assetPath;
        rejected.status = ProcessingStatus::FAILED;

        if (assetPath.empty())
            rejected.errorMessage = "Asset path is empty";
        else if (!m_acceptingJobs.load())
            rejected.errorMessage = "Asset pipeline is not accepting jobs";
        else if (!settings.enabled)
        {
            rejected.status = ProcessingStatus::SKIPPED;
            rejected.errorMessage = "Asset import is disabled";
        }
        else if (!fs::exists(assetPath))
            rejected.errorMessage = "Source file does not exist: " + assetPath;
        else if (!GetProcessorForAsset(assetPath))
            rejected.errorMessage = "No asset processor supports: " + fs::path(assetPath).extension().string();

        if (!rejected.errorMessage.empty())
        {
            if (callback)
            {
                try
                {
                    callback(rejected);
                }
                catch (...)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Asset rejection callback threw");
                }
            }
            return false;
        }

        ProcessingJob job;
        job.assetPath = assetPath;
        job.settings = settings;
        job.completionCallback = std::move(callback);
        job.priority = 0;
        job.submissionTime = std::chrono::system_clock::now();
        job.batchID = batchID;

        bool admitted = false;
        if (batchID != 0)
        {
            std::lock_guard<std::mutex> batchLock(m_batchMutex);
            auto batchIt = m_batchOperations.find(batchID);
            if (batchIt == m_batchOperations.end() || !batchIt->second.isActive)
            {
                rejected.status = ProcessingStatus::CANCELLED;
                rejected.errorMessage = "Batch was cancelled before asset admission";
            }
            else
            {
                std::lock_guard<std::mutex> queueLock(m_queueMutex);
                if (m_acceptingJobs.load())
                {
                    m_processingQueue.push(std::move(job));
                    admitted = true;
                }
                else
                {
                    rejected.status = ProcessingStatus::CANCELLED;
                    rejected.errorMessage = "Asset pipeline stopped before asset admission";
                }
            }
        }
        else
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            if (m_acceptingJobs.load())
            {
                m_processingQueue.push(std::move(job));
                admitted = true;
            }
            else
            {
                rejected.status = ProcessingStatus::CANCELLED;
                rejected.errorMessage = "Asset pipeline stopped before asset admission";
            }
        }

        if (!admitted)
        {
            if (job.completionCallback)
            {
                try
                {
                    job.completionCallback(rejected);
                }
                catch (...)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Asset cancellation callback threw during admission");
                }
            }
            return false;
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
            batch.isActive = !assetPaths.empty();
            batch.progress = assetPaths.empty() ? 1.0f : 0.0f;
            batch.completedAssets = 0;
            batch.totalAssets = static_cast<int>(assetPaths.size());
            batch.status = assetPaths.empty() ? ProcessingStatus::COMPLETED : ProcessingStatus::PROCESSING;
            batch.completionDispatched = assetPaths.empty();

            m_batchOperations[batchID] = std::move(batch);
        }

        if (assetPaths.empty())
        {
            // The callbacks were moved into the batch; copy the stored terminal
            // callbacks without holding the lock during invocation.
            std::function<void(float)> emptyProgress;
            std::function<void()> emptyCompletion;
            {
                std::lock_guard<std::mutex> lock(m_batchMutex);
                emptyProgress = m_batchOperations[batchID].progressCallback;
                emptyCompletion = m_batchOperations[batchID].completionCallback;
            }
            if (emptyProgress)
                QueueBatchNotification([callback = std::move(emptyProgress)] { callback(1.0f); });
            if (emptyCompletion)
                QueueBatchNotification(std::move(emptyCompletion));
            return batchID;
        }

        // Queue all assets with a callback that updates batch progress
        for (const auto& assetPath : assetPaths)
        {
            auto batchCallback = [this, batchID](const AssetMetadata& metadata)
            { CompleteBatchAsset(batchID, metadata); };

            EnqueueAsset(assetPath, settings, std::move(batchCallback), batchID);
        }

        return batchID;
    }

    bool AdvancedAssetPipeline::CancelBatchOperation(uint32_t operationID)
    {
        std::function<void()> completion;
        std::vector<ProcessingJob> cancelledJobs;
        std::priority_queue<ProcessingJob> filteredQueue;
        {
            std::lock_guard<std::mutex> batchLock(m_batchMutex);
            auto it = m_batchOperations.find(operationID);
            if (it == m_batchOperations.end() || !it->second.isActive)
                return false;

            it->second.isActive = false;
            it->second.status = ProcessingStatus::CANCELLED;
            if (!it->second.completionDispatched)
            {
                it->second.completionDispatched = true;
                completion = it->second.completionCallback;
            }

            // Enqueue the terminal event while the batch transition is still
            // serialized so earlier progress cannot race behind completion.
            if (completion)
            {
                QueueBatchNotification(std::move(completion));
                completion = nullptr;
            }

            // Queue ownership is by batch ID, not path: cancelling one batch
            // must not erase a standalone/other-batch request for the same file.
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            while (!m_processingQueue.empty())
            {
                ProcessingJob job = m_processingQueue.top();
                m_processingQueue.pop();
                if (job.batchID == operationID)
                    cancelledJobs.push_back(std::move(job));
                else
                    filteredQueue.push(std::move(job));
            }
            m_processingQueue = std::move(filteredQueue);
        }

        for (auto& job : cancelledJobs)
        {
            if (!job.completionCallback)
                continue;
            AssetMetadata cancelled;
            cancelled.sourceFilePath = job.assetPath;
            cancelled.status = ProcessingStatus::CANCELLED;
            cancelled.errorMessage = "Batch operation cancelled";
            try
            {
                job.completionCallback(cancelled);
            }
            catch (...)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Asset cancellation callback threw");
            }
        }

        return true;
    }

    bool AdvancedAssetPipeline::GetBatchOperation(uint32_t operationID, BatchOperation& outOperation) const
    {
        std::lock_guard<std::mutex> lock(m_batchMutex);
        auto it = m_batchOperations.find(operationID);
        if (it == m_batchOperations.end())
            return false;
        outOperation = it->second;
        return true;
    }

    void AdvancedAssetPipeline::CompleteBatchAsset(uint32_t batchID, const AssetMetadata& metadata)
    {
        std::function<void(float)> progressCallback;
        std::function<void()> completionCallback;
        float progress = 0.0f;

        {
            std::lock_guard<std::mutex> lock(m_batchMutex);
            auto it = m_batchOperations.find(batchID);
            if (it == m_batchOperations.end() || !it->second.isActive)
                return;

            BatchOperation& batch = it->second;
            ++batch.completedAssets;
            if (metadata.status == ProcessingStatus::FAILED || metadata.status == ProcessingStatus::CANCELLED)
                ++batch.failedAssets;

            batch.progress = batch.totalAssets > 0
                                 ? static_cast<float>(batch.completedAssets) / static_cast<float>(batch.totalAssets)
                                 : 1.0f;
            progress = batch.progress;
            progressCallback = batch.progressCallback;

            if (batch.completedAssets >= batch.totalAssets)
            {
                batch.isActive = false;
                batch.progress = 1.0f;
                progress = 1.0f;
                batch.status = batch.failedAssets > 0 ? ProcessingStatus::FAILED : ProcessingStatus::COMPLETED;
                if (!batch.completionDispatched)
                {
                    batch.completionDispatched = true;
                    completionCallback = batch.completionCallback;
                }
            }

            // Batch locking defines progress order across multiple workers;
            // enqueue before releasing it so notifications remain monotonic.
            if (progressCallback)
                QueueBatchNotification([callback = std::move(progressCallback), progress] { callback(progress); });
            if (completionCallback)
                QueueBatchNotification(std::move(completionCallback));
        }
    }

    void AdvancedAssetPipeline::QueueBatchNotification(std::function<void()> notification)
    {
        if (!notification)
            return;
        std::lock_guard<std::mutex> lock(m_batchNotificationMutex);
        m_batchNotifications.push_back(std::move(notification));
    }

    void AdvancedAssetPipeline::DrainBatchNotifications()
    {
        std::vector<std::function<void()>> notifications;
        {
            std::lock_guard<std::mutex> lock(m_batchNotificationMutex);
            notifications.swap(m_batchNotifications);
        }

        for (auto& notification : notifications)
        {
            try
            {
                notification();
            }
            catch (...)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Batch notification callback threw");
            }
        }
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
        metadata.sourceModifiedTime = ToSystemClock(fs::last_write_time(path));
        metadata.checksum = CalculateChecksum(assetPath);

        return true;
    }

    int AdvancedAssetPipeline::ScanDirectory(const std::string& directoryPath, bool recursive)
    {
        const fs::path dirPath(directoryPath);
        if (!fs::exists(dirPath) || !fs::is_directory(dirPath))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Scan directory does not exist: %s", directoryPath.c_str());
            return 0;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Scanning directory: %s (recursive=%s)", directoryPath.c_str(),
                       recursive ? "true" : "false");
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
            metadata.sourceModifiedTime = ToSystemClock(entry.last_write_time());
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
            stats.compressionRatio = static_cast<float>(stats.totalProcessedSize) / static_cast<float>(totalSourceSize);
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

        m_maxProcessingThreads = threadCount;
        if (!m_isInitialized)
            return;

        // Stop existing threads (flag published under the queue mutex, see Shutdown)
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_shouldStopProcessing.store(true);
        }
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Exporting asset database to: %s", filePath.c_str());
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to open file for database export: %s",
                            filePath.c_str());
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


} // namespace SparkEditor
