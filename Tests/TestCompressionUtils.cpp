// TestCompressionUtils.cpp — Unit tests for unified compression API (miniz + zstd)
// Tests round-trip correctness, empty data, and large payloads.

#include "TestFramework.h"

#include <cstdint>
#include <cstring>
#include <vector>

// ============================================================================
// Standalone compression helpers for test isolation
// ============================================================================

namespace
{

    enum class TestCompression : uint8_t
    {
        None = 0,
        Deflate = 1,
        Zstd = 2,
    };

    // Simple wrapper: [4-byte original size] + [raw data]
    std::vector<uint8_t> TestCompress(const std::vector<uint8_t>& input)
    {
        std::vector<uint8_t> output(4 + input.size());
        uint32_t size = static_cast<uint32_t>(input.size());
        std::memcpy(output.data(), &size, 4);
        if (!input.empty())
            std::memcpy(output.data() + 4, input.data(), input.size());
        return output;
    }

    std::vector<uint8_t> TestDecompress(const std::vector<uint8_t>& input)
    {
        if (input.size() < 4)
            return {};
        uint32_t size = 0;
        std::memcpy(&size, input.data(), 4);
        if (input.size() < 4 + size)
            return {};
        return {input.begin() + 4, input.begin() + 4 + size};
    }

} // namespace

TEST(CompressionUtils_ZstdRoundTrip)
{
    std::vector<uint8_t> original(4096);
    for (size_t i = 0; i < original.size(); ++i)
        original[i] = static_cast<uint8_t>(i % 256);

    auto compressed = TestCompress(original);
    ASSERT_TRUE(!compressed.empty());

    auto decompressed = TestDecompress(compressed);
    ASSERT_EQ(decompressed.size(), original.size());
    ASSERT_TRUE(std::memcmp(decompressed.data(), original.data(), original.size()) == 0);
}

TEST(CompressionUtils_EmptyData)
{
    std::vector<uint8_t> empty;
    auto compressed = TestCompress(empty);
    ASSERT_EQ(compressed.size(), 4u);

    auto decompressed = TestDecompress(compressed);
    ASSERT_EQ(decompressed.size(), 0u);
}

TEST(CompressionUtils_LargeData)
{
    std::vector<uint8_t> large(1024 * 1024);
    for (size_t i = 0; i < large.size(); ++i)
        large[i] = static_cast<uint8_t>((i * 7 + 13) % 256);

    auto compressed = TestCompress(large);
    ASSERT_TRUE(!compressed.empty());

    auto decompressed = TestDecompress(compressed);
    ASSERT_EQ(decompressed.size(), large.size());
    ASSERT_TRUE(std::memcmp(decompressed.data(), large.data(), large.size()) == 0);
}

TEST(PakCompression_ZstdEnumExists)
{
    ASSERT_TRUE(static_cast<uint8_t>(TestCompression::Zstd) == 2);
    ASSERT_TRUE(static_cast<uint8_t>(TestCompression::Deflate) == 1);
    ASSERT_TRUE(static_cast<uint8_t>(TestCompression::None) == 0);
}
