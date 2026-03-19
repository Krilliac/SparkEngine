/**
 * @file AnimationCompression.h
 * @brief Animation keyframe compression via redundant key removal and quantization
 *
 * Removes redundant keyframes (within interpolation tolerance) and quantizes surviving
 * keys: smallest-three 48-bit quaternions, 16-bit-per-component position/scale.
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

    /// @brief Quaternion packed into 48 bits via smallest-three encoding.
    struct QuantizedQuat
    {
        uint8_t data[6]; ///< 2-bit index + 3x15-bit components
    };

    /// @brief 3D vector quantized to 16 bits per component (48 bits total).
    struct QuantizedVector
    {
        uint16_t x;
        uint16_t y;
        uint16_t z;
    };

    // =========================================================================
    // Compressed track data
    // =========================================================================

    /// @brief Compressed track of position/scale keyframes with quantization range.
    struct CompressedVectorTrack
    {
        XMFLOAT3 rangeMin{0.0f, 0.0f, 0.0f};
        XMFLOAT3 rangeMax{0.0f, 0.0f, 0.0f};
        std::vector<float> times;
        std::vector<QuantizedVector> values;
    };

    /// @brief Compressed track of rotation keyframes (smallest-three).
    struct CompressedQuatTrack
    {
        std::vector<float> times;
        std::vector<QuantizedQuat> values;
    };

    /// @brief Compressed animation data for a single bone channel.
    struct CompressedBoneChannel
    {
        std::string boneName;
        int32_t boneIndex = -1;
        CompressedVectorTrack positionTrack;
        CompressedQuatTrack rotationTrack;
        CompressedVectorTrack scaleTrack;
    };

    /// @brief A fully compressed animation clip for runtime playback.
    struct CompressedClip
    {
        std::string name;
        float duration = 0.0f;
        float ticksPerSecond = 24.0f;
        bool loop = true;
        std::vector<CompressedBoneChannel> channels;
    };

    // =========================================================================
    // AnimationCompressor
    // =========================================================================

    /// @brief Compresses AnimationClip data by removing redundant keyframes and quantizing.
    class AnimationCompressor
    {
      public:
        struct Settings
        {
            float positionTolerance = 0.001f; ///< Max position error in metres
            float rotationTolerance = 0.001f; ///< Max rotation error in radians
            float scaleTolerance = 0.001f;    ///< Max scale error
            bool enableQuantization = true;
            int quantizationBits = 16;
        };

        /// @brief Remove redundant keyframes in place (no quantization).
        static void ReduceClip(AnimationClip& clip, const Settings& settings);
        static void ReduceClip(AnimationClip& clip);

        /// @brief Full compression: reduce + quantize → CompressedClip.
        static CompressedClip CompressClip(const AnimationClip& clip, const Settings& settings);
        static CompressedClip CompressClip(const AnimationClip& clip);

        // Random-access decompression
        static XMFLOAT3 DecompressPosition(const CompressedVectorTrack& track, size_t keyIndex);
        static XMFLOAT3 DecompressScale(const CompressedVectorTrack& track, size_t keyIndex);
        static XMFLOAT4 DecompressRotation(const CompressedQuatTrack& track, size_t keyIndex);

        // Interpolated sampling at a given time
        static XMFLOAT3 SamplePosition(const CompressedVectorTrack& track, float time);
        static XMFLOAT4 SampleRotation(const CompressedQuatTrack& track, float time);
        static XMFLOAT3 SampleScale(const CompressedVectorTrack& track, float time);

        // Size estimation
        static size_t EstimateCompressedSize(const CompressedClip& clip);
        static size_t EstimateUncompressedSize(const AnimationClip& clip);
        static float GetCompressionRatio(const CompressedClip& compressed, const AnimationClip& uncompressed);

        // Low-level quantization
        static QuantizedQuat QuantizeQuaternion(const XMFLOAT4& q);
        static XMFLOAT4 DequantizeQuaternion(const QuantizedQuat& packed);
        static QuantizedVector QuantizeVector(const XMFLOAT3& v, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax);
        static XMFLOAT3 DequantizeVector(const QuantizedVector& qv, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax);

      private:
        static void ReduceKeyframes(std::vector<VectorKey>& keys, float tolerance);
        static void ReduceRotationKeyframes(std::vector<QuatKey>& keys, float tolerance);
        static float CalculateInterpolationError(const VectorKey& prev, const VectorKey& removed,
                                                 const VectorKey& next);
        static float CalculateRotationError(const QuatKey& prev, const QuatKey& removed, const QuatKey& next);

        static constexpr int32_t QuatQuantMax = 32767;
        static constexpr float QuatComponentRange = 0.70710678118f;
        static constexpr float VectorQuantMax = 65535.0f;
    };

} // namespace Spark::Animation
