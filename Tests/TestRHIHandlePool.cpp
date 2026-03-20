// TestRHIHandlePool.cpp - Tests for typed handle pool with generation counters

#include "TestFramework.h"

#include <cstdint>
#include <type_traits>
#include <vector>

// Standalone handle pool reproduction for CI testing
namespace TestHandles
{

    template <typename Tag> struct TypedHandle
    {
        uint64_t value = 0;

        constexpr TypedHandle() = default;
        constexpr explicit TypedHandle(uint64_t v) : value(v) {}

        [[nodiscard]] constexpr bool IsValid() const { return value != 0; }
        [[nodiscard]] constexpr uint32_t GetIndex() const { return static_cast<uint32_t>((value >> 16) & 0xFFFFFF); }
        [[nodiscard]] constexpr uint32_t GetGeneration() const
        {
            return static_cast<uint32_t>((value >> 40) & 0xFFFFFF);
        }
        [[nodiscard]] constexpr uint16_t GetTag() const { return static_cast<uint16_t>(value & 0xFFFF); }

        constexpr bool operator==(const TypedHandle& other) const { return value == other.value; }
        constexpr bool operator!=(const TypedHandle& other) const { return value != other.value; }

        static constexpr TypedHandle Make(uint32_t index, uint32_t generation, uint16_t tag)
        {
            return TypedHandle{(static_cast<uint64_t>(generation & 0xFFFFFF) << 40) |
                               (static_cast<uint64_t>(index & 0xFFFFFF) << 16) | static_cast<uint64_t>(tag)};
        }
    };

    struct BufferTag
    {
    };
    struct TextureTag
    {
    };
    struct ShaderTag
    {
    };

    using BufferHandle = TypedHandle<BufferTag>;
    using TextureHandle = TypedHandle<TextureTag>;
    using ShaderHandle = TypedHandle<ShaderTag>;

    template <typename T, typename Tag, uint32_t Capacity = 4096> class HandlePool
    {
      public:
        using Handle = TypedHandle<Tag>;

        HandlePool() { Clear(); }

        Handle Allocate(T* resource)
        {
            if (m_freeList.empty())
                return Handle{};

            uint32_t index = m_freeList.back();
            m_freeList.pop_back();
            auto& slot = m_slots[index];
            slot.resource = resource;
            slot.alive = true;
            ++m_count;
            return Handle::Make(index, slot.generation, 1);
        }

        T* Get(Handle handle) const
        {
            if (!handle.IsValid())
                return nullptr;
            uint32_t index = handle.GetIndex();
            if (index >= Capacity)
                return nullptr;
            const auto& slot = m_slots[index];
            if (!slot.alive || slot.generation != handle.GetGeneration())
                return nullptr;
            return slot.resource;
        }

        bool Free(Handle handle)
        {
            if (!handle.IsValid())
                return false;
            uint32_t index = handle.GetIndex();
            if (index >= Capacity)
                return false;
            auto& slot = m_slots[index];
            if (!slot.alive || slot.generation != handle.GetGeneration())
                return false;
            slot.resource = nullptr;
            slot.alive = false;
            slot.generation++;
            m_freeList.push_back(index);
            --m_count;
            return true;
        }

        [[nodiscard]] bool IsValid(Handle handle) const { return Get(handle) != nullptr; }
        [[nodiscard]] uint32_t Count() const { return m_count; }
        [[nodiscard]] constexpr uint32_t GetCapacity() const { return Capacity; }

        void Clear()
        {
            m_freeList.clear();
            m_freeList.reserve(Capacity);
            m_count = 0;
            for (uint32_t i = 0; i < Capacity; ++i)
            {
                m_slots[i].resource = nullptr;
                m_slots[i].generation = 1;
                m_slots[i].alive = false;
                m_freeList.push_back(Capacity - 1 - i);
            }
        }

      private:
        struct Slot
        {
            T* resource = nullptr;
            uint32_t generation = 1;
            bool alive = false;
        };

        Slot m_slots[Capacity];
        std::vector<uint32_t> m_freeList;
        uint32_t m_count = 0;
    };

} // namespace TestHandles

struct MockBuffer
{
    int data = 0;
};

struct MockTexture
{
    int width = 0;
};

TEST(RHIHandlePool_AllocateAndGet)
{
    TestHandles::HandlePool<MockBuffer, TestHandles::BufferTag, 16> pool;
    MockBuffer buf;
    buf.data = 42;

    auto handle = pool.Allocate(&buf);
    EXPECT_TRUE(handle.IsValid());
    EXPECT_EQ(pool.Count(), 1u);

    auto* result = pool.Get(handle);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(result->data, 42);
}

TEST(RHIHandlePool_FreeInvalidatesHandle)
{
    TestHandles::HandlePool<MockBuffer, TestHandles::BufferTag, 16> pool;
    MockBuffer buf;

    auto handle = pool.Allocate(&buf);
    EXPECT_TRUE(pool.IsValid(handle));

    EXPECT_TRUE(pool.Free(handle));
    EXPECT_EQ(pool.Count(), 0u);
    EXPECT_TRUE(pool.Get(handle) == nullptr);
    EXPECT_FALSE(pool.IsValid(handle));
    EXPECT_FALSE(pool.Free(handle)); // double free
}

TEST(RHIHandlePool_GenerationCounter)
{
    TestHandles::HandlePool<MockBuffer, TestHandles::BufferTag, 4> pool;
    MockBuffer buf1, buf2;
    buf1.data = 1;
    buf2.data = 2;

    auto handle1 = pool.Allocate(&buf1);
    uint32_t gen1 = handle1.GetGeneration();
    pool.Free(handle1);

    auto handle2 = pool.Allocate(&buf2);
    EXPECT_EQ(handle2.GetIndex(), handle1.GetIndex());
    EXPECT_EQ(handle2.GetGeneration(), gen1 + 1);
    EXPECT_TRUE(pool.Get(handle1) == nullptr);
    EXPECT_EQ(pool.Get(handle2)->data, 2);
}

TEST(RHIHandlePool_CapacityLimit)
{
    TestHandles::HandlePool<MockBuffer, TestHandles::BufferTag, 4> pool;
    MockBuffer bufs[5];

    for (int i = 0; i < 4; ++i)
    {
        auto h = pool.Allocate(&bufs[i]);
        EXPECT_TRUE(h.IsValid());
    }
    EXPECT_EQ(pool.Count(), 4u);

    auto overflow = pool.Allocate(&bufs[4]);
    EXPECT_FALSE(overflow.IsValid());
}

TEST(RHIHandlePool_Clear)
{
    TestHandles::HandlePool<MockBuffer, TestHandles::BufferTag, 8> pool;
    MockBuffer buf;

    auto handle = pool.Allocate(&buf);
    EXPECT_EQ(pool.Count(), 1u);

    pool.Clear();
    EXPECT_EQ(pool.Count(), 0u);
    EXPECT_TRUE(pool.Get(handle) == nullptr);

    auto handle2 = pool.Allocate(&buf);
    EXPECT_TRUE(handle2.IsValid());
}

TEST(RHIHandlePool_BitLayout)
{
    auto handle = TestHandles::BufferHandle::Make(100, 50, 1);
    EXPECT_EQ(handle.GetIndex(), 100u);
    EXPECT_EQ(handle.GetGeneration(), 50u);
    EXPECT_EQ(handle.GetTag(), static_cast<uint16_t>(1));
    EXPECT_TRUE(handle.IsValid());

    TestHandles::BufferHandle invalid;
    EXPECT_FALSE(invalid.IsValid());
    EXPECT_EQ(invalid.value, static_cast<uint64_t>(0));
}

TEST(RHIHandlePool_TypeSafety)
{
    EXPECT_TRUE((!std::is_same_v<TestHandles::BufferHandle, TestHandles::TextureHandle>));
    EXPECT_TRUE((!std::is_same_v<TestHandles::BufferHandle, TestHandles::ShaderHandle>));
}

TEST(RHIHandlePool_HandleEquality)
{
    auto h1 = TestHandles::BufferHandle::Make(10, 1, 1);
    auto h2 = TestHandles::BufferHandle::Make(10, 1, 1);
    auto h3 = TestHandles::BufferHandle::Make(10, 2, 1);

    EXPECT_TRUE(h1 == h2);
    EXPECT_TRUE(h1 != h3);
}

TEST(RHIHandlePool_GetCapacity)
{
    TestHandles::HandlePool<MockBuffer, TestHandles::BufferTag, 256> pool;
    EXPECT_EQ(pool.GetCapacity(), 256u);
}

TEST(RHIHandlePool_InvalidHandleOps)
{
    TestHandles::HandlePool<MockBuffer, TestHandles::BufferTag, 16> pool;

    TestHandles::BufferHandle invalid;
    EXPECT_TRUE(pool.Get(invalid) == nullptr);
    EXPECT_FALSE(pool.Free(invalid));
    EXPECT_FALSE(pool.IsValid(invalid));

    auto outOfRange = TestHandles::BufferHandle::Make(9999, 1, 1);
    EXPECT_TRUE(pool.Get(outOfRange) == nullptr);
    EXPECT_FALSE(pool.Free(outOfRange));
}
