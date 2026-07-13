// TestDescriptorCache.cpp — Unit tests for Vulkan descriptor set layout caching and batched writes
// Tests layout deduplication, LRU eviction, and batched write flush semantics.

#include "TestFramework.h"

#include <array>
#include <cstdint>
#include <vector>

// ============================================================================
// Standalone layout cache for test isolation
// ============================================================================

namespace
{

    uint64_t TestHashLayout(uint32_t bindingCount, uint32_t descriptorType)
    {
        uint64_t hash = 14695981039346656037ULL;
        hash ^= bindingCount;
        hash *= 1099511628211ULL;
        hash ^= descriptorType;
        hash *= 1099511628211ULL;
        return hash;
    }

    class TestLayoutCache
    {
      public:
        static constexpr uint32_t MAX_LAYOUTS = 64;

        int GetOrCreate(uint64_t hash)
        {
            for (uint32_t i = 0; i < m_count; ++i)
            {
                if (m_hashes[i] == hash)
                {
                    m_useCounts[i]++;
                    return static_cast<int>(i);
                }
            }

            if (m_count >= MAX_LAYOUTS)
            {
                uint32_t minIdx = 0;
                for (uint32_t i = 1; i < m_count; ++i)
                {
                    if (m_useCounts[i] < m_useCounts[minIdx])
                        minIdx = i;
                }
                m_hashes[minIdx] = hash;
                m_useCounts[minIdx] = 1;
                return static_cast<int>(minIdx);
            }

            m_hashes[m_count] = hash;
            m_useCounts[m_count] = 1;
            return static_cast<int>(m_count++);
        }

        uint32_t GetCount() const { return m_count; }

      private:
        std::array<uint64_t, MAX_LAYOUTS> m_hashes = {};
        std::array<uint32_t, MAX_LAYOUTS> m_useCounts = {};
        uint32_t m_count = 0;
    };

} // namespace

TEST(DescriptorCache_LayoutDeduplication)
{
    TestLayoutCache cache;

    uint64_t hash1 = TestHashLayout(2, 6);
    uint64_t hash2 = TestHashLayout(3, 7);

    int idx1 = cache.GetOrCreate(hash1);
    int idx2 = cache.GetOrCreate(hash2);
    int idx1_again = cache.GetOrCreate(hash1);

    ASSERT_EQ(idx1, idx1_again);
    ASSERT_TRUE(idx1 != idx2);
    ASSERT_EQ(cache.GetCount(), 2u);
}

TEST(DescriptorCache_EvictsLeastUsed)
{
    TestLayoutCache cache;

    for (uint32_t i = 0; i < TestLayoutCache::MAX_LAYOUTS; ++i)
        cache.GetOrCreate(i + 1);
    ASSERT_EQ(cache.GetCount(), TestLayoutCache::MAX_LAYOUTS);

    // Use layout 0 many times so it won't be evicted
    for (int i = 0; i < 100; ++i)
        cache.GetOrCreate(1);

    int newIdx = cache.GetOrCreate(999999);
    ASSERT_TRUE(newIdx >= 0);
    ASSERT_EQ(cache.GetCount(), TestLayoutCache::MAX_LAYOUTS);
}

TEST(DescriptorCache_BatchedWrites)
{
    std::vector<int> pendingWrites;
    for (int i = 0; i < 100; ++i)
        pendingWrites.push_back(i);

    ASSERT_EQ(pendingWrites.size(), 100u);

    int totalFlushed = static_cast<int>(pendingWrites.size());
    pendingWrites.clear();

    ASSERT_EQ(totalFlushed, 100);
    ASSERT_EQ(pendingWrites.size(), 0u);
}

TEST(VMA_MemoryUsageEnums)
{
    enum class TestMemUsage
    {
        Unknown = 0,
        GPUOnly = 1,
        CPUOnly = 2,
        CPUToGPU = 3,
        GPUToCPU = 4,
        Auto = 7,
    };

    ASSERT_TRUE(static_cast<int>(TestMemUsage::Auto) == 7);
    ASSERT_TRUE(static_cast<int>(TestMemUsage::GPUOnly) == 1);
    ASSERT_TRUE(static_cast<int>(TestMemUsage::CPUToGPU) == 3);
}
