/**
 * @file AnimationCompression.h
 * @brief Animation keyframe compression via redundant keyframe removal and quantization
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Implements R5.3 (Animation Compression) from the architecture analysis. Provides
 * lossless and lossy compression of AnimationClip keyframe data by:
 *
 * 1. **Redundant keyframe removal** -- Keyframes that can be reconstructed via linear
 *    interpolation (position/scale) or SLERP (rotation) within a configurable tolerance
 *    are removed. This is the primary compression method.
 *
 * 2. **Quaternion quantization** -- Smallest-three encoding packs a unit quaternion into
 *    48 bits (2-bit index + 3 x 15-bit components). The largest component is omitted
 *    and reconstructed from the unit-length constraint.
 *
 * 3. **Position/scale quantization** -- Each 3D vector is quantized to 16 bits per
 *    component relative to a per-channel min/max range, giving 48 bits total.
 *
 * 4. **CompressClip / DecompressKeyframe** -- High-level API that takes an AnimationClip,
 *    produces a CompressedClip, and provides random-access decompression of individual
 *    keyframes for runtime playback.
 *
 * ## Algorithm
 * For each bone channel, the compressor iterates keyframes and tests whether removing
 * a keyframe would produce an interpolation error exceeding the tolerance. If the error
 * is within tolerance, the keyframe is removed. The algorithm preserves the first and
 * last keyframes to maintain clip duration. After reduction, the surviving keyframes are
 * quantized into compact binary representations.
 *
 * ## Usage
 * @code
 *   // Compress
 *   auto clip = AnimationManager::GetInstance().GetClip("Run");
 *   AnimationCompressor::Settings settings;
 *   settings.positionTolerance = 0.001f; // 1mm
 *   settings.rotationTolerance = 0.001f; // ~0.06 degrees
 *   CompressedClip compressed = AnimationCompressor::CompressClip(*clip, settings);
 *
 *   // Decompress a single keyframe at runtime
 *   XMFLOAT3 pos = AnimationCompressor::DecompressPosition(compressed.channels[0].positionTrack, 3);
 *   XMFLOAT4 rot = AnimationCompressor::DecompressRotation(compressed.channels[0].rotationTrack, 3);
 * @endcode
 */

#pragma once
#include "AnimationSystem.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>


namespace Spark::Animation
{

    // =========================================================================
    // Quantized data types
    // =========================================================================

    /**
     * @brief A quaternion packed into 48 bits using smallest-three encoding.
     *
     * Layout (6 bytes / 48 bits):
     *   - Bits [47:46] : 2-bit index of the largest (dropped) component (0=x,1=y,2=z,3=w)
     *   - Bits [45:31] : 15-bit quantized value of the first kept component
     *   - Bits [30:16] : 15-bit quantized value of the second kept component
     *   - Bits [15: 0] : 15-bit quantized value of the third kept component
     *
     * Each 15-bit value represents a float in [-1/sqrt(2), 1/sqrt(2)] mapped to [0, 32767].
     * The dropped component is reconstructed: sqrt(1 - a^2 - b^2 - c^2), always positive
     * (we ensure the dropped component is positive by negating the quaternion if needed).
     */
    struct QuantizedQuat
    {
        /** @brief Packed 48-bit representation stored in a uint8_t[6] array. */
        uint8_t data[6];
    };

    /**
     * @brief A 3D vector quantized to 16 bits per component (48 bits total).
     *
     * Each component is linearly mapped from [rangeMin, rangeMax] to [0, 65535].
     * The range is stored per-track in `CompressedVectorTrack`.
     */
    struct QuantizedVector
    {
        uint16_t x;
        uint16_t y;
        uint16_t z;
    };

    // =========================================================================
    // Compressed track data
    // =========================================================================

    /**
     * @brief A compressed track of position or scale keyframes.
     *
     * Stores the quantization range and an array of (time, quantized-value) pairs.
     */
    struct CompressedVectorTrack
    {
        /** @brief Per-component minimum values used for dequantization. */
        XMFLOAT3 rangeMin{0.0f, 0.0f, 0.0f};

        /** @brief Per-component maximum values used for dequantization. */
        XMFLOAT3 rangeMax{0.0f, 0.0f, 0.0f};

        /** @brief Keyframe times in seconds (parallel array with `values`). */
        std::vector<float> times;

        /** @brief Quantized vector values (parallel array with `times`). */
        std::vector<QuantizedVector> values;
    };

    /**
     * @brief A compressed track of rotation keyframes using smallest-three encoding.
     *
     * No range is needed because quaternion components are bounded by [-1/sqrt(2), 1/sqrt(2)].
     */
    struct CompressedQuatTrack
    {
        /** @brief Keyframe times in seconds (parallel array with `values`). */
        std::vector<float> times;

        /** @brief Quantized quaternion values (parallel array with `times`). */
        std::vector<QuantizedQuat> values;
    };

    /**
     * @brief Compressed animation data for a single bone channel.
     */
    struct CompressedBoneChannel
    {
        /** @brief Name of the bone this channel animates. */
        std::string boneName;

        /** @brief Pre-resolved bone index (cached for performance). */
        int32_t boneIndex = -1;

        /** @brief Compressed position keyframes. */
        CompressedVectorTrack positionTrack;

        /** @brief Compressed rotation keyframes (smallest-three, 48-bit). */
        CompressedQuatTrack rotationTrack;

        /** @brief Compressed scale keyframes. */
        CompressedVectorTrack scaleTrack;
    };

    /**
     * @brief A fully compressed animation clip.
     *
     * Produced by `AnimationCompressor::CompressClip()`. Contains all the data
     * needed for runtime playback at reduced memory cost.
     */
    struct CompressedClip
    {
        /** @brief Human-readable clip name. */
        std::string name;

        /** @brief Total clip duration in seconds. */
        float duration = 0.0f;

        /** @brief Ticks per second from the source clip. */
        float ticksPerSecond = 24.0f;

        /** @brief Whether the clip loops. */
        bool loop = true;

        /** @brief Per-bone compressed channels. */
        std::vector<CompressedBoneChannel> channels;
    };

    // =========================================================================
    // AnimationCompressor
    // =========================================================================

    /**
     * @class AnimationCompressor
     * @brief Compresses AnimationClip data by removing redundant keyframes and quantizing.
     *
     * All methods are static -- the compressor holds no state. Two usage modes:
     *
     * 1. **In-place reduction** (`ReduceClip`): modifies an AnimationClip by removing
     *    redundant keyframes. No quantization; useful as a pre-process.
     *
     * 2. **Full compression** (`CompressClip`): reduces AND quantizes into a CompressedClip
     *    with smallest-three quaternions and 16-bit position/scale. Use `DecompressPosition`,
     *    `DecompressRotation`, `DecompressScale` for random-access playback.
     */
    class AnimationCompressor
    {
      public:
        /**
         * @brief Configuration for keyframe compression tolerances.
         *
         * Lower tolerances produce higher quality at the cost of less compression.
         * The defaults are suitable for typical gameplay animations.
         */
        struct Settings
        {
            /** @brief Maximum allowed position error in metres. Default: 0.001 m (1 mm). */
            float positionTolerance = 0.001f;

            /** @brief Maximum allowed rotation error in radians. Default: 0.001 rad (~0.06 degrees). */
            float rotationTolerance = 0.001f;

            /** @brief Maximum allowed scale error (unitless). Default: 0.001. */
            float scaleTolerance = 0.001f;

            /** @brief Whether to enable quantization in CompressClip. Default: true. */
            bool enableQuantization = true;

            /** @brief Bits per component for quantized position/scale. Default: 16. */
            int quantizationBits = 16;
        };

        // =====================================================================
        // In-place keyframe reduction (modifies AnimationClip directly)
        // =====================================================================

        /**
         * @brief Reduce an animation clip by removing redundant keyframes in place.
         *
         * Iterates all bone channels and removes keyframes that can be reconstructed
         * via interpolation within the specified tolerances. Modifies the clip in place.
         * Does NOT quantize -- use CompressClip for full compression.
         *
         * @param clip      The AnimationClip to reduce. Modified in place.
         * @param settings  Compression tolerances and options.
         */
        static void ReduceClip(AnimationClip& clip, const Settings& settings);

        /** @brief Overload using default settings. */
        static void ReduceClip(AnimationClip& clip);

        // =====================================================================
        // Full compression pipeline (reduce + quantize -> CompressedClip)
        // =====================================================================

        /**
         * @brief Compress an animation clip: reduce keyframes then quantize.
         *
         * Produces a CompressedClip with smallest-three quaternion encoding (48-bit)
         * and 16-bit-per-component position/scale quantization. The original clip
         * is NOT modified.
         *
         * @param clip      The source AnimationClip to compress.
         * @param settings  Compression tolerances and quantization options.
         * @return          A CompressedClip ready for runtime playback.
         */
        static CompressedClip CompressClip(const AnimationClip& clip, const Settings& settings);

        /** @brief Overload using default settings. */
        static CompressedClip CompressClip(const AnimationClip& clip);

        // =====================================================================
        // Decompression (random access)
        // =====================================================================

        /**
         * @brief Decompress a single position keyframe from a compressed track.
         *
         * @param track       The compressed vector track.
         * @param keyIndex    Index of the keyframe to decompress.
         * @return            Decompressed world-space position.
         */
        static XMFLOAT3 DecompressPosition(const CompressedVectorTrack& track, size_t keyIndex);

        /**
         * @brief Decompress a single scale keyframe from a compressed track.
         *
         * Identical to DecompressPosition (same quantization scheme).
         *
         * @param track       The compressed vector track.
         * @param keyIndex    Index of the keyframe to decompress.
         * @return            Decompressed scale value.
         */
        static XMFLOAT3 DecompressScale(const CompressedVectorTrack& track, size_t keyIndex);

        /**
         * @brief Decompress a single rotation keyframe from a compressed track.
         *
         * Reconstructs the full quaternion from smallest-three encoding.
         *
         * @param track       The compressed quaternion track.
         * @param keyIndex    Index of the keyframe to decompress.
         * @return            Decompressed unit quaternion (x, y, z, w).
         */
        static XMFLOAT4 DecompressRotation(const CompressedQuatTrack& track, size_t keyIndex);

        /**
         * @brief Decompress an interpolated position at a given time.
         *
         * Finds the surrounding keyframes, decompresses both, and linearly interpolates.
         *
         * @param track  The compressed vector track.
         * @param time   Time in seconds.
         * @return       Interpolated decompressed position.
         */
        static XMFLOAT3 SamplePosition(const CompressedVectorTrack& track, float time);

        /**
         * @brief Decompress an interpolated rotation at a given time.
         *
         * Finds the surrounding keyframes, decompresses both, and applies SLERP.
         *
         * @param track  The compressed quaternion track.
         * @param time   Time in seconds.
         * @return       Interpolated decompressed quaternion (x, y, z, w).
         */
        static XMFLOAT4 SampleRotation(const CompressedQuatTrack& track, float time);

        /**
         * @brief Decompress an interpolated scale at a given time.
         *
         * @param track  The compressed vector track.
         * @param time   Time in seconds.
         * @return       Interpolated decompressed scale.
         */
        static XMFLOAT3 SampleScale(const CompressedVectorTrack& track, float time);

        // =====================================================================
        // Size estimation
        // =====================================================================

        /**
         * @brief Estimate the compressed size of a CompressedClip in bytes.
         *
         * @param clip  The CompressedClip to estimate.
         * @return      Estimated size in bytes.
         */
        static size_t EstimateCompressedSize(const CompressedClip& clip);

        /**
         * @brief Estimate the uncompressed size of an AnimationClip in bytes.
         *
         * @param clip  The AnimationClip to estimate.
         * @return      Estimated uncompressed size in bytes.
         */
        static size_t EstimateUncompressedSize(const AnimationClip& clip);

        /**
         * @brief Get the compression ratio for a compressed clip vs its source.
         *
         * @param compressed    The compressed clip.
         * @param uncompressed  The original clip.
         * @return              Ratio in [0, 1]. 0.5 means 50% size reduction.
         */
        static float GetCompressionRatio(const CompressedClip& compressed, const AnimationClip& uncompressed);

        // =====================================================================
        // Low-level quantization utilities
        // =====================================================================

        /**
         * @brief Encode a unit quaternion into smallest-three 48-bit format.
         *
         * @param q  Input quaternion (x, y, z, w). Must be normalized.
         * @return   Packed 48-bit representation.
         */
        static QuantizedQuat QuantizeQuaternion(const XMFLOAT4& q);

        /**
         * @brief Decode a smallest-three 48-bit quaternion back to float.
         *
         * @param packed  The packed quaternion.
         * @return        Reconstructed unit quaternion (x, y, z, w).
         */
        static XMFLOAT4 DequantizeQuaternion(const QuantizedQuat& packed);

        /**
         * @brief Quantize a 3D vector to 16-bit per component given a known range.
         *
         * @param v        The vector to quantize.
         * @param rangeMin Per-component minimum of the range.
         * @param rangeMax Per-component maximum of the range.
         * @return         Quantized 48-bit vector.
         */
        static QuantizedVector QuantizeVector(const XMFLOAT3& v, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax);

        /**
         * @brief Dequantize a 16-bit-per-component vector back to float.
         *
         * @param qv       The quantized vector.
         * @param rangeMin Per-component minimum of the range.
         * @param rangeMax Per-component maximum of the range.
         * @return         Reconstructed float vector.
         */
        static XMFLOAT3 DequantizeVector(const QuantizedVector& qv, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax);

      private:
        // =====================================================================
        // Keyframe reduction (internal)
        // =====================================================================

        /**
         * @brief Remove redundant position/scale keyframes within tolerance.
         */
        static void ReduceKeyframes(std::vector<VectorKey>& keys, float tolerance);

        /**
         * @brief Remove redundant rotation keyframes within tolerance.
         */
        static void ReduceRotationKeyframes(std::vector<QuatKey>& keys, float tolerance);

        /**
         * @brief Calculate interpolation error for a candidate position/scale keyframe removal.
         */
        static float CalculateInterpolationError(const VectorKey& prev, const VectorKey& removed,
                                                 const VectorKey& next);

        /**
         * @brief Calculate SLERP interpolation error for a candidate rotation keyframe removal.
         */
        static float CalculateRotationError(const QuatKey& prev, const QuatKey& removed, const QuatKey& next);

        /** @brief Maximum value for a 15-bit component in smallest-three encoding. */
        static constexpr int32_t QuatQuantMax = 32767; // 2^15 - 1

        /** @brief Range of each component in smallest-three: [-1/sqrt(2), 1/sqrt(2)]. */
        static constexpr float QuatComponentRange = 0.70710678118f; // 1/sqrt(2)

        /** @brief Maximum value for 16-bit vector component quantization. */
        static constexpr float VectorQuantMax = 65535.0f;
    };

} // namespace Spark::Animation
