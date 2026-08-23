/**
 * @file EXRLoader.h
 * @brief Bounded OpenEXR memory loading for HDR engine assets.
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#if defined(SPARK_HAS_TINYEXR) && SPARK_HAS_TINYEXR
#include <tinyexr.h>
#endif

namespace Spark::Graphics
{
    /** @brief Convert IEEE 754 half-precision to single-precision. */
    inline float HalfToFloat(uint16_t h)
    {
        const bool negative = (h & 0x8000u) != 0;
        const uint32_t exponent = (h >> 10) & 0x1Fu;
        const uint32_t mantissa = h & 0x3FFu;

        if (exponent == 0)
        {
            const float value = std::ldexp(static_cast<float>(mantissa), -24);
            return negative ? -value : value;
        }

        if (exponent == 31)
        {
            const uint32_t bits = (negative ? 0x80000000u : 0u) | 0x7F800000u | (mantissa << 13);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        const float value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
        return negative ? -value : value;
    }

    /** @brief Convert single-precision to IEEE 754 half-precision. */
    inline uint16_t FloatToHalf(float f)
    {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));

        const uint32_t sign = (bits >> 16) & 0x8000u;
        int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFFu;

        if (exponent <= 0)
        {
            if (exponent < -10)
                return static_cast<uint16_t>(sign);
            mantissa |= 0x800000u;
            const uint32_t shift = static_cast<uint32_t>(1 - exponent + 13);
            return static_cast<uint16_t>(sign | (mantissa >> shift));
        }
        if (exponent >= 31)
            return static_cast<uint16_t>(sign | 0x7C00u);

        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
    }

    struct EXRImage
    {
        std::vector<float> pixels; ///< RGBA float32 pixel data.
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 4;
        bool isHDR = true;
    };

    /**
     * @brief Loads a single-part EXR through the real tinyexr backend.
     *
     * The loader rejects unsupported layouts and oversized data windows before
     * decoding. Output is transactional: callers retain their previous image on
     * every failure. If tinyexr is unavailable, EXR loading fails closed.
     */
    class EXRLoader
    {
      public:
        static bool Load(const uint8_t* data, size_t dataSize, EXRImage& outImage)
        {
#if defined(SPARK_HAS_TINYEXR) && SPARK_HAS_TINYEXR
            if (!IsEXR(data, dataSize))
                return false;

            EXRVersion version{};
            if (ParseEXRVersionFromMemory(&version, data, dataSize) != TINYEXR_SUCCESS || version.tiled ||
                version.multipart || version.non_image)
                return false;

            EXRHeader header;
            InitEXRHeader(&header);
            const char* error = nullptr;
            if (ParseEXRHeaderFromMemory(&header, &version, data, dataSize, &error) != TINYEXR_SUCCESS)
            {
                if (error)
                    FreeEXRErrorMessage(error);
                FreeEXRHeader(&header);
                return false;
            }

            const int64_t width = static_cast<int64_t>(header.data_window.max_x) - header.data_window.min_x + 1;
            const int64_t height = static_cast<int64_t>(header.data_window.max_y) - header.data_window.min_y + 1;
            constexpr int64_t kMaxDimension = 16'384;
            constexpr uint64_t kMaxPixels = 16'777'216; // 256 MiB RGBA32F output cap.
            const bool invalidDimensions = width <= 0 || height <= 0 || width > kMaxDimension ||
                                           height > kMaxDimension ||
                                           static_cast<uint64_t>(width) > kMaxPixels / static_cast<uint64_t>(height);
            FreeEXRHeader(&header);
            if (error)
                FreeEXRErrorMessage(error);
            if (invalidDimensions)
                return false;

            float* rgba = nullptr;
            int decodedWidth = 0;
            int decodedHeight = 0;
            error = nullptr;
            const int result = LoadEXRFromMemory(&rgba, &decodedWidth, &decodedHeight, data, dataSize, &error);
            if (result != TINYEXR_SUCCESS || !rgba || decodedWidth != width || decodedHeight != height)
            {
                std::free(rgba);
                if (error)
                    FreeEXRErrorMessage(error);
                return false;
            }

            EXRImage parsed;
            parsed.width = static_cast<uint32_t>(decodedWidth);
            parsed.height = static_cast<uint32_t>(decodedHeight);
            parsed.channels = 4;
            parsed.isHDR = true;
            const size_t componentCount = static_cast<size_t>(decodedWidth) * static_cast<size_t>(decodedHeight) * 4;
            try
            {
                parsed.pixels.assign(rgba, rgba + componentCount);
            }
            catch (...)
            {
                std::free(rgba);
                if (error)
                    FreeEXRErrorMessage(error);
                return false;
            }

            std::free(rgba);
            if (error)
                FreeEXRErrorMessage(error);
            outImage = std::move(parsed);
            return true;
#else
            (void)data;
            (void)dataSize;
            (void)outImage;
            return false;
#endif
        }

        static bool IsEXR(const uint8_t* data, size_t size)
        {
            return data && size >= 4 && data[0] == 0x76 && data[1] == 0x2F && data[2] == 0x31 && data[3] == 0x01;
        }
    };
} // namespace Spark::Graphics
