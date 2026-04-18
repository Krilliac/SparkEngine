/**
 * @file Quantization.cpp
 * @brief Shared implementations of smallest-three 48-bit quaternion packing
 *        and 16-bit bounded-vector packing.
 */

#include "Quantization.h"

#include <algorithm>
#include <cmath>

namespace Spark::Utils::Quantization
{
    namespace
    {
        constexpr int32_t QuatQuantMax = 32767;
        constexpr float QuatComponentRange = 0.70710678118f; // 1 / sqrt(2)
        constexpr float VectorQuantMax = 65535.0f;

        inline uint16_t QuantizeQuatComponent(float value)
        {
            const float clamped = std::max(-QuatComponentRange, std::min(QuatComponentRange, value));
            const float normalized = (clamped + QuatComponentRange) / (2.0f * QuatComponentRange);
            const int32_t quantized = static_cast<int32_t>(normalized * static_cast<float>(QuatQuantMax) + 0.5f);
            return static_cast<uint16_t>(std::max(0, std::min(QuatQuantMax, quantized)));
        }

        inline float DequantizeQuatComponent(uint16_t value)
        {
            const float normalized = static_cast<float>(value) / static_cast<float>(QuatQuantMax);
            return normalized * (2.0f * QuatComponentRange) - QuatComponentRange;
        }

        inline uint16_t QuantizeAxis(float value, float minVal, float maxVal)
        {
            const float range = maxVal - minVal;
            if (range <= 0.0f)
            {
                return 0;
            }
            const float normalized = (value - minVal) / range;
            const float clamped = std::max(0.0f, std::min(1.0f, normalized));
            return static_cast<uint16_t>(clamped * VectorQuantMax + 0.5f);
        }

        inline float DequantizeAxis(uint16_t value, float minVal, float maxVal)
        {
            const float range = maxVal - minVal;
            if (range <= 0.0f)
            {
                return minVal;
            }
            const float normalized = static_cast<float>(value) / VectorQuantMax;
            return minVal + normalized * range;
        }
    } // namespace

    PackedQuat48 PackQuat48(const XMFLOAT4& q)
    {
        float components[4] = {q.x, q.y, q.z, q.w};
        int32_t largestIndex = 0;
        float largestAbs = std::abs(components[0]);

        for (int32_t i = 1; i < 4; ++i)
        {
            const float absVal = std::abs(components[i]);
            if (absVal > largestAbs)
            {
                largestAbs = absVal;
                largestIndex = i;
            }
        }

        // Ensure the dropped component is positive: q and -q are the same rotation.
        if (components[largestIndex] < 0.0f)
        {
            components[0] = -components[0];
            components[1] = -components[1];
            components[2] = -components[2];
            components[3] = -components[3];
        }

        float kept[3];
        int32_t keptIdx = 0;
        for (int32_t i = 0; i < 4; ++i)
        {
            if (i != largestIndex)
            {
                kept[keptIdx++] = components[i];
            }
        }

        const uint16_t a = QuantizeQuatComponent(kept[0]);
        const uint16_t b = QuantizeQuatComponent(kept[1]);
        const uint16_t c = QuantizeQuatComponent(kept[2]);

        // Bit layout across 48 bits:
        //   [47:46] = largest-index (2 bits)
        //   [45:31] = component a   (15 bits)
        //   [30:16] = component b   (15 bits)
        //   [15: 1] = component c   (15 bits)
        //   [    0] = unused (reserved, kept zero for forward compatibility)
        uint64_t packed = 0;
        packed |= (static_cast<uint64_t>(largestIndex) & 0x3) << 46;
        packed |= (static_cast<uint64_t>(a) & 0x7FFF) << 31;
        packed |= (static_cast<uint64_t>(b) & 0x7FFF) << 16;
        packed |= (static_cast<uint64_t>(c) & 0x7FFF) << 1;

        PackedQuat48 result;
        result.data[0] = static_cast<uint8_t>((packed >> 40) & 0xFF);
        result.data[1] = static_cast<uint8_t>((packed >> 32) & 0xFF);
        result.data[2] = static_cast<uint8_t>((packed >> 24) & 0xFF);
        result.data[3] = static_cast<uint8_t>((packed >> 16) & 0xFF);
        result.data[4] = static_cast<uint8_t>((packed >> 8) & 0xFF);
        result.data[5] = static_cast<uint8_t>((packed >> 0) & 0xFF);
        return result;
    }

    XMFLOAT4 UnpackQuat48(const PackedQuat48& packed)
    {
        uint64_t bits = 0;
        bits |= static_cast<uint64_t>(packed.data[0]) << 40;
        bits |= static_cast<uint64_t>(packed.data[1]) << 32;
        bits |= static_cast<uint64_t>(packed.data[2]) << 24;
        bits |= static_cast<uint64_t>(packed.data[3]) << 16;
        bits |= static_cast<uint64_t>(packed.data[4]) << 8;
        bits |= static_cast<uint64_t>(packed.data[5]) << 0;

        const int32_t largestIndex = static_cast<int32_t>((bits >> 46) & 0x3);
        const uint16_t qa = static_cast<uint16_t>((bits >> 31) & 0x7FFF);
        const uint16_t qb = static_cast<uint16_t>((bits >> 16) & 0x7FFF);
        const uint16_t qc = static_cast<uint16_t>((bits >> 1) & 0x7FFF);

        const float a = DequantizeQuatComponent(qa);
        const float b = DequantizeQuatComponent(qb);
        const float c = DequantizeQuatComponent(qc);

        const float sumSquares = a * a + b * b + c * c;
        const float d = std::sqrt(std::max(0.0f, 1.0f - sumSquares));

        float result[4];
        int32_t keptIdx = 0;
        for (int32_t i = 0; i < 4; ++i)
        {
            if (i == largestIndex)
            {
                result[i] = d;
            }
            else
            {
                switch (keptIdx)
                {
                case 0:
                    result[i] = a;
                    break;
                case 1:
                    result[i] = b;
                    break;
                case 2:
                    result[i] = c;
                    break;
                default:
                    result[i] = 0.0f;
                    break;
                }
                ++keptIdx;
            }
        }
        return {result[0], result[1], result[2], result[3]};
    }

    PackedVec16 PackVec16(const XMFLOAT3& v, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax)
    {
        return {QuantizeAxis(v.x, rangeMin.x, rangeMax.x), QuantizeAxis(v.y, rangeMin.y, rangeMax.y),
                QuantizeAxis(v.z, rangeMin.z, rangeMax.z)};
    }

    XMFLOAT3 UnpackVec16(const PackedVec16& packed, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax)
    {
        return {DequantizeAxis(packed.x, rangeMin.x, rangeMax.x), DequantizeAxis(packed.y, rangeMin.y, rangeMax.y),
                DequantizeAxis(packed.z, rangeMin.z, rangeMax.z)};
    }

} // namespace Spark::Utils::Quantization
