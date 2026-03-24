// TestStringPool.cpp - Tests for StringPool and StringID
// Tests interning, resolve, contains, and O(1) equality

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// Simplified FNV1a64 for testing (mirrors Hash.h)
[[nodiscard]] constexpr uint64_t FNV1a64(std::string_view sv) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    for (char c : sv)
    {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }
    return hash;
}

// Simplified StringID/StringPool for testing (mirrors engine implementation)
struct StringID
{
    uint64_t hash = 0;
    bool operator==(const StringID&) const = default;
    [[nodiscard]] bool IsNull() const noexcept { return hash == 0; }
    [[nodiscard]] static StringID Null() noexcept { return {}; }
};

class StringPool
{
  public:
    [[nodiscard]] StringID Intern(std::string_view str)
    {
        uint64_t h = FNV1a64(str);
        if (m_hashToString.contains(h))
            return {h};
        auto [it, inserted] = m_strings.emplace(str);
        m_hashToString.emplace(h, &(*it));
        return {h};
    }

    [[nodiscard]] std::string_view Resolve(StringID id) const
    {
        auto it = m_hashToString.find(id.hash);
        if (it == m_hashToString.end())
            return {};
        return *it->second;
    }

    [[nodiscard]] bool Contains(std::string_view str) const
    {
        uint64_t h = FNV1a64(str);
        return m_hashToString.contains(h);
    }

    [[nodiscard]] size_t GetCount() const { return m_strings.size(); }

    void Clear()
    {
        m_strings.clear();
        m_hashToString.clear();
    }

  private:
    std::unordered_set<std::string> m_strings;
    std::unordered_map<uint64_t, const std::string*> m_hashToString;
};

TEST(StringPool_InternAndResolve)
{
    StringPool pool;
    StringID id = pool.Intern("hello");
    EXPECT_FALSE(id.IsNull());
    EXPECT_EQ(pool.Resolve(id), std::string_view("hello"));
}

TEST(StringPool_SameStringReturnsSameID)
{
    StringPool pool;
    StringID a = pool.Intern("Patrol");
    StringID b = pool.Intern("Patrol");
    EXPECT_TRUE(a == b);
}

TEST(StringPool_DifferentStringsReturnDifferentIDs)
{
    StringPool pool;
    StringID a = pool.Intern("Guard");
    StringID b = pool.Intern("Idle");
    EXPECT_FALSE(a == b);
}

TEST(StringPool_ResolveUnknownReturnsEmpty)
{
    StringPool pool;
    StringID unknown = {12345};
    EXPECT_TRUE(pool.Resolve(unknown).empty());
}

TEST(StringPool_Contains)
{
    StringPool pool;
    pool.Intern("alpha");
    EXPECT_TRUE(pool.Contains("alpha"));
    EXPECT_FALSE(pool.Contains("beta"));
}

TEST(StringPool_Count)
{
    StringPool pool;
    EXPECT_EQ(pool.GetCount(), 0u);
    pool.Intern("a");
    pool.Intern("b");
    pool.Intern("a"); // duplicate
    EXPECT_EQ(pool.GetCount(), 2u);
}

TEST(StringPool_Clear)
{
    StringPool pool;
    pool.Intern("x");
    pool.Intern("y");
    pool.Clear();
    EXPECT_EQ(pool.GetCount(), 0u);
    EXPECT_FALSE(pool.Contains("x"));
}

TEST(StringPool_NullID)
{
    StringID id = StringID::Null();
    EXPECT_TRUE(id.IsNull());
    EXPECT_EQ(id.hash, 0u);
}
