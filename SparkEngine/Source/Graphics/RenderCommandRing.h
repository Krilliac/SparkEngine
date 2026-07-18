/**
 * @file RenderCommandRing.h
 * @brief Thread-decoupled render command ring buffer
 *
 * Decouples the game thread from the render thread via a typed command
 * queue with power-of-2
 * ring sizing and bitmask wrapping.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Spark
{
    namespace Graphics
    {

        /**
         * @brief Command types that can be posted to the render thread.
         */
        enum class RenderCommandType : uint8_t
        {
            Nop,
            BeginFrame,
            EndFrame,
            SetViewport,
            SetScissor,
            DrawIndexed,
            Dispatch,
            ResourceBarrier,
            CopyResource,
            ClearTarget,
            Lambda,
            VSync,
            Shutdown,
            Count
        };

        /**
         * @brief A single command entry in the ring buffer.
         *
         * Fixed-size for cache-friendly ring traversal. Complex commands
         * use the Lambda type with a stored callback.
         */
        struct RenderCommand
        {
            RenderCommandType type = RenderCommandType::Nop;
            uint32_t param0 = 0;
            uint32_t param1 = 0;
            uint32_t param2 = 0;
            uint32_t param3 = 0;
            float fParam0 = 0.0f;
            float fParam1 = 0.0f;
            float fParam2 = 0.0f;
            float fParam3 = 0.0f;
            std::function<void()> lambda;
        };

        /**
         * @brief Lock-free SPSC ring buffer for render commands.
         *
         * The game thread writes commands; the render thread reads them.
         * Ring size is a power of 2 for fast bitmask wrapping.
         *
         * @tparam SizeLog2 Log2 of ring capacity (default: 14 = 16384 entries)
         */
        template <uint32_t SizeLog2 = 14> class RenderCommandRing
        {
          public:
            static constexpr uint32_t CAPACITY = 1u << SizeLog2;
            static constexpr uint32_t MASK = CAPACITY - 1;

            RenderCommandRing() = default;

            /**
             * @brief Post a command to the ring buffer (game thread).
             *
             * @return true if the command was queued, false if ring is full.
             */
            bool Post(const RenderCommand& cmd)
            {
                uint32_t w = m_writePos.load(std::memory_order_relaxed);
                uint32_t r = m_readPos.load(std::memory_order_acquire);

                if (w - r >= CAPACITY)
                    return false; // Ring full

                m_ring[w & MASK] = cmd;
                m_writePos.store(w + 1, std::memory_order_release);
                m_totalPosted.fetch_add(1, std::memory_order_relaxed);
                return true;
            }

            /**
             * @brief Post a lambda to execute on the render thread.
             */
            bool PostLambda(std::function<void()> fn)
            {
                RenderCommand cmd;
                cmd.type = RenderCommandType::Lambda;
                cmd.lambda = std::move(fn);
                return Post(cmd);
            }

            /**
             * @brief Consume the next command (render thread).
             *
             * @param outCmd Output command.
             * @return true if a command was available, false if ring is empty.
             */
            bool Consume(RenderCommand& outCmd)
            {
                uint32_t r = m_readPos.load(std::memory_order_relaxed);
                uint32_t w = m_writePos.load(std::memory_order_acquire);

                // Modular comparison: counters are free-running and wrap past 2^32 (matches Post's full check)
                if (r == w)
                    return false; // Ring empty

                outCmd = std::move(m_ring[r & MASK]);
                m_readPos.store(r + 1, std::memory_order_release);
                m_totalConsumed.fetch_add(1, std::memory_order_relaxed);
                return true;
            }

            /**
             * @brief Drain all pending commands, invoking a handler for each.
             */
            uint32_t DrainAll(const std::function<void(const RenderCommand&)>& handler)
            {
                uint32_t count = 0;
                RenderCommand cmd;
                while (Consume(cmd))
                {
                    handler(cmd);
                    count++;
                }
                return count;
            }

            uint32_t GetPendingCount() const
            {
                return m_writePos.load(std::memory_order_acquire) - m_readPos.load(std::memory_order_acquire);
            }

            bool IsEmpty() const { return GetPendingCount() == 0; }
            bool IsFull() const { return GetPendingCount() >= CAPACITY; }
            uint64_t GetTotalPosted() const { return m_totalPosted.load(std::memory_order_relaxed); }
            uint64_t GetTotalConsumed() const { return m_totalConsumed.load(std::memory_order_relaxed); }

            void Reset()
            {
                m_writePos.store(0, std::memory_order_relaxed);
                m_readPos.store(0, std::memory_order_relaxed);
                m_totalPosted.store(0, std::memory_order_relaxed);
                m_totalConsumed.store(0, std::memory_order_relaxed);
            }

          private:
            std::array<RenderCommand, CAPACITY> m_ring;
            alignas(64) std::atomic<uint32_t> m_writePos{0};
            alignas(64) std::atomic<uint32_t> m_readPos{0};
            std::atomic<uint64_t> m_totalPosted{0};
            std::atomic<uint64_t> m_totalConsumed{0};
        };

        /**
         * @brief Singleton render command queue with default ring size.
         */
        class RenderCommandQueue
        {
          public:
            static RenderCommandQueue& GetInstance()
            {
                static RenderCommandQueue instance;
                return instance;
            }

            void Initialize() { m_ring.Reset(); }
            void Shutdown() { m_ring.Reset(); }

            bool Post(const RenderCommand& cmd) { return m_ring.Post(cmd); }
            bool PostLambda(std::function<void()> fn) { return m_ring.PostLambda(std::move(fn)); }
            bool Consume(RenderCommand& outCmd) { return m_ring.Consume(outCmd); }

            uint32_t DrainAll(const std::function<void(const RenderCommand&)>& handler)
            {
                return m_ring.DrainAll(handler);
            }

            uint32_t GetPendingCount() const { return m_ring.GetPendingCount(); }
            bool IsEmpty() const { return m_ring.IsEmpty(); }
            uint64_t GetTotalPosted() const { return m_ring.GetTotalPosted(); }
            uint64_t GetTotalConsumed() const { return m_ring.GetTotalConsumed(); }

          private:
            RenderCommandQueue() = default;
            RenderCommandRing<14> m_ring;
        };

    } // namespace Graphics
} // namespace Spark
