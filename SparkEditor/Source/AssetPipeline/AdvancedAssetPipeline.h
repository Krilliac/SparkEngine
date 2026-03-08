/**
 * @file AdvancedAssetPipeline.h
 * @brief Advanced asset processing and pipeline system for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a comprehensive asset pipeline system with custom importers,
 * batch processing, optimization, dependency tracking, and validation capabilities
 * similar to Unity's AssetPostprocessor and Unreal's Asset Registry systems.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <filesystem>
#include <chrono>

namespace SparkEditor
{

    /**
 * @brief Asset types supported by the pipeline
 */
    enum class AssetType
    {
        UNKNOWN = 0,
        TEXTURE = 1,
        MESH = 2,
        MATERIAL = 3,
        SHADER = 4,
        AUDIO = 5,
        ANIMATION = 6,
        SCRIPT = 7,
        FONT = 8,
        VIDEO = 9,
        SCENE = 10,
        PREFAB = 11,
        TERRAIN = 12,
        LIGHTMAP = 13,
        PHYSICS_MATERIAL = 14,
        COMPUTE_SHADER = 15,
        CUSTOM = 1000
    };

    /**
 * @brief Asset processing status
 */
    enum class ProcessingStatus
    {
        PENDING = 0,    ///< Waiting to be processed
        PROCESSING = 1, ///< Currently being processed
        COMPLETED = 2,  ///< Successfully processed
        FAILED = 3,     ///< Processing failed
        SKIPPED = 4,    ///< Processing skipped (up to date)
        CANCELLED = 5   ///< Processing was cancelled
    };

    /**
 * @brief Asset import settings
 */
    struct AssetImportSettings
    {
        // Common settings
        bool enabled = true;            ///< Whether asset should be imported
        std::string outputDirectory;    ///< Output directory for processed assets
        bool overwriteExisting = false; ///< Overwrite existing processed assets

        // Texture settings
        struct TextureSettings
        {
            enum Format
            {
                AUTO = 0,
                DXT1 = 1,
                DXT5 = 2,
                BC7 = 3,
                UNCOMPRESSED = 4
            } format = AUTO;

            int maxTextureSize = 2048;        ///< Maximum texture dimension
            bool generateMipMaps = true;      ///< Generate mip maps
            bool sRGB = true;                 ///< Use sRGB color space
            float compressionQuality = 0.8f;  ///< Compression quality (0-1)
            bool alphaIsTransparency = false; ///< Treat alpha as transparency

        } textureSettings;

        // Mesh settings
        struct MeshSettings
        {
            bool generateNormals = true;        ///< Generate normals if missing
            bool generateTangents = true;       ///< Generate tangent vectors
            bool generateLightmapUVs = false;   ///< Generate second UV set for lightmaps
            float normalSmoothingAngle = 60.0f; ///< Normal smoothing angle threshold
            bool optimizeMesh = true;           ///< Optimize mesh for rendering
            bool weldVertices = true;           ///< Weld duplicate vertices
            float weldThreshold = 0.0001f;      ///< Vertex welding threshold

        } meshSettings;

        // Audio settings
        struct AudioSettings
        {
            enum Format
            {
                AUTO = 0,
                WAV = 1,
                OGG = 2,
                MP3 = 3
            } format = AUTO;

            int sampleRate = 44100;          ///< Audio sample rate
            int bitDepth = 16;               ///< Audio bit depth
            bool force3D = false;            ///< Force 3D audio processing
            float compressionQuality = 0.7f; ///< Audio compression quality
            bool loadInBackground = true;    ///< Load audio in background

        } audioSettings;

        // Animation settings
        struct AnimationSettings
        {
            bool importAnimation = true;     ///< Import animation data
            bool optimizeKeyframes = true;   ///< Optimize animation keyframes
            float keyframeReduction = 0.01f; ///< Keyframe reduction threshold
            bool compressRotation = true;    ///< Compress rotation curves
            bool compressPosition = false;   ///< Compress position curves
            bool compressScale = false;      ///< Compress scale curves

        } animationSettings;

        // Custom settings
        std::unordered_map<std::string, std::string> customSettings; ///< Custom importer settings
    };

    /**
 * @brief Asset metadata
 */
    struct AssetMetadata
    {
        std::string guid;                                         ///< Unique asset identifier
        std::string sourceFilePath;                               ///< Source file path
        std::string processedFilePath;                            ///< Processed asset path
        AssetType type;                                           ///< Asset type
        size_t sourceFileSize = 0;                                ///< Source file size in bytes
        size_t processedFileSize = 0;                             ///< Processed file size in bytes
        std::chrono::system_clock::time_point sourceModifiedTime; ///< Source file modification time
        std::chrono::system_clock::time_point processedTime;      ///< Last processing time
        std::string checksum;                                     ///< File content checksum
        ProcessingStatus status = ProcessingStatus::PENDING;      ///< Processing status
        std::string errorMessage;                                 ///< Error message if processing failed

        // Dependencies
        std::vector<std::string> dependencies; ///< Assets this asset depends on
        std::vector<std::string> dependents;   ///< Assets that depend on this asset

        // Processing info
        float processingTime = 0.0f;        ///< Time taken to process (seconds)
        std::string processorName;          ///< Name of processor used
        AssetImportSettings importSettings; ///< Import settings used

        // Thumbnail
        std::string thumbnailPath; ///< Path to generated thumbnail

        // Custom metadata
        std::unordered_map<std::string, std::string> customData; ///< Custom metadata
    };

    /**
 * @brief Base class for asset processors
 */
    class AssetProcessor
    {
      public:
        /**
     * @brief Virtual destructor
     */
        virtual ~AssetProcessor() = default;

        /**
     * @brief Get processor name
     * @return Processor name
     */
        virtual std::string GetName() const = 0;

        /**
     * @brief Get supported file extensions
     * @return Vector of supported extensions (with dots)
     */
        virtual std::vector<std::string> GetSupportedExtensions() const = 0;

        /**
     * @brief Get asset type this processor handles
     * @return Asset type
     */
        virtual AssetType GetAssetType() const = 0;

        /**
     * @brief Check if processor can handle file
     * @param filePath File path to check
     * @return true if processor can handle the file
     */
        virtual bool CanProcess(const std::string& filePath) const;

        /**
     * @brief Process asset file
     * @param metadata Asset metadata
     * @param settings Import settings
     * @param progressCallback Progress callback (0-1)
     * @return true if processing succeeded
     */
        virtual bool Process(AssetMetadata& metadata, const AssetImportSettings& settings,
                             std::function<void(float)> progressCallback = nullptr) = 0;

        /**
     * @brief Generate thumbnail for asset
     * @param metadata Asset metadata
     * @param thumbnailSize Thumbnail size in pixels
     * @return true if thumbnail was generated
     */
        virtual bool GenerateThumbnail(const AssetMetadata& metadata, int thumbnailSize = 128) = 0;

        /**
     * @brief Validate processed asset
     * @param metadata Asset metadata
     * @return true if asset is valid
     */
        virtual bool Validate(const AssetMetadata& metadata) = 0;
    };

    /**
 * @brief Texture asset processor
 */
    class TextureProcessor : public AssetProcessor
    {
      public:
        std::string GetName() const override { return "Texture Processor"; }
        std::vector<std::string> GetSupportedExtensions() const override;
        AssetType GetAssetType() const override { return AssetType::TEXTURE; }
        bool Process(AssetMetadata& metadata, const AssetImportSettings& settings,
                     std::function<void(float)> progressCallback = nullptr) override;
        bool GenerateThumbnail(const AssetMetadata& metadata, int thumbnailSize = 128) override;
        bool Validate(const AssetMetadata& metadata) override;

      private:
        bool CompressTexture(const std::string& inputPath, const std::string& outputPath,
                             const AssetImportSettings::TextureSettings& settings);
        bool GenerateMipMaps(const std::string& texturePath);
    };

    /**
 * @brief Mesh asset processor
 */
    class MeshProcessor : public AssetProcessor
    {
      public:
        std::string GetName() const override { return "Mesh Processor"; }
        std::vector<std::string> GetSupportedExtensions() const override;
        AssetType GetAssetType() const override { return AssetType::MESH; }
        bool Process(AssetMetadata& metadata, const AssetImportSettings& settings,
                     std::function<void(float)> progressCallback = nullptr) override;
        bool GenerateThumbnail(const AssetMetadata& metadata, int thumbnailSize = 128) override;
        bool Validate(const AssetMetadata& metadata) override;

      private:
        bool OptimizeMesh(const std::string& meshPath, const AssetImportSettings::MeshSettings& settings);
        bool GenerateNormals(const std::string& meshPath, float smoothingAngle);
        bool GenerateTangents(const std::string& meshPath);
        bool GenerateLightmapUVs(const std::string& meshPath);
    };

    /**
 * @brief Audio asset processor
 */
    class AudioProcessor : public AssetProcessor
    {
      public:
        std::string GetName() const override { return "Audio Processor"; }
        std::vector<std::string> GetSupportedExtensions() const override;
        AssetType GetAssetType() const override { return AssetType::AUDIO; }
        bool Process(AssetMetadata& metadata, const AssetImportSettings& settings,
                     std::function<void(float)> progressCallback = nullptr) override;
        bool GenerateThumbnail(const AssetMetadata& metadata, int thumbnailSize = 128) override;
        bool Validate(const AssetMetadata& metadata) override;

      private:
        bool ConvertAudio(const std::string& inputPath, const std::string& outputPath,
                          const AssetImportSettings::AudioSettings& settings);
        bool AnalyzeAudio(const std::string& audioPath, AssetMetadata& metadata);
    };

    /**
 * @brief Asset processing job
 */
    struct ProcessingJob
    {
        std::string assetPath;                                        ///< Asset file path
        AssetImportSettings settings;                                 ///< Import settings
        std::function<void(const AssetMetadata&)> completionCallback; ///< Completion callback
        int priority = 0;                                             ///< Job priority (higher = more important)
        std::chrono::system_clock::time_point submissionTime;         ///< Job submission time

        bool operator<(const ProcessingJob& other) const
        {
            if (priority != other.priority)
            {
                return priority < other.priority; // Higher priority first
            }
            return submissionTime > other.submissionTime; // Earlier submission first
        }
    };

    /**
 * @brief Batch processing operation
 */
    struct BatchOperation
    {
        std::string name;                            ///< Operation name
        std::vector<std::string> assetPaths;         ///< Assets to process
        AssetImportSettings settings;                ///< Batch import settings
        std::function<void(float)> progressCallback; ///< Progress callback
        std::function<void()> completionCallback;    ///< Completion callback
        bool isActive = false;                       ///< Whether operation is active
        float progress = 0.0f;                       ///< Current progress (0-1)
        int completedAssets = 0;                     ///< Number of completed assets
        int totalAssets = 0;                         ///< Total number of assets
    };

    /**
 * @brief Asset dependency graph
 */
    class AssetDependencyGraph
    {
      public:
        /**
     * @brief Add asset to dependency graph
     * @param assetPath Asset path
     */
        void AddAsset(const std::string& assetPath);

        /**
     * @brief Remove asset from dependency graph
     * @param assetPath Asset path
     */
        void RemoveAsset(const std::string& assetPath);

        /**
     * @brief Add dependency relationship
     * @param dependent Asset that depends on dependency
     * @param dependency Asset that is depended upon
     */
        void AddDependency(const std::string& dependent, const std::string& dependency);

        /**
     * @brief Remove dependency relationship
     * @param dependent Dependent asset
     * @param dependency Dependency asset
     */
        void RemoveDependency(const std::string& dependent, const std::string& dependency);

        /**
     * @brief Get direct dependencies of an asset
     * @param assetPath Asset path
     * @return Vector of dependency paths
     */
        std::vector<std::string> GetDependencies(const std::string& assetPath) const;

        /**
     * @brief Get assets that depend on this asset
     * @param assetPath Asset path
     * @return Vector of dependent asset paths
     */
        std::vector<std::string> GetDependents(const std::string& assetPath) const;

        /**
     * @brief Get processing order for assets
     * @param assetPaths Assets to order
     * @return Ordered list of assets (dependencies first)
     */
        std::vector<std::string> GetProcessingOrder(const std::vector<std::string>& assetPaths) const;

        /**
     * @brief Check for circular dependencies
     * @param assetPaths Assets to check
     * @return Vector of assets involved in circular dependencies
     */
        std::vector<std::string> DetectCircularDependencies(const std::vector<std::string>& assetPaths) const;

        /**
     * @brief Get all assets that would be affected by changing an asset
     * @param assetPath Asset path
     * @return Vector of affected asset paths
     */
        std::vector<std::string> GetAffectedAssets(const std::string& assetPath) const;

      private:
        std::unordered_map<std::string, std::vector<std::string>> m_dependencies; ///< Asset -> Dependencies
        std::unordered_map<std::string, std::vector<std::string>> m_dependents;   ///< Asset -> Dependents
    };

    /**
 * @brief Advanced asset pipeline system
 * 
 * Provides comprehensive asset processing capabilities including:
 * - Custom asset importers and processors
 * - Batch asset processing with priority queues
 * - Asset dependency tracking and management
 * - Automatic asset optimization and validation
 * - Real-time file system monitoring
 * - Asset metadata and thumbnail generation
 * - Multi-threaded processing with progress tracking
 * - Integration with version control systems
 * 
 * Inspired by Unity's AssetPostprocessor system and Unreal's Asset Registry.
 */
    class AdvancedAssetPipeline : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        AdvancedAssetPipeline();

        /**
     * @brief Destructor
     */
        ~AdvancedAssetPipeline() override;

        /**
     * @brief Initialize the asset pipeline
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update asset pipeline
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render asset pipeline UI
     */
        void Render() override;

        /**
     * @brief Shutdown the asset pipeline
     */
        void Shutdown() override;

        /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Register asset processor
     * @param processor Asset processor to register
     */
        void RegisterProcessor(std::unique_ptr<AssetProcessor> processor);

        /**
     * @brief Process single asset
     * @param assetPath Path to asset file
     * @param settings Import settings
     * @param callback Completion callback
     * @return true if job was queued successfully
     */
        bool ProcessAsset(const std::string& assetPath, const AssetImportSettings& settings,
                          std::function<void(const AssetMetadata&)> callback = nullptr);

        /**
     * @brief Process multiple assets in batch
     * @param assetPaths Paths to asset files
     * @param settings Import settings
     * @param progressCallback Progress callback
     * @param completionCallback Completion callback
     * @return Batch operation ID
     */
        uint32_t ProcessAssetsBatch(const std::vector<std::string>& assetPaths, const AssetImportSettings& settings,
                                    std::function<void(float)> progressCallback = nullptr,
                                    std::function<void()> completionCallback = nullptr);

        /**
     * @brief Cancel batch operation
     * @param operationID Batch operation ID
     * @return true if operation was cancelled
     */
        bool CancelBatchOperation(uint32_t operationID);

        /**
     * @brief Get asset metadata
     * @param assetPath Asset path
     * @return Pointer to metadata, or nullptr if not found
     */
        const AssetMetadata* GetAssetMetadata(const std::string& assetPath) const;

        /**
     * @brief Refresh asset metadata
     * @param assetPath Asset path
     * @return true if metadata was refreshed
     */
        bool RefreshAssetMetadata(const std::string& assetPath);

        /**
     * @brief Scan directory for assets
     * @param directoryPath Directory to scan
     * @param recursive Whether to scan recursively
     * @return Number of assets found
     */
        int ScanDirectory(const std::string& directoryPath, bool recursive = true);

        /**
     * @brief Get processing statistics
     */
        struct ProcessingStatistics
        {
            int totalAssets = 0;
            int processedAssets = 0;
            int failedAssets = 0;
            int pendingAssets = 0;
            float averageProcessingTime = 0.0f;
            size_t totalProcessedSize = 0;
            float compressionRatio = 1.0f;
        };
        ProcessingStatistics GetProcessingStatistics() const;

        /**
     * @brief Get asset dependency graph
     * @return Reference to dependency graph
     */
        const AssetDependencyGraph& GetDependencyGraph() const { return m_dependencyGraph; }

        /**
     * @brief Enable/disable file system monitoring
     * @param enabled Whether monitoring is enabled
     */
        void SetFileSystemMonitoring(bool enabled);

        /**
     * @brief Check if file system monitoring is enabled
     * @return true if monitoring is enabled
     */
        bool IsFileSystemMonitoring() const { return m_fileSystemMonitoring; }

        /**
     * @brief Set processing thread count
     * @param threadCount Number of processing threads
     */
        void SetProcessingThreadCount(int threadCount);

        /**
     * @brief Get processing thread count
     * @return Current number of processing threads
     */
        int GetProcessingThreadCount() const { return static_cast<int>(m_processingThreads.size()); }

        /**
     * @brief Optimize all assets
     * @param progressCallback Progress callback
     */
        void OptimizeAllAssets(std::function<void(float)> progressCallback = nullptr);

        /**
     * @brief Validate all assets
     * @return Vector of invalid asset paths
     */
        std::vector<std::string> ValidateAllAssets();

        /**
     * @brief Export asset database
     * @param filePath Output file path
     * @return true if export succeeded
     */
        bool ExportAssetDatabase(const std::string& filePath);

        /**
     * @brief Import asset database
     * @param filePath Input file path
     * @return true if import succeeded
     */
        bool ImportAssetDatabase(const std::string& filePath);

      private:
        /**
     * @brief Render asset list
     */
        void RenderAssetList();

        /**
     * @brief Render processing queue
     */
        void RenderProcessingQueue();

        /**
     * @brief Render batch operations
     */
        void RenderBatchOperations();

        /**
     * @brief Render asset inspector
     */
        void RenderAssetInspector();

        /**
     * @brief Render dependency viewer
     */
        void RenderDependencyViewer();

        /**
     * @brief Render processing statistics
     */
        void RenderProcessingStatistics();

        /**
     * @brief Render import settings
     */
        void RenderImportSettings();

        /**
     * @brief Processing thread function
     */
        void ProcessingThreadFunction();

        /**
     * @brief File system monitoring thread function
     */
        void FileSystemMonitoringFunction();

        /**
     * @brief Process next job in queue
     * @return true if job was processed
     */
        bool ProcessNextJob();

        /**
     * @brief Get processor for asset
     * @param assetPath Asset path
     * @return Pointer to processor, or nullptr if none found
     */
        AssetProcessor* GetProcessorForAsset(const std::string& assetPath);

        /**
     * @brief Calculate asset checksum
     * @param filePath File path
     * @return File checksum
     */
        std::string CalculateChecksum(const std::string& filePath);

        /**
     * @brief Update dependency graph from metadata
     */
        void UpdateDependencyGraph();

        /**
     * @brief Save asset metadata to file
     * @param metadata Metadata to save
     * @return true if save succeeded
     */
        bool SaveMetadata(const AssetMetadata& metadata);

        /**
     * @brief Load asset metadata from file
     * @param assetPath Asset path
     * @return Loaded metadata, or nullptr if not found
     */
        std::unique_ptr<AssetMetadata> LoadMetadata(const std::string& assetPath);

      private:
        // Asset processors
        std::vector<std::unique_ptr<AssetProcessor>> m_processors;       ///< Registered processors
        std::unordered_map<std::string, AssetProcessor*> m_processorMap; ///< Extension to processor map

        // Asset metadata
        std::unordered_map<std::string, AssetMetadata> m_assetMetadata; ///< Asset metadata cache
        mutable std::mutex m_metadataMutex;                             ///< Metadata access mutex

        // Processing queue
        std::priority_queue<ProcessingJob> m_processingQueue; ///< Priority queue for processing jobs
        mutable std::mutex m_queueMutex;                      ///< Queue access mutex
        std::condition_variable m_queueCondition;             ///< Queue condition variable

        // Processing threads
        std::vector<std::thread> m_processingThreads;    ///< Processing worker threads
        std::atomic<bool> m_shouldStopProcessing{false}; ///< Stop processing flag

        // Batch operations
        std::unordered_map<uint32_t, BatchOperation> m_batchOperations; ///< Active batch operations
        uint32_t m_nextBatchID = 1;                                     ///< Next batch operation ID
        mutable std::mutex m_batchMutex;                                ///< Batch operations mutex

        // Dependency tracking
        AssetDependencyGraph m_dependencyGraph; ///< Asset dependency graph

        // File system monitoring
        bool m_fileSystemMonitoring = true;              ///< File system monitoring enabled
        std::thread m_monitoringThread;                  ///< File system monitoring thread
        std::atomic<bool> m_shouldStopMonitoring{false}; ///< Stop monitoring flag

        // UI state
        std::string m_selectedAsset;                 ///< Currently selected asset
        AssetImportSettings m_currentImportSettings; ///< Current import settings
        bool m_showProcessingQueue = true;           ///< Show processing queue panel
        bool m_showBatchOperations = true;           ///< Show batch operations panel
        bool m_showDependencyViewer = false;         ///< Show dependency viewer panel
        bool m_showStatistics = true;                ///< Show statistics panel

        // Filtering and search
        std::string m_searchFilter;                                  ///< Asset search filter
        AssetType m_typeFilter = AssetType::UNKNOWN;                 ///< Asset type filter
        ProcessingStatus m_statusFilter = ProcessingStatus::PENDING; ///< Status filter

        // Statistics
        mutable ProcessingStatistics m_statistics; ///< Processing statistics

        // Configuration
        std::string m_assetDirectory = "Assets/";  ///< Asset directory path
        std::string m_cacheDirectory = "Library/"; ///< Cache directory path
        int m_maxProcessingThreads = 4;            ///< Maximum processing threads
        bool m_autoProcessOnImport = true;         ///< Auto-process new assets
        bool m_generateThumbnails = true;          ///< Generate asset thumbnails
        float m_thumbnailUpdateInterval = 1.0f;    ///< Thumbnail update interval
    };

} // namespace SparkEditor