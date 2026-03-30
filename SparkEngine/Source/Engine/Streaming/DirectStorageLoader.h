/**
 * @file DirectStorageLoader.h
 * @brief GPU-direct asset loading via DirectStorage (Windows 11+)
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides hardware-accelerated asset streaming that bypasses CPU
 * decompression. On Windows 11+ with NVMe SSDs, DirectStorage can
 * deliver 10-100x faster texture/mesh loading by decompressing directly
 * on the GPU. Falls back to traditional async I/O on older systems.
 *
 * @see TextureStreaming.cpp, SeamlessAreaManager.h
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <queue>
#include <vector>

namespace Spark::Streaming
{

    /// @brief Load request priority
    enum class LoadPriority : uint8_t
    {
        Low = 0,     ///< Background prefetch
        Normal = 1,  ///< Standard asset loading
        High = 2,    ///< Needed this frame or next
        Critical = 3 ///< Blocking load (stall if necessary)
    };

    /// @brief Destination for loaded data
    enum class LoadDestination : uint8_t
    {
        CPUMemory,  ///< Load into CPU-accessible buffer
        GPUTexture, ///< Load directly into GPU texture (GPU decompression)
        GPUBuffer   ///< Load directly into GPU buffer
    };

    /// @brief Completion status for async loads
    enum class LoadStatus : uint8_t
    {
        Pending,    ///< Not yet started
        InProgress, ///< I/O in flight
        Completed,  ///< Successfully loaded
        Failed      ///< I/O error or decompression failure
    };

    /// @brief Opaque handle to a pending load request
    struct LoadRequestHandle
    {
        uint64_t id = 0;
        bool IsValid() const { return id != 0; }
    };

    /// @brief Callback invoked when a load completes
    using LoadCompletionCallback = std::function<void(LoadRequestHandle handle, LoadStatus status)>;

    /// @brief Describes a single load request
    struct LoadRequest
    {
        std::string filePath;    ///< Path to asset file
        uint64_t fileOffset = 0; ///< Byte offset within file
        uint64_t loadSize = 0;   ///< Bytes to read (0 = entire file)
        LoadDestination destination = LoadDestination::CPUMemory;
        LoadPriority priority = LoadPriority::Normal;
        LoadCompletionCallback callback; ///< Called on completion (may be null)

        // GPU destination (used when destination != CPUMemory)
        void* gpuResource = nullptr;   ///< ID3D12Resource* or ID3D11Texture2D*, etc.
        uint32_t subresourceIndex = 0; ///< Mip level or array slice
    };

    /// @brief Performance statistics
    struct DirectStorageStats
    {
        uint64_t totalBytesLoaded = 0;
        uint64_t totalRequests = 0;
        uint64_t pendingRequests = 0;
        uint64_t failedRequests = 0;
        float averageBandwidthMBps = 0.0f;
        bool usingDirectStorage = false;    ///< true if hardware path active
        bool usingGPUDecompression = false; ///< true if GPU decompression available
    };

    /**
     * @brief GPU-direct asset loader with DirectStorage acceleration
     *
     * On Windows 11+ with supported hardware, uses DirectStorage for
     * GPU-direct I/O with hardware decompression. Otherwise falls back
     * to standard async file I/O with background threads.
     *
     * Usage:
     * @code
     *   auto& loader = DirectStorageLoader::GetInstance();
     *   loader.Initialize(device);
     *
     *   LoadRequest req;
     *   req.filePath = "textures/diffuse.dds";
     *   req.destination = LoadDestination::GPUTexture;
     *   req.gpuResource = myTexture;
     *   req.callback = [](auto handle, auto status) { ... };
     *
     *   auto handle = loader.Submit(req);
     *   loader.Flush(); // submit batch to hardware
     * @endcode
     */
    class DirectStorageLoader
    {
      public:
        static DirectStorageLoader& GetInstance()
        {
            static DirectStorageLoader instance;
            return instance;
        }

        /// @brief Initialize the loader
        /// @param graphicsDevice Native graphics device (ID3D12Device* or nullptr for CPU-only)
        /// @return true if initialized successfully
        bool Initialize(void* graphicsDevice = nullptr);

        /// @brief Shutdown and release all resources
        void Shutdown();

        /// @brief Submit a load request
        /// @return Handle for tracking the request
        LoadRequestHandle Submit(const LoadRequest& request);

        /// @brief Submit all pending requests to hardware
        /// @details Call after batching multiple Submit() calls for optimal throughput
        void Flush();

        /// @brief Poll for completed requests and invoke callbacks
        /// @details Call once per frame from the main loop
        void ProcessCompletions();

        /// @brief Cancel a pending request
        /// @return true if the request was cancelled before completion
        bool Cancel(LoadRequestHandle handle);

        /// @brief Check the status of a request
        LoadStatus GetStatus(LoadRequestHandle handle) const;

        /// @brief Get the loaded data buffer (only valid for CPUMemory destination after completion)
        /// @return Pointer to loaded data, or nullptr if not ready
        const void* GetLoadedData(LoadRequestHandle handle, uint64_t* outSize = nullptr) const;

        /// @brief Release the loaded data buffer (frees CPU memory)
        void ReleaseLoadedData(LoadRequestHandle handle);

        /// @brief Get performance statistics
        const DirectStorageStats& GetStats() const { return m_stats; }

        /// @brief Check if hardware DirectStorage is available
        bool IsHardwareAvailable() const { return m_stats.usingDirectStorage; }

        /// @brief Console status report
        std::string Console_GetStatus() const;

      private:
        DirectStorageLoader() = default;
        ~DirectStorageLoader();
        DirectStorageLoader(const DirectStorageLoader&) = delete;
        DirectStorageLoader& operator=(const DirectStorageLoader&) = delete;

        struct InternalRequest
        {
            LoadRequest request;
            LoadRequestHandle handle;
            LoadStatus status = LoadStatus::Pending;
            std::vector<uint8_t> cpuData;
        };

        /// @brief Fallback: async file I/O via background thread
        void FallbackAsyncLoad(std::shared_ptr<InternalRequest> req);

        mutable std::mutex m_mutex;
        std::vector<std::shared_ptr<InternalRequest>> m_activeRequests;
        std::queue<std::shared_ptr<InternalRequest>> m_pendingQueue;
        DirectStorageStats m_stats;
        uint64_t m_nextHandleId = 1;
        bool m_initialized = false;
    };

} // namespace Spark::Streaming
