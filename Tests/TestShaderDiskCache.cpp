// TestShaderDiskCache.cpp — Unit tests for persistent shader disk cache and async compilation
// Tests cache store/lookup, miss handling, clear, async futures, variant parallelism, and hit rate.

#include "TestFramework.h"

#include <cstdint>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone types for test isolation
// ============================================================================

namespace
{

    struct TestShaderBlob
    {
        std::vector<uint8_t> bytecode;
        bool success = false;
        uint64_t hash = 0;
    };

    class TestShaderDiskCache
    {
      public:
        void Store(uint64_t hash, const TestShaderBlob& blob) { m_cache[hash] = blob; }

        TestShaderBlob Lookup(uint64_t hash) const
        {
            auto it = m_cache.find(hash);
            if (it != m_cache.end())
                return it->second;
            return {};
        }

        size_t GetEntryCount() const { return m_cache.size(); }
        void Clear() { m_cache.clear(); }

      private:
        std::unordered_map<uint64_t, TestShaderBlob> m_cache;
    };

    uint64_t HashShaderSource(const std::string& source)
    {
        uint64_t hash = 14695981039346656037ULL;
        for (char c : source)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

} // namespace

// ============================================================================
// Disk cache tests
// ============================================================================

TEST("ShaderDiskCache_StoreAndLookup", "[shader]")
{
    TestShaderDiskCache cache;

    TestShaderBlob blob;
    blob.bytecode = {0x01, 0x02, 0x03, 0x04};
    blob.success = true;
    blob.hash = HashShaderSource("float4 main() : SV_Target { return 1; }");

    cache.Store(blob.hash, blob);
    ASSERT_EQ(cache.GetEntryCount(), 1u);

    auto retrieved = cache.Lookup(blob.hash);
    ASSERT_TRUE(retrieved.success);
    ASSERT_EQ(retrieved.bytecode.size(), 4u);
    ASSERT_EQ(retrieved.bytecode[0], 0x01);
}

TEST("ShaderDiskCache_MissReturnsEmpty", "[shader]")
{
    TestShaderDiskCache cache;
    auto result = cache.Lookup(12345);
    ASSERT_TRUE(!result.success);
    ASSERT_TRUE(result.bytecode.empty());
}

TEST("ShaderDiskCache_Clear", "[shader]")
{
    TestShaderDiskCache cache;
    TestShaderBlob blob;
    blob.success = true;
    blob.bytecode = {0xAA};

    cache.Store(1, blob);
    cache.Store(2, blob);
    ASSERT_EQ(cache.GetEntryCount(), 2u);

    cache.Clear();
    ASSERT_EQ(cache.GetEntryCount(), 0u);
}

// ============================================================================
// Async compilation tests
// ============================================================================

TEST("AsyncShaderCompile_FutureResolves", "[shader]")
{
    auto future = std::async(std::launch::async,
                             []()
                             {
                                 TestShaderBlob blob;
                                 blob.bytecode = {0xDE, 0xAD, 0xBE, 0xEF};
                                 blob.success = true;
                                 return blob;
                             });

    auto result = future.get();
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.bytecode.size(), 4u);
}

TEST("AsyncShaderCompile_MultipleVariants", "[shader]")
{
    std::vector<std::future<TestShaderBlob>> futures;

    for (int i = 0; i < 8; ++i)
    {
        futures.push_back(std::async(std::launch::async,
                                     [i]()
                                     {
                                         TestShaderBlob blob;
                                         blob.bytecode = {static_cast<uint8_t>(i)};
                                         blob.success = true;
                                         blob.hash = static_cast<uint64_t>(i);
                                         return blob;
                                     }));
    }

    for (int i = 0; i < 8; ++i)
    {
        auto result = futures[i].get();
        ASSERT_TRUE(result.success);
        ASSERT_EQ(result.bytecode[0], static_cast<uint8_t>(i));
    }
}

TEST("ShaderCacheStats_HitRate", "[shader]")
{
    struct CacheStats
    {
        uint64_t hits = 0;
        uint64_t misses = 0;
        float hitRate() const
        {
            return (hits + misses > 0) ? static_cast<float>(hits) / static_cast<float>(hits + misses) : 0.0f;
        }
    };

    CacheStats stats;
    stats.hits = 7;
    stats.misses = 3;
    float rate = stats.hitRate();
    ASSERT_TRUE(rate > 0.69f && rate < 0.71f);
}
