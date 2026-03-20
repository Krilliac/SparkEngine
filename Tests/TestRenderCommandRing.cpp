/**
 * @file TestRenderCommandRing.cpp
 * @brief Tests for SPSC render command ring buffer
 */

#include "TestFramework.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace
{

    enum class TestRenderCmd : uint8_t
    {
        Nop,
        BeginFrame,
        EndFrame,
        Draw,
        Lambda
    };

    struct TestCommand
    {
        TestRenderCmd type = TestRenderCmd::Nop;
        uint32_t param = 0;
        std::function<void()> lambda;
    };

    template <uint32_t SizeLog2 = 10> class TestCommandRing
    {
      public:
        static constexpr uint32_t CAPACITY = 1u << SizeLog2;
        static constexpr uint32_t MASK = CAPACITY - 1;

        bool Post(const TestCommand& cmd)
        {
            uint32_t w = m_writePos.load(std::memory_order_relaxed);
            uint32_t r = m_readPos.load(std::memory_order_acquire);
            if (w - r >= CAPACITY)
                return false;
            m_ring[w & MASK] = cmd;
            m_writePos.store(w + 1, std::memory_order_release);
            return true;
        }

        bool Consume(TestCommand& outCmd)
        {
            uint32_t r = m_readPos.load(std::memory_order_relaxed);
            uint32_t w = m_writePos.load(std::memory_order_acquire);
            if (r >= w)
                return false;
            outCmd = std::move(m_ring[r & MASK]);
            m_readPos.store(r + 1, std::memory_order_release);
            return true;
        }

        uint32_t GetPendingCount() const
        {
            return m_writePos.load(std::memory_order_acquire) - m_readPos.load(std::memory_order_acquire);
        }

        bool IsEmpty() const { return GetPendingCount() == 0; }

      private:
        std::vector<TestCommand> m_ring = std::vector<TestCommand>(CAPACITY);
        std::atomic<uint32_t> m_writePos{0};
        std::atomic<uint32_t> m_readPos{0};
    };

} // anonymous namespace

TEST(RenderCommandRing_PostConsume)
{
    TestCommandRing<4> ring; // 16 entries
    TestCommand cmd;
    cmd.type = TestRenderCmd::Draw;
    cmd.param = 42;
    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_EQ(ring.GetPendingCount(), 1u);

    TestCommand out;
    EXPECT_TRUE(ring.Consume(out));
    EXPECT_EQ(static_cast<int>(out.type), static_cast<int>(TestRenderCmd::Draw));
    EXPECT_EQ(out.param, 42u);
    EXPECT_TRUE(ring.IsEmpty());
}

TEST(RenderCommandRing_Full)
{
    TestCommandRing<2> ring; // 4 entries
    TestCommand cmd;
    cmd.type = TestRenderCmd::Draw;

    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_FALSE(ring.Post(cmd)); // Full
    EXPECT_EQ(ring.GetPendingCount(), 4u);

    TestCommand out;
    EXPECT_TRUE(ring.Consume(out));
    EXPECT_TRUE(ring.Post(cmd)); // Space freed
}

TEST(RenderCommandRing_Lambda)
{
    TestCommandRing<4> ring;
    int counter = 0;
    TestCommand cmd;
    cmd.type = TestRenderCmd::Lambda;
    cmd.lambda = [&counter] { counter++; };
    ring.Post(cmd);

    TestCommand out;
    ring.Consume(out);
    if (out.lambda)
        out.lambda();
    EXPECT_EQ(counter, 1);
}

TEST(RenderCommandRing_SPSC_Threading)
{
    TestCommandRing<10> ring; // 1024 entries
    constexpr int NUM_ITEMS = 500;
    std::atomic<int> consumed{0};

    std::thread producer(
        [&]
        {
            for (int i = 0; i < NUM_ITEMS; ++i)
            {
                TestCommand cmd;
                cmd.type = TestRenderCmd::Draw;
                cmd.param = static_cast<uint32_t>(i);
                while (!ring.Post(cmd))
                    std::this_thread::yield();
            }
        });

    std::thread consumer(
        [&]
        {
            while (consumed.load() < NUM_ITEMS)
            {
                TestCommand out;
                if (ring.Consume(out))
                    consumed.fetch_add(1);
                else
                    std::this_thread::yield();
            }
        });

    producer.join();
    consumer.join();
    EXPECT_EQ(consumed.load(), NUM_ITEMS);
    EXPECT_TRUE(ring.IsEmpty());
}
