/**
 * @file BasisTranscoder.h
 * @brief GPU texture transcoding from Basis Universal format to native GPU formats
 * @author Spark Engine Team
 * @date 2025
 *
 * Transcodes Basis Universal compressed textures (.basis, .ktx2) to native GPU
 * formats at load time. A single compressed asset works across all RHI backends:
 * - BC1/BC3/BC7 for D3D11/D3D12
 * - ASTC for Metal/mobile
 * - ETC1/ETC2 for OpenGL ES
 *
 * Basis Universal provides 4-8x VRAM reduction vs. uncompressed RGBA. Two modes:
 * - ETC1S: Highest compression ratio, best for diffuse/color textures
 * - UASTC: Higher quality, best for normal maps and HDR data
 *
 * This is a transcoder-only implementation. Encoding is done offline by the
 * asset pipeline or SparkShaderCompiler.
 *
 * @see TextureSystem.h, RHI/RHITypes.h
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Transcoder Target Formats
    // =========================================================================

    /**
     * @brief GPU-compressed target formats for transcoding
     */
    enum class TranscoderFormat : uint8_t
    {
        // Desktop (D3D11/D3D12)
        BC1_RGB,  ///< 4 bpp, opaque (equivalent to DXT1)
        BC3_RGBA, ///< 8 bpp, with alpha (equivalent to DXT5)
        BC4_R,    ///< 4 bpp, single channel (grayscale/roughness)
        BC5_RG,   ///< 8 bpp, two channels (normal maps)
        BC7_RGBA, ///< 8 bpp, highest quality RGBA

        // Mobile
        ETC1_RGB,  ///< 4 bpp, OpenGL ES 2.0+
        ETC2_RGBA, ///< 8 bpp, OpenGL ES 3.0+
        ASTC_4x4,  ///< 8 bpp, Metal / mobile

        // Fallback
        RGBA32, ///< Uncompressed 32 bpp (always supported)

        Count
    };

    // =========================================================================
    // Basis File Header
    // =========================================================================

    /**
     * @brief Basis Universal file header (simplified)
     */
    struct BasisFileHeader
    {
        uint32_t magic = 0; ///< 'sB' (0x4273)
        uint16_t version = 0;
        uint16_t flags = 0;
        uint32_t totalImages = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 0;
        bool isETC1S = false; ///< true=ETC1S, false=UASTC
        bool hasAlpha = false;
    };

    /**
     * @brief Transcoded image data ready for GPU upload
     */
    struct TranscodedImage
    {
        std::vector<uint8_t> data;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevel = 0;
        TranscoderFormat format = TranscoderFormat::RGBA32;
        uint32_t rowPitch = 0; ///< Bytes per row (for block-compressed: rows of blocks)
    };

    // =========================================================================
    // Basis Transcoder
    // =========================================================================

    /**
     * @brief Transcodes Basis Universal textures to native GPU formats
     *
     * The engine currently exposes format selection, but does not ship a Basis
     * Universal decoder backend. Header parsing and transcoding therefore fail
     * closed instead of guessing the format layout or fabricating pixels. Link
     * basisu and replace the backend gate before accepting .basis content.
     *
     * Intended usage once a backend is available:
     * 1. Call SelectBestFormat() to pick the optimal format for the current RHI backend
     * 2. Call Transcode() to decode the Basis data into the target GPU format
     * 3. Upload the TranscodedImage data to an IRHITexture
     */
    class BasisTranscoder
    {
      public:
        BasisTranscoder() = default;
        ~BasisTranscoder() = default;

        /**
         * @brief Select the best transcoder format for the current GPU/API
         *
         * @param hasAlpha     Does the texture have alpha?
         * @param isNormalMap  Is this a normal map? (prefers BC5/RG formats)
         * @param isHDR        Is this HDR data? (prefers higher quality)
         * @return Best format for the current RHI backend
         */
        static TranscoderFormat SelectBestFormat(bool hasAlpha, bool isNormalMap, bool isHDR)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            // D3D11/D3D12: BC formats
            if (isNormalMap)
                return TranscoderFormat::BC5_RG;
            if (isHDR || hasAlpha)
                return TranscoderFormat::BC7_RGBA;
            return TranscoderFormat::BC1_RGB;
#elif defined(__APPLE__)
            return TranscoderFormat::ASTC_4x4;
#else
            // OpenGL / Linux fallback
            if (hasAlpha)
                return TranscoderFormat::ETC2_RGBA;
            return TranscoderFormat::ETC1_RGB;
#endif
        }

        /**
         * @brief Parse Basis file header without transcoding
         */
        static bool ParseHeader(const uint8_t* data, size_t dataSize, BasisFileHeader& outHeader)
        {
            (void)data;
            (void)dataSize;
            (void)outHeader;
            return false;
        }

        /**
         * @brief Transcode a Basis Universal file to a GPU format
         *
         * @param basisData    Raw .basis or .ktx2 file data
         * @param basisSize    Size in bytes
         * @param targetFormat Desired GPU format (use SelectBestFormat)
         * @param imageIndex   Image index (0 for single image)
         * @param mipLevel     Mip level to transcode
         * @return Transcoded image ready for GPU upload
         */
        static TranscodedImage Transcode(const uint8_t* basisData, size_t basisSize, TranscoderFormat targetFormat,
                                         uint32_t imageIndex = 0, uint32_t mipLevel = 0)
        {
            (void)basisData;
            (void)basisSize;
            (void)targetFormat;
            (void)imageIndex;
            (void)mipLevel;

            // No basisu backend is linked. Returning an empty result is an
            // explicit unsupported-format failure; synthetic texture data is
            // never safe to publish as a successful asset decode.
            return {};
        }

        /** @brief Get block size in bytes for a compressed format */
        static uint32_t GetBlockSize(TranscoderFormat format)
        {
            switch (format)
            {
            case TranscoderFormat::BC1_RGB:
            case TranscoderFormat::BC4_R:
            case TranscoderFormat::ETC1_RGB:
                return 8;
            case TranscoderFormat::BC3_RGBA:
            case TranscoderFormat::BC5_RG:
            case TranscoderFormat::BC7_RGBA:
            case TranscoderFormat::ETC2_RGBA:
            case TranscoderFormat::ASTC_4x4:
                return 16;
            case TranscoderFormat::RGBA32:
                return 4; // per pixel, not per block
            default:
                return 16;
            }
        }

        /** @brief Get DXGI format equivalent */
        static uint32_t GetDXGIFormat(TranscoderFormat format)
        {
            switch (format)
            {
            case TranscoderFormat::BC1_RGB:
                return 71; // DXGI_FORMAT_BC1_UNORM
            case TranscoderFormat::BC3_RGBA:
                return 77; // DXGI_FORMAT_BC3_UNORM
            case TranscoderFormat::BC4_R:
                return 80; // DXGI_FORMAT_BC4_UNORM
            case TranscoderFormat::BC5_RG:
                return 83; // DXGI_FORMAT_BC5_UNORM
            case TranscoderFormat::BC7_RGBA:
                return 98; // DXGI_FORMAT_BC7_UNORM
            case TranscoderFormat::RGBA32:
                return 28; // DXGI_FORMAT_R8G8B8A8_UNORM
            default:
                return 28;
            }
        }

        /** @brief Calculate VRAM savings ratio for a given format */
        static float GetCompressionRatio(TranscoderFormat format)
        {
            switch (format)
            {
            case TranscoderFormat::BC1_RGB:
            case TranscoderFormat::BC4_R:
            case TranscoderFormat::ETC1_RGB:
                return 8.0f; // 4 bpp vs 32 bpp
            case TranscoderFormat::BC3_RGBA:
            case TranscoderFormat::BC5_RG:
            case TranscoderFormat::BC7_RGBA:
            case TranscoderFormat::ETC2_RGBA:
            case TranscoderFormat::ASTC_4x4:
                return 4.0f; // 8 bpp vs 32 bpp
            default:
                return 1.0f;
            }
        }

      private:
        /**
         * @brief Decode Basis data to uncompressed RGBA
         *
         * This is the fallback path when GPU-compressed transcoding is not available.
         * ETC1S uses a codebook-based approach; UASTC uses per-block decompression.
         */
        static bool DecodeToRGBA(const uint8_t*, size_t, const BasisFileHeader&, uint32_t, uint32_t, uint8_t*)
        {
            return false;
        }

        /**
         * @brief Transcode Basis blocks to a target compressed format
         */
        static bool TranscodeBlocks(const uint8_t*, size_t, const BasisFileHeader&, uint32_t, uint32_t,
                                    TranscoderFormat, uint8_t*, size_t)
        {
            return false;
        }

        /**
         * @brief Encode a 4x4 RGBA block to a target compressed format
         */
        static void EncodeBlockToFormat(const uint8_t* rgba4x4, uint8_t* outBlock, TranscoderFormat format)
        {
            switch (format)
            {
            case TranscoderFormat::BC1_RGB:
                EncodeBC1(rgba4x4, outBlock);
                break;
            case TranscoderFormat::BC3_RGBA:
                EncodeBC3(rgba4x4, outBlock);
                break;
            case TranscoderFormat::BC4_R:
                EncodeBC4(rgba4x4, outBlock);
                break;
            case TranscoderFormat::BC5_RG:
                EncodeBC5(rgba4x4, outBlock);
                break;
            case TranscoderFormat::BC7_RGBA:
                EncodeBC7(rgba4x4, outBlock);
                break;
            case TranscoderFormat::ETC1_RGB:
                EncodeETC1(rgba4x4, outBlock);
                break;
            case TranscoderFormat::ETC2_RGBA:
                EncodeETC2(rgba4x4, outBlock);
                break;
            case TranscoderFormat::ASTC_4x4:
                EncodeASTC(rgba4x4, outBlock);
                break;
            default:
                memset(outBlock, 0, GetBlockSize(format));
                break;
            }
        }

        /**
         * @brief Quick BC1 (DXT1) encoder: finds min/max colors, builds 2-bit indices
         */
        static void EncodeBC1(const uint8_t* rgba4x4, uint8_t* out8bytes)
        {
            // Find min/max RGB across the 4x4 block
            uint8_t minR = 255, minG = 255, minB = 255;
            uint8_t maxR = 0, maxG = 0, maxB = 0;

            for (int i = 0; i < 16; ++i)
            {
                uint8_t r = rgba4x4[i * 4 + 0];
                uint8_t g = rgba4x4[i * 4 + 1];
                uint8_t b = rgba4x4[i * 4 + 2];
                minR = std::min(minR, r);
                minG = std::min(minG, g);
                minB = std::min(minB, b);
                maxR = std::max(maxR, r);
                maxG = std::max(maxG, g);
                maxB = std::max(maxB, b);
            }

            // Pack to RGB565
            uint16_t c0 = ((maxR >> 3) << 11) | ((maxG >> 2) << 5) | (maxB >> 3);
            uint16_t c1 = ((minR >> 3) << 11) | ((minG >> 2) << 5) | (minB >> 3);

            if (c0 < c1)
                std::swap(c0, c1);

            // Write color endpoints
            out8bytes[0] = static_cast<uint8_t>(c0 & 0xFF);
            out8bytes[1] = static_cast<uint8_t>(c0 >> 8);
            out8bytes[2] = static_cast<uint8_t>(c1 & 0xFF);
            out8bytes[3] = static_cast<uint8_t>(c1 >> 8);

            // Build 2-bit index table
            float maxR_f = maxR / 255.0f, maxG_f = maxG / 255.0f, maxB_f = maxB / 255.0f;
            float minR_f = minR / 255.0f, minG_f = minG / 255.0f, minB_f = minB / 255.0f;
            float rangeR = maxR_f - minR_f;
            float rangeG = maxG_f - minG_f;
            float rangeB = maxB_f - minB_f;
            float rangeSq = rangeR * rangeR + rangeG * rangeG + rangeB * rangeB;

            uint32_t indices = 0;
            for (int i = 0; i < 16; ++i)
            {
                float r = rgba4x4[i * 4 + 0] / 255.0f;
                float g = rgba4x4[i * 4 + 1] / 255.0f;
                float b = rgba4x4[i * 4 + 2] / 255.0f;

                float t = 0.0f;
                if (rangeSq > 0.0001f)
                    t = ((r - minR_f) * rangeR + (g - minG_f) * rangeG + (b - minB_f) * rangeB) / rangeSq;

                // Quantize to 2 bits (4 levels: 0, 1/3, 2/3, 1 → indices 0,2,3,1)
                int idx;
                if (t < 1.0f / 6.0f)
                    idx = 1; // c1
                else if (t < 3.0f / 6.0f)
                    idx = 3; // 1/3*c0 + 2/3*c1
                else if (t < 5.0f / 6.0f)
                    idx = 2; // 2/3*c0 + 1/3*c1
                else
                    idx = 0; // c0

                indices |= (idx << (i * 2));
            }

            out8bytes[4] = static_cast<uint8_t>((indices >> 0) & 0xFF);
            out8bytes[5] = static_cast<uint8_t>((indices >> 8) & 0xFF);
            out8bytes[6] = static_cast<uint8_t>((indices >> 16) & 0xFF);
            out8bytes[7] = static_cast<uint8_t>((indices >> 24) & 0xFF);
        }

        /**
         * @brief Quick BC3 encoder: BC1 for RGB + separate 8-bit alpha block
         */
        static void EncodeBC3(const uint8_t* rgba4x4, uint8_t* out16bytes)
        {
            // Alpha block (first 8 bytes)
            uint8_t minA = 255, maxA = 0;
            for (int i = 0; i < 16; ++i)
            {
                uint8_t a = rgba4x4[i * 4 + 3];
                minA = std::min(minA, a);
                maxA = std::max(maxA, a);
            }

            out16bytes[0] = maxA;
            out16bytes[1] = minA;

            // 3-bit alpha indices (48 bits = 6 bytes)
            uint64_t alphaIndices = 0;
            float alphaRange = static_cast<float>(maxA - minA);
            for (int i = 0; i < 16; ++i)
            {
                uint8_t a = rgba4x4[i * 4 + 3];
                float t = alphaRange > 0 ? static_cast<float>(a - minA) / alphaRange : 0.0f;
                uint64_t idx = static_cast<uint64_t>(std::clamp(static_cast<int>(t * 7.0f + 0.5f), 0, 7));
                // Remap: 0→0(maxA), 1→1(minA), 2-7 → interpolated
                if (idx == 0)
                    idx = 0;
                else if (idx == 7)
                    idx = 1;
                else
                    idx = idx + 1;
                alphaIndices |= (idx << (i * 3));
            }
            for (int i = 0; i < 6; ++i)
                out16bytes[2 + i] = static_cast<uint8_t>((alphaIndices >> (i * 8)) & 0xFF);

            // RGB block (last 8 bytes) using BC1
            EncodeBC1(rgba4x4, out16bytes + 8);
        }

        /**
         * @brief BC4 encoder: single red channel with 8-level interpolation
         */
        static void EncodeBC4(const uint8_t* rgba4x4, uint8_t* out8bytes)
        {
            uint8_t minR = 255, maxR = 0;
            for (int i = 0; i < 16; ++i)
            {
                uint8_t r = rgba4x4[i * 4 + 0];
                minR = std::min(minR, r);
                maxR = std::max(maxR, r);
            }

            out8bytes[0] = maxR;
            out8bytes[1] = minR;

            uint64_t indices = 0;
            float range = static_cast<float>(maxR - minR);
            for (int i = 0; i < 16; ++i)
            {
                uint8_t r = rgba4x4[i * 4 + 0];
                float t = range > 0 ? static_cast<float>(r - minR) / range : 0.0f;
                uint64_t idx = static_cast<uint64_t>(std::clamp(static_cast<int>(t * 7.0f + 0.5f), 0, 7));
                if (idx == 0)
                    idx = 0;
                else if (idx == 7)
                    idx = 1;
                else
                    idx = idx + 1;
                indices |= (idx << (i * 3));
            }
            for (int i = 0; i < 6; ++i)
                out8bytes[2 + i] = static_cast<uint8_t>((indices >> (i * 8)) & 0xFF);
        }

        /**
         * @brief BC5 encoder: two channels (RG) each with 8-level interpolation
         */
        static void EncodeBC5(const uint8_t* rgba4x4, uint8_t* out16bytes)
        {
            // Red channel (first 8 bytes)
            EncodeBC4(rgba4x4, out16bytes);

            // Green channel (second 8 bytes) — offset source by 1 byte
            uint8_t greenBlock[16 * 4];
            for (int i = 0; i < 16; ++i)
            {
                greenBlock[i * 4 + 0] = rgba4x4[i * 4 + 1]; // G → R slot
                greenBlock[i * 4 + 1] = 0;
                greenBlock[i * 4 + 2] = 0;
                greenBlock[i * 4 + 3] = 255;
            }
            EncodeBC4(greenBlock, out16bytes + 8);
        }

        /**
         * @brief BC7 encoder (mode 6): RGBA with per-pixel 4-bit indices
         *
         * Uses mode 6 (64 partitions, 7-bit endpoints, 4-bit indices) for
         * high-quality RGBA compression. Simplified single-mode encoder.
         */
        static void EncodeBC7(const uint8_t* rgba4x4, uint8_t* out16bytes)
        {
            memset(out16bytes, 0, 16);

            // Mode 6: 1 subset, RGBA 7777 endpoints, 4-bit indices
            // Mode bit: bit 6 set (0x40)
            out16bytes[0] = 0x40; // Mode 6

            // Find min/max RGBA endpoints
            uint8_t minC[4] = {255, 255, 255, 255};
            uint8_t maxC[4] = {0, 0, 0, 0};
            for (int i = 0; i < 16; ++i)
            {
                for (int c = 0; c < 4; ++c)
                {
                    minC[c] = std::min(minC[c], rgba4x4[i * 4 + c]);
                    maxC[c] = std::max(maxC[c], rgba4x4[i * 4 + c]);
                }
            }

            // Pack 7-bit endpoints (quantize 8-bit to 7-bit)
            uint8_t ep0[4], ep1[4];
            for (int c = 0; c < 4; ++c)
            {
                ep0[c] = maxC[c] >> 1;
                ep1[c] = minC[c] >> 1;
            }

            // Pack endpoints into bytes 1-7 (28 bits × 2 = 56 bits = 7 bytes)
            // Simplified: pack sequentially
            uint64_t endpointBits = 0;
            for (int c = 0; c < 4; ++c)
            {
                endpointBits |= static_cast<uint64_t>(ep0[c] & 0x7F) << (c * 14);
                endpointBits |= static_cast<uint64_t>(ep1[c] & 0x7F) << (c * 14 + 7);
            }
            for (int i = 0; i < 7; ++i)
                out16bytes[1 + i] = static_cast<uint8_t>((endpointBits >> (i * 8)) & 0xFF);

            // Pack 4-bit indices (16 pixels × 4 bits = 64 bits = 8 bytes)
            for (int i = 0; i < 16; ++i)
            {
                float totalDist = 0;
                float pixDist = 0;
                for (int c = 0; c < 4; ++c)
                {
                    float range = static_cast<float>(maxC[c]) - minC[c];
                    totalDist += range * range;
                    float d = static_cast<float>(rgba4x4[i * 4 + c]) - minC[c];
                    pixDist += d * range;
                }
                float t = totalDist > 0.0001f ? pixDist / totalDist : 0.0f;
                uint8_t idx = static_cast<uint8_t>(std::clamp(static_cast<int>(t * 15.0f + 0.5f), 0, 15));

                int bitOffset = i * 4;
                int byteIdx = 8 + bitOffset / 8;
                int bitPos = bitOffset % 8;
                if (byteIdx < 16)
                {
                    out16bytes[byteIdx] |= (idx << bitPos) & 0xFF;
                    if (bitPos > 4 && byteIdx + 1 < 16)
                        out16bytes[byteIdx + 1] |= idx >> (8 - bitPos);
                }
            }
        }

        /**
         * @brief ETC1 encoder: RGB block with two 4×2 subblocks
         */
        static void EncodeETC1(const uint8_t* rgba4x4, uint8_t* out8bytes)
        {
            memset(out8bytes, 0, 8);

            // Individual mode: two independent 4-bit RGB444 base colors
            // Compute average color for each 4x2 half
            float avgL[3] = {0, 0, 0};
            float avgR[3] = {0, 0, 0};
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 2; ++x)
                {
                    int idx = (y * 4 + x) * 4;
                    avgL[0] += rgba4x4[idx + 0];
                    avgL[1] += rgba4x4[idx + 1];
                    avgL[2] += rgba4x4[idx + 2];
                }
                for (int x = 2; x < 4; ++x)
                {
                    int idx = (y * 4 + x) * 4;
                    avgR[0] += rgba4x4[idx + 0];
                    avgR[1] += rgba4x4[idx + 1];
                    avgR[2] += rgba4x4[idx + 2];
                }
            }
            for (int c = 0; c < 3; ++c)
            {
                avgL[c] /= 8.0f;
                avgR[c] /= 8.0f;
            }

            // Pack base colors as RGB444 (4 bits each)
            uint8_t baseL[3] = {static_cast<uint8_t>(avgL[0] / 17.0f), static_cast<uint8_t>(avgL[1] / 17.0f),
                                static_cast<uint8_t>(avgL[2] / 17.0f)};
            uint8_t baseR[3] = {static_cast<uint8_t>(avgR[0] / 17.0f), static_cast<uint8_t>(avgR[1] / 17.0f),
                                static_cast<uint8_t>(avgR[2] / 17.0f)};

            // Pack header: individual mode (bit 1), flip bit (bit 0)
            out8bytes[0] = ((baseL[0] & 0xF) << 4) | (baseR[0] & 0xF);
            out8bytes[1] = ((baseL[1] & 0xF) << 4) | (baseR[1] & 0xF);
            out8bytes[2] = ((baseL[2] & 0xF) << 4) | (baseR[2] & 0xF);
            out8bytes[3] = 0x01; // Individual mode, flip=1 (horizontal split)

            // Pack 2-bit pixel indices (32 bits for 16 pixels)
            uint32_t indices = 0;
            for (int i = 0; i < 16; ++i)
            {
                int x = i % 4;
                float* avg = (x < 2) ? avgL : avgR;
                float luma = 0.299f * rgba4x4[i * 4 + 0] + 0.587f * rgba4x4[i * 4 + 1] + 0.114f * rgba4x4[i * 4 + 2];
                float baseLuma = 0.299f * avg[0] + 0.587f * avg[1] + 0.114f * avg[2];
                float diff = luma - baseLuma;

                uint32_t idx;
                if (diff > 20)
                    idx = 0; // Brightest
                else if (diff > 0)
                    idx = 2; // Slightly bright
                else if (diff > -20)
                    idx = 3; // Slightly dark
                else
                    idx = 1; // Darkest

                // ETC1 index ordering: MSB and LSB are stored separately
                uint32_t msb = (idx >> 1) & 1;
                uint32_t lsb = idx & 1;
                indices |= (msb << (i + 16)) | (lsb << i);
            }
            out8bytes[4] = static_cast<uint8_t>((indices >> 24) & 0xFF);
            out8bytes[5] = static_cast<uint8_t>((indices >> 16) & 0xFF);
            out8bytes[6] = static_cast<uint8_t>((indices >> 8) & 0xFF);
            out8bytes[7] = static_cast<uint8_t>((indices >> 0) & 0xFF);
        }

        /**
         * @brief ETC2 encoder: ETC1 RGB + separate 8-bit alpha block (EAC)
         */
        static void EncodeETC2(const uint8_t* rgba4x4, uint8_t* out16bytes)
        {
            // Alpha block (first 8 bytes) using EAC encoding
            uint8_t minA = 255, maxA = 0;
            for (int i = 0; i < 16; ++i)
            {
                minA = std::min(minA, rgba4x4[i * 4 + 3]);
                maxA = std::max(maxA, rgba4x4[i * 4 + 3]);
            }

            out16bytes[0] = maxA; // Base codeword
            out16bytes[1] = 0;    // Multiplier + table index

            // 3-bit alpha indices
            uint64_t alphaIndices = 0;
            float range = static_cast<float>(maxA - minA);
            for (int i = 0; i < 16; ++i)
            {
                float t = range > 0 ? static_cast<float>(rgba4x4[i * 4 + 3] - minA) / range : 0.0f;
                uint64_t idx = static_cast<uint64_t>(std::clamp(static_cast<int>(t * 7.0f + 0.5f), 0, 7));
                alphaIndices |= (idx << (i * 3));
            }
            for (int i = 0; i < 6; ++i)
                out16bytes[2 + i] = static_cast<uint8_t>((alphaIndices >> (i * 8)) & 0xFF);

            // RGB block (last 8 bytes) using ETC1
            EncodeETC1(rgba4x4, out16bytes + 8);
        }

        /**
         * @brief ASTC 4x4 encoder: simplified single-partition block
         *
         * ASTC blocks are 128 bits. Uses the simplest ASTC encoding mode:
         * single partition, RGBA endpoints, uniform quantization.
         */
        static void EncodeASTC(const uint8_t* rgba4x4, uint8_t* out16bytes)
        {
            memset(out16bytes, 0, 16);

            // ASTC block header: block mode + partition count
            // Use a simple void-extent block for solid colors, or a basic
            // single-partition CEM 12 (RGBA Direct) mode

            // Find min/max RGBA
            uint8_t minC[4] = {255, 255, 255, 255};
            uint8_t maxC[4] = {0, 0, 0, 0};
            for (int i = 0; i < 16; ++i)
            {
                for (int c = 0; c < 4; ++c)
                {
                    minC[c] = std::min(minC[c], rgba4x4[i * 4 + c]);
                    maxC[c] = std::max(maxC[c], rgba4x4[i * 4 + c]);
                }
            }

            // Check if block is near-solid (use void-extent)
            bool isSolid = true;
            for (int c = 0; c < 4; ++c)
            {
                if (maxC[c] - minC[c] > 2)
                {
                    isSolid = false;
                    break;
                }
            }

            if (isSolid)
            {
                // Void-extent block: constant color
                // Header: 0x1FC (void extent, no reserved bits)
                out16bytes[0] = 0xFC;
                out16bytes[1] = 0x01;
                // RGBA16 color values at bytes 8-15
                uint8_t avg[4];
                for (int c = 0; c < 4; ++c)
                    avg[c] = static_cast<uint8_t>((minC[c] + maxC[c]) / 2);

                // Pack as UNORM16 values
                for (int c = 0; c < 4; ++c)
                {
                    uint16_t val = static_cast<uint16_t>(avg[c]) * 257; // 8-bit to 16-bit
                    out16bytes[8 + c * 2] = static_cast<uint8_t>(val & 0xFF);
                    out16bytes[8 + c * 2 + 1] = static_cast<uint8_t>(val >> 8);
                }
            }
            else
            {
                // Single-partition block with CEM 12 (RGBA Direct)
                // Block mode: weight grid 4x4, high precision
                out16bytes[0] = 0x42; // Block mode for 4x4 weights
                out16bytes[1] = 0x80; // 1 partition, CEM=12

                // Endpoints: 8-bit RGBA min/max
                out16bytes[2] = minC[0];
                out16bytes[3] = maxC[0];
                out16bytes[4] = minC[1];
                out16bytes[5] = maxC[1];
                out16bytes[6] = minC[2];
                out16bytes[7] = maxC[2];
                out16bytes[8] = minC[3];
                out16bytes[9] = maxC[3];

                // Weight indices (2 bits per pixel, 32 bits total)
                uint32_t weights = 0;
                for (int i = 0; i < 16; ++i)
                {
                    float totalDist = 0;
                    float pixDist = 0;
                    for (int c = 0; c < 4; ++c)
                    {
                        float range = static_cast<float>(maxC[c]) - minC[c];
                        totalDist += range * range;
                        float d = static_cast<float>(rgba4x4[i * 4 + c]) - minC[c];
                        pixDist += d * range;
                    }
                    float t = totalDist > 0.0001f ? pixDist / totalDist : 0.0f;
                    uint32_t idx = static_cast<uint32_t>(std::clamp(static_cast<int>(t * 3.0f + 0.5f), 0, 3));
                    weights |= (idx << (i * 2));
                }
                out16bytes[10] = static_cast<uint8_t>((weights >> 0) & 0xFF);
                out16bytes[11] = static_cast<uint8_t>((weights >> 8) & 0xFF);
                out16bytes[12] = static_cast<uint8_t>((weights >> 16) & 0xFF);
                out16bytes[13] = static_cast<uint8_t>((weights >> 24) & 0xFF);
            }
        }
    };

} // namespace Spark::Graphics
