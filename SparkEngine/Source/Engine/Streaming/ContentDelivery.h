/**
 * @file ContentDelivery.h
 * @brief Asset bundle versioning and content streaming system
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides content delivery infrastructure for live-service games:
 * - Asset bundles with version tracking
 * - Patch manifest comparison and delta updates
 * - Download queue with priority and progress tracking
 * - Bundle integrity verification (checksum)
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <mutex>

namespace Spark
{

    // =============================================================================
    // AssetBundle
    // =============================================================================

    /**
 * @brief A versioned collection of assets that can be downloaded/patched.
 */
    struct AssetBundle
    {
        std::string name;                ///< Bundle name (e.g. "weapons_pack_01")
        std::string version;             ///< Semantic version
        uint64_t sizeBytes = 0;          ///< Total size in bytes
        std::string checksum;            ///< SHA-256 or CRC checksum
        std::string url;                 ///< Download URL
        bool installed = false;          ///< Whether this bundle is installed locally
        bool updateAvailable = false;    ///< Whether a newer version exists
        std::vector<std::string> assets; ///< List of asset paths in this bundle
    };

    // =============================================================================
    // PatchManifest
    // =============================================================================

    /**
 * @brief Describes available updates for all bundles.
 */
    struct PatchManifest
    {
        std::string gameVersion;          ///< Target game version
        uint64_t totalDownloadSize = 0;   ///< Total download size for all updates
        std::vector<AssetBundle> bundles; ///< All available bundles
    };

    // =============================================================================
    // DownloadTask
    // =============================================================================

    /**
 * @brief A single download task in the queue.
 */
    struct DownloadTask
    {
        std::string bundleName;
        std::string url;
        uint64_t totalBytes = 0;
        uint64_t downloadedBytes = 0;
        float progress = 0.0f;
        bool completed = false;
        bool failed = false;
        int priority = 0;
    };

    // =============================================================================
    // ContentDelivery
    // =============================================================================

    /**
 * @class ContentDelivery
 * @brief Manages asset bundle versioning, patching, and downloads.
 */
    class ContentDelivery
    {
      public:
        ContentDelivery();
        ~ContentDelivery() = default;

        /**
     * @brief Check for updates by comparing local manifest to remote.
     * @param manifestUrl URL to the remote patch manifest.
     * @return true if manifest was fetched and parsed.
     */
        bool CheckForUpdates(const std::string& manifestUrl);

        /**
     * @brief Get all bundles that have updates available.
     */
        std::vector<AssetBundle> GetAvailableUpdates() const;

        /**
     * @brief Queue a bundle for download.
     * @param bundleName Bundle to download.
     * @param priority   Download priority (higher = sooner).
     * @return true if the bundle was found and queued.
     */
        bool QueueDownload(const std::string& bundleName, int priority = 0);

        /**
     * @brief Process the download queue (call per frame).
     * @param deltaTime Frame time.
     */
        void Update(float deltaTime);

        /**
     * @brief Get overall download progress.
     * @return Progress 0.0 to 1.0.
     */
        float GetOverallProgress() const;

        /** @brief Get the current download queue. */
        std::vector<DownloadTask> GetDownloadQueue() const;

        /** @brief Cancel all pending downloads. */
        void CancelAll();

        /**
     * @brief Verify integrity of an installed bundle.
     * @param bundleName Bundle to verify.
     * @return true if checksum matches.
     */
        bool VerifyBundle(const std::string& bundleName) const;

        /**
     * @brief Delete a locally installed bundle.
     * @param bundleName Bundle to delete.
     */
        void DeleteBundle(const std::string& bundleName);

        /** @brief Get local install path for bundles. */
        void SetInstallPath(const std::string& path) { m_installPath = path; }

        // --- Callbacks ---

        /** @brief Register download progress callback. */
        void OnProgress(std::function<void(const std::string&, float)> callback);

        /** @brief Register download completion callback. */
        void OnComplete(std::function<void(const std::string&, bool)> callback);

        // --- Console integration ---

        /** @brief Get content delivery status (console integration). */
        std::string Console_GetStatus() const;

      private:
        PatchManifest m_remoteManifest;
        std::unordered_map<std::string, AssetBundle> m_localBundles;
        std::vector<DownloadTask> m_downloadQueue;
        std::string m_installPath = "Data/Bundles/";
        std::vector<std::function<void(const std::string&, float)>> m_progressCallbacks;
        std::vector<std::function<void(const std::string&, bool)>> m_completeCallbacks;
        mutable std::mutex m_mutex;
    };

} // namespace Spark
