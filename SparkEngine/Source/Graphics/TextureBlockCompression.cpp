/**
 * @file TextureBlockCompression.cpp
 * @brief BC1 and BC7 block compression implementations
 */

#include "TextureBlockCompression.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace Spark::Graphics
{

    void CompressBlockBC1(const uint8_t* block4x4, uint8_t* output)
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

    void CompressBlockBC7(const uint8_t* block4x4, uint8_t* output)
    {
        constexpr std::array<uint8_t, 16> weights = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
        uint8_t sourceEndpoints[2][4] = {{255, 255, 255, 255}, {0, 0, 0, 0}};
        for (uint32_t texel = 0; texel < 16; ++texel)
        {
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                sourceEndpoints[0][channel] = std::min(sourceEndpoints[0][channel], block4x4[texel * 4 + channel]);
                sourceEndpoints[1][channel] = std::max(sourceEndpoints[1][channel], block4x4[texel * 4 + channel]);
            }
        }

        uint8_t endpoints[2][4]{};
        uint8_t pbits[2]{};
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
        {
            uint32_t bestError = std::numeric_limits<uint32_t>::max();
            for (uint32_t pbit = 0; pbit < 2; ++pbit)
            {
                uint8_t candidate[4]{};
                uint32_t error = 0;
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    const int source = sourceEndpoints[endpoint][channel];
                    const int quantized = std::clamp((source - static_cast<int>(pbit) + 1) / 2, 0, 127);
                    candidate[channel] = static_cast<uint8_t>(quantized);
                    const int reconstructed = (quantized << 1) | static_cast<int>(pbit);
                    const int delta = source - reconstructed;
                    error += static_cast<uint32_t>(delta * delta);
                }
                if (error < bestError)
                {
                    bestError = error;
                    pbits[endpoint] = static_cast<uint8_t>(pbit);
                    std::copy(candidate, candidate + 4, endpoints[endpoint]);
                }
            }
        }

        std::array<uint8_t, 16> indices{};
        for (uint32_t texel = 0; texel < 16; ++texel)
        {
            uint32_t bestError = std::numeric_limits<uint32_t>::max();
            for (uint32_t index = 0; index < weights.size(); ++index)
            {
                uint32_t error = 0;
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    const uint32_t endpoint0 = (static_cast<uint32_t>(endpoints[0][channel]) << 1) | pbits[0];
                    const uint32_t endpoint1 = (static_cast<uint32_t>(endpoints[1][channel]) << 1) | pbits[1];
                    const uint32_t reconstructed =
                        ((64u - weights[index]) * endpoint0 + weights[index] * endpoint1 + 32u) >> 6;
                    const int delta = static_cast<int>(block4x4[texel * 4 + channel]) - static_cast<int>(reconstructed);
                    error += static_cast<uint32_t>(delta * delta);
                }
                if (error < bestError)
                {
                    bestError = error;
                    indices[texel] = static_cast<uint8_t>(index);
                }
            }
        }

        // Mode 6 stores the anchor index in three bits. Reverse the endpoint
        // direction when necessary so texel zero remains representable.
        if (indices[0] >= 8)
        {
            for (uint32_t channel = 0; channel < 4; ++channel)
                std::swap(endpoints[0][channel], endpoints[1][channel]);
            std::swap(pbits[0], pbits[1]);
            for (auto& index : indices)
                index = static_cast<uint8_t>(15 - index);
        }

        std::memset(output, 0, 16);
        uint32_t bitPosition = 0;
        const auto writeBits = [&](uint32_t value, uint32_t bitCount)
        {
            for (uint32_t bit = 0; bit < bitCount; ++bit, ++bitPosition)
            {
                output[bitPosition / 8] |= static_cast<uint8_t>(((value >> bit) & 1u) << (bitPosition % 8));
            }
        };

        writeBits(0x40, 7); // Mode 6 one-hot selector.
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            writeBits(endpoints[0][channel], 7);
            writeBits(endpoints[1][channel], 7);
        }
        writeBits(pbits[0], 1);
        writeBits(pbits[1], 1);
        writeBits(indices[0], 3);
        for (uint32_t texel = 1; texel < indices.size(); ++texel)
            writeBits(indices[texel], 4);
    }

} // namespace Spark::Graphics
