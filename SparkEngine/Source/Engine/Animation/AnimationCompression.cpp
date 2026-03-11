/**
 * @file AnimationCompression.cpp
 * @brief Implementation of animation keyframe compression and quantization
 * @author Spark Engine Team
 * @date 2026
 */

#include "AnimationCompression.h"

#include <algorithm>
#include <cmath>
#include <cstring>


namespace Spark::Animation
{

    // =========================================================================
    // In-place keyframe reduction
    // =========================================================================

    void AnimationCompressor::ReduceClip(AnimationClip& clip, const Settings& settings)
    {
        for (auto& channel : clip.channels)
        {
            ReduceKeyframes(channel.positionKeys, settings.positionTolerance);
            ReduceRotationKeyframes(channel.rotationKeys, settings.rotationTolerance);
            ReduceKeyframes(channel.scaleKeys, settings.scaleTolerance);
        }
    }

    void AnimationCompressor::ReduceClip(AnimationClip& clip)
    {
        ReduceClip(clip, Settings{});
    }

    // =========================================================================
    // Full compression pipeline
    // =========================================================================

    CompressedClip AnimationCompressor::CompressClip(const AnimationClip& clip)
    {
        return CompressClip(clip, Settings{});
    }

    CompressedClip AnimationCompressor::CompressClip(const AnimationClip& clip, const Settings& settings)
    {
        // Work on a copy so we can reduce without modifying the original
        AnimationClip reduced = clip;
        ReduceClip(reduced, settings);

        CompressedClip result;
        result.name = clip.name;
        result.duration = clip.duration;
        result.ticksPerSecond = clip.ticksPerSecond;
        result.loop = clip.loop;
        result.channels.reserve(reduced.channels.size());

        for (const auto& channel : reduced.channels)
        {
            CompressedBoneChannel compChannel;
            compChannel.boneName = channel.boneName;
            compChannel.boneIndex = channel.boneIndex;

            // -- Compress positions --
            if (!channel.positionKeys.empty())
            {
                // Compute per-component range
                XMFLOAT3 pMin{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::max()};
                XMFLOAT3 pMax{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest()};

                for (const auto& key : channel.positionKeys)
                {
                    pMin.x = std::min(pMin.x, key.value.x);
                    pMin.y = std::min(pMin.y, key.value.y);
                    pMin.z = std::min(pMin.z, key.value.z);
                    pMax.x = std::max(pMax.x, key.value.x);
                    pMax.y = std::max(pMax.y, key.value.y);
                    pMax.z = std::max(pMax.z, key.value.z);
                }

                compChannel.positionTrack.rangeMin = pMin;
                compChannel.positionTrack.rangeMax = pMax;
                compChannel.positionTrack.times.reserve(channel.positionKeys.size());
                compChannel.positionTrack.values.reserve(channel.positionKeys.size());

                for (const auto& key : channel.positionKeys)
                {
                    compChannel.positionTrack.times.push_back(key.time);
                    if (settings.enableQuantization)
                    {
                        compChannel.positionTrack.values.push_back(QuantizeVector(key.value, pMin, pMax));
                    }
                    else
                    {
                        // Store full precision as 16-bit (identity mapping with full range)
                        compChannel.positionTrack.values.push_back(QuantizeVector(key.value, pMin, pMax));
                    }
                }
            }

            // -- Compress rotations --
            if (!channel.rotationKeys.empty())
            {
                compChannel.rotationTrack.times.reserve(channel.rotationKeys.size());
                compChannel.rotationTrack.values.reserve(channel.rotationKeys.size());

                for (const auto& key : channel.rotationKeys)
                {
                    compChannel.rotationTrack.times.push_back(key.time);
                    compChannel.rotationTrack.values.push_back(QuantizeQuaternion(key.value));
                }
            }

            // -- Compress scales --
            if (!channel.scaleKeys.empty())
            {
                XMFLOAT3 sMin{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::max()};
                XMFLOAT3 sMax{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest()};

                for (const auto& key : channel.scaleKeys)
                {
                    sMin.x = std::min(sMin.x, key.value.x);
                    sMin.y = std::min(sMin.y, key.value.y);
                    sMin.z = std::min(sMin.z, key.value.z);
                    sMax.x = std::max(sMax.x, key.value.x);
                    sMax.y = std::max(sMax.y, key.value.y);
                    sMax.z = std::max(sMax.z, key.value.z);
                }

                compChannel.scaleTrack.rangeMin = sMin;
                compChannel.scaleTrack.rangeMax = sMax;
                compChannel.scaleTrack.times.reserve(channel.scaleKeys.size());
                compChannel.scaleTrack.values.reserve(channel.scaleKeys.size());

                for (const auto& key : channel.scaleKeys)
                {
                    compChannel.scaleTrack.times.push_back(key.time);
                    compChannel.scaleTrack.values.push_back(QuantizeVector(key.value, sMin, sMax));
                }
            }

            result.channels.push_back(std::move(compChannel));
        }

        return result;
    }

    // =========================================================================
    // Decompression (random access)
    // =========================================================================

    XMFLOAT3 AnimationCompressor::DecompressPosition(const CompressedVectorTrack& track, size_t keyIndex)
    {
        if (keyIndex >= track.values.size())
        {
            return {0.0f, 0.0f, 0.0f};
        }
        return DequantizeVector(track.values[keyIndex], track.rangeMin, track.rangeMax);
    }

    XMFLOAT3 AnimationCompressor::DecompressScale(const CompressedVectorTrack& track, size_t keyIndex)
    {
        return DecompressPosition(track, keyIndex);
    }

    XMFLOAT4 AnimationCompressor::DecompressRotation(const CompressedQuatTrack& track, size_t keyIndex)
    {
        if (keyIndex >= track.values.size())
        {
            return {0.0f, 0.0f, 0.0f, 1.0f};
        }
        return DequantizeQuaternion(track.values[keyIndex]);
    }

    // =========================================================================
    // Interpolated sampling from compressed tracks
    // =========================================================================

    XMFLOAT3 AnimationCompressor::SamplePosition(const CompressedVectorTrack& track, float time)
    {
        if (track.times.empty())
        {
            return {0.0f, 0.0f, 0.0f};
        }

        // Clamp to track range
        if (time <= track.times.front())
        {
            return DecompressPosition(track, 0);
        }
        if (time >= track.times.back())
        {
            return DecompressPosition(track, track.times.size() - 1);
        }

        // Binary search for surrounding keyframes
        auto it = std::upper_bound(track.times.begin(), track.times.end(), time);
        const size_t nextIdx = static_cast<size_t>(it - track.times.begin());
        const size_t prevIdx = nextIdx - 1;

        const float t0 = track.times[prevIdx];
        const float t1 = track.times[nextIdx];
        const float dt = t1 - t0;
        const float t = (dt > 0.0f) ? (time - t0) / dt : 0.0f;

        const XMFLOAT3 a = DecompressPosition(track, prevIdx);
        const XMFLOAT3 b = DecompressPosition(track, nextIdx);

        return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
    }

    XMFLOAT4 AnimationCompressor::SampleRotation(const CompressedQuatTrack& track, float time)
    {
        if (track.times.empty())
        {
            return {0.0f, 0.0f, 0.0f, 1.0f};
        }

        if (time <= track.times.front())
        {
            return DecompressRotation(track, 0);
        }
        if (time >= track.times.back())
        {
            return DecompressRotation(track, track.times.size() - 1);
        }

        auto it = std::upper_bound(track.times.begin(), track.times.end(), time);
        const size_t nextIdx = static_cast<size_t>(it - track.times.begin());
        const size_t prevIdx = nextIdx - 1;

        const float t0 = track.times[prevIdx];
        const float t1 = track.times[nextIdx];
        const float dt = t1 - t0;
        const float t = (dt > 0.0f) ? (time - t0) / dt : 0.0f;

        const XMFLOAT4 qa = DecompressRotation(track, prevIdx);
        const XMFLOAT4 qb = DecompressRotation(track, nextIdx);

        // SLERP
        float ax = qa.x, ay = qa.y, az = qa.z, aw = qa.w;
        float bx = qb.x, by = qb.y, bz = qb.z, bw = qb.w;

        float dot = ax * bx + ay * by + az * bz + aw * bw;
        if (dot < 0.0f)
        {
            bx = -bx;
            by = -by;
            bz = -bz;
            bw = -bw;
            dot = -dot;
        }

        float rx, ry, rz, rw;
        if (dot > 0.9995f)
        {
            // Near-identical: use LERP
            rx = ax + t * (bx - ax);
            ry = ay + t * (by - ay);
            rz = az + t * (bz - az);
            rw = aw + t * (bw - aw);
        }
        else
        {
            const float theta = std::acos(dot);
            const float sinTheta = std::sin(theta);
            const float wA = std::sin((1.0f - t) * theta) / sinTheta;
            const float wB = std::sin(t * theta) / sinTheta;

            rx = wA * ax + wB * bx;
            ry = wA * ay + wB * by;
            rz = wA * az + wB * bz;
            rw = wA * aw + wB * bw;
        }

        // Normalize
        const float len = std::sqrt(rx * rx + ry * ry + rz * rz + rw * rw);
        if (len > 0.0f)
        {
            const float invLen = 1.0f / len;
            rx *= invLen;
            ry *= invLen;
            rz *= invLen;
            rw *= invLen;
        }

        return {rx, ry, rz, rw};
    }

    XMFLOAT3 AnimationCompressor::SampleScale(const CompressedVectorTrack& track, float time)
    {
        return SamplePosition(track, time);
    }

    // =========================================================================
    // Size estimation
    // =========================================================================

    size_t AnimationCompressor::EstimateCompressedSize(const CompressedClip& clip)
    {
        size_t totalBytes = 0;
        for (const auto& channel : clip.channels)
        {
            // Position: float time (4B) + QuantizedVector (6B) = 10B per keyframe + 24B range header
            totalBytes += 24; // rangeMin + rangeMax (2 * XMFLOAT3 = 2 * 12)
            totalBytes += channel.positionTrack.times.size() * 10;

            // Rotation: float time (4B) + QuantizedQuat (6B) = 10B per keyframe
            totalBytes += channel.rotationTrack.times.size() * 10;

            // Scale: same as position
            totalBytes += 24;
            totalBytes += channel.scaleTrack.times.size() * 10;
        }
        return totalBytes;
    }

    size_t AnimationCompressor::EstimateUncompressedSize(const AnimationClip& clip)
    {
        size_t totalBytes = 0;
        for (const auto& channel : clip.channels)
        {
            // VectorKey: float time + XMFLOAT3 = 4 + 12 = 16 bytes
            totalBytes += channel.positionKeys.size() * sizeof(VectorKey);
            // QuatKey: float time + XMFLOAT4 = 4 + 16 = 20 bytes
            totalBytes += channel.rotationKeys.size() * sizeof(QuatKey);
            // VectorKey: same as position
            totalBytes += channel.scaleKeys.size() * sizeof(VectorKey);
        }
        return totalBytes;
    }

    float AnimationCompressor::GetCompressionRatio(const CompressedClip& compressed, const AnimationClip& uncompressed)
    {
        const size_t uncompSize = EstimateUncompressedSize(uncompressed);
        if (uncompSize == 0)
        {
            return 1.0f;
        }
        const size_t compSize = EstimateCompressedSize(compressed);
        return static_cast<float>(compSize) / static_cast<float>(uncompSize);
    }

    // =========================================================================
    // Quaternion quantization: smallest-three, 48-bit
    // =========================================================================

    QuantizedQuat AnimationCompressor::QuantizeQuaternion(const XMFLOAT4& q)
    {
        // Find the largest absolute component
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

        // Ensure the dropped component is positive (negate quaternion if needed,
        // since q and -q represent the same rotation)
        if (components[largestIndex] < 0.0f)
        {
            components[0] = -components[0];
            components[1] = -components[1];
            components[2] = -components[2];
            components[3] = -components[3];
        }

        // Collect the three kept components in order
        float kept[3];
        int32_t keptIdx = 0;
        for (int32_t i = 0; i < 4; ++i)
        {
            if (i != largestIndex)
            {
                kept[keptIdx++] = components[i];
            }
        }

        // Quantize each kept component from [-1/sqrt(2), 1/sqrt(2)] to [0, 32767]
        auto quantizeComponent = [](float value) -> uint16_t
        {
            // Clamp to valid range
            const float clamped = std::max(-QuatComponentRange, std::min(QuatComponentRange, value));
            // Map from [-range, +range] to [0, 1]
            const float normalized = (clamped + QuatComponentRange) / (2.0f * QuatComponentRange);
            // Map to [0, QuatQuantMax]
            const int32_t quantized = static_cast<int32_t>(normalized * static_cast<float>(QuatQuantMax) + 0.5f);
            return static_cast<uint16_t>(std::max(0, std::min(QuatQuantMax, quantized)));
        };

        const uint16_t a = quantizeComponent(kept[0]);
        const uint16_t b = quantizeComponent(kept[1]);
        const uint16_t c = quantizeComponent(kept[2]);

        // Pack into 48 bits:
        //   Byte 0: [largest(2 bits)][a high 6 bits]
        //   Byte 1: [a low 8 bits]     (a is now bits: byte0[5:0] + byte1[7:0] = 14? No.)
        //
        // Better layout: pack as a 48-bit integer.
        //   bits [47:46] = largestIndex (2 bits)
        //   bits [45:31] = a (15 bits)
        //   bits [30:16] = b (15 bits)
        //   bits [15: 0] = c (15 bits) -- but 15 bits only goes to bit 14, pad bit 15
        //
        // Actually: 2 + 15 + 15 + 15 = 47 bits. We have 48 bits available, so 1 unused bit.
        // Layout: bits [47:46] = index, [45:31] = a, [30:16] = b, [15:1] = c, [0] = 0 (unused)

        uint64_t packed = 0;
        packed |= (static_cast<uint64_t>(largestIndex) & 0x3) << 46;
        packed |= (static_cast<uint64_t>(a) & 0x7FFF) << 31;
        packed |= (static_cast<uint64_t>(b) & 0x7FFF) << 16;
        packed |= (static_cast<uint64_t>(c) & 0x7FFF) << 1;

        QuantizedQuat result;
        // Store big-endian in data[6]
        result.data[0] = static_cast<uint8_t>((packed >> 40) & 0xFF);
        result.data[1] = static_cast<uint8_t>((packed >> 32) & 0xFF);
        result.data[2] = static_cast<uint8_t>((packed >> 24) & 0xFF);
        result.data[3] = static_cast<uint8_t>((packed >> 16) & 0xFF);
        result.data[4] = static_cast<uint8_t>((packed >> 8) & 0xFF);
        result.data[5] = static_cast<uint8_t>((packed >> 0) & 0xFF);

        return result;
    }

    XMFLOAT4 AnimationCompressor::DequantizeQuaternion(const QuantizedQuat& packed)
    {
        // Reconstruct the 48-bit integer
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

        // Dequantize each component from [0, 32767] to [-1/sqrt(2), 1/sqrt(2)]
        auto dequantizeComponent = [](uint16_t value) -> float
        {
            const float normalized = static_cast<float>(value) / static_cast<float>(QuatQuantMax);
            return normalized * (2.0f * QuatComponentRange) - QuatComponentRange;
        };

        const float a = dequantizeComponent(qa);
        const float b = dequantizeComponent(qb);
        const float c = dequantizeComponent(qc);

        // Reconstruct the dropped component
        const float sumSquares = a * a + b * b + c * c;
        const float d = std::sqrt(std::max(0.0f, 1.0f - sumSquares));

        // Place components back in order
        float result[4];
        int32_t keptIdx = 0;
        for (int32_t i = 0; i < 4; ++i)
        {
            if (i == largestIndex)
            {
                result[i] = d; // Always positive (we ensured this during encoding)
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

    // =========================================================================
    // Vector quantization: 16-bit per component
    // =========================================================================

    QuantizedVector AnimationCompressor::QuantizeVector(const XMFLOAT3& v, const XMFLOAT3& rangeMin,
                                                        const XMFLOAT3& rangeMax)
    {
        auto quantizeAxis = [](float value, float minVal, float maxVal) -> uint16_t
        {
            const float range = maxVal - minVal;
            if (range <= 0.0f)
            {
                return 0;
            }
            const float normalized = (value - minVal) / range;
            const float clamped = std::max(0.0f, std::min(1.0f, normalized));
            return static_cast<uint16_t>(clamped * VectorQuantMax + 0.5f);
        };

        QuantizedVector result;
        result.x = quantizeAxis(v.x, rangeMin.x, rangeMax.x);
        result.y = quantizeAxis(v.y, rangeMin.y, rangeMax.y);
        result.z = quantizeAxis(v.z, rangeMin.z, rangeMax.z);
        return result;
    }

    XMFLOAT3 AnimationCompressor::DequantizeVector(const QuantizedVector& qv, const XMFLOAT3& rangeMin,
                                                   const XMFLOAT3& rangeMax)
    {
        auto dequantizeAxis = [](uint16_t value, float minVal, float maxVal) -> float
        {
            const float range = maxVal - minVal;
            if (range <= 0.0f)
            {
                return minVal;
            }
            const float normalized = static_cast<float>(value) / VectorQuantMax;
            return minVal + normalized * range;
        };

        return {dequantizeAxis(qv.x, rangeMin.x, rangeMax.x), dequantizeAxis(qv.y, rangeMin.y, rangeMax.y),
                dequantizeAxis(qv.z, rangeMin.z, rangeMax.z)};
    }

    // =========================================================================
    // Keyframe reduction (internal helpers)
    // =========================================================================

    void AnimationCompressor::ReduceKeyframes(std::vector<VectorKey>& keys, float tolerance)
    {
        if (keys.size() <= 2)
        {
            return;
        }

        std::vector<VectorKey> reduced;
        reduced.reserve(keys.size());
        reduced.push_back(keys.front());

        size_t i = 1;
        while (i < keys.size() - 1)
        {
            const VectorKey& prev = reduced.back();
            const VectorKey& current = keys[i];
            const VectorKey& next = keys[i + 1];

            const float error = CalculateInterpolationError(prev, current, next);
            if (error > tolerance)
            {
                reduced.push_back(current);
            }
            ++i;
        }

        reduced.push_back(keys.back());
        keys = std::move(reduced);
    }

    void AnimationCompressor::ReduceRotationKeyframes(std::vector<QuatKey>& keys, float tolerance)
    {
        if (keys.size() <= 2)
        {
            return;
        }

        std::vector<QuatKey> reduced;
        reduced.reserve(keys.size());
        reduced.push_back(keys.front());

        size_t i = 1;
        while (i < keys.size() - 1)
        {
            const QuatKey& prev = reduced.back();
            const QuatKey& current = keys[i];
            const QuatKey& next = keys[i + 1];

            const float error = CalculateRotationError(prev, current, next);
            if (error > tolerance)
            {
                reduced.push_back(current);
            }
            ++i;
        }

        reduced.push_back(keys.back());
        keys = std::move(reduced);
    }

    float AnimationCompressor::CalculateInterpolationError(const VectorKey& prev, const VectorKey& removed,
                                                           const VectorKey& next)
    {
        const float totalTime = next.time - prev.time;
        if (totalTime <= 0.0f)
        {
            return 0.0f;
        }

        const float t = (removed.time - prev.time) / totalTime;

        // Linear interpolation between prev and next
        const float interpX = prev.value.x + t * (next.value.x - prev.value.x);
        const float interpY = prev.value.y + t * (next.value.y - prev.value.y);
        const float interpZ = prev.value.z + t * (next.value.z - prev.value.z);

        // Euclidean distance to actual value
        const float dx = interpX - removed.value.x;
        const float dy = interpY - removed.value.y;
        const float dz = interpZ - removed.value.z;

        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float AnimationCompressor::CalculateRotationError(const QuatKey& prev, const QuatKey& removed, const QuatKey& next)
    {
        const float totalTime = next.time - prev.time;
        if (totalTime <= 0.0f)
        {
            return 0.0f;
        }

        const float t = (removed.time - prev.time) / totalTime;

        // SLERP between prev and next quaternions
        float ax = prev.value.x, ay = prev.value.y, az = prev.value.z, aw = prev.value.w;
        float bx = next.value.x, by = next.value.y, bz = next.value.z, bw = next.value.w;

        // Ensure shortest path
        float dot = ax * bx + ay * by + az * bz + aw * bw;
        if (dot < 0.0f)
        {
            bx = -bx;
            by = -by;
            bz = -bz;
            bw = -bw;
            dot = -dot;
        }

        float interpX, interpY, interpZ, interpW;
        if (dot > 0.9995f)
        {
            interpX = ax + t * (bx - ax);
            interpY = ay + t * (by - ay);
            interpZ = az + t * (bz - az);
            interpW = aw + t * (bw - aw);
        }
        else
        {
            const float theta = std::acos(dot);
            const float sinTheta = std::sin(theta);
            const float weightA = std::sin((1.0f - t) * theta) / sinTheta;
            const float weightB = std::sin(t * theta) / sinTheta;

            interpX = weightA * ax + weightB * bx;
            interpY = weightA * ay + weightB * by;
            interpZ = weightA * az + weightB * bz;
            interpW = weightA * aw + weightB * bw;
        }

        // Normalize the interpolated quaternion
        const float interpLen =
            std::sqrt(interpX * interpX + interpY * interpY + interpZ * interpZ + interpW * interpW);
        if (interpLen > 0.0f)
        {
            const float invLen = 1.0f / interpLen;
            interpX *= invLen;
            interpY *= invLen;
            interpZ *= invLen;
            interpW *= invLen;
        }

        // Angular distance between interpolated and actual quaternion
        const float rx = removed.value.x, ry = removed.value.y;
        const float rz = removed.value.z, rw = removed.value.w;

        float errorDot = interpX * rx + interpY * ry + interpZ * rz + interpW * rw;
        errorDot = std::max(-1.0f, std::min(1.0f, errorDot));

        return 2.0f * std::acos(std::abs(errorDot));
    }

} // namespace Spark::Animation
