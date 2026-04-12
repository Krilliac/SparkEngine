/**
 * @file TestCompressionUtilsReal.cpp
 * @brief Real-class tests for Spark::CompressionUtils functions
 */

#include "TestFramework.h"
#include "Utils/CompressionUtils.h"

#include <cstring>
#include <vector>

TEST(CompressionUtilsReal_NoneIsPassthrough)
{
    const std::vector<uint8_t> input = {1, 2, 3, 4, 5, 6, 7, 8};
    auto compressed = Spark::CompressData(input.data(), input.size(), Spark::CompressionMethod::None);
    EXPECT_EQ(compressed.size(), input.size());
    EXPECT_EQ(std::memcmp(compressed.data(), input.data(), input.size()), 0);

    auto decompressed =
        Spark::DecompressData(compressed.data(), compressed.size(), input.size(), Spark::CompressionMethod::None);
    EXPECT_EQ(decompressed.size(), input.size());
    EXPECT_EQ(std::memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST(CompressionUtilsReal_GetMethodName)
{
    EXPECT_TRUE(Spark::GetCompressionMethodName(Spark::CompressionMethod::None) != nullptr);
    EXPECT_TRUE(Spark::GetCompressionMethodName(Spark::CompressionMethod::Deflate) != nullptr);
    EXPECT_TRUE(Spark::GetCompressionMethodName(Spark::CompressionMethod::Zstd) != nullptr);
}

TEST(CompressionUtilsReal_IsAvailableForNone)
{
    // "None" is always available as a passthrough.
    EXPECT_TRUE(Spark::IsCompressionAvailable(Spark::CompressionMethod::None));
}

TEST(CompressionUtilsReal_CompressBoundNone)
{
    // For "None" method, bound should equal input size.
    const size_t input = 1024;
    const size_t bound = Spark::CompressBound(input, Spark::CompressionMethod::None);
    EXPECT_EQ(bound, input);
}

TEST(CompressionUtilsReal_CompressBoundDeflate)
{
    // For Deflate, bound should be at least input size.
    const size_t input = 1024;
    const size_t bound = Spark::CompressBound(input, Spark::CompressionMethod::Deflate);
    EXPECT_TRUE(bound >= input);
}

TEST(CompressionUtilsReal_EmptyInput)
{
    auto compressed = Spark::CompressData(nullptr, 0, Spark::CompressionMethod::None);
    EXPECT_EQ(compressed.size(), static_cast<size_t>(0));
}

TEST(CompressionUtilsReal_DeflateRoundTripIfAvailable)
{
    if (!Spark::IsCompressionAvailable(Spark::CompressionMethod::Deflate))
    {
        // Deflate backend not available — skip the round-trip test.
        EXPECT_TRUE(true);
        return;
    }

    // Use highly compressible input (repeating bytes).
    std::vector<uint8_t> input(512, 0x42);
    auto compressed = Spark::CompressData(input.data(), input.size(), Spark::CompressionMethod::Deflate);
    EXPECT_TRUE(!compressed.empty());

    auto decompressed =
        Spark::DecompressData(compressed.data(), compressed.size(), input.size(), Spark::CompressionMethod::Deflate);
    EXPECT_EQ(decompressed.size(), input.size());
    EXPECT_EQ(std::memcmp(decompressed.data(), input.data(), input.size()), 0);
}
