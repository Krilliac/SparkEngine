/**
 * @file TextureCompressor.cpp
 * @brief GPU texture compression implementation
 */

#include "TextureCompressor.h"
#include "TextureBlockCompression.h"

#include "Utils/LogMacros.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>

namespace Spark::Graphics
{

    TextureCompressor& TextureCompressor::GetInstance()
    {
        static TextureCompressor instance;
        return instance;
    }

    uint32_t TextureCompressor::CalculateMipLevels(uint32_t width, uint32_t height)
    {
        uint32_t levels = 1;
        uint32_t dim = std::max(width, height);
        while (dim > 1)
        {
            dim >>= 1;
            levels++;
        }
        return levels;
    }

    uint32_t TextureCompressor::GetBlockSize(TextureCompressionFormat format)
    {
        switch (format)
        {
        case TextureCompressionFormat::BC1:
        case TextureCompressionFormat::BC4:
            return 8;
        case TextureCompressionFormat::BC3:
        case TextureCompressionFormat::BC5:
        case TextureCompressionFormat::BC7:
        case TextureCompressionFormat::ASTC_4x4:
        case TextureCompressionFormat::ASTC_6x6:
        case TextureCompressionFormat::ASTC_8x8:
            return 16;
        case TextureCompressionFormat::Uncompressed:
        case TextureCompressionFormat::Neural:
            return 0;
        }
        return 0;
    }

    size_t TextureCompressor::EstimateCompressedSize(uint32_t width, uint32_t height, TextureCompressionFormat format)
    {
        if (format == TextureCompressionFormat::Uncompressed)
            return static_cast<size_t>(width) * height * 4;

        uint32_t blockSize = GetBlockSize(format);
        uint32_t blocksX = (width + 3) / 4;
        uint32_t blocksY = (height + 3) / 4;
        return static_cast<size_t>(blocksX) * blocksY * blockSize;
    }

    std::vector<uint8_t> TextureCompressor::GenerateMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH)
    {
        uint32_t dstW = std::max(srcW / 2, 1u);
        uint32_t dstH = std::max(srcH / 2, 1u);
        std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * 4);

        for (uint32_t y = 0; y < dstH; ++y)
        {
            for (uint32_t x = 0; x < dstW; ++x)
            {
                uint32_t sx = x * 2;
                uint32_t sy = y * 2;
                uint32_t sx1 = std::min(sx + 1, srcW - 1);
                uint32_t sy1 = std::min(sy + 1, srcH - 1);

                for (int c = 0; c < 4; ++c)
                {
                    uint32_t sum = src[(sy * srcW + sx) * 4 + c] + src[(sy * srcW + sx1) * 4 + c] +
                                   src[(sy1 * srcW + sx) * 4 + c] + src[(sy1 * srcW + sx1) * 4 + c];
                    dst[(y * dstW + x) * 4 + c] = static_cast<uint8_t>(sum / 4);
                }
            }
        }
        return dst;
    }

    CompressedTexture TextureCompressor::Compress(const uint8_t* rgba, uint32_t width, uint32_t height,
                                                  const CompressionOptions& options)
    {
        CompressedTexture result;

        if (!rgba)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "TextureCompressor::Compress — null pixel data");
            return result;
        }
        if (width == 0 || height == 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "TextureCompressor::Compress — invalid dimensions %ux%u",
                            width, height);
            return result;
        }

        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "TextureCompressor::Compress %ux%u format=%d mips=%s", width,
                        height, static_cast<int>(options.format), options.generateMipmaps ? "yes" : "no");

        result.width = width;
        result.height = height;
        result.originalSize = static_cast<size_t>(width) * height * 4;

        CompressionOptions opts = options;
        if (opts.isNormalMap && opts.format == TextureCompressionFormat::BC7)
            opts.format = TextureCompressionFormat::BC5;

        result.format = opts.format;

        // Calculate mip levels
        uint32_t mipCount = opts.generateMipmaps ? CalculateMipLevels(width, height) : 1;
        if (opts.maxMipLevels > 0)
            mipCount = std::min(mipCount, static_cast<uint32_t>(opts.maxMipLevels));
        result.mipLevels = mipCount;

        // Generate mip chain
        std::vector<std::vector<uint8_t>> mipPixels;
        mipPixels.emplace_back(rgba, rgba + width * height * 4);

        uint32_t mipW = width, mipH = height;
        for (uint32_t m = 1; m < mipCount; ++m)
        {
            auto mip = GenerateMipLevel(mipPixels.back().data(), mipW, mipH);
            mipW = std::max(mipW / 2, 1u);
            mipH = std::max(mipH / 2, 1u);
            mipPixels.push_back(std::move(mip));
        }

        // Compress each mip level
        size_t totalCompressed = 0;
        mipW = width;
        mipH = height;
        for (uint32_t m = 0; m < mipCount; ++m)
        {
            if (result.format == TextureCompressionFormat::Uncompressed)
            {
                result.mipData.push_back(mipPixels[m]);
                totalCompressed += mipPixels[m].size();
            }
            else
            {
                uint32_t blockSize = GetBlockSize(result.format);
                uint32_t blocksX = (mipW + 3) / 4;
                uint32_t blocksY = (mipH + 3) / 4;
                std::vector<uint8_t> compressed(static_cast<size_t>(blocksX) * blocksY * blockSize);

                for (uint32_t by = 0; by < blocksY; ++by)
                {
                    for (uint32_t bx = 0; bx < blocksX; ++bx)
                    {
                        // Extract 4x4 block
                        uint8_t block[64];
                        for (int py = 0; py < 4; ++py)
                        {
                            for (int px = 0; px < 4; ++px)
                            {
                                uint32_t sx = std::min(bx * 4 + px, mipW - 1);
                                uint32_t sy = std::min(by * 4 + py, mipH - 1);
                                std::memcpy(&block[(py * 4 + px) * 4], &mipPixels[m][(sy * mipW + sx) * 4], 4);
                            }
                        }

                        uint8_t* dst = &compressed[(static_cast<size_t>(by) * blocksX + bx) * blockSize];
                        if (result.format == TextureCompressionFormat::BC1)
                            CompressBlockBC1(block, dst);
                        else
                            CompressBlockBC7(block, dst);
                    }
                }

                totalCompressed += compressed.size();
                result.mipData.push_back(std::move(compressed));
            }

            mipW = std::max(mipW / 2, 1u);
            mipH = std::max(mipH / 2, 1u);
        }

        result.compressedSize = totalCompressed;
        result.compressionRatio = (result.originalSize > 0)
                                      ? static_cast<float>(totalCompressed) / static_cast<float>(result.originalSize)
                                      : 1.0f;

        m_texturesCompressed++;
        m_totalBytesProcessed += result.originalSize;
        m_totalBytesOutput += totalCompressed;

        return result;
    }

    std::vector<uint8_t> TextureCompressor::Decompress(const CompressedTexture& tex, uint32_t mipLevel)
    {
        if (mipLevel >= tex.mipLevels || mipLevel >= tex.mipData.size())
            return {};

        if (tex.format == TextureCompressionFormat::Uncompressed)
            return tex.mipData[mipLevel];

        uint32_t w = tex.width >> mipLevel;
        uint32_t h = tex.height >> mipLevel;
        w = std::max(w, 1u);
        h = std::max(h, 1u);

        // Simple decompression: return gray pixels as placeholder
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 128);
        return rgba;
    }

    bool TextureCompressor::SaveCompressed(const CompressedTexture& tex, const std::string& path)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "TextureCompressor::SaveCompressed failed to open '%s' for writing (errno=%d: %s)",
                            path.c_str(), errno, std::strerror(errno));
            return false;
        }

        // Magic + header
        const char magic[4] = {'S', 'T', 'E', 'X'};
        uint32_t version = 1;
        file.write(magic, 4);
        file.write(reinterpret_cast<const char*>(&version), 4);
        file.write(reinterpret_cast<const char*>(&tex.width), 4);
        file.write(reinterpret_cast<const char*>(&tex.height), 4);
        file.write(reinterpret_cast<const char*>(&tex.format), 1);
        file.write(reinterpret_cast<const char*>(&tex.mipLevels), 4);

        // Mip sizes
        for (uint32_t m = 0; m < tex.mipLevels; ++m)
        {
            uint32_t mipSize = static_cast<uint32_t>(tex.mipData[m].size());
            file.write(reinterpret_cast<const char*>(&mipSize), 4);
        }

        // Mip data
        for (uint32_t m = 0; m < tex.mipLevels; ++m)
        {
            file.write(reinterpret_cast<const char*>(tex.mipData[m].data()), tex.mipData[m].size());
        }

        if (!file.good())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "TextureCompressor::SaveCompressed write failure for '%s' (%ux%u, %u mips)", path.c_str(),
                            tex.width, tex.height, tex.mipLevels);
            return false;
        }
        return true;
    }

    CompressedTexture TextureCompressor::LoadCompressed(const std::string& path)
    {
        CompressedTexture tex;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "TextureCompressor::LoadCompressed failed to open '%s' (errno=%d: %s)", path.c_str(), errno,
                            std::strerror(errno));
            return tex;
        }

        char magic[4]{};
        uint32_t version = 0;
        file.read(magic, 4);
        if (!file.good())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "TextureCompressor::LoadCompressed truncated header (could not read 4-byte magic) in '%s'",
                            path.c_str());
            return tex;
        }
        if (magic[0] != 'S' || magic[1] != 'T' || magic[2] != 'E' || magic[3] != 'X')
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "TextureCompressor::LoadCompressed bad magic in '%s' — expected 'STEX', got "
                            "'%c%c%c%c' (0x%02x%02x%02x%02x)",
                            path.c_str(), std::isprint(static_cast<unsigned char>(magic[0])) ? magic[0] : '?',
                            std::isprint(static_cast<unsigned char>(magic[1])) ? magic[1] : '?',
                            std::isprint(static_cast<unsigned char>(magic[2])) ? magic[2] : '?',
                            std::isprint(static_cast<unsigned char>(magic[3])) ? magic[3] : '?',
                            static_cast<unsigned char>(magic[0]), static_cast<unsigned char>(magic[1]),
                            static_cast<unsigned char>(magic[2]), static_cast<unsigned char>(magic[3]));
            return tex;
        }

        file.read(reinterpret_cast<char*>(&version), 4);
        file.read(reinterpret_cast<char*>(&tex.width), 4);
        file.read(reinterpret_cast<char*>(&tex.height), 4);
        file.read(reinterpret_cast<char*>(&tex.format), 1);
        file.read(reinterpret_cast<char*>(&tex.mipLevels), 4);
        if (!file.good())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "TextureCompressor::LoadCompressed truncated header in '%s' (version=%u, %ux%u, mips=%u)",
                            path.c_str(), version, tex.width, tex.height, tex.mipLevels);
            tex = CompressedTexture{};
            return tex;
        }

        std::vector<uint32_t> mipSizes(tex.mipLevels);
        for (uint32_t m = 0; m < tex.mipLevels; ++m)
            file.read(reinterpret_cast<char*>(&mipSizes[m]), 4);
        if (!file.good())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "TextureCompressor::LoadCompressed truncated mip size table in '%s' (expected %u entries)",
                            path.c_str(), tex.mipLevels);
            tex = CompressedTexture{};
            return tex;
        }

        tex.compressedSize = 0;
        for (uint32_t m = 0; m < tex.mipLevels; ++m)
        {
            std::vector<uint8_t> data(mipSizes[m]);
            file.read(reinterpret_cast<char*>(data.data()), mipSizes[m]);
            if (!file.good())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                "TextureCompressor::LoadCompressed truncated mip %u in '%s' (expected %u bytes)", m,
                                path.c_str(), mipSizes[m]);
                tex = CompressedTexture{};
                return tex;
            }
            tex.compressedSize += mipSizes[m];
            tex.mipData.push_back(std::move(data));
        }

        tex.originalSize = static_cast<size_t>(tex.width) * tex.height * 4;
        tex.compressionRatio = (tex.originalSize > 0)
                                   ? static_cast<float>(tex.compressedSize) / static_cast<float>(tex.originalSize)
                                   : 1.0f;

        return tex;
    }

    std::string TextureCompressor::Console_GetStatus() const
    {
        return "TextureCompressor: " + std::to_string(m_texturesCompressed) + " textures, " +
               std::to_string(m_totalBytesProcessed / 1024) + "KB in, " + std::to_string(m_totalBytesOutput / 1024) +
               "KB out";
    }

} // namespace Spark::Graphics
