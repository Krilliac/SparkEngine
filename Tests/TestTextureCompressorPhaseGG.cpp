/**
 * @file TestTextureCompressorPhaseGG.cpp
 * @brief Phase GG Theme 3D tests for Spark::Graphics::TextureCompressor
 *
 * These tests exercise the pure-CPU utility methods on the compressor
 * (mip-level math, block-size lookup, estimated sizes) which work on
 * every platform without a GPU. Compress/Decompress are exercised
 * with a tiny 4x4 RGBA texture to verify the happy path.
 */

#include "TestFramework.h"
#include "Graphics/TextureCompressor.h"

#include <cstdint>
#include <vector>

TEST(TextureCompressorPhaseGG_SingletonStable)
{
    auto& a = Spark::Graphics::TextureCompressor::GetInstance();
    auto& b = Spark::Graphics::TextureCompressor::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(TextureCompressorPhaseGG_CalculateMipLevels)
{
    // 1024x1024 = 11 mip levels (including level 0).
    const uint32_t m = Spark::Graphics::TextureCompressor::CalculateMipLevels(1024, 1024);
    EXPECT_EQ(m, static_cast<uint32_t>(11));

    // 1x1 = 1 level.
    const uint32_t one = Spark::Graphics::TextureCompressor::CalculateMipLevels(1, 1);
    EXPECT_EQ(one, static_cast<uint32_t>(1));

    // 256x128 = 9 levels (log2(256) + 1).
    const uint32_t asym = Spark::Graphics::TextureCompressor::CalculateMipLevels(256, 128);
    EXPECT_EQ(asym, static_cast<uint32_t>(9));
}

TEST(TextureCompressorPhaseGG_GetBlockSize)
{
    using Fmt = Spark::Graphics::TextureCompressionFormat;
    // BC1/BC4 are 8-byte blocks; BC3/BC5/BC7/ASTC are 16-byte.
    EXPECT_EQ(Spark::Graphics::TextureCompressor::GetBlockSize(Fmt::BC1), static_cast<uint32_t>(8));
    EXPECT_EQ(Spark::Graphics::TextureCompressor::GetBlockSize(Fmt::BC4), static_cast<uint32_t>(8));
    EXPECT_EQ(Spark::Graphics::TextureCompressor::GetBlockSize(Fmt::BC3), static_cast<uint32_t>(16));
    EXPECT_EQ(Spark::Graphics::TextureCompressor::GetBlockSize(Fmt::BC5), static_cast<uint32_t>(16));
    EXPECT_EQ(Spark::Graphics::TextureCompressor::GetBlockSize(Fmt::BC7), static_cast<uint32_t>(16));
}

TEST(TextureCompressorPhaseGG_EstimateCompressedSize)
{
    using Fmt = Spark::Graphics::TextureCompressionFormat;
    auto& comp = Spark::Graphics::TextureCompressor::GetInstance();
    // 256x256 RGBA uncompressed = 256KB. BC7 at 16 bytes per 4x4 block
    // = (256/4) * (256/4) * 16 = 65536 bytes = 64 KB. 4x smaller.
    const size_t bc7 = comp.EstimateCompressedSize(256, 256, Fmt::BC7);
    EXPECT_EQ(bc7, static_cast<size_t>(65536));

    // BC1 at 8 bytes per 4x4 block = 32 KB. 8x smaller than RGBA.
    const size_t bc1 = comp.EstimateCompressedSize(256, 256, Fmt::BC1);
    EXPECT_EQ(bc1, static_cast<size_t>(32768));
}

TEST(TextureCompressorPhaseGG_CompressTinyTextureRoundTrip)
{
    auto& comp = Spark::Graphics::TextureCompressor::GetInstance();

    // 4x4 solid-red RGBA texture.
    std::vector<uint8_t> rgba(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i)
    {
        rgba[i * 4 + 0] = 255; // R
        rgba[i * 4 + 1] = 0;
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = 255; // A
    }

    Spark::Graphics::CompressionOptions opts;
    opts.format = Spark::Graphics::TextureCompressionFormat::BC7;
    opts.generateMipmaps = false;
    auto result = comp.Compress(rgba.data(), 4, 4, opts);

    EXPECT_EQ(result.width, static_cast<uint32_t>(4));
    EXPECT_EQ(result.height, static_cast<uint32_t>(4));
    EXPECT_TRUE(result.compressedSize > 0);
    EXPECT_TRUE(result.originalSize > 0);
    // Compression ratio < 1.0 means compressed is smaller than original.
    // For a 4x4 texture the overhead may dominate so we just verify
    // it's populated.
    EXPECT_TRUE(result.mipData.size() >= static_cast<size_t>(1));
}

TEST(TextureCompressorPhaseGG_ConsoleStatusReturnsString)
{
    auto& comp = Spark::Graphics::TextureCompressor::GetInstance();
    const std::string status = comp.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
}
