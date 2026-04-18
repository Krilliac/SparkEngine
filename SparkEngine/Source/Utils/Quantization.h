/**
 * @file Quantization.h
 * @brief Low-precision packing for transforms: smallest-three 48-bit quaternions
 *        and 16-bit-per-component bounded-box positions.
 *
 * Shared math primitives used by the networking transform pipeline
 * (`Engine/Networking/NetQuantize.h`) and by the offline animation
 * compressor (`Engine/Animation/AnimationCompression.h`). Both domains want
 * the same bit layout but have different packaging around it; centralising
 * the math here keeps a single source of truth for the round-trip format.
 */

#pragma once

#include "../Core/Platform.h"

#include <array>
#include <cstdint>

namespace Spark::Utils::Quantization
{
    /// @brief Quaternion packed into 48 bits via smallest-three encoding.
    struct PackedQuat48
    {
        std::array<uint8_t, 6> data{};
    };

    /// @brief 3D vector packed into 16 bits per component over an arbitrary
    ///        axis-aligned bounding range.
    struct PackedVec16
    {
        uint16_t x{0};
        uint16_t y{0};
        uint16_t z{0};
    };

    /// @brief Pack a normalized quaternion into 6 bytes.
    ///
    /// The largest-magnitude component is identified and dropped (it can be
    /// reconstructed from the other three plus the unit-length constraint);
    /// the remaining three components are scaled into [0, 32767] and packed
    /// alongside a 2-bit index for the dropped component.
    PackedQuat48 PackQuat48(const XMFLOAT4& q);

    /// @brief Reverse of PackQuat48. Always returns a unit quaternion.
    XMFLOAT4 UnpackQuat48(const PackedQuat48& packed);

    /// @brief Pack a position into 6 bytes, relative to a caller-supplied AABB.
    ///
    /// Values outside [rangeMin, rangeMax] are clamped. Quantization step is
    /// (rangeMax - rangeMin) / 65535 per axis, so larger ranges trade
    /// precision for reach.
    PackedVec16 PackVec16(const XMFLOAT3& v, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax);

    /// @brief Reverse of PackVec16.
    XMFLOAT3 UnpackVec16(const PackedVec16& packed, const XMFLOAT3& rangeMin, const XMFLOAT3& rangeMax);

} // namespace Spark::Utils::Quantization
