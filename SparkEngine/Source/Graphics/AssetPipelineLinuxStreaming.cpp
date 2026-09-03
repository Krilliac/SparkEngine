/**
 * @file AssetPipelineLinuxStreaming.cpp
 * @brief Linux background streaming, metrics, and RHI mesh upload for AssetPipeline
 *
 * Holds the streaming-thread control, loading-thread body, metrics collection,
 * and the RHI vertex/index buffer upload step. Split from AssetPipelineLinux.cpp,
 * which keeps the pipeline lifecycle and load/unload/cache entry points. The
 * Windows counterpart lives in AssetPipelineWindows.cpp.
 */

#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "GraphicsEngineRHI.h"
#include "RHI/RHIResources.h"
#include "../Utils/LogMacros.h"

// ============================================================================
// STREAMING CONTROL (Linux)
// ============================================================================

void AssetPipeline::EnableBackgroundStreaming(bool enabled)
{
    m_backgroundStreaming = enabled;
}

void AssetPipeline::SetStreamingThreadCount(int count)
{
    // Stop existing threads. Publish the stop flag under the waiter's mutex: a worker that has already
    // evaluated its wait predicate but not yet blocked would otherwise miss this
    // notification and the join below would hang.
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_shouldStop = true;
    }
    m_queueCondition.notify_all();
    for (auto& thread : m_loadingThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    m_loadingThreads.clear();

    // Start new threads if requested
    if (count > 0 && m_backgroundStreaming)
    {
        m_shouldStop = false;
        for (int i = 0; i < count; ++i)
        {
            m_loadingThreads.emplace_back(&AssetPipeline::LoadingThreadFunction, this);
        }
    }
}

// ============================================================================
// METRICS (Linux)
// ============================================================================

AssetPipeline::AssetMetrics AssetPipeline::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

// ============================================================================
// PRIVATE HELPERS (Linux)
// ============================================================================

void AssetPipeline::LoadingThreadFunction()
{
    while (!m_shouldStop)
    {
        AssetLoadRequest request;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCondition.wait(lock, [this] { return m_shouldStop || !m_loadQueue.empty(); });

            if (m_shouldStop && m_loadQueue.empty())
                return;
            if (m_loadQueue.empty())
                continue;

            request = std::move(m_loadQueue.front());
            m_loadQueue.pop();
        }

        auto asset = LoadAsset(request.assetPath, request.expectedType);
        if (asset && request.onLoaded)
        {
            request.onLoaded(asset);
        }
        else if (!asset && request.onError)
        {
            request.onError("Failed to load asset: " + request.assetPath);
            std::lock_guard<std::mutex> lock(m_metricsMutex);
            m_metrics.failedLoads++;
        }
    }
}

void AssetPipeline::UpdateMetrics()
{
    // Collect data that requires m_assetsMutex first (consistent lock ordering:
    // assets -> metrics, matching LoadAsset and other call-sites).
    uint32_t totalAssets = 0;
    uint32_t loadedAssets = 0;
    size_t memoryUsage = 0;
    {
        std::lock_guard<std::mutex> assetsLock(m_assetsMutex);
        totalAssets = static_cast<uint32_t>(m_assets.size());
        for (const auto& pair : m_assets)
        {
            if (pair.second && pair.second->IsLoaded())
            {
                loadedAssets++;
                memoryUsage += pair.second->GetMemoryUsage();
            }
        }
    }

    uint32_t pendingRequests = 0;
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        pendingRequests = static_cast<uint32_t>(m_loadQueue.size());
    }

    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.totalAssets = totalAssets;
    m_metrics.loadedAssets = loadedAssets;
    m_metrics.memoryUsage = memoryUsage;

    if (m_metrics.memoryUsage > m_metrics.maxMemoryUsage)
    {
        m_metrics.maxMemoryUsage = m_metrics.memoryUsage;
    }

    m_metrics.pendingRequests = pendingRequests;

    if (m_cache)
    {
        m_metrics.cacheHitRatio = m_cache->GetHitRatio();
    }
    m_metrics.backgroundLoading = m_backgroundStreaming;
    m_metrics.streamingThreads = static_cast<uint32_t>(m_loadingThreads.size());
}

// ============================================================================
// RHI BUFFER UPLOAD (Linux / macOS)
// ============================================================================
// Moves the freshly-loaded CPU-side vertex / index data from `MeshAsset` onto
// the RHI device. This is what lets the non-Windows `ProcessDrawList` /
// `BindMesh` / `DrawBoundMesh` path actually issue draws — the MeshAsset's
// D3D11 ComPtr buffers are stubs on Linux, so without this step the render
// path has nothing to bind.
//
// The bridge singleton is shared across all GraphicsEngine TUs
// (`LinuxRHIState::bridge`); we reach for it directly instead of threading
// a pointer through the async load queue, which would force AssetPipeline
// to know about GraphicsEngine's internals.

void AssetPipeline::BuildRHIBuffersForMesh(MeshAsset& mesh)
{
    auto& rhi = Spark::Graphics::Detail::GetRHI();
    if (!rhi.initialized)
    {
        // Pre-graphics init (e.g. tests loading assets directly) — keep the
        // mesh data on the CPU side and let a later `BuildRHIBuffersForMesh`
        // retry fill the buffers when the bridge is up. Not a warning because
        // headless tooling legitimately uses this path.
        return;
    }

    const MeshAssetData& meshData = mesh.GetMeshData();
    if (meshData.vertices.empty() || meshData.indices.empty())
    {
        // Nothing to upload — a format loader may have returned success on an
        // empty stub asset. Leave the RHI buffers as nullptr; the render path
        // guards on `GetRHIVertexBuffer() != nullptr` before dispatching.
        return;
    }

    const uint64_t vbSize = static_cast<uint64_t>(meshData.vertices.size() * sizeof(MeshAssetData::Vertex));
    const uint32_t vbStride = static_cast<uint32_t>(sizeof(MeshAssetData::Vertex));
    auto vb = rhi.bridge.CreateVertexBuffer(meshData.vertices.data(), vbSize, vbStride);

    const uint64_t ibSize = static_cast<uint64_t>(meshData.indices.size() * sizeof(uint32_t));
    constexpr uint32_t ibStride = sizeof(uint32_t);
    auto ib = rhi.bridge.CreateIndexBuffer(meshData.indices.data(), ibSize, ibStride);

    if (!vb || !ib)
    {
        // CreateVertexBuffer / CreateIndexBuffer only return null on OOM or
        // backend failure. Warn once per mesh so CI logs surface the issue
        // but the asset stays usable (the draw path's null guard keeps the
        // frame rendering, minus this mesh).
        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                       "AssetPipeline::BuildRHIBuffersForMesh: RHI buffer creation failed for '%s' "
                       "(vb=%p, ib=%p)",
                       mesh.GetPath().c_str(), static_cast<void*>(vb.get()), static_cast<void*>(ib.get()));
        return;
    }

    mesh.SetRHIBuffers(std::move(vb), std::move(ib));
}

#endif // !SPARK_PLATFORM_WINDOWS
