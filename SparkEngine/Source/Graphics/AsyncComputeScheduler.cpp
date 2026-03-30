/**
 * @file AsyncComputeScheduler.cpp
 * @brief Implementation of the async compute workload scheduler
 * @author Spark Engine Team
 * @date 2026
 *
 * D3D11 path: dispatches compute shaders synchronously on the immediate
 * context. The API surface is identical to what D3D12/Vulkan backends
 * would use, so call sites remain unchanged when adding real async queues.
 */

#include "AsyncComputeScheduler.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <chrono>

namespace Spark::Graphics
{

    // ============================================================================
    // Singleton
    // ============================================================================

    AsyncComputeScheduler& AsyncComputeScheduler::GetInstance()
    {
        static AsyncComputeScheduler instance;
        return instance;
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    bool AsyncComputeScheduler::Initialize()
    {
        if (m_initialized.load(std::memory_order_acquire))
        {
            return true;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        m_pendingQueue.clear();
        m_nextHandleId.store(0, std::memory_order_relaxed);
        m_stats = {};

#ifdef SPARK_PLATFORM_WINDOWS
        m_deviceContext = nullptr;
        m_registeredShaders.clear();
#endif

        m_initialized.store(true, std::memory_order_release);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "AsyncComputeScheduler initialized");
        return true;
    }

    void AsyncComputeScheduler::Shutdown()
    {
        if (!m_initialized.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "AsyncComputeScheduler shutting down (total dispatches: %u)",
                       m_stats.totalDispatches);

        std::lock_guard<std::mutex> lock(m_mutex);

        m_pendingQueue.clear();
        m_stats = {};

#ifdef SPARK_PLATFORM_WINDOWS
        m_deviceContext = nullptr;
        m_registeredShaders.clear();
#endif
    }

    // ============================================================================
    // Platform-specific setup (D3D11)
    // ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

    void AsyncComputeScheduler::SetDeviceContext(ID3D11DeviceContext* context)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceContext = context;
    }

    void AsyncComputeScheduler::RegisterComputeShader(const std::string& name, ID3D11ComputeShader* shader)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Replace existing registration if present
        for (auto& [registeredName, registeredShader] : m_registeredShaders)
        {
            if (registeredName == name)
            {
                registeredShader = shader;
                SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "AsyncCompute: re-registered shader '%s'", name.c_str());
                return;
            }
        }

        m_registeredShaders.emplace_back(name, shader);
        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "AsyncCompute: registered shader '%s' (%zu total)", name.c_str(),
                        m_registeredShaders.size());
    }

    ID3D11ComputeShader* AsyncComputeScheduler::FindShader(const std::string& name) const
    {
        for (const auto& [registeredName, shader] : m_registeredShaders)
        {
            if (registeredName == name)
            {
                return shader;
            }
        }
        return nullptr;
    }

    void AsyncComputeScheduler::DispatchWorkItem(const ComputeWorkItem& item)
    {
        if (m_deviceContext == nullptr)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "AsyncCompute: dispatch skipped, no device context");
            return;
        }

        ID3D11ComputeShader* shader = FindShader(item.shaderName);
        if (shader == nullptr)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "AsyncCompute: shader '%s' not found for dispatch",
                            item.shaderName.c_str());
            return;
        }

        // Bind compute shader
        m_deviceContext->CSSetShader(shader, nullptr, 0);

        // Dispatch the compute work
        m_deviceContext->Dispatch(item.threadGroupsX, item.threadGroupsY, item.threadGroupsZ);

        // Unbind compute shader to avoid resource hazards
        m_deviceContext->CSSetShader(nullptr, nullptr, 0);
    }

#endif // SPARK_PLATFORM_WINDOWS

    // ============================================================================
    // Work submission
    // ============================================================================

    ComputeWorkHandle AsyncComputeScheduler::SubmitComputeWork(const ComputeWorkItem& item)
    {
        if (!m_initialized.load(std::memory_order_acquire))
        {
            return ComputeWorkHandle{};
        }

        ComputeWorkHandle handle;
        handle.id = m_nextHandleId.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingQueue.push_back(PendingWork{handle, item});
        }

        return handle;
    }

    // ============================================================================
    // Flush — dispatch all pending work
    // ============================================================================

    void AsyncComputeScheduler::Flush()
    {
        if (!m_initialized.load(std::memory_order_acquire))
        {
            return;
        }

        auto flushStart = std::chrono::high_resolution_clock::now();

        std::vector<PendingWork> workToDispatch;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            workToDispatch = std::move(m_pendingQueue);
            m_pendingQueue.clear();
        }

        // Sort by priority (highest first)
        std::sort(workToDispatch.begin(), workToDispatch.end(),
                  [](const PendingWork& a, const PendingWork& b) { return a.item.priority > b.item.priority; });

        uint32_t dispatchCount = 0;

        for (const auto& work : workToDispatch)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            DispatchWorkItem(work.item);
#endif
            // On non-Windows platforms, dispatches are no-ops until a backend is wired in.
            // The work item is still tracked for statistics.
            ++dispatchCount;
        }

        auto flushEnd = std::chrono::high_resolution_clock::now();
        float elapsedMs = std::chrono::duration<float, std::milli>(flushEnd - flushStart).count();

        // Update statistics
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stats.dispatchesThisFrame += dispatchCount;
            m_stats.totalDispatches += dispatchCount;
            m_stats.lastFlushTimeMs = elapsedMs;
            m_stats.totalFlushTimeMs += elapsedMs;
            ++m_stats.flushCount;
        }
    }

    // ============================================================================
    // Wait — synchronization point
    // ============================================================================

    void AsyncComputeScheduler::WaitForCompletion()
    {
        // D3D11 immediate context dispatch is synchronous, so nothing to wait on.
        // D3D12/Vulkan backends would fence-wait on the async compute queue here.
    }

    // ============================================================================
    // Frame management
    // ============================================================================

    void AsyncComputeScheduler::BeginFrame()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.dispatchesThisFrame = 0;
        m_stats.pendingItems = static_cast<uint32_t>(m_pendingQueue.size());
    }

    // ============================================================================
    // Statistics and console reporting
    // ============================================================================

    AsyncComputeStats AsyncComputeScheduler::GetStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        AsyncComputeStats stats = m_stats;
        stats.pendingItems = static_cast<uint32_t>(m_pendingQueue.size());
        return stats;
    }

    std::string AsyncComputeScheduler::Console_GetStatus() const
    {
        auto stats = GetStats();

        std::string status = "Async Compute Scheduler:\n";
        status += "  Initialized:       " + std::string(IsInitialized() ? "yes" : "no") + "\n";
        status += "  Total dispatches:  " + std::to_string(stats.totalDispatches) + "\n";
        status += "  Frame dispatches:  " + std::to_string(stats.dispatchesThisFrame) + "\n";
        status += "  Pending items:     " + std::to_string(stats.pendingItems) + "\n";
        status += "  Flush count:       " + std::to_string(stats.flushCount) + "\n";

        float avgFlushMs =
            (stats.flushCount > 0) ? (stats.totalFlushTimeMs / static_cast<float>(stats.flushCount)) : 0.0f;
        status += "  Last flush time:   " + std::to_string(stats.lastFlushTimeMs) + " ms\n";
        status += "  Avg flush time:    " + std::to_string(avgFlushMs) + " ms\n";

#ifdef SPARK_PLATFORM_WINDOWS
        status += "  Backend:           D3D11 (immediate context)\n";
        status += "  Context bound:     " + std::string(m_deviceContext != nullptr ? "yes" : "no") + "\n";
#else
        status += "  Backend:           None (headless)\n";
#endif

        return status;
    }

} // namespace Spark::Graphics
