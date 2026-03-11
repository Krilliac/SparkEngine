/**
 * @file ThreadSafeQueue.h
 * @brief Lock-based thread-safe queue for producer/consumer patterns
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a bounded FIFO queue safe for concurrent push/pop from multiple
 * threads. Used by NetworkManager for message I/O, JobSystem for work
 * distribution, and AudioEngine for command buffering.
 *
 * ## Usage
 * @code
 *   Spark::ThreadSafeQueue<NetworkMessage> messageQueue(1024);
 *
 *   // Producer thread
 *   messageQueue.Push(NetworkMessage{...});
 *
 *   // Consumer thread
 *   NetworkMessage msg;
 *   if (messageQueue.TryPop(msg)) {
 *       ProcessMessage(msg);
 *   }
 *
 *   // Drain all pending items
 *   std::vector<NetworkMessage> batch;
 *   messageQueue.PopAll(batch);
 * @endcode
 */

#pragma once

#include <cstddef>
#include <mutex>
#include <queue>
#include <vector>

namespace Spark
{

    /**
     * @class ThreadSafeQueue
     * @brief Mutex-guarded FIFO queue with optional capacity limit.
     *
     * All public methods are thread-safe. Push blocks callers if the queue
     * is at capacity (returns false for TryPush). Pop methods never block;
     * they return false/empty if no items are available.
     *
     * @tparam T  Element type. Must be move-constructible.
     */
    template <typename T> class ThreadSafeQueue
    {
      public:
        /**
         * @brief Construct a queue with an optional maximum size.
         * @param maxSize  Maximum elements (0 = unlimited).
         */
        explicit ThreadSafeQueue(size_t maxSize = 0) : m_maxSize(maxSize) {}

        /**
         * @brief Push an element. Fails if at capacity.
         * @param item  Item to enqueue (moved).
         * @return      true if the item was enqueued, false if full.
         */
        bool TryPush(T item)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_maxSize > 0 && m_queue.size() >= m_maxSize)
                return false;
            m_queue.push(std::move(item));
            return true;
        }

        /**
         * @brief Push an element, always succeeding (ignores capacity limit).
         * @param item  Item to enqueue (moved).
         */
        void ForcePush(T item)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(std::move(item));
        }

        /**
         * @brief Try to pop the front element.
         * @param[out] item  Receives the popped item if successful.
         * @return           true if an item was dequeued, false if empty.
         */
        bool TryPop(T& item)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty())
                return false;
            item = std::move(m_queue.front());
            m_queue.pop();
            return true;
        }

        /**
         * @brief Drain all items into the provided vector.
         *
         * Appends all queued items to `out` and leaves the queue empty.
         * Efficient for batch processing on the consumer thread.
         *
         * @param[out] out  Vector to receive all items.
         */
        void PopAll(std::vector<T>& out)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            out.reserve(out.size() + m_queue.size());
            while (!m_queue.empty())
            {
                out.push_back(std::move(m_queue.front()));
                m_queue.pop();
            }
        }

        /**
         * @brief Get the current number of elements.
         * @return  Queue size (snapshot; may change immediately after return).
         */
        size_t Size() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.size();
        }

        /**
         * @brief Check if the queue is empty.
         * @return  true if empty at the time of the call.
         */
        bool IsEmpty() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.empty();
        }

        /**
         * @brief Remove all elements from the queue.
         */
        void Clear()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::queue<T> empty;
            std::swap(m_queue, empty);
        }

      private:
        std::queue<T> m_queue;
        mutable std::mutex m_mutex;
        size_t m_maxSize;
    };

} // namespace Spark
