/**
 * @file AreaAssetLoader.h
 * @brief Bridges SeamlessAreaManager with DirectStorageLoader for real asset I/O
 * @author Spark Engine Team
 * @date 2026
 *
 * When SeamlessAreaManager decides an area needs loading, AreaAssetLoader
 * submits async load requests for every asset listed in the area's
 * SceneManifest. It tracks outstanding loads per area and fires a
 * completion callback when all assets are ready.
 *
 * @see SeamlessAreaManager.h, DirectStorageLoader.h, SceneManifest.h
 */

#pragma once

#include "DirectStorageLoader.h"
#include "SceneManifest.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Streaming
{

    /// @brief Unique area identifier (same type as SeamlessAreaManager::AreaID)
    using AreaID = uint32_t;

    /// @brief Called when all assets for an area have finished loading
    using AreaLoadCompleteCallback = std::function<void(AreaID areaId)>;

    /**
     * @brief Tracks loading state for a single area's assets
     */
    struct AreaLoadState
    {
        SceneManifest manifest;
        std::vector<LoadRequestHandle> handles; ///< DirectStorageLoader handles
        uint32_t totalAssets = 0;               ///< Total assets to load
        uint32_t completedAssets = 0;           ///< Assets that finished loading
        uint32_t failedAssets = 0;              ///< Assets that failed to load
        AreaLoadCompleteCallback onComplete;    ///< Fires when all assets done
        bool loading = false;                   ///< Currently loading
    };

    /**
     * @brief Async area asset loader using DirectStorageLoader
     *
     * Submits per-asset load requests and tracks completion. Areas with
     * empty manifests complete immediately. Thread safety: all methods
     * are main-thread only (same as SeamlessAreaManager).
     */
    class AreaAssetLoader
    {
      public:
        AreaAssetLoader() = default;
        ~AreaAssetLoader() = default;

        // -- Lifecycle --

        void Initialize();
        void Shutdown();

        /**
         * @brief Poll DirectStorageLoader for completions and check area readiness
         *
         * Call once per frame, typically from SeamlessAreaManager::Update().
         */
        void Update();

        // -- Manifest management --

        /**
         * @brief Associate a SceneManifest with an area
         * @param areaId The area to set the manifest for
         * @param manifest The asset list for this area
         */
        void SetManifest(AreaID areaId, SceneManifest manifest);

        /**
         * @brief Remove a manifest for an area
         */
        void RemoveManifest(AreaID areaId);

        /**
         * @brief Check if a manifest is registered for an area
         */
        bool HasManifest(AreaID areaId) const;

        // -- Loading operations --

        /**
         * @brief Begin async loading of all assets in an area's manifest
         *
         * Submits load requests to DirectStorageLoader for each asset.
         * If the manifest is empty, the callback fires immediately.
         *
         * @param areaId Area to load
         * @param onComplete Called when all assets finish (success or failure)
         */
        void BeginAreaLoad(AreaID areaId, AreaLoadCompleteCallback onComplete);

        /**
         * @brief Cancel and release all loaded data for an area
         * @param areaId Area to unload
         * @param onComplete Called when unload is done (synchronous, fires immediately)
         */
        void BeginAreaUnload(AreaID areaId, AreaLoadCompleteCallback onComplete);

        // -- Queries --

        /**
         * @brief Check if an area's load is fully complete
         */
        bool IsAreaLoadComplete(AreaID areaId) const;

        /**
         * @brief Get the number of loaded assets for an area
         */
        size_t GetLoadedAssetCount(AreaID areaId) const;

        /**
         * @brief Get loading progress for an area (0.0 to 1.0)
         */
        float GetAreaLoadProgress(AreaID areaId) const;

        /**
         * @brief Get diagnostic status string
         */
        std::string Console_GetStatus() const;

      private:
        void CheckAreaCompletion(AreaID areaId);

        std::unordered_map<AreaID, SceneManifest> m_manifests;
        std::unordered_map<AreaID, AreaLoadState> m_loadStates;
        bool m_initialized = false;
    };

} // namespace Spark::Streaming
