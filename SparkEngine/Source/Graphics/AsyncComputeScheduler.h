/**
 * @file AsyncComputeScheduler.h
 * @brief Async compute workload scheduler with multi-backend support
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages compute shader workloads that can run in parallel with graphics.
 * On D3D12/Vulkan this would use actual async compute queues; on D3D11 it
 * falls back to immediate-context dispatch (same API, synchronous execution).
 *
 * ## Usage
 * @code
 *   auto& scheduler = Spark::Graphics::AsyncComputeScheduler::GetInstance();
 *   scheduler.Initialize();
 *
 *   ComputeWorkItem item;
 *   item.name = "ParticleSim";
 *   item.shaderName = "CS_ParticleUpdate";
 *   item.threadGroupsX = 64;
 *   item.threadGroupsY = 1;
 *   item.threadGroupsZ = 1;
 *   auto handle = scheduler.SubmitComputeWork(item);
 *
 *   scheduler.Flush();           // dispatch all pending work
 *   scheduler.WaitForCompletion(); // no-op on D3D11
 *   scheduler.Shutdown();
 * @endcode
 *
 * @see RenderGraphTypes.h, GPUSceneBuffer.h
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // ============================================================================
    // ComputeWorkHandle — opaque handle for tracking submitted work
    // ============================================================================

    struct ComputeWorkHandle
    {
        static constexpr uint64_t INVALID = ~0ULL;

        uint64_t id = INVALID;

        constexpr bool IsValid() const { return id != INVALID; }

        constexpr bool operator==(const ComputeWorkHandle& other) const { return id == other.id; }
        constexpr bool operator!=(const ComputeWorkHandle& other) const { return id != other.id; }
    };

    // ============================================================================
    // ComputeWorkItem — describes a single compute dispatch
    // ============================================================================

    struct ComputeWorkItem
    {
        std::string name;       ///< Debug name for this work item
        std::string shaderName; ///< Compute shader identifier

        uint32_t threadGroupsX = 1; ///< Dispatch X dimension
        uint32_t threadGroupsY = 1; ///< Dispatch Y dimension
        uint32_t threadGroupsZ = 1; ///< Dispatch Z dimension

        /// Resource binding slots (SRV register indices to bind before dispatch)
        std::vector<uint32_t> srvSlots;

        /// UAV binding slots (UAV register indices to bind before dispatch)
        std::vector<uint32_t> uavSlots;

        /// Handles this work item depends on (must complete before dispatch)
        std::vector<ComputeWorkHandle> dependencies;

        /// Priority: higher values are dispatched first within a flush
        int32_t priority = 0;
    };

    // ============================================================================
    // AsyncComputeStats — performance metrics
    // ============================================================================

    struct AsyncComputeStats
    {
        uint32_t totalDispatches = 0;     ///< Total dispatches since initialization
        uint32_t dispatchesThisFrame = 0; ///< Dispatches in the current frame
        uint32_t pendingItems = 0;        ///< Items waiting to be flushed
        float lastFlushTimeMs = 0.0f;     ///< Time of the most recent Flush() call
        float totalFlushTimeMs = 0.0f;    ///< Cumulative flush time
        uint32_t flushCount = 0;          ///< Number of Flush() calls
    };

    // ============================================================================
    // AsyncComputeScheduler
    // ============================================================================

    /**
     * @brief Schedules and dispatches async compute workloads.
     *
     * Singleton accessed via GetInstance(). On D3D11 all dispatches are
     * synchronous through the immediate context. The API is designed so that
     * D3D12/Vulkan backends can be added without changing call sites.
     */
    class AsyncComputeScheduler
    {
      public:
        static AsyncComputeScheduler& GetInstance();

        /** @brief Initialize the scheduler. Call once at engine startup. */
        bool Initialize();

        /** @brief Tear down the scheduler and release resources. */
        void Shutdown();

#ifdef SPARK_PLATFORM_WINDOWS
        /** @brief Set the D3D11 device context used for immediate dispatch. */
        void SetDeviceContext(ID3D11DeviceContext* context);

        /** @brief Set a compute shader to use for a given shader name. */
        void RegisterComputeShader(const std::string& name, ID3D11ComputeShader* shader);
#endif

        /**
         * @brief Submit a compute work item to the pending queue.
         * @param item The work item describing the dispatch.
         * @return Handle for tracking the submitted work.
         */
        ComputeWorkHandle SubmitComputeWork(const ComputeWorkItem& item);

        /**
         * @brief Dispatch all pending compute work items.
         *
         * On D3D11 this issues Dispatch() calls on the immediate context.
         * Items are dispatched in priority order (highest first).
         */
        void Flush();

        /**
         * @brief Wait for all dispatched compute work to complete.
         *
         * On D3D11 this is a no-op since immediate dispatch is synchronous.
         * On D3D12/Vulkan this would fence-wait on the async compute queue.
         */
        void WaitForCompletion();

        /** @brief Reset per-frame statistics. Call at frame start. */
        void BeginFrame();

        /** @brief Get current performance statistics. */
        AsyncComputeStats GetStats() const;

        /** @brief Get a human-readable status string for console display. */
        std::string Console_GetStatus() const;

        bool IsInitialized() const { return m_initialized.load(std::memory_order_acquire); }

      private:
        AsyncComputeScheduler() = default;
        ~AsyncComputeScheduler() { Shutdown(); }
        AsyncComputeScheduler(const AsyncComputeScheduler&) = delete;
        AsyncComputeScheduler& operator=(const AsyncComputeScheduler&) = delete;

        struct PendingWork
        {
            ComputeWorkHandle handle;
            ComputeWorkItem item;
        };

        mutable std::mutex m_mutex;
        std::vector<PendingWork> m_pendingQueue;
        std::atomic<uint64_t> m_nextHandleId{0};
        std::atomic<bool> m_initialized{false};

        AsyncComputeStats m_stats{};

#ifdef SPARK_PLATFORM_WINDOWS
        ID3D11DeviceContext* m_deviceContext = nullptr;
        std::vector<std::pair<std::string, ID3D11ComputeShader*>> m_registeredShaders;

        ID3D11ComputeShader* FindShader(const std::string& name) const;
        void DispatchWorkItem(const ComputeWorkItem& item);
#endif
    };

} // namespace Spark::Graphics
