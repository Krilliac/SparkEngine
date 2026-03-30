/**
 * @file TransientResourcePool.h
 * @brief Age-based transient GPU resource pooling for the render graph
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages a pool of GPU resources that are allocated and released each frame
 * by render graph passes. Resources are recycled based on descriptor matching
 * and garbage-collected when idle for too many frames.
 *
 * @see RenderGraphBuilder, RenderGraph
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Describes a transient GPU resource for pool matching.
     */
    struct TransientResourceDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;   ///< DXGI_FORMAT or engine-specific format enum
        uint32_t flags = 0;    ///< Usage flags (render target, UAV, etc.)
        std::string debugName; ///< Debug name (not used for matching)

        /// @brief Equality comparison (excludes debugName).
        bool operator==(const TransientResourceDesc& other) const
        {
            return width == other.width && height == other.height && format == other.format && flags == other.flags;
        }
    };

    /**
     * @brief A pooled resource entry with age tracking.
     */
    struct PooledResource
    {
        TransientResourceDesc desc;
        uint64_t handle = 0;
        uint32_t lastUsedFrame = 0;
        bool inUse = false;
    };

    /**
     * @brief Transient resource pool with age-based garbage collection.
     *
     * Render graph passes acquire and release resources each frame. The pool
     * reuses resources with matching descriptors and destroys resources that
     * have been idle for more than maxIdleFrames.
     *
     * ## Usage
     * @code
     *   auto& pool = TransientResourcePool::GetInstance();
     *   pool.Initialize(4);
     *
     *   pool.BeginFrame(frameIndex);
     *   uint64_t handle = pool.AcquireResource(desc);
     *   // ... use resource ...
     *   pool.ReleaseResource(handle);
     *   pool.GarbageCollect();
     * @endcode
     */
    class TransientResourcePool
    {
      public:
        /// @brief Get the singleton instance.
        static TransientResourcePool& GetInstance()
        {
            static TransientResourcePool instance;
            return instance;
        }

        TransientResourcePool(const TransientResourcePool&) = delete;
        TransientResourcePool& operator=(const TransientResourcePool&) = delete;

        /**
         * @brief Initialize the pool.
         * @param maxIdleFrames Number of frames a resource can be idle before being destroyed.
         * @return True if initialization succeeded.
         */
        bool Initialize(uint32_t maxIdleFrames = 4);

        /**
         * @brief Acquire a resource matching the given descriptor.
         *
         * Returns an existing pooled resource if one with a matching descriptor
         * is available, otherwise creates a new one.
         *
         * @param desc Resource descriptor to match.
         * @return Handle to the acquired resource.
         */
        uint64_t AcquireResource(const TransientResourceDesc& desc);

        /**
         * @brief Release a resource back to the pool (does not destroy it).
         * @param handle Resource handle previously returned by AcquireResource.
         */
        void ReleaseResource(uint64_t handle);

        /**
         * @brief Mark the start of a new frame for age tracking.
         * @param frameIndex Current frame number.
         */
        void BeginFrame(uint32_t frameIndex);

        /**
         * @brief Destroy resources that have been idle for more than maxIdleFrames.
         */
        void GarbageCollect();

        /// @brief Total number of resources in the pool (active + idle).
        uint32_t GetPooledResourceCount() const;

        /// @brief Number of resources currently acquired (in use).
        uint32_t GetActiveResourceCount() const;

        /// @brief Estimated total memory usage in bytes.
        size_t GetEstimatedMemoryUsage() const;

        /// @brief Console status string for debugging.
        std::string Console_GetStatus() const;

        /// @brief Release all resources and reset the pool.
        void Shutdown();

      private:
        TransientResourcePool() = default;

        /// @brief Estimate bytes for a single resource based on its descriptor.
        static size_t EstimateResourceSize(const TransientResourceDesc& desc);

        bool m_initialized = false;
        uint32_t m_maxIdleFrames = 4;
        uint32_t m_currentFrame = 0;
        uint64_t m_nextHandle = 1;

        std::vector<PooledResource> m_resources;
    };

} // namespace Spark::Graphics
