/**
 * @file AreaAssetLoader.cpp
 * @brief Bridges area streaming with async I/O via DirectStorageLoader
 */

#include "AreaAssetLoader.h"
#include "../../Utils/LogMacros.h"

#include <format>

namespace Spark::Streaming
{

    // ========================================================================
    // Lifecycle
    // ========================================================================

    void AreaAssetLoader::Initialize()
    {
        if (m_initialized)
            return;

        m_manifests.clear();
        m_loadStates.clear();
        m_initialized = true;

        SPARK_LOG_INFO(Spark::LogCategory::Scene, "AreaAssetLoader initialized");
    }

    void AreaAssetLoader::Shutdown()
    {
        if (!m_initialized)
            return;

        // Cancel any in-flight loads
        auto& loader = DirectStorageLoader::GetInstance();
        for (auto& [areaId, state] : m_loadStates)
        {
            if (state.loading)
            {
                for (const auto& handle : state.handles)
                {
                    loader.Cancel(handle);
                    loader.ReleaseLoadedData(handle);
                }
            }
        }

        m_loadStates.clear();
        m_manifests.clear();
        m_initialized = false;

        SPARK_LOG_INFO(Spark::LogCategory::Scene, "AreaAssetLoader shutdown");
    }

    void AreaAssetLoader::Update()
    {
        if (!m_initialized)
            return;

        // Poll DirectStorageLoader for completed requests
        DirectStorageLoader::GetInstance().ProcessCompletions();

        // Check each loading area for completion
        for (auto& [areaId, state] : m_loadStates)
        {
            if (state.loading)
            {
                CheckAreaCompletion(areaId);
            }
        }
    }

    // ========================================================================
    // Manifest Management
    // ========================================================================

    void AreaAssetLoader::SetManifest(AreaID areaId, SceneManifest manifest)
    {
        m_manifests[areaId] = std::move(manifest);
    }

    void AreaAssetLoader::RemoveManifest(AreaID areaId)
    {
        m_manifests.erase(areaId);
    }

    bool AreaAssetLoader::HasManifest(AreaID areaId) const
    {
        return m_manifests.contains(areaId);
    }

    // ========================================================================
    // Loading Operations
    // ========================================================================

    void AreaAssetLoader::BeginAreaLoad(AreaID areaId, AreaLoadCompleteCallback onComplete)
    {
        auto manifestIt = m_manifests.find(areaId);

        // No manifest or empty manifest — complete immediately
        if (manifestIt == m_manifests.end() || manifestIt->second.TotalAssetCount() == 0)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Scene, "AreaAssetLoader: area %u has no assets, completing immediately",
                            areaId);
            if (onComplete)
                onComplete(areaId);
            return;
        }

        const auto& manifest = manifestIt->second;
        auto allPaths = manifest.AllPaths();

        AreaLoadState state;
        state.manifest = manifest;
        state.totalAssets = static_cast<uint32_t>(allPaths.size());
        state.completedAssets = 0;
        state.failedAssets = 0;
        state.onComplete = std::move(onComplete);
        state.loading = true;

        auto& loader = DirectStorageLoader::GetInstance();

        // Submit a load request for each asset
        for (const auto& path : allPaths)
        {
            LoadRequest req;
            req.filePath = path;
            req.destination = LoadDestination::CPUMemory;
            req.priority = LoadPriority::Normal;

            // Track completion per handle via callback
            req.callback = [this, areaId](LoadRequestHandle /*handle*/, LoadStatus status)
            {
                auto it = m_loadStates.find(areaId);
                if (it == m_loadStates.end())
                    return;

                if (status == LoadStatus::Completed)
                    it->second.completedAssets++;
                else
                    it->second.failedAssets++;
            };

            auto handle = loader.Submit(req);
            state.handles.push_back(handle);
        }

        m_loadStates[areaId] = std::move(state);

        // Flush all submitted requests to start I/O
        loader.Flush();

        SPARK_LOG_INFO(Spark::LogCategory::Scene, "AreaAssetLoader: began loading area %u (%u assets)", areaId,
                       static_cast<uint32_t>(allPaths.size()));
    }

    void AreaAssetLoader::BeginAreaUnload(AreaID areaId, AreaLoadCompleteCallback onComplete)
    {
        auto it = m_loadStates.find(areaId);
        if (it != m_loadStates.end())
        {
            auto& loader = DirectStorageLoader::GetInstance();

            // Cancel pending loads and release completed data
            for (const auto& handle : it->second.handles)
            {
                loader.Cancel(handle);
                loader.ReleaseLoadedData(handle);
            }

            m_loadStates.erase(it);
            SPARK_LOG_INFO(Spark::LogCategory::Scene, "AreaAssetLoader: unloaded area %u", areaId);
        }

        // Unload is synchronous — callback fires immediately
        if (onComplete)
            onComplete(areaId);
    }

    // ========================================================================
    // Queries
    // ========================================================================

    bool AreaAssetLoader::IsAreaLoadComplete(AreaID areaId) const
    {
        auto it = m_loadStates.find(areaId);
        if (it == m_loadStates.end())
            return false;

        const auto& state = it->second;
        return !state.loading || (state.completedAssets + state.failedAssets >= state.totalAssets);
    }

    size_t AreaAssetLoader::GetLoadedAssetCount(AreaID areaId) const
    {
        auto it = m_loadStates.find(areaId);
        if (it == m_loadStates.end())
            return 0;
        return it->second.completedAssets;
    }

    float AreaAssetLoader::GetAreaLoadProgress(AreaID areaId) const
    {
        auto it = m_loadStates.find(areaId);
        if (it == m_loadStates.end())
            return 0.0f;

        const auto& state = it->second;
        if (state.totalAssets == 0)
            return 1.0f;

        return static_cast<float>(state.completedAssets + state.failedAssets) / static_cast<float>(state.totalAssets);
    }

    std::string AreaAssetLoader::Console_GetStatus() const
    {
        std::string result = "AreaAssetLoader:\n";
        result += std::format("  Manifests registered: {}\n", m_manifests.size());
        result += std::format("  Areas with load state: {}\n", m_loadStates.size());

        for (const auto& [areaId, state] : m_loadStates)
        {
            result += std::format("  [{}] {}/{} assets ({}loading)\n", areaId, state.completedAssets, state.totalAssets,
                                  state.loading ? "" : "not ");
        }

        return result;
    }

    // ========================================================================
    // Internal
    // ========================================================================

    void AreaAssetLoader::CheckAreaCompletion(AreaID areaId)
    {
        auto it = m_loadStates.find(areaId);
        if (it == m_loadStates.end() || !it->second.loading)
            return;

        auto& state = it->second;
        uint32_t finished = state.completedAssets + state.failedAssets;

        if (finished >= state.totalAssets)
        {
            state.loading = false;

            if (state.failedAssets > 0)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Scene, "AreaAssetLoader: area %u loaded with %u/%u failures", areaId,
                               state.failedAssets, state.totalAssets);
            }
            else
            {
                SPARK_LOG_INFO(Spark::LogCategory::Scene, "AreaAssetLoader: area %u fully loaded (%u assets)", areaId,
                               state.totalAssets);
            }

            if (state.onComplete)
                state.onComplete(areaId);
        }
    }

} // namespace Spark::Streaming
