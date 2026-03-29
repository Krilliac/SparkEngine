/**
 * @file IStreamingLoader.h
 * @brief Abstract interface for asset streaming/loading backends
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines the interface for async asset loading. This abstraction decouples
 * engine code from the concrete DirectStorageLoader implementation, enabling
 * platform-specific streaming backends (DirectStorage, standard I/O, etc.)
 * to be swapped through EngineContext.
 */

#pragma once

#include <cstdint>
#include <string>

namespace Spark::Streaming
{

    struct LoadRequest;
    struct LoadRequestHandle;
    struct DirectStorageStats;
    enum class LoadStatus : uint8_t;

    /**
     * @brief Abstract interface for async asset streaming backends.
     *
     * Implementations handle platform-specific I/O details (DirectStorage,
     * async file I/O, etc.) while engine code operates against this uniform API.
     */
    class IStreamingLoader
    {
      public:
        virtual ~IStreamingLoader() = default;

        // ================================================================
        // Lifecycle
        // ================================================================

        /// @brief Initialize the loader.
        /// @param graphicsDevice Native graphics device pointer (or nullptr for CPU-only).
        /// @return true on success.
        virtual bool Initialize(void* graphicsDevice = nullptr) = 0;

        /// @brief Shutdown and release all resources.
        virtual void Shutdown() = 0;

        // ================================================================
        // Request Management
        // ================================================================

        /// @brief Submit a load request.
        /// @return Handle for tracking the request.
        virtual LoadRequestHandle Submit(const LoadRequest& request) = 0;

        /// @brief Submit all pending requests to hardware for optimal throughput.
        virtual void Flush() = 0;

        /// @brief Poll for completed requests and invoke callbacks.
        virtual void ProcessCompletions() = 0;

        /// @brief Cancel a pending request.
        /// @return true if cancelled before completion.
        virtual bool Cancel(LoadRequestHandle handle) = 0;

        /// @brief Check the status of a request.
        virtual LoadStatus GetStatus(LoadRequestHandle handle) const = 0;

        // ================================================================
        // Data Access
        // ================================================================

        /// @brief Get the loaded data buffer (CPUMemory destination only, after completion).
        virtual const void* GetLoadedData(LoadRequestHandle handle, uint64_t* outSize = nullptr) const = 0;

        /// @brief Release the loaded data buffer.
        virtual void ReleaseLoadedData(LoadRequestHandle handle) = 0;

        // ================================================================
        // Query
        // ================================================================

        /// @brief Get performance statistics.
        virtual const DirectStorageStats& GetStats() const = 0;

        /// @brief Check if hardware-accelerated streaming is available.
        virtual bool IsHardwareAvailable() const = 0;
    };

} // namespace Spark::Streaming
