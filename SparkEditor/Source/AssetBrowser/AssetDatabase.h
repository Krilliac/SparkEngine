/**
 * @file AssetDatabase.h
 * @brief Advanced asset management system for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

namespace SparkEditor
{

    /**
 * @brief Asset information structure
 */
    struct AssetInfo
    {
        std::string path;                                      ///< Full path to asset
        std::string name;                                      ///< Asset name (filename)
        std::string type;                                      ///< Asset type (Texture, Model, etc.)
        std::string guid;                                      ///< Unique identifier
        size_t fileSize = 0;                                   ///< File size in bytes
        std::time_t lastModified = 0;                          ///< Last modification time
        std::time_t lastImported = 0;                          ///< Last import time
        bool isImported = false;                               ///< Import status
        bool isDirty = false;                                  ///< Needs reimport
        std::vector<std::string> dependencies;                 ///< Asset dependencies
        std::unordered_map<std::string, std::string> metadata; ///< Custom metadata
    };

    /**
 * @brief File system event types
 */
    enum class FileSystemEvent
    {
        Created,
        Modified,
        Deleted,
        Renamed
    };

    /**
 * @brief File system change notification
 */
    struct FileSystemChange
    {
        std::string path;
        FileSystemEvent event;
        std::chrono::steady_clock::time_point timestamp;
        std::string oldPath; // For rename events
    };

    /**
 * @brief Asset import settings
 */
    struct AssetImportSettings
    {
        // Texture settings
        bool generateMipmaps = true;
        int maxTextureSize = 2048;
        std::string compressionFormat = "BC3";

        // Model settings
        bool importMaterials = true;
        bool importAnimations = true;
        float scaleFactor = 1.0f;

        // Audio settings
        std::string audioFormat = "OGG";
        int audioQuality = 80;
        bool mono = false;
    };

    /**
 * @brief Advanced asset database with real-time monitoring
 * 
 * This system provides comprehensive asset management for the Spark Engine,
 * including real-time file system monitoring, dependency tracking,
 * and automatic import/reimport functionality.
 */
    class AssetDatabase
    {
      public:
        /**
     * @brief Constructor
     */
        AssetDatabase();

        /**
     * @brief Destructor
     */
        ~AssetDatabase();

        /**
     * @brief Initialize the asset database
     * @param assetDirectory Root directory for assets
     * @return true if initialization succeeded
     */
        bool Initialize(const std::string& assetDirectory = "Assets");

        /**
     * @brief Update the asset database (call once per frame)
     * @param deltaTime Time since last update
     */
        void Update(float deltaTime);

        /**
     * @brief Shutdown the asset database
     */
        void Shutdown();

        /**
     * @brief Get all assets
     * @return Vector of asset information
     */
        const std::vector<AssetInfo>& GetAssets() const { return m_assets; }

        /**
     * @brief Get asset by path
     * @param path Asset path
     * @return Pointer to asset info, or nullptr if not found
     */
        const AssetInfo* GetAssetByPath(const std::string& path) const;

        /**
     * @brief Get asset by GUID
     * @param guid Asset GUID
     * @return Pointer to asset info, or nullptr if not found
     */
        const AssetInfo* GetAssetByGUID(const std::string& guid) const;

        /**
     * @brief Import asset from file
     * @param filePath Path to file to import
     * @return true if import succeeded
     */
        bool ImportAsset(const std::string& filePath);

        /**
     * @brief Reimport asset
     * @param assetPath Asset path
     * @return true if reimport succeeded
     */
        bool ReimportAsset(const std::string& assetPath);

        /**
     * @brief Delete asset
     * @param assetPath Asset path
     * @return true if deletion succeeded
     */
        bool DeleteAsset(const std::string& assetPath);

        /**
     * @brief Get assets by type
     * @param type Asset type filter
     * @return Vector of matching assets
     */
        std::vector<const AssetInfo*> GetAssetsByType(const std::string& type) const;

        /**
     * @brief Search assets by name
     * @param searchTerm Search term
     * @return Vector of matching assets
     */
        std::vector<const AssetInfo*> SearchAssets(const std::string& searchTerm) const;

        /**
     * @brief Get asset dependencies
     * @param assetPath Asset path
     * @return Vector of dependency paths
     */
        std::vector<std::string> GetAssetDependencies(const std::string& assetPath) const;

        /**
     * @brief Set file system change callback
     * @param callback Callback function for file system changes
     */
        void SetFileSystemChangeCallback(std::function<void(const FileSystemChange&)> callback);

        /**
     * @brief Get import settings for asset
     * @param assetPath Asset path
     * @return Import settings
     */
        AssetImportSettings GetImportSettings(const std::string& assetPath) const;

        /**
     * @brief Set import settings for asset
     * @param assetPath Asset path
     * @param settings Import settings
     */
        void SetImportSettings(const std::string& assetPath, const AssetImportSettings& settings);

        /**
     * @brief Refresh asset database (rescan all files)
     */
        void RefreshDatabase();

        /**
     * @brief Get database statistics
     */
        struct DatabaseStats
        {
            size_t totalAssets = 0;
            size_t textureAssets = 0;
            size_t modelAssets = 0;
            size_t audioAssets = 0;
            size_t shaderAssets = 0;
            size_t sceneAssets = 0;
            size_t dirtyAssets = 0;
            size_t totalSize = 0;
        };
        DatabaseStats GetDatabaseStats() const;

      private:
        /**
     * @brief Scan directory for assets
     * @param directoryPath Directory to scan
     */
        void ScanDirectory(const std::string& directoryPath);

        /**
     * @brief Determine asset type from file extension
     * @param filePath File path
     * @return Asset type string
     */
        std::string DetermineAssetType(const std::string& filePath) const;

        /**
     * @brief Generate GUID for asset
     * @param filePath File path
     * @return Generated GUID
     */
        std::string GenerateGUID(const std::string& filePath) const;

        /**
     * @brief Load asset metadata from .meta file
     * @param assetPath Asset path
     * @return true if metadata was loaded
     */
        bool LoadAssetMetadata(const std::string& assetPath);

        /**
     * @brief Save asset metadata to .meta file
     * @param assetPath Asset path
     * @return true if metadata was saved
     */
        bool SaveAssetMetadata(const std::string& assetPath);

        /**
     * @brief File system monitoring thread function
     */
        void FileSystemMonitoringThread();

        /**
     * @brief Process file system changes
     */
        void ProcessFileSystemChanges();

        /**
     * @brief Handle file system change
     * @param change File system change
     */
        void HandleFileSystemChange(const FileSystemChange& change);

        /**
     * @brief Check if file is an asset type we care about
     * @param filePath File path
     * @return true if it's an asset
     */
        bool IsAssetFile(const std::string& filePath) const;

        /**
     * @brief Update asset modification time
     * @param assetPath Asset path
     */
        void UpdateAssetModificationTime(const std::string& assetPath);

      private:
        // Asset storage
        std::vector<AssetInfo> m_assets;                    ///< All assets
        std::unordered_map<std::string, size_t> m_assetMap; ///< Path to index map
        std::unordered_map<std::string, size_t> m_guidMap;  ///< GUID to index map
        mutable std::mutex m_assetsMutex;                   ///< Thread safety mutex

        // File system monitoring
        std::atomic<bool> m_isMonitoring{false};                 ///< Monitoring active flag
        std::thread m_monitoringThread;                          ///< Monitoring thread
        std::vector<FileSystemChange> m_pendingChanges;          ///< Pending changes queue
        std::mutex m_changesMutex;                               ///< Changes queue mutex
        std::chrono::steady_clock::time_point m_lastProcessTime; ///< Last processing time
        float m_processInterval = 2.0f;                          ///< Processing interval in seconds

        // Configuration
        std::string m_assetDirectory;                                          ///< Root asset directory
        std::string m_metadataDirectory;                                       ///< Metadata directory
        std::unordered_map<std::string, AssetImportSettings> m_importSettings; ///< Import settings per asset

        // Callbacks
        std::function<void(const FileSystemChange&)> m_fileSystemCallback; ///< File system change callback

        // Supported extensions
        static const std::unordered_map<std::string, std::string> s_extensionToType; ///< Extension to type mapping
    };

} // namespace SparkEditor