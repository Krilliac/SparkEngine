/**
 * @file GoldenImagePng.h
 * @brief Strict 8-bit PNG encode/decode for golden image baselines.
 *
 * Encoding uses the vendored miniz PNG writer (the same encoder
 * Graphics/ScreenCapture.h uses). Decoding is a deliberately narrow reader
 * for the files golden tests produce and review: 8-bit RGB or RGBA,
 * non-interlaced, all five scanline filters, with every chunk CRC checked.
 * Anything else (other bit depths, palettes, interlacing, truncated or
 * corrupt data, the legacy raw-RGBA layout) is rejected, so a baseline is
 * either decoded exactly or the comparison fails closed.
 *
 * The bundled ThirdParty/Utils/stb headers are API stubs whose encode and
 * decode calls always fail, so they cannot serve here.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#if defined(SPARK_MINIZ_AVAILABLE) && __has_include(<miniz.h>)
#include <miniz.h>
#define SPARK_GOLDEN_PNG_AVAILABLE 1
#endif

namespace Spark::GoldenPng
{
    /// Largest accepted image edge, in pixels.
    inline constexpr uint32_t kMaxDimension = 16384;

    /// Largest accepted PNG file, in bytes.
    inline constexpr size_t kMaxFileBytes = 256u * 1024u * 1024u;

    inline constexpr std::array<uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    /** @brief True when this build can encode and decode PNG files. */
    [[nodiscard]] constexpr bool IsAvailable()
    {
#ifdef SPARK_GOLDEN_PNG_AVAILABLE
        return true;
#else
        return false;
#endif
    }

    /**
     * @brief Encode tightly packed RGBA8 pixels (top row first) as a PNG file.
     * @return True when the complete file was written.
     */
    [[nodiscard]] inline bool WriteRGBA(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width,
                                        uint32_t height)
    {
        if (!rgba || width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension)
        {
            return false;
        }
#ifdef SPARK_GOLDEN_PNG_AVAILABLE
        size_t pngSize = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba, static_cast<int>(width), static_cast<int>(height),
                                                               4, &pngSize, MZ_DEFAULT_LEVEL, MZ_FALSE);
        if (!png)
        {
            return false;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (file)
        {
            file.write(static_cast<const char*>(png), static_cast<std::streamsize>(pngSize));
        }
        mz_free(png);
        return file.good();
#else
        (void)path;
        return false;
#endif
    }

    namespace Detail
    {
        [[nodiscard]] inline uint32_t ReadBE32(const uint8_t* p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }

        [[nodiscard]] inline uint8_t Paeth(uint8_t a, uint8_t b, uint8_t c)
        {
            const int p = int(a) + int(b) - int(c);
            const int pa = std::abs(p - int(a));
            const int pb = std::abs(p - int(b));
            const int pc = std::abs(p - int(c));
            if (pa <= pb && pa <= pc)
            {
                return a;
            }
            return (pb <= pc) ? b : c;
        }

        /**
         * @brief Undo PNG scanline filtering in place.
         * @param raw        Inflated data: height rows of (filter byte + stride bytes).
         * @param stride     Bytes per row without the filter byte.
         * @param bpp        Bytes per pixel.
         * @param unfiltered [out] height * stride reconstructed bytes.
         * @return False on an unknown filter type.
         */
        [[nodiscard]] inline bool Unfilter(const std::vector<uint8_t>& raw, size_t stride, size_t bpp, uint32_t height,
                                           std::vector<uint8_t>& unfiltered)
        {
            unfiltered.assign(stride * height, 0);
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint8_t filter = raw[y * (stride + 1)];
                const uint8_t* in = &raw[y * (stride + 1) + 1];
                uint8_t* out = &unfiltered[y * stride];
                const uint8_t* prev = (y > 0) ? &unfiltered[(y - 1) * stride] : nullptr;

                for (size_t x = 0; x < stride; ++x)
                {
                    const uint8_t left = (x >= bpp) ? out[x - bpp] : 0;
                    const uint8_t up = prev ? prev[x] : 0;
                    const uint8_t upLeft = (prev && x >= bpp) ? prev[x - bpp] : 0;
                    uint8_t predictor = 0;
                    switch (filter)
                    {
                    case 0:
                        predictor = 0;
                        break;
                    case 1:
                        predictor = left;
                        break;
                    case 2:
                        predictor = up;
                        break;
                    case 3:
                        predictor = static_cast<uint8_t>((int(left) + int(up)) / 2);
                        break;
                    case 4:
                        predictor = Paeth(left, up, upLeft);
                        break;
                    default:
                        return false;
                    }
                    out[x] = static_cast<uint8_t>(in[x] + predictor);
                }
            }
            return true;
        }
    } // namespace Detail

    /**
     * @brief Decode an 8-bit RGB/RGBA non-interlaced PNG file to RGBA8.
     * @param path   Input file.
     * @param width  [out] Image width (0 on failure).
     * @param height [out] Image height (0 on failure).
     * @return RGBA pixels, top row first; empty on any rejection.
     */
    [[nodiscard]] inline std::vector<uint8_t> ReadRGBA(const std::filesystem::path& path, uint32_t& width,
                                                       uint32_t& height)
    {
        width = 0;
        height = 0;
#ifdef SPARK_GOLDEN_PNG_AVAILABLE
        std::error_code ec;
        const auto fileSize = std::filesystem::file_size(path, ec);
        if (ec || fileSize > kMaxFileBytes)
        {
            return {};
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return {};
        }
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (bytes.size() < kSignature.size() || !std::equal(kSignature.begin(), kSignature.end(), bytes.begin()))
        {
            return {};
        }

        uint32_t w = 0;
        uint32_t h = 0;
        size_t bpp = 0;
        bool sawHeader = false;
        bool sawEnd = false;
        std::vector<uint8_t> compressed;

        size_t pos = kSignature.size();
        while (!sawEnd)
        {
            if (bytes.size() - pos < 12)
            {
                return {};
            }
            const uint32_t length = Detail::ReadBE32(&bytes[pos]);
            if (length > bytes.size() - pos - 12)
            {
                return {};
            }
            const uint8_t* type = &bytes[pos + 4];
            const uint8_t* data = &bytes[pos + 8];
            const uint32_t storedCrc = Detail::ReadBE32(data + length);
            if (static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, type, length + 4)) != storedCrc)
            {
                return {};
            }

            const std::string_view chunk(reinterpret_cast<const char*>(type), 4);
            if (!sawHeader && chunk != "IHDR")
            {
                return {};
            }
            if (chunk == "IHDR")
            {
                if (sawHeader || length != 13)
                {
                    return {};
                }
                w = Detail::ReadBE32(data);
                h = Detail::ReadBE32(data + 4);
                const uint8_t bitDepth = data[8];
                const uint8_t colorType = data[9];
                const bool supported = bitDepth == 8 && (colorType == 2 || colorType == 6) && data[10] == 0 &&
                                       data[11] == 0 && data[12] == 0;
                if (!supported || w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension)
                {
                    return {};
                }
                bpp = (colorType == 6) ? 4 : 3;
                sawHeader = true;
            }
            else if (chunk == "IDAT")
            {
                compressed.insert(compressed.end(), data, data + length);
            }
            else if (chunk == "IEND")
            {
                sawEnd = true;
            }
            else if ((type[0] & 0x20) == 0)
            {
                // Unknown critical chunk (e.g. PLTE on a truecolour image we do not handle).
                return {};
            }
            pos += size_t(length) + 12;
        }
        if (compressed.empty() || pos != bytes.size())
        {
            return {};
        }

        const size_t stride = size_t(w) * bpp;
        const size_t expected = (stride + 1) * h;
        std::vector<uint8_t> raw(expected);
        mz_ulong rawSize = static_cast<mz_ulong>(expected);
        if (mz_uncompress(raw.data(), &rawSize, compressed.data(), static_cast<mz_ulong>(compressed.size())) != MZ_OK ||
            rawSize != expected)
        {
            return {};
        }

        std::vector<uint8_t> unfiltered;
        if (!Detail::Unfilter(raw, stride, bpp, h, unfiltered))
        {
            return {};
        }

        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; ++i)
        {
            rgba[i * 4 + 0] = unfiltered[i * bpp + 0];
            rgba[i * 4 + 1] = unfiltered[i * bpp + 1];
            rgba[i * 4 + 2] = unfiltered[i * bpp + 2];
            rgba[i * 4 + 3] = (bpp == 4) ? unfiltered[i * bpp + 3] : 255;
        }
        width = w;
        height = h;
        return rgba;
#else
        (void)path;
        return {};
#endif
    }
} // namespace Spark::GoldenPng
