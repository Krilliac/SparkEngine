// TestVersionedHandle.cpp - Tests for VersionedHandle and HandleAllocator
// Tests allocation, freeing, generation bumping, and stale handle detection

#include "TestFramework.h"
#include <cstdint>
#include <vector>

// Simplified VersionedHandle/HandleAllocator for testing (mirrors engine implementation)
struct VersionedHandle
{
    uint32_t index = 0;
    uint32_t generation = 0;

    bool operator==(const VersionedHandle&) const = default;
    [[nodiscard]] bool IsNull() const noexcept { return index == UINT32_MAX && generation == 0; }
    [[nodiscard]] static VersionedHandle Null() noexcept { return {UINT32_MAX, 0}; }
};

class HandleAllocator
{
  public:
    explicit HandleAllocator(uint32_t capacity) : m_generations(capacity, 0), m_activeCount(0)
    {
        m_freeList.reserve(capacity);
        for (uint32_t i = capacity; i > 0; --i)
            m_freeList.push_back(i - 1);
    }

    [[nodiscard]] VersionedHandle Allocate()
    {
        if (m_freeList.empty())
            return VersionedHandle::Null();
        uint32_t idx = m_freeList.back();
        m_freeList.pop_back();
        ++m_activeCount;
        return {idx, m_generations[idx]};
    }

    void Free(VersionedHandle handle)
    {
        if (handle.IsNull())
            return;
        if (handle.index >= m_generations.size())
            return;
        if (m_generations[handle.index] != handle.generation)
            return;
        ++m_generations[handle.index];
        m_freeList.push_back(handle.index);
        --m_activeCount;
    }

    [[nodiscard]] bool IsValid(VersionedHandle handle) const
    {
        if (handle.IsNull())
            return false;
        if (handle.index >= m_generations.size())
            return false;
        return m_generations[handle.index] == handle.generation;
    }

    [[nodiscard]] uint32_t GetActiveCount() const { return m_activeCount; }
    [[nodiscard]] uint32_t GetCapacity() const { return static_cast<uint32_t>(m_generations.size()); }

  private:
    std::vector<uint32_t> m_generations;
    std::vector<uint32_t> m_freeList;
    uint32_t m_activeCount;
};

TEST(VersionedHandle_NullSentinel)
{
    auto h = VersionedHandle::Null();
    EXPECT_TRUE(h.IsNull());
    EXPECT_EQ(h.index, UINT32_MAX);
    EXPECT_EQ(h.generation, 0u);
}

TEST(VersionedHandle_DefaultNotNull)
{
    VersionedHandle h;
    EXPECT_FALSE(h.IsNull()); // default {0, 0} is NOT null
}

TEST(HandleAllocator_AllocateAndValidate)
{
    HandleAllocator alloc(10);
    auto h = alloc.Allocate();
    EXPECT_FALSE(h.IsNull());
    EXPECT_TRUE(alloc.IsValid(h));
    EXPECT_EQ(alloc.GetActiveCount(), 1u);
}

TEST(HandleAllocator_FreeInvalidatesHandle)
{
    HandleAllocator alloc(10);
    auto h = alloc.Allocate();
    alloc.Free(h);
    EXPECT_FALSE(alloc.IsValid(h));
    EXPECT_EQ(alloc.GetActiveCount(), 0u);
}

TEST(HandleAllocator_GenerationBump)
{
    HandleAllocator alloc(1);

    auto h1 = alloc.Allocate();
    alloc.Free(h1);

    auto h2 = alloc.Allocate();
    EXPECT_EQ(h1.index, h2.index);           // Same slot
    EXPECT_NE(h1.generation, h2.generation); // Different generation
    EXPECT_FALSE(alloc.IsValid(h1));         // Old handle is stale
    EXPECT_TRUE(alloc.IsValid(h2));          // New handle is valid
}

TEST(HandleAllocator_ExhaustCapacity)
{
    HandleAllocator alloc(3);
    auto a = alloc.Allocate();
    auto b = alloc.Allocate();
    auto c = alloc.Allocate();
    EXPECT_FALSE(a.IsNull());
    EXPECT_FALSE(b.IsNull());
    EXPECT_FALSE(c.IsNull());

    auto d = alloc.Allocate();
    EXPECT_TRUE(d.IsNull()); // Exhausted
}

TEST(HandleAllocator_DoubleFreeIsSafe)
{
    HandleAllocator alloc(5);
    auto h = alloc.Allocate();
    alloc.Free(h);
    alloc.Free(h); // Second free is a no-op (generation mismatch)
    EXPECT_EQ(alloc.GetActiveCount(), 0u);
}

TEST(HandleAllocator_NullIsNeverValid)
{
    HandleAllocator alloc(5);
    EXPECT_FALSE(alloc.IsValid(VersionedHandle::Null()));
}

TEST(HandleAllocator_MultipleAllocFree)
{
    HandleAllocator alloc(100);

    std::vector<VersionedHandle> handles;
    for (int i = 0; i < 50; ++i)
        handles.push_back(alloc.Allocate());
    EXPECT_EQ(alloc.GetActiveCount(), 50u);

    for (auto& h : handles)
        alloc.Free(h);
    EXPECT_EQ(alloc.GetActiveCount(), 0u);

    // All old handles should be stale
    for (auto& h : handles)
        EXPECT_FALSE(alloc.IsValid(h));
}
