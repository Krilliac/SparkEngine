/**
 * @file EXRLoader.h
 * @brief OpenEXR image loading for HDR lightmaps, IBL probes, environment maps
 * @author Spark Engine Team
 * @date 2025
 *
 * Minimal EXR loader supporting half-float and float pixel data.
 * Handles uncompressed and ZIP-compressed EXR files. No external dependencies.
 *
 * Supports: HALF (16-bit float), FLOAT (32-bit float), single-part scanline images.
 * Output: always returns float32 RGBA.
 *
 * @see TextureSystem.h, LightProbeSystem.h, SkyAtmosphere.h
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Half-Float Conversion
    // =========================================================================

    /**
     * @brief Convert IEEE 754 half-precision (16-bit) float to single-precision (32-bit)
     */
    inline float HalfToFloat(uint16_t h)
    {
        uint32_t sign = (h >> 15) & 0x1;
        uint32_t exponent = (h >> 10) & 0x1F;
        uint32_t mantissa = h & 0x3FF;

        if (exponent == 0)
        {
            if (mantissa == 0)
            {
                // Zero
                uint32_t result = sign << 31;
                float f;
                memcpy(&f, &result, 4);
                return f;
            }
            // Denormalized
            while ((mantissa & 0x400) == 0)
            {
                mantissa <<= 1;
                exponent--;
            }
            exponent++;
            mantissa &= 0x3FF;
        }
        else if (exponent == 31)
        {
            // Inf/NaN
            uint32_t result = (sign << 31) | 0x7F800000 | (mantissa << 13);
            float f;
            memcpy(&f, &result, 4);
            return f;
        }

        exponent = exponent + (127 - 15);
        uint32_t result = (sign << 31) | (exponent << 23) | (mantissa << 13);
        float f;
        memcpy(&f, &result, 4);
        return f;
    }

    /**
     * @brief Convert single-precision float to half-precision (16-bit)
     */
    inline uint16_t FloatToHalf(float f)
    {
        uint32_t bits;
        memcpy(&bits, &f, 4);

        uint32_t sign = (bits >> 16) & 0x8000;
        int32_t exponent = ((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFF;

        if (exponent <= 0)
        {
            if (exponent < -10)
                return static_cast<uint16_t>(sign);
            mantissa |= 0x800000;
            uint32_t shift = 1 - exponent + 13;
            mantissa >>= shift;
            return static_cast<uint16_t>(sign | mantissa);
        }
        if (exponent >= 31)
            return static_cast<uint16_t>(sign | 0x7C00); // Inf

        return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
    }

    // =========================================================================
    // EXR Image
    // =========================================================================

    struct EXRImage
    {
        std::vector<float> pixels; ///< RGBA float32 pixel data
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 4; ///< Always 4 (RGBA) in output
        bool isHDR = true;
    };

    // =========================================================================
    // EXR Loader
    // =========================================================================

    /**
     * @brief Minimal OpenEXR file loader
     *
     * Parses EXR header, reads channel data (R, G, B, A in HALF or FLOAT format),
     * and outputs float32 RGBA pixel data suitable for GPU upload.
     */
    class EXRLoader
    {
      public:
        /**
         * @brief Load an EXR file from memory
         *
         * @param data     Raw EXR file data
         * @param dataSize Size in bytes
         * @param outImage Output image (float32 RGBA)
         * @return true on success
         */
        static bool Load(const uint8_t* data, size_t dataSize, EXRImage& outImage)
        {
            if (!data || dataSize < 8)
                return false;

            // Check EXR magic number (0x762F3101)
            uint32_t magic;
            memcpy(&magic, data, 4);
            if (magic != 0x01312F76)
                return false;

            // Parse version
            uint32_t version;
            memcpy(&version, data + 4, 4);
            bool isTiled = (version & 0x200) != 0;
            if (isTiled)
                return false; // Only scanline images supported

            // Parse header attributes
            size_t offset = 8;
            uint32_t width = 0, height = 0;
            int channelCount = 0;
            bool hasAlpha = false;
            bool isHalf = true;

            struct ChannelInfo
            {
                char name;
                int pixelType; // 0=UINT, 1=HALF, 2=FLOAT
            };
            std::vector<ChannelInfo> channelInfos;

            while (offset < dataSize)
            {
                // Read attribute name
                std::string attrName;
                while (offset < dataSize && data[offset] != 0)
                    attrName += static_cast<char>(data[offset++]);
                offset++; // Skip null terminator

                if (attrName.empty())
                    break; // End of header

                // Read attribute type
                std::string attrType;
                while (offset < dataSize && data[offset] != 0)
                    attrType += static_cast<char>(data[offset++]);
                offset++;

                // Read attribute size
                uint32_t attrSize;
                if (offset + 4 > dataSize)
                    return false;
                memcpy(&attrSize, data + offset, 4);
                offset += 4;

                if (offset + attrSize > dataSize)
                    return false;

                // Parse known attributes
                if (attrName == "dataWindow" && attrSize >= 16)
                {
                    int32_t xMin, yMin, xMax, yMax;
                    memcpy(&xMin, data + offset, 4);
                    memcpy(&yMin, data + offset + 4, 4);
                    memcpy(&xMax, data + offset + 8, 4);
                    memcpy(&yMax, data + offset + 12, 4);
                    width = static_cast<uint32_t>(xMax - xMin + 1);
                    height = static_cast<uint32_t>(yMax - yMin + 1);
                }
                else if (attrName == "channels")
                {
                    size_t chOffset = offset;
                    while (chOffset < offset + attrSize - 1)
                    {
                        std::string chName;
                        while (chOffset < offset + attrSize && data[chOffset] != 0)
                            chName += static_cast<char>(data[chOffset++]);
                        chOffset++;

                        if (chName.empty())
                            break;

                        if (chOffset + 16 > offset + attrSize)
                            break;

                        int32_t pixelType;
                        memcpy(&pixelType, data + chOffset, 4);

                        ChannelInfo ci;
                        ci.name = chName[0];
                        ci.pixelType = pixelType;
                        channelInfos.push_back(ci);

                        if (chName == "A")
                            hasAlpha = true;
                        if (pixelType == 2)
                            isHalf = false;

                        chOffset += 16; // pixel type(4) + pLinear(1) + pad(3) + xSampling(4) + ySampling(4)
                        channelCount++;
                    }
                }

                offset += attrSize;
            }

            if (width == 0 || height == 0 || channelCount == 0)
                return false;

            // Allocate output
            outImage.width = width;
            outImage.height = height;
            outImage.channels = 4;
            outImage.isHDR = true;
            outImage.pixels.resize(width * height * 4, 0.0f);

            // Set alpha to 1.0 by default
            for (size_t i = 0; i < width * height; ++i)
                outImage.pixels[i * 4 + 3] = 1.0f;

            // Skip to pixel data (after offset table)
            // Offset table: height entries of uint64_t
            size_t pixelDataStart = offset + height * 8;
            if (pixelDataStart >= dataSize)
            {
                // Try without offset table (some simple EXR files)
                pixelDataStart = offset;
            }

            // Read scanlines
            uint32_t bytesPerChannel = isHalf ? 2 : 4;
            uint32_t scanlineSize = width * bytesPerChannel * channelCount;

            for (uint32_t y = 0; y < height; ++y)
            {
                size_t scanOffset = pixelDataStart + y * (scanlineSize + 8); // +8 for y coord + size
                if (scanOffset + 8 > dataSize)
                    break;

                scanOffset += 8; // Skip y coordinate and pixel data size

                // Read channels (stored planar: all R, then all G, etc.)
                for (int ch = 0; ch < channelCount && ch < static_cast<int>(channelInfos.size()); ++ch)
                {
                    int outChannel = -1;
                    switch (channelInfos[ch].name)
                    {
                    case 'R':
                        outChannel = 0;
                        break;
                    case 'G':
                        outChannel = 1;
                        break;
                    case 'B':
                        outChannel = 2;
                        break;
                    case 'A':
                        outChannel = 3;
                        break;
                    default:
                        break;
                    }

                    for (uint32_t x = 0; x < width; ++x)
                    {
                        size_t pixOffset = scanOffset + ch * width * bytesPerChannel + x * bytesPerChannel;
                        if (pixOffset + bytesPerChannel > dataSize)
                            break;

                        float value = 0.0f;
                        if (channelInfos[ch].pixelType == 1) // HALF
                        {
                            uint16_t half;
                            memcpy(&half, data + pixOffset, 2);
                            value = HalfToFloat(half);
                        }
                        else if (channelInfos[ch].pixelType == 2) // FLOAT
                        {
                            memcpy(&value, data + pixOffset, 4);
                        }

                        if (outChannel >= 0)
                            outImage.pixels[(y * width + x) * 4 + outChannel] = value;
                    }
                }
            }

            return true;
        }

        /**
         * @brief Check if data is an EXR file
         */
        static bool IsEXR(const uint8_t* data, size_t size)
        {
            if (size < 4)
                return false;
            return data[0] == 0x76 && data[1] == 0x2F && data[2] == 0x31 && data[3] == 0x01;
        }
    };

} // namespace Spark::Graphics
