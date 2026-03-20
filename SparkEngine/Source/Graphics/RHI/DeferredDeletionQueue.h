/**
 * @file DeferredDeletionQueue.h
 * @brief Frame-delayed GPU resource destruction queue
 *
 * Prevents use-after-free when resources are destroyed while still
 * referenced by in-flight GPU command lists. Resources are queued for
 * deletion and actually freed N frames later, after the GPU has finished
 * all work that could reference them.
 *
 * Inspired by bgfx's frame-delayed destruction and Filament's handle-based
 * resource management.
 *
 * @see IRHIDevice::BeginFrame(), IRHIDevice::EndFrame()
 */

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace Spark::RHI
{

    /**
     * @brief Queue that defers resource destruction by a configurable frame count.
     *
     * Usage:
     * 1. Call Enqueue() instead of `delete resource` in Destroy*() methods.
     * 2. Call ProcessQueue() once per frame in BeginFrame() or EndFrame().
     *
     * Thread-safe: Enqueue() can be called from any thread.
     */
    class DeferredDeletionQueue
    {
      public:
        /**
         * @param framesToDefer Number of frames to wait before actually deleting.
         *                     Default: 3 (safe for triple-buffered rendering).
         */
        explicit DeferredDeletionQueue(uint32_t framesToDefer = 3) : m_framesToDefer(framesToDefer) {}

        /**
         * @brief Enqueue a resource for deferred deletion.
         *
         * @param deletionFunc  Lambda that performs the actual deletion (e.g., `[p]{ delete p; }`).
         *                      Captured by value — must own the resource pointer.
         */
        void Enqueue(std::function<void()> deletionFunc)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.push_back({std::move(deletionFunc), m_currentFrame + m_framesToDefer});
        }

        /**
         * @brief Process the queue, deleting resources whose defer period has elapsed.
         *
         * Call once per frame from BeginFrame() or EndFrame(). Increments the
         * internal frame counter.
         */
        void ProcessQueue()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_currentFrame++;

            // Partition: move expired entries to the front
            auto it = m_pending.begin();
            while (it != m_pending.end())
            {
                if (m_currentFrame >= it->deleteAtFrame)
                {
                    it->deletionFunc();
                    it = m_pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        /**
         * @brief Flush all pending deletions immediately (e.g., at shutdown).
         *
         * Ignores the frame counter — deletes everything now.
         */
        void FlushAll()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& entry : m_pending)
            {
                entry.deletionFunc();
            }
            m_pending.clear();
        }

        /** @brief Number of resources currently awaiting deletion. */
        size_t GetPendingCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_pending.size();
        }

        /** @brief Get the current internal frame counter. */
        uint64_t GetCurrentFrame() const { return m_currentFrame; }

      private:
        struct DeferredEntry
        {
            std::function<void()> deletionFunc;
            uint64_t deleteAtFrame = 0;
        };

        mutable std::mutex m_mutex;
        std::vector<DeferredEntry> m_pending;
        uint64_t m_currentFrame = 0;
        uint32_t m_framesToDefer = 3;
    };

} // namespace Spark::RHI
