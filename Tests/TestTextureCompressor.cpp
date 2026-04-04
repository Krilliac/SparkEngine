/**
 * @file TestTextureCompressor.cpp
 * @brief Tests for texture compression pipeline
 */

#include "TestFramework.h"
#include "Graphics/TextureCompressor.h"

using namespace Spark::Graphics;

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
    EXPECT_EQ(result.mipData[0].size(), static_cast<size_t>(8)); // 1 block = 8 bytes
}

TEST(TexComp_BC7ProducesCorrectSize)
{
    auto& comp = TextureCompressor::GetInstance();

    std::vector<uint8_t> pixels(4 * 4 * 4, 128);
    CompressionOptions opts;
    opts.format = TextureCompressionFormat::BC7;
    opts.generateMipmaps = false;

    auto result = comp.Compress(pixels.data(), 4, 4, opts);
    EXPECT_EQ(result.mipData[0].size(), static_cast<size_t>(16)); // 1 block = 16 bytes
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
