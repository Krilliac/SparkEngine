/**
 * @file AssetPipelineTypes.h
 * @brief Type definitions for the advanced asset pipeline system
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains enums, configuration structs, and data types used by the asset pipeline.
 * Extracted from AdvancedAssetPipeline.h to reduce header size and improve compile times.
 */

#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

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

    enum class LODTargetPlatform
    {
        Desktop = 0,
        Mobile = 1,
        Console = 2
    };

    enum class LODQualityTier
    {
        Low = 0,
        Medium = 1,
        High = 2,
        Ultra = 3
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
            bool generateMipMaps = false;     ///< Generate mip maps (unsupported offline; opt-in fails closed)
            bool sRGB = true;                 ///< Use sRGB color space
            float compressionQuality = 0.8f;  ///< Compression quality (0-1)
            bool alphaIsTransparency = false; ///< Treat alpha as transparency

        } textureSettings;

        // Mesh settings
        struct MeshSettings
        {
            bool generateNormals = false;       ///< Generate normals if missing (unsupported offline)
            bool generateTangents = false;      ///< Generate tangent vectors (unsupported offline)
            bool generateLightmapUVs = false;   ///< Generate second UV set for lightmaps
            float normalSmoothingAngle = 60.0f; ///< Normal smoothing angle threshold
            bool optimizeMesh = false;          ///< Optimize mesh for rendering (unsupported offline)
            bool weldVertices = false;          ///< Weld duplicate vertices (unsupported offline)
            float weldThreshold = 0.0001f;      ///< Vertex welding threshold
            bool autoGenerateLODs = false;      ///< Generate LOD chain during import
            bool previewGeneratedLODs = true;   ///< Show LOD chain preview in inspector
            LODTargetPlatform lodPlatform = LODTargetPlatform::Desktop;
            LODQualityTier lodQuality = LODQualityTier::High;

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
 * @brief Asset processing job
 */
    struct ProcessingJob
    {
        std::string assetPath;                                        ///< Asset file path
        AssetImportSettings settings;                                 ///< Import settings
        std::function<void(const AssetMetadata&)> completionCallback; ///< Completion callback
        int priority = 0;                                             ///< Job priority (higher = more important)
        std::chrono::system_clock::time_point submissionTime;         ///< Job submission time
        uint32_t batchID = 0;                                         ///< Owning batch (0 = standalone)

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
        std::string name;                                    ///< Operation name
        std::vector<std::string> assetPaths;                 ///< Assets to process
        AssetImportSettings settings;                        ///< Batch import settings
        std::function<void(float)> progressCallback;         ///< Progress callback
        std::function<void()> completionCallback;            ///< Completion callback
        bool isActive = false;                               ///< Whether operation is active
        float progress = 0.0f;                               ///< Current progress (0-1)
        int completedAssets = 0;                             ///< Number of completed assets
        int totalAssets = 0;                                 ///< Total number of assets
        int failedAssets = 0;                                ///< Number of failed assets
        ProcessingStatus status = ProcessingStatus::PENDING; ///< Aggregate batch state
        bool completionDispatched = false;                   ///< Exactly-once completion guard
    };

} // namespace SparkEditor
