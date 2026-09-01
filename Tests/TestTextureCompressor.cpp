/**
 * @file TestTextureCompressor.cpp
 * @brief Tests for texture compression pipeline
 */

#include "TestFramework.h"
#include "Graphics/TextureCompressor.h"

#include <array>

using namespace Spark::Graphics;

namespace
{
    constexpr std::array<uint8_t, 16> BC7_MODE6_WEIGHTS = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

    uint32_t ReadMode6Bits(const std::vector<uint8_t>& block, uint32_t& bitPosition, uint32_t bitCount)
    {
        uint32_t value = 0;
        for (uint32_t bit = 0; bit < bitCount; ++bit, ++bitPosition)
        {
            const uint32_t sourceBit = (block[bitPosition / 8] >> (bitPosition % 8)) & 1u;
            value |= sourceBit << bit;
        }
        return value;
    }

    std::array<uint8_t, 64> DecodeBC7Mode6(const std::vector<uint8_t>& block)
    {
        std::array<uint8_t, 64> decoded{};
        uint32_t bitPosition = 7;
        uint8_t endpoints[2][4]{};

        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            endpoints[0][channel] = static_cast<uint8_t>(ReadMode6Bits(block, bitPosition, 7));
            endpoints[1][channel] = static_cast<uint8_t>(ReadMode6Bits(block, bitPosition, 7));
        }

        const uint8_t pbit0 = static_cast<uint8_t>(ReadMode6Bits(block, bitPosition, 1));
        const uint8_t pbit1 = static_cast<uint8_t>(ReadMode6Bits(block, bitPosition, 1));
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            endpoints[0][channel] = static_cast<uint8_t>((endpoints[0][channel] << 1) | pbit0);
            endpoints[1][channel] = static_cast<uint8_t>((endpoints[1][channel] << 1) | pbit1);
        }

        std::array<uint8_t, 16> indices{};
        indices[0] = static_cast<uint8_t>(ReadMode6Bits(block, bitPosition, 3));
        for (uint32_t texel = 1; texel < 16; ++texel)
            indices[texel] = static_cast<uint8_t>(ReadMode6Bits(block, bitPosition, 4));

        for (uint32_t texel = 0; texel < 16; ++texel)
        {
            const uint32_t weight = BC7_MODE6_WEIGHTS[indices[texel]];
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                decoded[texel * 4 + channel] = static_cast<uint8_t>(
                    ((64 - weight) * endpoints[0][channel] + weight * endpoints[1][channel] + 32) >> 6);
            }
        }
        return decoded;
    }
} // namespace

TEST(TexComp_MipLevelCalculation)
{
    EXPECT_EQ(TextureCompressor::CalculateMipLevels(1024, 1024), static_cast<uint32_t>(11));
    EXPECT_EQ(TextureCompressor::CalculateMipLevels(512, 256), static_cast<uint32_t>(10));
    EXPECT_EQ(TextureCompressor::CalculateMipLevels(1, 1), static_cast<uint32_t>(1));
    EXPECT_EQ(TextureCompressor::CalculateMipLevels(4, 4), static_cast<uint32_t>(3));
    EXPECT_EQ(TextureCompressor::CalculateMipLevels(2, 2), static_cast<uint32_t>(2));
}

TEST(TexComp_BlockSizeBC1)
{
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC1), static_cast<uint32_t>(8));
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC4), static_cast<uint32_t>(8));
}

TEST(TexComp_BlockSizeBC7)
{
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC7), static_cast<uint32_t>(16));
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC3), static_cast<uint32_t>(16));
    EXPECT_EQ(TextureCompressor::GetBlockSize(TextureCompressionFormat::BC5), static_cast<uint32_t>(16));
}

TEST(TexComp_BC1ProducesCorrectSize)
{
    auto& comp = TextureCompressor::GetInstance();

    // 4x4 red texture (1 block)
    std::vector<uint8_t> pixels(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i)
    {
        pixels[i * 4 + 0] = 255; // R
        pixels[i * 4 + 3] = 255; // A
    }

    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC1;
    opts.generateMipmaps = false;

    auto result = comp.Compress(pixels.data(), 4, 4, opts);
    EXPECT_EQ(result.mipLevels, static_cast<uint32_t>(1));
    ASSERT_EQ(result.mipData.size(), static_cast<size_t>(1));
    ASSERT_EQ(result.mipData.front().size(), static_cast<size_t>(8)); // 1 block = 8 bytes
}

TEST(TexComp_BC7ProducesCorrectSize)
{
    auto& comp = TextureCompressor::GetInstance();

    std::vector<uint8_t> pixels(4 * 4 * 4, 128);
    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC7;
    opts.generateMipmaps = false;

    auto result = comp.Compress(pixels.data(), 4, 4, opts);
    ASSERT_EQ(result.mipData.size(), static_cast<size_t>(1));
    ASSERT_EQ(result.mipData.front().size(), static_cast<size_t>(16)); // 1 block = 16 bytes
}

TEST(TexComp_BC7Mode6Solid128MatchesGoldenBlock)
{
    auto& comp = TextureCompressor::GetInstance();
    std::vector<uint8_t> pixels(4 * 4 * 4, 128);
    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC7;
    opts.generateMipmaps = false;

    const auto result = comp.Compress(pixels.data(), 4, 4, opts);
    constexpr std::array<uint8_t, 16> expected = {0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x81, 0x40,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    ASSERT_EQ(result.mipData.size(), static_cast<size_t>(1));
    ASSERT_EQ(result.mipData.front().size(), expected.size());
    const auto& block = result.mipData.front();
    for (size_t byte = 0; byte < expected.size(); ++byte)
        EXPECT_EQ(static_cast<uint32_t>(block[byte]), static_cast<uint32_t>(expected[byte]));
}

TEST(TexComp_BC7Mode6AsymmetricRGBAEndpointsMatchGoldenBlock)
{
    auto& comp = TextureCompressor::GetInstance();
    constexpr std::array<uint8_t, 4> rgba = {2, 64, 130, 200};
    std::vector<uint8_t> pixels(4 * 4 * 4);
    for (size_t texel = 0; texel < 16; ++texel)
    {
        for (size_t channel = 0; channel < rgba.size(); ++channel)
            pixels[texel * 4 + channel] = rgba[channel];
    }

    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC7;
    opts.generateMipmaps = false;
    const auto result = comp.Compress(pixels.data(), 4, 4, opts);
    constexpr std::array<uint8_t, 16> expected = {0xC0, 0x40, 0x00, 0x04, 0x0A, 0x06, 0xC9, 0x64,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    ASSERT_EQ(result.mipData.size(), static_cast<size_t>(1));
    ASSERT_EQ(result.mipData.front().size(), expected.size());
    const auto& block = result.mipData.front();
    for (size_t byte = 0; byte < expected.size(); ++byte)
        EXPECT_EQ(static_cast<uint32_t>(block[byte]), static_cast<uint32_t>(expected[byte]));

    const auto decoded = DecodeBC7Mode6(block);
    for (size_t byte = 0; byte < pixels.size(); ++byte)
        EXPECT_EQ(static_cast<uint32_t>(decoded[byte]), static_cast<uint32_t>(pixels[byte]));
}

TEST(TexComp_BC7Mode6AnchorGradientRoundTrips)
{
    auto& comp = TextureCompressor::GetInstance();
    constexpr std::array<uint8_t, 16> values = {255, 0,   16,  36,  52,  68,  84,  104,
                                                120, 135, 151, 171, 187, 203, 219, 239};
    std::vector<uint8_t> pixels(4 * 4 * 4);
    for (size_t texel = 0; texel < values.size(); ++texel)
    {
        for (size_t channel = 0; channel < 4; ++channel)
            pixels[texel * 4 + channel] = values[texel];
    }

    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC7;
    opts.generateMipmaps = false;
    const auto result = comp.Compress(pixels.data(), 4, 4, opts);

    ASSERT_EQ(result.mipData.size(), static_cast<size_t>(1));
    ASSERT_EQ(result.mipData.front().size(), static_cast<size_t>(16));
    const auto& block = result.mipData.front();
    EXPECT_EQ(static_cast<uint32_t>(block.front() & 0x7Fu), static_cast<uint32_t>(0x40));
    const auto decoded = DecodeBC7Mode6(block);
    for (size_t byte = 0; byte < pixels.size(); ++byte)
        EXPECT_EQ(static_cast<uint32_t>(decoded[byte]), static_cast<uint32_t>(pixels[byte]));
}

TEST(TexComp_CompressionRatioReasonable)
{
    auto& comp = TextureCompressor::GetInstance();

    // 64x64 texture
    std::vector<uint8_t> pixels(64 * 64 * 4, 200);
    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC1;
    opts.generateMipmaps = false;

    auto result = comp.Compress(pixels.data(), 64, 64, opts);

    // BC1 is 4:1 compression (64 bits per 4x4 block of 16 pixels * 32bpp = 512 bits)
    // Expected: 64/4 * 64/4 * 8 = 16 * 16 * 8 = 2048 bytes
    // Original: 64 * 64 * 4 = 16384 bytes
    EXPECT_TRUE(result.compressionRatio < 0.3f);
    EXPECT_TRUE(result.compressedSize < result.originalSize);
}

TEST(TexComp_MipmapGeneration)
{
    auto& comp = TextureCompressor::GetInstance();

    std::vector<uint8_t> pixels(16 * 16 * 4, 100);
    CompressionOptions opts;
    opts.format = TextureCompressionFormat::Uncompressed;
    opts.generateMipmaps = true;

    auto result = comp.Compress(pixels.data(), 16, 16, opts);

    // 16x16 -> 8x8 -> 4x4 -> 2x2 -> 1x1 = 5 mip levels
    EXPECT_EQ(result.mipLevels, static_cast<uint32_t>(5));
    EXPECT_EQ(result.mipData.size(), static_cast<size_t>(5));
}

TEST(TexComp_EstimateSize)
{
    auto& comp = TextureCompressor::GetInstance();

    size_t bc1Size = comp.EstimateCompressedSize(1024, 1024, TextureCompressionFormat::BC1);
    size_t bc7Size = comp.EstimateCompressedSize(1024, 1024, TextureCompressionFormat::BC7);
    size_t uncompSize = comp.EstimateCompressedSize(1024, 1024, TextureCompressionFormat::Uncompressed);

    EXPECT_EQ(bc1Size, static_cast<size_t>(256 * 256 * 8));  // 8 bytes per block
    EXPECT_EQ(bc7Size, static_cast<size_t>(256 * 256 * 16)); // 16 bytes per block
    EXPECT_EQ(uncompSize, static_cast<size_t>(1024 * 1024 * 4));
    EXPECT_TRUE(bc1Size < bc7Size);
    EXPECT_TRUE(bc7Size < uncompSize);
}

TEST(TexComp_NormalMapAutoSelectsBC5)
{
    auto& comp = TextureCompressor::GetInstance();

    std::vector<uint8_t> pixels(4 * 4 * 4, 128);
    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC7;
    opts.isNormalMap = true;
    opts.generateMipmaps = false;

    auto result = comp.Compress(pixels.data(), 4, 4, opts);
    EXPECT_EQ(static_cast<int>(result.format), static_cast<int>(TextureCompressionFormat::BC5));
}
