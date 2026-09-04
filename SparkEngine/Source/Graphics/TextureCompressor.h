/**
 * @file TextureCompressor.h
 * @brief GPU texture compression pipeline (BC1/BC7/ASTC)
 * @author Spark Engine Team
 * @date 2026
 *
 * Compresses RGBA textures into GPU-native block formats for 4-8x VRAM savings.
 * Supports mipmap generation, .stex file format for cached results, and
 * runtime decompression for CPU access.
 *
 * ## Usage
 * @code
 *   auto& comp = Spark::Graphics::TextureCompressor::GetInstance();
 *   auto result = comp.Compress(rgbaPixels, 1024, 1024);
 *   // result.compressedSize is ~4x smaller than originalSize
 *   comp.SaveCompressed(result, "cached.stex");
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    /** @brief Block compression format for GPU textures. */
    enum class TextureCompressionFormat : uint8_t
    {
        BC1,         ///< RGB 4:1 compression (DXT1), no alpha
        BC3,         ///< RGBA 4:1 compression (DXT5), interpolated alpha
        BC4,         ///< Single channel, grayscale or height maps
        BC5,         ///< Two channel, normal maps (RG)
        BC7,         ///< High quality RGBA, best quality-to-size ratio
        ASTC_4x4,    ///< ASTC 4x4 block, mobile-friendly
        ASTC_6x6,    ///< ASTC 6x6 block, higher compression
        ASTC_8x8,    ///< ASTC 8x8 block, maximum compression
        Neural,      ///< Neural texture compression (MLP per block)
        Uncompressed ///< No compression (passthrough)
    };

    /** @brief Compression quality/speed tradeoff. */
    struct CompressionOptions
    {
        TextureCompressionFormat format = TextureCompressionFormat::BC7;
        int qualityLevel = 2; ///< 0=fast, 1=normal, 2=high, 3=best
        bool generateMipmaps = true;
        int maxMipLevels = 0; ///< 0 = auto (log2)
        bool sRGB = true;
        bool isNormalMap = false; ///< Auto-selects BC5 for normal maps
    };

    /** @brief Result of texture compression including all mip levels. */
    struct CompressedTexture
    {
        uint32_t width = 0;
        uint32_t height = 0;
        TextureCompressionFormat format = TextureCompressionFormat::Uncompressed;
        uint32_t mipLevels = 0;
        std::vector<std::vector<uint8_t>> mipData; ///< Per-mip compressed blocks
        size_t originalSize = 0;
        size_t compressedSize = 0;
        float compressionRatio = 1.0f;
    };

    /**
     * @brief GPU texture compression pipeline
     *
     * Compresses raw RGBA pixel data into BC1/BC7/ASTC block formats.
     * Includes mipmap generation, .stex serialization, and quality metrics.
     */
    class TextureCompressor
    {
      public:
        static TextureCompressor& GetInstance();

        /**
         * @brief Compress raw RGBA pixels into a block-compressed texture
         * @param rgba RGBA pixel data (4 bytes per pixel, row-major)
         * @param width Texture width (must be multiple of 4 for BC formats)
         * @param height Texture height (must be multiple of 4 for BC formats)
         * @param options Compression settings
         * @return Compressed texture with all mip levels
         */
        CompressedTexture Compress(const uint8_t* rgba, uint32_t width, uint32_t height,
                                   const CompressionOptions& options = {});

        /**
         * @brief Decompress a single mip level back to RGBA
         * @param tex Compressed texture
         * @param mipLevel Mip level to decompress (0 = full resolution)
         * @return RGBA pixel data
         */
        std::vector<uint8_t> Decompress(const CompressedTexture& tex, uint32_t mipLevel = 0);

        /**
         * @brief Save compressed texture to .stex file
         * @param tex Compressed texture data
         * @param path Output file path
         * @return True on success
         */
        bool SaveCompressed(const CompressedTexture& tex, const std::string& path);

        /**
         * @brief Load compressed texture from .stex file
         * @param path Input file path
         * @return Loaded compressed texture
         */
        CompressedTexture LoadCompressed(const std::string& path);

        /**
         * @brief Estimate compressed size for given dimensions and format
         * @param width Texture width
         * @param height Texture height
         * @param format Compression format
         * @return Estimated size in bytes
         */
        size_t EstimateCompressedSize(uint32_t width, uint32_t height, TextureCompressionFormat format);

        /**
         * @brief Calculate number of mip levels for given dimensions
         * @param width Texture width
         * @param height Texture height
         * @return Number of mip levels (including level 0)
         */
        static uint32_t CalculateMipLevels(uint32_t width, uint32_t height);

        /**
         * @brief Get block size in bytes for a compression format
         * @param format Compression format
         * @return Block size in bytes (8 for BC1/BC4, 16 for BC3/BC5/BC7/ASTC)
         */
        static uint32_t GetBlockSize(TextureCompressionFormat format);

        /** @brief Console status string. */
        std::string Console_GetStatus() const;

      private:
        TextureCompressor() = default;

        std::vector<uint8_t> GenerateMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH);

        uint32_t m_texturesCompressed = 0;
        uint64_t m_totalBytesProcessed = 0;
        uint64_t m_totalBytesOutput = 0;
    };

} // namespace Spark::Graphics
