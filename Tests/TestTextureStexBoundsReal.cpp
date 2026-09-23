/**
 * @file TestTextureStexBoundsReal.cpp
 * @brief Hostile-input regressions for the .stex container loader
 *        (Spark::Graphics::TextureCompressor::LoadCompressed), run against the
 *        shipped class. SEC-120, parser inventory id "texture-stex-compressor".
 *
 * Before the SEC-120 bound the loader trusted every header field: mipLevels
 * sized a std::vector<uint32_t> and each mip size sized a std::vector<uint8_t>
 * before a single payload byte was read (a 21-byte file could demand 16 GiB),
 * and it accepted any version, any format byte, any dimensions, and mip sizes
 * that did not match the dimensions they describe. Every test below except the
 * two round trips and the allocation-bomb test returned a populated texture
 * before the fix, so its EXPECT fails against the unbounded loader. The
 * allocation-bomb inputs are also committed as fuzz regressions in
 * Tests/Fuzz/corpus/texture-stex, where libFuzzer's 256 MB malloc cap turns
 * the pre-fix allocation into a deterministic failure.
 */

#include "TestFramework.h"

#include "Graphics/TextureCompressor.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    using Spark::Graphics::CompressedTexture;
    using Spark::Graphics::CompressionOptions;
    using Spark::Graphics::TextureCompressionFormat;
    using Spark::Graphics::TextureCompressor;

    constexpr uint8_t kBC1 = static_cast<uint8_t>(TextureCompressionFormat::BC1);

    std::filesystem::path ScratchPath(const std::string& name)
    {
        auto directory = std::filesystem::temp_directory_path() / "spark_stex_bounds_tests";
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        return directory / name;
    }

    void AppendU32(std::vector<uint8_t>& out, uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }

    /// Serialise a .stex header exactly as SaveCompressed lays it out.
    std::vector<uint8_t> StexHeader(uint32_t version, uint32_t width, uint32_t height, uint8_t format,
                                    uint32_t mipLevels, const std::vector<uint32_t>& mipSizes)
    {
        std::vector<uint8_t> bytes = {'S', 'T', 'E', 'X'};
        AppendU32(bytes, version);
        AppendU32(bytes, width);
        AppendU32(bytes, height);
        bytes.push_back(format);
        AppendU32(bytes, mipLevels);
        for (uint32_t size : mipSizes)
            AppendU32(bytes, size);
        return bytes;
    }

    CompressedTexture LoadBytes(const std::string& name, const std::vector<uint8_t>& bytes)
    {
        const auto path = ScratchPath(name);
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        CompressedTexture loaded = TextureCompressor::GetInstance().LoadCompressed(path.string());
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return loaded;
    }

    bool IsRejected(const CompressedTexture& tex)
    {
        return tex.width == 0 && tex.height == 0 && tex.mipLevels == 0 && tex.mipData.empty();
    }

    /// A well-formed 4x4 single-mip BC1 file (one 8-byte block).
    std::vector<uint8_t> ValidBc1_4x4()
    {
        auto bytes = StexHeader(TextureCompressor::kStexVersion, 4, 4, kBC1, 1, {8});
        for (uint8_t i = 0; i < 8; ++i)
            bytes.push_back(i);
        return bytes;
    }

    void ExpectRoundTrip(const char* name, uint32_t width, uint32_t height, TextureCompressionFormat format)
    {
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < rgba.size(); ++i)
            rgba[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);

        auto& compressor = TextureCompressor::GetInstance();
        CompressionOptions options;
        options.format = format;
        options.generateMipmaps = true;
        options.sRGB = false;
        const CompressedTexture original = compressor.Compress(rgba.data(), width, height, options);
        ASSERT_EQ(original.mipLevels, TextureCompressor::CalculateMipLevels(width, height));

        const auto path = ScratchPath(name);
        ASSERT_TRUE(compressor.SaveCompressed(original, path.string()));
        const CompressedTexture loaded = compressor.LoadCompressed(path.string());
        std::error_code ec;
        std::filesystem::remove(path, ec);

        EXPECT_EQ(loaded.width, width);
        EXPECT_EQ(loaded.height, height);
        EXPECT_TRUE(loaded.format == format);
        EXPECT_EQ(loaded.mipLevels, original.mipLevels);
        ASSERT_EQ(loaded.mipData.size(), original.mipData.size());
        for (size_t m = 0; m < loaded.mipData.size(); ++m)
            EXPECT_TRUE(loaded.mipData[m] == original.mipData[m]);
    }
} // namespace

// ============================================================================
// Legitimate files still load (the bound must not reject what Save writes)
// ============================================================================

TEST(SecurityParsers_StexRoundTripBc7ChainStillLoads)
{
    ExpectRoundTrip("roundtrip_bc7.stex", 8, 8, TextureCompressionFormat::BC7);
}

TEST(SecurityParsers_StexRoundTripUncompressedNonPowerOfTwoStillLoads)
{
    // 5x3 -> 2x1 -> 1x1: exercises the max(dim / 2, 1) mip-size walk.
    ExpectRoundTrip("roundtrip_uncompressed.stex", 5, 3, TextureCompressionFormat::Uncompressed);
}

TEST(SecurityParsers_StexHandBuiltValidFileLoads)
{
    const CompressedTexture tex = LoadBytes("valid_bc1.stex", ValidBc1_4x4());
    EXPECT_EQ(tex.width, 4u);
    EXPECT_EQ(tex.height, 4u);
    EXPECT_EQ(tex.mipLevels, 1u);
    ASSERT_EQ(tex.mipData.size(), static_cast<size_t>(1));
    EXPECT_EQ(tex.mipData[0].size(), static_cast<size_t>(8));
}

// ============================================================================
// Structural lies the pre-fix loader accepted
// ============================================================================

TEST(SecurityParsers_StexMipSizeNotMatchingDimensionsRejected)
{
    // A 4x4 BC1 mip is one 8-byte block; this file declares (and supplies) 1 byte.
    // Pre-fix: accepted with mipData[0].size() == 1, so any consumer that walks
    // 4x4 worth of blocks read 7 bytes past the buffer.
    auto bytes = StexHeader(TextureCompressor::kStexVersion, 4, 4, kBC1, 1, {1});
    bytes.push_back(0);
    EXPECT_TRUE(IsRejected(LoadBytes("mip_size_mismatch.stex", bytes)));
}

TEST(SecurityParsers_StexUnknownVersionRejected)
{
    auto bytes = ValidBc1_4x4();
    bytes[4] = 2; // version field, little-endian low byte
    EXPECT_TRUE(IsRejected(LoadBytes("bad_version.stex", bytes)));
}

TEST(SecurityParsers_StexUnknownFormatRejected)
{
    auto bytes = ValidBc1_4x4();
    bytes[16] = 200; // format byte: not a TextureCompressionFormat enumerator
    EXPECT_TRUE(IsRejected(LoadBytes("bad_format.stex", bytes)));
}

TEST(SecurityParsers_StexOversizedDimensionsRejected)
{
    // 2^31 x 2^31 with a single 8-byte mip. Pre-fix: accepted, after which
    // Decompress() computed width * height * 4 = 2^64 (wraps to 0 in size_t).
    auto bytes = StexHeader(TextureCompressor::kStexVersion, 0x80000000u, 0x80000000u, kBC1, 1, {8});
    bytes.resize(bytes.size() + 8, 0);
    EXPECT_TRUE(IsRejected(LoadBytes("huge_dimensions.stex", bytes)));

    auto overCap = StexHeader(TextureCompressor::kStexVersion, TextureCompressor::kMaxStexDimension + 1, 4, kBC1, 1,
                              {static_cast<uint32_t>(((TextureCompressor::kMaxStexDimension + 1 + 3) / 4) * 8)});
    overCap.resize(overCap.size() + ((TextureCompressor::kMaxStexDimension + 1 + 3) / 4) * 8, 0);
    EXPECT_TRUE(IsRejected(LoadBytes("over_cap_dimensions.stex", overCap)));
}

TEST(SecurityParsers_StexZeroDimensionOrZeroMipsRejected)
{
    EXPECT_TRUE(
        IsRejected(LoadBytes("zero_width.stex", StexHeader(TextureCompressor::kStexVersion, 0, 4, kBC1, 0, {}))));

    auto zeroMips = StexHeader(TextureCompressor::kStexVersion, 4, 4, kBC1, 0, {});
    EXPECT_TRUE(IsRejected(LoadBytes("zero_mips.stex", zeroMips)));
}

TEST(SecurityParsers_StexMipCountBeyondChainRejected)
{
    // 4x4 has a 3-level chain (4x4, 2x2, 1x1); five levels of one BC1 block each
    // is internally consistent with its own size table but not with the image.
    auto bytes = StexHeader(TextureCompressor::kStexVersion, 4, 4, kBC1, 5, {8, 8, 8, 8, 8});
    bytes.resize(bytes.size() + 40, 0);
    EXPECT_TRUE(IsRejected(LoadBytes("too_many_mips.stex", bytes)));
}

TEST(SecurityParsers_StexTrailingBytesRejected)
{
    auto bytes = ValidBc1_4x4();
    bytes.push_back(0xAB);
    EXPECT_TRUE(IsRejected(LoadBytes("trailing_bytes.stex", bytes)));
}

// ============================================================================
// Allocation bombs: header counts must not size an allocation
// ============================================================================

TEST(SecurityParsers_StexHeaderCountsDoNotSizeAllocations)
{
    // mipLevels = 0xFFFFFFFF: pre-fix allocated a 16 GiB std::vector<uint32_t>
    // for the mip-size table from this 21-byte file.
    EXPECT_TRUE(IsRejected(
        LoadBytes("miplevels_bomb.stex", StexHeader(TextureCompressor::kStexVersion, 4, 4, kBC1, 0xFFFFFFFFu, {}))));

    // One mip declaring 0xFFFFFFF0 bytes: pre-fix allocated ~4 GiB before
    // discovering the file ends after the size table.
    EXPECT_TRUE(IsRejected(
        LoadBytes("mipsize_bomb.stex", StexHeader(TextureCompressor::kStexVersion, 4, 4, kBC1, 1, {0xFFFFFFF0u}))));
}
