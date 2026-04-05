/**
 * @file TextureCompressor.cpp
 * @brief GPU texture compression implementation
 */

#include "TextureCompressor.h"

#include "Utils/LogMacros.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <cmath>
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

    void TextureCompressor::CompressBlockBC1(const uint8_t* block4x4, uint8_t* output)
    {
        // Find min/max RGB colors in the 4x4 block
        uint8_t minR = 255, minG = 255, minB = 255;
        uint8_t maxR = 0, maxG = 0, maxB = 0;

        for (int i = 0; i < 16; ++i)
        {
            uint8_t r = block4x4[i * 4 + 0];
            uint8_t g = block4x4[i * 4 + 1];
            uint8_t b = block4x4[i * 4 + 2];
            minR = std::min(minR, r);
            minG = std::min(minG, g);
            minB = std::min(minB, b);
            maxR = std::max(maxR, r);
            maxG = std::max(maxG, g);
            maxB = std::max(maxB, b);
        }

        // Quantize to RGB565
        uint16_t color0 = ((maxR >> 3) << 11) | ((maxG >> 2) << 5) | (maxB >> 3);
        uint16_t color1 = ((minR >> 3) << 11) | ((minG >> 2) << 5) | (minB >> 3);

        // Ensure color0 > color1 for 4-color mode
        if (color0 < color1)
            std::swap(color0, color1);

        // Write endpoint colors
        output[0] = static_cast<uint8_t>(color0 & 0xFF);
        output[1] = static_cast<uint8_t>(color0 >> 8);
        output[2] = static_cast<uint8_t>(color1 & 0xFF);
        output[3] = static_cast<uint8_t>(color1 >> 8);

        // Compute interpolated colors for index assignment
        float r0 = static_cast<float>(maxR), g0 = static_cast<float>(maxG), b0 = static_cast<float>(maxB);
        float r1 = static_cast<float>(minR), g1 = static_cast<float>(minG), b1 = static_cast<float>(minB);

        float palette[4][3] = {
            {r0, g0, b0},
            {r1, g1, b1},
            {r0 * 2.0f / 3.0f + r1 / 3.0f, g0 * 2.0f / 3.0f + g1 / 3.0f, b0 * 2.0f / 3.0f + b1 / 3.0f},
            {r0 / 3.0f + r1 * 2.0f / 3.0f, g0 / 3.0f + g1 * 2.0f / 3.0f, b0 / 3.0f + b1 * 2.0f / 3.0f}};

        // Assign 2-bit indices per texel
        uint32_t indices = 0;
        for (int i = 0; i < 16; ++i)
        {
            float pr = static_cast<float>(block4x4[i * 4 + 0]);
            float pg = static_cast<float>(block4x4[i * 4 + 1]);
            float pb = static_cast<float>(block4x4[i * 4 + 2]);

            float bestDist = 1e10f;
            uint32_t bestIdx = 0;
            for (uint32_t j = 0; j < 4; ++j)
            {
                float dr = pr - palette[j][0];
                float dg = pg - palette[j][1];
                float db = pb - palette[j][2];
                float dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestIdx = j;
                }
            }
            indices |= (bestIdx << (i * 2));
        }

        output[4] = static_cast<uint8_t>((indices >> 0) & 0xFF);
        output[5] = static_cast<uint8_t>((indices >> 8) & 0xFF);
        output[6] = static_cast<uint8_t>((indices >> 16) & 0xFF);
        output[7] = static_cast<uint8_t>((indices >> 24) & 0xFF);
    }

    void TextureCompressor::CompressBlockBC7(const uint8_t* block4x4, uint8_t* output)
    {
        // BC7 Mode 6: 2 RGBA endpoints, 4-bit indices, partition 0
        // This is a simplified implementation for the most common mode

        // Find min/max RGBA
        uint8_t minC[4] = {255, 255, 255, 255};
        uint8_t maxC[4] = {0, 0, 0, 0};
        for (int i = 0; i < 16; ++i)
        {
            for (int c = 0; c < 4; ++c)
            {
                minC[c] = std::min(minC[c], block4x4[i * 4 + c]);
                maxC[c] = std::max(maxC[c], block4x4[i * 4 + c]);
            }
        }

        // Zero-fill output (16 bytes)
        std::memset(output, 0, 16);

        // Mode 6: bit 6 set
        output[0] = 0x40;

        // Pack endpoints (7 bits per channel, 2 endpoints)
        // Simplified: store quantized endpoints in bytes 1-8
        for (int c = 0; c < 4; ++c)
        {
            output[1 + c] = maxC[c];
            output[5 + c] = minC[c];
        }

        // Assign 4-bit indices
        for (int i = 0; i < 16; ++i)
        {
            float dist = 0.0f;
            float range = 0.0f;
            for (int c = 0; c < 4; ++c)
            {
                float d = static_cast<float>(block4x4[i * 4 + c]) - static_cast<float>(minC[c]);
                float r = static_cast<float>(maxC[c]) - static_cast<float>(minC[c]);
                dist += d;
                range += r;
            }
            uint8_t idx = (range > 0) ? static_cast<uint8_t>(std::clamp(dist / range * 15.0f, 0.0f, 15.0f)) : 0;
            int byteIdx = 9 + (i / 2);
            if (i % 2 == 0)
                output[byteIdx] |= (idx & 0x0F);
            else
                output[byteIdx] |= ((idx & 0x0F) << 4);
        }
    }

    std::vector<uint8_t> TextureCompressor::GenerateMipLevel(const uint8_t* src, uint32_t srcW, uint32_t srcH)
    {
        uint32_t dstW = std::max(srcW / 2, 1u);
        uint32_t dstH = std::max(srcH / 2, 1u);
        std::vector<uint8_t> dst(dstW * dstH * 4);

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
                std::vector<uint8_t> compressed(blocksX * blocksY * blockSize);

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

                        uint8_t* dst = &compressed[(by * blocksX + bx) * blockSize];
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
        std::vector<uint8_t> rgba(w * h * 4, 128);
        return rgba;
    }

    bool TextureCompressor::SaveCompressed(const CompressedTexture& tex, const std::string& path)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open())
            return false;

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

        return file.good();
    }

    CompressedTexture TextureCompressor::LoadCompressed(const std::string& path)
    {
        CompressedTexture tex;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return tex;

        char magic[4];
        uint32_t version;
        file.read(magic, 4);
        if (magic[0] != 'S' || magic[1] != 'T' || magic[2] != 'E' || magic[3] != 'X')
            return tex;

        file.read(reinterpret_cast<char*>(&version), 4);
        file.read(reinterpret_cast<char*>(&tex.width), 4);
        file.read(reinterpret_cast<char*>(&tex.height), 4);
        file.read(reinterpret_cast<char*>(&tex.format), 1);
        file.read(reinterpret_cast<char*>(&tex.mipLevels), 4);

        std::vector<uint32_t> mipSizes(tex.mipLevels);
        for (uint32_t m = 0; m < tex.mipLevels; ++m)
            file.read(reinterpret_cast<char*>(&mipSizes[m]), 4);

        tex.compressedSize = 0;
        for (uint32_t m = 0; m < tex.mipLevels; ++m)
        {
            std::vector<uint8_t> data(mipSizes[m]);
            file.read(reinterpret_cast<char*>(data.data()), mipSizes[m]);
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
